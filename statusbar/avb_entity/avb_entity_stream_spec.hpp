#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_stream_spec.hpp
/// @brief StreamSpec — the entity's stream topology derived from its blob
/// (Entity Construction Kit phase 1; see docs/ENTITY_CONSTRUCTION_KIT.md).
///
/// The declarative entity model already carries everything the data plane
/// needs to shape itself: each STREAM_INPUT/OUTPUT descriptor's
/// current_format word encodes the stream kind (AM824 / AAF / CRF), sample
/// rate, channel count, and CRF timing. Until now every entity re-typed that
/// information as C++ constants (stream index enums, per-class sample rates,
/// per-entity MSRP frame-size math) that had to agree with the blob by hand.
///
/// This header derives it instead:
///   - decode_stream_format()      — structured (not string) decode of one
///                                   IEEE 1722 stream-format word
///   - talker/listener_stream_specs() — the entity's stream table, one
///                                   StreamSpec per STREAM_OUTPUT/INPUT
///                                   descriptor in the blob
///   - srp_max_frame_size()        — the MSRP TSpec frame size implied by a
///                                   format at the Class A cadence
///   - common_audio_sample_rate()  — the single audio rate the data plane's
///                                   one media clock must run at, validated
///                                   across every audio stream in the table

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_stream_format.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::avb_entity {

/// Stream kind decoded from the format word's AVTP subtype byte.
enum class StreamKind : uint8_t
{
    am824,  ///< IEC 61883-6 AM824 (MBLA audio)
    aaf,    ///< AVTP Audio Format
    crf,    ///< Clock Reference Format (media clock, no audio payload)
    other,  ///< unrecognized subtype / non-AM824 61883 format
};

[[nodiscard]] constexpr auto stream_kind_name(StreamKind kind) noexcept -> char const*
{
    switch (kind) {
        case StreamKind::am824:
            return "AM824";
        case StreamKind::aaf:
            return "AAF";
        case StreamKind::crf:
            return "CRF";
        case StreamKind::other:
        default:
            return "other";
    }
}

/// Structured decode of one 8-byte IEEE 1722 stream-format word (the field
/// counterpart of avtp::stream_format_to_string). Audio kinds fill
/// sample_rate_hz/channels; AAF additionally fills aaf_format/bit_depth/
/// aaf_samples_per_frame; CRF fills the crf_* block.
struct StreamFormatFields
{
    StreamKind kind{StreamKind::other};
    uint32_t sample_rate_hz{0};  ///< audio kinds; 0 = unknown rate code
    uint16_t channels{0};        ///< audio kinds
    uint8_t bit_depth{0};        ///< AAF bit_depth byte; AM824 is fixed 24-in-32

    avtp::AafFormat aaf_format{avtp::AafFormat::user_specified};
    uint16_t aaf_samples_per_frame{0};  ///< 0 = unspecified by the format word

    uint8_t crf_type{0};
    uint16_t crf_timestamp_interval{0};
    uint8_t crf_timestamps_per_pdu{0};
    uint8_t crf_pull{0};
    uint32_t crf_base_frequency_hz{0};
};

[[nodiscard]] constexpr auto decode_stream_format(uint64_t fmt) noexcept -> StreamFormatFields
{
    StreamFormatFields f{};
    auto const b = avtp::stream_format_bytes(fmt);
    switch (b[0]) {
        case avtp::AvtpSubtype::aaf: {
            f.kind = StreamKind::aaf;
            f.sample_rate_hz = avtp::aaf_nsr_to_hz(b[1] & 0x0FU);
            f.aaf_format = static_cast<avtp::AafFormat>(b[2]);
            f.bit_depth = b[3];
            uint32_t const tail = (static_cast<uint32_t>(b[4]) << 24) | (static_cast<uint32_t>(b[5]) << 16) |
                (static_cast<uint32_t>(b[6]) << 8) | static_cast<uint32_t>(b[7]);
            f.channels = static_cast<uint16_t>((tail >> 22) & 0x3FFU);
            f.aaf_samples_per_frame = static_cast<uint16_t>((tail >> 12) & 0x3FFU);
            break;
        }
        case avtp::AvtpSubtype::iec_61883_iidc: {
            // Byte 1 must indicate IEC 61883-6 AM824 (sf=1, fmt=0x10); other
            // 61883 formats (e.g. 61883-4 MPEG-TS) decode as `other` rather
            // than being mislabeled (same gate as decode_am824_stream_format).
            if ((b[1] & 0xFEU) != 0xA0U) {
                break;
            }
            f.kind = StreamKind::am824;
            f.sample_rate_hz = avtp::am824_sfc_to_hz(b[2]);
            f.channels = b[3];
            f.bit_depth = 24;
            break;
        }
        case avtp::AvtpSubtype::crf: {
            f.kind = StreamKind::crf;
            f.crf_type = static_cast<uint8_t>((fmt >> 52) & 0x0FU);
            f.crf_timestamp_interval = static_cast<uint16_t>((fmt >> 40) & 0xFFFU);
            f.crf_timestamps_per_pdu = static_cast<uint8_t>((fmt >> 32) & 0xFFU);
            f.crf_pull = static_cast<uint8_t>((fmt >> 29) & 0x07U);
            f.crf_base_frequency_hz = static_cast<uint32_t>(fmt & 0x1FFFFFFFU);
            break;
        }
        default:
            break;
    }
    return f;
}

/// One stream endpoint of the entity, derived from a STREAM_INPUT/OUTPUT
/// descriptor. `index` is the descriptor index — identical to the ACMP
/// talker/listener unique id and to the entity's stream-index convention.
struct StreamSpec
{
    uint16_t index{0};
    uint64_t format_word{0};    ///< the descriptor's current_format, verbatim
    StreamFormatFields format;  ///< decoded current_format
    uint16_t clock_domain_index{0};
    uint16_t avb_interface_index{0};
};

/// Upper bound on streams per direction the data plane composes. The blob may
/// declare more (the spec allows 65535); deriving specs from such a blob is an
/// error rather than a silent truncation.
inline constexpr size_t MAX_ENTITY_STREAMS = 8;

using StreamSpecs = sg14::inplace_vector<StreamSpec, MAX_ENTITY_STREAMS>;

/// The entity's talker (STREAM_OUTPUT) stream table for one configuration.
/// One StreamSpec per descriptor, in index order. Empty result = no talker
/// streams declared. Errors: storage read failure other than not-found, or
/// more than MAX_ENTITY_STREAMS streams.
[[nodiscard]] auto talker_stream_specs(atdecc::aem::DescriptorStorage const& storage, uint16_t config_idx)
    -> StatusValue<StreamSpecs>;

/// The entity's listener (STREAM_INPUT) stream table for one configuration.
[[nodiscard]] auto listener_stream_specs(atdecc::aem::DescriptorStorage const& storage, uint16_t config_idx)
    -> StatusValue<StreamSpecs>;

/// AVB Class A wire cadence (packets per second per stream).
inline constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;

/// Audio samples per Class A packet at @p sample_rate_hz (12 @ 96k, 6 @ 48k).
[[nodiscard]] constexpr auto class_a_samples_per_packet(uint32_t sample_rate_hz) noexcept -> uint32_t
{
    return sample_rate_hz / CLASS_A_PACKETS_PER_SEC;
}

/// The MSRP TSpec max_frame_size implied by @p format at the Class A cadence:
/// L2 payload bytes of the largest AVTP frame the stream can emit. Audio
/// kinds reserve one extra sample per channel over the nominal cadence — the
/// gPTP-paced media clock may emit nominal+1 samples on a wake. CRF is sized
/// exactly from its timestamps-per-PDU. Returns 0 for StreamKind::other or a
/// format with an unknown rate.
[[nodiscard]] auto srp_max_frame_size(StreamFormatFields const& format) noexcept -> uint16_t;

/// The single audio sample rate shared by every AM824/AAF stream in @p specs
/// — the rate the entity's one media clock must run at. CRF streams don't
/// constrain it (a 48 kHz CRF base is routinely advertised under 96 kHz
/// audio). Errors when audio streams disagree or when no audio stream (and
/// no fallback) exists; @p fallback_hz is returned for CRF-only/empty tables
/// when nonzero.
[[nodiscard]] auto common_audio_sample_rate(StreamSpecs const& specs, uint32_t fallback_hz = 0) -> StatusValue<uint32_t>;

/// First stream of @p kind in the table, or nullopt. Replaces the per-entity
/// hardcoded index constants (AM824=0 / AAF=1 / CRF=2 style).
[[nodiscard]] auto find_stream(StreamSpecs const& specs, StreamKind kind) noexcept -> std::optional<uint16_t>;

}  // namespace statusbar::avb_entity
