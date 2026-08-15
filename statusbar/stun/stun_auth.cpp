// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/stun/stun_auth.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/stun/stun_message.hpp"

#include <cstring>

namespace statusbar::stun {

namespace {

// Byte marshalling via the ieee network-ordered types, adapted to the
// raw-pointer cursor style used here.
void write_u16_be(uint8_t* p, uint16_t v) noexcept
{
    ieee::doublet_t const d{v};
    statusbar::span_copy(std::span<uint8_t, 2>{p, 2}, d.span());
}

[[nodiscard]] auto read_u16_be(uint8_t const* p) noexcept -> uint16_t
{
    ieee::doublet_t d{};
    statusbar::span_copy(d.span(), std::span<uint8_t const, 2>{p, 2});
    return d.get();
}

/// Compute the deterministic AES-128-SIV tag over `aad` with empty
/// plaintext. The library encrypts in place; an empty span is a no-op.
[[nodiscard]] auto compute_siv_tag(statusbar::crypto::Aes128SivKey const& key, std::span<uint8_t const> aad) -> MicTag
{
    std::span<uint8_t> const empty_plaintext{};
    auto const tag = statusbar::crypto::aes128_siv_encrypt(key, empty_plaintext, aad);
    MicTag out{};
    statusbar::span_copy(statusbar::make_span(out), statusbar::make_const_span(tag));
    return out;
}

[[nodiscard]] auto verify_siv_tag(
    statusbar::crypto::Aes128SivKey const& key, std::span<uint8_t const> aad, std::span<uint8_t const, MIC_SIZE> tag) -> bool
{
    std::span<uint8_t> const empty{};
    return statusbar::crypto::aes128_siv_decrypt(key, empty, tag, aad);
}

}  // namespace

auto append_mic(std::span<uint8_t> buf, size_t& cursor, statusbar::crypto::Aes128SivKey const& key) -> std::error_code
{
    constexpr size_t mic_tlv_size = ATTRIBUTE_HEADER_SIZE + MIC_SIZE;

    if (cursor < HEADER_SIZE) {
        return make_error_code(StunError::BufferTooSmall);
    }
    if (cursor + mic_tlv_size > buf.size()) {
        return make_error_code(StunError::BufferTooSmall);
    }

    auto const new_body_length = static_cast<uint16_t>((cursor + mic_tlv_size) - HEADER_SIZE);
    write_u16_be(buf.data() + 2, new_body_length);

    auto const tag = compute_siv_tag(key, std::span<uint8_t const>{buf.data(), cursor});

    auto* p = buf.data() + cursor;
    write_u16_be(p + 0, static_cast<uint16_t>(AttributeType::Mic));
    write_u16_be(p + 2, static_cast<uint16_t>(MIC_SIZE));
    statusbar::span_copy(std::span<uint8_t>{p + ATTRIBUTE_HEADER_SIZE, MIC_SIZE}, statusbar::make_const_span(tag));
    cursor += mic_tlv_size;
    return {};
}

auto verify_mic(std::span<uint8_t const> datagram, statusbar::crypto::Aes128SivKey const& key, size_t& inner_end_out)
    -> std::error_code
{
    constexpr size_t mic_tlv_size = ATTRIBUTE_HEADER_SIZE + MIC_SIZE;
    if (datagram.size() < HEADER_SIZE + mic_tlv_size) {
        return make_error_code(StunError::AuthenticationMissing);
    }

    size_t const mic_offset = datagram.size() - mic_tlv_size;
    auto const* tail = datagram.data() + mic_offset;
    if (read_u16_be(tail + 0) != static_cast<uint16_t>(AttributeType::Mic)) {
        return make_error_code(StunError::AuthenticationMissing);
    }
    if (read_u16_be(tail + 2) != MIC_SIZE) {
        return make_error_code(StunError::InvalidAttributeLength);
    }

    std::span<uint8_t const, MIC_SIZE> const tag{tail + ATTRIBUTE_HEADER_SIZE, MIC_SIZE};
    if (!verify_siv_tag(key, std::span<uint8_t const>{datagram.data(), mic_offset}, tag)) {
        return make_error_code(StunError::AuthFailed);
    }

    inner_end_out = mic_offset;
    return {};
}

}  // namespace statusbar::stun
