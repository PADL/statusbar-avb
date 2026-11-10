#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// MaapHandler — a driver/facade over the MAAP state machine (maap_sm::Machine).
///
/// The state machine (avtp_maap_sm.hpp) is a pure protocol algorithm: it calls
/// out through a `Context::callbacks` struct for every timer and PDU operation
/// and reports progress only by its state + context fields. MaapHandler owns the
/// SM and Context, implements those eight callbacks (address selection, the two
/// timers, and the three PDU sends), and exposes a small entity-facing API:
///
///   - acquire(count): begin claiming `count` contiguous multicast addresses
///     from the IEEE 1722 dynamic pool (91:E0:F0:00:00:00 .. :00:FD:FF);
///   - on_acquired / on_lost callbacks: learn when the range becomes defended
///     (usable as a stream destination) or is lost to a conflict;
///   - receive(): feed a received MAAP frame (drives conflict handling);
///   - tick(): drive the probe/announce timers.
///
/// This mirrors the MvrpHandler/MsrpHandler facade pattern so a Pollable can host
/// it in the reactor (Phase 2). The handler is transport-agnostic: it frames PDUs
/// and hands the bytes to a `SendFn`; the caller sends them to MAAP_MULTICAST_MAC.
///
/// Thread model: single-threaded, like the other protocol handlers — acquire(),
/// release(), tick(), and receive() must be serialized on one thread.

#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/avtp/avtp_maap_sm.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <span>
#include <utility>

namespace statusbar::avtp {

using ieee::Eui48;
using tsn::StreamId;

/// The @p offset-th address of a MAAP-acquired contiguous block starting at
/// @p block_start. A talker assigns block_start+0, +1, ... to its streams.
[[nodiscard]] inline auto maap_block_address(Eui48 const& block_start, uint16_t offset) noexcept -> Eui48
{
    Eui48 addr{};
    addr.from_uint64(block_start.to_uint64() + offset);
    return addr;
}

class MaapHandler
{
  public:
    /// Send a framed (28-byte) MAAP PDU. The caller sends it to MAAP_MULTICAST_MAC.
    /// Return true on success.
    using SendFn = statusbar::sg14::inplace_function<bool(std::span<uint8_t const>), 64>;

    /// The contiguous range [start, start+count) is now defended and usable as a
    /// stream destination address. Told the @p now_ns of the tick()/receive()
    /// that defended it (the handler's own timeline).
    using AcquiredFn = statusbar::sg14::inplace_function<void(Eui48 const& start, uint16_t count, int64_t now_ns), 64>;

    /// A conflict forced us off [start, start+count) at now_ns (the handler's own timeline). The
    /// handler is already re-probing a freshly-picked range and will call the acquired callback
    /// again once it settles.
    using LostFn = statusbar::sg14::inplace_function<void(Eui48 const& start, uint16_t count, int64_t now_ns), 64>;

    /// @param our_mac    this station's MAC (conflict tie-break + self-frame filtering)
    /// @param stream_id  stream id placed in emitted MAAP PDUs
    /// @param seed       PRNG seed for address/jitter selection; 0 => derive from our_mac
    explicit MaapHandler(Eui48 const& our_mac, StreamId const& stream_id, uint64_t seed = 0) noexcept;

    // Self-referential: the SM callbacks (ctx_.callbacks) capture `this`, so a
    // default move/copy would leave them pointing at the moved-from object. The
    // handler is constructed in place and never relocated.
    MaapHandler(MaapHandler const&) = delete;
    auto operator=(MaapHandler const&) -> MaapHandler& = delete;
    MaapHandler(MaapHandler&&) = delete;
    auto operator=(MaapHandler&&) -> MaapHandler& = delete;

    void set_send(SendFn fn) noexcept { send_ = std::move(fn); }
    void set_on_acquired(AcquiredFn fn) noexcept { on_acquired_ = std::move(fn); }
    void set_on_lost(LostFn fn) noexcept { on_lost_ = std::move(fn); }

    /// Begin acquiring @p count contiguous addresses from the dynamic pool.
    /// Picks a random start, sends the first PROBE, and starts the probe timer.
    void acquire(uint16_t count, int64_t now_ns);

    /// Release the current allocation and return to idle (stops defending/announcing).
    void release(int64_t now_ns);

    /// The longest gap between tick() calls the timer draws leave room for, so a send stays inside
    /// IEEE 1722-2016 Annex B.3.4's interval when a tick is late (ns).
    static constexpr int64_t MAX_TICK_PERIOD_NS = 10'000'000;

    /// Drive the probe/announce timers. Call at least every MAX_TICK_PERIOD_NS (e.g. every reactor tick).
    void tick(int64_t now_ns);

    /// Feed a received Ethernet MAAP frame from @p src_mac (AVTP payload, i.e.
    /// starting at the subtype octet).
    void receive(Eui48 const& src_mac, std::span<uint8_t const> frame, int64_t now_ns);

    /// True once the requested range is being defended (acquired and usable).
    [[nodiscard]] auto is_acquired() const noexcept -> bool { return sm_.current_state() == maap_sm::Def::State::Defend; }

    /// The current start address (meaningful once is_acquired()).
    [[nodiscard]] auto address() const noexcept -> Eui48 { return ctx_.requested_start; }

    /// The number of addresses in the current range.
    [[nodiscard]] auto count() const noexcept -> uint16_t { return ctx_.requested_count; }

    /// The raw SM state (for diagnostics/tests).
    [[nodiscard]] auto state() const noexcept -> maap_sm::Def::State { return sm_.current_state(); }

  private:
    void wire_callbacks();

    /// Dispatch an SM event, firing on_acquired/on_lost on a Defend boundary.
    void dispatch(maap_sm::Def::Event ev, int64_t now_ns);

    [[nodiscard]] auto next_random() noexcept -> uint64_t;
    void pick_address() noexcept;  // generate_address callback body
    [[nodiscard]] auto probe_interval_ns() noexcept -> int64_t;
    [[nodiscard]] auto announce_interval_ns() noexcept -> int64_t;

    /// True if [their_start, their_start+their_count) overlaps our current range.
    [[nodiscard]] auto overlaps(Eui48 const& their_start, uint16_t their_count) const noexcept -> bool;

    maap_sm::Context ctx_{};
    maap_sm::Machine sm_{};
    StreamId stream_id_{};
    uint64_t rng_{1};

    // Timer deadlines in the caller's ns timebase; *_active_ gates them.
    int64_t probe_deadline_ns_{0};
    int64_t announce_deadline_ns_{0};
    bool probe_timer_active_{false};
    bool announce_timer_active_{false};

    SendFn send_{};
    AcquiredFn on_acquired_{};
    LostFn on_lost_{};
};

}  // namespace statusbar::avtp
