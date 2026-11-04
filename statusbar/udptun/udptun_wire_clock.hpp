#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <ctime>

namespace statusbar::gptp {
struct GptpTimeBridge;
}

namespace statusbar::ptpclient {
class PtpTimeBridge;
}

namespace statusbar::udptun {

/// Monotonic clock reading in nanoseconds. The udptun framework uses
/// this as the *internal* timeline for poll-loop scheduling, RX
/// timestamping, and TX-history record times — it never goes onto the
/// wire. Linux uses CLOCK_MONOTONIC_RAW (immune to NTP slewing); other
/// POSIX systems fall back to CLOCK_MONOTONIC, which is good enough for
/// our scheduling and RTT-bookkeeping needs.
[[nodiscard]] inline auto monotonic_raw_ns() noexcept -> int64_t
{
    timespec ts{};
#if defined(CLOCK_MONOTONIC_RAW)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec;
}

/// CLOCK_REALTIME reading in nanoseconds. Used as the fallback wire
/// clock when no master clock is available (`--time-source=realtime`
/// deployments where both endpoints rely on NTP-synced realtime).
[[nodiscard]] inline auto realtime_ns() noexcept -> int64_t
{
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec;
}

/// Translate from CLOCK_MONOTONIC_RAW to the wire timeline. The
/// `Translator` is the customization point: each time source provides
/// its own closure that maps an mraw_ns reading to the wire (master
/// clock) timeline. When the closure is empty, `realtime_ns()` is
/// read directly — the "no master clock" path.
///
/// Concrete translators are produced by `make_gptp_slave_translator()`
/// (offset-only mapping through statusbar's own gPTP slave) and
/// `make_ptp4l_translator()` (regression-based mapping through a
/// ptp4l-fed PHC, with a once-sampled raw↔mono offset adjustment).
struct WireClock
{
    using Translator = statusbar::sg14::inplace_function<int64_t(int64_t mraw_ns), 64>;
    Translator translate{};

    [[nodiscard]] auto wire_ns(int64_t mraw_ns) const noexcept -> int64_t
    {
        if (translate) {
            return translate(mraw_ns);
        }
        return realtime_ns();
    }
};

/// Default TAI − UTC offset in nanoseconds (37 s; the value in force since the
/// 2017-01-01 leap second). Added to CLOCK_REALTIME to form TAI for the
/// inter-site transport timeline. Configurable per deployment — see
/// `make_realtime_tai_translator`.
inline constexpr int64_t DEFAULT_TAI_OFFSET_NS = 37'000'000'000LL;

/// Build a translator whose wire timeline is **TAI** (International Atomic Time),
/// sourced as `CLOCK_REALTIME + tai_offset_ns`. This is the inter-site transport
/// clock: both endpoints read their NTP-disciplined (GPS-locked) CLOCK_REALTIME
/// and add the same constant leap-second offset, so they share an absolute,
/// leap-immune timeline WITHOUT any PTP/PHC involvement (the local gPTP/PHC is
/// untouched). The closure ignores its CLOCK_MONOTONIC_RAW argument and reads
/// realtime fresh on each call, matching the empty-translator (realtime)
/// semantics but shifted into the TAI epoch. `tai_offset_ns` is applied manually
/// (default `DEFAULT_TAI_OFFSET_NS`) rather than read from the kernel tai_offset,
/// so it works regardless of whether a given box's kernel TAI is configured.
[[nodiscard]] inline auto make_realtime_tai_translator(int64_t const tai_offset_ns = DEFAULT_TAI_OFFSET_NS) -> WireClock::Translator
{
    return [tai_offset_ns](int64_t /*mraw_ns*/) noexcept -> int64_t { return realtime_ns() + tai_offset_ns; };
}

/// Build a translator that forwards to `gptp::GptpTimeBridge::to_gptp`
/// (offset-only translation; immune to per-sample rate noise from the
/// gPTP servo's pull-in).
[[nodiscard]] auto make_gptp_slave_translator(gptp::GptpTimeBridge const& bridge) -> WireClock::Translator;

/// Build a translator over a `ptpclient::PtpTimeBridge`. The bridge's
/// reference clock is CLOCK_MONOTONIC_RAW, the clock the wire timeline
/// runs on, so the closure is `bridge.from_raw_ns(mraw_ns)`: the primary
/// PHC<->RAW line, exact, with no clock read of its own.
[[nodiscard]] auto make_ptp4l_translator(ptpclient::PtpTimeBridge const& bridge) -> WireClock::Translator;

}  // namespace statusbar::udptun
