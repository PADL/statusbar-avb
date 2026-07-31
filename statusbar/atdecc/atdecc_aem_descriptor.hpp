#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC Entity Model (AEM) Descriptors - IEEE 1722.1 Clause 7
/// Modernized C++23 implementation based on jdksatdecc-c

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::atdecc::aem {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::octet_t;
using ieee::octlet_t;
using ieee::quadlet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;

//
// AEM Constants - IEEE 1722.1 Clause 7
//
/// Maximum descriptor size - Clause 7.2
constexpr size_t AEM_DESCRIPTOR_SIZE = 508;

//
// Descriptor Types - Clause 7.2
//
constexpr uint16_t DESCRIPTOR_ENTITY = 0x0000;
constexpr uint16_t DESCRIPTOR_CONFIGURATION = 0x0001;
constexpr uint16_t DESCRIPTOR_AUDIO_UNIT = 0x0002;
constexpr uint16_t DESCRIPTOR_VIDEO_UNIT = 0x0003;
constexpr uint16_t DESCRIPTOR_SENSOR_UNIT = 0x0004;
constexpr uint16_t DESCRIPTOR_STREAM_INPUT = 0x0005;
constexpr uint16_t DESCRIPTOR_STREAM_OUTPUT = 0x0006;
constexpr uint16_t DESCRIPTOR_JACK_INPUT = 0x0007;
constexpr uint16_t DESCRIPTOR_JACK_OUTPUT = 0x0008;
constexpr uint16_t DESCRIPTOR_AVB_INTERFACE = 0x0009;
constexpr uint16_t DESCRIPTOR_CLOCK_SOURCE = 0x000a;
constexpr uint16_t DESCRIPTOR_MEMORY_OBJECT = 0x000b;
constexpr uint16_t DESCRIPTOR_LOCALE = 0x000c;
constexpr uint16_t DESCRIPTOR_STRINGS = 0x000d;
constexpr uint16_t DESCRIPTOR_STREAM_PORT_INPUT = 0x000e;
constexpr uint16_t DESCRIPTOR_STREAM_PORT_OUTPUT = 0x000f;
constexpr uint16_t DESCRIPTOR_EXTERNAL_PORT_INPUT = 0x0010;
constexpr uint16_t DESCRIPTOR_EXTERNAL_PORT_OUTPUT = 0x0011;
constexpr uint16_t DESCRIPTOR_INTERNAL_PORT_INPUT = 0x0012;
constexpr uint16_t DESCRIPTOR_INTERNAL_PORT_OUTPUT = 0x0013;
constexpr uint16_t DESCRIPTOR_AUDIO_CLUSTER = 0x0014;
constexpr uint16_t DESCRIPTOR_VIDEO_CLUSTER = 0x0015;
constexpr uint16_t DESCRIPTOR_SENSOR_CLUSTER = 0x0016;
constexpr uint16_t DESCRIPTOR_AUDIO_MAP = 0x0017;
constexpr uint16_t DESCRIPTOR_VIDEO_MAP = 0x0018;
constexpr uint16_t DESCRIPTOR_SENSOR_MAP = 0x0019;
constexpr uint16_t DESCRIPTOR_CONTROL = 0x001a;
constexpr uint16_t DESCRIPTOR_SIGNAL_SELECTOR = 0x001b;
constexpr uint16_t DESCRIPTOR_MIXER = 0x001c;
constexpr uint16_t DESCRIPTOR_MATRIX = 0x001d;
constexpr uint16_t DESCRIPTOR_MATRIX_SIGNAL = 0x001e;
constexpr uint16_t DESCRIPTOR_SIGNAL_SPLITTER = 0x001f;
constexpr uint16_t DESCRIPTOR_SIGNAL_COMBINER = 0x0020;
constexpr uint16_t DESCRIPTOR_SIGNAL_DEMULTIPLEXER = 0x0021;
constexpr uint16_t DESCRIPTOR_SIGNAL_MULTIPLEXER = 0x0022;
constexpr uint16_t DESCRIPTOR_SIGNAL_TRANSCODER = 0x0023;
constexpr uint16_t DESCRIPTOR_CLOCK_DOMAIN = 0x0024;
constexpr uint16_t DESCRIPTOR_CONTROL_BLOCK = 0x0025;
constexpr uint16_t DESCRIPTOR_TIMING = 0x0026;
constexpr uint16_t DESCRIPTOR_PTP_INSTANCE = 0x0027;
constexpr uint16_t DESCRIPTOR_PTP_PORT = 0x0028;
constexpr uint16_t DESCRIPTOR_INVALID = 0xffff;

constexpr uint16_t NUM_DESCRIPTOR_TYPES = 0x0029;

/// Get human-readable name for descriptor type
/// @param type AEM descriptor type code
[[nodiscard]] auto descriptor_type_name(uint16_t type) noexcept -> std::string_view;

//
// Jack Types - Clause 7.2.7.2
//
constexpr uint16_t JACK_TYPE_SPEAKER = 0x0000;
constexpr uint16_t JACK_TYPE_HEADPHONE = 0x0001;
constexpr uint16_t JACK_TYPE_ANALOG_MICROPHONE = 0x0002;
constexpr uint16_t JACK_TYPE_SPDIF = 0x0003;
constexpr uint16_t JACK_TYPE_ADAT = 0x0004;
constexpr uint16_t JACK_TYPE_TDIF = 0x0005;
constexpr uint16_t JACK_TYPE_MADI = 0x0006;
constexpr uint16_t JACK_TYPE_UNBALANCED_ANALOG = 0x0007;
constexpr uint16_t JACK_TYPE_BALANCED_ANALOG = 0x0008;
constexpr uint16_t JACK_TYPE_DIGITAL = 0x0009;
constexpr uint16_t JACK_TYPE_MIDI = 0x000a;
constexpr uint16_t JACK_TYPE_AES_EBU = 0x000b;
constexpr uint16_t JACK_TYPE_COMPOSITE_VIDEO = 0x000c;
constexpr uint16_t JACK_TYPE_S_VHS_VIDEO = 0x000d;
constexpr uint16_t JACK_TYPE_COMPONENT_VIDEO = 0x000e;
constexpr uint16_t JACK_TYPE_DVI = 0x000f;
constexpr uint16_t JACK_TYPE_HDMI = 0x0010;
constexpr uint16_t JACK_TYPE_UDI = 0x0011;
constexpr uint16_t JACK_TYPE_DISPLAYPORT = 0x0012;
constexpr uint16_t JACK_TYPE_ANTENNA = 0x0013;
constexpr uint16_t JACK_TYPE_ANALOG_TUNER = 0x0014;
constexpr uint16_t JACK_TYPE_ETHERNET = 0x0015;
constexpr uint16_t JACK_TYPE_WIFI = 0x0016;
constexpr uint16_t JACK_TYPE_USB = 0x0017;
constexpr uint16_t JACK_TYPE_PCI = 0x0018;
constexpr uint16_t JACK_TYPE_PCI_E = 0x0019;
constexpr uint16_t JACK_TYPE_SCSI = 0x001a;
constexpr uint16_t JACK_TYPE_ATA = 0x001b;
constexpr uint16_t JACK_TYPE_IMAGER = 0x001c;
constexpr uint16_t JACK_TYPE_IR = 0x001d;
constexpr uint16_t JACK_TYPE_THUNDERBOLT = 0x001e;
constexpr uint16_t JACK_TYPE_SATA = 0x001f;
constexpr uint16_t JACK_TYPE_SMPTE_LTC = 0x0020;
constexpr uint16_t JACK_TYPE_DIGITAL_MICROPHONE = 0x0021;
constexpr uint16_t JACK_TYPE_AUDIO_MEDIA_CLOCK = 0x0022;
constexpr uint16_t JACK_TYPE_VIDEO_MEDIA_CLOCK = 0x0023;
constexpr uint16_t JACK_TYPE_GNSS_CLOCK = 0x0024;
constexpr uint16_t JACK_TYPE_PPS = 0x0025;
constexpr uint16_t JACK_TYPE_EXPANSION = 0xffff;

//
// AVB Interface Flags - Clause 7.2.8.1
//
constexpr uint16_t AVB_INTERFACE_FLAG_GPTP_GRANDMASTER_SUPPORTED = 0x0001;
constexpr uint16_t AVB_INTERFACE_FLAG_GPTP_SUPPORTED = 0x0002;
constexpr uint16_t AVB_INTERFACE_FLAG_SRP_SUPPORTED = 0x0004;
constexpr uint16_t AVB_INTERFACE_FLAG_FQTSS_NOT_SUPPORTED = 0x0008;
constexpr uint16_t AVB_INTERFACE_FLAG_SCHEDULED_TRAFFIC_SUPPORTED = 0x0010;
constexpr uint16_t AVB_INTERFACE_FLAG_CAN_LISTEN_TO_SELF = 0x0020;
constexpr uint16_t AVB_INTERFACE_FLAG_CAN_LISTEN_TO_OTHER_SELF = 0x0040;

//
// Clock Source Types - Clause 7.2.9.2
//
constexpr uint16_t CLOCK_SOURCE_TYPE_INTERNAL = 0x0000;
constexpr uint16_t CLOCK_SOURCE_TYPE_EXTERNAL = 0x0001;
constexpr uint16_t CLOCK_SOURCE_TYPE_INPUT_STREAM = 0x0002;
constexpr uint16_t CLOCK_SOURCE_TYPE_MEDIA_CLOCK_STREAM =
    0x0003;  // Not in IEEE 1722.1-2021 (0x0003 is reserved); retained for draft compatibility
constexpr uint16_t CLOCK_SOURCE_TYPE_EXPANSION = 0xffff;

//
// Memory Object Types - Clause 7.2.10.1
//
constexpr uint16_t MEMORY_OBJECT_TYPE_FIRMWARE_IMAGE = 0x0000;
constexpr uint16_t MEMORY_OBJECT_TYPE_VENDOR_SPECIFIC = 0x0001;
constexpr uint16_t MEMORY_OBJECT_TYPE_CRASH_DUMP = 0x0002;
constexpr uint16_t MEMORY_OBJECT_TYPE_LOG_OBJECT = 0x0003;
constexpr uint16_t MEMORY_OBJECT_TYPE_AUTOSTART_SETTINGS = 0x0004;
constexpr uint16_t MEMORY_OBJECT_TYPE_SNAPSHOT_SETTINGS = 0x0005;
constexpr uint16_t MEMORY_OBJECT_TYPE_SVG_MANUFACTURER = 0x0006;
constexpr uint16_t MEMORY_OBJECT_TYPE_SVG_ENTITY = 0x0007;
constexpr uint16_t MEMORY_OBJECT_TYPE_SVG_GENERIC = 0x0008;
constexpr uint16_t MEMORY_OBJECT_TYPE_PNG_MANUFACTURER = 0x0009;
constexpr uint16_t MEMORY_OBJECT_TYPE_PNG_ENTITY = 0x000a;
constexpr uint16_t MEMORY_OBJECT_TYPE_PNG_GENERIC = 0x000b;
constexpr uint16_t MEMORY_OBJECT_TYPE_DAE_MANUFACTURER = 0x000c;
constexpr uint16_t MEMORY_OBJECT_TYPE_DAE_ENTITY = 0x000d;
constexpr uint16_t MEMORY_OBJECT_TYPE_DAE_GENERIC = 0x000e;

//
// Audio Cluster Format - Clause 7.2.16.1
//
constexpr uint8_t AUDIO_CLUSTER_FORMAT_IEC_60958 = 0x00;
constexpr uint8_t AUDIO_CLUSTER_FORMAT_MBLA = 0x40;
constexpr uint8_t AUDIO_CLUSTER_FORMAT_MIDI = 0x80;
constexpr uint8_t AUDIO_CLUSTER_FORMAT_SMPTE = 0x88;

//
// Stream Flags - Clause 7.2.6.1
//
constexpr uint16_t STREAM_FLAG_CLOCK_SYNC_SOURCE = 0x0001;
constexpr uint16_t STREAM_FLAG_CLASS_A = 0x0002;
constexpr uint16_t STREAM_FLAG_CLASS_B = 0x0004;
constexpr uint16_t STREAM_FLAG_SUPPORTS_ENCRYPTED = 0x0008;
constexpr uint16_t STREAM_FLAG_PRIMARY_BACKUP_SUPPORTED = 0x0010;
constexpr uint16_t STREAM_FLAG_PRIMARY_BACKUP_VALID = 0x0020;
constexpr uint16_t STREAM_FLAG_SECONDARY_BACKUP_SUPPORTED = 0x0040;
constexpr uint16_t STREAM_FLAG_SECONDARY_BACKUP_VALID = 0x0080;
constexpr uint16_t STREAM_FLAG_TERTIARY_BACKUP_SUPPORTED = 0x0100;
constexpr uint16_t STREAM_FLAG_TERTIARY_BACKUP_VALID = 0x0200;
constexpr uint16_t STREAM_FLAG_SUPPORTS_AVTP_UDPV4 = 0x0400;
constexpr uint16_t STREAM_FLAG_SUPPORTS_AVTP_UDPV6 = 0x0800;
constexpr uint16_t STREAM_FLAG_NO_SUPPORT_AVTP_NATIVE = 0x1000;
constexpr uint16_t STREAM_FLAG_TIMING_FIELD_VALID = 0x2000;
constexpr uint16_t STREAM_FLAG_NO_MEDIA_CLOCK = 0x4000;
constexpr uint16_t STREAM_FLAG_SUPPORTS_NO_SRP = 0x8000;

//
// Timing Algorithm - Clause 7.2.34.1
//
constexpr uint16_t TIMING_ALGORITHM_SINGLE = 0x0000;
constexpr uint16_t TIMING_ALGORITHM_FALLBACK = 0x0001;
constexpr uint16_t TIMING_ALGORITHM_COMBINED = 0x0002;

//
// PTP Port Types - Clause 7.2.36.1
//
constexpr uint16_t PTP_PORT_TYPE_P2P_LINK_LAYER = 0x0000;
constexpr uint16_t PTP_PORT_TYPE_P2P_MULTICAST_UDPV4 = 0x0001;
constexpr uint16_t PTP_PORT_TYPE_P2P_MULTICAST_UDPV6 = 0x0002;
constexpr uint16_t PTP_PORT_TYPE_TIMING_MEASUREMENT = 0x0003;
constexpr uint16_t PTP_PORT_TYPE_FINE_TIMING_MEASUREMENT = 0x0004;
constexpr uint16_t PTP_PORT_TYPE_E2E_LINK_LAYER = 0x0005;
constexpr uint16_t PTP_PORT_TYPE_E2E_MULTICAST_UDPV4 = 0x0006;
constexpr uint16_t PTP_PORT_TYPE_E2E_MULTICAST_UDPV6 = 0x0007;
constexpr uint16_t PTP_PORT_TYPE_P2P_UNICAST_UDPV4 = 0x0008;
constexpr uint16_t PTP_PORT_TYPE_P2P_UNICAST_UDPV6 = 0x0009;
constexpr uint16_t PTP_PORT_TYPE_E2E_UNICAST_UDPV4 = 0x000a;
constexpr uint16_t PTP_PORT_TYPE_E2E_UNICAST_UDPV6 = 0x000b;

//
// PTP Instance Flags - Clause 7.2.35.1
//
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_SET_INSTANCE_ENABLE = 0x00000001;
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_SET_PRIORITY1 = 0x00000002;
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_SET_PRIORITY2 = 0x00000004;
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_SET_DOMAIN_NUMBER = 0x00000008;
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_SET_EXTERNAL_PORT_CONFIGURATION = 0x00000010;
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_SET_SLAVE_ONLY = 0x00000020;
constexpr uint32_t PTP_INSTANCE_FLAG_CAN_ENABLE_PERFORMANCE = 0x00000040;
constexpr uint32_t PTP_INSTANCE_FLAG_PERFORMANCE_MONITORING = 0x40000000;
constexpr uint32_t PTP_INSTANCE_FLAG_GRANDMASTER_CAPABLE = 0x80000000;

//
// ATDECC String - 64-byte fixed UTF-8 string
//
/// Fixed 64-byte ATDECC string per IEEE 1722.1
struct AtdeccString
{
    static constexpr size_t LENGTH = 64;

    std::array<uint8_t, LENGTH> value{};

    constexpr AtdeccString() noexcept = default;

    constexpr explicit AtdeccString(char const* s) noexcept { assign(s); }

    constexpr void assign(char const* s) noexcept
    {
        size_t i = 0;
        for (; i < LENGTH && s[i] != '\0'; ++i) {
            value[i] = static_cast<uint8_t>(s[i]);
        }
        for (; i < LENGTH; ++i) {
            value[i] = 0;
        }
    }

    [[nodiscard]] constexpr auto length() const noexcept
    {
        size_t len = 0;
        for (size_t i = 0; i < LENGTH; ++i) {
            if (value[i] == 0) {
                break;
            }
            len = i + 1;
        }
        return len;
    }

    [[nodiscard]] auto as_string_view() const noexcept -> std::string_view
    {
        return ::statusbar::as_string_view(std::span<uint8_t const>{value.data(), length()});
    }
    constexpr auto operator<=>(AtdeccString const& rhs) const noexcept -> std::strong_ordering
    {
        return as_string_view() <=> rhs.as_string_view();
    }
};

static_assert(sizeof(AtdeccString) == 64, "AtdeccString must be exactly 64 bytes");

/// Parsed result of a localized_description field (IEEE 1722.1 Clause 7.3.7).
/// The field encodes a STRINGS descriptor index and a string offset within it.
struct LocalizedStringRef
{
    uint16_t strings_descriptor_index{0};  ///< (localized_description >> 3) & 0x1FFF
    uint8_t string_offset{0};              ///< localized_description & 0x07  (0..6)
};

/// Parse a localized_description field into a STRINGS descriptor index and
/// string offset. Returns nullopt for 0xFFFF (no localized description) or
/// if the string offset exceeds 6.
[[nodiscard]] inline auto parse_localized_description(uint16_t loc) noexcept -> std::optional<LocalizedStringRef>
{
    if (loc == 0xFFFF) {
        return std::nullopt;
    }
    auto const strings_idx = static_cast<uint16_t>((loc >> 3) & 0x1FFFU);
    auto const offset = static_cast<uint8_t>(loc & 0x07U);
    if (offset > 6) {
        return std::nullopt;
    }
    return LocalizedStringRef{.strings_descriptor_index = strings_idx, .string_offset = offset};
}

//
// Descriptor addressing primitives
//

/// Identifies a single AEM descriptor instance within an entity.
///
/// Used as a parameter bundle for AEM command handlers (READ_DESCRIPTOR,
/// GET_NAME/SET_NAME, GET_CONFIGURATION, etc.) so call sites don't pass
/// three back-to-back uint16_t arguments that are easy to swap by mistake.
///
/// IEEE 1722.1 addresses every descriptor by the tuple
/// (configuration_index, descriptor_type, descriptor_index). The ENTITY
/// and CONFIGURATION descriptors are the only descriptors that exist
/// outside of a specific configuration; for those, configuration_index
/// is ignored by the entity model.
struct DescriptorRef
{
    uint16_t configuration_index{0};  ///< Configuration containing the descriptor (0 for ENTITY/CONFIGURATION)
    uint16_t descriptor_type{0};      ///< DESCRIPTOR_* constant (Clause 7.2)
    uint16_t descriptor_index{0};     ///< Zero-based index within the descriptor type

    auto operator<=>(DescriptorRef const&) const noexcept = default;
};

/// Identifies one of the named fields of a descriptor for use with
/// GET_NAME and SET_NAME (IEEE 1722.1 Clause 7.4.17, 7.4.18).
///
/// A descriptor may expose more than one user-settable name field
/// (e.g. ENTITY has entity_name and group_name, indexed 0 and 1).
/// The (descriptor, name_index) pair identifies which name is being read
/// or written.
struct NameRef
{
    DescriptorRef descriptor{};  ///< Descriptor owning the name
    uint16_t name_index{0};      ///< Which name within the descriptor (Clause 7.4.17.1)

    auto operator<=>(NameRef const&) const noexcept = default;
};

//
// Entity Descriptor - Clause 7.2.1
//
/// ENTITY Descriptor - Clause 7.2.1
/// This describes the top-level entity in the ATDECC device.
struct DescriptorEntity
{
    static constexpr size_t LENGTH = 312;

    doublet_t descriptor_type{DESCRIPTOR_ENTITY};  // 0
    doublet_t descriptor_index{0};                 // 2
    Eui64 entity_id{};                             // 4
    Eui64 entity_model_id{};                       // 12
    quadlet_t entity_capabilities{0};              // 20
    doublet_t talker_stream_sources{0};            // 24
    doublet_t talker_capabilities{0};              // 26
    doublet_t listener_stream_sinks{0};            // 28
    doublet_t listener_capabilities{0};            // 30
    quadlet_t controller_capabilities{0};          // 32
    quadlet_t available_index{0};                  // 36
    Eui64 association_id{};                        // 40
    AtdeccString entity_name{};                    // 48
    doublet_t vendor_name_string{0};               // 112
    doublet_t model_name_string{0};                // 114
    AtdeccString firmware_version{};               // 116
    AtdeccString group_name{};                     // 180
    AtdeccString serial_number{};                  // 244
    doublet_t configurations_count{0};             // 308
    doublet_t current_configuration{0};            // 310

    auto operator<=>(DescriptorEntity const&) const noexcept = default;

    /// Number of on-wire bytes this descriptor occupies in a READ_DESCRIPTOR
    /// response. ENTITY has no variable-length trailer, so this is a
    /// compile-time constant equal to LENGTH.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorEntity) == 312, "DescriptorEntity must be exactly 312 bytes");
static_assert(offsetof(DescriptorEntity, descriptor_type) == 0);
static_assert(offsetof(DescriptorEntity, descriptor_index) == 2);
static_assert(offsetof(DescriptorEntity, entity_id) == 4);
static_assert(offsetof(DescriptorEntity, entity_model_id) == 12);
static_assert(offsetof(DescriptorEntity, entity_capabilities) == 20);
static_assert(offsetof(DescriptorEntity, entity_name) == 48);
static_assert(offsetof(DescriptorEntity, vendor_name_string) == 112);
static_assert(offsetof(DescriptorEntity, firmware_version) == 116);
static_assert(offsetof(DescriptorEntity, configurations_count) == 308);

//
// Configuration Descriptor - Clause 7.2.2
//

/// A single (descriptor_type, count) entry in the descriptor_counts table
/// of a CONFIGURATION descriptor. IEEE 1722.1 Clause 7.2.2, 4 bytes on
/// the wire: a pair of network-byte-order doublets.
struct DescriptorCountEntry
{
    doublet_t descriptor_type{0};  ///< Descriptor type code (DESCRIPTOR_* constant)
    doublet_t count{0};            ///< Number of descriptors of this type in the configuration
};

static_assert(sizeof(DescriptorCountEntry) == 4, "DescriptorCountEntry must be exactly 4 bytes");

/// CONFIGURATION Descriptor - Clause 7.2.2
/// Describes one configuration of an entity.
///
/// Wire format: 74-byte fixed header followed by `descriptor_counts_count`
/// × 4-byte (type, count) entries. IEEE 1722.1 Clause 7.2.2 caps the
/// count at 108 entries per descriptor (74 + 108*4 = 506 ≤ 508).
///
/// The `descriptor_counts` trailer is stored inline as typed storage;
/// handlers and parsers access entries through the typed array without
/// byte-offset math.
struct DescriptorConfiguration
{
    /// Fixed header size — also the minimum wire size.
    static constexpr size_t LENGTH = 74;

    /// IEEE 1722.1 Clause 7.2.2: "The maximum value for this field is
    /// 108 for this version of AEM." 74 + 108*4 = 506 bytes, just under
    /// the 508 byte AECP budget.
    static constexpr size_t MAX_DESCRIPTOR_COUNTS = 108;

    doublet_t descriptor_type{DESCRIPTOR_CONFIGURATION};                          // 0
    doublet_t descriptor_index{0};                                                // 2
    AtdeccString object_name{};                                                   // 4
    doublet_t localized_description{0};                                           // 68
    doublet_t descriptor_counts_count{0};                                         // 70
    doublet_t descriptor_counts_offset{LENGTH};                                   // 72: always = LENGTH on output
    std::array<DescriptorCountEntry, MAX_DESCRIPTOR_COUNTS> descriptor_counts{};  // 74..506

    // No defaulted operator<=>: the unused tail entries in
    // `descriptor_counts` would produce surprising equality results.
    // Compare via `wire_span(lhs) == wire_span(rhs)` or inspect the
    // first `descriptor_counts_count` entries explicitly.

    /// On-wire size in a READ_DESCRIPTOR response: fixed header plus 4 bytes per
    /// populated entry. Count clamped to capacity so a corrupt/future-larger
    /// count can never make this exceed sizeof(*this).
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        size_t const n = std::min(static_cast<size_t>(descriptor_counts_count.get()), MAX_DESCRIPTOR_COUNTS);
        return LENGTH + (n * sizeof(DescriptorCountEntry));
    }

    /// Clamp descriptor_counts_count to capacity (serve a valid truncated descriptor).
    constexpr void clamp_trailer_count() noexcept
    {
        if (static_cast<size_t>(descriptor_counts_count.get()) > MAX_DESCRIPTOR_COUNTS) {
            descriptor_counts_count = static_cast<uint16_t>(MAX_DESCRIPTOR_COUNTS);
        }
    }

    /// View of the populated descriptor_counts entries (length = descriptor_counts_count).
    [[nodiscard]] auto used_descriptor_counts() const noexcept -> std::span<DescriptorCountEntry const>
    {
        size_t const n = std::min(static_cast<size_t>(descriptor_counts_count.get()), MAX_DESCRIPTOR_COUNTS);
        return std::span<DescriptorCountEntry const>{descriptor_counts.data(), n};
    }

    /// Append a descriptor_counts entry. Updates descriptor_counts_count on success.
    /// @param entry Descriptor type/count pair to append
    /// @return true on success, false at MAX_DESCRIPTOR_COUNTS capacity
    [[nodiscard]] auto push_descriptor_count(DescriptorCountEntry entry) noexcept -> bool
    {
        size_t const n = descriptor_counts_count.get();
        if (n >= MAX_DESCRIPTOR_COUNTS) {
            return false;
        }
        descriptor_counts[n] = entry;
        descriptor_counts_count = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset descriptor_counts to empty. Backing storage is not zeroed.
    constexpr void clear_descriptor_counts() noexcept { descriptor_counts_count = static_cast<uint16_t>(0); }
};

static_assert(offsetof(DescriptorConfiguration, descriptor_type) == 0);
static_assert(offsetof(DescriptorConfiguration, object_name) == 4);
static_assert(offsetof(DescriptorConfiguration, localized_description) == 68);
static_assert(offsetof(DescriptorConfiguration, descriptor_counts_count) == 70);
static_assert(offsetof(DescriptorConfiguration, descriptor_counts_offset) == 72);
static_assert(
    offsetof(DescriptorConfiguration, descriptor_counts) == DescriptorConfiguration::LENGTH,
    "DescriptorConfiguration.descriptor_counts must start immediately after the 74-byte header");
static_assert(
    sizeof(DescriptorConfiguration) ==
        DescriptorConfiguration::LENGTH + (DescriptorConfiguration::MAX_DESCRIPTOR_COUNTS * sizeof(DescriptorCountEntry)),
    "DescriptorConfiguration must have no padding around the inline descriptor_counts array");

//
// Audio Unit Descriptor - Clause 7.2.3
//
/// AUDIO_UNIT Descriptor - Clause 7.2.3
///
/// Wire format: 144-byte fixed header followed by `sampling_rates_count`
/// × 4-byte `quadlet_t` sampling-rate entries. Each entry encodes a
/// pull-field and a nominal sample rate (see Clause 7.3.1).
///
/// The `sampling_rates` trailer is stored inline as typed storage; the
/// maximum is bounded by the 508-byte AECP budget: (508 - 144) / 4 = 91.
struct DescriptorAudioUnit
{
    static constexpr size_t LENGTH = 144;
    // Budget-bounded: (AEM_DESCRIPTOR_SIZE - LENGTH) / sizeof(quadlet_t) = (508 - 144) / 4 = 91.
    // Using the aem-namespace AEM_DESCRIPTOR_SIZE to avoid a cross-header dependency on
    // atdecc_aecp_aem.hpp::MAX_AEM_DESCRIPTOR_SIZE (which carries the same value 508).
    static constexpr size_t MAX_SAMPLING_RATES = (AEM_DESCRIPTOR_SIZE - LENGTH) / sizeof(quadlet_t);

    doublet_t descriptor_type{DESCRIPTOR_AUDIO_UNIT};            // 0
    doublet_t descriptor_index{0};                               // 2
    AtdeccString object_name{};                                  // 4
    doublet_t localized_description{0};                          // 68
    doublet_t clock_domain_index{0};                             // 70
    doublet_t number_of_stream_input_ports{0};                   // 72
    doublet_t base_stream_input_port{0};                         // 74
    doublet_t number_of_stream_output_ports{0};                  // 76
    doublet_t base_stream_output_port{0};                        // 78
    doublet_t number_of_external_input_ports{0};                 // 80
    doublet_t base_external_input_port{0};                       // 82
    doublet_t number_of_external_output_ports{0};                // 84
    doublet_t base_external_output_port{0};                      // 86
    doublet_t number_of_internal_input_ports{0};                 // 88
    doublet_t base_internal_input_port{0};                       // 90
    doublet_t number_of_internal_output_ports{0};                // 92
    doublet_t base_internal_output_port{0};                      // 94
    doublet_t number_of_controls{0};                             // 96
    doublet_t base_control{0};                                   // 98
    doublet_t number_of_signal_selectors{0};                     // 100
    doublet_t base_signal_selector{0};                           // 102
    doublet_t number_of_mixers{0};                               // 104
    doublet_t base_mixer{0};                                     // 106
    doublet_t number_of_matrices{0};                             // 108
    doublet_t base_matrix{0};                                    // 110
    doublet_t number_of_splitters{0};                            // 112
    doublet_t base_splitter{0};                                  // 114
    doublet_t number_of_combiners{0};                            // 116
    doublet_t base_combiner{0};                                  // 118
    doublet_t number_of_demultiplexers{0};                       // 120
    doublet_t base_demultiplexer{0};                             // 122
    doublet_t number_of_multiplexers{0};                         // 124
    doublet_t base_multiplexer{0};                               // 126
    doublet_t number_of_transcoders{0};                          // 128
    doublet_t base_transcoder{0};                                // 130
    doublet_t number_of_control_blocks{0};                       // 132
    doublet_t base_control_block{0};                             // 134
    quadlet_t current_sampling_rate{0};                          // 136
    doublet_t sampling_rates_offset{LENGTH};                     // 140: always = LENGTH on output
    doublet_t sampling_rates_count{0};                           // 142
    std::array<quadlet_t, MAX_SAMPLING_RATES> sampling_rates{};  // 144..508

    // No defaulted operator<=>: unused tail slots in `sampling_rates`
    // would produce surprising equality. Compare via wire_span() or
    // iterate the first `sampling_rates_count` entries.

    /// On-wire size: fixed header plus 4 bytes per populated sampling rate. Count
    /// clamped to capacity so a corrupt/future-larger count can never make this
    /// exceed sizeof(*this).
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        size_t const n = std::min(static_cast<size_t>(sampling_rates_count.get()), MAX_SAMPLING_RATES);
        return LENGTH + (n * sizeof(quadlet_t));
    }

    /// Clamp sampling_rates_count to capacity (serve a valid truncated descriptor).
    constexpr void clamp_trailer_count() noexcept
    {
        if (static_cast<size_t>(sampling_rates_count.get()) > MAX_SAMPLING_RATES) {
            sampling_rates_count = static_cast<uint16_t>(MAX_SAMPLING_RATES);
        }
    }

    /// View of the populated sampling_rates entries (length = sampling_rates_count).
    [[nodiscard]] auto used_sampling_rates() const noexcept -> std::span<quadlet_t const>
    {
        size_t const n = std::min(static_cast<size_t>(sampling_rates_count.get()), MAX_SAMPLING_RATES);
        return std::span<quadlet_t const>{sampling_rates.data(), n};
    }

    /// Append a sampling_rate. Updates sampling_rates_count on success.
    /// @param rate Pull-field + nominal sample-rate entry (Clause 7.3.1)
    /// @return true on success, false at MAX_SAMPLING_RATES capacity
    [[nodiscard]] auto push_sampling_rate(quadlet_t rate) noexcept -> bool
    {
        size_t const n = sampling_rates_count.get();
        if (n >= MAX_SAMPLING_RATES) {
            return false;
        }
        sampling_rates[n] = rate;
        sampling_rates_count = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset sampling_rates to empty. Backing storage is not zeroed.
    constexpr void clear_sampling_rates() noexcept { sampling_rates_count = static_cast<uint16_t>(0); }
};

static_assert(offsetof(DescriptorAudioUnit, current_sampling_rate) == 136);
static_assert(
    offsetof(DescriptorAudioUnit, sampling_rates) == DescriptorAudioUnit::LENGTH,
    "DescriptorAudioUnit.sampling_rates must start immediately after the 144-byte header");
static_assert(
    sizeof(DescriptorAudioUnit) == DescriptorAudioUnit::LENGTH + (DescriptorAudioUnit::MAX_SAMPLING_RATES * sizeof(quadlet_t)),
    "DescriptorAudioUnit must have no padding around the inline sampling_rates array");

//
// Video Unit Descriptor - Clause 7.2.4
//
/// VIDEO_UNIT Descriptor - Clause 7.2.4
struct DescriptorVideoUnit
{
    static constexpr size_t LENGTH = 136;

    doublet_t descriptor_type{DESCRIPTOR_VIDEO_UNIT};  // 0
    doublet_t descriptor_index{0};                     // 2
    AtdeccString object_name{};                        // 4
    doublet_t localized_description{0};                // 68
    doublet_t clock_domain_index{0};                   // 70
    doublet_t number_of_stream_input_ports{0};         // 72
    doublet_t base_stream_input_port{0};               // 74
    doublet_t number_of_stream_output_ports{0};        // 76
    doublet_t base_stream_output_port{0};              // 78
    doublet_t number_of_external_input_ports{0};       // 80
    doublet_t base_external_input_port{0};             // 82
    doublet_t number_of_external_output_ports{0};      // 84
    doublet_t base_external_output_port{0};            // 86
    doublet_t number_of_internal_input_ports{0};       // 88
    doublet_t base_internal_input_port{0};             // 90
    doublet_t number_of_internal_output_ports{0};      // 92
    doublet_t base_internal_output_port{0};            // 94
    doublet_t number_of_controls{0};                   // 96
    doublet_t base_control{0};                         // 98
    doublet_t number_of_signal_selectors{0};           // 100
    doublet_t base_signal_selector{0};                 // 102
    doublet_t number_of_mixers{0};                     // 104
    doublet_t base_mixer{0};                           // 106
    doublet_t number_of_matrices{0};                   // 108
    doublet_t base_matrix{0};                          // 110
    doublet_t number_of_splitters{0};                  // 112
    doublet_t base_splitter{0};                        // 114
    doublet_t number_of_combiners{0};                  // 116
    doublet_t base_combiner{0};                        // 118
    doublet_t number_of_demultiplexers{0};             // 120
    doublet_t base_demultiplexer{0};                   // 122
    doublet_t number_of_multiplexers{0};               // 124
    doublet_t base_multiplexer{0};                     // 126
    doublet_t number_of_transcoders{0};                // 128
    doublet_t base_transcoder{0};                      // 130
    doublet_t number_of_control_blocks{0};             // 132
    doublet_t base_control_block{0};                   // 134

    auto operator<=>(DescriptorVideoUnit const&) const noexcept = default;

    /// On-wire size. The current struct models only the fixed header;
    /// the IEEE 1722.1 VIDEO_UNIT variable trailer (video_cluster_formats)
    /// is not represented here, so wire_size() == LENGTH.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorVideoUnit) == 136, "DescriptorVideoUnit must be exactly 136 bytes");
static_assert(offsetof(DescriptorVideoUnit, base_control_block) == 134);

//
// Sensor Unit Descriptor - Clause 7.2.5
//
/// SENSOR_UNIT Descriptor - Clause 7.2.5
struct DescriptorSensorUnit
{
    static constexpr size_t LENGTH = 136;

    doublet_t descriptor_type{DESCRIPTOR_SENSOR_UNIT};  // 0
    doublet_t descriptor_index{0};                      // 2
    AtdeccString object_name{};                         // 4
    doublet_t localized_description{0};                 // 68
    doublet_t clock_domain_index{0};                    // 70
    doublet_t number_of_stream_input_ports{0};          // 72
    doublet_t base_stream_input_port{0};                // 74
    doublet_t number_of_stream_output_ports{0};         // 76
    doublet_t base_stream_output_port{0};               // 78
    doublet_t number_of_external_input_ports{0};        // 80
    doublet_t base_external_input_port{0};              // 82
    doublet_t number_of_external_output_ports{0};       // 84
    doublet_t base_external_output_port{0};             // 86
    doublet_t number_of_internal_input_ports{0};        // 88
    doublet_t base_internal_input_port{0};              // 90
    doublet_t number_of_internal_output_ports{0};       // 92
    doublet_t base_internal_output_port{0};             // 94
    doublet_t number_of_controls{0};                    // 96
    doublet_t base_control{0};                          // 98
    doublet_t number_of_signal_selectors{0};            // 100
    doublet_t base_signal_selector{0};                  // 102
    doublet_t number_of_mixers{0};                      // 104
    doublet_t base_mixer{0};                            // 106
    doublet_t number_of_matrices{0};                    // 108
    doublet_t base_matrix{0};                           // 110
    doublet_t number_of_splitters{0};                   // 112
    doublet_t base_splitter{0};                         // 114
    doublet_t number_of_combiners{0};                   // 116
    doublet_t base_combiner{0};                         // 118
    doublet_t number_of_demultiplexers{0};              // 120
    doublet_t base_demultiplexer{0};                    // 122
    doublet_t number_of_multiplexers{0};                // 124
    doublet_t base_multiplexer{0};                      // 126
    doublet_t number_of_transcoders{0};                 // 128
    doublet_t base_transcoder{0};                       // 130
    doublet_t number_of_control_blocks{0};              // 132
    doublet_t base_control_block{0};                    // 134

    auto operator<=>(DescriptorSensorUnit const&) const noexcept = default;

    /// On-wire size. The current struct models only the fixed header;
    /// the IEEE 1722.1 SENSOR_UNIT variable trailer (sensor_formats) is
    /// not represented here, so wire_size() == LENGTH.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSensorUnit) == 136, "DescriptorSensorUnit must be exactly 136 bytes");
static_assert(offsetof(DescriptorSensorUnit, base_control_block) == 134);

//
// Stream Descriptor - Clause 7.2.6
//
/// STREAM_INPUT/STREAM_OUTPUT Descriptor - Clause 7.2.6
///
/// Wire format: 138-byte fixed header followed by `number_of_formats` ×
/// 8-byte stream-format EUI-64 entries. IEEE 1722.1-2021 also appends
/// a `number_of_redundant_streams` trailer of doublet_t descriptor
/// indices after the stream_formats array; the current inline storage
/// models only the primary stream_formats trailer. Entities that
/// populate redundant_streams should handle that out-of-band until the
/// inline representation is extended.
///
/// Max stream_formats capped by AECP 508-byte budget:
/// (508 - 138) / 8 = 46.
struct DescriptorStream
{
    static constexpr size_t LENGTH = 138;
    static constexpr size_t MINIMUM_LENGTH = 132;                                                 // 2013 compat
    static constexpr size_t MAX_STREAM_FORMATS = (AEM_DESCRIPTOR_SIZE - LENGTH) / sizeof(Eui64);  // 46

    doublet_t descriptor_type{0};
    doublet_t descriptor_index{0};
    AtdeccString object_name{};
    doublet_t localized_description{0};
    doublet_t clock_domain_index{0};
    doublet_t stream_flags{0};
    Eui64 current_format{};
    doublet_t formats_offset{LENGTH};  // always = LENGTH on output
    doublet_t number_of_formats{0};
    Eui64 backup_talker_entity_id_0{};
    doublet_t backup_talker_unique_id_0{0};
    Eui64 backup_talker_entity_id_1{};
    doublet_t backup_talker_unique_id_1{0};
    Eui64 backup_talker_entity_id_2{};
    doublet_t backup_talker_unique_id_2{0};
    Eui64 backedup_talker_entity_id{};
    doublet_t backedup_talker_unique_id{0};
    doublet_t avb_interface_index{0};
    quadlet_t buffer_length{0};
    doublet_t redundant_offset{0};                           // 132 - new in 2021
    doublet_t number_of_redundant_streams{0};                // 134 - new in 2021
    doublet_t timing{0};                                     // 136 - new in 2021
    std::array<Eui64, MAX_STREAM_FORMATS> stream_formats{};  // 138..506

    // No defaulted operator<=>: unused tail slots in `stream_formats`
    // would corrupt equality. Compare via wire_span() or iterate the
    // populated entries explicitly.

    /// On-wire size: fixed header plus 8 bytes per populated stream_format.
    /// Does not include the redundant_streams trailer (not modeled here). The
    /// count is clamped to capacity (matching used_stream_formats) so a corrupt
    /// or future-larger count can never make this exceed sizeof(*this).
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        size_t const n = std::min(static_cast<size_t>(number_of_formats.get()), MAX_STREAM_FORMATS);
        return LENGTH + (n * sizeof(Eui64));
    }

    /// Clamp number_of_formats to the struct's capacity, so a blob-loaded count
    /// larger than we can hold serves a valid, truncated descriptor.
    constexpr void clamp_trailer_count() noexcept
    {
        if (static_cast<size_t>(number_of_formats.get()) > MAX_STREAM_FORMATS) {
            number_of_formats = static_cast<uint16_t>(MAX_STREAM_FORMATS);
        }
    }

    /// View of the populated stream_formats entries (length = number_of_formats).
    [[nodiscard]] auto used_stream_formats() const noexcept -> std::span<Eui64 const>
    {
        size_t const n = std::min(static_cast<size_t>(number_of_formats.get()), MAX_STREAM_FORMATS);
        return std::span<Eui64 const>{stream_formats.data(), n};
    }

    /// Append a stream_format entry. Updates number_of_formats on success.
    /// @param format Stream format identifier (Clause 7.3.2)
    /// @return true on success, false at MAX_STREAM_FORMATS capacity
    [[nodiscard]] auto push_stream_format(Eui64 format) noexcept -> bool
    {
        size_t const n = number_of_formats.get();
        if (n >= MAX_STREAM_FORMATS) {
            return false;
        }
        stream_formats[n] = format;
        number_of_formats = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset stream_formats to empty. Backing storage is not zeroed.
    constexpr void clear_stream_formats() noexcept { number_of_formats = static_cast<uint16_t>(0); }
};

static_assert(offsetof(DescriptorStream, descriptor_type) == 0);
static_assert(offsetof(DescriptorStream, object_name) == 4);
static_assert(offsetof(DescriptorStream, current_format) == 74);
static_assert(offsetof(DescriptorStream, buffer_length) == 128);
static_assert(
    offsetof(DescriptorStream, redundant_offset) == DescriptorStream::MINIMUM_LENGTH,
    "redundant_offset is the first 2021-only field. Its offset must equal MINIMUM_LENGTH so "
    "format_to() can gate tail-field reads on data_size >= LENGTH — see "
    "docs/ATDECC_DESCRIPTOR_2013_COMPAT.md");
static_assert(offsetof(DescriptorStream, timing) == 136);
static_assert(
    offsetof(DescriptorStream, stream_formats) == DescriptorStream::LENGTH,
    "DescriptorStream.stream_formats must start immediately after the 138-byte header");
static_assert(
    sizeof(DescriptorStream) == DescriptorStream::LENGTH + (DescriptorStream::MAX_STREAM_FORMATS * sizeof(Eui64)),
    "DescriptorStream must have no padding around the inline stream_formats array");

//
// Jack Descriptor - Clause 7.2.7
//
/// JACK_INPUT/JACK_OUTPUT Descriptor - Clause 7.2.7
struct DescriptorJack
{
    static constexpr size_t LENGTH = 78;

    doublet_t descriptor_type{0};        // 0
    doublet_t descriptor_index{0};       // 2
    AtdeccString object_name{};          // 4
    doublet_t localized_description{0};  // 68
    doublet_t jack_flags{0};             // 70
    doublet_t jack_type{0};              // 72
    doublet_t number_of_controls{0};     // 74
    doublet_t base_control{0};           // 76

    auto operator<=>(DescriptorJack const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. JACK has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorJack) == 78, "DescriptorJack must be exactly 78 bytes");
static_assert(offsetof(DescriptorJack, descriptor_type) == 0);
static_assert(offsetof(DescriptorJack, object_name) == 4);
static_assert(offsetof(DescriptorJack, jack_type) == 72);

//
// AVB Interface Descriptor - Clause 7.2.8
//
/// AVB_INTERFACE Descriptor - Clause 7.2.8
struct DescriptorAvbInterface
{
    static constexpr size_t LENGTH = 102;
    static constexpr size_t MINIMUM_LENGTH = 98;  // 2013 compat

    doublet_t descriptor_type{DESCRIPTOR_AVB_INTERFACE};
    doublet_t descriptor_index{0};
    AtdeccString object_name{};
    doublet_t localized_description{0};
    Eui48 mac_address{};
    doublet_t interface_flags{0};
    Eui64 clock_identity{};
    octet_t priority1{0};
    octet_t clock_class{0};
    doublet_t offset_scaled_log_variance{0};
    octet_t clock_accuracy{0};
    octet_t priority2{0};
    octet_t domain_number{0};
    octet_t log_sync_interval{0};      // int8_t
    octet_t log_announce_interval{0};  // int8_t
    octet_t log_pdelay_interval{0};    // int8_t
    doublet_t port_number{0};
    doublet_t number_of_controls{0};  // 98 - new in 2021
    doublet_t base_control{0};        // 100 - new in 2021

    auto operator<=>(DescriptorAvbInterface const&) const noexcept = default;

    /// On-wire size. The current struct models only the fixed header;
    /// the IEEE 1722.1-2021 AVB_INTERFACE variable trailer
    /// (msrp_mappings) is not represented here, so wire_size() == LENGTH.
    /// Loaders should use span_load_padded to safely ingest 2013-era
    /// payloads of MINIMUM_LENGTH = 98 bytes.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorAvbInterface) == 102, "DescriptorAvbInterface must be exactly 102 bytes");
static_assert(offsetof(DescriptorAvbInterface, mac_address) == 70);
static_assert(offsetof(DescriptorAvbInterface, clock_identity) == 78);
static_assert(offsetof(DescriptorAvbInterface, port_number) == 96);
static_assert(
    offsetof(DescriptorAvbInterface, number_of_controls) == DescriptorAvbInterface::MINIMUM_LENGTH,
    "number_of_controls is the first 2021-only field. Its offset must equal MINIMUM_LENGTH "
    "— see docs/ATDECC_DESCRIPTOR_2013_COMPAT.md");
static_assert(offsetof(DescriptorAvbInterface, base_control) == 100);

//
// Clock Source Descriptor - Clause 7.2.9
//
/// CLOCK_SOURCE Descriptor - Clause 7.2.9
struct DescriptorClockSource
{
    static constexpr size_t LENGTH = 86;

    doublet_t descriptor_type{DESCRIPTOR_CLOCK_SOURCE};  // 0
    doublet_t descriptor_index{0};                       // 2
    AtdeccString object_name{};                          // 4
    doublet_t localized_description{0};                  // 68
    doublet_t clock_source_flags{0};                     // 70
    doublet_t clock_source_type{0};                      // 72
    Eui64 clock_source_identifier{};                     // 74
    doublet_t clock_source_location_type{0};             // 82
    doublet_t clock_source_location_index{0};            // 84

    auto operator<=>(DescriptorClockSource const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. CLOCK_SOURCE has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorClockSource) == 86, "DescriptorClockSource must be exactly 86 bytes");
static_assert(offsetof(DescriptorClockSource, clock_source_identifier) == 74);

//
// Memory Object Descriptor - Clause 7.2.10
//
/// MEMORY_OBJECT Descriptor - Clause 7.2.10
struct DescriptorMemoryObject
{
    static constexpr size_t LENGTH = 108;

    doublet_t descriptor_type{DESCRIPTOR_MEMORY_OBJECT};  // 0
    doublet_t descriptor_index{0};                        // 2
    AtdeccString object_name{};                           // 4
    doublet_t localized_description{0};                   // 68
    doublet_t memory_object_type{0};                      // 70
    doublet_t target_descriptor_type{0};                  // 72
    doublet_t target_descriptor_index{0};                 // 74
    octlet_t start_address{0};                            // 76
    octlet_t maximum_length{0};                           // 84
    octlet_t length{0};                                   // 92
    octlet_t maximum_segment_length{0};                   // 100

    auto operator<=>(DescriptorMemoryObject const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. MEMORY_OBJECT has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorMemoryObject) == 108, "DescriptorMemoryObject must be exactly 108 bytes");
static_assert(offsetof(DescriptorMemoryObject, start_address) == 76);
static_assert(offsetof(DescriptorMemoryObject, maximum_segment_length) == 100);

//
// Locale Descriptor - Clause 7.2.11
//
/// LOCALE Descriptor - Clause 7.2.11
struct DescriptorLocale
{
    static constexpr size_t LENGTH = 72;

    doublet_t descriptor_type{DESCRIPTOR_LOCALE};
    doublet_t descriptor_index{0};
    AtdeccString locale_identifier{};
    doublet_t number_of_strings{0};
    doublet_t base_strings{0};

    auto operator<=>(DescriptorLocale const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. LOCALE has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorLocale) == 72, "DescriptorLocale must be exactly 72 bytes");

//
// Strings Descriptor - Clause 7.2.12
//
/// STRINGS Descriptor - Clause 7.2.12
/// Contains 7 localized strings
struct DescriptorStrings
{
    static constexpr size_t LENGTH = 452;

    doublet_t descriptor_type{DESCRIPTOR_STRINGS};
    doublet_t descriptor_index{0};
    AtdeccString string_0{};
    AtdeccString string_1{};
    AtdeccString string_2{};
    AtdeccString string_3{};
    AtdeccString string_4{};
    AtdeccString string_5{};
    AtdeccString string_6{};

    /// View the 7 strings as a const-pointer array for indexed access.
    [[nodiscard]] auto as_string_array() const noexcept -> std::array<AtdeccString const*, 7>
    {
        return {&string_0, &string_1, &string_2, &string_3, &string_4, &string_5, &string_6};
    }

    auto operator<=>(DescriptorStrings const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. STRINGS is a fixed
    /// layout of 7 × 64-byte strings plus a 4-byte header (452 bytes).
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorStrings) == 452, "DescriptorStrings must be exactly 452 bytes");

//
// Stream Port Descriptor - Clause 7.2.13
//
/// STREAM_PORT_INPUT/STREAM_PORT_OUTPUT Descriptor - Clause 7.2.13
struct DescriptorStreamPort
{
    static constexpr size_t LENGTH = 20;

    doublet_t descriptor_type{0};     // 0
    doublet_t descriptor_index{0};    // 2
    doublet_t clock_domain_index{0};  // 4
    doublet_t port_flags{0};          // 6
    doublet_t number_of_controls{0};  // 8
    doublet_t base_control{0};        // 10
    doublet_t number_of_clusters{0};  // 12
    doublet_t base_cluster{0};        // 14
    doublet_t number_of_maps{0};      // 16
    doublet_t base_map{0};            // 18

    auto operator<=>(DescriptorStreamPort const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. STREAM_PORT is a fixed
    /// 20-byte descriptor that references clusters/maps/controls by index
    /// range; it has no variable-length trailer of its own.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorStreamPort) == 20, "DescriptorStreamPort must be exactly 20 bytes");
static_assert(offsetof(DescriptorStreamPort, base_map) == 18);

//
// Signal Source triple - Clause 7.2.14 onward
//
/// One {signal_type, signal_index, signal_output} signal-source triple —
/// the recurring 6-byte wire unit naming the object a signal comes from.
/// Used by the port, cluster, and signal-processing descriptors'
/// signal_source field, SIGNAL_SELECTOR's current/default selection and
/// sources trailer (Clause 7.2.23), and the tail of the
/// SET/GET_SIGNAL_SELECTOR payloads (Clause 7.4.29/7.4.30).
struct SignalSource
{
    static constexpr size_t LENGTH = 6;

    doublet_t signal_type{0};    // 0
    doublet_t signal_index{0};   // 2
    doublet_t signal_output{0};  // 4

    auto operator<=>(SignalSource const&) const noexcept = default;
};

static_assert(sizeof(SignalSource) == SignalSource::LENGTH, "SignalSource must be exactly 6 bytes");

//
// External Port Descriptor - Clause 7.2.14
//
/// EXTERNAL_PORT_INPUT/EXTERNAL_PORT_OUTPUT Descriptor - Clause 7.2.14
struct DescriptorExternalPort
{
    static constexpr size_t LENGTH = 24;

    doublet_t descriptor_type{0};     // 0
    doublet_t descriptor_index{0};    // 2
    doublet_t clock_domain_index{0};  // 4
    doublet_t port_flags{0};          // 6
    doublet_t number_of_controls{0};  // 8
    doublet_t base_control{0};        // 10
    SignalSource signal_source{};     // 12
    quadlet_t block_latency{0};       // 18
    doublet_t jack_index{0};          // 22

    auto operator<=>(DescriptorExternalPort const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. EXTERNAL_PORT has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorExternalPort) == 24, "DescriptorExternalPort must be exactly 24 bytes");
static_assert(offsetof(DescriptorExternalPort, jack_index) == 22);

//
// Internal Port Descriptor - Clause 7.2.15
//
/// INTERNAL_PORT_INPUT/INTERNAL_PORT_OUTPUT Descriptor - Clause 7.2.15
struct DescriptorInternalPort
{
    static constexpr size_t LENGTH = 24;

    doublet_t descriptor_type{0};     // 0
    doublet_t descriptor_index{0};    // 2
    doublet_t clock_domain_index{0};  // 4
    doublet_t port_flags{0};          // 6
    doublet_t number_of_controls{0};  // 8
    doublet_t base_control{0};        // 10
    SignalSource signal_source{};     // 12
    quadlet_t block_latency{0};       // 18
    doublet_t internal_index{0};      // 22

    auto operator<=>(DescriptorInternalPort const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. INTERNAL_PORT has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorInternalPort) == 24, "DescriptorInternalPort must be exactly 24 bytes");
static_assert(offsetof(DescriptorInternalPort, internal_index) == 22);

//
// Audio Cluster Descriptor - Clause 7.2.16
//
/// AUDIO_CLUSTER Descriptor - Clause 7.2.16
struct DescriptorAudioCluster
{
    static constexpr size_t LENGTH = 90;
    static constexpr size_t MINIMUM_LENGTH = 87;  // 2013 compat

    doublet_t descriptor_type{DESCRIPTOR_AUDIO_CLUSTER};  // 0
    doublet_t descriptor_index{0};                        // 2
    AtdeccString object_name{};                           // 4
    doublet_t localized_description{0};                   // 68
    SignalSource signal_source{};                         // 70
    quadlet_t path_latency{0};                            // 76
    quadlet_t block_latency{0};                           // 80
    doublet_t channel_count{0};                           // 84
    octet_t format{0};                                    // 86
    octet_t aes3_data_type_reference{0};                  // 87 - new in 2021
    doublet_t aes3_data_type{0};                          // 88 - new in 2021

    auto operator<=>(DescriptorAudioCluster const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. AUDIO_CLUSTER has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorAudioCluster) == 90, "DescriptorAudioCluster must be exactly 90 bytes");
static_assert(offsetof(DescriptorAudioCluster, format) == 86);
static_assert(
    offsetof(DescriptorAudioCluster, aes3_data_type_reference) == DescriptorAudioCluster::MINIMUM_LENGTH,
    "aes3_data_type_reference is the first 2021-only field. Its offset must equal MINIMUM_LENGTH "
    "— see docs/ATDECC_DESCRIPTOR_2013_COMPAT.md");
static_assert(offsetof(DescriptorAudioCluster, aes3_data_type) == 88);

//
// Video Cluster Descriptor - Clause 7.2.17
//
/// VIDEO_CLUSTER Descriptor - Clause 7.2.17
struct DescriptorVideoCluster
{
    static constexpr size_t LENGTH = 121;

    doublet_t descriptor_type{DESCRIPTOR_VIDEO_CLUSTER};  // 0
    doublet_t descriptor_index{0};                        // 2
    AtdeccString object_name{};                           // 4
    doublet_t localized_description{0};                   // 68
    SignalSource signal_source{};                         // 70
    quadlet_t path_latency{0};                            // 76
    quadlet_t block_latency{0};                           // 80
    octet_t format{0};                                    // 84
    quadlet_t current_format_specific{0};                 // 85
    doublet_t supported_format_specifics_offset{0};       // 89
    doublet_t supported_format_specifics_count{0};        // 91
    quadlet_t current_sampling_rate{0};                   // 93
    doublet_t supported_sampling_rates_offset{0};         // 97
    doublet_t supported_sampling_rates_count{0};          // 99
    doublet_t current_aspect_ratio{0};                    // 101
    doublet_t supported_aspect_ratios_offset{0};          // 103
    doublet_t supported_aspect_ratios_count{0};           // 105
    quadlet_t current_size{0};                            // 107
    doublet_t supported_sizes_offset{0};                  // 111
    doublet_t supported_sizes_count{0};                   // 113
    doublet_t current_color_space{0};                     // 115
    doublet_t supported_color_spaces_offset{0};           // 117
    doublet_t supported_color_spaces_count{0};            // 119

    auto operator<=>(DescriptorVideoCluster const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. VIDEO_CLUSTER has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorVideoCluster) == 121, "DescriptorVideoCluster must be exactly 121 bytes");
static_assert(offsetof(DescriptorVideoCluster, supported_color_spaces_count) == 119);

//
// Sensor Cluster Descriptor - Clause 7.2.18
//
/// SENSOR_CLUSTER Descriptor - Clause 7.2.18
struct DescriptorSensorCluster
{
    static constexpr size_t LENGTH = 104;

    doublet_t descriptor_type{DESCRIPTOR_SENSOR_CLUSTER};  // 0
    doublet_t descriptor_index{0};                         // 2
    AtdeccString object_name{};                            // 4
    doublet_t localized_description{0};                    // 68
    SignalSource signal_source{};                          // 70
    quadlet_t path_latency{0};                             // 76
    quadlet_t block_latency{0};                            // 80
    Eui64 current_format{};                                // 84
    doublet_t supported_formats_offset{0};                 // 92
    doublet_t supported_formats_count{0};                  // 94
    quadlet_t current_sampling_rate{0};                    // 96
    doublet_t supported_sampling_rates_offset{0};          // 100
    doublet_t supported_sampling_rates_count{0};           // 102

    auto operator<=>(DescriptorSensorCluster const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. SENSOR_CLUSTER has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSensorCluster) == 104, "DescriptorSensorCluster must be exactly 104 bytes");
static_assert(offsetof(DescriptorSensorCluster, supported_sampling_rates_count) == 102);

//
// Audio Map Descriptor - Clause 7.2.19
//

/// A single (stream_channel -> cluster_channel) mapping entry in an
/// AUDIO_MAP descriptor trailer. IEEE 1722.1 Clause 7.2.19, Figure 7.27.
/// On-wire layout is 8 bytes of network-byte-order doublets — mapped
/// 1:1 onto the C++ struct below.
struct AudioMapping
{
    doublet_t mapping_stream_index{0};     ///< Stream index within the AUDIO_UNIT (0 .. n-1)
    doublet_t mapping_stream_channel{0};   ///< Channel number within that stream
    doublet_t mapping_cluster_offset{0};   ///< Offset into the target cluster (0 .. number_of_clusters-1)
    doublet_t mapping_cluster_channel{0};  ///< Channel number within the target cluster
};

static_assert(sizeof(AudioMapping) == 8, "AudioMapping must be exactly 8 bytes");

/// AUDIO_MAP Descriptor - Clause 7.2.19
///
/// Wire format is an 8-byte fixed header followed by `number_of_mappings`
/// × 8-byte `AudioMapping` entries. IEEE 1722.1 Clause 7.2.19 caps the
/// count at 62 mappings per descriptor, which coincidentally also fits
/// within MAX_AEM_DESCRIPTOR_SIZE (8 + 62*8 = 504 ≤ 508).
///
/// The `mappings` trailer is stored inline as typed storage so handlers
/// never do byte-offset math. `wire_size()` returns the actual on-wire
/// length based on `number_of_mappings.get()`.
struct DescriptorAudioMap
{
    /// Fixed header size — also the minimum wire size (empty map).
    /// Kept named LENGTH for symmetry with the other descriptors and
    /// for the atdecc_aem_print.hpp dispatch table.
    static constexpr size_t LENGTH = 8;

    /// IEEE 1722.1 Clause 7.2.19: a descriptor may contain a maximum of
    /// 62 mapping structures. This bound is tighter than the AECP
    /// 508-byte budget (62*8+8 = 504), so 62 is the limit to enforce.
    static constexpr size_t MAX_MAPPINGS = 62;

    doublet_t descriptor_type{DESCRIPTOR_AUDIO_MAP};    // 0
    doublet_t descriptor_index{0};                      // 2
    doublet_t mappings_offset{LENGTH};                  // 4: always = LENGTH on output
    doublet_t number_of_mappings{0};                    // 6
    std::array<AudioMapping, MAX_MAPPINGS> mappings{};  // 8..504

    // No defaulted operator<=>: the unused tail entries in the `mappings`
    // array would participate in comparisons and produce surprising
    // results for maps whose number_of_mappings differ but whose
    // populated prefix is identical. Compare by `wire_span(lhs) == wire_span(rhs)`
    // or by inspecting the first `number_of_mappings` entries explicitly.

    /// On-wire size in a READ_DESCRIPTOR response: fixed header plus 8 bytes per
    /// populated mapping entry. Count clamped to capacity so a corrupt/future-
    /// larger count can never make this exceed sizeof(*this).
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        size_t const n = std::min(static_cast<size_t>(number_of_mappings.get()), MAX_MAPPINGS);
        return LENGTH + (n * sizeof(AudioMapping));
    }

    /// Clamp number_of_mappings to capacity (serve a valid truncated descriptor).
    constexpr void clamp_trailer_count() noexcept
    {
        if (static_cast<size_t>(number_of_mappings.get()) > MAX_MAPPINGS) {
            number_of_mappings = static_cast<uint16_t>(MAX_MAPPINGS);
        }
    }

    /// View of the populated mappings (length = number_of_mappings).
    [[nodiscard]] auto used_mappings() const noexcept -> std::span<AudioMapping const>
    {
        size_t const n = std::min(static_cast<size_t>(number_of_mappings.get()), MAX_MAPPINGS);
        return std::span<AudioMapping const>{mappings.data(), n};
    }

    /// Append a mapping entry. Updates number_of_mappings on success.
    /// @param entry AudioMapping entry to append
    /// @return true on success, false at MAX_MAPPINGS capacity
    [[nodiscard]] auto push_mapping(AudioMapping entry) noexcept -> bool
    {
        size_t const n = number_of_mappings.get();
        if (n >= MAX_MAPPINGS) {
            return false;
        }
        mappings[n] = entry;
        number_of_mappings = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset mappings to empty. Backing storage is not zeroed.
    constexpr void clear_mappings() noexcept { number_of_mappings = static_cast<uint16_t>(0); }
};

static_assert(
    offsetof(DescriptorAudioMap, mappings) == DescriptorAudioMap::LENGTH,
    "DescriptorAudioMap.mappings must start immediately after the fixed header");
static_assert(
    sizeof(DescriptorAudioMap) == DescriptorAudioMap::LENGTH + (DescriptorAudioMap::MAX_MAPPINGS * sizeof(AudioMapping)),
    "DescriptorAudioMap must have no padding around the inline mappings array");

//
// Video Map Descriptor - Clause 7.2.20
//

/// A single mapping entry in a VIDEO_MAP descriptor trailer.
/// IEEE 1722.1 Clause 7.2.20, 8 bytes on the wire.
struct VideoMapping
{
    doublet_t mapping_stream_index{0};       ///< Stream index within the VIDEO_UNIT
    doublet_t mapping_program_stream{0};     ///< MPEG program stream identifier
    doublet_t mapping_elementary_stream{0};  ///< MPEG elementary stream identifier
    doublet_t mapping_cluster_offset{0};     ///< Offset into the target video cluster
};

static_assert(sizeof(VideoMapping) == 8, "VideoMapping must be exactly 8 bytes");

/// VIDEO_MAP Descriptor - Clause 7.2.20
///
/// Wire format: 8-byte fixed header followed by `number_of_mappings` ×
/// 8-byte `VideoMapping` entries. Like AUDIO_MAP, the trailer is stored
/// inline as typed storage and the maximum is 62 entries (fits within
/// the 508-byte AECP budget).
struct DescriptorVideoMap
{
    static constexpr size_t LENGTH = 8;
    static constexpr size_t MAX_MAPPINGS = 62;

    doublet_t descriptor_type{DESCRIPTOR_VIDEO_MAP};    // 0
    doublet_t descriptor_index{0};                      // 2
    doublet_t mappings_offset{LENGTH};                  // 4
    doublet_t number_of_mappings{0};                    // 6
    std::array<VideoMapping, MAX_MAPPINGS> mappings{};  // 8..504

    // No defaulted operator<=>: unused tail entries in `mappings` would
    // corrupt comparisons. See DescriptorAudioMap for the same rationale.

    /// On-wire size: fixed header plus 8 bytes per populated mapping.
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        return LENGTH + (static_cast<size_t>(number_of_mappings.get()) * sizeof(VideoMapping));
    }

    /// View of the populated mappings (length = number_of_mappings).
    [[nodiscard]] auto used_mappings() const noexcept -> std::span<VideoMapping const>
    {
        size_t const n = std::min(static_cast<size_t>(number_of_mappings.get()), MAX_MAPPINGS);
        return std::span<VideoMapping const>{mappings.data(), n};
    }

    /// Append a mapping entry. Updates number_of_mappings on success.
    /// @param entry VideoMapping entry to append
    /// @return true on success, false at MAX_MAPPINGS capacity
    [[nodiscard]] auto push_mapping(VideoMapping entry) noexcept -> bool
    {
        size_t const n = number_of_mappings.get();
        if (n >= MAX_MAPPINGS) {
            return false;
        }
        mappings[n] = entry;
        number_of_mappings = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset mappings to empty. Backing storage is not zeroed.
    constexpr void clear_mappings() noexcept { number_of_mappings = static_cast<uint16_t>(0); }
};

static_assert(
    offsetof(DescriptorVideoMap, mappings) == DescriptorVideoMap::LENGTH,
    "DescriptorVideoMap.mappings must start immediately after the fixed header");
static_assert(
    sizeof(DescriptorVideoMap) == DescriptorVideoMap::LENGTH + (DescriptorVideoMap::MAX_MAPPINGS * sizeof(VideoMapping)),
    "DescriptorVideoMap must have no padding around the inline mappings array");

//
// Sensor Map Descriptor - Clause 7.2.21
//

/// A single mapping entry in a SENSOR_MAP descriptor trailer.
/// IEEE 1722.1 Clause 7.2.21, 8 bytes on the wire.
struct SensorMapping
{
    doublet_t mapping_stream_index{0};     ///< Stream index within the SENSOR_UNIT
    doublet_t mapping_stream_channel{0};   ///< Channel number within that stream
    doublet_t mapping_cluster_offset{0};   ///< Offset into the target sensor cluster
    doublet_t mapping_cluster_channel{0};  ///< Channel number within the target cluster
};

static_assert(sizeof(SensorMapping) == 8, "SensorMapping must be exactly 8 bytes");

/// SENSOR_MAP Descriptor - Clause 7.2.21
///
/// Wire format: 8-byte fixed header followed by `number_of_mappings` ×
/// 8-byte `SensorMapping` entries. Maximum 62 entries.
struct DescriptorSensorMap
{
    static constexpr size_t LENGTH = 8;
    static constexpr size_t MAX_MAPPINGS = 62;

    doublet_t descriptor_type{DESCRIPTOR_SENSOR_MAP};    // 0
    doublet_t descriptor_index{0};                       // 2
    doublet_t mappings_offset{LENGTH};                   // 4
    doublet_t number_of_mappings{0};                     // 6
    std::array<SensorMapping, MAX_MAPPINGS> mappings{};  // 8..504

    // No defaulted operator<=>: unused tail entries in `mappings` would
    // corrupt comparisons. See DescriptorAudioMap for the same rationale.

    /// On-wire size: fixed header plus 8 bytes per populated mapping.
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        return LENGTH + (static_cast<size_t>(number_of_mappings.get()) * sizeof(SensorMapping));
    }

    /// View of the populated mappings (length = number_of_mappings).
    [[nodiscard]] auto used_mappings() const noexcept -> std::span<SensorMapping const>
    {
        size_t const n = std::min(static_cast<size_t>(number_of_mappings.get()), MAX_MAPPINGS);
        return std::span<SensorMapping const>{mappings.data(), n};
    }

    /// Append a mapping entry. Updates number_of_mappings on success.
    /// @param entry SensorMapping entry to append
    /// @return true on success, false at MAX_MAPPINGS capacity
    [[nodiscard]] auto push_mapping(SensorMapping entry) noexcept -> bool
    {
        size_t const n = number_of_mappings.get();
        if (n >= MAX_MAPPINGS) {
            return false;
        }
        mappings[n] = entry;
        number_of_mappings = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset mappings to empty. Backing storage is not zeroed.
    constexpr void clear_mappings() noexcept { number_of_mappings = static_cast<uint16_t>(0); }
};

static_assert(
    offsetof(DescriptorSensorMap, mappings) == DescriptorSensorMap::LENGTH,
    "DescriptorSensorMap.mappings must start immediately after the fixed header");
static_assert(
    sizeof(DescriptorSensorMap) == DescriptorSensorMap::LENGTH + (DescriptorSensorMap::MAX_MAPPINGS * sizeof(SensorMapping)),
    "DescriptorSensorMap must have no padding around the inline mappings array");

//
// Control Descriptor - Clause 7.2.22
//
/// CONTROL Descriptor - Clause 7.2.22
///
/// The fixed header is 104 bytes. Immediately following is a variable
/// value_details trailer whose layout is controlled by the
/// control_value_type field; up to 404 octets per IEEE 1722.1-2021
/// Clause 7.2.22. We reserve that much inline storage here so the
/// trailer travels with the struct and can be queried / populated
/// through the typed accessors without caller-managed allocation.
///
/// Total storage: 104 + 404 = 508 bytes (matches the AECP MAX_AEM_DESCRIPTOR_SIZE).
///
/// `value_details_bytes()` returns a span of exactly the populated
/// trailer bytes, computed from `control_value_type` and
/// `number_of_values`. Typed accessors for each family
/// (linear_entries<T>, selector_view<T>, array_view<T>, smpte_time,
/// sample_rate, gptp_time, utf8, vendor, bode_plot_header /
/// bode_plot_points) live in atdecc_aem_descriptor.hpp's non-member
/// helpers — see the end of this file.
struct DescriptorControl
{
    static constexpr size_t LENGTH = 104;
    static constexpr size_t MAX_VALUE_DETAILS = 404;

    doublet_t descriptor_type{DESCRIPTOR_CONTROL};  // 0
    doublet_t descriptor_index{0};                  // 2
    AtdeccString object_name{};                     // 4
    doublet_t localized_description{0};             // 68
    quadlet_t block_latency{0};                     // 70
    quadlet_t control_latency{0};                   // 74
    doublet_t control_domain{0};                    // 78
    doublet_t control_value_type{0};                // 80
    Eui64 control_type{};                           // 82
    quadlet_t reset_time{0};                        // 90
    doublet_t values_offset{LENGTH};                // 94 — always LENGTH on output
    doublet_t number_of_values{0};                  // 96
    SignalSource signal_source{};                   // 98

    /// Variable-length value_details trailer (value_type-specific layout).
    /// Only the first `value_details_length()` bytes are populated.
    std::array<uint8_t, MAX_VALUE_DETAILS> value_details{};  // 104..507

    // No defaulted operator<=>: unused tail bytes in `value_details`
    // would produce surprising equality. Compare via wire_span() on
    // a wire_size()-length view, or field-by-field.

    /// On-wire length of the populated value_details trailer. Returns 0
    /// for a control whose control_value_type family or element size
    /// cannot be determined here; callers should treat the trailer as
    /// opaque in that case.
    [[nodiscard]] auto value_details_length() const noexcept -> size_t;

    /// On-wire size = fixed LENGTH + value_details_length().
    [[nodiscard]] auto wire_size() const noexcept -> size_t { return LENGTH + value_details_length(); }

    /// Read view of the populated value_details bytes.
    [[nodiscard]] auto value_details_bytes() const noexcept -> std::span<uint8_t const>
    {
        return std::span<uint8_t const>{value_details.data(), value_details_length()};
    }

    /// Mutable view of the full value_details buffer, for typed writers.
    [[nodiscard]] auto value_details_bytes_mutable() noexcept -> std::span<uint8_t> { return std::span<uint8_t>{value_details}; }
};

static_assert(sizeof(DescriptorControl) == 508, "DescriptorControl must be exactly 508 bytes (104 header + 404 trailer)");
static_assert(offsetof(DescriptorControl, signal_source) == 98);
static_assert(
    offsetof(DescriptorControl, value_details) == DescriptorControl::LENGTH,
    "DescriptorControl.value_details must start immediately after the 104-byte header");

//
// Signal Selector Descriptor - Clause 7.2.23
//
/// SIGNAL_SELECTOR Descriptor - Clause 7.2.23
struct DescriptorSignalSelector
{
    static constexpr size_t LENGTH = 96;

    doublet_t descriptor_type{DESCRIPTOR_SIGNAL_SELECTOR};  // 0
    doublet_t descriptor_index{0};                          // 2
    AtdeccString object_name{};                             // 4
    doublet_t localized_description{0};                     // 68
    quadlet_t block_latency{0};                             // 70
    quadlet_t control_latency{0};                           // 74
    doublet_t control_domain{0};                            // 78
    doublet_t sources_offset{0};                            // 80
    doublet_t number_of_sources{0};                         // 82
    SignalSource current_signal{};                          // 84
    SignalSource default_signal{};                          // 90

    auto operator<=>(DescriptorSignalSelector const&) const noexcept = default;

    /// On-wire size. SIGNAL_SELECTOR's sources trailer is not yet modeled
    /// inline; wire_size() returns LENGTH until the inline representation
    /// is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSignalSelector) == 96, "DescriptorSignalSelector must be exactly 96 bytes");
static_assert(offsetof(DescriptorSignalSelector, current_signal) == 84);
static_assert(offsetof(DescriptorSignalSelector, default_signal) == 90);

//
// Mixer Descriptor - Clause 7.2.24
//
/// MIXER Descriptor - Clause 7.2.24
struct DescriptorMixer
{
    static constexpr size_t LENGTH = 88;

    doublet_t descriptor_type{DESCRIPTOR_MIXER};  // 0
    doublet_t descriptor_index{0};                // 2
    AtdeccString object_name{};                   // 4
    doublet_t localized_description{0};           // 68
    quadlet_t block_latency{0};                   // 70
    quadlet_t control_latency{0};                 // 74
    doublet_t control_domain{0};                  // 78
    doublet_t control_value_type{0};              // 80
    doublet_t sources_offset{0};                  // 82
    doublet_t number_of_sources{0};               // 84
    doublet_t value_offset{0};                    // 86

    auto operator<=>(DescriptorMixer const&) const noexcept = default;

    /// On-wire size. MIXER has multiple conceptual trailers (sources
    /// and a mixer value); neither is modeled inline yet. wire_size()
    /// returns LENGTH until the inline representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorMixer) == 88, "DescriptorMixer must be exactly 88 bytes");
static_assert(offsetof(DescriptorMixer, value_offset) == 86);

//
// Matrix Descriptor - Clause 7.2.25
//
/// MATRIX Descriptor - Clause 7.2.25
struct DescriptorMatrix
{
    static constexpr size_t LENGTH = 102;

    doublet_t descriptor_type{DESCRIPTOR_MATRIX};  // 0
    doublet_t descriptor_index{0};                 // 2
    AtdeccString object_name{};                    // 4
    doublet_t localized_description{0};            // 68
    quadlet_t block_latency{0};                    // 70
    quadlet_t control_latency{0};                  // 74
    doublet_t control_domain{0};                   // 78
    doublet_t control_value_type{0};               // 80
    Eui64 control_type{};                          // 82
    doublet_t width{0};                            // 90
    doublet_t height{0};                           // 92
    doublet_t values_offset{0};                    // 94
    doublet_t number_of_values{0};                 // 96
    doublet_t number_of_sources{0};                // 98
    doublet_t base_source{0};                      // 100

    auto operator<=>(DescriptorMatrix const&) const noexcept = default;

    /// On-wire size. MATRIX's values trailer is not yet modeled inline;
    /// wire_size() returns LENGTH until the inline representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorMatrix) == 102, "DescriptorMatrix must be exactly 102 bytes");
static_assert(offsetof(DescriptorMatrix, base_source) == 100);

//
// Matrix Signal Descriptor - Clause 7.2.26
//
/// MATRIX_SIGNAL Descriptor - Clause 7.2.26
struct DescriptorMatrixSignal
{
    static constexpr size_t LENGTH = 8;

    doublet_t descriptor_type{DESCRIPTOR_MATRIX_SIGNAL};  // 0
    doublet_t descriptor_index{0};                        // 2
    doublet_t signals_offset{0};                          // 4
    doublet_t signals_count{0};                           // 6

    auto operator<=>(DescriptorMatrixSignal const&) const noexcept = default;

    /// On-wire size. MATRIX_SIGNAL's signals trailer is not yet modeled
    /// inline; wire_size() returns LENGTH until the inline representation
    /// is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorMatrixSignal) == 8, "DescriptorMatrixSignal must be exactly 8 bytes");

//
// Signal Splitter Descriptor - Clause 7.2.27
//
/// SIGNAL_SPLITTER Descriptor - Clause 7.2.27
struct DescriptorSignalSplitter
{
    static constexpr size_t LENGTH = 92;

    doublet_t descriptor_type{DESCRIPTOR_SIGNAL_SPLITTER};  // 0
    doublet_t descriptor_index{0};                          // 2
    AtdeccString object_name{};                             // 4
    doublet_t localized_description{0};                     // 68
    quadlet_t block_latency{0};                             // 70
    quadlet_t control_latency{0};                           // 74
    doublet_t control_domain{0};                            // 78
    SignalSource signal_source{};                           // 80
    doublet_t number_of_outputs{0};                         // 86
    doublet_t splitter_map_count{0};                        // 88
    doublet_t splitter_map_offset{0};                       // 90

    auto operator<=>(DescriptorSignalSplitter const&) const noexcept = default;

    /// On-wire size. SIGNAL_SPLITTER's splitter_map / sources trailers
    /// are not yet modeled inline; wire_size() returns LENGTH until the
    /// inline representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSignalSplitter) == 92, "DescriptorSignalSplitter must be exactly 92 bytes");
static_assert(offsetof(DescriptorSignalSplitter, splitter_map_offset) == 90);

//
// Signal Combiner Descriptor - Clause 7.2.28
//
/// SIGNAL_COMBINER Descriptor - Clause 7.2.28
struct DescriptorSignalCombiner
{
    static constexpr size_t LENGTH = 88;

    doublet_t descriptor_type{DESCRIPTOR_SIGNAL_COMBINER};  // 0
    doublet_t descriptor_index{0};                          // 2
    AtdeccString object_name{};                             // 4
    doublet_t localized_description{0};                     // 68
    quadlet_t block_latency{0};                             // 70
    quadlet_t control_latency{0};                           // 74
    doublet_t control_domain{0};                            // 78
    doublet_t combiner_map_count{0};                        // 80
    doublet_t combiner_map_offset{0};                       // 82
    doublet_t sources_offset{0};                            // 84
    doublet_t number_of_sources{0};                         // 86

    auto operator<=>(DescriptorSignalCombiner const&) const noexcept = default;

    /// On-wire size. SIGNAL_COMBINER's combiner_map / sources trailers
    /// are not yet modeled inline; wire_size() returns LENGTH until the
    /// inline representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSignalCombiner) == 88, "DescriptorSignalCombiner must be exactly 88 bytes");
static_assert(offsetof(DescriptorSignalCombiner, number_of_sources) == 86);

//
// Signal Demultiplexer Descriptor - Clause 7.2.29
//
/// SIGNAL_DEMULTIPLEXER Descriptor - Clause 7.2.29
struct DescriptorSignalDemultiplexer
{
    static constexpr size_t LENGTH = 92;

    doublet_t descriptor_type{DESCRIPTOR_SIGNAL_DEMULTIPLEXER};  // 0
    doublet_t descriptor_index{0};                               // 2
    AtdeccString object_name{};                                  // 4
    doublet_t localized_description{0};                          // 68
    quadlet_t block_latency{0};                                  // 70
    quadlet_t control_latency{0};                                // 74
    doublet_t control_domain{0};                                 // 78
    SignalSource signal_source{};                                // 80
    doublet_t number_of_outputs{0};                              // 86
    doublet_t demultiplexer_map_count{0};                        // 88
    doublet_t demultiplexer_map_offset{0};                       // 90

    auto operator<=>(DescriptorSignalDemultiplexer const&) const noexcept = default;

    /// On-wire size. SIGNAL_DEMULTIPLEXER's demultiplexer_map trailer
    /// is not yet modeled inline; wire_size() returns LENGTH until the
    /// inline representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSignalDemultiplexer) == 92, "DescriptorSignalDemultiplexer must be exactly 92 bytes");
static_assert(offsetof(DescriptorSignalDemultiplexer, demultiplexer_map_offset) == 90);

//
// Signal Multiplexer Descriptor - Clause 7.2.30
//
/// SIGNAL_MULTIPLEXER Descriptor - Clause 7.2.30
struct DescriptorSignalMultiplexer
{
    static constexpr size_t LENGTH = 88;

    doublet_t descriptor_type{DESCRIPTOR_SIGNAL_MULTIPLEXER};  // 0
    doublet_t descriptor_index{0};                             // 2
    AtdeccString object_name{};                                // 4
    doublet_t localized_description{0};                        // 68
    quadlet_t block_latency{0};                                // 70
    quadlet_t control_latency{0};                              // 74
    doublet_t control_domain{0};                               // 78
    doublet_t multiplexer_map_count{0};                        // 80
    doublet_t multiplexer_map_offset{0};                       // 82
    doublet_t sources_offset{0};                               // 84
    doublet_t number_of_sources{0};                            // 86

    auto operator<=>(DescriptorSignalMultiplexer const&) const noexcept = default;

    /// On-wire size. SIGNAL_MULTIPLEXER's multiplexer_map / sources
    /// trailers are not yet modeled inline; wire_size() returns LENGTH
    /// until the inline representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSignalMultiplexer) == 88, "DescriptorSignalMultiplexer must be exactly 88 bytes");
static_assert(offsetof(DescriptorSignalMultiplexer, number_of_sources) == 86);

//
// Signal Transcoder Descriptor - Clause 7.2.31
//
/// SIGNAL_TRANSCODER Descriptor - Clause 7.2.31
struct DescriptorSignalTranscoder
{
    static constexpr size_t LENGTH = 100;
    static constexpr size_t MINIMUM_LENGTH = 92;  // 2013 compat

    doublet_t descriptor_type{DESCRIPTOR_SIGNAL_TRANSCODER};  // 0
    doublet_t descriptor_index{0};                            // 2
    AtdeccString object_name{};                               // 4
    doublet_t localized_description{0};                       // 68
    quadlet_t block_latency{0};                               // 70
    quadlet_t control_latency{0};                             // 74
    doublet_t control_domain{0};                              // 78
    doublet_t control_value_type{0};                          // 80
    doublet_t values_offset{0};                               // 82
    doublet_t number_of_values{0};                            // 84
    SignalSource signal_source{};                             // 86
    Eui64 transcoder_type{};                                  // 92 - new in 2021

    auto operator<=>(DescriptorSignalTranscoder const&) const noexcept = default;

    /// On-wire size. SIGNAL_TRANSCODER's values trailer is not yet
    /// modeled inline; wire_size() returns LENGTH until the inline
    /// representation is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorSignalTranscoder) == 100, "DescriptorSignalTranscoder must be exactly 100 bytes");
static_assert(offsetof(DescriptorSignalTranscoder, signal_source) == 86);
static_assert(
    offsetof(DescriptorSignalTranscoder, transcoder_type) == DescriptorSignalTranscoder::MINIMUM_LENGTH,
    "transcoder_type is the first 2021-only field. Its offset must equal MINIMUM_LENGTH so "
    "format_to(descriptor, data_size) can gate the read on data_size >= LENGTH — see "
    "docs/ATDECC_DESCRIPTOR_2013_COMPAT.md");

//
// Clock Domain Descriptor - Clause 7.2.32
//
/// CLOCK_DOMAIN Descriptor - Clause 7.2.32
///
/// Wire format: 76-byte fixed header followed by `clock_sources_count`
/// × 2-byte CLOCK_SOURCE descriptor-index entries. The trailer lists
/// the set of clock sources available to this clock domain; each entry
/// is a descriptor_index into the CLOCK_SOURCE descriptor list.
///
/// Max clock_sources capped by AECP 508-byte budget:
/// (508 - 76) / 2 = 216.
struct DescriptorClockDomain
{
    static constexpr size_t LENGTH = 76;
    static constexpr size_t MAX_CLOCK_SOURCES = (AEM_DESCRIPTOR_SIZE - LENGTH) / sizeof(doublet_t);  // 216

    doublet_t descriptor_type{DESCRIPTOR_CLOCK_DOMAIN};
    doublet_t descriptor_index{0};
    AtdeccString object_name;
    doublet_t localized_description{0};
    doublet_t clock_source_index{0};
    doublet_t clock_sources_offset{LENGTH};  // always = LENGTH on output
    doublet_t clock_sources_count{0};
    std::array<doublet_t, MAX_CLOCK_SOURCES> clock_sources{};  // 76..508

    // No defaulted operator<=>: unused tail slots in `clock_sources`
    // would corrupt equality. Compare via wire_span() or iterate the
    // first `clock_sources_count` entries explicitly.

    /// On-wire size: fixed header plus 2 bytes per populated clock_source entry.
    [[nodiscard]] auto wire_size() const noexcept -> size_t
    {
        return LENGTH + (static_cast<size_t>(clock_sources_count.get()) * sizeof(doublet_t));
    }

    /// View of the populated clock_source entries (length = clock_sources_count).
    /// Unused tail slots in the fixed-size backing array are not included.
    [[nodiscard]] auto used_clock_sources() const noexcept -> std::span<doublet_t const>
    {
        size_t const n = std::min(static_cast<size_t>(clock_sources_count.get()), MAX_CLOCK_SOURCES);
        return std::span<doublet_t const>{clock_sources.data(), n};
    }

    /// Append a clock_source index. Updates clock_sources_count on success.
    /// @param source_index Descriptor index of the CLOCK_SOURCE to append
    /// @return true on success, false if the array is at MAX_CLOCK_SOURCES capacity
    [[nodiscard]] auto push_clock_source(doublet_t source_index) noexcept -> bool
    {
        size_t const n = clock_sources_count.get();
        if (n >= MAX_CLOCK_SOURCES) {
            return false;
        }
        clock_sources[n] = source_index;
        clock_sources_count = static_cast<uint16_t>(n + 1);
        return true;
    }

    /// Reset the clock_sources list to empty. Does not zero the backing
    /// storage; used_clock_sources() will return an empty span afterward.
    constexpr void clear_clock_sources() noexcept { clock_sources_count = static_cast<uint16_t>(0); }
};

static_assert(
    offsetof(DescriptorClockDomain, clock_sources) == DescriptorClockDomain::LENGTH,
    "DescriptorClockDomain.clock_sources must start immediately after the 76-byte header");
static_assert(
    sizeof(DescriptorClockDomain) == DescriptorClockDomain::LENGTH + (DescriptorClockDomain::MAX_CLOCK_SOURCES * sizeof(doublet_t)),
    "DescriptorClockDomain must have no padding around the inline clock_sources array");

//
// Control Block Descriptor - Clause 7.2.33
//
/// CONTROL_BLOCK Descriptor - Clause 7.2.33
struct DescriptorControlBlock
{
    static constexpr size_t LENGTH = 82;
    static constexpr size_t MINIMUM_LENGTH = 76;  // 2013 compat

    doublet_t descriptor_type{DESCRIPTOR_CONTROL_BLOCK};  // 0
    doublet_t descriptor_index{0};                        // 2
    AtdeccString object_name;                             // 4
    doublet_t localized_description{0};                   // 68
    doublet_t number_of_controls{0};                      // 70
    doublet_t base_control{0};                            // 72
    doublet_t final_control_index{0};                     // 74
    SignalSource signal_source{};                         // 76 - new in 2021

    auto operator<=>(DescriptorControlBlock const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. CONTROL_BLOCK has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorControlBlock) == 82, "DescriptorControlBlock must be exactly 82 bytes");
static_assert(offsetof(DescriptorControlBlock, localized_description) == 68);
static_assert(offsetof(DescriptorControlBlock, final_control_index) == 74);
static_assert(
    offsetof(DescriptorControlBlock, signal_source) == DescriptorControlBlock::MINIMUM_LENGTH,
    "signal_source is the first 2021-only field. Its offset must equal MINIMUM_LENGTH so "
    "format_to(descriptor, data_size) can gate the read on data_size >= LENGTH — see "
    "docs/ATDECC_DESCRIPTOR_2013_COMPAT.md");

//
// Timing Descriptor - Clause 7.2.34
//
/// TIMING Descriptor - Clause 7.2.34
struct DescriptorTiming
{
    static constexpr size_t LENGTH = 76;

    doublet_t descriptor_type{DESCRIPTOR_TIMING};  // 0
    doublet_t descriptor_index{0};                 // 2
    AtdeccString object_name{};                    // 4
    doublet_t localized_description{0};            // 68
    doublet_t algorithm{0};                        // 70
    doublet_t ptp_instances_offset{0};             // 72
    doublet_t number_of_ptp_instances{0};          // 74

    auto operator<=>(DescriptorTiming const&) const noexcept = default;

    /// On-wire size. TIMING's ptp_instances trailer is not yet modeled
    /// inline; wire_size() returns LENGTH until the inline representation
    /// is added.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorTiming) == 76, "DescriptorTiming must be exactly 76 bytes");
static_assert(offsetof(DescriptorTiming, algorithm) == 70);
static_assert(offsetof(DescriptorTiming, number_of_ptp_instances) == 74);

//
// PTP Instance Descriptor - Clause 7.2.35
//
/// PTP_INSTANCE Descriptor - Clause 7.2.35
struct DescriptorPtpInstance
{
    static constexpr size_t LENGTH = 90;

    doublet_t descriptor_type{DESCRIPTOR_PTP_INSTANCE};  // 0
    doublet_t descriptor_index{0};                       // 2
    AtdeccString object_name{};                          // 4
    doublet_t localized_description{0};                  // 68
    Eui64 clock_identity{};                              // 70
    quadlet_t flags{0};                                  // 78
    doublet_t number_of_controls{0};                     // 82
    doublet_t base_control{0};                           // 84
    doublet_t number_of_ptp_ports{0};                    // 86
    doublet_t base_ptp_port{0};                          // 88

    auto operator<=>(DescriptorPtpInstance const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. PTP_INSTANCE has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorPtpInstance) == 90, "DescriptorPtpInstance must be exactly 90 bytes");
static_assert(offsetof(DescriptorPtpInstance, clock_identity) == 70);
static_assert(offsetof(DescriptorPtpInstance, flags) == 78);
static_assert(offsetof(DescriptorPtpInstance, base_ptp_port) == 88);

//
// PTP Port Descriptor - Clause 7.2.36
//
/// PTP_PORT Descriptor - Clause 7.2.36
struct DescriptorPtpPort
{
    static constexpr size_t LENGTH = 86;

    doublet_t descriptor_type{DESCRIPTOR_PTP_PORT};  // 0
    doublet_t descriptor_index{0};                   // 2
    AtdeccString object_name{};                      // 4
    doublet_t localized_description{0};              // 68
    doublet_t port_number{0};                        // 70
    doublet_t port_type{0};                          // 72
    quadlet_t flags{0};                              // 74
    doublet_t avb_interface_index{0};                // 78
    Eui48 profile_identifier{};                      // 80

    auto operator<=>(DescriptorPtpPort const&) const noexcept = default;

    /// On-wire size in a READ_DESCRIPTOR response. PTP_PORT has no variable trailer.
    [[nodiscard]] static constexpr auto wire_size() noexcept -> size_t { return LENGTH; }
};

static_assert(sizeof(DescriptorPtpPort) == 86, "DescriptorPtpPort must be exactly 86 bytes");
static_assert(offsetof(DescriptorPtpPort, port_number) == 70);
static_assert(offsetof(DescriptorPtpPort, flags) == 74);
static_assert(offsetof(DescriptorPtpPort, profile_identifier) == 80);

}  // namespace statusbar::atdecc::aem

// Serialization traits for wire format structs
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::AtdeccString> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorEntity> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorConfiguration> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorStream> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorJack> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorAvbInterface> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorClockSource> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorLocale> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorStrings> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorClockDomain> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorControlBlock> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorAudioUnit> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorVideoUnit> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSensorUnit> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorMemoryObject> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorStreamPort> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorExternalPort> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorInternalPort> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorAudioCluster> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorVideoCluster> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSensorCluster> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorAudioMap> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorVideoMap> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSensorMap> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorControl> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::SignalSource> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSignalSelector> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorMixer> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorMatrix> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorMatrixSignal> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSignalSplitter> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSignalCombiner> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSignalDemultiplexer> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSignalMultiplexer> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorSignalTranscoder> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorTiming> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorPtpInstance> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorPtpPort> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::atdecc::aem {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc::aem
