# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Reference decoder: apply the layouts to raw bytes exactly as the Lua does.

``decode_avtpdu`` returns ``{filter_name: value}`` for an AVTPDU, choosing
the same header layout the Lua dissector chooses (by subtype kind and
version). Values are ints (bools as 0/1, bytes as hex strings) so they can
be compared with tshark's JSON output after normalisation.
"""

from __future__ import annotations

from . import layouts as L

COMMON_HEADER_LENGTH = 12
STREAM_V0_HEADER_LENGTH = 16
STREAM_V1_HEADER_LENGTH = 32
IP_AVTPDU_HEADER_LENGTH = 4


def decode_layout(layout: L.Layout, data: bytes, off: int = 0) -> dict[str, int | str]:
    """Every field of ``layout`` read from ``data`` at ``off``."""
    out: dict[str, int | str] = {}
    for f in layout.fields:
        if f.length == 0:
            continue  # a placeholder the walker fills in itself
        chunk = data[off + f.offset : off + f.offset + f.length]
        if len(chunk) != f.length:
            raise ValueError(
                f"{layout.abbr(f)}: needs {f.length} octets at {off + f.offset}"
            )
        if f.kind in ("bytes", "eui48"):
            out[layout.abbr(f)] = chunk.hex()
            continue
        value = int.from_bytes(chunk, "big")
        if f.mask is not None:
            value = (value & f.mask) >> f.shift
        out[layout.abbr(f)] = value
    return out


def header_layout(data: bytes) -> tuple[L.Layout, int]:
    """The header layout and its length for an AVTPDU, as the Lua picks them."""
    subtype = data[0]
    kind = L.HEADER_KIND.get(subtype, "reserved")
    version = (data[1] & 0x70) >> 4
    if kind == "stream":
        if version == 1:
            return L.AVTP_STREAM_V1, STREAM_V1_HEADER_LENGTH
        return L.AVTP_STREAM_V0, STREAM_V0_HEADER_LENGTH
    if kind == "control":
        return L.AVTP_CONTROL, COMMON_HEADER_LENGTH
    if kind == "alternative":
        return L.AVTP_ALTERNATIVE, COMMON_HEADER_LENGTH
    return L.AVTP_COMMON, COMMON_HEADER_LENGTH


SPECS: dict[tuple[int, int], L.SubtypeSpec] = {
    (s.subtype, s.version): s for s in L.SUBTYPE_SPECS
}


def _post_crf_timestamps(data: bytes, start: int, out: dict[str, int | str]) -> None:
    """CRF: the timestamps after the header, as the Lua adds them.

    Wireshark's JSON keeps one value per repeated field (the last), so the
    reference records the last timestamp; a partial trailing quadlet pair is
    left undissected like the Lua does.
    """
    declared = int(out["avb.avtp.crf.crf_data_length"])
    available = len(data) - start
    count = min(declared, available) // 8
    for i in range(count):
        chunk = data[start + (8 * i) : start + (8 * i) + 8]
        out["avb.avtp.crf.timestamp"] = int.from_bytes(chunk, "big")
    rest = start + (8 * count)
    if rest < len(data):
        out["avb.avtp.payload"] = data[rest:].hex()


def _ones_complement_checksum(data: bytes) -> int:
    total = 0
    for i in range(0, len(data) - 1, 2):
        total += (data[i] << 8) | data[i + 1]
    if len(data) % 2:
        total += data[-1] << 8
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return 0xFFFF ^ total


def _crc32_reflected(data: bytes, poly: int) -> int:
    crc = 0xFFFFFFFF
    for octet in data:
        crc ^= octet
        for _ in range(8):
            crc = (crc >> 1) ^ poly if crc & 1 else crc >> 1
    return crc ^ 0xFFFFFFFF


CRC32_POLY = {0x0: 0xEDB88320, 0x1: 0xC8DF352F}


def _acf_pad(spec: L.AcfSpec, fields: dict[str, int | str], padded: int) -> int | None:
    """Pad octets for a message of @p spec, or None when the payload is inconsistent."""
    prefix = spec.layout.prefix
    if spec.pad_mode == "field":
        return int(fields[f"{prefix}.pad"])
    if spec.pad_mode == "none":
        return 0
    if spec.name == "parallel":
        bits = int(fields[f"{prefix}.bit_width"]) or 256
        octets = (bits + 7) // 8
    else:  # sensor / sensor_brief
        count = int(fields[f"{prefix}.num_sensors"]) or 128
        size = int(fields[f"{prefix}.sz"]) or 4
        octets = count * size
    return padded - octets if padded >= octets else None


def decode_acf(payload: bytes, out: dict[str, int | str]) -> None:
    """Walk the ACF messages of @p payload as acf.lua does (repeated keys: last wins)."""
    at = 0
    previous: bytes | None = None
    while at + 2 <= len(payload):
        header = int.from_bytes(payload[at : at + 2], "big")
        msg_type = header >> 9
        quadlets = header & 0x01FF
        if quadlets == 0 or at + (quadlets * 4) > len(payload):
            break
        message = payload[at : at + (quadlets * 4)]
        out["avb.acf.msg_type"] = msg_type
        out["avb.acf.msg_length"] = quadlets
        spec = L.ACF_SPECS.get(msg_type)
        if spec is not None and len(message) >= spec.length:
            fields = decode_layout(spec.layout, message)
            out.update(fields)
            if not spec.fixed_only:
                padded = len(message) - spec.length
                pad = _acf_pad(spec, fields, padded)
                if pad is not None and padded - pad > 0:
                    out[f"{spec.layout.prefix}.payload"] = message[
                        spec.length : spec.length + padded - pad
                    ].hex()
        elif msg_type == L.ACF_MSG_TYPE_CHECKSUM and len(message) >= 4:
            value = int.from_bytes(message[2:4], "big")
            out["avb.acf.checksum.checksum"] = value
            if previous is not None:
                out["avb.acf.checksum.valid"] = (
                    1 if _ones_complement_checksum(previous) == value else 0
                )
        elif msg_type == L.ACF_MSG_TYPE_CRC and len(message) >= 4:
            crc_type = message[3] & 0x0F
            out["avb.acf.crc.crc_type"] = crc_type
            crc_data = message[4:]
            if crc_data:
                out["avb.acf.crc.crc_data"] = crc_data.hex()
            if previous is not None and crc_type in CRC32_POLY and len(crc_data) == 4:
                expected = _crc32_reflected(previous, CRC32_POLY[crc_type])
                out["avb.acf.crc.valid"] = (
                    1 if int.from_bytes(crc_data, "big") == expected else 0
                )
        previous = message
        at += len(message)


def _post_acf(data: bytes, start: int, out: dict[str, int | str]) -> None:
    """TSCF/NTSCF: the ACF messages, bounded by the header's declared length."""
    subtype = data[0]
    version = (data[1] & 0x70) >> 4
    if subtype == 0x05:
        declared = int(out["avb.avtp.tscf.stream_data_length"])
    else:
        declared = int(out["avb.avtp.ntscf.ntscf_data_length"])
    del version
    available = len(data) - start
    decode_acf(data[start : start + min(declared, available)], out)


POST_HOOKS = {"crf_timestamps": _post_crf_timestamps, "acf": _post_acf}


def decode_avtpdu(data: bytes) -> dict[str, int | str]:
    """The fields the dissector shows for one AVTPDU (header + payload)."""
    if not data:
        return {}
    subtype = data[0]
    version = (data[1] & 0x70) >> 4 if len(data) > 1 else 0
    spec = SPECS.get((subtype, version))
    if spec is not None:
        if len(data) < spec.header_length:
            return {"avb.avtp.subtype": subtype}
        out: dict[str, int | str] = {}
        for layout in spec.layouts:
            out.update(decode_layout(layout, data))
        if spec.post is not None:
            POST_HOOKS[spec.post](data, spec.header_length, out)
        elif len(data) > spec.header_length:
            out["avb.avtp.payload"] = data[spec.header_length :].hex()
        return out
    layout, length = header_layout(data)
    if len(data) < length:
        return {"avb.avtp.subtype": subtype}
    out = decode_layout(layout, data)
    if len(data) > length:
        out["avb.avtp.payload"] = data[length:].hex()
    return out


def decode_ip_avtpdu(udp_payload: bytes) -> dict[str, int | str]:
    """The fields for an IP AVTPDU: the encapsulation header, then the AVTPDU."""
    if len(udp_payload) < IP_AVTPDU_HEADER_LENGTH:
        return {}
    out = decode_layout(L.IP_AVTPDU, udp_payload)
    out.update(decode_avtpdu(udp_payload[IP_AVTPDU_HEADER_LENGTH:]))
    return out
