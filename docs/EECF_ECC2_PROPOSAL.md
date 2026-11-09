# EECF ECC2: an HPKE-based `enc` value for IEEE Std 1722 Clause 17

*A statusbar proposal to the IEEE 1722 working group. Status: draft, implemented
in statusbar-crypto and statusbar-avb as `enc = 0x01`; nothing here is normative
until the working group adopts it.*

## Why a second `enc` value

IEEE Std 1722-2025 clause 17 (ECC Encrypted Control Format, EECF) defines one
encryption algorithm, ECC1 (Table 53, `enc = 0`): the IEEE Std 1363a-2004
DL/ECIES scheme with the option set P-256, ECSVDP-DHC, KDF2-SHA-256,
AES-256-CBC with a null IV and PKCS#7 padding, HMAC-SHA-256, DHAES mode and
EC2OSP-X point encoding (17.3.1), laid out on the wire as V ‖ T ‖ C (17.3.2)
over the plaintext encryption_timestamp ‖ encapsulated AVTPDU (17.3.3).

ECC1 is sound, and statusbar implements it to the letter (see
`statusbar/crypto/ecies` and `eecf_ecc1_*` in `statusbar/avtp_crypto`). Its
practical weaknesses are about interoperability, not cryptography:

1. **No published known answers.** No standard, test suite or widely used
   library publishes test vectors for this ECIES option set. IEEE 1363a, ANSI
   X9.63 and SEC 1 ship none; ISO/IEC 18033-2's vectors use SHA-1 and P-192;
   Wycheproof and NIST CAVP cover the primitives only. Two independent ECC1
   implementations can only be checked against each other, and a disagreement
   in any of the eight options (key-split order, KDF counter encoding, point
   format, padding, MAC input, mode) only shows up as a tag failure with no
   way to tell which side is wrong. statusbar found and fixed one such
   ambiguity (the 11.3.2 step 10a key order) in November 2026.
2. **A generic composition, built by hand.** CBC-then-HMAC with a fixed null IV
   is secure only because every message uses a fresh ECDH-derived key; nothing
   in the construction protects an implementer who reuses one. The EECF
   header (enc, encrypted_payload_length, key_id) is not authenticated at
   all, so a middlebox can rewrite it without the tag noticing.
3. **Library support.** ECIES with these options is not offered by OpenSSL,
   BoringSSL, libsodium, Go's standard library or the WebCrypto API; every
   implementer writes it from the primitives.

RFC 9180, Hybrid Public Key Encryption (HPKE), is the IETF's answer to exactly
this problem: one well-analysed hybrid construction, a small registry of
cipher suites, and a complete set of published test vectors covering every
intermediate value. It is deployed in TLS Encrypted Client Hello (RFC 9849),
Messaging Layer Security (RFC 9420) and Oblivious HTTP (RFC 9458), cited by
NIST SP 800-227, and implemented in BoringSSL, Go, Rust and others.

## The proposal

Add to Table 53 (EECF `enc` field):

| enc | Name | Algorithm |
|-----|------|-----------|
| 0x00 | ECC1 | IEEE Std 1363a-2004 DL/ECIES, as 17.3.1 (unchanged) |
| **0x01** | **ECC2** | **RFC 9180 HPKE, base mode, cipher suite DHKEM(P-256, HKDF-SHA256) 0x0010 / HKDF-SHA256 0x0001 / AES-256-GCM 0x0002** |
| 0x02–0x0F | — | Reserved |

### 17.3.x ECC2 encryption

For `enc = ECC2` the sender runs one HPKE context per EECF PDU:

```
enc, context = SetupBaseS(pkR, info)             # RFC 9180 5.1.1
ciphertext    = context.Seal(aad, plaintext)      # RFC 9180 5.2, seq = 0
```

with

- **pkR** the recipient's P-256 public key identified by `key_id` (the same key
  ECC1 uses; HPKE serialises it as a SEC1 uncompressed point, 0x04 ‖ x ‖ y);
- **info** the fixed ASCII string `"IEEE 1722 EECF ECC2"` (19 octets), which
  domain-separates EECF from any other HPKE use of the same key;
- **aad** the 12-octet EECF header exactly as transmitted: subtype,
  r ‖ version ‖ enc, reserved ‖ encrypted_payload_length, key_id (the length
  field holds the final encrypted_payload_length, which is known before
  encryption);
- **plaintext** the 17.3.3 plaintext, unchanged: encryption_timestamp (8
  octets) ‖ encapsulated AVTPDU. AES-GCM needs no padding.

The HPKE `enc` output (the sender's ephemeral public key, 65 octets) is sent
first, then the AES-256-GCM ciphertext (the plaintext's length), then the
16-octet tag (RFC 9180 writes the AEAD output as ciphertext ‖ tag):

```
encrypted_payload = enc (65) || C (len(plaintext)) || T (16)
encrypted_payload_length = 81 + 8 + len(encapsulated AVTPDU)
```

The sender's ephemeral key pair is `DeriveKeyPair(ikm)` (RFC 9180 7.1.3) from
at least 32 octets of fresh randomness per PDU, never reused.

### 17.3.y ECC2 decryption

The receiver takes `enc` from the first 65 octets, runs `SetupBaseR(enc, skR,
info)`, and `context.Open(aad, C ‖ T)` with the received 12-octet EECF header
as aad. Any failure (a malformed `enc`, a tag mismatch) discards the PDU. A
receiver MUST treat an `encrypted_payload_length` below 81 + 8 as malformed.

### Properties

- **Known answers end to end.** RFC 9180 Appendix A and the CFRG vector grid
  pin DeriveKeyPair, Encap, the key schedule, every Seal at chosen sequence
  numbers and the exports for this exact suite in all modes. statusbar's
  implementation reproduces all of them (`statusbar/crypto/hpke/hpke_test.cpp`).
- **Authenticated header.** enc, encrypted_payload_length and key_id are
  bound to the ciphertext. A PDU re-labelled as ECC1, re-addressed to another
  key_id or re-lengthed fails authentication instead of being misparsed.
- **Same key material.** The recipient's P-256 key and `key_id` are the ones
  ECC1 already distributes; a device can offer both enc values with one key.
- **Standard AEAD.** AES-256-GCM is NIST SP 800-38D with hardware support on
  every relevant CPU; the HPKE nonce is derived, so the sender manages no IV.
- **Size.** 81 octets of overhead versus ECC1's 65 plus up to 16 of padding;
  for a typical 50–200 octet AVTPDU the PDUs are within a few octets of each
  other.
- **Room to grow.** HPKE's auth mode (the sender's static key) and PSK mode
  are available if the working group later wants sender authentication inside
  the encryption rather than a separate ESCF signature; they would be further
  `enc` values, not changes to ECC2.

## Test vectors

Recipient key: d = SHA-256(seed) mod n with seed = 0x01 0x02 … 0x20 (statusbar's
`p256_ecdsa_keypair_from_seed`; the resulting d is what matters). Sender
ikm = `DEADBEEFCAFEBABE 0102030405060708 090A0B0C0D0E0F10 1112131415161718`.
key_id = 70-B3-D5-ED-C4-00-00-42.

**Payload-level** (plaintext `"Hello, HPKE ECC2"`, 16 octets; aad = EECF header
for a 97-octet payload, `ED 01 00 61 70 B3 D5 ED C4 00 00 42`):

```
04289dce76026f47dcb7d77006982f29b06f0e58c166b738d5dfcd276280de6d23
7c66c8670a4191c3ffb0de2cb2bb793c885fc8631d0dca63795c4dad394aa7cd    enc (65)
e927f21be61587d511f47e61b2a848ec                                    C  (16)
df4524fd10762fe8efa7ab8b526ef733                                    T  (16)
```

**Framed** (encryption_timestamp 0x0123456789ABCDEF, encapsulated AVTPDU
0x10 … 0x1F; aad = `ED 01 00 69 70 B3 D5 ED C4 00 00 42`):

```
04289dce76026f47dcb7d77006982f29b06f0e58c166b738d5dfcd276280de6d23
7c66c8670a4191c3ffb0de2cb2bb793c885fc8631d0dca63795c4dad394aa7cd    enc (65)
a061db1000926a7251ae2952e3fe1dc92245c6bf41452b5b                    C  (24)
84bbe99ed14496be08c5186eb4fdbd3f                                    T  (16)
```

The second vector is frame 20 of `wireshark/golden/statusbar-avb-golden.pcap`;
the Wireshark dissector in `wireshark/` splits it into enc, C and T. Both
vectors were produced by an independent Python HPKE implementation over the
`cryptography` package and reproduced by the C++ stack; the ECC1 vectors for
the same inputs are in `avtp_aem_frame_test.cpp` for comparison.

## Reference implementation

- `statusbar/crypto/hpke` — RFC 9180 for this suite family (both AES-GCM sizes,
  all four modes), with the CFRG vectors.
- `statusbar/crypto/aes_gcm` — AES-GCM with the GCM specification's and
  Wycheproof's vectors.
- `statusbar/avtp_crypto/avtp_aem_frame.hpp` — `eecf_ecc2_encrypt` /
  `eecf_ecc2_decrypt` and the payload-level functions; `eecf_ecc2_header`
  produces the aad.
- `statusbar-avtp-crypto-tool eecf_ecc2_encrypt` / `eecf_ecc2_decrypt` — hex
  command-line access for interoperability testing.
- `python/avtp_crypto/avb_key_exchange_frame_test.py` — the same in Python.

## Open questions for the working group

1. The `info` string. A fixed string is the simplest domain separator; the
   group may prefer to fold the AVTP subtype or a version into it.
2. Whether the header should be the aad (as proposed) or `aad` should be
   empty with the header left unauthenticated as in ECC1. The former costs
   nothing and closes the re-labelling gap.
3. Whether to register AES-128-GCM (suite 0x0010 / 0x0001 / 0x0001, RFC 9180
   Appendix A.3's suite, which the RFC prints in full) as a further value.
