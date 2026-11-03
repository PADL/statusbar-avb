// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerGate methods — the per-stream transmit gate, moved out of
// avb_entity_audio_io.cpp (god-object phase 4). The gate holds same-typed refs
// (config_/components_).

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

namespace statusbar::avb_entity {

void TalkerGate::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, RawTimePoint const now)
{
    // Per-stream transmit gate: record whether this talker stream now has a
    // listener that permits transmit (MSRP Listener Ready), and when that was
    // last known. Matched to the stream index by the ACMP stream_id (same
    // value MSRP advertises).
    uint64_t const sid_u64 = stream_id.to_uint64();
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(listener_ready_.size()); ++idx) {
        auto const s = components_.talker_stream(idx);
        if (s.has_value() && s->stream_id.to_uint64() == sid_u64) {
            bool const was = listener_ready_[idx].exchange(ready, std::memory_order_relaxed);
            if (ready && acmp_conn_[idx].load() > 0) {
                // Only a reservation made for a live connection admits: a Ready seen with none admits nothing.
                latch_ready(idx);
            }
            if (was != ready && logger_) {
                logger_->status(
                    "srp: talker stream {} MSRP listener-ready -> {} ({} gate)",
                    idx,
                    ready,
                    needs_acmp_connection_ ? logging::lit("ACMP-AND-MSRP") : logging::lit("MSRP"));
            }
        }
    }
}

void TalkerGate::latch_ready(uint16_t const idx) noexcept
{
    reservation_ready_[idx] = true;
}

void TalkerGate::note_acmp_connections(uint16_t const idx, uint32_t const count) noexcept
{
    if (idx >= acmp_conn_.size()) {
        return;
    }
    acmp_conn_[idx].publish(count);
    if (count == 0) {
        reservation_ready_[idx] = false;
    } else if (listener_ready_[idx].load(std::memory_order_relaxed)) {
        latch_ready(idx);  // connected to a listener whose Ready is already declared
    }
}

auto TalkerGate::should_transmit(uint16_t const idx, RawTimePoint const now) noexcept -> bool
{
    if (!gate_enabled_) {
        // Gate disabled: transmit unconditionally. NOTE: this puts the stream on the
        // SR class with NO ACMP connection and NO reservation, which is not AVB-spec
        // compliant -- for bench debugging / free-running reference sources only.
        return true;
    }
    // Spec-correct SR-class admission for THIS stream, evaluated on its own state
    // (a CRF stream must never ride the audio streams' gate). Requires, together:
    //   (1) an ACMP connection for this stream (implies our Talker Advertise was
    //       registered, so no separate Talker-attribute check is needed), and
    //   (2) the stream is Started (defaults true), and
    //   (3) MSRP Listener Ready for this stream.
    // The connection count is published by the reactor (note_acmp_connections).
    if (idx >= stream_started_.size()) {
        return false;
    }
    if (acmp_conn_[idx].load() == 0) {
        last_seen_ready_[idx] = NEVER;  // the last disconnect forgets the grace window
        return false;
    }
    return stream_started_[idx].load(std::memory_order_relaxed) && listener_ready_within_grace(idx, now);
}

auto TalkerGate::listener_ready_within_grace(uint16_t const idx, RawTimePoint const now) const noexcept -> bool
{
    // MSRP Listener Ready, held across the MRP LeaveAll re-registration blip by the
    // grace: the flag first (acquire), then the stamp it was published after. With
    // no grace the registration alone admits, whatever instant the caller was told.
    if (listener_ready_[idx].load(std::memory_order_acquire)) {
        return true;
    }
    if (grace_ <= std::chrono::nanoseconds{0}) {
        return false;
    }
    auto const ends = stamp_plus_grace(idx);
    return ends.has_value() && now < *ends;
}

auto TalkerGate::stamp_plus_grace(uint16_t const idx) const noexcept -> std::optional<RawTimePoint>
{
    int64_t const last_ready = ready_raw_ns_[idx].load(std::memory_order_relaxed);
    if (last_ready == 0) {
        return std::nullopt;
    }
    return RawTimePoint::clock::from_ns(last_ready) + grace_;
}

}  // namespace statusbar::avb_entity
