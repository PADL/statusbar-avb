// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvbEntityKit methods — the Entity Construction Kit facade (refactor phase D;
// the wiring bodies come verbatim from AvbEntityToneGenerator/AvbEntityAudioIO,
// which each carried a copy).

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/net/net_interface_info.hpp"
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
    std::atomic<uint64_t>& last_gptp_ns,
    StreamRxAudioSink* audio_sink,
    std::pmr::memory_resource* memory_resource)
    : config_{config}
    , talker_specs_{talker_specs}
    , listener_specs_{listener_specs}
    , sample_rate_{sample_rate}
    , storage_handler_{handler}
    , host_{handler, default_adp_advertiser_config(), talker_specs_.size(), 4, listener_specs_.size(), config.milan}
    , gate_{config.gate_talker_on_listener, host_.components()}
    , declarations_{host_.components().profile.milan}
    , talker_{std::make_unique<TalkerStreams>(
          TalkerStreamsConfig{.sample_rate = sample_rate, .vlan_id = config.vlan_id, .stream_pcp = config.stream_pcp},
          media_clock,
          last_gptp_ns,
          memory_resource)}
{
    active_clock_source_.store(initial_clock_source, std::memory_order_relaxed);
    crf_clock_source_index_ = crf_clock_source_index;
    if (!listener_specs_.empty()) {
        listener_ = std::make_unique<ListenerStreams>(
            config_.lock_tolerance_ns, sample_rate_, host_.components(), last_gptp_ns, audio_sink);
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
        (void)host_.components().acmp_talker.configure_stream(
            spec.index, stream_id_for(stream_base_mac, spec.index), dest, config_.vlan_id);
        if (spec.index < talker_interface_.size()) {
            if (auto const desc = host_.get_descriptor(DESCRIPTOR_STREAM_OUTPUT, spec.index); desc.has_value()) {
                atdecc::aem::DescriptorStream stream_desc{};
                span_load_padded(stream_desc, *desc);
                talker_interface_[spec.index] = stream_desc.avb_interface_index.get();
                if (talker_interface_[spec.index] != 0) {
                    // The entity runs on one AVB interface (0): probes for this
                    // output will be refused INCOMPATIBLE_REQUEST.
                    host_.ctl_log().warning(
                        "kit: STREAM_OUTPUT {} names AVB_INTERFACE {}; the entity has only interface 0",
                        spec.index,
                        talker_interface_[spec.index]);
                }
            }
        }
    }
    // Static destination addresses are valid from the start; MAAP's once
    // its block is defended (start() and the acquirer's callbacks).
    for (auto const& spec : talker_specs_) {
        declarations_.note_address_valid(spec.index, config_.stream_address_mode != "maap");
    }
    // Milan v1.2 Section 5.3.7.6: every stream output starts at the
    // configured presentation offset (2 ms by default), within the range a
    // controller may set (0..0x7FFFFFFF ns).
    for (auto& offset : presentation_offset_ns_) {
        offset.store(default_presentation_offset_ns(), std::memory_order_relaxed);
    }
    // The name the entity was given (a saved one replaces it only while the
    // configuration still says this).
    configured_entity_name_ = std::string{storage_handler_.entity_name().as_string_view()};

    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, sm::Clock::now());
    host_.components().msrp_handler.set_domain(
        DomainInfo{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = config_.vlan_id});
    // The Class A VID in use (adopted MSRP Domain): tag stream frames with
    // it, and keep it on the NIC's VLAN filter for the listener streams (a
    // no-op until bind_listener_vlan()).
    host_.set_on_domain_vid_changed([this](uint16_t const vlan_id, TimePoint) {
        talker_->set_vlan_id(vlan_id);
#if defined(__linux__)
        listener_vlan_.follow(vlan_id);
#endif
    });
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
    storage_handler_.set_on_streaming_changed([this](uint16_t type, uint16_t index, bool streaming) -> uint8_t {
        if (type == DESCRIPTOR_STREAM_OUTPUT) {
            talker_->set_stream_stopped(index, !streaming);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
    // The data plane runs each stream in its spec's format at the kit's rate: SET_STREAM_FORMAT
    // and SET_SAMPLING_RATE take only those, so no descriptor reports (and no file keeps) a
    // format or rate the streams are not using.
    storage_handler_.set_on_stream_format_changed([this](uint16_t const type, uint16_t const index, uint64_t const format) -> uint8_t {
        auto const& specs = type == DESCRIPTOR_STREAM_OUTPUT ? talker_specs_ : listener_specs_;
        bool const current = std::ranges::any_of(
            specs, [&](StreamSpec const& spec) { return spec.index == index && spec.format_word == format; });
        return current ? atdecc::AEM_STATUS_SUCCESS : atdecc::AEM_STATUS_NOT_SUPPORTED;
    });
    storage_handler_.set_on_sampling_rate_changed([this](uint16_t, uint32_t const rate) -> uint8_t {
        return rate == sample_rate_ ? atdecc::AEM_STATUS_SUCCESS : atdecc::AEM_STATUS_NOT_SUPPORTED;
    });
    // Kit phase 3c: a controller's SET_CLOCK_SOURCE switches the active
    // source; the media thread reads it per tick (media_rate()).
    storage_handler_.set_on_clock_source_changed([this](uint16_t domain, uint16_t source) -> uint8_t {
        if (domain == 0) {
            active_clock_source_.store(source, std::memory_order_release);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    });
    // The Milan talker's ACMP and AEM hooks need no network.
    wire_milan_talker();
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
    auto const idx = talker_index_of(stream_id);
    // Only a declared stream streams (Milan v1.2 Section 5.3.7.3): a Listener
    // registered for a withdrawn stream opens nothing; re-declaring re-learns it.
    if (!idx || declarations_.declared(*idx)) {
        gate_.note_listener_ready(stream_id, ready, now_ns);
    }
    // Any Listener registration keeps the stream declared (Section 4.3.3.1),
    // and GET_STREAM_INFO reports REGISTERING / REGISTERING_FAILED.
    if (idx && refresh_talker_listener(*idx, now_ns)) {
        bool const was_declared = declarations_.declared(*idx);
        update_talker_declarations(now_ns);
        if (declarations_.declared(*idx) == was_declared) {
            notify_talker_stream_info(*idx);  // else the (un)declaration notified
        }
    }
    note_admission(now_ns);
}

auto AvbEntityKit::is_talker_index(uint16_t const idx) const noexcept -> bool
{
    return std::ranges::any_of(talker_specs_, [idx](StreamSpec const& spec) { return spec.index == idx; });
}

auto AvbEntityKit::talker_index_of(nanoavb::StreamId const& stream_id) const noexcept -> std::optional<uint16_t>
{
    uint64_t const sid = stream_id.to_uint64();
    for (auto const& spec : talker_specs_) {
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index);
            s != nullptr && s->stream_id.to_uint64() == sid) {
            return spec.index;
        }
    }
    return std::nullopt;
}

auto AvbEntityKit::refresh_talker_listener(uint16_t const idx, int64_t const now_ns) -> bool
{
    if (idx >= talker_listener_.size()) {
        return false;
    }
    auto const* s = host_.components().acmp_talker.get_stream(idx);
    if (s == nullptr) {
        return false;
    }
    statusbar::tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
    auto const listener = host_.components().msrp_handler.registered_listener(sid);
    bool const changed = listener != talker_listener_[idx];
    talker_listener_[idx] = listener;
    declarations_.note_listener_registered(idx, listener != srp::msrp::ListenerDeclaration::Ignore, now_ns);
    return changed;
}

void AvbEntityKit::notify_talker_stream_info(uint16_t const idx)
{
    (void)host_.components().aem_handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, idx);
}

void AvbEntityKit::note_talker_probe(uint16_t const idx, int64_t const now_ns)
{
    if (!is_talker_index(idx)) {
        return;
    }
    if (!declarations_.probe_window_open(idx, now_ns)) {
        host_.ctl_log().status(
            "acmp: talker stream {} ({}) probed (PROBE_TX): Talker Advertise wanted for 15 s",
            idx,
            logging::static_str(stream_kind_name(talker_kind_of(idx))));
    }
    declarations_.note_probe(idx, now_ns);
    update_talker_declarations(now_ns);
}

void AvbEntityKit::note_talker_addresses(bool const valid, int64_t const now_ns)
{
    std::array<bool, MAX_ENTITY_STREAMS> was_declared{};
    for (auto const& spec : talker_specs_) {
        declarations_.note_address_valid(spec.index, valid);
        was_declared[spec.index] = declarations_.declared(spec.index);
    }
    update_talker_declarations(now_ns);
    // The destination address changed: notify the streams whose declaration
    // did not (that notified already).
    for (auto const& spec : talker_specs_) {
        if (declarations_.declared(spec.index) == was_declared[spec.index]) {
            notify_talker_stream_info(spec.index);
        }
    }
}

void AvbEntityKit::update_talker_declarations(int64_t const now_ns, bool const refresh)
{
    // MSRP calls back on the reactor; a nested evaluation would act on the
    // table the outer one is walking: it is noted, and the outer one runs
    // again once its pass is done.
    if (updating_declarations_) {
        declarations_pending_ = true;
        declarations_pending_refresh_ = declarations_pending_refresh_ || refresh;
        return;
    }
    updating_declarations_ = true;
    run_talker_declarations(now_ns, refresh);
    for (int pass = 0; declarations_pending_ && pass < 4; ++pass) {
        bool const again_refresh = declarations_pending_refresh_;
        declarations_pending_ = false;
        declarations_pending_refresh_ = false;
        run_talker_declarations(now_ns, again_refresh);
    }
    declarations_pending_ = false;
    declarations_pending_refresh_ = false;
    updating_declarations_ = false;
    declaration_deadline_ns_ = declarations_.next_deadline(now_ns);
    note_admission(now_ns);
}

void AvbEntityKit::run_talker_declarations(int64_t const now_ns, bool const refresh)
{
    using Action = TalkerDeclarations::Action;
    auto& msrp = host_.components().msrp_handler;
    TimePoint const time = sm::Clock::now();
    for (auto const& spec : talker_specs_) {
        auto const info = make_talker_srp_info(spec);
        TalkerDeclarations::Params params{.dest_mac = info.dest_address, .vlan_id = info.vlan_id};
        if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
            params.stream_id = s->stream_id;
        }
        bool const was_declared = declarations_.declared(spec.index);
        switch (declarations_.evaluate(spec.index, now_ns, params, refresh)) {
            case Action::declare: {
                if (auto result = msrp.talker_advertise(info, time); !result) {
                    host_.ctl_log().warning(
                        "msrp: talker_advertise (stream {}) failed: errno {}", spec.index, result.error().value());
                    break;
                }
                declarations_.note_declared(spec.index, params);
                // Re-learn the gate from the live registration: a withdraw forgot it,
                // and a registration that stayed raises no fresh MSRP event.
                gate_.note_listener_ready(info.stream_id, msrp.listener_permit_debug(info.stream_id).permits, now_ns);
                (void)refresh_talker_listener(spec.index, now_ns);
                if (!was_declared) {
                    host_.ctl_log().status(
                        "msrp: talker stream {} ({}) Talker Advertise declared",
                        spec.index,
                        logging::static_str(stream_kind_name(spec.format.kind)));
                    notify_talker_stream_info(spec.index);
                }
                break;
            }
            case Action::withdraw:
                (void)msrp.talker_withdraw(info.stream_id, time);
                declarations_.note_withdrawn(spec.index, now_ns);
                // No Talker Advertise, no streaming (Milan v1.2 5.3.7.3).
                gate_.note_talker_withdrawn(spec.index);
                host_.ctl_log().status(
                    "msrp: talker stream {} ({}) Talker Advertise withdrawn",
                    spec.index,
                    logging::static_str(stream_kind_name(spec.format.kind)));
                notify_talker_stream_info(spec.index);
                break;
            case Action::none:
            default:
                break;
        }
    }
}

void AvbEntityKit::withdraw_talker_streams(TimePoint const time)
{
    for (auto const& spec : talker_specs_) {
        (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info(spec).stream_id, time);
    }
    int64_t const now_ns = steady_now_ns();
    bool any = false;
    for (auto const& spec : talker_specs_) {
        any = any || declarations_.declared(spec.index);
    }
    declarations_.set_active(false);
    declarations_.note_all_withdrawn(now_ns);
    declaration_deadline_ns_ = declarations_.next_deadline(now_ns);
    // No Talker Advertise, no streaming (Milan 1.2 5.3.7.3): close every
    // stream now, without the grace window.
    gate_.note_talker_withdrawn();
    note_admission(now_ns);
    if (any) {
        for (auto const& spec : talker_specs_) {
            notify_talker_stream_info(spec.index);
        }
    }
}

void AvbEntityKit::wire_stream_callbacks()
{
    // MSRP reservations for the talker streams. The host declares the SR class
    // domain before calling these, then advertises/withdraws on the MSRP cycle.
    host_.set_advertise_streams([this](TimePoint time) { advertise_talker_streams(time); });
    host_.set_withdraw_streams([this](TimePoint time) { withdraw_talker_streams(time); });
    // The transmit gate tracks the MSRP listener declarations against our talker
    // streams; that alone admits a stream (docs/AVB_ENTITY_KIT.md).
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { note_listener_ready(stream_id, ready, steady_now_ns()); });

    // ACMP: log talker connections (classic only: they no longer gate
    // anything); drive the listener (MSRP attach + mcast join) on listener
    // connect/disconnect.
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
            [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac) {
                listener_->on_listener_connected(stream_index, stream_id, dest_mac);
            },
            [this](uint16_t stream_index) { listener_->on_listener_disconnected(stream_index); });
    }

    // AECP GET_COUNTERS (STREAM_OUTPUT talker rate + STREAM_INPUT listener
    // health; ENTITY, AVB_INTERFACE and CLOCK_DOMAIN from the host) and GET_STREAM_INFO (talker stream identity, queried by a Milan
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
            return host_.fill_counters(descriptor_type, descriptor_index, valid, counters);
        });
    host_.components().aem_handler.set_get_stream_info(
        [this](
            uint16_t descriptor_type,
            uint16_t descriptor_index,
            atdecc::aem::AemStreamInfoPayload& out,
            atdecc::aem::AemStreamInfoMilanExtension& ext) -> bool {
            bool const milan = host_.components().profile.milan;
            if (descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
                if (!is_talker_index(descriptor_index)) {
                    return false;
                }
                TalkerStreamStatus const status{
                    .milan = milan,
                    .declaring = declarations_.declared(descriptor_index),
                    .listener = talker_listener_[descriptor_index],
                    .presentation_offset_ns = presentation_offset_ns(descriptor_index)};
                return fill_talker_stream_info(host_, descriptor_index, status, out, ext);
            }
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT && listener_ != nullptr) {
                return fill_listener_stream_info(host_, descriptor_index, milan, out, ext);
            }
            return false;
        });
}

void AvbEntityKit::wire_milan_talker()
{
    auto& components = host_.components();
    // Milan v1.2 Section 5.5.4: the ACMP talker answers from what only the kit
    // knows (MAAP's address, the registered Listener, the interface), and a
    // probe opens the stream's declaration window.
    components.acmp_talker.set_milan_callbacks(
        [this](uint16_t const idx) -> nanoavb::MilanTalkerStreamState {
            auto const* s = host_.components().acmp_talker.get_stream(idx);
            return nanoavb::MilanTalkerStreamState{
                .dest_mac_valid = declarations_.address_valid(idx) && s != nullptr && s->stream_dest_mac != ieee::Eui48{},
                .registering_failed = idx < talker_listener_.size() && declarations_.declared(idx) &&
                    talker_listener_[idx] == srp::msrp::ListenerDeclaration::AskingFailed,
                .avb_interface_index = idx < talker_interface_.size() ? talker_interface_[idx] : uint16_t{0}};
        },
        [this](uint16_t const idx, TimePoint) { note_talker_probe(idx, steady_now_ns()); });

    // The presentation time offset (Milan v1.2 Sections 5.4.2.9-10): refused
    // while the output is streaming and when the entity cannot apply it.
    components.aem_handler.set_presentation_offset_hooks(
        [this](uint16_t const idx) -> std::optional<uint32_t> {
            return is_talker_index(idx) ? std::optional<uint32_t>{presentation_offset_ns(idx)} : std::nullopt;
        },
        [this](uint16_t const idx, uint32_t const offset_ns) -> uint8_t {
            if (!on_presentation_offset_changed_) {
                return atdecc::AEM_STATUS_NOT_SUPPORTED;
            }
            // Streaming as Milan v1.2 Section 5.3.7.3 has it: declaring and a
            // Listener Ready / Ready Failed registered (the grace window
            // included), even with the transmit gate disabled.
            if (declarations_.declared(idx) && gate_.listener_admitted(idx, steady_now_ns())) {
                return atdecc::AEM_STATUS_STREAM_IS_RUNNING;
            }
            uint32_t const before = presentation_offset_ns(idx);
            if (offset_ns == before) {
                return atdecc::AEM_STATUS_SUCCESS;
            }
            if (auto const status = on_presentation_offset_changed_(idx, offset_ns); status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
            presentation_offset_ns_[idx].store(offset_ns, std::memory_order_relaxed);
            host_.ctl_log().status("aem: talker stream {} presentation offset {} -> {} ns", idx, before, offset_ns);
            return atdecc::AEM_STATUS_SUCCESS;
        });
}

auto AvbEntityKit::acquire_maap_addresses(net::MessageReactor& reactor) -> Status
{
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices{};
    for (auto const& spec : talker_specs_) {
        (void)indices.try_push_back(spec.index);
    }
    // On (re)defend the streams declare against the addresses; a conflict
    // withdraws them and a new block waits out 2 x LeaveAll (Milan v1.2 Table 5.3).
    return maap_.acquire(
        reactor,
        config_.interface_name,
        stream_base_mac_for(config_.interface_name, config_.entity_id),
        host_.components(),
        *talker_,
        indices,
        host_.ctl_log(),
        [this]() { note_talker_addresses(true, steady_now_ns()); },
        [this]() { note_talker_addresses(false, steady_now_ns()); });
}

auto AvbEntityKit::start(net::MessageReactor& reactor) -> Status
{
    // Bring up the shared control plane (net handlers + generic SM wiring),
    // then attach the stream-specific callbacks (hooks + ACMP + AEM handlers).
    gate_.set_logger(host_.ctl_log());
    // A restarted kit reports its first admitted set afresh.
    admitted_.clear();
    if (listener_ != nullptr) {
        listener_->set_logger(host_.ctl_log());
    }
    host_.set_local_atdecc_socket(config_.atdecc_socket_path, config_.atdecc_socket_mode);
    // The saved settings, before the entity shows itself to the network.
    load_persistent_state();
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();
    // A restarted kit whose control plane stayed Ready gets no advertise hook
    // again (stop() withdrew everything): advertise now.
    if (host_.is_ready()) {
        advertise_talker_streams(sm::Clock::now());
    }

    // In "maap" mode, claim the talker stream destination addresses via MAAP and
    // overwrite the static defaults BEFORE the data plane reads them below.
    // Non-fatal inside: on socket failure the entity keeps the static MACs.
    if (config_.stream_address_mode == "maap") {
        if (auto status = acquire_maap_addresses(reactor); !status) {
            return status;
        }
    }
    // Static addresses are valid at once, MAAP's when the block is defended. Until
    // then the configured default must not reach a listener (TALKER_DEST_MAC_FAIL).
    for (auto const& spec : talker_specs_) {
        declarations_.note_address_valid(spec.index, maap_.ready());
        if (!maap_.ready()) {
            if (auto const* s = host_.components().acmp_talker.get_stream(spec.index); s != nullptr) {
                (void)host_.components().acmp_talker.configure_stream(spec.index, s->stream_id, ieee::Eui48{}, s->stream_vlan_id);
            }
        }
    }
    // The declarations' timer: a probe window closing, a hold ending. It
    // reaches the kit through a token stop() (or the kit's end) drops, and
    // then finishes: the reactor discards it and a restart adds a new one.
    class DeclarationTimer final : public net::Pollable
    {
      public:
        explicit DeclarationTimer(std::weak_ptr<AvbEntityKit* const> kit) noexcept
            : kit_{std::move(kit)}
        {}
        [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
        void on_ready(int64_t /*now_ns*/) override {}
        void tick(int64_t /*now_ns*/) override
        {
            if (auto const kit = kit_.lock(); kit != nullptr) {
                (*kit)->on_declaration_timer(steady_now_ns());
            }
        }
        [[nodiscard]] auto finished() const noexcept -> bool override { return kit_.expired(); }

      private:
        std::weak_ptr<AvbEntityKit* const> kit_;
    };
    declaration_timer_token_ = std::make_shared<AvbEntityKit* const>(this);
    reactor.add(std::make_unique<DeclarationTimer>(declaration_timer_token_));

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
        // In RT-timer mode the returned handler (its socket) is kept alive
        // here; the tool's SCHED_FIFO RX timer drains it via drain_stream_rx()
        // with its wake gPTP time, so RX can't be starved by the control plane.
        if (auto rx = listener_->attach_rx(config_.interface_name, rx_groups, reactor, config_.stream_rx_rt_timer)) {
            rt_rx_handler_ = std::move(*rx);
        } else {
            stream_rx_error_ = rx.error();  // tolerated here; a host may refuse (stream_rx_error())
        }
#if defined(__linux__)
        // The VID's tagged stream frames must get past the NIC's VLAN filter.
        // Never fatal: without CAP_NET_ADMIN this only warns.
        // (A control bound before start() -- a test's -- is used as it is.)
        if (!listener_vlan_.bound()) {
            if (auto const ifindex = net::interface_index(config_.interface_name); !ifindex) {
                host_.ctl_log().warning(
                    "listener: no VLAN filter registration: no index for the interface: errno {}", ifindex.error().value());
            } else if (vlan_client_ == nullptr || !vlan_client_->is_open()) {
                auto client = std::make_unique<net::vlan::VlanLinkClient>();
                if (auto const opened = client->open(); !opened) {
                    host_.ctl_log().warning(
                        "listener: no VLAN filter registration: rtnetlink socket: errno {}", opened.error().value());
                } else {
                    vlan_client_ = std::move(client);
                    bind_listener_vlan(*vlan_client_, *ifindex);
                }
            } else {
                bind_listener_vlan(*vlan_client_, *ifindex);
            }
        }
#endif
        // Persisted fast-connect bindings (kit phase 5b): a restarted entity
        // re-connects its inputs without a controller (see AvbEntityHost),
        // kept in the persistent state when there is one.
        if (persistence_active_) {
            apply_saved_listener_bindings();
        } else {
            host_.enable_listener_binding_persistence(config_.listener_bindings_path);
        }
    }

    // TX socket + optional pcap capture (TalkerStreams assembles its own I/O).
    // Tolerated here (the entity stays a control-plane-only node); recorded
    // for a host that needs the data plane (stream_tx_error()).
    if (auto tx = talker_->open_tx(
            config_.interface_name,
            TalkerStreams::TxPcapConfig{
                .path = config_.tx_pcap_path, .max_bytes = config_.tx_pcap_max_bytes, .seconds = config_.tx_pcap_seconds});
        !tx) {
        stream_tx_error_ = tx.error();
    } else if (auto const priority = talker_->tx_priority_error(); priority) {
        host_.ctl_log().warning(
            "talker: SO_PRIORITY {} on the stream TX socket failed: errno {}; streams use the default TX queue",
            config_.stream_pcp,
            priority.value());
    }

    // Write what start() ended up with: an imported, fresh or pruned state
    // (nothing when the file already says it).
    persist();

    // Menu/selection diagnostics: registrations the model left inert. Status
    // level -- typo-finding, never an error.
    for (auto const& binding : control_bindings_) {
        if (!binding.resolved) {
            host_.ctl_log().status("control: registered symbol 0x{:08x} not a CONTROL in this model (inert)", binding.symbol);
        }
    }

    return success();
}

#if defined(__linux__)
void AvbEntityKit::bind_listener_vlan(net::vlan::VlanControl& control, int const ifindex)
{
    if (listener_ == nullptr) {
        return;
    }
    listener_vlan_.bind(control, config_.interface_name, ifindex);
    listener_vlan_.follow(host_.components().msrp_handler.domain().sr_class_vid);
}
#endif

auto AvbEntityKit::stop() -> Status
{
#if defined(__linux__)
    listener_vlan_.release();
#endif
    declaration_timer_token_.reset();  // the timer finishes
    if (persist_pending_) {
        persist();  // a listener goal change of this pass
    }
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    auto const now = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    // Stopping the control plane runs no withdraw hook: withdraw here, so a
    // restart starts undeclared, inactive and with every gate closed.
    withdraw_talker_streams(now);
    return host_.stop_control_plane(now);
}

void AvbEntityKit::on_declaration_timer(int64_t const now_ns)
{
    // A listener goal change during this pass is saved now, after its ACMP
    // response.
    if (persist_pending_) {
        persist();
    }
    if (declaration_deadline_ns_ && host_.is_running() && now_ns >= *declaration_deadline_ns_) {
        update_talker_declarations(now_ns);
    }
}

auto AvbEntityKit::default_presentation_offset_ns() const noexcept -> uint32_t
{
    return static_cast<uint32_t>(std::min<uint64_t>(config_.presentation_offset_ns, atdecc::aem::MILAN_PRESENTATION_OFFSET_MAX_NS));
}

auto AvbEntityKit::served_entity_model_id() const -> uint64_t
{
    atdecc::aem::DescriptorEntity entity{};
    nanoavb::DescriptorId const id{
        .ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0}, .symbol = 0};
    if (!storage_handler_.on_get_entity(id, entity)) {
        return config_.entity_model_id.to_uint64();
    }
    return entity.entity_model_id.to_uint64();
}

void AvbEntityKit::load_persistent_state()
{
    persistence_active_ = false;
    persistence_blocked_ = false;
    persist_pending_ = false;
    saved_bindings_.clear();
    if (config_.persistent_state_path.empty()) {
        return;
    }
    auto loaded = avb_entity::load_persistent_state(
        config_.persistent_state_path, served_entity_model_id(), listener_ != nullptr ? config_.listener_bindings_path : std::string{});
    auto log = host_.ctl_log();
    persistence_active_ = true;
    // Every later change is saved: AEM settings, the MVU values (the
    // listener bindings once the listener is up).
    host_.components().aem_handler.set_on_settings_changed([this](uint16_t) { persist(); });
    host_.components().mvu_handler.set_on_settings_changed([this] { persist(); });
    switch (loaded.outcome) {
        case PersistentStateOutcome::fresh:
            log.status("kit: no persistent state yet; starting from the model's defaults");
            break;
        case PersistentStateOutcome::loaded:
            // Unchanged by what follows, it needs no rewrite.
            persisted_text_ = serialize_persistent_state(loaded.state);
            break;
        case PersistentStateOutcome::imported:
            log.status(
                "kit: no persistent state yet; {} listener binding(s) imported from the listener bindings file, which is no "
                "longer written",
                loaded.state.listener_bindings.size());
            break;
        case PersistentStateOutcome::corrupt:
            log.warning(
                "kit: the persistent state file is corrupt (error {}); starting fresh, the old file kept as {}",
                loaded.error.value(),
                logging::embed<48>(loaded.bad_copy.empty() ? std::string_view{"(not kept)"} : std::string_view{loaded.bad_copy}));
            break;
        case PersistentStateOutcome::unreadable:
        case PersistentStateOutcome::newer_version:
            // Never overwrite what could not be read: run on the model's
            // defaults and write nothing this run.
            log.warning(
                "kit: the persistent state file {} (error {}); running on the model's defaults, saving nothing this run",
                logging::static_str(
                    loaded.outcome == PersistentStateOutcome::unreadable ? "cannot be read" : "is of a later version"),
                loaded.error.value());
            persistence_blocked_ = true;
            return;
        case PersistentStateOutcome::model_changed:
            log.warning(
                "kit: the persistent state belongs to entity model {:016x}, not this model {:016x}: discarded",
                loaded.discarded_model_id,
                loaded.state.entity_model_id);
            break;
    }
    apply_saved_settings(loaded.state);
    saved_bindings_ = std::move(loaded.state.listener_bindings);
}

void AvbEntityKit::apply_saved_settings(PersistentState const& saved)
{
    using atdecc::AEM_STATUS_SUCCESS;
    using atdecc::aem::DESCRIPTOR_AUDIO_UNIT;
    using atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN;
    auto log = host_.ctl_log();
    auto const id = [](uint16_t const type, uint16_t const index) {
        return nanoavb::DescriptorId{.ref = {.configuration_index = 0, .descriptor_type = type, .descriptor_index = index}, .symbol = 0};
    };
    // A refused value is dropped (the file is rewritten without it).
    auto const refused = [&log](std::string_view const what, uint16_t const type, uint16_t const index, uint8_t const status) {
        log.warning(
            "kit: saved {} of descriptor {}:{} refused (status {}): dropped", logging::embed<24>(what), type, index, status);
    };
    // Milan's restore order (lib_avb 70c93a1): rate, formats, clock source,
    // then the rest. Each goes through the handler's own validation.
    for (auto const& rate : saved.sampling_rates) {
        ieee::quadlet_t const value{rate.rate};
        if (auto const status = storage_handler_.on_set_descriptor_value(
                atdecc::AEM_COMMAND_SET_SAMPLING_RATE, id(DESCRIPTOR_AUDIO_UNIT, rate.audio_unit), make_const_span(value));
            status != AEM_STATUS_SUCCESS) {
            refused("sampling rate", DESCRIPTOR_AUDIO_UNIT, rate.audio_unit, status);
        }
    }
    for (auto const& format : saved.stream_formats) {
        ieee::Eui64 value{};
        value.from_uint64(format.format);
        if (auto const status = storage_handler_.on_set_descriptor_value(
                atdecc::AEM_COMMAND_SET_STREAM_FORMAT, id(format.descriptor_type, format.descriptor_index), make_const_span(value));
            status != AEM_STATUS_SUCCESS) {
            refused("stream format", format.descriptor_type, format.descriptor_index, status);
        }
    }
    for (auto const& source : saved.clock_sources) {
        // clock_source_index and the reserved doublet (AemClockSourcePayload's tail)
        std::array<uint8_t, 4> const value{
            static_cast<uint8_t>(source.clock_source >> 8U), static_cast<uint8_t>(source.clock_source & 0xFFU), 0, 0};
        if (auto const status = storage_handler_.on_set_descriptor_value(
                atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, id(DESCRIPTOR_CLOCK_DOMAIN, source.clock_domain), value);
            status != AEM_STATUS_SUCCESS) {
            refused("clock source", DESCRIPTOR_CLOCK_DOMAIN, source.clock_domain, status);
        }
    }
    bool const offsets_stand = saved.configured_presentation_offset_ns == default_presentation_offset_ns();
    if (!saved.presentation_offsets.empty() && !offsets_stand) {
        log.status("kit: the configured presentation offset changed: the saved ones are dropped");
    }
    for (auto const& offset : saved.presentation_offsets) {
        if (!offsets_stand) {
            break;
        }
        uint8_t status = atdecc::AEM_STATUS_NOT_SUPPORTED;
        if (is_talker_index(offset.stream_output) && offset.ns <= atdecc::aem::MILAN_PRESENTATION_OFFSET_MAX_NS &&
            on_presentation_offset_changed_) {
            status = on_presentation_offset_changed_(offset.stream_output, offset.ns);
        }
        if (status != AEM_STATUS_SUCCESS) {
            refused("presentation offset", DESCRIPTOR_STREAM_OUTPUT, offset.stream_output, status);
            continue;
        }
        presentation_offset_ns_[offset.stream_output].store(offset.ns, std::memory_order_relaxed);
    }
    for (auto const& name : saved.names) {
        // A saved entity name stands only while the configuration still
        // names the entity as it did when it was saved.
        if (name.descriptor_type == atdecc::aem::DESCRIPTOR_ENTITY && name.descriptor_index == 0 && name.name_index == 0 &&
            saved.configured_entity_name != configured_entity_name_) {
            log.status("kit: the configured entity name changed: the saved one is dropped");
            continue;
        }
        if (auto const status = storage_handler_.on_set_name(
                id(name.descriptor_type, name.descriptor_index), name.name_index, atdecc::aem::AtdeccString{name.value});
            status != AEM_STATUS_SUCCESS) {
            refused("name", name.descriptor_type, name.descriptor_index, status);
        }
    }
    auto& mvu = host_.components().mvu_handler;
    mvu.set_system_unique_id(saved.system_unique_id);
    for (auto const& mcr : saved.media_clock_references) {
        nanoavb::MvuCommandHandler::McrInfo const info{
            .user_mcr_prio = mcr.user_priority, .domain_name = nanoavb::mcr_domain_name(mcr.domain_name)};
        if (!mvu.set_mcr_info(mcr.clock_domain, info)) {
            log.warning("kit: saved media clock reference of CLOCK_DOMAIN {}: no such clock domain: dropped", mcr.clock_domain);
        }
    }
}

void AvbEntityKit::apply_saved_listener_bindings()
{
    auto& listener = host_.components().acmp_listener;
    size_t armed = 0;
    for (auto const& binding : saved_bindings_) {
        bool const known = std::ranges::any_of(
            listener_specs_, [&binding](StreamSpec const& spec) { return spec.index == binding.stream_input; });
        if (!known) {
            host_.ctl_log().warning("kit: saved listener binding of STREAM_INPUT {}: no such input: dropped", binding.stream_input);
            continue;
        }
        // An input whose format changed is not the input the binding was
        // made for.
        if (auto const format = storage_handler_.current_stream_format(
                {.configuration_index = 0, .descriptor_type = DESCRIPTOR_STREAM_INPUT, .descriptor_index = binding.stream_input});
            binding.format != 0 && (!format || format->to_uint64() != binding.format)) {
            host_.ctl_log().warning(
                "kit: saved listener binding of STREAM_INPUT {}: the input's format changed: dropped", binding.stream_input);
            continue;
        }
        ieee::Eui64 talker{};
        talker.from_uint64(binding.talker_entity_id);
        listener.set_fast_connect_goal(binding.stream_input, talker, binding.talker_unique_id);
        ++armed;
    }
    if (armed != 0) {
        host_.ctl_log().status("acmp: {} saved listener binding(s) loaded; fast-connect armed", armed);
    }
    saved_bindings_.clear();
    // Sticky from here on: every successful connect is remembered, every goal
    // change (connect, retarget, controller DISCONNECT) saved once the
    // reactor pass that made it is done (after the ACMP response).
    listener.enable_sticky_bindings();
    listener.set_on_goals_changed([this] { persist_pending_ = true; });
}

auto AvbEntityKit::collect_persistent_state() const -> PersistentState
{
    PersistentState state{};
    state.entity_model_id = served_entity_model_id();
    state.configured_entity_name = configured_entity_name_;
    state.configured_presentation_offset_ns = default_presentation_offset_ns();
    if (auto const name = storage_handler_.entity_name().as_string_view(); name != configured_entity_name_) {
        state.names.push_back(
            {.descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0, .name_index = 0, .value = std::string{name}});
    }
    storage_handler_.for_each_object_name_override(
        [&state](uint16_t const type, uint16_t const index, atdecc::aem::AtdeccString const& name) {
            state.names.push_back(
                {.descriptor_type = type, .descriptor_index = index, .name_index = 0, .value = std::string{name.as_string_view()}});
        });
    storage_handler_.for_each_runtime_stream_format([this, &state](uint16_t const type, uint16_t const index, ieee::Eui64 const& format) {
        // A stream's own format is the model's: nothing to keep.
        auto const& specs = type == DESCRIPTOR_STREAM_OUTPUT ? talker_specs_ : listener_specs_;
        bool const authored = std::ranges::any_of(
            specs, [&](StreamSpec const& spec) { return spec.index == index && spec.format_word == format.to_uint64(); });
        if (!authored) {
            state.stream_formats.push_back({.descriptor_type = type, .descriptor_index = index, .format = format.to_uint64()});
        }
    });
    storage_handler_.for_each_runtime_sampling_rate([&state](uint16_t const unit, uint32_t const rate) {
        state.sampling_rates.push_back({.audio_unit = unit, .rate = rate});
    });
    storage_handler_.for_each_runtime_clock_source([&state](uint16_t const domain, uint16_t const source) {
        state.clock_sources.push_back({.clock_domain = domain, .clock_source = source});
    });
    for (auto const& spec : talker_specs_) {
        if (auto const ns = presentation_offset_ns(spec.index); ns != default_presentation_offset_ns()) {
            state.presentation_offsets.push_back({.stream_output = spec.index, .ns = ns});
        }
    }
    if (listener_ != nullptr) {
        // Before the listener is up (start() arms them then), the saved ones.
        if (!saved_bindings_.empty()) {
            state.listener_bindings = saved_bindings_;
        } else {
            for (auto const& goal : host_.components().acmp_listener.fast_connect_goals()) {
                auto const format = storage_handler_.current_stream_format(
                    {.configuration_index = 0, .descriptor_type = DESCRIPTOR_STREAM_INPUT, .descriptor_index = goal.listener_unique_id});
                state.listener_bindings.push_back(
                    {.stream_input = goal.listener_unique_id,
                     .talker_entity_id = goal.talker_entity_id.to_uint64(),
                     .talker_unique_id = goal.talker_unique_id,
                     .format = format ? format->to_uint64() : 0});
            }
        }
    }
    auto const& mvu = host_.components().mvu_handler;
    state.system_unique_id = mvu.system_unique_id();
    nanoavb::MvuCommandHandler::McrInfo const defaults{.user_mcr_prio = mvu.default_mcr_priority()};
    for (size_t index = 0; index < mvu.clock_domain_count(); ++index) {
        if (auto const* info = mvu.mcr_info(index); info != nullptr && !(*info == defaults)) {
            auto const& field = info->domain_name;
            auto const length = static_cast<size_t>(std::ranges::find(field, '\0') - field.begin());
            state.media_clock_references.push_back(
                {.clock_domain = static_cast<uint16_t>(index),
                 .user_priority = info->user_mcr_prio,
                 .domain_name = std::string{field.data(), length}});
        }
    }
    return state;
}

void AvbEntityKit::persist()
{
    persist_pending_ = false;
    if (!persistence_active_) {
        return;
    }
    if (persistence_blocked_) {
        return;  // the file did not read at start: it is never overwritten this run
    }
    auto const state = collect_persistent_state();
    auto text = serialize_persistent_state(state);
    if (text == persisted_text_) {
        return;
    }
    if (auto const saved = save_persistent_state(config_.persistent_state_path, state); !saved) {
        host_.ctl_log().warning("kit: could not save the persistent state: error {}", saved.error().value());
        return;
    }
    persisted_text_ = std::move(text);
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

void AvbEntityKit::advertise_talker_streams(TimePoint const /*time*/)
{
    // The control plane advertises: each stream declares when TalkerDeclarations
    // says so, after its MAAP address and, under Milan, interest.
    declarations_.set_active(true);
    update_talker_declarations(steady_now_ns(), true);
}

}  // namespace statusbar::avb_entity
