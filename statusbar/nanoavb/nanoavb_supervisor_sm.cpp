// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_supervisor_sm.hpp"

#include "statusbar/sm/sm_core.hpp"

#include <functional>
#include <string>

namespace statusbar::nanoavb::supervisor_sm {

void init_iface(Context& ctx, TimePoint time)
{
    if (ctx.callbacks.init_iface) {
        ctx.callbacks.init_iface(ctx, time);
    }
}

void start_protocols(Context& ctx, TimePoint time)
{
    if (ctx.callbacks.start_protocols) {
        ctx.callbacks.start_protocols(ctx, time);
    }
}

void enter_ready(Context& ctx, TimePoint time)
{
    if (ctx.callbacks.enter_ready) {
        ctx.callbacks.enter_ready(ctx, time);
    }
}

void degrade_stop_streams(Context& ctx, TimePoint time)
{
    if (ctx.callbacks.degrade_stop_streams) {
        ctx.callbacks.degrade_stop_streams(ctx, time);
    }
}

void stop_all(Context& ctx, TimePoint time)
{
    if (ctx.callbacks.stop_all) {
        ctx.callbacks.stop_all(ctx, time);
    }
}

void timeout_gptp(Context& ctx, TimePoint time)
{
    // Stopping is handled by the Down entry hook (stop_all runs after this
    // action, preserving the original notify-then-stop order).
    if (ctx.callbacks.timeout_gptp) {
        ctx.callbacks.timeout_gptp(ctx, time);
    }
}

}  // namespace statusbar::nanoavb::supervisor_sm
