#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared AVTP stream utilities — timestamp reconstruction common to AM824 and AAF
///
/// The AVTP timestamp field (IEEE 1722-2016 Section 4.4.4) carries only the lower
/// 32 bits of the gPTP time in nanoseconds. Both AM824 and AAF streams need to
/// reconstruct the full 64-bit gPTP-domain value using an approximate receive time.

#include <chrono>
#include <cstdint>
#include <ratio>

namespace statusbar::avtp {

/// A time on a stream's timeline: a signed 64-bit count of nanoseconds since
/// that timeline's own zero. AVTP does not say which clock it is. It is gPTP
/// time on an AVB network, and it may as well be another gPTP domain's time or
/// a time that came from somewhere else; the caller knows, and hands the
/// count over. Signed, so a difference is a plain subtraction and a timeline
/// that starts near zero has room below it; 63 bits of nanoseconds are 292
/// years. The type is the one core's realtime::Nanoseconds names, spelled out
/// here so that avtp needs nothing from the realtime module.
using Nanoseconds = std::chrono::duration<int64_t, std::nano>;

/// Reconstruct a full 64-bit gPTP timestamp from a 32-bit AVTP timestamp
/// and an approximate current gPTP time.
///
/// The AVTP timestamp wraps every ~4.295 seconds. To reconstruct the full
/// 64-bit value, we combine the upper 32 bits from the approximate receive
/// time with the lower 32 bits from the AVTP field, selecting the candidate
/// closest to gptp_now_ns to handle wrap-around in both directions.
///
/// @param avtp_timestamp The 32-bit AVTP presentation timestamp from the AVTPDU
/// @param gptp_now_ns    Approximate gPTP time when the packet was received (full 64-bit)
/// @return Full 64-bit gPTP-domain presentation timestamp
[[nodiscard]] auto reconstruct_avtp_timestamp(uint32_t avtp_timestamp, uint64_t gptp_now_ns) noexcept -> uint64_t;

/// The same reconstruction on the signed time: the time nearest to @p now whose
/// low 32 bits are @p avtp_timestamp. It agrees with the unsigned form wherever
/// that one has an answer, and it also has one below zero: told a time less
/// than one wrap after the timeline's start, a field from just before the
/// start reconstructs to a small negative time, where the unsigned form can
/// only answer with a time a whole wrap too late.
///
/// @param avtp_timestamp The 32-bit AVTP presentation timestamp from the AVTPDU
/// @param now            Approximate time on the stream's timeline when the packet was received
/// @return The full presentation time
[[nodiscard]] auto reconstruct_avtp_timestamp(uint32_t avtp_timestamp, Nanoseconds now) noexcept -> Nanoseconds;

}  // namespace statusbar::avtp
