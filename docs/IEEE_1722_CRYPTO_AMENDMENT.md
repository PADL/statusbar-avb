# IEEE Std 1722-2025 security clauses: the November 2026 amendment in statusbar

*statusbar is the proof of concept for the amendment proposal presented to the
IEEE 1722 working group in November 2026 ("IEEE Std 1722-2025 security clauses:
corrections and new variants", following the proposal of 2 February 2026).
This document maps each item of that proposal to what this stack implements,
and carries the test vectors. Nothing here is normative until the working
group adopts it; the published text remains IEEE Std 1722-2025.*

## Summary

The amendment has two parts. Part A corrects Clauses 13, 16, 17 and Annex F
without changing the wire format. Part B adds one `sig` value (ED25519) and two
`enc` values (ECC2 and ECC3, both RFC 9180 HPKE), plus the field renaming they
need. statusbar implements every item that touches code; the table says how.

| Item | Where | Change | statusbar |
|------|-------|--------|-----------|
| A1 | 13.2.4 | An AES-SIV key is two AES keys (256 or 512 bits) | `Aes128SivKey` (32 octets) and `Aes256SivKey` (64 octets) in statusbar-crypto already carry K1 ‖ K2 |
| A2 | 13.3.2, 13.3.3 | K is the AES-SIV key; AES-SIV is invoked with no associated data | The AES-SIV module takes the full key; AEF payload encryption with no AD is how the keychain tools use it |
| A3 | 16.2.5, 16.2.7, F.43 | Discard a signing_timestamp more than 100 ms early or in the future; discard a failed signature | `escf_signing_timestamp_is_acceptable(ts, now)` and `escf_verify_frame`; the Listener is told its PTP time |
| A4 | 16.2.1, 17.2.1 | Field list entry `encrypted_payload_length: 2 octets`; `r` is 1 bit | Both headers model the length as a full 16-bit field (Figures 109 and 110), not reserved(5) ‖ length(11) as this stack did before |
| A5 | 17.2.4 | key_id identifies the recipient's key pair; the sender uses its public key | So documented and used by every `eecf_ecc*` function |
| A6 | 16.3.1, 17.3.1, 17.3.2 | The 1363a options ECC1 and ECSSA leave unstated | ECC1: secp256r1, V of 33 octets, MAC1 with SHA-256 and a 256-bit tag, KDF2-SHA-256 with AES-256-CBC-IV0, K1 = leftmost 256 bits, P1 and P2 empty (`crypto/ecies`). ECSSA: secp256r1, SHA-256, c then d (`escf_ecssa_*`) |
| A7 | 17.3.3.2, 2, 3.2, F.41, F.44, 16.3.1 | Editorial | n/a |
| B1 | Tables 52, 53 | ED25519 = 1; ECC2 = 1, ECC3 = 2 | `EscfSigMode::ed25519`, `EecfEncMode::ecc2`, `EecfEncMode::ecc3`; the dissector's value tables |
| B2 | 16.1, 16.2.1, Fig. 109, 16.2.7, 16.3.1 | One 64-octet `signature_value`; ECSSA keeps c in octets 0–31 and d in 32–63 | `EscfPdu::signature_value`, `escf_ecssa_sign` writes r ‖ s |
| B3 | 16.4 (new) | Ed25519 variant: RFC 8032 5.1, no prehash, no context, over key_id ‖ signing_timestamp ‖ encapsulated_avtpdu | `escf_ed25519_sign` / `escf_ed25519_verify` (5.1.7's S < L check included) |
| B4 | 17.4 (new) | HPKE variants ECC2 (P-256) and ECC3 (X25519) | `eecf_ecc2_*`, `eecf_ecc3_*` over `crypto/hpke` |
| B5 | 2, 3.2, Annex F | References, acronyms, PICS rows | n/a |

## 17.4 HPKE variants ECC2 and ECC3

For `enc = ECC2` or `ECC3` the encrypted_payload is produced with RFC 9180
HPKE in base mode, one HPKE context per EECF AVTPDU (Table 53a):

| enc | KEM | KDF | AEAD | Npk | info |
|-----|-----|-----|------|-----|------|
| ECC2 (1) | DHKEM(P-256, HKDF-SHA256) 0x0010 | HKDF-SHA256 0x0001 | AES-256-GCM 0x0002 | 65 | `"IEEE 1722 EECF ECC2"` |
| ECC3 (2) | DHKEM(X25519, HKDF-SHA256) 0x0020 | HKDF-SHA256 0x0001 | AES-256-GCM 0x0002 | 32 | `"IEEE 1722 EECF ECC3"` |

Encryption (17.4.2):

```
enc, ctx = SetupBaseS(pkR, info)            # RFC 9180 5.1.1
C || T   = ctx.Seal(aad, pt)                 # RFC 9180 5.2, the only Seal
encrypted_payload        = enc || C || T
encrypted_payload_length = Npk + len(pt) + 16
```

- **pkR** is the recipient's public key identified by key_id, serialized per
  RFC 9180 7.1.1 (SEC1 uncompressed `0x04 || x || y` for P-256, the 32-octet
  u-coordinate for X25519).
- **aad** is the 12 octets of the EECF AVTPDU from subtype through key_id,
  with the final encrypted_payload_length.
- **pt** is the 17.3.3 plaintext payload (encryption_timestamp ‖
  encapsulated_avtpdu), not padded.
- The sender generates a new ephemeral key pair for every AVTPDU
  (`DeriveKeyPair` from at least 32 octets of fresh randomness) and never
  reuses it.

Decryption (17.4.3) discards the AVTPDU when any step fails: no key for
key_id and enc; encrypted_payload_length below Npk + 24; `SetupBaseR` fails
(an invalid enc, an all-zero X25519 result); `Open` does not authenticate.
Replay protection is the higher layer's (the IEEE 1722.1 AUTH_GET_NONCE /
AUTH_ADD_KEY_NONCE nonces travel inside the encrypted AVTPDU).

Sizes (17.4.4): EncapMTU = MTU − 36 − Npk, so with a 1500-octet MTU the
encapsulated AVTPDU is at most 1399 octets for ECC2 and 1432 for ECC3
(`eecf_ecc2_max_encapsulated_avtpdu_size`, `eecf_ecc3_max_encapsulated_avtpdu_size`).

## 16.4 Ed25519 signature variant

When `sig = ED25519`, signature_value is the 64-octet RFC 8032 Section 5.1
signature (R ‖ S; Ed25519ph and Ed25519ctx are not used) by the Ed25519
private key key_id identifies, over the same message ECSSA signs:
key_id ‖ signing_timestamp ‖ encapsulated_avtpdu. Validation follows RFC
8032 Section 5.1.7, which rejects S ≥ L. A3's timestamp window and discard
rules apply to both variants.

## Test vectors

Common inputs: seed = 0x01 0x02 … 0x20; sender ikmE = `DEADBEEFCAFEBABE
0102030405060708 090A0B0C0D0E0F10 1112131415161718`; key_id =
70-B3-D5-ED-C4-00-00-42; signing_timestamp = encryption_timestamp =
0x0123456789ABCDEF; encapsulated AVTPDU = 0x10 0x11 … 0x1F (16 octets).
"Illustration only": a real sender never reuses ikmE.

**Keys.** P-256: d = SHA-256(seed) mod n =
`ae216c2ef5247a3782c135efa279a3e4cdc61094270f5d2be58c6204b7a612c9`, public key
x ‖ y = `7b6c6cc40e03dd032b1284af91f936018ddb9f87089287e0ecf3b6282b2eb129`
`1ad1fbc90a6246b8d5e6d4012b5f03634d91f21171d6f6536bfddf00618219d0`
(`statusbar-avtp-crypto-tool p256_ecdsa_pubkey <seed>`). X25519: the HPKE
`DeriveKeyPair(seed)` key, skR =
`7357bc70e6c39c9fb33488b6378c36a628fd5beef3c8b8d9d6e3f35e5f6bb624`, pkR =
`49e4874e25fe389ed3c9fa2fd09d077907ecc5809c8619e2127128b6a72c7c7a`
(`hpke_x25519_keypair <seed>`). Ed25519: the RFC 8032 key of seed, public key
`79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664`.

**ECC2, framed** (aad `ED 01 00 69 70 B3 D5 ED C4 00 00 42`):

```
04289dce76026f47dcb7d77006982f29b06f0e58c166b738d5dfcd276280de6d23
7c66c8670a4191c3ffb0de2cb2bb793c885fc8631d0dca63795c4dad394aa7cd    enc (65)
a061db1000926a7251ae2952e3fe1dc92245c6bf41452b5b                    C  (24)
84bbe99ed14496be08c5186eb4fdbd3f                                    T  (16)
```

**ECC2, payload-level** (plaintext `"Hello, HPKE ECC2"`, aad `ED 01 00 61 …`):
same enc, C `e927f21be61587d511f47e61b2a848ec`, T `df4524fd10762fe8efa7ab8b526ef733`.

**ECC3, framed** (aad `ED 02 00 48 70 B3 D5 ED C4 00 00 42`):

```
8a7d2773c3dcc1cbfa275f67915a8b3f7b28b6e2c9b47346fbfd232cdf38fc22    enc (32)
884725ddf868909a3e8d26f45a1aec2fb0540ebcd1eb7df3                    C  (24)
0eb95b3b510a5abe6c6be677da632843                                    T  (16)
```

**ECC3, payload-level** (plaintext `"Hello, HPKE ECC3"`, aad `ED 02 00 40 …`):
same enc, C `c1010cd61eef7d3d7ed771c70b4cb90b`, T `b51f77e0d23ee646ec9e33e1b23d2c56`.

**ESCF signed message** (both variants):
`70b3d5edc40000420123456789abcdef101112131415161718191a1b1c1d1e1f`.

**ESCF ED25519** signature_value:

```
60a4f03c792333de185d84b8f98b8b84e020e2348c7cceaaa61f1ea4ac3b4d34
375624c2a4b7c674e0a2cc4a52c6966bed1e34b772ac5a845f91cf883529450f
```

**ESCF ECSSA_DSA_EMSA1_SHA256** signature_value (c then d; RFC 6979 nonce, so
this stack always produces it; any ECDSA verifier accepts it):

```
993ebbb74f57d10e01c554e0f70662d0a546d0aa667e83fb6962fee43b96a613
580e68bba17a988c1968394becbf2a7205ff9446a263ceccfc363b8d385b1c59
```

The ECC2 and ECC3 vectors were produced by an independent Python HPKE
implementation over the `cryptography` package and reproduced by the C++
stack; the Ed25519 signature by the `cryptography` package's Ed25519 and
reproduced by the C++ stack; the ECSSA signature by the C++ stack and
verified by the `cryptography` package's ECDSA. The golden capture
`wireshark/golden/statusbar-avb-golden.pcap` carries these as its two ESCF
frames (ECSSA, then ED25519) and three EECF frames (ECC1, ECC2, ECC3), which
the dissector in `wireshark/` splits into their fields.

## Where it is in the code

- `statusbar/crypto/hpke` (statusbar-crypto) — RFC 9180 for both KEMs, all
  four modes, both AES-GCM sizes, with the sixteen CFRG vectors.
- `statusbar/avtp/avtp_eecf.hpp`, `avtp_escf.hpp` — the headers as Figures
  109 and 110 draw them: 16-bit length fields, the 84-octet ESCF header with
  signing_timestamp and signature_value, Table 52 and 53 values.
- `statusbar/avtp_crypto/avtp_aem_frame.hpp` — `eecf_ecc2_*` and
  `eecf_ecc3_*` (payload-level and framed), the size rules, EECF frames.
- `statusbar/avtp_crypto/avtp_escf_frame.hpp` — the signed message, ECSSA and
  Ed25519 sign/verify, the timestamp window, ESCF frames and
  `escf_verify_frame`.
- `statusbar-avtp-crypto-tool` — `eecf_ecc2_encrypt` / `eecf_ecc2_decrypt`,
  `eecf_ecc3_encrypt` / `eecf_ecc3_decrypt`, `hpke_x25519_keypair`,
  `escf_sign` / `escf_verify` for interoperability testing (see `TOOLS.md`).
- `python/avtp_crypto/avb_key_exchange_frame_test.py` — the same in Python
  over `python/crypto/statusbar_crypto.py`, cross-checked against the tool.
- `wireshark/` — ESCF and EECF layouts, the ECC1/ECC2/ECC3 payload split and
  the ESCF encapsulated AVTPDU.

## Open items

The proposal's open items, and what this proof of concept does meanwhile:

1. Code points: ED25519 = 1, ECC2 = 1, ECC3 = 2, as proposed.
2. Replay protection: left to the higher layer; nothing on the 1722 wire.
3. info strings: the fixed `"IEEE 1722 EECF ECC2"` / `"IEEE 1722 EECF ECC3"`.
4. AES-128-GCM suites: not registered; `crypto/hpke` implements them should
   the group want further enc values.
5. One P-256 key for ECC1 and ECC2: the tests and vectors share one key
   (1363a D.5.3 Note 2 recommends one option set per key; the group decides).
6. ECSSA and ECC1 options: as A6, to be confirmed with other implementers.
7. Future-timestamp tolerance: none; a timestamp later than the Listener's
   PTP time is discarded.
