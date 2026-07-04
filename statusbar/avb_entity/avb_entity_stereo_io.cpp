// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Stereo I/O Implementation
/// Implements the AvbEntityStereoIO class for stereo audio passthrough with DSP

#include "statusbar/avb_entity/avb_entity_stereo_io.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_handler.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/dsp/dsp_biquad.hpp"
#include "statusbar/dsp/dsp_vec_base.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_gptp_sm.hpp"
#include "statusbar/nanoavb/nanoavb_listener_engine_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_listener_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_talker_sm.hpp"
#include "statusbar/nanoavb/nanoavb_mvrp_sm.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/nanoavb/nanoavb_supervisor_sm.hpp"
#include "statusbar/nanoavb/nanoavb_talker_engine_sm.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace statusbar::avb_entity {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

//
// Constructor / Destructor
//

// ADP advertiser configuration for this entity (valid_time + reannounce).
static auto make_adp_config() -> AdpAdvertiserConfig
{
    AdpAdvertiserConfig adp_config{};
    adp_config.valid_time = 31;                                        // 62 seconds
    adp_config.reannounce_interval = std::chrono::milliseconds{5000};  // 5 seconds
    return adp_config;
}

AvbEntityStereoIO::AvbEntityStereoIO(AvbEntityStereoIOConfig config)
    // Build the control plane host in place from the hand-built model: 1 talker
    // stream (4 max listeners), 1 listener stream. create_entity_model() reads
    // config_ (constructed first); the host's NanoAvbComponents is non-movable.
    : config_{std::move(config)}
    , host_{create_entity_model(), make_adp_config(), 1, 4, 1}
{
    // Configure biquad filters for both channels
    configure_filter(config_.filter_freq_hz, config_.filter_gain_db, config_.filter_q);

    // Size the interleaved TX scratch (TalkerStreams reads it in transmit_am824).
    audio_buffer_.assign(SAMPLES_PER_PACKET * CHANNELS, 0.0F);

    // Post-construction setup that needs config-supplied values.
    auto const& entity = host_.components().entity_model.get_entity();

    // Configure talker stream 0 with the hardwired multicast destination MAC.
    ieee::Eui64 stream_id{};
    span_copy(stream_id.span().first(6), entity.entity_id.span().first(6));
    (void)host_.components().acmp_talker.configure_stream(0, stream_id, config_.talker_dest_mac);

    // Register the VLAN with MVRP and set the MSRP SR-class domain.
    (void)host_.components().mvrp_handler.register_vlan(config_.vlan_id, sm::Clock::now());
    host_.components().msrp_handler.set_domain(DomainInfo{
        .sr_class_id = 6,                   // SR Class A
        .sr_class_priority = 3,             // Priority 3
        .sr_class_vid = config_.vlan_id});  // VLAN ID
}

AvbEntityStereoIO::~AvbEntityStereoIO()
{
    // Destructors must not throw; catch_or_status contains any exception from
    // stop() (and compiles to a plain call under -fno-exceptions).
    (void)statusbar::catch_or_status(
        [&]() -> statusbar::Status {
            if (host_.is_running()) {
                (void)stop();
            }
            return {};
        },
        std::errc::io_error);
}

//
// Entity Model Creation
//

auto AvbEntityStereoIO::create_entity_model() const -> nanoavb::EntityModel
{
    using namespace statusbar::atdecc::aem;

    // Configure for minimal stereo I/O entity
    EntityModelConfig cfg;
    cfg.max_configurations = 1;
    cfg.max_stream_inputs = 1;
    cfg.max_stream_outputs = 1;
    cfg.max_avb_interfaces = 1;
    cfg.max_audio_units = 1;
    cfg.max_audio_clusters = 2;  // One for input, one for output

    EntityModel model{cfg};

    // Set up entity descriptor
    DescriptorEntity entity{};
    entity.entity_id = config_.entity_id;
    entity.entity_model_id = config_.entity_model_id;
    entity.entity_capabilities = atdecc::entity_capabilities::AEM_SUPPORTED | atdecc::entity_capabilities::CLASS_A_SUPPORTED |
        atdecc::entity_capabilities::GPTP_SUPPORTED;
    entity.talker_stream_sources.set(1);
    entity.talker_capabilities = atdecc::talker_capabilities::IMPLEMENTED | atdecc::talker_capabilities::AUDIO_SOURCE;
    entity.listener_stream_sinks.set(1);
    entity.listener_capabilities = atdecc::listener_capabilities::IMPLEMENTED | atdecc::listener_capabilities::AUDIO_SINK;
    entity.entity_name = AtdeccString{config_.entity_name.c_str()};
    entity.firmware_version = AtdeccString{config_.firmware_version.c_str()};
    entity.group_name = AtdeccString{"Stereo I/O"};
    entity.serial_number = AtdeccString{"000001"};
    entity.current_configuration.set(0);

    model.set_entity(entity);

    // Add configuration descriptor
    DescriptorConfiguration config_desc{};
    config_desc.object_name = AtdeccString{"Default"};
    config_desc.localized_description.set(0xFFFF);
    config_desc.descriptor_counts_count.set(0);
    (void)model.add_configuration(config_desc);

    // Add AVB interface descriptor
    DescriptorAvbInterface avb_if{};
    avb_if.object_name = AtdeccString{"AVB Port 1"};
    avb_if.localized_description.set(0xFFFF);
    // MAC address will be set at runtime from network interface
    (void)model.add_avb_interface(avb_if);

    // Stream format: AM824, 48 kHz, 2 channels (stereo)
    // Format is encoded in EUI-64: IEC 61883-6 AM824 format
    // See IEEE 1722.1-2021 Annex A.5
    ieee::Eui64 stream_format{};
    // IEC 61883-6 AM824 format indicator with sample rate and channel count
    // This is a simplified format encoding - proper implementation would use
    // the full format descriptor structure
    stream_format = ieee::Eui64{0x00, 0xA0, 0x02, 0x00, 0x02, 0x01, 0x00, 0x00};

    // Add stream input (listener) descriptor
    DescriptorStream stream_in{};
    stream_in.descriptor_type.set(DESCRIPTOR_STREAM_INPUT);
    stream_in.object_name = AtdeccString{"Stereo In"};
    stream_in.localized_description.set(0xFFFF);
    stream_in.clock_domain_index.set(0);
    stream_in.stream_flags.set(0);
    stream_in.current_format = stream_format;
    stream_in.avb_interface_index.set(0);
    stream_in.buffer_length.set(SAMPLES_PER_PACKET * CHANNELS * 4);  // 4 bytes per sample (AM824)
    (void)model.add_stream_input(stream_in);

    // Add stream output (talker) descriptor
    DescriptorStream stream_out{};
    stream_out.descriptor_type.set(DESCRIPTOR_STREAM_OUTPUT);
    stream_out.object_name = AtdeccString{"Stereo Out"};
    stream_out.localized_description.set(0xFFFF);
    stream_out.clock_domain_index.set(0);
    stream_out.stream_flags.set(0);
    stream_out.current_format = stream_format;
    stream_out.avb_interface_index.set(0);
    stream_out.buffer_length.set(SAMPLES_PER_PACKET * CHANNELS * 4);
    (void)model.add_stream_output(stream_out);

    return model;
}

//
// Stream-specific control-plane wiring (the host owns the generic SM wiring)
//

void AvbEntityStereoIO::wire_stream_callbacks()
{
    // One talker stream. The host declares the SR class domain for us before these
    // fire, then advertises/withdraws on the MSRP cycle. No transmit gate and no
    // listener hook here -- this entity's readiness is the host's generic SM drive.
    host_.set_advertise_streams([this](TimePoint time) {
        auto result = host_.components().msrp_handler.talker_advertise(make_talker_srp_info(), time);
        if (!result) {
            std::print(stderr, "Warning: MSRP talker_advertise failed: {}\n", result.error().message());
        }
    });
    host_.set_withdraw_streams(
        [this](TimePoint time) { (void)host_.components().msrp_handler.talker_withdraw(make_talker_srp_info().stream_id, time); });

    // ACMP: observe controller-initiated connections to our talker (diagnostic;
    // streaming is gated by MSRP listener-ready + gPTP).
    host_.components().acmp_talker.set_connection_callbacks(
        [](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} CONNECTED  by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
        },
        [](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} DISCONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
        });
}

//
// Start / Stop
//

auto AvbEntityStereoIO::start(net::MessageReactor& reactor) -> Status
{
    // Bring up the shared control plane (net handlers + generic SM wiring), then
    // attach this entity's stream-specific callbacks. The host has no data plane of
    // its own to set up here -- this entity's TX/RX runs through the nanoavb handlers.
    if (auto status = host_.start_control_plane(reactor, config_.interface_name); !status) {
        return status;
    }
    wire_stream_callbacks();

    // --- Stream data plane (one stereo 48 kHz AM824 loopback) ------------------
    // Resolve the talker stream identity (id + dest MAC) from ACMP stream 0 so the
    // AVTP stream, MSRP reservation, and ACMP all agree.
    statusbar::tsn::StreamId sid{};
    if (auto const* s = host_.components().acmp_talker.get_stream(0); s != nullptr) {
        (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &sid);
        stream_dest_mac_ = s->stream_dest_mac;
    }
    talker_.am824_dest_mac_ = stream_dest_mac_;

    // Deterministic media clock (r=1.0, gPTP-locked): supplies the smooth presentation
    // timestamp for the re-transmitted audio. The stream-output context adds ZERO extra
    // offset -- the timestamp handed to it is already the final presentation time.
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(SAMPLE_RATE), .presentation_offset_ns = 1'000'000}};

    // AM824 TX/RX contexts at 48 kHz stereo (single stream; no AAF/CRF).
    talker_.am824_out_.emplace(
        sid, avtp::Am824SampleRate::rate_48_khz, static_cast<uint8_t>(CHANNELS), /*presentation_offset_ns=*/0);
    listener_in_.emplace(avtp::Am824SampleRate::rate_48_khz, static_cast<uint8_t>(CHANNELS));

    // Transmit socket (media-timer egress). qdisc-bypass so we do not re-receive our
    // own stream frames on this host.
    (void)talker_.stream_tx_.open(config_.interface_name, avtp::AVTP_ETHERTYPE, nullptr, /*qdisc_bypass=*/true);

    // Receive port: join the stream multicast group; the entity decodes each AM824 frame
    // and publishes it to the loopback pipe (the pipe's producer). Same handler shape as
    // AvbEntityAudioIO / AvbEntityAm824IO.
    std::array<ieee::Eui48, 1> const rx_groups{stream_dest_mac_};
    auto rx = std::make_unique<StreamRxHandler>(config_.interface_name, rx_groups, [this] { drain_stream_rx_reactor(); });
    if (rx->valid()) {
        rx_sock_ = rx->socket();  // borrow before the move; used for dynamic listener joins
        if (config_.stream_rx_rt_timer) {
            rt_rx_handler_ = std::move(rx);  // kept alive; drained by the tool's SCHED_FIFO RX timer
        } else {
            reactor.add(std::move(rx));  // default: drained on the shared reactor thread
        }
    }

    // The per-stream transmit gate tracks MSRP Listener Ready + the ACMP connection
    // count. (Re-sets the acmp_talker callbacks wire_stream_callbacks installed for
    // logging, adding the gate publish -- the media timer never reads the reactor-
    // mutated connection list directly.)
    host_.set_on_listener_ready(
        [this](nanoavb::StreamId const& stream_id, bool ready) { gate_.note_listener_ready(stream_id, ready); });
    host_.components().acmp_talker.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} CONNECTED  by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        },
        [this](uint16_t stream_index, ieee::Eui64 listener_entity_id, uint16_t listener_unique_id) {
            std::print(
                "[acmp] talker stream {} DISCONNECTED by listener {:012x} unique_id {}\n",
                stream_index,
                listener_entity_id.to_uint64(),
                listener_unique_id);
            gate_.note_acmp_connections(
                stream_index, static_cast<uint32_t>(host_.components().acmp_talker.connection_count(stream_index)));
        });

    // Listener connect/disconnect -> MSRP Listener Ready + join/leave the talker's
    // multicast group on the RX socket (a talker uses a fresh dest MAC per connection).
    host_.components().acmp_listener.set_connection_callbacks(
        [this](uint16_t stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac) {
            tsn::StreamId lsid{};
            (void)statusbar::tsn::load_unchecked(stream_id.span(), &lsid);
            auto const result = host_.components().msrp_handler.listener_ready(lsid, sm::Clock::now());
            bool const joined = rx_sock_ != nullptr && rx_sock_->join_multicast(dest_mac).has_value();
            std::print(
                "[acmp] listener stream {} CONNECTED to talker dest={:012x} -> MSRP Listener Ready {}, mcast join {}\n",
                stream_index,
                dest_mac.to_uint64(),
                result.has_value() ? "declared" : "failed",
                joined ? "ok" : "FAILED");
        },
        [this](uint16_t stream_index) {
            if (auto const* stream = host_.components().acmp_listener.get_stream(stream_index); stream != nullptr) {
                tsn::StreamId lsid{};
                (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &lsid);
                (void)host_.components().msrp_handler.listener_withdraw(lsid, sm::Clock::now());
                if (rx_sock_ != nullptr) {
                    (void)rx_sock_->leave_multicast(stream->stream_dest_mac);
                }
            }
            std::print("[acmp] listener stream {} DISCONNECTED from talker -> MSRP Listener withdrawn\n", stream_index);
        });

    // AECP GET_COUNTERS: expose the IEEE 1722.1 STREAM_INPUT health counters
    // (Clause 7.4.42) for our single AM824 listener stream (index 0).
    host_.components().aem_handler.set_get_counters(
        [this](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type == DESCRIPTOR_STREAM_INPUT) {
                return fill_stream_input_counters(descriptor_index, valid, counters);
            }
            return false;
        });

    return success();
}

auto AvbEntityStereoIO::stop() -> Status
{
    if (!host_.is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }
    auto const now = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    return host_.stop_control_plane(now);
}

//
// State Queries
//

void AvbEntityStereoIO::print_state() const
{
    std::print(
        "State: supervisor={} gptp={} mvrp={} acmp_connections={} stream_tx={} rx_packets={} rx_bad={}\n",
        state_string(),
        host_.gptp_locked() ? "Locked" : "Unlocked",
        host_.mvrp_joined() ? "Joined" : "NotJoined",
        host_.components().acmp_talker.connection_count(0),
        talker_.am824_tx_packets_,
        rx_packets_.load(),
        rx_bad_.load());
}

//
// MSRP talker reservation
//

auto AvbEntityStereoIO::make_talker_srp_info() const -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{};

    // Source stream id / destination / VLAN from the ACMP-configured talker
    // stream 0 so MSRP advertises the exact same stream the controller sees.
    if (auto const* stream = host_.components().acmp_talker.get_stream(0); stream != nullptr) {
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &info.stream_id);
        info.dest_address = stream->stream_dest_mac;
        info.vlan_id = stream->stream_vlan_id;
    } else {
        info.vlan_id = config_.vlan_id;
    }

    // SR class A: one AVTP packet per 125 us measurement interval.
    info.max_interval_frames = 1;

    // max_frame_size is the L2 payload (MSDU): AVTP stream header (24) + CIP
    // header (8) = 32 bytes, then SAMPLES_PER_PACKET AM824 quadlets per channel
    // at 4 bytes each.
    constexpr uint16_t AVTP_AM824_HEADER_BYTES = 32;
    constexpr uint16_t AM824_QUADLET_BYTES = 4;
    info.max_frame_size = static_cast<uint16_t>(AVTP_AM824_HEADER_BYTES + (SAMPLES_PER_PACKET * CHANNELS * AM824_QUADLET_BYTES));

    // We are the media source; downstream bridges accumulate their own latency.
    info.accumulated_latency = 0;
    return info;
}

//
// Event Handlers
//

// The entity's events drive the shared SM stack, owned by the host.
void AvbEntityStereoIO::on_link_up(TimePoint time)
{
    host_.on_link_up(time);
}
void AvbEntityStereoIO::on_link_down(TimePoint time)
{
    host_.on_link_down(time);
}
void AvbEntityStereoIO::on_gptp_announce(TimePoint time, bool has_grandmaster)
{
    host_.on_gptp_announce(time, has_grandmaster);
}
void AvbEntityStereoIO::on_timeout(TimePoint time)
{
    host_.on_timeout(time);
}

//
// Audio Processing
//

void AvbEntityStereoIO::process_audio(TimePoint time)
{
    auto const gptp_now = static_cast<uint64_t>(time.time_since_epoch().count());
    // Publish this media-timer wake's gPTP time so the reactor-path RX drain stamps
    // ingress against it for the STREAM_INPUT LATE/EARLY classification.
    last_gptp_ns_.store(gptp_now, std::memory_order_relaxed);

    // Advance the deterministic media clock (r=1.0, gPTP-locked -- stereo has no
    // separate GPS media clock). first_index anchors the smooth presentation
    // timestamp used for the re-transmitted packets (jitter-free, offset ahead).
    auto const tick = media_clock_.advance(gptp_now, 1.0, static_cast<uint32_t>(SAMPLES_PER_PACKET));
    uint64_t const pts_base = media_clock_.timestamp_for(tick.first_index);

    // Steady-clock now for the MSRP-ready grace window (same clock note_listener_ready
    // stamps last-ready with); decide the gate once per wake.
    int64_t const now_steady_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    bool const gate_open = gate_.should_transmit(0, now_steady_ns);

    // Reclock every loopback block whose gPTP presentation time has elapsed onto our
    // local media clock. Both ends run 48 kHz gPTP so the pipe is naturally rate-matched
    // (typically one due block per wake), but drain ALL due blocks to be safe.
    while (auto const block = loopback_pipe_.try_consume_due(gptp_now)) {
        // Copy the interleaved stereo block into the TX scratch (TalkerStreams reads it
        // in transmit_am824). Layout is [s0L,s0R,s1L,s1R,...].
        for (size_t i = 0; i < SAMPLES_PER_PACKET * CHANNELS; ++i) {
            audio_buffer_[i] = block->samples[i];
        }
        // Process through biquad filters (even indices = left, odd = right).
        for (size_t i = 0; i < SAMPLES_PER_PACKET; ++i) {
            audio_buffer_[(i * 2)] = biquad_left_(audio_buffer_[(i * 2)]);
            audio_buffer_[(i * 2) + 1] = biquad_right_(audio_buffer_[(i * 2) + 1]);
        }
        // Call custom audio callback if set.
        if (audio_callback_) {
            audio_callback_(std::span{audio_buffer_}, SAMPLES_PER_PACKET);
        }
        // Re-transmit the processed audio, gated on an admitted downstream listener.
        if (gate_open) {
            talker_.transmit_am824(pts_base, static_cast<uint32_t>(SAMPLES_PER_PACKET));
        }
    }
}

void AvbEntityStereoIO::configure_filter(double const freq_hz, double const gain_db, double const q)
{
    double const sample_rate_recip = 1.0 / static_cast<double>(SAMPLE_RATE);

    // Configure peak EQ filter for both channels
    // Negative gain = cut, positive gain = boost
    biquad_left_.coeffs.calculate_peak(
        {.sample_rate_recip = sample_rate_recip, .frequency = freq_hz, .q = q, .gain_db = gain_db, .channel = 0});
    biquad_right_.coeffs.calculate_peak(
        {.sample_rate_recip = sample_rate_recip, .frequency = freq_hz, .q = q, .gain_db = gain_db, .channel = 0});

    // Reset filter states
    biquad_left_.reset();
    biquad_right_.reset();
}

//
// Stream RX data plane (RX-timer / reactor thread; loopback-pipe producer)
//

auto AvbEntityStereoIO::drain_rx(int64_t const gptp_now_ns) -> size_t
{
    if (rx_sock_ == nullptr || !listener_in_) {
        return 0;
    }
    // Drain every queued frame in one pass (arrival rate is 8000/s = 125 us apart; a
    // 50 us RX-timer tick or a reactor wake sees 0-3 frames). All frames in this pass
    // are stamped with the caller's fresh gPTP "now".
    ieee::Eui48 src{};
    ieee::Eui48 dst{};
    size_t drained = 0;
    while (true) {
        auto const r = rx_sock_->recv(&src, &dst, rx_buf_);
        if (!r || *r <= 0) {
            break;
        }
        ++drained;
        std::span<uint8_t const> const frame{rx_buf_.data(), static_cast<size_t>(*r)};

        // Stereo entity carries a single AM824 stream; ignore anything else on the group.
        if (frame.size() < avtp::Am824Pdu::HEADER_LENGTH || frame[0] != avtp::AvtpSubtype::iec_61883_iidc) {
            continue;
        }
        avtp::Am824Pdu pdu{};
        span_load(pdu, frame.first(avtp::Am824Pdu::HEADER_LENGTH));
        if (!pdu.is_valid()) {
            rx_bad_.add(1);
            update_stream_input_counters(0, 0, false, false, false, /*format_ok=*/false, 0, gptp_now_ns);
            continue;
        }

        // Deserialize the interleaved stereo block + its 64-bit gPTP presentation time.
        std::span<uint8_t const> const audio = frame.subspan(avtp::Am824Pdu::HEADER_LENGTH);
        LoopbackBlock block{};
        uint64_t base_pts = static_cast<uint64_t>(gptp_now_ns);
        uint64_t samples_this = 0;
        avtp::am824_deserialize_mbla(
            *listener_in_,
            pdu,
            audio,
            static_cast<uint64_t>(gptp_now_ns),
            [&block, &base_pts, &samples_this](uint8_t ch, std::span<float> s, uint64_t pts, uint64_t /*period*/) {
                base_pts = pts;
                samples_this = s.size();
                if (ch >= CHANNELS) {
                    return;
                }
                size_t const n = (s.size() < SAMPLES_PER_PACKET) ? s.size() : SAMPLES_PER_PACKET;
                for (size_t i = 0; i < n; ++i) {
                    block.samples[(i * CHANNELS) + ch] = s[i];
                }
            });
        rx_packets_.add(1);
        update_stream_input_counters(
            pdu.stream_header.sequence_num.get(),
            pdu.avtp_timestamp(),
            pdu.stream_header.tv(),
            pdu.stream_header.tu(),
            pdu.stream_header.mr(),
            /*format_ok=*/true,
            samples_this,
            gptp_now_ns);

        // Hand the block to the media-timer thread at its gPTP presentation time; the
        // pipe reclocks it onto our local media clock (no FIFO, no lock). A full pipe
        // (media timer wedged) drops the block rather than blocking the RX thread.
        (void)loopback_pipe_.try_publish(base_pts, block);
    }
    return drained;
}

void AvbEntityStereoIO::update_stream_input_counters(
    uint8_t const seq,
    uint32_t const avtp_ts,
    bool const tv,
    bool const tu,
    bool const mr,
    bool const format_ok,
    uint64_t const samples_per_ch,
    int64_t const gptp_now_ns)
{
    // ts_sparse=true: 61883-6 carries a valid AVTP timestamp only on SYT-bearing
    // packets, so tv=0 in between is normal (not a TIMESTAMP_NOT_VALID fault).
    tally_stream_input_packet(
        stream_in_counters_,
        seq,
        avtp_ts,
        tv,
        tu,
        mr,
        format_ok,
        samples_per_ch,
        /*ts_sparse=*/true,
        static_cast<uint64_t>(gptp_now_ns),
        config_.lock_tolerance_ns,
        SAMPLE_RATE);
}

auto AvbEntityStereoIO::fill_stream_input_counters(
    uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool
{
    if (descriptor_index != 0) {
        return false;
    }
    auto const& c = stream_in_counters_;
    auto const relaxed = std::memory_order_relaxed;
    // IEEE 1722.1 STREAM_INPUT counter bit positions (Clause 7.4.42). out[bit] holds the
    // value for the bit set in `valid`.
    auto set = [&](size_t bit, uint32_t v) {
        valid |= (1U << bit);
        out[bit] = v;
    };
    set(0, c.media_locked.load(relaxed));
    set(1, c.media_unlocked.load(relaxed));
    set(3, c.seq_num_mismatch.load(relaxed));
    set(4, c.media_reset.load(relaxed));
    set(5, c.timestamp_uncertain.load(relaxed));
    set(6, c.timestamp_valid.load(relaxed));
    set(7, c.timestamp_not_valid.load(relaxed));
    set(8, c.unsupported_format.load(relaxed));
    set(9, c.late_timestamp.load(relaxed));
    set(10, c.early_timestamp.load(relaxed));
    set(11, c.frames_rx.load(relaxed));
    return true;
}

}  // namespace statusbar::avb_entity
