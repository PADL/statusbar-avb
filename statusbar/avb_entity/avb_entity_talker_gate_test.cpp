// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_ethernet_port.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

auto sid(uint16_t const uid) -> tsn::StreamId
{
    return tsn::StreamId{ieee::Eui48{0x00, 0x1b, 0x21, 0x0a, 0x0b, 0x0c}, uid};
}

/// A caller's own table: StreamID unique id 0x100 + n is talker stream n, for n below 4.
auto lookup(tsn::StreamId const& s) -> std::optional<uint16_t>
{
    auto const uid = s.get_unique_id();
    if (uid < 0x100 || uid >= 0x104) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(uid - 0x100);
}

using namespace std::chrono_literals;
/// An instant on the gate's clock (CLOCK_MONOTONIC_RAW): the reactor stamps it, a media wake is told it.
constexpr auto T0 = TalkerGate::RawTimePoint{100s};
constexpr auto GRACE = TalkerGate::GRACE;

}  // namespace

// The gate finds a stream through the caller's lookup, not an entity's ACMP table, and keeps
// IEEE 802.1Q-2022 Clause 35.2.4.4's rule: transmit while a Ready declaration is registered.
TEST(talker_gate, an_injected_lookup_admits_the_named_stream_only)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x102), true, T0);
    EXPECT_TRUE(gate.admitted(2, T0));
    EXPECT_FALSE(gate.admitted(1, T0));
    EXPECT_FALSE(gate.admitted(3, T0));
}

TEST(talker_gate, a_stream_id_the_lookup_does_not_know_is_ignored)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x200), true, T0);
    for (uint16_t idx = 0; idx < 4; ++idx) {
        EXPECT_FALSE(gate.admitted(idx, T0));
    }
}

TEST(talker_gate, an_index_beyond_the_gate_is_ignored)
{
    TalkerGate gate{true, [](tsn::StreamId const&) { return std::optional<uint16_t>{TalkerGate::STREAM_COUNT}; }};
    gate.note_listener_ready(sid(0x100), true, T0);
    EXPECT_FALSE(gate.admitted(0, T0));
    EXPECT_FALSE(gate.admitted(TalkerGate::STREAM_COUNT, T0));
    EXPECT_FALSE(gate.grace_ends(TalkerGate::STREAM_COUNT).has_value());
}

TEST(talker_gate, without_a_lookup_nothing_is_admitted)
{
    TalkerGate gate{true, TalkerGate::StreamIndexFn{}};
    gate.note_listener_ready(sid(0x100), true, T0);
    EXPECT_FALSE(gate.admitted(0, T0));
}

// The LeaveAll grace (Milan v1.2 Section 5.3.7.3) holds for a looked-up stream as for the kit's.
TEST(talker_gate, a_withdrawn_listener_keeps_the_gate_open_for_the_grace)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x101), true, T0);
    gate.note_listener_ready(sid(0x101), false, T0 + 1ns);
    EXPECT_TRUE(gate.grace_ends(1) == T0 + 1ns + GRACE);
    EXPECT_TRUE(gate.admitted(1, T0 + GRACE));
    EXPECT_FALSE(gate.admitted(1, T0 + 1ns + GRACE));
    gate.note_stream_started(1, false);
    gate.note_listener_ready(sid(0x101), true, T0 + 2ns);
    EXPECT_FALSE(gate.admitted(1, T0 + 2ns));
    EXPECT_TRUE(gate.listener_admitted(1, T0 + 2ns));
}

TEST(talker_gate, a_disabled_gate_admits_every_stream)
{
    TalkerGate gate{false, lookup};
    EXPECT_TRUE(gate.admitted(0, T0));
    EXPECT_TRUE(gate.admitted(3, T0));
}

// Without a time, a Listener Ready and its withdrawal are stamped at the gate's clock's now: the
// stream stays admitted for GRACE after the withdrawal, then closes.
TEST(talker_gate, a_listener_ready_without_a_time_is_stamped_now)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x102), true);
    gate.note_listener_ready(sid(0x102), false);
    auto const now = net::rx_clock_now();
    EXPECT_TRUE(gate.admitted(2, now));
    EXPECT_FALSE(gate.admitted(2, now + 2 * GRACE));
}

// The media side reads the predicate the reactor stamps: one clock, one answer. A Ready
// registered and withdrawn between two media wakes opens the grace from its stamp on both.
TEST(talker_gate, the_media_side_and_the_reactor_agree_on_a_ready_set_and_cleared_between_wakes)
{
    TalkerGate gate{true, lookup};
    EXPECT_FALSE(gate.should_transmit(0, T0));
    gate.note_listener_ready(sid(0x100), true, T0 + 10us);
    gate.note_listener_ready(sid(0x100), false, T0 + 20us);
    for (auto const at : {T0 + 125us, T0 + 20us + GRACE - 1ns}) {
        EXPECT_TRUE(gate.should_transmit(0, at));
        EXPECT_TRUE(gate.admitted(0, at));
    }
    EXPECT_FALSE(gate.should_transmit(0, T0 + 20us + GRACE));
    EXPECT_FALSE(gate.admitted(0, T0 + 20us + GRACE));
}

// A Ready that came and went while the media side was not asked (no wakes while PTP is
// unlocked) is judged by its stamp, not by when the media side looks: closed after the grace.
TEST(talker_gate, a_ready_missed_over_a_long_gap_is_judged_by_its_stamp)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x100), true, T0);
    gate.note_listener_ready(sid(0x100), false, T0 + 1ns);
    EXPECT_TRUE(gate.should_transmit(0, T0 + 4s));
    EXPECT_FALSE(gate.should_transmit(0, T0 + 10s));
    EXPECT_FALSE(gate.admitted(0, T0 + 10s));
}

// A Stopped stream's grace is the stamp's: STOP, a LeaveAll blip, START within the grace, and
// both sides transmit until it runs out.
TEST(talker_gate, a_stream_stopped_through_a_blip_is_admitted_again_within_the_grace)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x101), true, T0);
    gate.note_stream_started(1, false);
    EXPECT_FALSE(gate.should_transmit(1, T0 + 125us));
    gate.note_listener_ready(sid(0x101), false, T0 + 1s);  // the blip
    gate.note_stream_started(1, true);
    EXPECT_TRUE(gate.should_transmit(1, T0 + 2s));
    EXPECT_TRUE(gate.admitted(1, T0 + 2s));
    EXPECT_FALSE(gate.should_transmit(1, T0 + 1s + GRACE));
    EXPECT_FALSE(gate.admitted(1, T0 + 1s + GRACE));
}

// A Talker withdrawal closes the stream at once on both sides; a Ready set and cleared after it
// opens the grace again from its own stamp.
TEST(talker_gate, a_talker_withdrawal_closes_at_once_and_a_later_ready_reopens)
{
    TalkerGate gate{true, lookup};
    gate.note_listener_ready(sid(0x102), true, T0);
    EXPECT_TRUE(gate.should_transmit(2, T0 + 125us));
    gate.note_talker_withdrawn(2);
    EXPECT_FALSE(gate.should_transmit(2, T0 + 250us));
    EXPECT_FALSE(gate.admitted(2, T0 + 250us));
    EXPECT_FALSE(gate.grace_ends(2).has_value());
    gate.note_listener_ready(sid(0x102), true, T0 + 300us);
    gate.note_listener_ready(sid(0x102), false, T0 + 310us);
    EXPECT_TRUE(gate.should_transmit(2, T0 + 375us));
    EXPECT_TRUE(gate.admitted(2, T0 + 375us));
    EXPECT_FALSE(gate.should_transmit(2, T0 + 310us + GRACE));
    gate.note_talker_withdrawn();
    EXPECT_FALSE(gate.should_transmit(2, T0 + 400us));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_gate_test)
