// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

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

constexpr int64_t T0 = 1'000'000'000;

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
    gate.note_listener_ready(sid(0x101), false, T0 + 1);
    EXPECT_TRUE(gate.admitted(1, T0 + TalkerGate::GRACE_NS));
    EXPECT_FALSE(gate.admitted(1, T0 + 1 + TalkerGate::GRACE_NS));
    gate.note_stream_started(1, false);
    gate.note_listener_ready(sid(0x101), true, T0 + 2);
    EXPECT_FALSE(gate.admitted(1, T0 + 2));
    EXPECT_TRUE(gate.listener_admitted(1, T0 + 2));
}

TEST(talker_gate, a_disabled_gate_admits_every_stream)
{
    TalkerGate gate{false, lookup};
    EXPECT_TRUE(gate.admitted(0, T0));
    EXPECT_TRUE(gate.should_transmit(3, T0));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_gate_test)
