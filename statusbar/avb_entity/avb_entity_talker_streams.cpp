// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerStreams methods — the local AVB stream TX path, moved out of
// avb_entity_audio_io.cpp (god-object phase 3). Bodies unchanged: the talker
// owns the serializers/socket/counters and holds same-named refs (config_/
// media_clock_/audio_buffer_/channels_/last_gptp_ns_), so only the method
// qualifier changed.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace statusbar::avb_entity {

void TalkerStreams::transmit_am824(uint64_t now_ns, uint32_t samples)
{
    if (!am824_out_ || stream_tx_.fd() < 0) {
        return;
    }
    static constexpr size_t MAX_FRAME =
        avtp::Am824Pdu::HEADER_LENGTH + (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);
    std::array<uint8_t, MAX_FRAME> frame{};

    avtp::Am824Pdu pdu{};
    pdu.init(am824_out_->stream_id, static_cast<uint8_t>(channels_), avtp::Am824SampleRate::rate_96_khz);

    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::am824_serialize_mbla(
        *am824_out_, pdu, payload, static_cast<uint8_t>(samples), now_ns, [this](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = audio_buffer_[(s * channels_) + ch];
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
        &am824_dest_mac_, std::span<uint8_t const>{frame.data(), frame_len}, config_.vlan_id, config_.stream_pcp);
    ++am824_tx_packets_;
}

void TalkerStreams::transmit_aaf(uint64_t now_ns, uint16_t samples, std::span<float const> src)
{
    if (!aaf_out_ || stream_tx_.fd() < 0) {
        return;
    }
    static constexpr size_t MAX_FRAME =
        avtp::AafPdu::HEADER_LENGTH + (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);
    std::array<uint8_t, MAX_FRAME> frame{};

    avtp::AafPdu pdu{};
    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    size_t const audio_bytes =
        avtp::aaf_stream_serialize(*aaf_out_, pdu, payload, samples, now_ns, [this, src](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels_) + ch];
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
        &aaf_dest_mac_, std::span<uint8_t const>{frame.data(), frame_len}, config_.vlan_id, config_.stream_pcp);
    ++aaf_tx_packets_;
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

void TalkerStreams::transmit_crf(uint64_t const base_index)
{
    if (!crf_out_ || stream_tx_.fd() < 0 || !media_clock_.anchored()) {
        return;
    }
    uint16_t const n_ts = config_.crf_timestamps_per_packet;
    uint16_t const interval = config_.crf_timestamp_interval;
    // Convert the DECLARED interval (in CRF base-frequency events) into our audio
    // SAMPLE_RATE (96 kHz) sample-index domain that media_clock_.timestamp_for()
    // speaks. With a 48 kHz CRF base and 96 kHz audio, each declared CRF event spans
    // SAMPLE_RATE/base (= 2) audio samples, so the emitted timestamp VALUES stay
    // spaced at interval/base seconds regardless of the base we advertise.
    uint32_t const sample_stride = crf_sample_stride(interval, crf_out_->base_frequency);

    static constexpr size_t MAX_FRAME = avtp::CrfPdu::HEADER_LENGTH + (64 * avtp::CrfPdu::TIMESTAMP_SIZE);
    std::array<uint8_t, MAX_FRAME> frame{};

    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(crf_out_->stream_id, crf_out_->base_frequency, crf_out_->pull, interval, n_ts);
    pdu.set_sequence_num(crf_out_->sequence_num);

    std::span<uint8_t> const ts_data = std::span<uint8_t>{frame}.subspan(avtp::CrfPdu::HEADER_LENGTH);
    fill_crf_timestamps(media_clock_, base_index, sample_stride, n_ts, ts_data);
    crf_out_->sequence_num = static_cast<uint8_t>((crf_out_->sequence_num + 1U) & 0xFFU);
    ++crf_out_->packets_sent;

    span_store(std::span<uint8_t>{frame}.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::CrfPdu::HEADER_LENGTH + (static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE);
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);  // gPTP timestamp for the optional TX pcap tap
    (void)stream_tx_.send_vlan(
        &crf_dest_mac_, std::span<uint8_t const>{frame.data(), frame_len}, config_.vlan_id, config_.stream_pcp);
}

}  // namespace statusbar::avb_entity
