# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Reference decoder for the ATDECC PDUs, mirroring statusbar_avb/atdecc.lua.

Repeated fields keep their last occurrence (what tshark's JSON keeps), and the
control value types seen in descriptors are remembered across frames in the
order they are decoded, exactly as the Lua does.
"""

from __future__ import annotations

from . import atdecc_layouts as A
from . import atdecc_std as STD
from . import atdecc_table as T
from .decode import decode_layout

ADP = 0xFA
AECP = 0xFB
ACMP = 0xFC
ADP_LENGTH = 68
ACMP_LENGTH = 56
DESCRIPTOR_MATRIX = 0x0010

#: "<target>:<descriptor_type>:<descriptor_index>" -> (value_type, count)
control_types: dict[str, tuple[int, int]] = {}


def _u16(data: bytes, at: int) -> int:
    return int.from_bytes(data[at : at + 2], "big")


def _eui64_hex(data: bytes, at: int) -> str:
    return "0x" + data[at : at + 8].hex()


def _stream_format(data: bytes, at: int, out: dict) -> None:
    layout = A.STREAM_FORMAT_LAYOUTS.get(data[at])
    if layout is not None:
        out.update(decode_layout(layout, data, at))


def _typed(fam: tuple, data: bytes, at: int, out: dict) -> None:
    family, size, kind = fam
    name = "value_string_ref" if kind == "string_ref" else f"value_{kind}"
    f = next(f for f in A.CONTROL_VALUE.fields if f.name == name)
    out.update(decode_layout(A.Layout("v", A.CONTROL_PREFIX, "", (f,)), data, at))


def _units(data: bytes, at: int, out: dict) -> None:
    fields = tuple(
        f
        for f in A.CONTROL_VALUE.fields
        if f.name in ("units_multiplier", "units_code")
    )
    out.update(decode_layout(A.Layout("u", A.CONTROL_PREFIX, "", fields), data, at))


def _localized(data: bytes, at: int, out: dict) -> None:
    f = next(f for f in A.CONTROL_VALUE.fields if f.name == "localized_string")
    out.update(decode_layout(A.Layout("l", A.CONTROL_PREFIX, "", (f,)), data, at))


def _control_values(
    data: bytes,
    at: int,
    stop: int,
    value_type: int,
    n: int,
    mode: str,
    out: dict,
    raw_key: str | None,
) -> int:
    """add_control_values in atdecc.lua."""
    fam = STD.CONTROL_VALUE_FAMILY.get(value_type)
    if fam is None or fam[0] in ("vendor", "expansion"):
        if stop > at:
            out[A.CONTROL_VALUE.abbr(A.CONTROL_VENDOR)] = data[at:stop].hex()
        return stop
    family, v, _kind = fam
    if family == "linear":
        if mode == "current":
            for _ in range(n):
                if at + v > stop:
                    break
                _typed(fam, data, at, out)
                at += v
        else:
            entry = 5 * v + 4
            for _ in range(n):
                if at + entry > stop:
                    break
                for k in range(5):
                    _typed(fam, data, at + k * v, out)
                _units(data, at + 5 * v, out)
                _localized(data, at + 5 * v + 2, out)
                at += entry
    elif family == "selector":
        if mode == "current":
            if at + v <= stop:
                _typed(fam, data, at, out)
                at += v
        else:
            total = (n + 2) * v + 2
            if at + total <= stop:
                for k in range(n + 2):
                    _typed(fam, data, at + k * v, out)
                _units(data, at + (n + 2) * v, out)
                at += total
    elif family == "array":
        if mode == "current":
            for _ in range(n):
                if at + v > stop:
                    break
                _typed(fam, data, at, out)
                at += v
        else:
            total = (n + 4) * v + 4
            if at + total <= stop:
                for k in range(4):
                    _typed(fam, data, at + k * v, out)
                _units(data, at + 4 * v, out)
                _localized(data, at + 4 * v + 2, out)
                for i in range(n):
                    _typed(fam, data, at + 4 * v + 4 + i * v, out)
                at += total
    elif family == "utf8":
        if stop > at:
            out[A.CONTROL_VALUE.abbr(A.CONTROL_UTF8)] = (
                data[at:stop].split(b"\0", 1)[0].decode("utf-8", "replace")
            )
        at = stop
    elif family == "bode_plot":
        if mode != "current":
            if at + 48 > stop:
                return at
            out.update(decode_layout(A.CONTROL_BODE_HEADER, data, at))
            at += 48
        for _ in range(n):
            if at + 12 > stop:
                break
            out.update(decode_layout(A.CONTROL_BODE_POINT, data, at))
            at += 12
    elif family == "smpte_time":
        if at + 10 <= stop:
            out.update(decode_layout(A.CONTROL_SMPTE, data, at))
            at += 10
    elif family == "sample_rate":
        if at + 16 <= stop:
            out.update(decode_layout(A.CONTROL_SAMPLE_RATE, data, at))
            at += 16
        elif at + 4 <= stop:
            fields = A.CONTROL_SAMPLE_RATE.fields[:2]
            out.update(
                decode_layout(
                    A.Layout("sr", A.CONTROL_SAMPLE_RATE.prefix, "", fields), data, at
                )
            )
            at += 4
    elif family == "gptp_time":
        if at + 10 <= stop:
            out.update(decode_layout(A.CONTROL_GPTP, data, at))
            at += 10
    if raw_key is not None and stop > at:
        out[raw_key] = data[at:stop].hex()
        at = stop
    return at


def _descriptor(data: bytes, at: int, stop: int, out: dict, target: str | None) -> None:
    kind = _u16(data, at)
    spec = A.DESCRIPTORS.get(kind)
    if spec is None or stop - at < spec.length:
        return
    out.update(decode_layout(spec.layout, data, at))
    for off in A.STREAM_FORMAT_AT.get(spec.name, ()):
        _stream_format(data, at + off, out)
    members = {m["name"]: m for m in T.DESCRIPTOR_STRUCTS[spec.struct]["members"]}
    for tab in spec.trailers:
        count = (
            _u16(data, at + members[tab.count_field]["offset"])
            if tab.count_field
            else 1
        )
        start = at + _u16(data, at + members[tab.offset_field]["offset"])
        if tab.element == "values":
            value_type = (
                _u16(data, at + members[tab.value_type_field]["offset"]) & 0x3FFF
            )
            if target is not None:
                control_types[f"{target}:{kind}:{_u16(data, at + 2)}"] = (
                    value_type,
                    count,
                )
            if start < stop:
                _control_values(data, start, stop, value_type, count, "full", out, None)
        elif start + count * tab.element_size <= stop:
            element = A.TRAILER_ELEMENTS[tab.element]
            for i in range(count):
                pos = start + i * tab.element_size
                out.update(decode_layout(element, data, pos))
                if tab.element == "stream_format":
                    _stream_format(data, pos, out)


def _control_values_untyped(data: bytes, at: int, stop: int, out: dict) -> None:
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


def _payload_values(
    data: bytes, at: int, stop: int, target: str, spec: A.PayloadSpec, out: dict
) -> None:
    kind = _u16(data, A.AEM_HEADER_LENGTH)
    index = _u16(data, A.AEM_HEADER_LENGTH + 2)
    known = control_types.get(f"{target}:{kind}:{index}")
    if known is not None and stop > at:
        value_type, n = known
        if kind == DESCRIPTOR_MATRIX:
            n = _u16(data, A.AEM_HEADER_LENGTH + 12) & 0x3FFF
        elif "mixer" in spec.name:
            n = 1
        _control_values(
            data, at, stop, value_type, n, "current", out, A.AEM.abbr(A.AEM_PAYLOAD_RAW)
        )
        return
    _control_values_untyped(data, at, stop, out)


def _dynamic_infos(data: bytes, at: int, stop: int, is_command: bool, out: dict) -> int:
    """GET_DYNAMIC_INFO entries, as dissect_dynamic_infos in atdecc.lua."""
    while at + A.DYNAMIC_INFO_HEADER_LENGTH <= stop:
        length = _u16(data, at)
        code = _u16(data, at + 6) & 0x3FFF
        total = A.DYNAMIC_INFO_HEADER_LENGTH + length
        if at + total > stop:
            total = stop - at
        out.update(decode_layout(A.DYNAMIC_INFO, data, at))
        specs = A.AEM_PAYLOADS.get(code)
        spec = (specs[0] if is_command else specs[1]) if specs else None
        body = at + A.DYNAMIC_INFO_HEADER_LENGTH
        if spec is not None and spec.trailer is None and length >= spec.length:
            out.update(decode_layout(spec.layout, data, body - A.AEM_HEADER_LENGTH))
            if length > spec.length:
                out[A.AEM.abbr(A.AEM_PAYLOAD_RAW)] = data[
                    body + spec.length : body + length
                ].hex()
        elif length > 0:
            out[A.AEM.abbr(A.AEM_PAYLOAD_RAW)] = data[
                body : body + min(length, stop - body)
            ].hex()
        at += total
    return at


def _aem(data: bytes, stop: int, is_command: bool, out: dict) -> None:
    out.update(decode_layout(A.AEM, data))
    code = _u16(data, 22) & 0x3FFF
    specs = A.AEM_PAYLOADS.get(code)
    spec = (specs[0] if is_command else specs[1]) if specs else None
    target = _eui64_hex(data, 4)
    at = A.AEM_HEADER_LENGTH
    if spec is not None and stop - at >= spec.length:
        out.update(decode_layout(spec.layout, data))
        for off in A.STREAM_FORMAT_AT.get(spec.name, ()):
            _stream_format(data, off, out)
        at += spec.length
        trailer = spec.trailer
        if trailer is not None:
            if trailer.kind == "descriptor":
                if stop - at >= 4:
                    _descriptor(data, at, stop, out, target)
                at = stop
            elif trailer.kind in ("values", "raw"):
                _payload_values(data, at, stop, target, spec, out)
                at = stop
            elif trailer.kind == "elements":
                members = {
                    m["name"]: m for m in A.ALL_PAYLOAD_STRUCTS[spec.struct]["members"]
                }
                count = _u16(
                    data, A.AEM_HEADER_LENGTH + members[trailer.count_field]["offset"]
                )
                size = A.ELEMENT_SIZE[trailer.element]
                element = A.TRAILER_ELEMENTS[trailer.element]
                for _ in range(count):
                    if at + size > stop:
                        break
                    out.update(decode_layout(element, data, at))
                    at += size
            elif trailer.kind == "blob":
                members = {
                    m["name"]: m for m in A.ALL_PAYLOAD_STRUCTS[spec.struct]["members"]
                }
                length = min(
                    _u16(
                        data,
                        A.AEM_HEADER_LENGTH + members[trailer.count_field]["offset"],
                    ),
                    stop - at,
                )
                if length > 0:
                    out[A.AEM_BLOBS.abbr(A.AEM_BLOB_FIELDS[trailer.element])] = data[
                        at : at + length
                    ].hex()
                    at += length
            elif trailer.kind == "dynamic_infos":
                at = _dynamic_infos(data, at, stop, is_command, out)
    if stop > at:
        out[A.AEM.abbr(A.AEM_PAYLOAD_RAW)] = data[at:stop].hex()


def _mvu(data: bytes, stop: int, is_command: bool, out: dict) -> None:
    out.update(decode_layout(A.MVU, data))
    code = _u16(data, 28) & 0x7FFF
    specs = A.MVU_PAYLOADS.get(code)
    spec = (specs[0] if is_command else specs[1]) if specs else None
    at = A.MVU_HEADER_LENGTH
    if spec is not None and stop - at >= spec.length:
        out.update(decode_layout(spec.layout, data))
        at += spec.length
        if spec.name == "mvu_get_milan_info_response":
            fields = tuple(
                f for f in A.MVU_FLAGS.fields if f.name.startswith("features_flags.")
            )
            out.update(
                decode_layout(A.Layout("ff", A.MVU_FLAGS.prefix, "", fields), data)
            )
        elif spec.name == "mvu_media_clock_reference_info":
            fields = tuple(
                f for f in A.MVU_FLAGS.fields if f.name.startswith("mcr_flags.")
            )
            out.update(
                decode_layout(A.Layout("mf", A.MVU_FLAGS.prefix, "", fields), data)
            )
        if spec.optional_name is not None and stop - at >= 64:
            f = next(f for f in spec.layout.fields if f.name == spec.optional_name)
            out[spec.layout.abbr(f)] = (
                data[at : at + 64].split(b"\0", 1)[0].decode("utf-8", "replace")
            )
            at += 64
    if stop > at:
        out[A.VU.abbr(A.VU_PAYLOAD)] = data[at:stop].hex()


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
    cdl = _u16(data, 2) & 0x7FF
    stop = min(12 + cdl, len(data))
    is_command = msg_type % 2 == 0
    if msg_type in (0, 1) and len(data) >= A.AEM_HEADER_LENGTH:
        _aem(data, stop, is_command, out)
    elif msg_type in (2, 3) and len(data) >= A.AA_HEADER_LENGTH:
        out.update(decode_layout(A.AA, data))
        at = A.AA_HEADER_LENGTH
        while at + 10 <= stop:
            length = _u16(data, at) & 0x0FFF
            out.update(decode_layout(A.AA_TLV, data, at))
            if length > 0 and at + 10 + length <= stop:
                out[A.AA_TLV.abbr(A.AA_TLV_DATA)] = data[
                    at + 10 : at + 10 + length
                ].hex()
            at += 10 + length
    elif msg_type in (4, 5) and len(data) >= A.AVC_HEADER_LENGTH:
        out.update(
            decode_layout(A.Layout("avc", A.AVC.prefix, "", A.AVC.fields[:1]), data)
        )
        length = min(_u16(data, 22), stop - A.AVC_HEADER_LENGTH)
        if length > 0:
            out[A.AVC.abbr(A.AVC_PAYLOAD)] = data[
                A.AVC_HEADER_LENGTH : A.AVC_HEADER_LENGTH + length
            ].hex()
    elif msg_type in (8, 9) and len(data) >= A.HDCP_APM_HEADER_LENGTH:
        out.update(
            decode_layout(
                A.Layout("hdcp", A.HDCP_APM.prefix, "", A.HDCP_APM.fields[:-1]), data
            )
        )
        if stop > A.HDCP_APM_HEADER_LENGTH:
            out[A.HDCP_APM.abbr(A.HDCP_APM_DATA)] = data[
                A.HDCP_APM_HEADER_LENGTH : stop
            ].hex()
    elif msg_type in (6, 7) and len(data) >= A.VU_HEADER_LENGTH:
        out.update(decode_layout(A.VU, data))
        if (
            data[22:28].hex() == STD.MVU_PROTOCOL_ID
            and len(data) >= A.MVU_HEADER_LENGTH
        ):
            _mvu(data, stop, is_command, out)
        elif stop > A.VU_HEADER_LENGTH:
            out[A.VU.abbr(A.VU_PAYLOAD)] = data[A.VU_HEADER_LENGTH : stop].hex()
    elif stop > A.AECP_HEADER_LENGTH:
        out[A.AECP.abbr(A.AECP_PAYLOAD)] = data[A.AECP_HEADER_LENGTH : stop].hex()
