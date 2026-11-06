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
///     from the IEEE 1722 dynamic pool (91:E0:F0:00:00:00 .. :00:FD:FF), or
///     prepare(count) and let the first port_operational() begin it;
///   - port_operational(): B.3.5.9 Link Up — begin acquiring, or re-probe the
///     range held;
///   - on_acquired / on_lost callbacks: learn when the range becomes defended
///     (usable as a stream destination) or is lost to a conflict;
///   - receive(): feed a received MAAP frame (drives conflict handling);
///   - tick(): drive the probe/announce timers.
///
/// This mirrors the MvrpHandler/MsrpHandler facade pattern so a Pollable can host
/// it in the reactor (Phase 2). The handler is transport-agnostic: it frames PDUs
/// and hands the bytes and their destination to a `SendFn`. A PROBE or ANNOUNCE
/// goes to MAAP_MULTICAST_MAC; a DEFEND goes to the station whose PROBE it
/// answers (IEEE 1722-2025 B.2.1). Every PDU carries a zero stream_id (B.2.4).
///
/// Thread model: single-threaded, like the other protocol handlers — acquire(),
/// release(), tick(), and receive() must be serialized on one thread.

#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/avtp/avtp_maap_sm.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <optional>
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
    /// Send a framed (28-byte) MAAP PDU to @p dest: MAAP_MULTICAST_MAC for a PROBE
    /// or an ANNOUNCE, the prober's own MAC for a DEFEND (B.2.1). Return true on
    /// success.
    using SendFn = statusbar::sg14::inplace_function<bool(Eui48 const& dest, std::span<uint8_t const>), 64>;

    /// The contiguous range [start, start+count) is now defended and usable as a
    /// stream destination address. Told the @p now_ns of the tick()/receive()
    /// that defended it (the handler's own timeline).
    using AcquiredFn = statusbar::sg14::inplace_function<void(Eui48 const& start, uint16_t count, int64_t now_ns), 64>;

    /// A conflict forced us off [start, start+count) at now_ns (the handler's own timeline). The
    /// handler is already re-probing a freshly-picked range and will call the acquired callback
    /// again once it settles.
    using LostFn = statusbar::sg14::inplace_function<void(Eui48 const& start, uint16_t count, int64_t now_ns), 64>;

    /// @param our_mac    this station's MAC (conflict tie-break + self-frame filtering)
    /// @param seed       PRNG seed for address/jitter selection; 0 => derive from our_mac
    explicit MaapHandler(Eui48 const& our_mac, uint64_t seed = 0) noexcept;

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

    /// Name the size of the range to acquire and wait: the first
    /// port_operational() begins acquiring it (Table B.7, PortOperational! in
    /// INITIAL), the way acquire() would begin it now.
    void prepare(uint16_t count);

    /// prepare(count), with the start to try first: the range this station
    /// defended in an earlier life, saved and handed back. B.3.6.1 leaves the
    /// draw to the station, and a station that draws afresh on every start
    /// moves its streams to new destination addresses under the same
    /// StreamIDs - which a bridge still holding the old registration refuses
    /// (802.1Q-2018 35.2.2.8, Talker Failed code 16) until it lapses. The
    /// saved range is probed first, once: a conflict during that probe, or a
    /// defence lost later, draws afresh as always. A start the range would
    /// not fit in the pool from is ignored, and the draw is fresh.
    void prepare(uint16_t count, Eui48 const& start);

    /// B.3.5.9 PortOperational!: the port can transmit and receive AVTPDUs
    /// (again). Idle with a range prepared, this begins acquiring it. Probing or
    /// defending, it restarts the probe sequence on the range held — that range
    /// supplied back to the restart (Table B.7 note a) — so a range nobody took
    /// while the link was down is defended again and one that was taken is
    /// found out. A defended range is reported lost as its re-probe starts and
    /// acquired again when it completes. With nothing to acquire: nothing.
    void port_operational(int64_t now_ns);

    /// Release the current allocation and return to idle (stops defending/announcing).
    void release(int64_t now_ns);

    /// Drive the probe/announce timers. Call at or soon after
    /// next_deadline_ns(): a PDU goes out on the tick that finds its deadline
    /// passed, and the deadlines are drawn so a tick up to
    /// MAAP_LATE_TICK_ALLOWANCE_US late still lands it inside its window. The
    /// probe whose decrement empties maap_probe_count is followed by the
    /// ANNOUNCE in the same tick (B.3.6.3 probeCount!).
    void tick(int64_t now_ns);

    /// When the next tick() has something to send: the nearer of the armed
    /// probe and announce deadlines, in the caller's ns timebase, or nullopt
    /// while neither timer runs.
    [[nodiscard]] auto next_deadline_ns() const noexcept -> std::optional<int64_t>;

    /// How long a poll loop may wait before a deadline: the whole of
    /// @p max_ms when there is none or it is further off, else the time to it
    /// rounded up to the millisecond, and 0 once it has passed.
    [[nodiscard]] static auto poll_timeout_ms(std::optional<int64_t> deadline_ns, int64_t now_ns, int max_ms) noexcept -> int;

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

    /// Whether [start, start + count) lies within the dynamic pool.
    [[nodiscard]] static auto range_fits_pool(Eui48 const& start, uint16_t count) noexcept -> bool;

  private:
    void wire_callbacks();
    void wire_timer_callbacks();
    void wire_send_callbacks();

    /// Frame @p du and hand it to the send callback for @p dest.
    void send_pdu(Eui48 const& dest, MaapDu const& du);

    /// B.3.6.6 sDefend: the DEFEND answering the PROBE held in ctx_.last_received_pdu,
    /// echoing its requested range and naming the first of our addresses it conflicts
    /// with and how many of ours it covers.
    [[nodiscard]] auto make_defend() const noexcept -> MaapDu;

    /// The probe timer has run out: the next PROBE, and the ANNOUNCE after the last.
    void serve_probe_deadline(int64_t now_ns);

    /// Dispatch an SM event, firing on_acquired/on_lost on a Defend boundary.
    void dispatch(maap_sm::Def::Event ev, int64_t now_ns);

    /// Run the Start->Initial UCT, once, before the requested count is set.
    void prime();

    [[nodiscard]] auto next_random() noexcept -> uint64_t;
    void pick_address() noexcept;  // generate_address callback body
    /// A uniform draw of the start offset into the dynamic pool for a range of
    /// @p want addresses, so the whole range fits.
    [[nodiscard]] auto random_pool_offset(uint16_t want) noexcept -> uint64_t;
    [[nodiscard]] auto probe_interval_ns() noexcept -> int64_t;
    [[nodiscard]] auto announce_interval_ns() noexcept -> int64_t;
    /// A deadline strictly inside (BASE, BASE + VARIATION), with
    /// MAAP_LATE_TICK_ALLOWANCE_US to spare under the top.
    [[nodiscard]] auto interval_ns(uint64_t base_us, uint64_t variation_us) noexcept -> int64_t;

    /// The received PDU when it is a valid MAAP PDU from another station that
    /// conflicts with the range we are probing or defending; nullopt otherwise.
    [[nodiscard]] auto conflicting_pdu(Eui48 const& src_mac, std::span<uint8_t const> frame) const -> std::optional<MaapDu>;

    /// The SM event a conflicting PDU raises, after Table B.7's compare_MAC
    /// (B.3.6.4) has been applied; nullopt when the comparison says to ignore it.
    [[nodiscard]] auto conflict_event(MaapDu const& du, Eui48 const& src_mac) const -> std::optional<maap_sm::Def::Event>;

    [[nodiscard]] auto is_probing_or_defending() const noexcept -> bool
    {
        auto const st = sm_.current_state();
        return st == maap_sm::Def::State::Probe || st == maap_sm::Def::State::Defend;
    }

    /// True if [their_start, their_start+their_count) overlaps our current range.
    [[nodiscard]] auto overlaps(Eui48 const& their_start, uint16_t their_count) const noexcept -> bool;

    /// The part of our range that [their_start, their_start+their_count) covers:
    /// its first address and its length (B.2.7, B.2.8). Only meaningful when
    /// overlaps() holds.
    [[nodiscard]] auto conflict_range(Eui48 const& their_start, uint16_t their_count) const noexcept -> std::pair<Eui48, uint16_t>;

    maap_sm::Context ctx_{};
    maap_sm::Machine sm_{};
    uint64_t rng_{1};
    /// Set while a PortOperational! restart runs: generate_address keeps the
    /// range held instead of drawing a new one (Table B.7 note a).
    bool reprobe_held_range_{false};
    /// The start prepare() was handed, for the next draw and that one only.
    std::optional<Eui48> supplied_start_{};

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
