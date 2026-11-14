#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan Vendor Unique (MVU) AECP messages - Milan v1.3 5.4.3 and 5.4.4.
///
/// Milan rides the AECP vendor-unique extensibility of IEEE 1722.1-2021 9.6:
/// a VENDOR_UNIQUE command or response whose protocol_id is Avnu's MVU id,
/// followed by a 16-bit u/command_type word and the command's own data. The
/// one every PAAD-AE must answer is GET_MILAN_INFO (5.4.4.1), which is how a
/// Milan controller tells a Milan device from any other ATDECC entity.
///
/// The stream input commands are here as wire formats only: BIND_STREAM
/// and UNBIND_STREAM (5.4.4.6, 5.4.4.7), which bind a sink as ACMP's
/// BIND_RX and UNBIND_RX commands do, and GET_STREAM_INPUT_INFO_EX
/// (5.4.4.8). What they mean to a sink is in atdecc_milan_mvu_stream_input.hpp.

#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace statusbar::atdecc {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::octet_t;
using ieee::quadlet_t;

//
// MVU protocol_id - 5.4.3.2.1: Avnu's OUI-36 00-1B-C5-0A-C with protocol 0x100
//
inline constexpr Eui48 MVU_PROTOCOL_ID{0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00};

//
// MVU command types - Table 5.15
//
constexpr uint16_t MVU_COMMAND_GET_MILAN_INFO = 0x0000;
constexpr uint16_t MVU_COMMAND_SET_SYSTEM_UNIQUE_ID = 0x0001;
constexpr uint16_t MVU_COMMAND_GET_SYSTEM_UNIQUE_ID = 0x0002;
constexpr uint16_t MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO = 0x0003;
constexpr uint16_t MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO = 0x0004;
constexpr uint16_t MVU_COMMAND_BIND_STREAM = 0x0005;
constexpr uint16_t MVU_COMMAND_UNBIND_STREAM = 0x0006;
constexpr uint16_t MVU_COMMAND_GET_STREAM_INPUT_INFO_EX = 0x0007;

//
// MVU status codes - Table 5.16
//
constexpr uint8_t MVU_STATUS_SUCCESS = 0;
constexpr uint8_t MVU_STATUS_NOT_IMPLEMENTED = 1;
constexpr uint8_t MVU_STATUS_NO_SUCH_DESCRIPTOR = 2;
constexpr uint8_t MVU_STATUS_ENTITY_LOCKED = 3;
constexpr uint8_t MVU_STATUS_BAD_ARGUMENTS = 7;
constexpr uint8_t MVU_STATUS_ENTITY_MISBEHAVING = 10;
constexpr uint8_t MVU_STATUS_PAYLOAD_TOO_SHORT = 13;

/// 5.4.3.4: every MVU command times out after 250 ms; an entity answers within 240.
constexpr uint32_t MVU_TIMEOUT_MS = 250;

//
// GET_MILAN_INFO - 5.4.4.1
//

/// The one Milan protocol version there is.
constexpr uint32_t MVU_PROTOCOL_VERSION = 1;

/// features_flags - Table 5.17
constexpr uint32_t MVU_FEATURE_REDUNDANCY = 0x00000001;
constexpr uint32_t MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING = 0x00000002;
constexpr uint32_t MVU_FEATURE_MVU_BINDING = 0x00000004;
constexpr uint32_t MVU_FEATURE_TALKER_SIGNAL_PRESENCE = 0x00000008;

/// The name of an MVU command type, "Unknown" for one Table 5.15 lacks.
[[nodiscard]] auto mvu_command_name(uint16_t command_type) noexcept -> std::string_view;

/// The name of an MVU status code, "Unknown" for one Table 5.16 lacks.
[[nodiscard]] auto mvu_status_name(uint8_t status) noexcept -> std::string_view;

/// The name of a Probing status (Table 5.5), "Unknown" for a reserved value.
[[nodiscard]] auto mvu_probing_status_name(uint8_t pbsta) noexcept -> std::string_view;

/// A certification_version or specification_version word: four 8-bit numbers,
/// most significant first, read as dot-separated decimals (5.4.4.1).
[[nodiscard]] constexpr auto mvu_version(uint8_t const a, uint8_t const b, uint8_t const c = 0, uint8_t const d = 0) noexcept
    -> uint32_t
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) | d;
}

/// The word for a dotted version text: "1.3" or "1.3.0.0", one to four
/// decimal numbers of at most 255 each. nullopt for anything else.
[[nodiscard]] auto mvu_version_from_dotted(std::string_view text) noexcept -> std::optional<uint32_t>;

/// The dotted text of a version word, as the specification prints it.
[[nodiscard]] auto mvu_version_to_dotted(uint32_t version) -> std::string;

//
// MVU AECPDU header - Figure 5.4: AECP common (22) + protocol_id (6) + u/command_type (2)
//
struct AecpMvuDu
{
    /// Octets on the wire up to and including command_type.
    static constexpr size_t LENGTH = 30;
    /// What control_data_length counts of this header: everything after the
    /// 12-octet common control header.
    static constexpr size_t MVU_DATA_LENGTH = LENGTH - AecpDuCommon::HEADER_LENGTH;

    /// The vendor-unique AECP common header.
    AecpDuCommon common{};
    /// Bytes 22-27: the protocol, MVU_PROTOCOL_ID for a Milan message.
    Eui48 protocol_id{};
    /// Bytes 28-29: u[15] | command_type[14:0]
    doublet_t u_command_type{0};

    /// The command type, Table 5.15.
    [[nodiscard]] constexpr auto command_type() const noexcept -> uint16_t { return u_command_type.get_bits<uint16_t>(0x7FFF); }

    /// The u bit, 5.4.3.2.2: set on a response that is a notification, clear
    /// on a command and on the direct response to it.
    [[nodiscard]] constexpr auto is_unsolicited() const noexcept -> bool { return u_command_type.has_flag(uint16_t{0x8000}); }

    constexpr void set_command_type(uint16_t const command, bool const unsolicited = false) noexcept
    {
        u_command_type = static_cast<uint16_t>((command & 0x7FFF) | (unsolicited ? 0x8000 : 0));
    }

    /// Whether this is a Milan message at all: a VENDOR_UNIQUE command or
    /// response carrying the MVU protocol_id.
    [[nodiscard]] constexpr auto is_mvu() const noexcept -> bool
    {
        return common.is_vendor_unique() && protocol_id == MVU_PROTOCOL_ID;
    }

    [[nodiscard]] constexpr auto is_command() const noexcept -> bool
    {
        return common.message_type() == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND;
    }

    [[nodiscard]] constexpr auto is_response() const noexcept -> bool
    {
        return common.message_type() == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE;
    }

    /// A well-formed MVU header: a valid AECP common header, the MVU
    /// protocol, and a control_data_length that covers the command_type.
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        return common.is_valid() && is_mvu() && common.control_data_length() >= MVU_DATA_LENGTH;
    }

    /// Set up a command of @p command whose command-specific data is @p payload_length octets.
    constexpr void init_command(uint16_t const command, uint16_t const payload_length) noexcept
    {
        common.init_command(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND, static_cast<uint16_t>(MVU_DATA_LENGTH + payload_length));
        protocol_id = MVU_PROTOCOL_ID;
        set_command_type(command);
    }

    /// Set up a response of @p command and @p status whose command-specific
    /// data is @p payload_length octets; @p unsolicited sets the u bit.
    constexpr void init_response(
        uint16_t const command, uint8_t const status, uint16_t const payload_length, bool const unsolicited = false) noexcept
    {
        common.init_response(
            AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE, status, static_cast<uint16_t>(MVU_DATA_LENGTH + payload_length));
        protocol_id = MVU_PROTOCOL_ID;
        set_command_type(command, unsolicited);
    }

    auto operator<=>(AecpMvuDu const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(AecpMvuDu) == 30, "AecpMvuDu must be exactly 30 bytes");
static_assert(alignof(AecpMvuDu) <= 4, "AecpMvuDu alignment must not exceed 4 bytes");
static_assert(offsetof(AecpMvuDu, common) == 0, "common must be at offset 0");
static_assert(offsetof(AecpMvuDu, protocol_id) == 22, "protocol_id must be at offset 22");
static_assert(offsetof(AecpMvuDu, u_command_type) == 28, "u_command_type must be at offset 28");

//
// GET_MILAN_INFO command-specific data - Figures 5.5 and 5.6
//

/// The command carries two reserved octets after command_type.
constexpr size_t MVU_GET_MILAN_INFO_COMMAND_LENGTH = 2;

/// The response's command-specific data: what the PAAD-AE is, for its
/// currently active Configuration. None of it changes while that
/// Configuration is active (5.4.4.1, Note).
struct MvuGetMilanInfoResponse
{
    static constexpr size_t LENGTH = 18;

    /// Bytes 30-31: reserved, 0.
    doublet_t reserved{0};
    /// Bytes 32-35: MVU_PROTOCOL_VERSION.
    quadlet_t protocol_version{MVU_PROTOCOL_VERSION};
    /// Bytes 36-39: Table 5.17 flags.
    quadlet_t features_flags{0};
    /// Bytes 40-43: the Milan certification passed, mvu_version(); 0 for none.
    quadlet_t certification_version{0};
    /// Bytes 44-47: the Milan specification supported, mvu_version(major, minor).
    quadlet_t specification_version{0};

    auto operator<=>(MvuGetMilanInfoResponse const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuGetMilanInfoResponse) == 18, "MvuGetMilanInfoResponse must be exactly 18 bytes");
static_assert(offsetof(MvuGetMilanInfoResponse, protocol_version) == 2);
static_assert(offsetof(MvuGetMilanInfoResponse, features_flags) == 6);
static_assert(offsetof(MvuGetMilanInfoResponse, certification_version) == 10);
static_assert(offsetof(MvuGetMilanInfoResponse, specification_version) == 14);

//
// SET_MEDIA_CLOCK_REFERENCE_INFO and GET_MEDIA_CLOCK_REFERENCE_INFO - 5.4.4.4, 5.4.4.5
//

/// flags - Table 5.18: which of a SET's fields apply; a response sets the ones the entity supports.
constexpr uint8_t MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID = 0x01;
constexpr uint8_t MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID = 0x02;
constexpr uint8_t MVU_MCR_FLAGS_SUPPORTED =
    MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID | MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID;

/// Default Media Clock Reference Priorities by type of device - Table 7.3 (7.6.1.1);
/// MVU_MCR_PRIO_DEFAULT is the priority of a device that gives none.
constexpr uint8_t MVU_MCR_PRIO_MEDIA_CLOCK_GENERATOR = 240;
constexpr uint8_t MVU_MCR_PRIO_ROUTER = 224;  ///< matrix mixers, signal routers, switches with audio processing
constexpr uint8_t MVU_MCR_PRIO_MIXING_CONSOLE = 208;
constexpr uint8_t MVU_MCR_PRIO_AUDIO_INTERFACE = 192;  ///< stageboxes, audio interfaces
constexpr uint8_t MVU_MCR_PRIO_LOUDSPEAKER_PROCESSOR = 176;
constexpr uint8_t MVU_MCR_PRIO_AMPLIFIER = 160;  ///< amplifiers, powered loudspeakers, in-ear transmitters
constexpr uint8_t MVU_MCR_PRIO_RECORDER = 144;
constexpr uint8_t MVU_MCR_PRIO_DEFAULT = 128;
constexpr uint8_t MVU_MCR_PRIO_EFFECT_PROCESSOR = 112;
constexpr uint8_t MVU_MCR_PRIO_WIRELESS_RECEIVER = 80;
constexpr uint8_t MVU_MCR_PRIO_MICROPHONE = 64;
constexpr uint8_t MVU_MCR_PRIO_INSTRUMENT = 48;

/// The media_clock_domain_name a clock domain has until a controller names it (7.6.2).
inline constexpr std::string_view MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME = "DEFAULT";

/// The command-specific data of a GET_MEDIA_CLOCK_REFERENCE_INFO command.
struct MvuGetMediaClockReferenceInfoCommand
{
    static constexpr size_t LENGTH = 2;

    /// Bytes 30-31: the CLOCK_DOMAIN asked about.
    doublet_t clock_domain_index{0};

    auto operator<=>(MvuGetMediaClockReferenceInfoCommand const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuGetMediaClockReferenceInfoCommand) == 2, "MvuGetMediaClockReferenceInfoCommand must be exactly 2 bytes");

/// The command-specific data of a SET_MEDIA_CLOCK_REFERENCE_INFO command and its response,
/// and of a GET_MEDIA_CLOCK_REFERENCE_INFO response: one CLOCK_DOMAIN's settings (7.6).
struct MvuMediaClockReferenceInfoPayload
{
    static constexpr size_t LENGTH = 74;

    /// Bytes 30-31: the CLOCK_DOMAIN.
    doublet_t clock_domain_index{0};
    /// Byte 32: Table 5.18.
    octet_t flags{0};
    /// Byte 33: reserved, 0.
    octet_t reserved{0};
    /// Byte 34: the vendor's default priority; a command's is ignored.
    octet_t default_mcr_prio{0};
    /// Byte 35: the user's priority, default_mcr_prio until a controller sets it.
    octet_t user_mcr_prio{0};
    /// Bytes 36-39: reserved, 0.
    quadlet_t reserved2{0};
    /// Bytes 40-103: the media clock domain name, UTF-8.
    aem::AtdeccString media_clock_domain_name{};

    auto operator<=>(MvuMediaClockReferenceInfoPayload const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuMediaClockReferenceInfoPayload) == 74, "MvuMediaClockReferenceInfoPayload must be exactly 74 bytes");
static_assert(offsetof(MvuMediaClockReferenceInfoPayload, flags) == 2);
static_assert(offsetof(MvuMediaClockReferenceInfoPayload, default_mcr_prio) == 4);
static_assert(offsetof(MvuMediaClockReferenceInfoPayload, user_mcr_prio) == 5);
static_assert(offsetof(MvuMediaClockReferenceInfoPayload, media_clock_domain_name) == 10);

//
// SET_SYSTEM_UNIQUE_ID / GET_SYSTEM_UNIQUE_ID - 5.4.4.2, 5.4.4.3, Figure 5.7
//

/// The GET_SYSTEM_UNIQUE_ID command carries two reserved octets after command_type.
constexpr size_t MVU_GET_SYSTEM_UNIQUE_ID_COMMAND_LENGTH = 2;

/// The command-specific data of a SET_SYSTEM_UNIQUE_ID command and its response, and of a
/// GET_SYSTEM_UNIQUE_ID response: the system the entity belongs to, by number and name.
struct MvuSystemUniqueIdPayload
{
    static constexpr size_t LENGTH = 74;

    /// Bytes 30-31: reserved, 0.
    doublet_t reserved{0};
    /// Bytes 32-39: the system unique id, 0 until a controller sets one.
    ieee::octlet_t system_unique_id{0};
    /// Bytes 40-103: the system name, UTF-8.
    aem::AtdeccString system_name{};

    auto operator<=>(MvuSystemUniqueIdPayload const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuSystemUniqueIdPayload) == 74, "MvuSystemUniqueIdPayload must be exactly 74 bytes");
static_assert(offsetof(MvuSystemUniqueIdPayload, system_unique_id) == 2);
static_assert(offsetof(MvuSystemUniqueIdPayload, system_name) == 10);

//
// BIND_STREAM - 5.4.4.6, Figure 5.10
//

/// flags - Table 5.19: set, the STREAM_INPUT is bound in the stopped state.
constexpr uint16_t MVU_BIND_STREAM_FLAG_STREAMING_WAIT = 0x0001;

/// The command-specific data of a BIND_STREAM command, and of its response,
/// which gives every field back as the command had it.
struct MvuBindStream
{
    static constexpr size_t LENGTH = 18;

    /// Bytes 30-31: Table 5.19.
    doublet_t flags{0};
    /// Bytes 32-33: always STREAM_INPUT.
    doublet_t descriptor_type{0};
    /// Bytes 34-35: the STREAM_INPUT to bind, ACMP's listener_unique_id (Table 5.22).
    doublet_t descriptor_index{0};
    /// Bytes 36-43: the talker to bind it to.
    Eui64 talker_entity_id{};
    /// Bytes 44-45: the talker's STREAM_OUTPUT, ACMP's talker_unique_id (Table 5.22).
    doublet_t talker_stream_index{0};
    /// Bytes 46-47: reserved, 0.
    doublet_t reserved{0};

    [[nodiscard]] constexpr auto streaming_wait() const noexcept -> bool
    {
        return flags.has_flag(MVU_BIND_STREAM_FLAG_STREAMING_WAIT);
    }

    auto operator<=>(MvuBindStream const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuBindStream) == 18, "MvuBindStream must be exactly 18 bytes");
static_assert(offsetof(MvuBindStream, descriptor_type) == 2);
static_assert(offsetof(MvuBindStream, descriptor_index) == 4);
static_assert(offsetof(MvuBindStream, talker_entity_id) == 6);
static_assert(offsetof(MvuBindStream, talker_stream_index) == 14);
static_assert(offsetof(MvuBindStream, reserved) == 16);

//
// UNBIND_STREAM - 5.4.4.7, and the GET_STREAM_INPUT_INFO_EX command - 5.4.4.8: Figure 5.11
//

/// The command-specific data that names one STREAM_INPUT and nothing more:
/// an UNBIND_STREAM command, its response, and a GET_STREAM_INPUT_INFO_EX
/// command.
struct MvuStreamInput
{
    static constexpr size_t LENGTH = 6;

    /// Bytes 30-31: reserved, 0; a receiver ignores it.
    doublet_t reserved{0};
    /// Bytes 32-33: always STREAM_INPUT.
    doublet_t descriptor_type{0};
    /// Bytes 34-35: the STREAM_INPUT.
    doublet_t descriptor_index{0};

    auto operator<=>(MvuStreamInput const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuStreamInput) == 6, "MvuStreamInput must be exactly 6 bytes");
static_assert(offsetof(MvuStreamInput, descriptor_type) == 2);
static_assert(offsetof(MvuStreamInput, descriptor_index) == 4);

//
// GET_STREAM_INPUT_INFO_EX response - 5.4.4.8, Figure 5.12
//

/// What a STREAM_INPUT is bound to and how far its probing has come
/// (5.3.8.6). An unbound input answers a zero talker and PROBING_DISABLED.
struct MvuGetStreamInputInfoExResponse
{
    static constexpr size_t LENGTH = 18;

    /// Bytes 30-31: reserved, 0.
    doublet_t reserved{0};
    /// Bytes 32-33: as in the command.
    doublet_t descriptor_type{0};
    /// Bytes 34-35: as in the command.
    doublet_t descriptor_index{0};
    /// Bytes 36-43: the talker the input is bound to.
    Eui64 talker_entity_id{};
    /// Bytes 44-45: the talker's STREAM_OUTPUT.
    doublet_t talker_unique_id{0};
    /// Byte 46: pbsta[7:5] | acmpsta[4:0]
    octet_t pbsta_acmpsta{0};
    /// Byte 47: reserved, 0.
    octet_t reserved_end{0};

    /// The Probing status, Table 5.5.
    [[nodiscard]] constexpr auto pbsta() const noexcept -> uint8_t { return static_cast<uint8_t>(pbsta_acmpsta.get() >> 5U); }

    /// The ACMP status of the last probe (IEEE 1722.1-2021 Table 8-3),
    /// defined while the Probing status is PROBING_ACTIVE and 0 otherwise.
    [[nodiscard]] constexpr auto acmpsta() const noexcept -> uint8_t { return static_cast<uint8_t>(pbsta_acmpsta.get() & 0x1FU); }

    constexpr void set_status(uint8_t const pbsta, uint8_t const acmpsta) noexcept
    {
        pbsta_acmpsta = static_cast<uint8_t>(((pbsta & 0x07U) << 5U) | (acmpsta & 0x1FU));
    }

    auto operator<=>(MvuGetStreamInputInfoExResponse const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(MvuGetStreamInputInfoExResponse) == 18, "MvuGetStreamInputInfoExResponse must be exactly 18 bytes");
static_assert(offsetof(MvuGetStreamInputInfoExResponse, descriptor_type) == 2);
static_assert(offsetof(MvuGetStreamInputInfoExResponse, descriptor_index) == 4);
static_assert(offsetof(MvuGetStreamInputInfoExResponse, talker_entity_id) == 6);
static_assert(offsetof(MvuGetStreamInputInfoExResponse, talker_unique_id) == 14);
static_assert(offsetof(MvuGetStreamInputInfoExResponse, pbsta_acmpsta) == 16);
static_assert(offsetof(MvuGetStreamInputInfoExResponse, reserved_end) == 17);

}  // namespace statusbar::atdecc

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AecpMvuDu> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuGetMilanInfoResponse> : std::true_type
{};

//
// SYSTEM_UNIQUE_ID, MEDIA_CLOCK_REFERENCE_INFO and the binding commands - 5.4.4.2 to 5.4.4.8
//

namespace statusbar::atdecc {

/// BIND_STREAM flags (Milan v1.3 Table 5.19): set, the STREAM_INPUT is bound stopped.
constexpr uint16_t MVU_BIND_FLAGS_STREAMING_WAIT = 0x0001;

/// SET_MEDIA_CLOCK_REFERENCE_INFO flags (Milan v1.3 Table 5.18), MSB-numbered as above
constexpr uint8_t MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID = 0x01;
constexpr uint8_t MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID = 0x02;
/// Every SET_MEDIA_CLOCK_REFERENCE_INFO flag: a response carries the bits the
/// entity supports (Section 5.4.4.4), and it supports both.
constexpr uint8_t MVU_MCR_FLAGS_SUPPORTED =
    MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID | MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID;

/// Default Media Clock Reference Priorities by type of device (Table 7.2), a
/// selection: 192 is "stageboxes, network interfaces", 224 "matrix mixing
/// devices, signal routers", 128 the priority of a device that gives none.
constexpr uint8_t MVU_MCR_PRIO_ROUTER = 224;
constexpr uint8_t MVU_MCR_PRIO_NETWORK_INTERFACE = 192;
constexpr uint8_t MVU_MCR_PRIO_DEFAULT = 128;

/// The default media_clock_domain_name (Section 7.6.2)
inline constexpr std::string_view MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME = "DEFAULT";

/// The shortest AVTPDU in an MVU frame: a 64-octet Ethernet frame (the minimum,
/// FCS included) less the 14-octet header and the 4-octet FCS. Shorter MVU
/// PDUs are padded to this; the padding is not in control_data_length.
constexpr size_t MVU_MIN_PDU_LENGTH = 64 - 14 - 4;

/// True when @p pdu is an AECP VENDOR_UNIQUE command carrying the MVU
/// protocol_id, with room (received and declared) for protocol_id and
/// command_type. Says nothing about the target or the command_specific_data.
[[nodiscard]] inline auto is_mvu_command(std::span<uint8_t const> const pdu) noexcept -> bool
{
    if (pdu.size() < AecpMvuDu::LENGTH) {
        return false;
    }
    AecpMvuDu du{};
    span_load(du, pdu.first(AecpMvuDu::LENGTH));
    return du.common.subtype.get() == AvtpSubtype::aecp && du.is_command() && du.is_valid();
}

// ---------------------------------------------------------------------------
// Command payloads (command_specific_data, after AecpMvuDu) - Section 5.4.4
// ---------------------------------------------------------------------------

/// SET_SYSTEM_UNIQUE_ID command and response, GET_SYSTEM_UNIQUE_ID response (Milan v1.2
/// Figure 5.5): one quadlet, as the figure draws it (docs/MVU.md).
struct MvuSystemUniqueIdPayload
{
    static constexpr size_t LENGTH = 6;

    /// Bytes 30-31: reserved
    doublet_t reserved{0};

    /// Bytes 32-35: system_unique_id (default 0; SET to 0 is invalid)
    quadlet_t system_unique_id{0};

    auto operator<=>(MvuSystemUniqueIdPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuSystemUniqueIdPayload) == 6, "MvuSystemUniqueIdPayload must be 6 bytes");

/// SET_SYSTEM_UNIQUE_ID command and response, GET_SYSTEM_UNIQUE_ID response (Milan v1.3
/// Figure 5.7): a u64 number and a 64-octet name. A v1.2 entity accepts a number that fits its u32.
struct MvuSystemUniqueIdPayloadV13
{
    static constexpr size_t LENGTH = 74;

    /// Bytes 30-31: reserved
    doublet_t reserved{0};

    /// Bytes 32-39: number
    ieee::octlet_t number{0};

    /// Bytes 40-103: name (UTF-8, zero padded)
    std::array<char, 64> name{};

    auto operator<=>(MvuSystemUniqueIdPayloadV13 const&) const noexcept = default;
};

static_assert(sizeof(MvuSystemUniqueIdPayloadV13) == 74, "MvuSystemUniqueIdPayloadV13 must be 74 bytes");

/// GET_MEDIA_CLOCK_REFERENCE_INFO command (Figure 5.7)
struct MvuGetMediaClockReferenceInfoCommand
{
    static constexpr size_t LENGTH = 2;

    /// Bytes 30-31: CLOCK_DOMAIN descriptor index
    doublet_t clock_domain_index{0};

    auto operator<=>(MvuGetMediaClockReferenceInfoCommand const&) const noexcept = default;
};

static_assert(sizeof(MvuGetMediaClockReferenceInfoCommand) == 2, "MvuGetMediaClockReferenceInfoCommand must be 2 bytes");

/// SET_MEDIA_CLOCK_REFERENCE_INFO command and response,
/// GET_MEDIA_CLOCK_REFERENCE_INFO response (Figure 5.6)
struct MvuMediaClockReferenceInfoPayload
{
    static constexpr size_t LENGTH = 74;

    /// Bytes 30-31: CLOCK_DOMAIN descriptor index
    doublet_t clock_domain_index{0};

    /// Byte 32: flags (MVU_MCR_FLAGS_*)
    octet_t flags{0};

    /// Byte 33: reserved
    octet_t reserved1{0};

    /// Byte 34: default_mcr_prio (ignored in a command)
    octet_t default_mcr_prio{0};

    /// Byte 35: user_mcr_prio (default: default_mcr_prio)
    octet_t user_mcr_prio{0};

    /// Bytes 36-39: reserved
    quadlet_t reserved2{0};

    /// Bytes 40-103: media_clock_domain_name (UTF-8, zero padded, no trailing
    /// NUL when 64 bytes long; default "DEFAULT")
    std::array<char, 64> media_clock_domain_name{};

    auto operator<=>(MvuMediaClockReferenceInfoPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuMediaClockReferenceInfoPayload) == 74, "MvuMediaClockReferenceInfoPayload must be 74 bytes");

/// BIND_STREAM command and response (Milan v1.3 Figure 5.10)
struct MvuBindStreamPayload
{
    static constexpr size_t LENGTH = 18;

    /// Bytes 30-31: flags (MVU_BIND_FLAGS_*)
    doublet_t flags{0};

    /// Bytes 32-35: the STREAM_INPUT to bind (descriptor_index = ACMP listener_unique_id)
    doublet_t descriptor_type{0};
    doublet_t descriptor_index{0};

    /// Bytes 36-43: the talker
    ieee::Eui64 talker_entity_id{};

    /// Bytes 44-45: the talker's STREAM_OUTPUT (ACMP talker_unique_id)
    doublet_t talker_stream_index{0};

    /// Bytes 46-47: reserved
    doublet_t reserved{0};

    auto operator<=>(MvuBindStreamPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuBindStreamPayload) == 18, "MvuBindStreamPayload must be 18 bytes");
static_assert(offsetof(MvuBindStreamPayload, talker_entity_id) == 6);

/// UNBIND_STREAM command and response, GET_STREAM_INPUT_INFO_EX command (Milan v1.3 Figure 5.11)
struct MvuStreamInputPayload
{
    static constexpr size_t LENGTH = 6;

    /// Bytes 30-31: reserved
    doublet_t reserved{0};

    /// Bytes 32-35: the STREAM_INPUT
    doublet_t descriptor_type{0};
    doublet_t descriptor_index{0};

    auto operator<=>(MvuStreamInputPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuStreamInputPayload) == 6, "MvuStreamInputPayload must be 6 bytes");

/// GET_STREAM_INPUT_INFO_EX response (Milan v1.3 Figure 5.12)
struct MvuStreamInputInfoExPayload
{
    static constexpr size_t LENGTH = 18;

    /// Bytes 30-35: as MvuStreamInputPayload
    MvuStreamInputPayload input{};

    /// Bytes 36-43: the talker the input is bound to (0 unbound)
    ieee::Eui64 talker_entity_id{};

    /// Bytes 44-45: its STREAM_OUTPUT
    doublet_t talker_unique_id{0};

    /// Byte 46: pbsta[7:5] (Milan v1.3 Table 5.5), acmpsta[4:0] (Section 5.3.8.6)
    octet_t pbsta_acmpsta{0};

    /// Byte 47: reserved
    octet_t reserved{0};

    [[nodiscard]] constexpr auto probing_status() const noexcept -> uint8_t { return aem::probing_status_for(pbsta_acmpsta.get()); }
    [[nodiscard]] constexpr auto acmp_status() const noexcept -> uint8_t { return aem::acmp_status_for(pbsta_acmpsta.get()); }
    constexpr void set_probing(uint8_t const pbsta, uint8_t const acmpsta) noexcept
    {
        pbsta_acmpsta = aem::pack_probing(pbsta, acmpsta);
    }

    auto operator<=>(MvuStreamInputInfoExPayload const&) const noexcept = default;
};

static_assert(sizeof(MvuStreamInputInfoExPayload) == 18, "MvuStreamInputInfoExPayload must be 18 bytes");
static_assert(offsetof(MvuStreamInputInfoExPayload, pbsta_acmpsta) == 16);

}  // namespace statusbar::atdecc

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuSystemUniqueIdPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuSystemUniqueIdPayloadV13> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuGetMediaClockReferenceInfoCommand>
    : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuMediaClockReferenceInfoPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuBindStreamPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuStreamInputPayload> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::MvuStreamInputInfoExPayload> : std::true_type
{};
