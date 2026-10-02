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
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_ip_encap.hpp"
#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/avtp/avtp_ntscf.hpp"
#include "statusbar/avtp/avtp_tscf.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/ip/ip_ipv4.hpp"
#include "statusbar/ip/ip_ipv4_address.hpp"
#include "statusbar/ip/ip_udp.hpp"
#include "statusbar/pcap/pcap_writer.hpp"
#include "statusbar/tsn/tsn_clock_identity.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
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
    udp(aaf_v0(), IP_AVTPDU_PORT_CONTINUOUS, 42);
    udp(tscf_with_can(), IP_AVTPDU_PORT_CONTINUOUS, 43);
    udp(adp_entity_available(), IP_AVTPDU_PORT_DISCRETE, 44);
    writer->flush();
    return 0;
}
