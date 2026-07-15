// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerStreams methods — the local AVB stream TX path. Slots are shaped by
// StreamSpecs derived from the entity blob (kit phase 1); the per-kind
// serialization bodies are unchanged from the named-slot era.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

auto TalkerStreams::open_stream(StreamSpec const& spec, tsn::StreamId const stream_id, ieee::Eui48 const dest_mac) -> Status
{
    if (slots_.size() == MAX_ENTITY_STREAMS) {
        return failure(std::errc::result_out_of_range);
    }
    TalkerStreamSlot slot{};
    slot.spec = spec;
    slot.dest_mac = dest_mac;
    switch (spec.format.kind) {
        case StreamKind::am824: {
            auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.am824.emplace(stream_id, *rate, static_cast<uint8_t>(spec.format.channels), /*pres_offset=*/0);
            break;
        }
        case StreamKind::aaf: {
            auto const rate = avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.aaf.emplace(
                stream_id, spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth, /*pres_offset=*/0);
            slot.reframer.emplace(spec.format.channels, samples_per_packet_, 4, mem_resource_);
            break;
        }
        case StreamKind::crf: {
            if (spec.format.crf_base_frequency_hz == 0 || spec.format.crf_timestamps_per_pdu == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.crf.emplace(
                stream_id,
                static_cast<avtp::CrfType>(spec.format.crf_type),
                spec.format.crf_base_frequency_hz,
                static_cast<avtp::CrfPull>(spec.format.crf_pull),
                spec.format.crf_timestamp_interval,
                spec.format.crf_timestamps_per_pdu);
            break;
        }
        case StreamKind::other:
        default:
            return failure(std::errc::not_supported);
    }
    slots_.push_back(std::move(slot));
    return success();
}

auto TalkerStreams::slot_for(uint16_t const stream_index) noexcept -> TalkerStreamSlot*
{
    for (auto& slot : slots_) {
        if (slot.spec.index == stream_index) {
            return &slot;
        }
    }
    return nullptr;
}

auto TalkerStreams::slot_for(uint16_t const stream_index) const noexcept -> TalkerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.spec.index == stream_index) {
            return &slot;
        }
    }
    return nullptr;
}

auto TalkerStreams::slot_of(StreamKind const kind) noexcept -> TalkerStreamSlot*
{
    for (auto& slot : slots_) {
        if (slot.spec.format.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto TalkerStreams::slot_of(StreamKind const kind) const noexcept -> TalkerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.spec.format.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

void TalkerStreams::transmit_am824(
    TalkerStreamSlot& slot, uint64_t const now_ns, uint32_t const samples, std::span<float const> src)
{
    if (!slot.am824 || stream_tx_.fd() < 0) {
        return;
    }
    static constexpr size_t MAX_FRAME =
        avtp::Am824Pdu::HEADER_LENGTH + (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);
    std::array<uint8_t, MAX_FRAME> frame{};

    size_t const channels = slot.spec.format.channels;
    avtp::Am824Pdu pdu{};
    // Nominal AM824 rate (CIP FDF) from the configured sample rate: hardcoding
    // 96 kHz would mislabel a 48 kHz stream on the wire; value_or keeps the
    // 96 kHz default for any unmapped rate.
    auto const am824_rate = avtp::am824_sample_rate_from_hz(config_.sample_rate).value_or(avtp::Am824SampleRate::rate_96_khz);
    pdu.init(slot.am824->stream_id, static_cast<uint8_t>(channels), am824_rate);

    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::am824_serialize_mbla(
        *slot.am824, pdu, payload, static_cast<uint8_t>(samples), now_ns, [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    span_store(std::span<uint8_t>{frame}.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::Am824Pdu::HEADER_LENGTH + audio_bytes;
    // AVB stream frames must be VLAN-tagged with the SR class VID + PCP so bridges
    // admit them to the reserved SR class (untagged AVTP is not part of any SR class).
    // pcap stamp = the gPTP WALL-CLOCK transmit time (not now_ns, which is the
    // media-clock PRESENTATION timestamp also written into the AVTP header). Using
    // the wall clock lets a capture reveal the real presentation lead
    // (avtp_ts - wall_clock = presentation_offset).
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    (void)stream_tx_.send_vlan(
        &slot.dest_mac, std::span<uint8_t const>{frame.data(), frame_len}, config_.vlan_id, config_.stream_pcp);
    ++slot.tx_packets;
}

void TalkerStreams::transmit_aaf(TalkerStreamSlot& slot, uint64_t const now_ns, uint16_t const samples, std::span<float const> src)
{
    if (!slot.aaf || stream_tx_.fd() < 0) {
        return;
    }
    static constexpr size_t MAX_FRAME =
        avtp::AafPdu::HEADER_LENGTH + (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);
    std::array<uint8_t, MAX_FRAME> frame{};

    size_t const channels = slot.spec.format.channels;
    avtp::AafPdu pdu{};
    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    size_t const audio_bytes =
        avtp::aaf_stream_serialize(*slot.aaf, pdu, payload, samples, now_ns, [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::AafPdu::HEADER_LENGTH + audio_bytes;
    // pcap stamp = gPTP WALL-CLOCK transmit time (see transmit_am824); now_ns here
    // is the media-clock PRESENTATION time, also written into the AVTP header.
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    (void)stream_tx_.send_vlan(
        &slot.dest_mac, std::span<uint8_t const>{frame.data(), frame_len}, config_.vlan_id, config_.stream_pcp);
    ++slot.tx_packets;
}

void fill_crf_timestamps(
    ptpclient::MediaClockGenerator const& media_clock,
    uint64_t const base_index,
    uint32_t const sample_stride,
    uint16_t const n_ts,
    std::span<uint8_t> ts_data) noexcept
{
    // Timestamps come from the media clock, so the CRF conveys exactly our media
    // clock (event times, evenly spaced by sample_stride in the SAMPLE_RATE domain).
    // timestamp_for() already carries the presentation offset (Open1722's CRF
    // talker likewise offsets by the max transit time).
    for (uint16_t i = 0; i < n_ts; ++i) {
        uint64_t const ts = media_clock.timestamp_for(base_index + (static_cast<uint64_t>(i) * sample_stride));
        (void)avtp::crf_set_timestamp(ts_data, i, ts);
    }
}

void TalkerStreams::transmit_crf(TalkerStreamSlot& slot, uint64_t const base_index)
{
    if (!slot.crf || stream_tx_.fd() < 0 || !media_clock_.anchored()) {
        return;
    }
    uint16_t const n_ts = slot.crf->timestamps_per_packet;
    uint16_t const interval = slot.crf->timestamp_interval;
    // Convert the DECLARED interval (in CRF base-frequency events) into the audio
    // sample-index domain that media_clock_.timestamp_for() speaks. With a 48 kHz
    // CRF base and 96 kHz audio, each declared CRF event spans SAMPLE_RATE/base
    // (= 2) audio samples, so the emitted timestamp VALUES stay spaced at
    // interval/base seconds regardless of the base we advertise.
    uint32_t const sample_stride = crf_sample_stride(interval, slot.crf->base_frequency, config_.sample_rate);

    static constexpr size_t MAX_FRAME = avtp::CrfPdu::HEADER_LENGTH + (64 * avtp::CrfPdu::TIMESTAMP_SIZE);
    std::array<uint8_t, MAX_FRAME> frame{};

    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(slot.crf->stream_id, slot.crf->base_frequency, slot.crf->pull, interval, n_ts);
    pdu.set_sequence_num(slot.crf->sequence_num);

    std::span<uint8_t> const ts_data = std::span<uint8_t>{frame}.subspan(avtp::CrfPdu::HEADER_LENGTH);
    fill_crf_timestamps(media_clock_, base_index, sample_stride, n_ts, ts_data);
    slot.crf->sequence_num = static_cast<uint8_t>((slot.crf->sequence_num + 1U) & 0xFFU);
    ++slot.crf->packets_sent;

    span_store(std::span<uint8_t>{frame}.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::CrfPdu::HEADER_LENGTH + (static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE);
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);  // gPTP timestamp for the optional TX pcap tap
    (void)stream_tx_.send_vlan(
        &slot.dest_mac, std::span<uint8_t const>{frame.data(), frame_len}, config_.vlan_id, config_.stream_pcp);
    ++slot.tx_packets;
}

void TalkerStreams::transmit_if_due(
    TalkerStreamSlot& slot,
    ptpclient::MediaClockGenerator::Emit const& tick,
    bool const srp_gate_open,
    size_t const samples,
    std::span<float const> src)
{
    // A STOP_STREAMING'd stream behaves exactly like a closed SRP gate:
    // no emission, AAF FIFO cleared, CRF phase reset for a clean restart.
    bool const gate_open = srp_gate_open && !stream_stopped(slot.spec.index);
    switch (slot.spec.format.kind) {
        case StreamKind::am824: {
            if (gate_open && samples > 0) {
                transmit_am824(slot, media_clock_.timestamp_for(tick.first_index), tick.samples, src);
            }
            break;
        }
        case StreamKind::aaf: {
            if (!slot.reframer) {
                break;
            }
            if (!gate_open) {
                // Gate closed: drop any partial block so a later reconnect starts
                // clean (no stale samples / stale timestamps).
                slot.reframer->clear();
                break;
            }
            // AAF must be constant-size: buffer this wake's variable samples and emit
            // only whole SAMPLES_PER_PACKET blocks (0, 1, or 2+ this wake); the < block
            // remainder carries to the next wake. Each block's avtp_timestamp is the
            // jitter-free media-clock time of its first sample.
            slot.reframer->push(src.first(samples * slot.spec.format.channels), static_cast<uint16_t>(samples), tick.first_index);
            slot.reframer->drain([this, &slot](uint64_t first_index, std::span<float const> block) {
                transmit_aaf(slot, media_clock_.timestamp_for(first_index), static_cast<uint16_t>(samples_per_packet_), block);
            });
            break;
        }
        case StreamKind::crf: {
            if (!slot.crf) {
                break;
            }
            if (!gate_open) {
                slot.crf_decim = 0;  // gate closed: next emission starts a fresh PDU phase
                break;
            }
            // One PDU carries timestamps_per_packet timestamps, each spaced
            // sample_stride audio samples, so a PDU spans pkts_per_crf audio packets
            // (the Milan 48 kHz/interval-96/1-ts format under 96 kHz audio -> 192
            // samples = every 16 packets -> 500 PDU/s).
            uint32_t const sample_stride =
                crf_sample_stride(slot.crf->timestamp_interval, slot.crf->base_frequency, config_.sample_rate);
            uint32_t pkts_per_crf = (static_cast<uint32_t>(slot.crf->timestamps_per_packet) * sample_stride) / samples_per_packet_;
            if (pkts_per_crf == 0) {
                pkts_per_crf = 1;
            }
            if (slot.crf_decim == 0) {
                // Re-base to the live media-clock position each PDU so CRF timestamps
                // track gPTP-now, not the (possibly long-past) media-clock anchor.
                transmit_crf(slot, crf_aligned_base(tick.first_index, sample_stride));
            }
            slot.crf_decim = static_cast<uint16_t>((slot.crf_decim + 1U) % pkts_per_crf);
            break;
        }
        case StreamKind::other:
        default:
            break;
    }
}

}  // namespace statusbar::avb_entity
