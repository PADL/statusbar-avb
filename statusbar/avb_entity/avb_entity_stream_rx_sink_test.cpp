// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the StreamRxAudioSink seam as implemented by EntityUdptunBridge
// (god-object phase 3, RX-listener prep). on_listener_audio() is the abstraction a
// listener delivers received stream audio through; the bridge consumes it only for
// the configured tunnel-source stream, skips it while the test sweep is active, and
// transcodes AM824 MBLA (but not AAF int32) on the way in.
//
// Observable without a socket: with enable_ set, the AM824 path resizes the
// internal am824_transcode_buf_ (the MBLA->int32 scratch) before handing off to the
// (un-set-up, early-returning) ingest. So a grown buffer == "took the AM824 ingest
// path"; an empty buffer == "gated out / not the AM824 path".

#include "statusbar/avb_entity/avb_entity_udptun_bridge.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

using statusbar::avb_entity::AvbEntityAudioIOConfig;
using statusbar::avb_entity::EntityUdptunBridge;
using statusbar::avb_entity::MediaClockRateTracker;
using statusbar::avb_entity::StreamAudioFormat;

namespace {

/// Owns the references the bridge binds, so a bridge can be built in a test.
struct BridgeFixture
{
    AvbEntityAudioIOConfig config{};
    MediaClockRateTracker rate_tracker{};
    statusbar::itc::AtomicTripleBuffer<statusbar::ptpclient::GpsTaiSnapshot> tai_snapshot{};
    std::pmr::vector<float> audio_buffer{};
    size_t channels{8};
    std::atomic<uint64_t> last_gptp_ns{0};

    auto make() -> EntityUdptunBridge
    {
        return EntityUdptunBridge{config, rate_tracker, tai_snapshot, audio_buffer, channels, last_gptp_ns};
    }
};

// 2 MBLA quadlets (8 bytes) -> a non-empty AM824 payload.
constexpr std::array<uint8_t, 8> kMbla{0x40, 0x12, 0x34, 0x56, 0x41, 0x78, 0x9A, 0xBC};

}  // namespace

TEST(stream_rx_sink, am824_for_source_stream_takes_ingest_path)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;
    fx.config.sweep_enable = false;
    auto bridge = fx.make();
    bridge.enable_ = true;  // arm the ingest so the AM824 path allocates its transcode scratch

    EXPECT_TRUE(bridge.am824_transcode_buf_.empty());
    bridge.on_listener_audio(0, StreamAudioFormat::am824_mbla, kMbla);
    EXPECT_EQ(bridge.am824_transcode_buf_.size(), size_t{8});  // grew -> went down the AM824 ingest path
}

TEST(stream_rx_sink, skips_non_source_stream)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;  // tunnel sources stream 0
    auto bridge = fx.make();
    bridge.enable_ = true;

    bridge.on_listener_audio(1, StreamAudioFormat::am824_mbla, kMbla);  // stream 1 != source
    EXPECT_TRUE(bridge.am824_transcode_buf_.empty());                   // gated out
}

TEST(stream_rx_sink, skips_while_sweep_active)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;
    fx.config.sweep_enable = true;  // the sweep replaces the tunnel source
    auto bridge = fx.make();
    bridge.enable_ = true;

    bridge.on_listener_audio(0, StreamAudioFormat::am824_mbla, kMbla);
    EXPECT_TRUE(bridge.am824_transcode_buf_.empty());  // gated out
}

TEST(stream_rx_sink, aaf_does_not_take_am824_transcode_path)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;
    auto bridge = fx.make();
    bridge.enable_ = true;

    // AAF int32 is already linear -> goes straight to udptun_ingest_audio, never the
    // MBLA transcode. The format dispatch must NOT route AAF through the AM824 path.
    bridge.on_listener_audio(0, StreamAudioFormat::aaf_int32, kMbla);
    EXPECT_TRUE(bridge.am824_transcode_buf_.empty());
}

// Test runner

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_rx_sink_test)
