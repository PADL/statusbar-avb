// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::tsn;
using namespace statusbar::ieee;

//
// Compile-time verification of constexpr functions using static_assert
//

// AM824 sample rate names - tested at runtime (non-constexpr)

// AM824 sample rate Hz values
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_32_khz) == 32000);
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_44_1_khz) == 44100);
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_48_khz) == 48000);
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_88_2_khz) == 88200);
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_96_khz) == 96000);
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_176_4_khz) == 176400);
static_assert(am824_sample_rate_hz(Am824SampleRate::rate_192_khz) == 192000);

// AM824 SYT interval values
static_assert(am824_syt_interval(Am824SampleRate::rate_32_khz) == 8);
static_assert(am824_syt_interval(Am824SampleRate::rate_44_1_khz) == 8);
static_assert(am824_syt_interval(Am824SampleRate::rate_48_khz) == 8);
static_assert(am824_syt_interval(Am824SampleRate::rate_88_2_khz) == 16);
static_assert(am824_syt_interval(Am824SampleRate::rate_96_khz) == 16);
static_assert(am824_syt_interval(Am824SampleRate::rate_176_4_khz) == 32);
static_assert(am824_syt_interval(Am824SampleRate::rate_192_khz) == 32);

//
// Name function tests (runtime, non-constexpr)
//

TEST(am824_names, sample_rate_name)
{
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_32_khz)[0], '3');
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_44_1_khz)[0], '4');
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_48_khz)[0], '4');
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_88_2_khz)[0], '8');
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_96_khz)[0], '9');
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_176_4_khz)[0], '1');
    EXPECT_EQ(am824_sample_rate_name(Am824SampleRate::rate_192_khz)[0], '1');
}

//
// Tests: AM824 Sample Rate
//

TEST(am824_sample_rate, name_48khz)
{
    auto const name = am824_sample_rate_name(Am824SampleRate::rate_48_khz);
    EXPECT_EQ(name, "48 kHz");
}

TEST(am824_sample_rate, hz_48khz)
{
    auto const hz = am824_sample_rate_hz(Am824SampleRate::rate_48_khz);
    EXPECT_EQ(hz, 48000U);
}

TEST(am824_sample_rate, syt_interval_48khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_48_khz);
    EXPECT_EQ(interval, 8U);
}

TEST(am824_sample_rate, syt_interval_96khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_96_khz);
    EXPECT_EQ(interval, 16U);
}

TEST(am824_sample_rate, syt_interval_192khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_192_khz);
    EXPECT_EQ(interval, 32U);
}

//
// Tests: AM824 Sample Conversion
//

TEST(am824_sample_convert, zero_to_float)
{
    int32_t const sample = 0;
    float const result = am824_sample_to_float(sample);
    EXPECT_TRUE(std::fabs(result) < 0.00001f);
}

TEST(am824_sample_convert, max_positive_to_float)
{
    int32_t const sample = 8388607;  // Max 24-bit positive
    float const result = am824_sample_to_float(sample);
    EXPECT_TRUE(std::fabs(result - 1.0f) < 0.0001f);
}

TEST(am824_sample_convert, max_negative_to_float)
{
    int32_t const sample = -8388608;  // Min 24-bit negative
    float const result = am824_sample_to_float(sample);
    EXPECT_TRUE(std::fabs(result - (-1.0f)) < 0.0001f);
}

TEST(am824_sample_convert, float_zero_to_sample)
{
    float const input = 0.0f;
    int32_t const result = float_to_am824_sample(input);
    EXPECT_EQ(result, 0);
}

TEST(am824_sample_convert, float_positive_max_clamps)
{
    float const input = 1.5f;  // Beyond range
    int32_t const result = float_to_am824_sample(input);
    EXPECT_EQ(result, 8388607);  // Should clamp
}

TEST(am824_sample_convert, float_negative_max_clamps)
{
    float const input = -1.5f;  // Beyond range
    int32_t const result = float_to_am824_sample(input);
    EXPECT_EQ(result, -8388608);  // Should clamp
}

TEST(am824_sample_convert, round_trip)
{
    int32_t const original = 1234567;
    float const f = am824_sample_to_float(original);
    int32_t const restored = float_to_am824_sample(f);
    // Allow some rounding error
    EXPECT_TRUE(std::abs(original - restored) <= 1);
}

//
// Tests: AM824 Quadlet Parsing
//

TEST(am824_quadlet, parse_audio_sample)
{
    // AM824 quadlet: label=0x40 (MBLA), sample=0x123456
    uint32_t const quadlet = 0x40123456U;
    int32_t const sample = parse_am824_quadlet(quadlet);
    EXPECT_EQ(sample, 0x123456);
}

TEST(am824_quadlet, parse_negative_sample)
{
    // AM824 quadlet: label=0x40 (MBLA), sample=0x800000 (negative in 24-bit)
    uint32_t const quadlet = 0x40800000U;
    int32_t const sample = parse_am824_quadlet(quadlet);
    EXPECT_TRUE(sample < 0);
}

TEST(am824_quadlet, parse_non_audio_returns_zero)
{
    // AM824 quadlet with non-audio label
    uint32_t const quadlet = 0x00123456U;  // label != 0x40
    int32_t const sample = parse_am824_quadlet(quadlet);
    EXPECT_EQ(sample, 0);
}

TEST(am824_quadlet, create_audio_sample)
{
    int32_t const sample = 0x123456;
    uint32_t const quadlet = create_am824_quadlet(sample);
    EXPECT_EQ(quadlet, 0x40123456U);
}

TEST(am824_quadlet, create_masks_to_24bits)
{
    int32_t const sample = 0x12345678;  // More than 24 bits
    uint32_t const quadlet = create_am824_quadlet(sample);
    // Lower 24 bits only
    EXPECT_EQ(quadlet, 0x40345678U);
}

TEST(am824_quadlet, round_trip)
{
    int32_t const original = 0x7ABCDE;
    uint32_t const quadlet = create_am824_quadlet(original);
    int32_t const restored = parse_am824_quadlet(quadlet);
    EXPECT_EQ(original, restored);
}

//
// Tests: AvtpStreamHeader
//

TEST(avtp_stream_header, default_constructor)
{
    AvtpStreamHeader header{};
    EXPECT_EQ(header.subtype.get(), 0U);
    EXPECT_FALSE(header.sv());
}

TEST(avtp_stream_header, init_61883_iidc)
{
    AvtpStreamHeader header{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    header.init_61883_iidc(sid);

    EXPECT_TRUE(header.subtype == AvtpSubtype::iec_61883_iidc);
    EXPECT_TRUE(header.sv());
    EXPECT_EQ(header.version(), 0U);
    EXPECT_EQ(header.stream_id(), sid);
}

TEST(avtp_stream_header, size_is_24_bytes)
{
    EXPECT_EQ(sizeof(AvtpStreamHeader), 24U);
}

//
// Tests: Cip61883Header
//

TEST(cip_header, default_constructor)
{
    Cip61883Header header{};
    EXPECT_EQ(header.data_block_size(), 0U);
    EXPECT_EQ(header.data_block_count(), 0U);
}

TEST(cip_header, init_am824)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);

    EXPECT_EQ(header.data_block_size(), 8U);  // 8 channels
    EXPECT_EQ(header.fmt(), AM824_FMT);
    EXPECT_EQ(header.sample_rate(), Am824SampleRate::rate_48_khz);
}

TEST(cip_header, size_is_8_bytes)
{
    EXPECT_EQ(sizeof(Cip61883Header), 8U);
}

TEST(cip_header, syt_not_valid_when_ffff)
{
    Cip61883Header header{};
    header.set_syt_timestamp(0xFFFFU);
    EXPECT_FALSE(header.syt_valid());
}

TEST(cip_header, syt_valid_when_set)
{
    Cip61883Header header{};
    header.set_syt_timestamp(0x1234U);
    EXPECT_TRUE(header.syt_valid());
    EXPECT_EQ(header.syt_timestamp(), 0x1234U);
}

//
// Tests: Am824Pdu
//

TEST(am824_pdu, default_constructor)
{
    Am824Pdu pdu{};
    EXPECT_FALSE(pdu.is_valid());
}

TEST(am824_pdu, init_creates_valid_packet)
{
    Am824Pdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, 8, Am824SampleRate::rate_48_khz);

    EXPECT_TRUE(pdu.is_valid());
    EXPECT_EQ(pdu.channel_count(), 8U);
    EXPECT_EQ(pdu.sample_rate(), Am824SampleRate::rate_48_khz);
}

TEST(am824_pdu, size_is_32_bytes)
{
    EXPECT_EQ(sizeof(Am824Pdu), 32U);
    EXPECT_EQ(Am824Pdu::HEADER_LENGTH, 32U);
}

TEST(am824_pdu, set_dimensions)
{
    Am824Pdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, 8, Am824SampleRate::rate_48_khz);
    pdu.set_dimensions(6, 8);  // 6 samples, 8 channels

    EXPECT_EQ(pdu.sample_count(), 6U);
    EXPECT_EQ(pdu.channel_count(), 8U);
    // stream_data_length = CIP header (8) + audio (6 * 8 * 4)
    EXPECT_EQ(pdu.stream_data_length(), 8U + 192U);
}

TEST(am824_pdu, sequence_num_wraps)
{
    Am824Pdu pdu{};
    pdu.set_sequence_num(255U);
    pdu.increment_sequence_num();
    EXPECT_EQ(pdu.sequence_num(), 0U);
}

//
// Tests: AM824 Serialization/Deserialization
//

TEST(am824_serialize, interleaved_simple)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 16> payload{};

    size_t const bytes = am824_serialize_interleaved(std::span{input}, 2, 2, std::span{payload});

    EXPECT_EQ(bytes, 16U);  // 2 * 2 * 4 bytes
    // First sample should have MBLA label (0x40)
    EXPECT_EQ(payload[0], 0x40U);
}

TEST(am824_serialize, interleaved_insufficient_output)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};
    std::array<uint8_t, 8> payload{};  // Too small

    size_t const bytes = am824_serialize_interleaved(std::span{input}, 2, 2, std::span{payload});

    EXPECT_EQ(bytes, 0U);  // Should fail
}

TEST(am824_deserialize, interleaved_simple)
{
    // Create test payload: 2 channels, 2 samples
    // Sample values: ch0_s0=0x100000, ch1_s0=0x200000, ch0_s1=0x300000, ch1_s1=0x400000
    std::array<uint8_t, 16> payload = {
        0x40,
        0x10,
        0x00,
        0x00,  // ch0_s0
        0x40,
        0x20,
        0x00,
        0x00,  // ch1_s0
        0x40,
        0x30,
        0x00,
        0x00,  // ch0_s1
        0x40,
        0x40,
        0x00,
        0x00,  // ch1_s1
    };
    std::array<float, 4> output{};

    size_t const samples = am824_deserialize_interleaved(std::span{payload}, 2, 2, std::span{output});

    EXPECT_EQ(samples, 2U);
    // Check that samples were extracted (not exact values due to conversion)
    EXPECT_TRUE(output[0] > 0.0f);
    EXPECT_TRUE(output[1] > output[0]);  // ch1_s0 > ch0_s0
}

TEST(am824_deserialize, channel_extraction)
{
    // Create test payload: 2 channels, 3 samples
    std::array<uint8_t, 24> payload = {
        // Sample 0
        0x40,
        0x10,
        0x00,
        0x00,  // ch0
        0x40,
        0x20,
        0x00,
        0x00,  // ch1
        // Sample 1
        0x40,
        0x30,
        0x00,
        0x00,  // ch0
        0x40,
        0x40,
        0x00,
        0x00,  // ch1
        // Sample 2
        0x40,
        0x50,
        0x00,
        0x00,  // ch0
        0x40,
        0x60,
        0x00,
        0x00,  // ch1
    };
    std::array<float, 3> ch0_output{};
    std::array<float, 3> ch1_output{};

    size_t const ch0_samples = am824_deserialize_channel(std::span{payload}, 2, 3, 0, std::span{ch0_output});
    size_t const ch1_samples = am824_deserialize_channel(std::span{payload}, 2, 3, 1, std::span{ch1_output});

    EXPECT_EQ(ch0_samples, 3U);
    EXPECT_EQ(ch1_samples, 3U);
    // Channel 1 should have larger values
    EXPECT_TRUE(ch1_output[0] > ch0_output[0]);
}

TEST(am824_serialize_deserialize, round_trip)
{
    // Create test audio data
    std::array<float, 16> original{};  // 4 channels, 4 samples
    for (size_t i = 0; i < 16; ++i) {
        original[i] = static_cast<float>(i) / 32.0f - 0.25f;  // Various values
    }

    std::array<uint8_t, 64> payload{};
    std::array<float, 16> restored{};

    // Serialize
    size_t const bytes = am824_serialize_interleaved(std::span{original}, 4, 4, std::span{payload});
    EXPECT_EQ(bytes, 64U);

    // Deserialize
    size_t const samples = am824_deserialize_interleaved(std::span<uint8_t const>{payload}, 4, 4, std::span{restored});
    EXPECT_EQ(samples, 4U);

    // Verify (allow for quantization error)
    for (size_t i = 0; i < 16; ++i) {
        float const diff = std::fabs(original[i] - restored[i]);
        EXPECT_TRUE(diff < 0.0001f);
    }
}

//
// Tests: High-level packet functions
//

TEST(am824_packet, packet_size_calculation)
{
    size_t const size = am824_packet_size(8, 6);  // 8 channels, 6 samples
    // Header (32) + payload (8 * 6 * 4)
    EXPECT_EQ(size, 32U + 192U);
}

TEST(am824_packet, create_and_parse)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    std::array<float, 8> audio{};  // 2 channels, 4 samples
    for (size_t i = 0; i < 8; ++i) {
        audio[i] = static_cast<float>(i) / 16.0f;
    }

    std::array<uint8_t, 64> packet{};

    // Create packet
    size_t const bytes =
        am824_create_packet(sid, 5, 0x12345678, Am824SampleRate::rate_48_khz, 0, std::span{audio}, 2, 4, std::span{packet});

    EXPECT_EQ(bytes, 32U + 32U);  // Header + 2*4*4 audio

    // Parse header
    auto pdu = am824_parse_header(std::span<uint8_t const>{packet.data(), bytes});
    EXPECT_TRUE(pdu.has_value());
    EXPECT_TRUE(pdu->is_valid());
    EXPECT_EQ(pdu->sequence_num(), 5U);
    EXPECT_EQ(pdu->avtp_timestamp(), 0x12345678U);
    EXPECT_EQ(pdu->channel_count(), 2U);
    EXPECT_EQ(pdu->sample_count(), 4U);
    EXPECT_EQ(pdu->sample_rate(), Am824SampleRate::rate_48_khz);
}

TEST(am824_packet, parse_invalid_returns_null)
{
    std::array<uint8_t, 32> packet{};  // All zeros
    auto pdu = am824_parse_header(std::span<uint8_t const>{packet});
    EXPECT_FALSE(pdu.has_value());  // Invalid packet
}

TEST(am824_packet, get_audio_payload)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    std::array<float, 4> audio = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 64> packet{};

    size_t const bytes = am824_create_packet(sid, 0, 0, Am824SampleRate::rate_48_khz, 0, std::span{audio}, 2, 2, std::span{packet});

    auto const payload = am824_get_audio_payload(std::span<uint8_t const>{packet.data(), bytes});
    EXPECT_EQ(payload.size(), 16U);  // 2 * 2 * 4

    // Deserialize and verify
    std::array<float, 4> restored{};
    size_t const samples = am824_deserialize_interleaved(payload, 2, 2, std::span{restored});
    EXPECT_EQ(samples, 2U);

    for (size_t i = 0; i < 4; ++i) {
        float const diff = std::fabs(audio[i] - restored[i]);
        EXPECT_TRUE(diff < 0.0001f);
    }
}

//
// Real AM824 Test Frames - Captured 8-channel 48kHz packets with Ethernet+VLAN header
// From IEEE 1722 AVTP stream capture
//

namespace {

/// Ethernet header offset: 6 (dest MAC) + 6 (src MAC) + 4 (VLAN) + 2 (ethertype) = 18 bytes
constexpr size_t ETHERNET_HEADER_SIZE = 18;

/// 0xdd768829 is 125 ns before the earliest timestamped packet (0xdd7688a6)
constexpr uint32_t START_AVTP_TIMESTAMP = 0xdd768829U;

// clang-format off

/// Frame 0: 8 channels, 48kHz, 6 samples, sequence=0x14
static uint8_t const session_1_8ch48kHz_0[242] = {
    0x91, 0xe0, 0xf0, 0x00, 0x16, 0xe1, 0x00, 0x01,  // Dest MAC
    0xf2, 0x00, 0xeb, 0x84, 0x81, 0x00, 0x60, 0x02,  // Src MAC + VLAN
    0x22, 0xf0, 0x00, 0x80, 0x14, 0x00, 0x00, 0x01,  // EtherType + AVTP header start
    0xf2, 0x00, 0xeb, 0x84, 0x00, 0x00, 0x00, 0x00,  // Stream ID + timestamp
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8,  // Format specific + stream_data_length
    0x5f, 0xa0, 0x3f, 0x08, 0x00, 0x72, 0x90, 0x02,  // CIP headers
    0x00, 0x08, 0x40, 0xff, 0xff, 0xea, 0x40, 0xff,  // Audio data
    0xff, 0xf0, 0x40, 0x00, 0x00, 0x29, 0x40, 0xff,
    0xff, 0xfb, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xf6, 0x40, 0xff,
    0xff, 0xcc, 0x40, 0xff, 0xff, 0xe1, 0x40, 0x00,
    0x00, 0x17, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x03, 0x40, 0xff,
    0xff, 0xeb, 0x40, 0x00, 0x00, 0x18, 0x40, 0xff,
    0xff, 0xff, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x36, 0x40, 0x00,
    0x00, 0x18, 0x40, 0x00, 0x00, 0x34, 0x40, 0xff,
    0xff, 0xc9, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xe2, 0x40, 0xff,
    0xff, 0xee, 0x40, 0x00, 0x00, 0x0c, 0x40, 0x00,
    0x00, 0x11, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x0e, 0x40, 0x00,
    0x00, 0x22, 0x40, 0x00, 0x00, 0x13, 0x40, 0x00,
    0x00, 0x06, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00
};

/// Frame 1: 8 channels, 48kHz, 6 samples, sequence=0x15, has AVTP timestamp
static uint8_t const session_1_8ch48kHz_1[242] = {
    0x91, 0xe0, 0xf0, 0x00, 0x16, 0xe1, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x81, 0x00, 0x60, 0x02,
    0x22, 0xf0, 0x00, 0x81, 0x15, 0x00, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x00, 0x00, 0xdd, 0x76,  // Has timestamp 0xdd7688a6
    0x88, 0xa6, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8,
    0x5f, 0xa0, 0x3f, 0x08, 0x00, 0x78, 0x90, 0x02,
    0x00, 0x08, 0x40, 0x00, 0x00, 0x0d, 0x40, 0x00,
    0x00, 0x09, 0x40, 0x00, 0x00, 0x0f, 0x40, 0x00,
    0x00, 0x0f, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x07, 0x40, 0x00,
    0x00, 0x16, 0x40, 0x00, 0x00, 0x1b, 0x40, 0xff,
    0xff, 0xd2, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xe9, 0x40, 0x00,
    0x00, 0x09, 0x40, 0xff, 0xff, 0xfe, 0x40, 0x00,
    0x00, 0x1e, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xdc, 0x40, 0xff,
    0xff, 0xd8, 0x40, 0x00, 0x00, 0x13, 0x40, 0xff,
    0xff, 0xda, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x09, 0x40, 0x00,
    0x00, 0x14, 0x40, 0x00, 0x00, 0x23, 0x40, 0xff,
    0xff, 0xd5, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x31, 0x40, 0x00,
    0x00, 0x1d, 0x40, 0xff, 0xff, 0xde, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00
};

/// Frame 2: 8 channels, 48kHz, 6 samples, sequence=0x16
static uint8_t const session_1_8ch48kHz_2[242] = {
    0x91, 0xe0, 0xf0, 0x00, 0x16, 0xe1, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x81, 0x00, 0x60, 0x02,
    0x22, 0xf0, 0x00, 0x81, 0x16, 0x00, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x00, 0x00, 0xdd, 0x79,
    0x13, 0xb5, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8,
    0x5f, 0xa0, 0x3f, 0x08, 0x00, 0x7e, 0x90, 0x02,
    0x00, 0x08, 0x40, 0xff, 0xff, 0xd3, 0x40, 0xff,
    0xff, 0xe6, 0x40, 0xff, 0xff, 0xe3, 0x40, 0xff,
    0xff, 0xfd, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xf4, 0x40, 0x00,
    0x00, 0x09, 0x40, 0x00, 0x00, 0x01, 0x40, 0xff,
    0xff, 0xef, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x18, 0x40, 0xff,
    0xff, 0xfa, 0x40, 0xff, 0xff, 0xf7, 0x40, 0x00,
    0x00, 0x21, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0x40, 0xff,
    0xff, 0xec, 0x40, 0xff, 0xff, 0xfd, 0x40, 0xff,
    0xff, 0xe3, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x40, 0x00,
    0x00, 0x09, 0x40, 0xff, 0xff, 0xe0, 0x40, 0x00,
    0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xf8, 0x40, 0xff,
    0xff, 0xfd, 0x40, 0x00, 0x00, 0x35, 0x40, 0x00,
    0x00, 0x2d, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00
};

/// Frame 3: 8 channels, 48kHz, 6 samples, sequence=0x17
static uint8_t const session_1_8ch48kHz_3[242] = {
    0x91, 0xe0, 0xf0, 0x00, 0x16, 0xe1, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x81, 0x00, 0x60, 0x02,
    0x22, 0xf0, 0x00, 0x81, 0x17, 0x00, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x00, 0x00, 0xdd, 0x7b,
    0x9e, 0xc2, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8,
    0x5f, 0xa0, 0x3f, 0x08, 0x00, 0x84, 0x90, 0x02,
    0x00, 0x08, 0x40, 0x00, 0x00, 0x12, 0x40, 0xff,
    0xff, 0xfa, 0x40, 0xff, 0xff, 0xd4, 0x40, 0x00,
    0x00, 0x43, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xa1, 0x40, 0xff,
    0xff, 0xdd, 0x40, 0x00, 0x00, 0x23, 0x40, 0x00,
    0x00, 0x16, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xc3, 0x40, 0xff,
    0xff, 0xef, 0x40, 0xff, 0xff, 0xfd, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xf3, 0x40, 0xff,
    0xff, 0xe1, 0x40, 0x00, 0x00, 0x0e, 0x40, 0xff,
    0xff, 0xe0, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xfe, 0x40, 0x00,
    0x00, 0x08, 0x40, 0x00, 0x00, 0x2a, 0x40, 0xff,
    0xff, 0xeb, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x08, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xe7, 0x40, 0xff,
    0xff, 0xd2, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00
};

/// Frame 4: 8 channels, 48kHz, 6 samples, sequence=0x18
static uint8_t const session_1_8ch48kHz_4[242] = {
    0x91, 0xe0, 0xf0, 0x00, 0x16, 0xe1, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x81, 0x00, 0x60, 0x02,
    0x22, 0xf0, 0x00, 0x80, 0x18, 0x00, 0x00, 0x01,
    0xf2, 0x00, 0xeb, 0x84, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc8,
    0x5f, 0xa0, 0x3f, 0x08, 0x00, 0x8a, 0x90, 0x02,
    0x00, 0x08, 0x40, 0x00, 0x00, 0x0f, 0x40, 0x00,
    0x00, 0x0c, 0x40, 0x00, 0x00, 0x0d, 0x40, 0x00,
    0x00, 0x1d, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xe9, 0x40, 0x00,
    0x00, 0x0c, 0x40, 0x00, 0x00, 0x09, 0x40, 0x00,
    0x00, 0x06, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0xff,
    0xff, 0xe2, 0x40, 0xff, 0xff, 0xf0, 0x40, 0x00,
    0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0xff, 0xff, 0xf5, 0x40, 0x00,
    0x00, 0x05, 0x40, 0x00, 0x00, 0x04, 0x40, 0x00,
    0x00, 0x08, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x24, 0x40, 0x00,
    0x00, 0x1e, 0x40, 0xff, 0xff, 0xf2, 0x40, 0x00,
    0x00, 0x1d, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x03, 0x40, 0xff,
    0xff, 0xe8, 0x40, 0x00, 0x00, 0x1c, 0x40, 0xff,
    0xff, 0xe0, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00
};

// clang-format on

/// Array of test frames for iteration
static uint8_t const* const test_frames[] = {
    session_1_8ch48kHz_0,
    session_1_8ch48kHz_1,
    session_1_8ch48kHz_2,
    session_1_8ch48kHz_3,
    session_1_8ch48kHz_4,
};

constexpr size_t NUM_TEST_FRAMES = 5;
constexpr size_t TEST_FRAME_SIZE = 242;

/// Get AVTP packet portion of a test frame (after Ethernet header)
inline std::span<uint8_t const> get_avtp_packet(uint8_t const* frame)
{
    return std::span<uint8_t const>{frame + ETHERNET_HEADER_SIZE, TEST_FRAME_SIZE - ETHERNET_HEADER_SIZE};
}

}  // namespace

//
// Tests: Real AM824 Packet Parsing
//

TEST(am824_real_frames, parse_frame_0_header)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_0);
    auto pdu = am824_parse_header(packet);

    EXPECT_TRUE(pdu.has_value());
    EXPECT_TRUE(pdu->is_valid());
    EXPECT_EQ(pdu->channel_count(), 8U);
    EXPECT_EQ(pdu->sample_rate(), Am824SampleRate::rate_48_khz);
    EXPECT_EQ(pdu->sequence_num(), 0x14U);
}

TEST(am824_real_frames, parse_frame_1_has_timestamp)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_1);
    auto pdu = am824_parse_header(packet);

    EXPECT_TRUE(pdu.has_value());
    EXPECT_TRUE(pdu->is_valid());
    EXPECT_EQ(pdu->sequence_num(), 0x15U);
    // Frame 1 has AVTP timestamp 0xdd7688a6
    EXPECT_EQ(pdu->avtp_timestamp(), 0xdd7688a6U);
}

TEST(am824_real_frames, parse_all_frames_valid)
{
    for (size_t i = 0; i < NUM_TEST_FRAMES; ++i) {
        auto const packet = get_avtp_packet(test_frames[i]);
        auto pdu = am824_parse_header(packet);

        EXPECT_TRUE(pdu.has_value());
        EXPECT_TRUE(pdu->is_valid());
        EXPECT_EQ(pdu->channel_count(), 8U);
        EXPECT_EQ(pdu->sample_rate(), Am824SampleRate::rate_48_khz);
    }
}

TEST(am824_real_frames, sequence_numbers_increment)
{
    uint8_t expected_seq[] = {0x14, 0x15, 0x16, 0x17, 0x18};

    for (size_t i = 0; i < NUM_TEST_FRAMES; ++i) {
        auto const packet = get_avtp_packet(test_frames[i]);
        auto pdu = am824_parse_header(packet);

        EXPECT_TRUE(pdu.has_value());
        EXPECT_EQ(pdu->sequence_num(), expected_seq[i]);
    }
}

TEST(am824_real_frames, stream_id_consistent)
{
    // Expected stream ID: 00:01:f2:00:eb:84 (from source MAC) with unique_id
    auto const packet0 = get_avtp_packet(session_1_8ch48kHz_0);
    auto pdu0 = am824_parse_header(packet0);
    EXPECT_TRUE(pdu0.has_value());

    StreamId const expected_sid = pdu0->stream_id();

    for (size_t i = 1; i < NUM_TEST_FRAMES; ++i) {
        auto const packet = get_avtp_packet(test_frames[i]);
        auto pdu = am824_parse_header(packet);

        EXPECT_TRUE(pdu.has_value());
        EXPECT_EQ(pdu->stream_id(), expected_sid);
    }
}

//
// Tests: Real AM824 Audio Deserialization
//

TEST(am824_real_frames, deserialize_frame_0_audio)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_0);
    auto pdu = am824_parse_header(packet);
    EXPECT_TRUE(pdu.has_value());

    uint8_t const channel_count = pdu->channel_count();
    uint8_t const sample_count = pdu->sample_count();
    EXPECT_EQ(channel_count, 8U);
    EXPECT_EQ(sample_count, 6U);  // 192 bytes payload / (8 channels * 4 bytes) = 6 samples

    auto const payload = am824_get_audio_payload(packet);
    EXPECT_EQ(payload.size(), 192U);  // 8 * 6 * 4

    // Deserialize all channels interleaved
    std::array<float, 48> audio{};  // 8 channels * 6 samples
    size_t const samples = am824_deserialize_interleaved(payload, channel_count, sample_count, std::span{audio});
    EXPECT_EQ(samples, 6U);

    // Channels 4-7 should be silent (all zeros in the test data)
    for (size_t s = 0; s < 6; ++s) {
        for (uint8_t ch = 4; ch < 8; ++ch) {
            EXPECT_TRUE(std::fabs(audio[s * 8 + ch]) < 0.0001f);
        }
    }
}

TEST(am824_real_frames, deserialize_per_channel)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_0);
    auto pdu = am824_parse_header(packet);
    EXPECT_TRUE(pdu.has_value());

    auto const payload = am824_get_audio_payload(packet);

    // Deserialize each channel separately
    std::array<std::array<float, 6>, 8> channels{};

    for (uint8_t ch = 0; ch < 8; ++ch) {
        size_t const samples =
            am824_deserialize_channel(payload, pdu->channel_count(), pdu->sample_count(), ch, std::span{channels[ch]});
        EXPECT_EQ(samples, 6U);
    }

    // Channels 4-7 should be all zeros
    for (uint8_t ch = 4; ch < 8; ++ch) {
        for (size_t s = 0; s < 6; ++s) {
            EXPECT_TRUE(std::fabs(channels[ch][s]) < 0.0001f);
        }
    }
}

TEST(am824_real_frames, deserialize_planar)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_1);
    auto pdu = am824_parse_header(packet);
    EXPECT_TRUE(pdu.has_value());

    auto const payload = am824_get_audio_payload(packet);

    // Allocate planar buffers
    std::array<float, 6> ch0{}, ch1{}, ch2{}, ch3{}, ch4{}, ch5{}, ch6{}, ch7{};
    std::array<float*, 8> channel_ptrs = {
        ch0.data(), ch1.data(), ch2.data(), ch3.data(), ch4.data(), ch5.data(), ch6.data(), ch7.data()};

    size_t const samples = am824_deserialize_planar(payload, 8, 6, std::span{channel_ptrs}, 6);
    EXPECT_EQ(samples, 6U);

    // Channels 4-7 should be silent
    for (size_t s = 0; s < 6; ++s) {
        EXPECT_TRUE(std::fabs(ch4[s]) < 0.0001f);
        EXPECT_TRUE(std::fabs(ch5[s]) < 0.0001f);
        EXPECT_TRUE(std::fabs(ch6[s]) < 0.0001f);
        EXPECT_TRUE(std::fabs(ch7[s]) < 0.0001f);
    }
}

//
// Tests: AM824 Round-trip Serialization with Real Frames
//

TEST(am824_real_frames, round_trip_audio_payload)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_0);
    auto pdu = am824_parse_header(packet);
    EXPECT_TRUE(pdu.has_value());

    auto const payload = am824_get_audio_payload(packet);

    // Deserialize
    std::array<float, 48> audio{};
    size_t const samples = am824_deserialize_interleaved(payload, 8, 6, std::span{audio});
    EXPECT_EQ(samples, 6U);

    // Re-serialize
    std::array<uint8_t, 192> new_payload{};
    size_t const bytes = am824_serialize_interleaved(std::span{audio}, 8, 6, std::span{new_payload});
    EXPECT_EQ(bytes, 192U);

    // Compare payloads - should be identical
    EXPECT_TRUE(span_compare(payload.first(192), make_const_span(new_payload)));
}

TEST(am824_real_frames, round_trip_all_frames)
{
    for (size_t i = 0; i < NUM_TEST_FRAMES; ++i) {
        auto const packet = get_avtp_packet(test_frames[i]);
        auto pdu = am824_parse_header(packet);
        EXPECT_TRUE(pdu.has_value());

        auto const payload = am824_get_audio_payload(packet);

        // Deserialize
        std::array<float, 48> audio{};
        size_t const samples = am824_deserialize_interleaved(payload, 8, 6, std::span{audio});
        EXPECT_EQ(samples, 6U);

        // Re-serialize
        std::array<uint8_t, 192> new_payload{};
        size_t const bytes = am824_serialize_interleaved(std::span{audio}, 8, 6, std::span{new_payload});
        EXPECT_EQ(bytes, 192U);

        // Compare - should be bit-identical
        EXPECT_TRUE(span_compare(payload.first(192), make_const_span(new_payload)));
    }
}

TEST(am824_real_frames, round_trip_planar)
{
    auto const packet = get_avtp_packet(session_1_8ch48kHz_2);
    auto pdu = am824_parse_header(packet);
    EXPECT_TRUE(pdu.has_value());

    auto const payload = am824_get_audio_payload(packet);

    // Deserialize to planar
    std::array<float, 6> ch0{}, ch1{}, ch2{}, ch3{}, ch4{}, ch5{}, ch6{}, ch7{};
    std::array<float*, 8> channel_ptrs = {
        ch0.data(), ch1.data(), ch2.data(), ch3.data(), ch4.data(), ch5.data(), ch6.data(), ch7.data()};
    size_t const samples = am824_deserialize_planar(payload, 8, 6, std::span{channel_ptrs}, 6);
    EXPECT_EQ(samples, 6U);

    // Re-serialize from planar
    std::array<float const*, 8> const_channel_ptrs = {
        ch0.data(), ch1.data(), ch2.data(), ch3.data(), ch4.data(), ch5.data(), ch6.data(), ch7.data()};
    std::array<uint8_t, 192> new_payload{};
    size_t const bytes = am824_serialize_planar(std::span{const_channel_ptrs}, 8, 6, std::span{new_payload});
    EXPECT_EQ(bytes, 192U);

    // Compare - should be bit-identical
    EXPECT_TRUE(span_compare(payload.first(192), make_const_span(new_payload)));
}

TEST(am824_real_frames, recreate_full_packet)
{
    // Parse original packet
    auto const original_packet = get_avtp_packet(session_1_8ch48kHz_1);
    auto original_pdu = am824_parse_header(original_packet);
    EXPECT_TRUE(original_pdu.has_value());

    auto const original_payload = am824_get_audio_payload(original_packet);

    // Deserialize audio
    std::array<float, 48> audio{};
    size_t const deser_samples = am824_deserialize_interleaved(original_payload, 8, 6, std::span{audio});
    EXPECT_EQ(deser_samples, 6U);

    // Create new packet with same parameters
    std::array<uint8_t, 256> new_packet{};
    size_t const bytes = am824_create_packet(
        original_pdu->stream_id(),
        original_pdu->sequence_num(),
        original_pdu->avtp_timestamp(),
        original_pdu->sample_rate(),
        original_pdu->data_block_count(),
        std::span{audio},
        8,
        6,
        std::span{new_packet});

    EXPECT_EQ(bytes, Am824Pdu::HEADER_LENGTH + 192);

    // Parse new packet
    auto new_pdu = am824_parse_header(std::span<uint8_t const>{new_packet.data(), bytes});
    EXPECT_TRUE(new_pdu.has_value());
    EXPECT_TRUE(new_pdu->is_valid());

    // Verify header fields match
    EXPECT_EQ(new_pdu->stream_id(), original_pdu->stream_id());
    EXPECT_EQ(new_pdu->sequence_num(), original_pdu->sequence_num());
    EXPECT_EQ(new_pdu->avtp_timestamp(), original_pdu->avtp_timestamp());
    EXPECT_EQ(new_pdu->channel_count(), original_pdu->channel_count());
    EXPECT_EQ(new_pdu->sample_count(), original_pdu->sample_count());
    EXPECT_EQ(new_pdu->sample_rate(), original_pdu->sample_rate());

    // Verify audio payload matches
    auto const new_payload = am824_get_audio_payload(std::span<uint8_t const>{new_packet.data(), bytes});
    EXPECT_TRUE(span_compare(original_payload.first(192), new_payload.first(192)));
}

//
// Tests: Additional Sample Rate Functions (Coverage)
//

TEST(am824_sample_rate, hz_32khz)
{
    auto const hz = am824_sample_rate_hz(Am824SampleRate::rate_32_khz);
    EXPECT_EQ(hz, 32000U);
}

TEST(am824_sample_rate, hz_44_1khz)
{
    auto const hz = am824_sample_rate_hz(Am824SampleRate::rate_44_1_khz);
    EXPECT_EQ(hz, 44100U);
}

TEST(am824_sample_rate, hz_88_2khz)
{
    auto const hz = am824_sample_rate_hz(Am824SampleRate::rate_88_2_khz);
    EXPECT_EQ(hz, 88200U);
}

TEST(am824_sample_rate, hz_176_4khz)
{
    auto const hz = am824_sample_rate_hz(Am824SampleRate::rate_176_4_khz);
    EXPECT_EQ(hz, 176400U);
}

TEST(am824_sample_rate, hz_invalid)
{
    auto const hz = am824_sample_rate_hz(static_cast<Am824SampleRate>(0xFF));
    EXPECT_EQ(hz, 0U);
}

TEST(am824_sample_rate, syt_interval_32khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_32_khz);
    EXPECT_EQ(interval, 8U);
}

TEST(am824_sample_rate, syt_interval_44_1khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_44_1_khz);
    EXPECT_EQ(interval, 8U);
}

TEST(am824_sample_rate, syt_interval_88_2khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_88_2_khz);
    EXPECT_EQ(interval, 16U);
}

TEST(am824_sample_rate, syt_interval_176_4khz)
{
    auto const interval = am824_syt_interval(Am824SampleRate::rate_176_4_khz);
    EXPECT_EQ(interval, 32U);
}

TEST(am824_sample_rate, syt_interval_invalid)
{
    auto const interval = am824_syt_interval(static_cast<Am824SampleRate>(0xFF));
    EXPECT_EQ(interval, 0U);
}

//
// Tests: AvtpStreamHeader Additional Accessors (Coverage)
//

TEST(avtp_stream_header, set_sv_false)
{
    AvtpStreamHeader header{};
    header.set_sv(true);
    EXPECT_TRUE(header.sv());
    header.set_sv(false);
    EXPECT_FALSE(header.sv());
}

TEST(avtp_stream_header, mr_accessor)
{
    AvtpStreamHeader header{};
    EXPECT_FALSE(header.mr());
    header.set_mr(true);
    EXPECT_TRUE(header.mr());
    header.set_mr(false);
    EXPECT_FALSE(header.mr());
}

TEST(avtp_stream_header, gv_accessor)
{
    AvtpStreamHeader header{};
    EXPECT_FALSE(header.gv());
    // gv doesn't have a setter, only a getter (read from flags)
}

TEST(avtp_stream_header, tv_accessor)
{
    AvtpStreamHeader header{};
    EXPECT_FALSE(header.tv());
    header.set_tv(true);
    EXPECT_TRUE(header.tv());
    header.set_tv(false);
    EXPECT_FALSE(header.tv());
}

TEST(avtp_stream_header, tu_accessor)
{
    AvtpStreamHeader header{};
    EXPECT_FALSE(header.tu());
    // tu is read from reserved_tu byte, bit 0
    // It doesn't have a public setter, but we can verify the getter
}

TEST(avtp_stream_header, tag_accessor)
{
    AvtpStreamHeader header{};
    header.set_protocol_specific(2, 10, 5, 3);  // tag=2, channel=10, tcode=5, sy=3
    EXPECT_EQ(header.tag(), 2U);
}

TEST(avtp_stream_header, channel_accessor)
{
    AvtpStreamHeader header{};
    header.set_protocol_specific(1, 42, 10, 0);  // tag=1, channel=42, tcode=10, sy=0
    EXPECT_EQ(header.channel(), 42U);
}

TEST(avtp_stream_header, tcode_accessor)
{
    AvtpStreamHeader header{};
    header.set_protocol_specific(1, 0, 0x0A, 0);  // tag=1, channel=0, tcode=0x0A, sy=0
    EXPECT_EQ(header.tcode(), 0x0AU);
}

TEST(avtp_stream_header, sy_accessor)
{
    AvtpStreamHeader header{};
    header.set_protocol_specific(0, 0, 0, 0x0F);  // tag=0, channel=0, tcode=0, sy=0x0F
    EXPECT_EQ(header.sy(), 0x0FU);
}

//
// Tests: Cip61883Header Additional Accessors (Coverage)
//

TEST(cip_header, qi1_accessor)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);
    // QI_1 should be 0b00 for CIP
    EXPECT_EQ(header.qi1(), 0U);
}

TEST(cip_header, sid_accessor)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);
    EXPECT_EQ(header.sid(), 0U);  // Default SID is 0
    header.set_sid(0x3F);
    EXPECT_EQ(header.sid(), 0x3FU);
}

TEST(cip_header, fn_accessor)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);
    // FN should be 0 after init
    EXPECT_EQ(header.fn(), 0U);
}

TEST(cip_header, qpc_accessor)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);
    // QPC should be 0 after init
    EXPECT_EQ(header.qpc(), 0U);
}

TEST(cip_header, sph_accessor)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);
    // SPH should be 0 after init (no source packet header)
    EXPECT_FALSE(header.sph());
}

TEST(cip_header, qi2_accessor)
{
    Cip61883Header header{};
    header.init_am824(8, Am824SampleRate::rate_48_khz);
    // QI_2 should be 0b10 for CIP
    EXPECT_EQ(header.qi2(), 2U);
}

TEST(cip_header, set_fmt)
{
    Cip61883Header header{};
    header.set_fmt(AM824_FMT);
    EXPECT_EQ(header.fmt(), AM824_FMT);
    // QI_2 should still be 0b10 after set_fmt
    EXPECT_EQ(header.qi2(), 2U);
}

TEST(cip_header, set_format_dependent_field)
{
    Cip61883Header header{};
    header.set_format_dependent_field(0x05);  // 176.4 kHz
    EXPECT_EQ(header.format_dependent_field(), 0x05U);
}

TEST(cip_header, set_sample_rate)
{
    Cip61883Header header{};
    header.set_sample_rate(Am824SampleRate::rate_96_khz);
    EXPECT_EQ(header.sample_rate(), Am824SampleRate::rate_96_khz);
}

//
// Tests: Am824Pdu Additional Accessors (Coverage)
//

TEST(am824_pdu, set_stream_id)
{
    Am824Pdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x1234};

    pdu.set_stream_id(sid);
    EXPECT_EQ(pdu.stream_id(), sid);
}

TEST(am824_pdu, set_sample_rate)
{
    Am824Pdu pdu{};
    pdu.set_sample_rate(Am824SampleRate::rate_192_khz);
    EXPECT_EQ(pdu.sample_rate(), Am824SampleRate::rate_192_khz);
}

TEST(am824_pdu, increment_data_block_count)
{
    Am824Pdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, 8, Am824SampleRate::rate_48_khz);
    pdu.set_data_block_count(250);
    pdu.increment_data_block_count(10);
    EXPECT_EQ(pdu.data_block_count(), 4U);  // (250 + 10) & 0xFF = 4 (wraps)
}

TEST(am824_pdu, syt_timestamp_accessor)
{
    Am824Pdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, 8, Am824SampleRate::rate_48_khz);
    // After init, SYT should be 0xFFFF (no timestamp)
    EXPECT_EQ(pdu.syt_timestamp(), 0xFFFFU);

    pdu.set_syt_timestamp(0x1234);
    EXPECT_EQ(pdu.syt_timestamp(), 0x1234U);
}

TEST(am824_pdu, tv_accessor)
{
    Am824Pdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, 8, Am824SampleRate::rate_48_khz);
    // After init, tv should be false
    EXPECT_FALSE(pdu.tv());

    pdu.set_tv(true);
    EXPECT_TRUE(pdu.tv());

    pdu.set_tv(false);
    EXPECT_FALSE(pdu.tv());
}

//
// Tests: Runtime coverage for constexpr utility functions
//

TEST(am824_rt_coverage, sample_rate_hz_all_rates)
{
    auto volatile r32 = Am824SampleRate::rate_32_khz;
    auto volatile r44 = Am824SampleRate::rate_44_1_khz;
    auto volatile r48 = Am824SampleRate::rate_48_khz;
    auto volatile r88 = Am824SampleRate::rate_88_2_khz;
    auto volatile r96 = Am824SampleRate::rate_96_khz;
    auto volatile r176 = Am824SampleRate::rate_176_4_khz;
    auto volatile r192 = Am824SampleRate::rate_192_khz;

    EXPECT_EQ(am824_sample_rate_hz(r32), 32000U);
    EXPECT_EQ(am824_sample_rate_hz(r44), 44100U);
    EXPECT_EQ(am824_sample_rate_hz(r48), 48000U);
    EXPECT_EQ(am824_sample_rate_hz(r88), 88200U);
    EXPECT_EQ(am824_sample_rate_hz(r96), 96000U);
    EXPECT_EQ(am824_sample_rate_hz(r176), 176400U);
    EXPECT_EQ(am824_sample_rate_hz(r192), 192000U);
}

TEST(am824_rt_coverage, syt_interval_all_rates)
{
    auto volatile r32 = Am824SampleRate::rate_32_khz;
    auto volatile r44 = Am824SampleRate::rate_44_1_khz;
    auto volatile r48 = Am824SampleRate::rate_48_khz;
    auto volatile r88 = Am824SampleRate::rate_88_2_khz;
    auto volatile r96 = Am824SampleRate::rate_96_khz;
    auto volatile r176 = Am824SampleRate::rate_176_4_khz;
    auto volatile r192 = Am824SampleRate::rate_192_khz;

    EXPECT_EQ(am824_syt_interval(r32), 8U);
    EXPECT_EQ(am824_syt_interval(r44), 8U);
    EXPECT_EQ(am824_syt_interval(r48), 8U);
    EXPECT_EQ(am824_syt_interval(r88), 16U);
    EXPECT_EQ(am824_syt_interval(r96), 16U);
    EXPECT_EQ(am824_syt_interval(r176), 32U);
    EXPECT_EQ(am824_syt_interval(r192), 32U);
}

// avtp#2: the payload accessor must honor the declared stream_data_length, not the
// raw buffer extent -- a wire packet is padded to the 60-byte Ethernet minimum and
// that padding must not be returned as audio.
TEST(am824_pdu, get_audio_payload_ignores_trailing_padding)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};
    std::array<float, 4> audio = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 64> packet{};
    size_t const bytes = am824_create_packet(sid, 0, 0, Am824SampleRate::rate_48_khz, 0, std::span{audio}, 2, 2, std::span{packet});
    EXPECT_EQ(bytes, 48U);  // 32 header + 16 audio

    // Pass the WHOLE 64-byte buffer (16 bytes of trailing padding) rather than `bytes`.
    auto const payload = am824_get_audio_payload(std::span<uint8_t const>{packet});
    EXPECT_EQ(payload.size(), 16U);  // declared audio length, not 64 - 32 = 32
}

// avtp#3: the FDF's SFC (sample-frequency code) is only the low 3 bits; the upper
// bits select the rate-control mode. is_valid() must accept command-based mode
// (FDF = 0000 1xxx) and reject only a reserved SFC.
TEST(am824_pdu, is_valid_accepts_command_based_fdf)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};
    Am824Pdu pdu{};
    pdu.init(sid, 2, Am824SampleRate::rate_48_khz);  // clock-based, SFC = 0x02
    EXPECT_TRUE(pdu.is_valid());

    // Command-based rate-control mode: FDF = 0000 1xxx (bit 3 set), SFC unchanged.
    pdu.cip_header.set_format_dependent_field(0x08U | 0x02U);  // 0x0A
    EXPECT_TRUE(pdu.is_valid());
    EXPECT_EQ(pdu.cip_header.sample_rate(), Am824SampleRate::rate_48_khz);

    // A reserved SFC (low 3 bits == 0x07) is still rejected, whatever the upper bits.
    pdu.cip_header.set_format_dependent_field(0x0FU);
    EXPECT_FALSE(pdu.is_valid());
}

// avtp#SPH/FN: SPH (source packet header) and FN != 0 (fragmentation) change the CIP
// payload layout, which the flat sample model here cannot parse -- is_valid() must
// reject them rather than misinterpret the payload.
TEST(am824_pdu, is_valid_rejects_sph_and_fragmented)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};
    Am824Pdu pdu{};
    pdu.init(sid, 2, Am824SampleRate::rate_48_khz);
    EXPECT_TRUE(pdu.is_valid());

    pdu.cip_header.fn_qpc_sph = 0x04U;  // SPH = 1
    EXPECT_FALSE(pdu.is_valid());

    pdu.cip_header.fn_qpc_sph = 0x40U;  // FN = 01 (fragmented)
    EXPECT_FALSE(pdu.is_valid());
}

//
// Main test runner
//

TEST_MAIN(statusbar_avtp, avtp_am824_test)