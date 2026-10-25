#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVTP stream format code decoder.
///
/// The IEEE 1722-2016 stream format is an 8-byte value (carried as a u64 or
/// Eui64 on the wire, e.g. in DescriptorStream::current_format). Byte 0 is
/// the AVTP subtype; subsequent bytes are subtype-specific.
///
/// This header provides a decoder that produces a short human-readable
/// description like "AAF 2ch 48kHz 24-bit" or "AM824 MBLA 8ch 48kHz" for
/// common formats. Unknown formats fall back to a hex string.

#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace statusbar::avtp {

/// Convert a u64 stream format code to an 8-byte array (big-endian on wire).
[[nodiscard]] constexpr auto stream_format_bytes(uint64_t fmt) noexcept -> std::array<uint8_t, 8>
{
    return std::bit_cast<std::array<uint8_t, 8>>(ieee::octlet_t{fmt});
}

/// AAF stream-format wire layout (IEEE 1722-2016 Clause 7.3.4), viewed over
/// the 8-byte format code:
///   octet 0:    subtype (0x02 AAF)
///   octet 1:    reserved[7:4] | nsr[3:0]
///   octet 2:    format (AAF sample format code: INT_32=0x02, ...)
///   octet 3:    bit_depth
///   octets 4-7: channels_per_frame[31:22] | samples_per_frame[21:12] | rsv[11:0]
struct AafStreamFormat
{
    ieee::octet_t subtype{0};
    ieee::octet_t nsr_field{0};
    ieee::octet_t format{0};
    ieee::octet_t bit_depth{0};
    ieee::quadlet_t packing{0};

    static constexpr uint32_t CHANNELS_MASK = 0xFFC0'0000U;
    static constexpr unsigned CHANNELS_SHIFT = 22;
    static constexpr uint32_t SAMPLES_MASK = 0x003F'F000U;
    static constexpr unsigned SAMPLES_SHIFT = 12;

    [[nodiscard]] constexpr auto nsr() const noexcept -> uint8_t { return nsr_field.get_bits<uint8_t>(0x0FU); }
    [[nodiscard]] constexpr auto channels_per_frame() const noexcept -> uint16_t
    {
        return packing.get_bits<uint16_t>(CHANNELS_MASK, CHANNELS_SHIFT);
    }
    [[nodiscard]] constexpr auto samples_per_frame() const noexcept -> uint16_t
    {
        return packing.get_bits<uint16_t>(SAMPLES_MASK, SAMPLES_SHIFT);
    }

    /// View a u64 stream-format code as its AAF wire fields.
    [[nodiscard]] static constexpr auto from_u64(uint64_t fmt) noexcept -> AafStreamFormat
    {
        return std::bit_cast<AafStreamFormat>(ieee::octlet_t{fmt});
    }
};

static_assert(sizeof(AafStreamFormat) == 8, "AafStreamFormat must overlay the 8-byte format code");

/// AAF nsr (nominal sample rate) codes → Hz (IEEE 1722-2016 Table 11)
[[nodiscard]] constexpr auto aaf_nsr_to_hz(uint8_t nsr) noexcept -> uint32_t
{
    switch (nsr) {
        case 0x01:
            return 8000;
        case 0x02:
            return 16000;
        case 0x03:
            return 32000;
        case 0x04:
            return 44100;
        case 0x05:
            return 48000;
        case 0x06:
            return 88200;
        case 0x07:
            return 96000;
        case 0x08:
            return 176400;
        case 0x09:
            return 192000;
        case 0x0A:
            return 24000;
        default:
            return 0;
    }
}

/// AM824/IEC 61883-6 SFC (sampling frequency code, IEC 61883-6 Section 10.3.2) → Hz
[[nodiscard]] constexpr auto am824_sfc_to_hz(uint8_t sfc) noexcept -> uint32_t
{
    switch (sfc) {
        case 0:
            return 32000;
        case 1:
            return 44100;
        case 2:
            return 48000;
        case 3:
            return 88200;
        case 4:
            return 96000;
        case 5:
            return 176400;
        case 6:
            return 192000;
        default:
            return 0;
    }
}

/// Decode an AAF stream format. Layout (IEEE 1722-2016 Clause 7.3.4), verified
/// byte-for-byte against a reference device (02 07 02 20 02 00 c0 00 =
/// AAF 96 kHz INT_32 8ch 12 samples/frame):
///   Byte 0: subtype (0x02)
///   Byte 1: reserved[7:4] | nsr[3:0]
///   Byte 2: format (AAF sample format code: INT_32=0x02, ...)
///   Byte 3: bit_depth
///   Bytes 4-7 (32-bit BE): channels_per_frame[31:22] | samples_per_frame[21:12] | rsv[11:0]
[[nodiscard]] inline auto decode_aaf_stream_format(uint64_t fmt) -> std::string
{
    auto const f = AafStreamFormat::from_u64(fmt);
    uint8_t const nsr = f.nsr();
    uint16_t const channels = f.channels_per_frame();
    uint8_t const depth = f.bit_depth.get();
    uint32_t const rate = aaf_nsr_to_hz(nsr);
    if (rate == 0) {
        return std::format("AAF rate?({}) {}ch {}-bit", nsr, channels, depth);
    }
    if (rate % 1000 == 0) {
        return std::format("AAF {}ch {}kHz {}-bit", channels, rate / 1000, depth);
    }
    return std::format("AAF {}ch {}.{}kHz {}-bit", channels, rate / 1000, (rate / 100) % 10, depth);
}

//
// IEC 61883-6 stream format - IEEE 1722-2016 Annex I
//
/// The IEC 61883-6 (AM824) stream-format word (IEEE 1722-2016 Annex I), viewed over the 8 octets:
/// subtype, sf|fmt|r, FDF (evt[7:3] | sfc[2:0]), DBS, b|nb|ut|sc|rsv, then the IEC 60958, MBLA and
/// MIDI|SMPTE label counts. E.g. 0x00a0020840000800 is 48 kHz, non-blocking, 8 MBLA quadlets.
struct Am824StreamFormat
{
    ieee::octet_t subtype{0};               ///< 0x00, IEC 61883/IIDC
    ieee::octet_t sf_fmt{0};                ///< sf[7] | fmt[6:1] | r[0]
    ieee::octet_t fdf{0};                   ///< evt[7:3] | sfc[2:0]
    ieee::octet_t dbs{0};                   ///< quadlets per data block
    ieee::octet_t flags{0};                 ///< b | nb | ut | sc | reserved[3:0]
    ieee::octet_t label_iec_60958_cnt{0};   ///< IEC 60958 quadlets per block
    ieee::octet_t label_mbla_cnt{0};        ///< MBLA quadlets per block
    ieee::octet_t label_midi_smpte_cnt{0};  ///< MIDI[7:4] | SMPTE[3:0] quadlets per block

    static constexpr uint8_t SF_FMT_AM824 = 0xA0U;  ///< sf = 1, fmt = 0x10 (IEC 61883-6), r = 0
    static constexpr uint8_t FLAG_B = 0x80U;        ///< blocking transmission
    static constexpr uint8_t FLAG_NB = 0x40U;       ///< non-blocking transmission
    static constexpr uint8_t FLAG_UT = 0x20U;       ///< up to DBS channels
    static constexpr uint8_t FLAG_SC = 0x10U;       ///< synchronous clock

    /// Whether byte 1 names IEC 61883-6 (sf = 1, fmt = 0x10).
    [[nodiscard]] constexpr auto is_61883_6() const noexcept -> bool { return (sf_fmt.get() & 0xFEU) == SF_FMT_AM824; }
    /// The FDF event type (0 = AM824 data, IEC 61883-6 Section 10.3).
    [[nodiscard]] constexpr auto evt() const noexcept -> uint8_t { return static_cast<uint8_t>(fdf.get() >> 3U); }
    /// The sampling frequency code (IEC 61883-6 Section 10.3.2).
    [[nodiscard]] constexpr auto sfc() const noexcept -> uint8_t { return static_cast<uint8_t>(fdf.get() & 0x07U); }
    /// b: blocking transmission.
    [[nodiscard]] constexpr auto blocking() const noexcept -> bool { return (flags.get() & FLAG_B) != 0; }
    /// nb: non-blocking transmission.
    [[nodiscard]] constexpr auto non_blocking() const noexcept -> bool { return (flags.get() & FLAG_NB) != 0; }
    /// ut: the stream carries up to DBS channels.
    [[nodiscard]] constexpr auto up_to() const noexcept -> bool { return (flags.get() & FLAG_UT) != 0; }
    /// sc: synchronous clock.
    [[nodiscard]] constexpr auto synchronous_clock() const noexcept -> bool { return (flags.get() & FLAG_SC) != 0; }
    /// MIDI quadlets per data block.
    [[nodiscard]] constexpr auto label_midi_cnt() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>(label_midi_smpte_cnt.get() >> 4U);
    }
    /// SMPTE time code quadlets per data block.
    [[nodiscard]] constexpr auto label_smpte_cnt() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>(label_midi_smpte_cnt.get() & 0x0FU);
    }

    /// View a u64 stream-format code as its IEC 61883-6 fields.
    [[nodiscard]] static constexpr auto from_u64(uint64_t fmt) noexcept -> Am824StreamFormat
    {
        return std::bit_cast<Am824StreamFormat>(ieee::octlet_t{fmt});
    }
};

static_assert(sizeof(Am824StreamFormat) == 8, "Am824StreamFormat must overlay the 8-byte format code");
static_assert(offsetof(Am824StreamFormat, fdf) == 2);
static_assert(offsetof(Am824StreamFormat, dbs) == 3);
static_assert(offsetof(Am824StreamFormat, flags) == 4);
static_assert(offsetof(Am824StreamFormat, label_iec_60958_cnt) == 5);
static_assert(offsetof(Am824StreamFormat, label_mbla_cnt) == 6);
static_assert(offsetof(Am824StreamFormat, label_midi_smpte_cnt) == 7);

/// The non-blocking, MBLA-only IEC 61883-6 format word for @p channels at sampling frequency code
/// @p sfc (IEEE 1722-2016 Annex I), with the ut and sc flags as given.
[[nodiscard]] constexpr auto make_am824_stream_format(
    uint8_t const sfc, uint8_t const channels, bool const up_to = false, bool const synchronous_clock = false) noexcept -> uint64_t
{
    uint8_t const flags = static_cast<uint8_t>(
        Am824StreamFormat::FLAG_NB | (up_to ? Am824StreamFormat::FLAG_UT : 0U) |
        (synchronous_clock ? Am824StreamFormat::FLAG_SC : 0U));
    return (uint64_t{Am824StreamFormat::SF_FMT_AM824} << 48U) | (uint64_t{static_cast<uint8_t>(sfc & 0x07U)} << 40U) |
        (uint64_t{channels} << 32U) | (uint64_t{flags} << 24U) | (uint64_t{channels} << 8U);
}

/// Decode an IEC 61883-6 stream format (layout: Am824StreamFormat) as "AM824 <DBS>ch <rate>";
/// a word whose byte 1 is not IEC 61883-6 falls back to hex.
[[nodiscard]] inline auto decode_am824_stream_format(uint64_t fmt) -> std::string
{
    auto const f = Am824StreamFormat::from_u64(fmt);
    if (!f.is_61883_6()) {
        return std::format("0x{:016x}", fmt);
    }
    uint8_t const sfc = f.fdf.get();
    uint8_t const channels = f.dbs.get();
    uint32_t const rate = f.evt() == 0 ? am824_sfc_to_hz(f.sfc()) : 0;
    if (rate == 0) {
        return std::format("AM824 {}ch SFC=0x{:02x}", channels, sfc);
    }
    if (rate % 1000 == 0) {
        return std::format("AM824 {}ch {}kHz", channels, rate / 1000);
    }
    return std::format("AM824 {}ch {}.{}kHz", channels, rate / 1000, (rate / 100) % 10);
}

/// Decode a CRF (Clock Reference Format) stream format — the Milan media
/// clock stream format. Layout per IEEE 1722-2016 (CRF stream format used
/// in the 1722.1 stream_format field):
///   bits 63-56: subtype (0x04)
///   bits 55-52: type (1 = AUDIO_SAMPLE for a media clock stream)
///   bits 51-40: timestamp_interval (events per timestamp)
///   bits 39-32: timestamps_per_pdu
///   bits 31-29: pull (base_frequency multiplier, Table 28)
///   bits 28-0 : base_frequency in Hz
///
/// Example (Milan media clock, matching the Meyer Galaxy reference
/// capture): 0x041060010000bb80 = CRF AUDIO_SAMPLE 48 kHz,
/// timestamp_interval 96, 1 timestamp per PDU, pull x1.0.
[[nodiscard]] inline auto decode_crf_stream_format(uint64_t fmt) -> std::string
{
    uint8_t const type = static_cast<uint8_t>((fmt >> 52) & 0x0FU);
    uint16_t const timestamp_interval = static_cast<uint16_t>((fmt >> 40) & 0xFFFU);
    uint8_t const timestamps_per_pdu = static_cast<uint8_t>((fmt >> 32) & 0xFFU);
    uint8_t const pull = static_cast<uint8_t>((fmt >> 29) & 0x07U);
    uint32_t const base_frequency = static_cast<uint32_t>(fmt & 0x1FFFFFFFU);

    std::string rate;
    if (base_frequency % 1000 == 0) {
        rate = std::format("{}kHz", base_frequency / 1000);
    } else {
        rate = std::format("{}Hz", base_frequency);
    }
    auto out = std::format("CRF {} {} interval={} ts/pdu={}", crf_type_name(type), rate, timestamp_interval, timestamps_per_pdu);
    if (pull != 0) {
        out += std::format(" pull={}", crf_pull_name(pull));
    }
    return out;
}

/// Decode an 8-byte stream format code into a short human-readable string.
/// Recognises AAF, AM824 (IEC 61883-6), and CRF. Falls back to hex for others.
[[nodiscard]] inline auto stream_format_to_string(uint64_t fmt) -> std::string
{
    auto const b = stream_format_bytes(fmt);
    switch (b[0]) {
        case AvtpSubtype::aaf:
            return decode_aaf_stream_format(fmt);
        case AvtpSubtype::iec_61883_iidc:
            return decode_am824_stream_format(fmt);
        case AvtpSubtype::crf:
            return decode_crf_stream_format(fmt);
        default:
            return std::format("0x{:016x}", fmt);
    }
}

}  // namespace statusbar::avtp
