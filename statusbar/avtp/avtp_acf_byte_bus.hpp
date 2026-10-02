#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF_BYTE_BUS - Generic Byte Bus (GBB) message - IEEE 1722-2025 Clause 9.4.14
/// Carries a transaction for a byte-oriented bus (SPI, I2C, SMBus and their
/// derivatives) between a Requester and the Responder that owns the bus: the
/// target bus (byte_bus_id), the upper-layer evt bits, the hold-stop and
/// conditional-start controls for the bus, and the transport fields -
/// transaction_num, op (write/read), rsp (response), err, ms (more segments)
/// and read_size/segment_num - that pair responses with requests and let a
/// transfer span AVTPDUs (9.4.14.9 to 9.4.14.14).
///
/// Wire format (16-byte fixed part + payload quadlets), Figure 84:
///   Bytes 0-1:   acf_msg_type[15:9] | acf_msg_length[8:0]
///   Bytes 2-3:   pad[15:14] | mtv[13] | rsv[12:11] | byte_bus_id[10:0]
///   Bytes 4-11:  message_timestamp (ns, valid when mtv)
///   Bytes 12-13: evt[15:12] | rsv[11:10] | hs[9] | cs[8] | transaction_num[7:0]
///   Bytes 14-15: op[15] | rsp[14] | err[13] | ms[12] | read_size/segment_num[11:0]
///   Bytes 16+:   payload, zero-padded to the quadlet (pad octets)

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
using ieee::octlet_t;

/// Generic Byte Bus (GBB) message fixed part - IEEE 1722-2025 Clause 9.4.14, Figure 84
struct AcfByteBusMessage
{
    /// Length of the fixed part on the wire (the payload quadlets follow)
    static constexpr size_t LENGTH = 16;
    /// Payload bounds in quadlets (9.4.14)
    static constexpr size_t MIN_PAYLOAD_QUADLETS = 0;
    static constexpr size_t MAX_PAYLOAD_QUADLETS = ACF_MSG_LENGTH_MAX_QUADLETS - (LENGTH / ACF_QUADLET_OCTETS);

    // Bytes 0-1: common header
    AcfMessageHeader header;

    // Bytes 2-3: pad[15:14] | mtv[13] | rsv[12:11] | byte_bus_id[10:0]
    doublet_t pad_mtv_rsv_bus;

    // Bytes 4-11: message_timestamp (ns, valid when mtv)
    octlet_t message_timestamp;

    // Bytes 12-13: evt[15:12] | rsv[11:10] | hs[9] | cs[8] | transaction_num[7:0]
    doublet_t evt_flags_txn;

    // Bytes 14-15: op[15] | rsp[14] | err[13] | ms[12] | read_size/segment_num[11:0]
    doublet_t op_flags_size;

    // Accessors

    /// Get pad: padding octets at the end of the payload (0 to 3; zero when ms is set)
    [[nodiscard]] constexpr auto pad() const noexcept -> uint8_t { return pad_mtv_rsv_bus.get_bits<uint8_t>(0xC000U, 14); }

    /// Set pad
    constexpr void set_pad(uint8_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0xC000U, 14, value); }

    /// Get mtv: message_timestamp valid
    [[nodiscard]] constexpr auto mtv() const noexcept -> bool { return pad_mtv_rsv_bus.has_flag(0x2000U); }

    /// Set mtv
    constexpr void set_mtv(bool const value) noexcept { pad_mtv_rsv_bus.set_flag(0x2000U, value); }

    /// Get byte_bus_id: target byte bus, 11 bits (application specific, Annex L and O)
    [[nodiscard]] constexpr auto byte_bus_id() const noexcept -> uint16_t { return pad_mtv_rsv_bus.get_bits<uint16_t>(0x07FFU, 0); }

    /// Set byte_bus_id
    constexpr void set_byte_bus_id(uint16_t const value) noexcept { pad_mtv_rsv_bus.set_bits(0x07FFU, 0, value); }

    /// Get message_timestamp: acquisition time in ns (9.4.1.5)
    [[nodiscard]] constexpr auto get_message_timestamp() const noexcept -> uint64_t { return message_timestamp.get(); }

    /// Set message_timestamp
    constexpr void set_message_timestamp(uint64_t const value) noexcept { message_timestamp = value; }

    /// Get evt: upper-layer event bits (Annex G)
    [[nodiscard]] constexpr auto evt() const noexcept -> uint8_t { return evt_flags_txn.get_bits<uint8_t>(0xF000U, 12); }

    /// Set evt
    constexpr void set_evt(uint8_t const value) noexcept { evt_flags_txn.set_bits(0xF000U, 12, value); }

    /// Get hs: hold stop: do not end the payload with the bus's stop indication
    [[nodiscard]] constexpr auto hs() const noexcept -> bool { return evt_flags_txn.has_flag(0x0200U); }

    /// Set hs
    constexpr void set_hs(bool const value) noexcept { evt_flags_txn.set_flag(0x0200U, value); }

    /// Get cs: conditional start: send only if the chain so far had no error
    [[nodiscard]] constexpr auto cs() const noexcept -> bool { return evt_flags_txn.has_flag(0x0100U); }

    /// Set cs
    constexpr void set_cs(bool const value) noexcept { evt_flags_txn.set_flag(0x0100U, value); }

    /// Get transaction_num: request sequence number; responses echo it
    [[nodiscard]] constexpr auto transaction_num() const noexcept -> uint8_t { return evt_flags_txn.get_bits<uint8_t>(0x00FFU, 0); }

    /// Set transaction_num
    constexpr void set_transaction_num(uint8_t const value) noexcept { evt_flags_txn.set_bits(0x00FFU, 0, value); }

    /// Get op: operation: write when set, read when clear
    [[nodiscard]] constexpr auto op() const noexcept -> bool { return op_flags_size.has_flag(0x8000U); }

    /// Set op
    constexpr void set_op(bool const value) noexcept { op_flags_size.set_flag(0x8000U, value); }

    /// Get rsp: response when set, request when clear
    [[nodiscard]] constexpr auto rsp() const noexcept -> bool { return op_flags_size.has_flag(0x4000U); }

    /// Set rsp
    constexpr void set_rsp(bool const value) noexcept { op_flags_size.set_flag(0x4000U, value); }

    /// Get err: error on the associated request (responses only)
    [[nodiscard]] constexpr auto err() const noexcept -> bool { return op_flags_size.has_flag(0x2000U); }

    /// Set err
    constexpr void set_err(bool const value) noexcept { op_flags_size.set_flag(0x2000U, value); }

    /// Get ms: more segments follow in later AVTPDUs
    [[nodiscard]] constexpr auto ms() const noexcept -> bool { return op_flags_size.has_flag(0x1000U); }

    /// Set ms
    constexpr void set_ms(bool const value) noexcept { op_flags_size.set_flag(0x1000U, value); }

    /// Get read_size_segment_num: read_size on read requests, segment_num on read responses and write requests (9.4.14.14)
    [[nodiscard]] constexpr auto read_size_segment_num() const noexcept -> uint16_t
    {
        return op_flags_size.get_bits<uint16_t>(0x0FFFU, 0);
    }

    /// Set read_size_segment_num
    constexpr void set_read_size_segment_num(uint16_t const value) noexcept { op_flags_size.set_bits(0x0FFFU, 0, value); }

    // Initialization

    /// Initialize as the smallest ACF_BYTE_BUS message (fields zero, length = fixed
    /// part plus the minimum payload); set the fields, then build with the payload
    constexpr void init() noexcept
    {
        header.init(AcfMsgType::byte_bus, (LENGTH / ACF_QUADLET_OCTETS) + MIN_PAYLOAD_QUADLETS);
        pad_mtv_rsv_bus = 0U;
        message_timestamp = 0U;
        evt_flags_txn = 0U;
        op_flags_size = 0U;
    }

    // Validation

    /// Typed ACF_BYTE_BUS with a length that covers the fixed part and a payload
    /// within the type's bounds
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (header.msg_type() != AcfMsgType::byte_bus) {
            return false;
        }
        auto const octets = header.msg_length_octets();
        return octets >= LENGTH + (MIN_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS) &&
            octets <= LENGTH + (MAX_PAYLOAD_QUADLETS * ACF_QUADLET_OCTETS);
    }

    auto operator<=>(AcfByteBusMessage const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AcfByteBusMessage) == AcfByteBusMessage::LENGTH, "AcfByteBusMessage must be exactly 16 bytes");
static_assert(alignof(AcfByteBusMessage) <= 4, "AcfByteBusMessage alignment must not exceed 4 bytes");
static_assert(offsetof(AcfByteBusMessage, pad_mtv_rsv_bus) == 2, "pad_mtv_rsv_bus must be at offset 2");
static_assert(offsetof(AcfByteBusMessage, message_timestamp) == 4, "message_timestamp must be at offset 4");
static_assert(offsetof(AcfByteBusMessage, evt_flags_txn) == 12, "evt_flags_txn must be at offset 12");
static_assert(offsetof(AcfByteBusMessage, op_flags_size) == 14, "op_flags_size must be at offset 14");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AcfByteBusMessage> : std::true_type
{};

namespace statusbar::avtp {

/// A parsed ACF_BYTE_BUS message: fixed part plus payload (pad removed)
using AcfByteBusMessageView = AcfTypedMessageView<AcfByteBusMessage>;

/// Parse an ACF_BYTE_BUS message at the start of @p data (the message's own span,
/// as yielded by the walker)
[[nodiscard]] inline auto acf_byte_bus_parse(std::span<uint8_t const> const data) noexcept -> std::optional<AcfByteBusMessageView>
{
    return acf_parse_typed<AcfByteBusMessage>(data);
}

/// Build an ACF_BYTE_BUS message into @p out from @p fixed and the original @p payload.
/// Returns the octets written, or 0 on a bounds failure (see acf_build_typed)
[[nodiscard]] inline auto acf_byte_bus_build(
    std::span<uint8_t> const out, AcfByteBusMessage const& fixed, std::span<uint8_t const> const payload) noexcept -> size_t
{
    return acf_build_typed<AcfByteBusMessage>(out, fixed, payload);
}

/// read_size (9.4.14.14) in octets: bit 11 set means read_size[10:0] is in KiB,
/// where 0 means 1 KiB with segmentation not allowed; bit 11 clear means an
/// exact octet count up to 2047
[[nodiscard]] constexpr auto acf_byte_bus_read_size_octets(uint16_t const read_size) noexcept -> size_t
{
    constexpr uint16_t KIB_FLAG = 0x0800U;
    constexpr uint16_t COUNT_MASK = 0x07FFU;
    if ((read_size & KIB_FLAG) == 0U) {
        return read_size & COUNT_MASK;
    }
    auto const kib = static_cast<size_t>(read_size & COUNT_MASK);
    return (kib == 0U ? size_t{1} : kib) * 1024U;
}

/// True when read_size (bit 11 set, count 0) forbids a segmented response
[[nodiscard]] constexpr auto acf_byte_bus_read_size_unsegmented(uint16_t const read_size) noexcept -> bool
{
    return read_size == 0x0800U;
}

}  // namespace statusbar::avtp
