// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) test for the punch-service gating rule (entity#5). The
// substantive keepalive / egress self-heal it enables live inside
// udptun_punch_service (which needs a socket), but the GATE that decides
// whether the media thread runs the service at all is a pure predicate --
// pinned here so a future edit can't silently drop the DIRECT-SHARED arm and
// re-strand a no-STUN tunnel (its NAT pinhole would close when idle).

#include "statusbar/avb_entity/avb_entity_udptun_bridge.hpp"

#include "statusbar/test/test.hpp"

using namespace statusbar;
using Bridge = avb_entity::EntityUdptunBridge;

TEST(udptun_bridge, punch_service_gate_covers_direct_shared)
{
    // STUN worker path: punch_run drives it.
    EXPECT_TRUE(Bridge::punch_service_should_run(/*punch_run=*/true, /*direct_shared_mode=*/false));
    // DIRECT-SHARED path: no worker (punch_run stays false) but the service must
    // still run for the NAT keepalive + egress anchor-reset self-heal. This is the
    // regression -- the old gate was punch_run only, so this returned "don't run".
    EXPECT_TRUE(Bridge::punch_service_should_run(/*punch_run=*/false, /*direct_shared_mode=*/true));
    // Both flags set.
    EXPECT_TRUE(Bridge::punch_service_should_run(/*punch_run=*/true, /*direct_shared_mode=*/true));
    // Neither: an entity with no tunnel socket -- nothing to service.
    EXPECT_FALSE(Bridge::punch_service_should_run(/*punch_run=*/false, /*direct_shared_mode=*/false));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_udptun_bridge_test)
