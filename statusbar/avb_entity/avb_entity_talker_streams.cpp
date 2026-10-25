// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerStreams methods — the local AVB stream TX path. Slots are shaped by
// StreamSpecs derived from the entity blob (kit phase 1); the per-kind
// serialization bodies are unchanged from the named-slot era.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/net/net_rawnet.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

namespace {

/// One more in a counter that one thread writes: a load and a store, no read-modify-write.
void bump(std::atomic<uint32_t>& counter) noexcept
{
    counter.store(counter.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
}

/// One AVTPDU sent, with @p tu and @p mr: an mr that differs from the last AVTPDU's is a toggle.
void count_pdu(TalkerTxCounters& c, bool const tu, bool const mr) noexcept
{
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

/// @p k more in a counter that one thread writes.
void add(std::atomic<uint32_t>& counter, uint32_t const k) noexcept
{
    counter.store(counter.load(std::memory_order_relaxed) + k, std::memory_order_relaxed);
}

/// @p pdus AVTPDUs sent, @p tu_pdus of them with tu, every one with @p mr: an mr unlike the last AVTPDU's is a toggle.
void count_pdus(TalkerTxCounters& c, uint32_t const pdus, uint32_t const tu_pdus, bool const mr) noexcept
{
    add(c.pdus, pdus);
    add(c.tu_pdus, tu_pdus);
    if (c.have_mr && mr != c.last_mr) {
        bump(c.mr_toggles);
    }
    c.last_mr = mr;
    c.have_mr = true;
}

/// How many of @p sent's AVTPDUs are known to carry @p mr, the mr a toggle's hold is of: all of them without
/// last_mr; with it all when the wire carried @p mr before (@p c), the last one when it does from this report, else none.
auto pdus_carrying(TalkerTxCounters const& c, bool const mr, TalkerStreams::OffloadSent const& sent) noexcept -> uint32_t
{
    if (!sent.last_mr) {
        return sent.pdus;
    }
    if (*sent.last_mr != mr) {
        return 0;
    }
    return c.have_mr && c.last_mr == mr ? sent.pdus : 1U;
}

/// @p slot's serializer's next sequence_num.
auto next_sequence_for(TalkerStreamSlot const& slot) noexcept -> uint8_t
{
    if (slot.aaf) {
        return slot.aaf->sequence_num;
    }
    if (slot.am824) {
        return slot.am824->sequence_num;
    }
    return slot.crf ? slot.crf->sequence_num : uint8_t{0};
}

/// @p slot's serializer to give its next AVTPDU sequence_num @p sequence.
void set_sequence(TalkerStreamSlot& slot, uint8_t const sequence) noexcept
{
    if (slot.aaf) {
        slot.aaf->sequence_num = sequence;
    } else if (slot.am824) {
        slot.am824->sequence_num = sequence;
    } else if (slot.crf) {
        slot.crf->sequence_num = sequence;
    }
}

/// One slot's serializer for an AM824 spec.
auto open_am824(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id) -> Status
{
    auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
    if (!rate || spec.format.channels == 0) {
        return failure(std::errc::invalid_argument);
    }
    slot.am824.emplace(stream_id, *rate, static_cast<uint8_t>(spec.format.channels), /*pres_offset=*/0);
    return success();
}

/// One slot's serializer and reframer for an AAF spec. The frame is the
/// format's: its samples_per_frame, else the Table 18 count for its rate (6
/// at 44.1 kHz, not rate/8000 = 5).
auto open_aaf(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id, std::pmr::memory_resource* mr)
    -> Status
{
    auto const rate = avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz);
    slot.aaf_samples = static_cast<uint16_t>(aaf_samples_per_pdu(spec.format));
    if (!rate || spec.format.channels == 0 || slot.aaf_samples == 0) {
        return failure(std::errc::invalid_argument);
    }
    slot.aaf.emplace(stream_id, spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth, /*pres_offset=*/0);
    slot.reframer.emplace(spec.format.channels, slot.aaf_samples, 4, mr);
    return success();
}

/// One slot's serializer for a CRF spec.
auto open_crf(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id) -> Status
{
    if (spec.format.crf_base_frequency_hz == 0 || spec.format.crf_timestamps_per_pdu == 0 ||
        spec.format.crf_timestamps_per_pdu > TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU) {
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

/// The CRF header transmit_crf() sends for @p ctx's stream, sequence_num 0, tu and mr clear. Reads none of the
/// context's running state, so the reactor may call it while the media thread sends.
auto crf_header_for(avtp::CrfStreamOutputContext const& ctx) noexcept -> avtp::CrfPdu
{
    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(ctx.stream_id, ctx.base_frequency, ctx.pull, ctx.timestamp_interval, ctx.timestamps_per_packet);
    return pdu;
}

/// The serializer @p spec's kind calls for, engaged on @p slot.
auto open_kind(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id, std::pmr::memory_resource* mr)
    -> Status
{
    switch (spec.format.kind) {
        case StreamKind::am824:
            return open_am824(slot, spec, stream_id);
        case StreamKind::aaf:
            return open_aaf(slot, spec, stream_id, mr);
        case StreamKind::crf:
            return open_crf(slot, spec, stream_id);
        case StreamKind::other:
        default:
            return failure(std::errc::not_supported);
    }
}
/// The largest AM824/AAF stream frame built here: header plus the most samples
/// and channels a packet carries, 4 bytes each.
constexpr size_t MAX_AUDIO_FRAME = std::max(avtp::AafPdu::HEADER_LENGTH, avtp::Am824Pdu::HEADER_LENGTH) +
    (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);

/// The steady clock in ns: the time base of the tu window (note_grandmaster_changed).
[[nodiscard]] auto steady_now_ns() noexcept -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

auto TalkerStreams::shape(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id) -> Status
{
    return open_kind(slot, spec, stream_id, mem_resource_);
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
    keep_opened(std::move(slot));
    return success();
}

auto TalkerStreams::reformat_stream(StreamSpec const& spec) -> Status
{
    auto* const slot = slot_for(spec.index);
    if (slot == nullptr) {
        return failure(std::errc::no_such_device);
    }
    if (!kind_change_allowed(slot->spec.format.kind, spec.format.kind)) {
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
    // The stream goes on: its sequence number and sample count (the AM824 DBC) carry over.
    uint8_t const sequence = slot->aaf ? slot->aaf->sequence_num : slot->am824 ? slot->am824->sequence_num : 0;
    uint32_t const samples = slot->aaf ? slot->aaf->running_sample_count : slot->am824 ? slot->am824->running_dbc : 0;
    if (trial.aaf) {
        trial.aaf->sequence_num = sequence;
        trial.aaf->running_sample_count = samples;
    }
    if (trial.am824) {
        trial.am824->sequence_num = sequence;
        trial.am824->running_dbc = samples;
    }
    slot->am824 = trial.am824;
    slot->aaf = trial.aaf;
    slot->aaf_samples = trial.aaf_samples;
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
    TalkerStreamSlot& slot, realtime::GptpClock0::time_point const presentation, uint32_t const samples, std::span<float const> src)
{  // NOLINT(statusbar-block-statements) one frame, built field by field; pre-dates the gate
    if (!slot.am824 || port_ == nullptr) {
        return;
    }
    // The wire takes the presentation time as unsigned nanoseconds (and keeps 32 bits of it).
    auto const presentation_ns = static_cast<uint64_t>(realtime::GptpClock0::to_ns(presentation));
    std::array<uint8_t, MAX_AUDIO_FRAME> frame{};

    size_t const channels = slot.spec.format.channels;
    avtp::Am824Pdu pdu{};
    // The FDF is the stream's own rate, which a SET_STREAM_FORMAT may have changed.
    pdu.init(slot.am824->stream_id, static_cast<uint8_t>(channels), slot.am824->config.sample_rate);

    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::am824_serialize_mbla(
        *slot.am824,
        pdu,
        payload,
        static_cast<uint8_t>(samples),
        presentation.time_since_epoch(),  // the first sample's time; the context's offset is zero
        [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    pdu.stream_header.set_tu(timestamp_uncertain(steady_now_ns()));
    span_store(std::span<uint8_t>{frame}.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::Am824Pdu::HEADER_LENGTH + audio_bytes;
    send_stream_frame(slot, std::span<uint8_t const>{frame.data(), frame_len});
}

void TalkerStreams::transmit_aaf(
    TalkerStreamSlot& slot, realtime::GptpClock0::time_point const presentation, uint16_t const samples, std::span<float const> src)
{  // NOLINT(statusbar-block-statements) one frame, built field by field; pre-dates the gate
    if (!slot.aaf || port_ == nullptr) {
        return;
    }
    auto const presentation_ns = static_cast<uint64_t>(realtime::GptpClock0::to_ns(presentation));
    std::array<uint8_t, MAX_AUDIO_FRAME> frame{};

    size_t const channels = slot.spec.format.channels;
    avtp::AafPdu pdu{};
    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::aaf_stream_serialize(
        *slot.aaf, pdu, payload, samples, presentation.time_since_epoch(), [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    pdu.set_tu(timestamp_uncertain(steady_now_ns()));
    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::AafPdu::HEADER_LENGTH + audio_bytes;
    send_stream_frame(slot, std::span<uint8_t const>{frame.data(), frame_len});
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
    std::array<uint8_t, MAX_AUDIO_FRAME> frame;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t const frame_len =
        serialize_aaf_raw(*slot.aaf, frame, frames, first_index, pts_ns, render, timestamp_uncertain(steady_now_ns()));
    if (frame_len == 0) {
        return false;
    }
    send_stream_frame(slot, std::span<uint8_t const>{frame.data(), frame_len});
    return true;
}

auto serialize_am824_raw(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const frames,
    uint64_t const first_index,
    uint64_t const pts_ns,
    StreamRawRenderFn const& render,
    bool const timestamp_uncertain) -> size_t
{
    uint8_t const channels = ctx.config.channel_count;
    size_t const payload_len = static_cast<size_t>(frames) * channels * avtp::Am824Pdu::BYTES_PER_SAMPLE;
    if (frames == 0 || frames > avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET || channels == 0 || !render ||
        frame.size() < avtp::Am824Pdu::HEADER_LENGTH + payload_len) {
        return 0;
    }
    std::span<uint8_t> const payload = frame.subspan(avtp::Am824Pdu::HEADER_LENGTH, payload_len);
    // Render first: a refused packet must not consume a sequence number.
    if (!render(payload, frames, first_index, pts_ns)) {
        return 0;
    }
    avtp::Am824Pdu pdu{};
    pdu.init(ctx.stream_id, channels, ctx.config.sample_rate);
    // The DBC is the sample index; build_packet_header adds the context's offset, already in pts_ns.
    ctx.running_dbc = static_cast<uint32_t>(first_index);
    ctx.build_packet_header(pdu, static_cast<uint8_t>(frames), pts_ns - ctx.presentation_offset_ns);
    pdu.stream_header.set_tu(timestamp_uncertain);
    span_store(frame.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    return avtp::Am824Pdu::HEADER_LENGTH + payload_len;
}

auto TalkerStreams::transmit_am824_raw(
    TalkerStreamSlot& slot,
    uint64_t const first_index,
    uint64_t const pts_ns,
    uint16_t const frames,
    StreamRawRenderFn const& render) -> bool
{
    SlotGuard::Entry const entry{guards_[slot.spec.index]};
    if (!entry.entered() || !slot.am824 || port_ == nullptr || stream_stopped(slot.spec.index)) {
        return false;  // being re-shaped (reformat_stream), or not sending
    }
    // Not zeroed: serialize_am824_raw writes every byte it sends.
    std::array<uint8_t, MAX_AUDIO_FRAME> frame;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t const frame_len =
        serialize_am824_raw(*slot.am824, frame, frames, first_index, pts_ns, render, timestamp_uncertain(steady_now_ns()));
    if (frame_len == 0) {
        return false;
    }
    send_stream_frame(slot, std::span<uint8_t const>{frame.data(), frame_len});
    return true;
}

void TalkerStreams::send_stream_frame(TalkerStreamSlot& slot, std::span<uint8_t const> const frame)
{
    // Frames carry the SR class VID + PCP (untagged AVTP is in no SR class). The pcap stamp is the
    // gPTP wall-clock transmit time, so a capture shows avtp_ts - wall_clock = presentation_offset.
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    if (port_ != nullptr) {
        send_stream_frame(slot.dest_mac, frame);
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
{  // NOLINT(statusbar-block-statements) one frame, built field by field; pre-dates the gate
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
    // Not zeroed: serialize_crf_raw writes every byte it sends.
    std::array<uint8_t, MAX_FRAME> frame;  // NOLINT(cppcoreguidelines-pro-type-member-init)
    size_t const frame_len = serialize_crf_raw(*slot.crf, frame, timestamps_ns, timestamp_uncertain(steady_now_ns()));
    if (frame_len == 0) {
        return false;
    }
    send_stream_frame(slot, std::span<uint8_t const>{frame.data(), frame_len});
    return true;
}

auto TalkerStreams::open_tx(StreamPort& port, TxPcapConfig const& pcap) -> Status
{
    // A port that did not open is not bound: the slots then stay silent, as
    // they did when the socket would not open.
    port_ = port.valid() ? &port : nullptr;
    // Optional TX stream capture: the egress of a bypass socket or a ring
    // is invisible to any local capture, so the frames are recorded here as
    // they are built, gPTP-timestamped.
    if (!pcap.path.empty()) {
        tx_pcap_recorder_.configure(
            std::string{pcap.path}, pcap.max_bytes, /*snaplen=*/1522, static_cast<uint64_t>(pcap.seconds) * 1'000'000'000ULL);
    }
    return port.valid() ? success() : failure(std::errc::not_connected);
}

void TalkerStreams::close_tx() noexcept
{
    slots_.clear();
    port_ = nullptr;
    external_kick_ = false;
    for (uint16_t idx = 0; idx < MAX_ENTITY_STREAMS; ++idx) {
        mark_open(idx, false);
        stream_stopped_[idx].store(false, std::memory_order_release);
        follow_streaming(idx, false);       // the media thread has stopped: so has the stream
        tx_counters_[idx].have_mr = false;  // a reopened stream's first mr is no toggle
    }
}

auto TalkerStreams::toggle_media_clock_restart(uint16_t const stream_index) noexcept -> Status
{
    // The open flag and not slot_for(): the slots are the opening thread's, and this may be any.
    if (stream_index >= mr_requests_.size() || !stream_open_[stream_index].load(std::memory_order_acquire)) {
        return failure(std::errc::invalid_argument);
    }
    (void)mr_requests_[stream_index].fetch_add(1, std::memory_order_relaxed);  // several writers: a real add
    return success();
}

auto TalkerStreams::media_clock_restart_requests(uint16_t const stream_index) const noexcept -> uint32_t
{
    return mr_requests(stream_index);
}

auto TalkerStreams::set_signal_presence(uint16_t const stream_index, uint64_t const channels) noexcept -> Status
{
    if (stream_index >= signal_presence_.size()) {
        return failure(std::errc::invalid_argument);
    }
    uint64_t const reported = channels & ((uint64_t{1} << SIGNAL_PRESENCE_CHANNELS) - 1U);
    signal_presence_[stream_index].store(SIGNAL_PRESENCE_REPORTED | reported, std::memory_order_relaxed);
    return success();
}

auto TalkerStreams::signal_presence(uint16_t const stream_index) const noexcept -> std::optional<uint64_t>
{
    uint64_t const word =
        stream_index < signal_presence_.size() ? signal_presence_[stream_index].load(std::memory_order_relaxed) : 0U;
    return (word & SIGNAL_PRESENCE_REPORTED) != 0 ? std::optional<uint64_t>{word & ~SIGNAL_PRESENCE_REPORTED} : std::nullopt;
}

auto TalkerStreams::mr_requests(uint16_t const stream_index) const noexcept -> uint32_t
{
    return stream_index < mr_requests_.size() ? mr_requests_[stream_index].load(std::memory_order_relaxed) : 0U;
}

auto TalkerStreams::restart_owed(TalkerStreamSlot const& slot) const noexcept -> bool
{
    uint64_t const steps = media_clock_.step_count();
    return mr_requests(slot.spec.index) != slot.mr_requests_seen || steps != slot.media_clock_steps.value_or(steps);
}

auto TalkerStreams::next_mr(TalkerStreamSlot& slot) noexcept -> bool
{
    slot.media_clock_steps = slot.media_clock_steps.value_or(media_clock_.step_count());  // the first AVTPDU's
    if (slot.mr_hold == 0 && restart_owed(slot)) {
        slot.mr = !slot.mr;
        slot.mr_hold = MR_HOLD_PDUS;
        slot.mr_requests_seen = mr_requests(slot.spec.index);  // every restart so far: one toggle
        slot.media_clock_steps = media_clock_.step_count();
    }
    return slot.mr;
}

void TalkerStreams::count_sent(TalkerStreamSlot& slot, bool const tu, bool const mr) noexcept
{
    ++slot.tx_packets;
    slot.mr_hold -= slot.mr_hold != 0 ? 1 : 0;  // the hold is of AVTPDUs sent
    if (slot.spec.index < tx_counters_.size()) {
        count_pdu(tx_counters_[slot.spec.index], tu, mr);
    }
}

void TalkerStreams::follow_streaming(uint16_t const stream_index, bool const on) noexcept
{
    if (stream_index >= tx_counters_.size()) {
        return;
    }
    auto& changes = tx_counters_[stream_index].streaming_changes;
    bool const streaming = (changes.load(std::memory_order_relaxed) % 2U) != 0;
    if (on != streaming) {
        bump(changes);
    }
}

void TalkerStreams::send_stream_frame(ieee::Eui48 const& dest_mac, std::span<uint8_t const> const payload)
{
    StreamPort::Guard const guard{*port_};
    auto slot = port_->port().tx_start();
    if (!slot) {
        return;
    }
    size_t const length =
        build_stream_frame(slot->buffer, dest_mac, port_->port().hardware_address(), vlan_id(), stream_pcp(), payload);
    if (length == 0) {
        (void)port_->port().tx_cancel(slot->handle);
        return;
    }
    commit_stream_frame(*slot, length);
}

void TalkerStreams::commit_stream_frame(net::EthernetTxSlot const& slot, size_t const length)
{
    // Recorded before the commit: a ring slot belongs to the kernel after it.
    if (tx_pcap_recorder_.recording()) {
        tx_pcap_recorder_.record(slot.buffer.first(length), last_tx_gptp_ns_);
    }
    (void)port_->port().tx_commit(slot.handle, length);
}

void TalkerStreams::flush_tx()
{
    if (port_ == nullptr) {
        return;
    }
    StreamPort::Guard const guard{*port_};
    if (!external_kick_) {
        (void)port_->port().tx_flush();
    }
    (void)port_->port().end_cycle();
}

auto TalkerStreams::set_external_kick(bool const enable) noexcept -> bool
{
    bool const kickable = port_ != nullptr;
    external_kick_ = enable && kickable;
    return !enable || kickable;
}

void TalkerStreams::kick_tx()
{
    if (port_ != nullptr) {
        (void)port_->port().tx_flush();
    }
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
    follow_streaming(slot.spec.index, gate_open);
    switch (slot.spec.format.kind) {
        case StreamKind::am824: {
            if (gate_open && samples > 0) {
                transmit_am824(slot, presentation_for(tick.first_index), tick.samples, src);
            }
            break;
        }
        case StreamKind::aaf:
            serve_aaf(slot, tick, gate_open, samples, src);
            break;
        case StreamKind::crf:
            serve_crf(slot, tick, gate_open, samples);
            break;
        case StreamKind::other:
        default:
            break;
    }
}

void TalkerStreams::serve_aaf(
    TalkerStreamSlot& slot,
    ptpclient::MediaClockGenerator::Emit const& tick,
    bool const gate_open,
    size_t const samples,
    std::span<float const> src)
{
    if (!slot.reframer) {
        return;
    }
    if (!gate_open) {
        // Gate closed: drop any partial block so a later reconnect starts
        // clean (no stale samples / stale timestamps).
        slot.reframer->clear();
        return;
    }
    // AAF frames are constant-size (1722-2025 7.7): buffer this wake's
    // variable samples and emit only whole blocks of the format's samples per
    // AVTPDU (0, 1, or 2+ this wake); the remainder carries to the next wake.
    // The cadence is the media clock's, not the wake's: 6 samples at 44.1 kHz
    // is one frame per ~136 us, with a wake now and then that sends none. Each
    // block's avtp_timestamp is the jitter-free media-clock time of its first
    // sample.
    slot.reframer->push(src.first(samples * slot.spec.format.channels), static_cast<uint16_t>(samples), tick.first_index);
    slot.reframer->drain([this, &slot](uint64_t first_index, std::span<float const> block) {
        transmit_aaf(slot, presentation_for(first_index), slot.aaf_samples, block);
    });
}

void TalkerStreams::serve_crf(
    TalkerStreamSlot& slot, ptpclient::MediaClockGenerator::Emit const& tick, bool const gate_open, size_t const samples)
{
    if (!slot.crf) {
        return;
    }
    if (!gate_open) {
        slot.crf_due.reset();  // gate closed: the next PDU is the first one owed after the gate opens
        return;
    }
    // One PDU carries timestamps_per_packet timestamps, each spaced
    // sample_stride audio samples, so a PDU is owed every span samples of the
    // media clock (the Milan 48 kHz/interval-96/1-ts format under 96 kHz audio:
    // every 192 samples, 500 PDU/s; the same format at 44.1 kHz: every 96
    // samples, 459.375 PDU/s). Counting samples rather than wakes is what
    // gives a rate the wake cadence does not divide its exact count.
    uint32_t const sample_stride = crf_sample_stride(slot.crf->timestamp_interval, slot.crf->base_frequency, config_.sample_rate);
    auto const plan = crf_schedule(
        slot.crf_due,
        tick.first_index,
        static_cast<uint32_t>(samples),
        sample_stride,
        static_cast<uint64_t>(slot.crf->timestamps_per_packet) * sample_stride);
    slot.crf_due = plan.due;
    if (plan.send) {
        transmit_crf(slot, *plan.send);
    }
}

//
// Stream frame templates
//

namespace {

/// Where a template's varying fields sit in its AVTPDU header (timestamp 0 for one it does not carry).
struct PduOffsets
{
    size_t sequence;
    size_t timestamp;
    size_t mr;
    size_t tu;
};

constexpr PduOffsets AAF_OFFSETS{
    .sequence = offsetof(avtp::AafPdu, sequence_num),
    .timestamp = offsetof(avtp::AafPdu, avtp_timestamp),
    .mr = offsetof(avtp::AafPdu, sv_version_flags),
    .tu = offsetof(avtp::AafPdu, reserved_tu)};
constexpr PduOffsets CRF_OFFSETS{
    .sequence = offsetof(avtp::CrfPdu, sequence_num),
    .timestamp = 0,
    .mr = offsetof(avtp::CrfPdu, sv_version_mr_r_fs_tu),
    .tu = offsetof(avtp::CrfPdu, sv_version_mr_r_fs_tu)};

/// @p result's varying fields at @p offsets in the AVTPDU header that starts at its octet @p at.
void place_fields(StreamFrameTemplate& result, size_t const at, PduOffsets const offsets) noexcept
{
    result.sequence_offset = at + offsets.sequence;
    result.timestamp_offset = offsets.timestamp != 0 ? at + offsets.timestamp : 0;
    result.mr_offset = at + offsets.mr;
    result.tu_offset = at + offsets.tu;
}

/// A template of @p header and the AVTPDU header @p pdu behind it, its varying fields at @p offsets.
template <typename Pdu>
[[nodiscard]] auto template_for(ieee::EthernetFrame const& header, Pdu const& pdu, PduOffsets const offsets) noexcept
    -> StreamFrameTemplate
{
    StreamFrameTemplate result{};
    size_t const at = ieee::store_unchecked(std::span{result.bytes}, header);
    span_store(std::span{result.bytes}.subspan(at, Pdu::HEADER_LENGTH), pdu);
    result.length = at + Pdu::HEADER_LENGTH;
    place_fields(result, at, offsets);
    return result;
}

/// @p flag set in the octet at the start of @p out when @p on, cleared when not.
void store_flag(std::span<uint8_t> const out, uint8_t const flag, bool const on) noexcept
{
    ieee::octet_t octet{};
    span_load(octet, out.first(1));
    octet.set_flag(flag, on);
    span_store(out.first(1), octet);
}

/// @p tmpl's varying fields stored in @p out, a frame its header was copied to.
void store_fields(
    StreamFrameTemplate const& tmpl,
    std::span<uint8_t> const out,
    uint8_t const sequence,
    uint32_t const timestamp,
    bool const mr,
    bool const tu) noexcept
{
    span_store(out.subspan(tmpl.sequence_offset, 1), ieee::octet_t{sequence});
    if (tmpl.timestamp_offset != 0) {
        span_store(out.subspan(tmpl.timestamp_offset, 4), ieee::quadlet_t{timestamp});
    }
    store_flag(out.subspan(tmpl.mr_offset), StreamFrameTemplate::MR_BIT, mr);
    store_flag(out.subspan(tmpl.tu_offset), StreamFrameTemplate::TU_BIT, tu);
}

}  // namespace

auto StreamFrameTemplate::write(
    uint8_t const sequence, uint32_t const timestamp, bool const mr, bool const tu, std::span<uint8_t> const out) const noexcept
    -> bool
{
    if (out.size() < length) {
        return false;
    }
    span_copy(out.first(length), header());
    store_fields(*this, out, sequence, timestamp, mr, tu);
    return true;
}

auto aaf_frame_template(avtp::AafStreamOutputContext const& ctx, uint16_t const frames, ieee::EthernetFrame const& header) noexcept
    -> std::optional<StreamFrameTemplate>
{
    size_t const payload = static_cast<size_t>(frames) * ctx.channel_count * avtp::aaf_bytes_per_sample(ctx.format);
    if (payload == 0 || avtp::AafPdu::HEADER_LENGTH + payload > net::MAX_ETHERNET_PAYLOAD) {
        return std::nullopt;
    }
    // The header aaf_stream_serialize() builds, from a serializer of the stream at sequence 0, presenting at 0.
    avtp::AafStreamOutputContext twin{ctx.stream_id, ctx.format, ctx.sample_rate, ctx.channel_count, ctx.bit_depth, 0};
    avtp::AafPdu pdu{};
    twin.build_packet_header(pdu, frames, 0);
    return template_for(header, pdu, AAF_OFFSETS);
}

auto crf_frame_template(avtp::CrfStreamOutputContext const& ctx, ieee::EthernetFrame const& header) noexcept
    -> std::optional<StreamFrameTemplate>
{
    if (ctx.timestamps_per_packet == 0 || ctx.timestamps_per_packet > TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU) {
        return std::nullopt;
    }
    return template_for(header, crf_header_for(ctx), CRF_OFFSETS);
}

auto TalkerStreams::frame_template(uint16_t const stream_index, ieee::Eui48 const& src_mac) const noexcept
    -> std::optional<StreamFrameTemplate>
{
    auto const* const slot = slot_for(stream_index);
    if (slot == nullptr) {
        return std::nullopt;
    }
    auto const header = stream_frame_header(slot->dest_mac, src_mac, vlan_id(), stream_pcp());
    if (slot->aaf) {
        return aaf_frame_template(*slot->aaf, slot->aaf_samples, header);
    }
    return slot->crf ? crf_frame_template(*slot->crf, header) : std::nullopt;
}

//
// A talker stream sent beside the stream port
//

auto TalkerStreams::offload_header(TalkerStreamSlot& slot) noexcept -> OffloadHeader
{
    return offload_header_for(slot, next_mr(slot));
}

auto TalkerStreams::offload_header_for(TalkerStreamSlot const& slot, bool const mr) const noexcept -> OffloadHeader
{
    return {.sequence_num = next_sequence_for(slot), .mr = mr, .tu = wake_timestamp_uncertain()};
}

auto TalkerStreams::notify_offload_sent(TalkerStreamSlot& slot, bool const gate_open, OffloadSent const& sent) noexcept
    -> OffloadHeader
{
    bool const streams = gate_open && !stream_stopped(slot.spec.index);
    follow_streaming(slot.spec.index, streams);
    empty_own_schedule(slot);
    count_offload_sent(slot, sent);
    // As the kit's own path, which asks next_mr() only for an AVTPDU it sends.
    return streams ? offload_header(slot) : offload_header_for(slot, slot.mr);
}

void TalkerStreams::empty_own_schedule(TalkerStreamSlot& slot) noexcept
{
    // The kit's own path starts clean if the stream comes back to it, as after a closed gate.
    if (slot.reframer) {
        slot.reframer->clear();
    }
    slot.crf_due.reset();
}

void TalkerStreams::count_offload_sent(TalkerStreamSlot& slot, OffloadSent const& sent) noexcept
{
    if (sent.pdus == 0) {
        return;
    }
    slot.tx_packets += sent.pdus;
    set_sequence(slot, sent.next_sequence);
    if (slot.spec.index < tx_counters_.size()) {
        auto& c = tx_counters_[slot.spec.index];
        slot.mr_hold -= static_cast<uint8_t>(std::min<uint32_t>(slot.mr_hold, pdus_carrying(c, slot.mr, sent)));
        count_pdus(c, sent.pdus, std::min(sent.tu_pdus, sent.pdus), sent.last_mr.value_or(slot.mr));
    }
}

}  // namespace statusbar::avb_entity
