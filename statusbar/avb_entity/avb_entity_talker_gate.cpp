// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerGate's methods: the per-stream SRP transmit gate (avb_entity_talker_gate.hpp).

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include "statusbar/nanoavb/nanoavb_components.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace statusbar::avb_entity {

namespace {

/// The kit's lookup: the first ACMP talker stream declaring @p stream_id.
[[nodiscard]] auto acmp_talker_index(nanoavb::NanoAvbComponents const& components, tsn::StreamId const& stream_id) noexcept
    -> std::optional<uint16_t>
{
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(TalkerGate::STREAM_COUNT); ++idx) {
        auto const* s = components.acmp_talker.get_stream(idx);
        if (s != nullptr && s->stream_id.to_uint64() == stream_id.to_uint64()) {
            return idx;
        }
    }
    return std::nullopt;
}

}  // namespace

TalkerGate::TalkerGate(bool const gate_enabled, StreamIndexFn stream_index) noexcept
    : gate_enabled_{gate_enabled}
    , stream_index_{std::move(stream_index)}
{
    // Stream Started defaults true: a stream is admitted once a listener's
    // Ready is registered unless explicitly stopped via note_stream_started().
    for (auto& started : stream_started_) {
        started.store(true, std::memory_order_relaxed);
    }
}

TalkerGate::TalkerGate(bool const gate_enabled, nanoavb::NanoAvbComponents const& components) noexcept
    : TalkerGate{gate_enabled, [&components](tsn::StreamId const& sid) { return acmp_talker_index(components, sid); }}
{}

void TalkerGate::note_listener_ready(tsn::StreamId const& stream_id, bool const ready, int64_t const now_ns)
{
    // Record whether this talker stream has a listener that permits transmit (Listener Ready /
    // Ready Failed), matched to the stream index by stream_id.
    auto const found = stream_index_ ? stream_index_(stream_id) : std::nullopt;
    if (!found || *found >= listener_ready_.size()) {
        return;
    }
    uint16_t const idx = *found;
    bool const was = listener_ready_[idx].exchange(ready, std::memory_order_relaxed);
    stamp_ready(idx, ready, was, now_ns);
    if (was != ready && logger_) {
        logger_->status("srp: talker stream {} MSRP listener-ready -> {} (SRP gate)", idx, ready);
    }
}

void TalkerGate::stamp_ready(uint16_t const idx, bool const ready, bool const was, int64_t const now_ns) noexcept
{
    if (ready) {
        readies_[idx].fetch_add(1, std::memory_order_release);
    }
    if (ready || was) {
        // The grace window runs from the last moment the declaration was known registered;
        // a withdrawal seen while already withdrawn does not extend it.
        ready_ns_[idx].store(now_ns, std::memory_order_relaxed);
    }
}

void TalkerGate::note_talker_withdrawn() noexcept
{
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(listener_ready_.size()); ++idx) {
        note_talker_withdrawn(idx);
    }
}

void TalkerGate::note_talker_withdrawn(uint16_t const idx) noexcept
{
    if (idx < listener_ready_.size()) {
        listener_ready_[idx].store(false, std::memory_order_relaxed);
        ready_ns_[idx].store(0, std::memory_order_relaxed);
        withdrawals_[idx].fetch_add(1, std::memory_order_release);
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

auto TalkerGate::should_transmit(uint16_t const idx, RawTimePoint const now) noexcept -> bool
{
    if (!gate_enabled_) {
        return true;
    }
    if (idx >= stream_started_.size()) {
        return false;
    }
    catch_up(idx, now);  // a stopped stream keeps its grace current too
    return stream_started_[idx].load(std::memory_order_relaxed) && listener_ready_within_grace(idx, now);
}

void TalkerGate::catch_up(uint16_t const idx, RawTimePoint const now) noexcept
{
    // A Ready registered since the last wake counts as seen at this one, however briefly it was
    // set; a withdrawal since then closes the stream at once, forgetting the grace.
    if (uint32_t const readies = readies_[idx].load(std::memory_order_acquire); readies != seen_readies_[idx]) {
        seen_readies_[idx] = readies;
        last_seen_ready_[idx] = now;
    }
    if (uint32_t const withdrawals = withdrawals_[idx].load(std::memory_order_acquire); withdrawals != seen_withdrawals_[idx]) {
        seen_withdrawals_[idx] = withdrawals;
        last_seen_ready_[idx] = NEVER;
    }
}

auto TalkerGate::listener_ready_within_grace(uint16_t const idx, RawTimePoint const now) noexcept -> bool
{
    if (listener_ready_[idx].load(std::memory_order_relaxed)) {
        last_seen_ready_[idx] = now;
        return true;
    }
    return last_seen_ready_[idx] != NEVER && (now - last_seen_ready_[idx]) < GRACE;
}

}  // namespace statusbar::avb_entity
