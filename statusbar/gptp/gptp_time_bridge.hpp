#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// GptpTimeBridge — bidirectional conversion between gPTP master time
// and any application-side POSIX clock.
//
// Usage:
//
//   // Setup: bind the two clocks and take one cross-timestamp.
//   GptpTimeBridge bridge{
//       .get_gptp_time_ns = [&soft_clock]() { return soft_clock.get_ptp_time_ns(realtime_ns()); },
//       .get_app_time_ns  = []() { return read_monotonic_raw_ns(); },
//   };
//   bridge.update();
//
//   // On every Sync+FollowUp pairing, TOLD the pair: the app-clock instant
//   // of the Sync's arrival and the master time it carried. The bridge
//   // reads no clock here (gptp_slave_session wires this).
//   bridge.update_from_anchor(sync_rx_app_ns, tx_master_ns);
//
//   // Conversions are told an instant and read nothing:
//   int64_t play_at_app = bridge.from_gptp(avtp_header.get_timestamp());
//   int64_t play_at_app = bridge.from_gptp_rated(avtp_header.get_timestamp());
//
// The bridge keeps a cached offset between the two clocks plus a rate
// measurement. The rated conversions extrapolate from the last anchor
// and hold sub-ns accuracy for typical drift rates and intervals up to
// a few seconds; the unrated offset drifts by (rate x interval) between
// anchors. update() reads both clocks itself and is the setup path;
// refresh_offset() likewise, for exit-time diagnostics.
//
// Time domains: the app clock is whichever clock the owner binds, by
// policy CLOCK_MONOTONIC_RAW (the timing reference, see
// realtime::MonotonicRawClock) so that a RAW instant goes straight
// through to_gptp(). The master side is gPTP TAI nanoseconds.
//
// Rate representation — int64 picoseconds-per-second:
//
//   rate_offset_ppt is (rate_ratio − 1) × 10¹², stored as int64. This
//   avoids the precision wart of representing a value very close to 1.0
//   in IEEE 754 double — most of a double's 52 mantissa bits would be
//   wasted on the constant `1.` and the leading zeros. With ppt:
//     - exact integer storage: no fractional accumulation
//     - native atomicity (8-byte aligned int64 is hardware-atomic on
//       aarch64 / x86_64)
//     - self-documenting: 1 ppt = 1 picosecond per second = 10⁻¹²
//     - range ±9.2 million ppm (way more than oscillator stability)
//
// Quality and coasting: quality() says how well the bridge's line is known,
// as a realtime::ClockQuality. While Syncs arrive it is refreshed at every
// anchor: the bound is the furthest any recent anchor landed from the line
// that was predicting it. When Syncs stop nothing changes state: the rated
// conversions keep extrapolating with the offset and rate they have, and
// the quality's uncertainty grows from the last anchor at the drift bound,
// so whoever uses the time can see how long it stays good enough for them.
// A master step or restart_timeline() is a different matter: the epoch
// changes and the bound starts again.
//
// Thread safety: the four cached fields (offset, rate_offset, prev_app,
// prev_gptp) are published together under a single-writer seqlock
// (`rated_seq_`). Readers that need a consistent snapshot of the rated
// regression line (e.g. `to_gptp_rated` / `from_gptp_rated`) load all
// four under the seqlock; readers that only need the unrated offset
// (e.g. `to_gptp` / `from_gptp`) read `offset_ns_` directly with a
// single atomic load and are unaffected. The seqlock guards against
// the failure mode where a writer's update is observed mid-publish —
// in the worst case (a phase-jump update that bumps offset by the step
// size while leaving rate untouched), a torn read of OLD offset paired
// with NEW prev_a yields an error of exactly `−step`, i.e. potentially
// 100 ms+ for a real servo step.
//

#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/itc/itc_seqlock_value.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace statusbar::gptp {

struct GptpTimeBridge
{
    /// Function returning the current gPTP master time in nanoseconds.
    /// For the SoftClock path:
    ///   [&sc]() { return sc.get_ptp_time_ns(clock_realtime_ns()); }
    /// For the PHC path:
    ///   ops.get_local_time_ns  (the PHC IS the gPTP clock)
    statusbar::sg14::inplace_function<int64_t(), 64> get_gptp_time_ns;

    /// Function returning the application-side clock in ns. That clock is
    /// CLOCK_MONOTONIC_RAW and nothing else: the one reference every timer,
    /// stamp and deadline in the application uses, and the clock the
    /// bridge's typed conversions and its quality name.
    ///   []() { return realtime::read_monotonic_raw_ns(); }
    statusbar::sg14::inplace_function<int64_t(), 64> get_app_time_ns;

    /// How fast the line may go wrong while no Sync arrives, in parts per
    /// billion, unless the anchors themselves showed the rate moving more:
    /// the deployment's oscillator and temperature figure.
    uint32_t holdover_drift_ppb{realtime::DEFAULT_HOLDOVER_DRIFT_PPB};

    /// Where the domain's time comes from, as its grandmaster announces it.
    /// The owner sets it from the Announce; until then, a PTP grandmaster.
    realtime::TimeSource time_source{realtime::TimeSource::ptp};

    using RawTimePoint = realtime::MonotonicRawClock::time_point;
    using GptpTimePoint = realtime::GptpClock0::time_point;

    /// Refresh the cached offset and rate. Call this from the event loop
    /// after each servo pass (every sync interval), or at least once
    /// per second. The first update() seeds the previous-sample
    /// storage; rate_offset_ppt() returns 0 until the second update()
    /// arrives.
    void update() noexcept
    {
        if (!get_gptp_time_ns || !get_app_time_ns) {
            return;
        }
        // Read both clocks as close together as possible. The order
        // (app first, gptp second) minimizes the systematic bias: any
        // delay between the two reads is attributed to the offset,
        // which is corrected on the next update().
        int64_t const app = get_app_time_ns();
        int64_t const gptp = get_gptp_time_ns();

        int64_t new_ppt = rate_offset_ppt_.load(std::memory_order_relaxed);
        if (have_previous_) {
            int64_t const gptp_delta = gptp - prev_gptp_ns_.load(std::memory_order_relaxed);
            int64_t const app_delta = app - prev_app_ns_.load(std::memory_order_relaxed);
            int64_t const skew_ns = gptp_delta - app_delta;
            // Detect a clock step on the gPTP side. Real natural drift
            // is at most a few ms over an update interval (1000 ppm ×
            // 1 s = 1 ms); a skew of 100+ ms in a single update means
            // the gPTP clock was stepped (e.g. by the servo's
            // phase_jump path). Skip the rate computation for this
            // update so the rate doesn't spike — the previous rate
            // estimate stays valid because the natural oscillator
            // drift didn't actually change. Mirrors gptp_servo.cpp's
            // suppress_for_negative_time_jump pattern.
            constexpr int64_t step_detection_threshold_ns = 100'000'000;  // 100 ms
            bool const looks_like_step =
                (app_delta <= 0) || (skew_ns < -step_detection_threshold_ns) || (skew_ns > step_detection_threshold_ns);
            if (!looks_like_step) {
                // skew_ns is small by construction. The ratio
                // (skew × 1e12 / app_delta) is the rate offset in
                // ppt. Computed via double rather than int128 to stay
                // portable; the precision concern that motivated int64
                // ppt STORAGE doesn't apply here because the result
                // (a small correction value) is far from 1.0, so all
                // 15+ significant digits land in the meaningful range.
                double const ppt_d = (static_cast<double>(skew_ns) * 1e12) / static_cast<double>(app_delta);
                new_ppt = static_cast<int64_t>(ppt_d);
            }
        }
        publish_anchor(app, gptp, new_ppt);
    }

    /// Refresh the cached anchor + rate from one Sync+FollowUp pairing,
    /// TOLD both sides of the pair: @p sync_rx_raw is the CLOCK_MONOTONIC_RAW
    /// instant of the Sync's arrival (read once by the receive path, as
    /// early as user space can, and carried with the Sync until its
    /// FollowUp pairs it); @p tx_master is the grandmaster TAI time
    /// the FollowUp carried for that Sync (correctionField and
    /// rate-corrected link delay applied, i.e. master TAI at arrival).
    /// The bridge reads no clock here. Reading the app clock at this
    /// call instead would pair the master time of the Sync with the app
    /// time of the FollowUp's processing, a bias of the whole
    /// Sync-to-FollowUp gap.
    ///
    /// Preferred over `update()` once sync runs: the master anchor is
    /// the wire pair, not `get_gptp_time_ns()` sampled afterwards.
    ///
    /// The rate the rated conversions extrapolate with is the master
    /// clock's rate against THIS app clock, and the anchors are the only
    /// measurement of that pair: it is fitted over the anchors of the
    /// last ANCHOR_WINDOW pairings (16 s of Syncs at 8/s), oldest to
    /// newest. The servo's rate ratio is master against the PHC it
    /// disciplines, a different pair; once the servo has locked that
    /// ratio is 1.0 while the undisciplined app clock still runs its
    /// crystal's ppm off master, and extrapolating with it sawtooths by
    /// ppm x Sync interval every anchor. The long baseline is what keeps
    /// the user-space jitter of the app stamp (tens of us) out of the
    /// rate: it is divided by seconds, not by one Sync interval. Until
    /// the baseline is MIN_RATE_BASELINE_NS long the last rate stands
    /// (0 from construction). A skew past the step threshold, or a
    /// master time that went backwards, is a master step: the window
    /// restarts at this anchor and the rate is held.
    void update_from_anchor(RawTimePoint const sync_rx_raw, GptpTimePoint const tx_master) noexcept
    {
        anchor_at(realtime::MonotonicRawClock::to_ns(sync_rx_raw), realtime::GptpClock0::to_ns(tx_master));
    }

    /// update_from_anchor() in the bare nanoseconds the line is kept in.
    void anchor_at(int64_t sync_rx_app_ns, int64_t tx_master_ns) noexcept
    {
        int64_t ppt = rate_offset_ppt_.load(std::memory_order_relaxed);
        int64_t miss_ns = 0;
        if (anchor_count_ > 0) {
            Anchor const& oldest = anchors_[(anchor_head_ + ANCHOR_WINDOW - anchor_count_) % ANCHOR_WINDOW];
            int64_t const app_delta = sync_rx_app_ns - oldest.app_ns;
            int64_t const skew_ns = (tx_master_ns - oldest.gptp_ns) - app_delta;
            bool const looks_like_step = (app_delta <= 0) || (skew_ns < -STEP_THRESHOLD_NS) || (skew_ns > STEP_THRESHOLD_NS);
            if (looks_like_step) {
                begin_new_timeline();
            } else {
                miss_ns = miss_against_line(sync_rx_app_ns, tx_master_ns);
                ppt = fitted_rate(skew_ns, app_delta, ppt);
            }
        }
        remember_anchor(sync_rx_app_ns, tx_master_ns, ppt, miss_ns);
        publish_anchor(sync_rx_app_ns, tx_master_ns, ppt);
        publish_quality(sync_rx_app_ns);
    }

    /// The owner's word that the timeline has changed (a new grandmaster):
    /// the epoch changes, the anchor window restarts and no bound is
    /// claimed until Syncs of the new timeline have been compared with the
    /// line. The rate is held, as it is for a step the anchors show.
    void restart_timeline() noexcept
    {
        begin_new_timeline();
        publish_quality(prev_app_ns_.load(std::memory_order_relaxed));
    }

    /// How well the bridge's line is known (see the note at the top). Any
    /// thread. Unbounded until the rate has been fitted over its baseline
    /// and an anchor has been compared with the line.
    [[nodiscard]] auto quality() const noexcept -> realtime::ClockQuality { return quality_.load(); }

    /// Refresh only the cached offset, NOT the rate or anchor
    /// timestamps. Useful when you want a current offset reading
    /// without disturbing the rate measurement — e.g. at program
    /// exit time, where calling full update() would compute a noisy
    /// rate from an arbitrary (possibly very small) interval since
    /// the last sync-driven update.
    void refresh_offset() noexcept
    {
        if (!get_gptp_time_ns || !get_app_time_ns) {
            return;
        }
        int64_t const app = get_app_time_ns();
        int64_t const gptp = get_gptp_time_ns();
        // Bump the seqlock around the offset write so a concurrent
        // rated reader sees a consistent (NEW offset, OLD prev_a/g,
        // OLD ppt) snapshot rather than risking a torn pair. The
        // resulting snapshot is intentionally NOT internally
        // consistent (offset is anchored to "now" while prev_a/g are
        // anchored to the last full update), but that's by design:
        // refresh_offset exists specifically to give callers a current
        // offset while preserving the rate-fit anchor. Rated readers
        // get the previous fit's parameters until the next update().
        // refresh_offset intentionally updates only the offset while
        // preserving the existing rate fit's anchor (prev_a/g, ppt).
        // Load the current snapshot, modify the offset, republish.
        rated_buffer_.publish(RatedSnapshot{
            .offset_ns = gptp - app,
            .ppt = rate_offset_ppt_.load(std::memory_order_relaxed),
            .prev_gptp_ns = prev_gptp_ns_.load(std::memory_order_relaxed),
            .prev_app_ns = prev_app_ns_.load(std::memory_order_relaxed),
        });
        offset_ns_.store(gptp - app, std::memory_order_relaxed);
    }

    /// Convert a CLOCK_MONOTONIC_RAW instant to gPTP master time using
    /// only the cached offset (no rate compensation). Drift between
    /// updates is at most rate × interval-since-last-update.
    [[nodiscard]] auto to_gptp(RawTimePoint const raw) const noexcept -> GptpTimePoint
    {
        return GptpTimePoint{raw.time_since_epoch() + realtime::Nanoseconds{offset_ns_.load(std::memory_order_relaxed)}};
    }

    /// Convert a gPTP master instant to CLOCK_MONOTONIC_RAW using only the
    /// cached offset (no rate compensation).
    [[nodiscard]] auto from_gptp(GptpTimePoint const gptp) const noexcept -> RawTimePoint
    {
        return RawTimePoint{gptp.time_since_epoch() - realtime::Nanoseconds{offset_ns_.load(std::memory_order_relaxed)}};
    }

    /// Convert RAW→gptp using the cached rate offset to extrapolate
    /// from the most recent update's anchor point. Accurate to a few
    /// ns at any time within ~1 second of the last update for typical
    /// drift rates.
    ///
    /// Math:
    ///   app_delta_from_anchor = app_ns - prev_app_ns
    ///   correction_ns = app_delta * rate_offset_ppt / 10¹²
    ///   gptp = app_ns + offset + correction_ns
    ///
    /// The correction multiply uses double — overflow-safe for any
    /// realistic anchor age, and double precision is plenty because
    /// the correction value itself is small (µs–ms range, far from
    /// the value-near-1.0 wart that motivated int64 ppt STORAGE). Only
    /// the delta from the anchor goes through it, never the instant.
    [[nodiscard]] auto to_gptp_rated(RawTimePoint const raw) const noexcept -> GptpTimePoint
    {
        auto const snap = rated_buffer_.consume();
        int64_t const app_ns = realtime::MonotonicRawClock::to_ns(raw);
        double const correction_d = static_cast<double>(app_ns - snap.prev_app_ns) * (static_cast<double>(snap.ppt) * 1e-12);
        return realtime::GptpClock0::from_ns(app_ns + snap.offset_ns + static_cast<int64_t>(correction_d));
    }

    /// Convert gptp→RAW using the cached rate offset. Linear
    /// approximation of the inverse rate; error ≤ 1 ns for ±1000 ppm
    /// drift over ≤ 1 s.
    ///
    /// Math (linear-approx inverse):
    ///   gptp_delta_from_anchor = gptp_ns - prev_gptp_ns
    ///   correction_ns = gptp_delta * rate_offset_ppt / 10¹²
    ///   app = gptp_ns - offset - correction_ns
    [[nodiscard]] auto from_gptp_rated(GptpTimePoint const gptp) const noexcept -> RawTimePoint
    {
        auto const snap = rated_buffer_.consume();
        int64_t const gptp_ns = realtime::GptpClock0::to_ns(gptp);
        double const correction_d = static_cast<double>(gptp_ns - snap.prev_gptp_ns) * (static_cast<double>(snap.ppt) * 1e-12);
        return realtime::MonotonicRawClock::from_ns(gptp_ns - snap.offset_ns - static_cast<int64_t>(correction_d));
    }

    /// Get the current gPTP time (reads the gPTP source directly).
    [[nodiscard]] auto gptp_now() const noexcept -> GptpTimePoint
    {
        return realtime::GptpClock0::from_ns(get_gptp_time_ns ? get_gptp_time_ns() : 0);
    }

    /// Get the current CLOCK_MONOTONIC_RAW time through the bridge's hook.
    [[nodiscard]] auto raw_now() const noexcept -> RawTimePoint
    {
        return realtime::MonotonicRawClock::from_ns(get_app_time_ns ? get_app_time_ns() : 0);
    }

    /// Current cached offset (gptp - app) in ns.
    [[nodiscard]] auto offset() const noexcept -> int64_t { return offset_ns_.load(std::memory_order_relaxed); }

    /// Current rate offset in picoseconds per second.
    /// rate_offset_ppt = (rate_ratio − 1) × 10¹².
    /// Returns 0 (perfect 1.0 ratio) if fewer than two updates have
    /// been taken.
    [[nodiscard]] auto rate_offset_ppt() const noexcept -> int64_t { return rate_offset_ppt_.load(std::memory_order_relaxed); }

    /// Last anchor point's app-clock timestamp (the value of
    /// get_app_time_ns() at the most recent update() call). Used by
    /// to_gptp_rated / from_gptp_rated and exposed for diagnostics.
    [[nodiscard]] auto last_anchor_app_ns() const noexcept -> int64_t { return prev_app_ns_.load(std::memory_order_relaxed); }

    /// Last anchor point's gPTP timestamp.
    [[nodiscard]] auto last_anchor_gptp_ns() const noexcept -> int64_t { return prev_gptp_ns_.load(std::memory_order_relaxed); }

    /// Anchors the rate is fitted over: 16 s of Syncs at the 8/s gPTP rate.
    static constexpr size_t ANCHOR_WINDOW = 128;
    /// The shortest baseline the rate is fitted over; below it the last
    /// rate stands. 20 us of app-stamp jitter over 1 s is 20 ppm of rate
    /// noise at the start, 1.25 ppm once the window is full.
    static constexpr int64_t MIN_RATE_BASELINE_NS = 1'000'000'000;
    /// A skew of this much against the window's oldest anchor is a master
    /// step, not drift (1000 ppm over the whole window is 16 ms).
    static constexpr int64_t STEP_THRESHOLD_NS = 100'000'000;

    /// Total publishes that overwrote a still-unconsumed prior publish
    /// in the rated buffer. Diagnostic — non-zero is expected on
    /// healthy systems because the sampler publishes faster than the
    /// RT-timer consumer reads.
    [[nodiscard]] auto rated_buffer_overruns() const noexcept -> uint64_t { return rated_buffer_.overruns(); }

    /// One consistent snapshot of the rated-conversion state.
    /// Published atomically by update / update_from_anchor /
    /// refresh_offset via rated_buffer_; consumed by the rated
    /// readers (to_gptp_rated / from_gptp_rated).
    struct RatedSnapshot
    {
        int64_t offset_ns;
        int64_t ppt;
        int64_t prev_gptp_ns;
        int64_t prev_app_ns;
    };

  private:
    /// One Sync+FollowUp pairing as told to update_from_anchor, with the
    /// rate that was in force once it was taken in and how far the line
    /// had been from it.
    struct Anchor
    {
        int64_t app_ns;
        int64_t gptp_ns;
        int64_t ppt;
        int64_t miss_ns;
    };

    /// Append an anchor to the window, dropping the oldest when full.
    void remember_anchor(int64_t const app, int64_t const gptp, int64_t const ppt, int64_t const miss_ns) noexcept
    {
        anchors_[anchor_head_] = Anchor{.app_ns = app, .gptp_ns = gptp, .ppt = ppt, .miss_ns = miss_ns};
        anchor_head_ = (anchor_head_ + 1) % ANCHOR_WINDOW;
        if (anchor_count_ < ANCHOR_WINDOW) {
            ++anchor_count_;
        }
    }

    /// The rate for the window ending at this anchor: fitted once the
    /// baseline is long enough, else @p held. The first fit also gives its
    /// rate to the anchors taken in before there was one, so that they do
    /// not read as a rate that moved.
    [[nodiscard]] auto fitted_rate(int64_t const skew_ns, int64_t const app_delta, int64_t const held) noexcept -> int64_t
    {
        if (app_delta < MIN_RATE_BASELINE_NS) {
            return held;
        }
        auto const ppt = static_cast<int64_t>((static_cast<double>(skew_ns) * 1e12) / static_cast<double>(app_delta));
        if (!rate_fitted_) {
            rate_fitted_ = true;
            for (auto& anchor : anchors_) {
                anchor.ppt = ppt;
            }
        }
        return ppt;
    }

    /// How far the line as it stands is from a new anchor, before the
    /// anchor is taken in: the one direct measurement of the line's error.
    /// Writer thread; the same arithmetic the rated readers use.
    [[nodiscard]] auto miss_against_line(int64_t const app_ns, int64_t const gptp_ns) const noexcept -> int64_t
    {
        int64_t const app_delta = app_ns - prev_app_ns_.load(std::memory_order_relaxed);
        double const correction_d =
            static_cast<double>(app_delta) * (static_cast<double>(rate_offset_ppt_.load(std::memory_order_relaxed)) * 1e-12);
        int64_t const predicted = app_ns + offset_ns_.load(std::memory_order_relaxed) + static_cast<int64_t>(correction_d);
        return predicted > gptp_ns ? predicted - gptp_ns : gptp_ns - predicted;
    }

    /// A step, or the owner's word: what the anchors said of the old
    /// timeline says nothing of the new one. The rate is held.
    void begin_new_timeline() noexcept
    {
        anchor_count_ = 0;
        ++epoch_;
    }

    /// The furthest any anchor of the window landed from the line.
    [[nodiscard]] auto worst_miss_ns() const noexcept -> int64_t
    {
        int64_t worst = 0;
        for (size_t i = 0; i < anchor_count_; ++i) {
            worst = std::max(worst, anchors_[(anchor_head_ + ANCHOR_WINDOW - 1 - i) % ANCHOR_WINDOW].miss_ns);
        }
        return worst;
    }

    /// How far the fitted rate moved across the window, in parts per
    /// billion rounded up: the rate now held may be that far from the next.
    [[nodiscard]] auto rate_spread_ppb() const noexcept -> uint32_t
    {
        int64_t lowest = rate_offset_ppt_.load(std::memory_order_relaxed);
        int64_t highest = lowest;
        for (size_t i = 0; i < anchor_count_; ++i) {
            int64_t const ppt = anchors_[(anchor_head_ + ANCHOR_WINDOW - 1 - i) % ANCHOR_WINDOW].ppt;
            lowest = std::min(lowest, ppt);
            highest = std::max(highest, ppt);
        }
        int64_t const spread_ppb = ((highest - lowest) + 999) / 1000;
        return spread_ppb > int64_t{UINT32_MAX} ? UINT32_MAX : static_cast<uint32_t>(spread_ppb);
    }

    /// Publish how well the line is known as of the anchor at
    /// @p anchor_app_ns. No bound is claimed before the rate has been
    /// fitted, or before a second anchor of this timeline has been compared
    /// with the line.
    void publish_quality(int64_t const anchor_app_ns) noexcept
    {
        realtime::ClockQuality quality{.epoch = epoch_, .time_source = time_source};
        if (rate_fitted_ && anchor_count_ >= 2) {
            quality.synced_at = realtime::MonotonicRawClock::from_ns(anchor_app_ns);
            quality.bound_at_sync = realtime::Nanoseconds{worst_miss_ns()};
            quality.drift_bound_ppb = std::max(holdover_drift_ppb, rate_spread_ppb());
        }
        quality_.store(quality);
    }

    /// Publish one (app, gptp) anchor and rate as the four-field rated
    /// snapshot, atomically via the triple buffer: a phase jump would
    /// otherwise make a torn read catastrophic (NEW offset bumped by the
    /// step while OLD prev_app remains puts a reader off by exactly the
    /// step). Then mirror the fields for the solo-atomic readers.
    void publish_anchor(int64_t const app, int64_t const gptp, int64_t const ppt) noexcept
    {
        rated_buffer_.publish(RatedSnapshot{
            .offset_ns = gptp - app,
            .ppt = ppt,
            .prev_gptp_ns = gptp,
            .prev_app_ns = app,
        });
        offset_ns_.store(gptp - app, std::memory_order_relaxed);
        rate_offset_ppt_.store(ppt, std::memory_order_relaxed);
        prev_gptp_ns_.store(gptp, std::memory_order_relaxed);
        prev_app_ns_.store(app, std::memory_order_relaxed);
        have_previous_ = true;
    }

    // Solo atomic mirrors of the rated snapshot's fields. Kept so
    // unrated readers (to_gptp / from_gptp on offset_ns_) and the
    // diagnostic accessors (offset, rate_offset_ppt, last_anchor_*)
    // remain multi-reader safe via single atomic loads. The
    // multi-field rated readers go through rated_buffer_ instead.
    std::atomic<int64_t> offset_ns_{0};
    std::atomic<int64_t> rate_offset_ppt_{0};
    std::atomic<int64_t> prev_gptp_ns_{0};
    std::atomic<int64_t> prev_app_ns_{0};
    // Triple-buffered rated snapshot. Single writer (update /
    // update_from_anchor / refresh_offset, all called from the
    // event-loop thread); single consumer (the RT timer thread via
    // GptpSlaveClockSource). Replaces the previous rated_seq_ seqlock.
    mutable statusbar::itc::AtomicTripleBuffer<RatedSnapshot> rated_buffer_{};
    // The anchor window update_from_anchor fits the rate over (writer thread only).
    std::array<Anchor, ANCHOR_WINDOW> anchors_{};
    size_t anchor_head_{0};
    size_t anchor_count_{0};
    bool have_previous_{false};
    // Quality (writer thread builds it, any thread reads it through the seqlock).
    bool rate_fitted_{false};
    uint64_t epoch_{0};
    statusbar::itc::SeqlockValue<realtime::ClockQuality> quality_{realtime::ClockQuality{}};
};

}  // namespace statusbar::gptp
