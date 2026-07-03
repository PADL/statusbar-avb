// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Deterministic, GPS-frequency-locked media-clock timestamp generator.
//
// The AVTP presentation timestamp (avtp_timestamp / CIP SYT) must be the gPTP
// time at which a sample is presented. The naive implementation reads the media
// timer's wake time each tick and uses it as the base -- so the wake jitter
// (tens of ns) lands directly in the timestamp, and a listener that recovers its
// media clock from the timestamp (e.g. an audio interface) warbles.
//
// MediaClockGenerator removes that jitter with a PHASE ACCUMULATOR: the
// presentation time of each sample advances by exactly one GPS-pinned sample
// period, independent of the wake time, so the emitted timestamps are perfectly
// smooth and deterministic:
//
//   phase += samples_emitted * (nominal_period_ns * r)     // per tick
//   avtp_timestamp(i) = base + phase(i) + presentation_offset
//
// The per-sample period is pinned to the GPS frequency ratio r = switch/GPS,
// which the ptpclient *ratio Kalman* supplies already smoothed -- so the media
// clock is GPS-locked (every site agrees on the rate) rather than tracking the
// local PHC. The wake time is used ONLY to pace how many samples to emit this
// tick (the nominal count +/- 1) so production stays aligned to the presentation
// offset; it never enters the timestamp. Accumulating phase (rather than
// anchor + i*slope) also keeps the timestamp continuous across slow r changes --
// no growing lever arm. See avb/docs/GPS_MEDIA_CLOCK.md.
//
// Representation: the phase (which grows without bound) is carried as an EXACT
// integer-nanosecond accumulator (uint64) plus a small [0,1) ns fractional
// remainder. The hot path -- per-tick accumulate and per-packet timestamp_for()
// -- is therefore all integer + `float` (real_t); only the per-sample slope
// split runs in `double`, once per advance(). Floating point thus only ever
// holds SMALL values (a slope ~10416 and fractions < 1), so single precision is
// accurate -- cheap on low-power CPUs without (or slow at) double precision.
//
// Pure (no I/O, no platform deps) and unit-testable anywhere; the Linux PHC/GPS
// sampling that produces `r` lives in the gps_ratio_tracker / the entity.

#pragma once

#include <cmath>
#include <cstdint>

namespace statusbar::ptpclient {

class MediaClockGenerator
{
  public:
    struct Config
    {
        double sample_rate_hz = 96000.0;
        std::uint64_t presentation_offset_ns = 1'000'000;  // 1 ms ahead of "now"
    };

    MediaClockGenerator()
        : MediaClockGenerator(Config{})
    {}

    // Small-value arithmetic type for the HOT path (per-tick accumulate,
    // per-packet timestamp). The magnitude (the growing absolute presentation
    // time) is carried in uint64 nanoseconds; only the sub-nanosecond fractional
    // remainder and the small per-sample slope fraction are kept in real_t, so
    // they never exceed ~1 and `float` is plenty precise. Using float here is the
    // win on low-power CPUs (single-precision / no double FPU); the few double
    // ops left run once per advance() (off the per-packet path).
    using real_t = float;

    explicit MediaClockGenerator(Config c)
        : nominal_period_ns_{1e9 / c.sample_rate_hz}
        , offset_ns_{c.presentation_offset_ns}
    {
        set_rate(1.0);
    }

    struct Emit
    {
        std::uint32_t samples;      // how many samples to generate this tick
        std::uint64_t first_index;  // global index of the first of those samples
    };

    // Drive once per media-timer wake.
    //   gptp_now_ns     : the wake's gPTP time (jittery is fine -- it never
    //                     enters the timestamp, only the pacing)
    //   r               : smooth GPS ratio switch/GPS from the ratio Kalman
    //   nominal_samples : nominal samples per tick (e.g. 12); GPS pacing emits
    //                     this +/- 1 to track the GPS rate.
    Emit advance(std::uint64_t gptp_now_ns, double r, std::uint32_t nominal_samples)
    {
        set_rate(r);  // gPTP ns per sample, GPS-pinned (double, once per wake)

        if (!anchored_) {
            base_ = gptp_now_ns;
            phase_ns_ = offset_ns_;  // sample 0 presented one offset ahead of base_
            phase_frac_ = 0;
            anchored_ = true;
            return emit_(nominal_samples);
        }

        // Pace to the GPS rate: advance the phase to ~one presentation-offset
        // ahead of the wake. The wake jitter only nudges the integer emit count
        // by +/-1; it does NOT enter the accumulated phase (= the timestamps).
        // target - phase is BOUNDED (~one slope), so the uint64 subtraction is
        // exact and small; only that small remainder enters real_t -- no large
        // (growing) value is ever held in floating point.
        std::uint64_t const target_ns = (gptp_now_ns - base_) + offset_ns_;
        auto const diff_ns = static_cast<std::int64_t>(target_ns - phase_ns_);
        // gPTP timeline step (GM reboot / epoch reset): the phase is implausibly far
        // from where the wake clock says it should be. Slow catch-up (clamped to
        // nominal+1) would stream dead-epoch timestamps for seconds-to-forever on a
        // forward jump, or stall at 0 samples on a backward one, so re-anchor to the
        // new timeline instead (like a fresh anchor). One |diff| test catches both
        // directions: forward jump -> huge positive; backward -> huge negative after
        // the uint64 target wraps. n_ is NOT reset, so first_index stays monotonic
        // and timestamp_for() maps it onto the new base_ immediately.
        if (diff_ns > STEP_THRESHOLD_NS || diff_ns < -STEP_THRESHOLD_NS) {
            base_ = gptp_now_ns;
            phase_ns_ = offset_ns_;
            phase_frac_ = 0;
            ++step_count_;
            return emit_(nominal_samples);
        }
        real_t const diff = static_cast<real_t>(diff_ns) - phase_frac_;
        long want = std::lround(diff / slope_real());
        // Bound to nominal+1: keeps every packet within the advertised SRP frame
        // size and Class-A shape. Steady state is nominal or nominal-/+1; a rare
        // late wake recovers over a few ticks rather than emitting a jumbo packet.
        long const cap = static_cast<long>(nominal_samples) + 1;
        if (want < 0) {
            want = 0;
        }
        if (want > cap) {
            want = cap;
        }
        return emit_(static_cast<std::uint32_t>(want));
    }

    // Deterministic avtp_timestamp (full 64-bit gPTP ns, incl. presentation
    // offset) for a sample index, extrapolated linearly from the current packet's
    // phase/index. Free of wake jitter; continuous across r changes (extrapolated
    // from the accumulated packet phase, not a growing lever arm off sample 0).
    // The index may be BEFORE packet_index0_ (e.g. a reframer emitting a packet
    // whose first sample was buffered on an earlier tick), so the offset is taken
    // in signed arithmetic -- subtracting the two uint64 indices directly would
    // underflow and produce a garbage timestamp.
    [[nodiscard]] std::uint64_t timestamp_for(std::uint64_t sample_index) const
    {
        // off is small (a reframer's carry is +/- a packet; direct callers pass 0
        // or the current packet's range), and may be NEGATIVE for an index buffered
        // on an earlier tick. The integer ns part is exact (int64); only the small
        // fractional sum touches real_t.
        auto const off = static_cast<std::int64_t>(sample_index - packet_index0_);
        std::int64_t const int_delta = slope_int_ * off;
        real_t const frac = packet_phase_frac_ + (static_cast<real_t>(off) * slope_frac_);
        auto const frac_ns = static_cast<std::int64_t>(std::lround(frac));
        std::int64_t const pres_off = static_cast<std::int64_t>(packet_phase_ns_) + int_delta + frac_ns;
        return base_ + static_cast<std::uint64_t>(pres_off);
    }

    [[nodiscard]] std::uint64_t samples_emitted() const noexcept { return n_; }
    [[nodiscard]] bool anchored() const noexcept { return anchored_; }
    [[nodiscard]] double ns_per_sample() const noexcept { return slope_; }
    [[nodiscard]] double ratio() const noexcept { return r_; }

    /// Number of gPTP-timeline steps re-anchored so far (observability; a change
    /// signals the media clock restarted onto a new epoch this tick).
    [[nodiscard]] std::uint64_t step_count() const noexcept { return step_count_; }

    void reset() noexcept
    {
        anchored_ = false;
        n_ = 0;
        phase_ns_ = 0;
        phase_frac_ = 0;
        step_count_ = 0;
    }

    /// A gPTP jump larger than this (vs. where the wake clock says the phase should
    /// be) is treated as a timeline step and re-anchored rather than caught up. Far
    /// above SCHED_FIFO wake jitter / the ~1 ms presentation offset, far below any
    /// GM-reboot epoch step (seconds+).
    static constexpr std::int64_t STEP_THRESHOLD_NS = 100'000'000;  // 100 ms

  private:
    // Set the per-sample slope from the GPS ratio, split into an exact integer-ns
    // part and a [0,1) ns fractional part. The (cheap) double math runs once per
    // advance(), keeping the per-packet path integer + real_t.
    void set_rate(double r) noexcept
    {
        r_ = r;
        slope_ = nominal_period_ns_ * r;
        slope_int_ = static_cast<std::int64_t>(slope_);  // floor (slope_ > 0)
        slope_frac_ = static_cast<real_t>(slope_ - static_cast<double>(slope_int_));
    }

    // Per-sample slope as a single small real_t (~10416.67 @ 96 kHz) for pacing.
    [[nodiscard]] real_t slope_real() const noexcept { return static_cast<real_t>(slope_int_) + slope_frac_; }

    // Record this packet's phase/index base, then advance the accumulator by
    // `samples * slope`, carrying whole nanoseconds out of the fractional part so
    // phase_frac_ stays in [0,1) and never grows.
    Emit emit_(std::uint32_t samples)
    {
        packet_index0_ = n_;
        packet_phase_ns_ = phase_ns_;
        packet_phase_frac_ = phase_frac_;

        phase_ns_ += static_cast<std::uint64_t>(slope_int_ * static_cast<std::int64_t>(samples));
        phase_frac_ += static_cast<real_t>(samples) * slope_frac_;
        real_t const whole = std::floor(phase_frac_);
        phase_ns_ += static_cast<std::uint64_t>(whole);
        phase_frac_ -= whole;

        n_ += samples;
        return {.samples = samples, .first_index = packet_index0_};
    }

    double nominal_period_ns_;   // ns per sample at r=1 (slope source, accessors)
    std::uint64_t offset_ns_;    // presentation offset (integer ns)
    double r_{1.0};              // GPS ratio (for ratio())
    double slope_{0.0};          // ns/sample = nominal_period_ns_ * r_ (for ns_per_sample())
    std::int64_t slope_int_{0};  // floor(slope_): integer ns per sample
    real_t slope_frac_{0};       // slope_ - slope_int_: fractional ns per sample, [0,1)

    // Presentation phase (gPTP ns, relative to base_) of sample n_, split into an
    // exact integer part (carries the growing magnitude) and a small [0,1) frac.
    std::uint64_t phase_ns_ = 0;
    real_t phase_frac_ = 0;
    // Snapshot of the above at the current packet's first sample.
    std::uint64_t packet_index0_ = 0;
    std::uint64_t packet_phase_ns_ = 0;
    real_t packet_phase_frac_ = 0;

    std::uint64_t base_ = 0;
    std::uint64_t n_ = 0;
    std::uint64_t step_count_ = 0;
    bool anchored_ = false;
};

}  // namespace statusbar::ptpclient
