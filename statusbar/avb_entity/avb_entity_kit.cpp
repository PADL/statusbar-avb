// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityKit methods — the Entity Construction Kit facade (refactor phase D;
// the wiring bodies come verbatim from AvbEntityToneGenerator/AvbEntityAudioIO,
// which each carried a copy).

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace statusbar::avb_entity {

using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
using statusbar::nanoavb::DomainInfo;

AvbEntityKit::AvbEntityKit(
    AvbEntityAudioIOConfig const& config,
    nanoavb::DescriptorStorageHandler& handler,
    StreamSpecs talker_specs,
    StreamSpecs listener_specs,
    uint32_t const sample_rate,
    uint16_t const initial_clock_source,
    std::optional<uint16_t> const crf_clock_source_index,
    ptpclient::MediaClockGenerator& media_clock,
    std::atomic<int64_t>& last_gptp_ns,
    StreamRxAudioSink* audio_sink,
    std::pmr::memory_resource* memory_resource)
    : config_{config}
    , talker_specs_{talker_specs}
    , listener_specs_{listener_specs}
    , sample_rate_{sample_rate}
    , storage_handler_{handler}
    , host_{handler, default_adp_advertiser_config(), talker_specs_.size(), 4, listener_specs_.size()}
    , gate_{config.gate_talker_on_listener, host_.components()}
    , talker_{std::make_unique<TalkerStreams>(
          TalkerStreamsConfig{.sample_rate = sample_rate, .vlan_id = config.vlan_id, .stream_pcp = config.stream_pcp},
          media_clock,
          last_gptp_ns,
          memory_resource)}
{  // NOLINT(statusbar-block-statements) construction: each collaborator wired in turn; pre-dates the gate
    active_clock_source_.store(initial_clock_source, std::memory_order_relaxed);
    crf_clock_source_index_ = crf_clock_source_index;
    if (!listener_specs_.empty()) {
        listener_ = std::make_unique<ListenerStreams>(
            config_.lock_tolerance_ns, sample_rate_, host_.components(), last_gptp_ns, audio_sink);
        listener_->set_presentation_offset_ns(config_.presentation_offset_ns);
    }

    // ATDECC descriptor wire version. Default "2016": emit descriptors at their
    // 1722.1-2013/2016 lengths so controllers that reject 2021-length descriptors
    // can enumerate the model. "2021" emits the full 2021 forms.
    host_.components().aem_handler.set_legacy_2016(config_.atdecc_version != "2021");

    // Base the talker stream_ids on the NIC MAC (globally unique per box), and
    // seed the static dest MACs by kind (MAAP mode reassigns after acquisition).
    ieee::Eui48 const stream_base_mac = stream_base_mac_for(config_.interface_name, config_.entity_id);
    for (auto const& spec : talker_specs_) {
        ieee::Eui48 dest{};
        switch (spec.format.kind) {
            case StreamKind::am824:
                dest = config_.am824_talker_dest_mac;
                break;
            case StreamKind::aaf:
                dest = config_.aaf_talker_dest_mac;
                break;
            case StreamKind::crf:
                dest = config_.crf_talker_dest_mac;
                break;
            case StreamKind::other:
            default:
                break;
        }
        (void)host_.components().acmp_talker.configure_stream(spec.index, stream_id_for(stream_base_mac, spec.index), dest);
    }

    // The VID itself is registered at start() (stop() withdraws it).
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    host_.components().msrp_handler.set_redeclare_registered_listeners(config_.redeclare_registered_listeners);
    host_.components().msrp_handler.set_suppress_leaveall(config_.suppress_leaveall);

    // Storage-handler built-in hooks (all reactor thread):
    // Per-control dispatch (kit phase 4): the generic CONTROL built-in calls
    // back with each accepted SET_CONTROL; bound symbols get their handler's
    // verdict, everything else is accepted (store/serve only).
    storage_handler_.set_on_control_changed([this](uint16_t control_index, std::span<uint8_t const> value) -> uint8_t {
        for (auto& binding : control_bindings_) {
            if (binding.resolved && binding.control_index == control_index && binding.fn) {
                return binding.fn(value);
            }
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
    // Kit phase 5: a controller's STOP_STREAMING gates the talker slot
    // (the media thread treats it as a closed SRP gate); START reopens.
    // The change shows in GET_STREAM_INFO as STREAMING_WAIT (IEEE 1722.1-2021
    // Table 7-145), so every subscribed controller is told by an unsolicited
    // GET_STREAM_INFO response (7.5.2), beside the command's own echo.
    storage_handler_.set_on_streaming_changed([this](uint16_t type, uint16_t index, bool streaming) -> uint8_t {
        if (type == DESCRIPTOR_STREAM_OUTPUT) {
            talker_->set_stream_stopped(index, !streaming);
            (void)host_.components().aem_handler.notify_stream_info_changed(type, index);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
    // Kit phase 3c: a controller's SET_CLOCK_SOURCE switches the active
    // source; the media thread reads it per tick (media_rate()).
    storage_handler_.set_on_clock_source_changed([this](uint16_t domain, uint16_t source) -> uint8_t {
        if (domain == 0) {
            active_clock_source_.store(source, std::memory_order_release);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
}

void AvbEntityKit::on_control_symbol(uint32_t const symbol_code, ControlChangedFn fn)
{
    ControlBinding binding{};
    binding.symbol = symbol_code;
    binding.fn = std::move(fn);
    if (auto const entry = host_.descriptor_for_symbol(symbol_code);
        entry.has_value() && entry->descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL) {
        binding.control_index = entry->descriptor_index;
        binding.resolved = true;
    }
    for (auto& existing : control_bindings_) {
        if (existing.symbol == symbol_code) {
            existing = std::move(binding);
            return;
        }
    }
    if (control_bindings_.size() < MAX_CONTROL_BINDINGS) {
        control_bindings_.push_back(std::move(binding));
    }
}

auto AvbEntityKit::talker_kind_of(uint16_t const index) const noexcept -> StreamKind
{
    for (auto const& spec : talker_specs_) {
        if (spec.index == index) {
            return spec.format.kind;
        }
    }
    return StreamKind::other;
}

auto AvbEntityKit::admitted_talker_streams(int64_t const now_ns) const noexcept -> AdmittedStreams
{
    AdmittedStreams admitted{};
    for (auto const& spec : talker_specs_) {
        if (gate_.admitted(spec.index, now_ns)) {
            (void)admitted.try_push_back(spec.index);
        }
    }
    return admitted;
}

void AvbEntityKit::note_admission(int64_t const now_ns)
{
    auto admitted = admitted_talker_streams(now_ns);
    if (std::ranges::equal(admitted, admitted_)) {
        return;
    }
    admitted_ = admitted;
    if (on_admission_changed_) {
        on_admission_changed_(std::span<uint16_t const>{admitted_.data(), admitted_.size()});
    }
}

void AvbEntityKit::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, int64_t const now_ns)
{
    gate_.note_listener_ready(stream_id, ready, now_ns);
    note_admission(now_ns);
}

void AvbEntityKit::wire_stream_callbacks()
{  // NOLINT(statusbar-block-statements) the stream callback wiring, one install per hook; pre-dates the gate
    // MSRP reservations for the talker streams. The host declares the SR class
    // domain before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) {
        for (auto const& spec : talker_specs_) {
            (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
        }
        // No Talker Advertise, no streaming (Milan 1.2 5.3.7.3): close every
        // stream now, without the grace window.
        gate_.note_talker_withdrawn();
        note_admission(steady_now_ns());
    });
    // The transmit gate follows the MSRP Listener Ready / Ready Failed registered for our talker
    // streams (Milan v1.2 Section 5.3.7.3); ACMP connections play no part.
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { note_listener_ready(stream_id, ready, steady_now_ns()); });

    // ACMP: log talker connections (they no longer gate anything); drive the
    // listener (MSRP attach + mcast join) on listener connect/disconnect.
    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} ({}) CONNECTED by listener {:012x} unique_id {}",
                stream_index,
                logging::static_str(stream_kind_name(talker_kind_of(stream_index))),
                listener_entity_id.to_uint64(),
                listener_unique_id);
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            host_.ctl_log().status(
                "acmp: talker stream {} ({}) DISCONNECTED by listener {:012x} unique_id {}",
                stream_index,
                logging::static_str(stream_kind_name(talker_kind_of(stream_index))),
                listener_entity_id.to_uint64(),
                listener_unique_id);
        });
    if (listener_ != nullptr) {
        host_.components().acmp_listener.set_connection_callbacks(
            [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac, TimePoint now) {
                listener_->on_listener_connected(stream_index, stream_id, dest_mac, now);
            },
            [this](uint16_t stream_index, TimePoint now) { listener_->on_listener_disconnected(stream_index, now); });
    }

    // AECP GET_COUNTERS (STREAM_OUTPUT talker rate + STREAM_INPUT listener
    // health) and GET_STREAM_INFO (talker stream identity, queried by a Milan
    // listener to verify the stream before sustaining a connection; listener
    // sink identity so a controller can read what an input is connected to).
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                return fill_talker_stream_counters(*talker_, descriptor_index, valid, counters);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                return listener_->fill_stream_input_counters(descriptor_index, valid, counters);
            }
            return false;
        });
    host_.components().aem_handler.set_get_stream_info(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, atdecc::aem::AemStreamInfoPayload& out) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                return fill_talker_stream_info(
                    host_, descriptor_index, config_.presentation_offset_ns, talker_->stream_stopped(descriptor_index), out);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                return fill_listener_stream_info(host_, descriptor_index, out);
            }
            return false;
        });
}

auto AvbEntityKit::acquire_maap_addresses(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices{};
    for (auto const& spec : talker_specs_) {
        (void)indices.try_push_back(spec.index);
    }
    // On (re)defend: re-declare the MSRP Talker Advertise so listeners
    // reserve against the MAAP address, once the SR-class domain is up.
    return maap_.acquire(
        reactor,
        config_.interface_name,
        stream_base_mac_for(config_.interface_name, config_.entity_id),
        host_.components(),
        *talker_,
        indices,
        host_.ctl_log(),
        [this](TimePoint const time) {
            if (host_.is_ready()) {
                advertise_talker_streams(time);
            }
        },
        now);
}

auto AvbEntityKit::start(net::MessageReactor& reactor, TimePoint const now) -> Status
{  // NOLINT(statusbar-block-statements) the start sequence, one step after another; pre-dates the gate
    // Bring up the shared control plane (net handlers + generic SM wiring),
    // then attach the stream-specific callbacks (hooks + ACMP + AEM handlers).
    gate_.set_logger(host_.ctl_log());
    // A restarted kit reports its first admitted set afresh.
    admitted_.clear();
    if (listener_ != nullptr) {
        listener_->set_logger(host_.ctl_log());
    }
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();
    // The VID, registered on every start: stop() withdraws it with the rest.
    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, now);

    // The stream port, backend by name. Not fatal: an entity whose port
    // would not open keeps its control plane and has no streams, which is
    // what it did before when the socket would not open.
    if (auto status = stream_port_.open(
            {.backend = config_.net_backend,
             .interface_name = config_.interface_name,
             .stream_pcp = config_.stream_pcp,
             .xdp_queue = config_.net_xdp_queue});
        !status) {
        host_.ctl_log().error(
            "stream port: {} did not open: {}",
            logging::embed<8>(config_.net_backend),
            logging::embed<40>(status.error().message()));
    } else {
        host_.ctl_log().status(
            "stream port: {}{}",
            logging::embed<48>(stream_port_.describe()),
            stream_port_.serialized() ? logging::lit(", TX and RX serialised") : logging::lit(""));
    }

    // In "maap" mode, claim the talker stream destination addresses via MAAP and
    // overwrite the static defaults BEFORE the data plane reads them below.
    // Non-fatal inside: on socket failure the entity keeps the static MACs.
    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor, now); !status) {
            return status;
        }
    }

    // One TX slot per blob-declared stream, shaped by its format word, with
    // the stream identity (id + dest MAC) the ACMP model carries.
    for (auto const& spec : talker_specs_) {
        statusbar::tsn::StreamId sid{};
        ieee::Eui48 dest{};
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
            (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
            dest = s->stream_dest_mac;
        }
        if (auto status = talker_->open_stream(spec, sid, dest); !status) {
            return status;
        }
    }

    // RX slots per blob-declared stream input. CRF inputs feed the media-clock
    // recovery (kit phase 3c); the ACMP listener callbacks (wired above) drive
    // the MSRP attach + the multicast join per connection.
    if (listener_ != nullptr) {
        // Static RX groups: an audio input pre-joins the same-kind talker
        // output's group (the loopback topology listens on its own output
        // group); everything else joins dynamically on ACMP connect (a zero
        // MAC just opens the socket).
        sg14::inplace_vector<ieee::Eui48, MAX_ENTITY_STREAMS> rx_groups{};
        for (auto const& spec : listener_specs_) {
            if (auto status = listener_->open_stream(spec); !status) {
                return status;
            }
            if (spec.format.kind == StreamKind::crf) {
                listener_->set_crf(spec.index, crf_recovery_.make_consumer());
            }
            ieee::Eui48 group{};
            bool const is_audio = spec.format.kind == StreamKind::am824 || spec.format.kind == StreamKind::aaf;
            if (auto const* slot = is_audio ? talker_->slot_of(spec.format.kind) : nullptr; slot != nullptr) {
                group = slot->dest_mac;
            }
            (void)rx_groups.try_push_back(group);
        }
        // In RT-timer mode the returned handler is kept alive here; the tool's
        // SCHED_FIFO RX timer drains it via drain_stream_rx() with its wake gPTP
        // time, so RX can't be starved by the control plane. rx_to_reactor()
        // is the way back should that timer not start.
        if (auto rx = listener_->attach_rx(stream_port_, rx_groups, reactor, config_.stream_rx_rt_timer)) {
            rt_rx_handler_ = std::move(*rx);
        } else {
            stream_rx_error_ = rx.error();  // tolerated here; a host may refuse (stream_rx_error())
        }
        // Persisted fast-connect bindings (kit phase 5b): a restarted entity
        // re-connects its inputs without a controller (see AvbEntityHost).
        host_.enable_listener_binding_persistence(config_.listener_bindings_path);
    }

    // TX on the stream port + optional pcap capture.
    // Tolerated here (the entity stays a control-plane-only node); recorded
    // for a host that needs the data plane (stream_tx_error()).
    if (auto tx = talker_->open_tx(
            stream_port_,
            TalkerStreams::TxPcapConfig{
                .path = config_.tx_pcap_path, .max_bytes = config_.tx_pcap_max_bytes, .seconds = config_.tx_pcap_seconds});
        !tx) {
        stream_tx_error_ = tx.error();
    }
    // The TX kick off the media thread (the tool's kicker timer, or its RX
    // timer's tick). The tool must then run that timer, or call
    // kick_from_media_tick() / rx_to_reactor() when it cannot.
    bool const kick_elsewhere = config_.stream_tx_kicker || (config_.stream_tx_kick_in_rx_timer && config_.stream_rx_rt_timer);
    if (!talker_->set_external_kick(kick_elsewhere)) {
        host_.ctl_log().warning("stream_tx.kicker: no stream port to kick; kicking in the media tick");
    }

    // Menu/selection diagnostics: registrations the model left inert. Status
    // level -- typo-finding, never an error.
    for (auto const& binding : control_bindings_) {
        if (!binding.resolved) {
            host_.ctl_log().status("control: registered symbol 0x{:08x} not a CONTROL in this model (inert)", binding.symbol);
        }
    }

    return success();
}

void AvbEntityKit::rx_to_reactor(net::MessageReactor& reactor)
{
    if (rt_rx_handler_ != nullptr) {
        reactor.add(std::move(rt_rx_handler_));
    }
    if (config_.stream_tx_kick_in_rx_timer) {
        kick_from_media_tick();
    }
}

auto AvbEntityKit::stop(TimePoint const now) -> Status
{
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    // The data plane first, once the tool has stopped the timers that drive
    // it, then the control plane. start() builds both again.
    close_streams();
    return host_.stop_control_plane(now);
}

void AvbEntityKit::close_streams() noexcept
{
    rt_rx_handler_.reset();
    talker_->close_tx();
    if (listener_ != nullptr) {
        listener_->close_rx();
    }
    stream_port_.close();
}

auto AvbEntityKit::make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{};
    if (auto const* stream = host_.components().acmp_talker.get_stream(spec.index); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }
    // TSpec from the format word (kit phase 1) — no per-entity byte math to
    // keep in step with the model; the hardware shaper sizes by the same one.
    auto const tspec = talker_tspec(spec);
    info.max_interval_frames = tspec.max_interval_frames;
    info.max_frame_size = tspec.max_frame_size;
    info.accumulated_latency = 0;
    return info;
}

void AvbEntityKit::advertise_talker_streams(TimePoint const time)
{
    // Defer until the stream destination MACs are final. In MAAP mode they are
    // acquired asynchronously after gPTP lock (which first triggers this), so
    // advertising a pre-MAAP dest would not match the MAAP address ACMP hands the
    // listener -> AskingFailed. on_acquired re-calls this once the block is defended.
    if (!maap_.ready()) {
        return;
    }
    int64_t const now_ns = steady_now_ns();
    for (auto const& spec : talker_specs_) {
        auto const info = make_talker_srp_info(spec);
        auto result = host_.components().msrp_handler.talker_advertise(info, time);
        if (!result) {
            host_.ctl_log().warning("msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
            continue;
        }
        // Re-learn the gate's listener state from the live registration: a
        // withdraw forgot it, and a listener whose declaration stayed
        // registered throughout raises no fresh MSRP event.
        bool const permits = host_.components().msrp_handler.listener_permit_debug(info.stream_id).permits;
        gate_.note_listener_ready(info.stream_id, permits, now_ns);
    }
    note_admission(now_ns);
}

}  // namespace statusbar::avb_entity
