#!/usr/bin/env python3
# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "cryptography>=43.0",
#     "pycryptodome>=3.20",
# ]
# ///
"""
Frame-level integration tests for AVB controller-to-endpoint key distribution.

Wraps the AUTH_GET_NONCE / AUTH_ADD_KEY_NONCE protocol in complete IEEE 1722.1
AECP AEM Ethernet frames, building frames on the sender side and parsing them
on the receiver side to produce realistic wire-format packets.

The same test matrix as avb_key_exchange_test.py is used: Python/C++ backends,
all session key types, and optional YubiKey hardware ECDH.

Usage:
    uv run avb_key_exchange_frame_test.py -v
    uv run avb_key_exchange_frame_test.py -v --dump-frames
    uv run avb_key_exchange_frame_test.py -v --pcap-file capture.pcap
    uv run avb_key_exchange_frame_test.py -v --cli path/to/avtp_crypto_tool
    uv run avb_key_exchange_frame_test.py -v --dump-frames \\
        --yubikey-entity talker \\
        --yubikey-x25519-pubkey ~/yubikey_public.gpg-raw-public.txt \\
        --yubikey-x25519-keygrip 5D7B4D837571E7FCF2DBB6EE228AFF1FA1D8175A
"""

import argparse
import struct
import sys
import time
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "crypto"))
import statusbar_crypto as sc

from avb_key_exchange_test import (
    AvbEntity,
    Backend,
    Controller,
    EciesDecryptFn,
    EciesEncryptFn,
    Entity,
    SessionKeyConfig,
    SESSION_KEY_CONFIGS,
    YubikeyConfig,
    _add_tests,
    _KT_SESSION,
    _load_yubikey_config,
    create_entity,
    create_hardware_entity,
    find_cli,
    install_controller_keys,
    make_cpp_ecies_decrypt,
    make_cpp_ecies_encrypt,
    make_key_id,
    make_yubikey_ecies_decrypt,
    python_ecies_decrypt,
    python_ecies_encrypt,
)


# ---------------------------------------------------------------------------
# Wire format constants (IEEE 1722-2016 / IEEE 1722.1-2021)
# ---------------------------------------------------------------------------

ETHERNET_HEADER_SIZE = 14
AEM_DU_SIZE = 24
AEM_PAYLOAD_OFFSET = ETHERNET_HEADER_SIZE + AEM_DU_SIZE  # 38

AVTP_ETHERTYPE = 0x22F0

AECP_SUBTYPE = 0xFB
AECP_MESSAGE_TYPE_AEM_COMMAND = 0
AECP_MESSAGE_TYPE_AEM_RESPONSE = 1
AEM_STATUS_SUCCESS = 0

# AEM data length: controller_entity_id(8) + sequence_id(2) + command_type(2)
AEM_DATA_LENGTH = 12

# EECF subtype (IEEE 1722-2025 Clause 17)
EECF_SUBTYPE = 0xED
EECF_HEADER_SIZE = 12  # subtype_data(4) + key_id(8)
EECF_PAYLOAD_OFFSET = ETHERNET_HEADER_SIZE + EECF_HEADER_SIZE  # 26
EECF_ENC_ECC1 = 0
EECF_ENC_ECC2 = 1  # RFC 9180 HPKE over P-256 (IEEE 1722 security amendment, 17.4)
EECF_ENC_ECC3 = 2  # RFC 9180 HPKE over X25519 (IEEE 1722 security amendment, 17.4)
EECF_TIMESTAMP_SIZE = 8

# ESCF subtype (IEEE 1722-2025 Clause 16, as the amendment reads it)
ESCF_SUBTYPE = 0xEC
ESCF_HEADER_SIZE = 84  # through signature_value (Figure 109)
ESCF_PAYLOAD_OFFSET = ETHERNET_HEADER_SIZE + ESCF_HEADER_SIZE  # 98
ESCF_SIG_ECSSA = 0  # ECSSA_DSA_EMSA1_SHA256
ESCF_SIG_ED25519 = 1  # ED25519 (amendment 16.4)
ESCF_SIGNATURE_SIZE = 64
ESCF_MAX_ENCAPSULATED_AVTPDU = 1416  # 16.2.6
ESCF_TIMESTAMP_WINDOW_NS = 100_000_000  # 16.2.5 as amended (A3)


# ---------------------------------------------------------------------------
# Frame configuration (configurable global variables)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class FrameConfig:
    """Configurable network-level parameters for frame building/parsing.

    All MAC addresses are 6 bytes, entity IDs are 8-byte EUI-64.
    Default MACs use OUI-36 70:B3:D5:ED:CF:Fx (up to 16 devices).
    Default entity IDs use EUI-64 70:B3:D5:FF:FE:ED:CF:Fx.
    """

    controller_mac: bytes = b"\x70\xb3\xd5\xed\xcf\xf0"
    talker_mac: bytes = b"\x70\xb3\xd5\xed\xcf\xf1"
    listener1_mac: bytes = b"\x70\xb3\xd5\xed\xcf\xf2"
    listener2_mac: bytes = b"\x70\xb3\xd5\xed\xcf\xf3"

    controller_entity_id: bytes = b"\x70\xb3\xd5\xff\xfe\xed\xcf\xf0"
    talker_entity_id: bytes = b"\x70\xb3\xd5\xff\xfe\xed\xcf\xf1"
    listener1_entity_id: bytes = b"\x70\xb3\xd5\xff\xfe\xed\xcf\xf2"
    listener2_entity_id: bytes = b"\x70\xb3\xd5\xff\xfe\xed\xcf\xf3"

    starting_sequence_id: int = 0

    # AEM command codes (IEEE 1722.1-2021 §7.4.103-104)
    auth_get_nonce_command: int = 0x0067
    auth_add_key_nonce_command: int = 0x0068

    def mac_for(self, name: str) -> bytes:
        """Look up MAC address by entity name."""
        return _NAME_ATTR_MAP[name][0].__get__(self)

    def entity_id_for(self, name: str) -> bytes:
        """Look up entity ID by entity name."""
        return _NAME_ATTR_MAP[name][1].__get__(self)


# Mapping from entity name to (mac_property, entity_id_property) descriptors.
# Built lazily after the class is defined.
_NAME_ATTR_MAP: dict = {}


def _init_name_map():
    pairs = {
        "controller": ("controller_mac", "controller_entity_id"),
        "talker": ("talker_mac", "talker_entity_id"),
        "listener1": ("listener1_mac", "listener1_entity_id"),
        "listener2": ("listener2_mac", "listener2_entity_id"),
    }
    for name, (mac_attr, eid_attr) in pairs.items():
        _NAME_ATTR_MAP[name] = (
            _AttrGetter(mac_attr),
            _AttrGetter(eid_attr),
        )


class _AttrGetter:
    """Simple descriptor-like object for attribute access by name."""

    def __init__(self, attr: str):
        self._attr = attr

    def __get__(self, obj):
        return getattr(obj, self._attr)


_init_name_map()

DEFAULT_CONFIG = FrameConfig()


# ---------------------------------------------------------------------------
# Frame builder functions
# ---------------------------------------------------------------------------


def _build_aem_frame(
    dest_mac: bytes,
    src_mac: bytes,
    message_type: int,
    status: int,
    target_entity_id: bytes,
    controller_entity_id: bytes,
    sequence_id: int,
    command_code: int,
    payload: bytes,
) -> bytes:
    """Build a complete Ethernet + AemDu + payload frame.

    Wire layout matches AemDu from atdecc_aecp_aem.cppm exactly:
      Byte 0: subtype (0xFB)
      Byte 1: sv(0)|version(0)|message_type[3:0]
      Byte 2: status[7:3]|control_data_length[10:8]
      Byte 3: control_data_length[7:0]
      Bytes 4-11: target_entity_id
      Bytes 12-19: controller_entity_id
      Bytes 20-21: sequence_id (big-endian)
      Bytes 22-23: u(0)|command_type[14:0] (big-endian)
    """
    cdl = AEM_DATA_LENGTH + len(payload)

    # Ethernet header (14 bytes)
    eth = dest_mac + src_mac + struct.pack("!H", AVTP_ETHERTYPE)

    # AemDu header (24 bytes)
    byte0 = AECP_SUBTYPE
    byte1 = message_type & 0x0F
    byte2 = ((status & 0x1F) << 3) | ((cdl >> 8) & 0x07)
    byte3 = cdl & 0xFF
    aem = (
        struct.pack("BBBB", byte0, byte1, byte2, byte3)
        + target_entity_id
        + controller_entity_id
        + struct.pack("!HH", sequence_id & 0xFFFF, command_code & 0x7FFF)
    )

    return eth + aem + payload


def build_auth_get_nonce_frame(
    controller_nonce: bytes,
    *,
    dest_mac: bytes,
    src_mac: bytes,
    target_entity_id: bytes,
    controller_entity_id: bytes,
    sequence_id: int,
    config: FrameConfig = DEFAULT_CONFIG,
) -> bytes:
    """Build AUTH_GET_NONCE command frame (46 bytes total)."""
    return _build_aem_frame(
        dest_mac,
        src_mac,
        AECP_MESSAGE_TYPE_AEM_COMMAND,
        AEM_STATUS_SUCCESS,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        config.auth_get_nonce_command,
        controller_nonce,
    )


def build_auth_get_nonce_response_frame(
    controller_nonce: bytes,
    target_nonce: bytes,
    *,
    dest_mac: bytes,
    src_mac: bytes,
    target_entity_id: bytes,
    controller_entity_id: bytes,
    sequence_id: int,
    status: int = AEM_STATUS_SUCCESS,
    config: FrameConfig = DEFAULT_CONFIG,
) -> bytes:
    """Build AUTH_GET_NONCE response frame (54 bytes total)."""
    return _build_aem_frame(
        dest_mac,
        src_mac,
        AECP_MESSAGE_TYPE_AEM_RESPONSE,
        status,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        config.auth_get_nonce_command,
        controller_nonce + target_nonce,
    )


def build_auth_add_key_nonce_frame(
    ecies_ciphertext: bytes,
    *,
    dest_mac: bytes,
    src_mac: bytes,
    target_entity_id: bytes,
    controller_entity_id: bytes,
    sequence_id: int,
    config: FrameConfig = DEFAULT_CONFIG,
) -> bytes:
    """Build AUTH_ADD_KEY_NONCE command frame (38 + len(ciphertext) bytes)."""
    return _build_aem_frame(
        dest_mac,
        src_mac,
        AECP_MESSAGE_TYPE_AEM_COMMAND,
        AEM_STATUS_SUCCESS,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        config.auth_add_key_nonce_command,
        ecies_ciphertext,
    )


def build_auth_add_key_nonce_response_frame(
    controller_nonce: bytes,
    target_nonce: bytes,
    key_id: bytes,
    *,
    dest_mac: bytes,
    src_mac: bytes,
    target_entity_id: bytes,
    controller_entity_id: bytes,
    sequence_id: int,
    status: int = AEM_STATUS_SUCCESS,
    config: FrameConfig = DEFAULT_CONFIG,
) -> bytes:
    """Build AUTH_ADD_KEY_NONCE response frame (62 bytes total)."""
    return _build_aem_frame(
        dest_mac,
        src_mac,
        AECP_MESSAGE_TYPE_AEM_RESPONSE,
        status,
        target_entity_id,
        controller_entity_id,
        sequence_id,
        config.auth_add_key_nonce_command,
        controller_nonce + target_nonce + key_id,
    )


# ---------------------------------------------------------------------------
# Frame parser
# ---------------------------------------------------------------------------


@dataclass
class ParsedAemFrame:
    """Parsed fields from a complete Ethernet + AemDu frame."""

    dest_mac: bytes
    src_mac: bytes
    ethertype: int
    subtype: int
    message_type: int
    status: int
    control_data_length: int
    target_entity_id: bytes
    controller_entity_id: bytes
    sequence_id: int
    command_code: int
    payload: bytes


def parse_aem_frame(frame: bytes) -> ParsedAemFrame:
    """Parse a raw Ethernet frame into structured AEM fields.

    Raises ValueError if the frame is too short, or the ethertype/subtype
    do not match expected AVTP/AECP values.
    """
    if len(frame) < AEM_PAYLOAD_OFFSET:
        raise ValueError(
            f"Frame too short: {len(frame)} bytes, need at least {AEM_PAYLOAD_OFFSET}"
        )

    dest_mac = frame[0:6]
    src_mac = frame[6:12]
    (ethertype,) = struct.unpack_from("!H", frame, 12)

    if ethertype != AVTP_ETHERTYPE:
        raise ValueError(
            f"Wrong ethertype: 0x{ethertype:04x}, expected 0x{AVTP_ETHERTYPE:04x}"
        )

    subtype = frame[14]
    if subtype != AECP_SUBTYPE:
        raise ValueError(
            f"Wrong subtype: 0x{subtype:02x}, expected 0x{AECP_SUBTYPE:02x}"
        )

    byte1 = frame[15]
    message_type = byte1 & 0x0F

    byte2 = frame[16]
    byte3 = frame[17]
    status = (byte2 >> 3) & 0x1F
    control_data_length = ((byte2 & 0x07) << 8) | byte3

    target_entity_id = frame[18:26]
    controller_entity_id = frame[26:34]
    (sequence_id, cmd_type_raw) = struct.unpack_from("!HH", frame, 34)
    command_code = cmd_type_raw & 0x7FFF

    payload = frame[AEM_PAYLOAD_OFFSET:]

    return ParsedAemFrame(
        dest_mac=dest_mac,
        src_mac=src_mac,
        ethertype=ethertype,
        subtype=subtype,
        message_type=message_type,
        status=status,
        control_data_length=control_data_length,
        target_entity_id=target_entity_id,
        controller_entity_id=controller_entity_id,
        sequence_id=sequence_id,
        command_code=command_code,
        payload=payload,
    )


def extract_auth_get_nonce(parsed: ParsedAemFrame) -> bytes:
    """Extract controller_nonce (8 bytes) from AUTH_GET_NONCE command frame."""
    if len(parsed.payload) < 8:
        raise ValueError(f"AUTH_GET_NONCE payload too short: {len(parsed.payload)}")
    return parsed.payload[:8]


def extract_auth_get_nonce_response(
    parsed: ParsedAemFrame,
) -> tuple[bytes, bytes]:
    """Extract (controller_nonce, target_nonce) from AUTH_GET_NONCE response."""
    if len(parsed.payload) < 16:
        raise ValueError(
            f"AUTH_GET_NONCE response payload too short: {len(parsed.payload)}"
        )
    return parsed.payload[:8], parsed.payload[8:16]


def extract_auth_add_key_nonce_ciphertext(parsed: ParsedAemFrame) -> bytes:
    """Extract ECIES ciphertext from AUTH_ADD_KEY_NONCE command frame."""
    if len(parsed.payload) < sc.x25519_ecies_fixed_overhead + 16:
        raise ValueError(
            f"AUTH_ADD_KEY_NONCE ciphertext too short: {len(parsed.payload)}"
        )
    return parsed.payload


def extract_auth_add_key_nonce_response(
    parsed: ParsedAemFrame,
) -> tuple[bytes, bytes, bytes]:
    """Extract (controller_nonce, target_nonce, key_id) from response."""
    if len(parsed.payload) < 24:
        raise ValueError(
            f"AUTH_ADD_KEY_NONCE response payload too short: {len(parsed.payload)}"
        )
    return parsed.payload[:8], parsed.payload[8:16], parsed.payload[16:24]


# ---------------------------------------------------------------------------
# EECF frame builder/parser (IEEE 1722-2025 Clause 17)
# ---------------------------------------------------------------------------


@dataclass
class ParsedEecfFrame:
    """Parsed fields from a complete Ethernet + EECF frame."""

    dest_mac: bytes
    src_mac: bytes
    ethertype: int
    subtype: int
    enc: int
    encrypted_payload_length: int
    key_id: bytes
    header: bytes  # the 12-octet EECF header as received (the ECC2 / ECC3 HPKE aad)
    encrypted_payload: bytes


def build_eecf_frame(
    dest_mac: bytes,
    src_mac: bytes,
    enc: int,
    key_id: bytes,
    encrypted_payload: bytes,
) -> bytes:
    """Build a complete Ethernet + EECF frame.

    Wire layout (after 14-byte Ethernet header, Figure 110):
      Byte 0:     subtype (0xED)
      Byte 1:     r(1)|version(3)|enc(4)
      Bytes 2-3:  encrypted_payload_length (16 bits)
      Bytes 4-11: key_id (8 bytes, EUI-64)
      Bytes 12+:  encrypted_payload
    """
    epl = len(encrypted_payload)
    eth = dest_mac + src_mac + struct.pack("!H", AVTP_ETHERTYPE)

    byte0 = EECF_SUBTYPE
    byte1 = enc & 0x0F  # r=0, version=0, enc

    hdr = struct.pack("!BBH", byte0, byte1, epl) + key_id

    return eth + hdr + encrypted_payload


def parse_eecf_frame(frame: bytes) -> ParsedEecfFrame:
    """Parse a raw Ethernet frame as an EECF frame.

    Raises ValueError if frame is too short or has wrong ethertype/subtype.
    """
    if len(frame) < EECF_PAYLOAD_OFFSET:
        raise ValueError(f"EECF frame too short: {len(frame)} < {EECF_PAYLOAD_OFFSET}")

    dest_mac = frame[0:6]
    src_mac = frame[6:12]
    (ethertype,) = struct.unpack_from("!H", frame, 12)
    if ethertype != AVTP_ETHERTYPE:
        raise ValueError(f"Wrong ethertype: 0x{ethertype:04x}")

    subtype = frame[14]
    if subtype != EECF_SUBTYPE:
        raise ValueError(
            f"Wrong subtype: 0x{subtype:02x}, expected 0x{EECF_SUBTYPE:02x}"
        )

    enc = frame[15] & 0x0F
    (epl,) = struct.unpack_from("!H", frame, 16)
    key_id = frame[18:26]
    encrypted_payload = frame[EECF_PAYLOAD_OFFSET:]

    return ParsedEecfFrame(
        dest_mac=dest_mac,
        src_mac=src_mac,
        ethertype=ethertype,
        subtype=subtype,
        enc=enc,
        encrypted_payload_length=epl,
        key_id=key_id,
        header=frame[ETHERNET_HEADER_SIZE:EECF_PAYLOAD_OFFSET],
        encrypted_payload=encrypted_payload,
    )


# ECC1 encrypted_payload (IEEE 1722-2025 17.3.2): V (33), T (32), then C.
# statusbar_crypto's ecies_encrypt/ecies_decrypt speak the IEEE 1363a-2004
# triple as V || C || T; these two helpers are where the orders meet.
ECC1_V_SIZE = 33
ECC1_TAG_SIZE = 32
ECC1_OVERHEAD = ECC1_V_SIZE + ECC1_TAG_SIZE  # 65


def ecc1_payload_from_triple(vct: bytes) -> bytes:
    """IEEE 1363a triple V || C || T -> EECF ECC1 encrypted_payload V || T || C."""
    v, c, t = vct[:ECC1_V_SIZE], vct[ECC1_V_SIZE:-ECC1_TAG_SIZE], vct[-ECC1_TAG_SIZE:]
    return v + t + c


def ecc1_payload_to_triple(encrypted_payload: bytes) -> bytes:
    """EECF ECC1 encrypted_payload V || T || C -> IEEE 1363a triple V || C || T."""
    if len(encrypted_payload) < ECC1_OVERHEAD + 16 or (
        (len(encrypted_payload) - ECC1_OVERHEAD) % 16
    ):
        raise ValueError(
            f"not an ECC1 encrypted_payload: {len(encrypted_payload)} octets"
        )
    v = encrypted_payload[:ECC1_V_SIZE]
    t = encrypted_payload[ECC1_V_SIZE:ECC1_OVERHEAD]
    c = encrypted_payload[ECC1_OVERHEAD:]
    return v + c + t


def eecf_ecc1_encrypt_payload(
    recipient_pk: sc.P256PublicKey, plaintext: bytes, entropy: bytes
) -> bytes:
    """ECC1 encrypted_payload (V || T || C) for an arbitrary plaintext."""
    return ecc1_payload_from_triple(sc.ecies_encrypt(recipient_pk, plaintext, entropy))


def eecf_ecc1_decrypt_payload(
    recipient_sk: sc.P256PrivateKey, encrypted_payload: bytes
) -> bytes:
    """Plaintext of an ECC1 encrypted_payload (V || T || C)."""
    return sc.ecies_decrypt(recipient_sk, ecc1_payload_to_triple(encrypted_payload))


def eecf_encrypt(
    inner_avtpdu: bytes,
    recipient_pk: sc.P256PublicKey,
    key_id: bytes,
    timestamp: int,
    entropy: bytes,
) -> bytes:
    """Encrypt an inner AVTPDU for EECF ECC1 encapsulation.

    The ECC1 plaintext is: encryption_timestamp(8) || encapsulated_avtpdu (17.3.3).
    Returns the encrypted_payload laid out as V || T || C (17.3.2).
    """
    ts_bytes = struct.pack("!Q", timestamp)
    plaintext = ts_bytes + inner_avtpdu
    return eecf_ecc1_encrypt_payload(recipient_pk, plaintext, entropy)


def eecf_decrypt(
    encrypted_payload: bytes,
    recipient_sk: sc.P256PrivateKey,
) -> tuple[int, bytes]:
    """Decrypt an EECF ECC1 encrypted_payload (V || T || C).

    Returns (encryption_timestamp, encapsulated_avtpdu).
    """
    plaintext = eecf_ecc1_decrypt_payload(recipient_sk, encrypted_payload)
    if len(plaintext) < EECF_TIMESTAMP_SIZE:
        raise ValueError("EECF decrypted payload too short for timestamp")
    (timestamp,) = struct.unpack("!Q", plaintext[:EECF_TIMESTAMP_SIZE])
    avtpdu = plaintext[EECF_TIMESTAMP_SIZE:]
    return timestamp, avtpdu


# ECC2 encrypted_payload (IEEE 1722 security amendment 17.4, avb/docs/IEEE_1722_CRYPTO_AMENDMENT.md):
# RFC 9180 HPKE base mode with DHKEM(P-256, HKDF-SHA256) / HKDF-SHA256 / AES-256-GCM,
# laid out as enc (65) || C || T (16). The HPKE info is a fixed string; the HPKE aad is
# the frame's 12-octet EECF header, so enc, encrypted_payload_length and key_id are
# authenticated with the payload.
ECC2_ENC_SIZE = 65
ECC2_TAG_SIZE = 16
ECC2_OVERHEAD = ECC2_ENC_SIZE + ECC2_TAG_SIZE  # 81
ECC2_INFO = b"IEEE 1722 EECF ECC2"


def eecf_ecc2_header(key_id: bytes, encrypted_payload_size: int) -> bytes:
    """The EECF header an ECC2 payload is bound to: what build_eecf_frame writes for enc = ECC2."""
    return build_eecf_frame(
        bytes(6), bytes(6), EECF_ENC_ECC2, key_id, bytes(encrypted_payload_size)
    )[ETHERNET_HEADER_SIZE:EECF_PAYLOAD_OFFSET]


def eecf_ecc2_encrypt_payload(
    recipient_pk: sc.P256PublicKey, aad: bytes, plaintext: bytes, ikm_e: bytes
) -> bytes:
    """ECC2 encrypted_payload (enc || C || T) for an arbitrary plaintext and aad."""
    enc, ctx = sc.hpke_setup_base_s(
        sc.hpke_aead_aes256_gcm, recipient_pk, ECC2_INFO, ikm_e
    )
    tag, ct = sc.hpke_seal(ctx, aad, plaintext)
    return enc + ct + tag


def eecf_ecc2_decrypt_payload(
    recipient_sk: sc.P256PrivateKey, aad: bytes, encrypted_payload: bytes
) -> bytes:
    """Plaintext of an ECC2 encrypted_payload; ValueError on a bad shape or tag."""
    if len(encrypted_payload) < ECC2_OVERHEAD:
        raise ValueError(
            f"not an ECC2 encrypted_payload: {len(encrypted_payload)} octets"
        )
    ctx = sc.hpke_setup_base_r(
        sc.hpke_aead_aes256_gcm,
        encrypted_payload[:ECC2_ENC_SIZE],
        recipient_sk,
        ECC2_INFO,
    )
    ok, plaintext = sc.hpke_open(
        ctx,
        aad,
        encrypted_payload[ECC2_ENC_SIZE:-ECC2_TAG_SIZE],
        encrypted_payload[-ECC2_TAG_SIZE:],
    )
    if not ok:
        raise ValueError("ECC2 tag verification failed")
    return plaintext


def eecf_ecc2_encrypt(
    inner_avtpdu: bytes,
    recipient_pk: sc.P256PublicKey,
    key_id: bytes,
    timestamp: int,
    ikm_e: bytes,
) -> bytes:
    """Encrypt an inner AVTPDU for EECF ECC2 encapsulation under key_id.

    The plaintext is encryption_timestamp(8) || encapsulated_avtpdu (17.3.3); the
    aad is the EECF header the frame will carry. Returns enc || C || T.
    """
    plaintext = struct.pack("!Q", timestamp) + inner_avtpdu
    aad = eecf_ecc2_header(key_id, len(plaintext) + ECC2_OVERHEAD)
    return eecf_ecc2_encrypt_payload(recipient_pk, aad, plaintext, ikm_e)


def eecf_ecc2_decrypt(
    encrypted_payload: bytes, header: bytes, recipient_sk: sc.P256PrivateKey
) -> tuple[int, bytes]:
    """Decrypt an ECC2 encrypted_payload received under the 12-octet EECF header.

    Returns (encryption_timestamp, encapsulated_avtpdu).
    """
    plaintext = eecf_ecc2_decrypt_payload(recipient_sk, header, encrypted_payload)
    if len(plaintext) < EECF_TIMESTAMP_SIZE:
        raise ValueError("EECF decrypted payload too short for timestamp")
    (timestamp,) = struct.unpack("!Q", plaintext[:EECF_TIMESTAMP_SIZE])
    return timestamp, plaintext[EECF_TIMESTAMP_SIZE:]


# ECC3 encrypted_payload (amendment 17.4): ECC2 with DHKEM(X25519, HKDF-SHA256);
# enc is the 32-octet X25519 public key. A framed payload must be at least Npk + 24
# (17.4.3 step 2).
ECC3_ENC_SIZE = 32
ECC3_TAG_SIZE = 16
ECC3_OVERHEAD = ECC3_ENC_SIZE + ECC3_TAG_SIZE  # 48
ECC3_INFO = b"IEEE 1722 EECF ECC3"
ECC3_MIN_FRAMED_SIZE = ECC3_OVERHEAD + EECF_TIMESTAMP_SIZE  # 56
ECC2_MIN_FRAMED_SIZE = ECC2_OVERHEAD + EECF_TIMESTAMP_SIZE  # 89


def eecf_ecc3_header(key_id: bytes, encrypted_payload_size: int) -> bytes:
    """The EECF header an ECC3 payload is bound to: what build_eecf_frame writes for enc = ECC3."""
    return build_eecf_frame(
        bytes(6), bytes(6), EECF_ENC_ECC3, key_id, bytes(encrypted_payload_size)
    )[ETHERNET_HEADER_SIZE:EECF_PAYLOAD_OFFSET]


def eecf_ecc3_encrypt_payload(
    recipient_pk: sc.X25519PublicKey, aad: bytes, plaintext: bytes, ikm_e: bytes
) -> bytes:
    """ECC3 encrypted_payload (enc || C || T) for an arbitrary plaintext and aad."""
    enc, ctx = sc.hpke_setup_base_s(
        sc.hpke_aead_aes256_gcm, recipient_pk, ECC3_INFO, ikm_e
    )
    tag, ct = sc.hpke_seal(ctx, aad, plaintext)
    return enc + ct + tag


def eecf_ecc3_decrypt_payload(
    recipient_sk: sc.X25519PrivateKey, aad: bytes, encrypted_payload: bytes
) -> bytes:
    """Plaintext of an ECC3 encrypted_payload; ValueError on a bad shape, enc or tag."""
    if len(encrypted_payload) < ECC3_OVERHEAD:
        raise ValueError(
            f"not an ECC3 encrypted_payload: {len(encrypted_payload)} octets"
        )
    ctx = sc.hpke_setup_base_r(
        sc.hpke_aead_aes256_gcm,
        encrypted_payload[:ECC3_ENC_SIZE],
        recipient_sk,
        ECC3_INFO,
    )
    ok, plaintext = sc.hpke_open(
        ctx,
        aad,
        encrypted_payload[ECC3_ENC_SIZE:-ECC3_TAG_SIZE],
        encrypted_payload[-ECC3_TAG_SIZE:],
    )
    if not ok:
        raise ValueError("ECC3 tag verification failed")
    return plaintext


def eecf_ecc3_encrypt(
    inner_avtpdu: bytes,
    recipient_pk: sc.X25519PublicKey,
    key_id: bytes,
    timestamp: int,
    ikm_e: bytes,
) -> bytes:
    """Encrypt an inner AVTPDU for EECF ECC3 encapsulation under key_id. Returns enc || C || T."""
    plaintext = struct.pack("!Q", timestamp) + inner_avtpdu
    aad = eecf_ecc3_header(key_id, len(plaintext) + ECC3_OVERHEAD)
    return eecf_ecc3_encrypt_payload(recipient_pk, aad, plaintext, ikm_e)


def eecf_ecc3_decrypt(
    encrypted_payload: bytes, header: bytes, recipient_sk: sc.X25519PrivateKey
) -> tuple[int, bytes]:
    """Decrypt an ECC3 encrypted_payload received under the 12-octet EECF header.

    Returns (encryption_timestamp, encapsulated_avtpdu); ValueError below Npk + 24.
    """
    if len(encrypted_payload) < ECC3_MIN_FRAMED_SIZE:
        raise ValueError("ECC3 encrypted_payload shorter than Npk + 24 (17.4.3 step 2)")
    plaintext = eecf_ecc3_decrypt_payload(recipient_sk, header, encrypted_payload)
    (timestamp,) = struct.unpack("!Q", plaintext[:EECF_TIMESTAMP_SIZE])
    return timestamp, plaintext[EECF_TIMESTAMP_SIZE:]


# ---------------------------------------------------------------------------
# ESCF frame builder/parser and signatures (IEEE 1722-2025 Clause 16, amended)
# ---------------------------------------------------------------------------


@dataclass
class ParsedEscfFrame:
    """Parsed fields from a complete Ethernet + ESCF frame."""

    dest_mac: bytes
    src_mac: bytes
    ethertype: int
    subtype: int
    sig: int
    encapsulated_avtpdu_length: int
    key_id: bytes
    signing_timestamp: int
    signature_value: bytes
    encapsulated_avtpdu: bytes  # bounded by encapsulated_avtpdu_length


def escf_signed_message(key_id: bytes, signing_timestamp: int, avtpdu: bytes) -> bytes:
    """key_id || signing_timestamp || encapsulated_avtpdu (16.3.1, 16.4.1)."""
    if len(avtpdu) > ESCF_MAX_ENCAPSULATED_AVTPDU:
        raise ValueError(f"encapsulated_avtpdu over 16.2.6's limit: {len(avtpdu)}")
    return key_id + struct.pack("!Q", signing_timestamp) + avtpdu


def escf_ecssa_sign(
    sk: sc.P256PrivateKey, key_id: bytes, signing_timestamp: int, avtpdu: bytes
) -> bytes:
    """ECSSA_DSA_EMSA1_SHA256 (16.3.2): P-256 ECDSA over SHA-256; signature_value = c || d."""
    return sc.p256_ecdsa_sign(
        sk, escf_signed_message(key_id, signing_timestamp, avtpdu)
    ).data


def escf_ecssa_verify(
    pk: sc.P256PublicKey,
    key_id: bytes,
    signing_timestamp: int,
    avtpdu: bytes,
    signature_value: bytes,
) -> bool:
    """ECSSA_DSA_EMSA1_SHA256 (16.3.3)."""
    return sc.p256_ecdsa_verify(
        pk,
        escf_signed_message(key_id, signing_timestamp, avtpdu),
        sc.P256EcdsaSignature(data=signature_value),
    )


def escf_ed25519_sign(
    sk: sc.Ed25519PrivateKey, key_id: bytes, signing_timestamp: int, avtpdu: bytes
) -> bytes:
    """ED25519 (amendment 16.4.2): RFC 8032 5.1.6; signature_value = R || S."""
    return sc.ed25519_sign(
        sk, escf_signed_message(key_id, signing_timestamp, avtpdu)
    ).data


def escf_ed25519_verify(
    pk: sc.Ed25519PublicKey,
    key_id: bytes,
    signing_timestamp: int,
    avtpdu: bytes,
    signature_value: bytes,
) -> bool:
    """ED25519 (amendment 16.4.3): RFC 8032 5.1.7."""
    return sc.ed25519_verify(
        pk,
        escf_signed_message(key_id, signing_timestamp, avtpdu),
        sc.Ed25519Signature(data=signature_value),
    )


def escf_signing_timestamp_is_acceptable(
    signing_timestamp: int, now_ptp_ns: int
) -> bool:
    """16.2.5 as amended (A3): not later than now, and not more than 100 ms earlier."""
    return (
        signing_timestamp <= now_ptp_ns
        and now_ptp_ns - signing_timestamp <= ESCF_TIMESTAMP_WINDOW_NS
    )


def build_escf_frame(
    dest_mac: bytes,
    src_mac: bytes,
    sig: int,
    key_id: bytes,
    signing_timestamp: int,
    signature_value: bytes,
    encapsulated_avtpdu: bytes,
) -> bytes:
    """Build a complete Ethernet + ESCF frame (Figure 109).

    Wire layout (after 14-byte Ethernet header):
      Byte 0:      subtype (0xEC)
      Byte 1:      r(1)|version(3)|sig(4)
      Bytes 2-3:   encapsulated_avtpdu_length (16 bits)
      Bytes 4-11:  key_id (EUI-64)
      Bytes 12-19: signing_timestamp
      Bytes 20-83: signature_value
      Bytes 84+:   encapsulated_avtpdu
    """
    if len(signature_value) != ESCF_SIGNATURE_SIZE:
        raise ValueError("signature_value must be 64 octets")
    if len(encapsulated_avtpdu) > ESCF_MAX_ENCAPSULATED_AVTPDU:
        raise ValueError("encapsulated_avtpdu over 16.2.6's limit")
    eth = dest_mac + src_mac + struct.pack("!H", AVTP_ETHERTYPE)
    hdr = (
        struct.pack("!BBH", ESCF_SUBTYPE, sig & 0x0F, len(encapsulated_avtpdu))
        + key_id
        + struct.pack("!Q", signing_timestamp)
        + signature_value
    )
    return eth + hdr + encapsulated_avtpdu


def parse_escf_frame(frame: bytes) -> ParsedEscfFrame:
    """Parse a raw Ethernet frame as an ESCF frame; ValueError when malformed."""
    if len(frame) < ESCF_PAYLOAD_OFFSET:
        raise ValueError(f"ESCF frame too short: {len(frame)} < {ESCF_PAYLOAD_OFFSET}")
    (ethertype,) = struct.unpack_from("!H", frame, 12)
    if ethertype != AVTP_ETHERTYPE:
        raise ValueError(f"Wrong ethertype: 0x{ethertype:04x}")
    if frame[14] != ESCF_SUBTYPE or (frame[15] & 0x70) != 0:
        raise ValueError("Not a version-0 ESCF AVTPDU")
    (length,) = struct.unpack_from("!H", frame, 16)
    if ESCF_PAYLOAD_OFFSET + length > len(frame):
        raise ValueError("encapsulated_avtpdu_length past the end of the frame")
    (signing_timestamp,) = struct.unpack_from("!Q", frame, 26)
    return ParsedEscfFrame(
        dest_mac=frame[0:6],
        src_mac=frame[6:12],
        ethertype=ethertype,
        subtype=frame[14],
        sig=frame[15] & 0x0F,
        encapsulated_avtpdu_length=length,
        key_id=frame[18:26],
        signing_timestamp=signing_timestamp,
        signature_value=frame[34:98],
        encapsulated_avtpdu=frame[ESCF_PAYLOAD_OFFSET : ESCF_PAYLOAD_OFFSET + length],
    )


# ---------------------------------------------------------------------------
# Frame dump utility
# ---------------------------------------------------------------------------

DUMP_FRAMES = False
PCAP_FILE: Optional[BinaryIO] = None


def _pcap_write_header(f: BinaryIO) -> None:
    """Write pcap global header (LINKTYPE_ETHERNET = 1)."""
    f.write(
        struct.pack(
            "<IHHiIII",
            0xA1B2C3D4,  # magic number
            2,  # version major
            4,  # version minor
            0,  # thiszone (GMT)
            0,  # sigfigs
            65535,  # snaplen
            1,  # network (LINKTYPE_ETHERNET)
        )
    )


def _pcap_write_packet(f: BinaryIO, frame: bytes) -> None:
    """Write a single packet record to the pcap file."""
    now = time.time()
    ts_sec = int(now)
    ts_usec = int((now - ts_sec) * 1_000_000)
    f.write(struct.pack("<IIII", ts_sec, ts_usec, len(frame), len(frame)))
    f.write(frame)
    f.flush()


def _command_code_name(code: int) -> str:
    """Get human-readable name for an AEM command code."""
    names = {0x0067: "AUTH_GET_NONCE", 0x0068: "AUTH_ADD_KEY_NONCE"}
    return names.get(code, f"0x{code:04x}")


def _sanitize_label(label: str) -> str:
    """Convert a human label to a valid Python variable name."""
    result = label.lower()
    for ch in " :->()/":
        result = result.replace(ch, "_")
    while "__" in result:
        result = result.replace("__", "_")
    return result.strip("_")


def _format_hex_line(data: bytes, comment: str) -> str:
    """Format one line of hex bytes with a trailing comment."""
    hex_bytes = ", ".join(f"0x{b:02x}" for b in data)
    return f"    {hex_bytes},  # {comment}"


def dump_frame(label: str, frame: bytes) -> None:
    """If DUMP_FRAMES is enabled, print a frame as a Python bytes literal.
    If PCAP_FILE is set, also write the frame to the pcap file."""
    if PCAP_FILE is not None:
        _pcap_write_packet(PCAP_FILE, frame)
    if not DUMP_FRAMES:
        return

    # Detect EECF vs AEM by subtype at byte 14
    if len(frame) >= EECF_PAYLOAD_OFFSET and frame[14] == EECF_SUBTYPE:
        _dump_eecf_frame(label, frame)
    else:
        _dump_aem_frame(label, frame)


def _dump_aem_frame(label: str, frame: bytes) -> None:
    """Print an AECP AEM frame as a Python bytes literal."""
    parsed = parse_aem_frame(frame)
    cmd_name = _command_code_name(parsed.command_code)
    direction = "Command" if parsed.message_type == 0 else "Response"

    print(
        f"\n# {label}\n"
        f"# {cmd_name} {direction} (seq={parsed.sequence_id}, "
        f"cdl={parsed.control_data_length}, total={len(frame)} bytes)"
    )
    var_name = _sanitize_label(label)
    print(f"{var_name} = bytes([")

    # Ethernet header
    print(_format_hex_line(frame[0:6], f"dest: {frame[0:6].hex()}"))
    print(_format_hex_line(frame[6:12], f"src: {frame[6:12].hex()}"))
    print(_format_hex_line(frame[12:14], "ethertype (AVTP 0x22f0)"))

    # AemDu header
    print(
        _format_hex_line(
            frame[14:18],
            f"AECP {direction}, status={parsed.status}, cdl={parsed.control_data_length}",
        )
    )
    print(_format_hex_line(frame[18:26], f"target_entity_id"))
    print(_format_hex_line(frame[26:34], f"controller_entity_id"))
    print(_format_hex_line(frame[34:36], f"seq={parsed.sequence_id}"))
    print(_format_hex_line(frame[36:38], f"cmd={cmd_name}"))

    # Payload — break into 8-byte lines for readability
    payload = parsed.payload
    offset = 0
    while offset < len(payload):
        chunk = payload[offset : offset + 8]
        print(_format_hex_line(chunk, f"payload[{offset}:{offset + len(chunk)}]"))
        offset += 8

    print("])")


def _dump_eecf_frame(label: str, frame: bytes) -> None:
    """Print an EECF frame as a Python bytes literal."""
    parsed = parse_eecf_frame(frame)
    enc_names = {0: "ECC1"}
    enc_name = enc_names.get(parsed.enc, f"enc={parsed.enc}")

    print(
        f"\n# {label}\n"
        f"# EECF ({enc_name}, epl={parsed.encrypted_payload_length}, "
        f"key_id={parsed.key_id.hex()}, total={len(frame)} bytes)"
    )
    var_name = _sanitize_label(label)
    print(f"{var_name} = bytes([")

    # Ethernet header
    print(_format_hex_line(frame[0:6], f"dest: {frame[0:6].hex()}"))
    print(_format_hex_line(frame[6:12], f"src: {frame[6:12].hex()}"))
    print(_format_hex_line(frame[12:14], "ethertype (AVTP 0x22f0)"))

    # EECF header
    print(
        _format_hex_line(
            frame[14:18],
            f"EECF subtype=0xed, {enc_name}, epl={parsed.encrypted_payload_length}",
        )
    )
    print(_format_hex_line(frame[18:26], f"key_id: {parsed.key_id.hex()}"))

    # Encrypted payload — break into 8-byte lines
    ep = parsed.encrypted_payload
    offset = 0
    while offset < len(ep):
        chunk = ep[offset : offset + 8]
        print(
            _format_hex_line(
                chunk, f"encrypted_payload[{offset}:{offset + len(chunk)}]"
            )
        )
        offset += 8

    print("])")


# ---------------------------------------------------------------------------
# FrameController and FrameEntity wrappers
# ---------------------------------------------------------------------------


class FrameController:
    """Controller that wraps protocol messages in Ethernet frames."""

    def __init__(
        self,
        inner: Controller,
        entity_name: str,
        config: FrameConfig = DEFAULT_CONFIG,
        eecf_target_keys: Optional[dict] = None,
    ):
        self.inner = inner
        self.entity_name = entity_name
        self.config = config
        self._seq = config.starting_sequence_id
        self.eecf_target_keys = eecf_target_keys or {}

    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFFFF
        return seq

    def create_session_key(self, config: SessionKeyConfig) -> tuple[sc.KeyId, object]:
        """Delegate session key creation to the inner controller."""
        return self.inner.create_session_key(config)

    def build_auth_get_nonce(
        self, target_name: str, nonce_entropy: bytes
    ) -> tuple[bytes, sc.Nonce]:
        """Build AUTH_GET_NONCE as a complete Ethernet frame."""
        _payload_bytes, controller_nonce = self.inner.build_auth_get_nonce(
            nonce_entropy
        )
        frame = build_auth_get_nonce_frame(
            controller_nonce.data,
            dest_mac=self.config.mac_for(target_name),
            src_mac=self.config.mac_for(self.entity_name),
            target_entity_id=self.config.entity_id_for(target_name),
            controller_entity_id=self.config.entity_id_for(self.entity_name),
            sequence_id=self._next_seq(),
            config=self.config,
        )
        dump_frame(f"AUTH_GET_NONCE cmd: {self.entity_name} -> {target_name}", frame)
        return frame, controller_nonce

    def process_auth_get_nonce_response(
        self, frame: bytes, expected_nonce: sc.Nonce
    ) -> sc.AuthGetNonceResponsePayload:
        """Parse AUTH_GET_NONCE response frame and verify nonce."""
        dump_frame("AUTH_GET_NONCE response (received by controller)", frame)
        parsed = parse_aem_frame(frame)
        ctrl_nonce, tgt_nonce = extract_auth_get_nonce_response(parsed)
        # Reconstruct serialized response for inner protocol
        response_bytes = ctrl_nonce + tgt_nonce
        return self.inner.process_auth_get_nonce_response(
            response_bytes, expected_nonce
        )

    def build_auth_add_key_nonce(
        self,
        target_name: str,
        target_x25519_pk: sc.X25519PublicKey,
        controller_nonce: sc.Nonce,
        target_nonce: sc.Nonce,
        session_key_id: sc.KeyId,
        session_key: object,
        ecies_entropy: bytes,
        session_key_config: SessionKeyConfig,
        eecf_entropy: Optional[bytes] = None,
    ) -> bytes:
        """Build AUTH_ADD_KEY_NONCE as a complete Ethernet frame.

        If eecf_entropy is provided and the target has a registered EECF public
        key, the inner AEM AVTPDU is wrapped in an EECF encrypted control frame
        (IEEE 1722-2016 Clause 17).
        """
        ecies_ciphertext = self.inner.build_auth_add_key_nonce(
            target_x25519_pk,
            controller_nonce,
            target_nonce,
            session_key_id,
            session_key,
            ecies_entropy,
            session_key_config,
        )
        dest_mac = self.config.mac_for(target_name)
        src_mac = self.config.mac_for(self.entity_name)
        target_eid = self.config.entity_id_for(target_name)
        controller_eid = self.config.entity_id_for(self.entity_name)
        seq = self._next_seq()

        inner_frame = build_auth_add_key_nonce_frame(
            ecies_ciphertext,
            dest_mac=dest_mac,
            src_mac=src_mac,
            target_entity_id=target_eid,
            controller_entity_id=controller_eid,
            sequence_id=seq,
            config=self.config,
        )

        # Wrap in EECF if target has a registered EECF public key
        eecf_pk = self.eecf_target_keys.get(target_name)
        if eecf_pk is not None and eecf_entropy is not None:
            inner_avtpdu = inner_frame[ETHERNET_HEADER_SIZE:]
            timestamp = int(time.time() * 1e9) & 0xFFFFFFFFFFFFFFFF
            enc_payload = eecf_encrypt(
                inner_avtpdu,
                eecf_pk,
                target_eid,
                timestamp,
                eecf_entropy,
            )
            frame = build_eecf_frame(
                dest_mac,
                src_mac,
                EECF_ENC_ECC1,
                target_eid,
                enc_payload,
            )
            dump_frame(
                f"AUTH_ADD_KEY_NONCE EECF cmd: {self.entity_name} -> {target_name}",
                frame,
            )
        else:
            frame = inner_frame
            dump_frame(
                f"AUTH_ADD_KEY_NONCE cmd: {self.entity_name} -> {target_name}",
                frame,
            )
        return frame


class FrameEntity:
    """Entity that processes Ethernet-framed protocol messages."""

    def __init__(
        self,
        inner: Entity,
        entity_name: str,
        config: FrameConfig = DEFAULT_CONFIG,
        eecf_private_key=None,
    ):
        self.inner = inner
        self.entity_name = entity_name
        self.config = config
        self.eecf_private_key = eecf_private_key

    def process_auth_get_nonce(
        self, frame: bytes, nonce_entropy: bytes, controller_name: str
    ) -> bytes:
        """Process AUTH_GET_NONCE command frame, return response frame."""
        dump_frame(f"AUTH_GET_NONCE cmd (received by {self.entity_name})", frame)
        parsed = parse_aem_frame(frame)
        controller_nonce = extract_auth_get_nonce(parsed)

        # Delegate to inner protocol — it expects serialized command bytes
        response_payload = self.inner.process_auth_get_nonce(
            controller_nonce, nonce_entropy
        )

        # Parse the inner response to get both nonces
        resp = sc.deserialize_auth_get_nonce_response(response_payload)

        # Build response frame — swap src/dest MACs but keep entity IDs unchanged
        response_frame = build_auth_get_nonce_response_frame(
            resp.controller_nonce.data,
            resp.target_nonce.data,
            dest_mac=self.config.mac_for(controller_name),
            src_mac=self.config.mac_for(self.entity_name),
            target_entity_id=parsed.target_entity_id,
            controller_entity_id=parsed.controller_entity_id,
            sequence_id=parsed.sequence_id,
            config=self.config,
        )
        dump_frame(
            f"AUTH_GET_NONCE response: {self.entity_name} -> {controller_name}",
            response_frame,
        )
        return response_frame

    def process_auth_add_key_nonce(self, frame: bytes, controller_name: str) -> bytes:
        """Process AUTH_ADD_KEY_NONCE command frame, return response frame.

        If the incoming frame is an EECF frame (subtype 0xED), it is decrypted
        first to recover the inner AEM AVTPDU before processing.
        """
        # Detect EECF wrapping by checking subtype at byte 14
        if (
            len(frame) >= EECF_PAYLOAD_OFFSET
            and frame[14] == EECF_SUBTYPE
            and self.eecf_private_key is not None
        ):
            dump_frame(
                f"AUTH_ADD_KEY_NONCE EECF cmd (received by {self.entity_name})",
                frame,
            )
            parsed_eecf = parse_eecf_frame(frame)
            _timestamp, inner_avtpdu = eecf_decrypt(
                parsed_eecf.encrypted_payload,
                self.eecf_private_key,
            )
            # Reconstruct full Ethernet frame using original Ethernet header
            frame = frame[:ETHERNET_HEADER_SIZE] + inner_avtpdu

        dump_frame(f"AUTH_ADD_KEY_NONCE cmd (received by {self.entity_name})", frame)
        parsed = parse_aem_frame(frame)
        ecies_ciphertext = extract_auth_add_key_nonce_ciphertext(parsed)

        # Delegate decryption and key installation to inner protocol
        response = self.inner.process_auth_add_key_nonce(ecies_ciphertext)

        # Build response frame — swap MACs but keep entity IDs unchanged
        response_frame = build_auth_add_key_nonce_response_frame(
            response.controller_nonce.data,
            response.target_nonce.data,
            response.key_id.data,
            dest_mac=self.config.mac_for(controller_name),
            src_mac=self.config.mac_for(self.entity_name),
            target_entity_id=parsed.target_entity_id,
            controller_entity_id=parsed.controller_entity_id,
            sequence_id=parsed.sequence_id,
            config=self.config,
        )
        dump_frame(
            f"AUTH_ADD_KEY_NONCE response: {self.entity_name} -> {controller_name}",
            response_frame,
        )
        return response_frame


# ---------------------------------------------------------------------------
# Frame-level protocol orchestration
# ---------------------------------------------------------------------------


def run_frame_key_distribution(
    controller: AvbEntity,
    ctrl_frame: FrameController,
    target: AvbEntity,
    target_frame: FrameEntity,
    session_key_id: sc.KeyId,
    session_key: object,
    nonce_entropy: bytes,
    ecies_entropy: bytes,
    session_key_config: SessionKeyConfig,
    eecf_entropy: Optional[bytes] = None,
) -> sc.AuthAddKeyNonceResponsePayload:
    """Execute the complete AUTH protocol via Ethernet frames."""
    # Step 1: Controller builds AUTH_GET_NONCE frame
    cmd_frame, controller_nonce = ctrl_frame.build_auth_get_nonce(
        target.name, nonce_entropy
    )

    # Step 2: Entity processes the frame and returns response frame
    target_nonce_entropy = sc.sha256(nonce_entropy + target.name.encode())
    response_frame = target_frame.process_auth_get_nonce(
        cmd_frame, target_nonce_entropy, "controller"
    )

    # Step 3: Controller processes response frame
    response = ctrl_frame.process_auth_get_nonce_response(
        response_frame, controller_nonce
    )

    # Step 4: Controller builds encrypted AUTH_ADD_KEY_NONCE frame
    # If EECF entropy is provided and the controller has an EECF key for the
    # target, the frame is wrapped in an EECF encrypted control frame.
    encrypted_frame = ctrl_frame.build_auth_add_key_nonce(
        target.name,
        target.x25519_sk.public_key,
        response.controller_nonce,
        response.target_nonce,
        session_key_id,
        session_key,
        ecies_entropy,
        session_key_config,
        eecf_entropy=eecf_entropy,
    )

    # Step 5: Entity processes ADD_KEY_NONCE frame and returns response frame
    add_key_response_frame = target_frame.process_auth_add_key_nonce(
        encrypted_frame, "controller"
    )

    # Parse final response from frame
    parsed_resp = parse_aem_frame(add_key_response_frame)
    ctrl_n, tgt_n, kid = extract_auth_add_key_nonce_response(parsed_resp)
    return sc.AuthAddKeyNonceResponsePayload(
        controller_nonce=sc.Nonce(data=ctrl_n),
        target_nonce=sc.Nonce(data=tgt_n),
        key_id=sc.KeyId(data=kid),
    )


# ---------------------------------------------------------------------------
# Test cases: frame build/parse unit tests
# ---------------------------------------------------------------------------


class TestFrameBuildParse(unittest.TestCase):
    """Unit tests for frame builder/parser roundtrip consistency."""

    def test_auth_get_nonce_roundtrip(self):
        """Build an AUTH_GET_NONCE frame, parse it back, verify all fields."""
        nonce = bytes(range(8))
        frame = build_auth_get_nonce_frame(
            nonce,
            dest_mac=DEFAULT_CONFIG.talker_mac,
            src_mac=DEFAULT_CONFIG.controller_mac,
            target_entity_id=DEFAULT_CONFIG.talker_entity_id,
            controller_entity_id=DEFAULT_CONFIG.controller_entity_id,
            sequence_id=42,
        )
        self.assertEqual(len(frame), 46)
        parsed = parse_aem_frame(frame)
        self.assertEqual(parsed.dest_mac, DEFAULT_CONFIG.talker_mac)
        self.assertEqual(parsed.src_mac, DEFAULT_CONFIG.controller_mac)
        self.assertEqual(parsed.ethertype, AVTP_ETHERTYPE)
        self.assertEqual(parsed.subtype, AECP_SUBTYPE)
        self.assertEqual(parsed.message_type, AECP_MESSAGE_TYPE_AEM_COMMAND)
        self.assertEqual(parsed.status, AEM_STATUS_SUCCESS)
        self.assertEqual(parsed.control_data_length, 20)
        self.assertEqual(parsed.target_entity_id, DEFAULT_CONFIG.talker_entity_id)
        self.assertEqual(
            parsed.controller_entity_id, DEFAULT_CONFIG.controller_entity_id
        )
        self.assertEqual(parsed.sequence_id, 42)
        self.assertEqual(parsed.command_code, 0x0067)
        self.assertEqual(parsed.payload, nonce)

    def test_auth_get_nonce_response_roundtrip(self):
        """Build AUTH_GET_NONCE response, parse, verify."""
        ctrl_nonce = bytes(range(8))
        tgt_nonce = bytes(range(8, 16))
        frame = build_auth_get_nonce_response_frame(
            ctrl_nonce,
            tgt_nonce,
            dest_mac=DEFAULT_CONFIG.controller_mac,
            src_mac=DEFAULT_CONFIG.talker_mac,
            target_entity_id=DEFAULT_CONFIG.talker_entity_id,
            controller_entity_id=DEFAULT_CONFIG.controller_entity_id,
            sequence_id=42,
        )
        self.assertEqual(len(frame), 54)
        parsed = parse_aem_frame(frame)
        self.assertEqual(parsed.message_type, AECP_MESSAGE_TYPE_AEM_RESPONSE)
        self.assertEqual(parsed.control_data_length, 28)
        c, t = extract_auth_get_nonce_response(parsed)
        self.assertEqual(c, ctrl_nonce)
        self.assertEqual(t, tgt_nonce)

    def test_auth_add_key_nonce_roundtrip(self):
        """Build AUTH_ADD_KEY_NONCE command with dummy ciphertext, parse, verify."""
        # Shaped like a real X25519 ECIES output: V (32) || C (one block) || T (32).
        ciphertext = bytes(range(96))
        frame = build_auth_add_key_nonce_frame(
            ciphertext,
            dest_mac=DEFAULT_CONFIG.talker_mac,
            src_mac=DEFAULT_CONFIG.controller_mac,
            target_entity_id=DEFAULT_CONFIG.talker_entity_id,
            controller_entity_id=DEFAULT_CONFIG.controller_entity_id,
            sequence_id=43,
        )
        self.assertEqual(len(frame), 38 + 96)
        parsed = parse_aem_frame(frame)
        self.assertEqual(parsed.message_type, AECP_MESSAGE_TYPE_AEM_COMMAND)
        self.assertEqual(parsed.control_data_length, 12 + 96)
        self.assertEqual(parsed.command_code, 0x0068)
        self.assertEqual(extract_auth_add_key_nonce_ciphertext(parsed), ciphertext)

    def test_auth_add_key_nonce_response_roundtrip(self):
        """Build AUTH_ADD_KEY_NONCE response, parse, verify."""
        ctrl_nonce = bytes(range(8))
        tgt_nonce = bytes(range(8, 16))
        key_id = bytes(range(16, 24))
        frame = build_auth_add_key_nonce_response_frame(
            ctrl_nonce,
            tgt_nonce,
            key_id,
            dest_mac=DEFAULT_CONFIG.controller_mac,
            src_mac=DEFAULT_CONFIG.talker_mac,
            target_entity_id=DEFAULT_CONFIG.talker_entity_id,
            controller_entity_id=DEFAULT_CONFIG.controller_entity_id,
            sequence_id=43,
        )
        self.assertEqual(len(frame), 62)
        parsed = parse_aem_frame(frame)
        self.assertEqual(parsed.message_type, AECP_MESSAGE_TYPE_AEM_RESPONSE)
        self.assertEqual(parsed.control_data_length, 36)
        c, t, k = extract_auth_add_key_nonce_response(parsed)
        self.assertEqual(c, ctrl_nonce)
        self.assertEqual(t, tgt_nonce)
        self.assertEqual(k, key_id)

    def test_control_data_length_values(self):
        """Verify control_data_length matches expected spec values."""
        # AUTH_GET_NONCE: cdl = 12 + 8 = 20
        frame = build_auth_get_nonce_frame(
            bytes(8),
            dest_mac=bytes(6),
            src_mac=bytes(6),
            target_entity_id=bytes(8),
            controller_entity_id=bytes(8),
            sequence_id=0,
        )
        self.assertEqual(parse_aem_frame(frame).control_data_length, 20)

        # AUTH_GET_NONCE response: cdl = 12 + 16 = 28
        frame = build_auth_get_nonce_response_frame(
            bytes(8),
            bytes(8),
            dest_mac=bytes(6),
            src_mac=bytes(6),
            target_entity_id=bytes(8),
            controller_entity_id=bytes(8),
            sequence_id=0,
        )
        self.assertEqual(parse_aem_frame(frame).control_data_length, 28)

        # AUTH_ADD_KEY_NONCE response: cdl = 12 + 24 = 36
        frame = build_auth_add_key_nonce_response_frame(
            bytes(8),
            bytes(8),
            bytes(8),
            dest_mac=bytes(6),
            src_mac=bytes(6),
            target_entity_id=bytes(8),
            controller_entity_id=bytes(8),
            sequence_id=0,
        )
        self.assertEqual(parse_aem_frame(frame).control_data_length, 36)

    def test_parse_rejects_short_frame(self):
        """parse_aem_frame raises ValueError for truncated input."""
        with self.assertRaises(ValueError):
            parse_aem_frame(bytes(37))

    def test_parse_rejects_wrong_ethertype(self):
        """parse_aem_frame raises ValueError for non-AVTP ethertype."""
        frame = bytearray(46)
        struct.pack_into("!H", frame, 12, 0x0800)  # IPv4 ethertype
        with self.assertRaises(ValueError):
            parse_aem_frame(bytes(frame))

    def test_parse_rejects_wrong_subtype(self):
        """parse_aem_frame raises ValueError for non-AECP subtype."""
        frame = bytearray(46)
        struct.pack_into("!H", frame, 12, AVTP_ETHERTYPE)
        frame[14] = 0xFA  # ADP instead of AECP
        with self.assertRaises(ValueError):
            parse_aem_frame(bytes(frame))

    def test_eecf_frame_roundtrip(self):
        """Build EECF frame, parse it back, verify all fields."""
        enc_payload = bytes(range(96))
        key_id = DEFAULT_CONFIG.talker_entity_id
        frame = build_eecf_frame(
            dest_mac=DEFAULT_CONFIG.talker_mac,
            src_mac=DEFAULT_CONFIG.controller_mac,
            enc=EECF_ENC_ECC1,
            key_id=key_id,
            encrypted_payload=enc_payload,
        )
        # 14 (eth) + 12 (eecf hdr) + 96 (payload) = 122
        self.assertEqual(len(frame), 122)
        parsed = parse_eecf_frame(frame)
        self.assertEqual(parsed.dest_mac, DEFAULT_CONFIG.talker_mac)
        self.assertEqual(parsed.src_mac, DEFAULT_CONFIG.controller_mac)
        self.assertEqual(parsed.ethertype, AVTP_ETHERTYPE)
        self.assertEqual(parsed.subtype, EECF_SUBTYPE)
        self.assertEqual(parsed.enc, EECF_ENC_ECC1)
        self.assertEqual(parsed.encrypted_payload_length, 96)
        self.assertEqual(parsed.key_id, key_id)
        self.assertEqual(parsed.encrypted_payload, enc_payload)

    # Known answers from an independent IEEE 1363a-2004 11.3.2 reference
    # (also carried by avtp_aem_frame_test.cpp): recipient d = SHA-256(seed) mod n.
    ECC1_KAT_SEED = bytes(range(1, 33))
    ECC1_KAT_ENTROPY = bytes.fromhex(
        "deadbeefcafebabe0102030405060708090a0b0c0d0e0f101112131415161718"
    )
    ECC1_KAT_HELLO = bytes.fromhex(
        "01c23f7a6a51aa50aecd3aaac1c65933cfaa9256eafd363ca2d345f9e53d95a59a"
        "a0447cd56edb42a5cec04ca4d4506ee7c1a28f62077c3ca956e35fc894ba2187"
        "2921cb336cc09952820b97fdca2ac7c6ba7425af011e3dc5a267e5d2686e9a13"
    )
    ECC1_KAT_FRAMED = bytes.fromhex(
        "01c23f7a6a51aa50aecd3aaac1c65933cfaa9256eafd363ca2d345f9e53d95a59a"
        "e905f27629d33d7fb957de4ea4707a22aef622d95db6ad7d4761909365c67c4e"
        "166bed535fd26ff87446a6e23d2f8adf4d88a26447d016039a3d68619289604a"
    )

    def test_eecf_ecc1_payload_known_answer(self):
        """V || T || C for "Hello, DL/ECIES!" matches the independent reference."""
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        payload = eecf_ecc1_encrypt_payload(
            kp.public_key, b"Hello, DL/ECIES!", self.ECC1_KAT_ENTROPY
        )
        self.assertEqual(payload, self.ECC1_KAT_HELLO)
        self.assertEqual(
            eecf_ecc1_decrypt_payload(kp, self.ECC1_KAT_HELLO), b"Hello, DL/ECIES!"
        )
        # The library's own V || C || T order is not an ECC1 payload.
        vct = sc.ecies_encrypt(
            kp.public_key, b"Hello, DL/ECIES!", self.ECC1_KAT_ENTROPY
        )
        self.assertEqual(payload[:33], vct[:33])  # V
        self.assertEqual(payload[33:65], vct[-32:])  # T
        self.assertEqual(payload[65:], vct[33:-32])  # C
        with self.assertRaises(ValueError):
            eecf_ecc1_decrypt_payload(kp, vct)

    def test_eecf_ecc1_framed_known_answer(self):
        """17.3.3 plaintext timestamp || AVTPDU, encrypted, matches the reference."""
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        avtpdu = bytes(range(0x10, 0x20))
        payload = eecf_encrypt(
            avtpdu,
            kp.public_key,
            DEFAULT_CONFIG.talker_entity_id,
            0x0123456789ABCDEF,
            self.ECC1_KAT_ENTROPY,
        )
        self.assertEqual(payload, self.ECC1_KAT_FRAMED)
        ts, dec = eecf_decrypt(self.ECC1_KAT_FRAMED, kp)
        self.assertEqual(ts, 0x0123456789ABCDEF)
        self.assertEqual(dec, avtpdu)

    # ECC2 known answers from the Python HPKE reference (statusbar_crypto.py over the
    # cryptography package; also carried by avtp_aem_frame_test.cpp and the golden
    # capture): same recipient, ikm_e = ECC1_KAT_ENTROPY, key_id 70-B3-D5-ED-C4-00-00-42.
    ECC2_KAT_KEY_ID = bytes.fromhex("70b3d5edc4000042")
    ECC2_KAT_HELLO = bytes.fromhex(
        "04289dce76026f47dcb7d77006982f29b06f0e58c166b738d5dfcd276280de6d23"
        "7c66c8670a4191c3ffb0de2cb2bb793c885fc8631d0dca63795c4dad394aa7cd"
        "e927f21be61587d511f47e61b2a848ecdf4524fd10762fe8efa7ab8b526ef733"
    )
    ECC2_KAT_FRAMED = bytes.fromhex(
        "04289dce76026f47dcb7d77006982f29b06f0e58c166b738d5dfcd276280de6d23"
        "7c66c8670a4191c3ffb0de2cb2bb793c885fc8631d0dca63795c4dad394aa7cd"
        "a061db1000926a7251ae2952e3fe1dc92245c6bf41452b5b84bbe99ed14496be"
        "08c5186eb4fdbd3f"
    )

    def test_eecf_ecc2_payload_known_answer(self):
        """ECC2 payload for 'Hello, HPKE ECC2' under its header as aad."""
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        aad = eecf_ecc2_header(self.ECC2_KAT_KEY_ID, len(self.ECC2_KAT_HELLO))
        self.assertEqual(aad, bytes.fromhex("ed01006170b3d5edc4000042"))
        payload = eecf_ecc2_encrypt_payload(
            kp.public_key, aad, b"Hello, HPKE ECC2", self.ECC1_KAT_ENTROPY
        )
        self.assertEqual(payload, self.ECC2_KAT_HELLO)
        self.assertEqual(
            eecf_ecc2_decrypt_payload(kp, aad, self.ECC2_KAT_HELLO), b"Hello, HPKE ECC2"
        )

    def test_eecf_ecc2_framed_known_answer(self):
        """ECC2 payload of the 17.3.3 plaintext, through a built and parsed frame."""
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        payload = eecf_ecc2_encrypt(
            bytes(range(0x10, 0x20)),
            kp.public_key,
            self.ECC2_KAT_KEY_ID,
            0x0123456789ABCDEF,
            self.ECC1_KAT_ENTROPY,
        )
        self.assertEqual(payload, self.ECC2_KAT_FRAMED)
        frame = build_eecf_frame(
            bytes(6), bytes(6), EECF_ENC_ECC2, self.ECC2_KAT_KEY_ID, payload
        )
        parsed = parse_eecf_frame(frame)
        self.assertEqual(parsed.enc, EECF_ENC_ECC2)
        ts, dec = eecf_ecc2_decrypt(parsed.encrypted_payload, parsed.header, kp)
        self.assertEqual((ts, dec), (0x0123456789ABCDEF, bytes(range(0x10, 0x20))))

    def test_eecf_ecc2_header_is_authenticated(self):
        """A header with another key_id, enc or length does not open the payload."""
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        good = eecf_ecc2_header(self.ECC2_KAT_KEY_ID, len(self.ECC2_KAT_FRAMED))
        other_key = eecf_ecc2_header(
            bytes.fromhex("70b3d5edc4000043"), len(self.ECC2_KAT_FRAMED)
        )
        downgraded = bytes([good[0], EECF_ENC_ECC1]) + good[2:]
        for bad in (other_key, downgraded, good[:-1] + bytes([good[-1] ^ 1])):
            with self.assertRaises(ValueError):
                eecf_ecc2_decrypt(self.ECC2_KAT_FRAMED, bad, kp)
        with self.assertRaises(ValueError):
            eecf_ecc2_decrypt_payload(kp, good, self.ECC2_KAT_FRAMED[:80])

    # ECC3 known answers (amendment test-vector slide and the Python HPKE reference):
    # recipient key = DeriveKeyPair(ECC1_KAT_SEED) for DHKEM(X25519), same ikm_e,
    # key_id, timestamp and AVTPDU as ECC2.
    ECC3_KAT_RECIPIENT_PK = bytes.fromhex(
        "49e4874e25fe389ed3c9fa2fd09d077907ecc5809c8619e2127128b6a72c7c7a"
    )
    ECC3_KAT_HELLO = bytes.fromhex(
        "8a7d2773c3dcc1cbfa275f67915a8b3f7b28b6e2c9b47346fbfd232cdf38fc22"
        "c1010cd61eef7d3d7ed771c70b4cb90bb51f77e0d23ee646ec9e33e1b23d2c56"
    )
    ECC3_KAT_FRAMED = bytes.fromhex(
        "8a7d2773c3dcc1cbfa275f67915a8b3f7b28b6e2c9b47346fbfd232cdf38fc22"
        "884725ddf868909a3e8d26f45a1aec2fb0540ebcd1eb7df3"
        "0eb95b3b510a5abe6c6be677da632843"
    )

    def test_eecf_ecc3_payload_known_answer(self):
        """ECC3 payload for 'Hello, HPKE ECC3' under its header as aad."""
        kp = sc.hpke_x25519_derive_key_pair(self.ECC1_KAT_SEED)
        self.assertEqual(kp.public_key.data, self.ECC3_KAT_RECIPIENT_PK)
        aad = eecf_ecc3_header(self.ECC2_KAT_KEY_ID, len(self.ECC3_KAT_HELLO))
        self.assertEqual(aad, bytes.fromhex("ed02004070b3d5edc4000042"))
        payload = eecf_ecc3_encrypt_payload(
            kp.public_key, aad, b"Hello, HPKE ECC3", self.ECC1_KAT_ENTROPY
        )
        self.assertEqual(payload, self.ECC3_KAT_HELLO)
        self.assertEqual(
            eecf_ecc3_decrypt_payload(kp, aad, self.ECC3_KAT_HELLO), b"Hello, HPKE ECC3"
        )

    def test_eecf_ecc3_framed_known_answer(self):
        """The amendment's ECC3 test vector, through a built and parsed frame."""
        kp = sc.hpke_x25519_derive_key_pair(self.ECC1_KAT_SEED)
        payload = eecf_ecc3_encrypt(
            bytes(range(0x10, 0x20)),
            kp.public_key,
            self.ECC2_KAT_KEY_ID,
            0x0123456789ABCDEF,
            self.ECC1_KAT_ENTROPY,
        )
        self.assertEqual(payload, self.ECC3_KAT_FRAMED)
        frame = build_eecf_frame(
            bytes(6), bytes(6), EECF_ENC_ECC3, self.ECC2_KAT_KEY_ID, payload
        )
        self.assertEqual(frame[14:26], bytes.fromhex("ed02004870b3d5edc4000042"))
        parsed = parse_eecf_frame(frame)
        self.assertEqual(parsed.enc, EECF_ENC_ECC3)
        ts, dec = eecf_ecc3_decrypt(parsed.encrypted_payload, parsed.header, kp)
        self.assertEqual((ts, dec), (0x0123456789ABCDEF, bytes(range(0x10, 0x20))))

    def test_eecf_ecc3_header_is_authenticated_and_short_payloads_rejected(self):
        """A header with another key_id, enc or length does not open the payload."""
        kp = sc.hpke_x25519_derive_key_pair(self.ECC1_KAT_SEED)
        good = eecf_ecc3_header(self.ECC2_KAT_KEY_ID, len(self.ECC3_KAT_FRAMED))
        other_key = eecf_ecc3_header(
            bytes.fromhex("70b3d5edc4000043"), len(self.ECC3_KAT_FRAMED)
        )
        relabelled = bytes([good[0], EECF_ENC_ECC2]) + good[2:]
        for bad in (other_key, relabelled, good[:-1] + bytes([good[-1] ^ 1])):
            with self.assertRaises(ValueError):
                eecf_ecc3_decrypt(self.ECC3_KAT_FRAMED, bad, kp)
        with self.assertRaises(ValueError):
            eecf_ecc3_decrypt(bytes(55), eecf_ecc3_header(self.ECC2_KAT_KEY_ID, 55), kp)
        with self.assertRaises(ValueError):
            eecf_ecc2_decrypt(
                bytes(88),
                eecf_ecc2_header(self.ECC2_KAT_KEY_ID, 88),
                sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED),
            )
        with self.assertRaises(ValueError):  # small-order enc (all-zero u)
            eecf_ecc3_decrypt_payload(kp, good, bytes(32) + self.ECC3_KAT_FRAMED[32:])

    def test_eecf_length_field_is_sixteen_bits(self):
        """Figure 110: encrypted_payload_length is the whole of octets 2 and 3."""
        frame = build_eecf_frame(
            bytes(6), bytes(6), EECF_ENC_ECC1, bytes(8), bytes(0x1234)
        )
        self.assertEqual(frame[16:18], bytes.fromhex("1234"))
        self.assertEqual(parse_eecf_frame(frame).encrypted_payload_length, 0x1234)

    # ESCF: the ED25519 known answer from an independent RFC 8032 implementation (the
    # cryptography package) for seed ECC1_KAT_SEED over
    # key_id 70-B3-D5-ED-C4-00-00-42 || 0x0123456789ABCDEF || 0x10 .. 0x1F.
    ESCF_KAT_MESSAGE = bytes.fromhex(
        "70b3d5edc40000420123456789abcdef101112131415161718191a1b1c1d1e1f"
    )
    ESCF_KAT_ED25519_PK = bytes.fromhex(
        "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"
    )
    ESCF_KAT_ED25519_SIG = bytes.fromhex(
        "60a4f03c792333de185d84b8f98b8b84e020e2348c7cceaaa61f1ea4ac3b4d34"
        "375624c2a4b7c674e0a2cc4a52c6966bed1e34b772ac5a845f91cf883529450f"
    )
    # ECSSA_DSA_EMSA1_SHA256 by the C++ stack (RFC 6979 nonce) for d = SHA-256(seed) mod n;
    # test_escf_ecssa_known_answer verifies it with the cryptography package's ECDSA.
    ESCF_KAT_ECSSA_SIG = bytes.fromhex(
        "993ebbb74f57d10e01c554e0f70662d0a546d0aa667e83fb6962fee43b96a613"
        "580e68bba17a988c1968394becbf2a7205ff9446a263ceccfc363b8d385b1c59"
    )

    def test_escf_ecssa_known_answer(self):
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        self.assertTrue(
            escf_ecssa_verify(
                kp.public_key,
                self.ECC2_KAT_KEY_ID,
                0x0123456789ABCDEF,
                bytes(range(0x10, 0x20)),
                self.ESCF_KAT_ECSSA_SIG,
            )
        )

    def test_escf_signed_message_and_ed25519_known_answer(self):
        avtpdu = bytes(range(0x10, 0x20))
        self.assertEqual(
            escf_signed_message(self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF, avtpdu),
            self.ESCF_KAT_MESSAGE,
        )
        kp = sc.ed25519_keypair_from_seed(self.ECC1_KAT_SEED)
        self.assertEqual(kp.public_key.data, self.ESCF_KAT_ED25519_PK)
        sig = escf_ed25519_sign(kp, self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF, avtpdu)
        self.assertEqual(sig, self.ESCF_KAT_ED25519_SIG)
        self.assertTrue(
            escf_ed25519_verify(
                kp.public_key, self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF, avtpdu, sig
            )
        )
        self.assertFalse(
            escf_ed25519_verify(
                kp.public_key, self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF + 1, avtpdu, sig
            )
        )
        with self.assertRaises(ValueError):
            escf_signed_message(self.ECC2_KAT_KEY_ID, 0, bytes(1417))

    def test_escf_ecssa_sign_verify(self):
        avtpdu = bytes(range(0x10, 0x20))
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        sig = escf_ecssa_sign(kp, self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF, avtpdu)
        self.assertEqual(len(sig), 64)
        self.assertTrue(
            escf_ecssa_verify(
                kp.public_key, self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF, avtpdu, sig
            )
        )
        self.assertFalse(
            escf_ecssa_verify(kp.public_key, bytes(8), 0x0123456789ABCDEF, avtpdu, sig)
        )

    def test_escf_frame_roundtrip_both_variants(self):
        avtpdu = bytes(range(0x10, 0x20))
        p256 = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        ed = sc.ed25519_keypair_from_seed(self.ECC1_KAT_SEED)
        for sig_mode, sign, verify, kp in (
            (ESCF_SIG_ECSSA, escf_ecssa_sign, escf_ecssa_verify, p256),
            (ESCF_SIG_ED25519, escf_ed25519_sign, escf_ed25519_verify, ed),
        ):
            sig = sign(kp, self.ECC2_KAT_KEY_ID, 0x0123456789ABCDEF, avtpdu)
            frame = build_escf_frame(
                DEFAULT_CONFIG.talker_mac,
                DEFAULT_CONFIG.controller_mac,
                sig_mode,
                self.ECC2_KAT_KEY_ID,
                0x0123456789ABCDEF,
                sig,
                avtpdu,
            )
            self.assertEqual(len(frame), ESCF_PAYLOAD_OFFSET + 16)
            self.assertEqual(frame[14:18], bytes([ESCF_SUBTYPE, sig_mode, 0, 16]))
            parsed = parse_escf_frame(frame + bytes(20))  # padding is not the AVTPDU
            self.assertEqual(
                (parsed.sig, parsed.encapsulated_avtpdu_length, parsed.key_id),
                (sig_mode, 16, self.ECC2_KAT_KEY_ID),
            )
            self.assertEqual(parsed.signing_timestamp, 0x0123456789ABCDEF)
            self.assertEqual(
                (parsed.signature_value, parsed.encapsulated_avtpdu), (sig, avtpdu)
            )
            self.assertTrue(
                verify(
                    kp.public_key,
                    parsed.key_id,
                    parsed.signing_timestamp,
                    parsed.encapsulated_avtpdu,
                    parsed.signature_value,
                )
            )
            tampered = bytearray(frame)
            tampered[ESCF_PAYLOAD_OFFSET] ^= 1
            t = parse_escf_frame(bytes(tampered))
            self.assertFalse(
                verify(
                    kp.public_key,
                    t.key_id,
                    t.signing_timestamp,
                    t.encapsulated_avtpdu,
                    t.signature_value,
                )
            )
        with self.assertRaises(ValueError):
            parse_escf_frame(frame[: ESCF_PAYLOAD_OFFSET - 1])
        with self.assertRaises(ValueError):
            parse_escf_frame(frame[:16] + bytes([0, 200]) + frame[18:])

    def test_escf_timestamp_window(self):
        now = 1_000_000_000_000
        self.assertTrue(escf_signing_timestamp_is_acceptable(now, now))
        self.assertTrue(
            escf_signing_timestamp_is_acceptable(now - ESCF_TIMESTAMP_WINDOW_NS, now)
        )
        self.assertFalse(
            escf_signing_timestamp_is_acceptable(
                now - ESCF_TIMESTAMP_WINDOW_NS - 1, now
            )
        )
        self.assertFalse(escf_signing_timestamp_is_acceptable(now + 1, now))

    def test_eecf_ecc1_rejects_bad_shapes(self):
        """A payload without a whole cipher block after V and T is not ECC1."""
        kp = sc.p256_ecdsa_keypair_from_seed(self.ECC1_KAT_SEED)
        with self.assertRaises(ValueError):
            eecf_ecc1_decrypt_payload(kp, self.ECC1_KAT_HELLO[:65])
        with self.assertRaises(ValueError):
            eecf_ecc1_decrypt_payload(kp, self.ECC1_KAT_HELLO[:96])

    def test_eecf_encrypt_decrypt_roundtrip(self):
        """EECF encrypt/decrypt with P-256 ECIES roundtrip."""
        # Build an inner AEM AVTPDU (subtype byte onward, no Ethernet header)
        inner_avtpdu = bytes(range(24))  # dummy inner AVTPDU
        entropy = bytes(range(32))
        timestamp = 1234567890

        # Generate a P-256 keypair for the recipient
        seed = bytes(range(32))
        recipient_kp = sc.p256_ecdsa_keypair_from_seed(seed)

        enc_payload = eecf_encrypt(
            inner_avtpdu,
            recipient_kp.public_key,
            DEFAULT_CONFIG.talker_entity_id,
            timestamp,
            entropy,
        )
        dec_ts, dec_avtpdu = eecf_decrypt(enc_payload, recipient_kp)
        self.assertEqual(dec_ts, timestamp)
        self.assertEqual(dec_avtpdu, inner_avtpdu)

    def test_eecf_full_frame_encrypt_decrypt(self):
        """Build AEM frame, wrap in EECF, parse and decrypt, recover inner AVTPDU."""
        seed = bytes(range(32))
        recipient_kp = sc.p256_ecdsa_keypair_from_seed(seed)
        entropy = bytes(range(32, 64))
        timestamp = 999999999

        # Build inner AEM frame
        nonce = bytes(range(8))
        inner_frame = build_auth_get_nonce_frame(
            nonce,
            dest_mac=DEFAULT_CONFIG.talker_mac,
            src_mac=DEFAULT_CONFIG.controller_mac,
            target_entity_id=DEFAULT_CONFIG.talker_entity_id,
            controller_entity_id=DEFAULT_CONFIG.controller_entity_id,
            sequence_id=10,
        )
        # Inner AVTPDU = frame bytes after Ethernet header (subtype byte onward)
        inner_avtpdu = inner_frame[ETHERNET_HEADER_SIZE:]

        # Encrypt and wrap in EECF
        enc_payload = eecf_encrypt(
            inner_avtpdu,
            recipient_kp.public_key,
            DEFAULT_CONFIG.talker_entity_id,
            timestamp,
            entropy,
        )
        eecf_frame = build_eecf_frame(
            dest_mac=DEFAULT_CONFIG.talker_mac,
            src_mac=DEFAULT_CONFIG.controller_mac,
            enc=EECF_ENC_ECC1,
            key_id=DEFAULT_CONFIG.talker_entity_id,
            encrypted_payload=enc_payload,
        )

        # Parse EECF frame
        parsed_eecf = parse_eecf_frame(eecf_frame)
        self.assertEqual(parsed_eecf.subtype, EECF_SUBTYPE)
        self.assertEqual(parsed_eecf.enc, EECF_ENC_ECC1)

        # Decrypt to recover inner AVTPDU
        dec_ts, dec_avtpdu = eecf_decrypt(
            parsed_eecf.encrypted_payload,
            recipient_kp,
        )
        self.assertEqual(dec_ts, timestamp)
        self.assertEqual(dec_avtpdu, inner_avtpdu)

        # Verify inner AVTPDU parses correctly as AEM
        # Prepend a dummy Ethernet header so parse_aem_frame works
        reconstructed = inner_frame[:ETHERNET_HEADER_SIZE] + dec_avtpdu
        parsed_aem = parse_aem_frame(reconstructed)
        self.assertEqual(parsed_aem.command_code, 0x0067)
        self.assertEqual(parsed_aem.payload, nonce)

    def test_eecf_parse_rejects_short_frame(self):
        """parse_eecf_frame raises ValueError for truncated input."""
        with self.assertRaises(ValueError):
            parse_eecf_frame(bytes(25))

    def test_eecf_parse_rejects_wrong_subtype(self):
        """parse_eecf_frame raises ValueError for non-EECF subtype."""
        frame = bytearray(32)
        struct.pack_into("!H", frame, 12, AVTP_ETHERTYPE)
        frame[14] = 0xFB  # AECP instead of EECF
        with self.assertRaises(ValueError):
            parse_eecf_frame(bytes(frame))


# ---------------------------------------------------------------------------
# Test cases: protocol tests via frames
# ---------------------------------------------------------------------------


class TestAvbKeyExchangeFrame(unittest.TestCase):
    """Frame-level protocol tests, parameterized by backends and session key type."""

    controller_backend: Backend = Backend.PYTHON
    entity_backend: Backend = Backend.PYTHON
    session_key_config: SessionKeyConfig = SESSION_KEY_CONFIGS[1]  # aes256_siv
    cli_path: Optional[Path] = None
    yubikey_config: Optional[YubikeyConfig] = None
    frame_config: FrameConfig = DEFAULT_CONFIG

    def _make_encrypt_fn(self) -> EciesEncryptFn:
        if self.controller_backend == Backend.CPP:
            return make_cpp_ecies_encrypt(self.cli_path)
        return python_ecies_encrypt

    def _make_decrypt_fn(self, entity: AvbEntity) -> EciesDecryptFn:
        if (
            self.yubikey_config is not None
            and entity.name == self.yubikey_config.entity_name
        ):
            return make_yubikey_ecies_decrypt(self.yubikey_config)
        if self.entity_backend == Backend.CPP:
            return make_cpp_ecies_decrypt(self.cli_path, entity.x25519_seed)
        return python_ecies_decrypt

    def _create_entity(self, name: str, index: int) -> AvbEntity:
        if self.yubikey_config is not None and name == self.yubikey_config.entity_name:
            return create_hardware_entity(name, index, self.yubikey_config)
        return create_entity(name, index)

    def setUp(self):
        self.controller = create_entity("controller", 0x01)
        self.talker = self._create_entity("talker", 0x02)
        self.listener1 = self._create_entity("listener1", 0x03)
        self.listener2 = self._create_entity("listener2", 0x04)
        self.targets = [self.talker, self.listener1, self.listener2]
        for target in self.targets:
            install_controller_keys(self.controller, target)

        # Generate EECF P-256 keypairs for each target (used for EECF-wrapped
        # AUTH_ADD_KEY_NONCE transport encryption per IEEE 1722-2025 Clause 17).
        self.eecf_keypairs: dict = {}
        for target in self.targets:
            seed = sc.sha256(f"eecf_p256:{target.name}".encode())
            self.eecf_keypairs[target.name] = sc.p256_ecdsa_keypair_from_seed(seed)

    def test_eecf_ecc1_payload_matches_cpp_tool(self):
        """The C++ eecf_ecc1_encrypt/decrypt agree with the Python V || T || C."""
        if self.cli_path is None:
            self.skipTest("no avtp_crypto_tool")
        from avb_key_exchange_test import run_cli  # noqa: PLC0415

        seed = TestFrameBuildParse.ECC1_KAT_SEED
        entropy = TestFrameBuildParse.ECC1_KAT_ENTROPY
        kp = sc.p256_ecdsa_keypair_from_seed(seed)
        plaintext = struct.pack("!Q", 0x0123456789ABCDEF) + bytes(range(0x10, 0x20))
        cpp_payload = bytes.fromhex(
            run_cli(
                self.cli_path,
                "eecf_ecc1_encrypt",
                kp.public_key.data.hex(),
                entropy.hex(),
                plaintext.hex(),
            )
        )
        self.assertEqual(cpp_payload, TestFrameBuildParse.ECC1_KAT_FRAMED)
        self.assertEqual(
            cpp_payload, eecf_ecc1_encrypt_payload(kp.public_key, plaintext, entropy)
        )
        python_payload = eecf_ecc1_encrypt_payload(kp.public_key, plaintext, entropy)
        out = run_cli(
            self.cli_path, "eecf_ecc1_decrypt", seed.hex(), python_payload.hex()
        )
        self.assertEqual(out, "ok " + plaintext.hex())

    def test_eecf_ecc2_payload_matches_cpp_tool(self):
        """The C++ eecf_ecc2_encrypt/decrypt agree with the Python enc || C || T."""
        if self.cli_path is None:
            self.skipTest("no avtp_crypto_tool")
        from avb_key_exchange_test import run_cli  # noqa: PLC0415

        seed = TestFrameBuildParse.ECC1_KAT_SEED
        ikm_e = TestFrameBuildParse.ECC1_KAT_ENTROPY
        kp = sc.p256_ecdsa_keypair_from_seed(seed)
        plaintext = struct.pack("!Q", 0x0123456789ABCDEF) + bytes(range(0x10, 0x20))
        aad = eecf_ecc2_header(
            TestFrameBuildParse.ECC2_KAT_KEY_ID, len(plaintext) + ECC2_OVERHEAD
        )
        cpp_payload = bytes.fromhex(
            run_cli(
                self.cli_path,
                "eecf_ecc2_encrypt",
                kp.public_key.data.hex(),
                ikm_e.hex(),
                aad.hex(),
                plaintext.hex(),
            )
        )
        self.assertEqual(cpp_payload, TestFrameBuildParse.ECC2_KAT_FRAMED)
        python_payload = eecf_ecc2_encrypt_payload(kp.public_key, aad, plaintext, ikm_e)
        self.assertEqual(cpp_payload, python_payload)
        out = run_cli(
            self.cli_path,
            "eecf_ecc2_decrypt",
            seed.hex(),
            aad.hex(),
            python_payload.hex(),
        )
        self.assertEqual(out, "ok " + plaintext.hex())
        out = run_cli(
            self.cli_path,
            "eecf_ecc2_decrypt",
            seed.hex(),
            aad[:-1].hex() + "43",
            python_payload.hex(),
        )
        self.assertEqual(out, "fail")

    def test_eecf_ecc3_payload_matches_cpp_tool(self):
        """The C++ eecf_ecc3_encrypt/decrypt agree with the Python enc || C || T."""
        if self.cli_path is None:
            self.skipTest("no avtp_crypto_tool")
        from avb_key_exchange_test import run_cli  # noqa: PLC0415

        seed = TestFrameBuildParse.ECC1_KAT_SEED
        ikm_e = TestFrameBuildParse.ECC1_KAT_ENTROPY
        kp = sc.hpke_x25519_derive_key_pair(seed)
        sk_hex, pk_hex = run_cli(
            self.cli_path, "hpke_x25519_keypair", seed.hex()
        ).split()
        self.assertEqual((sk_hex, pk_hex), (kp.data.hex(), kp.public_key.data.hex()))
        plaintext = struct.pack("!Q", 0x0123456789ABCDEF) + bytes(range(0x10, 0x20))
        aad = eecf_ecc3_header(
            TestFrameBuildParse.ECC2_KAT_KEY_ID, len(plaintext) + ECC3_OVERHEAD
        )
        cpp_payload = bytes.fromhex(
            run_cli(
                self.cli_path,
                "eecf_ecc3_encrypt",
                kp.public_key.data.hex(),
                ikm_e.hex(),
                aad.hex(),
                plaintext.hex(),
            )
        )
        self.assertEqual(cpp_payload, TestFrameBuildParse.ECC3_KAT_FRAMED)
        self.assertEqual(
            cpp_payload, eecf_ecc3_encrypt_payload(kp.public_key, aad, plaintext, ikm_e)
        )
        out = run_cli(
            self.cli_path, "eecf_ecc3_decrypt", seed.hex(), aad.hex(), cpp_payload.hex()
        )
        self.assertEqual(out, "ok " + plaintext.hex())

    def test_escf_signatures_match_cpp_tool(self):
        """The C++ escf_sign/escf_verify agree with the Python ESCF signatures."""
        if self.cli_path is None:
            self.skipTest("no avtp_crypto_tool")
        from avb_key_exchange_test import run_cli  # noqa: PLC0415

        seed = TestFrameBuildParse.ECC1_KAT_SEED
        key_id = TestFrameBuildParse.ECC2_KAT_KEY_ID
        ts = 0x0123456789ABCDEF
        avtpdu = bytes(range(0x10, 0x20))
        args = (key_id.hex(), struct.pack("!Q", ts).hex(), avtpdu.hex())

        ed = sc.ed25519_keypair_from_seed(seed)
        cpp_ed = bytes.fromhex(
            run_cli(self.cli_path, "escf_sign", "01", seed.hex(), *args)
        )
        self.assertEqual(cpp_ed, TestFrameBuildParse.ESCF_KAT_ED25519_SIG)
        self.assertEqual(
            run_cli(
                self.cli_path,
                "escf_verify",
                "01",
                ed.public_key.data.hex(),
                *args,
                cpp_ed.hex(),
            ),
            "ok",
        )

        p256 = sc.p256_ecdsa_keypair_from_seed(seed)
        cpp_ecssa = bytes.fromhex(
            run_cli(self.cli_path, "escf_sign", "00", seed.hex(), *args)
        )
        self.assertEqual(cpp_ecssa, TestFrameBuildParse.ESCF_KAT_ECSSA_SIG)  # RFC 6979
        self.assertTrue(
            escf_ecssa_verify(p256.public_key, key_id, ts, avtpdu, cpp_ecssa)
        )
        py_ecssa = escf_ecssa_sign(p256, key_id, ts, avtpdu)
        self.assertEqual(
            run_cli(
                self.cli_path,
                "escf_verify",
                "00",
                p256.public_key.data.hex(),
                *args,
                py_ecssa.hex(),
            ),
            "ok",
        )
        bad = (py_ecssa[:-1] + bytes([py_ecssa[-1] ^ 1])).hex()
        self.assertEqual(
            run_cli(
                self.cli_path,
                "escf_verify",
                "00",
                p256.public_key.data.hex(),
                *args,
                bad,
            ),
            "fail",
        )

    def test_full_key_distribution_via_frames(self):
        """Run complete AUTH flow using frames for all 3 targets.

        AUTH_ADD_KEY_NONCE commands are wrapped in EECF encrypted control frames
        (IEEE 1722-2016 Clause 17) using each target's P-256 EECF public key.
        """
        encrypt_fn = self._make_encrypt_fn()
        ctrl_inner = Controller(self.controller, encrypt_fn)
        eecf_target_keys = {
            name: kp.public_key for name, kp in self.eecf_keypairs.items()
        }
        ctrl_frame = FrameController(
            ctrl_inner,
            "controller",
            self.frame_config,
            eecf_target_keys=eecf_target_keys,
        )
        session_key_id, session_key = ctrl_frame.create_session_key(
            self.session_key_config
        )

        for target in self.targets:
            decrypt_fn = self._make_decrypt_fn(target)
            target_inner = Entity(target, decrypt_fn)
            eecf_sk = self.eecf_keypairs[target.name]
            target_frame = FrameEntity(
                target_inner,
                target.name,
                self.frame_config,
                eecf_private_key=eecf_sk,
            )
            nonce_entropy = sc.sha256(f"avb_test:nonce:{target.name}".encode())
            ecies_entropy = sc.sha256(f"avb_test:ecies_entropy:{target.name}".encode())
            eecf_entropy = sc.sha256(f"avb_test:eecf:{target.name}".encode())
            response = run_frame_key_distribution(
                self.controller,
                ctrl_frame,
                target,
                target_frame,
                session_key_id,
                session_key,
                nonce_entropy,
                ecies_entropy,
                self.session_key_config,
                eecf_entropy=eecf_entropy,
            )
            self.assertEqual(response.key_id.data, session_key_id.data)

    def test_all_entities_have_same_session_key_via_frames(self):
        """After frame-level distribution, all entities have identical session key.

        Uses EECF-wrapped AUTH_ADD_KEY_NONCE for transport encryption.
        """
        encrypt_fn = self._make_encrypt_fn()
        ctrl_inner = Controller(self.controller, encrypt_fn)
        eecf_target_keys = {
            name: kp.public_key for name, kp in self.eecf_keypairs.items()
        }
        ctrl_frame = FrameController(
            ctrl_inner,
            "controller",
            self.frame_config,
            eecf_target_keys=eecf_target_keys,
        )
        session_key_id, session_key = ctrl_frame.create_session_key(
            self.session_key_config
        )

        for target in self.targets:
            decrypt_fn = self._make_decrypt_fn(target)
            target_inner = Entity(target, decrypt_fn)
            eecf_sk = self.eecf_keypairs[target.name]
            target_frame = FrameEntity(
                target_inner,
                target.name,
                self.frame_config,
                eecf_private_key=eecf_sk,
            )
            nonce_entropy = sc.sha256(f"avb_test:nonce:{target.name}".encode())
            ecies_entropy = sc.sha256(f"avb_test:ecies_entropy:{target.name}".encode())
            eecf_entropy = sc.sha256(f"avb_test:eecf:{target.name}".encode())
            run_frame_key_distribution(
                self.controller,
                ctrl_frame,
                target,
                target_frame,
                session_key_id,
                session_key,
                nonce_entropy,
                ecies_entropy,
                self.session_key_config,
                eecf_entropy=eecf_entropy,
            )

        # Controller has the key
        ctrl_entry = self.controller.keychains.find_session_key_entry(session_key_id)
        self.assertIsNotNone(ctrl_entry)
        self.assertEqual(ctrl_entry.key.data, session_key.data)

        # All targets have the same key
        for target in self.targets:
            entry = target.keychains.find_session_key_entry(session_key_id)
            self.assertIsNotNone(entry, f"{target.name} missing session key")
            self.assertIsInstance(entry, self.session_key_config.entry_class)
            self.assertEqual(
                entry.key.data,
                session_key.data,
                f"{target.name} has different session key",
            )

    def test_nonce_replay_protection_via_frames(self):
        """AUTH_ADD_KEY_NONCE with wrong target_nonce is rejected at frame level."""
        encrypt_fn = self._make_encrypt_fn()
        ctrl_inner = Controller(self.controller, encrypt_fn)
        ctrl_frame = FrameController(ctrl_inner, "controller", self.frame_config)
        session_key_id, session_key = ctrl_frame.create_session_key(
            self.session_key_config
        )

        target = self.talker
        nonce_entropy = sc.sha256(f"avb_test:nonce:{target.name}".encode())
        ecies_entropy = sc.sha256(f"avb_test:ecies_entropy:{target.name}".encode())

        # Normal nonce exchange via frames
        cmd_frame_bytes, controller_nonce = ctrl_frame.build_auth_get_nonce(
            target.name, nonce_entropy
        )
        decrypt_fn = self._make_decrypt_fn(target)
        target_inner = Entity(target, decrypt_fn)
        target_frame = FrameEntity(target_inner, target.name, self.frame_config)
        target_nonce_entropy = sc.sha256(nonce_entropy + target.name.encode())
        response_frame = target_frame.process_auth_get_nonce(
            cmd_frame_bytes, target_nonce_entropy, "controller"
        )
        response = ctrl_frame.process_auth_get_nonce_response(
            response_frame, controller_nonce
        )

        # Build AUTH_ADD_KEY_NONCE with WRONG target nonce
        wrong_target_nonce = sc.Nonce(data=bytes(8))
        header = sc.AuthAddKeyNonceHeader(
            controller_nonce=controller_nonce,
            target_nonce=wrong_target_nonce,
            key_id=session_key_id,
            key_type=self.session_key_config.key_type,
            key_length=self.session_key_config.key_size,
        )
        plaintext = sc.serialize_auth_add_key_nonce_header(header) + session_key.data
        ecies_ciphertext = encrypt_fn(
            target.x25519_sk.public_key, plaintext, ecies_entropy
        )

        # Wrap in frame
        encrypted_frame = build_auth_add_key_nonce_frame(
            ecies_ciphertext,
            dest_mac=self.frame_config.mac_for(target.name),
            src_mac=self.frame_config.mac_for("controller"),
            target_entity_id=self.frame_config.entity_id_for(target.name),
            controller_entity_id=self.frame_config.entity_id_for("controller"),
            sequence_id=99,
            config=self.frame_config,
        )

        with self.assertRaises(ValueError):
            target_frame.process_auth_add_key_nonce(encrypted_frame, "controller")

    def test_ecies_to_wrong_entity_fails_via_frames(self):
        """Message for listener1 cannot be decrypted by listener2 via frames."""
        encrypt_fn = self._make_encrypt_fn()
        ctrl_inner = Controller(self.controller, encrypt_fn)
        ctrl_frame = FrameController(ctrl_inner, "controller", self.frame_config)
        session_key_id, session_key = ctrl_frame.create_session_key(
            self.session_key_config
        )

        nonce_entropy = sc.sha256(b"avb_test:nonce:listener1")
        ecies_entropy = sc.sha256(b"avb_test:ecies_entropy:listener1")

        # Run nonce exchange with listener1 via frames
        l1_decrypt_fn = self._make_decrypt_fn(self.listener1)
        l1_inner = Entity(self.listener1, l1_decrypt_fn)
        l1_frame = FrameEntity(l1_inner, "listener1", self.frame_config)
        cmd_frame_bytes, controller_nonce = ctrl_frame.build_auth_get_nonce(
            "listener1", nonce_entropy
        )
        l1_nonce_entropy = sc.sha256(nonce_entropy + b"listener1")
        response_frame = l1_frame.process_auth_get_nonce(
            cmd_frame_bytes, l1_nonce_entropy, "controller"
        )
        response = ctrl_frame.process_auth_get_nonce_response(
            response_frame, controller_nonce
        )

        # Encrypt for listener1 via frame
        encrypted_frame = ctrl_frame.build_auth_add_key_nonce(
            "listener1",
            self.listener1.x25519_sk.public_key,
            response.controller_nonce,
            response.target_nonce,
            session_key_id,
            session_key,
            ecies_entropy,
            self.session_key_config,
        )

        # Listener2 tries to process the frame — should fail
        l2_decrypt_fn = self._make_decrypt_fn(self.listener2)
        l2_inner = Entity(self.listener2, l2_decrypt_fn)
        l2_frame = FrameEntity(l2_inner, "listener2", self.frame_config)
        l2_inner._target_nonce = response.target_nonce
        with self.assertRaises(ValueError):
            l2_frame.process_auth_add_key_nonce(encrypted_frame, "controller")


# ---------------------------------------------------------------------------
# Backend combination runner
# ---------------------------------------------------------------------------


def make_frame_test_class(
    ctrl_backend: Backend,
    entity_backend: Backend,
    session_key_config: SessionKeyConfig,
    cli_path: Optional[Path],
    yubikey_config: Optional[YubikeyConfig] = None,
    frame_config: FrameConfig = DEFAULT_CONFIG,
) -> type:
    """Create a TestAvbKeyExchangeFrame subclass for given backends and key type."""
    name = (
        f"TestAvbKeyExchangeFrame_ctrl_{ctrl_backend.value}"
        f"_entity_{entity_backend.value}"
        f"_key_{session_key_config.key_type.name}"
    )
    if yubikey_config is not None:
        name += f"_yubikey_{yubikey_config.entity_name}"

    cls = type(
        name,
        (TestAvbKeyExchangeFrame,),
        {
            "controller_backend": ctrl_backend,
            "entity_backend": entity_backend,
            "session_key_config": session_key_config,
            "cli_path": cli_path,
            "yubikey_config": yubikey_config,
            "frame_config": frame_config,
        },
    )
    return cls


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description="AVB key exchange FRAME-LEVEL integration tests"
    )
    parser.add_argument(
        "--cli", type=Path, default=None, help="Path to avtp_crypto_tool binary"
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=None,
        help="Build directory to search for CLI",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Verbose test output"
    )
    parser.add_argument(
        "--dump-frames",
        action="store_true",
        help="Print each frame as a Python bytes literal for copy-paste",
    )
    parser.add_argument(
        "--pcap-file",
        type=Path,
        default=None,
        help="Write all frames to a Wireshark pcap file",
    )
    parser.add_argument(
        "--yubikey-entity",
        type=str,
        default=None,
        help="Entity name to use YubiKey for (talker, listener1, or listener2)",
    )
    parser.add_argument(
        "--yubikey-x25519-pubkey",
        type=Path,
        default=None,
        help="Path to hex file with 32-byte X25519 public key",
    )
    parser.add_argument(
        "--yubikey-x25519-keygrip",
        type=str,
        default=None,
        help="GPG keygrip for the X25519 encryption slot",
    )
    parser.add_argument(
        "--yubikey-pin",
        type=str,
        default="123456",
        help="YubiKey card PIN (default: 123456)",
    )
    parser.add_argument(
        "--controller-mac",
        type=str,
        default=None,
        help="Override controller MAC address (hex, e.g. 91e0f0010001)",
    )
    parser.add_argument(
        "--talker-mac",
        type=str,
        default=None,
        help="Override talker MAC address (hex)",
    )
    parser.add_argument(
        "--listener1-mac",
        type=str,
        default=None,
        help="Override listener1 MAC address (hex)",
    )
    parser.add_argument(
        "--listener2-mac",
        type=str,
        default=None,
        help="Override listener2 MAC address (hex)",
    )
    parser.add_argument("tests", nargs="*", help="Specific test names to run")
    args = parser.parse_args()

    global DUMP_FRAMES, PCAP_FILE
    DUMP_FRAMES = args.dump_frames

    # Build FrameConfig from defaults + CLI overrides
    config_kwargs: dict = {}
    mac_overrides = {
        "controller_mac": args.controller_mac,
        "talker_mac": args.talker_mac,
        "listener1_mac": args.listener1_mac,
        "listener2_mac": args.listener2_mac,
    }
    for field, value in mac_overrides.items():
        if value is not None:
            config_kwargs[field] = bytes.fromhex(value)
    frame_config = FrameConfig(**config_kwargs) if config_kwargs else DEFAULT_CONFIG

    cli_path = find_cli(args)
    yubikey_config = _load_yubikey_config(args)
    verbosity = 2 if args.verbose else 1

    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    # Always run frame build/parse unit tests
    suite.addTests(loader.loadTestsFromTestCase(TestFrameBuildParse))

    # Build parameterized test classes
    backends = [Backend.PYTHON]
    if cli_path is not None:
        backends.append(Backend.CPP)

    for ctrl_backend in backends:
        for entity_backend in backends:
            for sk_config in SESSION_KEY_CONFIGS:
                test_cls = make_frame_test_class(
                    ctrl_backend,
                    entity_backend,
                    sk_config,
                    cli_path,
                    frame_config=frame_config,
                )
                _add_tests(suite, loader, test_cls, args.tests)

                if yubikey_config is not None:
                    test_cls = make_frame_test_class(
                        ctrl_backend,
                        entity_backend,
                        sk_config,
                        cli_path,
                        yubikey_config,
                        frame_config=frame_config,
                    )
                    _add_tests(suite, loader, test_cls, args.tests)

    # Use context manager for pcap file to ensure proper cleanup
    pcap_path = args.pcap_file
    if pcap_path is not None:
        PCAP_FILE = open(pcap_path, "wb")
        _pcap_write_header(PCAP_FILE)

    try:
        runner = unittest.TextTestRunner(verbosity=verbosity)
        result = runner.run(suite)
    finally:
        if PCAP_FILE is not None:
            PCAP_FILE.close()
            PCAP_FILE = None
            print(f"\nPcap file written: {pcap_path}")

    sys.exit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
