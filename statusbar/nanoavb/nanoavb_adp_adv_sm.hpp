#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The ADP Advertise state machine of one AVB interface: DOWN, WAITING and DELAY per Milan
/// v1.2 Section 5.6.3, the same machine as IEEE 1722.1-2021 Clause 6.2.4 with link state.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace statusbar::nanoavb::adp_adv_sm {

using namespace statusbar::sm;

/// randomDeviceDelay()'s generator (IEEE 1722.1-2021 Clause 6.2.4.2.2): xorshift64*,
/// period 2^64 - 1, no heap.
class DelayRandom
{
  public:
    /// Seed from the ones'-complement sum of @p mac and the real-time clock @p realtime_ns.
    void seed(ieee::Eui48 const& mac, uint64_t realtime_ns) noexcept;

    /// A delay uniform over [0, @p max] in whole milliseconds.
    [[nodiscard]] auto next(std::chrono::milliseconds max) noexcept -> std::chrono::milliseconds;

  private:
    uint64_t state_{0x9E37'79B9'7F4A'7C15ULL};
};

/// CLOCK_REALTIME in nanoseconds, the clock that seeds DelayRandom.
[[nodiscard]] auto realtime_ns() noexcept -> uint64_t;

/// The machine's variables. Actions set do_advertise / do_terminate; the owner transmits
/// (the Advertising Interface state machine, IEEE 1722.1-2021 Clause 6.2.5).
struct Context
{
    DelayRandom random{};
    std::chrono::milliseconds advertise_interval{5000};  ///< TMR_ADVERTISE
    std::chrono::milliseconds delay_max{4000};           ///< TMR_DELAY's range
    std::chrono::milliseconds startup_delay_max{2000};   ///< TMR_DELAY at startup with link up
    std::optional<TimePoint> advertise_at{};             ///< TMR_ADVERTISE deadline, if running
    std::optional<TimePoint> delay_until{};              ///< TMR_DELAY deadline, if running
    bool do_advertise{false};
    bool do_terminate{false};
};

struct Def
{
    using Context = adp_adv_sm::Context;
    enum class State : uint8_t
    {
        Start = 0,  ///< Started, link state not yet reported
        Down,
        Waiting,
        Delay,
        Count
    };
    enum class Event : uint8_t
    {
        LinkUp = 0,
        LinkDown,
        RcvDiscover,
        TmrAdvertise,
        TmrDelay,
        GmChange,
        EntityChange,
        Shutdown,
        Count
    };
};

/// Start TMR_DELAY over [0, startup_delay_max] (Milan v1.2 Section 5.6.3.5.2).
void start_startup_delay(Context& ctx, TimePoint time) noexcept;

/// Stop TMR_ADVERTISE, start TMR_DELAY over [0, delay_max] (Milan v1.2 Section 5.6.3.5.4).
void start_delay(Context& ctx, TimePoint time) noexcept;

/// Send ENTITY_AVAILABLE and start TMR_ADVERTISE (Milan v1.2 Section 5.6.3.5.9).
void advertise(Context& ctx, TimePoint time) noexcept;

/// Stop both timers (Milan v1.2 Sections 5.6.3.5.6 and 5.6.3.5.10).
void stop_timers(Context& ctx, TimePoint time) noexcept;

/// Stop both timers and send ENTITY_DEPARTING (Milan v1.2 Sections 5.6.3.5.8 and 5.6.3.5.11).
void depart(Context& ctx, TimePoint time) noexcept;

//
// Transitions - Milan v1.2 Section 5.6.3.5, Table 5.51
//
inline constexpr auto table = []() -> TransitionTable<Def> {  // NOLINT(statusbar-block-statements): one row per transition
    using S = Def::State;
    using E = Def::Event;
    using T = Transitions<Def>;
    TransitionTable<Def> t{};
    t.at(S::Start, E::LinkDown) = T::transition(S::Down);
    t.at(S::Start, E::LinkUp) = T::action<start_startup_delay>(S::Delay);
    t.at(S::Down, E::LinkUp) = T::action<start_delay>(S::Delay);

    t.at(S::Waiting, E::RcvDiscover) = T::action<start_delay>(S::Delay);
    t.at(S::Waiting, E::TmrAdvertise) = T::action<start_delay>(S::Delay);
    t.at(S::Waiting, E::GmChange) = T::action<start_delay>(S::Delay);
    // IEEE 1722.1-2021 Clause 6.2.7: other advertised changes set needsAdvertise too.
    t.at(S::Waiting, E::EntityChange) = T::action<start_delay>(S::Delay);
    t.at(S::Waiting, E::LinkDown) = T::action<stop_timers>(S::Down);
    t.at(S::Waiting, E::Shutdown) = T::action<depart>(S::Start);

    t.at(S::Delay, E::TmrDelay) = T::action<advertise>(S::Waiting);
    t.at(S::Delay, E::LinkDown) = T::action<stop_timers>(S::Down);
    t.at(S::Delay, E::Shutdown) = T::action<depart>(S::Start);
    return t;
}();

using Machine = StateMachine<Def, table>;

}  // namespace statusbar::nanoavb::adp_adv_sm
