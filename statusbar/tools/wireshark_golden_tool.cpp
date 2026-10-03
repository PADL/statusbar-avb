// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Writes the golden capture the Wireshark dissector test runs tshark over:
/// AVTPDUs produced by this repository's own builders, over Ethernet and over
/// the Annex J UDP encapsulation. The reference decoder in
/// python/wireshark_schema reads the same frames back, so the Lua, the schema
/// and the C++ wire structs are checked against each other.
///
/// Usage: statusbar_avb_wireshark_golden_tool <output.pcap>

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_v1.hpp"
#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/avtp/avtp_acf_can.hpp"
#include "statusbar/avtp/avtp_acf_checksum.hpp"
#include "statusbar/avtp/avtp_acf_gpc.hpp"
#include "statusbar/avtp/avtp_aef.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_am824_v1.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_crf_v1.hpp"
#include "statusbar/avtp/avtp_eecf.hpp"
#include "statusbar/avtp/avtp_escf.hpp"
#include "statusbar/avtp/avtp_ip_encap.hpp"
#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/avtp/avtp_ntscf.hpp"
#include "statusbar/avtp/avtp_ntscf_v1.hpp"
#include "statusbar/avtp/avtp_tscf.hpp"
#include "statusbar/avtp/avtp_tscf_v1.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/ip/ip_ipv4.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/ip/ip_udp.hpp"
#include "statusbar/pcap/pcap_writer.hpp"
#include "statusbar/tsn/tsn_clock_identity.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"
#include "wireshark_golden_acf.hpp"
#include "wireshark_golden_atdecc.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::avtp;
using ieee::Eui48;
using ieee::Eui64;
using tsn::StreamId;

namespace {

constexpr uint16_t AVTP_ETHERTYPE_VALUE = 0x22F0;
constexpr uint16_t IPV4_ETHERTYPE = 0x0800;
constexpr uint16_t UDP_SOURCE_PORT = 40000;

Eui48 const DA{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
Eui48 const SA{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
StreamId const SID{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, 0x0001};

/// Append a serializable wire struct to @p out
template <typename T>
void append(std::vector<uint8_t>& out, T const& value)
{
    auto const bytes = make_const_span(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Append raw octets to @p out
void append(std::vector<uint8_t>& out, std::span<uint8_t const> const bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

auto aaf_v0() -> std::vector<uint8_t>
{
    AafPdu pdu{};
    pdu.init(SID, AafFormat::int_32bit, AafSampleRate::rate_48_khz, 2, 32);
    pdu.set_sequence_num(7);
    pdu.set_tv(true);
    pdu.avtp_timestamp = 0x12345678U;
    pdu.set_stream_data_length(16);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 16> samples{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    append(out, std::span<uint8_t const>(samples));
    return out;
}

auto aaf_v1() -> std::vector<uint8_t>
{
    AafV1Pdu pdu{};
    pdu.init(SID, AafFormat::int_32bit, AafSampleRate::rate_48_khz, 2, 32);
    pdu.sequence_num = 0x01020304U;
    pdu.set_tv(true);
    pdu.avtp_timestamp = 0x1122334455667788ULL;
    pdu.ptp_grandmaster_identity = tsn::ClockIdentity{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x01};
    pdu.set_stream_data_length(16);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 16> samples{};
    samples[0] = 0xAA;
    samples[15] = 0x55;
    append(out, std::span<uint8_t const>(samples));
    return out;
}

auto crf() -> std::vector<uint8_t>
{
    CrfPdu pdu{};
    pdu.init_audio_sample(SID, 48000, CrfPull::multiply_1_0, 160, 2);
    pdu.set_sequence_num(3);
    pdu.set_crf_data_length(16);
    std::vector<uint8_t> out;
    append(out, pdu);
    for (uint64_t const ts : {0x0000000100000000ULL, 0x0000000100051615ULL}) {
        ieee::octlet_t const value{ts};
        append(out, value);
    }
    return out;
}

auto tscf_with_can() -> std::vector<uint8_t>
{
    std::array<uint8_t, 24> acf{};
    AcfCanMessage can{};
    can.init();
    can.set_can_bus_id(3);
    can.set_eff(true);
    can.set_can_identifier(0x18DAF110U);
    std::array<uint8_t, 3> const data{0x02, 0x10, 0x03};
    auto const can_octets = acf_can_build(std::span<uint8_t>(acf), can, std::span<uint8_t const>(data));
    (void)acf_checksum_build(std::span<uint8_t>(acf).subspan(can_octets), std::span<uint8_t const>(acf).first(can_octets));

    TscfPdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num(9);
    pdu.set_tv(true);
    pdu.set_avtp_timestamp(1000);
    pdu.set_stream_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, std::span<uint8_t const>(acf));
    return out;
}

auto ntscf_with_gpc() -> std::vector<uint8_t>
{
    std::array<uint8_t, 12> acf{};
    AcfGpcMessage gpc{};
    gpc.init();
    gpc.set_gpc_msg_id(Eui48{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56});
    std::array<uint8_t, 4> const payload{0xDE, 0xAD, 0xBE, 0xEF};
    (void)acf_gpc_build(std::span<uint8_t>(acf), gpc, std::span<uint8_t const>(payload));

    NtscfPdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num_lsb(4);
    pdu.set_ntscf_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, std::span<uint8_t const>(acf));
    return out;
}

auto adp_entity_available() -> std::vector<uint8_t>
{
    atdecc::AdpDu pdu{};
    pdu.init_entity_available(Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x03});
    std::vector<uint8_t> out;
    append(out, pdu);
    return out;
}

auto maap_probe() -> std::vector<uint8_t>
{
    MaapDu pdu{};
    pdu.init_probe(SID, Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00}, 4);
    std::vector<uint8_t> out;
    append(out, pdu);
    return out;
}

auto maap_defend() -> std::vector<uint8_t>
{
    MaapDu pdu{};
    pdu.init_defend(SID, Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00}, 4, Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x02}, 2);
    std::vector<uint8_t> out;
    append(out, pdu);
    return out;
}

auto maap_announce() -> std::vector<uint8_t>
{
    MaapDu pdu{};
    pdu.init_announce(SID, Eui48{0x91, 0xE0, 0xF0, 0x00, 0x10, 0x00}, 8);
    std::vector<uint8_t> out;
    append(out, pdu);
    return out;
}

auto aaf_v0_int16() -> std::vector<uint8_t>
{
    AafPdu pdu{};
    pdu.init(SID, AafFormat::int_16bit, AafSampleRate::rate_96_khz, 8, 16);
    pdu.set_sequence_num(200);
    pdu.set_stream_data_length(32);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 32> samples{};
    samples[1] = 0x7F;
    append(out, std::span<uint8_t const>(samples));
    return out;
}

/// A version-0 stream header for the formats that have no dedicated PDU
/// struct in this repository (CVF, SVF, RVF, VSF, MMA, EF, raw IEC 61883):
/// bytes 16-19 and 22-23 are the format-specific quadlet/doublet, the
/// payload follows verbatim.
auto stream_pdu(
    uint8_t const subtype,
    uint8_t const sequence,
    uint32_t const format_specific,
    uint16_t const data_length,
    uint16_t const protocol_specific,
    std::span<uint8_t const> const payload) -> std::vector<uint8_t>
{
    AvtpStreamHeader header{};
    header.subtype = subtype;
    header.set_sv(true);
    header.sequence_num = sequence;
    header.avtp_timestamp = 0x0BADCAFEU;
    header.format_specific_data = format_specific;
    header.stream_data_length = data_length;
    header.protocol_specific_header = protocol_specific;
    std::span<uint8_t const> const sid_bytes = make_const_span(SID);
    std::vector<uint8_t> out;
    append(out, header);
    std::copy(sid_bytes.begin(), sid_bytes.end(), out.begin() + 4);
    append(out, payload);
    return out;
}

/// IEC 61883/IIDC with tag 0: an IIDC (video) payload, no CIP header
auto iec61883_iidc() -> std::vector<uint8_t>
{
    std::array<uint8_t, 8> const video{0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    // tag 0, channel 5, tcode 0xA, sy 0
    return stream_pdu(0x00, 30, 0, 8, 0x05A0U, std::span<uint8_t const>(video));
}

/// IEC 61883-4 (FMT 0x20, SPH 1): CIP dbs=6 fn=3 (192-octet source packets),
/// two source packets each with a 4-octet timestamp and a 188-octet TS packet
auto iec61883_mpegts() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{
        0x00,
        0x06,
        0xC4,
        0x10,
        0xA0,
        0x80,
        0x00,
        0x00};  // CIP: qi1=0 sid=0 dbs=6 fn=3 sph=1 dbc=0x10 | qi2=2 fmt=0x20 fdf=0x800000
    for (uint8_t packet = 0; packet < 2; ++packet) {
        std::array<uint8_t, 4> const timestamp{0x12, 0x34, 0x56, static_cast<uint8_t>(0x78U + packet)};
        payload.insert(payload.end(), timestamp.begin(), timestamp.end());
        std::array<uint8_t, 188> ts{};
        ts[0] = 0x47;
        ts[1] = static_cast<uint8_t>(0x40U | 0x01U);  // PUSI, PID 0x0100
        ts[2] = 0x00;
        ts[3] = static_cast<uint8_t>(0x10U | packet);  // payload only, cc
        ts[4] = 0x00;
        ts[5] = 0x00;
        ts[6] = 0x01;
        ts[7] = 0xE0;  // PES start code, video stream 0
        payload.insert(payload.end(), ts.begin(), ts.end());
    }
    // tag 1, channel 63, tcode 0xA
    return stream_pdu(0x00, 31, 0, static_cast<uint16_t>(payload.size()), 0x7FA0U, std::span<uint8_t const>(payload));
}

/// IEC 61883 with a CIP FMT this dissector does not decode (DV, FMT 0x00)
auto iec61883_dv() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x78, 0x00, 0x05, 0x80, 0x00, 0x00, 0x00, 0x1F, 0x07, 0x00, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF};
    return stream_pdu(0x00, 32, 0, static_cast<uint16_t>(payload.size()), 0x7FA0U, std::span<uint8_t const>(payload));
}

/// CVF MJPEG: one fragment at offset 0, type 1 (4:2:0), Q 90, 320x240
auto cvf_mjpeg() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x00, 0x00, 0x00, 0x01, 0x5A, 0x28, 0x1E, 0xFF, 0xD8, 0xFF, 0xE0};
    return stream_pdu(0x03, 40, 0x02000000U, static_cast<uint16_t>(payload.size()), 0x1000U, std::span<uint8_t const>(payload));
}

/// CVF H.264: single NAL unit (SPS) with a valid payload timestamp (ptv), M set
auto cvf_h264_sps() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x01, 0x86, 0xA0, 0x67, 0x42, 0x00, 0x1E, 0xAB, 0x40};
    return stream_pdu(0x03, 41, 0x02010000U, static_cast<uint16_t>(payload.size()), 0x3000U, std::span<uint8_t const>(payload));
}

/// CVF H.264: FU-A fragment, start of an IDR slice
auto cvf_h264_fu_a() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x01, 0x86, 0xA1, 0x7C, 0x85, 0x88, 0x84, 0x00, 0x33, 0xFF};
    return stream_pdu(0x03, 42, 0x02010000U, static_cast<uint16_t>(payload.size()), 0x2000U, std::span<uint8_t const>(payload));
}

/// CVF H.265: aggregation packet (type 48, layer 0, tid 1)
auto cvf_h265_ap() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x02, 0x00, 0x00, 0x60, 0x01, 0x00, 0x04, 0x40, 0x01, 0x0C, 0x01};
    return stream_pdu(0x03, 43, 0x02030000U, static_cast<uint16_t>(payload.size()), 0x2000U, std::span<uint8_t const>(payload));
}

/// CVF H.265: FU (type 49), end of an IDR_W_RADL unit
auto cvf_h265_fu() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x02, 0x00, 0x01, 0x62, 0x01, 0x53, 0xAA, 0xBB};
    return stream_pdu(0x03, 44, 0x02030000U, static_cast<uint16_t>(payload.size()), 0x3000U, std::span<uint8_t const>(payload));
}

/// CVF JPEG 2000: progressive, main header present, tile 3, fragment offset 0x000400
auto cvf_jpeg2000() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x33, 0x07, 0x00, 0x03, 0x00, 0x00, 0x04, 0x00, 0xFF, 0x4F, 0xFF, 0x51};
    return stream_pdu(0x03, 45, 0x02020000U, static_cast<uint16_t>(payload.size()), 0x0000U, std::span<uint8_t const>(payload));
}

/// SVF 1080i/59.94, line 21, guard band, frame 2
auto svf() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x12, 0x02, 0x1E, 0x05, 0x3F, 0xF0, 0x00, 0x40};
    // format 3, i_seq_num 7, line_number 21 | stream_data_length 8 | gb | reserved
    return stream_pdu(0x06, 50, 0x03070015U, static_cast<uint16_t>(payload.size()), 0x4000U, std::span<uint8_t const>(payload));
}

/// RVF 1920x1080, 10-bit 4:2:2, 60 fps, BT.709, 2 lines starting at 540, interlaced second field
auto rvf() -> std::vector<uint8_t>
{
    std::vector<uint8_t> payload{0x00, 0x23, 0x18, 0x82, 0x00, 0x09, 0x02, 0x1C, 0xAA, 0xBB, 0xCC, 0xDD};
    // active_pixels 1920 total_lines 1080 | stream_data_length | f | i
    return stream_pdu(0x07, 51, 0x07800438U, static_cast<uint16_t>(payload.size()), 0x2040U, std::span<uint8_t const>(payload));
}

/// Vendor specific stream, vendor id 00:1C:AB:00:00:01
auto vsf() -> std::vector<uint8_t>
{
    std::array<uint8_t, 4> const data{0xCA, 0xFE, 0xF0, 0x0D};
    return stream_pdu(0x6F, 52, 0x001CAB00U, 4, 0x0001U, std::span<uint8_t const>(data));
}

/// MMA stream (MIDI over AVTP, opaque here)
auto mma() -> std::vector<uint8_t>
{
    std::array<uint8_t, 4> const data{0x90, 0x3C, 0x7F, 0x00};
    return stream_pdu(0x01, 53, 0, 4, 0, std::span<uint8_t const>(data));
}

/// Experimental stream format
auto ef_stream() -> std::vector<uint8_t>
{
    std::array<uint8_t, 2> const data{0xEE, 0xFF};
    return stream_pdu(0x7F, 54, 0, 2, 0, std::span<uint8_t const>(data));
}

/// Experimental control format (control header kind)
auto ef_control() -> std::vector<uint8_t>
{
    return {0xFF, 0x80, 0x00, 0x04, 0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD};
}

/// 24-bit PCM, one channel, six samples including negative values
auto aaf_v0_int24() -> std::vector<uint8_t>
{
    AafPdu pdu{};
    pdu.init(SID, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 1, 24);
    pdu.set_sequence_num(21);
    pdu.set_stream_data_length(18);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 18> samples{
        0x00, 0x00, 0x01, 0x7F, 0xFF, 0xFF, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x12, 0x34, 0x56, 0xED, 0xCB, 0xAA};
    append(out, std::span<uint8_t const>(samples));
    return out;
}

/// 32-bit float, two channels, three frames of exactly representable values
auto aaf_v0_float32() -> std::vector<uint8_t>
{
    AafPdu pdu{};
    pdu.init(SID, AafFormat::float_32bit, AafSampleRate::rate_96_khz, 2, 32);
    pdu.set_sequence_num(22);
    pdu.set_stream_data_length(24);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<float, 6> const values{0.5F, -0.25F, 1.0F, -1.0F, 0.0F, 0.125F};
    for (auto const value : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        quadlet_t sample{};
        sample = bits;
        append(out, sample);
    }
    return out;
}

/// 16-bit PCM, two channels, with a trailing partial frame (one odd octet)
auto aaf_v0_partial_frame() -> std::vector<uint8_t>
{
    AafPdu pdu{};
    pdu.init(SID, AafFormat::int_16bit, AafSampleRate::rate_48_khz, 2, 16);
    pdu.set_sequence_num(23);
    pdu.set_stream_data_length(9);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 9> samples{0x00, 0x10, 0xFF, 0xF0, 0x00, 0x20, 0xFF, 0xE0, 0xAB};
    append(out, std::span<uint8_t const>(samples));
    return out;
}

/// AES3: two AES3 streams (four subframes per frame), two frames, with the
/// B/C/U/V bits exercised and a SMPTE ST 338 data type
auto aaf_v0_aes3() -> std::vector<uint8_t>
{
    AafPdu pdu{};
    pdu.init(SID, AafFormat::aes3_32bit, AafSampleRate::rate_48_khz, 2, 0);
    pdu.set_sequence_num(24);
    pdu.bit_depth = 0x01;                                                     // aes3_data_type_h
    pdu.rsv_sp_evt = static_cast<uint8_t>(pdu.rsv_sp_evt.get() | (2U << 5));  // DT_SMPTE338
    pdu.reserved = 0x02;                                                      // aes3_data_type_l
    pdu.set_stream_data_length(32);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 32> subframes{
        0x08, 0x00, 0x01, 0x00,                                                  // stream 0 sf 1: B, sample 0x000100
        0x04, 0xFF, 0xFF, 0x00,                                                  // stream 0 sf 2: C, sample -256
        0x02, 0x12, 0x34, 0x56,                                                  // stream 1 sf 1: U
        0x01, 0x80, 0x00, 0x00,                                                  // stream 1 sf 2: V, sample -8388608
        0x00, 0x00, 0x02, 0x00,                                                  // frame 1
        0x00, 0xFF, 0xFE, 0x00, 0x00, 0x65, 0x43, 0x21, 0x0F, 0x7F, 0xFF, 0xFF,  // all four bits, max positive
    };
    append(out, std::span<uint8_t const>(subframes));
    return out;
}

/// AM824 with three channels of mixed labels: MBLA audio, an IEC 60958
/// conformant sample and a MIDI conformant quadlet, two data blocks
auto am824_v0_mixed_labels() -> std::vector<uint8_t>
{
    Am824Pdu pdu{};
    pdu.init(SID, 3, Am824SampleRate::rate_44_1_khz);
    pdu.set_sequence_num(12);
    pdu.set_stream_data_length(static_cast<uint16_t>(Cip61883Header::LENGTH + 24));
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 24> quadlets{
        0x40, 0xFF, 0xFF, 0xFF,                          // MBLA -1
        0x00, 0x7F, 0xFF, 0xFF,                          // IEC 60958 conformant, max positive
        0x81, 0x90, 0x00, 0x00,                          // MIDI, 1 byte (note on)
        0x40, 0x80, 0x00, 0x00,                          // MBLA min negative
        0x00, 0x00, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00,  // MIDI no data
    };
    append(out, std::span<uint8_t const>(quadlets));
    return out;
}

auto am824_v0() -> std::vector<uint8_t>
{
    Am824Pdu pdu{};
    pdu.init(SID, 2, Am824SampleRate::rate_48_khz);
    pdu.set_sequence_num(11);
    pdu.set_stream_data_length(static_cast<uint16_t>(Cip61883Header::LENGTH + 16));
    pdu.set_syt_timestamp(0x1234);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 16> quadlets{
        0x40, 0x00, 0x00, 0x01, 0x40, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x03, 0x40, 0x00, 0x00, 0x04};
    append(out, std::span<uint8_t const>(quadlets));
    return out;
}

auto am824_v1() -> std::vector<uint8_t>
{
    Am824V1Pdu pdu{};
    pdu.init(SID, 2, Am824SampleRate::rate_96_khz);
    pdu.set_sequence_num(0x00010000U);
    pdu.set_stream_data_length(static_cast<uint16_t>(Cip61883Header::LENGTH + 8));
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 8> quadlets{0x40, 0x00, 0x00, 0x05, 0x40, 0x00, 0x00, 0x06};
    append(out, std::span<uint8_t const>(quadlets));
    return out;
}

auto crf_v1() -> std::vector<uint8_t>
{
    CrfV1Pdu pdu{};
    pdu.init_audio_sample(SID, 96000, CrfPull::multiply_1_div_1001, 192);
    pdu.set_sequence_num(0x0000ABCDU);
    pdu.set_crf_data_length(8);
    pdu.ptp_grandmaster_identity = tsn::ClockIdentity{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x01};
    std::vector<uint8_t> out;
    append(out, pdu);
    ieee::octlet_t const ts{0x0000000200000000ULL};
    append(out, ts);
    return out;
}

auto tscf_v1() -> std::vector<uint8_t>
{
    std::array<uint8_t, 12> acf{};
    AcfGpcMessage gpc{};
    gpc.init();
    gpc.set_gpc_msg_id(Eui48{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x01});
    std::array<uint8_t, 4> const payload{0x01, 0x02, 0x03, 0x04};
    (void)acf_gpc_build(std::span<uint8_t>(acf), gpc, std::span<uint8_t const>(payload));
    TscfV1Pdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num(0x00000101U);
    pdu.set_stream_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, std::span<uint8_t const>(acf));
    return out;
}

auto ntscf_v1() -> std::vector<uint8_t>
{
    std::array<uint8_t, 24> acf{};
    AcfCanMessage can{};
    can.init();
    can.set_can_bus_id(1);
    can.set_can_identifier(0x123U);
    std::array<uint8_t, 8> const data{1, 2, 3, 4, 5, 6, 7, 8};
    (void)acf_can_build(std::span<uint8_t>(acf), can, std::span<uint8_t const>(data));
    NtscfV1Pdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num(0x00000202U);
    pdu.set_ntscf_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, std::span<uint8_t const>(acf));
    return out;
}

Eui64 const KEY_ID{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x42};

auto aef_continuous() -> std::vector<uint8_t>
{
    AefContinuousPdu pdu{};
    pdu.init(static_cast<uint8_t>(AefEncMode::aes_gcm_siv), KEY_ID);
    pdu.set_stream_data_length(16);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 16> cipher{};
    cipher[0] = 0xC0;
    append(out, std::span<uint8_t const>(cipher));
    return out;
}

auto aef_discrete() -> std::vector<uint8_t>
{
    AefDiscretePdu pdu{};
    pdu.init(static_cast<uint8_t>(AefEncMode::aes_siv), KEY_ID);
    pdu.set_control_data_length(8);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 8> cipher{};
    cipher[7] = 0xD1;
    append(out, std::span<uint8_t const>(cipher));
    return out;
}

auto escf() -> std::vector<uint8_t>
{
    EscfPdu pdu{};
    pdu.init(0, KEY_ID);
    pdu.set_control_data_length(12);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 12> signed_payload{};
    signed_payload[0] = 0x51;
    append(out, std::span<uint8_t const>(signed_payload));
    return out;
}

auto eecf() -> std::vector<uint8_t>
{
    EecfPdu pdu{};
    pdu.init(0, KEY_ID);
    pdu.set_encrypted_payload_length(12);
    std::vector<uint8_t> out;
    append(out, pdu);
    std::array<uint8_t, 12> cipher{};
    cipher[0] = 0xE1;
    append(out, std::span<uint8_t const>(cipher));
    return out;
}

/// A TSCF version-0 AVTPDU carrying @p acf
auto tscf_frame(std::span<uint8_t const> const acf, uint8_t const sequence) -> std::vector<uint8_t>
{
    TscfPdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num(sequence);
    pdu.set_stream_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, acf);
    return out;
}

/// An NTSCF version-0 AVTPDU carrying @p acf
auto ntscf_frame(std::span<uint8_t const> const acf, uint8_t const sequence) -> std::vector<uint8_t>
{
    NtscfPdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num_lsb(sequence);
    pdu.set_ntscf_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, acf);
    return out;
}

/// An NTSCF version-1 AVTPDU carrying @p acf
auto ntscf_v1_frame(std::span<uint8_t const> const acf, uint32_t const sequence) -> std::vector<uint8_t>
{
    NtscfV1Pdu pdu{};
    pdu.init(SID);
    pdu.set_sequence_num(sequence);
    pdu.set_ntscf_data_length(static_cast<uint16_t>(acf.size()));
    std::vector<uint8_t> out;
    append(out, pdu);
    append(out, acf);
    return out;
}

/// @p parts concatenated
auto concat(std::initializer_list<std::vector<uint8_t>> const parts) -> std::vector<uint8_t>
{
    std::vector<uint8_t> out;
    for (auto const& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

/// An Ethernet payload carrying IPv4 + UDP + IP AVTPDU header + @p avtpdu
auto udp_encapsulated(std::span<uint8_t const> const avtpdu, uint16_t const dst_port, uint32_t const sequence)
    -> std::vector<uint8_t>
{
    IpAvtpduHeader encap{};
    encap.init(sequence);
    auto const udp_payload_length = static_cast<uint16_t>(IpAvtpduHeader::LENGTH + avtpdu.size());

    ip::UdpHeader udp{};
    udp.init(UDP_SOURCE_PORT, dst_port, udp_payload_length);

    ip::IPv4Header ipv4{};
    ipv4.init(ip::IPv4Header::PROTOCOL_UDP, static_cast<uint16_t>(ip::UdpHeader::LENGTH + udp_payload_length));
    ipv4.src_addr = ip::IPv4Address{192, 168, 1, 10};
    ipv4.dst_addr = ip::IPv4Address{192, 168, 1, 20};
    ip::update_ipv4_checksum(ipv4);

    std::vector<uint8_t> out;
    append(out, ipv4);
    append(out, udp);
    append(out, encap);
    append(out, avtpdu);
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.pcap>\n", argv[0]);
        return 2;
    }
    // The writer appends to an existing file; the test fixture re-runs, so
    // start from an empty capture every time.
    (void)std::remove(argv[1]);
    auto writer = pcap::FileWriter::open(argv[1]);
    if (!writer.has_value()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    uint64_t timestamp_us = 1'700'000'000'000'000ULL;
    auto const ethernet = [&](std::vector<uint8_t> const& avtpdu) {
        (void)writer->write_packet(timestamp_us, DA, SA, AVTP_ETHERTYPE_VALUE, std::span<uint8_t const>(avtpdu));
        timestamp_us += 1000;
    };
    auto const udp = [&](std::vector<uint8_t> const& avtpdu, uint16_t const port, uint32_t const sequence) {
        auto const payload = udp_encapsulated(std::span<uint8_t const>(avtpdu), port, sequence);
        (void)writer->write_packet(timestamp_us, DA, SA, IPV4_ETHERTYPE, std::span<uint8_t const>(payload));
        timestamp_us += 1000;
    };

    ethernet(aaf_v0());
    ethernet(aaf_v1());
    ethernet(crf());
    ethernet(tscf_with_can());
    ethernet(ntscf_with_gpc());
    ethernet(adp_entity_available());
    ethernet(maap_probe());
    ethernet(maap_defend());
    ethernet(maap_announce());
    ethernet(aaf_v0_int16());
    ethernet(am824_v0());
    ethernet(am824_v1());
    ethernet(crf_v1());
    ethernet(tscf_v1());
    ethernet(ntscf_v1());
    ethernet(aef_continuous());
    ethernet(aef_discrete());
    ethernet(escf());
    ethernet(eecf());
    ethernet(aaf_v0_int24());
    ethernet(aaf_v0_float32());
    ethernet(aaf_v0_partial_frame());
    ethernet(aaf_v0_aes3());
    ethernet(am824_v0_mixed_labels());
    ethernet(iec61883_iidc());
    ethernet(iec61883_mpegts());
    ethernet(iec61883_dv());
    ethernet(cvf_mjpeg());
    ethernet(cvf_h264_sps());
    ethernet(cvf_h264_fu_a());
    ethernet(cvf_h265_ap());
    ethernet(cvf_h265_fu());
    ethernet(cvf_jpeg2000());
    ethernet(svf());
    ethernet(rvf());
    ethernet(vsf());
    ethernet(mma());
    ethernet(ef_stream());
    ethernet(ef_control());
    udp(cvf_h264_fu_a(), IP_AVTPDU_PORT_CONTINUOUS, 52);
    udp(aaf_v0(), IP_AVTPDU_PORT_CONTINUOUS, 42);
    udp(tscf_with_can(), IP_AVTPDU_PORT_CONTINUOUS, 43);
    udp(adp_entity_available(), IP_AVTPDU_PORT_DISCRETE, 44);
    udp(crf_v1(), IP_AVTPDU_PORT_CONTINUOUS, 45);
    udp(maap_announce(), IP_AVTPDU_PORT_DISCRETE, 46);

    // Every clause 9.4 ACF message type, one per TSCF frame ...
    auto const messages = golden::acf_golden_messages();
    uint8_t sequence = 0;
    for (auto const& message : messages) {
        ethernet(tscf_frame(std::span<uint8_t const>(message.octets), sequence++));
    }
    // ... then trailers, a user type, several messages per frame, both
    // control formats and versions, a broken trailer, and UDP.
    auto const& can = messages[1].octets;
    auto const& lin = messages[5].octets;
    auto const& most = messages[7].octets;
    auto const& gpc = messages[10].octets;
    auto const& flexray = messages[0].octets;
    auto const& i2c = messages[19].octets;
    auto const& serial = messages[11].octets;
    ethernet(ntscf_frame(
        concat(
            {golden::acf_with_checksum(can),
             golden::acf_with_crc(lin, AcfCrcType::crc_eth),
             golden::acf_with_crc(most, AcfCrcType::crc_32p4),
             golden::acf_user_message()}),
        1));
    auto broken = golden::acf_with_checksum(can);
    broken[5] ^= 0x40U;
    ethernet(tscf_frame(concat({broken, serial}), 0x77));
    ethernet(ntscf_v1_frame(concat({flexray, gpc}), 0x00000303U));
    udp(tscf_frame(concat({i2c, serial}), 0x55), IP_AVTPDU_PORT_CONTINUOUS, 47);
    udp(ntscf_frame(concat({golden::acf_with_crc(can, AcfCrcType::crc_eth)}), 2), IP_AVTPDU_PORT_DISCRETE, 48);

    // ATDECC: discovery, connection management, enumeration/control (each
    // command followed by its response so the dissector's pairing is exercised),
    // Address Access, Vendor Unique, then a few over UDP (1722.1 Annex)
    auto const atdecc_frames = atdecc::golden::atdecc_golden_frames();
    for (auto const& frame : atdecc_frames) {
        ethernet(frame.octets);
    }
    udp(atdecc_frames[0].octets, IP_AVTPDU_PORT_DISCRETE, 49);
    udp(atdecc_frames[3].octets, IP_AVTPDU_PORT_DISCRETE, 50);
    udp(atdecc_frames[9].octets, IP_AVTPDU_PORT_DISCRETE, 51);
    writer->flush();
    return 0;
}
