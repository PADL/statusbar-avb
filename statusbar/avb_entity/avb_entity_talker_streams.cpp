// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerStreams methods — the local AVB stream TX path. Slots are shaped by
// StreamSpecs derived from the entity blob (kit phase 1); the per-kind
// serialization bodies are unchanged from the named-slot era.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

namespace {

/// The window an audio AVTPDU is serialized into: the most a tagged frame
/// carries (a format needing more is refused by its serializer).
constexpr size_t MAX_AUDIO_PAYLOAD = StreamPort::MAX_TX_PAYLOAD;

/// The steady clock in ns: the time base of the tu window (note_grandmaster_changed).
[[nodiscard]] auto steady_now_ns() noexcept -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

auto TalkerStreams::shape(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id) -> Status
{
    switch (spec.format.kind) {
        case StreamKind::am824: {
            auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.am824.emplace(stream_id, *rate, static_cast<uint8_t>(spec.format.channels), /*pres_offset=*/0);
            return success();
        }
        case StreamKind::aaf: {
            auto const rate = avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.aaf.emplace(stream_id, spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth, /*pres_offset=*/0);
            slot.reframer.emplace(spec.format.channels, samples_per_packet_, 4, mem_resource_);
            return success();
        }
        case StreamKind::crf: {
            if (spec.format.crf_base_frequency_hz == 0 || spec.format.crf_timestamps_per_pdu == 0 ||
                spec.format.crf_timestamps_per_pdu > MAX_CRF_TIMESTAMPS_PER_PDU) {
                return failure(std::errc::invalid_argument);
            }
            slot.crf.emplace(
                stream_id,
                static_cast<avtp::CrfType>(spec.format.crf_type),
                spec.format.crf_base_frequency_hz,
                static_cast<avtp::CrfPull>(spec.format.crf_pull),
                spec.format.crf_timestamp_interval,
                spec.format.crf_timestamps_per_pdu);
            return success();
        }
        case StreamKind::other:
        default:
            return failure(std::errc::not_supported);
    }
}

auto TalkerStreams::open_stream(StreamSpec const& spec, tsn::StreamId const stream_id, ieee::Eui48 const dest_mac) -> Status
{
    if (slots_.size() == MAX_ENTITY_STREAMS || spec.index >= MAX_ENTITY_STREAMS) {
        return failure(std::errc::result_out_of_range);
    }
    TalkerStreamSlot slot{};
    slot.spec = spec;
    slot.dest_mac = dest_mac;
    if (auto status = shape(slot, spec, stream_id); !status) {
        return status;
    }
    slots_.push_back(std::move(slot));
    return success();
}

auto TalkerStreams::reformat_stream(StreamSpec const& spec) -> Status
{
    auto* const slot = slot_for(spec.index);
    if (slot == nullptr) {
        return failure(std::errc::no_such_device);
    }
    if (spec.format.kind != slot->spec.format.kind) {
        return failure(std::errc::invalid_argument);
    }
    if (spec.format.kind == StreamKind::crf || spec.format.kind == StreamKind::other) {
        // One CRF format a stream: nothing to re-shape.
        return spec.format_word == slot->spec.format_word ? Status{} : Status{failure(std::errc::not_supported)};
    }
    auto const stream_id = slot->aaf ? slot->aaf->stream_id : slot->am824 ? slot->am824->stream_id : tsn::StreamId{};
    // A trial first, so that a refused format leaves the slot as it was.
    TalkerStreamSlot trial{};
    if (auto status = shape(trial, spec, stream_id); !status) {
        return status;
    }
    SlotGuard::Lock const lock{guards_[spec.index]};
    slot->am824 = trial.am824;
    slot->aaf = trial.aaf;
    slot->reframer = std::move(trial.reframer);
    // The index stays (slot_for reads it unlocked); the format is the new one.
    slot->spec.format_word = spec.format_word;
    slot->spec.format = spec.format;
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
    if (!slot.am824 || port_ == nullptr) {
        return;
    }
    std::span<uint8_t> const frame = port_->tx_begin(MAX_AUDIO_PAYLOAD);
    if (frame.empty()) {
        return;
    }
    span_zero(frame);

    size_t const channels = slot.spec.format.channels;
    avtp::Am824Pdu pdu{};
    // Nominal AM824 rate (CIP FDF) from the configured sample rate: hardcoding
    // 96 kHz would mislabel a 48 kHz stream on the wire; value_or keeps the
    // 96 kHz default for any unmapped rate.
    auto const am824_rate = avtp::am824_sample_rate_from_hz(config_.sample_rate).value_or(avtp::Am824SampleRate::rate_96_khz);
    pdu.init(slot.am824->stream_id, static_cast<uint8_t>(channels), am824_rate);

    std::span<uint8_t> const payload = frame.subspan(avtp::Am824Pdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::am824_serialize_mbla(
        *slot.am824, pdu, payload, static_cast<uint8_t>(samples), now_ns, [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    span_store(frame.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    send_stream_frame(slot, frame.first(avtp::Am824Pdu::HEADER_LENGTH + audio_bytes));
}

void TalkerStreams::transmit_aaf(TalkerStreamSlot& slot, uint64_t const now_ns, uint16_t const samples, std::span<float const> src)
{
    if (!slot.aaf || port_ == nullptr) {
        return;
    }
    std::span<uint8_t> const frame = port_->tx_begin(MAX_AUDIO_PAYLOAD);
    if (frame.empty()) {
        return;
    }
    span_zero(frame);

    size_t const channels = slot.spec.format.channels;
    avtp::AafPdu pdu{};
    std::span<uint8_t> const payload = frame.subspan(avtp::AafPdu::HEADER_LENGTH);
    size_t const audio_bytes =
        avtp::aaf_stream_serialize(*slot.aaf, pdu, payload, samples, now_ns, [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    pdu.set_tu(timestamp_uncertain(steady_now_ns()));
    span_store(frame.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    send_stream_frame(slot, frame.first(avtp::AafPdu::HEADER_LENGTH + audio_bytes));
}

auto serialize_aaf_raw(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const frames,
    uint64_t const first_index,
    uint64_t const pts_ns,
    StreamRawRenderFn const& render,
    bool const timestamp_uncertain) -> size_t
{
    size_t const bps = avtp::aaf_bytes_per_sample(ctx.format);
    size_t const payload_len = static_cast<size_t>(frames) * ctx.channel_count * bps;
    if (bps == 0 || payload_len == 0 || !render || frame.size() < avtp::AafPdu::HEADER_LENGTH + payload_len) {
        return 0;
    }
    std::span<uint8_t> const payload = frame.subspan(avtp::AafPdu::HEADER_LENGTH, payload_len);
    // Render first: a refused packet must not consume a sequence number.
    if (!render(payload, frames, first_index, pts_ns)) {
        return 0;
    }
    avtp::AafPdu pdu{};
    ctx.build_packet_header(pdu, frames, pts_ns);
    // The wire timestamp is pts_ns itself: build_packet_header adds the context's
    // presentation offset, which the caller has already applied to pts_ns.
    pdu.set_avtp_timestamp(static_cast<uint32_t>(pts_ns & 0xFFFF'FFFFU));
    pdu.set_tu(timestamp_uncertain);
    span_store(frame.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    return avtp::AafPdu::HEADER_LENGTH + payload_len;
}

auto TalkerStreams::transmit_aaf_raw(
    TalkerStreamSlot& slot,
    uint64_t const first_index,
    uint64_t const pts_ns,
    uint16_t const frames,
    StreamRawRenderFn const& render) -> bool
{
    SlotGuard::Entry const entry{guards_[slot.spec.index]};
    if (!entry.entered() || !slot.aaf || port_ == nullptr || stream_stopped(slot.spec.index)) {
        return false;  // being re-shaped (reformat_stream), or not sending
    }
    // Not zeroed: serialize_aaf_raw writes every byte it sends.
    std::span<uint8_t> const frame = port_->tx_begin(MAX_AUDIO_PAYLOAD);
    if (frame.empty()) {
        return false;
    }
    size_t const frame_len =
        serialize_aaf_raw(*slot.aaf, frame, frames, first_index, pts_ns, render, timestamp_uncertain(steady_now_ns()));
    if (frame_len == 0) {
        return false;
    }
    send_stream_frame(slot, frame.first(frame_len));
    return true;
}

void TalkerStreams::send_stream_frame(TalkerStreamSlot& slot, std::span<uint8_t const> const frame)
{
    // Frames carry the SR class VID + PCP (untagged AVTP is in no SR class). The pcap stamp is the
    // gPTP wall-clock transmit time, so a capture shows avtp_ts - wall_clock = presentation_offset.
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    if (port_ != nullptr) {
        (void)port_->tx_commit(frame, slot.dest_mac, vlan_id_.load(std::memory_order_relaxed), config_.stream_pcp);
    }
    ++slot.tx_packets;
    if (slot.spec.index < tx_counters_.size() && frame.size() >= 4) {
        // AVTP stream header: mr is bit 3 of octet 1; tu is bit 0 of octet 3,
        // or of octet 1 in a CRF PDU (IEEE 1722-2016 4.4.4, 10.4).
        auto& c = tx_counters_[slot.spec.index];
        auto const bump = [](std::atomic<uint32_t>& n) { n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); };
        bool const mr = (frame[1] & 0x08U) != 0;
        bool const tu = ((frame[0] == avtp::AvtpSubtype::crf ? frame[1] : frame[3]) & 0x01U) != 0;
        bump(c.pdus);
        if (tu) {
            bump(c.tu_pdus);
        }
        if (c.have_mr && mr != c.last_mr) {
            bump(c.mr_toggles);
        }
        c.last_mr = mr;
        c.have_mr = true;
    }
}

void fill_crf_timestamps(
    ptpclient::MediaClockGenerator const& media_clock,
    uint64_t const base_index,
    uint32_t const sample_stride,
    std::span<uint64_t> const timestamps) noexcept
{
    // Timestamps come from the media clock, so the CRF conveys exactly our media
    // clock (event times, evenly spaced by sample_stride in the SAMPLE_RATE domain).
    // timestamp_for() already carries the presentation offset (Open1722's CRF
    // talker likewise offsets by the max transit time).
    for (size_t i = 0; i < timestamps.size(); ++i) {
        timestamps[i] = media_clock.timestamp_for(base_index + (static_cast<uint64_t>(i) * sample_stride));
    }
}

auto serialize_crf_raw(
    avtp::CrfStreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    std::span<uint64_t const> const timestamps,
    bool const timestamp_uncertain) -> size_t
{
    uint16_t const n_ts = ctx.timestamps_per_packet;
    size_t const frame_len = avtp::CrfPdu::HEADER_LENGTH + (static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE);
    if (n_ts == 0 || timestamps.size() != n_ts || frame.size() < frame_len) {
        return 0;
    }
    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(ctx.stream_id, ctx.base_frequency, ctx.pull, ctx.timestamp_interval, n_ts);
    pdu.set_sequence_num(ctx.sequence_num);
    pdu.set_tu(timestamp_uncertain);
    std::span<uint8_t> const ts_data = frame.subspan(avtp::CrfPdu::HEADER_LENGTH, frame_len - avtp::CrfPdu::HEADER_LENGTH);
    for (uint16_t i = 0; i < n_ts; ++i) {
        (void)avtp::crf_set_timestamp(ts_data, i, timestamps[i]);
    }
    span_store(frame.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
    ctx.sequence_num = static_cast<uint8_t>((ctx.sequence_num + 1U) & 0xFFU);
    ++ctx.packets_sent;
    ctx.total_timestamps_sent += n_ts;
    return frame_len;
}

void TalkerStreams::transmit_crf(TalkerStreamSlot& slot, uint64_t const base_index)
{
    if (!slot.crf || port_ == nullptr || !media_clock_.anchored()) {
        return;
    }
    // Convert the DECLARED interval (in CRF base-frequency events) into the audio
    // sample-index domain that media_clock_.timestamp_for() speaks. With a 48 kHz
    // CRF base and 96 kHz audio, each declared CRF event spans SAMPLE_RATE/base
    // (= 2) audio samples, so the emitted timestamp VALUES stay spaced at
    // interval/base seconds regardless of the base we advertise.
    uint32_t const sample_stride = crf_sample_stride(slot.crf->timestamp_interval, slot.crf->base_frequency, config_.sample_rate);
    // Not zeroed: fill_crf_timestamps writes the n_ts sent (open_stream capped
    // n_ts at MAX_CRF_TIMESTAMPS_PER_PDU).
    std::array<uint64_t, MAX_CRF_TIMESTAMPS_PER_PDU> timestamps;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    std::span<uint64_t> const used{timestamps.data(), slot.crf->timestamps_per_packet};
    fill_crf_timestamps(media_clock_, base_index, sample_stride, used);
    (void)transmit_crf_raw(slot, used);
}

auto TalkerStreams::transmit_crf_raw(TalkerStreamSlot& slot, std::span<uint64_t const> const timestamps_ns) -> bool
{
    if (!slot.crf || port_ == nullptr || stream_stopped(slot.spec.index)) {
        return false;
    }
    static constexpr size_t MAX_FRAME = avtp::CrfPdu::HEADER_LENGTH + (MAX_CRF_TIMESTAMPS_PER_PDU * avtp::CrfPdu::TIMESTAMP_SIZE);
    static_assert(MAX_FRAME <= StreamPort::MAX_TX_PAYLOAD);
    // Not zeroed: serialize_crf_raw writes every byte it sends.
    std::span<uint8_t> const frame = port_->tx_begin(MAX_FRAME);
    if (frame.empty()) {
        return false;
    }
    size_t const frame_len = serialize_crf_raw(*slot.crf, frame, timestamps_ns, timestamp_uncertain(steady_now_ns()));
    if (frame_len == 0) {
        return false;
    }
    send_stream_frame(slot, frame.first(frame_len));
    return true;
}

auto TalkerStreams::open_tx(StreamPort& port, TxPcapConfig const& pcap) -> Status
{
    port_ = &port;
    // Optional TX stream capture: qdisc-bypass egress is invisible to any
    // local capture, so tap the frames at the port, gPTP-timestamped.
    if (!pcap.path.empty()) {
        tx_pcap_recorder_.configure(
            std::string{pcap.path}, pcap.max_bytes, /*snaplen=*/1522, static_cast<uint64_t>(pcap.seconds) * 1'000'000'000ULL);
        port.set_tx_tap([this](std::span<uint8_t const> frame) { tx_pcap_recorder_.record(frame, last_tx_gptp_ns_); });
    }
    return success();
}

void TalkerStreams::transmit_if_due(
    TalkerStreamSlot& slot,
    ptpclient::MediaClockGenerator::Emit const& tick,
    bool const srp_gate_open,
    size_t const samples,
    std::span<float const> src)
{
    SlotGuard::Entry const entry{guards_[slot.spec.index]};
    if (!entry.entered()) {
        return;  // being re-shaped (reformat_stream): this tick sends nothing
    }
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
