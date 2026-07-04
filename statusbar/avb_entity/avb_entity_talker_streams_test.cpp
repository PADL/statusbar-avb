// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) tests for the CRF timestamp math extracted from
// TalkerStreams::transmit_crf. The regression under test (entity#4): CRF
// timestamps must track the LIVE media-clock position, not the media-clock
// anchor, so a stream whose gate opens seconds after start still conveys
// gPTP-now. transmit_crf() itself early-returns without a socket, but the
// timestamp math is now a pure function driven by a software MediaClockGenerator.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>

using namespace statusbar;
using statusbar::avb_entity::crf_aligned_base;
using statusbar::avb_entity::crf_sample_stride;
using statusbar::avb_entity::fill_crf_timestamps;

namespace {

// Milan-style CRF: 48 kHz base, 96 kHz audio -> each declared event spans 2
// audio samples; interval 96 -> stride 192 samples.
constexpr uint16_t kInterval = 96;
constexpr uint32_t kCrfBase = 48'000;
constexpr uint32_t kAudioRate = 96'000;  // 96 kHz audio

// Advance a fresh media clock by `ticks` packets of `nominal` samples at r=1.0
// and return the resulting live first_index. Anchors on the first advance.
auto advance_media_clock(ptpclient::MediaClockGenerator& mc, uint64_t t0_ns, uint32_t nominal, int ticks) -> uint64_t
{
    uint64_t const tick_ns = (static_cast<uint64_t>(nominal) * 1'000'000'000ULL) / kAudioRate;
    uint64_t first_index = 0;
    for (int k = 0; k < ticks; ++k) {
        auto const emit = mc.advance(t0_ns + (static_cast<uint64_t>(k) * tick_ns), 1.0, nominal);
        first_index = emit.first_index;
    }
    return first_index;
}

}  // namespace

TEST(crf_math, sample_stride)
{
    EXPECT_EQ(crf_sample_stride(kInterval, kCrfBase, kAudioRate), 192U);  // 96 * 96000 / 48000
    EXPECT_EQ(crf_sample_stride(1, 96'000, kAudioRate), 1U);
    EXPECT_EQ(crf_sample_stride(96, 0, kAudioRate), 0U);              // degenerate: base 0 -> 0, no div-by-zero
    EXPECT_EQ(crf_sample_stride(kInterval, kCrfBase, 48'000U), 96U);  // 48 kHz audio: 96 * 48000 / 48000
}

TEST(crf_math, aligned_base_floors_to_stride)
{
    EXPECT_EQ(crf_aligned_base(1000, 192), 960U);  // floor(1000/192)*192 = 5*192
    EXPECT_EQ(crf_aligned_base(960, 192), 960U);   // already aligned
    EXPECT_EQ(crf_aligned_base(1234, 0), 1234U);   // stride 0 -> passthrough
    // Successive PDUs advance first_index by a whole multiple of stride, so the
    // aligned base advances by exactly that multiple (contiguous, no gap).
    EXPECT_EQ(crf_aligned_base(960 + 192, 192) - crf_aligned_base(960, 192), 192U);
}

TEST(crf_math, fill_matches_media_clock_timestamps)
{
    ptpclient::MediaClockGenerator mc{};
    uint64_t const base = advance_media_clock(mc, /*t0_ns=*/1'000'000'000ULL, /*nominal=*/12, /*ticks=*/100);
    uint32_t const stride = crf_sample_stride(kInterval, kCrfBase, kAudioRate);
    uint64_t const aligned = crf_aligned_base(base, stride);

    constexpr uint16_t n_ts = 6;
    std::array<uint8_t, static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE> ts_data{};
    fill_crf_timestamps(mc, aligned, stride, n_ts, ts_data);

    for (uint16_t i = 0; i < n_ts; ++i) {
        auto const got = avtp::crf_get_timestamp(ts_data, i);
        EXPECT_TRUE(got.has_value());
        // Each timestamp is exactly the media clock's presentation time for that
        // sample index -- evenly spaced by `stride` samples.
        EXPECT_EQ(*got, mc.timestamp_for(aligned + (static_cast<uint64_t>(i) * stride)));
    }
}

// The regression itself: after the clock has been running ~1 s, a CRF PDU based
// on the LIVE position carries a timestamp ~1 s ahead of one based on the anchor
// (index 0). Before the fix, transmit_crf always used an anchor-relative counter
// starting at 0, so a gate that opened late emitted permanently-stale timestamps.
TEST(crf_math, live_base_tracks_now_not_anchor)
{
    ptpclient::MediaClockGenerator mc{};
    // 8000 ticks * 12 samples / 96 kHz = 1.0 s of media time elapsed.
    uint64_t const base = advance_media_clock(mc, /*t0_ns=*/1'000'000'000ULL, /*nominal=*/12, /*ticks=*/8000);
    uint32_t const stride = crf_sample_stride(kInterval, kCrfBase, kAudioRate);

    uint64_t const ts_live = mc.timestamp_for(crf_aligned_base(base, stride));
    uint64_t const ts_anchor = mc.timestamp_for(0);  // the old, stale behavior

    EXPECT_TRUE(ts_live > ts_anchor);
    uint64_t const delta = ts_live - ts_anchor;
    // ~1 s elapsed; allow a few ms of slack for pacing quantization.
    EXPECT_TRUE(delta > 995'000'000ULL);
    EXPECT_TRUE(delta < 1'005'000'000ULL);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_streams_test)
