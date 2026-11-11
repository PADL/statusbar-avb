// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_pulse.hpp"

#if defined(__linux__)
#    include "statusbar/net/net_socket.hpp"

#    include <unistd.h>

#    include <linux/ptp_clock.h>
#    include <sys/ioctl.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace statusbar::ptpclient {

namespace {

constexpr int64_t SECOND_NS = 1'000'000'000;

/// The whole second nearest @p ns.
[[nodiscard]] constexpr auto nearest_second(int64_t const ns) noexcept -> int64_t
{
    int64_t const shifted = ns + (SECOND_NS / 2);
    int64_t const seconds = (shifted / SECOND_NS) - (shifted % SECOND_NS < 0 ? 1 : 0);
    return seconds * SECOND_NS;
}

[[nodiscard]] constexpr auto magnitude(int64_t const ns) noexcept -> int64_t
{
    return ns < 0 ? -ns : ns;
}

}  // namespace

auto PulseLock::pulse(int64_t const event_ns, int64_t const reference_ns, int64_t const local_ns) noexcept -> bool
{
    int64_t const second = nearest_second(event_ns);
    if (magnitude(event_ns - second) > params_.edge_window_ns) {
        return false;  // the pulse's other edge
    }
    offset_ns_ = event_ns - second;
    last_pulse_ns_ = local_ns;
    // The reference read within a second after the edge: the sink is not whole seconds off, which a pulse
    // cannot show; a reactor late by under a second still finds it so.
    bool const same_second = reference_ns - second > -(SECOND_NS / 2) && reference_ns - second < SECOND_NS;
    return notify_pulse(same_second && magnitude(offset_ns_) <= params_.max_offset_ns);
}

auto PulseLock::expire(int64_t const local_ns) noexcept -> bool
{
    if (local_ns - last_pulse_ns_ < params_.max_silence_ns) {
        return false;
    }
    return notify_pulse(false);
}

auto PulseLock::notify_pulse(bool const good) noexcept -> bool
{
    good_in_a_row_ = good ? std::min(good_in_a_row_ + 1, params_.settle_pulses) : 0;
    bool const locked = good && good_in_a_row_ >= params_.settle_pulses;
    bool const changed = locked != locked_;
    locked_ = locked;
    return changed;
}

ExttsReader::ExttsReader(PhcDevice phc, uint32_t const channel, OnEvent on_event) noexcept
    : phc_{std::move(phc)}
    , channel_{channel}
    , on_event_{std::move(on_event)}
{}

#if defined(__linux__)

auto ExttsReader::open(std::string_view const phc_path, uint32_t channel, OnEvent on_event)
    -> StatusValue<std::unique_ptr<ExttsReader>>
{
    auto phc = PhcDevice::open(phc_path);
    if (!phc) {
        return forward_failure(phc);
    }
    // Without the mask ioctls (before Linux 6.7) a PHC has one queue, its owner's: reading it would steal.
    if (::ioctl(phc->fd(), PTP_MASK_CLEAR_ALL) != 0 || ::ioctl(phc->fd(), PTP_MASK_EN_SINGLE, &channel) != 0) {
        int const error = errno;
        return failure(error == ENOTTY ? std::make_error_code(std::errc::not_supported) : net::errno_code(error));
    }
    return std::unique_ptr<ExttsReader>{new ExttsReader{std::move(*phc), channel, std::move(on_event)}};
}

void ExttsReader::on_ready(int64_t const now_ns)
{
    // One read: ptp_read() blocks on an empty queue whatever O_NONBLOCK says, and poll found it not empty.
    std::array<ptp_extts_event, 8> events{};
    ssize_t const n = ::read(phc_.fd(), events.data(), sizeof(events));
    if (n < 0) {
        finished_ = errno != EAGAIN && errno != EINTR;
        return;
    }
    size_t const count = static_cast<size_t>(n) / sizeof(ptp_extts_event);
    for (size_t i = 0; i < count; ++i) {
        ptp_extts_event const& event = events.at(i);
        if (event.index == channel_ && on_event_) {
            on_event_((event.t.sec * SECOND_NS) + event.t.nsec, now_ns);
        }
    }
}

#else

auto ExttsReader::open(std::string_view /*phc_path*/, uint32_t /*channel*/, OnEvent /*on_event*/)
    -> StatusValue<std::unique_ptr<ExttsReader>>
{
    return failure(std::errc::not_supported);
}

void ExttsReader::on_ready(int64_t /*now_ns*/)
{}

#endif  // __linux__

}  // namespace statusbar::ptpclient
