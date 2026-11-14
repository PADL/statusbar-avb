// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_maap_sm.hpp"

namespace statusbar::avtp::maap_sm {

auto compare_mac(Eui48 const& our_mac, Eui48 const& received_mac) noexcept -> bool
{
    // B.3.6.4: Octet-wise reverse order comparison.
    // Compare from least-significant octet (index 5) to most-significant (index 0).
    // Return true if our_mac is numerically lower (we win).
    for (int i = 5; i >= 0; --i) {
        uint8_t const ours = our_mac.value[static_cast<size_t>(i)];
        uint8_t const theirs = received_mac.value[static_cast<size_t>(i)];
        if (ours < theirs) {
            return true;
        }
        if (ours > theirs) {
            return false;
        }
    }
    // Equal MACs: we do not win
    return false;
}

void init(Context& ctx, TimePoint /*time*/)
{
    ctx.requested_start = {};
    ctx.requested_count = 0;
    ctx.maap_probe_count = 0;
}

void begin_acquire(Context& ctx, TimePoint time)
{
    // B.3.5.1/B.3.5.9 flattened: generate_address + ReserveAddress!
    // = generate_address + init_maap_probe_count + start probe_timer + sProbe
    ctx.callbacks.generate_address(ctx, time);
    ctx.maap_probe_count = MAAP_PROBE_RETRANSMITS;
    ctx.callbacks.start_probe_timer(ctx, time);
    ctx.callbacks.send_probe(ctx, time);
}

void release(Context& ctx, TimePoint time)
{
    // Stop all timers, return to idle
    ctx.callbacks.stop_probe_timer(ctx, time);
    ctx.callbacks.stop_announce_timer(ctx, time);
}

void restart_probing(Context& ctx, TimePoint time)
{
    // Flattened INITIAL/Restart!: stop timers + generate_address + reserve
    ctx.callbacks.stop_probe_timer(ctx, time);
    ctx.callbacks.stop_announce_timer(ctx, time);
    ctx.callbacks.generate_address(ctx, time);
    ctx.maap_probe_count = MAAP_PROBE_RETRANSMITS;
    ctx.callbacks.start_probe_timer(ctx, time);
    ctx.callbacks.send_probe(ctx, time);
}

void probe_complete(Context& ctx, TimePoint time)
{
    // B.3.5.8 probeCount!: probing succeeded, transition to defending
    ctx.callbacks.stop_probe_timer(ctx, time);
    ctx.callbacks.start_announce_timer(ctx, time);
    ctx.callbacks.send_announce(ctx, time);
}

void probe_tick(Context& ctx, TimePoint time)
{
    // B.3.4.2 probeTimer!: send another probe, decrement count
    ctx.callbacks.start_probe_timer(ctx, time);
    ctx.callbacks.send_probe(ctx, time);
    // dec_maap_probe_count (B.3.6.3): the handler raises ProbeCount in the same
    // tick when this leaves the count at zero.
    if (ctx.maap_probe_count > 0) {
        --ctx.maap_probe_count;
    }
}

void send_defend(Context& ctx, TimePoint time)
{
    // B.3.6.6: Send MAAP_DEFEND using last_received_pdu info
    ctx.callbacks.send_defend(ctx, time);
}

void announce_tick(Context& ctx, TimePoint time)
{
    // B.3.4.1 announceTimer!: periodic announce
    ctx.callbacks.start_announce_timer(ctx, time);
    ctx.callbacks.send_announce(ctx, time);
}

}  // namespace statusbar::avtp::maap_sm
