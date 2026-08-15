#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AAF Version 1 - AVTP Audio Format with version 1 common stream header
/// IEEE 1722-2025 Clause 7 with version 1 header per 4.7.4
///
/// Version 1 improvements over version 0:
///   - sequence_num: 32-bit (was 8-bit) - reliable loss detection over lossy links
///   - avtp_timestamp: 64-bit (was 32-bit) - no 4-second rollover, no reconstruction needed
///   - ptp_grandmaster_identity: 8 bytes - clock domain identification at the transport layer
///
/// Wire format (40-byte header + variable audio payload):
///   Byte 0:      subtype (0x02)
///   Byte 1:      sv[7] | version=1[6:4] | mr[3] | rsv[2:1] | tv[0]
///   Byte 2:      format_specific_data_0 (reserved)
///   Byte 3:      reserved[7:1] | tu[0]
///   Bytes 4-11:  stream_id (8 bytes)
///   Bytes 12-15: sequence_num (32-bit)
///   Bytes 16-23: avtp_timestamp (64-bit PTP nanoseconds)
///   Bytes 24-31: ptp_grandmaster_identity (ClockIdentity, 8 bytes)
///   Byte 32:     format
///   Byte 33:     nsr[7:4] | rsv[3:2] | channels_per_frame[9:8]
///   Byte 34:     channels_per_frame[7:0]
///   Byte 35:     bit_depth[7:0]
///   Bytes 36-37: stream_data_length
///   Byte 38:     rsv[7:5] | sp[4] | evt[3:0]
///   Byte 39:     reserved

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/ieee/ieee_base.hpp"
#include "statusbar/tsn/tsn_clock_identity.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace statusbar::avtp {

using ieee::doublet_t;
using ieee::octet_t;
using ieee::octlet_t;
using ieee::quadlet_t;
using tsn::ClockIdentity;
using tsn::StreamId;

//
// AafV1Pdu - IEEE 1722-2025 Clause 7 with version 1 common stream header (Figure 11)
//

/// AAF PCM PDU Header version 1 - IEEE 1722-2025 Clause 7 (subtype 0x02, version 1)
struct AafV1Pdu
{
    /// Total length of AAF V1 header on wire
    static constexpr size_t LENGTH = 40;
    static constexpr size_t HEADER_LENGTH = LENGTH;

    /// Maximum channels per frame (10-bit field)
    static constexpr uint16_t MAX_CHANNELS = 1023;

    // Bytes 0-3: common header fields

    /// Byte 0: subtype[7:0] = 0x02 for AAF
    octet_t subtype;

    /// Byte 1: sv[7] | version[6:4] | mr[3] | rsv[2:1] | tv[0]
    octet_t sv_version_flags;

    /// Byte 2: format_specific_data_0 (reserved for AAF)
    octet_t format_specific_data_0;

    /// Byte 3: reserved[7:1] | tu[0]
    octet_t reserved_tu;

    // Bytes 4-11: Stream ID

    /// Bytes 4-11: Stream ID
    StreamId stream_id_;

    // Bytes 12-15: sequence_num (32-bit, version 1)

    /// Bytes 12-15: sequence_num (32-bit)
    quadlet_t sequence_num;

    // Bytes 16-23: avtp_timestamp (64-bit, version 1)

    /// Bytes 16-23: avtp_timestamp (64-bit PTP nanoseconds)
    octlet_t avtp_timestamp;

    // Bytes 24-31: ptp_grandmaster_identity

    /// Bytes 24-31: PTP grandmaster ClockIdentity
    ClockIdentity ptp_grandmaster_identity;

    // Bytes 32-35: format_specific_data_2 (AAF: format, nsr, channels, bit_depth)

    /// Byte 32: format field (Table 10)
    octet_t format;

    /// Bytes 33-34: nsr[15:12] | rsv[11:10] | channels_per_frame[9:0]
    doublet_t nsr_rsv_channels;

    /// Byte 35: bit_depth[7:0]
    octet_t bit_depth;

    // Bytes 36-37: stream_data_length

    /// Bytes 36-37: stream_data_length (payload length in octets)
    doublet_t stream_data_length;

    // Bytes 38-39: format_specific_data_3 (AAF: sp, evt, reserved)

    /// Byte 38: rsv[7:5] | sp[4] | evt[3:0]
    octet_t rsv_sp_evt;

    /// Byte 39: reserved
    octet_t reserved;

    // ========== Accessors - Common stream header flags (byte 1) ==========

    /// Get the stream valid (sv) bit
    [[nodiscard]] constexpr auto sv() const noexcept -> bool { return sv_version_flags.has_flag(0x80U); }

    /// Set the stream valid (sv) bit
    constexpr void set_sv(bool const value) noexcept { sv_version_flags.set_flag(0x80U, value); }

    /// Get the version field (bits 6:4) — should be 1 for this struct
    [[nodiscard]] constexpr auto version() const noexcept -> uint8_t { return sv_version_flags.get_bits(0x70U, 4); }

    /// Get the media clock restart (mr) bit
    [[nodiscard]] constexpr auto mr() const noexcept -> bool { return sv_version_flags.has_flag(0x08U); }

    /// Set the media clock restart (mr) bit
    constexpr void set_mr(bool const value) noexcept { sv_version_flags.set_flag(0x08U, value); }

    /// Get the timestamp valid (tv) bit
    [[nodiscard]] constexpr auto tv() const noexcept -> bool { return sv_version_flags.has_flag(0x01U); }

    /// Set the timestamp valid (tv) bit
    constexpr void set_tv(bool const value) noexcept { sv_version_flags.set_flag(0x01U, value); }

    /// Get the timestamp uncertain (tu) bit
    [[nodiscard]] constexpr auto tu() const noexcept -> bool { return reserved_tu.has_flag(0x01U); }

    /// Set the timestamp uncertain (tu) bit
    constexpr void set_tu(bool const value) noexcept { reserved_tu.set_flag(0x01U, value); }

    // ========== Accessors - Stream ID ==========

    /// Get the stream ID
    [[nodiscard]] constexpr auto stream_id() const noexcept -> StreamId { return stream_id_; }

    /// Set the stream ID
    constexpr void set_stream_id(StreamId const& sid) noexcept { stream_id_ = sid; }

    // ========== Accessors - Version 1 specific fields ==========

    /// Get the sequence number (32-bit, version 1)
    [[nodiscard]] constexpr auto get_sequence_num() const noexcept -> uint32_t { return sequence_num.get(); }

    /// Set the sequence number (32-bit)
    constexpr void set_sequence_num(uint32_t const value) noexcept { sequence_num = value; }

    /// Increment sequence number (wraps at 2^32)
    constexpr void increment_sequence_num() noexcept { sequence_num = get_sequence_num() + 1U; }

    /// Get the AVTP timestamp (64-bit PTP nanoseconds, valid when tv==1)
    [[nodiscard]] constexpr auto get_avtp_timestamp() const noexcept -> uint64_t { return avtp_timestamp.get(); }

    /// Set the AVTP timestamp (64-bit)
    constexpr void set_avtp_timestamp(uint64_t const value) noexcept { avtp_timestamp = value; }

    /// Get the PTP grandmaster identity (valid when tv==1)
    [[nodiscard]] constexpr auto get_ptp_grandmaster_identity() const noexcept -> ClockIdentity { return ptp_grandmaster_identity; }

    /// Set the PTP grandmaster identity
    constexpr void set_ptp_grandmaster_identity(ClockIdentity const& gm) noexcept { ptp_grandmaster_identity = gm; }

    // ========== Accessors - AAF format-specific fields ==========

    /// Get the format field
    [[nodiscard]] constexpr auto get_format() const noexcept -> AafFormat { return static_cast<AafFormat>(format.get()); }

    /// Set the format field
    constexpr void set_format(AafFormat const fmt) noexcept { format = static_cast<uint8_t>(fmt); }

    /// Get the nominal sample rate (nsr) field
    [[nodiscard]] constexpr auto nsr() const noexcept -> AafSampleRate
    {
        return static_cast<AafSampleRate>(nsr_rsv_channels.get_bits(0xF000U, 12));
    }

    /// Set the nominal sample rate (nsr) field
    constexpr void set_nsr(AafSampleRate const rate) noexcept
    {
        nsr_rsv_channels.set_bits(0xF000U, 12, static_cast<uint16_t>(rate));
    }

    /// Get channels_per_frame (10-bit field)
    [[nodiscard]] constexpr auto channels_per_frame() const noexcept -> uint16_t
    {
        return nsr_rsv_channels.get_bits<uint16_t>(0x03FFU);
    }

    /// Set channels_per_frame (10-bit field, max 1023)
    constexpr void set_channels_per_frame(uint16_t channels) noexcept
    {
        // set_bits masks to the 10-bit field, preserving nsr and rsv
        nsr_rsv_channels.set_bits(0x03FFU, 0, channels);
    }

    /// Get bit_depth field
    [[nodiscard]] constexpr auto get_bit_depth() const noexcept -> uint8_t { return bit_depth.get(); }

    /// Set bit_depth field
    constexpr void set_bit_depth(uint8_t const depth) noexcept { bit_depth = depth; }

    /// Get stream_data_length (payload length in octets)
    [[nodiscard]] constexpr auto get_stream_data_length() const noexcept -> uint16_t { return stream_data_length.get(); }

    /// Set stream_data_length
    constexpr void set_stream_data_length(uint16_t const length) noexcept { stream_data_length = length; }

    /// Get the sparse timestamp (sp) bit
    [[nodiscard]] constexpr auto sp() const noexcept -> bool { return rsv_sp_evt.has_flag(0x10U); }

    /// Set the sparse timestamp (sp) bit
    constexpr void set_sp(bool const value) noexcept { rsv_sp_evt.set_flag(0x10U, value); }

    /// Get the evt field (4 bits)
    [[nodiscard]] constexpr auto evt() const noexcept -> uint8_t { return rsv_sp_evt.get_bits(0x0FU, 0); }

    /// Set the evt field (4 bits)
    constexpr void set_evt(uint8_t const value) noexcept { rsv_sp_evt.set_bits(0x0FU, 0, value); }

    // ========== Computed accessors ==========

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
    constexpr void set_dimensions(uint16_t const samples, uint16_t const channels) noexcept
    {
        set_channels_per_frame(channels);
        size_t const bytes_per_sample = aaf_bytes_per_sample(get_format());
        uint16_t const payload_size = static_cast<uint16_t>(static_cast<size_t>(samples) * channels * bytes_per_sample);
        set_stream_data_length(payload_size);
    }

    // ========== Initialization ==========

    /// Initialize for AAF PCM audio stream with version 1 header
    constexpr void init(
        StreamId const& sid, AafFormat const fmt, AafSampleRate const rate, uint16_t const channels, uint8_t const depth) noexcept
    {
        subtype = AvtpSubtype::aaf;
        sv_version_flags = 0x90U;  // sv=1, version=1
        format_specific_data_0 = 0U;
        reserved_tu = 0U;
        set_stream_id(sid);
        sequence_num = 0U;
        avtp_timestamp = 0U;
        ptp_grandmaster_identity = ClockIdentity{};
        set_format(fmt);
        set_nsr(rate);
        set_channels_per_frame(channels);
        set_bit_depth(depth);
        stream_data_length = 0U;
        rsv_sp_evt = 0U;
        reserved = 0U;
    }

    // ========== Validation ==========

    /// Check if this is a valid AAF version 1 packet
    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool
    {
        if (subtype != AvtpSubtype::aaf) {
            return false;
        }
        if (!sv()) {
            return false;
        }
        if (version() != 1) {
            return false;
        }
        auto const fmt = get_format();
        if (!is_valid_aaf_pcm_format(fmt)) {
            return false;
        }
        if (channels_per_frame() == 0) {
            return false;
        }
        if (fmt == AafFormat::float_32bit && get_bit_depth() != 32) {
            return false;
        }
        return true;
    }

    auto operator<=>(AafV1Pdu const& rhs) const noexcept -> std::strong_ordering = default;
};

// Compile-time layout verification
static_assert(sizeof(AafV1Pdu) == 40, "AafV1Pdu must be exactly 40 bytes");
static_assert(alignof(AafV1Pdu) <= 4, "AafV1Pdu alignment must not exceed 4 bytes");
static_assert(offsetof(AafV1Pdu, subtype) == 0, "subtype must be at offset 0");
static_assert(offsetof(AafV1Pdu, sv_version_flags) == 1, "sv_version_flags must be at offset 1");
static_assert(offsetof(AafV1Pdu, format_specific_data_0) == 2, "format_specific_data_0 must be at offset 2");
static_assert(offsetof(AafV1Pdu, reserved_tu) == 3, "reserved_tu must be at offset 3");
static_assert(offsetof(AafV1Pdu, stream_id_) == 4, "stream_id must be at offset 4");
static_assert(offsetof(AafV1Pdu, sequence_num) == 12, "sequence_num must be at offset 12");
static_assert(offsetof(AafV1Pdu, avtp_timestamp) == 16, "avtp_timestamp must be at offset 16");
static_assert(offsetof(AafV1Pdu, ptp_grandmaster_identity) == 24, "ptp_grandmaster_identity must be at offset 24");
static_assert(offsetof(AafV1Pdu, format) == 32, "format must be at offset 32");
static_assert(offsetof(AafV1Pdu, stream_data_length) == 36, "stream_data_length must be at offset 36");
static_assert(offsetof(AafV1Pdu, rsv_sp_evt) == 38, "rsv_sp_evt must be at offset 38");

}  // namespace statusbar::avtp

// Serialization trait
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::avtp::AafV1Pdu> : std::true_type
{};

namespace statusbar::avtp {

using protocol::load_unchecked;
using protocol::store_unchecked;

//
// Parse/create helpers
//

/// Parse an AAF V1 header from a packet
/// @param packet Raw packet data (must have subtype==AAF and version==1)
[[nodiscard]] auto aaf_v1_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<AafV1Pdu>;

/// Get audio payload span from an AAF V1 packet
/// @param packet Raw packet data including the AAF V1 header
[[nodiscard]] auto aaf_v1_get_audio_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>;

}  // namespace statusbar::avtp
