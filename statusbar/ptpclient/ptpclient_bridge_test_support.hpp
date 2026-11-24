#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// For tests of PtpTimeBridge and of what is built on it: a made-up PHC to
/// tell the bridge its readings from (begin_told_sampling / observe), and
/// made-up nanosecond counts as instants of the clocks the bridge names.
/// Nothing here reads a clock.

#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/status/status.hpp"

#include <chrono>
#include <cstdint>

namespace statusbar::ptpclient::test_support {

using statusbar::realtime::GptpClock0;
using statusbar::realtime::MonotonicClock;
using statusbar::realtime::MonotonicRawClock;
using statusbar::realtime::Nanoseconds;
using namespace std::chrono_literals;

/// A made-up nanosecond count as an instant on each clock the bridge names.
inline constexpr auto ptp(int64_t const ns) -> GptpClock0::time_point
{
    return GptpClock0::from_ns(ns);
}
inline constexpr auto raw(int64_t const ns) -> MonotonicRawClock::time_point
{
    return MonotonicRawClock::from_ns(ns);
}
inline constexpr auto mono(int64_t const ns) -> MonotonicClock::time_point
{
    return MonotonicClock::from_ns(ns);
}
/// How far apart two instants of one clock are, either way round.
inline constexpr auto abs_ns(Nanoseconds const d) -> int64_t
{
    return d.count() < 0 ? -d.count() : d.count();
}

/// A made-up PHC, a fixed distance ahead of RAW and running `ppm` fast of
/// it, read every 2 ms, beside a made-up CLOCK_MONOTONIC running `mono_ppm`
/// fast of RAW. Each reading takes `bracket_ns` and catches the PHC at the
/// middle of it.
struct ToldPhc
{
    static constexpr int64_t SAMPLE_INTERVAL_NS = 2'000'000;
    static constexpr int64_t PHC_AHEAD_OF_RAW_NS = 1'000'000'000'000;

    int64_t raw_ns{50'000'000'000};                        ///< RAW at the start of the next reading
    int64_t phc_ns{50'000'000'000 + PHC_AHEAD_OF_RAW_NS};  ///< the PHC at that instant
    int64_t mono_ns{57'000'000'000};                       ///< CLOCK_MONOTONIC at that instant
    int64_t ppm{0};
    int64_t mono_ppm{0};
    int64_t bracket_ns{400};
    bool fails{false};

    /// The next reading, its PHC value @p phc_error_ns off the truth.
    auto next(int64_t phc_error_ns = 0) -> PtpTimeBridge::Reading
    {
        PtpTimeBridge::Reading reading = reading_now(phc_error_ns);
        raw_ns += SAMPLE_INTERVAL_NS;
        phc_ns += SAMPLE_INTERVAL_NS + ((SAMPLE_INTERVAL_NS * ppm) / 1'000'000);
        mono_ns += SAMPLE_INTERVAL_NS + ((SAMPLE_INTERVAL_NS * mono_ppm) / 1'000'000);
        return reading;
    }

    /// A reading that starts at the instant the three clocks stand at.
    [[nodiscard]] auto reading_now(int64_t phc_error_ns) const -> PtpTimeBridge::Reading
    {
        int64_t const after = raw_ns + bracket_ns;
        int64_t const phc_mid = phc_ns + (bracket_ns / 2) + phc_error_ns;
        return {
            .raw_before_phc = raw_ns,
            .phc = fails ? statusbar::StatusValue<int64_t>{statusbar::failure(make_error_code(PtpError::clock_gettime_failed))}
                         : statusbar::StatusValue<int64_t>{phc_mid},
            .raw_after_phc = after,
            .mono = mono_ns + bracket_ns + 100,
            .raw_after_mono = after + 200};
    }

    /// The RAW midpoint of the reading taken last: where its sample sits.
    [[nodiscard]] auto last_sample_raw_ns() const -> int64_t { return raw_ns - SAMPLE_INTERVAL_NS + (bracket_ns / 2); }
};

/// A small window so a test can fill it several times over: fitted from
/// 8 samples on, 32 at most.
inline auto told_params() -> BridgeSamplingParams
{
    BridgeSamplingParams params;
    params.window_size = 32;
    params.min_samples_for_healthy = 8;
    return params;
}

inline auto feed(PtpTimeBridge& bridge, PtpTimeBridge::SamplerState& state, ToldPhc& phc, int readings) -> void
{
    for (int i = 0; i < readings; ++i) {
        (void)bridge.observe(state, phc.next());
    }
}

}  // namespace statusbar::ptpclient::test_support
