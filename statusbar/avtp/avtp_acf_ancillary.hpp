#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_ANCILLARY - Ancillary Data (ANC) message - IEEE 1722-2025 Clause 9.4.13
/// Carries one SMPTE ST 291-1 ancillary data packet: the video line it rides
/// on, its DID and SDID/DBN, first/last-packet flags for the stream it belongs
/// to, and the user data words either as 8-bit octets (ANC_8BIT, parity and
/// checksum stripped) or as 10-bit words packed three per quadlet (ANC_10BIT,
/// Figure 83, checksum word included). A packet is never split across messages
/// (9.4.13.3.8.1).
///
/// Wire format (8-byte fixed part + payload quadlets), Figure 82:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | reserved[13:4] | mode[3:2] | fp[1] | lp[0]
///   Bytes 4-5:   line_number
///   Byte 6:      DID
///   Byte 7:      SDID/DBN
///   Bytes 8+:   payload, zero-padded to the quadlet (pad octets)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::quadlet_t;

/// Ancillary Data (ANC) message fixed part - IEEE 1722-2025 Clause 9.4.13, Figure 82
struct AcfAncillaryMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 8;
    /// Payload bounds in quadlets (9.4.13)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | reserved[13:4] | mode[3:2] | fp[1] | lp[0]
    doublet_t pad_mode_flags;

    // Bytes 4-5: line_number
    doublet_t line_number;

    // Byte 6: DID
    octet_t did;

    // Byte 7: SDID/DBN
    octet_t sdid_dbn;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mode_flags.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mode_flags.set_bits(0xC000U, 14, value); }

    /// Get mode: payload word format (Table 26: AcfAncMode::anc_8bit / anc_10bit)
    [[nodiscard]] constexpr auto mode() const noexcept -> uint8_t { return pad_mode_flags.get_bits<uint8_t>(0x000CU, 2); }

    /// Set mode
    constexpr void set_mode(uint8_t const value) noexcept { pad_mode_flags.set_bits(0x000CU, 2, value); }

    /// Get fp: first packet of the ancillary data stream
    [[nodiscard]] constexpr auto fp() const noexcept -> bool { return pad_mode_flags.has_flag(0x0002U); }

    /// Set fp
    constexpr void set_fp(bool const value) noexcept { pad_mode_flags.set_flag(0x0002U, value); }

    /// Get lp: last packet of the ancillary data stream
    [[nodiscard]] constexpr auto lp() const noexcept -> bool { return pad_mode_flags.has_flag(0x0001U); }

    /// Set lp
    constexpr void set_lp(bool const value) noexcept { pad_mode_flags.set_flag(0x0001U, value); }

    /// Get line_number: video line carrying the packet (progressive numbering)
    [[nodiscard]] constexpr auto get_line_number() const noexcept -> uint16_t { return line_number.get(); }

    /// Set line_number
    constexpr void set_line_number(uint16_t const value) noexcept { line_number = value; }

    /// Get did: the packet's DID
    [[nodiscard]] constexpr auto get_did() const noexcept -> uint8_t { return did.get(); }

    /// Set did
    constexpr void set_did(uint8_t const value) noexcept { did = value; }

    /// Get sdid_dbn: the packet's SDID (type 2) or DBN (type 1)
    [[nodiscard]] constexpr auto get_sdid_dbn() const noexcept -> uint8_t { return sdid_dbn.get(); }

    /// Set sdid_dbn
    constexpr void set_sdid_dbn(uint8_t const value) noexcept { sdid_dbn = value; }

    // Initialization

    /// Initialize as the smallest ACF_ANCILLARY message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::ancillary, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mode_flags = 0U;
        line_number = 0U;
        did = 0U;
        sdid_dbn = 0U;
    }

    // Validation

    /// Typed ACF_ANCILLARY with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::ancillary) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfAncillaryMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfAncillaryMessage) == AcfAncillaryMessage::LENGTH, "AcfAncillaryMessage must be exactly 8 bytes");
static_assert(alignof(AcfAncillaryMessage) <= 4, "AcfAncillaryMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfAncillaryMessage, pad_mode_flags) == 2, "pad_mode_flags must be at offset 2");
static_assert(offsetof(AcfAncillaryMessage, line_number) == 4, "line_number must be at offset 4");
static_assert(offsetof(AcfAncillaryMessage, did) == 6, "did must be at offset 6");
static_assert(offsetof(AcfAncillaryMessage, sdid_dbn) == 7, "sdid_dbn must be at offset 7");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfAncillaryMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_ANCILLARY message: fixed part plus payload (pad removed)
using AcfAncillaryMessageView = AcfTypedMessageView<AcfAncillaryMessage>;

/// Parse an ACF_ANCILLARY message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_ancillary_parse(std::span<uint8_t const> const data) noexcept
    -> std::optional<AcfAncillaryMessageView>
{
    return acf_parse_typed<AcfAncillaryMessage>(data);
}

/// Build an ACF_ANCILLARY message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_ancillary_build(
    std::span<uint8_t> const out, AcfAncillaryMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfAncillaryMessage>(out, fixed, payload);
}

/// mode values - IEEE 1722-2025 Table 26
namespace AcfAncMode {

constexpr uint8_t anc_8bit = 0x0U;   ///< 8-bit user data words, one per octet
constexpr uint8_t anc_10bit = 0x1U;  ///< 10-bit words, three per quadlet (Figure 83)
// 0x2 - 0x3: reserved

}  // namespace AcfAncMode

/// Words per quadlet in ANC_10BIT mode
constexpr size_t ACF_ANC_10BIT_WORDS_PER_QUADLET = 3;

/// Pack 10-bit words three per quadlet (bits 31:22, 21:12, 11:2; the low two
/// bits zero, Figure 83) into @p out. Returns the octets written (a quadlet
/// multiple), or 0 when @p out is too small or a word exceeds 10 bits.
[[nodiscard]] inline auto acf_anc_pack_10bit(std::span<uint16_t const> const words, std::span<uint8_t> const out) noexcept -> size_t
{
    size_t const quadlets = (words.size() + (ACF_ANC_10BIT_WORDS_PER_QUADLET - 1U)) / ACF_ANC_10BIT_WORDS_PER_QUADLET;
    if (out.size() < quadlets * ACF_QUADLET_OCTETS) {
        return 0;
    }
    for (size_t q = 0; q < quadlets; ++q) {
        uint32_t packed = 0;
        for (size_t k = 0; k < ACF_ANC_10BIT_WORDS_PER_QUADLET; ++k) {
            size_t const i = (q * ACF_ANC_10BIT_WORDS_PER_QUADLET) + k;
            uint32_t const word = i < words.size() ? words[i] : 0U;
            if (word > 0x3FFU) {
                return 0;
            }
            packed |= word << (22U - (10U * k));
        }
        quadlet_t const value{packed};
        span_copy(out.subspan(q * ACF_QUADLET_OCTETS, ACF_QUADLET_OCTETS), make_const_span(value));
    }
    return quadlets * ACF_QUADLET_OCTETS;
}

/// Unpack 10-bit words (three per quadlet) from @p payload into @p out. Returns
/// the words written - three per whole quadlet, a trailing partial quadlet is
/// ignored - or 0 when @p out is too small. The caller knows from the packet's
/// data count how many of the last quadlet's words are real.
[[nodiscard]] inline auto acf_anc_unpack_10bit(std::span<uint8_t const> const payload, std::span<uint16_t> const out) noexcept
    -> size_t
{
    size_t const quadlets = payload.size() / ACF_QUADLET_OCTETS;
    if (out.size() < quadlets * ACF_ANC_10BIT_WORDS_PER_QUADLET) {
        return 0;
    }
    for (size_t q = 0; q < quadlets; ++q) {
        quadlet_t value{};
        span_load(value, payload.subspan(q * ACF_QUADLET_OCTETS, ACF_QUADLET_OCTETS));
        for (size_t k = 0; k < ACF_ANC_10BIT_WORDS_PER_QUADLET; ++k) {
            out[(q * ACF_ANC_10BIT_WORDS_PER_QUADLET) + k] = static_cast<uint16_t>((value.get() >> (22U - (10U * k))) & 0x3FFU);
        }
    }
    return quadlets * ACF_ANC_10BIT_WORDS_PER_QUADLET;
}

}  // namespace statusbar::avtp
