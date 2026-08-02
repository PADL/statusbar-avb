#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AAF - AVTP Audio Format - IEEE 1722-2016 Clause 7
/// C++23 implementation for PCM audio format encapsulation
/// Supports: 16-bit, 24-bit, 32-bit signed integer and 32-bit float formats

#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::octet_t;
using ieee::quadlet_t;
using statusbar::BufferError;
using statusbar::failure;
using statusbar::StatusValue;
using statusbar::success;
using tsn::StreamId;

//
// AAF Format Values - IEEE 1722-2016 Table 9
//
/// AAF sample format codes
enum class AafFormat : uint8_t
{
    user_specified = 0x00U,  ///< User specified format
    float_32bit = 0x01U,     ///< 32-bit IEEE 754 floating point
    int_32bit = 0x02U,       ///< 32-bit signed integer
    int_24bit = 0x03U,       ///< 24-bit signed integer
    int_16bit = 0x04U,       ///< 16-bit signed integer
    aes3_32bit = 0x05U,      ///< 32-bit AES3 format (not implemented)
};

/// Get human-readable name for AAF format
/// @param format The AAF sample format code
[[nodiscard]] auto aaf_format_name(AafFormat format) noexcept -> std::string_view;

/// Get bytes per sample for a given format
/// @param format The AAF sample format code
[[nodiscard]] constexpr auto aaf_bytes_per_sample(AafFormat const format) noexcept -> size_t
{
    switch (format) {
        case AafFormat::float_32bit:
        case AafFormat::int_32bit:
        case AafFormat::aes3_32bit:
            return 4;
        case AafFormat::int_24bit:
            return 3;
        case AafFormat::int_16bit:
            return 2;
        case AafFormat::user_specified:
        default:
            return 0;  // Unknown
    }
}

/// Get default bit_depth for a given format
/// @param format The AAF sample format code
[[nodiscard]] constexpr auto aaf_default_bit_depth(AafFormat const format) noexcept -> uint8_t
{
    switch (format) {
        case AafFormat::float_32bit:
        case AafFormat::int_32bit:
        case AafFormat::aes3_32bit:
            return 32;
        case AafFormat::int_24bit:
            return 24;
        case AafFormat::int_16bit:
            return 16;
        case AafFormat::user_specified:
        default:
            return 0;
    }
}

//
// AAF Sample Rate Values - IEEE 1722-2016 Table 11
//
/// AAF nominal sample rate codes (nsr field)
enum class AafSampleRate : uint8_t
{
    user_specified = 0x00U,
    rate_8_khz = 0x01U,
    rate_16_khz = 0x02U,
    rate_32_khz = 0x03U,
    rate_44_1_khz = 0x04U,
    rate_48_khz = 0x05U,
    rate_88_2_khz = 0x06U,
    rate_96_khz = 0x07U,
    rate_176_4_khz = 0x08U,
    rate_192_khz = 0x09U,
    rate_24_khz = 0x0AU,
};

/// Get human-readable name for AAF sample rate
/// @param rate The AAF nominal sample rate code
[[nodiscard]] auto aaf_sample_rate_name(AafSampleRate rate) noexcept -> std::string_view;

/// Get numeric sample rate value in Hz
/// @param rate The AAF nominal sample rate code
[[nodiscard]] constexpr auto aaf_sample_rate_hz(AafSampleRate const rate) noexcept -> uint32_t
{
    switch (rate) {
        case AafSampleRate::rate_8_khz:
            return 8000U;
        case AafSampleRate::rate_16_khz:
            return 16000U;
        case AafSampleRate::rate_32_khz:
            return 32000U;
        case AafSampleRate::rate_44_1_khz:
            return 44100U;
        case AafSampleRate::rate_48_khz:
            return 48000U;
        case AafSampleRate::rate_88_2_khz:
            return 88200U;
        case AafSampleRate::rate_96_khz:
            return 96000U;
        case AafSampleRate::rate_176_4_khz:
            return 176400U;
        case AafSampleRate::rate_192_khz:
            return 192000U;
        case AafSampleRate::rate_24_khz:
            return 24000U;
        case AafSampleRate::user_specified:
        default:
            return 0U;
    }
}

/// Convert sample rate in Hz to AafSampleRate. Returns nullopt for unsupported rates.
/// @param hz Sample rate in Hz (e.g. 48000)
[[nodiscard]] constexpr auto aaf_sample_rate_from_hz(uint32_t hz) noexcept -> std::optional<AafSampleRate>
{
    switch (hz) {
        case 8000U:
            return AafSampleRate::rate_8_khz;
        case 16000U:
            return AafSampleRate::rate_16_khz;
        case 24000U:
            return AafSampleRate::rate_24_khz;
        case 32000U:
            return AafSampleRate::rate_32_khz;
        case 44100U:
            return AafSampleRate::rate_44_1_khz;
        case 48000U:
            return AafSampleRate::rate_48_khz;
        case 88200U:
            return AafSampleRate::rate_88_2_khz;
        case 96000U:
            return AafSampleRate::rate_96_khz;
        case 176400U:
            return AafSampleRate::rate_176_4_khz;
        case 192000U:
            return AafSampleRate::rate_192_khz;
        default:
            return std::nullopt;
    }
}

/// Convert a human-readable name to AafFormat (e.g. "int24", "float32").
/// Returns nullopt for unrecognized names.
/// @param name Format name string
[[nodiscard]] inline auto aaf_format_from_name(std::string_view name) noexcept -> std::optional<AafFormat>
{
    if (name == "int16") {
        return AafFormat::int_16bit;
    }
    if (name == "int24") {
        return AafFormat::int_24bit;
    }
    if (name == "int32") {
        return AafFormat::int_32bit;
    }
    if (name == "float32") {
        return AafFormat::float_32bit;
    }
    if (name == "aes3_32") {
        return AafFormat::aes3_32bit;
    }
    return std::nullopt;
}

/// Check if an AafFormat value is a valid PCM format for AAF
[[nodiscard]] constexpr auto is_valid_aaf_pcm_format(AafFormat fmt) noexcept -> bool
{
    return fmt == AafFormat::float_32bit || fmt == AafFormat::int_32bit || fmt == AafFormat::int_24bit ||
        fmt == AafFormat::int_16bit || fmt == AafFormat::user_specified;
}

//
// AAF PDU - IEEE 1722-2016 Figure 26 (AAF PCM AVTPDU format)
// Wire format: 24-byte header + variable-length audio payload
//
/// AAF PCM PDU Header - IEEE 1722-2016 Clause 7.3
struct AafPdu
{
    /// Total length of AAF header on wire
    static constexpr size_t HEADER_LENGTH = 24;

    /// Maximum channels per frame (10-bit field)
    static constexpr uint16_t MAX_CHANNELS = 1023;

    // Bytes 0-3: subtype, sv, version, mr, rsv, tv, sequence_num, reserved, tu

    /// Byte 0: subtype[7:0] = 0x02 for AAF
    octet_t subtype;

    /// Byte 1: sv[7] | version[6:4] | mr[3] | rsv[2] | tv[0]
    octet_t sv_version_flags;

    /// Byte 2: sequence_num[7:0]
    octet_t sequence_num;

    /// Byte 3: reserved[7:1] | tu[0]
    octet_t reserved_tu;

    // Bytes 4-11: Stream ID (EUI-48 + 16-bit unique ID)

    /// Bytes 4-11: Stream ID
    StreamId stream_id_;

    // Bytes 12-15: AVTP timestamp (32-bit)

    /// Bytes 12-15: AVTP timestamp
    quadlet_t avtp_timestamp;

    // Byte 16: format

    /// Byte 16: format field (Table 9)
    octet_t format;

    // Bytes 17-19: nsr, channels_per_frame, bit_depth
    // Byte 17: nsr[7:4] | rsv[3:2] | channels_per_frame[9:8]
    // Byte 18: channels_per_frame[7:0]
    // Byte 19: bit_depth[7:0]

    /// Byte 17: nsr[7:4] | rsv[3:2] | channels_hi[1:0]
    octet_t nsr_rsv_channels_hi;

    /// Byte 18: channels_lo[7:0]
    octet_t channels_lo;

    /// Byte 19: bit_depth[7:0]
    octet_t bit_depth;

    // Bytes 20-21: stream_data_length

    /// Bytes 20-21: stream_data_length (payload length in octets)
    doublet_t stream_data_length;

    // Bytes 22-23: rsv, sp, evt, reserved
    // Byte 22: rsv[7:5] | sp[4] | evt[3:0]
    // Byte 23: reserved

    /// Byte 22: rsv[7:5] | sp[4] | evt[3:0]
    octet_t rsv_sp_evt;

    /// Byte 23: reserved
    octet_t reserved;

    // Accessors - Control flags (byte 1)
    // Bit layout: sv[7] | version[6:4] | mr[3] | rsv[2] | gv[1] | tv[0]

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return sv_version_flags.has_flag(0x80U); }

    /// Set the stream valid (sv) bit
    /// @param value True to set sv, false to clear
    constexpr void set_sv(bool const value) noexcept { sv_version_flags.set_flag(0x80U, value); }

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return sv_version_flags.get_bits(0x70U, 4); }

    /// Get the media clock restart (mr) bit
    [[nodiscard]] constexpr auto mr() const noexcept -> bool { return sv_version_flags.has_flag(0x08U); }

    /// Set the media clock restart (mr) bit
    /// @param value True to set mr, false to clear
    constexpr void set_mr(bool const value) noexcept { sv_version_flags.set_flag(0x08U, value); }

    /// Get the timestamp valid (tv) bit
    [[nodiscard]] constexpr auto tv() const noexcept -> bool { return sv_version_flags.has_flag(0x01U); }

    /// Set the timestamp valid (tv) bit
    /// @param value True to set tv, false to clear
    constexpr void set_tv(bool const value) noexcept { sv_version_flags.set_flag(0x01U, value); }

    /// Get the timestamp uncertain (tu) bit
    [[nodiscard]] constexpr auto tu() const noexcept -> bool { return reserved_tu.has_flag(0x01U); }

    /// Set the timestamp uncertain (tu) bit
    /// @param value True to set tu, false to clear
    constexpr void set_tu(bool const value) noexcept { reserved_tu.set_flag(0x01U, value); }

    // Accessors - Stream ID

    /// Get the stream ID
    [[nodiscard]] constexpr auto stream_id() const noexcept -> StreamId { return stream_id_; }

    /// Set the stream ID
    /// @param sid The stream ID to set
    constexpr void set_stream_id(StreamId const& sid) noexcept { stream_id_ = sid; }

    // Accessors - Format specific fields

    /// Get the format field
    [[nodiscard]] constexpr auto get_format() const noexcept -> AafFormat { return static_cast<AafFormat>(format.get()); }

    /// Set the format field
    /// @param fmt The AAF sample format code
    constexpr void set_format(AafFormat const fmt) noexcept { format = static_cast<uint8_t>(fmt); }

    /// Get the nominal sample rate (nsr) field (bits 7:4 of byte 17)
    [[nodiscard]] constexpr auto nsr() const noexcept -> AafSampleRate
    {
        return static_cast<AafSampleRate>(nsr_rsv_channels_hi.get_bits(0xF0U, 4));
    }

    /// Set the nominal sample rate (nsr) field (bits 7:4 of byte 17)
    /// @param rate The nominal sample rate code
    constexpr void set_nsr(AafSampleRate const rate) noexcept
    {
        nsr_rsv_channels_hi.set_bits(0xF0U, 4, static_cast<uint8_t>(rate));
    }

    /// Get channels_per_frame (10-bit field: bits 1:0 of byte 17 + all 8 bits of byte 18)
    [[nodiscard]] constexpr auto channels_per_frame() const noexcept -> uint16_t
    {
        uint16_t const hi = nsr_rsv_channels_hi.get_bits<uint16_t>(0x03U, 0);
        uint16_t const lo = static_cast<uint16_t>(channels_lo.get());
        return static_cast<uint16_t>((hi << 8) | lo);
    }

    /// Set channels_per_frame (10-bit field, max 1023)
    /// @param channels Number of channels per frame (clamped to 10 bits)
    constexpr void set_channels_per_frame(uint16_t channels) noexcept
    {
        // Clamp to 10 bits
        channels = static_cast<uint16_t>(channels & 0x03FFU);
        // Set high 2 bits in byte 17 (bits 1:0), preserving nsr (bits 7:4) and rsv (bits 3:2)
        nsr_rsv_channels_hi.set_bits(0x03U, 0, static_cast<uint8_t>((channels >> 8) & 0x03U));
        // Set low 8 bits in byte 18
        channels_lo = static_cast<uint8_t>(channels & 0xFFU);
    }

    /// Get bit_depth field
    [[nodiscard]] constexpr auto get_bit_depth() const noexcept -> uint8_t { return bit_depth.get(); }

    /// Set bit_depth field
    /// @param depth The audio sample bit depth
    constexpr void set_bit_depth(uint8_t const depth) noexcept { bit_depth = depth; }

    /// Get stream_data_length (payload length in octets)
    [[nodiscard]] constexpr auto get_stream_data_length() const noexcept -> uint16_t { return stream_data_length.get(); }

    /// Set stream_data_length
    /// @param length Payload length in octets
    constexpr void set_stream_data_length(uint16_t const length) noexcept { stream_data_length = length; }

    /// Get the sparse timestamp (sp) bit
    [[nodiscard]] constexpr auto sp() const noexcept -> bool { return rsv_sp_evt.has_flag(0x10U); }

    /// Set the sparse timestamp (sp) bit
    /// @param value True to set sp, false to clear
    constexpr void set_sp(bool const value) noexcept { rsv_sp_evt.set_flag(0x10U, value); }

    /// Get the evt field (4 bits)
    [[nodiscard]] constexpr auto evt() const noexcept -> uint8_t { return rsv_sp_evt.get_bits(0x0FU, 0); }

    /// Set the evt field (4 bits)
    /// @param value The event field value (4 bits)
    constexpr void set_evt(uint8_t const value) noexcept { rsv_sp_evt.set_bits(0x0FU, 0, value); }

    // Computed accessors

    /// Calculate sample count from stream_data_length, format, and channel count
    [[nodiscard]] constexpr auto sample_count() const noexcept -> uint16_t
    {
        size_t const bytes_per_sample = aaf_bytes_per_sample(get_format());
        uint16_t const channels = channels_per_frame();
        if (bytes_per_sample == 0 || channels == 0) {
            return 0;
        }
        return static_cast<uint16_t>(get_stream_data_length() / (bytes_per_sample * channels));
    }

    /// Set dimensions (sample count and channel count) and update stream_data_length
    /// @param samples Number of samples per channel per packet
    /// @param channels Number of audio channels
    constexpr void set_dimensions(uint16_t const samples, uint16_t const channels) noexcept
    {
        set_channels_per_frame(channels);
        size_t const bytes_per_sample = aaf_bytes_per_sample(get_format());
        uint16_t const payload_size = static_cast<uint16_t>(static_cast<size_t>(samples) * channels * bytes_per_sample);
        set_stream_data_length(payload_size);
    }

    // Sequence number

    /// Get the sequence number
    [[nodiscard]] constexpr auto get_sequence_num() const noexcept -> uint8_t { return sequence_num.get(); }

    /// Set the sequence number
    /// @param value The sequence number (0-255)
    constexpr void set_sequence_num(uint8_t const value) noexcept { sequence_num = value; }

    /// Increment sequence number (wraps at 256)
    constexpr void increment_sequence_num() noexcept { sequence_num = static_cast<uint8_t>((sequence_num.get() + 1U) & 0xFFU); }

    // AVTP timestamp

    /// Get the AVTP timestamp
    [[nodiscard]] constexpr auto get_avtp_timestamp() const noexcept -> uint32_t { return avtp_timestamp.get(); }

    /// Set the AVTP timestamp
    /// @param value The 32-bit AVTP timestamp value
    constexpr void set_avtp_timestamp(uint32_t const value) noexcept { avtp_timestamp = value; }

    // Validation

    /// Check if this is a valid AAF packet
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype
        if (subtype != AvtpSubtype::aaf) {
            return false;
        }
        // Check sv bit
        if (!sv()) {
            return false;
        }
        // Check version
        if (version() != 0) {
            return false;
        }
        // Check format is valid PCM format
        auto const fmt = get_format();
        if (!is_valid_aaf_pcm_format(fmt)) {
            return false;
        }
        // Check channel count
        if (channels_per_frame() == 0) {
            return false;
        }
        // Check bit_depth for float format
        if (fmt == AafFormat::float_32bit && get_bit_depth() != 32) {
            return false;
        }
        return true;
    }

    // Initialization

    /// Initialize for AAF PCM audio stream
    /// @param sid The stream ID
    /// @param fmt The audio sample format
    /// @param rate The nominal sample rate
    /// @param channels Number of audio channels
    /// @param depth Bit depth of audio samples
    constexpr void init(
        StreamId const& sid, AafFormat const fmt, AafSampleRate const rate, uint16_t const channels, uint8_t const depth) noexcept
    {
        subtype = AvtpSubtype::aaf;
        sv_version_flags = 0x80U;  // sv=1, version=0
        sequence_num = 0U;
        reserved_tu = 0U;
        set_stream_id(sid);
        avtp_timestamp = 0U;
        set_format(fmt);
        set_nsr(rate);
        set_channels_per_frame(channels);
        set_bit_depth(depth);
        stream_data_length = 0U;
        rsv_sp_evt = 0U;
        reserved = 0U;
    }

    auto operator<=>(AafPdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AafPdu) == 24, "AafPdu must be exactly 24 bytes");
static_assert(alignof(AafPdu) <= 4, "AafPdu alignment must not exceed 4 bytes");
static_assert(offsetof(AafPdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AafPdu, stream_id_) == 4, "stream_id must be at offset 4");
static_assert(offsetof(AafPdu, avtp_timestamp) == 12, "avtp_timestamp must be at offset 12");
static_assert(offsetof(AafPdu, format) == 16, "format must be at offset 16");
static_assert(offsetof(AafPdu, stream_data_length) == 20, "stream_data_length must be at offset 20");

//
// AAF Sample Conversion Functions
//
/// Convert a 16-bit signed integer sample to float [-1.0, 1.0]
/// @param sample The 16-bit signed integer audio sample
[[nodiscard]] constexpr auto aaf_int16_to_float(int16_t const sample) noexcept -> float
{
    constexpr float scale = 1.0F / 32768.0F;
    return static_cast<float>(sample) * scale;
}

/// Convert a float sample [-1.0, 1.0] to 16-bit signed integer
/// @param sample The float audio sample in range [-1.0, 1.0]
[[nodiscard]] constexpr auto float_to_aaf_int16(float const sample) noexcept -> int16_t
{
    constexpr float scale = 32768.0F;
    float const scaled = sample * scale;
    float const rounded = (scaled >= 0.0F) ? (scaled + 0.5F) : (scaled - 0.5F);
    if (rounded >= 32767.0F) {
        return 32767;
    }
    if (rounded <= -32768.0F) {
        return -32768;
    }
    return static_cast<int16_t>(rounded);
}

/// Convert a 24-bit signed integer sample to float [-1.0, 1.0]
/// Input: 24-bit value sign-extended to int32_t
/// @param sample The 24-bit audio sample sign-extended to int32_t
[[nodiscard]] constexpr auto aaf_int24_to_float(int32_t const sample) noexcept -> float
{
    constexpr float scale = 1.0F / 8388608.0F;
    return static_cast<float>(sample) * scale;
}

/// Convert a float sample [-1.0, 1.0] to 24-bit signed integer
/// Returns: 24-bit value in lower 24 bits
/// @param sample The float audio sample in range [-1.0, 1.0]
[[nodiscard]] constexpr auto float_to_aaf_int24(float const sample) noexcept -> int32_t
{
    constexpr float scale = 8388608.0F;
    float const scaled = sample * scale;
    float const rounded = (scaled >= 0.0F) ? (scaled + 0.5F) : (scaled - 0.5F);
    if (rounded >= 8388607.0F) {
        return 8388607;
    }
    if (rounded <= -8388608.0F) {
        return -8388608;
    }
    return static_cast<int32_t>(rounded);
}

/// Convert a 32-bit signed integer sample to float [-1.0, 1.0]
/// @param sample The 32-bit signed integer audio sample
[[nodiscard]] constexpr auto aaf_int32_to_float(int32_t const sample) noexcept -> float
{
    constexpr float scale = 1.0F / 2147483648.0F;
    return static_cast<float>(sample) * scale;
}

/// Convert a float sample [-1.0, 1.0] to 32-bit signed integer
/// @param sample The float audio sample in range [-1.0, 1.0]
[[nodiscard]] constexpr auto float_to_aaf_int32(float const sample) noexcept -> int32_t
{
    constexpr double scale = 2147483648.0;  // Use double for precision
    double const scaled = static_cast<double>(sample) * scale;
    double const rounded = (scaled >= 0.0) ? (scaled + 0.5) : (scaled - 0.5);
    if (rounded >= 2147483647.0) {
        return 2147483647;
    }
    if (rounded <= -2147483648.0) {
        return -2147483648;
    }
    return static_cast<int32_t>(rounded);
}

/// Reinterpret float bits as uint32_t for network serialization
/// @param value The float value to convert
[[nodiscard]] inline auto float_to_bits(float const value) noexcept -> uint32_t
{
    return std::bit_cast<uint32_t>(value);
}

/// Reinterpret uint32_t bits as float for network deserialization
/// @param bits The raw 32-bit IEEE 754 representation
[[nodiscard]] inline auto bits_to_float(uint32_t const bits) noexcept -> float
{
    return std::bit_cast<float>(bits);
}

/// Decode one AAF sample from big-endian payload bytes to float
/// @return decoded float sample, or 0 on unsupported format
[[nodiscard]] auto decode_aaf_sample(std::span<uint8_t const> payload, size_t offset, AafFormat format) noexcept -> float;

/// Encode one float sample to big-endian AAF payload bytes
/// @return bytes written (bytes_per_sample), or 0 on unsupported format
[[nodiscard]] auto encode_aaf_sample(float sample_float, std::span<uint8_t> payload, size_t offset, AafFormat format) noexcept
    -> size_t;

//
// AAF Audio Deserialization - Extract audio from payload to float buffer
//
/// Deserialize AAF audio samples from payload to interleaved float buffer
/// Returns the number of samples per channel extracted, or 0 on error
/// @param payload Raw AAF audio payload bytes
/// @param format The AAF sample format code
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param output Destination buffer for interleaved float samples
[[nodiscard]] auto aaf_deserialize_interleaved(
    std::span<uint8_t const> payload,
    AafFormat format,
    uint16_t channel_count,
    uint16_t sample_count,
    std::span<float> output) noexcept -> size_t;

/// Deserialize AAF audio samples from payload to planar (non-interleaved) float buffers
/// @param payload Raw AAF audio payload bytes
/// @param format The AAF sample format code
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param channel_buffers Span of per-channel output buffer pointers
/// @param buffer_capacity Capacity of each channel buffer in samples
[[nodiscard]] auto aaf_deserialize_planar(
    std::span<uint8_t const> payload,
    AafFormat format,
    uint16_t channel_count,
    uint16_t sample_count,
    std::span<float* const> channel_buffers,
    size_t buffer_capacity) noexcept -> size_t;

/// Deserialize AAF audio samples for a single channel
/// @param payload Raw AAF audio payload bytes
/// @param format The AAF sample format code
/// @param channel_count Total number of audio channels in payload
/// @param sample_count Number of samples per channel
/// @param channel_index Zero-based index of the channel to extract
/// @param output Destination buffer for the extracted channel samples
[[nodiscard]] auto aaf_deserialize_channel(
    std::span<uint8_t const> payload,
    AafFormat format,
    uint16_t channel_count,
    uint16_t sample_count,
    uint16_t channel_index,
    std::span<float> output) noexcept -> size_t;

//
// AAF Audio Serialization - Pack float buffer into AAF payload
//
/// Serialize interleaved float audio samples to AAF payload
/// Returns bytes written, or 0 on error
/// @param input Interleaved float audio samples
/// @param format The AAF sample format code
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param payload Destination buffer for serialized payload bytes
[[nodiscard]] auto aaf_serialize_interleaved(
    std::span<float const> input,
    AafFormat format,
    uint16_t channel_count,
    uint16_t sample_count,
    std::span<uint8_t> payload) noexcept -> size_t;

/// Serialize planar (non-interleaved) float audio samples to AAF payload
/// @param channel_buffers Span of per-channel input buffer pointers
/// @param format The AAF sample format code
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param payload Destination buffer for serialized payload bytes
[[nodiscard]] auto aaf_serialize_planar(
    std::span<float const* const> channel_buffers,
    AafFormat format,
    uint16_t channel_count,
    uint16_t sample_count,
    std::span<uint8_t> payload) noexcept -> size_t;

//
// AAF Stream Context - Configuration for an AAF audio stream
//
/// Stream context for AAF packet creation
/// Contains all the per-stream configuration needed to create AAF packets
struct AafStreamContext
{
    StreamId stream_id{};                                   ///< Stream ID for this AAF stream
    uint8_t sequence_num{0};                                ///< Current sequence number (auto-incremented)
    AafFormat format{AafFormat::int_24bit};                 ///< Audio sample format
    AafSampleRate sample_rate{AafSampleRate::rate_48_khz};  ///< Nominal sample rate
    uint8_t bit_depth{24};                                  ///< Bit depth of audio samples
    uint16_t channel_count{2};                              ///< Number of audio channels
    uint16_t sample_count{6};                               ///< Number of samples per packet

    /// Get the next sequence number and auto-increment
    [[nodiscard]] constexpr auto next_sequence_num() noexcept -> uint8_t
    {
        uint8_t const current = sequence_num;
        sequence_num = static_cast<uint8_t>((sequence_num + 1U) & 0xFFU);
        return current;
    }

    /// Calculate the payload size for this stream configuration
    [[nodiscard]] constexpr auto payload_size() const noexcept -> size_t
    {
        return static_cast<size_t>(channel_count) * sample_count * aaf_bytes_per_sample(format);
    }

    /// Calculate the total packet size for this stream configuration
    [[nodiscard]] constexpr auto packet_size() const noexcept -> size_t { return AafPdu::HEADER_LENGTH + payload_size(); }
};

//
// High-level packet creation/parsing helpers
//
/// Calculate the payload size for an AAF packet
/// @param format The AAF sample format code
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
[[nodiscard]] constexpr auto aaf_payload_size(
    AafFormat const format, uint16_t const channel_count, uint16_t const sample_count) noexcept -> size_t
{
    return static_cast<size_t>(channel_count) * sample_count * aaf_bytes_per_sample(format);
}

/// Calculate the required buffer size for an AAF packet
/// @param format The AAF sample format code
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
[[nodiscard]] constexpr auto aaf_packet_size(
    AafFormat const format, uint16_t const channel_count, uint16_t const sample_count) noexcept -> size_t
{
    return AafPdu::HEADER_LENGTH + aaf_payload_size(format, channel_count, sample_count);
}

/// Create a complete AAF packet with audio data
/// Uses AafStreamContext for stream configuration (auto-increments sequence_num)
/// sparse_timestamp is always true
/// Returns bytes written, or 0 on error
/// @param ctx Stream context with configuration (sequence_num is auto-incremented)
/// @param avtp_timestamp The 32-bit AVTP presentation timestamp
/// @param interleaved_audio Interleaved float audio samples to encode
/// @param packet_buffer Destination buffer for the complete packet
[[nodiscard]] auto aaf_create_packet(
    AafStreamContext& ctx,
    uint32_t avtp_timestamp,
    std::span<float const> interleaved_audio,
    std::span<uint8_t> packet_buffer) noexcept -> size_t;

/// Parse an AAF packet header
/// Returns the parsed PDU if valid, nullopt otherwise
/// Uses memcpy to avoid alignment UB from reinterpret_cast on unaligned buffers
/// @param packet Raw packet data including the AAF header
[[nodiscard]] auto aaf_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<AafPdu>;

/// Get audio payload span from a packet
/// @param packet Raw packet data including the AAF header
[[nodiscard]] auto aaf_get_audio_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp

// Serialization traits - AafPdu is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AafPdu> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
