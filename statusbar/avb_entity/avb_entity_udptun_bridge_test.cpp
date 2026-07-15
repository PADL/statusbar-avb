// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) tests for the tunnel's media-thread gates.
//
// punch_service_should_run (UdptunTransport): the substantive keepalive /
// egress self-heal it enables live inside service() (which needs a socket),
// but the GATE that decides whether the media thread runs the service at all
// is a pure predicate -- pinned here so a future edit can't silently drop the
// DIRECT-SHARED arm and re-strand a no-STUN tunnel (its NAT pinhole would
// close when idle).
//
// should_emit_silence (EntityUdptunBridge facade): the silence source must
// stay OFF until the ingest is armed AND configured, and must mirror the
// keepalive's streaming predicate so real audio always wins.

#include "statusbar/avb_entity/avb_entity_udptun_bridge.hpp"

#include "statusbar/test/test.hpp"

#include <atomic>
#include <memory_resource>
#include <vector>

using namespace statusbar;
using avb_entity::EntityUdptunBridge;
using avb_entity::UdptunTransport;

TEST(udptun_bridge, punch_service_gate_covers_direct_shared)
{
    // STUN worker path: punch_run drives it.
    EXPECT_TRUE(UdptunTransport::punch_service_should_run(/*punch_run=*/true, /*direct_shared_mode=*/false));
    // DIRECT-SHARED path: no worker (punch_run stays false) but the service must
    // still run for the NAT keepalive + egress anchor-reset self-heal. This is the
    // regression -- the old gate was punch_run only, so this returned "don't run".
    EXPECT_TRUE(UdptunTransport::punch_service_should_run(/*punch_run=*/false, /*direct_shared_mode=*/true));
    // Both flags set.
    EXPECT_TRUE(UdptunTransport::punch_service_should_run(/*punch_run=*/true, /*direct_shared_mode=*/true));
    // Neither: an entity with no tunnel socket -- nothing to service.
    EXPECT_FALSE(UdptunTransport::punch_service_should_run(/*punch_run=*/false, /*direct_shared_mode=*/false));
}

namespace {

/// Owns the references the bridge binds, so a bridge can be built in a test.
struct BridgeFixture
{
    avb_entity::AvbEntityAudioIOConfig config{};
    avb_entity::MediaClockRateTracker rate_tracker{};
    itc::AtomicTripleBuffer<ptpclient::GpsTaiSnapshot> tai_snapshot{};
    std::pmr::vector<float> audio_buffer{};
    size_t channels{8};
    std::atomic<uint64_t> last_gptp_ns{0};

    auto make() -> EntityUdptunBridge
    {
        return EntityUdptunBridge{config, rate_tracker, tai_snapshot, audio_buffer, channels, last_gptp_ns};
    }
};

}  // namespace

TEST(udptun_bridge, silence_gate_requires_armed_and_configured_ingest)
{
    BridgeFixture fx{};
    fx.config.udptun_silence_source = true;
    auto bridge = fx.make();

    // Ingest not armed (no build_state): never emit silence, configured or not.
    EXPECT_FALSE(bridge.should_emit_silence(1'000'000'000));

    bridge.ingest().build_state();
    // Armed + configured + no real audio ever ingested -> the filler runs.
    EXPECT_TRUE(bridge.should_emit_silence(1'000'000'000));
}

TEST(udptun_bridge, silence_gate_off_when_not_configured)
{
    BridgeFixture fx{};
    fx.config.udptun_silence_source = false;  // listener-sourced node: keepalive covers idle
    auto bridge = fx.make();
    bridge.ingest().build_state();
    EXPECT_FALSE(bridge.should_emit_silence(1'000'000'000));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_udptun_bridge_test)
