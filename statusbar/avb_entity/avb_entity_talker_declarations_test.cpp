// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerDeclarations: Milan v1.2 Section 4.3.3.1's declare predicate (MAAP
// address && (a probe within 15 s || a Listener registered)) and Table 5.3's
// 2 x LeaveAll hold after a withdrawal.

#include "statusbar/avb_entity/avb_entity_talker_declarations.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar;
using namespace statusbar::avb_entity;
using Action = TalkerDeclarations::Action;

namespace {

constexpr int64_t S = 1'000'000'000;
constexpr int64_t T0 = 1000 * S;  // an arbitrary steady-clock origin

auto params(uint8_t const mac_low = 0x01, uint16_t const vid = 2) -> TalkerDeclarations::Params
{
    return TalkerDeclarations::Params{
        .stream_id = ieee::Eui64{0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00},
        .dest_mac = ieee::Eui48{0x91, 0xE0, 0xF0, 0x00, 0xFE, mac_low},
        .vlan_id = vid};
}

/// A Milan set with stream 0 active and its address valid.
auto milan_ready() -> TalkerDeclarations
{
    TalkerDeclarations d{true};
    d.set_active(true);
    d.note_address_valid(0, true);
    return d;
}

}  // namespace

TEST(talker_declarations, milan_declares_only_after_a_probe)
{
    auto d = milan_ready();
    // A valid address alone is not interest (Section 4.3.3.1 condition 2).
    EXPECT_FALSE(d.wants_declared(0, T0));
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::none);

    d.note_probe(0, T0);
    EXPECT_TRUE(d.probe_window_open(0, T0));
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::declare);
    d.note_declared(0, params());
    EXPECT_TRUE(d.declared(0));
    // Nothing more to do while the window is open, and its end is the deadline.
    EXPECT_TRUE(d.evaluate(0, T0 + (1 * S), params(), false) == Action::none);
    EXPECT_TRUE(d.next_deadline(T0 + (1 * S)) == T0 + TalkerDeclarations::PROBE_WINDOW_NS);
}

TEST(talker_declarations, withdraws_15_s_after_the_last_probe_with_no_listener)
{
    auto d = milan_ready();
    d.note_probe(0, T0);
    d.note_declared(0, params());
    // A later probe restarts the window.
    d.note_probe(0, T0 + (10 * S));
    EXPECT_TRUE(d.evaluate(0, T0 + (20 * S), params(), false) == Action::none);
    EXPECT_TRUE(d.next_deadline(T0 + (20 * S)) == T0 + (25 * S));
    EXPECT_TRUE(d.evaluate(0, T0 + (25 * S) - 1, params(), false) == Action::none);
    EXPECT_TRUE(d.evaluate(0, T0 + (25 * S), params(), false) == Action::withdraw);
    d.note_withdrawn(0, T0 + (25 * S));
    EXPECT_FALSE(d.declared(0));
}

TEST(talker_declarations, a_registered_listener_keeps_it_declared)
{
    auto d = milan_ready();
    d.note_probe(0, T0);
    d.note_declared(0, params());
    d.note_listener_registered(0, true, T0);
    // No probe deadline while a Listener is registered.
    EXPECT_FALSE(d.next_deadline(T0).has_value());
    EXPECT_TRUE(d.evaluate(0, T0 + (60 * S), params(), false) == Action::none);
    // The listener goes after the window: held through the grace (a LeaveAll
    // blip re-registers within it), withdrawn once it has run out.
    d.note_listener_registered(0, false, T0 + (61 * S));
    int64_t const grace_end = T0 + (61 * S) + TalkerDeclarations::LISTENER_GRACE_NS;
    EXPECT_TRUE(d.evaluate(0, T0 + (62 * S), params(), false) == Action::none);
    EXPECT_TRUE(d.next_deadline(T0 + (62 * S)) == grace_end);
    EXPECT_TRUE(d.evaluate(0, grace_end - 1, params(), false) == Action::none);
    EXPECT_TRUE(d.evaluate(0, grace_end, params(), false) == Action::withdraw);
}

TEST(talker_declarations, a_leave_all_blip_does_not_withdraw)
{
    auto d = milan_ready();
    d.note_probe(0, T0);
    d.note_declared(0, params());
    d.note_listener_registered(0, true, T0 + (1 * S));
    // Past the probe window the Listener blips out for 2 s and comes back.
    d.note_listener_registered(0, false, T0 + (20 * S));
    EXPECT_TRUE(d.evaluate(0, T0 + (21 * S), params(), false) == Action::none);
    d.note_listener_registered(0, true, T0 + (22 * S));
    EXPECT_TRUE(d.evaluate(0, T0 + (30 * S), params(), false) == Action::none);
    // A repeated "gone" does not extend the grace.
    d.note_listener_registered(0, false, T0 + (40 * S));
    d.note_listener_registered(0, false, T0 + (44 * S));
    EXPECT_TRUE(d.evaluate(0, T0 + (45 * S), params(), false) == Action::withdraw);
}

TEST(talker_declarations, a_probe_after_the_window_redeclares_the_same_stream_at_once)
{
    auto d = milan_ready();
    d.note_probe(0, T0);
    d.note_declared(0, params());
    d.note_withdrawn(0, T0 + (15 * S));
    // The hold binds only different parameters.
    d.note_probe(0, T0 + (16 * S));
    EXPECT_TRUE(d.evaluate(0, T0 + (16 * S), params(), false) == Action::declare);
}

TEST(talker_declarations, maap_conflict_withdraws_and_holds_two_leave_alls)
{
    auto d = milan_ready();
    d.note_listener_registered(0, true, T0);
    EXPECT_TRUE(d.evaluate(0, T0, params(0x01), false) == Action::declare);
    d.note_declared(0, params(0x01));

    // Conflict: the address is invalid, the attribute withdrawn.
    d.note_address_valid(0, false);
    EXPECT_TRUE(d.evaluate(0, T0 + (1 * S), params(0x01), false) == Action::withdraw);
    d.note_withdrawn(0, T0 + (1 * S));

    // MAAP defends a new block a second later: held for 2 x LeaveAll.
    d.note_address_valid(0, true);
    int64_t const hold_end = T0 + (1 * S) + TalkerDeclarations::REDECLARE_HOLD_NS;
    EXPECT_TRUE(d.evaluate(0, T0 + (2 * S), params(0x02), false) == Action::none);
    EXPECT_TRUE(d.next_deadline(T0 + (2 * S)) == hold_end);
    EXPECT_TRUE(d.evaluate(0, hold_end - 1, params(0x02), false) == Action::none);
    EXPECT_TRUE(d.evaluate(0, hold_end, params(0x02), false) == Action::declare);
}

TEST(talker_declarations, new_parameters_while_declared_withdraw_first)
{
    auto d = milan_ready();
    d.note_listener_registered(0, true, T0);
    d.note_declared(0, params(0x01));
    // A different address with no conflict seen: withdrawn, then held.
    EXPECT_TRUE(d.evaluate(0, T0, params(0x02), false) == Action::withdraw);
    d.note_withdrawn(0, T0);
    EXPECT_TRUE(d.evaluate(0, T0 + (1 * S), params(0x02), false) == Action::none);
    // The control plane's own change (the adopted VID) re-declares at once.
    auto r = milan_ready();
    r.note_listener_registered(0, true, T0);
    r.note_declared(0, params(0x01, 2));
    EXPECT_TRUE(r.evaluate(0, T0, params(0x01, 3), true) == Action::declare);
    // A refresh re-asserts an unchanged declaration too.
    EXPECT_TRUE(r.evaluate(0, T0, params(0x01, 2), true) == Action::declare);
}

TEST(talker_declarations, inactive_control_plane_declares_nothing)
{
    TalkerDeclarations d{true};
    d.note_address_valid(0, true);
    d.note_probe(0, T0);
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::none);
    d.set_active(true);
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::declare);
    d.note_declared(0, params());
    // Everything withdrawn (Degraded): held as a withdrawal, the same stream
    // back at once when active again.
    d.set_active(false);
    d.note_all_withdrawn(T0 + (1 * S));
    EXPECT_FALSE(d.declared(0));
    EXPECT_TRUE(d.evaluate(0, T0 + (2 * S), params(), false) == Action::none);
    d.set_active(true);
    EXPECT_TRUE(d.evaluate(0, T0 + (2 * S), params(), false) == Action::declare);
}

TEST(talker_declarations, classic_declares_on_a_valid_address)
{
    TalkerDeclarations d{false};
    d.set_active(true);
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::none);
    d.note_address_valid(0, true);
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::declare);
    d.note_declared(0, params());
    // No probe window to run out.
    EXPECT_FALSE(d.next_deadline(T0).has_value());
    EXPECT_TRUE(d.evaluate(0, T0 + (100 * S), params(), false) == Action::none);
}

TEST(talker_declarations, classic_and_a_new_vid_are_not_held)
{
    // Classic: a conflict's new address is declared at once (no hold).
    TalkerDeclarations c{false};
    c.set_active(true);
    c.note_address_valid(0, true);
    c.note_declared(0, params(0x01));
    c.note_address_valid(0, false);
    EXPECT_TRUE(c.evaluate(0, T0, params(0x01), false) == Action::withdraw);
    c.note_withdrawn(0, T0);
    c.note_address_valid(0, true);
    EXPECT_TRUE(c.evaluate(0, T0 + (1 * S), params(0x02), false) == Action::declare);
    // ... and a changed address while declared re-declares.
    c.note_declared(0, params(0x02));
    EXPECT_TRUE(c.evaluate(0, T0 + (2 * S), params(0x03), false) == Action::declare);

    // Milan: withdrawn (the control plane went down), then a new Class A VID
    // adopted: declared at once when active again.
    auto m = milan_ready();
    m.note_listener_registered(0, true, T0);
    m.note_declared(0, params(0x01, 2));
    m.set_active(false);
    m.note_all_withdrawn(T0);
    m.set_active(true);
    EXPECT_TRUE(m.evaluate(0, T0 + (1 * S), params(0x01, 3), true) == Action::declare);
}

TEST(talker_declarations, streams_are_independent)
{
    auto d = milan_ready();
    d.note_address_valid(1, true);
    d.note_probe(1, T0);
    EXPECT_TRUE(d.evaluate(0, T0, params(), false) == Action::none);
    EXPECT_TRUE(d.evaluate(1, T0, params(), false) == Action::declare);
    EXPECT_TRUE(d.evaluate(TalkerDeclarations::STREAM_COUNT, T0, params(), false) == Action::none);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_declarations_test)
