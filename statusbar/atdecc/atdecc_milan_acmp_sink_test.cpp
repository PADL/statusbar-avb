// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// One Milan sink with its two machines run together (MilanSink): what
// arrives becomes the right event, discovery tells the connection machine,
// and the timers fall due on the time the sink is told.

#include "statusbar/atdecc/atdecc_milan_acmp_sink.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using TimePoint = statusbar::sm::TimePoint;
using State = MilanSinkState;

namespace {

constexpr Eui64 LISTENER{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr Eui64 TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr Eui64 OTHER_TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x22};
constexpr Eui64 CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
constexpr Eui64 STREAM{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x05};
constexpr Eui48 DEST{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
constexpr ClockIdentity GRANDMASTER{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x01};
constexpr MilanGptpState OWN{.grandmaster_id = GRANDMASTER, .domain_number = 0};
constexpr uint16_t SINK = 2;
constexpr uint16_t SOURCE = 1;
constexpr MilanBinding SAVED{
    .listener_unique_id = SINK, .talker_entity_id = TALKER, .talker_unique_id = SOURCE, .controller_entity_id = CONTROLLER};

auto at(int const ms) -> TimePoint
{
    return TimePoint{std::chrono::milliseconds{ms}};
}

auto available(Eui64 const& talker, uint32_t const available_index) -> AdpDu
{
    AdpDu adp{};
    adp.init_entity_available(talker, 5);  // good for 10 seconds
    adp.available_index = available_index;
    adp.gptp_grandmaster_id = GRANDMASTER;
    return adp;
}

auto departing(Eui64 const& talker) -> AdpDu
{
    AdpDu adp{};
    adp.init_entity_departing(talker);
    return adp;
}

/// A sink in its listener's surroundings, everything it sends recorded.
struct Rig
{
    MilanListenerEnv env{};
    MilanSink sink{};
    std::vector<AcmpCommandResponse> probes{};
    std::vector<AcmpCommandResponse> responses{};
    int started{0};
    int stopped{0};
    MilanTalkerRegistration registration{MilanTalkerRegistration::None};

    Rig()
    {
        env.my_id = LISTENER;
        env.tx_command = [this](AcmpCommandResponse const& pdu) {
            probes.push_back(pdu);
            return true;
        };
        env.tx_response = [this](AcmpCommandResponse const& pdu) {
            responses.push_back(pdu);
            return true;
        };
        env.start_srp = [this](uint16_t, MilanSrpParameters const&, TimePoint) { ++started; };
        env.stop_srp = [this](uint16_t, TimePoint) { ++stopped; };
        env.random_delay = []() { return std::chrono::milliseconds{100}; };
        env.talker_registration = [this](MilanSrpParameters const& srp) {
            return srp.stream_id == STREAM ? registration : MilanTalkerRegistration::None;
        };
        sink.attach(env, SINK);
    }
    Rig(Rig const&) = delete;
    auto operator=(Rig const&) -> Rig& = delete;

    auto command(uint8_t const type, Eui64 const& talker) const -> AcmpCommandResponse
    {
        AcmpCommandResponse cmd{};
        cmd.init_command(type);
        cmd.controller_entity_id = CONTROLLER;
        cmd.talker_entity_id = talker;
        cmd.talker_unique_id = SOURCE;
        cmd.listener_entity_id = LISTENER;
        cmd.listener_unique_id = SINK;
        return cmd;
    }

    void bind(Eui64 const& talker, TimePoint const now)
    {
        sink.on_command(command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, talker), false, now);
    }

    /// The talker answers the last probe with its stream's parameters.
    void answer_probe(TimePoint const now, uint8_t const status = ACMP_STATUS_SUCCESS)
    {
        AcmpCommandResponse resp = probes.back();
        resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
        resp.set_status(status);
        resp.stream_id = STREAM;
        resp.stream_dest_mac = DEST;
        resp.stream_vlan_id = 2;
        sink.on_probe_response(resp, now);
    }
};

}  // namespace

TEST(milan_sink_wiring, a_restored_sink_probes_once_its_talker_advertises_and_settles_on_the_answer)
{
    // 5.5.3.6.2, then 5.6.4.5.1, 5.5.3.6.9, .10, .18 and .42 in turn.
    Rig rig;
    rig.sink.restore(SAVED, at(0));
    EXPECT_EQ(rig.sink.state(), State::PrbWAvail);
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkNotDiscovered);
    EXPECT_FALSE(rig.sink.next_deadline().has_value());
    rig.sink.tick(at(60'000));
    EXPECT_TRUE(rig.probes.empty());  // it waits for ADP, however long

    rig.sink.on_adp(available(TALKER, 1), OWN, at(61'000));
    EXPECT_EQ(rig.sink.state(), State::PrbWDelay);
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkDiscovered);
    EXPECT_EQ(*rig.sink.next_deadline(), at(61'100));  // TMR_DELAY, sooner than TMR_NO_ADP

    rig.sink.tick(at(61'099));
    EXPECT_TRUE(rig.probes.empty());
    rig.sink.tick(at(61'100));
    EXPECT_EQ(rig.probes.size(), 1U);
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);
    EXPECT_EQ(rig.probes.back().talker_entity_id, TALKER);
    EXPECT_EQ(rig.probes.back().controller_entity_id, CONTROLLER);
    EXPECT_EQ(rig.probes.back().listener_unique_id.get(), SINK);

    rig.answer_probe(at(61'105));
    EXPECT_EQ(rig.sink.state(), State::SettledNoRsv);
    EXPECT_TRUE(rig.sink.is_settled());
    EXPECT_EQ(rig.started, 1);
    EXPECT_EQ(rig.sink.context().srp.stream_id, STREAM);

    rig.registration = MilanTalkerRegistration::Advertise;
    rig.sink.on_srp_changed(at(61'400));
    EXPECT_EQ(rig.sink.state(), State::SettledRsvOk);
    EXPECT_EQ(rig.sink.context().probing_status, MilanProbingStatus::Completed);
}

TEST(milan_sink_wiring, a_talker_attribute_already_registered_is_seen_at_the_moment_of_settling)
{
    // EVT_TK_REGISTERED is the edge "matching attribute registered": the
    // match begins when the probe response gives the parameters to match.
    Rig rig;
    rig.registration = MilanTalkerRegistration::Advertise;
    rig.bind(TALKER, at(0));
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);
    rig.answer_probe(at(5));
    EXPECT_EQ(rig.sink.state(), State::SettledRsvOk);
    EXPECT_EQ(rig.started, 1);
}

TEST(milan_sink_wiring, an_attribute_that_stops_matching_sends_the_sink_back_to_probing)
{
    // 5.5.3.6.48, and 5.3.8.9: the sink's parameters are the probe's, not the attribute's.
    Rig rig;
    rig.registration = MilanTalkerRegistration::Advertise;
    rig.bind(TALKER, at(0));
    rig.answer_probe(at(5));
    rig.sink.on_adp(available(TALKER, 1), OWN, at(10));
    EXPECT_EQ(rig.sink.state(), State::SettledRsvOk);

    rig.sink.on_adp(available(TALKER, 2), OWN, at(25'000));  // the talker is still advertising
    rig.registration = MilanTalkerRegistration::None;
    rig.sink.on_srp_changed(at(30'000));
    EXPECT_EQ(rig.sink.state(), State::PrbWDelay);  // the talker is discovered: probe after the delay
    EXPECT_EQ(rig.stopped, 1);
    EXPECT_TRUE(rig.sink.context().srp == MilanSrpParameters{});
    rig.sink.tick(at(30'100));
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);
    EXPECT_EQ(rig.probes.size(), 2U);
    EXPECT_NE(rig.probes[1].sequence_id.get(), rig.probes[0].sequence_id.get());
}

TEST(milan_sink_wiring, ten_seconds_settled_with_no_talker_attribute_and_the_sink_waits_for_adp_again)
{
    // 5.5.3.6.36 with the talker not discovered: bound by a controller, the
    // talker answered the probe but never advertised.
    Rig rig;
    rig.bind(TALKER, at(0));
    rig.answer_probe(at(5));
    EXPECT_EQ(*rig.sink.next_deadline(), at(5) + MILAN_TMR_NO_TK);
    rig.sink.tick(at(10'004));
    EXPECT_EQ(rig.sink.state(), State::SettledNoRsv);
    rig.sink.tick(at(10'005));
    EXPECT_EQ(rig.sink.state(), State::PrbWAvail);
    EXPECT_EQ(rig.stopped, 1);
    EXPECT_EQ(rig.sink.context().probing_status, MilanProbingStatus::Passive);
}

TEST(milan_sink_wiring, adp_from_another_entity_or_another_gptp_domain_discovers_nothing)
{
    Rig rig;
    rig.sink.restore(SAVED, at(0));
    rig.sink.on_adp(available(OTHER_TALKER, 1), OWN, at(100));
    EXPECT_EQ(rig.sink.state(), State::PrbWAvail);
    rig.sink.on_adp(available(TALKER, 1), MilanGptpState{.grandmaster_id = GRANDMASTER, .domain_number = 1}, at(200));
    EXPECT_EQ(rig.sink.state(), State::PrbWAvail);
    rig.sink.on_adp(available(TALKER, 1), MilanGptpState{}, at(300));  // we have no grandmaster yet
    EXPECT_EQ(rig.sink.state(), State::PrbWAvail);
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkNotDiscovered);
}

TEST(milan_sink_wiring, an_unbound_sink_listens_to_no_adp)
{
    Rig rig;
    rig.sink.on_adp(available(TALKER, 1), OWN, at(100));
    EXPECT_EQ(rig.sink.state(), State::Unbound);
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::Stopped);
    EXPECT_FALSE(rig.sink.next_deadline().has_value());
}

TEST(milan_sink_wiring, a_probing_sink_whose_talker_departs_waits_and_a_settled_one_keeps_its_stream)
{
    // 5.5.3.6.22 against 5.5.3.6.41
    Rig probing;
    probing.sink.restore(SAVED, at(0));
    probing.sink.on_adp(available(TALKER, 1), OWN, at(100));
    probing.sink.tick(at(200));
    EXPECT_EQ(probing.sink.state(), State::PrbWResp);
    probing.sink.on_adp(departing(TALKER), OWN, at(250));
    EXPECT_EQ(probing.sink.state(), State::PrbWAvail);
    EXPECT_FALSE(probing.sink.next_deadline().has_value());

    Rig settled;
    settled.sink.restore(SAVED, at(0));
    settled.sink.on_adp(available(TALKER, 1), OWN, at(100));
    settled.sink.tick(at(200));
    settled.answer_probe(at(210));
    settled.sink.on_adp(departing(TALKER), OWN, at(250));
    EXPECT_EQ(settled.sink.state(), State::SettledNoRsv);
    EXPECT_FALSE(settled.sink.context().talker_discovered);
    EXPECT_EQ(settled.stopped, 0);
}

TEST(milan_sink_wiring, an_advertisement_that_lapses_is_a_departure)
{
    // 5.6.4.5.4 into 5.5.3.6.15
    Rig rig;
    rig.sink.restore(SAVED, at(0));
    rig.env.random_delay = []() { return std::chrono::milliseconds{1000}; };
    rig.sink.on_adp(available(TALKER, 1), OWN, at(0));
    rig.sink.tick(at(1000));
    rig.sink.tick(at(1200));
    rig.sink.tick(at(1400));
    EXPECT_EQ(rig.sink.state(), State::PrbWRetry);  // the talker answered neither probe
    EXPECT_TRUE(rig.sink.context().talker_discovered);
    rig.sink.tick(at(5400));
    EXPECT_EQ(rig.sink.state(), State::PrbWDelay);
    EXPECT_EQ(*rig.sink.next_deadline(), at(6400));
    rig.sink.tick(at(10'000));  // TMR_NO_ADP: 10 seconds after the one advertisement
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkNotDiscovered);
    EXPECT_EQ(rig.sink.state(), State::PrbWAvail);
    EXPECT_FALSE(rig.sink.next_deadline().has_value());
}

TEST(milan_sink_wiring, a_talker_that_restarted_is_probed_afresh)
{
    // 5.6.4.5.2 step 2: departed then discovered, so a sink waiting out
    // TMR_RETRY goes to the random delay and probes the restarted talker.
    Rig rig;
    rig.sink.restore(SAVED, at(0));
    rig.sink.on_adp(available(TALKER, 9), OWN, at(0));
    rig.sink.tick(at(100));
    rig.answer_probe(at(110), ACMP_STATUS_TALKER_DEST_MAC_FAIL);
    EXPECT_EQ(rig.sink.state(), State::PrbWRetry);
    EXPECT_EQ(rig.sink.context().acmp_status, ACMP_STATUS_TALKER_DEST_MAC_FAIL);

    rig.sink.on_adp(available(TALKER, 0), OWN, at(1000));  // available_index fell: it restarted
    EXPECT_EQ(rig.sink.state(), State::PrbWDelay);
    EXPECT_TRUE(rig.sink.context().talker_discovered);
    EXPECT_EQ(rig.sink.context().acmp_status, 0);
    rig.sink.tick(at(1100));
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);
    EXPECT_EQ(rig.probes.size(), 2U);
}

TEST(milan_sink_wiring, a_bind_starts_discovery_afresh_and_an_unbind_stops_it)
{
    Rig rig;
    rig.sink.restore(SAVED, at(0));
    rig.sink.on_adp(available(TALKER, 1), OWN, at(0));
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkDiscovered);

    // "Start the ADP Discovery state machine (at this point, talker has not been discovered)"
    rig.bind(OTHER_TALKER, at(50));
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkNotDiscovered);
    EXPECT_FALSE(rig.sink.context().talker_discovered);
    rig.sink.on_adp(available(TALKER, 2), OWN, at(60));  // the old talker is nobody now
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkNotDiscovered);
    rig.sink.on_adp(available(OTHER_TALKER, 1), OWN, at(70));
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::TkDiscovered);
    EXPECT_TRUE(rig.sink.context().talker_discovered);
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);

    rig.sink.on_command(rig.command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, OTHER_TALKER), false, at(80));
    EXPECT_EQ(rig.sink.state(), State::Unbound);
    EXPECT_FALSE(rig.sink.is_bound());
    EXPECT_EQ(rig.sink.discovery_state(), MilanDiscoveryState::Stopped);
    EXPECT_FALSE(rig.sink.next_deadline().has_value());
}

TEST(milan_sink_wiring, the_lock_is_passed_to_the_bind_and_unbind_but_not_to_the_query)
{
    Rig rig;
    rig.sink.on_command(rig.command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER), true, at(0));
    EXPECT_EQ(rig.sink.state(), State::Unbound);
    EXPECT_EQ(rig.responses.back().status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    rig.sink.on_command(rig.command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER), true, at(0));
    EXPECT_EQ(rig.responses.back().status(), ACMP_STATUS_SUCCESS);
    rig.bind(TALKER, at(1));
    rig.sink.on_command(rig.command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER), true, at(2));
    EXPECT_EQ(rig.sink.state(), State::PrbWResp);
    EXPECT_EQ(rig.responses.back().status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
}

TEST(milan_sink_wiring, a_restart_takes_a_bound_sink_back_to_waiting_and_leaves_an_unbound_one_alone)
{
    Rig bound;
    bound.registration = MilanTalkerRegistration::Advertise;
    bound.bind(TALKER, at(0));
    bound.answer_probe(at(5));
    EXPECT_EQ(bound.sink.state(), State::SettledRsvOk);
    bound.sink.restart(at(1000));
    EXPECT_EQ(bound.sink.state(), State::PrbWAvail);
    EXPECT_EQ(bound.stopped, 1);  // the reservation it held is given up
    EXPECT_TRUE(bound.sink.is_bound());
    EXPECT_EQ(bound.sink.context().binding->talker_entity_id, TALKER);
    EXPECT_TRUE(bound.sink.context().srp == MilanSrpParameters{});
    EXPECT_EQ(bound.sink.discovery_state(), MilanDiscoveryState::TkNotDiscovered);
    EXPECT_EQ(bound.sink.context().probing_status, MilanProbingStatus::Passive);
    EXPECT_EQ(bound.responses.size(), 1U);  // only the bind's: a restart answers nobody

    Rig unbound;
    unbound.sink.restart(at(1000));
    EXPECT_EQ(unbound.sink.state(), State::Unbound);
    EXPECT_EQ(unbound.sink.discovery_state(), MilanDiscoveryState::Stopped);
}

TEST(milan_sink_wiring, a_restored_binding_takes_the_sinks_own_index)
{
    Rig rig;
    auto saved = SAVED;
    saved.listener_unique_id = 9;  // a file edited by hand, or another model
    rig.sink.restore(saved, at(0));
    EXPECT_EQ(rig.sink.context().binding->listener_unique_id, SINK);
}

TEST_MAIN(statusbar_atdecc, atdecc_milan_acmp_sink_test)
