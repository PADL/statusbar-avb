// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/test/test.hpp"

#if defined(__linux__)

#    include "statusbar/gptp/gptp_slave_session.hpp"

namespace {

using namespace statusbar;
using namespace statusbar::gptp;

TEST(slave_session_construct, default_config_constructs_cleanly)
{
    SlaveSessionConfig cfg{};
    cfg.interface = "doesnotexist0";  // must not require root
    SlaveSession s{cfg};
    EXPECT_EQ(s.poll_fds().size(), size_t{1});
    EXPECT_EQ(s.poll_fds()[0], -1);  // not started yet
    EXPECT_FALSE(s.owns_fd(0));
}

}  // namespace

#endif  // __linux__

TEST_MAIN(statusbar_gptp, gptp_slave_session_test)
