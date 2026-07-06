# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""AAF stream format encode/decode per IEEE 1722-2016."""

from __future__ import annotations

# NSR codes from IEEE 1722-2016 Table 11
_RATE_TO_NSR: dict[int, int] = {
    8000: 0x01,
    16000: 0x02,
    32000: 0x03,
    44100: 0x04,
    48000: 0x05,
    88200: 0x06,
    96000: 0x07,
    176400: 0x08,
    192000: 0x09,
    24000: 0x0A,
}
_NSR_TO_RATE: dict[int, int] = {v: k for k, v in _RATE_TO_NSR.items()}

AAF_SUBTYPE = 0x02

# AAF "format" (sample-format) field, IEEE 1722-2016 Table 20.
AAF_FORMAT_USER = 0x00
AAF_FORMAT_FLOAT_32 = 0x01
AAF_FORMAT_INT_32 = 0x02
AAF_FORMAT_INT_24 = 0x03
AAF_FORMAT_INT_16 = 0x04


def encode_aaf_stream_format(
    rate: int,
    channels: int,
    depth: int,
    aaf_format: int = AAF_FORMAT_INT_32,
    samples_per_frame: int | None = None,
) -> int:
    """Encode AAF-PCM parameters into an 8-byte IEEE 1722-2016 stream format value (v=0).

    Wire layout (byte 0 = MSB), matching the authoritative C++ aaf_8ch_96k_32bit()
    (0x020702200200C000):
      byte 0            : 0x02 (AAF subtype)
      byte 1            : nsr in the LOW nibble (0x07 = 96 kHz)
      byte 2            : format -- sample format (0x02 = INT_32)
      byte 3            : bit_depth (0x20 = 32)
      bytes 4-7 (BE 32) : channels_per_frame[9:0] << 22 | samples_per_frame[9:0] << 12

    samples_per_frame defaults to the SR class A value rate/8000 (one 125 us packet;
    12 @ 96 kHz).
    """
    if rate not in _RATE_TO_NSR:
        raise ValueError(
            f"Unsupported sample rate {rate}, valid: {sorted(_RATE_TO_NSR.keys())}"
        )
    if channels < 1 or channels > 1023:
        raise ValueError(f"Channels must be 1-1023, got {channels}")
    if depth < 1 or depth > 255:
        raise ValueError(f"Bit depth must be 1-255, got {depth}")
    if samples_per_frame is None:
        samples_per_frame = rate // 8000
    if samples_per_frame < 0 or samples_per_frame > 1023:
        raise ValueError(f"samples_per_frame must be 0-1023, got {samples_per_frame}")

    b0 = AAF_SUBTYPE
    b1 = _RATE_TO_NSR[rate] & 0x0F  # nsr in the low nibble
    b2 = aaf_format & 0xFF
    b3 = depth & 0xFF
    lower = ((channels & 0x3FF) << 22) | ((samples_per_frame & 0x3FF) << 12)
    return (b0 << 56) | (b1 << 48) | (b2 << 40) | (b3 << 32) | lower


def decode_aaf_stream_format(fmt: int) -> dict | None:
    """Decode an 8-byte stream format to AAF parameters.

    Returns {"type": "AAF", "rate": int, "channels": int, "depth": int} or None
    if the format is not AAF.
    """
    if ((fmt >> 56) & 0xFF) != AAF_SUBTYPE:
        return None
    nsr = (fmt >> 48) & 0x0F  # nsr in the low nibble of byte 1
    rate = _NSR_TO_RATE.get(nsr)
    if rate is None:
        return None
    aaf_format = (fmt >> 40) & 0xFF
    depth = (fmt >> 32) & 0xFF
    lower = fmt & 0xFFFFFFFF
    channels = (lower >> 22) & 0x3FF
    samples_per_frame = (lower >> 12) & 0x3FF
    return {
        "type": "AAF",
        "rate": rate,
        "channels": channels,
        "depth": depth,
        "format": aaf_format,
        "samples_per_frame": samples_per_frame,
    }


def parse_stream_format(fmt_obj) -> int:
    """Parse a stream format from JSON (structured object or hex string) to int."""
    if isinstance(fmt_obj, str):
        return int(fmt_obj, 16)
    if isinstance(fmt_obj, dict):
        fmt_type = fmt_obj.get("type", "")
        if fmt_type == "AAF":
            return encode_aaf_stream_format(
                fmt_obj["rate"], fmt_obj["channels"], fmt_obj["depth"]
            )
        raise ValueError(f"Unsupported stream format type: {fmt_type!r}")
    raise ValueError(f"Stream format must be object or hex string, got {type(fmt_obj)}")


def format_stream_format(fmt: int) -> dict | str:
    """Convert a stream format int to structured object (AAF) or hex string."""
    decoded = decode_aaf_stream_format(fmt)
    if decoded is not None:
        return decoded
    return f"0x{fmt:016X}"
