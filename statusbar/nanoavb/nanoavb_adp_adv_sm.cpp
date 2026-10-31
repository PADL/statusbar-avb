// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_adp_adv_sm.hpp"

namespace statusbar::nanoavb::adp_adv_sm {

auto realtime_ns() noexcept -> uint64_t
{
    auto const since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

void DelayRandom::seed(ieee::Eui48 const& mac, uint64_t const realtime_ns) noexcept
{
    uint64_t const a = mac.to_uint64();
    uint64_t sum = a + realtime_ns;
    sum += sum < a ? 1U : 0U;  // end-around carry
    state_ = sum != 0 ? sum : 0x9E37'79B9'7F4A'7C15ULL;
}

auto DelayRandom::next(std::chrono::milliseconds const max) noexcept -> std::chrono::milliseconds
{
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    auto const r = (state_ * 0x2545'F491'4F6C'DD1DULL) >> 32U;
    if (max.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::milliseconds{static_cast<int64_t>(r % (static_cast<uint64_t>(max.count()) + 1U))};
}

void start_startup_delay(Context& ctx, TimePoint const time) noexcept
{
    ctx.advertise_at.reset();
    ctx.delay_until = time + ctx.random.next(ctx.startup_delay_max);
}

void start_delay(Context& ctx, TimePoint const time) noexcept
{
    ctx.advertise_at.reset();
    ctx.delay_until = time + ctx.random.next(ctx.delay_max);
}

void advertise(Context& ctx, TimePoint const time) noexcept
{
    ctx.delay_until.reset();
    ctx.do_advertise = true;
    ctx.advertise_at = time + ctx.advertise_interval;
}

void stop_timers(Context& ctx, TimePoint /*time*/) noexcept
{
    ctx.advertise_at.reset();
    ctx.delay_until.reset();
}

void depart(Context& ctx, TimePoint const time) noexcept
{
    stop_timers(ctx, time);
    ctx.do_terminate = true;
}

}  // namespace statusbar::nanoavb::adp_adv_sm
