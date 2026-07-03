#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity AM824 I/O Module
/// Defines an AVB entity with one N-channel 48 kHz input stream (listener)
/// and one N-channel 48 kHz output stream (talker) using AM824 format.
/// The entity model is loaded from a descriptor storage blob.
/// Audio flows from listener through DSP biquad filters to talker.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
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

/// Configuration for AVB AM824 I/O Entity
struct AvbEntityAm824IOConfig
{
    /// Entity ID (EUI-64) - unique identifier for this entity
    ieee::Eui64 entity_id{};

    /// Entity Model ID (EUI-64) - identifies the entity model/class
    ieee::Eui64 entity_model_id{};

    /// Descriptor storage blob (serialized AEM descriptors)
    std::vector<uint8_t> descriptor_storage_blob{};

    /// Network interface name (e.g., "en0" on macOS, "eth0" on Linux)
    std::string interface_name{"eth0"};

    /// Destination multicast MAC address for talker stream (no MAAP)
    ieee::Eui48 talker_dest_mac{0x71, 0xB3, 0xD5, 0xED, 0xCF, 0x00};

    /// VLAN ID for AVB streams (default: SR Class A VLAN 2)
    uint16_t vlan_id{2};

    /// DSP filter center frequency (Hz)
    double filter_freq_hz{1000.0};

    /// DSP filter gain (dB) - negative for cut, positive for boost
    double filter_gain_db{0.0};

    /// DSP filter Q factor
    double filter_q{0.707};

    /// Stream test-tone frequency (Hz) generated per channel. Default 96000/7.
    double tone_freq_hz{96000.0 / 7.0};

    /// Stream test-tone amplitude (0..1 linear).
    float tone_amplitude{0.5F};

    /// Entity name (displayed in ATDECC controllers)
    std::string entity_name{"AVB AM824 IO"};

    /// Firmware version string
    std::string firmware_version{"1.0.0"};
};

/// Audio processing callback type
/// Called with interleaved N-channel samples
/// @param samples Input/output sample buffer (modified in place)
/// @param sample_count Number of sample frames (each frame contains N channel samples)
using Am824AudioProcessCallback = statusbar::sg14::inplace_function<void(std::span<float> samples, size_t sample_count), 64>;

/// AVB Entity with N-channel AM824 input/output streams and DSP processing
///
/// This class implements an AVB entity that:
/// - Loads the entity model from a descriptor storage blob
/// - Receives audio via one N-channel 48 kHz AM824 listener stream
/// - Processes audio through configurable DSP biquad filters (one per channel)
/// - Transmits processed audio via one N-channel 48 kHz AM824 talker stream
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
///   AvbEntityAm824IOConfig config{
///       .entity_id = ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07},
///       .entity_model_id = ieee::Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77},
///       .descriptor_storage_blob = load_blob_from_file("entity.blob"),
///       .interface_name = "eth0",
///       .filter_gain_db = 0.0
///   };
///
///   auto result = AvbEntityAm824IO::create(std::move(config));
///   if (!result) { /* handle error */ }
///   auto entity = std::move(*result);
///
///   // Create reactor externally
///   net::MessageReactor reactor{shutdown_flag, net::monotonic_ns};
///
///   // Start the entity
///   entity.start(reactor);
///
///   // Run reactor loop
///   while (!shutdown) {
///       reactor.poll(100);
///   }
///
///   entity.stop();
/// @endcode
class AvbEntityAm824IO
{
  public:
    using TimePoint = sm::TimePoint;

    /// Stream format constants. The entity runs at 96 kHz to match the
    /// descriptor model and the third-party devices endpoints. SR class A uses a 125 us
    /// measurement interval (8000 packets/s), so samples-per-packet is the
    /// sample rate divided by that — 96000/8000 = 12 (was 6 at 48 kHz).
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;
    static constexpr uint32_t SAMPLE_RATE = 96000;
    static constexpr size_t SAMPLES_PER_PACKET = SAMPLE_RATE / CLASS_A_PACKETS_PER_SEC;

    /// AVTP presentation-time offset ahead of the capture/gPTP clock (2 ms).
    static constexpr uint64_t PRESENTATION_OFFSET_NS = 2'000'000;

    /// Factory method — constructs and validates the entity from configuration
    /// Parses the descriptor storage blob to determine channel count and entity model
    /// @param config Entity configuration including network, stream, and DSP settings
    /// @param memory_resource Memory resource for the per-channel DSP
    ///        buffers (biquads_ and audio_buffer_). nullptr is treated
    ///        as std::pmr::get_default_resource().
    /// @return Heap-allocated entity on success, or error code on invalid blob
    [[nodiscard]] static auto create(AvbEntityAm824IOConfig config, std::pmr::memory_resource* memory_resource = nullptr)
        -> StatusValue<std::unique_ptr<AvbEntityAm824IO>>;

    /// Destructor - stops entity if running
    ~AvbEntityAm824IO();

    // Non-copyable, non-movable (state machine callbacks capture this)
    AvbEntityAm824IO(AvbEntityAm824IO const&) = delete;
    auto operator=(AvbEntityAm824IO const&) -> AvbEntityAm824IO& = delete;
    AvbEntityAm824IO(AvbEntityAm824IO&&) = delete;
    auto operator=(AvbEntityAm824IO&&) -> AvbEntityAm824IO& = delete;

    /// Passkey that gates the public constructor: only AvbEntityAm824IO can
    /// mint one, so construction is effectively private to create() while still
    /// being reachable by std::make_unique (which cannot call a private ctor).
    class CreateKey
    {
        CreateKey() = default;
        friend class AvbEntityAm824IO;
    };

    /// Constructor — call via create(), which supplies the CreateKey. Takes the
    /// entity model by value and constructs `components_` in place
    /// (NanoAvbComponents is non-movable, so it cannot be passed pre-built).
    /// @param config       Validated configuration
    /// @param entity_model Entity model to hand to NanoAvbComponents
    /// @param channels     Number of audio channels derived from descriptor blob
    AvbEntityAm824IO(
        CreateKey,
        AvbEntityAm824IOConfig config,
        std::unique_ptr<nanoavb::AemEntityHandler> handler,
        size_t channels,
        std::pmr::memory_resource* memory_resource);

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

    /// Decode one received AVTP AM824 stream frame. Called from the stream RX
    /// handler on the reactor thread; meters packets/samples. Non-AM824 or
    /// invalid frames are ignored.
    /// @param frame  The received Ethernet payload (AVTP header onward)
    /// @param now_ns Arrival time in nanoseconds
    void on_stream_rx_frame(std::span<uint8_t const> frame, int64_t now_ns);

    /// Set custom audio processing callback (in addition to biquad filter)
    /// @param callback Function called with interleaved N-channel samples for processing
    auto set_audio_callback(Am824AudioProcessCallback callback) -> void { audio_callback_ = std::move(callback); }

    /// Reconfigure DSP filter parameters (applies to all per-channel biquad filters)
    /// @param freq_hz Filter center frequency in Hz
    /// @param gain_db Filter gain in dB (negative for cut, positive for boost)
    /// @param q Filter Q factor (bandwidth)
    auto configure_filter(double freq_hz, double gain_db, double q) -> void;

    /// Get access to NanoAVB components (for advanced use)
    [[nodiscard]] auto components() -> nanoavb::NanoAvbComponents& { return host_.components(); }
    [[nodiscard]] auto components() const -> nanoavb::NanoAvbComponents const& { return host_.components(); }

    /// Get access to network handlers (for advanced use)
    [[nodiscard]] auto net_handlers() -> nanoavb::NanoAvbNetHandlers* { return host_.net_handlers(); }

    /// Get the configuration
    [[nodiscard]] auto config() const noexcept -> AvbEntityAm824IOConfig const& { return config_; }

    /// Get the number of audio channels
    [[nodiscard]] auto channels() const noexcept -> size_t { return channels_; }

  private:
    /// Attach this entity's stream-specific control-plane behavior to the host:
    /// single-stream MSRP advertise/withdraw + the ACMP talker connection log. The
    /// host owns the generic SM wiring + lifecycle.
    auto wire_stream_callbacks() -> void;

    /// Build the MSRP talker reservation (TSpec) for our AM824 stream. The
    /// stream id / destination / VLAN are sourced from the ACMP-configured
    /// talker stream 0 so MSRP, ACMP, and the AVTP stream share one identity;
    /// the TSpec is derived from the 96 kHz AM824 framing.
    [[nodiscard]] auto make_talker_srp_info() const -> nanoavb::TalkerStreamSrpInfo;

    // Member order optimized to minimize struct padding
    // (config_ must precede components_ for initialization dependency)

    /// Configuration
    AvbEntityAm824IOConfig config_;

    /// Custom audio callback (optional)
    Am824AudioProcessCallback audio_callback_;

    /// The reusable AVB control plane: state machines + NanoAvbComponents + net
    /// handlers + lifecycle. This entity supplies only its single AM824 stream + DSP
    /// and attaches them via host_.components() + the typed hooks. Declared after
    /// config_ (the model is built from config_ and passed in at construction).
    AvbEntityHost host_;

    /// Number of audio channels (determined from descriptor blob at construction)
    size_t channels_{0};

    //
    // DSP Processing
    //

    /// Memory resource for DSP buffers below.
    std::pmr::memory_resource* mem_resource_{std::pmr::get_default_resource()};

    /// Biquad filters (one per channel)
    std::pmr::vector<dsp::BiQuad<float>> biquads_;

    /// Audio processing buffer (interleaved, channels_ samples per frame)
    std::pmr::vector<float> audio_buffer_;

    /// Per-channel sine oscillators (stream audio source). Each channel uses a
    /// distinct initial phase so no two channels are identical.
    std::pmr::vector<dsp::Oscillator<float>> oscillators_;

    //
    // Stream data plane (AVTP AM824)
    //

    /// AVTP stream transmit socket (used from the PTP timer thread). Opened with
    /// PACKET_QDISC_BYPASS so our own egress is not re-received on this host.
    net::RawnetContext stream_tx_{};

    /// Talker per-stream serialization state (DBC, timestamps, sequence).
    std::optional<avtp::Am824StreamOutputContext> talker_out_{};

    /// Listener per-stream deserialization state.
    std::optional<avtp::Am824StreamInputContext> listener_in_{};

    /// Resolved stream destination MAC (from ACMP talker stream 0).
    ieee::Eui48 stream_dest_mac_{};

    /// Stream data-plane counters. tx is touched only on the PTP thread; rx
    /// counters are atomic (written on the reactor thread, read for status).
    uint64_t stream_tx_packets_{0};
    std::atomic<uint64_t> stream_rx_packets_{0};
    std::atomic<uint64_t> stream_rx_samples_{0};
    std::atomic<uint64_t> stream_rx_bad_{0};

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
