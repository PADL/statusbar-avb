// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ListenerStreams methods — the local AVB stream RX path. Slots are shaped by
// StreamSpecs (kit phase 1); the per-kind deserialize bodies are unchanged
// from the fixed-two-context era, but a frame now finds its slot by the
// connected stream_id rather than by a hardcoded subtype->index mapping.

#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
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
    switch (slot->spec.format.kind) {
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

}  // namespace

auto ListenerStreams::open_stream(StreamSpec const& spec) -> Status
{
    if (slots_.size() == MAX_ENTITY_STREAMS) {
        return failure(std::errc::result_out_of_range);
    }
    switch (spec.format.kind) {
        case StreamKind::am824: {
            auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            auto& slot = slots_.emplace_back();  // in place: the slot holds atomics
            slot.spec = spec;
            slot.am824.emplace(*rate, static_cast<uint8_t>(spec.format.channels));
            return success();
        }
        case StreamKind::aaf: {
            auto const rate = avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz);
            if (!rate || spec.format.channels == 0) {
                return failure(std::errc::invalid_argument);
            }
            auto& slot = slots_.emplace_back();
            slot.spec = spec;
            slot.aaf.emplace(spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth);
            return success();
        }
        case StreamKind::crf:  // CRF input = media-clock recovery (kit phase 3)
        case StreamKind::other:
        default:
            return failure(std::errc::not_supported);
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
        if (slot.spec.format.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::slot_of(StreamKind const kind) const noexcept -> ListenerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.spec.format.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto ListenerStreams::drain_rx(int64_t const gptp_now_ns) -> size_t
{
    if (rx_sock_ == nullptr) {
        return 0;
    }
    // Drain every queued frame in one pass (the arrival rate is 8000/s = 125 us apart;
    // a 50 us RX-timer tick or a reactor wake sees 0-3 frames). All frames in this pass
    // share gptp_now_ns -- the caller's fresh gPTP "now".
    ieee::Eui48 src{};
    ieee::Eui48 dst{};
    size_t drained = 0;
    while (true) {
        auto const r = rx_sock_->recv(&src, &dst, rx_buf_);
        if (!r || *r <= 0) {
            break;
        }
        on_stream_rx_frame({rx_buf_.data(), static_cast<size_t>(*r)}, gptp_now_ns);
        ++drained;
    }
    return drained;
}

void ListenerStreams::on_stream_rx_frame(std::span<uint8_t const> frame, int64_t gptp_now_ns)
{
    if (frame.empty()) {
        return;
    }

    // Find the slot this frame belongs to: kind must match the AVTP subtype and
    // the frame's stream_id must match the stream connected to that input (the
    // promiscuous socket also sees our own TX and any other stream on the
    // segment). Stream_ids are unique, so the first match is the only match.
    auto const subtype_kind = frame[0] == avtp::AvtpSubtype::iec_61883_iidc ? StreamKind::am824
        : frame[0] == avtp::AvtpSubtype::aaf                                ? StreamKind::aaf
                                                                            : StreamKind::other;
    if (subtype_kind == StreamKind::other) {
        return;
    }
    ListenerStreamSlot* slot = nullptr;
    for (auto& s : slots_) {
        if (s.spec.format.kind == subtype_kind && frame_is_for_listener(s.spec.index, frame)) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) {
        return;
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
        std::span<uint8_t const> const audio = frame.subspan(avtp::Am824Pdu::HEADER_LENGTH);
        uint64_t samples_this = 0;
        avtp::am824_deserialize_mbla(
            *slot->am824,
            pdu,
            audio,
            static_cast<uint64_t>(gptp_now_ns),
            [&samples_this](uint8_t /*ch*/, std::span<float> s, uint64_t /*pts*/, uint64_t /*period*/) {
                samples_this = s.size();
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
    } else if (slot->aaf) {
        // --- AAF ---
        auto pdu_opt = avtp::aaf_parse_header(frame);
        if (!pdu_opt) {
            slot->rx_bad.add(1);
            update_stream_input_counters(
                *slot, 0, 0, false, false, false, /*format_ok=*/false, 0, /*ts_sparse=*/false, gptp_now_ns);
            return;
        }
        std::span<uint8_t const> const audio = avtp::aaf_get_audio_payload(frame);
        uint64_t samples_this = 0;
        avtp::aaf_stream_deserialize(
            *slot->aaf,
            *pdu_opt,
            audio,
            static_cast<uint64_t>(gptp_now_ns),
            [&samples_this](uint8_t /*ch*/, std::span<float> s, uint64_t /*pts*/, uint64_t /*period*/) {
                samples_this = s.size();
            });
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
    }
}

auto ListenerStreams::frame_is_for_listener(uint16_t const stream_index, std::span<uint8_t const> frame) const -> bool
{
    // AVTP stream_id is 8 bytes at offset 4 (subtype@0, sv/ver/flags@1, seq@2,
    // ...). The VLAN tag is already stripped by RawnetContext::recv.
    if (frame.size() < 12) {
        return false;
    }
    auto const* ls = components_.acmp_listener.get_stream(stream_index);
    if (ls == nullptr || !ls->connected) {
        return false;
    }
    auto const id_bytes = make_const_span(ls->stream_id);  // 8 bytes
    return std::equal(id_bytes.begin(), id_bytes.end(), frame.begin() + 4);
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
        static_cast<uint64_t>(gptp_now_ns),
        lock_tolerance_ns_,
        sample_rate_);
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

void ListenerStreams::on_listener_connected(uint16_t const stream_index, ieee::Eui64 const& stream_id, ieee::Eui48 dest_mac)
{
    tsn::StreamId sid{};
    (void)statusbar::tsn::load_unchecked(stream_id.span(), &sid);
    auto const now = sm::Clock::now();
    auto const result = components_.msrp_handler.listener_ready(sid, now);
    // Join the talker's stream group so the NIC delivers its frames to us.
    bool const joined = rx_sock_ != nullptr && rx_sock_->join_multicast(dest_mac).has_value();
    if (logger_) {
        logger_->status(
            "acmp: listener stream {} ({}) CONNECTED to talker dest={:012x} -> MSRP Listener Ready {}, mcast join {}",
            stream_index,
            kind_lit(slot_for(stream_index)),
            dest_mac.to_uint64(),
            result.has_value() ? logging::lit("declared") : logging::lit("failed"),
            joined ? logging::lit("ok") : logging::lit("FAILED"));
    }
}

void ListenerStreams::on_listener_disconnected(uint16_t const stream_index)
{
    if (auto const* stream = components_.acmp_listener.get_stream(stream_index); stream != nullptr) {
        tsn::StreamId sid{};
        (void)statusbar::tsn::load_unchecked(stream->stream_id.span(), &sid);
        (void)components_.msrp_handler.listener_withdraw(sid, sm::Clock::now());
        // Release the talker's stream group so per-connection dest-MAC churn does
        // not pile up stale NIC multicast filter entries.
        if (rx_sock_ != nullptr) {
            (void)rx_sock_->leave_multicast(stream->stream_dest_mac);
        }
    }
    if (logger_) {
        logger_->status(
            "acmp: listener stream {} ({}) DISCONNECTED from talker -> MSRP Listener withdrawn",
            stream_index,
            kind_lit(slot_for(stream_index)));
    }
}

}  // namespace statusbar::avb_entity
