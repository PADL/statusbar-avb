#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_PARALLEL - Parallel (PARALLEL) message - IEEE 1722-2025 Clause 9.4.9
/// Carries the state of a parallel interface or an array of binary control
/// points: bit_width valid bits (0 means 256) starting at bit 0 of the payload,
/// one bit per pin, zero-padded to the quadlet. There is no pad field; the
/// padding follows from bit_width (9.4.9.2).
///
/// Wire format (4-byte fixed part + payload quadlets), Figure 75:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Byte 2:      reserved
///   Byte 3:      bit_width (valid bits; 0 = 256)
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

using ieee::octet_t;

/// Parallel (PARALLEL) message fixed part - IEEE 1722-2025 Clause 9.4.9, Figure 75
struct AcfParallelMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 4;
    /// Payload bounds in quadlets (9.4.9)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 1;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 8;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Byte 2: reserved
    octet_t reserved0;

    // Byte 3: bit_width (valid bits; 0 = 256)
    octet_t bit_width;

    // Accessors

    /// Get bit_width: number of valid bits in the payload starting at bit 0 (0 means 256)
    [[nodiscard]] constexpr auto get_bit_width() const noexcept -> uint8_t { return bit_width.get(); }

    /// Set bit_width
    constexpr void set_bit_width(uint8_t const value) noexcept { bit_width = value; }

    /// Valid bits in the payload: bit_width, or 256 when it is zero (9.4.9.2)
    [[nodiscard]] constexpr auto valid_bits() const noexcept -> uint16_t
    {
        return get_bit_width() == 0U ? uint16_t{256} : uint16_t{get_bit_width()};
    }

    /// Octets the valid bits occupy before quadlet padding
    [[nodiscard]] constexpr auto payload_octets() const noexcept -> size_t { return (static_cast<size_t>(valid_bits()) + 7U) / 8U; }

    /// Implied pad: this message carries no pad field; the padding is whatever the
    /// declared length holds beyond payload_octets() (zero when the length is short,
    /// which is_valid() rejects)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t
    {
        auto const octets = header.msg_length_octets();
        if (octets < LENGTH + payload_octets()) {
            return 0U;
        }
        return static_cast<uint8_t>(octets - LENGTH - payload_octets());
    }

    /// No pad field to set: the builder's payload must match the fields (is_valid())
    constexpr void set_pad(uint8_t const /*value*/) noexcept {}

    // Initialization

    /// Initialize as the smallest ACF_PARALLEL message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::parallel, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        reserved0 = 0U;
        bit_width = 0U;
    }

    // Validation

    /// Typed ACF_PARALLEL with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::parallel) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        if (octets < LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) ||
            octets > LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS)) {
            return false;
        }
        // No pad field: the declared length must be exactly the quadlets the
        // fields' payload needs (9.4.1.7)
        if (octets != LENGTH + (acf_quadlets_for_octets(payload_octets()) * ACF_QUADLET_OCTETS)) {
            return false;
        }
        return true;
    }

    auto operator<=>(AcfParallelMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfParallelMessage) == AcfParallelMessage::LENGTH, "AcfParallelMessage must be exactly 4 bytes");
static_assert(alignof(AcfParallelMessage) <= 4, "AcfParallelMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfParallelMessage, reserved0) == 2, "reserved0 must be at offset 2");
static_assert(offsetof(AcfParallelMessage, bit_width) == 3, "bit_width must be at offset 3");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfParallelMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_PARALLEL message: fixed part plus payload (pad removed)
using AcfParallelMessageView = AcfTypedMessageView<AcfParallelMessage>;

/// Parse an ACF_PARALLEL message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_parallel_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfParallelMessageView>
{
    return acf_parse_typed<AcfParallelMessage>(data);
}

/// Build an ACF_PARALLEL message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_parallel_build(
    std::span<uint8_t> const out, AcfParallelMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfParallelMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
