#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB Base Module - Core types, error codes, and constants
/// Minimal AVB/ATDECC implementation wrapping statusbar.atdecc

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace statusbar::nanoavb {

using ieee::Eui48;
using ieee::Eui64;

//
// Threading Model
//
//
// NanoAVB uses a single-threaded event-driven reactor pattern. All state machine
// transitions and callbacks are invoked from a single thread (the network reactor
// thread). This design provides:
//
// 1. **Deterministic Execution**: No lock contention or priority inversion
// 2. **Simplified Reasoning**: All state changes happen sequentially
// 3. **Low Latency**: No mutex overhead for protocol processing
//
// Thread Safety Guarantees:
// - State machine process() calls must be serialized (single thread only)
// - Context modifications only occur within action callbacks
// - Callbacks are invoked synchronously during state transitions
//
// If external monitoring is needed:
// - Query current state via Machine::state() (returns enum, inherently safe)
// - Context status flags are informational and may be stale if read from
//   another thread (use memory barriers if cross-thread reads are required)
// - For thread-safe status monitoring, copy state in the reactor thread
//   and communicate via a concurrent queue or atomic snapshot
//
// The following components follow this model:
// - supervisor_sm, gptp_sm, mvrp_sm - Protocol coordination
// - msrp_talker_sm, msrp_listener_sm - Stream reservation
// - talker_engine_sm, listener_engine_sm - Audio streaming
// - NanoAvbNetHandlers - Reactor integration for all protocols
//
//
//
// NanoAVB Error Codes
//
/// Error codes specific to NanoAVB operations
enum class NanoAvbError : int
{
    Success = 0,
    InvalidDescriptorType,
    InvalidDescriptorIndex,
    DescriptorNotFound,
    DescriptorStorageFull,
    DescriptorTooLarge,  // Descriptor exceeds response buffer size
    InvalidConfiguration,
    EntityNotInitialized,
    CommandNotSupported,
    InvalidStreamIndex,
    StreamNotConnected,
    StreamAlreadyConnected,
    SrpRegistrationFailed,
    InvalidVlanId,
    VlanTableFull,  // All VLAN slots hold actively-registered VLANs
};

/// Get human-readable name for NanoAvbError
/// @param e The error code to convert to a string
[[nodiscard]] auto nanoavb_error_name(NanoAvbError e) noexcept -> std::string_view;

/// Error category for NanoAvb errors
class NanoAvbErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.nanoavb"; }

    /// @param ev The error code value to convert to a message string
    [[nodiscard]] auto message(int ev) const -> std::string override
    {
        return std::string{nanoavb_error_name(static_cast<NanoAvbError>(ev))};
    }
};

/// Get the NanoAvb error category singleton
[[nodiscard]] inline auto nanoavb_error_category() noexcept -> NanoAvbErrorCategory const&
{
    static NanoAvbErrorCategory const category;
    return category;
}

/// Create an error_code from a NanoAvbError
/// @param e The NanoAvbError value to convert
[[nodiscard]] inline auto make_error_code(NanoAvbError e) noexcept -> std::error_code
{
    return std::error_code{static_cast<int>(e), nanoavb_error_category()};
}

//
// NanoAVB Configuration Constants
//
/// Default maximum number of configurations per entity
constexpr size_t default_max_configurations = 1;

/// Default maximum number of audio units per configuration
constexpr size_t default_max_audio_units = 1;

/// Default maximum number of stream inputs per audio unit
constexpr size_t default_max_stream_inputs = 8;

/// Default maximum number of stream outputs per audio unit
constexpr size_t default_max_stream_outputs = 8;

/// Default maximum number of jack inputs
constexpr size_t default_max_jack_inputs = 8;

/// Default maximum number of jack outputs
constexpr size_t default_max_jack_outputs = 8;

/// Default maximum number of AVB interfaces
constexpr size_t default_max_avb_interfaces = 1;

/// Default maximum number of clock sources
constexpr size_t default_max_clock_sources = 4;

/// Default maximum number of clock domains
constexpr size_t default_max_clock_domains = 1;

/// Default maximum number of locales
constexpr size_t default_max_locales = 1;

/// Default maximum number of strings descriptors
constexpr size_t default_max_strings = 4;

/// Default maximum number of stream port inputs
constexpr size_t default_max_stream_port_inputs = 8;

/// Default maximum number of stream port outputs
constexpr size_t default_max_stream_port_outputs = 8;

/// Default maximum number of audio clusters
constexpr size_t default_max_audio_clusters = 16;

/// Default maximum number of audio maps
constexpr size_t default_max_audio_maps = 8;

/// Default maximum number of controls
constexpr size_t default_max_controls = 32;

//
// Entity Capabilities (subset for minimal implementation)
//
namespace entity_capabilities {

/// Entity supports AEM (AVDECC Entity Model)
constexpr uint32_t AEM_SUPPORTED = 0x00000001;

/// Entity supports class A streaming
constexpr uint32_t CLASS_A_SUPPORTED = 0x00000004;

/// Entity supports class B streaming
constexpr uint32_t CLASS_B_SUPPORTED = 0x00000008;

/// Entity supports gPTP
constexpr uint32_t GPTP_SUPPORTED = 0x00000010;

/// Entity is AEM interface index valid
constexpr uint32_t AEM_INTERFACE_INDEX_VALID = 0x00000020;

/// Entity supports ACMP
constexpr uint32_t GENERAL_CONTROLLER_IGNORE = 0x00000200;

/// Entity supports AEM authentication
constexpr uint32_t AEM_AUTHENTICATION_SUPPORTED = 0x00004000;

/// Entity supports AEM authentication required
constexpr uint32_t AEM_AUTHENTICATION_REQUIRED = 0x00008000;

/// Entity supports AEM persistent acquire
constexpr uint32_t AEM_PERSISTENT_ACQUIRE_SUPPORTED = 0x00010000;

/// Entity identifies as controller capable
constexpr uint32_t AEM_IDENTIFY_CONTROL_INDEX_VALID = 0x00020000;

/// Entity is interface index valid
constexpr uint32_t AEM_INTERFACE_INDEX_PRESENT = 0x00040000;

/// Minimal NanoAVB entity capabilities - AEM + Class A + gPTP
constexpr uint32_t NANOAVB_DEFAULT = AEM_SUPPORTED | CLASS_A_SUPPORTED | GPTP_SUPPORTED;

}  // namespace entity_capabilities

//
// Talker Capabilities
//
namespace talker_capabilities {

constexpr uint16_t IMPLEMENTED = 0x0001;
constexpr uint16_t HAS_OTHER_SOURCE = 0x0200;
constexpr uint16_t HAS_CONTROL_SOURCE = 0x0400;
constexpr uint16_t HAS_MEDIA_CLOCK_SOURCE = 0x0800;
constexpr uint16_t HAS_SMPTE_SOURCE = 0x1000;
constexpr uint16_t HAS_MIDI_SOURCE = 0x2000;
constexpr uint16_t HAS_AUDIO_SOURCE = 0x4000;
constexpr uint16_t HAS_VIDEO_SOURCE = 0x8000;

/// Minimal audio talker
constexpr uint16_t NANOAVB_AUDIO_TALKER = IMPLEMENTED | HAS_AUDIO_SOURCE;

}  // namespace talker_capabilities

//
// Listener Capabilities
//
namespace listener_capabilities {

constexpr uint16_t IMPLEMENTED = 0x0001;
constexpr uint16_t HAS_OTHER_SINK = 0x0200;
constexpr uint16_t HAS_CONTROL_SINK = 0x0400;
constexpr uint16_t HAS_MEDIA_CLOCK_SINK = 0x0800;
constexpr uint16_t HAS_SMPTE_SINK = 0x1000;
constexpr uint16_t HAS_MIDI_SINK = 0x2000;
constexpr uint16_t HAS_AUDIO_SINK = 0x4000;
constexpr uint16_t HAS_VIDEO_SINK = 0x8000;

/// Minimal audio listener
constexpr uint16_t NANOAVB_AUDIO_LISTENER = IMPLEMENTED | HAS_AUDIO_SINK;

}  // namespace listener_capabilities

//
// SRP Constants (used by nanoavb_srp)
//
namespace srp {

/// MVRP EtherType - IEEE 802.1Q
constexpr uint16_t MVRP_ETHERTYPE = 0x88F5;

/// MSRP EtherType - IEEE 802.1Q
constexpr uint16_t MSRP_ETHERTYPE = 0x22EA;

/// MVRP Multicast Address - 01:80:C2:00:00:21
constexpr uint64_t MVRP_MULTICAST_ADDRESS = 0x0180C2000021;

/// MSRP Multicast Address - 01:80:C2:00:00:0E
constexpr uint64_t MSRP_MULTICAST_ADDRESS = 0x0180C200000E;

/// Default SR Class A VLAN ID
constexpr uint16_t DEFAULT_SR_CLASS_A_VID = 2;

/// Default SR Class B VLAN ID
constexpr uint16_t DEFAULT_SR_CLASS_B_VID = 2;

/// SR Class A priority (highest)
constexpr uint8_t SR_CLASS_A_PRIORITY = 3;

/// SR Class B priority
constexpr uint8_t SR_CLASS_B_PRIORITY = 2;

}  // namespace srp

}  // namespace statusbar::nanoavb

// Enable std::error_code conversion
template <>
struct std::is_error_code_enum<statusbar::nanoavb::NanoAvbError> : std::true_type
{};
