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

namespace {

/// The kit's lookup: the first talker stream of the ACMP talker declaring @p stream_id.
[[nodiscard]] auto acmp_talker_index(nanoavb::NanoAvbComponents const& components, nanoavb::StreamId const& stream_id) noexcept
    -> std::optional<uint16_t>
{
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(TalkerGate::STREAM_COUNT); ++idx) {
        auto const s = components.talker_stream(idx);
        if (s.has_value() && s->stream_id.to_uint64() == stream_id.to_uint64()) {
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
    // Stream Started defaults true: a stream is admitted once ACMP-connected +
    // Listener Ready unless explicitly stopped via note_stream_started().
    for (auto& started : stream_started_) {
        started.store(true, std::memory_order_relaxed);
    }
}

TalkerGate::TalkerGate(bool const gate_enabled, nanoavb::NanoAvbComponents const& components) noexcept
    : TalkerGate{gate_enabled, [&components](nanoavb::StreamId const& sid) { return acmp_talker_index(components, sid); }}
{}

void TalkerGate::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready, RawTimePoint const now)
{
    // Per-stream transmit gate: record whether this talker stream now has a
    // listener that permits transmit (MSRP Listener Ready), and when that was
    // last known. Matched to the stream index by the stream_id MSRP advertises.
    auto const found = stream_index_ ? stream_index_(stream_id) : std::nullopt;
    if (!found || *found >= listener_ready_.size()) {
        return;
    }
    bool const was = stamp(*found, ready, now);
    if (was != ready && logger_) {
        logger_->status(
            "srp: talker stream {} MSRP listener-ready -> {} ({} gate)",
            *found,
            ready,
            needs_acmp_connection_ ? logging::lit("ACMP-AND-MSRP") : logging::lit("MSRP"));
    }
}

void TalkerGate::notify_talker_withdrawn() noexcept
{
    for (uint16_t idx = 0; idx < static_cast<uint16_t>(listener_ready_.size()); ++idx) {
        notify_talker_withdrawn(idx);
    }
}

void TalkerGate::notify_talker_withdrawn(uint16_t const idx) noexcept
{
    if (idx < listener_ready_.size()) {
        ready_raw_ns_[idx].store(0, std::memory_order_relaxed);  // forgotten before the flag drops
        listener_ready_[idx].store(false, std::memory_order_release);
    }
}

void TalkerGate::note_listener_ready(nanoavb::StreamId const& stream_id, bool const ready)
{
    if (told_now_ == RawTimePoint{} && !std::exchange(untold_logged_, true) && logger_) {
        logger_->status("srp: the talker gate was told no time (tell_time): a withdrawn Listener opens no grace");
    }
    note_listener_ready(stream_id, ready, told_now_);
}

auto TalkerGate::stamp(uint16_t const idx, bool const ready, RawTimePoint const now) noexcept -> bool
{
    // The reactor is the one writer. The grace runs from the last instant the
    // declaration was known registered, so a withdrawal seen while already
    // withdrawn does not extend it. The stamp goes before the flag (release).
    // An instant never told is RawTimePoint{}, which is stored as no stamp.
    bool const was = listener_ready_[idx].load(std::memory_order_relaxed);
    if (ready || was) {
        ready_raw_ns_[idx].store(RawTimePoint::clock::to_ns(now), std::memory_order_relaxed);
    }
    listener_ready_[idx].store(ready, std::memory_order_release);
    return was;
}

auto TalkerGate::admitted(uint16_t const idx, RawTimePoint const now) const noexcept -> bool
{
    // Before anything else, the disabled gate too: a stream whose Talker
    // Advertise is held back has no attribute on the wire to send under.
    if (idx < advertise_held_.size() && advertise_held_[idx].load(std::memory_order_relaxed)) {
        return false;
    }
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
    if (idx >= stream_started_.size() || !connected_and_started(idx)) {
        return false;
    }
    return listener_ready_within_grace(idx, now);
}

auto TalkerGate::listener_admitted(uint16_t const idx, RawTimePoint const now) const noexcept -> bool
{
    return idx < listener_ready_.size() && listener_ready_within_grace(idx, now);
}

auto TalkerGate::grace_ends(uint16_t const idx) const noexcept -> std::optional<RawTimePoint>
{
    if (idx >= listener_ready_.size() || listener_ready_[idx].load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return stamp_plus_grace(idx);
}

auto TalkerGate::connected_and_started(uint16_t const idx) const noexcept -> bool
{
    // ACMP connection count is published by the reactor thread (note_acmp_connections)
    // rather than read live from the reactor-mutated connection list here. A
    // Milan talker has no connections to count (set_needs_acmp_connection).
    bool const connected = !needs_acmp_connection_ || acmp_conn_[idx].load() > 0;
    return connected && stream_started_[idx].load(std::memory_order_relaxed);
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
