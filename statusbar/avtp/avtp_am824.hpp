#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AM824 - IEC 61883-6 Audio over IEEE 1722 AVTP
/// Modernized C++23 implementation based on IEEE 1722-2016 Section 5.4
/// and IEC 61883-6 Edition 2.0 for AM824 audio format

#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
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
// AM824 Constants - IEC 61883-6 Edition 2.0
//
/// AM824 format code (FMT field) - IEC 61883-6 Section 5.3
constexpr uint8_t AM824_FMT = 0x10U;

/// AM824 label for multi-bit linear audio (MBLA) - IEC 61883-6 Section 8.2.3
constexpr uint8_t AM824_LABEL_MBLA = 0x40U;

/// AM824 label for raw MIDI - IEC 61883-6 Section 8.2.3
constexpr uint8_t AM824_LABEL_RAW_MIDI = 0x80U;

/// AM824 label for SMPTE time code conformant data - IEC 61883-6 Section 8.2.6
/// Labels 0x88-0x8B: counter in bits 1:0 (00=no data, 01=first, 10=middle, 11=last)
constexpr uint8_t AM824_LABEL_SMPTE = 0x88U;

/// AM824 label for no-data (MIDI conformant no-data format) - IEC 61883-6 Figure 19
constexpr uint8_t AM824_LABEL_NO_DATA = 0x80U;

/// AM824 sampling frequency codes (FDF field) - IEC 61883-6 Section 10.3.2
enum class Am824SampleRate : uint8_t
{
    rate_32_khz = 0x00U,
    rate_44_1_khz = 0x01U,
    rate_48_khz = 0x02U,
    rate_88_2_khz = 0x03U,
    rate_96_khz = 0x04U,
    rate_176_4_khz = 0x05U,
    rate_192_khz = 0x06U,
};

/// Get human-readable name for AM824 sample rate
/// @param rate The AM824 sample rate code
[[nodiscard]] auto am824_sample_rate_name(Am824SampleRate rate) noexcept -> std::string_view;

/// Get numeric sample rate value in Hz
/// @param rate The AM824 sample rate code
[[nodiscard]] constexpr auto am824_sample_rate_hz(Am824SampleRate rate) noexcept -> uint32_t
{
    switch (rate) {
        case Am824SampleRate::rate_32_khz:
            return 32000U;
        case Am824SampleRate::rate_44_1_khz:
            return 44100U;
        case Am824SampleRate::rate_48_khz:
            return 48000U;
        case Am824SampleRate::rate_88_2_khz:
            return 88200U;
        case Am824SampleRate::rate_96_khz:
            return 96000U;
        case Am824SampleRate::rate_176_4_khz:
            return 176400U;
        case Am824SampleRate::rate_192_khz:
            return 192000U;
        default:
            return 0U;
    }
}

/// Convert sample rate in Hz to Am824SampleRate. Returns nullopt for unsupported rates.
/// @param hz Sample rate in Hz (e.g. 48000)
[[nodiscard]] constexpr auto am824_sample_rate_from_hz(uint32_t hz) noexcept -> std::optional<Am824SampleRate>
{
    switch (hz) {
        case 32000U:
            return Am824SampleRate::rate_32_khz;
        case 44100U:
            return Am824SampleRate::rate_44_1_khz;
        case 48000U:
            return Am824SampleRate::rate_48_khz;
        case 88200U:
            return Am824SampleRate::rate_88_2_khz;
        case 96000U:
            return Am824SampleRate::rate_96_khz;
        case 176400U:
            return Am824SampleRate::rate_176_4_khz;
        case 192000U:
            return Am824SampleRate::rate_192_khz;
        default:
            return std::nullopt;
    }
}

/// Get SYT timestamp interval for sample rate - IEC 61883-6 Section 4.2 and 9.2
/// @param rate The AM824 sample rate code
[[nodiscard]] constexpr auto am824_syt_interval(Am824SampleRate rate) noexcept -> uint8_t
{
    switch (rate) {
        case Am824SampleRate::rate_32_khz:
        case Am824SampleRate::rate_44_1_khz:
        case Am824SampleRate::rate_48_khz:
            return 8U;
        case Am824SampleRate::rate_88_2_khz:
        case Am824SampleRate::rate_96_khz:
            return 16U;
        case Am824SampleRate::rate_176_4_khz:
        case Am824SampleRate::rate_192_khz:
            return 32U;
        default:
            return 0U;
    }
}

/// Convert a presentation time (gPTP nanoseconds) to a 16-bit IEC 61883 SYT.
/// Layout: cycle_count[3:0] (in 8 kHz / 125 us cycles) in bits 15-12,
/// cycle_offset (0..3071, in 24.576 MHz ticks within the cycle) in bits 11-0.
/// This is the presentation time of the data block at a syt_interval boundary;
/// without it (SYT left at the 0xFFFF "no-info" sentinel) receivers have no
/// media-clock reference and free-run/slip. Paired with tv=1 + the AVTP
/// timestamp, which carry the same presentation time.
[[nodiscard]] constexpr auto am824_presentation_to_syt(uint64_t pts_ns) noexcept -> uint16_t
{
    constexpr uint64_t ns_per_cycle = 125'000U;  // 1/8000 s
    constexpr uint64_t ticks_per_cycle = 3072U;  // 24.576 MHz / 8000 Hz
    uint64_t const cycle = pts_ns / ns_per_cycle;
    uint64_t const offset = ((pts_ns % ns_per_cycle) * ticks_per_cycle) / ns_per_cycle;
    return static_cast<uint16_t>(((cycle & 0x0FU) << 12) | (offset & 0x0FFFU));
}

//
// AM824 PDU - IEEE 1722-2016 Figure 23 IEC 61883 with no source packet header
// Wire format: 24 byte AVTPDU common stream header + 8 byte CIP headers + audio data
//
/// AVTPDU Common Stream Header - IEEE 1722-2016 Figure 9
/// This is the header common to all IEC 61883/IIDC stream packets
struct AvtpStreamHeader
{
    /// Total length of common stream header on wire
    static constexpr size_t LENGTH = 24;

    // Bytes 0-3: subtype, sv, version, mr, r, gv, tv, sequence_num, reserved, tu

    /// Byte 0: subtype[7:0]
    octet_t subtype;

    /// Byte 1: sv[7] | version[6:4] | mr[3] | r[2] | gv[1] | tv[0]
    octet_t sv_version_flags;

    /// Byte 2: sequence_num[7:0]
    octet_t sequence_num;

    /// Byte 3: reserved[6:0] | tu[0]
    octet_t reserved_tu;

    // Bytes 4-11: Stream ID (EUI-48 + 16-bit unique ID)

    /// Bytes 4-11: Stream ID
    StreamId stream_id_;

    // Bytes 12-15: AVTP timestamp (32-bit)

    /// Bytes 12-15: AVTP timestamp
    quadlet_t avtp_timestamp;

    // Bytes 16-19: Format specific data

    /// Bytes 16-19: gateway_info for 61883/IIDC
    quadlet_t format_specific_data;

    // Bytes 20-23: stream_data_length and protocol_specific_header

    /// Bytes 20-21: stream_data_length
    doublet_t stream_data_length;

    /// Bytes 22-23: protocol_specific_header (tag, channel, tcode, sy for 61883)
    doublet_t protocol_specific_header;

    // Accessors - Control flags (byte 1)
    // Bit layout: sv[7] | version[6:4] | mr[3] | r[2] | gv[1] | tv[0]

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return sv_version_flags.has_flag(0x80U); }

    /// Set the stream valid (sv) bit
    /// @param value True to set sv, false to clear
    constexpr auto set_sv(bool value) noexcept -> void { sv_version_flags.set_flag(0x80U, value); }

    /// Get the version field (bits 6:4)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return sv_version_flags.get_bits(0x70U, 4); }

    /// Get the media clock restart (mr) bit
    [[nodiscard]] constexpr auto mr() const noexcept -> bool { return sv_version_flags.has_flag(0x08U); }

    /// Set the media clock restart (mr) bit
    /// @param value True to set mr, false to clear
    constexpr auto set_mr(bool value) noexcept -> void { sv_version_flags.set_flag(0x08U, value); }

    /// Get the gateway valid (gv) bit
    [[nodiscard]] constexpr auto gv() const noexcept -> bool { return sv_version_flags.has_flag(0x02U); }

    /// Get the timestamp valid (tv) bit
    [[nodiscard]] constexpr auto tv() const noexcept -> bool { return sv_version_flags.has_flag(0x01U); }

    /// Set the timestamp valid (tv) bit
    /// @param value True to set tv, false to clear
    constexpr auto set_tv(bool value) noexcept -> void { sv_version_flags.set_flag(0x01U, value); }

    /// Get the timestamp uncertain (tu) bit
    [[nodiscard]] constexpr auto tu() const noexcept -> bool { return reserved_tu.has_flag(0x01U); }

    /// Get the stream ID
    [[nodiscard]] constexpr auto stream_id() const noexcept -> StreamId { return stream_id_; }

    /// Set the stream ID
    /// @param sid The stream ID to set
    constexpr auto set_stream_id(StreamId const& sid) noexcept -> void { stream_id_ = sid; }

    // 61883/IIDC protocol specific header fields (bytes 22-23)

    /// Get tag field (bits 15:14 of protocol_specific_header)
    [[nodiscard]] constexpr auto tag() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>((protocol_specific_header.get() >> 14) & 0x03U);
    }

    /// Get channel field (bits 13:8 of protocol_specific_header)
    [[nodiscard]] constexpr auto channel() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>((protocol_specific_header.get() >> 8) & 0x3FU);
    }

    /// Get tcode field (bits 7:4 of protocol_specific_header)
    [[nodiscard]] constexpr auto tcode() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>((protocol_specific_header.get() >> 4) & 0x0FU);
    }

    /// Get sy field (bits 3:0 of protocol_specific_header)
    [[nodiscard]] constexpr auto sy() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>(protocol_specific_header.get() & 0x0FU);
    }

    /// Set protocol specific header fields for IEC 61883/IIDC
    /// @param tag_val Tag field value (2 bits)
    /// @param channel_val Channel field value (6 bits)
    /// @param tcode_val Transaction code field value (4 bits)
    /// @param sy_val Synchronization field value (4 bits)
    constexpr auto set_protocol_specific(uint8_t tag_val, uint8_t channel_val, uint8_t tcode_val, uint8_t sy_val) noexcept -> void
    {
        uint16_t const value = static_cast<uint16_t>(
            ((tag_val & 0x03U) << 14) | ((channel_val & 0x3FU) << 8) | ((tcode_val & 0x0FU) << 4) | (sy_val & 0x0FU));
        protocol_specific_header = value;
    }

    // Initialization

    /// Initialize for IEC 61883/IIDC stream
    /// @param sid The stream ID to assign
    constexpr auto init_61883_iidc(StreamId const& sid) noexcept -> void
    {
        subtype = AvtpSubtype::iec_61883_iidc;
        sv_version_flags = 0x80U;  // sv=1, version=0
        sequence_num = 0U;
        reserved_tu = 0U;
        set_stream_id(sid);
        avtp_timestamp = 0U;
        format_specific_data = 0U;
        stream_data_length = 0U;
        set_protocol_specific(1U, 0U, 0x0AU, 0U);  // tag=1 (CIP), tcode=0xA (stream)
    }

    auto operator<=>(AvtpStreamHeader const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AvtpStreamHeader) == 24, "AvtpStreamHeader must be exactly 24 bytes");
static_assert(alignof(AvtpStreamHeader) <= 4, "AvtpStreamHeader alignment must not exceed 4 bytes");

/// IEC 61883 CIP Header - IEEE 1722-2016 Figure 22
/// 8 bytes total (two 32-bit quadlets)
struct Cip61883Header
{
    /// Total length of CIP header on wire
    static constexpr size_t LENGTH = 8;

    // CIP #1 (first quadlet) - IEC 61883-1 Section 6.3

    /// Byte 0: qi_1[7:6] | sid[5:0]
    octet_t qi1_sid;

    /// Byte 1: dbs[7:0] - data block size (number of quadlets per data block)
    octet_t dbs;

    /// Byte 2: fn[7:6] | qpc[5:3] | sph[2] | rsv[1:0]
    octet_t fn_qpc_sph;

    /// Byte 3: dbc[7:0] - data block count
    octet_t dbc;

    // CIP #2 (second quadlet) - IEC 61883-1 Section 6.3

    /// Byte 4: qi_2[7:6] | fmt[5:0]
    octet_t qi2_fmt;

    /// Byte 5: fdf[7:0] - format dependent field (sample rate for AM824)
    octet_t fdf;

    /// Bytes 6-7: syt[15:0] - synchronization timestamp
    doublet_t syt;

    // Accessors for CIP #1
    // Byte 0: qi_1[7:6] | sid[5:0]
    // Byte 2: fn[7:6] | qpc[5:3] | sph[2] | rsv[1:0]

    /// Get QI_1 field (always 0b00 for CIP)
    [[nodiscard]] constexpr auto qi1() const noexcept -> uint8_t { return qi1_sid.get_bits(0xC0U, 6); }

    /// Get source ID (SID) field
    [[nodiscard]] constexpr auto sid() const noexcept -> uint8_t { return qi1_sid.get_bits(0x3FU, 0); }

    /// Set source ID (SID) field
    /// @param value The source ID value (6 bits)
    constexpr auto set_sid(uint8_t value) noexcept -> void { qi1_sid.set_bits(0x3FU, 0, value); }

    /// Get data block size (DBS) - number of quadlets per data block (equals channel count for AM824)
    [[nodiscard]] constexpr auto data_block_size() const noexcept { return dbs.get(); }

    /// Set data block size (DBS)
    /// @param value Number of quadlets per data block
    constexpr auto set_data_block_size(uint8_t value) noexcept -> void { dbs = value; }

    /// Get FN field (fraction number)
    [[nodiscard]] constexpr auto fn() const noexcept -> uint8_t { return fn_qpc_sph.get_bits(0xC0U, 6); }

    /// Get QPC field (quadlet padding count)
    [[nodiscard]] constexpr auto qpc() const noexcept -> uint8_t { return fn_qpc_sph.get_bits(0x38U, 3); }

    /// Get SPH field (source packet header)
    [[nodiscard]] constexpr auto sph() const noexcept -> bool { return fn_qpc_sph.has_flag(0x04U); }

    /// Get data block count (DBC) - increments with each data block
    [[nodiscard]] constexpr auto data_block_count() const noexcept { return dbc.get(); }

    /// Set data block count (DBC)
    /// @param value The data block count value
    constexpr auto set_data_block_count(uint8_t value) noexcept -> void { dbc = value; }

    // Accessors for CIP #2
    // Byte 4: qi_2[7:6] | fmt[5:0]

    /// Get QI_2 field (always 0b10 for CIP)
    [[nodiscard]] constexpr auto qi2() const noexcept -> uint8_t { return qi2_fmt.get_bits(0xC0U, 6); }

    /// Get FMT field (format code, 0x10 for AM824)
    [[nodiscard]] constexpr auto fmt() const noexcept -> uint8_t { return qi2_fmt.get_bits(0x3FU, 0); }

    /// Set FMT field (preserves qi_2 bits which should be 0b10)
    /// @param value The format code (6 bits)
    constexpr auto set_fmt(uint8_t value) noexcept -> void
    {
        qi2_fmt.set_bits(0xC0U, 6, 0x02U);  // Set qi_2 = 0b10
        qi2_fmt.set_bits(0x3FU, 0, value);
    }

    /// Get FDF field (format dependent field - sample rate for AM824)
    [[nodiscard]] constexpr auto format_dependent_field() const noexcept { return fdf.get(); }

    /// Set FDF field
    /// @param value The format dependent field value
    constexpr auto set_format_dependent_field(uint8_t value) noexcept -> void { fdf = value; }

    /// Get sample rate from FDF field
    [[nodiscard]] constexpr auto sample_rate() const noexcept -> Am824SampleRate
    {
        return static_cast<Am824SampleRate>(fdf.get() & 0x07U);
    }

    /// Set sample rate in FDF field
    /// @param rate The AM824 sample rate code
    constexpr auto set_sample_rate(Am824SampleRate rate) noexcept -> void { fdf = static_cast<uint8_t>(rate); }

    /// Get SYT timestamp
    [[nodiscard]] constexpr auto syt_timestamp() const noexcept { return syt.get(); }

    /// Set SYT timestamp
    /// @param value The 16-bit SYT timestamp value
    constexpr auto set_syt_timestamp(uint16_t value) noexcept -> void { syt = value; }

    /// Check if SYT is valid (0xFFFF means no timestamp)
    [[nodiscard]] constexpr auto syt_valid() const noexcept { return syt.get() != 0xFFFFU; }

    // Initialization

    /// Initialize for AM824 audio
    /// @param channel_count Number of audio channels
    /// @param rate The AM824 sample rate code
    constexpr auto init_am824(uint8_t channel_count, Am824SampleRate rate) noexcept -> void
    {
        qi1_sid = 0x00U;              // QI_1 = 0b00, SID = 0
        dbs = channel_count;          // DBS = channel count
        fn_qpc_sph = 0x00U;           // FN=0, QPC=0, SPH=0
        dbc = 0U;                     // DBC starts at 0
        qi2_fmt = 0x80U | AM824_FMT;  // QI_2 = 0b10, FMT = 0x10
        fdf = static_cast<uint8_t>(rate);
        syt = 0xFFFFU;  // No timestamp by default
    }

    auto operator<=>(Cip61883Header const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(Cip61883Header) == 8, "Cip61883Header must be exactly 8 bytes");
static_assert(alignof(Cip61883Header) <= 2, "Cip61883Header alignment must not exceed 2 bytes");

/// AM824 PDU - Complete packet structure
/// Combines AVTP stream header + CIP header + audio payload reference
struct Am824Pdu
{
    /// Minimum wire length (header only, no audio data)
    static constexpr size_t HEADER_LENGTH = AvtpStreamHeader::LENGTH + Cip61883Header::LENGTH;  // 32 bytes

    /// Maximum channel count supported
    static constexpr size_t MAX_CHANNELS = 64;

    /// Maximum samples per packet
    static constexpr size_t MAX_SAMPLES_PER_PACKET = 32;

    /// Bytes per AM824 audio sample (label + 24-bit audio)
    static constexpr size_t BYTES_PER_SAMPLE = 4;

    // Header fields

    /// AVTP common stream header
    AvtpStreamHeader stream_header{};

    /// IEC 61883 CIP header
    Cip61883Header cip_header{};

    // Accessors

    /// Get the stream ID
    [[nodiscard]] constexpr auto stream_id() const noexcept -> StreamId { return stream_header.stream_id(); }

    /// Set the stream ID
    /// @param sid The stream ID to set
    constexpr auto set_stream_id(StreamId const& sid) noexcept -> void { stream_header.set_stream_id(sid); }

    /// Get the AVTP timestamp
    [[nodiscard]] constexpr auto avtp_timestamp() const noexcept -> uint32_t { return stream_header.avtp_timestamp.get(); }

    /// Set the AVTP timestamp
    /// @param value The 32-bit AVTP timestamp value
    constexpr auto set_avtp_timestamp(uint32_t value) noexcept -> void { stream_header.avtp_timestamp = value; }

    /// Get the sequence number
    [[nodiscard]] constexpr auto sequence_num() const noexcept -> uint8_t { return stream_header.sequence_num.get(); }

    /// Set the sequence number
    /// @param value The sequence number (0-255)
    constexpr auto set_sequence_num(uint8_t value) noexcept -> void { stream_header.sequence_num = value; }

    /// Increment sequence number (wraps at 256)
    constexpr auto increment_sequence_num() noexcept -> void
    {
        stream_header.sequence_num = static_cast<uint8_t>((stream_header.sequence_num.get() + 1U) & 0xFFU);
    }

    /// Get channel count (from DBS field)
    [[nodiscard]] constexpr auto channel_count() const noexcept -> uint8_t { return cip_header.data_block_size(); }

    /// Set channel count
    /// @param count Number of audio channels
    constexpr auto set_channel_count(uint8_t count) noexcept -> void { cip_header.set_data_block_size(count); }

    /// Get sample rate
    [[nodiscard]] constexpr auto sample_rate() const noexcept -> Am824SampleRate { return cip_header.sample_rate(); }

    /// Set sample rate
    /// @param rate The AM824 sample rate code
    constexpr auto set_sample_rate(Am824SampleRate rate) noexcept -> void { cip_header.set_sample_rate(rate); }

    /// Get data block count
    [[nodiscard]] constexpr auto data_block_count() const noexcept -> uint8_t { return cip_header.data_block_count(); }

    /// Set data block count
    /// @param value The data block count value
    constexpr auto set_data_block_count(uint8_t value) noexcept -> void { cip_header.set_data_block_count(value); }

    /// Increment data block count by sample count
    /// @param sample_count Number of samples to advance the data block count
    constexpr auto increment_data_block_count(uint8_t sample_count) noexcept -> void
    {
        uint8_t const current = cip_header.data_block_count();
        cip_header.set_data_block_count(static_cast<uint8_t>((current + sample_count) & 0xFFU));
    }

    /// Get SYT timestamp
    [[nodiscard]] constexpr auto syt_timestamp() const noexcept -> uint16_t { return cip_header.syt_timestamp(); }

    /// Set SYT timestamp
    /// @param value The 16-bit SYT timestamp value
    constexpr auto set_syt_timestamp(uint16_t value) noexcept -> void { cip_header.set_syt_timestamp(value); }

    /// Check if timestamp is valid
    [[nodiscard]] constexpr auto tv() const noexcept -> bool { return stream_header.tv(); }

    /// Set timestamp valid
    /// @param value True to set tv, false to clear
    constexpr auto set_tv(bool value) noexcept -> void { stream_header.set_tv(value); }

    /// Get stream_data_length (includes CIP headers + audio payload)
    [[nodiscard]] constexpr auto stream_data_length() const noexcept -> uint16_t { return stream_header.stream_data_length.get(); }

    /// Set stream_data_length
    /// @param value Stream data length in bytes (includes CIP header + audio payload)
    constexpr auto set_stream_data_length(uint16_t value) noexcept -> void { stream_header.stream_data_length = value; }

    /// Calculate audio payload length from stream_data_length
    [[nodiscard]] constexpr auto audio_payload_length() const noexcept -> uint16_t
    {
        uint16_t const sdl = stream_data_length();
        return (sdl > Cip61883Header::LENGTH) ? static_cast<uint16_t>(sdl - Cip61883Header::LENGTH) : 0U;
    }

    /// Calculate sample count from stream_data_length and channel count
    [[nodiscard]] constexpr auto sample_count() const noexcept -> uint8_t
    {
        uint16_t const payload = audio_payload_length();
        uint8_t const channels = channel_count();
        if (channels == 0) {
            return 0U;
        }
        return static_cast<uint8_t>(payload / (channels * BYTES_PER_SAMPLE));
    }

    /// Set dimensions (sample count and channel count) and update stream_data_length
    /// @param samples Number of samples per channel per packet
    /// @param channels Number of audio channels
    constexpr auto set_dimensions(uint8_t samples, uint8_t channels) noexcept -> void
    {
        set_channel_count(channels);
        uint16_t const audio_payload = static_cast<uint16_t>(static_cast<size_t>(samples) * channels * BYTES_PER_SAMPLE);
        set_stream_data_length(static_cast<uint16_t>(Cip61883Header::LENGTH + audio_payload));
    }

    // Validation

    /// Check if this is a valid AM824 packet
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype
        if (stream_header.subtype != AvtpSubtype::iec_61883_iidc) {
            return false;
        }
        // Check sv bit
        if (!stream_header.sv()) {
            return false;
        }
        // Check version
        if (stream_header.version() != 0) {
            return false;
        }
        // Check FMT
        if (cip_header.fmt() != AM824_FMT) {
            return false;
        }
        // Check sample rate is valid. The SFC (sampling-frequency code) is only the
        // low 3 bits of the FDF: per IEEE Std 1722-2016 5.4.4.3 (FDF has the same
        // definition as IEC 61883) and Annex I.6, FDF = fdf_evt[7:3] + fdf_sfc[2:0].
        // The upper bits select the rate-control mode -- clock-based (0000 0xxx) or
        // command-based (0000 1xxx, IEC 61883-6 Clause 10.4) -- and must NOT gate
        // validity. Validate only the SFC sub-field, consistent with sample_rate().
        if ((static_cast<uint8_t>(cip_header.format_dependent_field()) & 0x07U) > 0x06U) {
            return false;
        }
        // Reject source-packet-header (SPH=1) and fragmented (FN != 0) CIP: both
        // change the payload layout (an SPH prepends a 4-byte timestamp to each
        // source packet; FN != 0 splits a source packet across CIPs), which the flat
        // audio_payload_length()/sample_count() model here cannot parse. Reject
        // rather than silently misinterpret the payload.
        if (cip_header.sph() || cip_header.fn() != 0) {
            return false;
        }
        // Check channel count
        if (channel_count() == 0) {
            return false;
        }
        return true;
    }

    // Initialization

    /// Initialize for AM824 audio stream
    /// @param sid The stream ID
    /// @param channels Number of audio channels
    /// @param rate The AM824 sample rate code
    constexpr auto init(StreamId const& sid, uint8_t channels, Am824SampleRate rate) noexcept -> void
    {
        stream_header.init_61883_iidc(sid);
        cip_header.init_am824(channels, rate);
    }

    auto operator<=>(Am824Pdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(Am824Pdu) == 32, "Am824Pdu must be exactly 32 bytes");
static_assert(alignof(Am824Pdu) <= 4, "Am824Pdu alignment must not exceed 4 bytes");
static_assert(offsetof(Am824Pdu, stream_header) == 0, "stream_header must be at offset 0");
static_assert(offsetof(Am824Pdu, cip_header) == 24, "cip_header must be at offset 24");

//
// AM824 Audio Sample Conversion
//
/// Convert a 24-bit signed integer audio sample to float [-1.0, 1.0]
/// @param sample The 24-bit audio sample sign-extended to int32_t
[[nodiscard]] constexpr auto am824_sample_to_float(int32_t sample) noexcept -> float
{
    // Scale from 24-bit to float
    // 24-bit range: -8388608 to 8388607
    constexpr float scale = 1.0F / 8388608.0F;
    return static_cast<float>(sample) * scale;
}

/// Convert a float audio sample [-1.0, 1.0] to 24-bit signed integer
/// Uses symmetric scaling (8388608) and rounding for bit-exact round-trip
/// @param sample The float audio sample in range [-1.0, 1.0]
[[nodiscard]] constexpr auto float_to_am824_sample(float sample) noexcept -> int32_t
{
    // Scale from float to 24-bit using symmetric factor for exact round-trip
    constexpr float scale = 8388608.0F;
    float const scaled = sample * scale;
    // Round to nearest integer (add 0.5 for positive, subtract 0.5 for negative)
    float const rounded = (scaled >= 0.0F) ? (scaled + 0.5F) : (scaled - 0.5F);
    // Clamp to 24-bit range
    if (rounded >= 8388607.0F) {
        return 8388607;
    }
    if (rounded <= -8388608.0F) {
        return -8388608;
    }
    return static_cast<int32_t>(rounded);
}

/// Parse a single AM824 quadlet and extract the audio sample
/// Returns 0 if the label is not MBLA (audio)
/// @param quadlet The 32-bit AM824 quadlet (label + 24-bit audio)
[[nodiscard]] constexpr auto parse_am824_quadlet(uint32_t quadlet) noexcept -> int32_t
{
    // Extract label (upper 8 bits)
    uint8_t const label = static_cast<uint8_t>((quadlet >> 24) & 0xFFU);

    // Check for audio label
    if (label != AM824_LABEL_MBLA) {
        return 0;
    }

    // Extract 24-bit audio sample (lower 24 bits)
    uint32_t const sample_bits = quadlet & 0x00FFFFFFU;

    // Sign extend from 24-bit to 32-bit
    if ((sample_bits & 0x00800000U) != 0) {
        // Negative value - sign extend
        return static_cast<int32_t>(sample_bits | 0xFF000000U);
    }
    return static_cast<int32_t>(sample_bits);
}

/// Create an AM824 quadlet from a 24-bit audio sample
/// @param sample The 24-bit audio sample (lower 24 bits used)
[[nodiscard]] constexpr auto create_am824_quadlet(int32_t sample) noexcept -> uint32_t
{
    // Mask to 24 bits and add MBLA label
    return (static_cast<uint32_t>(AM824_LABEL_MBLA) << 24) | (static_cast<uint32_t>(sample) & 0x00FFFFFFU);
}

//
// AM824 Audio Deserialization - Extract audio from packet to float buffer
//
/// Deserialize AM824 audio samples from a packet payload to interleaved float buffer
/// Returns the number of samples per channel extracted
/// @param payload Raw AM824 audio payload bytes
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param output Destination buffer for interleaved float samples
[[nodiscard]] auto am824_deserialize_interleaved(
    std::span<uint8_t const> payload, uint8_t channel_count, uint8_t sample_count, std::span<float> output) noexcept -> size_t;

/// Deserialize AM824 audio samples from a packet payload to non-interleaved (planar) float buffers
/// Each channel gets its own contiguous buffer
/// @param payload Raw AM824 audio payload bytes
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param channel_buffers Span of per-channel output buffer pointers
/// @param buffer_capacity Capacity of each channel buffer in samples
[[nodiscard]] auto am824_deserialize_planar(
    std::span<uint8_t const> payload,
    uint8_t channel_count,
    uint8_t sample_count,
    std::span<float* const> channel_buffers,
    size_t buffer_capacity) noexcept -> size_t;

/// Deserialize AM824 audio samples for a single channel
/// @param payload Raw AM824 audio payload bytes
/// @param channel_count Total number of audio channels in payload
/// @param sample_count Number of samples per channel
/// @param channel_index Zero-based index of the channel to extract
/// @param output Destination buffer for the extracted channel samples
[[nodiscard]] auto am824_deserialize_channel(
    std::span<uint8_t const> payload,
    uint8_t channel_count,
    uint8_t sample_count,
    uint8_t channel_index,
    std::span<float> output) noexcept -> size_t;

//
// AM824 Audio Serialization - Pack float buffer into AM824 packet payload
//
/// Serialize interleaved float audio samples to AM824 packet payload
/// Returns the number of bytes written
/// @param input Interleaved float audio samples
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param payload Destination buffer for serialized payload bytes
[[nodiscard]] auto am824_serialize_interleaved(
    std::span<float const> input, uint8_t channel_count, uint8_t sample_count, std::span<uint8_t> payload) noexcept -> size_t;

/// Serialize planar (non-interleaved) float audio samples to AM824 packet payload
/// @param channel_buffers Span of per-channel input buffer pointers
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param payload Destination buffer for serialized payload bytes
[[nodiscard]] auto am824_serialize_planar(
    std::span<float const* const> channel_buffers, uint8_t channel_count, uint8_t sample_count, std::span<uint8_t> payload) noexcept
    -> size_t;

//
// High-level packet creation/parsing helpers
//
/// Calculate the required buffer size for an AM824 packet
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
[[nodiscard]] constexpr auto am824_packet_size(uint8_t channel_count, uint8_t sample_count) noexcept -> size_t
{
    return Am824Pdu::HEADER_LENGTH + (static_cast<size_t>(channel_count) * sample_count * Am824Pdu::BYTES_PER_SAMPLE);
}

/// Create a complete AM824 packet with audio data
/// Returns bytes written, or 0 on error
/// @param stream_id The stream ID for this packet
/// @param sequence_num The packet sequence number
/// @param avtp_timestamp The 32-bit AVTP presentation timestamp
/// @param sample_rate The AM824 sample rate code
/// @param data_block_count The current data block count value
/// @param interleaved_audio Interleaved float audio samples to encode
/// @param channel_count Number of audio channels
/// @param sample_count Number of samples per channel
/// @param packet_buffer Destination buffer for the complete packet
[[nodiscard]] auto am824_create_packet(
    StreamId const& stream_id,
    uint8_t sequence_num,
    uint32_t avtp_timestamp,
    Am824SampleRate sample_rate,
    uint8_t data_block_count,
    std::span<float const> interleaved_audio,
    uint8_t channel_count,
    uint8_t sample_count,
    std::span<uint8_t> packet_buffer) noexcept -> size_t;

/// Parse an AM824 packet header
/// Returns pointer to the header if valid, nullptr otherwise
/// Note: The packed Am824Pdu struct has alignment <= 4 bytes. On platforms with strict
/// alignment requirements, ensure the packet buffer is appropriately aligned.
/// @param packet Raw packet data including the AM824 header
[[nodiscard]] auto am824_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<Am824Pdu>;

/// Get audio payload span from a packet
/// @param packet Raw packet data including the AM824 header
[[nodiscard]] auto am824_get_audio_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp

// Serialization traits - Am824Pdu is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::Am824Pdu> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AvtpStreamHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::Cip61883Header> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
