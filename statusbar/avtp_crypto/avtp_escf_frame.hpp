// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avtp_escf_frame.hpp
/// @brief IEEE 1722-2025 Clause 16 ESCF (ECC Signed Control Format): the signed message,
/// the two signature variants, the Listener's timestamp rule, and complete Ethernet +
/// ESCF frames.
///
/// An ESCF AVTPDU carries an encapsulated AVTPDU with a signature over
/// key_id || signing_timestamp || encapsulated_avtpdu (16.3.1, 16.4.1). The sig field
/// selects the variant:
///   ECSSA_DSA_EMSA1_SHA256 (0)  P-256 ECDSA with SHA-256 (16.3): signature_value is
///                               c (octets 0 to 31) then d (32 to 63)
///   ED25519 (1)                 RFC 8032 Ed25519, no prehash, no context (amendment 16.4):
///                               signature_value is R || S
/// The amendment (avb/docs/IEEE_1722_CRYPTO_AMENDMENT.md) names the 64 octets
/// signature_value (B2), adds ED25519 (B3) and states the Listener's discard rules (A3):
/// a signing_timestamp more than 100 ms earlier than, or later than, the Listener's
/// current PTP time, or a signature that fails validation, discards the AVTPDU.
///
/// Wire layout: 14-byte Ethernet header + 84-byte ESCF header + encapsulated_avtpdu.

#pragma once

#include "statusbar/avtp/avtp_escf.hpp"
#include "statusbar/avtp_crypto/avtp_aem_frame.hpp"
#include "statusbar/crypto/keys.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::crypto::avtp {

/// AVTP subtype for ESCF.
inline constexpr uint8_t escf_subtype = statusbar::avtp::AvtpSubtype::escf;

/// ESCF header size (through signature_value, Figure 109).
inline constexpr size_t escf_header_size = statusbar::avtp::EscfPdu::HEADER_LENGTH;  // 84

/// Offset of the encapsulated_avtpdu in an Ethernet + ESCF frame.
inline constexpr size_t escf_payload_offset = ethernet_header_size + escf_header_size;  // 98

/// ESCF sig field values (from statusbar::avtp).
inline constexpr uint8_t escf_sig_ecssa = static_cast<uint8_t>(statusbar::avtp::EscfSigMode::ecssa_dsa_emsa1_sha256);
inline constexpr uint8_t escf_sig_ed25519 = static_cast<uint8_t>(statusbar::avtp::EscfSigMode::ed25519);

/// signature_value size (both variants).
inline constexpr size_t escf_signature_size = statusbar::avtp::EscfPdu::SIGNATURE_LENGTH;  // 64

/// Largest encapsulated_avtpdu (16.2.6).
inline constexpr size_t escf_max_encapsulated_avtpdu_size = 1416;

/// Largest signed message: key_id (8) || signing_timestamp (8) || encapsulated_avtpdu.
inline constexpr size_t escf_max_signed_message_size = 8 + 8 + escf_max_encapsulated_avtpdu_size;

/// The Listener's window on signing_timestamp (16.2.5 as amended, A3): 100 ms either side.
inline constexpr uint64_t escf_signing_timestamp_window_ns = avtp_crypto_timestamp_window_ns;

using EscfSignature = std::array<uint8_t, escf_signature_size>;

/// The signed message (16.3.1, 16.4.1): key_id || signing_timestamp || encapsulated_avtpdu,
/// written into @p out. nullopt when the AVTPDU is over 16.2.6's limit or @p out is too small.
auto escf_signed_message(
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu,
    std::span<uint8_t> out) -> std::optional<std::span<uint8_t const>>;

/// ECSSA_DSA_EMSA1_SHA256 (16.3.2): P-256 ECDSA over SHA-256 of the signed message, c then d.
/// Deterministic (RFC 6979), so a given key and message always give the same signature.
auto escf_ecssa_sign(
    P256PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu) -> std::optional<EscfSignature>;

/// ECSSA_DSA_EMSA1_SHA256 (16.3.3): validate with the public key key_id identifies.
auto escf_ecssa_verify(
    P256PublicKey const& pk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu,
    std::span<uint8_t const, escf_signature_size> signature_value) -> bool;

/// ED25519 (amendment 16.4.2): RFC 8032 5.1.6 over the signed message, R || S.
auto escf_ed25519_sign(
    Ed25519PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu) -> std::optional<EscfSignature>;

/// ED25519 (amendment 16.4.3): RFC 8032 5.1.7, which rejects a signature whose S is not below L.
auto escf_ed25519_verify(
    Ed25519PublicKey const& pk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu,
    std::span<uint8_t const, escf_signature_size> signature_value) -> bool;

/// 16.2.5 as amended (A3): a Listener discards an ESCF AVTPDU whose signing_timestamp
/// differs from its current PTP time by more than 100 ms in either direction (both
/// parties share PTP time). The Listener is told its time; this never asks a clock.
[[nodiscard]] constexpr auto escf_signing_timestamp_is_acceptable(uint64_t const signing_timestamp, uint64_t const now_ptp_ns)
    -> bool
{
    return avtp_crypto_timestamp_is_acceptable(signing_timestamp, now_ptp_ns);
}

/// Parsed fields from a complete Ethernet + ESCF frame. encapsulated_avtpdu is a
/// zero-copy view into the frame, bounded by encapsulated_avtpdu_length.
struct ParsedEscfFrame
{
    statusbar::ieee::Eui48 dest_mac{};
    statusbar::ieee::Eui48 src_mac{};
    statusbar::ieee::doublet_t ethertype{};
    uint8_t subtype{};
    uint8_t sig{};
    uint16_t encapsulated_avtpdu_length{};
    statusbar::ieee::Eui64 key_id{};
    uint64_t signing_timestamp{};
    EscfSignature signature_value{};
    std::span<uint8_t const> encapsulated_avtpdu;
};

/// Build a complete Ethernet + ESCF frame from already-computed fields.
/// @param out Output buffer, at least escf_payload_offset + encapsulated_avtpdu.size().
/// @return The written frame within @p out, or nullopt when it does not fit or the
///         AVTPDU is over 16.2.6's limit.
auto build_escf_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    uint8_t sig,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const, escf_signature_size> signature_value,
    std::span<uint8_t const> encapsulated_avtpdu) -> std::optional<std::span<uint8_t const>>;

/// Sign with the key_id's key and build the frame: ECSSA_DSA_EMSA1_SHA256 for a P-256 key,
/// ED25519 for an Ed25519 key.
auto build_signed_escf_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    P256PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu) -> std::optional<std::span<uint8_t const>>;
auto build_signed_escf_frame(
    std::span<uint8_t> out,
    statusbar::ieee::Eui48 const& dest_mac,
    statusbar::ieee::Eui48 const& src_mac,
    Ed25519PrivateKey const& sk,
    statusbar::ieee::Eui64 const& key_id,
    uint64_t signing_timestamp,
    std::span<uint8_t const> encapsulated_avtpdu) -> std::optional<std::span<uint8_t const>>;

/// Parse a raw Ethernet frame as an ESCF frame. nullopt when it is too short, has the
/// wrong ethertype or subtype, or declares more AVTPDU than the frame holds.
auto parse_escf_frame(std::span<uint8_t const> frame) -> std::optional<ParsedEscfFrame>;

/// Validate a parsed frame's signature with the public key key_id identifies: the sig
/// value must be the key's variant and the signature must verify (16.2.7 as amended).
auto escf_verify_frame(ParsedEscfFrame const& frame, P256PublicKey const& pk) -> bool;
auto escf_verify_frame(ParsedEscfFrame const& frame, Ed25519PublicKey const& pk) -> bool;

/// The outcome of a told-the-time verification, in the order the gates run: the
/// timestamp window (a comparison), the replay guard (a lookup), then the signature
/// (the expensive one, reached only by a frame the first two let through).
enum class EscfVerdict : uint8_t
{
    accepted,
    stale_timestamp,  ///< signing_timestamp over 100 ms from the Listener's PTP time (A3)
    replayed,         ///< signed no later than a frame already accepted from this key_id
    bad_signature,    ///< the sig value is not the key's variant, or the signature fails
};

/// A Listener's replay guard for ESCF: the newest signing_timestamp accepted from each
/// signer (key_id). A frame signed at or before it is a replay, since a signer stamps
/// each frame with its PTP time as it signs. Fixed capacity, no allocation: a signer
/// unseen beyond max_signers evicts the one whose last accepted frame is the oldest.
///
/// This tolerates no reordering: a frame overtaken by a later one from the same key is
/// refused as replayed, which on one Ethernet segment is the dropped frame's fate anyway.
/// A Listener that keeps its own replay state uses escf_verify_frame instead.
class EscfReplayGuard
{
  public:
    static constexpr size_t max_signers = 16;

    /// True when a frame from @p key_id at @p signing_timestamp is a replay of one accepted.
    [[nodiscard]] auto is_replay(statusbar::ieee::Eui64 const& key_id, uint64_t signing_timestamp) const noexcept -> bool;

    /// Record an accepted frame.
    void accept(statusbar::ieee::Eui64 const& key_id, uint64_t signing_timestamp) noexcept;

    /// How many signers are remembered.
    [[nodiscard]] auto signers() const noexcept -> size_t;

  private:
    struct Signer
    {
        statusbar::ieee::Eui64 key_id{};
        uint64_t last_signing_timestamp{};
        bool used{};
    };

    [[nodiscard]] auto find(statusbar::ieee::Eui64 const& key_id) const noexcept -> Signer const*;
    /// The signer's slot, or the slot to take for a new one: unused first, else the oldest.
    [[nodiscard]] auto slot_for(statusbar::ieee::Eui64 const& key_id) noexcept -> Signer&;

    std::array<Signer, max_signers> signers_{};
};

/// Verify a parsed frame as a Listener told its PTP time (16.2.5 and 16.2.7 as amended,
/// A3): the signing_timestamp has to be within 100 ms of @p now_ptp_ns, the frame must
/// not replay one @p guard has accepted, and the signature must verify with the key
/// key_id identifies. The two cheap gates run before the signature; an accepted frame
/// is recorded in @p guard, a refused one leaves it as it was. Nothing here asks a clock.
auto escf_verify_frame_at(ParsedEscfFrame const& frame, P256PublicKey const& pk, uint64_t now_ptp_ns, EscfReplayGuard& guard)
    -> EscfVerdict;
auto escf_verify_frame_at(ParsedEscfFrame const& frame, Ed25519PublicKey const& pk, uint64_t now_ptp_ns, EscfReplayGuard& guard)
    -> EscfVerdict;

}  // namespace statusbar::crypto::avtp
