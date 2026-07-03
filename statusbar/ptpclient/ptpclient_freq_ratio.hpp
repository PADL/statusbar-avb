// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Frequency-ratio estimators between the local PHC (switch gPTP time) and GPS
// (CLOCK_REALTIME, disciplined to the site GPS grandmaster by chrony). These track
//   r = switch_rate / GPS_rate
// from a stream of (PHC-GPS offset, elapsed-GPS-seconds) samples, for the
// GPS-rate media-clock generator. See avb/docs/GPS_MEDIA_CLOCK.md.
//
// Both consume the same input: d(offset)/d(gps_time) = r - 1 by definition, so
// the offset's slope vs GPS time *is* the fractional frequency. They are pure
// (no I/O, no platform deps) and unit-testable anywhere; the Linux PHC/GPS
// sampling lives in the gps_ratio_tracker tool.
//
//   OlsRatioTracker     -- sliding-window ordinary-least-squares slope. Simple;
//                          a frequency-ramp (thermal drift) lags by window/2.
//   KalmanRatioTracker  -- 3-state clock filter (phase, frequency, drift). Fast
//                          acquisition, tunable bandwidth, drift state => zero
//                          steady-state ramp lag, innovation outlier gating,
//                          self-reported uncertainty, and GPS-holdover coasting.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <deque>

namespace statusbar::ptpclient {

struct RatioEstimate
{
    double r = 1.0;                       // switch_rate / GPS_rate
    std::int64_t offset_ns = 0;           // latest PHC - GPS (raw measurement)
    std::int64_t filtered_offset_ns = 0;  // filter's phase estimate at the last
                                          // update (Kalman: base + x[0]; OLS
                                          // falls back to the raw offset). Use
                                          // this — not offset_ns — when the
                                          // offset itself feeds a translation,
                                          // so measurement jitter isn't
                                          // re-injected downstream.
    double freq_uncertainty_ppb = 0.0;    // 1-sigma on r [ppb], 0 if unavailable
    double drift_ppm_per_hr = 0.0;        // estimated df/dt (Kalman only)
    bool valid = false;

    [[nodiscard]] double ppm() const { return (r - 1.0) * 1e6; }
    [[nodiscard]] double phc_ns_per_sample(double fs) const { return r * 1e9 / fs; }
};

// --- Sliding-window OLS slope of (PHC-GPS offset) vs GPS time ---
class OlsRatioTracker
{
  public:
    explicit OlsRatioTracker(double window_s)
        : window_s_{window_s}
    {}

    void add(std::int64_t offset_ns, double dt_s)
    {
        if (!have_base_) {
            base_ = offset_ns;
            have_base_ = true;
        }
        last_offset_ = offset_ns;
        t_ += dt_s;
        samples_.push_back({t_, static_cast<double>(offset_ns - base_)});
        while (samples_.size() > 1 && (t_ - samples_.front().t) > window_s_) {
            samples_.pop_front();
        }
    }

    [[nodiscard]] RatioEstimate estimate() const
    {
        RatioEstimate e;
        e.offset_ns = last_offset_;
        e.filtered_offset_ns = last_offset_;  // OLS has no phase state; raw fallback
        std::size_t const n = samples_.size();
        if (n < 8) {
            return e;
        }
        double const t0 = samples_.front().t;
        double st = 0, sy = 0, stt = 0, sty = 0;
        for (auto const& p : samples_) {
            double const t = p.t - t0;
            st += t;
            sy += p.y;
            stt += t * t;
            sty += t * p.y;
        }
        double const N = static_cast<double>(n);
        double const sxx = stt - ((st * st) / N);
        if (sxx <= 0) {
            return e;
        }
        double const b = (sty - ((st * sy) / N)) / sxx;  // ns/s == (r-1)*1e9
        double const a = (sy - (b * st)) / N;
        double ss = 0;
        for (auto const& p : samples_) {
            double const t = p.t - t0;
            double const resid = p.y - ((b * t) + a);
            ss += resid * resid;
        }
        double const var_b = (ss / N) / sxx;
        e.r = 1.0 + (b * 1e-9);
        e.freq_uncertainty_ppb = std::sqrt(var_b > 0 ? var_b : 0.0);  // ns/s == ppb
        e.valid = true;
        return e;
    }

    void reset()
    {
        samples_.clear();
        t_ = 0;
        have_base_ = false;
    }

  private:
    struct Pt
    {
        double t;
        double y;
    };
    double window_s_;
    double t_ = 0.0;
    std::int64_t base_ = 0;
    bool have_base_ = false;
    std::int64_t last_offset_ = 0;
    std::deque<Pt> samples_;
};

// --- 3-state Kalman clock filter (phase, frequency, drift) ---
class KalmanRatioTracker
{
  public:
    struct Config
    {
        double meas_noise_ns = 200.0;  // sqrt(R): per-sample read jitter
        double jerk_psd = 1e-2;        // process noise on drift (white jerk)
        double gate_sigmas = 5.0;      // innovation outlier gate

        // Phase-STEP handling. The two clock domains we track (gPTP/PHC vs
        // CLOCK_REALTIME) have completely different epochs -- gPTP starts at 0
        // when the grandmaster boots, CLOCK_REALTIME is GPS/Unix -- so their
        // offset is huge (~1.78e9 s) and STEPS whenever the GM reboots or
        // ptp4l/chrony steps a clock. A step is NOT a frequency: a real
        // switch-vs-GPS rate is tens of ppm, never thousands. So a sample
        // implying more than max_freq_ppm of frequency is a step, and the filter
        // RE-ACQUIRES (re-bases and re-seeds the rate from the post-step regime)
        // instead of seeding a bogus rate or rejecting the step forever as an
        // outlier. The step floor sits well above any single read glitch, so a
        // momentary outlier is gated (not re-acquired); only a genuine ms+ step
        // re-acquires.
        double max_freq_ppm = 1000.0;        // plausible |switch-GPS| freq bound
        double step_floor_ns = 1'000'000.0;  // 1 ms min step (+ max_freq*dt)
    };

    KalmanRatioTracker()
        : KalmanRatioTracker(Config{})
    {}

    explicit KalmanRatioTracker(Config c)
        : R_{c.meas_noise_ns * c.meas_noise_ns}
        , q_{c.jerk_psd}
        , gate2_{c.gate_sigmas * c.gate_sigmas}
        , max_freq_ns_per_s_{c.max_freq_ppm * 1e3}  // ppm * 1e-6 * 1e9 ns/s
        , step_floor_ns_{c.step_floor_ns}
    {}

    // Returns the normalized innovation; NaN if the sample was gated (outlier).
    double add(std::int64_t offset_ns, double dt_s)
    {
        last_offset_ = offset_ns;
        if (!have_base_) {
            base_ = offset_ns;
            have_base_ = true;
            last_z_ = 0.0;
            n_ = 1;
            return 0.0;
        }
        double const z = static_cast<double>(offset_ns - base_);
        if (dt_s <= 0) {
            return 0.0;  // can't use a sample without a positive time delta
        }
        if (n_ == 1) {
            // Seed the frequency from the first two samples and start with a
            // CONSERVATIVE covariance. Starting at f=0 with a huge initial P
            // and letting the 3-state gain slew into place can overshoot and
            // diverge for some R / sample sequences -- seeding avoids that.
            double const f0 = (z - last_z_) / dt_s;  // ns/s
            if (std::abs(f0) > max_freq_ns_per_s_) {
                // This pair spans a phase STEP (GM reboot / ptp4l|chrony step),
                // not a real frequency -- seeding f0 here is exactly how the
                // media clock ran to +83345 ppm. Re-base to this sample and wait
                // for a clean pair instead of baking a bogus rate.
                base_ = offset_ns;
                last_z_ = 0.0;  // stay at n_==1; re-seed on the next sample
                return std::nan("");
            }
            x_ = {z, f0, 0.0};
            P_ = M3{};
            P_[0][0] = R_;                        // phase ~ measurement noise
            P_[1][1] = 2.0 * R_ / (dt_s * dt_s);  // freq from a single difference
            P_[2][2] = k_drift_init_var_;         // drift small; Q grows it as learned
            last_z_ = z;
            n_ = 2;
            return 0.0;
        }
        // Phase-STEP detection BEFORE committing the sample: an innovation far
        // beyond what any plausible frequency could produce over this interval is
        // a domain STEP (chrony steps CLOCK_REALTIME to GPS ~5 s after boot; a GM
        // reboot; a ptp4l step), not drift or noise. The 5-sigma gate would
        // REJECT it forever (stale estimate); a "snap phase, keep frequency" jump
        // is also wrong here, because the pre-step rate may be stale OR was
        // corrupted while CLOCK_REALTIME was still converging at boot. So
        // RE-ACQUIRE: re-base to this sample and re-seed the frequency from the
        // clean post-step regime. Smaller outliers (5-sigma .. step_ns) still fall
        // through to update()'s gate, so a single bad read doesn't drop the lock.
        double const predicted_phase = x_[0] + (dt_s * x_[1]) + (0.5 * dt_s * dt_s * x_[2]);
        double const step_ns = step_floor_ns_ + (max_freq_ns_per_s_ * dt_s);
        if (std::abs(z - predicted_phase) > step_ns) {
            base_ = offset_ns;  // re-base; relative coordinate restarts at 0
            last_z_ = 0.0;
            x_ = {0.0, 0.0, 0.0};
            P_ = init_P();
            n_ = 1;               // re-seed the frequency on the next sample
            return std::nan("");  // signal: re-based on a step (not an update)
        }
        last_z_ = z;
        ++n_;
        predict(dt_s);
        return update(z);
    }

    [[nodiscard]] RatioEstimate estimate() const
    {
        RatioEstimate e;
        e.offset_ns = last_offset_;
        if (n_ < 2) {  // valid once the frequency has been seeded
            e.filtered_offset_ns = last_offset_;
            return e;
        }
        e.filtered_offset_ns = base_ + static_cast<std::int64_t>(std::llround(x_[0]));
        e.r = 1.0 + (x_[1] * 1e-9);
        e.freq_uncertainty_ppb = std::sqrt(P_[1][1] > 0 ? P_[1][1] : 0.0);  // ns/s == ppb
        e.drift_ppm_per_hr = x_[2] * 1e-9 * 3600.0 * 1e6;
        e.valid = true;
        return e;
    }

    void reset()
    {
        x_ = {0, 0, 0};
        P_ = init_P();
        have_base_ = false;
        n_ = 0;
    }

  private:
    using M3 = std::array<std::array<double, 3>, 3>;

    // Conservative seed for the drift-state variance [(ns/s^2)^2]; the process
    // noise Q grows it as a real drift is observed, so the filter learns drift
    // gradually rather than letting it run away at startup.
    static constexpr double k_drift_init_var_ = 1.0;

    static M3 init_P() { return M3{}; }  // zeros; the real seed is set on sample 2

    static M3 matmul(const M3& a, const M3& b)
    {
        M3 c{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                double s = 0;
                for (int k = 0; k < 3; ++k) {
                    s += a[i][k] * b[k][j];
                }
                c[i][j] = s;
            }
        }
        return c;
    }

    static M3 transpose(const M3& a)
    {
        M3 t{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                t[i][j] = a[j][i];
            }
        }
        return t;
    }

    void predict(double dt)
    {
        double const t2 = dt * dt, t3 = t2 * dt, t4 = t3 * dt, t5 = t4 * dt;
        x_[0] += (dt * x_[1]) + (0.5 * t2 * x_[2]);
        x_[1] += dt * x_[2];
        const M3 F{{{1, dt, 0.5 * t2}, {0, 1, dt}, {0, 0, 1}}};
        P_ = matmul(matmul(F, P_), transpose(F));
        // Process-noise Q for the continuous white-noise-jerk (3rd-order integrated,
        // a.k.a. Wiener-process-acceleration) model: state = [offset, freq, drift],
        // driven by a white jerk of PSD q_. Q is the exact closed form of
        // integral_0^dt F(t) G G^T F(t)^T dt with G = [0,0,1]^T; the divisors
        // (20/8/6/3/2) are those integrals, not tuning knobs. See e.g. Bar-Shalom,
        // "Estimation with Applications to Tracking and Navigation", sec. 6.2-6.3.
        // Retune the model via q_ (jerk_psd); do not touch the divisors.
        const M3 Q{
            {{q_ * t5 / 20, q_ * t4 / 8, q_ * t3 / 6},
             {q_ * t4 / 8, q_ * t3 / 3, q_ * t2 / 2},
             {q_ * t3 / 6, q_ * t2 / 2, q_ * dt}}};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                P_[i][j] += Q[i][j];
            }
        }
    }

    double update(double z)
    {
        double const y = z - x_[0];      // innovation (H = [1 0 0])
        double const S = P_[0][0] + R_;  // scalar -> no inversion
        if ((y * y / S) > gate2_) {
            return std::nan("");  // reject spike, keep prediction
        }
        std::array<double, 3> const K{P_[0][0] / S, P_[1][0] / S, P_[2][0] / S};
        for (int i = 0; i < 3; ++i) {
            x_[i] += K[i] * y;
        }
        // Joseph form: P = (I - K H) P (I - K H)^T + K R K^T. Stays positive-
        // semidefinite under rounding far better than the short (I-KH)P form,
        // which is part of why the un-seeded filter could run away. H = [1 0 0].
        const M3 A{{{1.0 - K[0], 0, 0}, {-K[1], 1, 0}, {-K[2], 0, 1}}};
        M3 P = matmul(matmul(A, P_), transpose(A));
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                P[i][j] += K[i] * K[j] * R_;
                P_[i][j] = P[i][j];
            }
        }
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                P_[i][j] = 0.5 * (P[i][j] + P[j][i]);  // symmetrize
            }
        }
        return y / std::sqrt(S);
    }

    std::array<double, 3> x_{0, 0, 0};  // phase[ns], freq[ns/s], drift[ns/s^2]
    M3 P_ = init_P();
    double R_;
    double q_;
    double gate2_;
    double max_freq_ns_per_s_;
    double step_floor_ns_;
    std::int64_t base_ = 0;
    std::int64_t last_offset_ = 0;
    double last_z_ = 0.0;  // previous relative offset (for the 2-sample seed)
    bool have_base_ = false;
    std::uint64_t n_ = 0;
};

}  // namespace statusbar::ptpclient
