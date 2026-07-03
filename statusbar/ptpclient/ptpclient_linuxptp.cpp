// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_linuxptp.hpp"

#include "statusbar/status/throw_or_abort.hpp"

#if defined(__linux__)

namespace statusbar::ptpclient {

LinuxPtpClient::LinuxPtpClient(std::string_view device_path)
{
    if (auto status = open(device_path); !status) {
        statusbar::throw_or_abort(status.error());
    }
}

LinuxPtpClient::~LinuxPtpClient() noexcept
{
    close();
}

LinuxPtpClient::LinuxPtpClient(LinuxPtpClient&& other) noexcept
    : fd_{other.fd_}
    , clock_id_{other.clock_id_}
    , device_path_{std::move(other.device_path_)}
{
    other.fd_ = -1;
    other.clock_id_ = -1;
}

auto LinuxPtpClient::operator=(LinuxPtpClient&& other) noexcept -> LinuxPtpClient&
{
    if (this != &other) {
        close();
        fd_ = other.fd_;
        clock_id_ = other.clock_id_;
        device_path_ = std::move(other.device_path_);
        other.fd_ = -1;
        other.clock_id_ = -1;
    }
    return *this;
}

auto LinuxPtpClient::open(std::string_view device_path) noexcept -> Status
{
    close();

    if (device_path.empty()) {
        return failure(PtpError::invalid_device_path);
    }

    // Store device path
    device_path_ = std::string(device_path);

    // Open the PTP device read-only
    fd_ = ::open(device_path_.c_str(), O_RDONLY);
    if (fd_ < 0) {
        device_path_.clear();
        if (errno == ENOENT) {
            return failure(PtpError::device_not_found);
        }
        if (errno == EACCES || errno == EPERM) {
            return failure(PtpError::permission_denied);
        }
        return failure(PtpError::device_open_failed);
    }

    // Convert file descriptor to clock ID using Linux kernel's FD_TO_CLOCKID macro
    // From linux/time.h: #define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | CLOCKFD)
    // where CLOCKFD = 3
    // This creates a dynamic clock ID that clock_gettime() understands
    clock_id_ = ((~static_cast<clockid_t>(fd_)) << 3) | 3;

    return success();
}

void LinuxPtpClient::close() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    clock_id_ = -1;
    device_path_.clear();
}

auto LinuxPtpClient::get_time_ns() const noexcept -> StatusValue<int64_t>
{
    if (fd_ < 0) {
        return failure(PtpError::device_not_open);
    }

    struct timespec ts{};
    errno = 0;  // Clear errno before call
    int const rc = ::clock_gettime(clock_id_, &ts);
    if (rc < 0) {
        // Capture actual system error for better diagnostics
        return failure(std::error_code(errno, std::system_category()));
    }

    // Convert to nanoseconds since epoch
    int64_t const ns = (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL) + static_cast<int64_t>(ts.tv_nsec);

    return success(ns);
}

auto LinuxPtpClient::get_capabilities() const noexcept -> StatusValue<ptp_clock_caps>
{
    if (fd_ < 0) {
        return failure(PtpError::device_not_open);
    }

    ptp_clock_caps caps{};
    if (::ioctl(fd_, PTP_CLOCK_GETCAPS, &caps) < 0) {
        // A GETCAPS ioctl failure means the device doesn't support the capabilities
        // query -- not a clock_gettime failure (the previous, misleading code).
        return failure(PtpError::not_supported);
    }

    return success(caps);
}

}  // namespace statusbar::ptpclient

#endif  // __linux__
