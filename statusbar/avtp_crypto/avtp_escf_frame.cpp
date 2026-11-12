// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722-2025 Clause 16 ESCF: signed message, ECSSA and Ed25519 variants, frames.

#include "statusbar/avtp_crypto/avtp_escf_frame.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/crypto/25519/ed25519.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"

#include <array>

namespace statusbar::crypto::avtp {

using internal::span_copy;
using statusbar::span_load;
using statusbar::span_store;
using std::span;

namespace {

constexpr size_t signed_prefix_size = 8 + 8;  // key_id || signing_timestamp

// The signed message in a stack buffer (its size, never a span into a value that moves),
// or nullopt when the AVTPDU is over the limit.
struct SignedMessage
{
    std::array<uint8_t, escf_max_signed_message_size> storage{};
    size_t size{0};

    [[nodiscard]] auto view() const -> span<uint8_t const> { return span<uint8_t const>(storage).first(size); }
};

auto signed_message(statusbar::ieee::Eui64 const& key_id, uint64_t const signing_timestamp, span<uint8_t const> avtpdu)
    -> std::optional<SignedMessage>
{
    SignedMessage message{};
    auto const view = escf_signed_message(key_id, signing_timestamp, avtpdu, message.storage);
    if (!view) {
        return std::nullopt;
    }
    message.size = view->size();
    return message;
}

auto to_signature(span<uint8_t const, escf_signature_size> bytes) -> EscfSignature
{
    EscfSignature out{};
    span_copy(out, bytes);
    return out;
}

void write_ethernet_header(span<uint8_t> out, statusbar::ieee::Eui48 const& dest_mac, statusbar::ieee::Eui48 const& src_mac)
{
    span_store(out.subspan(0, 6), dest_mac);
    span_store(out.subspan(6, 6), src_mac);
    span_store(out.subspan(12, 2), statusbar::ieee::doublet_t{avtp_ethertype});
}

auto escf_header(
    uint8_t const sig,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const, escf_signature_size> signature_value,
    size_t const avtpdu_size) -> statusbar::avtp::EscfPdu
{
    statusbar::avtp::EscfPdu header{};
    header.init(sig, key_id);
    header.set_encapsulated_avtpdu_length(static_cast<uint16_t>(avtpdu_size));
    header.set_signing_timestamp(signing_timestamp);
    header.set_signature_value(signature_value);
    return header;
}

// Sign and build for either variant: the signature comes from the caller's signer.
auto build_signed(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t const sig,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    std::optional<EscfSignature> const& signature,
    span<uint8_t const> encapsulated_avtpdu) -> std::optional<span<uint8_t const>>
{
    if (!signature) {
        return std::nullopt;
    }
    return build_escf_frame(out, dest_mac, src_mac, sig, key_id, signing_timestamp, *signature, encapsulated_avtpdu);
}

}  // namespace

auto escf_signed_message(
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu,
    span<uint8_t> out) -> std::optional<span<uint8_t const>>
{
    size_t const size = signed_prefix_size + encapsulated_avtpdu.size();
    if (encapsulated_avtpdu.size() > escf_max_encapsulated_avtpdu_size || out.size() < size) {
        return std::nullopt;
    }
    span_store(out.subspan(0, 8), key_id);
    span_store(out.subspan(8, 8), statusbar::ieee::octlet_t{signing_timestamp});
    span_copy(out.subspan(signed_prefix_size, encapsulated_avtpdu.size()), encapsulated_avtpdu);
    return out.first(size);
}

auto escf_ecssa_sign(
    P256PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu) -> std::optional<EscfSignature>
{
    auto const message = signed_message(key_id, signing_timestamp, encapsulated_avtpdu);
    if (!message) {
        return std::nullopt;
    }
    return to_signature(p256_ecdsa_sign(sk, message->view()).data);  // r || s is c || d
}

auto escf_ecssa_verify(
    P256PublicKey const& pk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu,
    span<uint8_t const, escf_signature_size> signature_value) -> bool
{
    auto const message = signed_message(key_id, signing_timestamp, encapsulated_avtpdu);
    if (!message) {
        return false;
    }
    P256EcdsaSignature signature{};
    span_copy(signature.data, signature_value);
    return p256_ecdsa_verify(pk, message->view(), signature);
}

auto escf_ed25519_sign(
    Ed25519PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu) -> std::optional<EscfSignature>
{
    auto const message = signed_message(key_id, signing_timestamp, encapsulated_avtpdu);
    if (!message) {
        return std::nullopt;
    }
    return to_signature(ed25519_sign(sk, message->view()).data);
}

auto escf_ed25519_verify(
    Ed25519PublicKey const& pk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu,
    span<uint8_t const, escf_signature_size> signature_value) -> bool
{
    auto const message = signed_message(key_id, signing_timestamp, encapsulated_avtpdu);
    if (!message) {
        return false;
    }
    Ed25519Signature signature{};
    span_copy(signature.data, signature_value);
    return ed25519_verify(pk, message->view(), signature);
}

auto build_escf_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t const sig,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const, escf_signature_size> signature_value,
    span<uint8_t const> encapsulated_avtpdu) -> std::optional<span<uint8_t const>>
{
    size_t const frame_size = escf_payload_offset + encapsulated_avtpdu.size();
    if (encapsulated_avtpdu.size() > escf_max_encapsulated_avtpdu_size || out.size() < frame_size) {
        return std::nullopt;
    }
    write_ethernet_header(out, dest_mac, src_mac);
    span_store(
        out.subspan(ethernet_header_size, escf_header_size),
        escf_header(sig, key_id, signing_timestamp, signature_value, encapsulated_avtpdu.size()));
    span_copy(out.subspan(escf_payload_offset, encapsulated_avtpdu.size()), encapsulated_avtpdu);
    return out.first(frame_size);
}

auto build_signed_escf_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    P256PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu) -> std::optional<span<uint8_t const>>
{
    auto const signature = escf_ecssa_sign(sk, key_id, signing_timestamp, encapsulated_avtpdu);
    return build_signed(out, dest_mac, src_mac, escf_sig_ecssa, key_id, signing_timestamp, signature, encapsulated_avtpdu);
}

auto build_signed_escf_frame(
    span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    Ed25519PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t const signing_timestamp,
    span<uint8_t const> encapsulated_avtpdu) -> std::optional<span<uint8_t const>>
{
    auto const signature = escf_ed25519_sign(sk, key_id, signing_timestamp, encapsulated_avtpdu);
    return build_signed(out, dest_mac, src_mac, escf_sig_ed25519, key_id, signing_timestamp, signature, encapsulated_avtpdu);
}

auto parse_escf_frame(span<uint8_t const> frame) -> std::optional<ParsedEscfFrame>
{  // NOLINT(statusbar-block-statements) frame parse: Ethernet check, ESCF header, one copy per field, bounds
    if (frame.size() < escf_payload_offset) {
        return std::nullopt;
    }
    statusbar::ieee::doublet_t ethertype{};
    span_load(ethertype, frame.subspan(12, 2));
    if (ethertype != avtp_ethertype) {
        return std::nullopt;
    }
    auto const header = statusbar::avtp::escf_parse_header(frame.subspan(ethernet_header_size));
    if (!header) {
        return std::nullopt;
    }

    ParsedEscfFrame result{};
    span_load(result.dest_mac, frame.subspan(0, 6));
    span_load(result.src_mac, frame.subspan(6, 6));
    result.ethertype = ethertype;
    result.subtype = escf_subtype;
    result.sig = header->sig();
    result.encapsulated_avtpdu_length = header->encapsulated_avtpdu_length();
    result.key_id = header->key_id();
    result.signing_timestamp = header->signing_timestamp();
    result.signature_value = header->signature_value();

    // Bound the AVTPDU by the declared length and reject a length past the frame
    // (a padded frame must not sign-check its padding; a consumer must not read past it).
    if (escf_payload_offset + result.encapsulated_avtpdu_length > frame.size()) {
        return std::nullopt;
    }
    result.encapsulated_avtpdu = frame.subspan(escf_payload_offset, result.encapsulated_avtpdu_length);
    return result;
}

auto escf_verify_frame(ParsedEscfFrame const& frame, P256PublicKey const& pk) -> bool
{
    return frame.sig == escf_sig_ecssa &&
        escf_ecssa_verify(pk, frame.key_id, frame.signing_timestamp, frame.encapsulated_avtpdu, frame.signature_value);
}

auto escf_verify_frame(ParsedEscfFrame const& frame, Ed25519PublicKey const& pk) -> bool
{
    return frame.sig == escf_sig_ed25519 &&
        escf_ed25519_verify(pk, frame.key_id, frame.signing_timestamp, frame.encapsulated_avtpdu, frame.signature_value);
}

auto EscfReplayGuard::find(statusbar::ieee::Eui64 const& key_id) const noexcept -> Signer const*
{
    for (auto const& signer : signers_) {
        if (signer.used && signer.key_id == key_id) {
            return &signer;
        }
    }
    return nullptr;
}

auto EscfReplayGuard::slot_for(statusbar::ieee::Eui64 const& key_id) noexcept -> Signer&
{
    Signer* oldest = &signers_[0];
    for (auto& signer : signers_) {
        if (!signer.used || signer.key_id == key_id) {
            return signer;
        }
        if (signer.last_signing_timestamp < oldest->last_signing_timestamp) {
            oldest = &signer;
        }
    }
    return *oldest;
}

auto EscfReplayGuard::is_replay(statusbar::ieee::Eui64 const& key_id, uint64_t const signing_timestamp) const noexcept -> bool
{
    auto const* signer = find(key_id);
    return signer != nullptr && signing_timestamp <= signer->last_signing_timestamp;
}

void EscfReplayGuard::accept(statusbar::ieee::Eui64 const& key_id, uint64_t const signing_timestamp) noexcept
{
    auto& signer = slot_for(key_id);
    signer = Signer{.key_id = key_id, .last_signing_timestamp = signing_timestamp, .used = true};
}

auto EscfReplayGuard::signers() const noexcept -> size_t
{
    size_t n = 0;
    for (auto const& signer : signers_) {
        n += signer.used ? 1 : 0;
    }
    return n;
}

namespace {

// The gates in order; Verify is the variant's signature check over the parsed frame.
template <typename Verify>
auto verify_at(ParsedEscfFrame const& frame, uint64_t const now_ptp_ns, EscfReplayGuard& guard, Verify const& verify) -> EscfVerdict
{
    if (!escf_signing_timestamp_is_acceptable(frame.signing_timestamp, now_ptp_ns)) {
        return EscfVerdict::stale_timestamp;
    }
    if (guard.is_replay(frame.key_id, frame.signing_timestamp)) {
        return EscfVerdict::replayed;
    }
    if (!verify()) {
        return EscfVerdict::bad_signature;
    }
    guard.accept(frame.key_id, frame.signing_timestamp);
    return EscfVerdict::accepted;
}

}  // namespace

auto escf_verify_frame_at(ParsedEscfFrame const& frame, P256PublicKey const& pk, uint64_t const now_ptp_ns, EscfReplayGuard& guard)
    -> EscfVerdict
{
    return verify_at(frame, now_ptp_ns, guard, [&] { return escf_verify_frame(frame, pk); });
}

auto escf_verify_frame_at(
    ParsedEscfFrame const& frame, Ed25519PublicKey const& pk, uint64_t const now_ptp_ns, EscfReplayGuard& guard) -> EscfVerdict
{
    return verify_at(frame, now_ptp_ns, guard, [&] { return escf_verify_frame(frame, pk); });
}

}  // namespace statusbar::crypto::avtp
