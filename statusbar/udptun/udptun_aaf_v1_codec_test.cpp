// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_aaf_v1_codec.hpp"

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_v1.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"
#include "statusbar/tsn/tsn_well_known_grandmasters.hpp"
#include "statusbar/udptun/udptun_codec_concept.hpp"

#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;

namespace {

auto make_stream_id(uint16_t const unique_id) -> tsn::StreamId
{
    ieee::Eui48 const mac{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00};
    return tsn::StreamId{mac, unique_id};
}

auto sid_to_eui64(tsn::StreamId const& sid) -> ieee::Eui64
{
    ieee::Eui64 e{};
    e.from_uint64(sid.to_uint64());
    return e;
}

udptun::AafV1OverAnnexJCodec::Config base_config(tsn::StreamId const primary, tsn::StreamId const redundant)
{
    udptun::AafV1OverAnnexJCodec::Config c{};
    c.stream_id = primary;
    c.redundant_stream_id = redundant;
    c.format = avtp::AafFormat::int_32bit;
    c.sample_rate = avtp::AafSampleRate::rate_96_khz;
    c.channels = 8;
    c.bit_depth = 32;
    c.samples_per_packet = 12;
    c.interval_us = 125;
    return c;
}

// A full datagram: 44-byte header (written by encode) + PCM payload filled with
// a recognizable ramp so decode()'s audio span can be verified byte-for-byte.
auto make_datagram(udptun::AafV1OverAnnexJCodec const& codec, tsn::StreamId const sender, uint32_t const seq, int64_t const tai_ns)
    -> std::vector<uint8_t>
{
    auto const total = udptun::AafV1OverAnnexJCodec::header_size() + codec.payload_bytes();
    std::vector<uint8_t> buf(total, 0);
    for (size_t i = 0; i < codec.payload_bytes(); ++i) {
        buf[udptun::AafV1OverAnnexJCodec::header_size() + i] = static_cast<uint8_t>(i & 0xFFU);
    }
    codec.encode(buf, sid_to_eui64(sender), seq, tai_ns, /*interval_us*/ 125);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Concept compliance + sizes
// ---------------------------------------------------------------------------

TEST(aaf_v1_codec_concept, satisfies_udptun_codec)
{
    static_assert(udptun::Codec<udptun::AafV1OverAnnexJCodec>);
    // 4-byte Annex J encap + 40-byte AAF v1 header.
    EXPECT_EQ(udptun::AafV1OverAnnexJCodec::header_size(), size_t{44});
}

TEST(aaf_v1_codec_sizes, payload_bytes_matches_dimensions)
{
    auto const sid = make_stream_id(1);
    udptun::AafV1OverAnnexJCodec const codec{base_config(sid, sid)};
    // 12 samples * 8 channels * 4 bytes (int32) = 384.
    EXPECT_EQ(codec.payload_bytes(), size_t{384});
}

// ---------------------------------------------------------------------------
// encode/decode round-trip — header fields, TAI timestamp, grandmaster, payload
// ---------------------------------------------------------------------------

TEST(aaf_v1_codec_roundtrip, recovers_header_fields)
{
    auto const sid = make_stream_id(1);
    udptun::AafV1OverAnnexJCodec const codec{base_config(sid, sid)};

    int64_t const tai_ns = 1'780'000'000'123'456'789LL;  // TAI ns since epoch
    auto const buf = make_datagram(codec, sid, /*seq*/ 42, tai_ns);

    auto const pkt = codec.decode(std::span<uint8_t const>(buf));
    EXPECT_TRUE(pkt.has_value());

    EXPECT_EQ(codec.sequence(*pkt), uint32_t{42});
    EXPECT_EQ(codec.tx_gptp_ns(*pkt), tai_ns);  // TAI presentation time preserved
    EXPECT_EQ(codec.sender_id(*pkt), sid_to_eui64(sid));
    EXPECT_EQ(codec.announced_interval_us(*pkt), uint32_t{125});

    // AAF format fields survive the round-trip.
    EXPECT_TRUE(pkt->pdu.get_format() == avtp::AafFormat::int_32bit);
    EXPECT_TRUE(pkt->pdu.nsr() == avtp::AafSampleRate::rate_96_khz);
    EXPECT_EQ(pkt->pdu.channels_per_frame(), uint16_t{8});
    EXPECT_EQ(pkt->pdu.get_bit_depth(), uint8_t{32});
    EXPECT_TRUE(pkt->pdu.tv());

    // Clock domain tagged as TAI-from-GPS.
    EXPECT_TRUE(pkt->pdu.get_ptp_grandmaster_identity() == tsn::GRANDMASTER_TAI_FROM_GPS);

    // Annex J encapsulation sequence carried alongside the AAF sequence.
    EXPECT_EQ(pkt->encap_seq, uint32_t{42});
}

TEST(aaf_v1_codec_roundtrip, the_timestamp_uncertain_bit_is_set_only_on_request)
{
    auto const primary = make_stream_id(0x0001);
    udptun::AafV1OverAnnexJCodec const codec{base_config(primary, primary)};
    std::vector<uint8_t> buf(udptun::AafV1OverAnnexJCodec::header_size() + codec.payload_bytes(), 0);

    // The form without the argument vouches for its timestamp.
    codec.encode(buf, sid_to_eui64(primary), 1, 1'700'000'000'000'000'000LL, 125);
    auto const sure = codec.decode(buf);
    EXPECT_TRUE(sure.has_value());
    EXPECT_FALSE(sure->pdu.tu());

    codec.encode(buf, sid_to_eui64(primary), 2, 1'700'000'000'000'000'000LL, 125, /*timestamp_uncertain=*/true);
    auto const unsure = codec.decode(buf);
    EXPECT_TRUE(unsure.has_value());
    EXPECT_TRUE(unsure->pdu.tu());
    // Nothing else about the packet changes: the timestamp is still the
    // talker's best and the grandmaster identity still names its timeline.
    EXPECT_TRUE(unsure->pdu.tv());
    EXPECT_EQ(codec.tx_gptp_ns(*unsure), 1'700'000'000'000'000'000LL);
    EXPECT_TRUE(unsure->pdu.get_ptp_grandmaster_identity() == tsn::GRANDMASTER_TAI_FROM_GPS);

    codec.encode(buf, sid_to_eui64(primary), 3, 1'700'000'000'000'000'000LL, 125, /*timestamp_uncertain=*/false);
    EXPECT_FALSE(codec.decode(buf)->pdu.tu());
}

TEST(aaf_v1_codec_roundtrip, recovers_audio_payload)
{
    auto const sid = make_stream_id(1);
    udptun::AafV1OverAnnexJCodec const codec{base_config(sid, sid)};
    auto const buf = make_datagram(codec, sid, /*seq*/ 1, /*tai*/ 1'000'000'000LL);

    auto const pkt = codec.decode(std::span<uint8_t const>(buf));
    EXPECT_TRUE(pkt.has_value());
    EXPECT_EQ(pkt->audio.size(), codec.payload_bytes());
    bool ramp_ok = true;
    for (size_t i = 0; i < pkt->audio.size(); ++i) {
        if (pkt->audio[i] != static_cast<uint8_t>(i & 0xFFU)) {
            ramp_ok = false;
            break;
        }
    }
    EXPECT_TRUE(ramp_ok);
}

TEST(aaf_v1_codec_roundtrip, rejects_short_buffer)
{
    auto const sid = make_stream_id(1);
    udptun::AafV1OverAnnexJCodec const codec{base_config(sid, sid)};
    std::vector<uint8_t> buf(20, 0);  // < 44-byte header
    EXPECT_FALSE(codec.decode(std::span<uint8_t const>(buf)).has_value());
    EXPECT_FALSE(codec.validate_for_reflect(std::span<uint8_t const>(buf)));
}

TEST(aaf_v1_codec_roundtrip, rejects_corrupt_aaf_header)
{
    auto const sid = make_stream_id(1);
    udptun::AafV1OverAnnexJCodec const codec{base_config(sid, sid)};
    auto buf = make_datagram(codec, sid, 1, 1'000'000'000LL);
    buf[4] = 0x00;  // corrupt AAF subtype (byte 0 of the AVTPDU, after 4-byte encap)
    EXPECT_FALSE(codec.decode(std::span<uint8_t const>(buf)).has_value());
}

// ---------------------------------------------------------------------------
// classify — legacy (no redundancy) and primary/redundant pairing.
// ---------------------------------------------------------------------------

TEST(aaf_v1_codec_classify, legacy_self_and_remote)
{
    auto const sid = make_stream_id(1);
    udptun::AafV1OverAnnexJCodec const codec{base_config(sid, sid)};  // redundant == primary → legacy
    auto const buf = make_datagram(codec, sid, 1, 1'000'000'000LL);
    auto const pkt = codec.decode(std::span<uint8_t const>(buf));
    EXPECT_TRUE(pkt.has_value());

    EXPECT_TRUE(codec.classify(*pkt, sid_to_eui64(sid)) == udptun::PacketRole::SelfLegacy);
    EXPECT_TRUE(codec.classify(*pkt, sid_to_eui64(make_stream_id(99))) == udptun::PacketRole::RemoteLegacy);
}

TEST(aaf_v1_codec_classify, primary_and_redundant_pairing)
{
    auto const primary = make_stream_id(1);
    auto const redundant = make_stream_id(2);
    udptun::AafV1OverAnnexJCodec const codec{base_config(primary, redundant)};

    auto const buf_p = make_datagram(codec, primary, 1, 1'000'000'000LL);
    auto const buf_r = make_datagram(codec, redundant, 1, 1'000'000'000LL);
    auto const pkt_p = codec.decode(std::span<uint8_t const>(buf_p));
    auto const pkt_r = codec.decode(std::span<uint8_t const>(buf_r));
    EXPECT_TRUE(pkt_p.has_value());
    EXPECT_TRUE(pkt_r.has_value());

    // Self (my_pair_id == primary): primary→SelfPrimary, redundant→SelfRedundant.
    EXPECT_TRUE(codec.classify(*pkt_p, sid_to_eui64(primary)) == udptun::PacketRole::SelfPrimary);
    EXPECT_TRUE(codec.classify(*pkt_r, sid_to_eui64(primary)) == udptun::PacketRole::SelfRedundant);

    // Remote (my_pair_id is someone else).
    auto const other = sid_to_eui64(make_stream_id(99));
    EXPECT_TRUE(codec.classify(*pkt_p, other) == udptun::PacketRole::RemotePrimary);
    EXPECT_TRUE(codec.classify(*pkt_r, other) == udptun::PacketRole::RemoteRedundant);

    // Primary and redundant collapse to the same logical sender; literal ids differ.
    EXPECT_EQ(codec.sender_pair_id(*pkt_p), codec.sender_pair_id(*pkt_r));
    EXPECT_NE(codec.sender_id(*pkt_p), codec.sender_id(*pkt_r));
}

TEST_MAIN(statusbar_udptun, udptun_aaf_v1_codec_test)
