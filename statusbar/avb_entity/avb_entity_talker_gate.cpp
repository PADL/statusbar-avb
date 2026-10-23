// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerGate methods — the per-stream SRP transmit gate, moved out of
// avb_entity_audio_io.cpp (god-object phase 4).

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include <cstdint>

namespace statusbar::avb_entity {

void TalkerGate::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, int64_t const now_ns)
{
    // Record whether this talker stream has a listener that permits transmit (Listener Ready /
    // Ready Failed), matched to the stream index by stream_id.
    uint64_t const sid_u64 = stream_id.to_uint64();
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(listener_ready_.size()); ++idx) {
        auto const* s = components_.acmp_talker.get_stream(idx);
        if (s == nullptr || s->stream_id.to_uint64() != sid_u64) {
            continue;
        }
        bool const was = listener_ready_[idx].exchange(ready, std::memory_order_relaxed);
        if (ready || was) {
            // The grace window runs from the last moment the declaration was known registered;
            // a withdrawal seen while already withdrawn does not extend it.
            ready_ns_[idx].store(now_ns, std::memory_order_relaxed);
        }
        if (was != ready && logger_) {
            logger_->status("srp: talker stream {} MSRP listener-ready -> {} (SRP gate)", idx, ready);
        }
    }
}

void TalkerGate::note_talker_withdrawn() noexcept
{
    for (size_t idx = 0; idx < listener_ready_.size(); ++idx) {
        listener_ready_[idx].store(false, std::memory_order_relaxed);
        ready_ns_[idx].store(0, std::memory_order_relaxed);
    }
}

void TalkerGate::note_talker_withdrawn(uint16_t const idx) noexcept
{
    if (idx < listener_ready_.size()) {
        listener_ready_[idx].store(false, std::memory_order_relaxed);
        ready_ns_[idx].store(0, std::memory_order_relaxed);
    }
}

auto TalkerGate::admitted(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    if (!gate_enabled_) {
        // Gate disabled: transmit unconditionally. NOTE: this puts the stream on the
        // SR class with NO reservation, which is not AVB-spec compliant -- for
        // bench debugging / free-running reference sources only.
        return true;
    }
    if (idx >= stream_started_.size() || !stream_started_[idx].load(std::memory_order_relaxed)) {
        return false;
    }
    return listener_admitted(idx, now_ns);
}

auto TalkerGate::listener_admitted(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
{
    if (idx >= listener_ready_.size()) {
        return false;
    }
    // Listener Ready / Ready Failed registered, held across the MRP LeaveAll re-registration
    // blip by GRACE_NS; a listener that does not re-declare within it closes the gate.
    if (listener_ready_[idx].load(std::memory_order_relaxed)) {
        return true;
    }
    int64_t const last_ready = ready_ns_[idx].load(std::memory_order_relaxed);
    return last_ready != 0 && (now_ns - last_ready) < GRACE_NS;
}

}  // namespace statusbar::avb_entity
