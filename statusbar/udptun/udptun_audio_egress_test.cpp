// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_audio_egress.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;

namespace {

// Small egress: 2 ch, 1000 Hz, 2 bytes/sample, 4 frames/packet, WCL 0.
// frame_bytes = 4, packet = 16 bytes, ns_per_frame = 1e9/1000 = 1'000'000 ns.
using Egress = udptun::AudioEgress<64, 8>;

auto small_config(int64_t wcl_ns) -> Egress::Config
{
    Egress::Config c{};
    c.channels = 2;
    c.sample_rate_hz = 1000;
    c.bytes_per_sample = 2;
    c.frames_per_packet = 4;
    c.wcl_ns = wcl_ns;
    return c;
}

// A packet of 4 frames where frame f, all bytes = (start_byte + f).
auto make_packet(uint8_t start_byte, uint16_t n_frames, size_t frame_bytes) -> std::vector<uint8_t>
{
    std::vector<uint8_t> v(static_cast<size_t>(n_frames) * frame_bytes, 0);
    for (uint16_t f = 0; f < n_frames; ++f) {
        for (size_t b = 0; b < frame_bytes; ++b) {
            v[(f * frame_bytes) + b] = static_cast<uint8_t>(start_byte + f);
        }
    }
    return v;
}

}  // namespace

// Before any submit, playout is all silence and reports 0 real frames.
TEST(audio_egress_empty, silence_before_anchor)
{
    Egress eg{small_config(0)};
    std::vector<uint8_t> out(size_t{4} * 4, 0xAA);
    auto const real = eg.playout(/*now*/ 1'000'000, out, 4);
    EXPECT_EQ(real, size_t{0});
    EXPECT_FALSE(eg.anchored());
    for (auto b : out) {
        EXPECT_EQ(b, uint8_t{0});  // zero-filled
    }
}

// Submit one packet at TAI 0; with WCL 0, playing out at now=0 returns its frames.
TEST(audio_egress_basic, single_packet_roundtrip)
{
    Egress eg{small_config(/*wcl*/ 0)};
    auto const pkt = make_packet(/*start*/ 10, 4, eg.frame_bytes());
    EXPECT_TRUE(eg.submit(/*src_tai*/ 0, pkt, 4));
    EXPECT_TRUE(eg.anchored());

    std::vector<uint8_t> out(size_t{4} * 4, 0);
    auto const real = eg.playout(/*now*/ 0, out, 4);
    EXPECT_EQ(real, size_t{4});
    // frame 0 bytes == 10, frame 3 last byte == 13.
    EXPECT_EQ(out[0], uint8_t{10});
    EXPECT_EQ(out[15], uint8_t{13});
}

// Sub-packet serving: read 2 frames at a time out of a 4-frame packet.
TEST(audio_egress_subpacket, two_frame_reads)
{
    Egress eg{small_config(0)};
    auto const pkt = make_packet(20, 4, eg.frame_bytes());
    eg.submit(0, pkt, 4);

    std::vector<uint8_t> out(size_t{2} * 4, 0);
    // now=0 -> frames 0,1
    EXPECT_EQ(eg.playout(0, out, 2), size_t{2});
    EXPECT_EQ(out[0], uint8_t{20});  // frame 0
    EXPECT_EQ(out[4], uint8_t{21});  // frame 1
    // now=2 frames later (2 * 1e6 ns) -> frames 2,3
    EXPECT_EQ(eg.playout(2'000'000, out, 2), size_t{2});
    EXPECT_EQ(out[0], uint8_t{22});  // frame 2
    EXPECT_EQ(out[4], uint8_t{23});  // frame 3
}

// WCL delay: a packet at src_tai=0 plays out at now = WCL, not now = 0.
TEST(audio_egress_wcl, delays_playout_by_wcl)
{
    int64_t const wcl = 10'000'000;  // 10 ms = 10 frames @ 1 kHz
    Egress eg{small_config(wcl)};
    auto const pkt = make_packet(30, 4, eg.frame_bytes());
    eg.submit(/*src_tai*/ 0, pkt, 4);

    std::vector<uint8_t> out(size_t{4} * 4, 0);
    // At now=0: needed source = 0 - wcl < epoch -> silence.
    EXPECT_EQ(eg.playout(0, out, 4), size_t{0});
    // At now=wcl: needed source = 0 -> the packet's frame 0.
    EXPECT_EQ(eg.playout(wcl, out, 4), size_t{4});
    EXPECT_EQ(out[0], uint8_t{30});
}

// Drop-to-0: a gap between packets conceals the missing packet with silence,
// and a later-arriving packet still plays at its correct time slot.
TEST(audio_egress_gap, missing_packet_is_silence)
{
    Egress eg{small_config(0)};
    auto const p0 = make_packet(40, 4, eg.frame_bytes());
    auto const p2 = make_packet(60, 4, eg.frame_bytes());
    eg.submit(/*src_tai*/ 0, p0, 4);          // frames 0..3
    eg.submit(/*src_tai*/ 8'000'000, p2, 4);  // frames 8..11 (packet 1 = frames 4..7 missing)

    std::vector<uint8_t> out(size_t{4} * 4, 0);
    // packet 0 present
    EXPECT_EQ(eg.playout(0, out, 4), size_t{4});
    EXPECT_EQ(out[0], uint8_t{40});
    // packet 1 (frames 4..7) missing -> all silence
    EXPECT_EQ(eg.playout(4'000'000, out, 4), size_t{0});
    for (auto b : out) {
        EXPECT_EQ(b, uint8_t{0});
    }
    // packet 2 (frames 8..11) present at its slot
    EXPECT_EQ(eg.playout(8'000'000, out, 4), size_t{4});
    EXPECT_EQ(out[0], uint8_t{60});
}

// Reordering: a packet that arrives after a later one still lands in its slot.
TEST(audio_egress_reorder, out_of_order_submit)
{
    Egress eg{small_config(0)};
    auto const p0 = make_packet(70, 4, eg.frame_bytes());
    auto const p1 = make_packet(80, 4, eg.frame_bytes());
    // Submit packet 1 (frames 4..7) FIRST -- it anchors the epoch at its tai.
    // Then packet 0 arrives; its frame index is negative relative to that epoch,
    // so it is dropped (before-epoch). This documents the v1 anchor behavior.
    eg.submit(/*src_tai*/ 4'000'000, p1, 4);
    EXPECT_TRUE(eg.anchored());
    // Packet 0 (src_tai 0) is now before the epoch -> negative frame -> dropped.
    EXPECT_FALSE(eg.submit(/*src_tai*/ 0, p0, 4));
    // Packet 1 plays at its own time (epoch -> frame 0 here).
    std::vector<uint8_t> out(size_t{4} * 4, 0);
    EXPECT_EQ(eg.playout(4'000'000, out, 4), size_t{4});
    EXPECT_EQ(out[0], uint8_t{80});
}

// reset() drops the anchor + buffered packets: playout goes silent until the next
// submit re-anchors a fresh epoch. Used when the inter-site tunnel (re)punches so a
// stale timeline can never leave the egress stuck emitting silence.
TEST(audio_egress_reset, reanchors_after_reset)
{
    Egress eg{small_config(/*wcl*/ 0)};
    auto const pkt = make_packet(/*start*/ 20, 4, eg.frame_bytes());
    EXPECT_TRUE(eg.submit(/*src_tai*/ 0, pkt, 4));
    EXPECT_TRUE(eg.anchored());

    eg.reset();
    EXPECT_FALSE(eg.anchored());
    std::vector<uint8_t> out(size_t{4} * 4, 0xAA);
    EXPECT_EQ(eg.playout(/*now*/ 0, out, 4), size_t{0});  // silent until re-anchored
    for (auto b : out) {
        EXPECT_EQ(b, uint8_t{0});
    }

    // A fresh epoch (new src_tai far from the old one) re-anchors and plays.
    auto const pkt2 = make_packet(/*start*/ 30, 4, eg.frame_bytes());
    EXPECT_TRUE(eg.submit(/*src_tai*/ 9'000'000, pkt2, 4));
    EXPECT_TRUE(eg.anchored());
    std::vector<uint8_t> out2(size_t{4} * 4, 0);
    EXPECT_EQ(eg.playout(/*now*/ 9'000'000, out2, 4), size_t{4});
    EXPECT_EQ(out2[0], uint8_t{30});
}

// The caller reads a GPS-paced VARIABLE count each tick (nominal +/- 1) but
// positions reads on a FIXED TAI grid. Anchoring every read at frame_for_tai(now)
// tore -- it skipped a frame when the count was short and repeated one when long.
// The contiguous cursor must play every buffered frame exactly once, in order.
TEST(audio_egress_contiguous, variable_read_len_no_tear)
{
    Egress eg{small_config(/*wcl*/ 0)};  // 1 kHz, 4 frames/packet (= 4 ms grid)
    size_t const fb = eg.frame_bytes();
    // 5 packets of 4 frames; frame F's bytes all == F (its absolute index).
    for (int pk = 0; pk < 5; ++pk) {
        std::vector<uint8_t> p(size_t{4} * fb, 0);
        for (int f = 0; f < 4; ++f) {
            for (size_t b = 0; b < fb; ++b) {
                p[(static_cast<size_t>(f) * fb) + b] = static_cast<uint8_t>((pk * 4) + f);
            }
        }
        eg.submit(/*src_tai*/ static_cast<int64_t>(pk) * 4'000'000, p, 4);
    }
    // Read variable counts (sum 20) on the fixed 4 ms grid; a naive reader would
    // skip/dup a frame at every off-nominal read. Collect each frame's first byte.
    int const counts[] = {4, 5, 3, 4, 4};
    std::vector<uint8_t> got;
    int64_t now = 0;
    for (int k = 0; k < 5; ++k) {
        int const n = counts[k];
        std::vector<uint8_t> out(static_cast<size_t>(n) * fb, 0xEE);
        eg.playout(now, out, static_cast<uint16_t>(n));
        now += 4'000'000;  // fixed 4 ms step regardless of n read
        for (int f = 0; f < n; ++f) {
            got.push_back(out[static_cast<size_t>(f) * fb]);
        }
    }
    EXPECT_EQ(got.size(), size_t{20});
    for (size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i], static_cast<uint8_t>(i));  // 0,1,2,...,19 -- no skip, no repeat
    }
}

TEST_MAIN(statusbar_udptun, udptun_audio_egress_test)
