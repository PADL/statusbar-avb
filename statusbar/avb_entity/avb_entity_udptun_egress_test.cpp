// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the tunnel-egress PCM conversion helper
// (avb_entity_udptun_egress.hpp): de-interleave 32-bit big-endian (network byte
// order) PCM into normalized float. This is the buffer-agnostic seam the egress
// move (god-object phase 2) will write through instead of touching audio_buffer_.

#include "statusbar/avb_entity/avb_entity_udptun_egress.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using statusbar::avb_entity::udptun_egress_deinterleave_to_float;

namespace {
auto approx(float a, float b, float eps = 1e-6F) -> bool
{
    return std::fabs(a - b) <= eps;
}
// big-endian int32 bytes for a value
auto be(int32_t v) -> std::array<uint8_t, 4>
{
    std::array<uint8_t, 4> out{};
    statusbar::span_store(out, statusbar::ieee::quadlet_t{static_cast<uint32_t>(v)});
    return out;
}
}  // namespace

TEST(udptun_egress, decodes_endpoints_and_zero)
{
    // 1 channel, 3 samples: 0, +full-scale-1, -full-scale.
    std::vector<uint8_t> pcm;
    for (int32_t v : {0, 0x7FFFFFFF, static_cast<int32_t>(0x80000000)}) {
        auto b = be(v);
        pcm.insert(pcm.end(), b.begin(), b.end());
    }
    std::array<float, 3> out{};
    udptun_egress_deinterleave_to_float(pcm, out, /*channels=*/1, /*samples=*/3);
    EXPECT_TRUE(approx(out[0], 0.0F));
    EXPECT_TRUE(approx(out[1], 0.9999999F, 1e-6F));  // (2^31-1)/2^31
    EXPECT_TRUE(approx(out[2], -1.0F));              // -2^31 / 2^31
}

TEST(udptun_egress, preserves_interleave_across_channels)
{
    // 2 channels, 2 samples; distinct value per (sample,channel).
    std::vector<int32_t> vals{100, -200, 300, -400};  // s0c0 s0c1 s1c0 s1c1
    std::vector<uint8_t> pcm;
    for (int32_t v : vals) {
        auto b = be(v);
        pcm.insert(pcm.end(), b.begin(), b.end());
    }
    std::array<float, 4> out{};
    udptun_egress_deinterleave_to_float(pcm, out, /*channels=*/2, /*samples=*/2);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(approx(out[i], static_cast<float>(vals[i]) / 2147483648.0F));
    }
}

TEST(udptun_egress, short_input_writes_nothing)
{
    // out demands 4 samples but pcm only holds 1 -> no write, no OOB.
    std::array<uint8_t, 4> pcm{0xFF, 0xFF, 0xFF, 0xFF};
    std::array<float, 4> out{};
    out.fill(0.5F);
    udptun_egress_deinterleave_to_float(pcm, out, /*channels=*/1, /*samples=*/4);
    for (float f : out) {
        EXPECT_TRUE(approx(f, 0.5F));  // untouched
    }
}

TEST(udptun_egress, short_output_writes_nothing)
{
    std::array<uint8_t, 16> pcm{};  // 4 samples worth
    std::array<float, 2> out{};     // too small for 4
    out.fill(0.25F);
    udptun_egress_deinterleave_to_float(pcm, out, /*channels=*/1, /*samples=*/4);
    EXPECT_TRUE(approx(out[0], 0.25F));
    EXPECT_TRUE(approx(out[1], 0.25F));
}

// Test runner

TEST_MAIN(statusbar_avb_entity, avb_entity_udptun_egress_test)
