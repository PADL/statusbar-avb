# Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
# SPDX-License-Identifier: MIT
"""Wire layouts: fields, layouts and value tables.

A ``Field`` is one dissectable value: its filter abbreviation, display name,
byte offset and length within its layout, wire type, an optional bit mask
inside the containing integer, display base and value-name table. A
``Layout`` is an ordered list of fields with a shared abbreviation prefix.
Everything is authored from IEEE Std 1722-2025 / 1722.1 and this repository's
C++ wire structs - never from another dissector.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import acf_table as T


@dataclass(frozen=True)
class Field:
    """One field of a layout."""

    name: str
    offset: int
    length: int
    kind: str  # u8 u16 u24 u32 u64 i8 i16 i24 i32 i64 f32 f64 bool bytes eui48 string
    doc: str = ""
    mask: int | None = None
    base: str = "dec"  # dec hex
    values: str | None = None  # name of a value table in VALUE_TABLES
    repeat: int = 1  # a fixed array of `repeat` consecutive values of `length` octets

    @property
    def shift(self) -> int:
        """Right shift that brings a masked field down to bit 0."""
        if self.mask is None:
            return 0
        shift = 0
        mask = self.mask
        while mask & 1 == 0:
            mask >>= 1
            shift += 1
        return shift


@dataclass(frozen=True)
class Layout:
    """A named group of fields sharing an abbreviation prefix."""

    name: str
    prefix: str
    doc: str
    fields: tuple[Field, ...]

    def abbr(self, f: Field) -> str:
        """The Wireshark filter name of ``f`` in this layout."""
        return f"{self.prefix}.{f.name}"


@dataclass(frozen=True)
class ValueTable:
    """A code -> name table used for display and for the reference decoder."""

    name: str
    entries: dict[int, str] = field(default_factory=dict)


# IEEE 1722-2025 Table 6 - AVTP subtypes, with the header kind the C++
# avtp_get_header_type() assigns (stream / control / alternative).
AVTP_SUBTYPES: list[tuple[int, str, str, str]] = [
    (0x00, "iec_61883_iidc", "IEC 61883/IIDC", "stream"),
    (0x01, "mma_stream", "MMA Stream", "stream"),
    (0x02, "aaf", "AVTP Audio Format", "stream"),
    (0x03, "cvf", "Compressed Video Format", "stream"),
    (0x04, "crf", "Clock Reference Format", "alternative"),
    (0x05, "tscf", "Time-Synchronous Control Format", "stream"),
    (0x06, "svf", "SDI Video Format", "stream"),
    (0x07, "rvf", "Raw Video Format", "stream"),
    (0x6E, "aef_continuous", "AES Encrypted Format Continuous", "alternative"),
    (0x6F, "vsf_stream", "Vendor Specific Format Stream", "stream"),
    (0x7F, "ef_stream", "Experimental Format Stream", "stream"),
    (0x82, "ntscf", "Non-Time-Synchronous Control Format", "alternative"),
    (0xEC, "escf", "ECC Signed Control Format", "alternative"),
    (0xED, "eecf", "ECC Encrypted Control Format", "alternative"),
    (0xEE, "aef_discrete", "AES Encrypted Format Discrete", "alternative"),
    (0xFA, "adp", "ATDECC Discovery Protocol", "control"),
    (0xFB, "aecp", "ATDECC Enumeration and Control Protocol", "control"),
    (0xFC, "acmp", "ATDECC Connection Management Protocol", "control"),
    (0xFE, "maap", "MAAP", "control"),
    (0xFF, "ef_control", "Experimental Format Control", "control"),
]

VALUE_TABLES: dict[str, ValueTable] = {
    "avtp_subtype": ValueTable(
        "avtp_subtype", {code: title for code, _, title, _ in AVTP_SUBTYPES}
    ),
    # IEEE 1722-2025 Table 11 (AAF format), Table 12 (nsr)
    "aaf_format": ValueTable(
        "aaf_format",
        {
            0x00: "User specified",
            0x01: "32-bit float",
            0x02: "32-bit integer",
            0x03: "24-bit integer",
            0x04: "16-bit integer",
            0x05: "32-bit AES3",
        },
    ),
    "aaf_nsr": ValueTable(
        "aaf_nsr",
        {
            0x00: "User specified",
            0x01: "8 kHz",
            0x02: "16 kHz",
            0x03: "32 kHz",
            0x04: "44.1 kHz",
            0x05: "48 kHz",
            0x06: "88.2 kHz",
            0x07: "96 kHz",
            0x08: "176.4 kHz",
            0x09: "192 kHz",
            0x0A: "24 kHz",
        },
    ),
    # IEEE 1722-2025 Table 17 (aes3_dt_ref)
    "aaf_aes3_dt_ref": ValueTable(
        "aaf_aes3_dt_ref",
        {
            0: "DT_UNSPECIFIED",
            1: "DT_PCM",
            2: "DT_SMPTE338",
            3: "DT_IEC61937",
            4: "DT_VENDOR",
        },
    ),
    # IEC 61883-6: the AM824 label octet (exact codes; the ranges are named in
    # avtp_streams.lua) and the FDF sample-frequency code the CIP header carries
    "am824_label": ValueTable(
        "am824_label",
        {
            0x40: "MBLA (multi-bit linear audio)",
            0x80: "MIDI conformant, no data",
            0x81: "MIDI conformant, 1 byte",
            0x82: "MIDI conformant, 2 bytes",
            0x83: "MIDI conformant, 3 bytes",
            0x88: "SMPTE time code",
        },
    ),
    # IEC 61883-1 FMT codes carried in the CIP header
    "iec61883_fmt": ValueTable(
        "iec61883_fmt",
        {
            0x00: "DV (IEC 61883-2/-3/-5)",
            0x10: "Audio and music (IEC 61883-6, AM824)",
            0x20: "MPEG2-TS (IEC 61883-4)",
            0x21: "ITU-R BO.1294 System B (IEC 61883-7)",
            0x3F: "No data",
        },
    ),
    # IEEE 1722-2025 Table 20 / Table 21 (CVF format, format_subtype)
    "cvf_format": ValueTable("cvf_format", {0x02: "RFC"}),
    "cvf_format_subtype": ValueTable(
        "cvf_format_subtype",
        {0x00: "MJPEG", 0x01: "H.264", 0x02: "JPEG 2000", 0x03: "H.265"},
    ),
    # RFC 6184 Table 1 / RFC 7798 NAL unit types as they appear in a CVF payload
    "h264_nal_type": ValueTable(
        "h264_nal_type",
        {
            1: "Coded slice (non-IDR)",
            5: "Coded slice (IDR)",
            6: "SEI",
            7: "SPS",
            8: "PPS",
            9: "Access unit delimiter",
            24: "STAP-A",
            25: "STAP-B",
            26: "MTAP16",
            27: "MTAP24",
            28: "FU-A",
            29: "FU-B",
        },
    ),
    "h265_nal_type": ValueTable(
        "h265_nal_type",
        {
            19: "IDR_W_RADL",
            20: "IDR_N_LP",
            32: "VPS",
            33: "SPS",
            34: "PPS",
            35: "AUD",
            39: "PREFIX_SEI",
            40: "SUFFIX_SEI",
            48: "Aggregation packet (AP)",
            49: "Fragmentation unit (FU)",
            50: "PACI",
        },
    ),
    # IEEE 1722-2025 Table 37 (SVF format)
    "svf_format": ValueTable(
        "svf_format",
        {
            0x01: "525i/59.94",
            0x02: "625i/50",
            0x03: "1080i/59.94",
            0x04: "1080i/50",
            0x05: "720p/59.94",
            0x06: "720p/50",
            0x07: "1080p/23.98",
            0x08: "1080p/59.94",
            0x09: "1080p/50",
            0xFF: "SVF_USER",
        },
    ),
    # IEEE 1722-2025 Tables 45-48 (RVF)
    "rvf_pixel_depth": ValueTable(
        "rvf_pixel_depth", {1: "8", 2: "10", 3: "12", 4: "16", 0xF: "User defined"}
    ),
    "rvf_pixel_format": ValueTable(
        "rvf_pixel_format",
        {
            0: "Monochrome",
            1: "4:1:1",
            2: "4:2:0",
            3: "4:2:2",
            4: "4:4:4",
            6: "4:2:2:4",
            7: "4:4:4:4",
            8: "Bayer grbg",
            9: "Bayer rggb",
            0xA: "Bayer bggr",
            0xB: "Bayer gbrg",
            0xF: "User defined",
        },
    ),
    "rvf_frame_rate": ValueTable(
        "rvf_frame_rate",
        {
            0x01: "1",
            0x02: "2",
            0x03: "5",
            0x10: "10",
            0x11: "15",
            0x12: "20",
            0x13: "24",
            0x14: "25",
            0x15: "30",
            0x16: "48",
            0x17: "50",
            0x18: "60",
            0x19: "72",
            0x1A: "85",
            0x30: "100",
            0x31: "120",
            0x32: "150",
            0x33: "200",
            0x34: "240",
            0x35: "300",
            0xFF: "User defined",
        },
    ),
    "rvf_colorspace": ValueTable(
        "rvf_colorspace",
        {
            1: "YCbCr",
            2: "sRGB",
            3: "YCgCo",
            4: "Grayscale",
            5: "XYZ",
            6: "YCM",
            7: "BT Rec.601",
            8: "BT Rec.709",
            9: "ITU BT 2020",
            0xF: "User defined",
        },
    ),
    "am824_fdf": ValueTable(
        "am824_fdf",
        {
            0x00: "32 kHz",
            0x01: "44.1 kHz",
            0x02: "48 kHz",
            0x03: "88.2 kHz",
            0x04: "96 kHz",
            0x05: "176.4 kHz",
            0x06: "192 kHz",
            0xFF: "No data",
        },
    ),
    # IEEE 1722-2025 Table 26 (CRF type), Table 27 (pull)
    "crf_type": ValueTable(
        "crf_type",
        {
            0x00: "User specified",
            0x01: "Audio sample",
            0x02: "Video frame",
            0x03: "Video line",
            0x04: "Machine cycle",
        },
    ),
    "crf_pull": ValueTable(
        "crf_pull",
        {
            0x00: "x 1.0",
            0x01: "x 1/1.001",
            0x02: "x 1.001",
            0x03: "x 24/25",
            0x04: "x 25/24",
            0x05: "x 1/8",
        },
    ),
    # IEEE 1722-2025 Annex B (MAAP)
    "maap_message_type": ValueTable(
        "maap_message_type",
        {0x01: "MAAP_PROBE", 0x02: "MAAP_DEFEND", 0x03: "MAAP_ANNOUNCE"},
    ),
    # IEEE 1722-2025 Clause 13 (AEF), 16 (ESCF), 17 (EECF)
    "aef_enc": ValueTable("aef_enc", {0x00: "AES-SIV", 0x01: "AES-GCM-SIV"}),
    "escf_sig": ValueTable("escf_sig", {0x00: "ECC1"}),
    "eecf_enc": ValueTable("eecf_enc", {0x00: "ECC1"}),
}

HEADER_KIND: dict[int, str] = {code: kind for code, _, _, kind in AVTP_SUBTYPES}


def _common() -> tuple[Field, ...]:
    """Bytes 0-1 and 4-11 that every AVTPDU shares (4.4.3)."""
    return (
        Field(
            "subtype",
            0,
            1,
            "u8",
            "AVTP subtype (Table 6)",
            base="hex",
            values="avtp_subtype",
        ),
        Field("sv", 1, 1, "bool", "stream_id valid", mask=0x80),
        Field("version", 1, 1, "u8", "AVTP version", mask=0x70),
    )


STREAM_ID = Field("stream_id", 4, 8, "u64", "stream_id", base="hex")

# Stream data header, version 0 (4.4.4.1): 16 octets.
AVTP_STREAM_V0 = Layout(
    "stream_v0",
    "avb.avtp",
    "AVTP stream data header, version 0 (IEEE 1722-2025 4.4.4.1)",
    _common()
    + (
        Field("mr", 1, 1, "bool", "media clock restart", mask=0x08),
        Field("r", 1, 1, "u8", "reserved", mask=0x04),
        Field("fsd1", 1, 1, "u8", "format specific data (gv / fs / ...)", mask=0x02),
        Field("tv", 1, 1, "bool", "avtp_timestamp valid", mask=0x01),
        Field("sequence_num", 2, 1, "u8", "sequence_num (8-bit)"),
        Field(
            "fsd2",
            3,
            1,
            "u8",
            "format specific data (reserved / crf_type / ...)",
            mask=0xFE,
        ),
        Field("tu", 3, 1, "bool", "timestamp uncertain", mask=0x01),
        STREAM_ID,
        Field("avtp_timestamp", 12, 4, "u32", "avtp_timestamp (32-bit, ns)"),
    ),
)

# Stream data header, version 1 (4.7.4): 32 octets before the format-specific part.
AVTP_STREAM_V1 = Layout(
    "stream_v1",
    "avb.avtp",
    "AVTP stream data header, version 1 (IEEE 1722-2025 4.7.4)",
    _common()
    + (
        Field("mr", 1, 1, "bool", "media clock restart", mask=0x08),
        Field("r", 1, 1, "u8", "reserved", mask=0x04),
        Field("fsd1", 1, 1, "u8", "format specific data (gv / fs / ...)", mask=0x02),
        Field("tv", 1, 1, "bool", "avtp_timestamp valid", mask=0x01),
        Field(
            "format_specific_data_0", 2, 1, "u8", "format_specific_data_0", base="hex"
        ),
        Field(
            "fsd2",
            3,
            1,
            "u8",
            "format specific data (reserved / crf_type / ...)",
            mask=0xFE,
        ),
        Field("tu", 3, 1, "bool", "timestamp uncertain", mask=0x01),
        STREAM_ID,
        Field("sequence_num32", 12, 4, "u32", "sequence_num (32-bit)"),
        Field("avtp_timestamp64", 16, 8, "u64", "avtp_timestamp (64-bit, ns)"),
        Field(
            "ptp_grandmaster_identity",
            24,
            8,
            "u64",
            "ptp_grandmaster_identity",
            base="hex",
        ),
    ),
)

# Control data header (4.4.5): ADP, AECP, ACMP, MAAP, EF_CONTROL.
AVTP_CONTROL = Layout(
    "control",
    "avb.avtp",
    "AVTP control data header (IEEE 1722-2025 4.4.5)",
    _common()
    + (
        Field("control_data", 1, 1, "u8", "control_data / message_type", mask=0x0F),
        Field("status", 2, 2, "u8", "status (5 bits)", mask=0xF800),
        Field(
            "control_data_length",
            2,
            2,
            "u16",
            "control_data_length (11 bits)",
            mask=0x07FF,
        ),
        STREAM_ID,
    ),
)

# Alternative header (4.4.6): CRF, NTSCF, AEF, ESCF, EECF.
AVTP_ALTERNATIVE = Layout(
    "alternative",
    "avb.avtp",
    "AVTP alternative header (IEEE 1722-2025 4.4.6)",
    _common()
    + (
        Field(
            "subtype_data1",
            1,
            1,
            "u8",
            "subtype_data_1 (4 bits)",
            mask=0x0F,
            base="hex",
        ),
        Field("subtype_data2", 2, 2, "u16", "subtype_data_2 (16 bits)", base="hex"),
        STREAM_ID,
    ),
)

# Common prefix only, for reserved subtypes.
AVTP_COMMON = Layout(
    "common",
    "avb.avtp",
    "AVTP common header prefix (IEEE 1722-2025 4.4.3)",
    _common() + (STREAM_ID,),
)

# Annex J IP encapsulation: the IP AVTPDU header before the AVTPDU.
IP_AVTPDU = Layout(
    "ip_avtpdu",
    "avb.ipavtp",
    "IP AVTPDU header (IEEE 1722-2025 Annex J)",
    (Field("encapsulation_sequence_num", 0, 4, "u32", "encapsulation_sequence_num"),),
)

# Bytes after the decoded header when no subtype dissector exists yet.
PAYLOAD = Field("payload", 0, 0, "bytes", "undissected AVTPDU payload")

AVTP_LAYOUTS: tuple[Layout, ...] = (
    AVTP_COMMON,
    AVTP_STREAM_V0,
    AVTP_STREAM_V1,
    AVTP_CONTROL,
    AVTP_ALTERNATIVE,
)


# ---------------------------------------------------------------------------
# Subtype-specific layouts (wave 2). A SubtypeSpec replaces the generic header
# decode for one (subtype, version): its layouts are applied from offset 0 in
# order, header_length is where the undissected payload (or a post hook's
# data) starts. Offsets are absolute within the AVTPDU.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class SubtypeSpec:
    """How one (subtype, version) is dissected."""

    name: str
    subtype: int
    version: int
    layouts: tuple[Layout, ...]
    header_length: int
    post: str | None = None  # name of a post hook (Lua + decode.py) for the rest


def _shift(fields: tuple[Field, ...], by: int) -> tuple[Field, ...]:
    """The same fields at a different base offset (the v1 headers are 16 longer)."""
    return tuple(
        Field(f.name, f.offset + by, f.length, f.kind, f.doc, f.mask, f.base, f.values)
        for f in fields
    )


# AAF (Clause 7): the common part after the stream header (7.2), then the
# PCM (7.3) or AES3 (7.4) redefinition of the format-specific octets, chosen
# by the format field at dissection time (post hook "aaf_audio").
_AAF_FIELDS = (
    Field("format", 16, 1, "u8", "sample format (Table 11)", values="aaf_format"),
    Field(
        "stream_data_length", 20, 2, "u16", "stream_data_length (octets of audio data)"
    ),
    Field("sp", 22, 1, "bool", "sparse timestamp mode", mask=0x10),
    Field("evt", 22, 1, "u8", "event", mask=0x0F),
)
_AAF_PCM_FIELDS = (
    Field(
        "nsr",
        17,
        1,
        "u8",
        "nominal sample rate (Table 12)",
        mask=0xF0,
        values="aaf_nsr",
    ),
    Field(
        "channels_per_frame", 17, 2, "u16", "channels per frame (10 bits)", mask=0x03FF
    ),
    Field("bit_depth", 19, 1, "u8", "bit depth (valid bits, MSB-aligned)"),
)
_AAF_AES3_FIELDS = (
    Field(
        "nfr",
        17,
        1,
        "u8",
        "nominal AES3 frame rate (Table 16)",
        mask=0xF0,
        values="aaf_nsr",
    ),
    Field(
        "streams_per_frame",
        17,
        2,
        "u16",
        "AES3 streams per frame (10 bits; two subframes each)",
        mask=0x03FF,
    ),
    Field("data_type_h", 19, 1, "u8", "aes3_data_type, high octet", base="hex"),
    Field(
        "dt_ref",
        22,
        1,
        "u8",
        "aes3_data_type reference (Table 17)",
        mask=0xE0,
        values="aaf_aes3_dt_ref",
    ),
    Field("data_type_l", 23, 1, "u8", "aes3_data_type, low octet", base="hex"),
)
AAF_V0 = Layout(
    "aaf_v0",
    "avb.avtp.aaf",
    "AAF common header (IEEE 1722-2025 7.2)",
    _AAF_FIELDS,
)
AAF_V1 = Layout(
    "aaf_v1",
    "avb.avtp.aaf",
    "AAF common header, version 1 header",
    _shift(_AAF_FIELDS, 16),
)
AAF_PCM_V0 = Layout(
    "aaf_pcm_v0", "avb.avtp.aaf", "AAF PCM fields (IEEE 1722-2025 7.3)", _AAF_PCM_FIELDS
)
AAF_PCM_V1 = Layout(
    "aaf_pcm_v1",
    "avb.avtp.aaf",
    "AAF PCM fields, version 1 header",
    _shift(_AAF_PCM_FIELDS, 16),
)
AAF_AES3_V0 = Layout(
    "aaf_aes3_v0",
    "avb.avtp.aaf.aes3",
    "AAF AES3 fields (IEEE 1722-2025 7.4)",
    _AAF_AES3_FIELDS,
)
AAF_AES3_V1 = Layout(
    "aaf_aes3_v1",
    "avb.avtp.aaf.aes3",
    "AAF AES3 fields, version 1 header",
    _shift(_AAF_AES3_FIELDS, 16),
)

# The audio payload items the "aaf_audio" post hook adds: one per sample in
# the format's own width (7.3.5), or per AES3 subframe (7.4.6.2: bits 4-7 are
# B C U V, bits 8-31 the audio sample word). Offsets are relative to the
# sample; "frames" is a generated count.
AAF_AUDIO = Layout(
    "aaf_audio",
    "avb.avtp.aaf",
    "AAF audio samples",
    (
        Field("frames", 0, 0, "u16", "audio sample frames in this AVTPDU"),
        Field("pcm_data_payload", 0, 0, "bytes", "user-specified PCM data"),
        Field("sample_int16", 0, 2, "i16", "16-bit integer sample"),
        Field("sample_int24", 0, 3, "i24", "24-bit integer sample"),
        Field("sample_int32", 0, 4, "i32", "32-bit integer sample"),
        Field("sample_float32", 0, 4, "f32", "32-bit float sample"),
        Field("aes3.subframe", 0, 4, "u32", "AAF subframe", base="hex"),
        Field("aes3.b", 0, 4, "bool", "B (block start)", mask=0x08000000),
        Field("aes3.c", 0, 4, "bool", "C (channel status)", mask=0x04000000),
        Field("aes3.u", 0, 4, "bool", "U (user data)", mask=0x02000000),
        Field("aes3.v", 0, 4, "bool", "V (validity: 1 = not PCM)", mask=0x01000000),
        Field("aes3.audio_sample_word", 1, 3, "i24", "24-bit audio sample word"),
    ),
)

# IEC 61883/IIDC (Clause 5): the common header (Figure 15); the post hook
# "iec61883" adds the CIP header when tag == 1 and dissects by FMT/SPH.
_IEC61883_FIELDS = (
    Field("gateway_info", 16, 4, "u32", "gateway_info", base="hex"),
    Field("stream_data_length", 20, 2, "u16", "stream_data_length (CIP header + data)"),
    Field("tag", 22, 1, "u8", "tag (1 = CIP header present, 0 = IIDC)", mask=0xC0),
    Field("channel", 22, 1, "u8", "channel", mask=0x3F),
    Field("tcode", 23, 1, "u8", "tcode", mask=0xF0, base="hex"),
    Field("sy", 23, 1, "u8", "sy", mask=0x0F),
)
IEC61883_V0 = Layout(
    "iec61883_v0",
    "avb.avtp.iec61883",
    "IEC 61883/IIDC common header (IEEE 1722-2025 5.2)",
    _IEC61883_FIELDS,
)
IEC61883_V1 = Layout(
    "iec61883_v1",
    "avb.avtp.iec61883",
    "IEC 61883/IIDC common header, version 1 header",
    _shift(_IEC61883_FIELDS, 16),
)
_CIP_FIELDS = (
    Field("qi_1", 24, 1, "u8", "CIP quadlet indicator 1", mask=0xC0),
    Field("sid", 24, 1, "u8", "CIP source id", mask=0x3F),
    Field("dbs", 25, 1, "u8", "CIP data block size (quadlets)"),
    Field("fn", 26, 1, "u8", "CIP fraction number", mask=0xC0),
    Field("qpc", 26, 1, "u8", "CIP quadlet padding count", mask=0x38),
    Field("sph", 26, 1, "bool", "CIP source packet header", mask=0x04),
    Field("dbc", 27, 1, "u8", "CIP data block count"),
    Field("qi_2", 28, 1, "u8", "CIP quadlet indicator 2", mask=0xC0),
    Field(
        "fmt",
        28,
        1,
        "u8",
        "CIP format (IEC 61883-1 FMT)",
        mask=0x3F,
        base="hex",
        values="iec61883_fmt",
    ),
    Field(
        "fdf",
        29,
        1,
        "u8",
        "CIP format dependent field (AM824 sample frequency code)",
        base="hex",
        values="am824_fdf",
    ),
    Field("syt", 30, 2, "u16", "CIP synchronization timestamp", base="hex"),
)
CIP_V0 = Layout("cip_v0", "avb.avtp.cip", "IEC 61883-6 CIP header", _CIP_FIELDS)

# IEC 61883-4 (FMT 0x20, SPH = 1): source packets of dbs*4*2^fn octets, a
# 32-bit source packet header timestamp then a 188-octet MPEG2 transport
# packet. Offsets relative to the source packet; "source_packets" is a count.
MPEGTS = Layout(
    "mpegts",
    "avb.avtp.mpegts",
    "IEC 61883-4 source packet (MPEG2-TS)",
    (
        Field("source_packets", 0, 0, "u16", "source packets in this AVTPDU"),
        Field("source_packet_timestamp", 0, 4, "u32", "source packet header timestamp"),
        Field("sync_byte", 4, 1, "u8", "TS sync byte (0x47)", base="hex"),
        Field("tei", 5, 2, "bool", "transport error indicator", mask=0x8000),
        Field("pusi", 5, 2, "bool", "payload unit start indicator", mask=0x4000),
        Field("transport_priority", 5, 2, "bool", "transport priority", mask=0x2000),
        Field("pid", 5, 2, "u16", "PID", mask=0x1FFF, base="hex"),
        Field("tsc", 7, 1, "u8", "transport scrambling control", mask=0xC0),
        Field("afc", 7, 1, "u8", "adaptation field control", mask=0x30),
        Field("cc", 7, 1, "u8", "continuity counter", mask=0x0F),
    ),
)

# Compressed Video Format (Clause 8): the common header (Figure 39), then the
# RFC payload-type specific header the "cvf" post hook adds by format_subtype
# (MJPEG Figure 40, H.264 Figure 41 with its NAL unit header per RFC 6184,
# JPEG 2000 Figure 48, H.265 Figure 51 with its NAL unit header per RFC 7798).
_CVF_FIELDS = (
    Field("format", 16, 1, "u8", "format (Table 20)", values="cvf_format"),
    Field(
        "format_subtype",
        17,
        1,
        "u8",
        "format_subtype (Table 21)",
        values="cvf_format_subtype",
    ),
    Field("stream_data_length", 20, 2, "u16", "stream_data_length (octets)"),
    Field("ptv", 22, 1, "bool", "payload timestamp valid (H.264/H.265)", mask=0x20),
    Field(
        "m",
        22,
        1,
        "bool",
        "M (marker: last AVTPDU of the frame / access unit)",
        mask=0x10,
    ),
    Field("evt", 22, 1, "u8", "event", mask=0x0F),
)
CVF_V0 = Layout(
    "cvf_v0", "avb.avtp.cvf", "CVF header (IEEE 1722-2025 8.3)", _CVF_FIELDS
)
CVF_V1 = Layout(
    "cvf_v1", "avb.avtp.cvf", "CVF header, version 1 header", _shift(_CVF_FIELDS, 16)
)
CVF_MJPEG = Layout(
    "cvf_mjpeg",
    "avb.avtp.cvf.mjpeg",
    "MJPEG header (IEEE 1722-2025 8.4, RFC 2435)",
    (
        Field("type_specific", 0, 1, "u8", "type-specific"),
        Field("fragment_offset", 1, 3, "u24", "fragment offset"),
        Field("type", 4, 1, "u8", "type"),
        Field("q", 5, 1, "u8", "Q"),
        Field("width", 6, 1, "u8", "width (in 8-pixel blocks)"),
        Field("height", 7, 1, "u8", "height (in 8-pixel blocks)"),
    ),
)
CVF_H264 = Layout(
    "cvf_h264",
    "avb.avtp.cvf.h264",
    "H.264 header (IEEE 1722-2025 8.5)",
    (Field("timestamp", 0, 4, "u32", "h264_timestamp"),),
)
CVF_H264_NAL = Layout(
    "cvf_h264_nal",
    "avb.avtp.cvf.h264.nal",
    "H.264 NAL unit header (RFC 6184)",
    (
        Field("f", 0, 1, "bool", "forbidden_zero_bit", mask=0x80),
        Field("nri", 0, 1, "u8", "nal_ref_idc", mask=0x60),
        Field("type", 0, 1, "u8", "nal_unit_type", mask=0x1F, values="h264_nal_type"),
    ),
)
CVF_H264_FU = Layout(
    "cvf_h264_fu",
    "avb.avtp.cvf.h264.fu",
    "H.264 FU header (RFC 6184 5.8)",
    (
        Field("s", 0, 1, "bool", "start of fragmented NAL unit", mask=0x80),
        Field("e", 0, 1, "bool", "end of fragmented NAL unit", mask=0x40),
        Field("r", 0, 1, "u8", "reserved", mask=0x20),
        Field(
            "type",
            0,
            1,
            "u8",
            "nal_unit_type of the fragmented unit",
            mask=0x1F,
            values="h264_nal_type",
        ),
    ),
)
CVF_H265 = Layout(
    "cvf_h265",
    "avb.avtp.cvf.h265",
    "H.265 header (IEEE 1722-2025 8.7)",
    (Field("timestamp", 0, 4, "u32", "h265_timestamp"),),
)
CVF_H265_NAL = Layout(
    "cvf_h265_nal",
    "avb.avtp.cvf.h265.nal",
    "H.265 NAL unit header (RFC 7798)",
    (
        Field("f", 0, 2, "bool", "forbidden_zero_bit", mask=0x8000),
        Field(
            "type", 0, 2, "u16", "nal_unit_type", mask=0x7E00, values="h265_nal_type"
        ),
        Field("layer_id", 0, 2, "u16", "nuh_layer_id", mask=0x01F8),
        Field("tid", 0, 2, "u16", "nuh_temporal_id_plus1", mask=0x0007),
    ),
)
CVF_H265_FU = Layout(
    "cvf_h265_fu",
    "avb.avtp.cvf.h265.fu",
    "H.265 FU header (RFC 7798 4.4.3)",
    (
        Field("s", 0, 1, "bool", "start of fragmented NAL unit", mask=0x80),
        Field("e", 0, 1, "bool", "end of fragmented NAL unit", mask=0x40),
        Field(
            "type",
            0,
            1,
            "u8",
            "nal_unit_type of the fragmented unit",
            mask=0x3F,
            values="h265_nal_type",
        ),
    ),
)
CVF_JPEG2000 = Layout(
    "cvf_jpeg2000",
    "avb.avtp.cvf.jpeg2000",
    "JPEG 2000 payload header (IEEE 1722-2025 8.6, RFC 5371)",
    (
        Field("tp", 0, 1, "u8", "type (progressive/interlaced)", mask=0xC0),
        Field("mhf", 0, 1, "u8", "main header flag", mask=0x30),
        Field("mh_id", 0, 1, "u8", "main header identification", mask=0x0E),
        Field("t", 0, 1, "bool", "tile field", mask=0x01),
        Field("priority", 1, 1, "u8", "priority"),
        Field("tile_number", 2, 2, "u16", "tile number"),
        Field("fragment_offset", 5, 3, "u24", "fragment offset"),
    ),
)

# SDI Video Format (Clause 11, Figure 102)
_SVF_FIELDS = (
    Field("format", 16, 1, "u8", "format (Table 37)", values="svf_format"),
    Field("i_seq_num", 17, 1, "u8", "i_seq_num"),
    Field("line_number", 18, 2, "u16", "line number"),
    Field("stream_data_length", 20, 2, "u16", "stream_data_length (octets)"),
    Field("gb", 22, 1, "bool", "guard band", mask=0x40),
    Field("sp", 22, 1, "bool", "RP168 switch point", mask=0x20),
    Field("ef", 22, 1, "bool", "end of frame", mask=0x10),
    Field("evt", 22, 1, "u8", "event", mask=0x0F),
    Field("map", 24, 1, "u8", "map", mask=0xF0),
    Field("sample", 24, 1, "u8", "sample", mask=0x0F),
    Field("frame", 25, 1, "u8", "frame"),
    Field("frate", 26, 1, "u8", "frame rate"),
    Field("frcount", 27, 1, "u8", "frame count"),
)
SVF_V0 = Layout(
    "svf_v0", "avb.avtp.svf", "SVF header (IEEE 1722-2025 11.2)", _SVF_FIELDS
)
SVF_V1 = Layout(
    "svf_v1", "avb.avtp.svf", "SVF header, version 1 header", _shift(_SVF_FIELDS, 16)
)

# Raw Video Format (Clause 12, Figure 104)
_RVF_FIELDS = (
    Field("active_pixels", 16, 2, "u16", "active pixels per line"),
    Field("total_lines", 18, 2, "u16", "total lines per frame"),
    Field("stream_data_length", 20, 2, "u16", "stream_data_length (octets)"),
    Field("ap", 22, 1, "bool", "active pixels only", mask=0x80),
    Field("f", 22, 1, "bool", "field (interlaced: 0 = first, 1 = second)", mask=0x20),
    Field("ef", 22, 1, "bool", "end of frame", mask=0x10),
    Field("evt", 22, 1, "u8", "event", mask=0x0F),
    Field("pd", 23, 1, "bool", "pull-down", mask=0x80),
    Field("i", 23, 1, "bool", "interlaced", mask=0x40),
    Field(
        "pixel_depth",
        25,
        1,
        "u8",
        "pixel depth (Table 45)",
        mask=0xF0,
        values="rvf_pixel_depth",
    ),
    Field(
        "pixel_format",
        25,
        1,
        "u8",
        "pixel format (Table 46)",
        mask=0x0F,
        values="rvf_pixel_format",
    ),
    Field("frame_rate", 26, 1, "u8", "frame rate (Table 47)", values="rvf_frame_rate"),
    Field(
        "colorspace",
        27,
        1,
        "u8",
        "colorspace (Table 48)",
        mask=0xF0,
        values="rvf_colorspace",
    ),
    Field("num_lines", 27, 1, "u8", "lines in this AVTPDU", mask=0x0F),
    Field("i_seq_num", 29, 1, "u8", "i_seq_num"),
    Field("line_number", 30, 2, "u16", "line number"),
)
RVF_V0 = Layout(
    "rvf_v0", "avb.avtp.rvf", "RVF header (IEEE 1722-2025 12.2)", _RVF_FIELDS
)
RVF_V1 = Layout(
    "rvf_v1", "avb.avtp.rvf", "RVF header, version 1 header", _shift(_RVF_FIELDS, 16)
)

# Vendor Specific Format (Clause 14, Figure 108), MMA (Clause 6) and the
# Experimental stream format (Clause 15): common stream header + opaque payload
_VSF_FIELDS = (
    Field(
        "vendor_id_1",
        16,
        4,
        "u32",
        "vendor_id (first 4 octets of the OUI-based id)",
        base="hex",
    ),
    Field("stream_data_length", 20, 2, "u16", "stream_data_length (octets)"),
    Field("vendor_id_2", 22, 2, "u16", "vendor_id (last 2 octets)", base="hex"),
)
VSF_V0 = Layout(
    "vsf_v0", "avb.avtp.vsf", "VSF header (IEEE 1722-2025 14.1)", _VSF_FIELDS
)
# The vendor id is one OUI-based EUI-48 split around stream_data_length
# (14.1.1.2: vendor_id_1 holds bits 0-31, vendor_id_2 bits 32-47); the "vsf"
# post hook adds it reassembled as a generated item.
VSF_EXTRA = Layout(
    "vsf_extra",
    "avb.avtp.vsf",
    "VSF reassembled vendor id",
    (
        Field(
            "vendor_id",
            0,
            6,
            "eui48",
            "vendor id (vendor_id_1 || vendor_id_2, an OUI-based EUI-48)",
        ),
    ),
)
VSF_V1 = Layout(
    "vsf_v1", "avb.avtp.vsf", "VSF header, version 1 header", _shift(_VSF_FIELDS, 16)
)
_SDL_ONLY = (Field("stream_data_length", 20, 2, "u16", "stream_data_length (octets)"),)
MMA_V0 = Layout(
    "mma_v0", "avb.avtp.mma", "MMA stream (IEEE 1722-2025 Clause 6)", _SDL_ONLY
)
MMA_V1 = Layout(
    "mma_v1", "avb.avtp.mma", "MMA stream, version 1 header", _shift(_SDL_ONLY, 16)
)
EF_STREAM_V0 = Layout(
    "ef_stream_v0", "avb.avtp.ef", "Experimental stream (Clause 15)", _SDL_ONLY
)
EF_STREAM_V1 = Layout(
    "ef_stream_v1",
    "avb.avtp.ef",
    "Experimental stream, version 1 header",
    _shift(_SDL_ONLY, 16),
)

# The AM824 data the "am824_audio" post hook adds: dbs quadlets per data
# block, each a label octet and 24 bits of data (IEC 61883-6); MBLA and IEC
# 60958 labels carry a 24-bit audio sample. Offsets relative to the quadlet.
AM824_AUDIO = Layout(
    "am824_audio",
    "avb.avtp.am824",
    "AM824 data blocks",
    (
        Field("data_blocks", 0, 0, "u16", "data blocks (sample frames) in this AVTPDU"),
        Field("quadlet", 0, 4, "u32", "AM824 quadlet: label + 24 bits", base="hex"),
        Field("label", 0, 1, "u8", "AM824 label", base="hex", values="am824_label"),
        Field("sample", 1, 3, "i24", "24-bit audio sample (MBLA or IEC 60958)"),
        Field(
            "data", 0, 4, "u32", "24-bit non-audio data", mask=0x00FFFFFF, base="hex"
        ),
    ),
)
CIP_V1 = Layout(
    "cip_v1",
    "avb.avtp.cip",
    "IEC 61883-6 CIP header, version 1 header",
    _shift(_CIP_FIELDS, 16),
)

# TSCF (9.3): bytes beyond the stream header.
TSCF_V0 = Layout(
    "tscf_v0",
    "avb.avtp.tscf",
    "TSCF header, version 0 (IEEE 1722-2025 9.3, Figure 60)",
    (
        Field("sequence_num_lsb", 17, 1, "u8", "sequence_num_lsb"),
        Field(
            "stream_data_length",
            20,
            2,
            "u16",
            "stream_data_length (acf_payload_data octets)",
        ),
    ),
)
TSCF_V1 = Layout(
    "tscf_v1",
    "avb.avtp.tscf",
    "TSCF header, version 1 (IEEE 1722-2025 9.3, Figure 61)",
    (
        Field(
            "stream_data_length",
            36,
            2,
            "u16",
            "stream_data_length (acf_payload_data octets)",
        ),
    ),
)

# NTSCF (9.2): its own alternative-header packing.
NTSCF_V0 = Layout(
    "ntscf_v0",
    "avb.avtp",
    "NTSCF header, version 0 (IEEE 1722-2025 9.2, Figure 58)",
    _common()
    + (
        Field("ntscf.r", 1, 1, "u8", "reserved", mask=0x08),
        Field(
            "ntscf.ntscf_data_length",
            1,
            2,
            "u16",
            "ntscf_data_length (11 bits)",
            mask=0x07FF,
        ),
        Field("ntscf.sequence_num_lsb", 3, 1, "u8", "sequence_num_lsb"),
        STREAM_ID,
    ),
)
_ALT_V1 = (
    Field("sequence_num32", 4, 4, "u32", "sequence_num (32-bit)"),
    Field(
        "ptp_grandmaster_identity", 8, 8, "u64", "ptp_grandmaster_identity", base="hex"
    ),
)
_STREAM_ID_20 = Field("stream_id", 20, 8, "u64", "stream_id", base="hex")
NTSCF_V1 = Layout(
    "ntscf_v1",
    "avb.avtp",
    "NTSCF header, version 1 (IEEE 1722-2025 9.2, Figure 59)",
    _common()
    + _ALT_V1
    + (
        Field("ntscf.r", 16, 1, "u8", "reserved", mask=0x08),
        Field(
            "ntscf.ntscf_data_length",
            16,
            2,
            "u16",
            "ntscf_data_length (11 bits)",
            mask=0x07FF,
        ),
        Field("ntscf.sequence_num_lsb", 18, 1, "u8", "sequence_num_lsb"),
        _STREAM_ID_20,
    ),
)

# CRF (Clause 10): alternative header with its own byte 1 and byte 3.
_CRF_TAIL = (
    Field(
        "crf.pull", 12, 4, "u8", "pull (Table 27)", mask=0xE0000000, values="crf_pull"
    ),
    Field(
        "crf.base_frequency",
        12,
        4,
        "u32",
        "base_frequency (Hz, 29 bits)",
        mask=0x1FFFFFFF,
    ),
    Field(
        "crf.crf_data_length", 16, 2, "u16", "crf_data_length (octets of timestamps)"
    ),
    Field("crf.timestamp_interval", 18, 2, "u16", "timestamp_interval"),
)
CRF_V0 = Layout(
    "crf_v0",
    "avb.avtp",
    "CRF header, version 0 (IEEE 1722-2025 10.4, Figure 98)",
    _common()
    + (
        Field("mr", 1, 1, "bool", "media clock restart", mask=0x08),
        Field("r", 1, 1, "u8", "reserved", mask=0x04),
        Field("crf.fs", 1, 1, "bool", "frame sync", mask=0x02),
        Field("tu", 1, 1, "bool", "timestamp uncertain", mask=0x01),
        Field("sequence_num", 2, 1, "u8", "sequence_num (8-bit)"),
        Field("crf.type", 3, 1, "u8", "CRF type (Table 26)", values="crf_type"),
        STREAM_ID,
    )
    + _CRF_TAIL,
)
CRF_V1 = Layout(
    "crf_v1",
    "avb.avtp",
    "CRF header, version 1 (IEEE 1722-2025 10.4, Figure 99)",
    _common()
    + _ALT_V1
    + (
        Field("mr", 16, 1, "bool", "media clock restart", mask=0x08),
        Field("r", 16, 1, "u8", "reserved", mask=0x04),
        Field("crf.fs", 16, 1, "bool", "frame sync", mask=0x02),
        Field("tu", 16, 1, "bool", "timestamp uncertain", mask=0x01),
        Field("crf.sequence_num_lsb", 17, 1, "u8", "sequence_num_lsb"),
        Field("crf.type", 18, 1, "u8", "CRF type (Table 26)", values="crf_type"),
        _STREAM_ID_20,
    )
    + _shift(_CRF_TAIL, 16),
)
CRF_TIMESTAMP = Field("crf.timestamp", 0, 8, "u64", "CRF timestamp (ns)")
CRF_EXTRA = Layout("crf_extra", "avb.avtp", "CRF timestamps", (CRF_TIMESTAMP,))

# MAAP (Annex B): control header with its own names.
MAAP = Layout(
    "maap",
    "avb.avtp",
    "MAAP PDU (IEEE 1722-2025 Annex B, Figure B.1)",
    _common()
    + (
        Field(
            "maap.message_type",
            1,
            1,
            "u8",
            "message_type",
            mask=0x0F,
            values="maap_message_type",
        ),
        Field("maap.maap_version", 2, 2, "u8", "maap_version", mask=0xF800),
        Field(
            "maap.maap_data_length",
            2,
            2,
            "u16",
            "maap_data_length (11 bits)",
            mask=0x07FF,
        ),
        STREAM_ID,
        Field(
            "maap.requested_start_address", 12, 6, "eui48", "requested_start_address"
        ),
        Field("maap.requested_count", 18, 2, "u16", "requested_count"),
        Field("maap.conflict_start_address", 20, 6, "eui48", "conflict_start_address"),
        Field("maap.conflict_count", 26, 2, "u16", "conflict_count"),
    ),
)

# AEF (Clause 13), ESCF (16), EECF (17): r | version | mode, a length, a key id.
_VERSION_ONLY = (
    Field(
        "subtype",
        0,
        1,
        "u8",
        "AVTP subtype (Table 6)",
        base="hex",
        values="avtp_subtype",
    ),
    Field("version", 1, 1, "u8", "AVTP version", mask=0x70),
)
AEF_CONTINUOUS = Layout(
    "aef_continuous",
    "avb.avtp",
    "AEF continuous header (IEEE 1722-2025 13.3)",
    _VERSION_ONLY
    + (
        Field("aef.enc", 1, 1, "u8", "encryption mode", mask=0x0F, values="aef_enc"),
        Field("aef.stream_data_length", 2, 2, "u16", "stream_data_length"),
        Field("aef.key_id", 4, 8, "u64", "key_id (EUI-64)", base="hex"),
    ),
)
AEF_DISCRETE = Layout(
    "aef_discrete",
    "avb.avtp",
    "AEF discrete header (IEEE 1722-2025 13.4)",
    _VERSION_ONLY
    + (
        Field("aef.enc", 1, 1, "u8", "encryption mode", mask=0x0F, values="aef_enc"),
        Field(
            "aef.control_data_length",
            2,
            2,
            "u16",
            "control_data_length (11 bits)",
            mask=0x07FF,
        ),
        Field("aef.key_id", 4, 8, "u64", "key_id (EUI-64)", base="hex"),
    ),
)
ESCF = Layout(
    "escf",
    "avb.avtp",
    "ESCF header (IEEE 1722-2025 16.3)",
    _VERSION_ONLY
    + (
        Field(
            "escf.sig", 1, 1, "u8", "signature algorithm", mask=0x0F, values="escf_sig"
        ),
        Field(
            "escf.control_data_length",
            2,
            2,
            "u16",
            "control_data_length (11 bits)",
            mask=0x07FF,
        ),
        Field("escf.key_id", 4, 8, "u64", "key_id (EUI-64)", base="hex"),
    ),
)
EECF = Layout(
    "eecf",
    "avb.avtp",
    "EECF header (IEEE 1722-2025 17.3)",
    _VERSION_ONLY
    + (
        Field(
            "eecf.enc", 1, 1, "u8", "encryption algorithm", mask=0x0F, values="eecf_enc"
        ),
        Field(
            "eecf.encrypted_payload_length",
            2,
            2,
            "u16",
            "encrypted_payload_length (11 bits)",
            mask=0x07FF,
        ),
        Field("eecf.key_id", 4, 8, "u64", "key_id (EUI-64)", base="hex"),
    ),
)

SUBTYPE_SPECS: tuple[SubtypeSpec, ...] = (
    SubtypeSpec("aaf_v0", 0x02, 0, (AVTP_STREAM_V0, AAF_V0), 24, post="aaf_audio"),
    SubtypeSpec("aaf_v1", 0x02, 1, (AVTP_STREAM_V1, AAF_V1), 40, post="aaf_audio"),
    SubtypeSpec(
        "iec61883_v0", 0x00, 0, (AVTP_STREAM_V0, IEC61883_V0), 24, post="iec61883"
    ),
    SubtypeSpec(
        "iec61883_v1", 0x00, 1, (AVTP_STREAM_V1, IEC61883_V1), 40, post="iec61883"
    ),
    SubtypeSpec("cvf_v0", 0x03, 0, (AVTP_STREAM_V0, CVF_V0), 24, post="cvf"),
    SubtypeSpec("cvf_v1", 0x03, 1, (AVTP_STREAM_V1, CVF_V1), 40, post="cvf"),
    SubtypeSpec("svf_v0", 0x06, 0, (AVTP_STREAM_V0, SVF_V0), 28),
    SubtypeSpec("svf_v1", 0x06, 1, (AVTP_STREAM_V1, SVF_V1), 44),
    SubtypeSpec("rvf_v0", 0x07, 0, (AVTP_STREAM_V0, RVF_V0), 32),
    SubtypeSpec("rvf_v1", 0x07, 1, (AVTP_STREAM_V1, RVF_V1), 48),
    SubtypeSpec("vsf_v0", 0x6F, 0, (AVTP_STREAM_V0, VSF_V0), 24, post="vsf"),
    SubtypeSpec("vsf_v1", 0x6F, 1, (AVTP_STREAM_V1, VSF_V1), 40, post="vsf"),
    SubtypeSpec("mma_v0", 0x01, 0, (AVTP_STREAM_V0, MMA_V0), 24),
    SubtypeSpec("mma_v1", 0x01, 1, (AVTP_STREAM_V1, MMA_V1), 40),
    SubtypeSpec("ef_stream_v0", 0x7F, 0, (AVTP_STREAM_V0, EF_STREAM_V0), 24),
    SubtypeSpec("ef_stream_v1", 0x7F, 1, (AVTP_STREAM_V1, EF_STREAM_V1), 40),
    SubtypeSpec("tscf_v0", 0x05, 0, (AVTP_STREAM_V0, TSCF_V0), 24, post="acf"),
    SubtypeSpec("tscf_v1", 0x05, 1, (AVTP_STREAM_V1, TSCF_V1), 40, post="acf"),
    SubtypeSpec("ntscf_v0", 0x82, 0, (NTSCF_V0,), 12, post="acf"),
    SubtypeSpec("ntscf_v1", 0x82, 1, (NTSCF_V1,), 28, post="acf"),
    SubtypeSpec("crf_v0", 0x04, 0, (CRF_V0,), 20, post="crf_timestamps"),
    SubtypeSpec("crf_v1", 0x04, 1, (CRF_V1,), 36, post="crf_timestamps"),
    SubtypeSpec("maap", 0xFE, 0, (MAAP,), 28),
    SubtypeSpec("aef_continuous", 0x6E, 0, (AEF_CONTINUOUS,), 12),
    SubtypeSpec("aef_discrete", 0xEE, 0, (AEF_DISCRETE,), 12),
    SubtypeSpec("escf", 0xEC, 0, (ESCF,), 12),
    SubtypeSpec("eecf", 0xED, 0, (EECF,), 12),
)

SUBTYPE_LAYOUTS: tuple[Layout, ...] = (
    AAF_V0,
    AAF_V1,
    AAF_PCM_V0,
    AAF_PCM_V1,
    AAF_AES3_V0,
    AAF_AES3_V1,
    IEC61883_V0,
    IEC61883_V1,
    CIP_V0,
    CIP_V1,
    CVF_V0,
    CVF_V1,
    CVF_MJPEG,
    CVF_H264,
    CVF_H264_NAL,
    CVF_H264_FU,
    CVF_H265,
    CVF_H265_NAL,
    CVF_H265_FU,
    CVF_JPEG2000,
    SVF_V0,
    SVF_V1,
    RVF_V0,
    RVF_V1,
    VSF_V0,
    VSF_V1,
    MMA_V0,
    MMA_V1,
    EF_STREAM_V0,
    EF_STREAM_V1,
    TSCF_V0,
    TSCF_V1,
    NTSCF_V0,
    NTSCF_V1,
    CRF_V0,
    CRF_V1,
    MAAP,
    AEF_CONTINUOUS,
    AEF_DISCRETE,
    ESCF,
    EECF,
)


# ---------------------------------------------------------------------------
# ACF messages inside TSCF/NTSCF (wave 3): the common header, one layout per
# clause 9.4 type from acf_table.py, and the Checksum/CRC trailers. Offsets are
# from the start of the message (its two-octet header included).
# ---------------------------------------------------------------------------

VALUE_TABLES["acf_msg_type"] = ValueTable("acf_msg_type", dict(T.ACF_MSG_TYPE_NAMES))
VALUE_TABLES["acf_crc_type"] = ValueTable("acf_crc_type", dict(T.ACF_CRC_TYPE_NAMES))

ACF_HEADER = Layout(
    "acf_header",
    "avb.acf",
    "ACF common message header (IEEE 1722-2025 9.4.1, Figure 62)",
    (
        Field(
            "msg_type",
            0,
            2,
            "u8",
            "acf_msg_type (Table 23)",
            mask=0xFE00,
            base="hex",
            values="acf_msg_type",
        ),
        Field(
            "msg_length",
            0,
            2,
            "u16",
            "acf_msg_length (quadlets, header included)",
            mask=0x01FF,
        ),
    ),
)


def _acf_field(f: dict) -> Field:
    kind = "bool" if f["kind"] == "bool" else (f["ctype"] or "u8")
    mask = None if f["kind"] == "whole" else f["mask"]
    return Field(
        f["name"],
        f["offset"],
        f["length"],
        kind,
        f["doc"],
        mask=mask,
        base="hex" if f["hex"] else "dec",
    )


@dataclass(frozen=True)
class AcfSpec:
    """How one ACF message type is dissected."""

    name: str
    msg_type: int
    layout: Layout
    length: int  # octets of the fixed part (header included)
    pad_mode: str  # field / custom / none
    fixed_only: bool
    brief: bool
    min_q: int
    max_q: int | None


# A zero-length placeholder: the payload with its pad octets removed; added by
# hand, never by add_<layout>().
def _payload_field() -> Field:
    return Field("payload", 0, 0, "bytes", "payload (pad octets removed)")


ACF_SPECS: dict[int, AcfSpec] = {}
for _t in T.ACF_TYPES:
    _layout = Layout(
        f"acf_{_t['name']}",
        f"avb.acf.{_t['name']}",
        f"{_t['title']} (IEEE 1722-2025 {_t['clause']}, {_t['figure']})",
        tuple(_acf_field(f) for f in _t["fields"])
        + ((_payload_field(),) if not _t["fixed_only"] else ()),
    )
    ACF_SPECS[_t["msg_type"]] = AcfSpec(
        _t["name"],
        _t["msg_type"],
        _layout,
        _t["length"],
        _t["pad_mode"],
        _t["fixed_only"],
        _t["brief"],
        _t["min_q"],
        _t["max_q"],
    )

ACF_CHECKSUM = Layout(
    "acf_checksum",
    "avb.acf.checksum",
    "ACF Checksum message (IEEE 1722-2025 9.4.20)",
    (
        Field(
            "checksum",
            2,
            2,
            "u16",
            "ones-complement checksum of the preceding message",
            base="hex",
        ),
        Field(
            "valid",
            0,
            0,
            "bool",
            "the preceding message verifies against this checksum",
        ),
    ),
)
ACF_CRC = Layout(
    "acf_crc",
    "avb.acf.crc",
    "ACF CRC message (IEEE 1722-2025 9.4.21)",
    (
        Field(
            "crc_type",
            2,
            2,
            "u8",
            "crc_type (Table 30)",
            mask=0x000F,
            values="acf_crc_type",
        ),
        Field("crc_data", 0, 0, "bytes", "crc_data quadlets"),
        Field("valid", 0, 0, "bool", "the preceding message verifies against this CRC"),
    ),
)
ACF_MSG_TYPE_CHECKSUM = 0x76
ACF_MSG_TYPE_CRC = 0x77

ACF_LAYOUTS: tuple[Layout, ...] = (
    (ACF_HEADER,)
    + tuple(spec.layout for spec in ACF_SPECS.values())
    + (ACF_CHECKSUM, ACF_CRC)
)

#: Fields the post hooks add item by item (no generated add_ function); the
#: CRF timestamps, the AAF samples/subframes and the AM824 quadlets.
EXTRA_FIELD_LAYOUTS: tuple[Layout, ...] = (
    CRF_EXTRA,
    AAF_AUDIO,
    AM824_AUDIO,
    MPEGTS,
    VSF_EXTRA,
)
