// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The shaper's pure parts (queue layout, mqprio, IEEE 802.1Q Annex L cbs parameters), with the
// arithmetic worked by hand; EntityShaper needs rtnetlink and is not run here.

#include "statusbar/avb_entity/avb_entity_shaper.hpp"

#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <system_error>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

/// 8 channels of 48 kHz INT_24 AAF, as the bridge's AVB outputs declare it.
auto aaf_8ch_48k_int24() -> StreamSpec
{
    StreamSpec spec{};
    spec.index = 0;
    spec.format.kind = StreamKind::aaf;
    spec.format.sample_rate_hz = 48000;
    spec.format.channels = 8;
    spec.format.bit_depth = 24;
    spec.format.aaf_format = avtp::AafFormat::int_24bit;
    return spec;
}

/// The bridge's CRF talker: 0x041060010000BB80, one timestamp per PDU.
auto crf_one_timestamp() -> StreamSpec
{
    StreamSpec spec{};
    spec.index = 1;
    spec.format = decode_stream_format(0x041060010000BB80ULL);
    return spec;
}

}  // namespace

TEST(shaper_layout, igb_puts_the_sr_classes_on_the_bottom_queues)
{
    auto const layout = make_queue_layout("igb", 4);
    EXPECT_TRUE(layout.has_value());
    EXPECT_TRUE(
        *layout == (QueueLayout{.queue_count = 4, .class_a_queue = 0, .class_b_queue = 1, .legacy_offset = 2, .legacy_count = 2}));
    auto const igc = make_queue_layout("igc", 4);
    EXPECT_TRUE(igc.has_value() && *igc == *layout);
    EXPECT_TRUE(is_intel_qav_driver("igb"));
    EXPECT_FALSE(is_intel_qav_driver("igbvf"));
}

TEST(shaper_layout, other_drivers_put_the_sr_classes_on_the_top_queues)
{
    auto const layout = make_queue_layout("mv88e6085", 8);
    EXPECT_TRUE(layout.has_value());
    EXPECT_TRUE(
        *layout == (QueueLayout{.queue_count = 8, .class_a_queue = 7, .class_b_queue = 6, .legacy_offset = 0, .legacy_count = 6}));
    auto const three = make_queue_layout("dsa", 3);
    EXPECT_TRUE(three.has_value());
    EXPECT_TRUE(
        *three == (QueueLayout{.queue_count = 3, .class_a_queue = 2, .class_b_queue = 1, .legacy_offset = 0, .legacy_count = 1}));
}

TEST(shaper_layout, an_override_wins)
{
    // igb, but the SR classes on top (the "broken i210" case inverted)
    auto const top = make_queue_layout("igb", 4, QueueOverride{.class_a = 3, .class_b = 2});
    EXPECT_TRUE(top.has_value());
    EXPECT_TRUE(
        *top == (QueueLayout{.queue_count = 4, .class_a_queue = 3, .class_b_queue = 2, .legacy_offset = 0, .legacy_count = 2}));
    // a Marvell port, made to look like an i210
    auto const bottom = make_queue_layout("mv88e6085", 8, QueueOverride{.class_a = 0, .class_b = 1});
    EXPECT_TRUE(bottom.has_value());
    EXPECT_TRUE(
        *bottom == (QueueLayout{.queue_count = 8, .class_a_queue = 0, .class_b_queue = 1, .legacy_offset = 2, .legacy_count = 6}));
    // class B alone: A follows the policy (N-1)
    auto const b_only = make_queue_layout("mv88e6085", 8, QueueOverride{.class_b = 0});
    EXPECT_TRUE(b_only.has_value());
    EXPECT_TRUE(
        *b_only == (QueueLayout{.queue_count = 8, .class_a_queue = 7, .class_b_queue = 0, .legacy_offset = 1, .legacy_count = 6}));
}

TEST(shaper_layout, refuses_what_cannot_work)
{
    auto const two = make_queue_layout("igb", 2);
    EXPECT_FALSE(two.has_value());
    EXPECT_TRUE(two.error() == make_error_code(ShaperError::not_avb_capable));
    EXPECT_FALSE(make_queue_layout("igb", 1).has_value());  // ethtool -L combined 1
    EXPECT_FALSE(make_queue_layout("igb", 0).has_value());
    auto const bad = [](QueueOverride const& o) {
        auto const layout = make_queue_layout("igb", 4, o);
        return !layout.has_value() && layout.error() == make_error_code(ShaperError::bad_queue_override);
    };
    EXPECT_TRUE(bad({.class_a = 4}));                 // out of range
    EXPECT_TRUE(bad({.class_a = 1}));                 // B's default is 1 too
    EXPECT_TRUE(bad({.class_a = 2, .class_b = 2}));   // shared
    EXPECT_TRUE(bad({.class_a = 1, .class_b = 2}));   // legacy {0, 3}
    EXPECT_TRUE(bad({.class_a = 3}));                 // legacy {0, 2}
    EXPECT_FALSE(bad({.class_a = 0, .class_b = 3}));  // legacy {1, 2}
}

#if defined(__linux__)
TEST(shaper_mqprio, igb_four_queues)
{
    auto const layout = make_queue_layout("igb", 4);
    EXPECT_TRUE(layout.has_value());
    auto const made = mqprio_for(*layout, 0x9000);
    EXPECT_TRUE(made.has_value());
    auto const& q = *made;
    EXPECT_EQ(q.major, uint16_t{0x9000});
    EXPECT_EQ(q.num_tc, uint8_t{3});
    // PCP 3 (class A) -> TC2, PCP 2 (class B) -> TC1, everything else TC0
    std::array<uint8_t, 16> const map{0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(q.prio_tc_map == map);
    // TC0 2@2, TC1 1@1, TC2 1@0: `queues 2@2 1@1 1@0`
    EXPECT_EQ(q.count[0], uint16_t{2});
    EXPECT_EQ(q.offset[0], uint16_t{2});
    EXPECT_EQ(q.count[1], uint16_t{1});
    EXPECT_EQ(q.offset[1], uint16_t{1});
    EXPECT_EQ(q.count[2], uint16_t{1});
    EXPECT_EQ(q.offset[2], uint16_t{0});
    EXPECT_EQ(q.count[3], uint16_t{0});
    EXPECT_FALSE(q.hw);
}

TEST(shaper_mqprio, marvell_eight_queues)
{
    auto const layout = make_queue_layout("mv88e6085", 8);
    EXPECT_TRUE(layout.has_value());
    auto const made = mqprio_for(*layout, 0x9000);
    EXPECT_TRUE(made.has_value());
    auto const& q = *made;
    // `queues 6@0 1@6 1@7`
    EXPECT_EQ(q.count[0], uint16_t{6});
    EXPECT_EQ(q.offset[0], uint16_t{0});
    EXPECT_EQ(q.count[1], uint16_t{1});
    EXPECT_EQ(q.offset[1], uint16_t{6});
    EXPECT_EQ(q.count[2], uint16_t{1});
    EXPECT_EQ(q.offset[2], uint16_t{7});
    EXPECT_EQ(q.prio_tc_map[3], CLASS_A_TC);
    EXPECT_EQ(q.prio_tc_map[2], CLASS_B_TC);
    EXPECT_EQ(q.prio_tc_map[0], LEGACY_TC);
    EXPECT_EQ(q.prio_tc_map[7], LEGACY_TC);
}
#endif

TEST(shaper_mqprio, refuses_bad_priorities)
{
    auto const layout = make_queue_layout("igb", 4);
    EXPECT_TRUE(layout.has_value());
    auto const bad = [&](uint8_t a, uint8_t b) {
        auto const q = mqprio_for(*layout, 0x9000, a, b);
        return !q.has_value() && q.error() == make_error_code(ShaperError::bad_priority);
    };
    EXPECT_TRUE(bad(3, 3));  // one PCP for both classes
    EXPECT_TRUE(bad(8, 2));
    EXPECT_TRUE(bad(3, 8));
    EXPECT_FALSE(bad(5, 4));
    auto const custom = mqprio_for(*layout, 0x9000, 5, 4);
    EXPECT_TRUE(custom.has_value() && custom->prio_tc_map[5] == CLASS_A_TC && custom->prio_tc_map[4] == CLASS_B_TC);
    EXPECT_TRUE(custom.has_value() && custom->prio_tc_map[3] == LEGACY_TC);
    EXPECT_TRUE(validate_priorities(3, 2).has_value());
}

TEST(shaper_cbs, frame_sizes_and_bandwidth)
{
    // The TSpecs the kit advertises (srp_max_frame_size):
    //   AAF 8 ch INT_24 48 kHz: 24-byte AAF header + (6 + 1) samples x 8 ch x 3 B = 24 + 168 = 192
    //   CRF, one timestamp a PDU: 20-byte CRF header + 1 x 8 = 28
    auto const aaf = AvbEntityKit::talker_tspec(aaf_8ch_48k_int24());
    auto const crf = AvbEntityKit::talker_tspec(crf_one_timestamp());
    EXPECT_EQ(aaf.max_frame_size, uint16_t{192});
    EXPECT_EQ(aaf.max_interval_frames, uint16_t{1});
    EXPECT_EQ(crf.max_frame_size, uint16_t{28});
    // On the wire: max(MFS + 4 VLAN + 18 L2, 68) + 20 L1 + 1 drift
    //   AAF: 192 + 22 = 214 (>= 68), + 21 = 235
    //   CRF: 28 + 22 = 50 -> 68, + 21 = 89
    EXPECT_EQ(wire_frame_size(192), uint32_t{235});
    EXPECT_EQ(wire_frame_size(28), uint32_t{89});
    // Class A: 8000 frames/s. AAF 235 x 8 x 8000 / 1000 = 15040 kbit/s;
    // CRF 89 x 8 x 8000 / 1000 = 5696 kbit/s.
    EXPECT_EQ(stream_bandwidth_kbps(aaf, CLASS_A_INTERVALS_PER_SEC), uint32_t{15040});
    EXPECT_EQ(stream_bandwidth_kbps(crf, CLASS_A_INTERVALS_PER_SEC), uint32_t{5696});
    // Class B: 4000 frames/s: 235 x 8 x 4000 / 1000 = 7520 kbit/s
    EXPECT_EQ(stream_bandwidth_kbps(aaf, CLASS_B_INTERVALS_PER_SEC), uint32_t{7520});
    // MFS 100 -> 143 octets; 3 frames per 250 us: 143 x 8 x 3 x 4000 / 1000 = 13728.
    EXPECT_EQ(
        stream_bandwidth_kbps(TSpec{.max_frame_size = 100, .max_interval_frames = 3}, CLASS_B_INTERVALS_PER_SEC), uint32_t{13728});
    // Rounds up: one 143-octet frame a second is 143 x 8 / 1000 = 1.144 -> 2 kbit/s
    EXPECT_EQ(stream_bandwidth_kbps(TSpec{.max_frame_size = 100, .max_interval_frames = 1}, 1), uint32_t{2});
}

TEST(shaper_cbs, class_a_aaf_and_crf_on_a_gigabit_link)
{
    // One 8-channel 48 kHz INT_24 AAF stream and the CRF at 1 Gbit/s, MTU 1500: idleSlope 20736,
    // sendSlope -979264, hiCredit ceil(20736 x 1500 / 1000000) = 32, loCredit
    // ceil(-979264 x 235 / 1000000) = -230 (maxFrameSizeA: the larger wire frame, 235).
    std::array<TSpec, 2> const streams{
        AvbEntityKit::talker_tspec(aaf_8ch_48k_int24()), AvbEntityKit::talker_tspec(crf_one_timestamp())};
    auto const a = class_a_cbs(streams, 1'000'000, 1500);
    EXPECT_EQ(a.idleslope_kbps, int32_t{20736});
    EXPECT_EQ(a.sendslope_kbps, int32_t{-979264});
    EXPECT_EQ(a.hicredit, int32_t{32});
    EXPECT_EQ(a.locredit, int32_t{-230});
    EXPECT_EQ(a.max_frame_size, uint32_t{235});
    EXPECT_FALSE(a.empty());

    // The AAF alone at 100 Mbit/s: idleSlope 15040, sendSlope -84960,
    // hiCredit ceil(15040 x 1500 / 100000) = 226, loCredit ceil(-84960 x 235 / 100000) = -199.
    auto const slow = class_a_cbs(std::span{streams}.first(1), 100'000, 1500);
    EXPECT_EQ(slow.idleslope_kbps, int32_t{15040});
    EXPECT_EQ(slow.sendslope_kbps, int32_t{-84960});
    EXPECT_EQ(slow.hicredit, int32_t{226});
    EXPECT_EQ(slow.locredit, int32_t{-199});

    // Nothing admitted: nothing to shape
    EXPECT_TRUE(class_a_cbs({}, 1'000'000, 1500).empty());
    EXPECT_TRUE(class_a_cbs({}, 1'000'000, 1500) == CbsParameters{});
}

TEST(shaper_cbs, class_b_sees_class_a_as_interference)
{
    std::array<TSpec, 2> const a_streams{
        AvbEntityKit::talker_tspec(aaf_8ch_48k_int24()), AvbEntityKit::talker_tspec(crf_one_timestamp())};
    auto const a = class_a_cbs(a_streams, 1'000'000, 1500);
    // One AAF stream in class B (7520 kbit/s), class A as above: sendSlopeB -992480,
    // hiCreditB ceil(7520 x (1500 / (1000000 - 20736) + 235 / 1000000)) = 14 (L-11),
    // loCreditB ceil(-992480 x 235 / 1000000) = -233.
    std::array<TSpec, 1> const b_streams{AvbEntityKit::talker_tspec(aaf_8ch_48k_int24())};
    auto const b = class_b_cbs(b_streams, a, 1'000'000, 1500);
    EXPECT_EQ(b.idleslope_kbps, int32_t{7520});
    EXPECT_EQ(b.sendslope_kbps, int32_t{-992480});
    EXPECT_EQ(b.hicredit, int32_t{14});
    EXPECT_EQ(b.locredit, int32_t{-233});
    // With no class A: hiCreditB = ceil(7520 x 1500 / 1000000) = ceil(11.28) = 12
    auto const b_alone = class_b_cbs(b_streams, CbsParameters{}, 1'000'000, 1500);
    EXPECT_EQ(b_alone.hicredit, int32_t{12});
    EXPECT_TRUE(class_b_cbs({}, a, 1'000'000, 1500).empty());
}

TEST(shaper_errors, messages)
{
    EXPECT_TRUE(make_error_code(ShaperError::not_avb_capable).message().find("3 TX queues") != std::string::npos);
    EXPECT_EQ(std::string{shaper_error_category().name()}, std::string{"statusbar.avb_entity.shaper"});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_shaper_test)
