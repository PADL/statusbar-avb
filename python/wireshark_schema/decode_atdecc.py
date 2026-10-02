# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Reference decoder for the ATDECC PDUs, mirroring statusbar_avb/atdecc.lua."""

from __future__ import annotations

from . import atdecc_layouts as A
from . import atdecc_table as T
from .decode import decode_layout

ADP = 0xFA
AECP = 0xFB
ACMP = 0xFC
ADP_LENGTH = 68
ACMP_LENGTH = 56


def _descriptor(data: bytes, at: int, stop: int, out: dict) -> None:
    kind = int.from_bytes(data[at : at + 2], "big")
    spec = A.DESCRIPTORS.get(kind)
    if spec is None or stop - at < spec.length:
        return
    out.update(decode_layout(spec.layout, data, at))
    trailer = spec.trailer
    if trailer is None:
        return
    members = {m["name"]: m for m in T.DESCRIPTOR_STRUCTS[spec.struct]["members"]}
    count = int.from_bytes(
        data[at + members[trailer.count_field]["offset"] :][:2], "big"
    )
    offset = int.from_bytes(
        data[at + members[trailer.offset_field]["offset"] :][:2], "big"
    )
    if trailer.element == "raw":
        if at + offset < stop:
            out[A.DESCRIPTOR_LAYOUTS["DescriptorControl"].abbr(A.DESCRIPTOR_RAW)] = (
                data[at + offset : stop].hex()
            )
        return
    start = at + offset
    if start + (count * trailer.element_size) > stop:
        count = max(0, (stop - start) // trailer.element_size)
    element = A.TRAILER_ELEMENTS[trailer.element]
    for i in range(count):
        out.update(decode_layout(element, data, start + (i * trailer.element_size)))


def _control_values(data: bytes, at: int, stop: int, out: dict) -> None:
    if stop - at >= T.JDKS_LOG_BLOB["length"]:
        vendor = data[at : at + 8].hex()
        if vendor == T.JDKS_CONTROL_LOG_TEXT:
            out.update(decode_layout(A.JDKS_LOG, data, at))
            blob_size = int.from_bytes(data[at + 8 : at + 12], "big")
            text_length = min(
                max(blob_size - 2, 0), stop - at - T.JDKS_LOG_BLOB["length"]
            )
            if text_length > 0:
                text = data[at + T.JDKS_LOG_BLOB["length"] :][:text_length]
                out[A.JDKS_LOG.abbr(A.JDKS_LOG_TEXT)] = text.split(b"\0", 1)[0].decode(
                    "utf-8", "replace"
                )
            return
        if (
            vendor == T.JDKS_CONTROL_IPV4_PARAMETERS
            and stop - at >= 8 + T.JDKS_IPV4_PARAMS["length"]
        ):
            out.update(decode_layout(A.JDKS_IPV4, data, at + 8))
            return
    if stop > at:
        out[A.AEM.abbr(A.AEM_PAYLOAD_RAW)] = data[at:stop].hex()


def _aem(data: bytes, stop: int, is_command: bool, out: dict) -> None:
    out.update(decode_layout(A.AEM, data))
    code = int.from_bytes(data[22:24], "big") & 0x3FFF
    specs = A.AEM_PAYLOADS.get(code)
    spec = (specs[0] if is_command else specs[1]) if specs else None
    at = A.AEM_HEADER_LENGTH
    if spec is not None and stop - at >= spec.length:
        out.update(decode_layout(spec.layout, data))
        at += spec.length
        if spec.trailer == "descriptor":
            if stop - at >= 4:
                _descriptor(data, at, stop, out)
            at = stop
        elif spec.trailer == "values":
            _control_values(data, at, stop, out)
            at = stop
        elif spec.trailer == "mappings":
            count_at = A.AEM_HEADER_LENGTH + (
                6 if spec.name == "audio_map_response" else 4
            )
            count = int.from_bytes(data[count_at : count_at + 2], "big")
            size = T.AEM_PAYLOAD_STRUCTS["AemAudioMapping"]["length"]
            for _ in range(count):
                if at + size > stop:
                    break
                out.update(decode_layout(A.AUDIO_MAPPING, data, at))
                at += size
    if stop > at:
        out[A.AEM.abbr(A.AEM_PAYLOAD_RAW)] = data[at:stop].hex()


def decode_atdecc(data: bytes, out: dict) -> None:
    """The avb.atdecc.* fields for one ADP/AECP/ACMP AVTPDU."""
    subtype = data[0]
    if subtype == ADP:
        if len(data) >= ADP_LENGTH:
            out.update(decode_layout(A.ADP, data))
        return
    if subtype == ACMP:
        if len(data) >= ACMP_LENGTH:
            out.update(decode_layout(A.ACMP, data))
        return
    if len(data) < A.AECP_HEADER_LENGTH:
        return
    out.update(decode_layout(A.AECP, data))
    msg_type = data[1] & 0x0F
    cdl = int.from_bytes(data[2:4], "big") & 0x7FF
    stop = min(12 + cdl, len(data))
    is_command = msg_type % 2 == 0
    if msg_type in (0, 1) and len(data) >= A.AEM_HEADER_LENGTH:
        _aem(data, stop, is_command, out)
    elif msg_type in (2, 3) and len(data) >= A.AA_HEADER_LENGTH:
        out.update(decode_layout(A.AA, data))
        at = A.AA_HEADER_LENGTH
        while at + 10 <= stop:
            length = int.from_bytes(data[at : at + 2], "big") & 0x0FFF
            out.update(decode_layout(A.AA_TLV, data, at))
            if length > 0 and at + 10 + length <= stop:
                out[A.AA_TLV.abbr(A.AA_TLV_DATA)] = data[
                    at + 10 : at + 10 + length
                ].hex()
            at += 10 + length
    elif msg_type in (6, 7) and len(data) >= A.VU_HEADER_LENGTH:
        out.update(decode_layout(A.VU, data))
        if stop > A.VU_HEADER_LENGTH:
            out[A.VU.abbr(A.VU_PAYLOAD)] = data[A.VU_HEADER_LENGTH : stop].hex()
    elif stop > A.AECP_HEADER_LENGTH:
        out[A.AECP.abbr(A.AECP_PAYLOAD)] = data[A.AECP_HEADER_LENGTH : stop].hex()
