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
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace statusbar::avb_entity {

namespace {

/// StreamKind name as a static-storage log token (the deferred-formatting
/// logger rejects transient pointers; stream_kind_name() returns char const*).
auto kind_lit(ListenerStreamSlot const* slot)
{
    if (slot == nullptr) {
        return logging::lit("?");
    }
    switch (slot->kind) {
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

/// @p counts with no media lock in them.
auto without_lock(StreamInputCounts counts) noexcept -> StreamInputCounts
{
    counts.media_locked.reset();
    return counts;
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

void ListenerStreams::make_aaf_context(ListenerStreamSlot& slot, StreamFormatFields const& format)
{
    auto const rate = avtp::aaf_sample_rate_from_hz(format.sample_rate_hz);
    slot.aaf.emplace(format.aaf_format, rate.value_or(avtp::AafSampleRate::user_specified), format.channels, format.bit_depth);
    slot.aaf->samples_per_pdu = format.aaf_samples_per_frame;
}

auto ListenerStreams::can_take_stream_format(uint16_t const stream_index, uint64_t const format_word) const noexcept -> bool
{
    auto const* const slot = slot_for(stream_index);
    if (slot == nullptr || stream_index >= stream_format_.size()) {
        return false;
    }
    return format_word == slot->spec.format_word || aaf_input_can_take(slot->spec.format, format_word);
}

auto ListenerStreams::set_stream_format(uint16_t const stream_index, uint64_t const format_word) noexcept -> bool
{
    if (!can_take_stream_format(stream_index, format_word)) {
        return false;
    }
    // The opened word is kept as 0: a slot opened from fields alone has no
    // word of its own to be decoded again.
    bool const as_opened = format_word == slot_for(stream_index)->spec.format_word;
    uint64_t const before = stream_format(stream_index);
    stream_format_[stream_index].store(as_opened ? 0 : format_word, std::memory_order_release);
    if (format_word != before) {
        notify_input_changed(*slot_for(stream_index));
    }
    return true;
}

auto ListenerStreams::stream_format(uint16_t const stream_index) const noexcept -> uint64_t
{
    auto const* const slot = slot_for(stream_index);
    if (slot == nullptr || stream_index >= stream_format_.size()) {
        return 0;
    }
    uint64_t const set = stream_format_[stream_index].load(std::memory_order_acquire);
    return set != 0 ? set : slot->spec.format_word;
}

void ListenerStreams::follow_stream_format(ListenerStreamSlot& slot)
{
    uint64_t const wanted =
        slot.spec.index < stream_format_.size() ? stream_format_[slot.spec.index].load(std::memory_order_acquire) : 0;
    if (wanted == slot.rx_format_word) {
        return;
    }
    make_aaf_context(slot, wanted != 0 ? decode_stream_format(wanted) : slot.spec.format);
    slot.rx_format_word = wanted;
}

auto ListenerStreams::new_slot(StreamSpec const& spec) -> ListenerStreamSlot&
{
    auto& slot = slots_.emplace_back();  // in place: the slot holds atomics
    slot.spec = spec;
    slot.counters.margin_histogram.configure(margin_histogram_config_);
    return slot;
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

auto ListenerStreams::open_stream(StreamSpec const& spec) -> Status
{
    if (slots_.size() == MAX_ENTITY_STREAMS) {
        return failure(std::errc::result_out_of_range);
    }
    if (spec.format.kind == StreamKind::other) {
        return failure(std::errc::not_supported);
    }
    auto& slot = new_slot(spec);
    if (auto status = shape(slot, spec); !status) {
        slots_.pop_back();
        return status;
    }
    slot.kind = spec.format.kind;
    bind_pending_consume(slot);
    return success();
}

auto ListenerStreams::reformat_stream(StreamSpec const& spec) -> Status
{
    auto* const slot = slot_for(spec.index);
    if (slot == nullptr) {
        return failure(std::errc::no_such_device);
    }
    if (spec.format.kind != slot->kind) {
        return failure(std::errc::invalid_argument);
    }
    if (spec.format.kind == StreamKind::crf) {
        // One CRF format a stream: nothing to re-shape.
        return spec.format_word == slot->spec.format_word ? Status{} : Status{failure(std::errc::not_supported)};
    }
    // Checked first, so that a refused format leaves the slot as it was.
    bool const aaf = spec.format.kind == StreamKind::aaf;
    if ((aaf ? !avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz).has_value()
             : !avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz).has_value()) ||
        spec.format.channels == 0) {
        return failure(std::errc::invalid_argument);
    }
    SlotGuard::Lock const lock{slot->guard};
    (void)shape(*slot, spec);
    // The index stays (slot_for reads it unlocked); the format is the new one.
    slot->spec.format_word = spec.format_word;
    slot->spec.format = spec.format;
    return success();
}

void ListenerStreams::set_consume(uint16_t const stream_index, StreamConsumeFn fn)
{
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
        slot->consume = std::move(fn);
        return;
    }
    defer_consume(stream_index, std::move(fn));
}

void ListenerStreams::set_consume_timed(uint16_t const stream_index, StreamTimedConsumeFn fn)
{
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
        slot->consume_timed = std::move(fn);
        return;
    }
    defer_consume_timed(stream_index, std::move(fn));
}

auto ListenerStreams::pending_consume_for(uint16_t const stream_index) -> PendingConsume*
{
    for (auto& pending : pending_consume_) {
        if (pending.stream_index == stream_index) {
            return &pending;
        }
    }
    if (pending_consume_.size() >= MAX_ENTITY_STREAMS) {
        return nullptr;
    }
    pending_consume_.push_back(PendingConsume{.stream_index = stream_index});
    return &pending_consume_.back();
}

void ListenerStreams::defer_consume(uint16_t const stream_index, StreamConsumeFn fn)
{
    // No slot (yet): keep it pending. open_stream() binds it when the model
    // declares the index; otherwise it stays here, inert (menu/selection).
    if (auto* pending = pending_consume_for(stream_index); pending != nullptr) {
        pending->fn = std::move(fn);
    }
}

void ListenerStreams::defer_consume_timed(uint16_t const stream_index, StreamTimedConsumeFn fn)
{
    if (auto* pending = pending_consume_for(stream_index); pending != nullptr) {
        pending->timed = std::move(fn);
    }
}

void ListenerStreams::set_crf(uint16_t const stream_index, StreamCrfFn fn)
{
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
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
        }
        if (pending.stream_index == slot.spec.index && pending.timed) {
            slot.consume_timed = std::move(pending.timed);
            pending.timed = {};
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
        if (slot.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::slot_of(StreamKind const kind) const noexcept -> ListenerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.kind == kind) {
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
        slot.rx_stream_id.publish(0);  // released: the hook and input_state() agree on it
        notify_input_changed(slot);
        if (slot.consume) {
            defer_consume(slot.spec.index, std::move(slot.consume));
        }
        if (slot.consume_timed) {
            defer_consume_timed(slot.spec.index, std::move(slot.consume_timed));
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
    size_t const drained = drain_frames(gptp_now.time_since_epoch(), rx_now);
    end_port_cycle();
    return drained;
}

auto ListenerStreams::drain_frames(avtp::Nanoseconds const now, net::RxTimePoint const rx_now) -> size_t
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
            on_stream_rx_frame({rx_buf_.data(), *length}, now);
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
    (void)port_->port().end_cycle();
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
    // The slot in hand is tested as it is: looking it up again by its index
    // would walk the slots a second time for each one, and a frame for no
    // slot - our own transmit, another stream on the segment - pays the whole
    // walk on every frame.
    for (auto& s : slots_) {
        if (s.kind == subtype_kind && slot_receives(s, frame)) {
            return &s;
        }
    }
    return nullptr;
}

void ListenerStreams::on_stream_rx_frame(std::span<uint8_t const> frame, int64_t gptp_now_ns)
{
    on_stream_rx_frame(frame, avtp::Nanoseconds{gptp_now_ns});
}

void ListenerStreams::on_stream_rx_frame(std::span<uint8_t const> frame, avtp::Nanoseconds const now)
{
    ListenerStreamSlot* const slot = frame.empty() ? nullptr : find_slot_for_frame(frame);
    if (slot == nullptr) {
        return;
    }
    if (stream_stopped(slot->spec.index)) {
        tally_discarded_stream_input_packet(slot->counters);  // Milan v1.3 5.3.8.7
        return;
    }
    SlotGuard::Entry const entry{slot->guard};
    if (!entry.entered()) {
        return;  // being re-shaped (reformat_stream)
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
        // The declared audio, not the frame's tail (Ethernet padding).
        std::span<uint8_t const> const audio = avtp::am824_get_audio_payload(pdu, frame);
        uint64_t samples_this = 0;
        uint64_t pts_first = 0;
        avtp::am824_deserialize_mbla(
            *slot->am824,
            pdu,
            audio,
            static_cast<uint64_t>(gptp_now_ns),
            [&samples_this, &pts_first, slot](uint8_t ch, std::span<float> s, uint64_t pts, uint64_t period) {
                samples_this = s.size();
                pts_first = pts;
                // Kit phase 2: decoded floats + reconstructed PTS reach the
                // registered per-stream consumer (previously discarded).
                if (slot->consume) {
                    slot->consume(ch, s, pts, period);
                }
            });
        slot->rx_packets.add(1);
        slot->rx_samples.add(samples_this);
        update_stream_input_counters(
            *slot,
            pdu.stream_header.sequence_num.get(),
            pdu.avtp_timestamp(),
            pdu.stream_header.tv(),
            pdu.stream_header.tu(),
            pdu.stream_header.mr(),
            /*format_ok=*/true,
            samples_this,
            /*ts_sparse=*/true,  // 61883-6 SYT cadence: tv=0 between SYT packets is normal
            gptp_now_ns);

        // Hand the accepted audio to the RX sink (today the WAN tunnel; the sink
        // decides whether to consume it -- e.g. only when this is the configured
        // tunnel source). The listener does not know what the sink does with it.
        if (audio_sink_ != nullptr) {
            audio_sink_->on_listener_audio(slot->spec.index, StreamAudioFormat::am824_mbla, audio);
        }
        if (timed_sink_ != nullptr && samples_this != 0) {
            uint16_t const channels = slot->am824->config.channel_count;
            timed_sink_->on_listener_packet(StreamRxPacket{
                .stream_index = slot->spec.index,
                .fmt = StreamAudioFormat::am824_mbla,
                .bytes_per_sample = 4,
                .payload = audio.first(std::min(audio.size(), samples_this * 4 * channels)),
                .frames = static_cast<uint32_t>(samples_this),
                .channels = channels,
                .pts_ns = pts_first,
                .pts_valid = slot->am824->has_valid_anchor,
                .tu = pdu.stream_header.tu(),
                .mr = pdu.stream_header.mr(),
                .sequence_num = pdu.stream_header.sequence_num.get()});
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
        // --- CRF (kit phase 3): media-clock reference, no audio payload ---
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
        update_stream_input_counters(
            *slot, pdu.get_sequence_num(), 0, false, pdu.tu(), pdu.mr(), /*format_ok=*/true, 0, /*ts_sparse=*/true, gptp_now_ns);
        slot->crf->process_packet(pdu, ts_data, [slot, gptp_now_ns](uint64_t ts_ns, uint16_t index) {
            // Each (CRF timestamp, local receive time) pair feeds media-clock
            // recovery via the registered consumer.
            if (slot->on_crf) {
                slot->on_crf(ts_ns, index, gptp_now_ns);
            }
        });
        slot->rx_packets.add(1);
        slot->rx_samples.add(slot->crf->last_timestamp_count);
    }
}

auto ListenerStreams::deliver_aaf_audio(
    ListenerStreamSlot& slot, avtp::AafPdu const& pdu, std::span<uint8_t const> audio, avtp::Nanoseconds const now) -> uint64_t
{
    AafDelivery delivery{};
    avtp::aaf_stream_deserialize<MAX_AAF_INPUT_CHANNELS>(
        *slot.aaf,
        pdu,
        audio,
        now,
        [&delivery, &slot](uint8_t ch, std::span<float> s, avtp::Nanoseconds pts, avtp::Nanoseconds period) {
            delivery = AafDelivery{.samples = s.size(), .pts = pts, .period = period};
            offer_audio(slot, ch, s, pts, period);
        });
    offer_silent_channels(slot, slot.aaf->channel_count, delivery);
    slot.rx_packets.add(1);
    slot.rx_samples.add(delivery.samples);
    return delivery.samples;
}

auto ListenerStreams::accepted_aaf_pdu(ListenerStreamSlot& slot, std::span<uint8_t const> frame, avtp::Nanoseconds const now)
    -> std::optional<avtp::AafPdu>
{
    follow_stream_format(slot);
    auto const pdu = avtp::aaf_parse_header(frame);
    if (pdu && avtp::aaf_pdu_is_of_format(*slot.aaf, *pdu, avtp::aaf_get_audio_payload(*pdu, frame).size())) {
        return pdu;
    }
    slot.rx_bad.add(1);
    update_stream_input_counters(slot, 0, 0, false, false, false, /*format_ok=*/false, 0, /*ts_sparse=*/false, now);
    return std::nullopt;
}

void ListenerStreams::offer_aaf_to_sink(ListenerStreamSlot const& slot, std::span<uint8_t const> const audio)
{
    // Hand the accepted audio to the RX sink (today the WAN tunnel). Symmetric
    // to the AM824 path above; the sink ignores it unless AAF is its source.
    if (audio_sink_ == nullptr) {
        return;
    }
    uint16_t const width = slot.spec.format.channels;
    if (slot.aaf->channel_count == width) {
        audio_sink_->on_listener_audio(slot.spec.index, StreamAudioFormat::aaf_int32, audio);
        return;
    }
    std::array<uint8_t, SINK_SCRATCH_OCTETS> widened{};
    size_t const octets =
        avtp::aaf_frames_to_width(audio, avtp::aaf_bytes_per_sample(slot.aaf->format), slot.aaf->channel_count, width, widened);
    if (octets != 0) {
        audio_sink_->on_listener_audio(slot.spec.index, StreamAudioFormat::aaf_int32, std::span{widened}.first(octets));
    }
}

void ListenerStreams::receive_aaf_frame(ListenerStreamSlot& slot, std::span<uint8_t const> frame, avtp::Nanoseconds const now)
{
    auto const pdu = accepted_aaf_pdu(slot, frame, now);
    if (!pdu) {
        return;
    }
    std::span<uint8_t const> const audio = avtp::aaf_get_audio_payload(*pdu, frame);
    uint64_t const samples_this = deliver_aaf_audio(slot, *pdu, audio, now);
    update_stream_input_counters(
        slot,
        pdu->get_sequence_num(),
        pdu->get_avtp_timestamp(),
        pdu->tv(),
        pdu->tu(),
        pdu->mr(),
        /*format_ok=*/true,
        samples_this,
        /*ts_sparse=*/false,  // AAF timestamps every packet: tv=0 is a real fault
        now);
    offer_aaf_to_sink(slot, audio);
}

auto ListenerStreams::parse_crf_frame(ListenerStreamSlot& /*slot*/, std::span<uint8_t const> frame) -> std::optional<avtp::CrfPdu>
{
    if (frame.size() < avtp::CrfPdu::HEADER_LENGTH) {
        return std::nullopt;
    }
    avtp::CrfPdu pdu{};
    span_load(pdu, frame.first(avtp::CrfPdu::HEADER_LENGTH));
    return pdu.is_valid() ? std::optional{pdu} : std::nullopt;
}

namespace {

/// Whether @p pdu, with @p timestamp_octets of timestamp data behind its
/// header, is a CRF AVTPDU of @p format (IEEE 1722-2016 Clause 10.4; Milan
/// v1.3 Table 7.1): its type, base frequency, pull and timestamp interval,
/// and as many timestamps as a PDU of the format carries, all of them there.
[[nodiscard]] auto crf_pdu_is_of_format(
    StreamFormatFields const& format, avtp::CrfPdu const& pdu, size_t const timestamp_octets) noexcept -> bool
{
    bool const clock = static_cast<uint8_t>(pdu.get_crf_type()) == format.crf_type &&
        pdu.base_frequency() == format.crf_base_frequency_hz && static_cast<uint8_t>(pdu.get_pull()) == format.crf_pull &&
        pdu.timestamp_interval() == format.crf_timestamp_interval;
    bool const timestamps = pdu.timestamp_count() == format.crf_timestamps_per_pdu &&
        timestamp_octets >= size_t{pdu.timestamp_count()} * avtp::CrfPdu::TIMESTAMP_SIZE;
    return clock && timestamps;
}

}  // namespace

auto ListenerStreams::accepted_crf_pdu(ListenerStreamSlot& slot, std::span<uint8_t const> frame, avtp::Nanoseconds const now)
    -> std::optional<avtp::CrfPdu>
{
    auto const pdu = parse_crf_frame(slot, frame);
    if (pdu && crf_pdu_is_of_format(slot.spec.format, *pdu, frame.size() - avtp::CrfPdu::HEADER_LENGTH)) {
        return pdu;
    }
    slot.rx_bad.add(1);
    update_stream_input_counters(slot, 0, 0, false, false, false, /*format_ok=*/false, 0, /*ts_sparse=*/false, now);
    return std::nullopt;
}

void ListenerStreams::deliver_crf_timestamps(
    ListenerStreamSlot& slot, avtp::CrfPdu const& pdu, std::span<uint8_t const> const timestamp_data, avtp::Nanoseconds const now)
{
    if (!slot.crf) {
        return;  // no CRF input: the dispatcher sends none here, and a direct caller gets nothing
    }
    uint32_t const source = crf_source_for(slot, pdu);
    slot.crf->process_packet(
        pdu, timestamp_data, [&slot, rx_ns = now.count(), mr = pdu.mr(), source](uint64_t ts_ns, uint16_t index) {
            // Each (CRF timestamp, local receive time) pair feeds media-clock
            // recovery via the registered consumer, with the PDU's mr and source.
            if (slot.on_crf) {
                slot.on_crf(ts_ns, index, rx_ns, mr, source);
            }
        });
    slot.rx_packets.add(1);
    slot.rx_samples.add(slot.crf->last_timestamp_count);
}

auto ListenerStreams::crf_media_clock_events(uint16_t const stream_index) const noexcept -> std::optional<uint32_t>
{
    auto const* const slot = slot_for(stream_index);
    if (slot == nullptr || !slot->crf) {
        return std::nullopt;
    }
    auto const& c = slot->counters;
    return c.media_reset.load(std::memory_order_relaxed) + c.stream_interrupted.load(std::memory_order_relaxed) +
        slot->stream_changes.load(std::memory_order_acquire);
}

auto ListenerStreams::crf_source_for(ListenerStreamSlot& slot, avtp::CrfPdu const& pdu) noexcept -> uint32_t
{
    uint64_t const stream_id = pdu.stream_id().to_uint64();
    uint32_t const change = slot.stream_changes.load(std::memory_order_acquire);
    if (slot.crf_source == 0 || stream_id != slot.crf_stream_id || change != slot.crf_stream_change) {
        slot.crf_stream_id = stream_id;
        slot.crf_stream_change = change;
        slot.crf_source = ++crf_sources_;
    }
    return slot.crf_source;
}

void ListenerStreams::tally_crf_pdu(ListenerStreamSlot& slot, avtp::CrfPdu const& pdu, avtp::Nanoseconds const now)
{
    auto const& format = slot.spec.format;
    tally_stream_input_packet(
        slot.counters,
        pdu.get_sequence_num(),
        static_cast<uint32_t>(slot.crf->first_timestamp_ns),  // the low 32 bits, which an avtp_timestamp is of gPTP time
        /*tv=*/true,
        pdu.tu(),
        pdu.mr(),
        /*format_ok=*/true,
        uint64_t{format.crf_timestamps_per_pdu} * format.crf_timestamp_interval,
        /*ts_sparse=*/false,
        now.count(),
        lock_tolerance_ns_,
        format.crf_base_frequency_hz);
    drop_lock_of_a_lost_stream(slot);
}

void ListenerStreams::receive_crf_frame(ListenerStreamSlot& slot, std::span<uint8_t const> frame, avtp::Nanoseconds const now)
{
    auto const pdu = accepted_crf_pdu(slot, frame, now);
    if (!pdu) {
        return;
    }
    deliver_crf_timestamps(slot, *pdu, frame.subspan(avtp::CrfPdu::HEADER_LENGTH), now);
    tally_crf_pdu(slot, *pdu, now);
}

auto ListenerStreams::frame_is_for_listener(uint16_t const stream_index, std::span<uint8_t const> frame) const -> bool
{
    auto const* slot = slot_for(stream_index);
    return slot != nullptr && slot_receives(*slot, frame);
}

auto ListenerStreams::slot_receives(ListenerStreamSlot const& slot, std::span<uint8_t const> frame) -> bool
{
    // AVTP stream_id is 8 bytes at offset 4 (subtype@0, sv/ver/flags@1, seq@2,
    // ...). The frame arrives here as the AVTPDU: the L2 header and any VLAN
    // tag are gone. Checked here, on the one path every caller takes, so a
    // frame too short to read is refused whichever way it came.
    if (frame.size() < 4 + ieee::Eui64::LENGTH) {
        return false;
    }
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
    avtp::Nanoseconds const now)
{
    // The bookkeeping itself lives in avb_entity_stream_counters.* (unit-tested); feed
    // it the entity-state inputs it can't see (gPTP-now, lock tolerance, Fs). now
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
        now.count(),
        lock_tolerance_ns_,
        sample_rate_);
    drop_lock_of_a_lost_stream(slot);
}

void ListenerStreams::drop_lock_of_a_lost_stream(ListenerStreamSlot& slot)
{
    // An AVTPDU taken up just before the input lost its stream may lock it
    // after the reactor thread has dropped the lock (on_listener_disconnected).
    // The reactor clears the stream id first and the lock after, so a lock
    // set after that sees the id gone here, and is dropped; the run restarts,
    // so this AVTPDU is not the one the next stream's first is compared with.
    if (slot.rx_stream_id.load() == 0) {
        restart_stream_input(slot.counters);
    }
}

namespace {

/// The running counter behind each STREAM_INPUT counter of IEEE 1722.1-2021
/// Clause 7.4.42, by its bit in counters_valid: counters[bit] holds the value.
constexpr std::array<std::pair<uint8_t, std::atomic<uint32_t> StreamInputCounters::*>, 12> STREAM_INPUT_COUNTERS{{
    {0, &StreamInputCounters::media_locked},
    {1, &StreamInputCounters::media_unlocked},
    {2, &StreamInputCounters::stream_interrupted},
    {3, &StreamInputCounters::seq_num_mismatch},
    {4, &StreamInputCounters::media_reset},
    {5, &StreamInputCounters::timestamp_uncertain},
    {6, &StreamInputCounters::timestamp_valid},
    {7, &StreamInputCounters::timestamp_not_valid},
    {8, &StreamInputCounters::unsupported_format},
    {9, &StreamInputCounters::late_timestamp},
    {10, &StreamInputCounters::early_timestamp},
    {11, &StreamInputCounters::frames_rx},
}};

}  // namespace

auto ListenerStreams::fill_stream_input_counters(
    uint16_t const descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& out) const -> bool
{
    auto const* slot = slot_for(descriptor_index);
    if (slot == nullptr) {
        return false;
    }
    for (auto const& [bit, counter] : STREAM_INPUT_COUNTERS) {
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
        ready ? logging::lit("Ready") : logging::lit("not Ready yet (no Talker Advertise registered)"),
        joined ? logging::lit("ok") : logging::lit("none"));
}

void ListenerStreams::log_settled(
    uint16_t const stream_index, nanoavb::TalkerMatch const& probed, bool const ready, bool const joined)
{
    if (!logger_) {
        return;
    }
    logger_->status(
        "acmp: listener stream {} ({}) SETTLED on dest={:012x} vlan={} -> MSRP Listener {}, mcast join {}",
        stream_index,
        kind_lit(slot_for(stream_index)),
        probed.dest_address.to_uint64(),
        probed.vlan_id,
        ready ? logging::lit("Ready") : logging::lit("waits for that Talker attribute"),
        joined ? logging::lit("ok") : logging::lit("none"));
}

auto ListenerStreams::attach_ready(
    ieee::Eui64 const& stream_id, sm::TimePoint const now, std::optional<nanoavb::TalkerMatch> const& only) -> bool
{
    // Attach (kit phase 5d): declares Ready when the talker's Advertise is
    // registered. Until then (e.g. a fast-connect racing the talker's boot)
    // the handler waits with nothing declared and declares Ready when it
    // appears; a registered Talker Failed gets Asking Failed.
    auto const result = components_.msrp_handler.listener_attach(to_stream_id(stream_id), now, only);
    return result.has_value() && *result == nanoavb::ListenerReservationState::Ready;
}

auto ListenerStreams::attach_or_held_ready(
    ListenerConnection const& held,
    ieee::Eui64 const& stream_id,
    sm::TimePoint const now,
    std::optional<nanoavb::TalkerMatch> const& only) -> bool
{
    // One attachment per stream a slot holds: a slot already on this stream
    // attaches nothing again, so attach and withdraw stay one-for-one however
    // many times a controller repeats the CONNECT. What the declaration
    // follows is the last connection's all the same.
    return held.stream_id != stream_id ? attach_ready(stream_id, now, only) : follow_ready(stream_id, now, only);
}

auto ListenerStreams::follow_ready(
    ieee::Eui64 const& stream_id, sm::TimePoint const now, std::optional<nanoavb::TalkerMatch> const& only) -> bool
{
    return components_.msrp_handler.listener_follow(to_stream_id(stream_id), only, now) == nanoavb::ListenerReservationState::Ready;
}

void ListenerStreams::on_listener_connected(
    uint16_t const stream_index,
    ieee::Eui64 const& stream_id,
    ieee::Eui48 const dest_mac,
    sm::TimePoint const now,
    std::optional<nanoavb::TalkerMatch> const& only)
{
    auto* slot = slot_for(stream_index);
    if (slot == nullptr) {
        return;  // not one of our inputs: nothing to receive on, nothing to join for
    }
    bool const ready = attach_or_held_ready(slot->connection, stream_id, now, only);
    take_connection(slot->connection, stream_id, group_to_join(dest_mac), now);
    publish_connection(*slot, stream_id, only.has_value());
    log_attached(stream_index, dest_mac, only, ready);
}

void ListenerStreams::publish_connection(ListenerStreamSlot& slot, ieee::Eui64 const& stream_id, bool const settled)
{
    slot.connection.settled = settled;
    publish_rx_stream(slot, stream_id.to_uint64());  // the RX thread may match its frames from here on
}

void ListenerStreams::publish_rx_stream(ListenerStreamSlot& slot, uint64_t const stream_id) noexcept
{
    bool const moved = slot.rx_stream_id.load() != stream_id;
    if (moved) {
        // The count before the stream: an AVTPDU matched by the new stream_id is then delivered as a new
        // source. One of the old stream delivered after the count is caught by crf_source_for's stream_id.
        slot.stream_changes.fetch_add(1, std::memory_order_release);
    }
    slot.rx_stream_id.publish(stream_id);
    if (moved) {
        notify_input_changed(slot);
    }
}

void ListenerStreams::notify_input_changed(ListenerStreamSlot const& slot) const noexcept
{
    if (auto const state = input_state(slot.spec.index); state && on_input_changed_) {
        on_input_changed_(slot.spec.index, *state);
    }
}

auto ListenerStreams::input_state(uint16_t const stream_index) const noexcept -> std::optional<InputState>
{
    auto const* const slot = slot_for(stream_index);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return InputState{
        .stream_id = slot->rx_stream_id.load(), .stopped = stream_stopped(stream_index), .format = stream_format(stream_index)};
}

auto ListenerStreams::notify_offload_received(
    uint16_t const stream_index, StreamInputCounts const& counts, uint64_t const samples) noexcept -> bool
{
    auto* const slot = slot_for(stream_index);
    if (slot == nullptr) {
        return false;
    }
    if (stream_stopped(stream_index) || slot->rx_stream_id.load() == 0) {
        count_offload_idle(*slot, counts);  // nothing delivered
        return true;
    }
    add_stream_input_counts(slot->counters, counts);
    count_offload_delivered(*slot, counts, samples);
    return true;
}

void ListenerStreams::count_offload_idle(ListenerStreamSlot& slot, StreamInputCounts const& counts) noexcept
{
    add_stream_input_counts(slot.counters, without_lock(counts));
    // A stopped input is not synchronized and loses no lock as an interruption (Milan v1.3 5.3.8.7). One with no
    // stream is left to the reactor, which drops its lock as it releases the stream (5.3.8.10, Table 5.6).
    if (stream_stopped(slot.spec.index)) {
        unlock_stream_input(slot.counters);
    }
}

void ListenerStreams::count_offload_delivered(
    ListenerStreamSlot& slot, StreamInputCounts const& counts, uint64_t const samples) noexcept
{
    uint32_t const bad = std::min(counts.unsupported_format, counts.frames_rx);
    slot.rx_bad.add(bad);
    slot.rx_packets.add(counts.frames_rx - bad);
    slot.rx_samples.add(samples);
}

void ListenerStreams::set_stream_stopped(uint16_t const stream_index, bool const stopped) noexcept
{
    if (stream_index >= stream_stopped_.size() ||
        stream_stopped_[stream_index].exchange(stopped, std::memory_order_acq_rel) == stopped) {
        return;
    }
    if (auto const* const slot = slot_for(stream_index); slot != nullptr) {
        notify_input_changed(*slot);
    }
}

void ListenerStreams::log_attached(
    uint16_t const stream_index, ieee::Eui48 const dest_mac, std::optional<nanoavb::TalkerMatch> const& only, bool const ready)
{
    auto const* slot = slot_for(stream_index);
    bool const joined = slot != nullptr && slot->connection.group.has_value();
    if (only.has_value()) {
        log_settled(stream_index, *only, ready, joined);
    } else {
        log_connected(stream_index, dest_mac, ready, joined);
    }
}

void ListenerStreams::on_listener_disconnected(uint16_t const stream_index, sm::TimePoint const now, StreamInputLoss const why)
{
    bool settled = false;
    if (auto* slot = slot_for(stream_index); slot != nullptr) {
        settled = slot->connection.settled;
        publish_rx_stream(*slot, 0);  // no frame is this input's any more
        // With no stream it is not synchronized: a held lock is dropped here (Milan
        // v1.3 5.3.8.10), as an interruption unless the stream was released. After
        // the id, see update_stream_input_counters.
        if (why == StreamInputLoss::talker_lost) {
            interrupt_stream_input(slot->counters);
        }
        restart_stream_input(slot->counters);
        release_connection(slot->connection, now);
    }
    if (logger_) {
        logger_->status(
            "acmp: listener stream {} ({}) {} -> MSRP Listener withdrawn",
            stream_index,
            kind_lit(slot_for(stream_index)),
            settled ? logging::lit("no longer SETTLED") : logging::lit("DISCONNECTED from talker"));
    }
}

}  // namespace statusbar::avb_entity
