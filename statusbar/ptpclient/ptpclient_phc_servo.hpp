#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// A PI servo steering a PHC to a target time given per sample: linuxptp's pi.c with its
/// hardware-timestamping defaults, a first step only, a frequency clamp, freeze and reset.
/// The servo is pure (feed it errors and a fake clock); apply() drives a PhcDevice.

#include "statusbar/ptpclient/ptpclient_phc.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::ptpclient {

/// linuxptp's pi_* defaults for hardware timestamps; kp and ki follow from the interval
/// (kp = kp_scale * interval^kp_exponent, at most kp_norm_max / interval; ki likewise).
struct PhcServoParams
{
    int64_t interval_ns{62'500'000};          ///< between samples (16 a second)
    double kp_scale{0.7};                     ///< pi_proportional_scale; positive
    double kp_exponent{-0.3};                 ///< pi_proportional_exponent
    double kp_norm_max{0.7};                  ///< pi_proportional_norm_max; positive
    double ki_scale{0.3};                     ///< pi_integral_scale; positive
    double ki_exponent{0.4};                  ///< pi_integral_exponent
    double ki_norm_max{0.3};                  ///< pi_integral_norm_max; positive
    double max_frequency_ppb{900'000'000.0};  ///< clamp; give min(this, PhcDevice::max_frequency_ppb())
    int64_t first_step_threshold_ns{20'000};  ///< step before the first lock past this; 0 never
    int64_t lock_threshold_ns{100};           ///< locked after lock_count samples under it
    int lock_count{16};
    int64_t unlock_threshold_ns{1'000};  ///< unlocked after unlock_count samples over it
    int unlock_count{16};
};

enum class PhcServoState : uint8_t
{
    unlocked,  ///< estimating, converging, or past the unlock criterion
    locked,    ///< within lock_threshold_ns for lock_count samples
    frozen,    ///< holding its frequency; samples are ignored until thaw()
};

/// What to do to the PHC: set the frequency, then step by `step_ns` if present.
struct PhcServoAction
{
    double frequency_ppb{0.0};
    std::optional<int64_t> step_ns;
};

class PhcServo
{
  public:
    /// A servo starting from the frequency in force. Fails with invalid_argument for a
    /// non-positive interval, clamp, threshold or count.
    [[nodiscard]] static auto create(PhcServoParams const& params, double initial_frequency_ppb = 0.0) -> StatusValue<PhcServo>;

    /// One sample: `error_ns` = PHC time - target time, `local_ns` the sample's instant on a
    /// steady clock, `feed_forward_ppb` a known target rate added to the output.
    [[nodiscard]] auto sample(int64_t error_ns, int64_t local_ns, double feed_forward_ppb = 0.0) noexcept -> PhcServoAction;

    /// Hold the frequency in force; samples change nothing until thaw().
    void freeze() noexcept;
    /// Resume from a frozen state: re-estimate the rate, with no step if it has ever locked.
    void thaw() noexcept;
    /// Start over from the frequency in force, as at creation: a step is allowed again.
    void reset() noexcept;

    /// Frozen, locked or unlocked, by the lock criteria of PhcServoParams.
    [[nodiscard]] auto state() const noexcept -> PhcServoState;
    /// The frequency offset last output, in ppb, feed-forward included.
    [[nodiscard]] auto frequency_ppb() const noexcept -> double { return frequency_ppb_; }
    /// The proportional gain in ppb per ns of error.
    [[nodiscard]] auto kp() const noexcept -> double { return kp_; }
    /// The integral gain in ppb per ns of error per sample.
    [[nodiscard]] auto ki() const noexcept -> double { return ki_; }

  private:
    PhcServo(PhcServoParams const& params, double initial_frequency_ppb) noexcept;
    [[nodiscard]] auto clamp(double ppb) const noexcept -> double;
    void track_lock(int64_t error_ns) noexcept;
    /// The second sample, once FREQ_EST_KI / ki_ has passed since the first: the drift cancelled.
    [[nodiscard]] auto estimate_drift(int64_t error_ns, int64_t local_ns, double feed_forward_ppb) noexcept -> PhcServoAction;
    /// Set the output and integrator to cancel the drift seen over @p elapsed ns since the first sample.
    void cancel_drift(int64_t error_ns, double elapsed, double feed_forward_ppb) noexcept;
    /// One PI step on @p error_ns (running), and the lock it shows.
    void regulate(int64_t error_ns, double feed_forward_ppb) noexcept;

    PhcServoParams params_;
    double kp_{0.0};
    double ki_{0.0};
    double frequency_ppb_{0.0};  ///< the output in force
    double drift_ppb_{0.0};      ///< the integrator, excluding the feed-forward
    int64_t first_error_ns_{0};
    int64_t first_local_ns_{0};
    int count_{0};  ///< samples into the estimate: 0, 1, then 2 (running)
    int good_{0};
    int bad_{0};
    bool locked_{false};
    bool ever_locked_{false};
    bool frozen_{false};
};

/// Apply `action` to `device` (opened writable): frequency first, then the step.
[[nodiscard]] auto apply(PhcDevice const& device, PhcServoAction const& action) noexcept -> Status;

}  // namespace statusbar::ptpclient
