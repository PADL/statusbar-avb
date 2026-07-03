#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Stereo I/O Module
/// Defines an AVB entity with one stereo 48 kHz input stream (listener)
/// and one stereo 48 kHz output stream (talker) using AM824 format.
/// Audio flows from listener through DSP biquad filters to talker.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
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
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

    /// Process one audio packet period
    /// Called from PTP timer callback at packet rate (8000 Hz for 48 kHz audio)
    /// @param time Current PTP-synchronized time point for packet timestamping
    auto process_audio(TimePoint time) -> void;

    /// Set custom audio processing callback (in addition to biquad filter)
    /// @param callback Function called with interleaved stereo samples for processing
    auto set_audio_callback(AudioProcessCallback callback) -> void { audio_callback_ = std::move(callback); }

    /// Get access to NanoAVB components (for advanced use)
    [[nodiscard]] auto components() -> nanoavb::NanoAvbComponents& { return host_.components(); }
    [[nodiscard]] auto components() const -> nanoavb::NanoAvbComponents const& { return host_.components(); }

    /// Get access to network handlers (for advanced use)
    [[nodiscard]] auto net_handlers() -> nanoavb::NanoAvbNetHandlers* { return host_.net_handlers(); }

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

    /// Process received AM824 packet through DSP and queue for transmission
    /// @param packet Raw AM824 packet data received from listener stream
    /// @param time Receive time point for packet timing
    auto process_listener_packet(std::span<uint8_t const> packet, TimePoint time) -> void;

    // Member order optimized to minimize struct padding
    // (config_ must precede components_ for initialization dependency)

    /// Configuration
    AvbEntityStereoIOConfig config_;

    /// Custom audio callback (optional)
    AudioProcessCallback audio_callback_;

    /// The reusable AVB control plane: state machines + NanoAvbComponents + net
    /// handlers + lifecycle. This entity supplies only its single stereo stream +
    /// DSP and attaches them via host_.components() + the typed hooks. Declared
    /// after config_ (the model is built from config_ via create_entity_model()).
    AvbEntityHost host_;

    //
    // DSP Processing
    //

    /// Biquad filters (one per channel)
    dsp::BiQuad<float> biquad_left_{};
    dsp::BiQuad<float> biquad_right_{};

    /// Audio processing buffer (interleaved stereo)
    std::array<float, SAMPLES_PER_PACKET * CHANNELS> audio_buffer_{};

    //
    // Packet Handling
    //

    /// Talker sequence number
    uint8_t talker_sequence_num_{0};

    /// Talker data block count
    uint8_t talker_dbc_{0};

    /// Talker stream ID (derived from entity ID)
    ieee::Eui64 talker_stream_id_{};
};

}  // namespace statusbar::avb_entity
