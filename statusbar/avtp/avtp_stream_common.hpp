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

/// A time on a stream's timeline: a count of nanoseconds since that timeline's
/// zero. AVTP does not say which clock it is. It is gPTP time on an AVB
/// network, and it may as well be another gPTP domain's time or a time that
/// came from somewhere else; the caller knows, and hands the count over.
///
/// The count is never below zero. It is held in a signed 64-bit type all the
/// same, so that the difference of two times is a plain subtraction and no
/// caller has to cast on the way in; 63 bits of nanoseconds are 292 years. A
/// negative time handed to a function here is the caller's error, and what the
/// function then does is not specified. The type is the one core's
/// realtime::Nanoseconds names, spelled out here so that avtp needs nothing
/// from the realtime module.
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

/// The same reconstruction on a Nanoseconds time. It returns what the unsigned
/// form returns for every field and every time: the time nearest to @p now
/// whose low 32 bits are @p avtp_timestamp, and never a time below zero. In
/// the first seconds of a timeline the result therefore has its upper 32 bits
/// clear, as the field of a talker that has just started stands for a small
/// time.
///
/// What the caller owes: @p now is not below zero, and is expected to be within
/// one second of the time the field stands for. A listener's receive time and a
/// talker's presentation time are a few milliseconds apart, so that is met with
/// a great deal to spare. The answer is exact for any time less than half a
/// wrap (2.147 s) from @p now; a time further away than that cannot be told
/// from a nearer one with the same low bits, and the nearer one is returned.
///
/// @param avtp_timestamp The 32-bit AVTP presentation timestamp from the AVTPDU
/// @param now            Approximate time on the stream's timeline when the packet was received
/// @return The full presentation time
[[nodiscard]] auto reconstruct_avtp_timestamp(uint32_t avtp_timestamp, Nanoseconds now) noexcept -> Nanoseconds;

}  // namespace statusbar::avtp
