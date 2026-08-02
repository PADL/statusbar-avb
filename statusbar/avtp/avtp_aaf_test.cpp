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

// AAF format names - tested at runtime (non-constexpr)

// AAF bytes per sample
static_assert(aaf_bytes_per_sample(AafFormat::float_32bit) == 4);
static_assert(aaf_bytes_per_sample(AafFormat::int_32bit) == 4);
static_assert(aaf_bytes_per_sample(AafFormat::int_24bit) == 3);
static_assert(aaf_bytes_per_sample(AafFormat::int_16bit) == 2);
static_assert(aaf_bytes_per_sample(AafFormat::user_specified) == 0);

// AAF default bit depth
static_assert(aaf_default_bit_depth(AafFormat::float_32bit) == 32);
static_assert(aaf_default_bit_depth(AafFormat::int_32bit) == 32);
static_assert(aaf_default_bit_depth(AafFormat::int_24bit) == 24);
static_assert(aaf_default_bit_depth(AafFormat::int_16bit) == 16);
static_assert(aaf_default_bit_depth(AafFormat::user_specified) == 0);

// AAF sample rate names - tested at runtime (non-constexpr)

// AAF sample rate Hz values
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_8_khz) == 8000);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_16_khz) == 16000);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_32_khz) == 32000);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_44_1_khz) == 44100);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_48_khz) == 48000);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_88_2_khz) == 88200);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_96_khz) == 96000);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_176_4_khz) == 176400);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_192_khz) == 192000);
static_assert(aaf_sample_rate_hz(AafSampleRate::rate_24_khz) == 24000);
static_assert(aaf_sample_rate_hz(AafSampleRate::user_specified) == 0);

// AAF payload size calculation
static_assert(aaf_payload_size(AafFormat::int_24bit, 8, 6) == 144);
static_assert(aaf_payload_size(AafFormat::int_16bit, 2, 48) == 192);

// AAF packet size calculation (header + payload)
static_assert(aaf_packet_size(AafFormat::int_24bit, 8, 6) == 168);   // 24 + 144
static_assert(aaf_packet_size(AafFormat::int_16bit, 2, 48) == 216);  // 24 + 192

//
// Name function tests (runtime, non-constexpr)
//

TEST(aaf_names, format_name)
{
    EXPECT_EQ(aaf_format_name(AafFormat::user_specified)[0], 'U');
    EXPECT_EQ(aaf_format_name(AafFormat::float_32bit)[0], '3');
    EXPECT_EQ(aaf_format_name(AafFormat::int_32bit)[0], '3');
    EXPECT_EQ(aaf_format_name(AafFormat::int_24bit)[0], '2');
    EXPECT_EQ(aaf_format_name(AafFormat::int_16bit)[0], '1');
    EXPECT_EQ(aaf_format_name(AafFormat::aes3_32bit)[0], 'A');
}

TEST(aaf_names, sample_rate_name)
{
    EXPECT_EQ(aaf_sample_rate_name(AafSampleRate::user_specified)[0], 'U');
    EXPECT_EQ(aaf_sample_rate_name(AafSampleRate::rate_8_khz)[0], '8');
    EXPECT_EQ(aaf_sample_rate_name(AafSampleRate::rate_48_khz)[0], '4');
    EXPECT_EQ(aaf_sample_rate_name(AafSampleRate::rate_96_khz)[0], '9');
}

//
// Tests: AAF Format Enum
//

TEST(aaf_format, name_float_32bit)
{
    auto const name = aaf_format_name(AafFormat::float_32bit);
    EXPECT_EQ(name, "32-bit float");
}

TEST(aaf_format, name_int_32bit)
{
    auto const name = aaf_format_name(AafFormat::int_32bit);
    EXPECT_EQ(name, "32-bit integer");
}

TEST(aaf_format, name_int_24bit)
{
    auto const name = aaf_format_name(AafFormat::int_24bit);
    EXPECT_EQ(name, "24-bit integer");
}

TEST(aaf_format, name_int_16bit)
{
    auto const name = aaf_format_name(AafFormat::int_16bit);
    EXPECT_EQ(name, "16-bit integer");
}

TEST(aaf_format, bytes_per_sample_16bit)
{
    EXPECT_EQ(aaf_bytes_per_sample(AafFormat::int_16bit), 2U);
}

TEST(aaf_format, bytes_per_sample_24bit)
{
    EXPECT_EQ(aaf_bytes_per_sample(AafFormat::int_24bit), 3U);
}

TEST(aaf_format, bytes_per_sample_32bit)
{
    EXPECT_EQ(aaf_bytes_per_sample(AafFormat::int_32bit), 4U);
}

TEST(aaf_format, bytes_per_sample_float)
{
    EXPECT_EQ(aaf_bytes_per_sample(AafFormat::float_32bit), 4U);
}

TEST(aaf_format, default_bit_depth)
{
    EXPECT_EQ(aaf_default_bit_depth(AafFormat::int_16bit), 16U);
    EXPECT_EQ(aaf_default_bit_depth(AafFormat::int_24bit), 24U);
    EXPECT_EQ(aaf_default_bit_depth(AafFormat::int_32bit), 32U);
    EXPECT_EQ(aaf_default_bit_depth(AafFormat::float_32bit), 32U);
}

//
// Tests: AAF Sample Rate Enum
//

TEST(aaf_sample_rate, name_48khz)
{
    auto const name = aaf_sample_rate_name(AafSampleRate::rate_48_khz);
    EXPECT_EQ(name, "48 kHz");
}

TEST(aaf_sample_rate, hz_48khz)
{
    auto const hz = aaf_sample_rate_hz(AafSampleRate::rate_48_khz);
    EXPECT_EQ(hz, 48000U);
}

TEST(aaf_sample_rate, hz_8khz)
{
    auto const hz = aaf_sample_rate_hz(AafSampleRate::rate_8_khz);
    EXPECT_EQ(hz, 8000U);
}

TEST(aaf_sample_rate, hz_96khz)
{
    auto const hz = aaf_sample_rate_hz(AafSampleRate::rate_96_khz);
    EXPECT_EQ(hz, 96000U);
}

TEST(aaf_sample_rate, hz_192khz)
{
    auto const hz = aaf_sample_rate_hz(AafSampleRate::rate_192_khz);
    EXPECT_EQ(hz, 192000U);
}

TEST(aaf_sample_rate, hz_24khz)
{
    auto const hz = aaf_sample_rate_hz(AafSampleRate::rate_24_khz);
    EXPECT_EQ(hz, 24000U);
}

//
// Tests: AAF Sample Conversion - 16-bit
//

TEST(aaf_sample_convert_16, zero_to_float)
{
    int16_t const sample = 0;
    float const result = aaf_int16_to_float(sample);
    EXPECT_TRUE(std::fabs(result) < 0.00001f);
}

TEST(aaf_sample_convert_16, max_positive_to_float)
{
    int16_t const sample = 32767;
    float const result = aaf_int16_to_float(sample);
    EXPECT_TRUE(std::fabs(result - 0.999969f) < 0.0001f);  // 32767/32768
}

TEST(aaf_sample_convert_16, max_negative_to_float)
{
    int16_t const sample = -32768;
    float const result = aaf_int16_to_float(sample);
    EXPECT_TRUE(std::fabs(result - (-1.0f)) < 0.0001f);
}

TEST(aaf_sample_convert_16, float_to_sample_clamp_pos)
{
    float const input = 1.5f;
    int16_t const result = float_to_aaf_int16(input);
    EXPECT_EQ(result, 32767);
}

TEST(aaf_sample_convert_16, float_to_sample_clamp_neg)
{
    float const input = -1.5f;
    int16_t const result = float_to_aaf_int16(input);
    EXPECT_EQ(result, -32768);
}

TEST(aaf_sample_convert_16, round_trip)
{
    int16_t const original = 12345;
    float const f = aaf_int16_to_float(original);
    int16_t const restored = float_to_aaf_int16(f);
    EXPECT_TRUE(std::abs(original - restored) <= 1);
}

//
// Tests: AAF Sample Conversion - 24-bit
//

TEST(aaf_sample_convert_24, zero_to_float)
{
    int32_t const sample = 0;
    float const result = aaf_int24_to_float(sample);
    EXPECT_TRUE(std::fabs(result) < 0.00001f);
}

TEST(aaf_sample_convert_24, max_positive_to_float)
{
    int32_t const sample = 8388607;  // Max 24-bit positive
    float const result = aaf_int24_to_float(sample);
    EXPECT_TRUE(std::fabs(result - 1.0f) < 0.0001f);
}

TEST(aaf_sample_convert_24, max_negative_to_float)
{
    int32_t const sample = -8388608;  // Min 24-bit negative
    float const result = aaf_int24_to_float(sample);
    EXPECT_TRUE(std::fabs(result - (-1.0f)) < 0.0001f);
}

TEST(aaf_sample_convert_24, float_to_sample_clamp_pos)
{
    float const input = 1.5f;
    int32_t const result = float_to_aaf_int24(input);
    EXPECT_EQ(result, 8388607);
}

TEST(aaf_sample_convert_24, float_to_sample_clamp_neg)
{
    float const input = -1.5f;
    int32_t const result = float_to_aaf_int24(input);
    EXPECT_EQ(result, -8388608);
}

TEST(aaf_sample_convert_24, round_trip)
{
    int32_t const original = 1234567;
    float const f = aaf_int24_to_float(original);
    int32_t const restored = float_to_aaf_int24(f);
    EXPECT_TRUE(std::abs(original - restored) <= 1);
}

//
// Tests: AAF Sample Conversion - 32-bit
//

TEST(aaf_sample_convert_32, zero_to_float)
{
    int32_t const sample = 0;
    float const result = aaf_int32_to_float(sample);
    EXPECT_TRUE(std::fabs(result) < 0.00001f);
}

TEST(aaf_sample_convert_32, max_positive_to_float)
{
    int32_t const sample = 2147483647;  // Max 32-bit positive
    float const result = aaf_int32_to_float(sample);
    EXPECT_TRUE(std::fabs(result - 1.0f) < 0.0001f);
}

TEST(aaf_sample_convert_32, max_negative_to_float)
{
    int32_t const sample = -2147483647 - 1;  // Min 32-bit negative
    float const result = aaf_int32_to_float(sample);
    EXPECT_TRUE(std::fabs(result - (-1.0f)) < 0.0001f);
}

TEST(aaf_sample_convert_32, float_to_sample_clamp_pos)
{
    float const input = 1.5f;
    int32_t const result = float_to_aaf_int32(input);
    EXPECT_EQ(result, 2147483647);
}

TEST(aaf_sample_convert_32, float_to_sample_clamp_neg)
{
    float const input = -1.5f;
    int32_t const result = float_to_aaf_int32(input);
    EXPECT_EQ(result, -2147483647 - 1);
}

TEST(aaf_sample_convert_32, round_trip)
{
    // Note: 32-bit integers have more precision than float can represent
    // Use a value that fits in float precision
    int32_t const original = 12345678;
    float const f = aaf_int32_to_float(original);
    int32_t const restored = float_to_aaf_int32(f);
    // Allow more error due to float precision limits
    EXPECT_TRUE(std::abs(original - restored) < 256);
}

//
// Tests: AafPdu Structure
//

TEST(aaf_pdu, size_is_24_bytes)
{
    EXPECT_EQ(sizeof(AafPdu), 24U);
    EXPECT_EQ(AafPdu::HEADER_LENGTH, 24U);
}

TEST(aaf_pdu, default_constructor)
{
    AafPdu pdu{};
    EXPECT_EQ(pdu.subtype.get(), 0U);
    EXPECT_FALSE(pdu.sv());
}

TEST(aaf_pdu, init_creates_valid_packet)
{
    AafPdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 8, 24);

    EXPECT_TRUE(pdu.is_valid());
    EXPECT_EQ(pdu.channels_per_frame(), 8U);
    EXPECT_EQ(pdu.get_format(), AafFormat::int_24bit);
    EXPECT_EQ(pdu.nsr(), AafSampleRate::rate_48_khz);
    EXPECT_EQ(pdu.get_bit_depth(), 24U);
}

TEST(aaf_pdu, sv_flag)
{
    AafPdu pdu{};
    EXPECT_FALSE(pdu.sv());
    pdu.set_sv(true);
    EXPECT_TRUE(pdu.sv());
    pdu.set_sv(false);
    EXPECT_FALSE(pdu.sv());
}

TEST(aaf_pdu, tv_flag)
{
    AafPdu pdu{};
    EXPECT_FALSE(pdu.tv());
    pdu.set_tv(true);
    EXPECT_TRUE(pdu.tv());
    pdu.set_tv(false);
    EXPECT_FALSE(pdu.tv());
}

TEST(aaf_pdu, mr_flag)
{
    AafPdu pdu{};
    EXPECT_FALSE(pdu.mr());
    pdu.set_mr(true);
    EXPECT_TRUE(pdu.mr());
    pdu.set_mr(false);
    EXPECT_FALSE(pdu.mr());
}

TEST(aaf_pdu, tu_flag)
{
    AafPdu pdu{};
    EXPECT_FALSE(pdu.tu());
    pdu.set_tu(true);
    EXPECT_TRUE(pdu.tu());
    pdu.set_tu(false);
    EXPECT_FALSE(pdu.tu());
}

TEST(aaf_pdu, sp_flag)
{
    AafPdu pdu{};
    EXPECT_FALSE(pdu.sp());
    pdu.set_sp(true);
    EXPECT_TRUE(pdu.sp());
    pdu.set_sp(false);
    EXPECT_FALSE(pdu.sp());
}

TEST(aaf_pdu, evt_field)
{
    AafPdu pdu{};
    EXPECT_EQ(pdu.evt(), 0U);
    pdu.set_evt(0x0F);
    EXPECT_EQ(pdu.evt(), 0x0FU);
    pdu.set_evt(0x05);
    EXPECT_EQ(pdu.evt(), 0x05U);
}

TEST(aaf_pdu, channels_per_frame)
{
    AafPdu pdu{};
    pdu.set_channels_per_frame(8);
    EXPECT_EQ(pdu.channels_per_frame(), 8U);

    pdu.set_channels_per_frame(256);
    EXPECT_EQ(pdu.channels_per_frame(), 256U);

    // Max value (10 bits = 1023)
    pdu.set_channels_per_frame(1023);
    EXPECT_EQ(pdu.channels_per_frame(), 1023U);

    // Overflow should clamp
    pdu.set_channels_per_frame(2000);
    EXPECT_EQ(pdu.channels_per_frame(), 2000U & 0x3FFU);
}

TEST(aaf_pdu, sample_rate_preserves_channels)
{
    AafPdu pdu{};
    pdu.set_channels_per_frame(512);
    pdu.set_nsr(AafSampleRate::rate_96_khz);

    EXPECT_EQ(pdu.nsr(), AafSampleRate::rate_96_khz);
    EXPECT_EQ(pdu.channels_per_frame(), 512U);
}

TEST(aaf_pdu, sequence_num_wraps)
{
    AafPdu pdu{};
    pdu.set_sequence_num(255U);
    pdu.increment_sequence_num();
    EXPECT_EQ(pdu.get_sequence_num(), 0U);
}

TEST(aaf_pdu, set_dimensions)
{
    AafPdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24);
    pdu.set_dimensions(6, 2);  // 6 samples, 2 channels

    EXPECT_EQ(pdu.sample_count(), 6U);
    EXPECT_EQ(pdu.channels_per_frame(), 2U);
    // stream_data_length = 6 * 2 * 3 bytes = 36
    EXPECT_EQ(pdu.get_stream_data_length(), 36U);
}

TEST(aaf_pdu, set_dimensions_16bit)
{
    AafPdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, AafFormat::int_16bit, AafSampleRate::rate_48_khz, 8, 16);
    pdu.set_dimensions(6, 8);  // 6 samples, 8 channels

    EXPECT_EQ(pdu.sample_count(), 6U);
    EXPECT_EQ(pdu.channels_per_frame(), 8U);
    // stream_data_length = 6 * 8 * 2 bytes = 96
    EXPECT_EQ(pdu.get_stream_data_length(), 96U);
}

TEST(aaf_pdu, set_dimensions_32bit)
{
    AafPdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, AafFormat::int_32bit, AafSampleRate::rate_48_khz, 2, 32);
    pdu.set_dimensions(6, 2);  // 6 samples, 2 channels

    EXPECT_EQ(pdu.sample_count(), 6U);
    EXPECT_EQ(pdu.channels_per_frame(), 2U);
    // stream_data_length = 6 * 2 * 4 bytes = 48
    EXPECT_EQ(pdu.get_stream_data_length(), 48U);
}

TEST(aaf_pdu, invalid_without_sv)
{
    AafPdu pdu{};
    pdu.subtype = 0x02U;  // AAF subtype
    pdu.set_format(AafFormat::int_24bit);
    pdu.set_channels_per_frame(2);
    pdu.set_bit_depth(24);
    // sv is not set
    EXPECT_FALSE(pdu.is_valid());
}

TEST(aaf_pdu, invalid_wrong_subtype)
{
    AafPdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24);
    pdu.subtype = 0x00U;  // Wrong subtype
    EXPECT_FALSE(pdu.is_valid());
}

TEST(aaf_pdu, invalid_float_wrong_bitdepth)
{
    AafPdu pdu{};
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    pdu.init(sid, AafFormat::float_32bit, AafSampleRate::rate_48_khz, 2, 24);  // Wrong bit_depth for float
    EXPECT_FALSE(pdu.is_valid());

    pdu.set_bit_depth(32);
    EXPECT_TRUE(pdu.is_valid());
}

//
// Tests: AAF Serialization/Deserialization - 16-bit
//

TEST(aaf_serialize_16, interleaved_simple)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 8> payload{};

    size_t const bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_16bit, 2, 2, std::span{payload});

    EXPECT_EQ(bytes, 8U);  // 2 * 2 * 2 bytes
}

TEST(aaf_serialize_16, round_trip)
{
    std::array<float, 8> original = {0.5f, -0.5f, 0.25f, -0.25f, 0.1f, -0.1f, 0.0f, 0.75f};
    std::array<uint8_t, 16> payload{};
    std::array<float, 8> restored{};

    size_t const bytes = aaf_serialize_interleaved(std::span{original}, AafFormat::int_16bit, 2, 4, std::span{payload});
    EXPECT_EQ(bytes, 16U);

    size_t const samples =
        aaf_deserialize_interleaved(std::span<uint8_t const>{payload}, AafFormat::int_16bit, 2, 4, std::span{restored});
    EXPECT_EQ(samples, 4U);

    for (size_t i = 0; i < 8; ++i) {
        float const diff = std::fabs(original[i] - restored[i]);
        EXPECT_TRUE(diff < 0.001f);  // 16-bit has lower precision
    }
}

//
// Tests: AAF Serialization/Deserialization - 24-bit
//

TEST(aaf_serialize_24, interleaved_simple)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 12> payload{};

    size_t const bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_24bit, 2, 2, std::span{payload});

    EXPECT_EQ(bytes, 12U);  // 2 * 2 * 3 bytes
}

TEST(aaf_serialize_24, round_trip)
{
    std::array<float, 8> original = {0.5f, -0.5f, 0.25f, -0.25f, 0.1f, -0.1f, 0.0f, 0.75f};
    std::array<uint8_t, 24> payload{};
    std::array<float, 8> restored{};

    size_t const bytes = aaf_serialize_interleaved(std::span{original}, AafFormat::int_24bit, 2, 4, std::span{payload});
    EXPECT_EQ(bytes, 24U);

    size_t const samples =
        aaf_deserialize_interleaved(std::span<uint8_t const>{payload}, AafFormat::int_24bit, 2, 4, std::span{restored});
    EXPECT_EQ(samples, 4U);

    for (size_t i = 0; i < 8; ++i) {
        float const diff = std::fabs(original[i] - restored[i]);
        EXPECT_TRUE(diff < 0.0001f);
    }
}

//
// Tests: AAF Serialization/Deserialization - 32-bit integer
//

TEST(aaf_serialize_32, interleaved_simple)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 16> payload{};

    size_t const bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_32bit, 2, 2, std::span{payload});

    EXPECT_EQ(bytes, 16U);  // 2 * 2 * 4 bytes
}

TEST(aaf_serialize_32, round_trip)
{
    std::array<float, 8> original = {0.5f, -0.5f, 0.25f, -0.25f, 0.1f, -0.1f, 0.0f, 0.75f};
    std::array<uint8_t, 32> payload{};
    std::array<float, 8> restored{};

    size_t const bytes = aaf_serialize_interleaved(std::span{original}, AafFormat::int_32bit, 2, 4, std::span{payload});
    EXPECT_EQ(bytes, 32U);

    size_t const samples =
        aaf_deserialize_interleaved(std::span<uint8_t const>{payload}, AafFormat::int_32bit, 2, 4, std::span{restored});
    EXPECT_EQ(samples, 4U);

    for (size_t i = 0; i < 8; ++i) {
        float const diff = std::fabs(original[i] - restored[i]);
        EXPECT_TRUE(diff < 0.0001f);
    }
}

//
// Tests: AAF Serialization/Deserialization - 32-bit float
//

TEST(aaf_serialize_float, interleaved_simple)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 16> payload{};

    size_t const bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::float_32bit, 2, 2, std::span{payload});

    EXPECT_EQ(bytes, 16U);  // 2 * 2 * 4 bytes
}

TEST(aaf_serialize_float, round_trip_exact)
{
    // Float format should be bit-exact round trip
    std::array<float, 8> original = {0.5f, -0.5f, 0.25f, -0.25f, 0.1f, -0.1f, 0.0f, 0.75f};
    std::array<uint8_t, 32> payload{};
    std::array<float, 8> restored{};

    size_t const bytes = aaf_serialize_interleaved(std::span{original}, AafFormat::float_32bit, 2, 4, std::span{payload});
    EXPECT_EQ(bytes, 32U);

    size_t const samples =
        aaf_deserialize_interleaved(std::span<uint8_t const>{payload}, AafFormat::float_32bit, 2, 4, std::span{restored});
    EXPECT_EQ(samples, 4U);

    // Floats should be bit-exact
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(original[i], restored[i]);
    }
}

//
// Tests: AAF Planar Serialization/Deserialization
//

TEST(aaf_planar, serialize_deserialize_round_trip)
{
    // Planar input: 2 channels, 4 samples each
    std::array<float, 4> ch0_in = {0.1f, 0.2f, 0.3f, 0.4f};
    std::array<float, 4> ch1_in = {-0.1f, -0.2f, -0.3f, -0.4f};
    std::array<float const*, 2> in_ptrs = {ch0_in.data(), ch1_in.data()};

    std::array<uint8_t, 24> payload{};  // 2 * 4 * 3 bytes for 24-bit

    size_t const bytes = aaf_serialize_planar(std::span{in_ptrs}, AafFormat::int_24bit, 2, 4, std::span{payload});
    EXPECT_EQ(bytes, 24U);

    // Planar output
    std::array<float, 4> ch0_out{}, ch1_out{};
    std::array<float*, 2> out_ptrs = {ch0_out.data(), ch1_out.data()};

    size_t const samples =
        aaf_deserialize_planar(std::span<uint8_t const>{payload}, AafFormat::int_24bit, 2, 4, std::span{out_ptrs}, 4);
    EXPECT_EQ(samples, 4U);

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(std::fabs(ch0_in[i] - ch0_out[i]) < 0.0001f);
        EXPECT_TRUE(std::fabs(ch1_in[i] - ch1_out[i]) < 0.0001f);
    }
}

//
// Tests: AAF Channel Extraction
//

TEST(aaf_channel, extract_single_channel)
{
    // Create interleaved data: 3 channels, 4 samples
    std::array<float, 12> input{};
    for (size_t s = 0; s < 4; ++s) {
        for (size_t ch = 0; ch < 3; ++ch) {
            input[s * 3 + ch] = static_cast<float>(ch) * 0.25f + static_cast<float>(s) * 0.01f;
        }
    }

    std::array<uint8_t, 36> payload{};  // 3 * 4 * 3 bytes
    size_t const bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_24bit, 3, 4, std::span{payload});
    EXPECT_EQ(bytes, 36U);

    // Extract channel 1
    std::array<float, 4> ch1_out{};
    size_t const samples =
        aaf_deserialize_channel(std::span<uint8_t const>{payload}, AafFormat::int_24bit, 3, 4, 1, std::span{ch1_out});
    EXPECT_EQ(samples, 4U);

    // Verify channel 1 values
    for (size_t s = 0; s < 4; ++s) {
        float const expected = 1.0f * 0.25f + static_cast<float>(s) * 0.01f;
        EXPECT_TRUE(std::fabs(ch1_out[s] - expected) < 0.0001f);
    }
}

//
// Tests: AAF Packet Creation and Parsing
//

TEST(aaf_packet, payload_size_calculation_24bit)
{
    size_t const size = aaf_payload_size(AafFormat::int_24bit, 8, 6);  // 8 channels, 6 samples
    // payload only: 8 * 6 * 3 = 144
    EXPECT_EQ(size, 144U);
}

TEST(aaf_packet, payload_size_calculation_16bit)
{
    size_t const size = aaf_payload_size(AafFormat::int_16bit, 2, 6);  // 2 channels, 6 samples
    // payload only: 2 * 6 * 2 = 24
    EXPECT_EQ(size, 24U);
}

TEST(aaf_packet, size_calculation_24bit)
{
    size_t const size = aaf_packet_size(AafFormat::int_24bit, 8, 6);  // 8 channels, 6 samples
    // Header (24) + payload (8 * 6 * 3)
    EXPECT_EQ(size, 24U + 144U);
}

TEST(aaf_packet, size_calculation_16bit)
{
    size_t const size = aaf_packet_size(AafFormat::int_16bit, 2, 6);  // 2 channels, 6 samples
    // Header (24) + payload (2 * 6 * 2)
    EXPECT_EQ(size, 24U + 24U);
}

TEST(aaf_packet, create_and_parse)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    std::array<float, 8> audio{};  // 2 channels, 4 samples
    for (size_t i = 0; i < 8; ++i) {
        audio[i] = static_cast<float>(i) / 16.0f;
    }

    std::array<uint8_t, 64> packet{};

    // Create stream context with 24-bit format
    AafStreamContext ctx{
        .stream_id = sid,
        .sequence_num = 5,
        .format = AafFormat::int_24bit,
        .sample_rate = AafSampleRate::rate_48_khz,
        .bit_depth = 24,
        .channel_count = 2,
        .sample_count = 4,
    };

    size_t const bytes = aaf_create_packet(ctx, 0x12345678, std::span{audio}, std::span{packet});

    EXPECT_EQ(bytes, 24U + 24U);  // Header + 2*4*3 audio

    // Parse header
    auto pdu = aaf_parse_header(std::span<uint8_t const>{packet.data(), bytes});
    EXPECT_TRUE(pdu.has_value());
    EXPECT_TRUE(pdu->is_valid());
    EXPECT_EQ(pdu->get_sequence_num(), 5U);
    EXPECT_EQ(pdu->get_avtp_timestamp(), 0x12345678U);
    EXPECT_EQ(pdu->channels_per_frame(), 2U);
    EXPECT_EQ(pdu->sample_count(), 4U);
    EXPECT_EQ(pdu->get_format(), AafFormat::int_24bit);
    EXPECT_EQ(pdu->nsr(), AafSampleRate::rate_48_khz);
    EXPECT_TRUE(pdu->sp());  // sparse_timestamp is always true now

    // Verify sequence number was auto-incremented
    EXPECT_EQ(ctx.sequence_num, 6U);

    // Verify stream_id accessor
    StreamId parsed_sid = pdu->stream_id();
    EXPECT_TRUE(parsed_sid == sid);
}

TEST(aaf_packet, create_float_format)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    std::array<float, 4> audio = {0.5f, -0.5f, 0.25f, -0.25f};
    std::array<uint8_t, 48> packet{};

    AafStreamContext ctx{
        .stream_id = sid,
        .sequence_num = 0,
        .format = AafFormat::float_32bit,
        .sample_rate = AafSampleRate::rate_96_khz,
        .bit_depth = 32,
        .channel_count = 2,
        .sample_count = 2,
    };

    size_t const bytes = aaf_create_packet(ctx, 0x11111111, std::span{audio}, std::span{packet});

    auto pdu = aaf_parse_header(std::span<uint8_t const>{packet.data(), bytes});
    EXPECT_TRUE(pdu.has_value());
    EXPECT_TRUE(pdu->sp());  // sparse_timestamp is always true
    EXPECT_EQ(pdu->get_format(), AafFormat::float_32bit);
    EXPECT_EQ(pdu->nsr(), AafSampleRate::rate_96_khz);
}

TEST(aaf_packet, parse_invalid_returns_null)
{
    std::array<uint8_t, 24> packet{};  // All zeros
    auto pdu = aaf_parse_header(std::span<uint8_t const>{packet});
    EXPECT_FALSE(pdu.has_value());  // Invalid packet
}

TEST(aaf_packet, get_audio_payload)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    std::array<float, 4> audio = {0.5f, -0.5f, 0.25f, -0.25f};  // 2 channels, 2 samples
    std::array<uint8_t, 48> packet{};

    AafStreamContext ctx{
        .stream_id = sid,
        .sequence_num = 0,
        .format = AafFormat::int_24bit,
        .sample_rate = AafSampleRate::rate_48_khz,
        .bit_depth = 24,
        .channel_count = 2,
        .sample_count = 2,
    };

    size_t const bytes = aaf_create_packet(ctx, 0, std::span{audio}, std::span{packet});

    auto const payload = aaf_get_audio_payload(std::span<uint8_t const>{packet.data(), bytes});
    EXPECT_EQ(payload.size(), 12U);  // 2 * 2 * 3

    // Deserialize and verify
    std::array<float, 4> restored{};
    size_t const samples = aaf_deserialize_interleaved(payload, AafFormat::int_24bit, 2, 2, std::span{restored});
    EXPECT_EQ(samples, 2U);

    for (size_t i = 0; i < 4; ++i) {
        float const diff = std::fabs(audio[i] - restored[i]);
        EXPECT_TRUE(diff < 0.0001f);
    }
}

TEST(aaf_packet, full_round_trip_all_formats)
{
    Eui48 mac{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56};
    StreamId sid{mac, 0x0001};

    std::array<float, 16> audio{};  // 4 channels, 4 samples
    for (size_t i = 0; i < 16; ++i) {
        audio[i] = static_cast<float>(i) / 32.0f - 0.25f;
    }

    struct FormatTest
    {
        AafFormat format;
        uint8_t bit_depth;
        float tolerance;
    };

    std::array<FormatTest, 4> formats = {
        {{AafFormat::int_16bit, 16, 0.001f},
         {AafFormat::int_24bit, 24, 0.0001f},
         {AafFormat::int_32bit, 32, 0.0001f},
         {AafFormat::float_32bit, 32, 0.0f}}  // Exact for float
    };

    for (auto const& fmt : formats) {
        std::array<uint8_t, 128> packet{};

        AafStreamContext ctx{
            .stream_id = sid,
            .sequence_num = 0,
            .format = fmt.format,
            .sample_rate = AafSampleRate::rate_48_khz,
            .bit_depth = fmt.bit_depth,
            .channel_count = 4,
            .sample_count = 4,
        };

        size_t const bytes = aaf_create_packet(ctx, 0, std::span{audio}, std::span{packet});

        EXPECT_TRUE(bytes > 0);

        auto pdu = aaf_parse_header(std::span<uint8_t const>{packet.data(), bytes});
        EXPECT_TRUE(pdu.has_value());
        EXPECT_TRUE(pdu->is_valid());
        EXPECT_EQ(pdu->get_format(), fmt.format);

        auto const payload = aaf_get_audio_payload(std::span<uint8_t const>{packet.data(), bytes});
        std::array<float, 16> restored{};
        size_t const samples = aaf_deserialize_interleaved(payload, fmt.format, 4, 4, std::span{restored});
        EXPECT_EQ(samples, 4U);

        for (size_t i = 0; i < 16; ++i) {
            if (fmt.tolerance == 0.0f) {
                EXPECT_EQ(audio[i], restored[i]);
            } else {
                EXPECT_TRUE(std::fabs(audio[i] - restored[i]) < fmt.tolerance);
            }
        }
    }
}

//
// Tests: Edge Cases
//

TEST(aaf_edge_cases, empty_input)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};
    std::array<uint8_t, 16> payload{};

    // Zero channels
    size_t bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_24bit, 0, 2, std::span{payload});
    EXPECT_EQ(bytes, 0U);

    // Zero samples
    bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_24bit, 2, 0, std::span{payload});
    EXPECT_EQ(bytes, 0U);
}

TEST(aaf_edge_cases, insufficient_output_buffer)
{
    std::array<float, 4> input = {0.5f, -0.5f, 0.25f, -0.25f};
    std::array<uint8_t, 4> payload{};  // Too small

    size_t const bytes = aaf_serialize_interleaved(std::span{input}, AafFormat::int_24bit, 2, 2, std::span{payload});
    EXPECT_EQ(bytes, 0U);
}

TEST(aaf_edge_cases, insufficient_payload_for_deser)
{
    std::array<uint8_t, 8> payload{};  // Too small for 2 channels, 2 samples of 24-bit
    std::array<float, 4> output{};

    size_t const samples =
        aaf_deserialize_interleaved(std::span<uint8_t const>{payload}, AafFormat::int_24bit, 2, 2, std::span{output});
    EXPECT_EQ(samples, 0U);
}

TEST(aaf_edge_cases, channel_index_out_of_range)
{
    std::array<uint8_t, 12> payload{};
    std::array<float, 4> output{};

    // Channel 5 out of range for 2-channel stream
    size_t const samples =
        aaf_deserialize_channel(std::span<uint8_t const>{payload}, AafFormat::int_24bit, 2, 2, 5, std::span{output});
    EXPECT_EQ(samples, 0U);
}

//
// Tests: Runtime coverage for constexpr utility functions
//

TEST(aaf_rt_coverage, payload_and_packet_size)
{
    // aaf_payload_size and aaf_packet_size are only verified via static_assert above
    auto volatile fmt24 = AafFormat::int_24bit;
    auto volatile fmt16 = AafFormat::int_16bit;
    uint16_t volatile ch8 = 8, ch2 = 2;
    uint16_t volatile s6 = 6, s48 = 48;

    EXPECT_EQ(aaf_payload_size(fmt24, ch8, s6), 144U);
    EXPECT_EQ(aaf_payload_size(fmt16, ch2, s48), 192U);
    EXPECT_EQ(aaf_packet_size(fmt24, ch8, s6), 168U);
    EXPECT_EQ(aaf_packet_size(fmt16, ch2, s48), 216U);
}

TEST(aaf_rt_coverage, default_bit_depth_all_formats)
{
    auto volatile f_float = AafFormat::float_32bit;
    auto volatile f_32 = AafFormat::int_32bit;
    auto volatile f_24 = AafFormat::int_24bit;
    auto volatile f_16 = AafFormat::int_16bit;
    auto volatile f_user = AafFormat::user_specified;

    EXPECT_EQ(aaf_default_bit_depth(f_float), 32U);
    EXPECT_EQ(aaf_default_bit_depth(f_32), 32U);
    EXPECT_EQ(aaf_default_bit_depth(f_24), 24U);
    EXPECT_EQ(aaf_default_bit_depth(f_16), 16U);
    EXPECT_EQ(aaf_default_bit_depth(f_user), 0U);
}

TEST(aaf_rt_coverage, sample_rate_hz_all_rates)
{
    auto volatile r8 = AafSampleRate::rate_8_khz;
    auto volatile r16 = AafSampleRate::rate_16_khz;
    auto volatile r24 = AafSampleRate::rate_24_khz;
    auto volatile r32 = AafSampleRate::rate_32_khz;
    auto volatile r44 = AafSampleRate::rate_44_1_khz;
    auto volatile r48 = AafSampleRate::rate_48_khz;
    auto volatile r88 = AafSampleRate::rate_88_2_khz;
    auto volatile r96 = AafSampleRate::rate_96_khz;
    auto volatile r176 = AafSampleRate::rate_176_4_khz;
    auto volatile r192 = AafSampleRate::rate_192_khz;
    auto volatile ruser = AafSampleRate::user_specified;

    EXPECT_EQ(aaf_sample_rate_hz(r8), 8000U);
    EXPECT_EQ(aaf_sample_rate_hz(r16), 16000U);
    EXPECT_EQ(aaf_sample_rate_hz(r24), 24000U);
    EXPECT_EQ(aaf_sample_rate_hz(r32), 32000U);
    EXPECT_EQ(aaf_sample_rate_hz(r44), 44100U);
    EXPECT_EQ(aaf_sample_rate_hz(r48), 48000U);
    EXPECT_EQ(aaf_sample_rate_hz(r88), 88200U);
    EXPECT_EQ(aaf_sample_rate_hz(r96), 96000U);
    EXPECT_EQ(aaf_sample_rate_hz(r176), 176400U);
    EXPECT_EQ(aaf_sample_rate_hz(r192), 192000U);
    EXPECT_EQ(aaf_sample_rate_hz(ruser), 0U);
}

//
// Main test runner
//

TEST_MAIN(statusbar_avtp, avtp_aaf_test)