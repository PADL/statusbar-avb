#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// For tests of GptpTimeBridge and of what is built on it: the bridge's typed
/// conversions on made-up nanosecond counts. A test writes its instants as
/// plain numbers; these give each number its clock on the way in and take it
/// off on the way out, so the test still reads as arithmetic.

#include "statusbar/gptp/gptp_time_bridge.hpp"
#include "statusbar/realtime/realtime_clock.hpp"

#include <cstdint>

namespace statusbar::gptp::test_support {

/// One Sync pairing: RAW read @p raw_ns when the master said @p master_ns.
inline auto anchor(GptpTimeBridge& bridge, int64_t const raw_ns, int64_t const master_ns) -> void
{
    bridge.update_from_anchor(realtime::MonotonicRawClock::from_ns(raw_ns), realtime::GptpClock0::from_ns(master_ns));
}

[[nodiscard]] inline auto to_gptp_ns(GptpTimeBridge const& bridge, int64_t const raw_ns) -> int64_t
{
    return realtime::GptpClock0::to_ns(bridge.to_gptp(realtime::MonotonicRawClock::from_ns(raw_ns)));
}

[[nodiscard]] inline auto from_gptp_ns(GptpTimeBridge const& bridge, int64_t const gptp_ns) -> int64_t
{
    return realtime::MonotonicRawClock::to_ns(bridge.from_gptp(realtime::GptpClock0::from_ns(gptp_ns)));
}

[[nodiscard]] inline auto to_gptp_rated_ns(GptpTimeBridge const& bridge, int64_t const raw_ns) -> int64_t
{
    return realtime::GptpClock0::to_ns(bridge.to_gptp_rated(realtime::MonotonicRawClock::from_ns(raw_ns)));
}

[[nodiscard]] inline auto from_gptp_rated_ns(GptpTimeBridge const& bridge, int64_t const gptp_ns) -> int64_t
{
    return realtime::MonotonicRawClock::to_ns(bridge.from_gptp_rated(realtime::GptpClock0::from_ns(gptp_ns)));
}

}  // namespace statusbar::gptp::test_support
