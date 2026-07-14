#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Stereo I/O Module
/// Defines an AVB entity with one stereo 48 kHz input stream (listener)
/// and one stereo 48 kHz output stream (talker) using AM824 format.
/// Audio flows from listener through DSP biquad filters to talker.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_message_pipe.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace statusbar::avb_entity {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

/// Configuration for AVB Stereo I/O Entity
struct AvbEntityStereoIOConfig
{
    /// Entity ID (EUI-64) - unique identifier for this entity
    ieee::Eui64 entity_id{};

    /// Entity Model ID (EUI-64) - identifies the entity model/class
    ieee::Eui64 entity_model_id{};

    /// Network interface name (e.g., "en0" on macOS, "eth0" on Linux)
    std::string interface_name{"en0"};

    /// Destination multicast MAC address for talker stream (no MAAP)
    ieee::Eui48 talker_dest_mac{0x71, 0xB3, 0xD5, 0xED, 0xCF, 0x00};

    /// VLAN ID for AVB streams (default: SR Class A VLAN 2)
    uint16_t vlan_id{2};

    /// DSP filter center frequency (Hz)
    double filter_freq_hz{1000.0};

    /// DSP filter gain (dB) - negative for cut, positive for boost
    double filter_gain_db{-12.0};

    /// DSP filter Q factor
    double filter_q{2.0};

    /// Entity name (displayed in ATDECC controllers)
    std::string entity_name{"AVB Stereo IO"};

    /// Firmware version string
    std::string firmware_version{"1.0.0"};

    /// Gate transmit on an admitted downstream (ACMP connection + MSRP Listener Ready).
    /// See AvbEntityAudioIOConfig::gate_talker_on_listener. Default true.
    bool gate_talker_on_listener{true};

    /// MEDIA_LOCKED detector tolerance in ns for the STREAM_INPUT health counters.
    uint32_t lock_tolerance_ns{5'000};

    /// Drain the AM824 stream RX on a dedicated SCHED_FIFO timer (its own isolated
    /// core) instead of the shared reactor -- see AvbEntityAudioIOConfig::stream_rx_rt_timer.
    bool stream_rx_rt_timer{false};
    int stream_rx_cpu_affinity{2};     ///< isolated core for the RX timer (media timer = 3)
    uint32_t stream_rx_period_us{50};  ///< RX drain tick
};

/// Audio processing callback type
/// Called with interleaved stereo samples (left, right, left, right, ...)
/// @param samples Input/output sample buffer (modified in place)
/// @param sample_count Number of stereo sample pairs
using AudioProcessCallback = statusbar::sg14::inplace_function<void(std::span<float> samples, size_t sample_count), 64>;

/// AVB Entity with stereo input/output streams and DSP processing
///
/// This class implements an AVB entity that:
/// - Receives audio via one stereo 48 kHz AM824 listener stream
/// - Processes audio through configurable DSP biquad filters
/// - Transmits processed audio via one stereo 48 kHz AM824 talker stream
///
/// The entity uses the nanoavb supervisor state machine to coordinate:
/// - gPTP synchronization
/// - MVRP VLAN registration
/// - MSRP stream reservation
/// - ACMP connection management
/// - ADP entity discovery
///
/// Example usage:
/// @code
///   AvbEntityStereoIOConfig config{
///       .entity_id = ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
///       .entity_model_id = ieee::Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
///       .interface_name = "en0",
///       .filter_gain_db = -12.0  // 12 dB cut at 1 kHz
///   };
///
///   AvbEntityStereoIO entity{config};
///
///   // Create reactor and PTP bridge externally
///   net::MessageReactor reactor{shutdown_flag, net::monotonic_ns};
///   auto bridge_result = ptpclient::setup_ptp_bridge(...);
///
///   // Start the entity
///   entity.start(reactor, *bridge_result);
///
///   // Run reactor loop
///   while (!shutdown) {
///       reactor.poll(100);
///   }
///
///   entity.stop();
/// @endcode
class AvbEntityStereoIO
{
  public:
    using TimePoint = sm::TimePoint;

    /// Stream format constants
    static constexpr uint32_t SAMPLE_RATE = 48000;
    static constexpr size_t CHANNELS = 2;
    static constexpr size_t SAMPLES_PER_PACKET = 6;

    /// One received packet of interleaved stereo audio, queued for loopback playout.
    /// POD (trivially copyable) so the lock-free itc pipe carries it by value; its gPTP
    /// presentation time is the pipe's per-entry activation time, not a field here.
    struct LoopbackBlock
    {
        std::array<float, SAMPLES_PER_PACKET * CHANNELS> samples{};
    };

    /// Presentation-time compensation buffer for the RX->TX loopback: an itc SPSC
    /// Timestamped pipe. The RX thread publishes each decoded block at its gPTP
    /// presentation time; the media-timer thread pops only blocks whose presentation
    /// time has elapsed (try_consume_due), so the pipe reclocks the source stream onto
    /// the local media clock -- no FIFO, no cross-thread lock. Capacity spans the
    /// presentation window (~2 ms @ 125 us = 16 packets; 32 gives headroom).
    static constexpr size_t LOOPBACK_PIPE_CAPACITY = 32;
    using LoopbackPipe = itc::MessagePipe<LoopbackBlock, LOOPBACK_PIPE_CAPACITY, itc::Policy::Timestamped>;

    /// Construct entity with configuration
    /// @param config Entity configuration including network, stream, and DSP settings
    explicit AvbEntityStereoIO(AvbEntityStereoIOConfig config);

    /// Destructor - stops entity if running
    ~AvbEntityStereoIO();

    // Non-copyable, non-movable (due to internal state machine references)
    AvbEntityStereoIO(AvbEntityStereoIO const&) = delete;
    auto operator=(AvbEntityStereoIO const&) -> AvbEntityStereoIO& = delete;
    AvbEntityStereoIO(AvbEntityStereoIO&&) = delete;
    auto operator=(AvbEntityStereoIO&&) -> AvbEntityStereoIO& = delete;

    /// Start the entity and add handlers to reactor
    /// @param reactor Message reactor (caller must run poll loop)
    /// @return Status indicating success or failure
    [[nodiscard]] auto start(net::MessageReactor& reactor) -> Status;

    /// Stop the entity and clean up resources
    /// @return Status indicating success or failure
    [[nodiscard]] auto stop() -> Status;

    /// Check if entity is running
    [[nodiscard]] auto is_running() const noexcept -> bool { return host_.is_running(); }

    /// Check if entity has reached Ready state (gPTP locked, VLAN registered)
    [[nodiscard]] auto is_ready() const noexcept -> bool { return host_.is_ready(); }

    /// Get current supervisor state as string
    [[nodiscard]] auto state_string() const -> std::string_view { return host_.state_string(); }

    /// Print current state of all state machines
    auto print_state() const -> void;

    /// Handle link up event (call from external link monitor)
    /// @param time Current time point for state machine transition
    auto on_link_up(TimePoint time) -> void;

    /// Handle link down event (call from external link monitor)
    /// @param time Current time point for state machine transition
    auto on_link_down(TimePoint time) -> void;

    /// Handle gPTP announce received (called internally from gPTP handler)
    /// @param time Current time point for state machine transition
    /// @param has_grandmaster True if a gPTP grandmaster is present on the network
    auto on_gptp_announce(TimePoint time, bool has_grandmaster) -> void;

    /// Handle timeout in waiting states (call periodically)
    /// @param time Current time point for timeout evaluation
    auto on_timeout(TimePoint time) -> void;

    /// Process one audio packet period on the media-timer (SCHED_FIFO) thread: pop the
    /// loopback blocks now due (their gPTP presentation time has elapsed) from the
    /// compensation pipe, run each through the DSP biquads, and transmit via TalkerStreams
    /// (gated). @p time is the media-timer wake (gPTP).
    auto process_audio(TimePoint time) -> void;

    /// Batch-drain the AM824 RX socket, decoding + publishing each block to the loopback
    /// pipe stamped with @p wake_gptp_ns. Called from the tool's dedicated SCHED_FIFO RX
    /// timer when stream_rx_rt_timer is set; a no-op when RX is on the reactor instead.
    auto drain_stream_rx(int64_t wake_gptp_ns) -> size_t { return (rt_rx_handler_ != nullptr) ? drain_rx(wake_gptp_ns) : 0; }

    /// Reactor-path RX drain: stamp frames with the media-timer gPTP (last_gptp_ns_).
    /// Called by the reactor StreamRxHandler when stream_rx_rt_timer is off.
    auto drain_stream_rx_reactor() -> size_t
    {
        return drain_rx(static_cast<int64_t>(last_gptp_ns_.load(std::memory_order_relaxed)));
    }

    /// Set custom audio processing callback (in addition to biquad filter)
    /// @param callback Function called with interleaved stereo samples for processing
    auto set_audio_callback(AudioProcessCallback callback) -> void { audio_callback_ = std::move(callback); }

    /// Get access to NanoAVB components (for advanced use)
    [[nodiscard]] auto components() -> nanoavb::NanoAvbComponents& { return host_.components(); }
    [[nodiscard]] auto components() const -> nanoavb::NanoAvbComponents const& { return host_.components(); }

    /// Get access to network handlers (for advanced use)
    [[nodiscard]] auto net_handlers() -> nanoavb::NanoAvbNetHandlers* { return host_.net_handlers(); }

    // --- Logging (see AvbEntityHost) --------------------------------------------
    [[nodiscard]] auto ctl_log_channel() noexcept -> logging::LogChannelBase& { return host_.ctl_log_channel(); }
    [[nodiscard]] auto media_log_channel() noexcept -> logging::LogChannelBase& { return host_.media_log_channel(); }
    void set_log_verbosity(logging::LogLevel const v) noexcept { host_.set_log_verbosity(v); }

    /// Get the configuration
    [[nodiscard]] auto config() const noexcept -> AvbEntityStereoIOConfig const& { return config_; }

    /// Reconfigure DSP filter parameters
    /// @param freq_hz Filter center frequency in Hz
    /// @param gain_db Filter gain in dB (negative for cut, positive for boost)
    /// @param q Filter Q factor (bandwidth)
    auto configure_filter(double freq_hz, double gain_db, double q) -> void;

  private:
    /// Create the entity model with stereo I/O descriptors (hand-built in code -- the
    /// canonical STATIC example; no AEM blob needed).
    [[nodiscard]] auto create_entity_model() const -> nanoavb::EntityModel;

    /// Attach this entity's stream-specific control-plane behavior to the host:
    /// the single-stream MSRP advertise/withdraw + the ACMP talker connection log.
    /// The host owns the generic SM wiring + lifecycle.
    auto wire_stream_callbacks() -> void;

    /// Build the MSRP talker reservation (TSpec) for our stereo AM824 stream,
    /// sourcing stream id / destination / VLAN from the ACMP-configured talker
    /// stream 0 so MSRP, ACMP, and the AVTP stream share one identity.
    [[nodiscard]] auto make_talker_srp_info() const -> nanoavb::TalkerStreamSrpInfo;

    /// Batch-drain the RX socket: for each AM824 frame, deserialize to interleaved stereo
    /// samples + their gPTP presentation time, publish the block to the loopback pipe at
    /// that presentation time, and tally STREAM_INPUT health counters against @p gptp_now_ns.
    /// Runs on the reactor / RX-timer thread (the pipe's producer). Returns frames drained.
    auto drain_rx(int64_t gptp_now_ns) -> size_t;

    /// Feed one decoded packet's inputs to the pure tally_stream_input_packet.
    void update_stream_input_counters(
        uint8_t seq, uint32_t avtp_ts, bool tv, bool tu, bool mr, bool format_ok, uint64_t samples_per_ch, int64_t gptp_now_ns);

    /// Fill the GET_COUNTERS bitmap + values for our single STREAM_INPUT (index 0).
    [[nodiscard]] auto fill_stream_input_counters(uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const
        -> bool;

    // --- Members (declaration order carries init dependencies) ------------------
    /// Configuration (declared first: host_ + talker_ read it).
    AvbEntityStereoIOConfig config_;

    /// Custom audio callback (optional; runs after the biquads in process_audio).
    AudioProcessCallback audio_callback_;

    /// The reusable AVB control plane (SMs + NanoAvbComponents + net handlers +
    /// lifecycle). Declared after config_ (the model is built from config_).
    AvbEntityHost host_;

    /// Channel count (fixed stereo) + DSP memory resource; declared before the buffer
    /// and TalkerStreams that reference them.
    size_t channels_{CHANNELS};
    std::pmr::memory_resource* mem_resource_{std::pmr::get_default_resource()};

    /// Interleaved stereo TX scratch (TalkerStreams reads it in transmit_am824). Sized
    /// SAMPLES_PER_PACKET*CHANNELS in the constructor.
    std::pmr::vector<float> audio_buffer_{mem_resource_};

    /// gPTP wake time published by process_audio; read by the RX tally for LATE/EARLY.
    std::atomic<uint64_t> last_gptp_ns_{0};

    /// Media clock for TX presentation timestamps (r=1.0, gPTP-locked -- stereo has no
    /// separate GPS media clock). Advanced once per media-timer wake.
    ptpclient::MediaClockGenerator media_clock_{};

    /// Shared TX path (48 kHz via TalkerStreamsConfig). Declared after config_/
    /// media_clock_/audio_buffer_/channels_/last_gptp_ns_/mem_resource_.
    TalkerStreams talker_{
        TalkerStreamsConfig{.sample_rate = SAMPLE_RATE, .vlan_id = config_.vlan_id, .stream_pcp = 3},
        media_clock_,
        audio_buffer_,
        channels_,
        last_gptp_ns_,
        mem_resource_};

    /// Per-stream transmit gate (ACMP connection + MSRP Listener Ready). After host_.
    TalkerGate gate_{config_.gate_talker_on_listener, host_.components()};

    /// RX AM824 deserialize context (emplaced in start() with the channel count).
    std::optional<avtp::Am824StreamInputContext> listener_in_{};

    /// IEEE 1722.1 STREAM_INPUT health counters for the single listener stream.
    StreamInputCounters stream_in_counters_{};

    /// Presentation-time compensation buffer (RX producer -> media-timer consumer).
    LoopbackPipe loopback_pipe_{};

    /// Borrowed RX socket (owned by the StreamRxHandler) + drain scratch. Set in start().
    net::RawnetContext* rx_sock_{nullptr};
    std::array<uint8_t, 2048> rx_buf_{};

    /// The stream RX socket handler. Default: moved into the reactor by start(). With
    /// stream_rx_rt_timer set it is kept HERE + drained by the tool's RX timer via
    /// drain_stream_rx(). Base type so the concrete handler stays private to the .cpp.
    std::unique_ptr<net::Pollable> rt_rx_handler_{};

    /// Resolved stream destination MAC (from ACMP talker stream 0).
    ieee::Eui48 stream_dest_mac_{};

    /// RX data-plane counters (published for status).
    itc::TelemetryCounter<uint64_t> rx_packets_{};
    itc::TelemetryCounter<uint64_t> rx_bad_{};

    // --- DSP -------------------------------------------------------------------
    dsp::BiQuad<float> biquad_left_{};
    dsp::BiQuad<float> biquad_right_{};
};

}  // namespace statusbar::avb_entity
