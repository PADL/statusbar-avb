// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722.1 AECP AEM frame builder/parser implementation.

#include "statusbar/avtp_crypto/avtp_aem_frame.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"

namespace statusbar::crypto::avtp {

using internal::span_copy;
using statusbar::span_load;
using statusbar::span_store;
using std::span;

// Generic AEM frame builder

auto build_aem_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t message_type,
    uint8_t status,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    uint16_t command_code,
    span<uint8_t const> payload) -> std::optional<span<uint8_t const>>
{
    size_t const frame_size = aem_payload_offset + payload.size();
    if (out.size() < frame_size) {
        return std::nullopt;
    }

    auto const cdl = static_cast<uint16_t>(aem_data_length + payload.size());

    // Ethernet header (14 bytes)
    span_store(out.subspan(0, 6), dest_mac);
    span_store(out.subspan(6, 6), src_mac);
    span_store(out.subspan(12, 2), statusbar::ieee::doublet_t{avtp_ethertype});

    // AemDu header (24 bytes)
    out[14] = aecp_subtype;
    out[15] = static_cast<uint8_t>(message_type & 0x0F);
    out[16] = static_cast<uint8_t>(((status & 0x1F) << 3) | ((cdl >> 8) & 0x07));
    out[17] = static_cast<uint8_t>(cdl & 0xFF);
    span_store(out.subspan(18, 8), target_entity_id);
    span_store(out.subspan(26, 8), controller_entity_id);
    span_store(out.subspan(34, 2), statusbar::ieee::doublet_t{sequence_id});
    span_store(out.subspan(36, 2), statusbar::ieee::doublet_t{static_cast<uint16_t>(command_code & 0x7FFF)});

    // Payload
    if (!payload.empty()) {
        span_copy(out.subspan(aem_payload_offset, payload.size()), payload);
    }

    return span<uint8_t const>(out.data(), frame_size);
}

// AUTH_GET_NONCE frame builders

auto build_auth_get_nonce_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    Nonce const& controller_nonce) -> std::optional<span<uint8_t const>>
{
    return build_aem_frame(
        out,
        dest_mac,
        src_mac,
        aecp_message_type_aem_command,
        aem_status_success,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        aem_cmd_auth_get_nonce,
        controller_nonce.data);
}

auto build_auth_get_nonce_response_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    uint8_t status,
    Nonce const& controller_nonce,
    Nonce const& target_nonce) -> std::optional<span<uint8_t const>>
{
    // Assemble 16-byte payload: controller_nonce(8) + target_nonce(8)
    std::array<uint8_t, 2 * nonce_size> payload{};
    span_copy(span<uint8_t>(payload).subspan(0, nonce_size), controller_nonce.data);
    span_copy(span<uint8_t>(payload).subspan(nonce_size, nonce_size), target_nonce.data);

    return build_aem_frame(
        out,
        dest_mac,
        src_mac,
        aecp_message_type_aem_response,
        status,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        aem_cmd_auth_get_nonce,
        payload);
}

// AUTH_ADD_KEY_NONCE frame builders

auto build_auth_add_key_nonce_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    span<uint8_t const> ecies_ciphertext) -> std::optional<span<uint8_t const>>
{
    return build_aem_frame(
        out,
        dest_mac,
        src_mac,
        aecp_message_type_aem_command,
        aem_status_success,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        aem_cmd_auth_add_key_nonce,
        ecies_ciphertext);
}

auto build_auth_add_key_nonce_response_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    statusbar::ieee::Eui64 const& target_entity_id,
    statusbar::ieee::Eui64 const& controller_entity_id,
    uint16_t sequence_id,
    uint8_t status,
    Nonce const& controller_nonce,
    Nonce const& target_nonce,
    KeyId const& key_id) -> std::optional<span<uint8_t const>>
{
    // Assemble 24-byte payload: controller_nonce(8) + target_nonce(8) + key_id(8)
    std::array<uint8_t, (2 * nonce_size) + key_id_size> payload{};
    span_copy(span<uint8_t>(payload).subspan(0, nonce_size), controller_nonce.data);
    span_copy(span<uint8_t>(payload).subspan(nonce_size, nonce_size), target_nonce.data);
    span_copy(span<uint8_t>(payload).subspan((2 * nonce_size), key_id_size), key_id.data);

    return build_aem_frame(
        out,
        dest_mac,
        src_mac,
        aecp_message_type_aem_response,
        status,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        aem_cmd_auth_add_key_nonce,
        payload);
}

// Frame parser

auto parse_aem_frame(span<uint8_t const> frame) -> std::optional<ParsedAemFrame>
{
    if (frame.size() < aem_payload_offset) {
        return std::nullopt;
    }

    // Ethernet header
    statusbar::ieee::doublet_t ethertype{};
    span_load(ethertype, frame.subspan(12, 2));
    if (ethertype != avtp_ethertype) {
        return std::nullopt;
    }

    // AemDu header
    auto const subtype = frame[14];
    if (subtype != aecp_subtype) {
        return std::nullopt;
    }

    ParsedAemFrame result{};
    span_load(result.dest_mac, frame.subspan(0, 6));
    span_load(result.src_mac, frame.subspan(6, 6));
    result.ethertype = ethertype;
    result.subtype = subtype;
    result.message_type = static_cast<uint8_t>(frame[15] & 0x0F);
    result.status = static_cast<uint8_t>((frame[16] >> 3) & 0x1F);
    result.control_data_length = static_cast<uint16_t>(((frame[16] & 0x07) << 8) | frame[17]);
    span_load(result.target_entity_id, frame.subspan(18, 8));
    span_load(result.controller_entity_id, frame.subspan(26, 8));
    statusbar::ieee::doublet_t sequence_id{};
    span_load(sequence_id, frame.subspan(34, 2));
    result.sequence_id = sequence_id;
    statusbar::ieee::doublet_t command_code{};
    span_load(command_code, frame.subspan(36, 2));
    result.command_code = static_cast<uint16_t>(command_code.get() & 0x7FFF);

    // Bound the payload by the declared control_data_length, not the frame extent
    // (which may include Ethernet padding or trailing bytes). control_data_length
    // counts the octets after target_entity_id, i.e. aem_data_length + payload; the
    // inverse recovers the payload length. Reject a CDL that under-runs the AEM
    // common header or over-runs the received frame, so a consumer trusting the
    // declared length can never read past the buffer.
    if (result.control_data_length < aem_data_length) {
        return std::nullopt;
    }
    size_t const payload_len = result.control_data_length - aem_data_length;
    if (aem_payload_offset + payload_len > frame.size()) {
        return std::nullopt;
    }
    result.payload = frame.subspan(aem_payload_offset, payload_len);

    return result;
}

// Payload extraction helpers

auto extract_auth_get_nonce(ParsedAemFrame const& frame) -> std::optional<Nonce>
{
    if (frame.payload.size() < nonce_size) {
        return std::nullopt;
    }
    Nonce nonce{};
    span_copy(nonce.data, frame.payload.subspan(0, nonce_size));
    return nonce;
}

auto extract_auth_get_nonce_response(ParsedAemFrame const& frame) -> std::optional<AuthGetNonceResponsePayload>
{
    if (frame.payload.size() < 2 * nonce_size) {
        return std::nullopt;
    }
    AuthGetNonceResponsePayload result{};
    span_copy(result.controller_nonce.data, frame.payload.subspan(0, nonce_size));
    span_copy(result.target_nonce.data, frame.payload.subspan(nonce_size, nonce_size));
    return result;
}

auto extract_auth_add_key_nonce_ciphertext(ParsedAemFrame const& frame) -> span<uint8_t const>
{
    return frame.payload;
}

auto extract_auth_add_key_nonce_response(ParsedAemFrame const& frame) -> std::optional<AuthAddKeyNonceResponsePayload>
{
    if (frame.payload.size() < (2 * nonce_size) + key_id_size) {
        return std::nullopt;
    }
    AuthAddKeyNonceResponsePayload result{};
    span_copy(result.controller_nonce.data, frame.payload.subspan(0, nonce_size));
    span_copy(result.target_nonce.data, frame.payload.subspan(nonce_size, nonce_size));
    span_copy(result.key_id.data, frame.payload.subspan((2 * nonce_size), key_id_size));
    return result;
}

// EECF frame builder (IEEE 1722-2016 Clause 17)

auto build_eecf_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t enc,
    statusbar::ieee::Eui64 const& key_id,
    span<uint8_t const> encrypted_payload) -> std::optional<span<uint8_t const>>
{
    size_t const frame_size = eecf_payload_offset + encrypted_payload.size();
    if (out.size() < frame_size) {
        return std::nullopt;
    }

    // Ethernet header (14 bytes)
    span_store(out.subspan(0, 6), dest_mac);
    span_store(out.subspan(6, 6), src_mac);
    span_store(out.subspan(12, 2), statusbar::ieee::doublet_t{avtp_ethertype});

    // EECF header (12 bytes) - use avtp::EecfPdu struct
    statusbar::avtp::EecfPdu eecf_hdr{};
    eecf_hdr.init(enc, key_id);
    eecf_hdr.set_encrypted_payload_length(static_cast<uint16_t>(encrypted_payload.size()));
    span_store(out.subspan(ethernet_header_size, statusbar::avtp::EecfPdu::HEADER_LENGTH), eecf_hdr);

    // Encrypted payload
    if (!encrypted_payload.empty()) {
        span_copy(out.subspan(eecf_payload_offset, encrypted_payload.size()), encrypted_payload);
    }

    return span<uint8_t const>(out.data(), frame_size);
}

// EECF frame parser

auto parse_eecf_frame(span<uint8_t const> frame) -> std::optional<ParsedEecfFrame>
{
    if (frame.size() < eecf_payload_offset) {
        return std::nullopt;
    }

    // Ethernet header
    statusbar::ieee::doublet_t ethertype{};
    span_load(ethertype, frame.subspan(12, 2));
    if (ethertype != avtp_ethertype) {
        return std::nullopt;
    }

    // Parse EECF header using avtp::EecfPdu
    auto eecf_opt = statusbar::avtp::eecf_parse_header(frame.subspan(ethernet_header_size));
    if (!eecf_opt) {
        return std::nullopt;
    }

    ParsedEecfFrame result{};
    span_load(result.dest_mac, frame.subspan(0, 6));
    span_load(result.src_mac, frame.subspan(6, 6));
    result.ethertype = ethertype;
    result.subtype = eecf_subtype;
    result.enc = eecf_opt->enc();
    result.encrypted_payload_length = eecf_opt->encrypted_payload_length();
    result.key_id = eecf_opt->key_id();

    // Bound the ciphertext by the declared encrypted_payload_length, not the frame
    // extent, and reject a length that over-runs the frame -- otherwise padding is
    // decrypted as ciphertext, or a consumer trusting the declared length reads
    // past the buffer.
    if (eecf_payload_offset + result.encrypted_payload_length > frame.size()) {
        return std::nullopt;
    }
    result.encrypted_payload = frame.subspan(eecf_payload_offset, result.encrypted_payload_length);

    return result;
}

}  // namespace statusbar::crypto::avtp
