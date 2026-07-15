// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the StreamRxAudioSink seam as implemented by EntityUdptunBridge
// (routed to UdptunIngestPath since refactor phase C). on_listener_audio() is the
// abstraction a listener delivers received stream audio through; the ingest
// consumes it only for the configured tunnel-source stream, skips it while the
// test sweep is active, and transcodes AM824 MBLA (but not AAF int32) on the way in.
//
// Observable without a socket: build_state() (the real arming path — the punch
// worker calls it long before any socket exists) pre-sizes the AM824->int32
// transcode scratch to one media tick; an MBLA payload LARGER than that makes the
// AM824 ingest path grow it. So a grown buffer == "took the AM824 ingest path";
// an unchanged buffer == "gated out / not the AM824 path". The (socket-less)
// downstream ingest just reframes and drops the send.

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

// An MBLA payload larger than the pre-sized transcode scratch (one media tick =
// (SAMPLES_PER_PACKET+1) * channels * 4 = 416 bytes here), so the AM824 path
// must grow the scratch — the observable.
auto big_mbla() -> std::vector<uint8_t>
{
    std::vector<uint8_t> mbla(1024, 0);
    for (size_t q = 0; q < mbla.size(); q += 4) {
        mbla[q] = 0x40;  // MBLA label
    }
    return mbla;
}

}  // namespace

TEST(stream_rx_sink, am824_for_source_stream_takes_ingest_path)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;
    fx.config.sweep_enable = false;
    auto bridge = fx.make();
    bridge.ingest().build_state();  // arm the ingest (socket-free)

    size_t const pre = bridge.ingest().am824_transcode_bytes();
    bridge.on_listener_audio(0, StreamAudioFormat::am824_mbla, big_mbla());
    EXPECT_TRUE(bridge.ingest().am824_transcode_bytes() > pre);  // grew -> went down the AM824 ingest path
}

TEST(stream_rx_sink, skips_non_source_stream)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;  // tunnel sources stream 0
    auto bridge = fx.make();
    bridge.ingest().build_state();

    size_t const pre = bridge.ingest().am824_transcode_bytes();
    bridge.on_listener_audio(1, StreamAudioFormat::am824_mbla, big_mbla());  // stream 1 != source
    EXPECT_EQ(bridge.ingest().am824_transcode_bytes(), pre);                 // gated out
}

TEST(stream_rx_sink, skips_while_sweep_active)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;
    fx.config.sweep_enable = true;  // the sweep replaces the tunnel source
    auto bridge = fx.make();
    bridge.ingest().build_state();

    size_t const pre = bridge.ingest().am824_transcode_bytes();
    bridge.on_listener_audio(0, StreamAudioFormat::am824_mbla, big_mbla());
    EXPECT_EQ(bridge.ingest().am824_transcode_bytes(), pre);  // gated out
}

TEST(stream_rx_sink, aaf_does_not_take_am824_transcode_path)
{
    BridgeFixture fx{};
    fx.config.udptun_source_stream = 0;
    auto bridge = fx.make();
    bridge.ingest().build_state();

    // AAF int32 is already linear -> goes straight to the plain ingest, never the
    // MBLA transcode. The format dispatch must NOT route AAF through the AM824 path.
    size_t const pre = bridge.ingest().am824_transcode_bytes();
    bridge.on_listener_audio(0, StreamAudioFormat::aaf_int32, big_mbla());
    EXPECT_EQ(bridge.ingest().am824_transcode_bytes(), pre);
}

// Test runner

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_rx_sink_test)
