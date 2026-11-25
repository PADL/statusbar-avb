#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// One Milan v1.3 STREAM_INPUT: its connection state machine (5.5.3) and its
/// discovery state machine (5.6.4) run together.
///
/// The two machines are separate tables, as the specification has them.
/// This class is the wiring between them and to the outside: it turns what
/// arrives (an ACMP PDU, an ADP PDU, a timer falling due, a change in what
/// SRP registers) into the event it is, runs the machine it belongs to, and
/// carries one machine's requests to the other.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"
#include "statusbar/atdecc/atdecc_milan_adp_discovery_sm.hpp"
#include "statusbar/sm/sm.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::atdecc {

class MilanSink
{
  public:
    using TimePoint = sm::TimePoint;

    /// Give the sink its listener and its STREAM_INPUT index. Once, before
    /// anything else; @p env must outlive the sink and stay where it is.
    void attach(MilanListenerEnv& env, uint16_t index) noexcept;

    /// Startup with a saved binding (5.5.3.6.2): the sink waits in
    /// PRB_W_AVAIL for its talker to be discovered. Whatever the sink held
    /// before is dropped without a word to any controller.
    void restore(MilanBinding const& binding, TimePoint now);

    /// The entity starts again: a bound sink is restored from its own
    /// binding, an unbound one stays unbound.
    void restart(TimePoint now);

    /// A BIND_RX_COMMAND, UNBIND_RX_COMMAND or GET_RX_STATE_COMMAND for this
    /// sink. @p locked_out: the entity is locked by another controller.
    void on_command(AcmpCommandResponse const& cmd, bool locked_out, TimePoint now);

    /// A PROBE_TX_RESPONSE naming this sink.
    void on_probe_response(AcmpCommandResponse const& resp, TimePoint now);

    /// An ADP message. Ignored unless it is from the talker the sink is
    /// bound to; @p own is this listener's gPTP state on the port.
    void on_adp(AdpDu const& adp, MilanGptpState const& own, TimePoint now);

    /// What SRP registers may have changed: a settled sink looks again
    /// (EVT_TK_REGISTERED, EVT_TK_UNREGISTERED).
    void on_srp_changed(TimePoint now);

    /// Run the timers that have fallen due at @p now.
    void tick(TimePoint now);

    /// The earliest of the sink's timers, nullopt with none running.
    [[nodiscard]] auto next_deadline() const noexcept -> std::optional<TimePoint>;

    [[nodiscard]] auto state() const noexcept -> MilanSinkState { return sm_.current_state(); }
    [[nodiscard]] auto discovery_state() const noexcept -> MilanDiscoveryState { return discovery_.current_state(); }

    /// The sink's variables: binding, SRP parameters, Probing status, ACMP status.
    [[nodiscard]] auto context() const noexcept -> MilanSinkContext const& { return ctx_; }

    [[nodiscard]] auto is_bound() const noexcept -> bool { return ctx_.binding.has_value(); }
    [[nodiscard]] auto is_settled() const noexcept -> bool
    {
        return state() == MilanSinkState::SettledNoRsv || state() == MilanSinkState::SettledRsvOk;
    }

  private:
    /// Run @p event, then what it asked of discovery, then look at SRP.
    void handle(MilanSinkEvent event, TimePoint now);
    void apply_discovery_requests(TimePoint now);
    /// Run @p event on the discovery machine and tell the connection machine what it triggers.
    void run_discovery(MilanDiscoveryEvent event, TimePoint now);
    void check_registration(TimePoint now);
    void expire_timer(TimePoint now);
    /// Drop everything the sink holds and return both machines to their first state.
    void reset(TimePoint now);

    MilanSinkContext ctx_{};
    MilanDiscoveryContext discovery_ctx_{};
    MilanSinkStateMachine<> sm_{};
    MilanDiscoveryStateMachine<> discovery_{};
};

}  // namespace statusbar::atdecc
