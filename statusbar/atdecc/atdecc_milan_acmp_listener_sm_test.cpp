// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The Milan v1.3 sink state machine: every transition of Table 5.31, each
// against the numbered treatment of 5.5.3.6 and the PDU table it names.
// Synthetic time throughout: the machine is told its time.

#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <array>
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
using Event = MilanSinkEvent;

namespace {

constexpr Eui64 LISTENER{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr Eui64 TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr Eui64 OTHER_TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x22};
constexpr Eui64 CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
constexpr Eui64 OTHER_CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x99};
constexpr Eui64 STREAM{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x05};
constexpr Eui48 DEST{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
constexpr uint16_t VLAN = 2;
constexpr uint16_t SINK = 3;
constexpr uint16_t SOURCE = 5;

constexpr std::array ALL_STATES{
    State::Unbound,
    State::PrbWAvail,
    State::PrbWDelay,
    State::PrbWResp,
    State::PrbWResp2,
    State::PrbWRetry,
    State::SettledNoRsv,
    State::SettledRsvOk};
constexpr std::array BOUND_STATES{
    State::PrbWAvail,
    State::PrbWDelay,
    State::PrbWResp,
    State::PrbWResp2,
    State::PrbWRetry,
    State::SettledNoRsv,
    State::SettledRsvOk};
constexpr std::array PROBING_STATES{State::PrbWAvail, State::PrbWDelay, State::PrbWResp, State::PrbWResp2, State::PrbWRetry};

auto at(int const ms) -> TimePoint
{
    return TimePoint{std::chrono::milliseconds{ms}};
}

/// One sink with everything it sends and asks for recorded.
struct Sink
{
    MilanListenerEnv env{};
    MilanSinkContext ctx{};
    MilanSinkStateMachine<> sm{};
    std::vector<AcmpCommandResponse> commands{};
    std::vector<AcmpCommandResponse> responses{};
    std::vector<MilanSrpParameters> started{};
    int stopped{0};
    int saved{0};
    /// How many responses had been sent when the binding was last saved.
    size_t responses_at_save{0};
    std::chrono::milliseconds delay{250};
    MilanTalkerRegistration registration{MilanTalkerRegistration::None};

    Sink()
    {
        env.my_id = LISTENER;
        env.next_sequence_id = 100;
        ctx.env = &env;
        ctx.index = SINK;
        env.tx_command = [this](AcmpCommandResponse const& pdu) {
            commands.push_back(pdu);
            return true;
        };
        env.tx_response = [this](AcmpCommandResponse const& pdu) {
            responses.push_back(pdu);
            return true;
        };
        env.start_srp = [this](uint16_t, MilanSrpParameters const& srp, TimePoint) { started.push_back(srp); };
        env.stop_srp = [this](uint16_t, TimePoint) { ++stopped; };
        env.binding_changed = [this](uint16_t) {
            ++saved;
            responses_at_save = responses.size();
        };
        env.random_delay = [this]() { return delay; };
        env.talker_registration = [this](MilanSrpParameters const&) { return registration; };
    }
    Sink(Sink const&) = delete;
    auto operator=(Sink const&) -> Sink& = delete;

    [[nodiscard]] auto state() const -> State { return sm.current_state(); }

    void fire(Event const event, TimePoint const now) { sm.handle_event(ctx, event, now); }

    /// A controller command for this sink, as it arrives on the wire.
    auto command(uint8_t const type, Eui64 const& talker, uint16_t const source, uint16_t const flags) const -> AcmpCommandResponse
    {
        AcmpCommandResponse cmd{};
        cmd.init_command(type);
        cmd.controller_entity_id = CONTROLLER;
        cmd.talker_entity_id = talker;
        cmd.listener_entity_id = LISTENER;
        cmd.talker_unique_id = source;
        cmd.listener_unique_id = SINK;
        cmd.sequence_id = 77;
        cmd.flags = flags;
        // What the sender leaves in fields the listener must not echo.
        cmd.stream_id = Eui64{1, 2, 3, 4, 5, 6, 7, 8};
        cmd.stream_dest_mac = Eui48{1, 2, 3, 4, 5, 6};
        cmd.stream_vlan_id = 99;
        cmd.connection_count = 42;
        return cmd;
    }

    void receive(AcmpCommandResponse const& cmd, bool const locked_out, TimePoint const now)
    {
        env.rcvd = cmd;
        auto const event = milan_sink_event_for_command(ctx, cmd, locked_out);
        EXPECT_TRUE(event.has_value());
        if (event) {
            fire(*event, now);
        }
    }

    void bind(Eui64 const& talker, TimePoint const now, uint16_t const flags = 0, bool const locked_out = false)
    {
        receive(command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, talker, SOURCE, flags), locked_out, now);
    }

    void unbind(TimePoint const now, bool const locked_out = false)
    {
        receive(command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, SOURCE, 0), locked_out, now);
    }

    void get_rx_state(TimePoint const now) { receive(command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, Eui64{}, 0, 0), false, now); }

    /// The talker's answer to the probe last sent.
    auto probe_response(uint8_t const status) const -> AcmpCommandResponse
    {
        AcmpCommandResponse resp = ctx.probe;
        resp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
        resp.set_status(status);
        resp.stream_id = STREAM;
        resp.stream_dest_mac = DEST;
        resp.stream_vlan_id = VLAN;
        return resp;
    }

    void receive_probe_response(AcmpCommandResponse const& resp, TimePoint const now)
    {
        env.rcvd = resp;
        if (auto const event = milan_sink_event_for_probe_response(ctx, resp)) {
            fire(*event, now);
        }
    }

    void expire(TimePoint const now)
    {
        auto const event = milan_sink_timer_event(ctx, state());
        EXPECT_TRUE(event.has_value());
        ctx.timer.reset();
        if (event) {
            fire(*event, now);
        }
    }

    /// Take a fresh sink to @p target by the shortest path, the talker
    /// discovered on the way only where the path needs it, then forget what
    /// was sent getting there.
    void enter(State const target)
    {
        if (target == State::PrbWAvail || target == State::PrbWDelay) {
            ctx.binding = MilanBinding{
                .listener_unique_id = SINK,
                .talker_entity_id = TALKER,
                .talker_unique_id = SOURCE,
                .controller_entity_id = CONTROLLER};
            fire(Event::StartBound, at(0));
        }
        if (target == State::PrbWDelay) {
            fire(Event::TalkerDiscovered, at(0));
        }
        if (target != State::Unbound && target != State::PrbWAvail && target != State::PrbWDelay) {
            bind(TALKER, at(0));
        }
        if (target == State::PrbWResp2 || target == State::PrbWRetry) {
            expire(at(200));
        }
        if (target == State::PrbWRetry) {
            expire(at(400));
        }
        if (target == State::SettledNoRsv || target == State::SettledRsvOk) {
            receive_probe_response(probe_response(ACMP_STATUS_SUCCESS), at(10));
        }
        if (target == State::SettledRsvOk) {
            fire(Event::TalkerRegistered, at(20));
        }
        EXPECT_EQ(state(), target);
        forget();
    }

    void forget()
    {
        commands.clear();
        responses.clear();
        started.clear();
        stopped = 0;
        saved = 0;
        ctx.discovery_start_requested = false;
        ctx.discovery_stop_requested = false;
    }
};

auto settled(State const state) -> bool
{
    return state == State::SettledNoRsv || state == State::SettledRsvOk;
}

/// The fields every response repeats from its command.
void expect_echo(AcmpCommandResponse const& resp, AcmpCommandResponse const& cmd)
{
    EXPECT_EQ(resp.message_type(), cmd.message_type() + 1);
    EXPECT_EQ(resp.controller_entity_id, cmd.controller_entity_id);
    EXPECT_EQ(resp.listener_entity_id, cmd.listener_entity_id);
    EXPECT_EQ(resp.listener_unique_id.get(), cmd.listener_unique_id.get());
    EXPECT_EQ(resp.sequence_id.get(), cmd.sequence_id.get());
}

void expect_no_stream(AcmpCommandResponse const& pdu)
{
    EXPECT_EQ(pdu.stream_id, Eui64{});
    EXPECT_EQ(pdu.stream_dest_mac, Eui48{});
    EXPECT_EQ(pdu.stream_vlan_id.get(), 0);
}

void expect_stream(AcmpCommandResponse const& pdu)
{
    EXPECT_EQ(pdu.stream_id, STREAM);
    EXPECT_EQ(pdu.stream_dest_mac, DEST);
    EXPECT_EQ(pdu.stream_vlan_id.get(), VLAN);
}

/// Table 5.36.
void expect_probe(AcmpCommandResponse const& probe, Eui64 const& talker, uint16_t const sequence_id)
{
    EXPECT_EQ(probe.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(probe.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(probe.controller_entity_id, CONTROLLER);
    EXPECT_EQ(probe.talker_entity_id, talker);
    EXPECT_EQ(probe.listener_entity_id, LISTENER);
    EXPECT_EQ(probe.talker_unique_id.get(), SOURCE);
    EXPECT_EQ(probe.listener_unique_id.get(), SINK);
    EXPECT_EQ(probe.sequence_id.get(), sequence_id);
    EXPECT_EQ(probe.connection_count.get(), 0);
    EXPECT_EQ(probe.flags.get(), acmp_flags::FAST_CONNECT);
    expect_no_stream(probe);
}

/// Table 5.34, for a bind that asked for @p flags.
void expect_bind_success(AcmpCommandResponse const& resp, AcmpCommandResponse const& cmd)
{
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, cmd.talker_entity_id);
    EXPECT_EQ(resp.talker_unique_id.get(), cmd.talker_unique_id.get());
    EXPECT_EQ(resp.connection_count.get(), 1);
    EXPECT_EQ(resp.flags.get(), cmd.flags.get() & acmp_flags::STREAMING_WAIT);
    expect_no_stream(resp);
}

/// Table 5.40.
void expect_unbind_success(AcmpCommandResponse const& resp, AcmpCommandResponse const& cmd)
{
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, Eui64{});
    EXPECT_EQ(resp.talker_unique_id.get(), 0);
    EXPECT_EQ(resp.connection_count.get(), 0);
    EXPECT_EQ(resp.flags.get(), 0);
    expect_no_stream(resp);
}

/// Tables 5.32 and 5.38: refused, every field as it came.
void expect_refused(AcmpCommandResponse const& resp, AcmpCommandResponse const& cmd)
{
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_EQ(resp.talker_entity_id, cmd.talker_entity_id);
    EXPECT_EQ(resp.talker_unique_id.get(), cmd.talker_unique_id.get());
}

}  // namespace

TEST(milan_sink, a_sink_with_no_saved_binding_starts_unbound)
{
    // 5.5.3.6.1
    Sink sink;
    EXPECT_EQ(sink.state(), State::Unbound);
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Disabled);
    EXPECT_EQ(sink.ctx.acmp_status, 0);
    EXPECT_FALSE(sink.ctx.binding.has_value());
}

TEST(milan_sink, a_sink_with_a_saved_binding_starts_waiting_for_its_talker)
{
    // 5.5.3.6.2
    Sink sink;
    sink.ctx.binding = MilanBinding{.listener_unique_id = SINK, .talker_entity_id = TALKER, .talker_unique_id = SOURCE};
    sink.fire(Event::StartBound, at(0));
    EXPECT_EQ(sink.state(), State::PrbWAvail);
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Passive);
    EXPECT_EQ(sink.ctx.acmp_status, 0);
    EXPECT_TRUE(sink.ctx.discovery_start_requested);
    EXPECT_FALSE(sink.ctx.talker_discovered);
    EXPECT_TRUE(sink.commands.empty());  // nothing is probed until the talker is discovered
    EXPECT_FALSE(sink.ctx.timer.has_value());
}

TEST(milan_sink, a_bind_of_an_unbound_sink_is_saved_answered_then_probed)
{
    // 5.5.3.6.3, Tables 5.34 and 5.36
    Sink sink;
    auto const cmd =
        sink.command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, SOURCE, acmp_flags::STREAMING_WAIT | acmp_flags::CLASS_B);
    sink.receive(cmd, false, at(1000));

    EXPECT_EQ(sink.state(), State::PrbWResp);
    EXPECT_TRUE(sink.ctx.binding.has_value());
    EXPECT_EQ(sink.ctx.binding->talker_entity_id, TALKER);
    EXPECT_EQ(sink.ctx.binding->talker_unique_id, SOURCE);
    EXPECT_EQ(sink.ctx.binding->controller_entity_id, CONTROLLER);
    EXPECT_EQ(sink.ctx.binding->listener_unique_id, SINK);
    EXPECT_TRUE(sink.ctx.binding->streaming_wait);
    EXPECT_EQ(sink.saved, 1);
    EXPECT_EQ(sink.responses_at_save, 0U);  // saved before the response is sent

    EXPECT_EQ(sink.responses.size(), 1U);
    expect_bind_success(sink.responses.front(), cmd);
    EXPECT_EQ(sink.responses.front().flags.get(), acmp_flags::STREAMING_WAIT);  // CLASS_B is not echoed (Table 5.25)

    EXPECT_EQ(sink.commands.size(), 1U);
    expect_probe(sink.commands.front(), TALKER, 100);
    EXPECT_TRUE(sink.commands.front() == sink.ctx.probe);  // a copy is kept
    EXPECT_EQ(*sink.ctx.timer, at(1000) + MILAN_TMR_NO_RESP);
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Active);
    EXPECT_EQ(sink.ctx.acmp_status, 0);
    EXPECT_TRUE(sink.ctx.discovery_start_requested);
    EXPECT_FALSE(sink.ctx.talker_discovered);
    EXPECT_TRUE(sink.started.empty());
}

TEST(milan_sink, a_bind_from_a_controller_the_lock_shuts_out_is_refused_in_every_state)
{
    // Step 1 of 5.5.3.6.3, .6, .11, .17, .24, .31, .37, .43; Table 5.32
    for (auto const state : ALL_STATES) {
        Sink sink;
        sink.enter(state);
        auto const before = sink.ctx;
        auto const cmd = sink.command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, OTHER_TALKER, SOURCE, 0);
        sink.receive(cmd, true, at(5000));
        EXPECT_EQ(sink.state(), state);
        EXPECT_EQ(sink.responses.size(), 1U);
        expect_refused(sink.responses.front(), cmd);
        EXPECT_EQ(sink.saved, 0);
        EXPECT_TRUE(sink.commands.empty());
        EXPECT_TRUE(sink.ctx.binding == before.binding);
        EXPECT_TRUE(sink.ctx.timer == before.timer);
        EXPECT_EQ(sink.stopped, 0);
    }
}

TEST(milan_sink, an_unbind_from_a_controller_the_lock_shuts_out_is_refused_in_every_state)
{
    // Step 1 of 5.5.3.6.5, .8, .13, .20, .27, .33, .39, .45; Table 5.38
    for (auto const state : ALL_STATES) {
        Sink sink;
        sink.enter(state);
        auto const before = sink.ctx;
        auto const cmd = sink.command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, SOURCE, 0);
        sink.receive(cmd, true, at(5000));
        EXPECT_EQ(sink.state(), state);
        EXPECT_EQ(sink.responses.size(), 1U);
        expect_refused(sink.responses.front(), cmd);
        EXPECT_TRUE(sink.ctx.binding == before.binding);
        EXPECT_EQ(sink.saved, 0);
        EXPECT_EQ(sink.stopped, 0);
    }
}

TEST(milan_sink, get_rx_state_on_an_unbound_sink_reports_nothing_bound)
{
    // 5.5.3.6.4, Table 5.37
    Sink sink;
    auto const cmd = sink.command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER, SOURCE, acmp_flags::STREAMING_WAIT);
    sink.receive(cmd, true, at(0));  // the lock does not shut out a query
    EXPECT_EQ(sink.state(), State::Unbound);
    EXPECT_EQ(sink.responses.size(), 1U);
    auto const& resp = sink.responses.front();
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, Eui64{});
    EXPECT_EQ(resp.talker_unique_id.get(), 0);
    EXPECT_EQ(resp.connection_count.get(), 0);
    EXPECT_EQ(resp.flags.get(), 0);
    expect_no_stream(resp);
}

TEST(milan_sink, an_unbind_of_an_unbound_sink_succeeds_and_changes_nothing)
{
    // 5.5.3.6.5, Table 5.40
    Sink sink;
    auto const cmd = sink.command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, SOURCE, acmp_flags::STREAMING_WAIT);
    sink.receive(cmd, false, at(0));
    EXPECT_EQ(sink.state(), State::Unbound);
    EXPECT_EQ(sink.responses.size(), 1U);
    expect_unbind_success(sink.responses.front(), cmd);
    EXPECT_EQ(sink.saved, 0);
}

TEST(milan_sink, a_bind_to_the_source_already_bound_only_updates_controller_and_streaming_wait)
{
    // Step 2 of 5.5.3.6.6, .11, .17, .24, .31, .37, .43
    for (auto const state : BOUND_STATES) {
        Sink sink;
        sink.enter(state);
        auto const before = sink.ctx;
        auto cmd = sink.command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, SOURCE, acmp_flags::STREAMING_WAIT);
        cmd.controller_entity_id = OTHER_CONTROLLER;
        sink.receive(cmd, false, at(5000));

        EXPECT_EQ(sink.state(), state);
        EXPECT_EQ(sink.ctx.binding->controller_entity_id, OTHER_CONTROLLER);
        EXPECT_TRUE(sink.ctx.binding->streaming_wait);
        EXPECT_EQ(sink.ctx.binding->talker_entity_id, TALKER);
        EXPECT_EQ(sink.saved, 1);
        EXPECT_EQ(sink.responses.size(), 1U);
        expect_bind_success(sink.responses.front(), cmd);
        // The probing, the settlement and the reservation go on as they were.
        EXPECT_TRUE(sink.commands.empty());
        EXPECT_TRUE(sink.ctx.timer == before.timer);
        EXPECT_TRUE(sink.ctx.srp == before.srp);
        EXPECT_EQ(sink.ctx.probing_status, before.probing_status);
        EXPECT_EQ(sink.stopped, 0);
        EXPECT_FALSE(sink.ctx.discovery_start_requested);
        EXPECT_FALSE(sink.ctx.discovery_stop_requested);
    }
}

TEST(milan_sink, a_bind_to_another_source_replaces_the_binding_and_probes_at_once)
{
    // From step 3 of 5.5.3.6.6, .11, .17, .24, .31, .37, .43
    for (auto const state : BOUND_STATES) {
        Sink sink;
        sink.enter(state);
        uint16_t const next_sequence = sink.env.next_sequence_id;
        sink.ctx.talker_discovered = true;
        auto const cmd = sink.command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, OTHER_TALKER, SOURCE, 0);
        sink.receive(cmd, false, at(5000));

        EXPECT_EQ(sink.state(), State::PrbWResp);
        EXPECT_EQ(sink.ctx.binding->talker_entity_id, OTHER_TALKER);
        EXPECT_EQ(sink.saved, 1);
        EXPECT_EQ(sink.responses_at_save, 0U);
        EXPECT_EQ(sink.responses.size(), 1U);
        expect_bind_success(sink.responses.front(), cmd);
        EXPECT_EQ(sink.commands.size(), 1U);
        expect_probe(sink.commands.front(), OTHER_TALKER, next_sequence);
        EXPECT_EQ(*sink.ctx.timer, at(5000) + MILAN_TMR_NO_RESP);  // the old state's timer is gone
        EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Active);
        EXPECT_EQ(sink.ctx.acmp_status, 0);
        // Discovery of the old talker stops and the new one's starts, not discovered.
        EXPECT_TRUE(sink.ctx.discovery_stop_requested);
        EXPECT_TRUE(sink.ctx.discovery_start_requested);
        EXPECT_FALSE(sink.ctx.talker_discovered);
        // A settled sink clears its SRP parameters and stops SRP first.
        EXPECT_EQ(sink.stopped, settled(state) ? 1 : 0);
        EXPECT_TRUE(sink.ctx.srp == MilanSrpParameters{});
    }
}

TEST(milan_sink, get_rx_state_while_probing_reports_the_binding_and_no_stream)
{
    // 5.5.3.6.7, .12, .19, .26, .32; Table 5.42
    for (auto const state : PROBING_STATES) {
        Sink sink;
        sink.enter(state);
        sink.ctx.binding->streaming_wait = true;
        auto const cmd = sink.command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, Eui64{}, 0, 0);
        sink.receive(cmd, false, at(5000));
        EXPECT_EQ(sink.state(), state);
        EXPECT_EQ(sink.responses.size(), 1U);
        auto const& resp = sink.responses.front();
        expect_echo(resp, cmd);
        EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
        EXPECT_EQ(resp.talker_entity_id, TALKER);
        EXPECT_EQ(resp.talker_unique_id.get(), SOURCE);
        EXPECT_EQ(resp.connection_count.get(), 1);
        EXPECT_EQ(resp.flags.get(), acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT);
        expect_no_stream(resp);
    }
}

TEST(milan_sink, get_rx_state_while_settled_reports_the_stream_and_registering_failed_only_once_registered)
{
    // 5.5.3.6.38 Table 5.43, 5.5.3.6.44 Table 5.44
    for (auto const state : {State::SettledNoRsv, State::SettledRsvOk}) {
        for (auto const registration : {MilanTalkerRegistration::Advertise, MilanTalkerRegistration::Failed}) {
            Sink sink;
            sink.enter(state);
            sink.registration = registration;
            sink.get_rx_state(at(5000));
            EXPECT_EQ(sink.state(), state);
            auto const& resp = sink.responses.front();
            EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
            EXPECT_EQ(resp.talker_entity_id, TALKER);
            EXPECT_EQ(resp.talker_unique_id.get(), SOURCE);
            EXPECT_EQ(resp.connection_count.get(), 1);
            bool const failed = state == State::SettledRsvOk && registration == MilanTalkerRegistration::Failed;
            EXPECT_EQ(
                resp.flags.get(),
                failed ? acmp_flags::FAST_CONNECT | acmp_flags::SRP_REGISTRATION_FAILED : acmp_flags::FAST_CONNECT);
            expect_stream(resp);
        }
    }
}

TEST(milan_sink, an_unbind_clears_the_binding_in_every_bound_state)
{
    // 5.5.3.6.8, .13, .20, .27, .33, .39, .45; Table 5.40
    for (auto const state : BOUND_STATES) {
        Sink sink;
        sink.enter(state);
        auto const cmd =
            sink.command(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, OTHER_TALKER, 9, 0);  // the talker named does not matter
        sink.receive(cmd, false, at(5000));

        EXPECT_EQ(sink.state(), State::Unbound);
        EXPECT_FALSE(sink.ctx.binding.has_value());
        EXPECT_EQ(sink.saved, 1);
        EXPECT_EQ(sink.responses_at_save, 0U);
        EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Disabled);
        EXPECT_EQ(sink.ctx.acmp_status, 0);
        EXPECT_FALSE(sink.ctx.timer.has_value());
        EXPECT_TRUE(sink.ctx.discovery_stop_requested);
        EXPECT_FALSE(sink.ctx.discovery_start_requested);
        EXPECT_EQ(sink.stopped, settled(state) ? 1 : 0);
        EXPECT_TRUE(sink.ctx.srp == MilanSrpParameters{});
        EXPECT_EQ(sink.responses.size(), 1U);
        expect_unbind_success(sink.responses.front(), cmd);
        EXPECT_TRUE(sink.commands.empty());  // the talker is not involved
    }
}

TEST(milan_sink, a_discovered_talker_is_probed_after_a_random_delay)
{
    // 5.5.3.6.9 then 5.5.3.6.10
    Sink sink;
    sink.enter(State::PrbWAvail);
    sink.delay = std::chrono::milliseconds{730};
    sink.fire(Event::TalkerDiscovered, at(1000));
    EXPECT_EQ(sink.state(), State::PrbWDelay);
    EXPECT_TRUE(sink.ctx.talker_discovered);
    EXPECT_EQ(*sink.ctx.timer, at(1730));
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Active);
    EXPECT_EQ(sink.ctx.acmp_status, 0);
    EXPECT_TRUE(sink.commands.empty());

    sink.expire(at(1730));
    EXPECT_EQ(sink.state(), State::PrbWResp);
    EXPECT_EQ(sink.commands.size(), 1U);
    expect_probe(sink.commands.front(), TALKER, 100);
    EXPECT_TRUE(sink.commands.front() == sink.ctx.probe);
    EXPECT_EQ(*sink.ctx.timer, at(1730) + MILAN_TMR_NO_RESP);
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Active);
}

TEST(milan_sink, the_random_delay_is_held_to_one_second)
{
    Sink sink;
    sink.enter(State::PrbWAvail);
    sink.delay = std::chrono::milliseconds{5000};
    sink.fire(Event::TalkerDiscovered, at(1000));
    EXPECT_EQ(*sink.ctx.timer, at(1000) + MILAN_TMR_DELAY_MAX);
}

TEST(milan_sink, discovery_is_noted_and_departure_returns_to_waiting_in_the_probing_states)
{
    // 5.5.3.6.14 and .15, .21 and .22, .28 and .29, .34 and .35
    for (auto const state : {State::PrbWDelay, State::PrbWResp, State::PrbWResp2, State::PrbWRetry}) {
        Sink sink;
        sink.enter(state);
        sink.ctx.talker_discovered = false;
        auto const timer = sink.ctx.timer;
        sink.fire(Event::TalkerDiscovered, at(5000));
        EXPECT_EQ(sink.state(), state);
        EXPECT_TRUE(sink.ctx.talker_discovered);
        EXPECT_TRUE(sink.ctx.timer == timer);

        sink.ctx.acmp_status = ACMP_STATUS_TALKER_DEST_MAC_FAIL;
        sink.fire(Event::TalkerDeparted, at(5001));
        EXPECT_EQ(sink.state(), State::PrbWAvail);
        EXPECT_FALSE(sink.ctx.talker_discovered);
        EXPECT_FALSE(sink.ctx.timer.has_value());
        EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Passive);
        EXPECT_EQ(sink.ctx.acmp_status, 0);
        EXPECT_TRUE(sink.commands.empty());
    }
}

TEST(milan_sink, an_unanswered_probe_is_sent_once_more_then_given_four_seconds)
{
    // 5.5.3.6.16 then 5.5.3.6.23
    Sink sink;
    sink.enter(State::PrbWResp);
    auto const first = sink.ctx.probe;
    sink.expire(at(200));
    EXPECT_EQ(sink.state(), State::PrbWResp2);
    EXPECT_EQ(sink.commands.size(), 1U);
    EXPECT_TRUE(sink.commands.front() == first);  // a duplicate: the same sequence_id
    EXPECT_EQ(*sink.ctx.timer, at(200) + MILAN_TMR_NO_RESP);
    EXPECT_EQ(sink.ctx.acmp_status, 0);

    sink.expire(at(400));
    EXPECT_EQ(sink.state(), State::PrbWRetry);
    EXPECT_EQ(sink.commands.size(), 1U);
    EXPECT_EQ(*sink.ctx.timer, at(400) + MILAN_TMR_RETRY);
    EXPECT_EQ(sink.ctx.acmp_status, ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Active);
}

TEST(milan_sink, a_successful_probe_response_settles_the_sink_on_its_parameters)
{
    // 5.5.3.6.18 step 4, and 5.5.3.6.25
    for (auto const state : {State::PrbWResp, State::PrbWResp2}) {
        Sink sink;
        sink.enter(state);
        sink.receive_probe_response(sink.probe_response(ACMP_STATUS_SUCCESS), at(1000));
        EXPECT_EQ(sink.state(), State::SettledNoRsv);
        EXPECT_EQ(sink.ctx.srp.stream_id, STREAM);
        EXPECT_EQ(sink.ctx.srp.stream_dest_mac, DEST);
        EXPECT_EQ(sink.ctx.srp.stream_vlan_id, VLAN);
        EXPECT_EQ(sink.started.size(), 1U);
        EXPECT_TRUE(sink.started.front() == sink.ctx.srp);
        EXPECT_EQ(*sink.ctx.timer, at(1000) + MILAN_TMR_NO_TK);
        EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Completed);
        EXPECT_EQ(sink.ctx.acmp_status, 0);
    }
}

TEST(milan_sink, a_refused_probe_waits_four_seconds_with_the_talkers_status)
{
    // 5.5.3.6.18 step 3, and 5.5.3.6.25
    for (auto const state : {State::PrbWResp, State::PrbWResp2}) {
        Sink sink;
        sink.enter(state);
        sink.receive_probe_response(sink.probe_response(ACMP_STATUS_TALKER_DEST_MAC_FAIL), at(1000));
        EXPECT_EQ(sink.state(), State::PrbWRetry);
        EXPECT_EQ(*sink.ctx.timer, at(1000) + MILAN_TMR_RETRY);
        EXPECT_EQ(sink.ctx.acmp_status, ACMP_STATUS_TALKER_DEST_MAC_FAIL);
        EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Active);
        EXPECT_TRUE(sink.started.empty());
        EXPECT_TRUE(sink.ctx.srp == MilanSrpParameters{});
    }
}

TEST(milan_sink, a_probe_response_that_does_not_answer_the_probe_sent_is_ignored)
{
    // 5.5.3.6.18 step 1: controller, talker, source and sequence_id must all match.
    Sink sink;
    sink.enter(State::PrbWResp);
    auto const good = sink.probe_response(ACMP_STATUS_SUCCESS);
    auto wrong_controller = good;
    wrong_controller.controller_entity_id = OTHER_CONTROLLER;
    auto wrong_talker = good;
    wrong_talker.talker_entity_id = OTHER_TALKER;
    auto wrong_source = good;
    wrong_source.talker_unique_id = SOURCE + 1;
    auto wrong_sequence = good;
    wrong_sequence.sequence_id = good.sequence_id.get() + 1;
    auto not_a_probe_response = good;
    not_a_probe_response.set_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
    for (auto const& resp : {wrong_controller, wrong_talker, wrong_source, wrong_sequence, not_a_probe_response}) {
        EXPECT_FALSE(milan_sink_event_for_probe_response(sink.ctx, resp).has_value());
        sink.receive_probe_response(resp, at(100));
        EXPECT_EQ(sink.state(), State::PrbWResp);
    }
    EXPECT_TRUE(milan_sink_event_for_probe_response(sink.ctx, good).has_value());
}

TEST(milan_sink, a_probe_response_outside_the_two_waiting_states_changes_nothing)
{
    // Table 5.31: "-" under RCV_PROBE_TX_RESP everywhere but PRB_W_RESP and PRB_W_RESP2.
    for (auto const state : {State::PrbWAvail, State::PrbWDelay, State::PrbWRetry, State::SettledNoRsv, State::SettledRsvOk}) {
        Sink sink;
        sink.enter(state);
        auto const before = sink.ctx;
        sink.fire(Event::ProbeAccepted, at(5000));
        sink.fire(Event::ProbeRejected, at(5000));
        EXPECT_EQ(sink.state(), state);
        EXPECT_TRUE(sink.ctx.timer == before.timer);
        EXPECT_TRUE(sink.ctx.srp == before.srp);
        EXPECT_TRUE(sink.started.empty());
    }
}

TEST(milan_sink, after_the_retry_wait_a_discovered_talker_is_probed_again_and_an_absent_one_waited_for)
{
    // 5.5.3.6.30
    Sink discovered;
    discovered.enter(State::PrbWRetry);
    discovered.ctx.talker_discovered = true;
    discovered.delay = std::chrono::milliseconds{400};
    discovered.expire(at(5000));
    EXPECT_EQ(discovered.state(), State::PrbWDelay);
    EXPECT_EQ(*discovered.ctx.timer, at(5400));
    EXPECT_EQ(discovered.ctx.probing_status, MilanProbingStatus::Active);
    EXPECT_EQ(discovered.ctx.acmp_status, ACMP_STATUS_LISTENER_TALKER_TIMEOUT);  // what the last probe ended with

    Sink absent;
    absent.enter(State::PrbWRetry);
    absent.expire(at(5000));
    EXPECT_EQ(absent.state(), State::PrbWAvail);
    EXPECT_FALSE(absent.ctx.timer.has_value());
    EXPECT_EQ(absent.ctx.probing_status, MilanProbingStatus::Passive);
    EXPECT_EQ(absent.ctx.acmp_status, 0);
}

TEST(milan_sink, a_settled_sink_with_no_talker_attribute_in_ten_seconds_probes_again)
{
    // 5.5.3.6.36
    Sink discovered;
    discovered.enter(State::SettledNoRsv);
    discovered.ctx.talker_discovered = true;
    discovered.delay = std::chrono::milliseconds{300};
    discovered.expire(at(10'010));
    EXPECT_EQ(discovered.state(), State::PrbWDelay);
    EXPECT_EQ(discovered.stopped, 1);
    EXPECT_TRUE(discovered.ctx.srp == MilanSrpParameters{});
    EXPECT_EQ(discovered.ctx.probing_status, MilanProbingStatus::Active);
    EXPECT_EQ(*discovered.ctx.timer, at(10'310));

    Sink absent;
    absent.enter(State::SettledNoRsv);
    absent.expire(at(10'010));
    EXPECT_EQ(absent.state(), State::PrbWAvail);
    EXPECT_EQ(absent.stopped, 1);
    EXPECT_TRUE(absent.ctx.srp == MilanSrpParameters{});
    EXPECT_EQ(absent.ctx.probing_status, MilanProbingStatus::Passive);
    EXPECT_FALSE(absent.ctx.timer.has_value());
}

TEST(milan_sink, a_settled_sink_only_notes_its_talkers_discovery_and_departure)
{
    // 5.5.3.6.40 and .41, .46 and .47: the stream stays until SRP says otherwise.
    for (auto const state : {State::SettledNoRsv, State::SettledRsvOk}) {
        Sink sink;
        sink.enter(state);
        auto const timer = sink.ctx.timer;
        sink.fire(Event::TalkerDiscovered, at(5000));
        EXPECT_TRUE(sink.ctx.talker_discovered);
        sink.fire(Event::TalkerDeparted, at(5001));
        EXPECT_FALSE(sink.ctx.talker_discovered);
        EXPECT_EQ(sink.state(), state);
        EXPECT_EQ(sink.stopped, 0);
        EXPECT_EQ(sink.ctx.srp.stream_id, STREAM);
        EXPECT_TRUE(sink.ctx.timer == timer);
        EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Completed);
    }
}

TEST(milan_sink, the_talker_attribute_registered_ends_the_ten_second_wait)
{
    // 5.5.3.6.42
    Sink sink;
    sink.enter(State::SettledNoRsv);
    sink.fire(Event::TalkerRegistered, at(3000));
    EXPECT_EQ(sink.state(), State::SettledRsvOk);
    EXPECT_FALSE(sink.ctx.timer.has_value());
    EXPECT_EQ(sink.ctx.srp.stream_id, STREAM);
    EXPECT_EQ(sink.stopped, 0);
    EXPECT_EQ(sink.ctx.probing_status, MilanProbingStatus::Completed);
}

TEST(milan_sink, the_talker_attribute_unregistered_drops_the_parameters_and_probes_again)
{
    // 5.5.3.6.48
    Sink discovered;
    discovered.enter(State::SettledRsvOk);
    discovered.ctx.talker_discovered = true;
    discovered.delay = std::chrono::milliseconds{900};
    discovered.fire(Event::TalkerUnregistered, at(60'000));
    EXPECT_EQ(discovered.state(), State::PrbWDelay);
    EXPECT_EQ(discovered.stopped, 1);
    EXPECT_TRUE(discovered.ctx.srp == MilanSrpParameters{});
    EXPECT_EQ(discovered.ctx.probing_status, MilanProbingStatus::Active);
    EXPECT_EQ(discovered.ctx.acmp_status, 0);
    EXPECT_EQ(*discovered.ctx.timer, at(60'900));

    Sink absent;
    absent.enter(State::SettledRsvOk);
    absent.fire(Event::TalkerUnregisteredNoTalker, at(60'000));
    EXPECT_EQ(absent.state(), State::PrbWAvail);
    EXPECT_EQ(absent.stopped, 1);
    EXPECT_EQ(absent.ctx.probing_status, MilanProbingStatus::Passive);
    EXPECT_FALSE(absent.ctx.timer.has_value());
}

TEST(milan_sink, an_event_table_5_31_marks_impossible_leaves_the_state_alone)
{
    // "x" in Table 5.31: each timer belongs to its own state, the SRP events to the settled ones.
    for (auto const state : ALL_STATES) {
        Sink sink;
        sink.enter(state);
        if (state != State::PrbWDelay) {
            sink.fire(Event::TmrDelay, at(9000));
        }
        if (state != State::PrbWResp && state != State::PrbWResp2) {
            sink.fire(Event::TmrNoResp, at(9000));
        }
        if (state != State::PrbWRetry) {
            sink.fire(Event::TmrRetry, at(9000));
            sink.fire(Event::TmrRetryNoTalker, at(9000));
        }
        if (state != State::SettledNoRsv) {
            sink.fire(Event::TmrNoTk, at(9000));
            sink.fire(Event::TmrNoTkNoTalker, at(9000));
            sink.fire(Event::TalkerRegistered, at(9000));
        }
        if (state != State::SettledRsvOk) {
            sink.fire(Event::TalkerUnregistered, at(9000));
            sink.fire(Event::TalkerUnregisteredNoTalker, at(9000));
        }
        if (state != State::Unbound) {
            sink.fire(Event::StartBound, at(9000));
        }
        EXPECT_EQ(sink.state(), state);
        EXPECT_TRUE(sink.commands.empty());
        EXPECT_TRUE(sink.responses.empty());
        EXPECT_EQ(sink.stopped, 0);
    }
    Sink unbound;
    unbound.fire(Event::TalkerDiscovered, at(0));
    unbound.fire(Event::TalkerDeparted, at(0));
    unbound.fire(Event::BindSameTalker, at(0));
    EXPECT_EQ(unbound.state(), State::Unbound);
}

TEST(milan_sink, the_timer_event_is_the_one_the_state_runs)
{
    MilanSinkContext ctx{};
    EXPECT_FALSE(milan_sink_timer_event(ctx, State::Unbound).has_value());
    EXPECT_FALSE(milan_sink_timer_event(ctx, State::PrbWAvail).has_value());
    EXPECT_FALSE(milan_sink_timer_event(ctx, State::SettledRsvOk).has_value());
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::PrbWDelay), Event::TmrDelay);
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::PrbWResp), Event::TmrNoResp);
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::PrbWResp2), Event::TmrNoResp);
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::PrbWRetry), Event::TmrRetryNoTalker);
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::SettledNoRsv), Event::TmrNoTkNoTalker);
    ctx.talker_discovered = true;
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::PrbWRetry), Event::TmrRetry);
    EXPECT_EQ(*milan_sink_timer_event(ctx, State::SettledNoRsv), Event::TmrNoTk);
}

TEST(milan_sink, a_registration_event_fires_only_on_the_edge)
{
    MilanSinkContext ctx{};
    EXPECT_EQ(*milan_sink_registration_event(ctx, State::SettledNoRsv, true), Event::TalkerRegistered);
    EXPECT_FALSE(milan_sink_registration_event(ctx, State::SettledNoRsv, false).has_value());
    EXPECT_FALSE(milan_sink_registration_event(ctx, State::SettledRsvOk, true).has_value());
    EXPECT_EQ(*milan_sink_registration_event(ctx, State::SettledRsvOk, false), Event::TalkerUnregisteredNoTalker);
    ctx.talker_discovered = true;
    EXPECT_EQ(*milan_sink_registration_event(ctx, State::SettledRsvOk, false), Event::TalkerUnregistered);
    for (auto const state : PROBING_STATES) {
        EXPECT_FALSE(milan_sink_registration_event(ctx, state, true).has_value());
        EXPECT_FALSE(milan_sink_registration_event(ctx, state, false).has_value());
    }
}

TEST(milan_sink, a_command_that_is_not_for_a_listener_is_no_event)
{
    Sink sink;
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND,
          ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND,
          ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND,
          ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND,
          ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE}) {
        EXPECT_FALSE(milan_sink_event_for_command(sink.ctx, sink.command(type, TALKER, SOURCE, 0), false).has_value());
    }
}

TEST(milan_sink, the_response_to_an_unknown_sink_is_listener_unknown_id)
{
    // 5.5.3.1 step 1, Table 5.27
    Sink sink;
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND}) {
        auto const cmd = sink.command(type, TALKER, SOURCE, 0);
        auto const resp = milan_unknown_sink_response(cmd);
        expect_echo(resp, cmd);
        EXPECT_EQ(resp.status(), ACMP_STATUS_LISTENER_UNKNOWN_ID);
        EXPECT_EQ(resp.talker_entity_id, TALKER);
        EXPECT_EQ(resp.talker_unique_id.get(), SOURCE);
    }
}

TEST_MAIN(statusbar_atdecc, atdecc_milan_acmp_listener_sm_test)
