# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Wire-format serialization helpers for AEM descriptors (big-endian)."""

from __future__ import annotations

import struct


def _checked(val: int, bits: int) -> int:
    """Range-check a wire integer instead of silently masking it.

    Accepts the unsigned range [0, 2^bits) plus two's-complement negatives
    down to -2^(bits-1) (fields such as log_sync_interval are signed on the
    wire). Anything wider raises, so an authoring typo like signal_index
    70000 fails the build rather than silently truncating to 4464.
    """
    if val < -(1 << (bits - 1)) or val >= (1 << bits):
        raise ValueError(f"value {val} does not fit a {bits}-bit wire field")
    return val & ((1 << bits) - 1)


def pack_u8(val: int) -> bytes:
    return struct.pack("!B", _checked(val, 8))


def pack_u16(val: int) -> bytes:
    return struct.pack("!H", _checked(val, 16))


def pack_u32(val: int) -> bytes:
    return struct.pack("!I", _checked(val, 32))


def pack_u64(val: int) -> bytes:
    return struct.pack("!Q", _checked(val, 64))


def pack_string64(s: str) -> bytes:
    raw = s.encode("utf-8")[:64]
    return raw.ljust(64, b"\x00")


def pack_eui48(val: int) -> bytes:
    return struct.pack("!Q", val)[2:]  # last 6 bytes of 8-byte big-endian


def pack_eui64(val: int) -> bytes:
    return struct.pack("!Q", val)


def pack_localized_string_ref(offset: int, index: int) -> bytes:
    """Pack a 2-byte localized string reference (13-bit offset + 3-bit index)."""
    return pack_u16(((offset & 0x1FFF) << 3) | (index & 0x07))


def unpack_u8(data: bytes, pos: int) -> int:
    return struct.unpack_from("!B", data, pos)[0]


def unpack_u16(data: bytes, pos: int) -> int:
    return struct.unpack_from("!H", data, pos)[0]


def unpack_u32(data: bytes, pos: int) -> int:
    return struct.unpack_from("!I", data, pos)[0]


def unpack_u64(data: bytes, pos: int) -> int:
    return struct.unpack_from("!Q", data, pos)[0]


def unpack_string64(data: bytes, pos: int) -> str:
    raw = data[pos : pos + 64]
    end = raw.find(b"\x00")
    if end >= 0:
        raw = raw[:end]
    return raw.decode("utf-8", errors="replace")


def unpack_eui48(data: bytes, pos: int) -> int:
    return struct.unpack_from("!Q", b"\x00\x00" + data[pos : pos + 6], 0)[0]


def unpack_eui64(data: bytes, pos: int) -> int:
    return struct.unpack_from("!Q", data, pos)[0]


def unpack_localized_string_ref(data: bytes, pos: int) -> tuple[int, int]:
    """Unpack a 2-byte localized string reference -> (offset, index)."""
    val = unpack_u16(data, pos)
    return (val >> 3) & 0x1FFF, val & 0x07
