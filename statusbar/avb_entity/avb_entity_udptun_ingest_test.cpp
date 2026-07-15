// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the tunnel-ingest AM824->int32 transcode helper
// (avb_entity_udptun_ingest.hpp): drop the MBLA label byte and MSB-align the
// 24-bit audio into an int32 quadlet. Symmetric to the egress deinterleave; the
// buffer-agnostic seam the ingest move (god-object phase 2) writes through.

#include "statusbar/avb_entity/avb_entity_udptun_ingest.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <vector>

using statusbar::avb_entity::udptun_am824_mbla_to_int32;

TEST(udptun_ingest, drops_label_and_msb_aligns)
{
    // One MBLA quadlet: label 0x40, audio 0x123456 (big-endian).
    std::array<uint8_t, 4> mbla{0x40, 0x12, 0x34, 0x56};
    std::array<uint8_t, 4> out{0xAA, 0xAA, 0xAA, 0xAA};
    auto const n = udptun_am824_mbla_to_int32(mbla, out);
    EXPECT_EQ(n, size_t{4});
    // int32 = audio << 8 = [0x12][0x34][0x56][0x00]; label is gone.
    EXPECT_EQ(static_cast<int>(out[0]), 0x12);
    EXPECT_EQ(static_cast<int>(out[1]), 0x34);
    EXPECT_EQ(static_cast<int>(out[2]), 0x56);
    EXPECT_EQ(static_cast<int>(out[3]), 0x00);
}

TEST(udptun_ingest, transcodes_multiple_quadlets)
{
    // 3 samples, distinct labels (which must all be discarded).
    std::vector<uint8_t> mbla{0x40, 0x00, 0x00, 0x01, 0x41, 0xFF, 0xFF, 0xFF, 0x42, 0x80, 0x00, 0x00};
    std::vector<uint8_t> out(12, 0xCC);
    auto const n = udptun_am824_mbla_to_int32(mbla, out);
    EXPECT_EQ(n, size_t{12});
    std::array<uint8_t, 12> expect{0x00, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0x00, 0x00, 0x00};
    for (size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(static_cast<int>(out[i]), static_cast<int>(expect[i]));
    }
}

TEST(udptun_ingest, processes_only_whole_quadlets)
{
    // 6 input bytes = 1 whole quadlet + 2 trailing bytes (ignored).
    std::array<uint8_t, 6> mbla{0x40, 0x11, 0x22, 0x33, 0x99, 0x99};
    std::array<uint8_t, 8> out{};
    out.fill(0x7E);
    auto const n = udptun_am824_mbla_to_int32(mbla, out);
    EXPECT_EQ(n, size_t{4});
    EXPECT_EQ(static_cast<int>(out[0]), 0x11);
    EXPECT_EQ(static_cast<int>(out[3]), 0x00);
    EXPECT_EQ(static_cast<int>(out[4]), 0x7E);  // beyond n: untouched
}

TEST(udptun_ingest, bounded_by_smaller_span)
{
    // out only holds 1 quadlet though mbla holds 2 -> stop at out's capacity.
    std::array<uint8_t, 8> mbla{0x40, 0x01, 0x02, 0x03, 0x41, 0x04, 0x05, 0x06};
    std::array<uint8_t, 4> out{};
    auto const n = udptun_am824_mbla_to_int32(mbla, out);
    EXPECT_EQ(n, size_t{4});
    EXPECT_EQ(static_cast<int>(out[0]), 0x01);
}

//
// udptun_source_streaming — the ONE predicate behind both the punch keepalive
// and the silence-source gate (they must mirror or the two threads double-feed
// the reframer; refactor phase C pinned them to this shared definition).
//

TEST(udptun_ingest, source_streaming_within_window)
{
    using statusbar::avb_entity::udptun_source_streaming;
    int64_t const now = 10'000'000'000;
    EXPECT_TRUE(udptun_source_streaming(now - 50'000'000, now));    // 50 ms ago: streaming
    EXPECT_FALSE(udptun_source_streaming(now - 150'000'000, now));  // 150 ms ago: idle
    EXPECT_FALSE(udptun_source_streaming(0, now));                  // never ingested
    EXPECT_FALSE(udptun_source_streaming(now - 50'000'000, 0));     // no clock -> not streaming
}

//
// udptun_sweep_frames_due — TAI-locked sweep pacing (moved out of the entity's
// process_audio in refactor phase C): cumulative frames track elapsed TAI at
// the sample rate exactly, with catch-up bursts bounded by the cap.
//

TEST(udptun_ingest, sweep_pacing_tracks_elapsed_tai)
{
    using statusbar::avb_entity::udptun_sweep_frames_due;
    constexpr uint32_t RATE = 96000;
    // 1 ms elapsed @ 96 kHz = 96 frames due; none emitted yet.
    EXPECT_EQ(udptun_sweep_frames_due(1'000'000, 0, RATE, 1000), size_t{96});
    // Already caught up -> nothing due.
    EXPECT_EQ(udptun_sweep_frames_due(1'000'000, 96, RATE, 1000), size_t{0});
    // Emitted ahead of the clock (anchor just moved) -> nothing due, no underflow.
    EXPECT_EQ(udptun_sweep_frames_due(1'000'000, 200, RATE, 1000), size_t{0});
    // Negative elapsed (clock step) -> nothing due.
    EXPECT_EQ(udptun_sweep_frames_due(-5'000'000, 0, RATE, 1000), size_t{0});
    // A long stall is capped, not burst all at once: 1 s behind but cap 48.
    EXPECT_EQ(udptun_sweep_frames_due(1'000'000'000, 0, RATE, 48), size_t{48});
}

// Test runner

TEST_MAIN(statusbar_avb_entity, avb_entity_udptun_ingest_test)
