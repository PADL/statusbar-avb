#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_SERIAL - Serial (SERIAL) message - IEEE 1722-2025 Clause 9.4.8
/// Carries data to or from an RS-232 serial interface together with the state
/// of its DCD/DTR/DSR/RTS/CTS/RI pins.
///
/// Wire format (4-byte fixed part + payload quadlets), Figure 74:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | reserved[13:6] | dcd[5] | dtr[4] | dsr[3] | rts[2] | cts[1] | ri[0]
///   Bytes 4+:   payload, zero-padded to the quadlet (pad octets)

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

/// Serial (SERIAL) message fixed part - IEEE 1722-2025 Clause 9.4.8, Figure 74
struct AcfSerialMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 4;
    /// Payload bounds in quadlets (9.4.8)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | reserved[13:6] | dcd[5] | dtr[4] | dsr[3] | rts[2] | cts[1] | ri[0]
    doublet_t pad_reserved_flags;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_reserved_flags.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_reserved_flags.set_bits(0xC000U, 14, value); }

    /// Get dcd: data carrier detect
    [[nodiscard]] constexpr auto dcd() const noexcept -> bool { return pad_reserved_flags.has_flag(0x0020U); }

    /// Set dcd
    constexpr void set_dcd(bool const value) noexcept { pad_reserved_flags.set_flag(0x0020U, value); }

    /// Get dtr: data terminal ready
    [[nodiscard]] constexpr auto dtr() const noexcept -> bool { return pad_reserved_flags.has_flag(0x0010U); }

    /// Set dtr
    constexpr void set_dtr(bool const value) noexcept { pad_reserved_flags.set_flag(0x0010U, value); }

    /// Get dsr: data set ready
    [[nodiscard]] constexpr auto dsr() const noexcept -> bool { return pad_reserved_flags.has_flag(0x0008U); }

    /// Set dsr
    constexpr void set_dsr(bool const value) noexcept { pad_reserved_flags.set_flag(0x0008U, value); }

    /// Get rts: request to send
    [[nodiscard]] constexpr auto rts() const noexcept -> bool { return pad_reserved_flags.has_flag(0x0004U); }

    /// Set rts
    constexpr void set_rts(bool const value) noexcept { pad_reserved_flags.set_flag(0x0004U, value); }

    /// Get cts: clear to send
    [[nodiscard]] constexpr auto cts() const noexcept -> bool { return pad_reserved_flags.has_flag(0x0002U); }

    /// Set cts
    constexpr void set_cts(bool const value) noexcept { pad_reserved_flags.set_flag(0x0002U, value); }

    /// Get ri: ring indicator
    [[nodiscard]] constexpr auto ri() const noexcept -> bool { return pad_reserved_flags.has_flag(0x0001U); }

    /// Set ri
    constexpr void set_ri(bool const value) noexcept { pad_reserved_flags.set_flag(0x0001U, value); }

    // Initialization

    /// Initialize as the smallest ACF_SERIAL message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::serial, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_reserved_flags = 0U;
    }

    // Validation

    /// Typed ACF_SERIAL with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::serial) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfSerialMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfSerialMessage) == AcfSerialMessage::LENGTH, "AcfSerialMessage must be exactly 4 bytes");
static_assert(alignof(AcfSerialMessage) <= 4, "AcfSerialMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfSerialMessage, pad_reserved_flags) == 2, "pad_reserved_flags must be at offset 2");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfSerialMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_SERIAL message: fixed part plus payload (pad removed)
using AcfSerialMessageView = AcfTypedMessageView<AcfSerialMessage>;

/// Parse an ACF_SERIAL message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_serial_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfSerialMessageView>
{
    return acf_parse_typed<AcfSerialMessage>(data);
}

/// Build an ACF_SERIAL message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_serial_build(
    std::span<uint8_t> const out, AcfSerialMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfSerialMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
