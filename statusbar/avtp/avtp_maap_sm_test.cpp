// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_maap_sm.hpp"

#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace statusbar::avtp::maap_sm;
using namespace statusbar::avtp;
using namespace statusbar::sm;
using statusbar::ieee::Eui48;

namespace {

/// Helper to build a Context with recording callbacks
struct TestHarness
{
    Context ctx{};
    std::vector<std::string> actions;

    TestHarness()
    {
        ctx.our_mac = Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        ctx.requested_count = 1;

        ctx.callbacks.generate_address = [this](Context& c, TimePoint) {
            actions.push_back("generate_address");
            c.requested_start = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x00, 0x01};
        };
        ctx.callbacks.start_probe_timer = [this](Context&, TimePoint) { actions.push_back("start_probe_timer"); };
        ctx.callbacks.stop_probe_timer = [this](Context&, TimePoint) { actions.push_back("stop_probe_timer"); };
        ctx.callbacks.start_announce_timer = [this](Context&, TimePoint) { actions.push_back("start_announce_timer"); };
        ctx.callbacks.stop_announce_timer = [this](Context&, TimePoint) { actions.push_back("stop_announce_timer"); };
        ctx.callbacks.send_probe = [this](Context&, TimePoint) { actions.push_back("send_probe"); };
        ctx.callbacks.send_defend = [this](Context&, TimePoint) { actions.push_back("send_defend"); };
        ctx.callbacks.send_announce = [this](Context&, TimePoint) { actions.push_back("send_announce"); };
    }
};

/// The instant these tests tell the machine: any will do, none is read from a clock.
constexpr auto now() -> TimePoint
{
    return TimePoint{std::chrono::seconds{100}};
}

}  // namespace

//
// compare_mac tests (B.3.6.4)
//

TEST(maap_sm_compare, our_mac_lower_wins)
{
    // Our MAC is lower in reverse-octet order → we win
    Eui48 const ours{0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    Eui48 const theirs{0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    EXPECT_TRUE(compare_mac(ours, theirs));
}

TEST(maap_sm_compare, our_mac_higher_loses)
{
    Eui48 const ours{0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    Eui48 const theirs{0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    EXPECT_FALSE(compare_mac(ours, theirs));
}

TEST(maap_sm_compare, equal_macs_lose)
{
    Eui48 const mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    EXPECT_FALSE(compare_mac(mac, mac));
}

TEST(maap_sm_compare, reverse_octet_order)
{
    // Reverse-octet comparison: octet[5] is most significant for comparison
    // ours has lower octet[5] → wins, even though octet[0] is higher
    Eui48 const ours{0xFF, 0x00, 0x00, 0x00, 0x00, 0x01};
    Eui48 const theirs{0x01, 0x00, 0x00, 0x00, 0x00, 0x02};
    EXPECT_TRUE(compare_mac(ours, theirs));
}

TEST(maap_sm_compare, reverse_octet_tiebreak)
{
    // Last octets equal, tiebreak on earlier octets (still reverse order)
    Eui48 const ours{0x00, 0x00, 0x00, 0x00, 0x01, 0xAA};
    Eui48 const theirs{0x00, 0x00, 0x00, 0x00, 0x02, 0xAA};
    EXPECT_TRUE(compare_mac(ours, theirs));
}

//
// State machine transition tests
//

TEST(maap_sm_transitions, start_uct_to_initial)
{
    TestHarness h;
    Machine sm;
    EXPECT_EQ(sm.current_state(), Def::State::Start);
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    // UCT fires first (Start→Initial), then Begin fires (Initial→Probe)
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
}

TEST(maap_sm_transitions, begin_from_initial)
{
    TestHarness h;
    Machine sm;
    // Get to Initial via a no-op event that triggers UCT
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);

    // Verify begin_acquire actions
    EXPECT_TRUE(h.actions.size() >= 3);
    // Should have: generate_address, start_probe_timer, send_probe
    bool found_generate = false;
    bool found_start_probe = false;
    bool found_send_probe = false;
    for (auto const& a : h.actions) {
        if (a == "generate_address") {
            found_generate = true;
        }
        if (a == "start_probe_timer") {
            found_start_probe = true;
        }
        if (a == "send_probe") {
            found_send_probe = true;
        }
    }
    EXPECT_TRUE(found_generate);
    EXPECT_TRUE(found_start_probe);
    EXPECT_TRUE(found_send_probe);
    EXPECT_EQ(h.ctx.maap_probe_count, MAAP_PROBE_RETRANSMITS);
}

TEST(maap_sm_transitions, release_from_probe)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::Release, now());
    EXPECT_EQ(sm.current_state(), Def::State::Initial);
    // Should stop timers
    bool found_stop_probe = false;
    bool found_stop_announce = false;
    for (auto const& a : h.actions) {
        if (a == "stop_probe_timer") {
            found_stop_probe = true;
        }
        if (a == "stop_announce_timer") {
            found_stop_announce = true;
        }
    }
    EXPECT_TRUE(found_stop_probe);
    EXPECT_TRUE(found_stop_announce);
}

TEST(maap_sm_transitions, probe_timer_tick)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    EXPECT_EQ(h.ctx.maap_probe_count, MAAP_PROBE_RETRANSMITS);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    EXPECT_EQ(h.ctx.maap_probe_count, MAAP_PROBE_RETRANSMITS - 1);

    bool found_start = false;
    bool found_send = false;
    for (auto const& a : h.actions) {
        if (a == "start_probe_timer") {
            found_start = true;
        }
        if (a == "send_probe") {
            found_send = true;
        }
    }
    EXPECT_TRUE(found_start);
    EXPECT_TRUE(found_send);
}

TEST(maap_sm_transitions, probe_count_to_defend)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());

    // Tick down probe count
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    EXPECT_EQ(h.ctx.maap_probe_count, 0u);

    // Component detects count==0 and fires ProbeCount
    h.actions.clear();
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);

    bool found_stop_probe = false;
    bool found_start_announce = false;
    bool found_send_announce = false;
    for (auto const& a : h.actions) {
        if (a == "stop_probe_timer") {
            found_stop_probe = true;
        }
        if (a == "start_announce_timer") {
            found_start_announce = true;
        }
        if (a == "send_announce") {
            found_send_announce = true;
        }
    }
    EXPECT_TRUE(found_stop_probe);
    EXPECT_TRUE(found_start_announce);
    EXPECT_TRUE(found_send_announce);
}

TEST(maap_sm_transitions, announce_timer_in_defend)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::AnnounceTimer, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    bool found_start = false;
    bool found_send = false;
    for (auto const& a : h.actions) {
        if (a == "start_announce_timer") {
            found_start = true;
        }
        if (a == "send_announce") {
            found_send = true;
        }
    }
    EXPECT_TRUE(found_start);
    EXPECT_TRUE(found_send);
}

TEST(maap_sm_transitions, release_from_defend)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::Release, now());
    EXPECT_EQ(sm.current_state(), Def::State::Initial);
}

TEST(maap_sm_transitions, rprobe_in_probe_restarts)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::rProbe, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    // Should have restarted: stop timers, generate, start probe, send probe
    bool found_stop = false;
    bool found_generate = false;
    for (auto const& a : h.actions) {
        if (a == "stop_probe_timer") {
            found_stop = true;
        }
        if (a == "generate_address") {
            found_generate = true;
        }
    }
    EXPECT_TRUE(found_stop);
    EXPECT_TRUE(found_generate);
    EXPECT_EQ(h.ctx.maap_probe_count, MAAP_PROBE_RETRANSMITS);
}

TEST(maap_sm_transitions, rdefend_in_probe_restarts)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::rDefend, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    bool found_generate = false;
    for (auto const& a : h.actions) {
        if (a == "generate_address") {
            found_generate = true;
        }
    }
    EXPECT_TRUE(found_generate);
}

TEST(maap_sm_transitions, rprobe_in_defend_sends_defend)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::rProbe, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    EXPECT_EQ(h.actions.size(), 1u);
    EXPECT_EQ(h.actions[0], "send_defend");
}

TEST(maap_sm_transitions, rdefend_in_defend_restarts)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::rDefend, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    bool found_generate = false;
    for (auto const& a : h.actions) {
        if (a == "generate_address") {
            found_generate = true;
        }
    }
    EXPECT_TRUE(found_generate);
}

TEST(maap_sm_transitions, rannounce_in_defend_restarts)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::rAnnounce, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
}

TEST(maap_sm_transitions, port_operational_from_initial)
{
    TestHarness h;
    Machine sm;
    // Fire a no-op to get past Start UCT → need to trigger UCT manually
    // Just fire PortOperational - UCT will fire first (Start→Initial),
    // then PortOperational fires in Initial → Probe
    sm.handle_event(h.ctx, Def::Event::PortOperational, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
}

TEST(maap_sm_transitions, port_operational_from_probe_restarts)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::PortOperational, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    bool found_generate = false;
    for (auto const& a : h.actions) {
        if (a == "generate_address") {
            found_generate = true;
        }
    }
    EXPECT_TRUE(found_generate);
}

TEST(maap_sm_transitions, port_operational_from_defend_restarts)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::PortOperational, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
}

//
// Ignored events (Table B.7 "-x-" entries)
//

TEST(maap_sm_ignored, begin_in_probe_ignored)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::Begin, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    EXPECT_TRUE(h.actions.empty());
}

TEST(maap_sm_ignored, announce_timer_in_probe_ignored)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::AnnounceTimer, now());
    EXPECT_EQ(sm.current_state(), Def::State::Probe);
    EXPECT_TRUE(h.actions.empty());
}

TEST(maap_sm_ignored, probe_timer_in_defend_ignored)
{
    TestHarness h;
    Machine sm;
    sm.handle_event(h.ctx, Def::Event::Begin, now());
    for (uint32_t i = 0; i < MAAP_PROBE_RETRANSMITS; ++i) {
        sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    }
    sm.handle_event(h.ctx, Def::Event::ProbeCount, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    h.actions.clear();

    sm.handle_event(h.ctx, Def::Event::ProbeTimer, now());
    EXPECT_EQ(sm.current_state(), Def::State::Defend);
    EXPECT_TRUE(h.actions.empty());
}

TEST(maap_sm_ignored, release_in_initial_ignored)
{
    TestHarness h;
    Machine sm;
    // Get to Initial via any event (UCT fires)
    sm.handle_event(h.ctx, Def::Event::Release, now());
    // UCT: Start→Initial, Release in Initial: -x-
    EXPECT_EQ(sm.current_state(), Def::State::Initial);
}

//
// Test Runner
//

TEST_MAIN(statusbar_avtp, avtp_maap_sm_test)
