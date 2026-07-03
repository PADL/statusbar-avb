// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerGate methods — the per-stream transmit gate, moved out of
// avb_entity_audio_io.cpp (god-object phase 4). Bodies unchanged except the
// member names (msrp_listener_ready_ -> listener_ready_, msrp_ready_ns_ ->
// ready_ns_) and the qualifier; the gate holds same-typed refs (config_/components_).

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include <chrono>
#include <cstdint>
#include <print>

namespace statusbar::avb_entity {

void TalkerGate::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready)
{
    // Per-stream transmit gate: record whether this talker stream now has a
    // listener that permits transmit (MSRP Listener Ready). Matched to the stream
    // index by the ACMP stream_id (same value MSRP advertises).
    uint64_t const sid_u64 = stream_id.to_uint64();
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(listener_ready_.size()); ++idx) {
        auto const* s = components_.acmp_talker.get_stream(idx);
        if (s != nullptr && s->stream_id.to_uint64() == sid_u64) {
            bool const was = listener_ready_[idx].exchange(ready, std::memory_order_relaxed);
            if (ready) {
                // Stamp the last-ready time so the strict gate's grace window
                // survives the next LeaveAll re-registration blip.
                ready_ns_[idx].store(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                        .count(),
                    std::memory_order_relaxed);
            }
            if (was != ready) {
                std::print("[srp] talker stream {} MSRP listener-ready -> {} (ACMP-AND-MSRP gate)\n", idx, ready);
            }
        }
    }
}

auto TalkerGate::should_transmit(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    if (!config_.gate_talker_on_listener) {
        // Gate disabled: transmit unconditionally. NOTE: this puts the stream on the
        // SR class with NO ACMP connection and NO reservation, which is not AVB-spec
        // compliant -- for bench debugging / free-running reference sources only.
        return true;
    }
    if (idx >= stream_started_.size()) {
        return false;
    }
    // Spec-correct SR-class admission for THIS stream, evaluated on its own state
    // (a CRF stream must never ride the audio streams' gate). Requires, together:
    //   (1) an ACMP connection for this stream (implies our Talker Advertise was
    //       registered, so no separate Talker-attribute check is needed), and
    //   (2) the stream is Started (defaults true), and
    //   (3) MSRP Listener Ready for this stream.
    // ACMP connection count is published by the reactor thread (note_acmp_connections)
    // rather than read live from the reactor-mutated connection list here.
    bool const acmp = acmp_conn_[idx].load() > 0;
    if (!acmp) {
        return false;
    }
    if (!stream_started_[idx].load(std::memory_order_relaxed)) {
        return false;
    }
    // MSRP Listener Ready, held across the MRP LeaveAll re-registration blip by a
    // grace window: the peer's Listener declaration ages out and re-declares on a
    // ~10s leave-all cycle (Ready momentarily withdrawn for ~1-3s), which must NOT
    // chop the stream. Stay up while Ready is currently set OR was set within
    // GRACE. A genuine listener departure (no re-declare for GRACE) closes the gate.
    int64_t const last_ready = ready_ns_[idx].load(std::memory_order_relaxed);
    bool const msrp = listener_ready_[idx].load(std::memory_order_relaxed) || (last_ready != 0 && (now_ns - last_ready) < GRACE_NS);
    return msrp;
}

}  // namespace statusbar::avb_entity
