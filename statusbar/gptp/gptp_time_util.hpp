#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared gPTP time utilities used by multiple translation units.

#include "statusbar/gptp/gptp_base.hpp"

#include <algorithm>
#include <cstdint>

namespace statusbar::gptp {

/// Valid gPTP logMessageInterval range (IEEE 802.1AS-2020 Clause 10.6.2.1).
inline constexpr int8_t k_log_interval_min = -7;
inline constexpr int8_t k_log_interval_max = 7;

/// Clamp a log2 message-interval exponent to the valid range. Config values are
/// pre-validated to this range, but a value taken from a received message header
/// (e.g. a Sync's logMessageInterval) is NOT -- an out-of-range exponent would
/// push the pow()/int64 conversion into undefined behaviour (large positive) or
/// arm a zero-length timeout (large negative). Clamp before converting.
[[nodiscard]] constexpr auto clamp_log_interval(int8_t log_interval) noexcept -> int8_t
{
    return std::clamp<int8_t>(log_interval, k_log_interval_min, k_log_interval_max);
}

/// Convert a gptp::Timestamp (6 byte seconds + 4 byte nanos) to a
/// signed int64 nanoseconds value. Only differences between timestamps
/// matter for rate ratio math. For realistic PTP timestamps (post-2000
/// TAI) this is ~2^30 seconds x 10^9 ≈ 2^60 ns, well within int64 range.
inline auto ts_to_ns(Timestamp const& ts) noexcept -> int64_t
{
    int64_t const secs = static_cast<int64_t>(ts.seconds());
    int64_t const ns = static_cast<int64_t>(ts.nanos());
    return (secs * 1'000'000'000LL) + ns;
}

}  // namespace statusbar::gptp
