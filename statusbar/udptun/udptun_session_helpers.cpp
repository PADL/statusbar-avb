// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_session_helpers.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <print>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#if defined(__linux__)
#    include "statusbar/gptp/gptp_slave_session.hpp"

#    include <net/if.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#    include <net/if.h>
#endif

namespace statusbar::udptun {

void bind_to_interface(int fd, int family, std::string const& iface)
{
    if (!net::bind_to_interface(fd, family, iface)) {
        std::println(stderr, "warning: bind_to_interface({}) failed: {}", iface, ::strerror(errno));
    }
}

auto is_ipv4_multicast(net::SocketAddress const& addr) -> bool
{
    if (addr.family() != AF_INET) {
        return false;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* sin = reinterpret_cast<sockaddr_in const*>(addr.sockaddr());
    auto const a = ntohl(sin->sin_addr.s_addr);
    return (a >> 28) == 0xE;  // 224.0.0.0/4
}

auto same_host(net::SocketAddress const& a, net::SocketAddress const& b) -> bool
{
    if (a.family() != b.family()) {
        return false;
    }
    if (a.family() == AF_INET) {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* sa = reinterpret_cast<sockaddr_in const*>(a.sockaddr());
        auto const* sb = reinterpret_cast<sockaddr_in const*>(b.sockaddr());
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        return sa->sin_addr.s_addr == sb->sin_addr.s_addr;
    }
    if (a.family() == AF_INET6) {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
        auto const* sa = reinterpret_cast<sockaddr_in6 const*>(a.sockaddr());
        auto const* sb = reinterpret_cast<sockaddr_in6 const*>(b.sockaddr());
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        return std::memcmp(&sa->sin6_addr, &sb->sin6_addr, sizeof(sa->sin6_addr)) == 0;
    }
    return false;
}

auto configure_multicast(int fd, std::string const& iface, net::SocketAddress const& group, int ttl) -> bool
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* sin = reinterpret_cast<sockaddr_in const*>(group.sockaddr());

    // ip_mreq is portable across Linux/macOS/BSD. The Linux extension
    // ip_mreqn (with imr_ifindex) is dropped here in favor of a separate
    // IP_MULTICAST_IF call that uses the resolved interface index.
    ip_mreq mreq{};
    mreq.imr_multiaddr = sin->sin_addr;
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        std::println(stderr, "IP_ADD_MEMBERSHIP failed: {}", ::strerror(errno));
        return false;
    }

    int const ttl_val = ttl;
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_val, sizeof(ttl_val)) < 0) {
        std::println(stderr, "IP_MULTICAST_TTL failed: {}", ::strerror(errno));
    }
    int const loop = 0;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    if (!iface.empty()) {
        unsigned const idx = ::if_nametoindex(iface.c_str());
        if (idx != 0) {
#if defined(IP_MULTICAST_IF)
#    if defined(__linux__)
            // Linux accepts an ip_mreqn with imr_ifindex via IP_MULTICAST_IF.
            ip_mreqn mreqn{};
            mreqn.imr_multiaddr = sin->sin_addr;
            mreqn.imr_address.s_addr = htonl(INADDR_ANY);
            mreqn.imr_ifindex = static_cast<int>(idx);
            ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &mreqn, sizeof(mreqn));
#    else
            // macOS / BSD use a 4-byte interface index for IP_MULTICAST_IF.
            uint32_t const idx32 = idx;
            ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &idx32, sizeof(idx32));
#    endif
#endif
        }
    }
    return true;
}

#if defined(__linux__)
auto build_pollfds(std::optional<gptp::SlaveSession> const& session, int udp_fd) noexcept -> SessionPollFds
{
    SessionPollFds fds;
    if (session.has_value()) {
        for (int const gfd : session->poll_fds()) {
            if (fds.try_push_back(pollfd{.fd = gfd, .events = POLLIN, .revents = 0}) == nullptr) {
                break;
            }
        }
    }
    (void)fds.try_push_back(pollfd{.fd = udp_fd, .events = POLLIN, .revents = 0});
    return fds;
}
#endif

auto build_pollfds_udp_only(int udp_fd) noexcept -> SessionPollFds
{
    return SessionPollFds{pollfd{.fd = udp_fd, .events = POLLIN, .revents = 0}};
}

}  // namespace statusbar::udptun
