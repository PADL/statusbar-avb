#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_ingest.hpp
/// @brief Pure helpers for the local-stream -> inter-site tunnel ingest path.
///
/// Companion to avb_entity_udptun_egress.hpp in the EntityUdptunBridge
/// extraction (god-object phase 2): the buffer-agnostic, unit-testable byte
/// conversions the ingest path uses, kept out of AvbEntityAudioIO.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::avb_entity {

/// Transcode AM824 MBLA data blocks to AAF-style int32 PCM into `out`.
///
/// Each MBLA quadlet is [label:8][audio:24] big-endian. The inter-site tunnel
/// and the far egress are AAF int32, so we drop the label and MSB-align the
/// 24-bit audio into an int32 ([a23:16][a15:8][a7:0][0x00] = audio << 8) — which
/// is exactly what stops the far end from reading the 0x40 label as the sample's
/// MSB. Lossless for 24-bit audio.
///
/// Processes floor(min(mbla, out)/4) quadlets and returns the bytes written.
inline auto udptun_am824_mbla_to_int32(std::span<uint8_t const> mbla, std::span<uint8_t> out) -> size_t
{
    size_t const quads = std::min(mbla.size(), out.size()) / 4;
    for (size_t q = 0; q < quads; ++q) {
        size_t const i = q * 4;
        out[i + 0] = mbla[i + 1];  // audio[23:16]
        out[i + 1] = mbla[i + 2];  // audio[15:8]
        out[i + 2] = mbla[i + 3];  // audio[7:0]
        out[i + 3] = 0x00;         // int32 low byte (24-bit MSB-aligned)
    }
    return quads * 4;
}

/// True while real (listener-sourced) tunnel audio is flowing: the last real
/// ingest happened within @p window_ns of @p now_tai_ns. Shared by the punch
/// keepalive (stands down while streaming) and the media thread's
/// silence-source gate (which MUST mirror the keepalive so real audio always
/// wins) — one definition so the two predicates can never drift apart. A zero
/// last/now (no sample yet / no clock) reads as not streaming.
[[nodiscard]] constexpr auto udptun_source_streaming(
    int64_t const last_real_ingest_tai_ns, int64_t const now_tai_ns, int64_t const window_ns = 100'000'000) noexcept -> bool
{
    return last_real_ingest_tai_ns != 0 && now_tai_ns != 0 && (now_tai_ns - last_real_ingest_tai_ns) < window_ns;
}

/// Sweep pacing: how many frames to emit this tick so the cumulative emitted
/// count tracks elapsed TAI at @p sample_rate exactly — the ingest
/// avtp_timestamp stays locked to TAI with zero drift and the far egress
/// (playing on the same GPS-TAI clock) never under/over-runs. Bounded by
/// @p cap per call so a stall never bursts unbounded catch-up.
[[nodiscard]] constexpr auto udptun_sweep_frames_due(
    int64_t const elapsed_ns, uint64_t const frames_emitted, uint32_t const sample_rate, size_t const cap) noexcept -> size_t
{
    auto const target =
        (elapsed_ns > 0) ? static_cast<uint64_t>((elapsed_ns * static_cast<int64_t>(sample_rate)) / 1'000'000'000LL) : uint64_t{0};
    if (target <= frames_emitted) {
        return 0;
    }
    auto const n = static_cast<size_t>(target - frames_emitted);
    return (n > cap) ? cap : n;
}

}  // namespace statusbar::avb_entity
