#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF - AVTP Control Format common message structure - IEEE 1722-2025 Clause 9.4.1
/// The acf_payload_data of an NTSCF (9.2) or TSCF (9.3) AVTPDU is a
/// concatenation of ACF messages, each a whole number of quadlets:
///
///   Bytes 0-1:  acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2+:   message-specific fields, then the message payload,
///               zero-padded to the quadlet boundary
///
/// acf_msg_length counts quadlets and includes the quadlet holding the
/// header, so it is at least 1 (9.4.1.3). Message types are Table 23; the
/// per-type layouts live in avtp_acf_<name>.hpp. A listener walks the
/// payload by length and skips types it does not understand; the
/// ACF_CHECKSUM (9.4.20) and ACF_CRC (9.4.21) messages validate the
/// message immediately before them and are only acted on when the walker
/// is asked to (AcfMessageWalker::next_verified).

#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;

//
// ACF message types - IEEE 1722-2025 Table 23
//

/// acf_msg_type values
namespace AcfMsgType {

constexpr uint8_t flexray = 0x00U;         ///< FlexRay message (9.4.2)
constexpr uint8_t can = 0x01U;             ///< CAN / CAN FD message (9.4.3)
constexpr uint8_t can_brief = 0x02U;       ///< Abbreviated CAN / CAN FD message (9.4.4)
constexpr uint8_t lin = 0x03U;             ///< LIN message (9.4.5)
constexpr uint8_t most = 0x04U;            ///< MOST message (9.4.6)
constexpr uint8_t gpc = 0x05U;             ///< General purpose control message (9.4.7)
constexpr uint8_t serial = 0x06U;          ///< Serial port message (9.4.8)
constexpr uint8_t parallel = 0x07U;        ///< Parallel port message (9.4.9)
constexpr uint8_t sensor = 0x08U;          ///< Analog sensor/actuator message (9.4.10)
constexpr uint8_t sensor_brief = 0x09U;    ///< Abbreviated sensor/actuator message (9.4.11)
constexpr uint8_t aecp = 0x0AU;            ///< IEEE Std 1722.1 AECP message (9.4.12)
constexpr uint8_t ancillary = 0x0BU;       ///< Video ancillary data message (9.4.13)
constexpr uint8_t gisf = 0x0CU;            ///< Generic Image Sensor Format message (Clause 18)
constexpr uint8_t byte_bus = 0x0DU;        ///< Generic byte bus data message (9.4.14)
constexpr uint8_t byte_bus_brief = 0x0EU;  ///< Abbreviated byte bus data message (9.4.15)
constexpr uint8_t i2c = 0x0FU;             ///< Inter-IC (I2C) message (9.4.16)
constexpr uint8_t i2c_brief = 0x10U;       ///< Abbreviated I2C message (9.4.17)
constexpr uint8_t can_xl = 0x11U;          ///< CAN XL message (9.4.18)
constexpr uint8_t can_xl_brief = 0x12U;    ///< Abbreviated CAN XL message (9.4.19)
// 0x13 - 0x20: reserved
constexpr uint8_t can_v2 = 0x21U;        ///< CAN / CAN FD message version 2 (9.4.3)
constexpr uint8_t can_brief_v2 = 0x22U;  ///< Abbreviated CAN / CAN FD message version 2 (9.4.4)
constexpr uint8_t lin_v2 = 0x23U;        ///< LIN message version 2 (9.4.5)
// 0x24 - 0x75: reserved
constexpr uint8_t checksum = 0x76U;    ///< 16-bit checksum of the preceding message (9.4.20)
constexpr uint8_t crc = 0x77U;         ///< 32-bit CRC of the preceding message (9.4.21)
constexpr uint8_t user_first = 0x78U;  ///< ACF_USER range start (user-defined)
constexpr uint8_t user_last = 0x7FU;   ///< ACF_USER range end

}  // namespace AcfMsgType

/// Name of an acf_msg_type value ("ACF_CAN", "ACF_USER", "ACF_RESERVED", ...)
[[nodiscard]] auto acf_msg_type_name(uint8_t msg_type) noexcept -> std::string_view;

//
// Quadlet arithmetic - IEEE 1722-2025 9.4.1.3 and 9.4.1.7 (Table 24)
//

/// Octets per quadlet
constexpr size_t ACF_QUADLET_OCTETS = 4;

/// Largest acf_msg_length (9-bit field), in quadlets
constexpr uint16_t ACF_MSG_LENGTH_MAX_QUADLETS = 0x1FFU;

/// Quadlets needed to carry @p octets (rounded up)
[[nodiscard]] constexpr auto acf_quadlets_for_octets(size_t const octets) noexcept -> size_t
{
    return (octets + (ACF_QUADLET_OCTETS - 1)) / ACF_QUADLET_OCTETS;
}

/// The pad field value for a payload of @p octets: zero octets appended to
/// reach the quadlet boundary (0 to 3)
[[nodiscard]] constexpr auto acf_pad_for_octets(size_t const octets) noexcept -> uint8_t
{
    return static_cast<uint8_t>((ACF_QUADLET_OCTETS - (octets % ACF_QUADLET_OCTETS)) % ACF_QUADLET_OCTETS);
}

//
// AcfMessageHeader - IEEE 1722-2025 Clause 9.4.1, Figure 62
//

/// The two octets every ACF message starts with
struct AcfMessageHeader
{
    /// Length of the common header on the wire
    static constexpr size_t LENGTH = 2;

    // Bytes 0-1: acf_msg_type[15:9] | acf_msg_length[8:0]
    doublet_t type_length;

    /// Get acf_msg_type (7 bits)
    [[nodiscard]] constexpr auto msg_type() const noexcept -> uint8_t { return type_length.get_bits<uint8_t>(0xFE00U, 9); }

    /// Set acf_msg_type (7 bits)
    constexpr void set_msg_type(uint8_t const type) noexcept { type_length.set_bits(0xFE00U, 9, type); }

    /// Get acf_msg_length in quadlets (9 bits, includes this header's quadlet)
    [[nodiscard]] constexpr auto msg_length() const noexcept -> uint16_t { return type_length.get_bits(0x01FFU, 0); }

    /// Set acf_msg_length in quadlets (9 bits)
    constexpr void set_msg_length(uint16_t const quadlets) noexcept { type_length.set_bits(0x01FFU, 0, quadlets); }

    /// The whole message's length in octets (acf_msg_length x 4)
    [[nodiscard]] constexpr auto msg_length_octets() const noexcept -> size_t
    {
        return static_cast<size_t>(msg_length()) * ACF_QUADLET_OCTETS;
    }

    /// Initialize with a type and a length in quadlets
    constexpr void init(uint8_t const type, uint16_t const quadlets) noexcept
    {
        type_length = 0U;
        set_msg_type(type);
        set_msg_length(quadlets);
    }

    /// A header is valid when its length is at least one quadlet (9.4.1.3)
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return msg_length() >= 1U; }

    auto operator<=>(AcfMessageHeader const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(AcfMessageHeader) == AcfMessageHeader::LENGTH, "AcfMessageHeader must be exactly 2 bytes");
static_assert(alignof(AcfMessageHeader) <= 2, "AcfMessageHeader alignment must not exceed 2 bytes");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfMessageHeader> : std::true_type
{};

namespace statusbar::avtp {

/// One ACF message located in an acf_payload_data buffer
struct AcfMessageView
{
    AcfMessageHeader header{};         ///< the parsed common header
    std::span<uint8_t const> message;  ///< the whole message, header quadlet through the last payload quadlet

    /// acf_msg_type of this message
    [[nodiscard]] constexpr auto msg_type() const noexcept -> uint8_t { return header.msg_type(); }

    /// Everything after the common header: the message-specific fields and
    /// the (padded) payload
    [[nodiscard]] constexpr auto after_header() const noexcept -> std::span<uint8_t const>
    {
        return message.subspan(AcfMessageHeader::LENGTH);
    }
};

/// Parse the ACF message at the start of @p data: the header must be valid
/// and the declared length must fit in @p data. The view's message span is
/// exactly acf_msg_length quadlets; any following messages are not part of
/// it.
[[nodiscard]] auto acf_parse_message(std::span<uint8_t const> data) noexcept -> std::optional<AcfMessageView>;

/// Store a common header at the start of @p out (at least 2 octets). The
/// caller fills the message-specific fields and payload after it and
/// zero-pads to the quadlet boundary.
[[nodiscard]] auto acf_store_header(std::span<uint8_t> out, uint8_t msg_type, uint16_t msg_length_quadlets) noexcept -> bool;

//
// AcfMessageWalker - iterate the messages of an acf_payload_data
//

/// Outcome of the opt-in trailer check (9.4.20 / 9.4.21)
enum class AcfTrailerStatus : uint8_t
{
    none,               ///< no ACF_CHECKSUM / ACF_CRC message followed
    checksum_ok,        ///< an ACF_CHECKSUM followed and validated
    checksum_bad,       ///< an ACF_CHECKSUM followed and did NOT validate - do not act on the message
    crc_ok,             ///< an ACF_CRC followed and validated
    crc_bad,            ///< an ACF_CRC followed and did NOT validate - do not act on the message
    crc_unsupported,    ///< an ACF_CRC followed with a crc_type this implementation cannot compute
    trailer_malformed,  ///< an ACF_CHECKSUM / ACF_CRC followed but its own layout is wrong
};

/// Name of an AcfTrailerStatus value
[[nodiscard]] auto acf_trailer_status_name(AcfTrailerStatus status) noexcept -> std::string_view;

/// A message with the result of its trailer check
struct AcfVerifiedMessage
{
    AcfMessageView message;
    AcfTrailerStatus trailer = AcfTrailerStatus::none;

    /// True when no trailer followed or the trailer validated
    [[nodiscard]] constexpr auto ok() const noexcept -> bool
    {
        return trailer == AcfTrailerStatus::none || trailer == AcfTrailerStatus::checksum_ok || trailer == AcfTrailerStatus::crc_ok;
    }
};

/// Walks the concatenated ACF messages of one acf_payload_data (the span
/// returned by tscf_get_acf_payload / ntscf_get_acf_payload). next() yields
/// every message, trailers included, which is how a listener that ignores
/// Checksum/CRC messages behaves; next_verified() consumes a following
/// ACF_CHECKSUM / ACF_CRC as the message's trailer and validates it. A
/// length of zero or a length past the end of the buffer stops the walk
/// with malformed() set.
class AcfMessageWalker
{
  public:
    explicit AcfMessageWalker(std::span<uint8_t const> const acf_payload_data) noexcept
        : remaining_(acf_payload_data)
    {}

    /// The next message, or nullopt at the end of the payload or on a
    /// malformed length
    [[nodiscard]] auto next() noexcept -> std::optional<AcfMessageView>;

    /// The next message with its trailer check. When the message after it
    /// is ACF_CHECKSUM or ACF_CRC, both are consumed: on *_ok the message is
    /// returned as validated; on *_bad, crc_unsupported or trailer_malformed
    /// it is returned flagged, and per 9.4.20 / 9.4.21 a listener shall not
    /// act on it. A trailer is never returned as a message of its own here.
    [[nodiscard]] auto next_verified() noexcept -> std::optional<AcfVerifiedMessage>;

    /// The not-yet-walked tail of the payload
    [[nodiscard]] auto remaining() const noexcept -> std::span<uint8_t const> { return remaining_; }

    /// True once a message with an impossible length was met
    [[nodiscard]] auto malformed() const noexcept -> bool { return malformed_; }

    /// True when nothing is left to walk (end reached or malformed)
    [[nodiscard]] auto done() const noexcept -> bool { return remaining_.empty() || malformed_; }

  private:
    std::span<uint8_t const> remaining_;
    bool malformed_ = false;
};

}  // namespace statusbar::avtp
