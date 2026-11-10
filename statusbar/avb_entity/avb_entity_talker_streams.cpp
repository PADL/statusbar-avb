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
#include <mutex>
#include <optional>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

namespace {

/// The window an audio AVTPDU is serialized into: the most a tagged frame
/// carries (a format needing more is refused by its serializer).
constexpr size_t MAX_AUDIO_PAYLOAD = net::MAX_ETHERNET_PAYLOAD;

/// The stream's sequence number and sample count (the AM824 DBC) from @p from into
/// @p to's serializer, so that a re-shaped stream goes on.
void carry_over(TalkerStreamSlot const& from, TalkerStreamSlot& to) noexcept
{
    uint8_t const sequence = from.aaf ? from.aaf->sequence_num : from.am824 ? from.am824->sequence_num : 0;
    uint32_t const samples = from.aaf ? from.aaf->running_sample_count : from.am824 ? from.am824->running_dbc : 0;
    if (to.aaf) {
        to.aaf->sequence_num = sequence;
        to.aaf->running_sample_count = samples;
    }
    if (to.am824) {
        to.am824->sequence_num = sequence;
        to.am824->running_dbc = samples;
    }
}

/// One more in a counter only the media thread writes.
void bump(std::atomic<uint32_t>& n) noexcept
{
    n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

/// Count an mr toggle when @p mr differs from the last PDU's.
void note_mr(TalkerTxCounters& c, bool const mr) noexcept
{
    if (c.have_mr && mr != c.last_mr) {
        bump(c.mr_toggles);
    }
    c.last_mr = mr;
    c.have_mr = true;
}

/// The first octets of @p pdu, which count_pdu reads, or none for a shorter one.
[[nodiscard]] auto pdu_head(std::span<uint8_t const> const pdu) noexcept -> std::optional<std::array<uint8_t, 4>>
{
    if (pdu.size() < 4) {
        return std::nullopt;
    }
    std::array<uint8_t, 4> head{};
    span_copy(std::span<uint8_t>{head}, pdu.first(head.size()));
    return head;
}

/// Count the sent AVTPDU @p frame (at least 4 octets): mr is bit 3 of octet 1; tu is bit 0 of
/// octet 3, or of octet 1 in a CRF PDU (IEEE 1722-2016 4.4.4, 10.4).
void count_pdu(TalkerTxCounters& c, std::span<uint8_t const> const frame) noexcept
{
    bool const tu = ((frame[0] == avtp::AvtpSubtype::crf ? frame[1] : frame[3]) & 0x01U) != 0;
    bump(c.pdus);
    if (tu) {
        bump(c.tu_pdus);
    }
    note_mr(c, (frame[1] & 0x08U) != 0);
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
            slot.aaf.emplace(
                stream_id, spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth, /*pres_offset=*/0);
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
    TalkerStreamSlot slot{.spec = spec, .dest_mac = dest_mac};
    if (auto status = shape(slot, spec, stream_id); !status) {
        return status;
    }
    slots_.push_back(std::move(slot));
    mark_open(spec.index);
    return success();
}

void TalkerStreams::mark_open(uint16_t const stream_index) noexcept
{
    // A request that raced a stop is no restart of this stream: cleared before a toggle may be accepted here.
    mr_pending_[stream_index].store(false, std::memory_order_relaxed);
    open_[stream_index].store(true, std::memory_order_release);
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
    return reshape(*slot, spec);
}

auto TalkerStreams::reshape(TalkerStreamSlot& slot, StreamSpec const& spec) -> Status
{
    auto const stream_id = slot.aaf ? slot.aaf->stream_id : slot.am824 ? slot.am824->stream_id : tsn::StreamId{};
    // A trial first, so that a refused format leaves the slot as it was.
    TalkerStreamSlot trial{};
    if (auto status = shape(trial, spec, stream_id); !status) {
        return status;
    }
    std::scoped_lock const lock{guards_[spec.index]};
    adopt_shape(slot, std::move(trial), spec);
    return success();
}

void TalkerStreams::adopt_shape(TalkerStreamSlot& slot, TalkerStreamSlot&& trial, StreamSpec const& spec) noexcept
{
    carry_over(slot, trial);
    slot.am824 = trial.am824;
    slot.aaf = trial.aaf;
    slot.reframer = std::move(trial.reframer);
    // The index stays (slot_for reads it unlocked); the format is the new one.
    slot.spec.format_word = spec.format_word;
    slot.spec.format = spec.format;
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

auto serialize_am824_float(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint32_t const samples,
    uint64_t const now_ns,
    std::span<float const> const src,
    bool const timestamp_uncertain,
    bool const media_clock_restart) -> size_t
{  // NOLINT(statusbar-block-statements): one frame, built field by field
    span_zero(frame);
    size_t const channels = ctx.config.channel_count;
    avtp::Am824Pdu pdu{};
    // The FDF is the stream's own rate, which a SET_STREAM_FORMAT may have changed.
    pdu.init(ctx.stream_id, static_cast<uint8_t>(channels), ctx.config.sample_rate);
    size_t const audio_bytes = avtp::am824_serialize_mbla(
        ctx,
        pdu,
        frame.subspan(avtp::Am824Pdu::HEADER_LENGTH),
        static_cast<uint8_t>(samples),
        now_ns,
        [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return 0;
    }
    pdu.stream_header.set_tu(timestamp_uncertain);
    pdu.stream_header.set_mr(media_clock_restart);
    span_store(frame.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    return avtp::Am824Pdu::HEADER_LENGTH + audio_bytes;
}

auto serialize_aaf_float(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const samples,
    uint64_t const now_ns,
    std::span<float const> const src,
    bool const timestamp_uncertain,
    bool const media_clock_restart) -> size_t
{  // NOLINT(statusbar-block-statements): one frame, built field by field
    span_zero(frame);
    size_t const channels = ctx.channel_count;
    avtp::AafPdu pdu{};
    size_t const audio_bytes = avtp::aaf_stream_serialize(
        ctx, pdu, frame.subspan(avtp::AafPdu::HEADER_LENGTH), samples, now_ns, [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return 0;
    }
    pdu.set_tu(timestamp_uncertain);
    pdu.set_mr(media_clock_restart);
    span_store(frame.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    return avtp::AafPdu::HEADER_LENGTH + audio_bytes;
}

void TalkerStreams::transmit_am824(
    TalkerStreamSlot& slot, realtime::GptpClock0::time_point const presentation, uint32_t const samples, std::span<float const> src)
{
    if (!slot.am824 || port_ == nullptr) {
        return;
    }
    // The wire takes the presentation time as unsigned nanoseconds (and keeps 32 bits of it).
    auto const presentation_ns = static_cast<uint64_t>(realtime::GptpClock0::to_ns(presentation));
    bool const tu = timestamp_uncertain(net::monotonic_ns());
    (void)send_serialized(slot, *slot.am824, MAX_AUDIO_PAYLOAD, 0, [&](std::span<uint8_t> const frame) {
        return serialize_am824_float(*slot.am824, frame, samples, presentation_ns, src, tu, next_mr(slot));
    });
}

void TalkerStreams::transmit_aaf(
    TalkerStreamSlot& slot, realtime::GptpClock0::time_point const presentation, uint16_t const samples, std::span<float const> src)
{
    if (!slot.aaf || port_ == nullptr) {
        return;
    }
    auto const presentation_ns = static_cast<uint64_t>(realtime::GptpClock0::to_ns(presentation));
    bool const tu = timestamp_uncertain(net::monotonic_ns());
    (void)send_serialized(slot, *slot.aaf, MAX_AUDIO_PAYLOAD, 0, [&](std::span<uint8_t> const frame) {
        return serialize_aaf_float(*slot.aaf, frame, samples, presentation_ns, src, tu, next_mr(slot));
    });
}

namespace {

/// The tu and mr bits of a raw packet's stream header.
struct HeaderFlags
{
    bool timestamp_uncertain{false};
    bool media_clock_restart{false};
};

/// The AAF header of a raw packet of @p frames presented at @p pts_ns into @p frame.
void store_aaf_raw_header(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const frames,
    uint64_t const pts_ns,
    HeaderFlags const flags) noexcept
{
    avtp::AafPdu pdu{};
    ctx.build_packet_header(pdu, frames, pts_ns);
    // The wire timestamp is pts_ns itself: build_packet_header adds the context's
    // presentation offset, which the caller has already applied to pts_ns.
    pdu.set_avtp_timestamp(static_cast<uint32_t>(pts_ns & 0xFFFF'FFFFU));
    pdu.set_tu(flags.timestamp_uncertain);
    pdu.set_mr(flags.media_clock_restart);
    span_store(frame.first(avtp::AafPdu::HEADER_LENGTH), pdu);
}

/// The AM824 header of a raw packet of @p frames from sample @p first_index, presented at
/// @p pts_ns, into @p frame.
void store_am824_raw_header(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const frames,
    uint64_t const first_index,
    uint64_t const pts_ns,
    HeaderFlags const flags) noexcept
{  // NOLINT(statusbar-block-statements): a header, built field by field
    avtp::Am824Pdu pdu{};
    pdu.init(ctx.stream_id, ctx.config.channel_count, ctx.config.sample_rate);
    // The DBC is the sample index; build_packet_header adds the context's offset, already in pts_ns.
    ctx.running_dbc = static_cast<uint32_t>(first_index);
    ctx.build_packet_header(pdu, static_cast<uint8_t>(frames), pts_ns - ctx.presentation_offset_ns);
    pdu.stream_header.set_tu(flags.timestamp_uncertain);
    pdu.stream_header.set_mr(flags.media_clock_restart);
    span_store(frame.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
}

}  // namespace

auto serialize_aaf_raw(
    avtp::AafStreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const frames,
    uint64_t const first_index,
    uint64_t const pts_ns,
    StreamRawRenderFn const& render,
    bool const timestamp_uncertain,
    bool const media_clock_restart) -> size_t
{
    size_t const bps = avtp::aaf_bytes_per_sample(ctx.format);
    size_t const payload_len = static_cast<size_t>(frames) * ctx.channel_count * bps;
    if (bps == 0 || payload_len == 0 || !render || frame.size() < avtp::AafPdu::HEADER_LENGTH + payload_len) {
        return 0;
    }
    // Render first: a refused packet must not consume a sequence number.
    if (!render(frame.subspan(avtp::AafPdu::HEADER_LENGTH, payload_len), frames, first_index, pts_ns)) {
        return 0;
    }
    store_aaf_raw_header(
        ctx, frame, frames, pts_ns, {.timestamp_uncertain = timestamp_uncertain, .media_clock_restart = media_clock_restart});
    return avtp::AafPdu::HEADER_LENGTH + payload_len;
}

template <typename Context, typename Serialize>
auto TalkerStreams::send_serialized(
    TalkerStreamSlot& slot, Context& ctx, size_t const max_frame, uint64_t const launch_time_ns, Serialize const& serialize) -> bool
{
    if (port_ == nullptr) {
        return false;
    }
    // On a port another thread also drives, the render stays outside the guard: staged in tx_buf_.
    return port_->writes_in_place() ? send_in_place(slot, ctx, max_frame, launch_time_ns, serialize)
                                    : send_staged(slot, ctx, max_frame, launch_time_ns, serialize);
}

template <typename Context, typename Serialize>
auto TalkerStreams::send_in_place(
    TalkerStreamSlot& slot, Context& ctx, size_t const max_frame, uint64_t const launch_time_ns, Serialize const& serialize) -> bool
{
    auto const frame = port_->port().tx_start();  // timed at the commit: after the render
    return frame && serialize_into(slot, ctx, *frame, max_frame, launch_time_ns, serialize);
}

template <typename Context, typename Serialize>
auto TalkerStreams::send_staged(
    TalkerStreamSlot& slot, Context& ctx, size_t const max_frame, uint64_t const launch_time_ns, Serialize const& serialize) -> bool
{
    std::span<uint8_t> const frame = tx_begin(max_frame);
    auto const saved = ctx;  // restored when the port refuses the frame
    size_t const length = frame.empty() ? 0 : serialize(frame);
    if (length == 0 || send_stream_frame(slot, frame.first(length), launch_time_ns)) {
        return length != 0;
    }
    ctx = saved;
    return false;
}

template <typename Context, typename Serialize>
auto TalkerStreams::serialize_into(
    TalkerStreamSlot& slot,
    Context& ctx,
    net::EthernetTxSlot const& frame,
    size_t const max_frame,
    uint64_t const launch_time_ns,
    Serialize const& serialize) -> bool
{
    size_t const header = store_stream_header(slot, frame.buffer);
    auto const saved = ctx;  // restored when the port refuses the frame
    size_t const length = header != 0 ? serialize(payload_window(frame.buffer, header, max_frame)) : 0;
    if (length == 0) {
        (void)port_->port().tx_cancel(frame.handle);  // nothing to send: the slot back
        return false;
    }
    if (!commit_frame(slot, frame, header, length, launch_time_ns)) {
        ctx = saved;
        return false;
    }
    return true;
}

auto TalkerStreams::transmit_aaf_raw(
    TalkerStreamSlot& slot,
    uint64_t const first_index,
    uint64_t const pts_ns,
    uint16_t const frames,
    StreamRawRenderFn const& render,
    uint64_t const launch_time_ns) -> bool
{
    std::unique_lock const entry{guards_[slot.spec.index], std::try_to_lock};
    if (!entry.owns_lock() || !slot.aaf || port_ == nullptr || stream_stopped(slot.spec.index)) {
        return false;  // being re-shaped (reformat_stream), or not sending
    }
    // Not zeroed: serialize_aaf_raw writes every byte it sends.
    return send_serialized(slot, *slot.aaf, MAX_AUDIO_PAYLOAD, launch_time_ns, [&](std::span<uint8_t> const frame) {
        return serialize_aaf_raw(
            *slot.aaf, frame, frames, first_index, pts_ns, render, timestamp_uncertain(net::monotonic_ns()), next_mr(slot));
    });
}

auto serialize_am824_raw(
    avtp::Am824StreamOutputContext& ctx,
    std::span<uint8_t> const frame,
    uint16_t const frames,
    uint64_t const first_index,
    uint64_t const pts_ns,
    StreamRawRenderFn const& render,
    bool const timestamp_uncertain,
    bool const media_clock_restart) -> size_t
{
    uint8_t const channels = ctx.config.channel_count;
    size_t const payload_len = static_cast<size_t>(frames) * channels * avtp::Am824Pdu::BYTES_PER_SAMPLE;
    if (frames == 0 || frames > avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET || channels == 0 || !render ||
        frame.size() < avtp::Am824Pdu::HEADER_LENGTH + payload_len) {
        return 0;
    }
    // Render first: a refused packet must not consume a sequence number.
    if (!render(frame.subspan(avtp::Am824Pdu::HEADER_LENGTH, payload_len), frames, first_index, pts_ns)) {
        return 0;
    }
    store_am824_raw_header(
        ctx,
        frame,
        frames,
        first_index,
        pts_ns,
        {.timestamp_uncertain = timestamp_uncertain, .media_clock_restart = media_clock_restart});
    return avtp::Am824Pdu::HEADER_LENGTH + payload_len;
}

auto TalkerStreams::transmit_am824_raw(
    TalkerStreamSlot& slot,
    uint64_t const first_index,
    uint64_t const pts_ns,
    uint16_t const frames,
    StreamRawRenderFn const& render,
    uint64_t const launch_time_ns) -> bool
{
    std::unique_lock const entry{guards_[slot.spec.index], std::try_to_lock};
    if (!entry.owns_lock() || !slot.am824 || port_ == nullptr || stream_stopped(slot.spec.index)) {
        return false;  // being re-shaped (reformat_stream), or not sending
    }
    // Not zeroed: serialize_am824_raw writes every byte it sends.
    return send_serialized(slot, *slot.am824, MAX_AUDIO_PAYLOAD, launch_time_ns, [&](std::span<uint8_t> const frame) {
        return serialize_am824_raw(
            *slot.am824, frame, frames, first_index, pts_ns, render, timestamp_uncertain(net::monotonic_ns()), next_mr(slot));
    });
}

auto TalkerStreams::send_stream_frame(TalkerStreamSlot& slot, std::span<uint8_t const> const frame, uint64_t const launch_time_ns)
    -> bool
{
    // Frames carry the SR class VID + PCP (untagged AVTP is in no SR class). The pcap stamp is the
    // gPTP wall-clock transmit time, so a capture shows avtp_ts - wall_clock = presentation_offset.
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    if (port_ != nullptr && !send_to_port(slot.dest_mac, frame, launch_time_ns)) {
        return false;
    }
    count_sent(slot, frame);
    return true;
}

void TalkerStreams::count_sent(TalkerStreamSlot& slot, std::span<uint8_t const> const frame) noexcept
{
    ++slot.tx_packets;
    if (slot.spec.index < tx_counters_.size() && frame.size() >= 4) {
        count_pdu(tx_counters_[slot.spec.index], frame);
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
    bool const timestamp_uncertain,
    bool const media_clock_restart) -> size_t
{  // NOLINT(statusbar-block-statements): a codec: the CRF PDU in wire order, then the context's counters
    uint16_t const n_ts = ctx.timestamps_per_packet;
    size_t const frame_len = avtp::CrfPdu::HEADER_LENGTH + (static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE);
    if (n_ts == 0 || timestamps.size() != n_ts || frame.size() < frame_len) {
        return 0;
    }
    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(ctx.stream_id, ctx.base_frequency, ctx.pull, ctx.timestamp_interval, n_ts);
    pdu.set_sequence_num(ctx.sequence_num);
    pdu.set_tu(timestamp_uncertain);
    pdu.set_mr(media_clock_restart);
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

auto TalkerStreams::transmit_crf_raw(
    TalkerStreamSlot& slot, std::span<uint64_t const> const timestamps_ns, uint64_t const launch_time_ns) -> bool
{
    if (!slot.crf || port_ == nullptr || stream_stopped(slot.spec.index)) {
        return false;
    }
    static constexpr size_t MAX_FRAME = avtp::CrfPdu::HEADER_LENGTH + (MAX_CRF_TIMESTAMPS_PER_PDU * avtp::CrfPdu::TIMESTAMP_SIZE);
    static_assert(MAX_FRAME <= net::MAX_ETHERNET_PAYLOAD);
    // Not zeroed: serialize_crf_raw writes every byte it sends.
    return send_serialized(slot, *slot.crf, MAX_FRAME, launch_time_ns, [&](std::span<uint8_t> const frame) {
        return serialize_crf_raw(*slot.crf, frame, timestamps_ns, timestamp_uncertain(net::monotonic_ns()), next_mr(slot));
    });
}

auto TalkerStreams::next_mr(TalkerStreamSlot& slot) noexcept -> bool
{
    // Requests since the last toggle toggle mr once (a load first: no read-modify-write while none is pending), once
    // the last toggle has held for MR_HOLD_PDUS PDUs; this PDU is the first of its hold.
    auto& pending = mr_pending_[slot.spec.index];
    if (slot.mr_hold == 0 && pending.load(std::memory_order_relaxed) && pending.exchange(false, std::memory_order_relaxed)) {
        slot.mr = !slot.mr;
        slot.mr_hold = MR_HOLD_PDUS;
    }
    slot.mr_hold -= slot.mr_hold != 0 ? 1 : 0;
    return slot.mr;
}

auto TalkerStreams::toggle_media_clock_restart(uint16_t const stream_index) noexcept -> Status
{
    if (stream_index >= MAX_ENTITY_STREAMS || !open_[stream_index].load(std::memory_order_acquire)) {
        return failure(std::errc::invalid_argument);
    }
    mr_pending_[stream_index].store(true, std::memory_order_relaxed);
    return success();
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
    for (size_t idx = 0; idx < MAX_ENTITY_STREAMS; ++idx) {
        stream_stopped_[idx].store(false, std::memory_order_release);
        open_[idx].store(false, std::memory_order_release);  // a toggle is refused until a stream opens here again
        mr_pending_[idx].store(false, std::memory_order_release);
        tx_counters_[idx].have_mr = false;  // a reopened stream's first mr is no toggle
    }
}

auto TalkerStreams::tx_begin(size_t const length) noexcept -> std::span<uint8_t>
{
    if (port_ == nullptr || length > tx_buf_.size()) {
        return {};
    }
    return std::span<uint8_t>{tx_buf_}.first(length);
}

auto TalkerStreams::store_stream_header(TalkerStreamSlot const& slot, std::span<uint8_t> const buffer) const noexcept -> size_t
{
    auto const tag = this->tag();
    auto const header = stream_frame_header(slot.dest_mac, port_->port().hardware_address(), tag.vlan_id, tag.pcp);
    return buffer.size() > ieee::wire_size(header) ? ieee::store_unchecked(buffer, header) : 0;
}

auto TalkerStreams::payload_window(std::span<uint8_t> const buffer, size_t const header, size_t const length) noexcept
    -> std::span<uint8_t>
{
    return buffer.subspan(header, std::min(length, buffer.size() - header));
}

auto TalkerStreams::commit_frame(
    TalkerStreamSlot& slot,
    net::EthernetTxSlot const& frame,
    size_t const header,
    size_t const length,
    uint64_t const launch_time_ns) -> bool
{
    port_->stamp_launch_time(frame.handle, launch_time_ns);            // after the render: a refused render holds no place
    auto const head = pdu_head(frame.buffer.subspan(header, length));  // read now: the slot is the port's after the commit
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    if (!commit_stream_frame(frame, header + length)) {
        return false;
    }
    count_sent(slot, head ? std::span<uint8_t const>{*head} : std::span<uint8_t const>{});
    return true;
}

auto TalkerStreams::send_to_port(ieee::Eui48 const& dest_mac, std::span<uint8_t const> const payload, uint64_t const launch_time_ns)
    -> bool
{
    StreamPort::Guard const guard{*port_};
    auto slot = port_->port().tx_start(port_->launch_time(launch_time_ns));
    if (!slot) {
        return false;
    }
    size_t const length = build_tagged_frame(slot->buffer, dest_mac, payload);
    if (length == 0) {
        (void)port_->port().tx_cancel(slot->handle);
        return false;
    }
    return commit_stream_frame(*slot, length);
}

auto TalkerStreams::build_tagged_frame(
    std::span<uint8_t> const buffer, ieee::Eui48 const& dest_mac, std::span<uint8_t const> const payload) const noexcept -> size_t
{
    auto const tag = this->tag();
    return build_stream_frame(buffer, dest_mac, port_->port().hardware_address(), tag.vlan_id, tag.pcp, payload);
}

auto TalkerStreams::commit_stream_frame(net::EthernetTxSlot const& slot, size_t const length) -> bool
{
    // Recorded before the commit: a ring slot belongs to the kernel after it.
    if (tx_pcap_recorder_.recording()) {
        tx_pcap_recorder_.record(slot.buffer.first(length), last_tx_gptp_ns_);
    }
    return port_->port().tx_commit(slot.handle, length).has_value();
}

auto TalkerStreams::flush_tx() -> Status
{
    if (port_ == nullptr) {
        return success();
    }
    StreamPort::Guard const guard{*port_};
    auto kicked = external_kick_ ? success() : port_->port().tx_flush();
    auto ended = port_->end_cycle();
    return kicked ? ended : kicked;
}

auto TalkerStreams::set_external_kick(bool const enable) noexcept -> bool
{
    bool const kickable = port_ != nullptr;
    external_kick_ = enable && kickable;
    return !enable || kickable;
}

auto TalkerStreams::kick_tx() -> Status
{
    return port_ != nullptr ? port_->port().tx_flush() : success();
}

void TalkerStreams::transmit_if_due(
    TalkerStreamSlot& slot,
    ptpclient::MediaClockGenerator::Emit const& tick,
    bool const srp_gate_open,
    size_t const samples,
    std::span<float const> src)
{
    std::unique_lock const entry{guards_[slot.spec.index], std::try_to_lock};
    if (!entry.owns_lock()) {
        return;  // being re-shaped (reformat_stream): this tick sends nothing
    }
    // A STOP_STREAMING'd stream behaves exactly like a closed SRP gate:
    // no emission, AAF FIFO cleared, CRF phase reset for a clean restart.
    bool const gate_open = srp_gate_open && !stream_stopped(slot.spec.index);
    switch (slot.spec.format.kind) {
        case StreamKind::am824: {
            if (gate_open && samples > 0) {
                transmit_am824(slot, presentation_for(tick.first_index), tick.samples, src);
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
                transmit_aaf(slot, presentation_for(first_index), static_cast<uint16_t>(samples_per_packet_), block);
            });
            break;
        }
        case StreamKind::crf: {
            if (!slot.crf) {
                break;
            }
            if (!gate_open) {
                slot.crf_next = 0;  // gate closed: next emission starts a fresh PDU phase
                break;
            }
            // One PDU per timestamps_per_packet timestamps, each sample_stride audio samples apart, sent
            // by the first wake at or past its first sample: the media clock's PDU rate at any audio rate
            // (the Milan CRF: 500 PDU/s under 48 kHz audio, 459.375 PDU/s under 44.1 kHz).
            uint32_t const sample_stride =
                crf_sample_stride(slot.crf->timestamp_interval, slot.crf->base_frequency, config_.sample_rate);
            uint64_t const span = uint64_t{slot.crf->timestamps_per_packet} * sample_stride;
            // Due, or the sample index went back (a media clock reset): rebased to the live position.
            if (tick.first_index >= slot.crf_next || tick.first_index + span < slot.crf_next) {
                uint64_t const base = crf_aligned_base(tick.first_index, sample_stride);
                transmit_crf(slot, base);
                slot.crf_next = base + span;
            }
            break;
        }
        case StreamKind::other:
        default:
            break;
    }
}

}  // namespace statusbar::avb_entity
