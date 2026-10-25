// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_phc.hpp"

#if defined(__linux__)
#    include <fcntl.h>
#    include <unistd.h>

#    include <linux/ptp_clock.h>
#    include <sys/ioctl.h>
#    include <sys/timex.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <system_error>
#include <utility>

namespace statusbar::ptpclient {

auto narrowest_bracket(std::span<PhcBracket const> const brackets) noexcept -> std::optional<PhcReading>
{
    std::optional<PhcReading> best;
    for (PhcBracket const& b : brackets) {
        int64_t const width = b.after_ns - b.before_ns;
        if (width < 0 || (best && width >= best->width_ns)) {
            continue;
        }
        best = PhcReading{.raw_ns = b.before_ns + (width / 2), .phc_ns = b.phc_ns, .width_ns = width};
    }
    return best;
}

auto cross_timestamp(PhcReading const& first0, PhcReading const& phcd, PhcReading const& second0) noexcept
    -> StatusValue<PhcCrossSample>
{
    int64_t const span = second0.raw_ns - first0.raw_ns;
    int64_t const into = phcd.raw_ns - first0.raw_ns;
    if (span <= 0 || into < 0 || into > span) {
        return failure(std::errc::invalid_argument);
    }
    auto const advance = static_cast<__int128>(second0.phc_ns - first0.phc_ns) * into / span;
    return PhcCrossSample{
        .phc0_ns = first0.phc_ns + static_cast<int64_t>(advance),
        .phcd_ns = phcd.phc_ns,
        .raw_ns = phcd.raw_ns,
        .uncertainty_ns = (phcd.width_ns / 2) + ((first0.width_ns + second0.width_ns) / 4),
    };
}

PhcDevice::PhcDevice(FileDescriptor fd, std::string path, PhcDeviceConfig const config) noexcept
    : fd_{std::move(fd)}
    , path_{std::move(path)}
    , config_{config}
{}

PhcCrossTimestamp::PhcCrossTimestamp(PhcDevice phc0, PhcDevice phcd) noexcept
    : phc0_{std::move(phc0)}
    , phcd_{std::move(phcd)}
{}

auto PhcCrossTimestamp::open(std::string_view const phc0_path, std::string_view const phcd_path, PhcDeviceConfig const config)
    -> StatusValue<PhcCrossTimestamp>
{
    PhcDeviceConfig read_only = config;
    read_only.writable = false;
    auto phc0 = PhcDevice::open(phc0_path, read_only);
    if (!phc0) {
        return forward_failure(phc0);
    }
    auto phcd = PhcDevice::open(phcd_path, read_only);
    if (!phcd) {
        return forward_failure(phcd);
    }
    return PhcCrossTimestamp{std::move(*phc0), std::move(*phcd)};
}

auto PhcCrossTimestamp::measure() const noexcept -> StatusValue<PhcCrossSample>
{
    auto const first0 = phc0_.read();
    if (!first0) {
        return forward_failure(first0);
    }
    auto const d = phcd_.read();
    if (!d) {
        return forward_failure(d);
    }
    auto const second0 = phc0_.read();
    if (!second0) {
        return forward_failure(second0);
    }
    return cross_timestamp(*first0, *d, *second0);
}

#if defined(__linux__)

namespace {

constexpr int64_t NS_PER_SECOND = 1'000'000'000;
constexpr int MAX_SAMPLES = 25;             ///< PTP_MAX_SAMPLES
constexpr double FREQUENCY_SCALE = 65.536;  ///< timex.freq is ppm with a 16-bit fraction

[[nodiscard]] auto errno_failure() noexcept -> std::unexpected<std::error_code>
{
    return failure(std::error_code{errno, std::generic_category()});
}

/// struct ptp_sys_offset_extended as Linux 6.11 has it, with `clockid` in place of rsv[0];
/// older headers lack the field, older kernels refuse a non-zero one with EINVAL.
struct SysOffsetExtended
{
    unsigned int n_samples;
    clockid_t clockid;
    unsigned int rsv[2];
    ptp_clock_time ts[MAX_SAMPLES][3];
};
static_assert(sizeof(SysOffsetExtended) == sizeof(ptp_sys_offset_extended));
static_assert(sizeof(clockid_t) == sizeof(unsigned int));

[[nodiscard]] constexpr auto to_ns(ptp_clock_time const& t) noexcept -> int64_t
{
    return (t.sec * NS_PER_SECOND) + t.nsec;
}

[[nodiscard]] auto to_ns(timespec const& t) noexcept -> int64_t
{
    return (static_cast<int64_t>(t.tv_sec) * NS_PER_SECOND) + t.tv_nsec;
}

/// The kernel's FD_TO_CLOCKID.
[[nodiscard]] auto fd_to_clockid(int const fd) noexcept -> clockid_t
{
    return static_cast<clockid_t>((~static_cast<unsigned int>(fd) << 3U) | 3U);
}

[[nodiscard]] auto read_precise(int const fd) noexcept -> StatusValue<PhcReading>
{
    ptp_sys_offset_precise precise{};
    if (::ioctl(fd, PTP_SYS_OFFSET_PRECISE, &precise) != 0) {
        return errno_failure();
    }
    return PhcReading{.raw_ns = to_ns(precise.sys_monoraw), .phc_ns = to_ns(precise.device), .width_ns = 0};
}

[[nodiscard]] auto read_extended(int const fd, int const samples) noexcept -> StatusValue<PhcReading>
{
    SysOffsetExtended offset{};
    offset.n_samples = static_cast<unsigned int>(samples);
    offset.clockid = CLOCK_MONOTONIC_RAW;
    if (::ioctl(fd, PTP_SYS_OFFSET_EXTENDED, &offset) != 0) {
        return errno_failure();
    }
    std::array<PhcBracket, MAX_SAMPLES> brackets{};
    for (size_t i = 0; i < static_cast<size_t>(samples); ++i) {
        brackets[i] =
            PhcBracket{.before_ns = to_ns(offset.ts[i][0]), .phc_ns = to_ns(offset.ts[i][1]), .after_ns = to_ns(offset.ts[i][2])};
    }
    auto const best = narrowest_bracket(std::span{brackets}.first(static_cast<size_t>(samples)));
    if (!best) {
        return failure(std::errc::invalid_argument);
    }
    return *best;
}

[[nodiscard]] auto read_software(int const fd, int const samples) noexcept -> StatusValue<PhcReading>
{
    clockid_t const clock = fd_to_clockid(fd);
    std::array<PhcBracket, MAX_SAMPLES> brackets{};
    for (size_t i = 0; i < static_cast<size_t>(samples); ++i) {
        timespec before{};
        timespec phc{};
        timespec after{};
        ::clock_gettime(CLOCK_MONOTONIC_RAW, &before);
        if (::clock_gettime(clock, &phc) != 0) {
            return errno_failure();
        }
        ::clock_gettime(CLOCK_MONOTONIC_RAW, &after);
        brackets[i] = PhcBracket{.before_ns = to_ns(before), .phc_ns = to_ns(phc), .after_ns = to_ns(after)};
    }
    auto const best = narrowest_bracket(std::span{brackets}.first(static_cast<size_t>(samples)));
    if (!best) {
        return failure(std::errc::invalid_argument);
    }
    return *best;
}

}  // namespace

auto PhcDevice::open(std::string_view const path, PhcDeviceConfig const config) -> StatusValue<PhcDevice>
{
    if (path.empty() || config.samples < 1 || config.samples > MAX_SAMPLES) {
        return failure(std::errc::invalid_argument);
    }
    std::string const name{path};
    FileDescriptor fd{::open(name.c_str(), (config.writable ? O_RDWR : O_RDONLY) | O_CLOEXEC)};
    if (!fd.valid()) {
        return errno_failure();
    }
    PhcDevice device{std::move(fd), name, config};
    ptp_clock_caps caps{};
    bool const cross = ::ioctl(device.fd(), PTP_CLOCK_GETCAPS, &caps) == 0 && caps.cross_timestamping != 0;
    device.extended_ = read_extended(device.fd(), 1).has_value();
    if (config.allow_precise && cross && read_precise(device.fd())) {
        device.method_ = PhcReadMethod::precise;
    } else {
        device.method_ = device.extended_ ? PhcReadMethod::extended : PhcReadMethod::software;
    }
    return device;
}

auto PhcDevice::read() const noexcept -> StatusValue<PhcReading>
{
    if (method_ == PhcReadMethod::precise) {
        if (auto reading = read_precise(fd_.get())) {
            return reading;
        }
    }
    if (extended_) {
        if (auto reading = read_extended(fd_.get(), config_.samples); reading || method_ == PhcReadMethod::extended) {
            return reading;
        }
    }
    return read_software(fd_.get(), config_.samples);
}

auto PhcDevice::max_frequency_ppb() const noexcept -> StatusValue<double>
{
    ptp_clock_caps caps{};
    if (::ioctl(fd_.get(), PTP_CLOCK_GETCAPS, &caps) != 0) {
        return errno_failure();
    }
    return static_cast<double>(caps.max_adj);
}

auto PhcDevice::frequency_ppb() const noexcept -> StatusValue<double>
{
    timex tx{};
    if (::clock_adjtime(fd_to_clockid(fd_.get()), &tx) < 0) {
        return errno_failure();
    }
    return static_cast<double>(tx.freq) / FREQUENCY_SCALE;
}

auto PhcDevice::set_frequency_ppb(double const ppb) const noexcept -> Status
{
    timex tx{};
    tx.modes = ADJ_FREQUENCY;
    tx.freq = std::lround(ppb * FREQUENCY_SCALE);
    if (::clock_adjtime(fd_to_clockid(fd_.get()), &tx) < 0) {
        return errno_failure();
    }
    return success();
}

auto PhcDevice::step_ns(int64_t const delta_ns) const noexcept -> Status
{
    timex tx{};
    tx.modes = ADJ_SETOFFSET | ADJ_NANO;
    int64_t seconds = delta_ns / NS_PER_SECOND;
    int64_t nanoseconds = delta_ns % NS_PER_SECOND;
    if (nanoseconds < 0) {  // the kernel wants tv_usec (ns with ADJ_NANO) in [0, 1e9)
        nanoseconds += NS_PER_SECOND;
        --seconds;
    }
    tx.time.tv_sec = static_cast<time_t>(seconds);
    tx.time.tv_usec = static_cast<suseconds_t>(nanoseconds);
    if (::clock_adjtime(fd_to_clockid(fd_.get()), &tx) < 0) {
        return errno_failure();
    }
    return success();
}

#else

auto PhcDevice::open(std::string_view /*path*/, PhcDeviceConfig /*config*/) -> StatusValue<PhcDevice>
{
    return failure(std::errc::not_supported);
}

auto PhcDevice::read() const noexcept -> StatusValue<PhcReading>
{
    return failure(std::errc::not_supported);
}

auto PhcDevice::max_frequency_ppb() const noexcept -> StatusValue<double>
{
    return failure(std::errc::not_supported);
}

auto PhcDevice::frequency_ppb() const noexcept -> StatusValue<double>
{
    return failure(std::errc::not_supported);
}

auto PhcDevice::set_frequency_ppb(double /*ppb*/) const noexcept -> Status
{
    return failure(std::errc::not_supported);
}

auto PhcDevice::step_ns(int64_t /*delta_ns*/) const noexcept -> Status
{
    return failure(std::errc::not_supported);
}

#endif  // __linux__

}  // namespace statusbar::ptpclient
