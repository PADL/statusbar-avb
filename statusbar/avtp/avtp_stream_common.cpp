// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_stream_common.hpp"

#include <cstdint>

namespace statusbar::avtp {

namespace {
/// The 32-bit timestamp field counts nanoseconds and wraps after this many.
constexpr int64_t FIELD_WRAP_NS = int64_t{1} << 32;
}  // namespace

auto reconstruct_avtp_timestamp(uint32_t avtp_timestamp, uint64_t gptp_now_ns) noexcept -> uint64_t
{
    // The AVTP timestamp carries the lower 32 bits of the gPTP time in nanoseconds.
    // We reconstruct the full 64-bit value by taking the upper 32 bits from the
    // approximate receive time and the lower 32 bits from the AVTP field.
    //
    // The presentation time is typically slightly ahead of "now" (within a few ms),
    // so we pick the reconstruction closest to gptp_now_ns.
    uint64_t const upper = gptp_now_ns & 0xFFFF'FFFF'0000'0000ULL;
    uint64_t const candidate = upper | avtp_timestamp;

    // Check if wrapping the upper bits by +1 or -1 gives a closer result.
    constexpr uint64_t wrap = 0x1'0000'0000ULL;  // 2^32

    uint64_t best = candidate;
    int64_t best_dist = static_cast<int64_t>(candidate) - static_cast<int64_t>(gptp_now_ns);
    if (best_dist < 0) {
        best_dist = -best_dist;
    }

    // Try candidate + 2^32 (avtp_timestamp is in the next upper-32-bit epoch)
    if (candidate + wrap > candidate) {  // overflow guard
        int64_t const dist_plus = static_cast<int64_t>(candidate + wrap) - static_cast<int64_t>(gptp_now_ns);
        int64_t const abs_dist_plus = dist_plus < 0 ? -dist_plus : dist_plus;
        if (abs_dist_plus < best_dist) {
            best = candidate + wrap;
            best_dist = abs_dist_plus;
        }
    }

    // Try candidate - 2^32 (avtp_timestamp is in the previous upper-32-bit epoch)
    if (candidate >= wrap) {
        int64_t const dist_minus = static_cast<int64_t>(candidate - wrap) - static_cast<int64_t>(gptp_now_ns);
        int64_t const abs_dist_minus = dist_minus < 0 ? -dist_minus : dist_minus;
        if (abs_dist_minus < best_dist) {
            best = candidate - wrap;
        }
    }

    return best;
}

auto reconstruct_avtp_timestamp(uint32_t const avtp_timestamp, Nanoseconds const now) noexcept -> Nanoseconds
{
    // The field is the low 32 bits of the time, so the time nearest to now
    // that carries it is now plus the signed 32-bit step from now's own low
    // bits to the field. Exactly half a wrap away two times are as near; the
    // one that shares now's upper bits is kept, as the unsigned form keeps it.
    auto const now_bits = static_cast<uint64_t>(now.count());
    auto const now_low = static_cast<uint32_t>(now_bits);
    int64_t step = static_cast<int32_t>(avtp_timestamp - now_low);
    if (step == INT32_MIN && avtp_timestamp > now_low) {
        step = -step;
    }
    // A stream time is never below zero. In the first half wrap of a timeline
    // the nearest value can be, for a field too far ahead to be a true one;
    // the answer is then the field as it stands, the same value a wrap later.
    auto const nearest = static_cast<int64_t>(now_bits + static_cast<uint64_t>(step));
    return Nanoseconds{nearest < 0 ? nearest + FIELD_WRAP_NS : nearest};
}

}  // namespace statusbar::avtp
