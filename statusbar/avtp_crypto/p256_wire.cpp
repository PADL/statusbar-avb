// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// IEEE 1722.1-2021 ECC_PUBLIC_256 and ECC_PRIVATE_256 wire format implementation.

#include "statusbar/avtp_crypto/p256_wire.hpp"

#include "statusbar/crypto/p256/p256.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/p256/p256_wire_constants.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"

namespace statusbar::crypto::avtp {

using internal::span_copy;
using std::span;

/// Fill the 6 common curve parameter fields in an ECC wire struct.
template <typename T>
static void fill_p256_curve_params(T& wire)
{
    span_copy(wire.field_size, span(p256_field_prime_bytes));
    span_copy(wire.semimajor, span(p256_curve_a_bytes));
    span_copy(wire.semiminor, span(p256_curve_b_bytes));
    span_copy(wire.prime_divisor, span(p256_group_order_bytes));
    span_copy(wire.generator_x, span(p256_generator_x_bytes));
    span_copy(wire.generator_y, span(p256_generator_y_bytes));
}

/// Validate that a wire struct's 6 curve domain parameters match the canonical P-256 values.
/// Rejects wire data that claims to be a different curve, preventing curve-substitution attacks.
template <typename T>
static auto validate_p256_curve_params(T const& wire) -> bool
{
    return wire.field_size == std::to_array(p256_field_prime_bytes) && wire.semimajor == std::to_array(p256_curve_a_bytes) &&
        wire.semiminor == std::to_array(p256_curve_b_bytes) && wire.prime_divisor == std::to_array(p256_group_order_bytes) &&
        wire.generator_x == std::to_array(p256_generator_x_bytes) && wire.generator_y == std::to_array(p256_generator_y_bytes);
}

//
// ECC_PUBLIC_256 wire format (IEEE 1722.1-2021 Table 7-182)
//

/// Serialize the first 272 bytes (the signed portion) of an EccPublic256Wire to a buffer.
static void serialize_ecc_public_256_signed_data(EccPublic256Wire const& wire, span<uint8_t, ecc_public_256_signed_data_size> out)
{
    size_t off = 0;
    span_copy(out.subspan(off, key_id_size), wire.related_key_id.data);
    off += key_id_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.field_size);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.semimajor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.semiminor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.prime_divisor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.generator_x);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.generator_y);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.public_x);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.public_y);
    off += p256_field_element_size;
    span_copy(out.subspan(off, key_id_size), wire.signature_key_id.data);
}

auto build_ecc_public_256_wire(
    KeyId const& related_key_id, P256PublicKey const& public_key, KeyId const& signature_key_id, P256PrivateKey const& signing_key)
    -> EccPublic256Wire
{
    EccPublic256Wire wire;
    wire.related_key_id = related_key_id;
    fill_p256_curve_params(wire);
    span_copy(wire.public_x, span<uint8_t const>(public_key.data).first<p256_field_element_size>());
    span_copy(wire.public_y, span<uint8_t const>(public_key.data).last<p256_field_element_size>());
    wire.signature_key_id = signature_key_id;

    // Sign bytes [0..271] with ECDSA/SHA-256
    std::array<uint8_t, ecc_public_256_signed_data_size> signed_data{};
    serialize_ecc_public_256_signed_data(wire, signed_data);
    auto sig = p256_ecdsa_sign(signing_key, signed_data);
    span_copy(wire.ecdsa_signature_c, span<uint8_t const>(sig.data).first<p256_field_element_size>());
    span_copy(wire.ecdsa_signature_d, span<uint8_t const>(sig.data).last<p256_field_element_size>());

    return wire;
}

auto verify_ecc_public_256_wire(EccPublic256Wire const& wire, P256PublicKey const& signer_public_key) -> bool
{
    // Reconstruct the 272-byte signed data
    std::array<uint8_t, ecc_public_256_signed_data_size> signed_data{};
    serialize_ecc_public_256_signed_data(wire, signed_data);

    // Reconstruct ECDSA signature
    P256EcdsaSignature sig{};
    span_copy(span(sig.data).first<p256_field_element_size>(), wire.ecdsa_signature_c);
    span_copy(span(sig.data).last<p256_field_element_size>(), wire.ecdsa_signature_d);

    return p256_ecdsa_verify(signer_public_key, signed_data, sig);
}

auto extract_p256_public_key(EccPublic256Wire const& wire) -> std::optional<P256PublicKey>
{
    // Validate curve domain parameters match canonical P-256 values
    if (!validate_p256_curve_params(wire)) {
        return std::nullopt;
    }

    P256PublicKey pk{};
    span_copy(span(pk.data).first<p256_field_element_size>(), wire.public_x);
    span_copy(span(pk.data).last<p256_field_element_size>(), wire.public_y);
    return pk;
}

auto serialize_ecc_public_256_wire(EccPublic256Wire const& wire, span<uint8_t, ecc_public_256_wire_size> out) -> void
{
    size_t off = 0;
    span_copy(out.subspan(off, key_id_size), wire.related_key_id.data);
    off += key_id_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.field_size);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.semimajor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.semiminor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.prime_divisor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.generator_x);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.generator_y);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.public_x);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.public_y);
    off += p256_field_element_size;
    span_copy(out.subspan(off, key_id_size), wire.signature_key_id.data);
    off += key_id_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.ecdsa_signature_c);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.ecdsa_signature_d);
}

auto deserialize_ecc_public_256_wire(span<uint8_t const, ecc_public_256_wire_size> in) -> EccPublic256Wire
{
    EccPublic256Wire wire;
    size_t off = 0;
    span_copy(wire.related_key_id.data, in.subspan(off, key_id_size));
    off += key_id_size;
    span_copy(wire.field_size, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.semimajor, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.semiminor, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.prime_divisor, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.generator_x, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.generator_y, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.public_x, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.public_y, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.signature_key_id.data, in.subspan(off, key_id_size));
    off += key_id_size;
    span_copy(wire.ecdsa_signature_c, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.ecdsa_signature_d, in.subspan(off, p256_field_element_size));
    return wire;
}

//
// ECC_PRIVATE_256 wire format (IEEE 1722.1-2021 Table 7-183)
//

auto build_ecc_private_256_wire(KeyId const& related_key_id, P256PrivateKey const& private_key) -> EccPrivate256Wire
{
    EccPrivate256Wire wire;
    wire.related_key_id = related_key_id;
    fill_p256_curve_params(wire);
    span_copy(wire.private_scalar, private_key.data);
    return wire;
}

auto extract_p256_private_key(EccPrivate256Wire const& wire) -> std::optional<P256PrivateKey>
{
    // Validate curve domain parameters match canonical P-256 values
    if (!validate_p256_curve_params(wire)) {
        return std::nullopt;
    }

    // Validate scalar in [1, n-1] and derive public key
    return p256_keypair_from_scalar(wire.private_scalar);
}

auto serialize_ecc_private_256_wire(EccPrivate256Wire const& wire, span<uint8_t, ecc_private_256_wire_size> out) -> void
{
    size_t off = 0;
    span_copy(out.subspan(off, key_id_size), wire.related_key_id.data);
    off += key_id_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.field_size);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.semimajor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.semiminor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.prime_divisor);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.generator_x);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.generator_y);
    off += p256_field_element_size;
    span_copy(out.subspan(off, p256_field_element_size), wire.private_scalar);
}

auto deserialize_ecc_private_256_wire(span<uint8_t const, ecc_private_256_wire_size> in) -> EccPrivate256Wire
{
    EccPrivate256Wire wire;
    size_t off = 0;
    span_copy(wire.related_key_id.data, in.subspan(off, key_id_size));
    off += key_id_size;
    span_copy(wire.field_size, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.semimajor, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.semiminor, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.prime_divisor, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.generator_x, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.generator_y, in.subspan(off, p256_field_element_size));
    off += p256_field_element_size;
    span_copy(wire.private_scalar, in.subspan(off, p256_field_element_size));
    return wire;
}

}  // namespace statusbar::crypto::avtp
