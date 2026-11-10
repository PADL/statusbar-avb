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
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_am824_v1.hpp"
#include "statusbar/avtp/avtp_stream_format.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/net/net_interface_info.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::avb_entity {

/// Stream kind decoded from the format word's AVTP subtype byte.
enum class StreamKind : uint8_t
{
    am824,  ///< IEC 61883-6 AM824 (MBLA audio)
    aaf,    ///< AVTP Audio Format
    crf,    ///< Clock Reference Format (media clock, no audio payload)
    other,  ///< unrecognized subtype, or a 61883 format the kit does not carry (am824_format_supported)
};

[[nodiscard]] constexpr auto stream_kind_name(StreamKind kind) noexcept -> std::string_view
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

/// Whether @p kind carries audio (AAF or AM824): an audio stream may change between the two.
[[nodiscard]] constexpr auto is_audio_kind(StreamKind const kind) noexcept -> bool
{
    return kind == StreamKind::aaf || kind == StreamKind::am824;
}

/// Whether a stream of kind @p from may adopt a format of kind @p to: its own kind, or AAF and AM824
/// either way. CRF never changes kind.
[[nodiscard]] constexpr auto kind_change_allowed(StreamKind const from, StreamKind const to) noexcept -> bool
{
    return from == to || (is_audio_kind(from) && is_audio_kind(to));
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

/// An Ethernet frame's largest payload, its MAC client data (IEEE 802.3-2022 Clause 3.2.7).
inline constexpr size_t ETHERNET_PAYLOAD_MAX = 1500;

/// Whether an AM824 frame of @p channels at @p rate_hz fits the MTU: a class A packet's samples
/// and one more, as the talker may send (srp_max_frame_size).
[[nodiscard]] constexpr auto am824_frame_fits(uint32_t const channels, uint32_t const rate_hz) noexcept -> bool
{
    auto const samples = static_cast<uint8_t>((rate_hz / 8000U) + 1U);
    return avtp::am824_packet_size(static_cast<uint8_t>(channels), samples) <= ETHERNET_PAYLOAD_MAX;
}

/// Whether the kit carries @p format (IEEE 1722-2016 Annex I): IEC 61883-6 AM824 data, non-blocking,
/// MBLA only (DBS = MBLA count, no IEC 60958/MIDI/SMPTE), 32-192 kHz, 1 to Am824Pdu::MAX_CHANNELS in the MTU.
[[nodiscard]] constexpr auto am824_format_supported(avtp::Am824StreamFormat const& format) noexcept -> bool
{
    return format.is_61883_6() && format.evt() == 0 && avtp::am824_sfc_to_hz(format.sfc()) != 0 && format.non_blocking() &&
        !format.blocking() && format.label_iec_60958_cnt.get() == 0 && format.label_midi_cnt() == 0 &&
        format.label_smpte_cnt() == 0 && format.label_mbla_cnt.get() == format.dbs.get() && format.dbs.get() >= 1 &&
        format.dbs.get() <= avtp::Am824Pdu::MAX_CHANNELS && am824_frame_fits(format.dbs.get(), avtp::am824_sfc_to_hz(format.sfc()));
}

[[nodiscard]] constexpr auto decode_stream_format(uint64_t fmt) noexcept -> StreamFormatFields
{
    StreamFormatFields f{};
    auto const b = avtp::stream_format_bytes(fmt);
    switch (b[0]) {
        case avtp::AvtpSubtype::aaf: {
            auto const aaf = avtp::AafStreamFormat::from_u64(fmt);
            f.kind = StreamKind::aaf;
            f.sample_rate_hz = avtp::aaf_nsr_to_hz(aaf.nsr());
            f.aaf_format = static_cast<avtp::AafFormat>(aaf.format.get());
            f.bit_depth = aaf.bit_depth.get();
            f.channels = aaf.channels_per_frame();
            f.aaf_samples_per_frame = aaf.samples_per_frame();
            break;
        }
        case avtp::AvtpSubtype::iec_61883_iidc: {
            auto const am824 = avtp::Am824StreamFormat::from_u64(fmt);
            if (!am824_format_supported(am824)) {
                break;
            }
            f.kind = StreamKind::am824;
            f.sample_rate_hz = avtp::am824_sfc_to_hz(am824.sfc());
            f.channels = am824.dbs.get();
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

/// Audio (AM824/AAF) streams per direction the data plane is sized for: 16
/// talker and 16 listener streams.
inline constexpr size_t MAX_ENTITY_AUDIO_STREAMS = 16;

/// Clock-reference (CRF) streams per direction beside the audio: one media
/// clock output (Milan v1.2 Section 7.3) and one media clock input per
/// CLOCK_DOMAIN, up to AvbEntityHost::MAX_CLOCK_DOMAINS domains.
inline constexpr size_t MAX_ENTITY_CRF_STREAMS = 4;

/// Upper bound on the descriptors of one direction the data plane composes:
/// 16 audio + 4 CRF. A blob declaring more is an error, not a truncation.
/// What it sizes: docs/AVB_ENTITY_KIT.md.
inline constexpr size_t MAX_ENTITY_STREAMS = MAX_ENTITY_AUDIO_STREAMS + MAX_ENTITY_CRF_STREAMS;

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
/// The 44.1 kHz family's Class A AAF packets per second: one per 136 us (IEEE 1722-2016 Table 17).
inline constexpr uint32_t CLASS_A_44_1_KHZ_PACKETS_PER_SEC = 7350;

/// Audio samples per Class A AAF packet at @p sample_rate_hz, the same in every packet: 6 @ 44.1k and
/// 48k, 12 @ 88.2k and 96k, 24 @ 176.4k and 192k (IEEE 1722-2016 Clause 7.7, Table 17).
[[nodiscard]] constexpr auto class_a_samples_per_packet(uint32_t sample_rate_hz) noexcept -> uint32_t
{
    if (sample_rate_hz % CLASS_A_PACKETS_PER_SEC == 0) {
        return sample_rate_hz / CLASS_A_PACKETS_PER_SEC;
    }
    if (sample_rate_hz % CLASS_A_44_1_KHZ_PACKETS_PER_SEC == 0) {
        return sample_rate_hz / CLASS_A_44_1_KHZ_PACKETS_PER_SEC;
    }
    return (sample_rate_hz + CLASS_A_PACKETS_PER_SEC - 1) / CLASS_A_PACKETS_PER_SEC;
}

/// The byte Milan v1.3 Table 4.4 adds for a sampling clock a little faster than nominal, to the
/// frame after the minimum-size rule (step 3).
inline constexpr uint16_t MILAN_TSPEC_SPARE_BYTES = 1;

/// The MSRP TSpec max_frame_size implied by @p format at the Class A cadence: the AVTPDU (Milan
/// v1.3 Section 4.3.3.2, Table 4.4). Returns 0 for StreamKind::other or an unknown rate.
[[nodiscard]] auto srp_max_frame_size(StreamFormatFields const& format) noexcept -> uint16_t;

/// One stream's MSRP TSpec (IEEE 802.1Q 35.2.2.8.4).
struct TSpec
{
    uint16_t max_frame_size{0};       ///< L2 payload octets (MaxFrameSize)
    uint16_t max_interval_frames{1};  ///< frames per class measurement interval (MaxIntervalFrames)
    /// Octets the shaper adds to each frame after the minimum-size rule, not declared (Milan
    /// v1.3 Table 4.4 step 3: MILAN_TSPEC_SPARE_BYTES).
    uint16_t clock_drift_bytes{0};

    auto operator==(TSpec const&) const noexcept -> bool = default;
};

//
// A Talker's frames on the wire and its port's latency - IEEE 802.1Q-2022 Clause 35.2.2.8.6
//

/// Per-frame media overhead on an 802.3 port (802.1Q 35.2.2.8.3: 42 octets).
inline constexpr uint32_t VLAN_OVERHEAD = 4;  ///< the 802.1Q tag
inline constexpr uint32_t L2_OVERHEAD = 18;   ///< Ethernet header + FCS
inline constexpr uint32_t L1_OVERHEAD = 20;   ///< preamble + SFD + inter-packet gap
/// Avnu ProAV Bridge 9.1: count at least a 68-octet frame (before L1).
inline constexpr uint32_t MIN_L2_FRAME = 68;
/// Class measurement intervals as frames per second (125 us, 250 us).
inline constexpr uint32_t CLASS_A_INTERVALS_PER_SEC = 8000;
inline constexpr uint32_t CLASS_B_INTERVALS_PER_SEC = 4000;

/// The octets a stream frame occupies on the wire (Milan v1.3 Table 4.4): F = MaxFrameSize + 22,
/// at least 68, plus @p clock_drift_bytes (v1.3's step 3; v1.2 has it in MaxFrameSize), plus 20.
[[nodiscard]] constexpr auto wire_frame_size(uint16_t const max_frame_size, uint16_t const clock_drift_bytes = 0) noexcept
    -> uint32_t
{
    uint32_t frame = uint32_t{max_frame_size} + VLAN_OVERHEAD + L2_OVERHEAD;
    if (frame < MIN_L2_FRAME) {
        frame = MIN_L2_FRAME;
    }
    return frame + clock_drift_bytes + L1_OVERHEAD;
}

/// The MTU assumed for the interfering frame when the port's is unknown: a full-size 802.3 frame's MSDU.
inline constexpr uint32_t FALLBACK_MTU = 1500;

/// The octets @p tspec's frames of one class measurement interval occupy on the wire (wire_frame_size).
[[nodiscard]] constexpr auto interval_wire_octets(TSpec const& tspec) noexcept -> uint64_t
{
    return uint64_t{wire_frame_size(tspec.max_frame_size, tspec.clock_drift_bytes)} * tspec.max_interval_frames;
}

/// The wire propagation time a Talker counts, ns (IEEE 802.1Q-2022 Clause 35.2.2.8.6 item d).
inline constexpr uint32_t PROPAGATION_DELAY_NS = 500;

/// The egress port a Talker's AccumulatedLatency is for. 0 is unknown: 1000 Mbit/s, FALLBACK_MTU.
struct TalkerPort
{
    uint32_t link_mbps{0};
    uint32_t mtu{0};
    /// The SR class's measurement intervals a second (CLASS_A_INTERVALS_PER_SEC, CLASS_B_INTERVALS_PER_SEC).
    uint32_t intervals_per_sec{CLASS_A_INTERVALS_PER_SEC};
    /// A cbs shapes the class's queue at an idleSlope of its streams' bandwidth (IEEE 802.1Q-2022 Annex L).
    bool cbs{false};
};

/// @p octets' transmission time at @p link_mbps (0: 1000), ns, rounded up.
[[nodiscard]] constexpr auto wire_time_ns(uint64_t const octets, uint32_t const link_mbps) noexcept -> uint64_t
{
    uint64_t const mbps = link_mbps == 0 ? 1000 : link_mbps;
    return ((octets * 8000U) + mbps - 1) / mbps;
}

/// The time a cbs whose idleSlope is @p class_octets an interval needs to send @p queued of them, ns,
/// rounded up: their share of the interval.
[[nodiscard]] constexpr auto cbs_drain_ns(
    uint64_t const queued, uint64_t const class_octets, uint32_t const intervals_per_sec) noexcept -> uint64_t
{
    uint64_t const per_sec = class_octets * intervals_per_sec;
    return per_sec == 0 ? 0 : ((queued * 1'000'000'000U) + per_sec - 1) / per_sec;
}

/// A talker port's portTcMaxLatency for @p own_octets among its class's @p class_octets, ns, rounded up (IEEE 802.1Q-2022
/// Clause 35.2.2.8.6): a) the others at the link speed or a cbs's idleSlope; b) an MTU frame, then its own; d) propagation.
/// A class B stream on a port also running class A would add class A's queue to a), which is not counted.
[[nodiscard]] constexpr auto talker_port_latency_ns(
    TalkerPort const& port, uint64_t const class_octets, uint64_t const own_octets) noexcept -> uint32_t
{
    auto const interfering = static_cast<uint16_t>(std::min<uint32_t>(port.mtu == 0 ? FALLBACK_MTU : port.mtu, UINT16_MAX));
    uint64_t const others = class_octets - std::min(own_octets, class_octets);
    uint64_t const at_link = wire_frame_size(interfering) + own_octets + (port.cbs ? 0 : others);
    uint64_t const queue_ns = port.cbs ? cbs_drain_ns(others, class_octets, port.intervals_per_sec) : 0;
    return static_cast<uint32_t>(
        std::min<uint64_t>(wire_time_ns(at_link, port.link_mbps) + queue_ns + PROPAGATION_DELAY_NS, UINT32_MAX));
}

/// talker_port_latency_ns for a lone, unshaped stream of @p octets on @p interface_name: @p port's link speed
/// and MTU where nonzero, else the interface's as read now (Linux; elsewhere, and unknown, the fallbacks).
[[nodiscard]] inline auto interface_talker_latency_ns(std::string_view const interface_name, TalkerPort port, uint64_t const octets)
    -> uint32_t
{
#if defined(__linux__)
    port.link_mbps = port.link_mbps != 0 ? port.link_mbps : net::link_speed_mbps(interface_name).value_or(0);
    port.mtu = port.mtu != 0 ? port.mtu : net::interface_mtu(interface_name).value_or(0);
#else
    (void)interface_name;
#endif
    return talker_port_latency_ns(port, octets, octets);
}

/// The single audio sample rate shared by every AM824/AAF stream in @p specs
/// — the rate the entity's one media clock must run at. CRF streams don't
/// constrain it (a 48 kHz CRF base is routinely advertised under 96 kHz
/// audio). Errors when audio streams disagree or when no audio stream (and
/// no fallback) exists; @p fallback_hz is returned for CRF-only/empty tables
/// when nonzero.
[[nodiscard]] auto common_audio_sample_rate(StreamSpecs const& specs, uint32_t fallback_hz = 0) -> StatusValue<uint32_t>;

/// The StreamSpec @p spec with the format word @p word (decoded into its
/// fields); the index, clock domain and interface are kept.
[[nodiscard]] auto with_format(StreamSpec const& spec, uint64_t word) noexcept -> StreamSpec;

/// First stream of @p kind in the table, or nullopt. Replaces the per-entity
/// hardcoded index constants (AM824=0 / AAF=1 / CRF=2 style).
[[nodiscard]] auto find_stream(StreamSpecs const& specs, StreamKind kind) noexcept -> std::optional<uint16_t>;

/// The 32-bit symbol code of an authored symbol name — zlib/IEEE 802.3 CRC-32,
/// bit-identical to the blob generator (python/aemxml/flatten.py
/// symbol_to_code). Constexpr so `set_render("aux_out", ...)`-style
/// registrations hash at compile time.
[[nodiscard]] constexpr auto symbol_code(std::string_view name) noexcept -> uint32_t
{
    uint32_t crc = 0xFFFFFFFFU;
    for (char const c : name) {
        crc ^= static_cast<uint8_t>(c);
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}
// Golden vector: the committed tone blobs' "identify" CONTROL symbol.
static_assert(symbol_code("identify") == 0x12619917U);

/// Per-stream TX audio source: fill @p audio (interleaved, frames x channels
/// floats) for this media-clock tick. @p first_index is the media-clock
/// sample index of the first frame; @p pts_ns its presentation time.
/// Runs on the SCHED_FIFO media-timer thread: no allocation, no blocking.
/// Registered by blob symbol or stream index (the code is a menu, the model
/// is the selection: bindings for symbols the model omits stay inert).
using StreamRenderFn =
    sg14::inplace_function<void(std::span<float> audio, uint32_t frames, uint64_t first_index, uint64_t pts_ns), 64>;

/// Per-stream raw TX render: fill @p payload with exactly @p frames frames in the stream's wire layout
/// (AAF samples, or AM824 quadlets) for the packet at sample index @p first_index, time @p pts_ns.
/// Return false to send nothing. Same thread as StreamRenderFn.
using StreamRawRenderFn =
    sg14::inplace_function<bool(std::span<uint8_t> payload, uint32_t frames, uint64_t first_index, uint64_t pts_ns), 64>;

/// Per-stream RX audio consumer: one decoded channel of @p samples floats
/// with its reconstructed 64-bit presentation time. Runs on the reactor/RX
/// thread: keep it cheap and non-blocking.
using StreamConsumeFn =
    sg14::inplace_function<void(uint8_t channel, std::span<float const> samples, uint64_t pts_ns, uint64_t period_ns), 64>;

/// One received CRF timestamp (64-bit gPTP time), its index in the PDU, the gPTP receive time, the mr bit
/// (a change is a media clock restart, IEEE 1722-2016 Clause 10.4) and @p source (1 from the input's
/// first timestamp, stepping on each ACMP connection or stream_id change). RX thread.
using StreamCrfFn = sg14::
    inplace_function<void(uint64_t crf_timestamp_ns, uint16_t index_in_pdu, int64_t rx_gptp_ns, bool mr, uint32_t source), 64>;

}  // namespace statusbar::avb_entity
