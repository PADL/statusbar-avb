#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// CRF - Clock Reference Format - IEEE 1722-2016 Section 10
/// Modernized C++23 implementation for clock reference distribution

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
// CRF Type Values - IEEE 1722-2016 Table 26
//
/// CRF type enumeration - indicates the type of timestamp
enum class CrfType : uint8_t
{
    user = 0x00U,           ///< User specified (10.4.13.2)
    audio_sample = 0x01U,   ///< Audio sample timestamp (10.4.13.3)
    video_frame = 0x02U,    ///< Video frame sync timestamp (10.4.13.4)
    video_line = 0x03U,     ///< Video line sync timestamp (10.4.13.5)
    machine_cycle = 0x04U,  ///< Machine cycle timestamp (10.4.13.6)
    // 0x05 - 0xFF: Reserved
};

/// Legacy CRF type constants for compatibility
constexpr uint8_t CRF_TYPE_USER = 0x00U;
constexpr uint8_t CRF_TYPE_AUDIO_SAMPLE = 0x01U;
constexpr uint8_t CRF_TYPE_VIDEO_FRAME = 0x02U;
constexpr uint8_t CRF_TYPE_VIDEO_LINE = 0x03U;
constexpr uint8_t CRF_TYPE_MACHINE_CYCLE = 0x04U;

/// Get human-readable name for CRF type
/// @param type The CRF type value (0x00-0x04)
[[nodiscard]] auto crf_type_name(uint8_t type) noexcept -> std::string_view;

/// Get human-readable name for CRF type enum
/// @param type The CRF type enum value
[[nodiscard]] auto crf_type_name(CrfType type) noexcept -> std::string_view;

//
// CRF Pull Field Values - IEEE 1722-2016 Table 27
//
/// CRF pull field enumeration - frequency multiplier modifier
enum class CrfPull : uint8_t
{
    multiply_1_0 = 0x00U,         ///< Multiply base_frequency by 1.0
    multiply_1_div_1001 = 0x01U,  ///< Multiply base_frequency by 1/1.001
    multiply_1001 = 0x02U,        ///< Multiply base_frequency by 1.001
    multiply_24_div_25 = 0x03U,   ///< Multiply base_frequency by 24/25
    multiply_25_div_24 = 0x04U,   ///< Multiply base_frequency by 25/24
    multiply_1_div_8 = 0x05U,     ///< Multiply base_frequency by 1/8
    // 0x06 - 0x07: Reserved
};

/// Legacy CRF pull constants for compatibility
constexpr uint8_t CRF_PULL_MULT_1_0 = 0x00U;
constexpr uint8_t CRF_PULL_MULT_1_DIV_1001 = 0x01U;
constexpr uint8_t CRF_PULL_MULT_1001 = 0x02U;
constexpr uint8_t CRF_PULL_MULT_24_DIV_25 = 0x03U;
constexpr uint8_t CRF_PULL_MULT_25_DIV_24 = 0x04U;
constexpr uint8_t CRF_PULL_MULT_1_DIV_8 = 0x05U;

/// Get human-readable name for pull field value
/// @param pull The CRF pull field value (0x00-0x05)
[[nodiscard]] auto crf_pull_name(uint8_t pull) noexcept -> std::string_view;

/// Get human-readable name for CrfPull enum
/// @param pull The CRF pull enum value
[[nodiscard]] auto crf_pull_name(CrfPull pull) noexcept -> std::string_view;

/// Calculate actual frequency from base_frequency and pull field
/// Returns the nominal frequency in Hz as a double
/// @param base_frequency The base frequency in Hz (29-bit field)
/// @param pull The CRF pull multiplier value (0x00-0x05)
[[nodiscard]] constexpr auto crf_calculate_frequency(uint32_t const base_frequency, uint8_t const pull) noexcept -> double
{
    double const base = static_cast<double>(base_frequency);
    switch (static_cast<CrfPull>(pull)) {
        case CrfPull::multiply_1_0:
            return base;
        case CrfPull::multiply_1_div_1001:
            return base / 1.001;
        case CrfPull::multiply_1001:
            return base * 1.001;
        case CrfPull::multiply_24_div_25:
            return base * 24.0 / 25.0;
        case CrfPull::multiply_25_div_24:
            return base * 25.0 / 24.0;
        case CrfPull::multiply_1_div_8:
            return base / 8.0;
        default:
            return base;  // Reserved values treated as 1.0
    }
}

/// Calculate actual frequency from base_frequency and CrfPull enum
/// @param base_frequency The base frequency in Hz (29-bit field)
/// @param pull The CRF pull enum value
[[nodiscard]] constexpr auto crf_calculate_frequency(uint32_t const base_frequency, CrfPull const pull) noexcept -> double
{
    return crf_calculate_frequency(base_frequency, static_cast<uint8_t>(pull));
}

//
// CrfPdu - IEEE 1722-2016 Section 10.4 (Figure 72)
// Wire format: 20 byte header + variable crf_data containing 64-bit timestamps
//
/// CRF Protocol Data Unit - Header only (without variable timestamp data)
/// Packed structure matching IEEE 1722 wire format
struct CrfPdu
{
    /// Total length of CrfPdu on wire (header only, timestamps are variable)
    static constexpr size_t LENGTH = 20;

    /// CRF header length (fixed portion) - alias for LENGTH
    static constexpr size_t HEADER_LENGTH = 20;

    /// Size of each CRF timestamp in bytes
    static constexpr size_t TIMESTAMP_SIZE = 8;

    // Bit masks and shifts for sv_version_mr_r_fs_tu field (byte 1)
    static constexpr uint8_t SV_FLAG = 0x80U;       ///< sv bit mask
    static constexpr uint8_t VERSION_MASK = 0x70U;  ///< version field mask
    static constexpr unsigned VERSION_SHIFT = 4;    ///< version field shift
    static constexpr uint8_t MR_FLAG = 0x08U;       ///< mr bit mask
    static constexpr uint8_t FS_FLAG = 0x02U;       ///< fs bit mask
    static constexpr uint8_t TU_FLAG = 0x01U;       ///< tu bit mask

    // Bit masks and shifts for pull_base_frequency field (bytes 12-15)
    static constexpr uint32_t PULL_MASK = 0xE0000000U;            ///< pull field mask (3 bits)
    static constexpr unsigned PULL_SHIFT = 29;                    ///< pull field shift
    static constexpr uint32_t BASE_FREQUENCY_MASK = 0x1FFFFFFFU;  ///< base_frequency field mask (29 bits)

    // Subtype data (4 bytes) - IEEE 1722-2016 Section 10.4 / Figure 72

    /// Byte 0: subtype[7:0] - shall be CRF (0x04)
    octet_t subtype;

    /// Byte 1: sv[7] | version[6:4] | mr[3] | r[2] | fs[1] | tu[0]
    octet_t sv_version_mr_r_fs_tu;

    /// Byte 2: sequence_num[7:0]
    octet_t sequence_num;

    /// Byte 3: type[7:0]
    octet_t type;

    // Stream ID (8 bytes) - IEEE 1722-2016 Section 10.4.8

    /// Bytes 4-11: Stream ID
    StreamId stream_id_;

    // Packet Info (8 bytes) - IEEE 1722-2016 Section 10.4.9-10.4.12

    /// Bytes 12-15: pull[31:29] | base_frequency[28:0]
    quadlet_t pull_base_frequency;

    /// Bytes 16-17: crf_data_length
    doublet_t crf_data_length_;

    /// Bytes 18-19: timestamp_interval
    doublet_t timestamp_interval_;

    // Accessors for subtype_data fields (byte 1)

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return sv_version_mr_r_fs_tu.has_flag(SV_FLAG); }

    /// Set the stream valid (sv) bit
    /// @param valid True to set sv, false to clear
    constexpr void set_sv(bool const valid) noexcept { sv_version_mr_r_fs_tu.set_flag(SV_FLAG, valid); }

    /// Get the version field (3 bits)
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t
    {
        return sv_version_mr_r_fs_tu.get_bits(VERSION_MASK, VERSION_SHIFT);
    }

    /// Set the version field (3 bits)
    /// @param ver The version value (3 bits)
    constexpr void set_version(uint8_t const ver) noexcept { sv_version_mr_r_fs_tu.set_bits(VERSION_MASK, VERSION_SHIFT, ver); }

    /// Get the media clock restart (mr) bit
    [[nodiscard]] constexpr auto mr() const noexcept -> bool { return sv_version_mr_r_fs_tu.has_flag(MR_FLAG); }

    /// Set the media clock restart (mr) bit
    /// @param restart True to set mr, false to clear
    constexpr void set_mr(bool const restart) noexcept { sv_version_mr_r_fs_tu.set_flag(MR_FLAG, restart); }

    /// Get the frame sync (fs) bit
    [[nodiscard]] constexpr auto fs() const noexcept -> bool { return sv_version_mr_r_fs_tu.has_flag(FS_FLAG); }

    /// Set the frame sync (fs) bit
    /// @param frame_sync True to set fs, false to clear
    constexpr void set_fs(bool const frame_sync) noexcept { sv_version_mr_r_fs_tu.set_flag(FS_FLAG, frame_sync); }

    /// Get the timestamp uncertain (tu) bit
    [[nodiscard]] constexpr auto tu() const noexcept -> bool { return sv_version_mr_r_fs_tu.has_flag(TU_FLAG); }

    /// Set the timestamp uncertain (tu) bit
    /// @param uncertain True to set tu, false to clear
    constexpr void set_tu(bool const uncertain) noexcept { sv_version_mr_r_fs_tu.set_flag(TU_FLAG, uncertain); }

    // Accessors for sequence_num and type fields

    /// Get the sequence number
    [[nodiscard]] constexpr auto get_sequence_num() const noexcept -> uint8_t { return sequence_num; }

    /// Set the sequence number
    /// @param seq The sequence number (0-255)
    constexpr void set_sequence_num(uint8_t const seq) noexcept { sequence_num = seq; }

    /// Get the CRF type
    [[nodiscard]] constexpr auto get_type() const noexcept -> uint8_t { return type; }

    /// Get the CRF type as enum
    [[nodiscard]] constexpr auto get_crf_type() const noexcept -> CrfType { return static_cast<CrfType>(type.get()); }

    /// Set the CRF type
    /// @param t The CRF type value
    constexpr void set_type(uint8_t const t) noexcept { type = t; }

    /// Set the CRF type from enum
    /// @param t The CRF type enum value
    constexpr void set_type(CrfType const t) noexcept { type = static_cast<uint8_t>(t); }

    // Accessors for Stream ID

    /// Get the stream ID
    [[nodiscard]] constexpr auto stream_id() const noexcept -> StreamId { return stream_id_; }

    /// Set the stream ID
    /// @param sid The stream ID to set
    constexpr void set_stream_id(StreamId const& sid) noexcept { stream_id_ = sid; }

    // Accessors for Packet Info fields

    /// Get the pull field (3 bits)
    [[nodiscard]] constexpr auto pull() const noexcept -> uint8_t
    {
        return pull_base_frequency.get_bits<uint8_t>(PULL_MASK, PULL_SHIFT);
    }

    /// Get the pull field as enum
    [[nodiscard]] constexpr auto get_pull() const noexcept -> CrfPull { return static_cast<CrfPull>(pull()); }

    /// Set the pull field (3 bits)
    /// @param p The pull multiplier value (3 bits)
    constexpr void set_pull(uint8_t const p) noexcept { pull_base_frequency.set_bits(PULL_MASK, PULL_SHIFT, p); }

    /// Set the pull field from enum
    /// @param p The CRF pull enum value
    constexpr void set_pull(CrfPull const p) noexcept { set_pull(static_cast<uint8_t>(p)); }

    /// Get the base_frequency field (29 bits)
    [[nodiscard]] constexpr auto base_frequency() const noexcept -> uint32_t
    {
        return pull_base_frequency.get_bits(BASE_FREQUENCY_MASK, 0);
    }

    /// Set the base_frequency field (29 bits, max 536870911 Hz)
    /// @param freq The base frequency in Hz (29 bits)
    constexpr void set_base_frequency(uint32_t const freq) noexcept { pull_base_frequency.set_bits(BASE_FREQUENCY_MASK, 0, freq); }

    /// Get the calculated nominal frequency (base_frequency * pull multiplier)
    [[nodiscard]] constexpr auto nominal_frequency() const noexcept -> double
    {
        return crf_calculate_frequency(base_frequency(), pull());
    }

    /// Get the crf_data_length field (length of timestamp data in bytes)
    [[nodiscard]] constexpr auto crf_data_length() const noexcept -> uint16_t { return crf_data_length_; }

    /// Set the crf_data_length field
    /// @param len Length of timestamp data in bytes
    constexpr void set_crf_data_length(uint16_t const len) noexcept { crf_data_length_ = len; }

    /// Get the number of timestamps in the crf_data field
    [[nodiscard]] constexpr auto timestamp_count() const noexcept -> uint16_t { return crf_data_length() / TIMESTAMP_SIZE; }

    /// Get the timestamp_interval field
    [[nodiscard]] constexpr auto timestamp_interval() const noexcept -> uint16_t { return timestamp_interval_; }

    /// Set the timestamp_interval field
    /// @param interval The timestamp interval value
    constexpr void set_timestamp_interval(uint16_t const interval) noexcept { timestamp_interval_ = interval; }

    // Type check helpers

    /// Check if this is a User type CRF
    [[nodiscard]] constexpr auto is_user_type() const noexcept -> bool { return get_crf_type() == CrfType::user; }

    /// Check if this is an Audio Sample type CRF
    [[nodiscard]] constexpr auto is_audio_sample_type() const noexcept -> bool { return get_crf_type() == CrfType::audio_sample; }

    /// Check if this is a Video Frame type CRF
    [[nodiscard]] constexpr auto is_video_frame_type() const noexcept -> bool { return get_crf_type() == CrfType::video_frame; }

    /// Check if this is a Video Line type CRF
    [[nodiscard]] constexpr auto is_video_line_type() const noexcept -> bool { return get_crf_type() == CrfType::video_line; }

    /// Check if this is a Machine Cycle type CRF
    [[nodiscard]] constexpr auto is_machine_cycle_type() const noexcept -> bool { return get_crf_type() == CrfType::machine_cycle; }

    // Initialization

    /// Initialize as an Audio Sample CRF
    /// @param sid The stream ID
    /// @param base_freq The base frequency in Hz
    /// @param pull_val The frequency pull multiplier
    /// @param ts_interval The timestamp interval
    /// @param num_timestamps Number of timestamps in the packet
    constexpr void init_audio_sample(
        StreamId const& sid,
        uint32_t const base_freq,
        CrfPull const pull_val,
        uint16_t const ts_interval,
        uint16_t const num_timestamps) noexcept
    {
        subtype = AvtpSubtype::crf;
        sv_version_mr_r_fs_tu = 0x80U;  // sv=1, version=0, mr=0, r=0, fs=0, tu=0
        sequence_num = 0;
        type = static_cast<uint8_t>(CrfType::audio_sample);
        set_stream_id(sid);
        set_pull(pull_val);
        set_base_frequency(base_freq);
        set_crf_data_length(num_timestamps * TIMESTAMP_SIZE);
        set_timestamp_interval(ts_interval);
    }

    /// Initialize as a Video Frame CRF
    /// @param sid The stream ID
    /// @param base_freq The base frequency in Hz
    /// @param pull_val The frequency pull multiplier
    /// @param ts_interval The timestamp interval
    /// @param num_timestamps Number of timestamps in the packet
    constexpr void init_video_frame(
        StreamId const& sid,
        uint32_t const base_freq,
        CrfPull const pull_val,
        uint16_t const ts_interval,
        uint16_t const num_timestamps) noexcept
    {
        subtype = AvtpSubtype::crf;
        sv_version_mr_r_fs_tu = 0x80U;  // sv=1, version=0, mr=0, r=0, fs=0, tu=0
        sequence_num = 0;
        type = static_cast<uint8_t>(CrfType::video_frame);
        set_stream_id(sid);
        set_pull(pull_val);
        set_base_frequency(base_freq);
        set_crf_data_length(num_timestamps * TIMESTAMP_SIZE);
        set_timestamp_interval(ts_interval);
    }

    /// Initialize as a Video Line CRF
    /// @param sid The stream ID
    /// @param base_freq The base frequency in Hz
    /// @param pull_val The frequency pull multiplier
    /// @param ts_interval The timestamp interval
    /// @param num_timestamps Number of timestamps in the packet
    /// @param frame_sync True to set the frame sync (fs) bit
    constexpr void init_video_line(
        StreamId const& sid,
        uint32_t const base_freq,
        CrfPull const pull_val,
        uint16_t const ts_interval,
        uint16_t const num_timestamps,
        bool const frame_sync = false) noexcept
    {
        subtype = AvtpSubtype::crf;
        sv_version_mr_r_fs_tu = 0x80U;  // sv=1, version=0, mr=0, r=0, fs=0, tu=0
        if (frame_sync) {
            set_fs(true);
        }
        sequence_num = 0;
        type = static_cast<uint8_t>(CrfType::video_line);
        set_stream_id(sid);
        set_pull(pull_val);
        set_base_frequency(base_freq);
        set_crf_data_length(num_timestamps * TIMESTAMP_SIZE);
        set_timestamp_interval(ts_interval);
    }

    /// Initialize as a Machine Cycle CRF
    /// @param sid The stream ID
    /// @param base_freq The base frequency in Hz
    /// @param pull_val The frequency pull multiplier
    /// @param ts_interval The timestamp interval
    /// @param num_timestamps Number of timestamps in the packet
    constexpr void init_machine_cycle(
        StreamId const& sid,
        uint32_t const base_freq,
        CrfPull const pull_val,
        uint16_t const ts_interval,
        uint16_t const num_timestamps) noexcept
    {
        subtype = AvtpSubtype::crf;
        sv_version_mr_r_fs_tu = 0x80U;  // sv=1, version=0, mr=0, r=0, fs=0, tu=0
        sequence_num = 0;
        type = static_cast<uint8_t>(CrfType::machine_cycle);
        set_stream_id(sid);
        set_pull(pull_val);
        set_base_frequency(base_freq);
        set_crf_data_length(num_timestamps * TIMESTAMP_SIZE);
        set_timestamp_interval(ts_interval);
    }

    // Validation

    /// Check if this is a valid CRF packet
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        // Check subtype
        if (subtype != AvtpSubtype::crf) {
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
        // Check type is valid (0-4)
        if (type.get() > 0x04U) {
            return false;
        }
        return true;
    }

    auto operator<=>(CrfPdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(CrfPdu) == 20, "CrfPdu must be exactly 20 bytes");
static_assert(alignof(CrfPdu) <= 4, "CrfPdu alignment must not exceed 4 bytes");
static_assert(offsetof(CrfPdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(CrfPdu, sv_version_mr_r_fs_tu) == 1, "sv_version_mr_r_fs_tu must be at offset 1");
static_assert(offsetof(CrfPdu, sequence_num) == 2, "sequence_num must be at offset 2");
static_assert(offsetof(CrfPdu, type) == 3, "type must be at offset 3");
static_assert(offsetof(CrfPdu, stream_id_) == 4, "stream_id_ must be at offset 4");
static_assert(offsetof(CrfPdu, pull_base_frequency) == 12, "pull_base_frequency must be at offset 12");
static_assert(offsetof(CrfPdu, crf_data_length_) == 16, "crf_data_length_ must be at offset 16");
static_assert(offsetof(CrfPdu, timestamp_interval_) == 18, "timestamp_interval_ must be at offset 18");

//
// CRF Parsing Helpers
//
/// Parse a CRF header from a payload span
/// Returns nullptr if payload is too small or header is invalid
/// Note: The packed CrfPdu struct has alignment <= 4 bytes. On platforms with strict
/// alignment requirements, ensure the payload buffer is appropriately aligned.
/// @param payload Raw packet data including the CRF header
[[nodiscard]] auto crf_parse_header(std::span<uint8_t const> payload) noexcept -> std::optional<CrfPdu>;

/// Get the CRF timestamp data span from a payload
/// Returns empty span if payload doesn't have enough data
/// @param payload Raw packet data including the CRF header
[[nodiscard]] auto crf_get_timestamp_data(std::span<uint8_t const> payload) noexcept -> std::span<uint8_t const>;

/// Extract a 64-bit timestamp from timestamp data at the given index
/// Returns std::nullopt if index is out of bounds
/// @param timestamp_data Raw timestamp data bytes from the CRF packet
/// @param index Zero-based index of the timestamp to extract
[[nodiscard]] auto crf_get_timestamp(std::span<uint8_t const> timestamp_data, size_t index) noexcept -> std::optional<uint64_t>;

/// Store a 64-bit timestamp into a buffer at the given index
/// Returns true on success, false if index is out of bounds
/// @param timestamp_data Mutable timestamp data buffer
/// @param index Zero-based index of the timestamp slot to write
/// @param timestamp The 64-bit timestamp value to store
[[nodiscard]] auto crf_set_timestamp(std::span<uint8_t> timestamp_data, size_t index, uint64_t timestamp) noexcept -> bool;

}  // namespace statusbar::avtp

// Serialization traits - CrfPdu is a packed wire format struct
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::CrfPdu> : std::true_type
{};

// Export using declarations for ADL to find the template functions
namespace statusbar::avtp {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::avtp
