// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/stun/stun_register.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/stun/stun_auth.hpp"
#include "statusbar/stun/stun_message.hpp"

#include <cstring>

namespace statusbar::stun {

namespace {

// Byte marshalling via the ieee network-ordered types, adapted to the
// raw-pointer cursor style used here.
void write_u32_be(uint8_t* p, uint32_t v) noexcept
{
    ieee::quadlet_t const q{v};
    statusbar::span_copy(std::span<uint8_t, 4>{p, 4}, q.span());
}

[[nodiscard]] auto read_u32_be(uint8_t const* p) noexcept -> uint32_t
{
    ieee::quadlet_t q{};
    statusbar::span_copy(q.span(), std::span<uint8_t const, 4>{p, 4});
    return q.get();
}

[[nodiscard]] auto write_session_id(std::span<uint8_t> buf, size_t& cursor, SessionId const& sid) -> std::error_code
{
    return append_attribute(
        buf, cursor, static_cast<uint16_t>(AttributeType::SessionId), std::span<uint8_t const>{sid.bytes.data(), sid.bytes.size()});
}

[[nodiscard]] auto write_eui64(std::span<uint8_t> buf, size_t& cursor, AttributeType type, statusbar::ieee::Eui64 const& eui)
    -> std::error_code
{
    auto const eui_span = eui.span();
    return append_attribute(buf, cursor, static_cast<uint16_t>(type), std::span<uint8_t const>{eui_span.data(), eui_span.size()});
}

[[nodiscard]] auto write_one_byte_attr(std::span<uint8_t> buf, size_t& cursor, AttributeType type, uint8_t value) -> std::error_code
{
    std::array<uint8_t, 1> data{value};
    return append_attribute(buf, cursor, static_cast<uint16_t>(type), std::span<uint8_t const>{data});
}

[[nodiscard]] auto write_u32_attr(std::span<uint8_t> buf, size_t& cursor, AttributeType type, uint32_t value) -> std::error_code
{
    std::array<uint8_t, 4> data{};
    write_u32_be(data.data(), value);
    return append_attribute(buf, cursor, static_cast<uint16_t>(type), std::span<uint8_t const>{data});
}

[[nodiscard]] auto encode_message_prologue(
    std::span<uint8_t> buf, size_t& cursor, Method method, Class klass, TransactionId const& txid) -> std::error_code
{
    if (auto ec = encode_header({.method = method, .klass = klass, .body_length = 0, .transaction_id = txid}, buf); ec) {
        return ec;
    }
    cursor = HEADER_SIZE;
    return {};
}

void patch_body_length(std::span<uint8_t> buf, size_t cursor) noexcept
{
    auto const body_len = static_cast<uint16_t>(cursor - HEADER_SIZE);
    buf[2] = static_cast<uint8_t>(body_len >> 8);
    buf[3] = static_cast<uint8_t>(body_len & 0xFFU);
}

}  // namespace

auto encode_register_request(
    RegisterRequest const& req, statusbar::crypto::Aes128SivKey const& key, std::span<uint8_t> buf, size_t& written_out)
    -> std::error_code
{
    size_t cursor = 0;
    if (auto ec = encode_message_prologue(buf, cursor, Method::Register, Class::Request, req.transaction_id); ec) {
        return ec;
    }
    if (auto ec = write_session_id(buf, cursor, req.session_id); ec) {
        return ec;
    }
    if (auto ec = write_eui64(buf, cursor, AttributeType::ClientEui64, req.client_eui64); ec) {
        return ec;
    }
    if (auto ec = write_one_byte_attr(buf, cursor, AttributeType::Role, static_cast<uint8_t>(req.role)); ec) {
        return ec;
    }
    patch_body_length(buf, cursor);
    if (auto ec = append_mic(buf, cursor, key); ec) {
        return ec;
    }
    written_out = cursor;
    return {};
}

namespace {

[[nodiscard]] auto write_response_pair_attrs(std::span<uint8_t> buf, size_t& cursor, RegisterResponseSuccess const& resp)
    -> std::error_code
{
    if (!resp.peer_eui64.has_value() || !resp.peer_xor_mapped_address.has_value()) {
        return {};
    }
    if (auto ec = write_eui64(buf, cursor, AttributeType::PeerEui64, *resp.peer_eui64); ec) {
        return ec;
    }
    return append_xor_mapped_address(
        buf,
        cursor,
        static_cast<uint16_t>(AttributeType::PeerXorMappedAddress),
        *resp.peer_xor_mapped_address,
        resp.transaction_id);
}

}  // namespace

auto encode_register_response_success(
    RegisterResponseSuccess const& resp, statusbar::crypto::Aes128SivKey const& key, std::span<uint8_t> buf, size_t& written_out)
    -> std::error_code
{
    size_t cursor = 0;
    if (auto ec = encode_message_prologue(buf, cursor, Method::Register, Class::SuccessResponse, resp.transaction_id); ec) {
        return ec;
    }
    if (auto ec = write_session_id(buf, cursor, resp.session_id); ec) {
        return ec;
    }
    if (auto ec = append_xor_mapped_address(
            buf, cursor, static_cast<uint16_t>(AttributeType::XorMappedAddress), resp.xor_mapped_address, resp.transaction_id);
        ec) {
        return ec;
    }
    if (auto ec = write_one_byte_attr(buf, cursor, AttributeType::SessionState, static_cast<uint8_t>(resp.state)); ec) {
        return ec;
    }
    if (auto ec = write_u32_attr(buf, cursor, AttributeType::RefreshIntervalMs, resp.refresh_interval_ms); ec) {
        return ec;
    }
    if (auto ec = write_response_pair_attrs(buf, cursor, resp); ec) {
        return ec;
    }
    patch_body_length(buf, cursor);
    if (auto ec = append_mic(buf, cursor, key); ec) {
        return ec;
    }
    written_out = cursor;
    return {};
}

auto encode_register_response_error(
    RegisterResponseError const& resp,
    std::string_view reason,
    statusbar::crypto::Aes128SivKey const& key,
    std::span<uint8_t> buf,
    size_t& written_out) -> std::error_code
{
    size_t cursor = 0;
    if (auto ec = encode_message_prologue(buf, cursor, Method::Register, Class::ErrorResponse, resp.transaction_id); ec) {
        return ec;
    }
    if (auto ec = append_error_code(buf, cursor, resp.error_code, reason); ec) {
        return ec;
    }
    patch_body_length(buf, cursor);
    if (auto ec = append_mic(buf, cursor, key); ec) {
        return ec;
    }
    written_out = cursor;
    return {};
}

namespace {

/// Reject a duplicate of an already-seen attribute, then size-check the
/// value. Returns an error_code on either failure; on success leaves
/// `have_flag` true and the caller proceeds with parsing.
[[nodiscard]] auto enforce_unique_with_size(bool& have_flag, std::span<uint8_t const> value, size_t expected_size)
    -> std::error_code
{
    if (have_flag) {
        return make_error_code(StunError::DuplicateAttribute);
    }
    if (value.size() != expected_size) {
        return make_error_code(StunError::InvalidAttributeLength);
    }
    have_flag = true;
    return {};
}

/// Walk a parsed body looking for the attributes that REGISTER requests
/// carry. Treats duplicates as protocol errors. Unknown
/// comprehension-required attributes are rejected; comprehension-optional
/// unknowns are silently ignored.
struct ParsedRegisterRequest
{
    bool have_session_id{false};
    SessionId session_id{};
    bool have_client_eui64{false};
    statusbar::ieee::Eui64 client_eui64{};
    bool have_role{false};
    Role role{Role::Initiator};
};

[[nodiscard]] auto handle_request_attribute(uint16_t attr_type, std::span<uint8_t const> value, ParsedRegisterRequest& parsed)
    -> std::error_code
{
    auto const known = static_cast<AttributeType>(attr_type);
    if (known == AttributeType::SessionId) {
        if (auto ec = enforce_unique_with_size(parsed.have_session_id, value, SESSION_ID_SIZE); ec) {
            return ec;
        }
        statusbar::span_copy(statusbar::make_span(parsed.session_id.bytes), value.first(SESSION_ID_SIZE));
        return {};
    }
    if (known == AttributeType::ClientEui64) {
        if (auto ec = enforce_unique_with_size(parsed.have_client_eui64, value, EUI64_SIZE); ec) {
            return ec;
        }
        auto eui_span = parsed.client_eui64.span();
        for (size_t i = 0; i < EUI64_SIZE; ++i) {
            eui_span[i] = value[i];
        }
        return {};
    }
    if (known == AttributeType::Role) {
        if (auto ec = enforce_unique_with_size(parsed.have_role, value, 1); ec) {
            return ec;
        }
        if (value[0] > static_cast<uint8_t>(Role::Responder)) {
            return make_error_code(StunError::InvalidRole);
        }
        parsed.role = static_cast<Role>(value[0]);
        return {};
    }
    if (is_comprehension_required(attr_type)) {
        return make_error_code(StunError::UnknownComprehensionRequiredAttribute);
    }
    return {};
}

}  // namespace

auto decode_register_request(std::span<uint8_t const> datagram, statusbar::crypto::Aes128SivKey const& key, RegisterRequest& out)
    -> std::error_code
{
    size_t inner_end = 0;
    if (auto ec = verify_mic(datagram, key, inner_end); ec) {
        return ec;
    }

    MessageHeader hdr{};
    if (auto ec = decode_header(datagram, hdr); ec) {
        return ec;
    }
    if (hdr.method != Method::Register) {
        return make_error_code(StunError::UnexpectedMethod);
    }
    if (hdr.klass != Class::Request) {
        return make_error_code(StunError::UnexpectedClass);
    }

    auto body = datagram.subspan(HEADER_SIZE, inner_end - HEADER_SIZE);
    AttributeIterator it{body};
    ParsedRegisterRequest parsed{};

    while (true) {
        uint16_t attr_type = 0;
        std::span<uint8_t const> value{};
        bool done = false;
        if (auto ec = it.next(attr_type, value, done); ec) {
            return ec;
        }
        if (done) {
            break;
        }
        if (auto ec = handle_request_attribute(attr_type, value, parsed); ec) {
            return ec;
        }
    }

    if (!parsed.have_session_id || !parsed.have_client_eui64) {
        return make_error_code(StunError::MissingRequiredAttribute);
    }

    out.transaction_id = hdr.transaction_id;
    out.session_id = parsed.session_id;
    out.client_eui64 = parsed.client_eui64;
    out.role = parsed.have_role ? parsed.role : Role::Initiator;
    return {};
}

namespace {

struct ParsedRegisterResponseSuccess
{
    bool have_session_id{false};
    SessionId session_id{};
    bool have_xor_mapped_address{false};
    statusbar::net::SocketAddress xor_mapped_address{};
    bool have_session_state{false};
    SessionState session_state{SessionState::Waiting};
    bool have_refresh_interval{false};
    uint32_t refresh_interval_ms{default_refresh_interval_ms};
    std::optional<statusbar::ieee::Eui64> peer_eui64{};
    std::optional<statusbar::net::SocketAddress> peer_xor_mapped_address{};
};

[[nodiscard]] auto parse_session_state_byte(uint8_t v, SessionState& out) -> std::error_code
{
    if (v > static_cast<uint8_t>(SessionState::Full)) {
        return make_error_code(StunError::InvalidSessionState);
    }
    out = static_cast<SessionState>(v);
    return {};
}

[[nodiscard]] auto handle_success_attribute(
    uint16_t attr_type, std::span<uint8_t const> value, TransactionId const& txid, ParsedRegisterResponseSuccess& parsed)
    -> std::error_code
{
    auto const known = static_cast<AttributeType>(attr_type);
    if (known == AttributeType::SessionId) {
        if (auto ec = enforce_unique_with_size(parsed.have_session_id, value, SESSION_ID_SIZE); ec) {
            return ec;
        }
        statusbar::span_copy(statusbar::make_span(parsed.session_id.bytes), value.first(SESSION_ID_SIZE));
        return {};
    }
    if (known == AttributeType::XorMappedAddress) {
        if (parsed.have_xor_mapped_address) {
            return make_error_code(StunError::DuplicateAttribute);
        }
        if (auto ec = decode_xor_mapped_address(value, txid, parsed.xor_mapped_address); ec) {
            return ec;
        }
        parsed.have_xor_mapped_address = true;
        return {};
    }
    if (known == AttributeType::SessionState) {
        if (auto ec = enforce_unique_with_size(parsed.have_session_state, value, 1); ec) {
            return ec;
        }
        return parse_session_state_byte(value[0], parsed.session_state);
    }
    if (known == AttributeType::RefreshIntervalMs) {
        if (auto ec = enforce_unique_with_size(parsed.have_refresh_interval, value, 4); ec) {
            return ec;
        }
        parsed.refresh_interval_ms = read_u32_be(value.data());
        return {};
    }
    if (known == AttributeType::PeerEui64) {
        if (parsed.peer_eui64.has_value()) {
            return make_error_code(StunError::DuplicateAttribute);
        }
        if (value.size() != EUI64_SIZE) {
            return make_error_code(StunError::InvalidAttributeLength);
        }
        statusbar::ieee::Eui64 e{};
        auto e_span = e.span();
        for (size_t i = 0; i < EUI64_SIZE; ++i) {
            e_span[i] = value[i];
        }
        parsed.peer_eui64 = e;
        return {};
    }
    if (known == AttributeType::PeerXorMappedAddress) {
        if (parsed.peer_xor_mapped_address.has_value()) {
            return make_error_code(StunError::DuplicateAttribute);
        }
        statusbar::net::SocketAddress addr{};
        if (auto ec = decode_xor_mapped_address(value, txid, addr); ec) {
            return ec;
        }
        parsed.peer_xor_mapped_address = addr;
        return {};
    }
    if (is_comprehension_required(attr_type)) {
        return make_error_code(StunError::UnknownComprehensionRequiredAttribute);
    }
    return {};
}

[[nodiscard]] auto walk_response_body(
    std::span<uint8_t const> body, TransactionId const& txid, ParsedRegisterResponseSuccess& parsed) -> std::error_code
{
    AttributeIterator it{body};
    while (true) {
        uint16_t attr_type = 0;
        std::span<uint8_t const> value{};
        bool done = false;
        if (auto ec = it.next(attr_type, value, done); ec) {
            return ec;
        }
        if (done) {
            return {};
        }
        if (auto ec = handle_success_attribute(attr_type, value, txid, parsed); ec) {
            return ec;
        }
    }
}

[[nodiscard]] auto walk_error_body(std::span<uint8_t const> body, uint16_t& code_out) -> std::error_code
{
    AttributeIterator it{body};
    bool have_code = false;
    while (true) {
        uint16_t attr_type = 0;
        std::span<uint8_t const> value{};
        bool done = false;
        if (auto ec = it.next(attr_type, value, done); ec) {
            return ec;
        }
        if (done) {
            break;
        }
        if (static_cast<AttributeType>(attr_type) == AttributeType::ErrorCode) {
            if (have_code) {
                return make_error_code(StunError::DuplicateAttribute);
            }
            std::span<uint8_t const> reason{};
            if (auto ec = decode_error_code(value, code_out, reason); ec) {
                return ec;
            }
            have_code = true;
            continue;
        }
        if (is_comprehension_required(attr_type)) {
            return make_error_code(StunError::UnknownComprehensionRequiredAttribute);
        }
    }
    if (!have_code) {
        return make_error_code(StunError::MissingRequiredAttribute);
    }
    return {};
}

}  // namespace

auto decode_register_response(
    std::span<uint8_t const> datagram,
    statusbar::crypto::Aes128SivKey const& key,
    bool& is_success_out,
    RegisterResponseSuccess& success_out,
    RegisterResponseError& error_out) -> std::error_code
{
    size_t inner_end = 0;
    if (auto ec = verify_mic(datagram, key, inner_end); ec) {
        return ec;
    }

    MessageHeader hdr{};
    if (auto ec = decode_header(datagram, hdr); ec) {
        return ec;
    }
    if (hdr.method != Method::Register) {
        return make_error_code(StunError::UnexpectedMethod);
    }

    auto const body = datagram.subspan(HEADER_SIZE, inner_end - HEADER_SIZE);
    if (hdr.klass == Class::SuccessResponse) {
        ParsedRegisterResponseSuccess parsed{};
        if (auto ec = walk_response_body(body, hdr.transaction_id, parsed); ec) {
            return ec;
        }
        if (!parsed.have_session_id || !parsed.have_xor_mapped_address || !parsed.have_session_state) {
            return make_error_code(StunError::MissingRequiredAttribute);
        }
        success_out.transaction_id = hdr.transaction_id;
        success_out.session_id = parsed.session_id;
        success_out.xor_mapped_address = parsed.xor_mapped_address;
        success_out.state = parsed.session_state;
        success_out.refresh_interval_ms = parsed.have_refresh_interval ? parsed.refresh_interval_ms : default_refresh_interval_ms;
        success_out.peer_eui64 = parsed.peer_eui64;
        success_out.peer_xor_mapped_address = parsed.peer_xor_mapped_address;
        is_success_out = true;
        return {};
    }
    if (hdr.klass == Class::ErrorResponse) {
        uint16_t code = 0;
        if (auto ec = walk_error_body(body, code); ec) {
            return ec;
        }
        error_out.transaction_id = hdr.transaction_id;
        error_out.error_code = code;
        is_success_out = false;
        return {};
    }
    return make_error_code(StunError::UnexpectedClass);
}

}  // namespace statusbar::stun
