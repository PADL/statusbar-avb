// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/stun/stun_message.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <cstring>
#include <string_view>

#include <netinet/in.h>

namespace statusbar::stun {

namespace {

// Byte marshalling goes through the ieee network-ordered types — these thin
// wrappers only adapt them to the parser's raw-pointer cursor style.

void write_u16_be(uint8_t* p, uint16_t v) noexcept
{
    ieee::doublet_t const d{v};
    statusbar::span_copy(std::span<uint8_t, 2>{p, 2}, d.span());
}

void write_u32_be(uint8_t* p, uint32_t v) noexcept
{
    ieee::quadlet_t const q{v};
    statusbar::span_copy(std::span<uint8_t, 4>{p, 4}, q.span());
}

[[nodiscard]] auto read_u16_be(uint8_t const* p) noexcept -> uint16_t
{
    ieee::doublet_t d{};
    statusbar::span_copy(d.span(), std::span<uint8_t const, 2>{p, 2});
    return d.get();
}

[[nodiscard]] auto read_u32_be(uint8_t const* p) noexcept -> uint32_t
{
    ieee::quadlet_t q{};
    statusbar::span_copy(q.span(), std::span<uint8_t const, 4>{p, 4});
    return q.get();
}

/// Encode method+class into the 16-bit STUN type field per RFC 8489 §5.
[[nodiscard]] auto encode_message_type(Method method, Class klass) noexcept -> uint16_t
{
    auto const m = static_cast<uint16_t>(method);
    auto const c = static_cast<uint8_t>(klass);

    // Split 12-bit method into M11..M0 fields and interleave with class bits.
    uint16_t const m_lo = m & 0x000FU;
    uint16_t const m_mid = m & 0x0070U;
    uint16_t const m_hi = m & 0x0F80U;

    uint16_t const c0_bit = (c & 0x01U) != 0U ? CLASS_C0_MASK : 0U;
    uint16_t const c1_bit = (c & 0x02U) != 0U ? CLASS_C1_MASK : 0U;

    return static_cast<uint16_t>(m_lo | (m_mid << 1) | (m_hi << 2) | c0_bit | c1_bit);
}

[[nodiscard]] auto decode_message_type(uint16_t raw, Method& method, Class& klass) noexcept -> std::error_code
{
    if ((raw & LEADING_BITS_MASK) != 0) {
        return make_error_code(StunError::InvalidLeadingBits);
    }

    uint8_t const c0 = (raw & CLASS_C0_MASK) != 0 ? 1U : 0U;
    uint8_t const c1 = (raw & CLASS_C1_MASK) != 0 ? 1U : 0U;
    klass = static_cast<Class>(static_cast<uint8_t>((c1 << 1) | c0));

    uint16_t const m_lo = raw & 0x000FU;
    uint16_t const m_mid = (raw & 0x00E0U) >> 1;
    uint16_t const m_hi = (raw & 0x3E00U) >> 2;
    method = static_cast<Method>(m_lo | m_mid | m_hi);
    return {};
}

[[nodiscard]] auto pad4(size_t n) noexcept -> size_t
{
    return (n + 3U) & ~size_t{3U};
}

}  // namespace

auto encode_header(MessageHeader const& h, std::span<uint8_t> buf) -> std::error_code
{
    if (buf.size() < HEADER_SIZE) {
        return make_error_code(StunError::BufferTooSmall);
    }
    if ((h.body_length & 0x3U) != 0) {
        return make_error_code(StunError::InvalidMessageLength);
    }

    uint16_t const raw_type = encode_message_type(h.method, h.klass);
    auto* d = buf.data();
    write_u16_be(d + 0, raw_type);
    write_u16_be(d + 2, h.body_length);
    write_u32_be(d + 4, MAGIC_COOKIE);
    statusbar::span_copy(std::span<uint8_t>{d + 8, TRANSACTION_ID_SIZE}, statusbar::make_const_span(h.transaction_id.bytes));
    return {};
}

auto decode_header(std::span<uint8_t const> buf, MessageHeader& out) -> std::error_code
{
    if (buf.size() < HEADER_SIZE) {
        return make_error_code(StunError::DatagramTooShort);
    }
    auto const* d = buf.data();

    if (auto ec = decode_message_type(read_u16_be(d + 0), out.method, out.klass); ec) {
        return ec;
    }

    uint16_t const length = read_u16_be(d + 2);
    if ((length & 0x3U) != 0) {
        return make_error_code(StunError::InvalidMessageLength);
    }
    out.body_length = length;

    if (read_u32_be(d + 4) != MAGIC_COOKIE) {
        return make_error_code(StunError::InvalidMagicCookie);
    }

    statusbar::span_copy(statusbar::make_span(out.transaction_id.bytes), std::span<uint8_t const>{d + 8, TRANSACTION_ID_SIZE});
    return {};
}

auto AttributeIterator::next(uint16_t& attr_type, std::span<uint8_t const>& value, bool& done_out) -> std::error_code
{
    done_out = false;
    if (cursor_ == body_.size()) {
        done_out = true;
        return {};
    }
    if (cursor_ + ATTRIBUTE_HEADER_SIZE > body_.size()) {
        return make_error_code(StunError::AttributeTruncated);
    }
    if (count_ >= max_attributes_per_message) {
        return make_error_code(StunError::AttributeTruncated);
    }

    auto const* p = body_.data() + cursor_;
    attr_type = read_u16_be(p + 0);
    uint16_t const length = read_u16_be(p + 2);
    size_t const padded = pad4(length);

    if (cursor_ + ATTRIBUTE_HEADER_SIZE + padded > body_.size()) {
        return make_error_code(StunError::AttributeTruncated);
    }

    value = body_.subspan(cursor_ + ATTRIBUTE_HEADER_SIZE, length);
    cursor_ += ATTRIBUTE_HEADER_SIZE + padded;
    ++count_;
    return {};
}

auto append_attribute(std::span<uint8_t> buf, size_t& cursor, uint16_t attr_type, std::span<uint8_t const> value) -> std::error_code
{
    size_t const padded = pad4(value.size());
    size_t const total = ATTRIBUTE_HEADER_SIZE + padded;
    if (cursor + total > buf.size()) {
        return make_error_code(StunError::BufferTooSmall);
    }

    auto* p = buf.data() + cursor;
    write_u16_be(p + 0, attr_type);
    write_u16_be(p + 2, static_cast<uint16_t>(value.size()));
    if (!value.empty()) {
        statusbar::span_copy(std::span<uint8_t>{p + ATTRIBUTE_HEADER_SIZE, value.size()}, value);
    }
    if (padded > value.size()) {
        statusbar::span_zero(std::span<uint8_t>{p + ATTRIBUTE_HEADER_SIZE + value.size(), padded - value.size()});
    }
    cursor += total;
    return {};
}

namespace {

/// Encode an XOR-MAPPED-ADDRESS value (the bytes after the TLV header).
/// Returns the number of bytes written into `value`, or an error.
[[nodiscard]] auto encode_xor_mapped_address_value(
    statusbar::net::SocketAddress const& addr, TransactionId const& txid, std::span<uint8_t> value, size_t& written_out)
    -> std::error_code
{
    if (!addr.valid()) {
        return make_error_code(StunError::InvalidAddressFamily);
    }
    auto const family = addr.family();
    auto const port = addr.port();
    uint16_t const x_port = static_cast<uint16_t>(port ^ PORT_XOR_MASK);

    if (family == AF_INET) {
        if (value.size() < 8) {
            return make_error_code(StunError::BufferTooSmall);
        }
        // Reinterpreting wire-format socket address bytes as sockaddr_in — standard POSIX practice for AF_INET address access.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* sin = reinterpret_cast<sockaddr_in const*>(addr.sockaddr());
        uint32_t const addr_be = sin->sin_addr.s_addr;
        uint32_t const addr_host = ntohl(addr_be);
        uint32_t const x_addr = addr_host ^ MAGIC_COOKIE;

        value[0] = 0;
        value[1] = static_cast<uint8_t>(AddressFamily::IPv4);
        write_u16_be(value.data() + 2, x_port);
        write_u32_be(value.data() + 4, x_addr);
        written_out = 8;
        return {};
    }
    if (family == AF_INET6) {
        if (value.size() < 20) {
            return make_error_code(StunError::BufferTooSmall);
        }
        // Reinterpreting wire-format socket address bytes as sockaddr_in6 — standard POSIX practice for AF_INET6 address access.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* sin6 = reinterpret_cast<sockaddr_in6 const*>(addr.sockaddr());
        std::array<uint8_t, 4> cookie_bytes{};
        write_u32_be(cookie_bytes.data(), MAGIC_COOKIE);

        value[0] = 0;
        value[1] = static_cast<uint8_t>(AddressFamily::IPv6);
        write_u16_be(value.data() + 2, x_port);

        for (size_t i = 0; i < 4; ++i) {
            value[4 + i] = sin6->sin6_addr.s6_addr[i] ^ cookie_bytes[i];
        }
        for (size_t i = 0; i < 12; ++i) {
            value[8 + i] = sin6->sin6_addr.s6_addr[4 + i] ^ txid.bytes[i];
        }
        written_out = 20;
        return {};
    }
    return make_error_code(StunError::UnsupportedAddressFamily);
}

}  // namespace

auto append_xor_mapped_address(
    std::span<uint8_t> buf,
    size_t& cursor,
    uint16_t attr_type,
    statusbar::net::SocketAddress const& addr,
    TransactionId const& txid) -> std::error_code
{
    std::array<uint8_t, 20> value_buf{};
    size_t written = 0;
    if (auto ec = encode_xor_mapped_address_value(addr, txid, value_buf, written); ec) {
        return ec;
    }
    return append_attribute(buf, cursor, attr_type, std::span<uint8_t const>{value_buf.data(), written});
}

auto decode_xor_mapped_address(std::span<uint8_t const> value, TransactionId const& txid, statusbar::net::SocketAddress& out)
    -> std::error_code
{
    if (value.size() < 4) {
        return make_error_code(StunError::InvalidAttributeLength);
    }
    if (value[0] != 0) {
        return make_error_code(StunError::InvalidAddressFamily);
    }
    auto const family = static_cast<AddressFamily>(value[1]);
    uint16_t const x_port = read_u16_be(value.data() + 2);
    uint16_t const port = static_cast<uint16_t>(x_port ^ PORT_XOR_MASK);

    if (family == AddressFamily::IPv4) {
        if (value.size() != 8) {
            return make_error_code(StunError::InvalidAttributeLength);
        }
        uint32_t const x_addr = read_u32_be(value.data() + 4);
        uint32_t const host_addr = x_addr ^ MAGIC_COOKIE;
        out = statusbar::net::SocketAddress::ipv4(host_addr, port);
        return {};
    }
    if (family == AddressFamily::IPv6) {
        if (value.size() != 20) {
            return make_error_code(StunError::InvalidAttributeLength);
        }
        std::array<uint8_t, 4> cookie_bytes{};
        write_u32_be(cookie_bytes.data(), MAGIC_COOKIE);

        std::array<uint8_t, 16> addr_bytes{};
        for (size_t i = 0; i < 4; ++i) {
            addr_bytes[i] = value[4 + i] ^ cookie_bytes[i];
        }
        for (size_t i = 0; i < 12; ++i) {
            addr_bytes[4 + i] = value[8 + i] ^ txid.bytes[i];
        }
        out = statusbar::net::SocketAddress::ipv6(addr_bytes, port);
        return {};
    }
    return make_error_code(StunError::UnsupportedAddressFamily);
}

auto xor_mapped_address_attribute_size(statusbar::net::SocketAddress const& addr) -> size_t
{
    if (addr.family() == AF_INET) {
        return ATTRIBUTE_HEADER_SIZE + 8;
    }
    if (addr.family() == AF_INET6) {
        return ATTRIBUTE_HEADER_SIZE + 20;
    }
    return 0;
}

auto append_error_code(std::span<uint8_t> buf, size_t& cursor, uint16_t code, std::string_view reason) -> std::error_code
{
    if (code < 100U || code > 699U) {
        return make_error_code(StunError::InvalidErrorCode);
    }
    auto const class_byte = static_cast<uint8_t>(code / 100U);
    auto const number_byte = static_cast<uint8_t>(code % 100U);

    size_t const value_len = 4 + reason.size();
    if (value_len > 0xFFFFU) {
        return make_error_code(StunError::InvalidAttributeLength);
    }

    std::array<uint8_t, 4 + 256> value_buf{};
    if (reason.size() > value_buf.size() - 4) {
        return make_error_code(StunError::InvalidAttributeLength);
    }
    value_buf[0] = 0;
    value_buf[1] = 0;
    value_buf[2] = class_byte & 0x07U;
    value_buf[3] = number_byte;
    statusbar::span_copy(std::span<uint8_t>{value_buf.data() + 4, reason.size()}, statusbar::make_const_span(reason));

    return append_attribute(
        buf, cursor, static_cast<uint16_t>(AttributeType::ErrorCode), std::span<uint8_t const>{value_buf.data(), value_len});
}

auto decode_error_code(std::span<uint8_t const> value, uint16_t& code, std::span<uint8_t const>& reason) -> std::error_code
{
    if (value.size() < 4) {
        return make_error_code(StunError::InvalidErrorCode);
    }
    auto const class_byte = static_cast<uint16_t>(value[2] & 0x07U);
    auto const number_byte = value[3];
    if (class_byte < 1U || class_byte > 6U || number_byte > 99U) {
        return make_error_code(StunError::InvalidErrorCode);
    }
    code = static_cast<uint16_t>((class_byte * 100U) + number_byte);
    reason = value.subspan(4);
    return {};
}

}  // namespace statusbar::stun
