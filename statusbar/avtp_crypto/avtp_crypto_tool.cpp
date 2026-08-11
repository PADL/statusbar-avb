// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Command-line interface to statusbar_crypto algorithms.
//
// Usage: avtp_crypto_cli <command> [hex_args...]
//
// All binary data is hex-encoded on input and output.
// Empty strings represent zero-length data.
// Output is space-separated hex strings on a single line.

#include "statusbar/crypto/25519/ed25519.hpp"
#include "statusbar/crypto/25519/x25519.hpp"
#include "statusbar/crypto/aes/aes128_hw.hpp"
#include "statusbar/crypto/aes/aes256_hw.hpp"
#include "statusbar/crypto/aes_gcm_siv/aes128_gcm_siv.hpp"
#include "statusbar/crypto/aes_gcm_siv/aes256_gcm_siv.hpp"
#include "statusbar/crypto/aes_siv/aes128_siv.hpp"
#include "statusbar/crypto/aes_siv/aes256_siv.hpp"
#include "statusbar/crypto/ecies/ecies.hpp"
#include "statusbar/crypto/ecies/x25519_ecies.hpp"
#include "statusbar/crypto/hkdf/hkdf.hpp"
#include "statusbar/crypto/p256/p256_ecdh.hpp"
#include "statusbar/crypto/p256/p256_ecdsa.hpp"
#include "statusbar/crypto/pkcs8/pkcs8_ed25519.hpp"
#include "statusbar/crypto/pkcs8/pkcs8_p256.hpp"
#include "statusbar/crypto/polyval/polyval_hw.hpp"
#include "statusbar/crypto/sha/sha256_hw.hpp"
#include "statusbar/crypto/sha/sha512_hw.hpp"
#include "statusbar/crypto/util/crypto_backend.hpp"
#include "statusbar/crypto/util/crypto_util_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <print>
#include <ranges>
#include <string_view>
#include <vector>

using namespace statusbar::crypto;
using internal::can_span_copy;
using internal::span_copy;
using std::span;

// RAII guard to securely zero heap-allocated key material on scope exit.
// Note: std::exit() bypasses destructors, but that terminates the process
// immediately anyway, so memory is reclaimed by the OS.
struct SecureVectorGuard
{
    std::vector<uint8_t>& v;
    ~SecureVectorGuard() { internal::secure_zero(span<uint8_t>(v)); }
};

//
// Hex encoding / decoding
//

/// Convert a single hex character to its 4-bit value, or -1 on invalid input.
static constexpr auto hex_digit_value(char c) noexcept -> int
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static auto hex_decode(char const* s) -> std::vector<uint8_t>
{
    std::vector<uint8_t> out;
    size_t const len = std::strlen(s);
    if (len % 2 != 0) {
        std::println(stderr, "error: odd-length hex string: {}", s);
        std::exit(1);
    }
    out.reserve(len / 2);
    for (size_t i = 0; i < len; i += 2) {
        int const hi = hex_digit_value(s[i]);
        int const lo = hex_digit_value(s[i + 1]);
        if (hi < 0 || lo < 0) {
            std::println(stderr, "error: invalid hex at position {}: {}", i, s);
            std::exit(1);
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

template <std::ranges::range T>
    requires std::same_as<std::ranges::range_value_t<T>, uint8_t>
static void hex_print(T const& container)
{
    for (auto b : container) {
        std::print("{:02x}", b);
    }
}

//
// Argument helpers
//

static void usage()
{
    std::println(
        stderr,
        "Usage: avtp_crypto_cli <command> [hex_args...]\n"
        "\n"
        "Commands:\n"
        "  aes128_encrypt       <key> <plaintext>           -> ciphertext\n"
        "  aes128_decrypt       <key> <ciphertext>          -> plaintext\n"
        "  aes128_cmac          <key> <message>             -> tag\n"
        "  aes128_encrypt_hw    <key> <plaintext>           -> ciphertext  (hw accel)\n"
        "  aes128_decrypt_hw    <key> <ciphertext>          -> plaintext   (hw accel)\n"
        "  aes128_cmac_hw       <key> <message>             -> tag         (hw accel)\n"
        "  aes256_encrypt       <key> <plaintext>           -> ciphertext\n"
        "  aes256_decrypt       <key> <ciphertext>          -> plaintext\n"
        "  aes256_cmac          <key> <message>             -> tag\n"
        "  aes256_encrypt_hw    <key> <plaintext>           -> ciphertext  (hw accel)\n"
        "  aes256_decrypt_hw    <key> <ciphertext>          -> plaintext   (hw accel)\n"
        "  aes256_cmac_hw       <key> <message>             -> tag         (hw accel)\n"
        "  aes128_siv_encrypt   <key> <aad> <plaintext>     -> siv ciphertext\n"
        "  aes128_siv_decrypt   <key> <siv> <aad> <ct>      -> ok|fail plaintext\n"
        "  aes256_siv_encrypt   <key> <aad> <plaintext>     -> siv ciphertext\n"
        "  aes256_siv_decrypt   <key> <siv> <aad> <ct>      -> ok|fail plaintext\n"
        "  aes128_gcm_siv_encrypt <key> <nonce> <aad> <pt>  -> tag ciphertext\n"
        "  aes128_gcm_siv_decrypt <key> <nonce> <tag> <aad> <ct> -> ok|fail pt\n"
        "  aes256_gcm_siv_encrypt <key> <nonce> <aad> <pt>  -> tag ciphertext\n"
        "  aes256_gcm_siv_decrypt <key> <nonce> <tag> <aad> <ct> -> ok|fail pt\n"
        "  sha256               <message>                   -> digest\n"
        "  sha256_hw            <message>                   -> digest      (hw accel)\n"
        "  sha512               <message>                   -> digest\n"
        "  sha512_hw            <message>                   -> digest      (hw accel)\n"
        "  sha256_hmac          <key> <message>             -> mac\n"
        "  sha256_hmac_hw       <key> <message>             -> mac         (hw accel)\n"
        "  hkdf_extract         <salt> <ikm>                -> prk\n"
        "  hkdf_expand          <prk> <info> <length>       -> okm\n"
        "  hkdf                 <salt> <ikm> <info> <length> -> okm\n"
        "  ed25519_pubkey       <seed>                      -> public_key\n"
        "  ed25519_sign         <seed> <message>            -> signature\n"
        "  ed25519_verify       <pubkey> <message> <sig>    -> ok|fail\n"
        "  x25519_pubkey        <seed>                      -> public_key\n"
        "  x25519               <private_key> <public_key>  -> shared_secret\n"
        "  ed25519_to_x25519_pk <ed25519_pubkey>            -> x25519_pubkey\n"
        "  ed25519_to_x25519_sk <seed>                      -> x25519_privkey x25519_pubkey\n"
        "  p256_ecdsa_pubkey    <seed>                      -> public_key (64 bytes x||y)\n"
        "  p256_ecdsa_sign      <seed> <message>            -> signature (64 bytes r||s)\n"
        "  p256_ecdsa_verify    <pubkey> <message> <sig>    -> ok|fail\n"
        "  p256_ecdh            <seed> <peer_pubkey>        -> shared_secret (32 bytes)\n"
        "  ecies_encrypt        <pubkey> <entropy> <pt>     -> V||C||T (hex)\n"
        "  ecies_decrypt        <seed> <ciphertext>         -> ok|fail plaintext\n"
        "  x25519_ecies_encrypt <pubkey> <entropy> <pt>     -> V||C||T (hex)\n"
        "  x25519_ecies_decrypt <seed> <ciphertext>         -> ok|fail plaintext\n"
        "  backend                                          -> resolved hw/sw backend per primitive\n"
        "\n"
        "All arguments are hex-encoded. Empty string \"\" = zero-length data.");
}

static void require_args(int argc, int needed, char const* cmd)
{
    // argc includes program name and command, so actual args = argc - 2
    if (argc - 2 < needed) {
        std::println(stderr, "error: {} requires {} argument(s), got {}", cmd, needed, argc - 2);
        std::exit(1);
    }
}

// Validate size and copy a dynamically-sized vector into a fixed-size array.
// Uses can_span_copy for validation and span_copy for the copy.
template <size_t N>
static void require_copy(std::array<uint8_t, N>& dest, span<uint8_t const> src, char const* name)
{
    if (!can_span_copy(dest, src)) {
        std::println(stderr, "error: {} must be {} bytes, got {}", name, N, src.size());
        std::exit(1);
    }
    span_copy(dest, src);
}

//
// Command handlers
//

static void cmd_aes128_encrypt(int argc, char** argv)
{
    require_args(argc, 2, "aes128_encrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes128_expand_key_hw(key);

    std::array<uint8_t, aes128_block_size> buf;
    require_copy(buf, block, "plaintext");
    aes128_encrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes128_decrypt(int argc, char** argv)
{
    require_args(argc, 2, "aes128_decrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes128_expand_key_hw(key);

    std::array<uint8_t, aes128_block_size> buf;
    require_copy(buf, block, "ciphertext");
    aes128_decrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes128_cmac(int argc, char** argv)
{
    require_args(argc, 2, "aes128_cmac");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto message = hex_decode(argv[3]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes128_expand_key_hw(key);

    auto tag = aes128_cmac_hw(rk, message);
    hex_print(tag);
    std::println("");
}

static void cmd_aes256_encrypt(int argc, char** argv)
{
    require_args(argc, 2, "aes256_encrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes256_expand_key_hw(key);

    std::array<uint8_t, aes256_block_size> buf;
    require_copy(buf, block, "plaintext");
    aes256_encrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes256_decrypt(int argc, char** argv)
{
    require_args(argc, 2, "aes256_decrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes256_expand_key_hw(key);

    std::array<uint8_t, aes256_block_size> buf;
    require_copy(buf, block, "ciphertext");
    aes256_decrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes256_cmac(int argc, char** argv)
{
    require_args(argc, 2, "aes256_cmac");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto message = hex_decode(argv[3]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes256_expand_key_hw(key);

    auto tag = aes256_cmac_hw(rk, message);
    hex_print(tag);
    std::println("");
}

//
// Hardware-accelerated command handlers
//

static void cmd_aes128_encrypt_hw(int argc, char** argv)
{
    require_args(argc, 2, "aes128_encrypt_hw");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes128_expand_key_hw(key);

    std::array<uint8_t, aes128_block_size> buf;
    require_copy(buf, block, "plaintext");
    aes128_encrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes128_decrypt_hw(int argc, char** argv)
{
    require_args(argc, 2, "aes128_decrypt_hw");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes128_expand_key_hw(key);

    std::array<uint8_t, aes128_block_size> buf;
    require_copy(buf, block, "ciphertext");
    aes128_decrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes128_cmac_hw(int argc, char** argv)
{
    require_args(argc, 2, "aes128_cmac_hw");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto message = hex_decode(argv[3]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes128_expand_key_hw(key);

    auto tag = aes128_cmac_hw(rk, message);
    hex_print(tag);
    std::println("");
}

static void cmd_aes256_encrypt_hw(int argc, char** argv)
{
    require_args(argc, 2, "aes256_encrypt_hw");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes256_expand_key_hw(key);

    std::array<uint8_t, aes256_block_size> buf;
    require_copy(buf, block, "plaintext");
    aes256_encrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes256_decrypt_hw(int argc, char** argv)
{
    require_args(argc, 2, "aes256_decrypt_hw");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto block = hex_decode(argv[3]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes256_expand_key_hw(key);

    std::array<uint8_t, aes256_block_size> buf;
    require_copy(buf, block, "ciphertext");
    aes256_decrypt_block_hw(rk, buf);
    hex_print(buf);
    std::println("");
}

static void cmd_aes256_cmac_hw(int argc, char** argv)
{
    require_args(argc, 2, "aes256_cmac_hw");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto message = hex_decode(argv[3]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    auto rk = aes256_expand_key_hw(key);

    auto tag = aes256_cmac_hw(rk, message);
    hex_print(tag);
    std::println("");
}

static void cmd_sha256_hw(int argc, char** argv)
{
    require_args(argc, 1, "sha256_hw");
    auto message = hex_decode(argv[2]);
    auto digest = sha256_hw(message);
    hex_print(digest);
    std::println("");
}

static void cmd_sha512_hw(int argc, char** argv)
{
    require_args(argc, 1, "sha512_hw");
    auto message = hex_decode(argv[2]);
    auto digest = sha512_hw(message);
    hex_print(digest);
    std::println("");
}

static void cmd_sha256_hmac_hw(int argc, char** argv)
{
    require_args(argc, 2, "sha256_hmac_hw");
    auto key = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key};
    auto message = hex_decode(argv[3]);
    auto mac = sha256_hmac_hw(key, message);
    hex_print(mac);
    std::println("");
}

//
// SIV / GCM-SIV command handlers
//

static void cmd_aes128_siv_encrypt(int argc, char** argv)
{
    require_args(argc, 3, "aes128_siv_encrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto aad = hex_decode(argv[3]);
    auto pt = hex_decode(argv[4]);

    Aes128SivKey key;
    require_copy(key.data, key_bytes, "key");

    auto siv = aes128_siv_encrypt(key, pt, aad);
    hex_print(siv);
    std::print(" ");
    hex_print(pt);
    std::println("");
}

static void cmd_aes128_siv_decrypt(int argc, char** argv)
{
    require_args(argc, 4, "aes128_siv_decrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto siv_bytes = hex_decode(argv[3]);
    auto aad = hex_decode(argv[4]);
    auto ct = hex_decode(argv[5]);

    Aes128SivKey key;
    require_copy(key.data, key_bytes, "key");

    std::array<uint8_t, aes128_block_size> siv;
    require_copy(siv, siv_bytes, "siv");

    bool const ok = aes128_siv_decrypt(key, ct, siv, aad);
    std::print("{} ", ok ? "ok" : "fail");
    hex_print(ct);
    std::println("");
}

static void cmd_aes256_siv_encrypt(int argc, char** argv)
{
    require_args(argc, 3, "aes256_siv_encrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto aad = hex_decode(argv[3]);
    auto pt = hex_decode(argv[4]);

    Aes256SivKey key;
    require_copy(key.data, key_bytes, "key");

    auto siv = aes256_siv_encrypt(key, pt, aad);
    hex_print(siv);
    std::print(" ");
    hex_print(pt);
    std::println("");
}

static void cmd_aes256_siv_decrypt(int argc, char** argv)
{
    require_args(argc, 4, "aes256_siv_decrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto siv_bytes = hex_decode(argv[3]);
    auto aad = hex_decode(argv[4]);
    auto ct = hex_decode(argv[5]);

    Aes256SivKey key;
    require_copy(key.data, key_bytes, "key");

    std::array<uint8_t, aes256_block_size> siv;
    require_copy(siv, siv_bytes, "siv");

    bool const ok = aes256_siv_decrypt(key, ct, siv, aad);
    std::print("{} ", ok ? "ok" : "fail");
    hex_print(ct);
    std::println("");
}

static void cmd_aes128_gcm_siv_encrypt(int argc, char** argv)
{
    require_args(argc, 4, "aes128_gcm_siv_encrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto nonce_bytes = hex_decode(argv[3]);
    auto aad = hex_decode(argv[4]);
    auto pt = hex_decode(argv[5]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    std::array<uint8_t, aes_gcm_siv_nonce_size> nonce;
    require_copy(nonce, nonce_bytes, "nonce");

    auto tag = aes128_gcm_siv_encrypt(key, nonce, pt, aad);
    hex_print(tag);
    std::print(" ");
    hex_print(pt);
    std::println("");
}

static void cmd_aes128_gcm_siv_decrypt(int argc, char** argv)
{
    require_args(argc, 5, "aes128_gcm_siv_decrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto nonce_bytes = hex_decode(argv[3]);
    auto tag_bytes = hex_decode(argv[4]);
    auto aad = hex_decode(argv[5]);
    auto ct = hex_decode(argv[6]);

    Aes128Key key;
    require_copy(key.data, key_bytes, "key");
    std::array<uint8_t, aes_gcm_siv_nonce_size> nonce;
    require_copy(nonce, nonce_bytes, "nonce");
    std::array<uint8_t, aes_gcm_siv_tag_size> tag;
    require_copy(tag, tag_bytes, "tag");

    bool const ok = aes128_gcm_siv_decrypt(key, nonce, ct, tag, aad);
    std::print("{} ", ok ? "ok" : "fail");
    hex_print(ct);
    std::println("");
}

static void cmd_aes256_gcm_siv_encrypt(int argc, char** argv)
{
    require_args(argc, 4, "aes256_gcm_siv_encrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto nonce_bytes = hex_decode(argv[3]);
    auto aad = hex_decode(argv[4]);
    auto pt = hex_decode(argv[5]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    std::array<uint8_t, aes_gcm_siv_nonce_size> nonce;
    require_copy(nonce, nonce_bytes, "nonce");

    auto tag = aes256_gcm_siv_encrypt(key, nonce, pt, aad);
    hex_print(tag);
    std::print(" ");
    hex_print(pt);
    std::println("");
}

static void cmd_aes256_gcm_siv_decrypt(int argc, char** argv)
{
    require_args(argc, 5, "aes256_gcm_siv_decrypt");
    auto key_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key_bytes};
    auto nonce_bytes = hex_decode(argv[3]);
    auto tag_bytes = hex_decode(argv[4]);
    auto aad = hex_decode(argv[5]);
    auto ct = hex_decode(argv[6]);

    Aes256Key key;
    require_copy(key.data, key_bytes, "key");
    std::array<uint8_t, aes_gcm_siv_nonce_size> nonce;
    require_copy(nonce, nonce_bytes, "nonce");
    std::array<uint8_t, aes_gcm_siv_tag_size> tag;
    require_copy(tag, tag_bytes, "tag");

    bool const ok = aes256_gcm_siv_decrypt(key, nonce, ct, tag, aad);
    std::print("{} ", ok ? "ok" : "fail");
    hex_print(ct);
    std::println("");
}

static void cmd_sha256(int argc, char** argv)
{
    require_args(argc, 1, "sha256");
    auto message = hex_decode(argv[2]);
    auto digest = sha256_hw(message);
    hex_print(digest);
    std::println("");
}

static void cmd_sha512(int argc, char** argv)
{
    require_args(argc, 1, "sha512");
    auto message = hex_decode(argv[2]);
    auto digest = sha512_hw(message);
    hex_print(digest);
    std::println("");
}

static void cmd_sha256_hmac(int argc, char** argv)
{
    require_args(argc, 2, "sha256_hmac");
    auto key = hex_decode(argv[2]);
    SecureVectorGuard const guard_key{key};
    auto message = hex_decode(argv[3]);
    auto mac = sha256_hmac_hw(key, message);
    hex_print(mac);
    std::println("");
}

static void cmd_hkdf_extract(int argc, char** argv)
{
    require_args(argc, 2, "hkdf_extract");
    auto salt = hex_decode(argv[2]);
    auto ikm = hex_decode(argv[3]);
    SecureVectorGuard const guard_ikm{ikm};
    auto prk = hkdf_sha256_extract(salt, ikm);
    hex_print(prk);
    std::println("");
}

static void cmd_hkdf_expand(int argc, char** argv)
{
    require_args(argc, 3, "hkdf_expand");
    auto prk_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_prk{prk_bytes};
    auto info = hex_decode(argv[3]);
    auto is_invalid_hkdf_length = [](char const* str, char const* end, long val) -> bool {
        return end == str || *end != '\0' || val <= 0 || val > 255L * 32;
    };
    char* endptr = nullptr;
    long const length = std::strtol(argv[4], &endptr, 10);
    if (is_invalid_hkdf_length(argv[4], endptr, length)) {
        std::println(stderr, "error: hkdf_expand length must be 1..{}", 255L * 32);
        std::exit(1);
    }

    std::array<uint8_t, hkdf_sha256_prk_size> prk;
    require_copy(prk, prk_bytes, "prk");

    std::vector<uint8_t> okm(static_cast<size_t>(length));
    bool const ok = hkdf_sha256_expand(prk, info, okm);
    if (!ok) {
        std::println(stderr, "error: hkdf_expand failed");
        std::exit(1);
    }
    hex_print(okm);
    std::println("");
}

static void cmd_hkdf(int argc, char** argv)
{
    require_args(argc, 4, "hkdf");
    auto salt = hex_decode(argv[2]);
    auto ikm = hex_decode(argv[3]);
    SecureVectorGuard const guard_ikm{ikm};
    auto info = hex_decode(argv[4]);
    auto is_invalid_hkdf_length = [](char const* str, char const* end, long val) -> bool {
        return end == str || *end != '\0' || val <= 0 || val > 255L * 32;
    };
    char* endptr = nullptr;
    long const length = std::strtol(argv[5], &endptr, 10);
    if (is_invalid_hkdf_length(argv[5], endptr, length)) {
        std::println(stderr, "error: hkdf length must be 1..{}", 255L * 32);
        std::exit(1);
    }

    std::vector<uint8_t> okm(static_cast<size_t>(length));
    bool const ok = hkdf_sha256(salt, ikm, info, okm);
    if (!ok) {
        std::println(stderr, "error: hkdf failed");
        std::exit(1);
    }
    hex_print(okm);
    std::println("");
}

static void cmd_ed25519_pubkey(int argc, char** argv)
{
    require_args(argc, 1, "ed25519_pubkey");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};

    std::array<uint8_t, ed25519_seed_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = ed25519_keypair_from_seed(seed_arr);
    auto pk = ed25519_public_key(sk);
    hex_print(pk.data);
    std::println("");
}

static void cmd_ed25519_sign(int argc, char** argv)
{
    require_args(argc, 2, "ed25519_sign");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};
    auto message = hex_decode(argv[3]);

    std::array<uint8_t, ed25519_seed_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = ed25519_keypair_from_seed(seed_arr);
    auto sig = ed25519_sign(sk, message);
    hex_print(sig.data);
    std::println("");
}

static void cmd_ed25519_verify(int argc, char** argv)
{
    require_args(argc, 3, "ed25519_verify");
    auto pk_bytes = hex_decode(argv[2]);
    auto message = hex_decode(argv[3]);
    auto sig_bytes = hex_decode(argv[4]);

    Ed25519PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");
    Ed25519Signature sig;
    require_copy(sig.data, sig_bytes, "signature");

    bool const ok = ed25519_verify(pk, message, sig);
    std::println("{}", ok ? "ok" : "fail");
}

static void cmd_x25519_pubkey(int argc, char** argv)
{
    require_args(argc, 1, "x25519_pubkey");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};

    std::array<uint8_t, X25519PrivateKey::LENGTH> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = x25519_keypair_from_seed(seed_arr);
    hex_print(sk.public_key.data);
    std::println("");
}

static void cmd_x25519(int argc, char** argv)
{
    require_args(argc, 2, "x25519");
    auto sk_bytes = hex_decode(argv[2]);
    SecureVectorGuard const guard_sk{sk_bytes};
    auto pk_bytes = hex_decode(argv[3]);

    X25519PrivateKey sk;
    require_copy(sk.data, sk_bytes, "private_key");
    X25519PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");

    auto shared = x25519(sk, pk);
    hex_print(shared);
    std::println("");
}

static void cmd_ed25519_to_x25519_pk(int argc, char** argv)
{
    require_args(argc, 1, "ed25519_to_x25519_pk");
    auto pk_bytes = hex_decode(argv[2]);

    Ed25519PublicKey ed_pk;
    require_copy(ed_pk.data, pk_bytes, "ed25519_public_key");

    auto x_pk = ed25519_pk_to_x25519_pk(ed_pk);
    if (!x_pk) {
        std::println(stderr, "Error: Ed25519 key is the identity point (y=1), cannot convert to X25519");
        std::exit(1);
    }
    hex_print(x_pk->data);
    std::println("");
}

static void cmd_ed25519_to_x25519_sk(int argc, char** argv)
{
    require_args(argc, 1, "ed25519_to_x25519_sk");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};

    std::array<uint8_t, ed25519_seed_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto ed_sk = ed25519_keypair_from_seed(seed_arr);
    auto x_sk = ed25519_sk_to_x25519_sk(ed_sk);
    hex_print(x_sk.data);
    std::print(" ");
    hex_print(x_sk.public_key.data);
    std::println("");
}

//
// P-256 ECDSA command handlers
//

static void cmd_p256_ecdsa_pubkey(int argc, char** argv)
{
    require_args(argc, 1, "p256_ecdsa_pubkey");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};

    std::array<uint8_t, p256_scalar_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = p256_ecdsa_keypair_from_seed(seed_arr);
    hex_print(sk.public_key.data);
    std::println("");
}

static void cmd_p256_ecdsa_sign(int argc, char** argv)
{
    require_args(argc, 2, "p256_ecdsa_sign");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};
    auto message = hex_decode(argv[3]);

    std::array<uint8_t, p256_scalar_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = p256_ecdsa_keypair_from_seed(seed_arr);
    auto sig = p256_ecdsa_sign(sk, message);
    hex_print(sig.data);
    std::println("");
}

static void cmd_p256_ecdsa_verify(int argc, char** argv)
{
    require_args(argc, 3, "p256_ecdsa_verify");
    auto pk_bytes = hex_decode(argv[2]);
    auto message = hex_decode(argv[3]);
    auto sig_bytes = hex_decode(argv[4]);

    P256PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");
    P256EcdsaSignature sig;
    require_copy(sig.data, sig_bytes, "signature");

    bool const ok = p256_ecdsa_verify(pk, message, sig);
    std::println("{}", ok ? "ok" : "fail");
}

static void cmd_p256_ecdh(int argc, char** argv)
{
    require_args(argc, 2, "p256_ecdh");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};
    auto peer_pk_bytes = hex_decode(argv[3]);

    std::array<uint8_t, p256_scalar_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    P256PublicKey peer_pk;
    require_copy(peer_pk.data, peer_pk_bytes, "peer_public_key");

    auto sk = p256_ecdsa_keypair_from_seed(seed_arr);
    auto shared = p256_ecdh(sk, peer_pk);
    hex_print(shared);
    std::println("");
}

//
// ECIES command handlers
//

static void cmd_ecies_encrypt(int argc, char** argv)
{
    require_args(argc, 3, "ecies_encrypt");
    auto pk_bytes = hex_decode(argv[2]);
    auto entropy_bytes = hex_decode(argv[3]);
    SecureVectorGuard const guard_entropy{entropy_bytes};
    auto pt = hex_decode(argv[4]);

    P256PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");
    std::array<uint8_t, p256_scalar_size> entropy;
    require_copy(entropy, entropy_bytes, "entropy");

    size_t const out_size = ecies_output_size(pt.size());
    std::vector<uint8_t> output(out_size);

    auto result = ecies_encrypt(pk, pt, output, entropy);
    if (result.empty()) {
        std::println(stderr, "error: ecies_encrypt failed");
        std::exit(1);
    }
    hex_print(result);
    std::println("");
}

static void cmd_ecies_decrypt(int argc, char** argv)
{
    require_args(argc, 2, "ecies_decrypt");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};
    auto ct = hex_decode(argv[3]);

    std::array<uint8_t, p256_scalar_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = p256_ecdsa_keypair_from_seed(seed_arr);
    std::vector<uint8_t> plaintext(ct.size());

    auto result = ecies_decrypt(sk, ct, plaintext);
    if (result.data() == nullptr) {
        std::println("fail");
    } else {
        std::print("ok ");
        hex_print(result);
        std::println("");
    }
}

//
// X25519 ECIES command handlers
//

static void cmd_x25519_ecies_encrypt(int argc, char** argv)
{
    require_args(argc, 3, "x25519_ecies_encrypt");
    auto pk_bytes = hex_decode(argv[2]);
    auto entropy_bytes = hex_decode(argv[3]);
    SecureVectorGuard const guard_entropy{entropy_bytes};
    auto pt = hex_decode(argv[4]);

    X25519PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");
    std::array<uint8_t, X25519PrivateKey::LENGTH> entropy;
    require_copy(entropy, entropy_bytes, "entropy");

    size_t const out_size = x25519_ecies_output_size(pt.size());
    std::vector<uint8_t> output(out_size);

    auto result = x25519_ecies_encrypt(pk, pt, output, entropy);
    if (result.empty()) {
        std::println(stderr, "error: x25519_ecies_encrypt failed");
        std::exit(1);
    }
    hex_print(result);
    std::println("");
}

static void cmd_x25519_ecies_decrypt(int argc, char** argv)
{
    require_args(argc, 2, "x25519_ecies_decrypt");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};
    auto ct = hex_decode(argv[3]);

    std::array<uint8_t, X25519PrivateKey::LENGTH> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto sk = x25519_keypair_from_seed(seed_arr);
    std::vector<uint8_t> plaintext(ct.size());

    auto result = x25519_ecies_decrypt(sk, ct, plaintext);
    if (result.data() == nullptr) {
        std::println("fail");
    } else {
        std::print("ok ");
        hex_print(result);
        std::println("");
    }
}

//
// PKCS#8 / SPKI command handlers
//

static void cmd_spki_export_p256(int argc, char** argv)
{
    require_args(argc, 1, "spki_export_p256");
    auto pk_bytes = hex_decode(argv[2]);

    P256PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");

    auto der = spki_export_p256(pk);
    hex_print(der);
    std::println("");
}

static void cmd_spki_import_p256(int argc, char** argv)
{
    require_args(argc, 1, "spki_import_p256");
    auto der = hex_decode(argv[2]);

    auto pk = spki_import_p256(der);
    if (!pk) {
        std::println("fail");
    } else {
        std::print("ok ");
        hex_print(pk->data);
        std::println("");
    }
}

static void cmd_pkcs8_export_p256(int argc, char** argv)
{
    require_args(argc, 1, "pkcs8_export_p256");
    auto scalar = hex_decode(argv[2]);
    SecureVectorGuard const guard_scalar{scalar};

    std::array<uint8_t, p256_scalar_size> scalar_arr;
    require_copy(scalar_arr, scalar, "scalar");

    auto sk = p256_keypair_from_scalar(scalar_arr);
    if (!sk) {
        std::println(stderr, "error: invalid scalar");
        std::exit(1);
    }

    auto der = pkcs8_export_p256(*sk);
    hex_print(der);
    std::println("");
}

static void cmd_pkcs8_import_p256(int argc, char** argv)
{
    require_args(argc, 1, "pkcs8_import_p256");
    auto der = hex_decode(argv[2]);

    auto sk = pkcs8_import_p256(der);
    if (!sk) {
        std::println("fail");
    } else {
        std::print("ok ");
        hex_print(span<uint8_t const>(sk->data).first<P256PrivateKey::LENGTH>());
        std::print(" ");
        hex_print(sk->public_key.data);
        std::println("");
    }
}

static void cmd_spki_export_ed25519(int argc, char** argv)
{
    require_args(argc, 1, "spki_export_ed25519");
    auto pk_bytes = hex_decode(argv[2]);

    Ed25519PublicKey pk;
    require_copy(pk.data, pk_bytes, "public_key");

    auto der = spki_export_ed25519(pk);
    hex_print(der);
    std::println("");
}

static void cmd_spki_import_ed25519(int argc, char** argv)
{
    require_args(argc, 1, "spki_import_ed25519");
    auto der = hex_decode(argv[2]);

    auto pk = spki_import_ed25519(der);
    if (!pk) {
        std::println("fail");
    } else {
        std::print("ok ");
        hex_print(pk->data);
        std::println("");
    }
}

static void cmd_pkcs8_export_ed25519(int argc, char** argv)
{
    require_args(argc, 1, "pkcs8_export_ed25519");
    auto seed = hex_decode(argv[2]);
    SecureVectorGuard const guard_seed{seed};

    std::array<uint8_t, ed25519_seed_size> seed_arr;
    require_copy(seed_arr, seed, "seed");

    auto der = pkcs8_export_ed25519(seed_arr);
    hex_print(der);
    std::println("");
}

static void cmd_pkcs8_import_ed25519(int argc, char** argv)
{
    require_args(argc, 1, "pkcs8_import_ed25519");
    auto der = hex_decode(argv[2]);

    auto sk = pkcs8_import_ed25519(der);
    if (!sk) {
        std::println("fail");
    } else {
        std::print("ok ");
        hex_print(sk->public_key.data);
        std::println("");
    }
}

//
// Main dispatch
//

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    std::string_view const cmd(argv[1]);

    if (cmd == "aes128_encrypt") {
        cmd_aes128_encrypt(argc, argv);
    } else if (cmd == "aes128_decrypt") {
        cmd_aes128_decrypt(argc, argv);
    } else if (cmd == "aes128_cmac") {
        cmd_aes128_cmac(argc, argv);
    } else if (cmd == "aes128_encrypt_hw") {
        cmd_aes128_encrypt_hw(argc, argv);
    } else if (cmd == "aes128_decrypt_hw") {
        cmd_aes128_decrypt_hw(argc, argv);
    } else if (cmd == "aes128_cmac_hw") {
        cmd_aes128_cmac_hw(argc, argv);
    } else if (cmd == "aes256_encrypt") {
        cmd_aes256_encrypt(argc, argv);
    } else if (cmd == "aes256_decrypt") {
        cmd_aes256_decrypt(argc, argv);
    } else if (cmd == "aes256_cmac") {
        cmd_aes256_cmac(argc, argv);
    } else if (cmd == "aes256_encrypt_hw") {
        cmd_aes256_encrypt_hw(argc, argv);
    } else if (cmd == "aes256_decrypt_hw") {
        cmd_aes256_decrypt_hw(argc, argv);
    } else if (cmd == "aes256_cmac_hw") {
        cmd_aes256_cmac_hw(argc, argv);
    } else if (cmd == "aes128_siv_encrypt") {
        cmd_aes128_siv_encrypt(argc, argv);
    } else if (cmd == "aes128_siv_decrypt") {
        cmd_aes128_siv_decrypt(argc, argv);
    } else if (cmd == "aes256_siv_encrypt") {
        cmd_aes256_siv_encrypt(argc, argv);
    } else if (cmd == "aes256_siv_decrypt") {
        cmd_aes256_siv_decrypt(argc, argv);
    } else if (cmd == "aes128_gcm_siv_encrypt") {
        cmd_aes128_gcm_siv_encrypt(argc, argv);
    } else if (cmd == "aes128_gcm_siv_decrypt") {
        cmd_aes128_gcm_siv_decrypt(argc, argv);
    } else if (cmd == "aes256_gcm_siv_encrypt") {
        cmd_aes256_gcm_siv_encrypt(argc, argv);
    } else if (cmd == "aes256_gcm_siv_decrypt") {
        cmd_aes256_gcm_siv_decrypt(argc, argv);
    } else if (cmd == "sha256") {
        cmd_sha256(argc, argv);
    } else if (cmd == "sha256_hw") {
        cmd_sha256_hw(argc, argv);
    } else if (cmd == "sha512") {
        cmd_sha512(argc, argv);
    } else if (cmd == "sha512_hw") {
        cmd_sha512_hw(argc, argv);
    } else if (cmd == "sha256_hmac") {
        cmd_sha256_hmac(argc, argv);
    } else if (cmd == "sha256_hmac_hw") {
        cmd_sha256_hmac_hw(argc, argv);
    } else if (cmd == "hkdf_extract") {
        cmd_hkdf_extract(argc, argv);
    } else if (cmd == "hkdf_expand") {
        cmd_hkdf_expand(argc, argv);
    } else if (cmd == "hkdf") {
        cmd_hkdf(argc, argv);
    } else if (cmd == "ed25519_pubkey") {
        cmd_ed25519_pubkey(argc, argv);
    } else if (cmd == "ed25519_sign") {
        cmd_ed25519_sign(argc, argv);
    } else if (cmd == "ed25519_verify") {
        cmd_ed25519_verify(argc, argv);
    } else if (cmd == "x25519_pubkey") {
        cmd_x25519_pubkey(argc, argv);
    } else if (cmd == "x25519") {
        cmd_x25519(argc, argv);
    } else if (cmd == "ed25519_to_x25519_pk") {
        cmd_ed25519_to_x25519_pk(argc, argv);
    } else if (cmd == "ed25519_to_x25519_sk") {
        cmd_ed25519_to_x25519_sk(argc, argv);
    } else if (cmd == "p256_ecdsa_pubkey") {
        cmd_p256_ecdsa_pubkey(argc, argv);
    } else if (cmd == "p256_ecdsa_sign") {
        cmd_p256_ecdsa_sign(argc, argv);
    } else if (cmd == "p256_ecdsa_verify") {
        cmd_p256_ecdsa_verify(argc, argv);
    } else if (cmd == "p256_ecdh") {
        cmd_p256_ecdh(argc, argv);
    } else if (cmd == "ecies_encrypt") {
        cmd_ecies_encrypt(argc, argv);
    } else if (cmd == "ecies_decrypt") {
        cmd_ecies_decrypt(argc, argv);
    } else if (cmd == "x25519_ecies_encrypt") {
        cmd_x25519_ecies_encrypt(argc, argv);
    } else if (cmd == "x25519_ecies_decrypt") {
        cmd_x25519_ecies_decrypt(argc, argv);
    } else if (cmd == "spki_export_p256") {
        cmd_spki_export_p256(argc, argv);
    } else if (cmd == "spki_import_p256") {
        cmd_spki_import_p256(argc, argv);
    } else if (cmd == "pkcs8_export_p256") {
        cmd_pkcs8_export_p256(argc, argv);
    } else if (cmd == "pkcs8_import_p256") {
        cmd_pkcs8_import_p256(argc, argv);
    } else if (cmd == "spki_export_ed25519") {
        cmd_spki_export_ed25519(argc, argv);
    } else if (cmd == "spki_import_ed25519") {
        cmd_spki_import_ed25519(argc, argv);
    } else if (cmd == "pkcs8_export_ed25519") {
        cmd_pkcs8_export_ed25519(argc, argv);
    } else if (cmd == "pkcs8_import_ed25519") {
        cmd_pkcs8_import_ed25519(argc, argv);
    } else if (cmd == "backend") {
        std::println("{}", statusbar::crypto::crypto_backend_summary());
    } else if (cmd == "--help" || cmd == "-h") {
        usage();
    } else {
        std::println(stderr, "error: unknown command: {}", argv[1]);
        usage();
        return 1;
    }

    return 0;
}
