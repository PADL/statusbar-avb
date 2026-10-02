#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_I2C_BRIEF - Abbreviated I2C message - IEEE 1722-2025 Clause 9.4.17
/// The I2C message (avtp_acf_i2c.hpp, which also holds the code and exception
/// tables) without the two-quadlet message_timestamp: exactly two quadlets. mtv
/// is transmitted as zero and ignored on receipt (9.4.17.2).
///
/// Wire format (8-byte fixed part + payload quadlets), Figure 91:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | mtv[13] | rsv[12:11] | i2c_bus_id[10:0]
///   Bytes 4-5:   i2c_code[15:12] | trr[11] | reserved[10:8] | transaction_num[7:0]
///   Bytes 6-7:   evt[15:12] | exception_code[11:8] | i2c_data[7:0]
///   (no payload: the message is exactly 8 octets)

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/avtp/avtp_acf_i2c.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/ieee/ieee_base.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;

/// Abbreviated I2C message fixed part - IEEE 1722-2025 Clause 9.4.17, Figure 91
struct AcfI2cBriefMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 8;
    /// Payload bounds in quadlets (9.4.17)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 0;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | mtv[13] | rsv[12:11] | i2c_bus_id[10:0]
    doublet_t pad_mtv_rsv_bus;

    // Bytes 4-5: i2c_code[15:12] | trr[11] | reserved[10:8] | transaction_num[7:0]
    doublet_t code_trr_txn;

    // Bytes 6-7: evt[15:12] | exception_code[11:8] | i2c_data[7:0]
    doublet_t evt_exc_data;

    // Accessors

    /// Get pad_bits: the pad field - transmitted zero and ignored on receipt (9.4.16.2); the message has no payload to pad
    [[nodiscard]] constexpr auto pad_bits() const noexcept -> uint8_t { return pad_mtv_rsv_bus.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad_bits
    constexpr void set_pad_bits(uint8_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0xC000U, 14, value); }

    /// Get mtv: message_timestamp valid - always zero on a brief message (9.4.17.2)
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_rsv_bus.has_flag(0x2000U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_rsv_bus.set_flag(0x2000U, value); }

    /// Get i2c_bus_id: I2C bus, 11 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto i2c_bus_id() const noexcept -> uint16_t { return pad_mtv_rsv_bus.get_bits<uint16_t>(0x07FFU, 0); }

    /// Set i2c_bus_id
    constexpr void set_i2c_bus_id(uint16_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0x07FFU, 0, value); }

    /// Get i2c_code: bus event (AcfI2cCode: Table 27 requests 0-7, Table 28 responses 8-12)
    [[nodiscard]] constexpr auto i2c_code() const noexcept -> uint8_t { return code_trr_txn.get_bits<uint8_t>(0xF000U, 12); }

    /// Set i2c_code
    constexpr void set_i2c_code(uint8_t const value) noexcept { code_trr_txn.set_bits(0xF000U, 12, value); }

    /// Get trr: target response request: answer a CR4/CR7 end request with TR5
    [[nodiscard]] constexpr auto trr() const noexcept -> bool { return code_trr_txn.has_flag(0x0800U); }

    /// Set trr
    constexpr void set_trr(bool const value) noexcept { code_trr_txn.set_flag(0x0800U, value); }

    /// Get transaction_num: request sequence number per bus; responses echo it
    [[nodiscard]] constexpr auto transaction_num() const noexcept -> uint8_t { return code_trr_txn.get_bits<uint8_t>(0x00FFU, 0); }

    /// Set transaction_num
    constexpr void set_transaction_num(uint8_t const value) noexcept { code_trr_txn.set_bits(0x00FFU, 0, value); }

    /// Get evt: upper-layer event bits (Annex G)
    [[nodiscard]] constexpr auto evt() const noexcept -> uint8_t { return evt_exc_data.get_bits<uint8_t>(0xF000U, 12); }

    /// Set evt
    constexpr void set_evt(uint8_t const value) noexcept { evt_exc_data.set_bits(0xF000U, 12, value); }

    /// Get exception_code: response exception (AcfI2cException, Table 29)
    [[nodiscard]] constexpr auto exception_code() const noexcept -> uint8_t { return evt_exc_data.get_bits<uint8_t>(0x0F00U, 8); }

    /// Set exception_code
    constexpr void set_exception_code(uint8_t const value) noexcept { evt_exc_data.set_bits(0x0F00U, 8, value); }

    /// Get i2c_data: the event's data byte: <addr,rw> for starts (address in bits 7:1, read = 1 in bit 0), data otherwise
    [[nodiscard]] constexpr auto i2c_data() const noexcept -> uint8_t { return evt_exc_data.get_bits<uint8_t>(0x00FFU, 0); }

    /// Set i2c_data
    constexpr void set_i2c_data(uint8_t const value) noexcept { evt_exc_data.set_bits(0x00FFU, 0, value); }

    /// This message type has no pad field: the payload is taken as carried
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return 0U; }

    /// No pad field to set (the shared builder calls this; nothing to record)
    constexpr void set_pad(uint8_t const /*value*/) noexcept {}

    // Initialization

    /// Initialize as the smallest ACF_I2C_BRIEF message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::i2c_brief, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_rsv_bus = 0U;
        code_trr_txn = 0U;
        evt_exc_data = 0U;
    }

    // Validation

    /// Typed ACF_I2C_BRIEF with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::i2c_brief) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfI2cBriefMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfI2cBriefMessage) == AcfI2cBriefMessage::LENGTH, "AcfI2cBriefMessage must be exactly 8 bytes");
static_assert(alignof(AcfI2cBriefMessage) <= 4, "AcfI2cBriefMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfI2cBriefMessage, pad_mtv_rsv_bus) == 2, "pad_mtv_rsv_bus must be at offset 2");
static_assert(offsetof(AcfI2cBriefMessage, code_trr_txn) == 4, "code_trr_txn must be at offset 4");
static_assert(offsetof(AcfI2cBriefMessage, evt_exc_data) == 6, "evt_exc_data must be at offset 6");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfI2cBriefMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_I2C_BRIEF message: fixed part plus payload (pad removed)
using AcfI2cBriefMessageView = AcfTypedMessageView<AcfI2cBriefMessage>;

/// Parse an ACF_I2C_BRIEF message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_i2c_brief_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfI2cBriefMessageView>
{
    return acf_parse_typed<AcfI2cBriefMessage>(data);
}

/// Build an ACF_I2C_BRIEF message into @p out from @p fixed and the original @p payload;
/// mtv is forced to zero (brief messages carry no timestamp). Returns the octets
/// written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_i2c_brief_build(
    std::span<uint8_t> const out, AcfI2cBriefMessage fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    fixed.set_mtv(false);
    return acf_build_typed<AcfI2cBriefMessage>(out, fixed, payload);
}

}  // namespace statusbar::avtp
