#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_I2C - I2C message - IEEE 1722-2025 Clause 9.4.16
/// Carries one I2C bus event between a Target Agent (watching the Controller's
/// bus) and a Target Controller (replaying it on the remote Target's bus): the
/// bus (i2c_bus_id), the event code (Table 27 requests, Table 28 responses),
/// the target-response request bit, the transaction number, upper-layer evt
/// bits, the exception code of a response (Table 29) and the one data byte the
/// event carries. The message is exactly four quadlets; pad is transmitted zero
/// and ignored (9.4.16.2).
///
/// Wire format (16-byte fixed part + payload quadlets), Figure 89:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | mtv[13] | rsv[12:11] | i2c_bus_id[10:0]
///   Bytes 4-11:  message_timestamp (ns, valid when mtv)
///   Bytes 12-13: i2c_code[15:12] | trr[11] | reserved[10:8] | transaction_num[7:0]
///   Bytes 14-15: evt[15:12] | exception_code[11:8] | i2c_data[7:0]
///   (no payload: the message is exactly 16 octets)

#include "statusbar/avtp/avtp_acf.hpp"
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
using ieee::octlet_t;

/// I2C message fixed part - IEEE 1722-2025 Clause 9.4.16, Figure 89
struct AcfI2cMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 16;
    /// Payload bounds in quadlets (9.4.16)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = 0;

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | mtv[13] | rsv[12:11] | i2c_bus_id[10:0]
    doublet_t pad_mtv_rsv_bus;

    // Bytes 4-11: message_timestamp (ns, valid when mtv)
    octlet_t message_timestamp;

    // Bytes 12-13: i2c_code[15:12] | trr[11] | reserved[10:8] | transaction_num[7:0]
    doublet_t code_trr_txn;

    // Bytes 14-15: evt[15:12] | exception_code[11:8] | i2c_data[7:0]
    doublet_t evt_exc_data;

    // Accessors

    /// Get pad_bits: the pad field - transmitted zero and ignored on receipt (9.4.16.2); the message has no payload to pad
    [[nodiscard]] constexpr auto pad_bits() const noexcept -> uint8_t { return pad_mtv_rsv_bus.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad_bits
    constexpr void set_pad_bits(uint8_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0xC000U, 14, value); }

    /// Get mtv: message_timestamp valid
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_rsv_bus.has_flag(0x2000U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_rsv_bus.set_flag(0x2000U, value); }

    /// Get i2c_bus_id: I2C bus, 11 bits (application specific, Annex L)
    [[nodiscard]] constexpr auto i2c_bus_id() const noexcept -> uint16_t { return pad_mtv_rsv_bus.get_bits<uint16_t>(0x07FFU, 0); }

    /// Set i2c_bus_id
    constexpr void set_i2c_bus_id(uint16_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0x07FFU, 0, value); }

    /// Get message_timestamp: acquisition time in ns (9.4.1.5)
    [[nodiscard]] constexpr auto get_message_timestamp() const noexcept -> uint64_t { return message_timestamp.get(); }

    /// Set message_timestamp
    constexpr void set_message_timestamp(uint64_t const value) noexcept { message_timestamp = value; }

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

    /// Initialize as the smallest ACF_I2C message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::i2c, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_rsv_bus = 0U;
        message_timestamp = 0U;
        code_trr_txn = 0U;
        evt_exc_data = 0U;
    }

    // Validation

    /// Typed ACF_I2C with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::i2c) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfI2cMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfI2cMessage) == AcfI2cMessage::LENGTH, "AcfI2cMessage must be exactly 16 bytes");
static_assert(alignof(AcfI2cMessage) <= 4, "AcfI2cMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfI2cMessage, pad_mtv_rsv_bus) == 2, "pad_mtv_rsv_bus must be at offset 2");
static_assert(offsetof(AcfI2cMessage, message_timestamp) == 4, "message_timestamp must be at offset 4");
static_assert(offsetof(AcfI2cMessage, code_trr_txn) == 12, "code_trr_txn must be at offset 12");
static_assert(offsetof(AcfI2cMessage, evt_exc_data) == 14, "evt_exc_data must be at offset 14");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfI2cMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_I2C message: fixed part plus payload (pad removed)
using AcfI2cMessageView = AcfTypedMessageView<AcfI2cMessage>;

/// Parse an ACF_I2C message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_i2c_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfI2cMessageView>
{
    return acf_parse_typed<AcfI2cMessage>(data);
}

/// Build an ACF_I2C message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_i2c_build(
    std::span<uint8_t> const out, AcfI2cMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfI2cMessage>(out, fixed, payload);
}

/// i2c_code values - IEEE 1722-2025 Table 27 (requests, from the Target Agent)
/// and Table 28 (responses, from the Target Controller)
namespace AcfI2cCode {

constexpr uint8_t cr1_start = 0x0U;             ///< bus transaction start with <addr,rw> (START)
constexpr uint8_t cr2_address_continue = 0x1U;  ///< address continuation <addr> (10-bit addressing)
constexpr uint8_t cr3_write_continue = 0x2U;    ///< write continue <data>
constexpr uint8_t cr4_write_end = 0x3U;         ///< write end (STOP)
constexpr uint8_t cr5_write_restart = 0x4U;     ///< write end followed by restart <addr,rw>
constexpr uint8_t cr6_read_continue = 0x5U;     ///< read continue (ACK)
constexpr uint8_t cr7_read_end = 0x6U;          ///< read end (NACK and STOP)
constexpr uint8_t cr8_read_restart = 0x7U;      ///< read acknowledge followed by restart <addr,rw>
constexpr uint8_t tr1_nack = 0x8U;              ///< bus transaction NACK
constexpr uint8_t tr2_ack = 0x9U;               ///< bus transaction ACK
constexpr uint8_t tr3_read_data = 0xAU;         ///< read data response <data>
constexpr uint8_t tr4_read_ack_data = 0xBU;     ///< read request acknowledgement with read data <data>
constexpr uint8_t tr5_stop = 0xCU;              ///< STOP confirmation (response to CR4 / CR7)
// 0xD - 0xF: reserved

}  // namespace AcfI2cCode

/// Name of an i2c_code value ("CR1", "TR5", "RESERVED")
[[nodiscard]] constexpr auto acf_i2c_code_name(uint8_t const code) noexcept -> std::string_view
{
    switch (code) {
        case AcfI2cCode::cr1_start:
            return "CR1";
        case AcfI2cCode::cr2_address_continue:
            return "CR2";
        case AcfI2cCode::cr3_write_continue:
            return "CR3";
        case AcfI2cCode::cr4_write_end:
            return "CR4";
        case AcfI2cCode::cr5_write_restart:
            return "CR5";
        case AcfI2cCode::cr6_read_continue:
            return "CR6";
        case AcfI2cCode::cr7_read_end:
            return "CR7";
        case AcfI2cCode::cr8_read_restart:
            return "CR8";
        case AcfI2cCode::tr1_nack:
            return "TR1";
        case AcfI2cCode::tr2_ack:
            return "TR2";
        case AcfI2cCode::tr3_read_data:
            return "TR3";
        case AcfI2cCode::tr4_read_ack_data:
            return "TR4";
        case AcfI2cCode::tr5_stop:
            return "TR5";
        default:
            return "RESERVED";
    }
}

/// True for a request code (Table 27), false for a response or reserved code
[[nodiscard]] constexpr auto acf_i2c_code_is_request(uint8_t const code) noexcept -> bool
{
    return code <= AcfI2cCode::cr8_read_restart;
}

/// exception_code values - IEEE 1722-2025 Table 29
namespace AcfI2cException {

constexpr uint8_t none = 0x0U;  ///< no exception detected
// 0x1 - 0x3: reserved
constexpr uint8_t user_first = 0x4U;  ///< user-defined exceptions 0x4 - 0x7
constexpr uint8_t user_last = 0x7U;
constexpr uint8_t bus_timeout = 0x8U;           ///< timed out waiting for an ack or data
constexpr uint8_t bus_busy = 0x9U;              ///< could not start: bus never went idle
constexpr uint8_t controller_conflict = 0xAU;   ///< i2c_bus_id does not match the initial request
constexpr uint8_t transaction_sequence = 0xBU;  ///< transaction_num out of sequence
constexpr uint8_t start_error = 0xCU;           ///< a non-Start request arrived on an idle bus
constexpr uint8_t sequence_error = 0xDU;        ///< request out of line with the previous request
// 0xE - 0xF: reserved

}  // namespace AcfI2cException

/// Name of an exception_code value
[[nodiscard]] constexpr auto acf_i2c_exception_name(uint8_t const code) noexcept -> std::string_view
{
    switch (code) {
        case AcfI2cException::none:
            return "NONE";
        case AcfI2cException::bus_timeout:
            return "BUS_TIMEOUT";
        case AcfI2cException::bus_busy:
            return "BUS_BUSY";
        case AcfI2cException::controller_conflict:
            return "CONTROLLER_CONFLICT";
        case AcfI2cException::transaction_sequence:
            return "TRANSACTION_SEQUENCE";
        case AcfI2cException::start_error:
            return "START_ERROR";
        case AcfI2cException::sequence_error:
            return "SEQUENCE_ERROR";
        default:
            break;
    }
    if (code >= AcfI2cException::user_first && code <= AcfI2cException::user_last) {
        return "USER";
    }
    return "RESERVED";
}

/// The 7-bit target address carried in i2c_data of a start/restart event
[[nodiscard]] constexpr auto acf_i2c_address(uint8_t const i2c_data) noexcept -> uint8_t
{
    return static_cast<uint8_t>(i2c_data >> 1U);
}

/// True when i2c_data of a start/restart event asks for a read (bit 0 set)
[[nodiscard]] constexpr auto acf_i2c_is_read(uint8_t const i2c_data) noexcept -> bool
{
    return (i2c_data & 0x01U) != 0U;
}

}  // namespace statusbar::avtp
