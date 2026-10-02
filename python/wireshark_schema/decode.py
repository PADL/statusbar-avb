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


POST_HOOKS = {"crf_timestamps": _post_crf_timestamps}


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
