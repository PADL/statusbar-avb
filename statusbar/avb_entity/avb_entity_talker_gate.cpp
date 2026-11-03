// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerGate methods — the per-stream transmit gate, moved out of
// avb_entity_audio_io.cpp (god-object phase 4). The gate holds same-typed refs
// (config_/components_).

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include <cstdint>

namespace statusbar::avb_entity {

void TalkerGate::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready)
{
    // Per-stream transmit gate: record whether this talker stream now has a
    // listener that permits transmit (MSRP Listener Ready). Matched to the stream
    // index by the ACMP stream_id (same value MSRP advertises). The grace window
    // is the media thread's: should_transmit records when it last saw the flag.
    uint64_t const sid_u64 = stream_id.to_uint64();
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(listener_ready_.size()); ++idx) {
        auto const* s = components_.acmp_talker.get_stream(idx);
        if (s != nullptr && s->stream_id.to_uint64() == sid_u64) {
            bool const was = listener_ready_[idx].exchange(ready, std::memory_order_relaxed);
            if (ready && acmp_conn_[idx].load() > 0) {
                // Only a reservation made for a live connection admits: a Ready seen with none admits nothing.
                latch_ready(idx);
            }
            if (was != ready && logger_) {
                logger_->status("srp: talker stream {} MSRP listener-ready -> {} (ACMP-AND-MSRP gate)", idx, ready);
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

auto TalkerGate::listener_ready_within_grace(uint16_t const idx, RawTimePoint const now) noexcept -> bool
{
    // MSRP Listener Ready, held across the MRP LeaveAll re-registration blip by a
    // grace window: the peer's Listener declaration ages out and re-declares on a
    // ~10s leave-all cycle (Ready momentarily withdrawn for ~1-3s), which must NOT
    // chop the stream. Stay up while Ready is currently set OR was seen set within
    // GRACE of this wake. A genuine listener departure (no re-declare for GRACE)
    // closes the gate. The wake we are told is the clock: nothing here reads one.
    if (listener_ready_[idx].load(std::memory_order_relaxed)) {
        last_seen_ready_[idx] = now;
        return true;
    }
    auto const last_seen = last_seen_ready_[idx];
    return last_seen != NEVER && (now - last_seen) < GRACE;
}

}  // namespace statusbar::avb_entity
