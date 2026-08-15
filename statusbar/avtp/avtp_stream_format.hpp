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

/// AM824/IEC 61883-6 SFC (sampling frequency code) → Hz
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

/// Decode an AM824/IEC 61883-6 stream format.
/// Layout per IEEE 1722.1 Annex A.5:
///   Byte 0: subtype (0x00 iec_61883_iidc)
///   Byte 1: sf[7] | fmt[6:1] | other — sf=1 + fmt=0x10 indicates IEC 61883-6 (AM824)
///   Byte 2: SFC (sampling frequency code, whole byte) — e.g. 0x02 = 48 kHz
///   Byte 3: DBS (data block size = channel count)
///   Byte 4: b[7] | nb[6] | reserved[5:2] | sph[1] | reserved[0]
///   Byte 5: label
///   Bytes 6-7: reserved
///
/// Example from the audio interface: 0x00a0020840000800
///   byte 2 = 0x02 → SFC 48 kHz
///   byte 3 = 0x08 → 8-channel AM824
[[nodiscard]] inline auto decode_am824_stream_format(uint64_t fmt) -> std::string
{
    auto const b = stream_format_bytes(fmt);
    // Byte 1 must actually indicate IEC 61883-6 AM824 (sf=1, fmt=0x10 ->
    // 0xA0 in bits 7:1); other 61883 formats (e.g. 61883-4 MPEG-TS) fall
    // back to hex rather than being mislabeled as AM824.
    if ((b[1] & 0xFEU) != 0xA0U) {
        return std::format("0x{:016x}", fmt);
    }
    uint8_t const sfc = b[2];
    uint8_t const channels = b[3];
    uint32_t const rate = am824_sfc_to_hz(sfc);
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
