#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP hardware clock access: one PHC read against CLOCK_MONOTONIC_RAW, a PHC-to-PHC
/// cross-timestamp bracketed as PHC0 / PHC_d / PHC0, and frequency and phase adjustment.
/// The bracket arithmetic is pure and portable; devices are Linux only. See docs/LINUXPTP.md.

#include "statusbar/buffer/file_descriptor.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace statusbar::ptpclient {

/// One PHC read bracketed by two CLOCK_MONOTONIC_RAW reads, all in ns.
struct PhcBracket
{
    int64_t before_ns{0};
    int64_t phc_ns{0};
    int64_t after_ns{0};
};

/// A PHC time and the CLOCK_MONOTONIC_RAW time it was read at (the bracket's midpoint);
/// `width_ns` is the bracket's width (0 for a hardware cross-timestamp).
struct PhcReading
{
    int64_t raw_ns{0};
    int64_t phc_ns{0};
    int64_t width_ns{0};
};

/// One PHC0/PHC_d pair: both clocks' times at the same instant, `raw_ns` that instant on
/// CLOCK_MONOTONIC_RAW, `uncertainty_ns` half PHC_d's bracket plus half PHC0's mean bracket.
struct PhcCrossSample
{
    int64_t phc0_ns{0};
    int64_t phcd_ns{0};
    int64_t raw_ns{0};
    int64_t uncertainty_ns{0};
};

/// The narrowest well-ordered bracket of `brackets` as a reading; empty if none is ordered.
[[nodiscard]] auto narrowest_bracket(std::span<PhcBracket const> brackets) noexcept -> std::optional<PhcReading>;

/// PHC0 interpolated at `phcd`'s instant between `first0` and `second0`. Fails with
/// invalid_argument unless first0.raw_ns <= phcd.raw_ns <= second0.raw_ns and first0 < second0.
[[nodiscard]] auto cross_timestamp(PhcReading const& first0, PhcReading const& phcd, PhcReading const& second0) noexcept
    -> StatusValue<PhcCrossSample>;

/// How a PhcDevice reads its clock against CLOCK_MONOTONIC_RAW, best first.
enum class PhcReadMethod : uint8_t
{
    precise,   ///< PTP_SYS_OFFSET_PRECISE: a hardware cross-timestamp (PCIe PTM)
    extended,  ///< PTP_SYS_OFFSET_EXTENDED with clockid CLOCK_MONOTONIC_RAW (Linux 6.11 and later)
    software,  ///< clock_gettime(RAW), clock_gettime(PHC), clock_gettime(RAW)
};

/// Hardware cross-timestamps are used when the device offers them; extended and software otherwise.
struct PhcDeviceConfig
{
    bool writable{false};      ///< open O_RDWR, needed for frequency and step
    bool allow_precise{true};  ///< false: never PTP_SYS_OFFSET_PRECISE
    int samples{9};            ///< brackets per read, the narrowest kept: 1 to 25 (PTP_MAX_SAMPLES)
};

/// An open /dev/ptpN. Movable, not copyable; reads never allocate.
class PhcDevice
{
  public:
    /// Open `path` and choose its read method. Fails with invalid_argument for a bad
    /// configuration, not_supported off Linux, or the open's errno.
    [[nodiscard]] static auto open(std::string_view path, PhcDeviceConfig config = {}) -> StatusValue<PhcDevice>;

    /// The device's descriptor, owned by this object.
    [[nodiscard]] auto fd() const noexcept -> int { return fd_.get(); }
    /// The read method chosen at open().
    [[nodiscard]] auto method() const noexcept -> PhcReadMethod { return method_; }
    /// The path given to open().
    [[nodiscard]] auto path() const noexcept -> std::string_view { return path_; }

    /// The PHC against CLOCK_MONOTONIC_RAW by method(), falling back to the next method when a
    /// hardware cross-timestamp fails (a PTM timeout); fails with the last errno.
    [[nodiscard]] auto read() const noexcept -> StatusValue<PhcReading>;
    /// The driver's max_adj (PTP_CLOCK_GETCAPS) in ppb.
    [[nodiscard]] auto max_frequency_ppb() const noexcept -> StatusValue<double>;
    /// The frequency offset in force, in ppb (clock_adjtime, modes 0). Needs `writable`: the
    /// kernel refuses clock_adjtime on a read-only PHC descriptor with EACCES.
    [[nodiscard]] auto frequency_ppb() const noexcept -> StatusValue<double>;
    /// Set the frequency offset (ADJ_FREQUENCY); positive runs the clock fast. Needs `writable`.
    [[nodiscard]] auto set_frequency_ppb(double ppb) const noexcept -> Status;
    /// Add `delta_ns` to the clock's time (ADJ_SETOFFSET). Needs `writable`.
    [[nodiscard]] auto step_ns(int64_t delta_ns) const noexcept -> Status;

  private:
    PhcDevice(FileDescriptor fd, std::string path, PhcDeviceConfig config) noexcept;
    /// Choose method_ (and extended_) by what the device supports and config_ allows.
    void probe_read_method() noexcept;

    FileDescriptor fd_;
    std::string path_;
    PhcDeviceConfig config_;
    PhcReadMethod method_{PhcReadMethod::software};
    bool extended_{false};  ///< PTP_SYS_OFFSET_EXTENDED with clockid works here
};

/// PHC0 (the engine's) and PHC_d (another NIC's) read at one instant: three reads,
/// PHC0 / PHC_d / PHC0, each the narrowest of `samples` (phc2sys's sysoff_measure, RAW-based).
class PhcCrossTimestamp
{
  public:
    /// Open both devices read-only; they may be the same device.
    [[nodiscard]] static auto open(std::string_view phc0_path, std::string_view phcd_path, PhcDeviceConfig config = {})
        -> StatusValue<PhcCrossTimestamp>;

    /// One pair; fails with a read's errno or invalid_argument for a disordered bracket.
    [[nodiscard]] auto measure() const noexcept -> StatusValue<PhcCrossSample>;

    [[nodiscard]] auto phc0() const noexcept -> PhcDevice const& { return phc0_; }
    [[nodiscard]] auto phcd() const noexcept -> PhcDevice const& { return phcd_; }

  private:
    PhcCrossTimestamp(PhcDevice phc0, PhcDevice phcd) noexcept;

    PhcDevice phc0_;
    PhcDevice phcd_;
};

}  // namespace statusbar::ptpclient
