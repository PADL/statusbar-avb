// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for AtdeccLocalPort's datagram intake and replies over an adopted connected
/// AF_UNIX socket: the input is a reply MAC (6 octets), then datagrams each prefixed by a 2-octet
/// length; a final odd octet closes the controller's end before the port reads.

#include "statusbar/nanoavb/nanoavb_local_port.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include <sys/socket.h>

namespace {

/// Send each length-prefixed datagram of @p input on @p fd; whether an odd octet is left over.
auto send_datagrams(int const fd, std::span<uint8_t const> input) -> bool
{
    while (input.size() >= 2) {
        size_t const length = std::min<size_t>((size_t{input[0]} << 8U) | input[1], input.size() - 2);
        (void)::send(fd, input.data() + 2, length, MSG_DONTWAIT);
        input = input.subspan(2 + length);
    }
    return !input.empty();
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    using statusbar::nanoavb::AtdeccLocalPort;
    std::span<uint8_t const> const input{data, size};
    statusbar::ieee::Eui48 reply_to{};
    std::copy_n(input.begin(), std::min<size_t>(input.size(), 6), reply_to.value.begin());
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
        return 0;
    }
    auto port = AtdeccLocalPort::adopt(fds[0]);
    if (!port) {
        ::close(fds[0]);
        ::close(fds[1]);
        return 0;
    }
    AtdeccLocalPort* const p = port->get();
    p->set_receiver(
        [p, reply_to](int64_t, statusbar::ieee::Eui48 const&, std::span<uint8_t const> frame) { (void)p->send(reply_to, frame); });
    if (send_datagrams(fds[1], input.subspan(std::min<size_t>(input.size(), 6)))) {
        ::close(fds[1]);
        fds[1] = -1;
    }
    p->on_ready(0);
    p->tick(1);
    if (fds[1] >= 0) {
        ::close(fds[1]);
    }
    return 0;
}
