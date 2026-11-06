// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_maap_handler.hpp"

#include <array>
#include <chrono>

namespace statusbar::avtp {

namespace {

using maap_sm::Context;
using sm::TimePoint;

/// Convert the SM's TimePoint back to the caller's ns timebase. The handler only
/// ever feeds TimePoints it built from caller ns, so this round-trips exactly.
[[nodiscard]] auto to_ns(TimePoint tp) noexcept -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] auto from_ns(int64_t ns) noexcept -> TimePoint
{
    return TimePoint{std::chrono::nanoseconds{ns}};
}

}  // namespace

MaapHandler::MaapHandler(Eui48 const& our_mac, StreamId const& stream_id, uint64_t seed) noexcept
    : stream_id_{stream_id}
    , rng_{seed != 0 ? seed : (our_mac.to_uint64() | 1U)}
{
    ctx_.our_mac = our_mac;
    wire_callbacks();
}

void MaapHandler::wire_callbacks()
{
    ctx_.callbacks.generate_address = [this](Context&, TimePoint) { pick_address(); };

    ctx_.callbacks.start_probe_timer = [this](Context&, TimePoint tp) {
        probe_deadline_ns_ = to_ns(tp) + probe_interval_ns();
        probe_timer_active_ = true;
    };
    ctx_.callbacks.stop_probe_timer = [this](Context&, TimePoint) { probe_timer_active_ = false; };

    ctx_.callbacks.start_announce_timer = [this](Context&, TimePoint tp) {
        announce_deadline_ns_ = to_ns(tp) + announce_interval_ns();
        announce_timer_active_ = true;
    };
    ctx_.callbacks.stop_announce_timer = [this](Context&, TimePoint) { announce_timer_active_ = false; };

    ctx_.callbacks.send_probe = [this](Context& c, TimePoint) {
        MaapDu du{};
        du.init_probe(stream_id_, c.requested_start, c.requested_count);
        std::array<uint8_t, MaapDu::LENGTH> buf{};
        (void)store_unchecked(buf, du);
        if (send_) {
            (void)send_(buf);
        }
    };

    ctx_.callbacks.send_announce = [this](Context& c, TimePoint) {
        MaapDu du{};
        du.init_announce(stream_id_, c.requested_start, c.requested_count);
        std::array<uint8_t, MaapDu::LENGTH> buf{};
        (void)store_unchecked(buf, du);
        if (send_) {
            (void)send_(buf);
        }
    };

    ctx_.callbacks.send_defend = [this](Context& c, TimePoint) {
        // B.3.6.6: DEFEND echoes the conflicting station's requested range in the
        // conflict fields, taken from the PDU that triggered the defense.
        MaapDu du{};
        du.init_defend(
            stream_id_,
            c.requested_start,
            c.requested_count,
            c.last_received_pdu.requested_start_address,
            c.last_received_pdu.requested_count.get());
        std::array<uint8_t, MaapDu::LENGTH> buf{};
        (void)store_unchecked(buf, du);
        if (send_) {
            (void)send_(buf);
        }
    };
}

void MaapHandler::acquire(uint16_t count, int64_t now_ns)
{
    // The very first handle_event runs the Start->Initial UCT, whose init() action
    // resets requested_count. Prime that transition with a benign event (Release is
    // a no-op in Initial) BEFORE setting the count, so our count survives into
    // begin_acquire's generate_address.
    if (sm_.current_state() == maap_sm::Def::State::Start) {
        sm_.handle_event(ctx_, maap_sm::Def::Event::Release, from_ns(now_ns));
    }
    ctx_.requested_count = count;  // pick_address bounds its offset by this
    dispatch(maap_sm::Def::Event::Begin, now_ns);
}

void MaapHandler::release(int64_t now_ns)
{
    dispatch(maap_sm::Def::Event::Release, now_ns);
}

void MaapHandler::tick(int64_t now_ns)
{
    using E = maap_sm::Def::Event;

    if (probe_timer_active_ && now_ns >= probe_deadline_ns_) {
        probe_timer_active_ = false;  // the SM action re-arms if it needs another probe
        // While probes remain, keep probing; once exhausted with no conflict, claim it.
        if (ctx_.maap_probe_count > 0) {
            dispatch(E::ProbeTimer, now_ns);
        } else {
            dispatch(E::ProbeCount, now_ns);
        }
    }

    if (announce_timer_active_ && now_ns >= announce_deadline_ns_) {
        announce_timer_active_ = false;
        dispatch(E::AnnounceTimer, now_ns);
    }
}

auto MaapHandler::next_deadline_ns() const noexcept -> std::optional<int64_t>
{
    std::optional<int64_t> next{};
    if (probe_timer_active_) {
        next = probe_deadline_ns_;
    }
    if (announce_timer_active_ && (!next || announce_deadline_ns_ < *next)) {
        next = announce_deadline_ns_;
    }
    return next;
}

auto MaapHandler::poll_timeout_ms(std::optional<int64_t> const deadline_ns, int64_t const now_ns, int const max_ms) noexcept -> int
{
    if (!deadline_ns) {
        return max_ms;
    }
    int64_t const remaining_ns = *deadline_ns - now_ns;
    if (remaining_ns <= 0) {
        return 0;
    }
    int64_t const remaining_ms = (remaining_ns + 999'999) / 1'000'000;  // up, so a wait never ends short of it
    return remaining_ms < max_ms ? static_cast<int>(remaining_ms) : max_ms;
}

void MaapHandler::receive(Eui48 const& src_mac, std::span<uint8_t const> frame, int64_t now_ns)
{
    auto const du = conflicting_pdu(src_mac, frame);
    if (!du) {
        return;
    }
    ctx_.last_received_pdu = *du;
    ctx_.last_received_sender = src_mac;
    if (auto const ev = conflict_event(*du, src_mac)) {
        dispatch(*ev, now_ns);
    }
}

auto MaapHandler::conflicting_pdu(Eui48 const& src_mac, std::span<uint8_t const> frame) const -> std::optional<MaapDu>
{
    // Our own frames (egress tap), frames received while we hold no allocation
    // and frames received while idle carry no conflict for us (Table B.7 note b).
    if (src_mac == ctx_.our_mac || ctx_.requested_count == 0 || !is_probing_or_defending()) {
        return std::nullopt;
    }
    MaapDu du{};
    if (statusbar::protocol::load(frame, &du) == 0 || !du.is_valid()) {
        return std::nullopt;
    }
    if (!overlaps(du.requested_start_address, du.requested_count.get())) {
        return std::nullopt;  // different address range: not our conflict
    }
    return du;
}

auto MaapHandler::conflict_event(MaapDu const& du, Eui48 const& src_mac) const -> std::optional<maap_sm::Def::Event>
{
    using Def = maap_sm::Def;
    bool const defending = sm_.current_state() == Def::State::Defend;
    // B.3.6.4: compare_MAC is TRUE when our MAC is the lower in octet-wise reverse
    // order, and then "no further processing or protocol action is taken".
    bool const we_win = maap_sm::compare_mac(ctx_.our_mac, src_mac);
    if (du.is_probe()) {
        // Table B.7 rProbe: while probing, compare_MAC decides who yields; while
        // defending, every probe of our range is answered with a DEFEND.
        return (!defending && we_win) ? std::nullopt : std::optional{Def::Event::rProbe};
    }
    // Table B.7 rDefend/rAnnounce: a defender runs compare_MAC first (note d) and
    // the lower MAC keeps the range; a prober yields to either without comparing.
    if (defending && we_win) {
        return std::nullopt;
    }
    return du.is_defend()  ? std::optional{Def::Event::rDefend}
        : du.is_announce() ? std::optional{Def::Event::rAnnounce}
                           : std::nullopt;
}

void MaapHandler::dispatch(maap_sm::Def::Event ev, int64_t now_ns)
{  // NOLINT(statusbar-block-statements) the dispatch: snapshot, step, report the Defend boundary; pre-dates the gate
    using S = maap_sm::Def::State;

    auto const before = sm_.current_state();
    // Snapshot the current range: restart_probing re-picks the address in place,
    // so on_lost must report the address we are leaving, not the new one.
    Eui48 const prev_start = ctx_.requested_start;
    uint16_t const prev_count = ctx_.requested_count;

    sm_.handle_event(ctx_, ev, from_ns(now_ns));

    auto const after = sm_.current_state();
    if (before != S::Defend && after == S::Defend) {
        if (on_acquired_) {
            on_acquired_(ctx_.requested_start, ctx_.requested_count, now_ns);
        }
    } else if (before == S::Defend && after != S::Defend) {
        if (on_lost_) {
            on_lost_(prev_start, prev_count, now_ns);
        }
    }
}

auto MaapHandler::next_random() noexcept -> uint64_t
{
    // xorshift64 — deterministic given the seed, adequate spread for address/jitter.
    uint64_t x = rng_;
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    rng_ = x;
    return x;
}

void MaapHandler::pick_address() noexcept
{
    // B.3.6.1: pick a random start so [start, start+count) fits in the dynamic pool.
    uint64_t const pool_start = MAAP_DYNAMIC_POOL_START.to_uint64();
    uint64_t const pool_end = MAAP_DYNAMIC_POOL_END.to_uint64();
    uint64_t const pool_span = (pool_end - pool_start) + 1U;

    uint16_t const want = ctx_.requested_count == 0 ? 1U : ctx_.requested_count;
    uint64_t const max_offset = want <= pool_span ? pool_span - want : 0U;

    uint64_t const offset = max_offset == 0 ? 0U : next_random() % (max_offset + 1U);
    ctx_.requested_start.from_uint64(pool_start + offset);
}

auto MaapHandler::probe_interval_ns() noexcept -> int64_t
{
    return interval_ns(MAAP_PROBE_INTERVAL_BASE_US, MAAP_PROBE_INTERVAL_VARIATION_US);
}

auto MaapHandler::announce_interval_ns() noexcept -> int64_t
{
    return interval_ns(MAAP_ANNOUNCE_INTERVAL_BASE_US, MAAP_ANNOUNCE_INTERVAL_VARIATION_US);
}

auto MaapHandler::interval_ns(uint64_t const base_us, uint64_t const variation_us) noexcept -> int64_t
{
    // B.3.4.1 / B.3.4.2: BASE < T < BASE + VARIATION, both ends open. The draw
    // is uniform over [BASE + 1 us, BASE + VARIATION - ALLOWANCE - 1 us], so
    // the PDU a late tick sends up to ALLOWANCE after the deadline is still
    // inside the window.
    uint64_t const room_us = variation_us - MAAP_LATE_TICK_ALLOWANCE_US - 1;
    uint64_t const interval_us = base_us + 1 + (next_random() % room_us);
    return static_cast<int64_t>(interval_us) * 1000;
}

auto MaapHandler::overlaps(Eui48 const& their_start, uint16_t their_count) const noexcept -> bool
{
    if (ctx_.requested_count == 0 || their_count == 0) {
        return false;
    }
    uint64_t const our0 = ctx_.requested_start.to_uint64();
    uint64_t const our1 = our0 + ctx_.requested_count;
    uint64_t const their0 = their_start.to_uint64();
    uint64_t const their1 = their0 + their_count;
    return our0 < their1 && their0 < our1;
}

}  // namespace statusbar::avtp
