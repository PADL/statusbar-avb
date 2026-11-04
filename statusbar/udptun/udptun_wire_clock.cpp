// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_wire_clock.hpp"

#include "statusbar/gptp/gptp_time_bridge.hpp"
#include "statusbar/ptpclient/ptpclient_bridge.hpp"

namespace statusbar::udptun {

auto make_gptp_slave_translator(gptp::GptpTimeBridge const& bridge) -> WireClock::Translator
{
    return [&bridge](int64_t mraw_ns) noexcept -> int64_t { return bridge.to_gptp(mraw_ns); };
}

auto make_ptp4l_translator(ptpclient::PtpTimeBridge const& bridge) -> WireClock::Translator
{
    // The bridge's reference clock is CLOCK_MONOTONIC_RAW, the clock the
    // wire timeline runs on, so a RAW instant goes straight through the
    // primary PHC<->RAW line: the translator reads no clock of its own.
    return [&bridge](int64_t mraw_ns) noexcept -> int64_t { return bridge.from_raw_ns(mraw_ns); };
}

}  // namespace statusbar::udptun
