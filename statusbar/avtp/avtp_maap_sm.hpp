#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// MAAP State Machine - IEEE 1722-2025 Annex B.3
/// Implements the MAC Address Acquisition Protocol state machine
/// using the statusbar::sm framework.

#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <cstdint>
#include <functional>

namespace statusbar::avtp::maap_sm {

using namespace statusbar::sm;
using ieee::Eui48;

struct Context;

/// Callbacks for timer and PDU operations.
/// The integrating component provides these to connect the SM to the network and timer infrastructure.
struct Callbacks
{
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64>
        generate_address;  ///< B.3.6.1: Select random address range
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> start_probe_timer;  ///< B.3.4.2: Start probe period timer
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> stop_probe_timer;   ///< Stop probe timer
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64>
        start_announce_timer;  ///< B.3.4.1: Start announce period timer
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> stop_announce_timer;  ///< Stop announce timer
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_probe;           ///< B.3.6.5: Send MAAP_PROBE PDU
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_defend;          ///< B.3.6.6: Send MAAP_DEFEND PDU
    statusbar::sg14::inplace_function<void(Context& ctx, TimePoint), 64> send_announce;        ///< B.3.6.7: Send MAAP_ANNOUNCE PDU
};

/// State machine context - holds all mutable state for one MAAP address range allocation.
struct Context
{
    Callbacks callbacks{};
    Eui48 our_mac{};               ///< This station's MAC address (for compare_MAC)
    Eui48 requested_start{};       ///< Currently requested/acquired start address
    uint16_t requested_count{0};   ///< Number of addresses requested
    uint32_t maap_probe_count{0};  ///< Remaining probe retransmissions (B.3.6.2)
    MaapDu last_received_pdu{};    ///< Last received conflicting PDU (for sDefend fields)
    Eui48 last_received_sender{};  ///< Sender MAC of last received PDU (for compare_MAC)
};

/// IEEE 1722-2025 B.3.6.4: Octet-wise reverse order MAC comparison.
/// Returns true if our_mac wins (numerically lower in reverse-octet order).
/// When true, the receiving station has priority and the conflict is ignored.
[[nodiscard]] auto compare_mac(Eui48 const& our_mac, Eui48 const& received_mac) noexcept -> bool;

/// SM definition: states and events mapped from Tables B.3 and B.6
struct Def
{
    using Context = maap_sm::Context;

    /// Protocol states (Table B.6) plus Start for SM framework
    enum class State : uint8_t
    {
        Start = 0,
        Initial,
        Probe,
        Defend,
        Count
    };

    /// Protocol events (Table B.3, flattened)
    enum class Event : uint8_t
    {
        UCT = 0,
        Begin,
        Release,
        rProbe,
        rDefend,
        rAnnounce,
        ProbeCount,
        AnnounceTimer,
        ProbeTimer,
        PortOperational,
        Count
    };
};

// Action functions (Table B.5, flattened)

void init(Context& ctx, TimePoint time);
void begin_acquire(Context& ctx, TimePoint time);
void release(Context& ctx, TimePoint time);
void restart_probing(Context& ctx, TimePoint time);
void probe_complete(Context& ctx, TimePoint time);
void probe_tick(Context& ctx, TimePoint time);
void send_defend(Context& ctx, TimePoint time);
void announce_tick(Context& ctx, TimePoint time);

/// Constexpr transition table (flattened from Table B.7)
inline constexpr auto table = []() -> TransitionTable<Def> {
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};

    // Start → Initial (UCT)
    t.at(S::Start, E::UCT) = T::action<init>(S::Initial);

    // Initial: Begin and PortOperational start acquisition
    t.at(S::Initial, E::Begin) = T::transition(S::Probe);
    t.at(S::Initial, E::PortOperational) = T::transition(S::Probe);

    // Probe state transitions
    t.at(S::Probe, E::Release) = T::action<release>(S::Initial);
    t.at(S::Probe, E::rProbe) = T::action<restart_probing>(S::Probe);
    t.at(S::Probe, E::rDefend) = T::action<restart_probing>(S::Probe);
    t.at(S::Probe, E::rAnnounce) = T::action<restart_probing>(S::Probe);
    t.at(S::Probe, E::ProbeCount) = T::action<probe_complete>(S::Defend);
    t.at(S::Probe, E::ProbeTimer) = T::action<probe_tick>(S::Probe);
    t.at(S::Probe, E::PortOperational) = T::action<restart_probing>(S::Probe);

    // Defend state transitions
    t.at(S::Defend, E::Release) = T::action<release>(S::Initial);
    t.at(S::Defend, E::rProbe) = T::action<send_defend>(S::Defend);
    t.at(S::Defend, E::rDefend) = T::action<restart_probing>(S::Probe);
    t.at(S::Defend, E::rAnnounce) = T::action<restart_probing>(S::Probe);
    t.at(S::Defend, E::AnnounceTimer) = T::action<announce_tick>(S::Defend);
    t.at(S::Defend, E::PortOperational) = T::action<restart_probing>(S::Probe);

    // Exit hook: leaving Initial always begins address acquisition.
    t.on_exit(S::Initial) = T::hook<begin_acquire>();

    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::avtp::maap_sm
