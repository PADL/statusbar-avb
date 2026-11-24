// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/test/test.hpp"

#if defined(__linux__)

#    include "statusbar/gptp/gptp_slave_session.hpp"

namespace {

using namespace statusbar;
using namespace statusbar::gptp;

TEST(slave_session_start, missing_interface_or_no_caps_fails)
{
    // Without CAP_NET_RAW the raw socket open fails before iface lookup;
    // with caps but a bogus interface, iface lookup fails. Either way,
    // start() must return false cleanly.
    SlaveSessionConfig cfg{};
    cfg.interface = "this_iface_does_not_exist_12345";
    SlaveSession s{cfg};
    EXPECT_FALSE(s.start());
}

}  // namespace

#endif  // __linux__

TEST_MAIN(statusbar_gptp, gptp_slave_session_functional_test)
