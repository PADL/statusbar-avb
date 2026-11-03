// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ListenerStreams methods — the local AVB stream RX path. Slots are shaped by
// StreamSpecs (kit phase 1); the per-kind deserialize bodies are unchanged
// from the fixed-two-context era, but a frame now finds its slot by the
// connected stream_id rather than by a hardcoded subtype->index mapping.

#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"

#include "statusbar/avb_entity/avb_entity_histogram_csv.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_handler.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <print>
#include <span>
#include <string>
#include <system_error>

namespace statusbar::avb_entity {

namespace {

/// StreamKind name as a static-storage log token (the deferred-formatting
/// logger rejects transient pointers; stream_kind_name() returns char const*).
auto kind_lit(ListenerStreamSlot const* slot)
{
    if (slot == nullptr) {
        return logging::lit("?");
    }
    switch (slot->kind.load(std::memory_order_relaxed)) {
        case StreamKind::am824:
            return logging::lit("AM824");
        case StreamKind::aaf:
            return logging::lit("AAF");
        case StreamKind::crf:
            return logging::lit("CRF");
        case StreamKind::other:
        default:
            return logging::lit("other");
    }
}

/// The blocks (samples) of an AM824 payload of @p payload_size octets in @p channels channels,
/// 0 when the deserializer would refuse them (avtp::am824_deserialize_mbla).
constexpr auto am824_rx_samples(uint8_t const channels, size_t const payload_size) noexcept -> uint8_t
{
    constexpr size_t MAX_BLOCKS = avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET;
    size_t const blocks = channels == 0 ? 0 : payload_size / (avtp::Am824Pdu::BYTES_PER_SAMPLE * channels);
    return blocks > MAX_BLOCKS ? 0 : static_cast<uint8_t>(blocks);
}

}  // namespace

auto aaf_timed_packet(
    uint16_t const stream_index,
    avtp::AafStreamInputContext const& ctx,
    avtp::AafPdu const& pdu,
    std::span<uint8_t const> const payload,
    uint16_t const frames,
    uint64_t const pts_ns) noexcept -> StreamRxPacket
{
    size_t const bps = avtp::aaf_bytes_per_sample(ctx.format);
    return StreamRxPacket{
        .stream_index = stream_index,
        .fmt = StreamAudioFormat::aaf_int32,
        .aaf_format = ctx.format,
        .bytes_per_sample = static_cast<uint8_t>(bps),
        .payload = payload.first(std::min(payload.size(), size_t{frames} * bps * ctx.channel_count)),
        .frames = frames,
        .channels = ctx.channel_count,
        .pts_ns = pts_ns,
        .pts_valid = ctx.has_valid_timestamp,
        .tu = pdu.tu(),
        .mr = pdu.mr(),
        .sequence_num = pdu.get_sequence_num()};
}

auto aaf_header_matches_format(StreamFormatFields const& format, avtp::AafPdu const& pdu) noexcept -> bool
{
    if (format.kind != StreamKind::aaf || pdu.get_format() != format.aaf_format ||
        avtp::aaf_nsr_to_hz(static_cast<uint8_t>(pdu.nsr())) != format.sample_rate_hz ||
        pdu.channels_per_frame() != format.channels) {
        return false;
    }
    size_t const bps = avtp::aaf_bytes_per_sample(format.aaf_format);
    if (bps != 0) {
        uint8_t const depth = pdu.get_bit_depth();
        if (depth == 0 || depth > bps * 8) {
            return false;
        }
    }
    if (format.aaf_samples_per_frame != 0 &&
        pdu.get_stream_data_length() != avtp::aaf_payload_size(format.aaf_format, format.channels, format.aaf_samples_per_frame)) {
        return false;
    }
    return true;
}

auto aaf_rx_packet(
    uint16_t const stream_index,
    avtp::AafStreamInputContext& ctx,
    avtp::AafPdu const& pdu,
    std::span<uint8_t const> const payload,
    uint64_t const gptp_now_ns) noexcept -> StreamRxPacket
{
    // The deserializer's acceptance rule (avtp::aaf_stream_deserialize's defaults).
    auto const frames = static_cast<uint16_t>(avtp::aaf_samples_in_packet<64, 256>(ctx, payload.size()));
    if (frames == 0) {
        return StreamRxPacket{.stream_index = stream_index, .aaf_format = ctx.format, .channels = ctx.channel_count};
    }
    ctx.process_packet_header(pdu, gptp_now_ns);
    uint64_t const pts = ctx.compute_pts_ns();
    ctx.advance_samples(frames);
    return aaf_timed_packet(stream_index, ctx, pdu, payload, frames, pts);
}

auto am824_timed_packet(
    uint16_t const stream_index,
    avtp::Am824StreamInputContext const& ctx,
    avtp::Am824Pdu const& pdu,
    std::span<uint8_t const> const payload,
    uint32_t const frames,
    uint64_t const pts_ns) noexcept -> StreamRxPacket
{
    uint16_t const channels = ctx.config.channel_count;
    size_t const bytes = size_t{frames} * channels * avtp::Am824Pdu::BYTES_PER_SAMPLE;
    return StreamRxPacket{
        .stream_index = stream_index,
        .fmt = StreamAudioFormat::am824_mbla,
        .bytes_per_sample = static_cast<uint8_t>(avtp::Am824Pdu::BYTES_PER_SAMPLE),
        .payload = payload.first(std::min(payload.size(), bytes)),
        .frames = frames,
        .channels = channels,
        .pts_ns = pts_ns,
        .pts_valid = ctx.has_valid_anchor,
        .tu = pdu.stream_header.tu(),
        .mr = pdu.stream_header.mr(),
        .sequence_num = pdu.sequence_num()};
}

auto am824_rx_packet(
    uint16_t const stream_index,
    avtp::Am824StreamInputContext& ctx,
    avtp::Am824Pdu const& pdu,
    std::span<uint8_t const> const payload,
    uint64_t const gptp_now_ns) noexcept -> StreamRxPacket
{
    uint8_t const channels = ctx.config.channel_count;
    uint8_t const samples = am824_rx_samples(channels, payload.size());
    if (!avtp::is_valid_am824_deserialize_params<avtp::Am824Pdu::MAX_CHANNELS, avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET>(
            samples, channels)) {
        return StreamRxPacket{.stream_index = stream_index, .fmt = StreamAudioFormat::am824_mbla, .channels = channels};
    }
    ctx.process_packet_header(pdu, samples, gptp_now_ns);
    return am824_timed_packet(stream_index, ctx, pdu, payload, samples, ctx.compute_base_pts_ns());
}

auto am824_header_matches_format(StreamFormatFields const& format, avtp::Am824Pdu const& pdu) noexcept -> bool
{
    return format.kind == StreamKind::am824 && pdu.channel_count() == format.channels &&
        avtp::am824_sample_rate_hz(pdu.sample_rate()) == format.sample_rate_hz;
}

auto crf_header_matches_format(StreamFormatFields const& format, avtp::CrfPdu const& pdu) noexcept -> bool
{
    return format.kind == StreamKind::crf && pdu.get_type() == format.crf_type &&
        pdu.timestamp_interval() == format.crf_timestamp_interval && pdu.timestamp_count() == format.crf_timestamps_per_pdu &&
        pdu.pull() == format.crf_pull && pdu.base_frequency() == format.crf_base_frequency_hz;
}

auto ListenerStreams::shape(ListenerStreamSlot& slot, StreamSpec const& spec) -> Status
{
    switch (spec.format.kind) {
        case StreamKind::am824: {
            auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.am824.emplace(*rate, static_cast<uint8_t>(spec.format.channels));
            return success();
        }
        case StreamKind::aaf: {
            auto const rate = avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.aaf.emplace(spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth);
            return success();
        }
        case StreamKind::crf: {
            // CRF input (kit phase 3): a media-clock reference stream. The
            // context tracks sequence/mr and delivers each 64-bit timestamp
            // to the slot's on_crf consumer (see set_crf).
            if (spec.format.crf_base_frequency_hz == 0) {
                return failure(std::errc::invalid_argument);
            }
            slot.crf.emplace(
                static_cast<avtp::CrfType>(spec.format.crf_type),
                spec.format.crf_base_frequency_hz,
                static_cast<avtp::CrfPull>(spec.format.crf_pull),
                spec.format.crf_timestamp_interval);
            return success();
        }
        case StreamKind::other:
        default:
            return failure(std::errc::not_supported);
    }
}

auto ListenerStreams::open_stream(StreamSpec const& spec) -> Status
{
    if (slots_.size() == MAX_ENTITY_STREAMS) {
        return failure(std::errc::result_out_of_range);
    }
    if (spec.format.kind == StreamKind::other) {
        return failure(std::errc::not_supported);
    }
    auto& slot = slots_.emplace_back();  // in place: the slot holds atomics
    if (auto status = shape(slot, spec); !status) {
        slots_.pop_back();
        return status;
    }
    begin_slot(slot, spec);
    return success();
}

void ListenerStreams::begin_slot(ListenerStreamSlot& slot, StreamSpec const& spec)
{
    slot.spec = spec;
    slot.counters.margin_histogram.configure(margin_histogram_config_);
    slot.kind.store(spec.format.kind, std::memory_order_relaxed);
    bind_pending_consume(slot);
}

void ListenerStreams::set_presentation_offset_ns(uint64_t const presentation_offset_ns)
{
    margin_histogram_config_ = StreamInputCounters::margin_histogram_config_for(presentation_offset_ns);
    for (auto& slot : slots_) {
        slot.counters.margin_histogram.configure(margin_histogram_config_);
    }
}

void ListenerStreams::print_timing() const
{
    std::string report;
    for (auto const& slot : slots_) {
        format_stream_input_timing_to(report, slot.spec.index, slot.counters);
    }
    std::print("{}", report);
}

auto ListenerStreams::write_margin_histogram_csvs(std::string const& path) const -> size_t
{
    std::filesystem::path const p{path};
    size_t written = 0;
    for (auto const& slot : slots_) {
        if (slot.counters.margin.count() == 0) {
            continue;
        }
        auto const file = p.parent_path() / std::format("{}-in{}{}", p.stem().string(), slot.spec.index, p.extension().string());
        written += write_histogram_csv(file.string(), slot.counters.margin_histogram.snapshot()) ? 1U : 0U;
    }
    return written;
}

auto ListenerStreams::reformat_stream(StreamSpec const& spec) -> Status
{
    auto* const slot = slot_for(spec.index);
    if (slot == nullptr) {
        return failure(std::errc::no_such_device);
    }
    if (!kind_change_allowed(slot->kind.load(std::memory_order_relaxed), spec.format.kind)) {
        return failure(std::errc::invalid_argument);
    }
    if (spec.format.kind == StreamKind::crf) {
        // One CRF format a stream: nothing to re-shape.
        return spec.format_word == slot->spec.format_word ? Status{} : Status{failure(std::errc::not_supported)};
    }
    return reshape(*slot, spec);
}

auto ListenerStreams::reshape(ListenerStreamSlot& slot, StreamSpec const& spec) -> Status
{
    // A trial first, so that a refused format leaves the slot as it was.
    ListenerStreamSlot trial{};
    if (auto status = shape(trial, spec); !status) {
        return status;
    }
    std::scoped_lock const lock{slot.guard};
    adopt_shape(slot, trial, spec);
    return success();
}

void ListenerStreams::adopt_shape(ListenerStreamSlot& slot, ListenerStreamSlot const& trial, StreamSpec const& spec) noexcept
{
    slot.am824 = trial.am824;
    slot.aaf = trial.aaf;
    slot.kind.store(spec.format.kind, std::memory_order_relaxed);
    // The index stays (slot_for reads it unlocked); the format is the new one.
    slot.spec.format_word = spec.format_word;
    slot.spec.format = spec.format;
}

void ListenerStreams::set_consume(uint16_t const stream_index, StreamConsumeFn fn)
{
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
        std::scoped_lock const lock{slot->guard};  // the RX thread skips the frame meanwhile
        slot->consume = std::move(fn);
        return;
    }
    defer_consume(stream_index, std::move(fn));
}

void ListenerStreams::defer_consume(uint16_t const stream_index, StreamConsumeFn fn)
{
    // No slot (yet): keep it pending. open_stream() binds it when the model
    // declares the index; otherwise it stays here, inert (menu/selection).
    for (auto& pending : pending_consume_) {
        if (pending.stream_index == stream_index) {
            pending.fn = std::move(fn);
            return;
        }
    }
    if (pending_consume_.size() < MAX_ENTITY_STREAMS) {
        pending_consume_.push_back(PendingConsume{.stream_index = stream_index, .fn = std::move(fn)});
    }
}

void ListenerStreams::set_crf(uint16_t const stream_index, StreamCrfFn fn)
{
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
        std::scoped_lock const lock{slot->guard};
        slot->on_crf = std::move(fn);
        return;
    }
    defer_crf(stream_index, std::move(fn));
}

void ListenerStreams::defer_crf(uint16_t const stream_index, StreamCrfFn fn)
{
    for (auto& pending : pending_crf_) {
        if (pending.stream_index == stream_index) {
            pending.fn = std::move(fn);
            return;
        }
    }
    if (pending_crf_.size() < MAX_ENTITY_STREAMS) {
        pending_crf_.push_back(PendingCrf{.stream_index = stream_index, .fn = std::move(fn)});
    }
}

void ListenerStreams::bind_pending_consume(ListenerStreamSlot& slot)
{
    for (auto& pending : pending_consume_) {
        if (pending.stream_index == slot.spec.index && pending.fn) {
            slot.consume = std::move(pending.fn);
            pending.fn = {};  // consumed; entry stays (never erased, slots are stable)
            break;
        }
    }
    for (auto& pending : pending_crf_) {
        if (pending.stream_index == slot.spec.index && pending.fn) {
            slot.on_crf = std::move(pending.fn);
            pending.fn = {};
            break;
        }
    }
}

auto ListenerStreams::slot_for(uint16_t const stream_index) noexcept -> ListenerStreamSlot*
{
    for (auto& slot : slots_) {
        if (slot.spec.index == stream_index) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::slot_for(uint16_t const stream_index) const noexcept -> ListenerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.spec.index == stream_index) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::slot_of(StreamKind const kind) noexcept -> ListenerStreamSlot*
{
    for (auto& slot : slots_) {
        if (slot.kind.load(std::memory_order_relaxed) == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::slot_of(StreamKind const kind) const noexcept -> ListenerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.kind.load(std::memory_order_relaxed) == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::attach_rx(
    StreamPort& port, std::span<ieee::Eui48 const> const static_groups, net::MessageReactor& reactor, bool const keep_handler)
    -> StatusValue<std::unique_ptr<net::Pollable>>
{
    if (!port.valid()) {
        return failure(std::errc::io_error);
    }
    port_ = &port;
    // The reactor owns a tick on neither clock: its adapter reads the RX clock
    // once per pass and stamps with the last media wake's gPTP time.
    auto rx = std::make_unique<StreamRxHandler>(
        port, static_groups, [this] { (void)drain_rx(realtime::GptpClock0::from_ns(current_gptp_ns()), net::rx_clock_now()); });
    if (keep_handler) {
        return success(std::unique_ptr<net::Pollable>{std::move(rx)});
    }
    reactor.add(std::move(rx));
    return success(std::unique_ptr<net::Pollable>{});
}

void ListenerStreams::close_rx() noexcept
{
    for (auto& slot : slots_) {
        if (slot.consume) {
            defer_consume(slot.spec.index, std::move(slot.consume));
        }
        if (slot.on_crf) {
            defer_crf(slot.spec.index, std::move(slot.on_crf));
        }
    }
    slots_.clear();
    port_ = nullptr;
}

auto ListenerStreams::take_rx_frame(net::RxTimePoint const rx_now) -> std::optional<size_t>
{
    StreamPort::Guard const guard{*port_};
    auto rx = port_->port().rx_start(rx_now);
    if (!rx || !rx->has_value()) {
        return std::nullopt;
    }
    size_t const length = copy_avtpdu((*rx)->buffer);
    (void)port_->port().rx_release((*rx)->handle);
    return length;
}

auto ListenerStreams::copy_avtpdu(std::span<uint8_t const> const frame) -> size_t
{
    // Where the AVTPDU starts depends on whether the frame arrived tagged;
    // a frame that is not AVTP at all is dropped here.
    auto const offset = net::avtp_payload_offset(frame);
    if (!offset || *offset >= frame.size()) {
        return 0;
    }
    size_t const length = std::min(frame.size() - *offset, rx_buf_.size());
    span_copy(std::span<uint8_t>{rx_buf_}.first(length), frame.subspan(*offset, length));
    return length;
}

auto ListenerStreams::drain_rx(realtime::GptpClock0::time_point const gptp_now, net::RxTimePoint const rx_now) -> size_t
{
    if (port_ == nullptr) {
        return 0;
    }
    // Drain every queued frame in one pass (the arrival rate is 8000/s = 125 us apart;
    // a 50 us RX-timer tick or a reactor wake sees 0-3 frames). All frames in this pass
    // share gptp_now -- the caller's gPTP "now" -- and rx_now, the RX clock as the
    // caller holds it, told to the port for each frame: no clock read in the pass.
    //
    // One pass is one port cycle. The XDP backend hands out at most its
    // per-cycle budget of frames between begin_cycle() and end_cycle(), and
    // without the begin the budget is never reset: the first 32 frames
    // arrive and nothing after them.
    begin_port_cycle();
    size_t const drained = drain_frames(realtime::GptpClock0::to_ns(gptp_now), rx_now);
    end_port_cycle();
    return drained;
}

auto ListenerStreams::drain_frames(int64_t const gptp_now_ns, net::RxTimePoint const rx_now) -> size_t
{
    size_t drained = 0;
    while (drained < MAX_DRAIN_PER_CALL) {
        auto const length = take_rx_frame(rx_now);
        if (!length) {
            break;  // the port is empty: the one thing that ends a pass
        }
        // A frame that is not AVTP was taken and released like any other;
        // it is simply not a stream frame. It must not end the pass, or every
        // stream frame queued behind it waits for the next drain.
        if (*length != 0) {
            on_stream_rx_frame({rx_buf_.data(), *length}, gptp_now_ns);
        }
        ++drained;
    }
    return drained;
}

void ListenerStreams::begin_port_cycle()
{
    StreamPort::Guard const guard{*port_};
    port_->port().begin_cycle();
}

void ListenerStreams::end_port_cycle()
{
    // The ring backends refill their receive ring here; rawnet has nothing to do.
    StreamPort::Guard const guard{*port_};
    (void)port_->end_cycle();  // and a counter reset asked for (StreamPort::reset_counters)
}

auto ListenerStreams::find_slot_for_frame(std::span<uint8_t const> frame) -> ListenerStreamSlot*
{
    // Find the slot this frame belongs to: kind must match the AVTP subtype and
    // the frame's stream_id must match the stream connected to that input (the
    // promiscuous socket also sees our own TX and any other stream on the
    // segment). Stream_ids are unique, so the first match is the only match.
    auto const subtype_kind = frame[0] == avtp::AvtpSubtype::iec_61883_iidc ? StreamKind::am824
        : frame[0] == avtp::AvtpSubtype::aaf                                ? StreamKind::aaf
        : frame[0] == avtp::AvtpSubtype::crf                                ? StreamKind::crf
                                                                            : StreamKind::other;
    if (subtype_kind == StreamKind::other) {
        return nullptr;
    }
    for (auto& s : slots_) {
        if (s.kind.load(std::memory_order_relaxed) == subtype_kind && frame_is_for_listener(s.spec.index, frame)) {
            return &s;
        }
    }
    return nullptr;
}

void ListenerStreams::on_stream_rx_frame(std::span<uint8_t const> frame, int64_t gptp_now_ns)
{
    if (frame.empty()) {
        return;
    }
    ListenerStreamSlot* const slot = find_slot_for_frame(frame);
    if (slot == nullptr || slot->stopped.load(std::memory_order_relaxed)) {
        return;
    }
    std::unique_lock const entry{slot->guard, std::try_to_lock};
    if (!entry.owns_lock() || slot->kind.load(std::memory_order_relaxed) != subtype_kind) {
        return;  // being re-shaped (reformat_stream), or re-shaped to another kind since matched
    }

    if (slot->am824) {
        // --- AM824 ---
        if (frame.size() < avtp::Am824Pdu::HEADER_LENGTH) {
            return;
        }
        avtp::Am824Pdu pdu{};
        span_load(pdu, frame.first(avtp::Am824Pdu::HEADER_LENGTH));
        if (!pdu.is_valid()) {
            slot->rx_bad.add(1);
            update_stream_input_counters(*slot, 0, 0, false, false, false, /*format_ok=*/false, 0, /*ts_sparse=*/true, gptp_now_ns);
            return;
        }
        uint8_t const seq = pdu.sequence_num();
        bool const tv = pdu.stream_header.tv();
        bool const tu = pdu.stream_header.tu();
        bool const mr = pdu.stream_header.mr();
        if (!am824_header_matches_format(slot->spec.format, pdu)) {
            // Discarded and counted in UNSUPPORTED_FORMAT, as for AAF.
            update_stream_input_counters(
                *slot, seq, pdu.avtp_timestamp(), tv, tu, mr, /*format_ok=*/false, 0, /*ts_sparse=*/true, gptp_now_ns);
            return;
        }
        // The declared audio length, not the Ethernet padding past it.
        std::span<uint8_t const> const audio = avtp::am824_get_audio_payload(pdu, frame);
        uint64_t samples_this = 0;
        if (timed_sink_ != nullptr && !slot->consume) {
            // Raw-only: the timed sink receives the quadlets as they are; no float decode.
            auto const packet = am824_rx_packet(slot->spec.index, *slot->am824, pdu, audio, static_cast<uint64_t>(gptp_now_ns));
            samples_this = packet.frames;
            if (packet.frames != 0) {
                timed_sink_->on_listener_packet(packet);
            }
        } else {
            uint64_t pts_first = 0;
            avtp::am824_deserialize_mbla(
                *slot->am824,
                pdu,
                audio,
                static_cast<uint64_t>(gptp_now_ns),
                [&samples_this, &pts_first, slot](uint8_t ch, std::span<float> s, uint64_t pts, uint64_t period) {
                    samples_this = s.size();
                    pts_first = pts;
                    if (slot->consume) {
                        slot->consume(ch, s, pts, period);
                    }
                });
            if (timed_sink_ != nullptr && samples_this != 0) {
                timed_sink_->on_listener_packet(
                    am824_timed_packet(slot->spec.index, *slot->am824, pdu, audio, static_cast<uint32_t>(samples_this), pts_first));
            }
        }
        slot->rx_packets.add(1);
        slot->rx_samples.add(samples_this);
        update_stream_input_counters(
            *slot,
            seq,
            pdu.avtp_timestamp(),
            tv,
            tu,
            mr,
            /*format_ok=*/true,
            samples_this,
            /*ts_sparse=*/true,  // 61883-6 SYT cadence: tv=0 between SYT packets is normal
            gptp_now_ns);

        // Hand the accepted audio to the RX sink (today the WAN tunnel; the sink
        // decides whether to consume it). The listener does not know what the sink does with it.
        if (audio_sink_ != nullptr) {
            audio_sink_->on_listener_audio(slot->spec.index, StreamAudioFormat::am824_mbla, audio);
        }
    } else if (slot->aaf) {
        // --- AAF ---
        auto pdu_opt = avtp::aaf_parse_header(frame);
        if (!pdu_opt) {
            slot->rx_bad.add(1);
            update_stream_input_counters(
                *slot, 0, 0, false, false, false, /*format_ok=*/false, 0, /*ts_sparse=*/false, gptp_now_ns);
            return;
        }
        if (!aaf_header_matches_format(slot->spec.format, *pdu_opt)) {
            // Milan v1.2 Section 4.4.2.2: discard, counted in UNSUPPORTED_FORMAT.
            update_stream_input_counters(
                *slot,
                pdu_opt->get_sequence_num(),
                pdu_opt->get_avtp_timestamp(),
                pdu_opt->tv(),
                pdu_opt->tu(),
                pdu_opt->mr(),
                /*format_ok=*/false,
                0,
                /*ts_sparse=*/false,
                gptp_now_ns);
            return;
        }
        std::span<uint8_t const> const audio = avtp::aaf_get_audio_payload(frame);
        uint64_t samples_this = 0;
        if (timed_sink_ != nullptr && !slot->consume) {
            // Raw-only: the timed sink receives the payload as is, so skip the
            // float decode and read the timing from the header alone.
            auto const packet = aaf_rx_packet(slot->spec.index, *slot->aaf, *pdu_opt, audio, static_cast<uint64_t>(gptp_now_ns));
            samples_this = packet.frames;
            if (packet.frames != 0) {
                timed_sink_->on_listener_packet(packet);
            }
        } else {
            uint64_t pts_first = 0;
            avtp::aaf_stream_deserialize(
                *slot->aaf,
                *pdu_opt,
                audio,
                static_cast<uint64_t>(gptp_now_ns),
                [&samples_this, &pts_first, slot](uint8_t ch, std::span<float> s, uint64_t pts, uint64_t period) {
                    samples_this = s.size();
                    pts_first = pts;
                    if (slot->consume) {
                        slot->consume(ch, s, pts, period);
                    }
                });
            if (timed_sink_ != nullptr && samples_this != 0) {
                timed_sink_->on_listener_packet(aaf_timed_packet(
                    slot->spec.index, *slot->aaf, *pdu_opt, audio, static_cast<uint16_t>(samples_this), pts_first));
            }
        }
        slot->rx_packets.add(1);
        slot->rx_samples.add(samples_this);
        update_stream_input_counters(
            *slot,
            pdu_opt->get_sequence_num(),
            pdu_opt->get_avtp_timestamp(),
            pdu_opt->tv(),
            pdu_opt->tu(),
            pdu_opt->mr(),
            /*format_ok=*/true,
            samples_this,
            /*ts_sparse=*/false,  // AAF timestamps every packet: tv=0 is a real fault
            gptp_now_ns);

        // Hand the accepted audio to the RX sink (today the WAN tunnel). Symmetric
        // to the AM824 path above; the sink ignores it unless AAF is its source.
        if (audio_sink_ != nullptr) {
            audio_sink_->on_listener_audio(slot->spec.index, StreamAudioFormat::aaf_int32, audio);
        }
    } else if (slot->crf) {
        // --- CRF: media clock reference, no audio payload ---
        if (frame.size() < avtp::CrfPdu::HEADER_LENGTH) {
            return;
        }
        avtp::CrfPdu pdu{};
        span_load(pdu, frame.first(avtp::CrfPdu::HEADER_LENGTH));
        if (!pdu.is_valid()) {
            slot->rx_bad.add(1);
            return;
        }
        std::span<uint8_t const> const ts_data = frame.subspan(avtp::CrfPdu::HEADER_LENGTH);
        if (!crf_header_matches_format(slot->spec.format, pdu)) {
            update_stream_input_counters(
                *slot, pdu.get_sequence_num(), 0, false, pdu.tu(), pdu.mr(), /*format_ok=*/false, 0, /*ts_sparse=*/true, 0);
            return;
        }
        // Media lock on the first timestamps' step, timestamp_interval x timestamps_per_pdu
        // events apart (IEEE 1722-2016 Clause 10.4); no AVTP timestamp to call late or early.
        auto const first_ts = avtp::crf_get_timestamp(ts_data, 0);
        uint64_t const events_per_pdu = uint64_t{pdu.timestamp_interval()} * pdu.timestamp_count();
        update_stream_input_counters(
            *slot,
            pdu.get_sequence_num(),
            static_cast<uint32_t>(first_ts.value_or(0)),
            first_ts.has_value(),
            pdu.tu(),
            pdu.mr(),
            /*format_ok=*/true,
            events_per_pdu,
            /*ts_sparse=*/true,
            /*gptp_now_ns=*/0);
        auto const stream_id = pdu.stream_id().to_uint64();
        auto const connection = slot->connections.load(std::memory_order_acquire);
        slot->crf->process_packet(
            pdu, ts_data, [slot, gptp_now_ns, mr = pdu.mr(), stream_id, connection](uint64_t ts_ns, uint16_t index) {
                if (slot->crf_source == 0 || stream_id != slot->crf_stream_id || connection != slot->crf_connection) {
                    slot->crf_stream_id = stream_id;
                    slot->crf_connection = connection;
                    ++slot->crf_source;
                }
                if (slot->on_crf) {
                    slot->on_crf(ts_ns, index, gptp_now_ns, mr, slot->crf_source);
                }
            });
        slot->rx_packets.add(1);
        slot->rx_samples.add(slot->crf->last_timestamp_count);
    }
}

auto ListenerStreams::frame_is_for_listener(uint16_t const stream_index, std::span<uint8_t const> frame) const -> bool
{
    // AVTP stream_id is 8 bytes at offset 4 (subtype@0, sv/ver/flags@1, seq@2,
    // ...). The VLAN tag is already stripped by RawnetContext::recv.
    if (frame.size() < 12) {
        return false;
    }
    auto const* slot = slot_for(stream_index);
    return slot != nullptr && slot_receives(*slot, frame);
}

auto ListenerStreams::slot_receives(ListenerStreamSlot const& slot, std::span<uint8_t const> frame) -> bool
{
    // The connection as the reactor published it (on_listener_connected), not
    // the ACMP listener's record, which that thread writes with no hand-off.
    uint64_t const connected = slot.rx_stream_id.load();
    ieee::Eui64 carried{};
    span_load(carried, frame.subspan(4, ieee::Eui64::LENGTH));
    return connected != 0 && carried.to_uint64() == connected;
}

void ListenerStreams::update_stream_input_counters(
    ListenerStreamSlot& slot,
    uint8_t const seq,
    uint32_t const avtp_ts,
    bool const tv,
    bool const tu,
    bool const mr,
    bool const format_ok,
    uint64_t const samples_per_ch,
    bool const ts_sparse,
    int64_t const gptp_now_ns)
{
    // The bookkeeping itself lives in avb_entity_stream_counters.* (unit-tested); feed
    // it the entity-state inputs it can't see (gPTP-now, lock tolerance, Fs). gptp_now_ns
    // is the caller's fresh receive time -- the reactor path passes current_gptp_ns()
    // (the media-timer wake), the RT-timer path passes its own wake time.
    tally_stream_input_packet(
        slot.counters,
        seq,
        avtp_ts,
        tv,
        tu,
        mr,
        format_ok,
        samples_per_ch,
        ts_sparse,
        gptp_now_ns,
        lock_tolerance_ns_,
        slot.spec.format.kind == StreamKind::crf ? slot.spec.format.crf_base_frequency_hz : slot.spec.format.sample_rate_hz);
}

auto ListenerStreams::fill_stream_input_counters(
    uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool
{
    auto const* slot = slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    auto const& c = slot->counters;
    auto const relaxed = std::memory_order_relaxed;
    // IEEE 1722.1 STREAM_INPUT counter bit positions (Clause 7.4.42). counters[bit]
    // holds the value for the bit set in `valid`.
    auto set = [&](size_t bit, uint32_t v) {
        valid |= (1U << bit);
        out[bit] = v;
    };
    set(0, c.media_locked.load(relaxed));
    set(1, c.media_unlocked.load(relaxed));
    set(2, c.stream_interrupted.load(relaxed));
    set(3, c.seq_num_mismatch.load(relaxed));
    set(4, c.media_reset.load(relaxed));
    set(5, c.timestamp_uncertain.load(relaxed));
    set(6, c.timestamp_valid.load(relaxed));
    set(7, c.timestamp_not_valid.load(relaxed));
    set(8, c.unsupported_format.load(relaxed));
    set(9, c.late_timestamp.load(relaxed));
    set(10, c.early_timestamp.load(relaxed));
    set(11, c.frames_rx.load(relaxed));
    return true;
}

auto ListenerStreams::fill_interval_stream_input_counters(
    uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool
{
    auto const* slot = slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    valid |= StreamInputIntervals::VALID;
    for (size_t i = 0; i < slot->intervals.values.size(); ++i) {
        if ((StreamInputIntervals::VALID & (1U << i)) != 0) {
            out[i] = slot->intervals.values[i];
        }
    }
    return true;
}

auto ListenerStreams::fold_input_counters(uint16_t const descriptor_index) -> bool
{
    auto* const slot = slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    // A stopped input discards its AVTPDUs on purpose (STOP_STREAMING): its
    // lock goes without an interruption.
    if (slot->stopped.load(std::memory_order_relaxed)) {
        unlock_stream_input(slot->counters);
    }
    return slot->intervals.fold(slot->counters);
}

void ListenerStreams::reset_input_counters(uint16_t const descriptor_index)
{
    if (auto* const slot = slot_for(descriptor_index); slot != nullptr) {
        slot->intervals.reset(slot->counters);
    }
}

void ListenerStreams::note_input_unbound(uint16_t const descriptor_index)
{
    if (auto* const slot = slot_for(descriptor_index); slot != nullptr) {
        unlock_stream_input(slot->counters);
    }
}

namespace {

auto to_stream_id(ieee::Eui64 const& id) -> tsn::StreamId
{
    tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(id.span(), &sid);
    return sid;
}

}  // namespace

auto ListenerStreams::group_to_join(ieee::Eui48 const dest_mac) noexcept -> std::optional<ieee::Eui48>
{
    // IEEE 1722.1-2021 8.2.1.13: the listener joins stream_dest_mac when it
    // is a multicast address. A unicast destination is delivered without a
    // group, and a zero one (a talker that has none yet) is not a group
    // either -- joining it put 00:00:00:00:00:00 in the NIC's table.
    if (dest_mac.is_multicast()) {
        return dest_mac;
    }
    return std::nullopt;
}

auto ListenerStreams::join_group(std::optional<ieee::Eui48> const group) -> std::optional<ieee::Eui48>
{
    if (!group.has_value() || port_ == nullptr) {
        return std::nullopt;
    }
    StreamPort::Guard const guard{*port_};
    if (!port_->port().join_multicast(*group).has_value()) {
        return std::nullopt;
    }
    return group;
}

void ListenerStreams::leave_group(std::optional<ieee::Eui48> const group)
{
    if (!group.has_value() || port_ == nullptr) {
        return;
    }
    StreamPort::Guard const guard{*port_};
    (void)port_->port().leave_multicast(*group);
}

void ListenerStreams::take_connection(
    ListenerConnection& held, ieee::Eui64 const& stream_id, std::optional<ieee::Eui48> const group, sm::TimePoint const now)
{
    // A stream this slot already declared and is leaving behind is withdrawn;
    // the same stream again is one declaration (listener_attach is idempotent).
    if (held.stream_id.is_set() && held.stream_id != stream_id) {
        (void)components_.msrp_handler.listener_withdraw(to_stream_id(held.stream_id), now);
    }
    held.stream_id = stream_id;
    // One membership per slot: the same group again is kept as it is, a
    // different one replaces it, join and leave balanced either way.
    if (held.group != group) {
        leave_group(held.group);
        held.group = join_group(group);
    }
}

void ListenerStreams::release_connection(ListenerConnection& held, sm::TimePoint const now)
{
    if (held.stream_id.is_set()) {
        (void)components_.msrp_handler.listener_withdraw(to_stream_id(held.stream_id), now);
    }
    // Release the talker's stream group so per-connection dest-MAC churn does
    // not pile up stale NIC multicast filter entries.
    leave_group(held.group);
    held = ListenerConnection{};
}

void ListenerStreams::log_connected(uint16_t const stream_index, ieee::Eui48 const dest_mac, bool const ready, bool const joined)
{
    if (!logger_) {
        return;
    }
    logger_->status(
        "acmp: listener stream {} ({}) CONNECTED to talker dest={:012x} -> MSRP Listener {}, mcast join {}",
        stream_index,
        kind_lit(slot_for(stream_index)),
        dest_mac.to_uint64(),
        ready ? logging::lit("Ready") : logging::lit("AskingFailed (talker not advertising yet)"),
        joined ? logging::lit("ok") : logging::lit("none"));
}

auto ListenerStreams::attach_ready(ieee::Eui64 const& stream_id, sm::TimePoint const now) -> bool
{
    // Attach (kit phase 5d): declares Ready when the talker's Advertise is
    // registered, AskingFailed until then (e.g. a fast-connect racing the
    // talker's boot) -- the handler upgrades to Ready when it appears.
    auto const result = components_.msrp_handler.listener_attach(to_stream_id(stream_id), now);
    return result.has_value() && *result == nanoavb::ListenerReservationState::Ready;
}

auto ListenerStreams::attach_or_held_ready(ListenerConnection const& held, ieee::Eui64 const& stream_id, sm::TimePoint const now)
    -> bool
{
    // One attachment per stream a slot holds: a slot already on this stream
    // re-declares nothing, so attach and withdraw stay one-for-one however
    // many times a controller repeats the CONNECT.
    return held.stream_id != stream_id ? attach_ready(stream_id, now) : held_ready(stream_id);
}

auto ListenerStreams::held_ready(ieee::Eui64 const& stream_id) const -> bool
{
    auto const sid = to_stream_id(stream_id);
    for (auto const& stream : components_.msrp_handler.listener_streams()) {
        if (stream.stream_id == sid) {
            return stream.state == nanoavb::ListenerReservationState::Ready;
        }
    }
    return false;
}

void ListenerStreams::on_listener_connected(
    uint16_t const stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 const dest_mac, sm::TimePoint const now)
{
    auto* slot = slot_for(stream_index);
    if (slot == nullptr) {
        return;  // not one of our inputs: nothing to receive on, nothing to join for
    }
    slot->connections.fetch_add(1, std::memory_order_release);  // a new CRF source (StreamCrfFn)
    bool const ready = attach_or_held_ready(slot->connection, stream_id, now);
    take_connection(slot->connection, stream_id, group_to_join(dest_mac), now);
    slot->rx_stream_id.publish(stream_id.to_uint64());  // the RX thread may match its frames from here on
    log_connected(stream_index, dest_mac, ready, slot->connection.group.has_value());
}

void ListenerStreams::on_listener_disconnected(uint16_t const stream_index, sm::TimePoint const now)
{
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
        slot->rx_stream_id.publish(0);  // no frame is this input's any more
        release_connection(slot->connection, now);
    }
    if (logger_) {
        logger_->status(
            "acmp: listener stream {} ({}) DISCONNECTED from talker -> MSRP Listener withdrawn",
            stream_index,
            kind_lit(slot_for(stream_index)));
    }
}

}  // namespace statusbar::avb_entity
