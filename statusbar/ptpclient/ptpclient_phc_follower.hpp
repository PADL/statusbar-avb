#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// One PHC steered to follow another PHC, or a line over it: per sample one PHC0/PHC_d pair, the error against the
/// line, a PhcServo sample, the action applied. The loop runs over a PhcFollowerPort, a device pair or a test's fake.
/// See docs/LINUXPTP.md.

#include "statusbar/ptpclient/ptpclient_phc.hpp"
#include "statusbar/ptpclient/ptpclient_phc_servo.hpp"
#include "statusbar/status/status.hpp"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>

namespace statusbar::ptpclient {

/// What the followed PHC is steered onto: `target_ns` at PHC0's `anchor_ns`, advancing at 1 + rate_offset_ppb * 1e-9
/// followed ns per PHC0 ns. Following PHC0 itself, offset by a constant, is offset_from_phc0(). A line whose rate is
/// not finite, or past the servo's clamp, is refused by PhcFollower::sample().
struct PhcLine
{
    int64_t anchor_ns{0};         ///< PHC0 time of the anchor, ns
    int64_t target_ns{0};         ///< the followed PHC's time there, ns
    double rate_offset_ppb{0.0};  ///< the line's rate against PHC0, ppb; finite and within the servo's clamp

    /// The line at PHC0 time `phc0_ns`, the rate term rounded to the nearest ns.
    [[nodiscard]] auto at(int64_t const phc0_ns) const noexcept -> int64_t
    {
        int64_t const elapsed = phc0_ns - anchor_ns;
        return target_ns + elapsed + std::llround(static_cast<double>(elapsed) * rate_offset_ppb * 1e-9);
    }

    /// PHC0's own time plus `offset_ns` (a calibration of the pair).
    [[nodiscard]] static constexpr auto offset_from_phc0(int64_t const offset_ns) noexcept -> PhcLine
    {
        return PhcLine{.anchor_ns = 0, .target_ns = offset_ns, .rate_offset_ppb = 0.0};
    }

    /// Field-wise equality.
    auto operator==(PhcLine const&) const noexcept -> bool = default;
};

/// The pair a follower steers: PHC0 read against the followed PHC, and the followed PHC's adjustment.
class PhcFollowerPort
{
  public:
    virtual ~PhcFollowerPort() = default;
    /// PHC0 and the followed PHC at one instant (raw_ns its CLOCK_MONOTONIC_RAW time); the read's error on failure.
    [[nodiscard]] virtual auto measure() noexcept -> StatusValue<PhcCrossSample> = 0;
    /// Set the followed PHC's frequency offset, ppb (ADJ_FREQUENCY); the device's error on failure.
    [[nodiscard]] virtual auto set_frequency_ppb(double ppb) noexcept -> Status = 0;
    /// Step the followed PHC by `ns` (ADJ_SETOFFSET); the device's error on failure.
    [[nodiscard]] virtual auto step_ns(int64_t ns) noexcept -> Status = 0;
    /// The followed PHC's frequency offset in force, ppb.
    [[nodiscard]] virtual auto frequency_ppb() const noexcept -> StatusValue<double> = 0;
    /// The followed PHC's adjustment range, ppb (the driver's max_adj).
    [[nodiscard]] virtual auto max_frequency_ppb() const noexcept -> StatusValue<double> = 0;
};

/// The device pair: PHC0 at `phc0_path` read against the PHC at `followed_path`, opened writable for its adjustment
/// (PhcCrossAccess::phcd_writable; read methods as `config` allows). Errors are the opens'. Linux only, as PhcDevice.
[[nodiscard]] auto open_phc_follower_port(std::string_view phc0_path, std::string_view followed_path, PhcDeviceConfig config = {})
    -> StatusValue<std::unique_ptr<PhcFollowerPort>>;

/// A follower's servo, and when it steps again after its first step.
struct PhcFollowerParams
{
    PhcServoParams servo{};
    /// Once the servo's rate estimate is done (PhcServo::estimated()), an error past this for `restep_count` samples
    /// starts the servo over, so it may step again (a timescale change of PHC0); the samples counted are held, not
    /// fed to the servo. 0: never. Else at least the servo's first step and unlock thresholds, the first step on.
    int64_t restep_threshold_ns{0};
    int restep_count{3};  ///< samples past the threshold that restart the servo; at least 1
};

/// Which part of a sample failed.
enum class PhcFollowerStage : uint8_t
{
    none,       ///< nothing failed
    line,       ///< the line's rate is not finite, or past the servo's clamp (invalid_argument)
    measure,    ///< the pair could not be read
    frequency,  ///< the frequency was refused: nothing applied, the follower as before the sample
    step,       ///< the frequency was applied, the step refused: the first time the servo starts over (the step owed
                ///< again); refused twice running, it slews and the restep rule is off until reset()
};

/// What one sample did. On a failure `failed` names the stage and `error` is its error.
struct PhcFollowerSample
{
    PhcCrossSample pair{};  ///< the pair measured (phc0, phcd, raw instant, uncertainty); zero when measure failed
    int64_t error_ns{0};    ///< followed PHC less the line, before any action
    PhcServoState state{PhcServoState::unlocked};  ///< as PhcFollower::state() after the sample
    bool steered{false};                           ///< a frequency was applied (not while frozen, nor while a restep is counted)
    bool stepped{false};                           ///< and the followed PHC was stepped
    bool restarted{false};                         ///< the restep rule started the servo over at this sample
    PhcFollowerStage failed{PhcFollowerStage::none};
    std::error_code error{};

    /// Whether no stage failed.
    [[nodiscard]] auto ok() const noexcept -> bool { return failed == PhcFollowerStage::none; }
};

/// A PHC following a line over PHC0, sample by sample. Not for a real-time thread (each sample reads both PHCs and
/// adjusts one: system calls). Movable, not copyable.
class PhcFollower
{
  public:
    /// A follower over `port`, its servo starting from the followed PHC's frequency, clamped to the smaller of
    /// `params.servo.max_frequency_ppb` and the port's range. invalid_argument for bad parameters (a restep rule that
    /// cannot settle among them) or a null port; the port's error when it cannot read its frequency or range.
    [[nodiscard]] static auto create(PhcFollowerParams const& params, std::unique_ptr<PhcFollowerPort> port)
        -> StatusValue<PhcFollower>;

    /// One sample onto `line`, its rate fed forward; the servo's time is the pair's raw_ns (CLOCK_MONOTONIC_RAW).
    /// While the line is not running, pass its rate as 0 (or freeze()). While frozen, or while a restep is counted,
    /// the pair is measured and the error reported, and nothing is steered. A failure is reported in the sample
    /// (PhcFollowerStage), the follower left as each stage says.
    [[nodiscard]] auto sample(PhcLine const& line) noexcept -> PhcFollowerSample;

    /// The servo's state, except unlocked while a restep is being counted (a large error is not locked).
    [[nodiscard]] auto state() const noexcept -> PhcServoState;
    /// The servo, for its gains and frequency.
    [[nodiscard]] auto servo() const noexcept -> PhcServo const& { return servo_; }
    /// The port it steers.
    [[nodiscard]] auto port() noexcept -> PhcFollowerPort& { return *port_; }
    /// Hold the frequency in force, steering nothing, until thaw() (PhcServo::freeze()): for a line that stopped
    /// running, so the followed PHC free-runs on its last correction. Safe to call twice.
    void freeze() noexcept;
    /// Resume after freeze(): the servo estimates the rate again, with no step if it has ever locked; an error still
    /// past the restep threshold after that estimate steps by the restep rule (a line that resumed elsewhere).
    /// Nothing when not frozen, so it is safe to call on every sample.
    void thaw() noexcept;
    /// Start over: the servo re-estimates and may step again (PhcServo::reset()); it also ends a freeze, and turns
    /// the restep rule back on after refused steps.
    void reset() noexcept;

  private:
    PhcFollower(PhcFollowerParams const& params, std::unique_ptr<PhcFollowerPort> port, PhcServo const& servo) noexcept;
    /// The servo's sample of `pair` onto `line`, applied.
    [[nodiscard]] auto steer(PhcFollowerSample sample, PhcLine const& line) noexcept -> PhcFollowerSample;
    /// Count `error_ns` against the restep rule (off while steps are refused): whether the sample is held, and whether
    /// it starts the servo over.
    struct Restep
    {
        bool hold{false};
        bool restart{false};
    };
    [[nodiscard]] auto held(int64_t error_ns) noexcept -> Restep;
    /// A refused step on `sample`: the stage and `error` reported, the servo started over the first time.
    void refuse_step(PhcFollowerSample& sample, std::error_code error) noexcept;
    /// What a refused frequency puts back: the follower as before the sample.
    struct Undo
    {
        PhcServo servo;
        int over{0};
    };
    /// `action` applied to the port, the failed stage reported in `sample`; `undo` put back on a refused frequency.
    [[nodiscard]] auto apply(PhcServoAction const& action, PhcFollowerSample sample, Undo const& undo) noexcept
        -> PhcFollowerSample;

    PhcFollowerParams params_;
    std::unique_ptr<PhcFollowerPort> port_;
    PhcServo servo_;
    int over_{0};           ///< consecutive samples past the restep threshold
    int refused_steps_{0};  ///< consecutive steps the device refused, since the last reset()
};

}  // namespace statusbar::ptpclient
