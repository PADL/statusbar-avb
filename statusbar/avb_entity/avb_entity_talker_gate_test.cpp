// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// TalkerGate: one Listener Ready stamp per stream on CLOCK_MONOTONIC_RAW, written by
/// the reactor and read by any thread told a RAW instant, so the media side and the
/// reactor agree on when a stream is admitted and when its grace runs out.

#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

using namespace statusbar;
using namespace statusbar::avb_entity;
using namespace std::chrono_literals;

namespace {

constexpr uint16_t STREAMS = 4;
/// An instant on the gate's clock: the reactor stamps it, a media wake is told it.
constexpr auto T0 = TalkerGate::RawTimePoint{100s};
constexpr auto GRACE = TalkerGate::GRACE;

/// Talker stream @p idx's StreamID in components_with_talkers().
auto stream_id(uint16_t const idx) -> ieee::Eui64
{
    return ieee::Eui64{0x00, 0x1b, 0x21, 0x0a, 0x0b, 0x0c, 0x01, static_cast<uint8_t>(idx)};
}

auto sid(uint16_t const idx) -> nanoavb::StreamId
{
    nanoavb::StreamId s{};
    s.from_uint64(stream_id(idx).to_uint64());
    return s;
}

/// Components whose ACMP talker has STREAMS stream outputs, stream n declared as stream_id(n).
auto components_with_talkers() -> std::unique_ptr<nanoavb::NanoAvbComponents>
{
    nanoavb::EntityModel model{nanoavb::EntityModelConfig{.max_configurations = 1, .max_stream_outputs = STREAMS}};
    atdecc::aem::DescriptorEntity entity{};
    entity.entity_id = ieee::Eui64{0x00, 0x1b, 0x21, 0x0a, 0x0b, 0x0c, 0x00, 0x01};
    model.set_entity(entity);
    auto built = nanoavb::NanoAvbComponentsBuilder{}
                     .with_entity_model(std::move(model))
                     .with_acmp_talker_streams(STREAMS, 4)
                     .build(sm::TimePoint{});
    EXPECT_TRUE(built.has_value());
    for (uint16_t idx = 0; idx < STREAMS; ++idx) {
        EXPECT_TRUE(
            (*built)->configure_talker_stream(idx, stream_id(idx), ieee::Eui48{0x91, 0xe0, 0xf0, 0x00, 0x0e, 0x80}, 2).has_value());
    }
    return std::move(*built);
}

/// A gate that admits on the MSRP Listener alone, as a Milan talker's does, over its own components.
struct MsrpGate
{
    std::unique_ptr<nanoavb::NanoAvbComponents> components{components_with_talkers()};
    TalkerGate gate{true, *components};

    MsrpGate() noexcept { gate.set_needs_acmp_connection(false); }
};

}  // namespace

// IEEE 1722.1-2021 Clause 8.2.1: an IEEE 1722.1 talker stream is admitted only while it has an ACMP
// connection and its Listener Ready is registered; set_needs_acmp_connection(false) leaves MSRP alone.
TEST(talker_gate, a_stream_needs_an_acmp_connection_by_default)
{
    auto const components = components_with_talkers();
    TalkerGate gate{true, *components};
    gate.note_listener_ready(sid(1), true, T0);
    EXPECT_FALSE(gate.admitted(1, T0));
    gate.note_acmp_connections(1, 1);
    EXPECT_TRUE(gate.admitted(1, T0));
    EXPECT_FALSE(gate.admitted(2, T0));
    gate.note_acmp_connections(1, 0);
    EXPECT_FALSE(gate.admitted(1, T0));
    gate.set_needs_acmp_connection(false);
    EXPECT_TRUE(gate.admitted(1, T0));
}

TEST(talker_gate, a_disabled_gate_admits_every_stream)
{
    auto const components = components_with_talkers();
    TalkerGate const gate{false, *components};
    EXPECT_TRUE(gate.admitted(0, T0));
    EXPECT_TRUE(gate.should_transmit(3, T0));
}

// IEEE 802.1Q 35.2.2.8: a stream whose changed Talker Advertise is waiting out
// the value it replaces has no attribute on the wire. It is not admitted,
// whatever else holds, and a disabled gate does not let it through either.
TEST(talker_gate, a_stream_whose_advertise_is_held_back_is_not_admitted_with_the_gate_disabled_too)
{
    MsrpGate msrp{};
    msrp.gate.note_listener_ready(sid(1), true, T0);
    EXPECT_TRUE(msrp.gate.admitted(1, T0));
    EXPECT_TRUE(msrp.gate.note_advertise_held(1, true));
    EXPECT_FALSE(msrp.gate.note_advertise_held(1, true));  // told again: no change
    EXPECT_FALSE(msrp.gate.admitted(1, T0));
    EXPECT_FALSE(msrp.gate.should_transmit(1, T0));
    EXPECT_TRUE(msrp.gate.note_advertise_held(1, false));
    EXPECT_TRUE(msrp.gate.admitted(1, T0));

    auto const components = components_with_talkers();
    TalkerGate open{false, *components};
    EXPECT_TRUE(open.note_advertise_held(2, true));
    EXPECT_FALSE(open.admitted(2, T0));
    EXPECT_TRUE(open.admitted(1, T0));  // the stream beside it is not held
    EXPECT_FALSE(open.note_advertise_held(static_cast<uint16_t>(TalkerGate::STREAM_COUNT), true));
}

TEST(talker_gate, an_index_beyond_the_gate_is_ignored)
{
    MsrpGate const msrp;
    auto const& gate = msrp.gate;
    EXPECT_FALSE(gate.admitted(TalkerGate::STREAM_COUNT, T0));
    EXPECT_FALSE(gate.grace_ends(TalkerGate::STREAM_COUNT).has_value());
}

// A withdrawn Listener Ready keeps the stream admitted for the grace from the withdrawal's stamp,
// which grace_ends() gives; there is no grace while the Listener is registered.
TEST(talker_gate, the_grace_runs_out_at_grace_ends)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    EXPECT_FALSE(gate.grace_ends(1).has_value());  // never registered
    gate.note_listener_ready(sid(1), true, T0);
    EXPECT_FALSE(gate.grace_ends(1).has_value());  // registered
    gate.note_listener_ready(sid(1), false, T0 + 1s);
    EXPECT_TRUE(gate.grace_ends(1) == T0 + 1s + GRACE);
    EXPECT_TRUE(gate.admitted(1, T0 + 1s + GRACE - 1ns));
    EXPECT_FALSE(gate.admitted(1, T0 + 1s + GRACE));
    gate.note_listener_ready(sid(1), false, T0 + 2s);  // withdrawn again: the grace is not extended
    EXPECT_TRUE(gate.grace_ends(1) == T0 + 1s + GRACE);
}

// The media side reads the predicate the reactor stamps: a Ready registered and withdrawn between
// two media wakes opens the grace from its stamp on both.
TEST(talker_gate, both_sides_agree_on_a_ready_set_and_cleared_between_wakes)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    EXPECT_FALSE(gate.should_transmit(0, T0));
    gate.note_listener_ready(sid(0), true, T0 + 10us);
    gate.note_listener_ready(sid(0), false, T0 + 20us);
    for (auto const at : {T0 + 125us, T0 + 20us + GRACE - 1ns}) {
        EXPECT_TRUE(gate.should_transmit(0, at));
        EXPECT_TRUE(gate.admitted(0, at));
    }
    EXPECT_FALSE(gate.should_transmit(0, T0 + 20us + GRACE));
    EXPECT_FALSE(gate.admitted(0, T0 + 20us + GRACE));
}

// A Ready that came and went while the media side was not asked (no wakes while PTP is unlocked)
// is checked against its stamp, not against when the media side looks.
TEST(talker_gate, a_ready_missed_over_a_long_gap_is_checked_against_its_stamp)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    gate.note_listener_ready(sid(0), true, T0);
    gate.note_listener_ready(sid(0), false, T0 + 1ns);
    EXPECT_TRUE(gate.should_transmit(0, T0 + 4s));
    EXPECT_FALSE(gate.should_transmit(0, T0 + 10s));
    EXPECT_FALSE(gate.admitted(0, T0 + 10s));
}

// A withdrawal whose grace has run out, then a Ready: admitted again at once, and a second
// withdrawal opens the grace from its own stamp on both sides.
TEST(talker_gate, a_withdrawal_then_a_ready_reopens_from_the_new_stamp)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    gate.note_listener_ready(sid(2), true, T0);
    gate.note_listener_ready(sid(2), false, T0 + 1s);
    EXPECT_FALSE(gate.should_transmit(2, T0 + 1s + GRACE));
    gate.note_listener_ready(sid(2), true, T0 + 1s + GRACE + 300us);
    EXPECT_TRUE(gate.should_transmit(2, T0 + 1s + GRACE + 375us));
    EXPECT_FALSE(gate.grace_ends(2).has_value());
    gate.note_listener_ready(sid(2), false, T0 + 1s + GRACE + 400us);
    EXPECT_TRUE(gate.grace_ends(2) == T0 + 1s + 2 * GRACE + 400us);
    EXPECT_TRUE(gate.should_transmit(2, T0 + 1s + 2 * GRACE + 399us));
    EXPECT_TRUE(gate.admitted(2, T0 + 1s + 2 * GRACE + 399us));
    EXPECT_FALSE(gate.should_transmit(2, T0 + 1s + 2 * GRACE + 400us));
}

// Milan v1.3 Section 4.2.7.2.2: the registrar holds a Listener through a LeaveAll for LeaveTime,
// so a gate with no grace closes the moment its Listener is withdrawn, whatever instant it is told.
TEST(talker_gate, a_gate_with_no_grace_closes_at_the_withdrawal)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    gate.set_listener_grace(0ns);
    gate.note_listener_ready(sid(1), true, T0);
    EXPECT_TRUE(gate.should_transmit(1, T0 + 1s));
    gate.note_listener_ready(sid(1), false, T0 + 1s);
    EXPECT_FALSE(gate.should_transmit(1, T0 + 1s));
    EXPECT_FALSE(gate.admitted(1, T0 + 1s - 1ns));
    EXPECT_TRUE(gate.grace_ends(1) == T0 + 1s);
}

// A Stopped stream is not admitted however its Listener stands; Started again within the grace,
// it is admitted until the grace runs out.
TEST(talker_gate, a_stream_stopped_through_a_blip_is_admitted_again_within_the_grace)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    gate.note_listener_ready(sid(1), true, T0);
    gate.note_stream_started(1, false);
    EXPECT_FALSE(gate.should_transmit(1, T0 + 125us));
    gate.note_listener_ready(sid(1), false, T0 + 1s);
    gate.note_stream_started(1, true);
    EXPECT_TRUE(gate.should_transmit(1, T0 + 2s));
    EXPECT_FALSE(gate.should_transmit(1, T0 + 1s + GRACE));
}

// The gate reads no clock. A Listener Ready noted without an instant, as an MSRP hook notes it, is
// known at the instant the loop last told the gate: the reactor cycle's.
TEST(talker_gate, a_listener_ready_without_an_instant_is_stamped_with_the_time_told)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    EXPECT_TRUE(gate.told_time() == TalkerGate::RawTimePoint{});
    gate.tell_time(T0);
    gate.note_listener_ready(sid(3), true);
    gate.tell_time(T0 + 3s);  // a later cycle
    EXPECT_TRUE(gate.told_time() == T0 + 3s);
    gate.note_listener_ready(sid(3), false);
    EXPECT_TRUE(gate.grace_ends(3) == T0 + 3s + GRACE);
    EXPECT_TRUE(gate.should_transmit(3, T0 + 3s + GRACE - 1ns));
    EXPECT_FALSE(gate.should_transmit(3, T0 + 3s + GRACE));
    gate.tell_time(T0 + 60s);  // telling a time stamps nothing by itself
    EXPECT_TRUE(gate.grace_ends(3) == T0 + 3s + GRACE);
}

// Told no time, the gate has no instant to stamp: the registration alone admits, and its
// withdrawal opens no grace. The instant given explicitly is used whatever was told.
TEST(talker_gate, told_no_time_a_withdrawal_opens_no_grace)
{
    MsrpGate msrp;
    auto& gate = msrp.gate;
    gate.note_listener_ready(sid(2), true);
    EXPECT_TRUE(gate.should_transmit(2, T0));
    gate.note_listener_ready(sid(2), false);
    EXPECT_FALSE(gate.grace_ends(2).has_value());
    EXPECT_FALSE(gate.should_transmit(2, T0));
    EXPECT_FALSE(gate.should_transmit(2, TalkerGate::RawTimePoint{1ns}));

    gate.note_listener_ready(sid(2), true, T0);
    gate.note_listener_ready(sid(2), false, T0 + 1s);
    EXPECT_TRUE(gate.grace_ends(2) == T0 + 1s + GRACE);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_gate_test)
