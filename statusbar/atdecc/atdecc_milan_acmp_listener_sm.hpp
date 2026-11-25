#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Milan v1.3 listener sink state machine, section 5.5.3.
///
/// One machine per STREAM_INPUT. It is a second ACMP listener beside the
/// IEEE 1722.1-2021 one (atdecc_acmp_listener_sm.hpp), not a variant of it:
/// a Milan sink is bound by a controller, answers at once, and then probes
/// its talker and follows the talker's SRP attribute by itself for as long
/// as the binding stands. The states are Table 5.29's, the transitions
/// Table 5.31's, each action one numbered subsection of 5.5.3.6.
///
/// The framework gives each (state, event) one next state, so an event whose
/// treatment depends on a condition is split by that condition before it
/// reaches the machine: a bind command arrives as BindRefused, BindSameTalker
/// or BindNewTalker, and a timer that asks "has the talker been discovered?"
/// as two events. The milan_sink_event_for_* helpers do the splitting.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

//
// Timers - Milan v1.3 Table 5.30
//

/// TMR_NO_RESP: how long a PROBE_TX_COMMAND waits for its response (also Table 5.26).
inline constexpr std::chrono::milliseconds MILAN_TMR_NO_RESP{200};
/// TMR_RETRY: the pause after two unanswered probes, or a refused one.
inline constexpr std::chrono::seconds MILAN_TMR_RETRY{4};
/// TMR_DELAY: the upper bound of the random delay before a probe.
inline constexpr std::chrono::milliseconds MILAN_TMR_DELAY_MAX{1000};
/// TMR_NO_TK: how long a settled sink waits for the talker's SRP attribute.
inline constexpr std::chrono::seconds MILAN_TMR_NO_TK{10};

//
// Per-sink variables - Milan v1.3 5.3.8
//

/// Probing status, Table 5.5.
enum class MilanProbingStatus : uint8_t
{
    Disabled = 0,   ///< PROBING_DISABLED: not bound
    Passive = 1,    ///< PROBING_PASSIVE: waiting for the talker to be discovered
    Active = 2,     ///< PROBING_ACTIVE: querying the talker
    Completed = 3,  ///< PROBING_COMPLETED: settled
};

/// The binding parameters of a bound sink, 5.3.8.3: what is saved in
/// non-volatile memory and restored at startup.
struct MilanBinding
{
    uint16_t listener_unique_id{0};  ///< the STREAM_INPUT index
    ieee::Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};  ///< the talker's STREAM_OUTPUT index
    ieee::Eui64 controller_entity_id{};
    bool streaming_wait{false};  ///< the saved started/stopped state

    auto operator==(MilanBinding const&) const noexcept -> bool = default;
};

/// The SRP parameters of a settled sink, 5.3.8.9: exactly what the last
/// PROBE_TX_RESPONSE carried, and all zero while the sink is not settled.
struct MilanSrpParameters
{
    ieee::Eui64 stream_id{};
    ieee::Eui48 stream_dest_mac{};
    uint16_t stream_vlan_id{0};

    auto operator==(MilanSrpParameters const&) const noexcept -> bool = default;
};

/// What SRP registers for a settled sink's stream, 5.3.8.8. Only an
/// attribute whose StreamID, destination address and VLAN all match the
/// sink's SRP parameters counts.
enum class MilanTalkerRegistration : uint8_t
{
    None,
    Advertise,
    Failed,
};

//
// Sink state machine - Milan v1.3 5.5.3.3 and 5.5.3.4
//

/// The states of a sink, Table 5.29.
enum class MilanSinkState : uint8_t
{
    Unbound,       ///< UNBOUND
    PrbWAvail,     ///< PRB_W_AVAIL: waiting for the talker to be discovered
    PrbWDelay,     ///< PRB_W_DELAY: waiting the random delay before a probe
    PrbWResp,      ///< PRB_W_RESP: first PROBE_TX_COMMAND sent
    PrbWResp2,     ///< PRB_W_RESP2: second PROBE_TX_COMMAND sent
    PrbWRetry,     ///< PRB_W_RETRY: waiting to try again
    SettledNoRsv,  ///< SETTLED_NO_RSV: parameters known, no Talker attribute registered
    SettledRsvOk,  ///< SETTLED_RSV_OK: parameters known, Talker attribute registered
    Count
};

/// The events of Table 5.30, split where a transition has a condition.
enum class MilanSinkEvent : uint8_t
{
    StartBound,                  ///< startup with a saved binding (5.5.3.6.2)
    BindRefused,                 ///< RCV_BIND_RX_CMD from a controller the lock shuts out
    BindSameTalker,              ///< RCV_BIND_RX_CMD naming the source already bound
    BindNewTalker,               ///< RCV_BIND_RX_CMD naming another source, or the first
    GetRxState,                  ///< RCV_GET_RX_STATE
    UnbindRefused,               ///< RCV_UNBIND_RX_CMD from a controller the lock shuts out
    Unbind,                      ///< RCV_UNBIND_RX_CMD
    ProbeAccepted,               ///< RCV_PROBE_TX_RESP, status SUCCESS
    ProbeRejected,               ///< RCV_PROBE_TX_RESP, any other status
    TmrNoResp,                   ///< TMR_NO_RESP
    TmrRetry,                    ///< TMR_RETRY, talker discovered
    TmrRetryNoTalker,            ///< TMR_RETRY, talker not discovered
    TmrDelay,                    ///< TMR_DELAY
    TmrNoTk,                     ///< TMR_NO_TK, talker discovered
    TmrNoTkNoTalker,             ///< TMR_NO_TK, talker not discovered
    TalkerDiscovered,            ///< EVT_TK_DISCOVERED
    TalkerDeparted,              ///< EVT_TK_DEPARTED
    TalkerRegistered,            ///< EVT_TK_REGISTERED
    TalkerUnregistered,          ///< EVT_TK_UNREGISTERED, talker discovered
    TalkerUnregisteredNoTalker,  ///< EVT_TK_UNREGISTERED, talker not discovered
    Count
};

/// What the sinks of one listener share: its identity, the PDU the event
/// being handled arrived in, and the ways out of the machine.
struct MilanListenerEnv
{
    /// Entity ID of this listener.
    ieee::Eui64 my_id{};

    /// The command or response the event being handled arrived in.
    AcmpCommandResponse rcvd{};

    /// The sequence_id of the next new PROBE_TX_COMMAND.
    uint16_t next_sequence_id{0};

    /// Send a PROBE_TX_COMMAND to the talker.
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_command{};

    /// Send a response to the controller.
    statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response{};

    /// The sink settled: start the SRP reservation and listen for the stream.
    statusbar::sg14::inplace_function<void(uint16_t sink, MilanSrpParameters const&, sm::TimePoint), 64> start_srp{};

    /// "Clear the SRP parameters and stop SRP": withdraw the reservation and
    /// stop listening. The machine has cleared the parameters already.
    statusbar::sg14::inplace_function<void(uint16_t sink, sm::TimePoint), 64> stop_srp{};

    /// A sink's binding parameters changed or were cleared: save them to
    /// non-volatile memory. Called before the response is sent.
    statusbar::sg14::inplace_function<void(uint16_t sink), 64> binding_changed{};

    /// The next TMR_DELAY value, 0 to MILAN_TMR_DELAY_MAX. Unset: no delay.
    statusbar::sg14::inplace_function<std::chrono::milliseconds(), 64> random_delay{};

    /// What SRP registers now that matches @p parameters. Unset: nothing.
    statusbar::sg14::inplace_function<MilanTalkerRegistration(MilanSrpParameters const&), 64> talker_registration{};
};

/// One sink's variables.
struct MilanSinkContext
{
    /// The listener this sink belongs to. Set before the first event.
    MilanListenerEnv* env{nullptr};

    /// The STREAM_INPUT index, the listener_unique_id of its PDUs.
    uint16_t index{0};

    /// The binding parameters while bound (5.3.8.3).
    std::optional<MilanBinding> binding{};

    /// A copy of the PROBE_TX_COMMAND last sent (5.5.3.6.3 step 6): what a
    /// timeout sends again and a response is checked against.
    AcmpCommandResponse probe{};

    /// The SRP parameters while settled (5.3.8.9), zero otherwise.
    MilanSrpParameters srp{};

    /// start_srp has been called for srp and stop_srp not yet.
    bool srp_active{false};

    /// The deadline of the one timer the current state runs: TMR_DELAY,
    /// TMR_NO_RESP, TMR_RETRY or TMR_NO_TK.
    std::optional<sm::TimePoint> timer{};

    /// What the sink has noted from EVT_TK_DISCOVERED and EVT_TK_DEPARTED.
    bool talker_discovered{false};

    /// A transition asks for the ADP Discovery state machine to be stopped,
    /// or started afresh with the talker not discovered. Whoever runs the two
    /// machines of a sink together carries it out and clears the flags.
    bool discovery_start_requested{false};
    bool discovery_stop_requested{false};

    /// Probing status and ACMP status (5.3.8.6).
    MilanProbingStatus probing_status{MilanProbingStatus::Disabled};
    uint8_t acmp_status{ACMP_STATUS_SUCCESS};
};

//
// PDUs - the tables of 5.5.3
//

/// The response to a BIND_RX, UNBIND_RX or GET_RX_STATE command whose
/// listener_unique_id names no STREAM_INPUT (Table 5.27).
[[nodiscard]] auto milan_unknown_sink_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse;

/// The response to a bind or unbind from a controller other than the one
/// holding the lock (Tables 5.32 and 5.38).
[[nodiscard]] auto milan_not_authorized_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse;

/// BIND_RX_RESPONSE on success (Table 5.34).
[[nodiscard]] auto milan_bind_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse;

/// UNBIND_RX_RESPONSE on success (Table 5.40).
[[nodiscard]] auto milan_unbind_response(AcmpCommandResponse const& cmd) noexcept -> AcmpCommandResponse;

/// PROBE_TX_COMMAND (Table 5.36) for @p binding, from listener @p my_id.
[[nodiscard]] auto milan_probe_command(MilanBinding const& binding, ieee::Eui64 const& my_id, uint16_t sequence_id) noexcept
    -> AcmpCommandResponse;

/// GET_RX_STATE_RESPONSE for a sink as @p ctx stands: Table 5.37 unbound,
/// 5.42 probing, 5.43 and 5.44 settled, @p registering_failed being the
/// REGISTERING_FAILED flag of Table 5.23.
[[nodiscard]] auto milan_rx_state_response(
    AcmpCommandResponse const& cmd, MilanSinkContext const& ctx, bool registering_failed) noexcept -> AcmpCommandResponse;

//
// Actions - one per treatment in 5.5.3.6
//

namespace milan_sink_actions {

void start_bound(MilanSinkContext& ctx, sm::TimePoint now);
void refuse_command(MilanSinkContext& ctx, sm::TimePoint now);
void update_binding(MilanSinkContext& ctx, sm::TimePoint now);
void bind_and_probe(MilanSinkContext& ctx, sm::TimePoint now);
void answer_unbound(MilanSinkContext& ctx, sm::TimePoint now);
void unbind(MilanSinkContext& ctx, sm::TimePoint now);
void answer_rx_state(MilanSinkContext& ctx, sm::TimePoint now);
void answer_rx_state_reserved(MilanSinkContext& ctx, sm::TimePoint now);
void start_delay(MilanSinkContext& ctx, sm::TimePoint now);
void delay_again(MilanSinkContext& ctx, sm::TimePoint now);
void send_probe(MilanSinkContext& ctx, sm::TimePoint now);
void resend_probe(MilanSinkContext& ctx, sm::TimePoint now);
void note_probe_timeout(MilanSinkContext& ctx, sm::TimePoint now);
void note_probe_rejected(MilanSinkContext& ctx, sm::TimePoint now);
void settle(MilanSinkContext& ctx, sm::TimePoint now);
void wait_for_talker(MilanSinkContext& ctx, sm::TimePoint now);
void note_discovered(MilanSinkContext& ctx, sm::TimePoint now);
void note_departed(MilanSinkContext& ctx, sm::TimePoint now);
void note_registered(MilanSinkContext& ctx, sm::TimePoint now);
void drop_srp_and_delay(MilanSinkContext& ctx, sm::TimePoint now);
void drop_srp_and_wait(MilanSinkContext& ctx, sm::TimePoint now);

}  // namespace milan_sink_actions

struct MilanSinkDef
{
    using Context = MilanSinkContext;
    using State = MilanSinkState;
    using Event = MilanSinkEvent;
};

//
// Transition table - Milan v1.3 Table 5.31
//

namespace milan_sink_table_detail {

using T = sm::Transitions<MilanSinkDef>;
using Table = sm::TransitionTable<MilanSinkDef>;
using Row = sm::Transition<MilanSinkContext, MilanSinkState>;
using State = MilanSinkState;
using Event = MilanSinkEvent;
namespace a = milan_sink_actions;

/// A bind in a bound state: the lock check, the same source again, another source.
consteval void add_bind_commands(Table& t, State const s)
{
    t.at(s, Event::BindRefused) = T::action<a::refuse_command>(s);
    t.at(s, Event::BindSameTalker) = T::action<a::update_binding>(s);
    t.at(s, Event::BindNewTalker) = T::action<a::bind_and_probe>(State::PrbWResp);
}

/// What a controller may send in any bound state. @p rx_state is the
/// state's GET_RX_STATE treatment.
consteval void add_bound_commands(Table& t, State const s, Row const rx_state)
{
    add_bind_commands(t, s);
    t.at(s, Event::GetRxState) = rx_state;
    t.at(s, Event::UnbindRefused) = T::action<a::refuse_command>(s);
    t.at(s, Event::Unbind) = T::action<a::unbind>(State::Unbound);
}

/// EVT_TK_DISCOVERED and EVT_TK_DEPARTED in a probing state past
/// PRB_W_AVAIL: discovery is noted, departure goes back to waiting.
consteval void add_probing_discovery(Table& t, State const s)
{
    t.at(s, Event::TalkerDiscovered) = T::action<a::note_discovered>(s);
    t.at(s, Event::TalkerDeparted) = T::action<a::wait_for_talker>(State::PrbWAvail);
}

/// UNBOUND: 5.5.3.6.2 to 5.5.3.6.5
consteval void add_unbound_rows(Table& t)
{
    t.at(State::Unbound, Event::StartBound) = T::action<a::start_bound>(State::PrbWAvail);
    t.at(State::Unbound, Event::BindRefused) = T::action<a::refuse_command>(State::Unbound);
    t.at(State::Unbound, Event::BindNewTalker) = T::action<a::bind_and_probe>(State::PrbWResp);
    t.at(State::Unbound, Event::GetRxState) = T::action<a::answer_rx_state>(State::Unbound);
    t.at(State::Unbound, Event::UnbindRefused) = T::action<a::refuse_command>(State::Unbound);
    t.at(State::Unbound, Event::Unbind) = T::action<a::answer_unbound>(State::Unbound);
}

/// PRB_W_AVAIL: 5.5.3.6.6 to 5.5.3.6.9. PRB_W_DELAY: 5.5.3.6.10 to 5.5.3.6.15
consteval void add_waiting_rows(Table& t)
{
    add_bound_commands(t, State::PrbWAvail, T::action<a::answer_rx_state>(State::PrbWAvail));
    t.at(State::PrbWAvail, Event::TalkerDiscovered) = T::action<a::start_delay>(State::PrbWDelay);
    add_bound_commands(t, State::PrbWDelay, T::action<a::answer_rx_state>(State::PrbWDelay));
    add_probing_discovery(t, State::PrbWDelay);
    t.at(State::PrbWDelay, Event::TmrDelay) = T::action<a::send_probe>(State::PrbWResp);
}

/// PRB_W_RESP (5.5.3.6.16 to .22) and PRB_W_RESP2 (.23 to .29) differ only
/// in what TMR_NO_RESP does, @p no_response.
consteval void add_response_rows(Table& t, State const s, Row const no_response)
{
    add_bound_commands(t, s, T::action<a::answer_rx_state>(s));
    add_probing_discovery(t, s);
    t.at(s, Event::ProbeAccepted) = T::action<a::settle>(State::SettledNoRsv);
    t.at(s, Event::ProbeRejected) = T::action<a::note_probe_rejected>(State::PrbWRetry);
    t.at(s, Event::TmrNoResp) = no_response;
}

/// PRB_W_RETRY: 5.5.3.6.30 to 5.5.3.6.35
consteval void add_retry_rows(Table& t)
{
    add_bound_commands(t, State::PrbWRetry, T::action<a::answer_rx_state>(State::PrbWRetry));
    add_probing_discovery(t, State::PrbWRetry);
    t.at(State::PrbWRetry, Event::TmrRetry) = T::action<a::delay_again>(State::PrbWDelay);
    t.at(State::PrbWRetry, Event::TmrRetryNoTalker) = T::action<a::wait_for_talker>(State::PrbWAvail);
}

/// SETTLED_NO_RSV: 5.5.3.6.36 to 5.5.3.6.42
consteval void add_settled_no_rsv_rows(Table& t)
{
    add_bound_commands(t, State::SettledNoRsv, T::action<a::answer_rx_state>(State::SettledNoRsv));
    t.at(State::SettledNoRsv, Event::TmrNoTk) = T::action<a::drop_srp_and_delay>(State::PrbWDelay);
    t.at(State::SettledNoRsv, Event::TmrNoTkNoTalker) = T::action<a::drop_srp_and_wait>(State::PrbWAvail);
    t.at(State::SettledNoRsv, Event::TalkerDiscovered) = T::action<a::note_discovered>(State::SettledNoRsv);
    t.at(State::SettledNoRsv, Event::TalkerDeparted) = T::action<a::note_departed>(State::SettledNoRsv);
    t.at(State::SettledNoRsv, Event::TalkerRegistered) = T::action<a::note_registered>(State::SettledRsvOk);
}

/// SETTLED_RSV_OK: 5.5.3.6.43 to 5.5.3.6.48
consteval void add_settled_rsv_ok_rows(Table& t)
{
    add_bound_commands(t, State::SettledRsvOk, T::action<a::answer_rx_state_reserved>(State::SettledRsvOk));
    t.at(State::SettledRsvOk, Event::TalkerDiscovered) = T::action<a::note_discovered>(State::SettledRsvOk);
    t.at(State::SettledRsvOk, Event::TalkerDeparted) = T::action<a::note_departed>(State::SettledRsvOk);
    t.at(State::SettledRsvOk, Event::TalkerUnregistered) = T::action<a::drop_srp_and_delay>(State::PrbWDelay);
    t.at(State::SettledRsvOk, Event::TalkerUnregisteredNoTalker) = T::action<a::drop_srp_and_wait>(State::PrbWAvail);
}

consteval void add_probing_rows(Table& t)
{
    add_waiting_rows(t);
    add_response_rows(t, State::PrbWResp, T::action<a::resend_probe>(State::PrbWResp2));
    add_response_rows(t, State::PrbWResp2, T::action<a::note_probe_timeout>(State::PrbWRetry));
    add_retry_rows(t);
}

}  // namespace milan_sink_table_detail

/// Build the sink transition table.
consteval auto make_milan_sink_table()
{
    sm::TransitionTable<MilanSinkDef> t{};
    milan_sink_table_detail::add_unbound_rows(t);
    milan_sink_table_detail::add_probing_rows(t);
    milan_sink_table_detail::add_settled_no_rsv_rows(t);
    milan_sink_table_detail::add_settled_rsv_ok_rows(t);
    return t;
}

inline constexpr auto milan_sink_table = make_milan_sink_table();

/// The Milan sink state machine.
template <typename Observer = sm::NullObserver>
using MilanSinkStateMachine = sm::StateMachine<MilanSinkDef, milan_sink_table, Observer>;

//
// From what arrives to the event it is
//

/// The event a BIND_RX_COMMAND, UNBIND_RX_COMMAND or GET_RX_STATE_COMMAND
/// is for a sink in @p ctx's state, nullopt for any other message type.
/// @p locked_out is true when the entity is locked by a controller other
/// than the command's (step 1 of every bind and unbind treatment).
[[nodiscard]] auto milan_sink_event_for_command(
    MilanSinkContext const& ctx, AcmpCommandResponse const& cmd, bool locked_out) noexcept -> std::optional<MilanSinkEvent>;

/// The event a PROBE_TX_RESPONSE is, nullopt when it does not answer the
/// PROBE_TX_COMMAND that was sent (5.5.3.6.18 step 1).
[[nodiscard]] auto milan_sink_event_for_probe_response(MilanSinkContext const& ctx, AcmpCommandResponse const& resp) noexcept
    -> std::optional<MilanSinkEvent>;

/// The event the expiry of the timer @p state runs is.
[[nodiscard]] auto milan_sink_timer_event(MilanSinkContext const& ctx, MilanSinkState state) noexcept
    -> std::optional<MilanSinkEvent>;

/// The event for SRP now registering (@p registered) or no longer
/// registering a Talker attribute that matches a settled sink's parameters,
/// nullopt when @p state already says so.
[[nodiscard]] auto milan_sink_registration_event(MilanSinkContext const& ctx, MilanSinkState state, bool registered) noexcept
    -> std::optional<MilanSinkEvent>;

}  // namespace statusbar::atdecc
