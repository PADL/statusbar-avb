// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_audio_ingest.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;

namespace {

auto config_96k_8ch() -> udptun::AudioIngest<4096>::Config
{
    udptun::AudioIngest<4096>::Config c{};
    c.channels = 8;
    c.sample_rate_hz = 96'000;
    c.bytes_per_sample = 4;
    c.tunnel_frames_per_packet = 96;  // 1 ms
    return c;
}

auto zero_frames(uint16_t n_frames, size_t frame_bytes) -> std::vector<uint8_t>
{
    return std::vector<uint8_t>(static_cast<size_t>(n_frames) * frame_bytes, 0);
}

}  // namespace

// Before start(), submit is a no-op.
TEST(audio_ingest_gate, no_emit_before_start)
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    auto const frames = zero_frames(12, 32);
    size_t emits = 0;
    auto const n = ing.submit(frames, 12, [&](auto const&) { ++emits; });
    EXPECT_EQ(n, size_t{0});
    EXPECT_EQ(emits, size_t{0});
    EXPECT_FALSE(ing.started());
}

// Feeding 8 AVB packets of 12 frames each (= 96 frames = 1 ms) emits exactly one
// inter-site packet, stamped at the anchor TAI.
TEST(audio_ingest_assemble, eight_avb_packets_make_one_tunnel_packet)
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    int64_t const anchor = 1'780'000'000'000'000'000LL;  // TAI ns
    ing.start(anchor);

    std::vector<int64_t> tais;
    auto const frames = zero_frames(12, 32);
    size_t total = 0;
    for (int i = 0; i < 8; ++i) {
        total += ing.submit(frames, 12, [&](auto const& p) { tais.push_back(p.tai_ns); });
    }
    EXPECT_EQ(total, size_t{1});
    EXPECT_EQ(tais.size(), size_t{1});
    EXPECT_EQ(tais[0], anchor);  // first frame of the 1 ms packet == anchor
}

// Exact TAI advance: each 12-frame batch advances the running TAI by exactly
// 125000 ns; each completed 1 ms packet is stamped 1000000 ns after the prior.
TEST(audio_ingest_tai, exact_125us_per_batch_and_1ms_per_packet)
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    int64_t const anchor = 1'000'000'000LL;
    ing.start(anchor);

    std::vector<int64_t> tais;
    auto const frames = zero_frames(12, 32);
    for (int i = 0; i < 24; ++i) {  // 24 batches * 12 = 288 frames = 3 packets
        ing.submit(frames, 12, [&](auto const& p) { tais.push_back(p.tai_ns); });
    }
    EXPECT_EQ(tais.size(), size_t{3});
    EXPECT_EQ(tais[0], anchor);
    EXPECT_EQ(tais[1], anchor + 1'000'000);  // +1 ms
    EXPECT_EQ(tais[2], anchor + 2'000'000);  // +2 ms
    // 288 frames * 125000/12 ... running TAI advanced by 288 * (1e9/96000) = 3 ms.
    EXPECT_EQ(ing.running_tai_ns(), anchor + 3'000'000);
}

// No long-term rounding drift even with odd (nominal +/-1) batch sizes: feed a
// mix of 11/12/13-frame batches and confirm running TAI == anchor + exact
// duration of the total frame count.
TEST(audio_ingest_tai, no_drift_with_uneven_batches)
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    int64_t const anchor = 500'000'000LL;
    ing.start(anchor);

    auto const f11 = zero_frames(11, 32);
    auto const f12 = zero_frames(12, 32);
    auto const f13 = zero_frames(13, 32);

    int64_t total_frames = 0;
    for (int i = 0; i < 1000; ++i) {
        ing.submit(f11, 11, [](auto const&) {});
        ing.submit(f12, 12, [](auto const&) {});
        ing.submit(f13, 13, [](auto const&) {});
        total_frames += 36;
    }
    // Exact expected TAI advance = total_frames * 1e9 / 96000, integer-exact
    // because the accumulator carries the remainder.
    int64_t const expected = anchor + ((total_frames * 1'000'000'000LL) / 96'000);
    EXPECT_EQ(ing.running_tai_ns(), expected);
}

// Re-start() re-anchors and drops the partial packet.
TEST(audio_ingest_restart, reanchors_and_drops_partial)
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    ing.start(1'000);
    auto const frames = zero_frames(12, 32);
    ing.submit(frames, 12, [](auto const&) {});  // 12/96 buffered

    int64_t const new_anchor = 9'000'000'000LL;
    ing.start(new_anchor);
    EXPECT_EQ(ing.running_tai_ns(), new_anchor);

    // Now a full 96 frames (8 batches) emits one packet at the NEW anchor.
    std::vector<int64_t> tais;
    for (int i = 0; i < 8; ++i) {
        ing.submit(frames, 12, [&](auto const& p) { tais.push_back(p.tai_ns); });
    }
    EXPECT_EQ(tais.size(), size_t{1});
    EXPECT_EQ(tais[0], new_anchor);
}

namespace {
// Simulate one second of a 12-frame-per-batch source whose media clock is `ppm`
// off GPS (so each nominal-125000 ns batch spans a slightly different real GPS-TAI
// span). Returns the final |live_tai - running_tai| error in ns. `use_discipline`
// toggles calling discipline() with the live GPS-TAI before each submit.
auto run_offrate(double ppm, bool use_discipline, int64_t* worst_after_warmup = nullptr) -> int64_t
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    int64_t const anchor = 1'780'000'000'000'000'000LL;
    ing.start(anchor);
    auto const frames = zero_frames(12, 32);
    // batch 0 is "received" at the anchor; running_tai is the PT of the next frame
    // to emit (= this batch's first frame), so it is compared against live-now —
    // exactly how the entity calls discipline(live) before submit().
    double live = static_cast<double>(anchor);
    int64_t worst = 0;
    int64_t last_err = 0;
    for (int i = 0; i < 8000; ++i) {  // ~1 s at 8000 batches/s
        if (use_discipline) {
            ing.discipline(static_cast<int64_t>(live));
        }
        int64_t const err = static_cast<int64_t>(live) - ing.running_tai_ns();
        last_err = err < 0 ? -err : err;
        if (i > 500 && last_err > worst) {
            worst = last_err;  // steady-state, after convergence
        }
        ing.submit(frames, 12, [](auto const&) {});
        live += 125'000.0 * (1.0 + ppm / 1.0e6);  // next batch arrives one period later
    }
    if (worst_after_warmup != nullptr) {
        *worst_after_warmup = worst;
    }
    return last_err;
}
}  // namespace

// Without discipline, a +172 ppm off-GPS source drifts the frame-counted TAI by
// ~172 us over one second (the failure mode: ~500 ms/hour out of the far egress window).
TEST(audio_ingest_discipline, undisciplined_source_drifts)
{
    int64_t const drift_1s = run_offrate(172.0, /*use_discipline=*/false);
    EXPECT_TRUE(drift_1s > 150'000);  // > 150 us in one second
}

// discipline() slews the presentation TAI to the live GPS-TAI, bounding the error
// to a few microseconds regardless of the source's rate offset (either sign).
TEST(audio_ingest_discipline, locks_to_live_tai_under_offrate_source)
{
    int64_t worst_slow = 0;
    int64_t const final_slow = run_offrate(172.0, /*use_discipline=*/true, &worst_slow);
    EXPECT_TRUE(worst_slow < 5'000);  // < 5 us steady-state
    EXPECT_TRUE(final_slow < 5'000);
    int64_t worst_fast = 0;
    (void)run_offrate(-172.0, /*use_discipline=*/true, &worst_fast);
    EXPECT_TRUE(worst_fast < 5'000);
}

// The slew must not make the emitted presentation timestamps go backwards: each
// 1 ms tunnel packet's TAI stays strictly increasing while disciplined.
TEST(audio_ingest_discipline, emitted_timestamps_stay_monotonic)
{
    udptun::AudioIngest<4096> ing{config_96k_8ch()};
    int64_t const anchor = 1'780'000'000'000'000'000LL;
    ing.start(anchor);
    auto const frames = zero_frames(12, 32);
    double live = static_cast<double>(anchor);
    int64_t prev = 0;
    bool monotonic = true;
    size_t packets = 0;
    for (int i = 0; i < 8000; ++i) {
        live += 125'000.0 * (1.0 + 172.0 / 1.0e6);
        ing.discipline(static_cast<int64_t>(live));
        ing.submit(frames, 12, [&](auto const& p) {
            if (packets > 0 && p.tai_ns <= prev) {
                monotonic = false;
            }
            prev = p.tai_ns;
            ++packets;
        });
    }
    EXPECT_TRUE(monotonic);
    EXPECT_TRUE(packets > size_t{900});  // ~1000 tunnel packets in 1 s
}

TEST_MAIN(statusbar_udptun, udptun_audio_ingest_test)
