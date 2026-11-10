// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The shaper's pure parts (queue layout, mqprio, IEEE 802.1Q Annex L cbs parameters, placeholders)
// and ShaperQdiscs and PortShaper against a recording TcControl; EntityShaper needs rtnetlink and is
// not run here.
// The arithmetic is worked in docs/SHAPER.md ("Worked examples").

#include "statusbar/avb_entity/avb_entity_shaper.hpp"

#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

/// @p spec's TSpec with the clock-drift byte in MaxFrameSize, as Milan v1.2 Table 4.4 had it: the
/// CBS arithmetic below and docs/SHAPER.md's worked examples use these.
auto worked_tspec(StreamSpec const& spec) -> TSpec
{
    return TSpec{.max_frame_size = static_cast<uint16_t>(srp_max_frame_size(spec.format) + 1), .max_interval_frames = 1};
}

}  // namespace

// i210 7.2.7.4: queue 0 has the highest priority, queue 3 the lowest; PTP goes on queue 2, just below
// the SR classes, and legacy keeps queue 3.
TEST(shaper_layout, igb_puts_the_sr_classes_on_the_bottom_queues)
{
    auto const layout = make_queue_layout("igb", 4);
    EXPECT_TRUE(layout.has_value());
    EXPECT_TRUE(
        *layout ==
        (QueueLayout{
            .queue_count = 4, .class_a_queue = 0, .class_b_queue = 1, .legacy_offset = 3, .legacy_count = 1, .ptp_queue = 2}));
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
        *layout ==
        (QueueLayout{
            .queue_count = 8, .class_a_queue = 7, .class_b_queue = 6, .legacy_offset = 0, .legacy_count = 5, .ptp_queue = 5}));
    // one legacy queue: PTP stays legacy traffic
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
        *top ==
        (QueueLayout{
            .queue_count = 4, .class_a_queue = 3, .class_b_queue = 2, .legacy_offset = 0, .legacy_count = 1, .ptp_queue = 1}));
    // a Marvell port, made to look like an i210
    auto const bottom = make_queue_layout("mv88e6085", 8, QueueOverride{.class_a = 0, .class_b = 1});
    EXPECT_TRUE(bottom.has_value());
    EXPECT_TRUE(
        *bottom ==
        (QueueLayout{
            .queue_count = 8, .class_a_queue = 0, .class_b_queue = 1, .legacy_offset = 3, .legacy_count = 5, .ptp_queue = 2}));
    // class B alone: A follows the policy (N-1)
    auto const b_only = make_queue_layout("mv88e6085", 8, QueueOverride{.class_b = 0});
    EXPECT_TRUE(b_only.has_value());
    EXPECT_TRUE(
        *b_only ==
        (QueueLayout{
            .queue_count = 8, .class_a_queue = 7, .class_b_queue = 0, .legacy_offset = 2, .legacy_count = 5, .ptp_queue = 1}));
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
    EXPECT_TRUE(bad({.class_a = 1, .class_b = 0}));   // igb: class A under class B
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
    EXPECT_EQ(q.num_tc, uint8_t{4});
    // PCP 3 (class A) -> TC2, PCP 2 (class B) -> TC1, PTP 7 -> TC3, everything else TC0
    std::array<uint8_t, 16> const map{0, 0, 1, 2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(q.prio_tc_map == map);
    // `queues 1@3 1@1 1@0 1@2`
    std::array<uint16_t, 16> const count{1, 1, 1, 1};
    std::array<uint16_t, 16> const offset{3, 1, 0, 2};
    EXPECT_TRUE(q.count == count);
    EXPECT_TRUE(q.offset == offset);
    EXPECT_FALSE(q.offload);
}

// The request on the wire, byte for byte: what core's net_tc_qdisc_test checks for the same command,
// `tc qdisc replace dev <if3> root handle 9000: mqprio num_tc 4 map 0 0 1 2 0 0 0 3 0 0 0 0 0 0 0 0
// queues 1@3 1@1 1@0 1@2 hw 0`. Every PortShaper lays it, with cbs, etf or both under it.
TEST(shaper_mqprio, igb_four_queues_request_bytes)
{
    if constexpr (std::endian::native != std::endian::little) {
        return;
    }
    auto const made = mqprio_for(*make_queue_layout("igb", 4), 0x9000);
    net::tc::RequestBuffer out{};
    auto const length = made.and_then([&](net::tc::MqprioQdisc const& q) { return net::tc::build_replace_mqprio(out, 3, q, 7); });
    EXPECT_TRUE(length.has_value() && *length == 136);
    // tcmsg handle 9000:0 at 24, num_tc at 52, prio_tc_map at 53, hw at 69, count at 70, offset at 102
    EXPECT_TRUE(out[26] == 0x00 && out[27] == 0x90 && out[52] == 4 && out[69] == 0);
    std::array<uint8_t, 16> const map{0, 0, 1, 2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(std::ranges::equal(std::span{out}.subspan(53, 16), map));
    std::array<uint8_t, 8> const counts{1, 0, 1, 0, 1, 0, 1, 0};
    std::array<uint8_t, 8> const offsets{3, 0, 1, 0, 0, 0, 2, 0};
    EXPECT_TRUE(std::ranges::equal(std::span{out}.subspan(70, 8), counts));
    EXPECT_TRUE(std::ranges::equal(std::span{out}.subspan(102, 8), offsets));
}

TEST(shaper_mqprio, marvell_eight_queues)
{
    auto const layout = make_queue_layout("mv88e6085", 8);
    EXPECT_TRUE(layout.has_value());
    auto const made = mqprio_for(*layout, 0x9000);
    EXPECT_TRUE(made.has_value());
    auto const& q = *made;
    // `queues 5@0 1@6 1@7 1@5`
    EXPECT_EQ(q.num_tc, uint8_t{4});
    EXPECT_EQ(q.count[3], uint16_t{1});
    EXPECT_EQ(q.offset[3], uint16_t{5});
    EXPECT_EQ(q.prio_tc_map[7], PTP_TC);
    EXPECT_EQ(q.count[0], uint16_t{5});
    EXPECT_EQ(q.offset[0], uint16_t{0});
    EXPECT_EQ(q.count[1], uint16_t{1});
    EXPECT_EQ(q.offset[1], uint16_t{6});
    EXPECT_EQ(q.count[2], uint16_t{1});
    EXPECT_EQ(q.offset[2], uint16_t{7});
    EXPECT_EQ(q.prio_tc_map[3], CLASS_A_TC);
    EXPECT_EQ(q.prio_tc_map[2], CLASS_B_TC);
    EXPECT_EQ(q.prio_tc_map[0], LEGACY_TC);
    EXPECT_EQ(q.prio_tc_map[6], LEGACY_TC);
    // three queues: no PTP queue, num_tc 3, priority 7 legacy traffic
    auto const three = mqprio_for(*make_queue_layout("dsa", 3), 0x9000);
    EXPECT_TRUE(three && three->num_tc == 3 && three->prio_tc_map[7] == LEGACY_TC && three->count[3] == 0);
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
    EXPECT_TRUE(bad(7, 2));  // class A on PTP's priority
    EXPECT_FALSE(bad(5, 4));
    EXPECT_FALSE(validate_priorities(3, 2, 16).has_value());
    EXPECT_FALSE(validate_priorities(3, 2, 2).has_value());
    EXPECT_TRUE(validate_priorities(3, 2, 12).has_value());
    // no PTP queue (3 TX queues): the PTP priority is not used, so not checked
    EXPECT_TRUE(mqprio_for(*make_queue_layout("dsa", 3), 0x9000, 3, 2, 3).has_value());
    EXPECT_FALSE(mqprio_for(*layout, 0x9000, 3, 2, 3).has_value());
    auto const custom = mqprio_for(*layout, 0x9000, 5, 4);
    EXPECT_TRUE(custom.has_value() && custom->prio_tc_map[5] == CLASS_A_TC && custom->prio_tc_map[4] == CLASS_B_TC);
    EXPECT_TRUE(custom.has_value() && custom->prio_tc_map[3] == LEGACY_TC);
    EXPECT_TRUE(validate_priorities(3, 2).has_value());
}

TEST(shaper_cbs, frame_sizes_and_bandwidth)
{
    // The TSpecs the kit advertises (srp_max_frame_size, Milan v1.3 Table 4.4):
    //   AAF 8 ch INT_24 48 kHz: 24-byte AAF header + 6 samples x 8 ch x 3 B = 18*8 + 24 = 168
    //   CRF, one timestamp a PDU: 20-byte CRF header + 1 x 8 = 28
    auto const aaf = AvbEntityKit::talker_tspec(aaf_8ch_48k_int24());
    auto const crf = AvbEntityKit::talker_tspec(crf_one_timestamp());
    EXPECT_EQ(aaf.max_frame_size, uint16_t{168});
    EXPECT_EQ(aaf.max_interval_frames, uint16_t{1});
    EXPECT_EQ(crf.max_frame_size, uint16_t{28});
    EXPECT_EQ(aaf.clock_drift_bytes, MILAN_TSPEC_SPARE_BYTES);
    // On the wire (Table 4.4 notes): max(MFS + 22, 68) + 1 + 20
    //   AAF: 168 + 22 = 190 (>= 68), + 1 + 20 = 211
    //   CRF: 28 + 22 = 50 -> 68, + 1 + 20 = 89
    EXPECT_EQ(wire_frame_size(168, 1), uint32_t{211});
    EXPECT_EQ(wire_frame_size(28, 1), uint32_t{89});
    // Class A: 8000 frames/s. AAF 211 x 8 x 8000 / 1000 = 13504 kbit/s
    // (Table 4.4: 1152*8 + 4288); CRF 89 x 8 x 8000 / 1000 = 5696 (Table 4.4).
    EXPECT_EQ(stream_bandwidth_kbps(aaf, CLASS_A_INTERVALS_PER_SEC), uint32_t{13504});
    EXPECT_EQ(stream_bandwidth_kbps(crf, CLASS_A_INTERVALS_PER_SEC), uint32_t{5696});
    // Class B: 4000 frames/s: 211 x 8 x 4000 / 1000 = 6752 kbit/s
    EXPECT_EQ(stream_bandwidth_kbps(aaf, CLASS_B_INTERVALS_PER_SEC), uint32_t{6752});
    // MFS 100 -> 142 octets; 3 frames per 250 us: 142 x 8 x 3 x 4000 / 1000 = 13632.
    EXPECT_EQ(
        stream_bandwidth_kbps(TSpec{.max_frame_size = 100, .max_interval_frames = 3}, CLASS_B_INTERVALS_PER_SEC), uint32_t{13632});
    // Rounds up: one 142-octet frame a second is 142 x 8 / 1000 = 1.136 -> 2 kbit/s
    EXPECT_EQ(stream_bandwidth_kbps(TSpec{.max_frame_size = 100, .max_interval_frames = 1}, 1), uint32_t{2});
}

// Milan v1.3 Section 4.3.3.2, Table 4.4, every row: MaxFrameSize and the
// class A bandwidth the CBS is configured with, for 1 to 32 channels.
TEST(shaper_cbs, milan_table_4_4)
{
    struct Row
    {
        avtp::AafFormat format;
        uint32_t rate;
        uint32_t bytes_per_channel;  // per PDU: samples x sample size
        uint32_t kbps_per_channel;
    };
    std::array<Row, 6> const rows{{
        {avtp::AafFormat::int_32bit, 48000, 24, 1536},
        {avtp::AafFormat::int_32bit, 96000, 48, 3072},
        {avtp::AafFormat::int_32bit, 192000, 96, 6144},
        {avtp::AafFormat::int_24bit, 48000, 18, 1152},
        {avtp::AafFormat::int_24bit, 96000, 36, 2304},
        {avtp::AafFormat::int_24bit, 192000, 72, 4608},
    }};
    for (auto const& row : rows) {
        for (uint16_t n = 1; n <= 32; ++n) {
            StreamSpec spec = aaf_8ch_48k_int24();
            spec.format.aaf_format = row.format;
            spec.format.sample_rate_hz = row.rate;
            spec.format.channels = n;
            uint32_t const mfs = (row.bytes_per_channel * n) + 24;
            if (mfs > 1500) {
                continue;  // past the MTU: not a stream the model allows
            }
            auto const tspec = AvbEntityKit::talker_tspec(spec);
            EXPECT_EQ(uint32_t{tspec.max_frame_size}, mfs);
            EXPECT_EQ(tspec.max_interval_frames, uint16_t{1});
            // The column's linear formula holds while the frame is past the
            // 68-octet minimum; below it the notes' rule 2 pads the frame.
            uint32_t const expected = (mfs + 22 < 68) ? 5696 : (row.kbps_per_channel * n) + 4288;
            EXPECT_EQ(stream_bandwidth_kbps(tspec, CLASS_A_INTERVALS_PER_SEC), expected);
        }
    }
    auto const crf = AvbEntityKit::talker_tspec(crf_one_timestamp());
    EXPECT_EQ(crf.max_frame_size, uint16_t{28});
    EXPECT_EQ(stream_bandwidth_kbps(crf, CLASS_A_INTERVALS_PER_SEC), uint32_t{5696});
}

TEST(shaper_cbs, class_a_aaf_and_crf_on_a_gigabit_link)
{
    // One 8-channel 48 kHz INT_24 AAF stream and the CRF, 1 Gbit/s, MTU 1500 interfering:
    // idleSlope 19136, sendSlope -980864, hiCredit 30, loCredit -206 (docs/SHAPER.md).
    std::array<TSpec, 2> const streams{worked_tspec(aaf_8ch_48k_int24()), worked_tspec(crf_one_timestamp())};
    auto const a = class_a_cbs(streams, 1'000'000, 1500);
    EXPECT_EQ(a.idleslope_kbps, int32_t{19136});
    EXPECT_EQ(a.sendslope_kbps, int32_t{-980864});
    EXPECT_EQ(a.hicredit, int32_t{30});
    EXPECT_EQ(a.locredit, int32_t{-206});
    EXPECT_EQ(a.max_frame_size, uint32_t{211});
    EXPECT_FALSE(a.empty());

    // The AAF alone at 100 Mbit/s: idleSlope 13504, sendSlope -86496, hiCredit 209, loCredit -182.
    auto const slow = class_a_cbs(std::span{streams}.first(1), 100'000, 1500);
    EXPECT_EQ(slow.idleslope_kbps, int32_t{13504});
    EXPECT_EQ(slow.sendslope_kbps, int32_t{-86496});
    EXPECT_EQ(slow.hicredit, int32_t{209});
    EXPECT_EQ(slow.locredit, int32_t{-182});

    // Nothing admitted: nothing to shape
    EXPECT_TRUE(class_a_cbs({}, 1'000'000, 1500).empty());
    EXPECT_TRUE(class_a_cbs({}, 1'000'000, 1500) == CbsParameters{});
}

TEST(shaper_cbs, class_b_sees_class_a_as_interference)
{
    std::array<TSpec, 2> const a_streams{worked_tspec(aaf_8ch_48k_int24()), worked_tspec(crf_one_timestamp())};
    auto const a = class_a_cbs(a_streams, 1'000'000, 1500);
    // One AAF stream in class B (6752 kbit/s), class A as above: sendSlopeB -993248,
    // hiCreditB 13 (L-11), loCreditB -209 (docs/SHAPER.md).
    std::array<TSpec, 1> const b_streams{worked_tspec(aaf_8ch_48k_int24())};
    auto const b = class_b_cbs(b_streams, a, 1'000'000, 1500);
    EXPECT_EQ(b.idleslope_kbps, int32_t{6752});
    EXPECT_EQ(b.sendslope_kbps, int32_t{-993248});
    EXPECT_EQ(b.hicredit, int32_t{13});
    EXPECT_EQ(b.locredit, int32_t{-209});
    // With no class A: hiCreditB = ceil(6752 x 1542 / 1000000) = ceil(10.412) = 11
    auto const b_alone = class_b_cbs(b_streams, CbsParameters{}, 1'000'000, 1500);
    EXPECT_EQ(b_alone.hicredit, int32_t{11});
    EXPECT_TRUE(class_b_cbs({}, a, 1'000'000, 1500).empty());
}

// The kit's bound (MAX_ENTITY_AUDIO_STREAMS 32-channel 48 kHz INT_24 AAF talkers and
// MAX_ENTITY_CRF_STREAMS CRF talkers, gigabit) fits within 802.1Q's default 75% SR budget; the same at INT_32 does not.
// Arithmetic in docs/SHAPER.md ("Worked examples").
TEST(shaper_cbs, class_a_at_the_kits_stream_bound)
{
    auto aaf = aaf_8ch_48k_int24();
    aaf.format.channels = 32;
    EXPECT_EQ(worked_tspec(aaf).max_frame_size, uint16_t{601});
    EXPECT_EQ(stream_bandwidth_kbps(worked_tspec(aaf), CLASS_A_INTERVALS_PER_SEC), uint32_t{41152});
    std::vector<TSpec> streams(MAX_ENTITY_AUDIO_STREAMS, worked_tspec(aaf));
    streams.insert(streams.end(), MAX_ENTITY_CRF_STREAMS, worked_tspec(crf_one_timestamp()));
    EXPECT_EQ(streams.size(), MAX_ENTITY_STREAMS);
    auto const a = class_a_cbs(streams, 1'000'000, 1500);
    EXPECT_EQ(a.idleslope_kbps, int32_t{680960});
    EXPECT_EQ(a.sendslope_kbps, int32_t{-319040});
    EXPECT_EQ(a.hicredit, int32_t{1051});
    EXPECT_EQ(a.locredit, int32_t{-205});
    EXPECT_EQ(a.max_frame_size, uint32_t{643});
    EXPECT_TRUE(a.idleslope_kbps < 750'000);

    aaf.format.aaf_format = avtp::AafFormat::int_32bit;
    std::vector<TSpec> wide(MAX_ENTITY_AUDIO_STREAMS, worked_tspec(aaf));
    wide.insert(wide.end(), MAX_ENTITY_CRF_STREAMS, worked_tspec(crf_one_timestamp()));
    EXPECT_EQ(class_a_cbs(wide, 1'000'000, 1500).idleslope_kbps, int32_t{877568});
}

// The placeholder at half the 75% SR budget, 1 Gbit/s, MTU 1500 (wire frame 1542): idleSlope
// 375000, sendSlope -625000, hiCredit 579 (L-10), loCredit -963 (L-12).
TEST(shaper_cbs, placeholders)
{
    auto const idle = placeholder_cbs(1'000'000, 1500, 375'000);
    EXPECT_EQ(idle.idleslope_kbps, int32_t{375'000});
    EXPECT_EQ(idle.sendslope_kbps, int32_t{-625'000});
    EXPECT_EQ(idle.hicredit, int32_t{579});
    EXPECT_EQ(idle.locredit, int32_t{-963});
    EXPECT_EQ(idle.max_frame_size, uint32_t{1542});
    EXPECT_TRUE(idle.placeholder && !idle.empty());
    // Never under 1% of the link.
    EXPECT_EQ(placeholder_cbs(1'000'000, 1500, 85'936).idleslope_kbps, int32_t{85'936});
    EXPECT_EQ(placeholder_cbs(1'000'000, 1500, -110'672).idleslope_kbps, int32_t{10'000});
    EXPECT_TRUE(placeholder_cbs(0, 1500, 0).empty());

    // cbs_targets: nothing admitted, both classes hold half the budget, so
    // together they claim 75% of the link and legacy keeps the rest.
    auto const none = cbs_targets({}, {}, 1'000'000, 1500);
    EXPECT_TRUE(none.class_a == idle && none.class_b == idle);
    EXPECT_EQ(none.class_a.idleslope_kbps + none.class_b.idleslope_kbps, int32_t{750'000});
    // The same at 100 Mbit/s: 37500 each, hiCredit 579, loCredit -963.
    auto const slow = cbs_targets({}, {}, 100'000, 1500);
    EXPECT_EQ(slow.class_a.idleslope_kbps, int32_t{37'500});
    EXPECT_EQ(slow.class_a.hicredit, int32_t{579});
    EXPECT_EQ(slow.class_a.locredit, int32_t{-963});
    // The AAF and the CRF admitted (19136 kbit/s): class B's placeholder receives the rest of the
    // budget: idleSlope 730864, sendSlope -269136, hiCredit 1127, loCredit -415.
    std::array<TSpec, 2> const streams{worked_tspec(aaf_8ch_48k_int24()), worked_tspec(crf_one_timestamp())};
    auto const some = cbs_targets(streams, {}, 1'000'000, 1500);
    EXPECT_TRUE(some.class_a == class_a_cbs(streams, 1'000'000, 1500));
    EXPECT_FALSE(some.class_a.placeholder);
    EXPECT_TRUE(some.class_b.placeholder);
    EXPECT_EQ(some.class_b.idleslope_kbps, int32_t{730'864});
    EXPECT_EQ(some.class_b.sendslope_kbps, int32_t{-269'136});
    EXPECT_EQ(some.class_b.hicredit, int32_t{1127});
    EXPECT_EQ(some.class_b.locredit, int32_t{-415});
    EXPECT_EQ(some.class_a.idleslope_kbps + some.class_b.idleslope_kbps, int32_t{750'000});
    // At the kit's INT_32 bound class A alone exceeds the budget: class B's
    // placeholder falls to its 1% floor.
    auto aaf = aaf_8ch_48k_int24();
    aaf.format.channels = 32;
    aaf.format.aaf_format = avtp::AafFormat::int_32bit;
    std::vector<TSpec> wide(MAX_ENTITY_AUDIO_STREAMS, worked_tspec(aaf));
    EXPECT_EQ(cbs_targets(wide, {}, 1'000'000, 1500).class_b.idleslope_kbps, int32_t{10'000});
}

// One admitted 8-channel 48 kHz INT_32 AAF talker (the CRF gated, no listener), 1 Gbit/s,
// MTU 1500: the cbs an i210 was programmed with (docs/SHAPER.md, "Verifying on a target").
TEST(shaper_cbs, one_int32_talker_as_captured_on_an_i210)
{
    auto aaf = aaf_8ch_48k_int24();
    aaf.format.aaf_format = avtp::AafFormat::int_32bit;
    std::array<TSpec, 1> const streams{worked_tspec(aaf)};
    EXPECT_EQ(streams[0].max_frame_size, uint16_t{217});
    auto const targets = cbs_targets(streams, {}, 1'000'000, 1500);
    EXPECT_EQ(targets.class_a.idleslope_kbps, int32_t{16576});
    EXPECT_EQ(targets.class_a.sendslope_kbps, int32_t{-983424});
    EXPECT_EQ(targets.class_a.hicredit, int32_t{26});
    EXPECT_EQ(targets.class_a.locredit, int32_t{-254});
    EXPECT_EQ(targets.class_a.max_frame_size, uint32_t{259});
    EXPECT_TRUE(targets.class_b.placeholder);
    EXPECT_EQ(targets.class_b.idleslope_kbps, int32_t{733424});
    EXPECT_EQ(targets.class_b.sendslope_kbps, int32_t{-266576});
    EXPECT_EQ(targets.class_b.hicredit, int32_t{1131});
    EXPECT_EQ(targets.class_b.locredit, int32_t{-411});
}

#if defined(__linux__)
namespace {

/// A qdisc as parse_qdisc() reports it: @p kind on @p parent, handle @p handle, no parameters.
auto qdisc_info(uint32_t const handle, uint32_t const parent, std::string_view const kind) -> net::tc::QdiscInfo
{
    net::tc::QdiscInfo info{.handle = handle, .parent = parent};
    std::ranges::copy(kind.substr(0, net::tc::MAX_KIND_LENGTH), info.kind.begin());
    return info;
}

/// An mqprio root of handle @p handle with the layout @p mqprio.
auto mqprio_root(uint32_t const handle, net::tc::MqprioQdisc const& mqprio) -> net::tc::QdiscInfo
{
    auto info = qdisc_info(handle, net::tc::ROOT, "mqprio");
    info.has_mqprio_params = true;
    info.mqprio = mqprio;
    return info;
}

/// A TcControl that records each request as `tc` would spell it and answers
/// from a canned root and canned errors; the qdiscs below the root are kept as the kernel would.
class RecordingTc final : public net::tc::TcControl
{
  public:
    std::vector<std::string> requests{};
    std::optional<net::tc::QdiscInfo> root{};    ///< get(ROOT)'s answer (none: ENOENT)
    std::vector<net::tc::QdiscInfo> children{};  ///< the qdiscs below the root (get's answers)
    std::error_code mqprio_error{};              ///< replace_mqprio's first answer
    std::error_code cbs_error{};                 ///< every replace_cbs's answer
    std::optional<uint32_t> cbs_refused_on{};    ///< a replace_cbs on this parent answers cbs_refusal
    std::errc cbs_refusal{std::errc::operation_not_supported};
    std::error_code etf_error{};  ///< every replace_etf's answer
    uint16_t next_major{0x8001};  ///< the handle major the kernel gives the next new qdisc

    auto replace_mqprio(int /*ifindex*/, net::tc::MqprioQdisc const& q) -> Status override
    {
        requests.push_back(std::format("replace root mqprio {:x}:", q.major));
        if (mqprio_error) {
            return failure(std::exchange(mqprio_error, {}));
        }
        return success();
    }
    auto replace_cbs(int /*ifindex*/, net::tc::CbsQdisc const& q) -> Status override
    {
        requests.push_back(std::format(
            "replace {:x}:{:x} cbs {:x}: idleslope {}", q.parent >> 16U, q.parent & 0xFFFFU, q.handle_major, q.idleslope_kbps));
        if (q.handle_major == 0) {
            return failure(net::NetError::invalid_argument);  // as core's build_replace_cbs refuses it
        }
        if (cbs_error) {
            return failure(cbs_error);
        }
        if (cbs_refused_on == q.parent) {
            return failure(std::make_error_code(cbs_refusal));
        }
        if (auto* const there = find(q.parent); there != nullptr && there->handle >> 16U == q.handle_major) {
            there->cbs = q;  // a change in place
            return success();
        }
        erase(q.parent);  // a graft over what was there
        auto info = qdisc_info(net::tc::make_handle(q.handle_major, 0), q.parent, "cbs");
        info.has_cbs_params = true;
        info.cbs = q;
        children.push_back(std::move(info));
        return success();
    }
    auto replace_etf(int /*ifindex*/, net::tc::EtfQdisc const& q) -> Status override
    {
        requests.push_back(std::format(
            "replace {:x}:{:x} etf clockid {} delta {}{}{}{}",
            q.parent >> 16U,
            q.parent & 0xFFFFU,
            q.clockid,
            q.delta_ns,
            q.offload ? " offload" : "",
            q.deadline_mode ? " deadline_mode" : "",
            q.skip_sock_check ? " skip_sock_check" : ""));
        if (etf_error) {
            return failure(etf_error);
        }
        erase(q.parent);
        place(etf_info(0, q));
        return success();
    }
    auto replace_kind(int /*ifindex*/, uint32_t const parent, std::string_view const kind) -> Status override
    {
        requests.push_back(std::format("replace {:x}:{:x} {}", parent >> 16U, parent & 0xFFFFU, kind));
        erase(parent);
        return success();
    }
    auto remove(int /*ifindex*/, uint32_t const handle, uint32_t const parent) -> Status override
    {
        if (parent == net::tc::ROOT) {
            requests.push_back(std::format("del root {:x}:", handle >> 16U));
            return success();
        }
        requests.push_back(std::format("del {:x}:{:x} handle {:x}:", parent >> 16U, parent & 0xFFFFU, handle >> 16U));
        erase(parent);
        return success();
    }
    auto get(int /*ifindex*/, uint32_t const parent) -> StatusValue<net::tc::QdiscInfo> override
    {
        requests.push_back(
            parent == net::tc::ROOT ? std::string{"show root"} : std::format("show {:x}:{:x}", parent >> 16U, parent & 0xFFFFU));
        if (parent == net::tc::ROOT && root) {
            return *root;
        }
        if (auto const* const child = find(parent); child != nullptr && parent != net::tc::ROOT) {
            return *child;
        }
        return failure(net::NetError::invalid_handle);
    }
    [[nodiscard]] auto last_message() const noexcept -> std::string const& override { return message_; }

    /// A qdisc of @p kind on @p parent with handle @p major:, as an earlier run left it.
    void left(uint32_t const parent, uint16_t const major, std::string_view const kind)
    {
        children.push_back(qdisc_info(net::tc::make_handle(major, 0), parent, kind));
    }
    /// Our etf @p etf on its parent with handle @p major:, as an earlier run left it.
    void left(uint16_t const major, net::tc::EtfQdisc const& etf)
    {
        children.push_back(etf_info(net::tc::make_handle(major, 0), etf));
    }
    /// The requests since the last drain, cleared.
    auto drain() -> std::vector<std::string> { return std::exchange(requests, {}); }

  private:
    static auto etf_info(uint32_t const handle, net::tc::EtfQdisc const& etf) -> net::tc::QdiscInfo
    {
        auto info = qdisc_info(handle, etf.parent, "etf");
        info.has_etf_params = true;
        info.etf = etf;
        return info;
    }
    auto find(uint32_t const parent) -> net::tc::QdiscInfo*
    {
        auto const it = std::ranges::find(children, parent, &net::tc::QdiscInfo::parent);
        return it != children.end() ? &*it : nullptr;
    }
    /// A new qdisc on its parent, numbered by the "kernel".
    void place(net::tc::QdiscInfo info)
    {
        info.handle = net::tc::make_handle(next_major++, 0);
        children.push_back(std::move(info));
    }
    /// The qdisc on @p parent goes, and its own children with it.
    void erase(uint32_t const parent)
    {
        auto const* const gone = find(parent);
        if (gone == nullptr) {
            return;
        }
        uint32_t const major = gone->handle >> 16U;
        std::erase_if(children, [&](net::tc::QdiscInfo const& q) { return q.parent == parent || (q.parent >> 16U) == major; });
    }

    std::string message_{};
};

/// The etf the shaper asks for at @p delta_ns on @p parent.
auto our_etf(uint32_t const parent, int32_t const delta_ns = DEFAULT_ETF_DELTA_NS) -> net::tc::EtfQdisc
{
    return net::tc::EtfQdisc{.parent = parent, .delta_ns = delta_ns, .offload = true, .skip_sock_check = true};
}

auto igb_layout() -> QueueLayout
{
    return *make_queue_layout("igb", 4);
}

auto igb_mqprio() -> net::tc::MqprioQdisc
{
    return *mqprio_for(igb_layout(), 0x9000);
}

/// The admitted AAF + CRF (class A 19136 kbit/s) and nothing, at 1 Gbit/s.
auto admitted_targets() -> CbsTargets
{
    std::array<TSpec, 2> const streams{worked_tspec(aaf_8ch_48k_int24()), worked_tspec(crf_one_timestamp())};
    return cbs_targets(streams, {}, 1'000'000, 1500);
}

auto idle_targets() -> CbsTargets
{
    return cbs_targets({}, {}, 1'000'000, 1500);
}

using Requests = std::vector<std::string>;

}  // namespace

// A fresh start (the default mq root): mqprio, then a cbs on both class
// queues at once, class A's (9000:1, queue 0) first. On igb the first cbs
// switches FQTSS on (one reset); the second finds it on.
TEST(shaper_qdiscs, a_fresh_start_installs_both_cbs)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = qdisc_info(0, net::tc::ROOT, "mq");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.installed() && !qdiscs.reused());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 375000",
            "replace 9000:2 cbs 9002: idleslope 375000",
        }));
    EXPECT_TRUE(qdiscs.class_a().placeholder && qdiscs.class_b().placeholder);
    // Nothing changed: nothing sent.
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain().empty());
}

// Admission and withdrawal only ever change cbs parameters in place: no
// pfifo_fast graft, no delete, so the last cbs never goes while running.
TEST(shaper_qdiscs, admission_and_withdrawal_only_replace_cbs)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    (void)tc.drain();

    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "replace 9000:1 cbs 9001: idleslope 19136",
            "replace 9000:2 cbs 9002: idleslope 730864",
        }));
    EXPECT_FALSE(qdiscs.class_a().placeholder);

    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "replace 9000:1 cbs 9001: idleslope 375000",
            "replace 9000:2 cbs 9002: idleslope 375000",
        }));
    EXPECT_TRUE(qdiscs.class_a().placeholder && qdiscs.class_b().placeholder);
}

// A restart onto the root a previous run left (restore_qdisc = false, or a
// crash): kept as it is, no mqprio replace or delete (which would destroy its
// cbs and reset an igb twice); its cbs are changed in place.
TEST(shaper_qdiscs, a_restart_keeps_an_identical_mqprio)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = mqprio_root(0x9000'0000, igb_mqprio());
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.reused());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "show 9000:1",  // a kept root's class: no cbs on it, so a new handle
            "replace 9000:1 cbs 9001: idleslope 375000",
            "show 9000:2",
            "replace 9000:2 cbs 9002: idleslope 375000",
        }));
}

// A cbs is sent with its own handle (core refuses major 0): a restart over an earlier run's cbs
// 8009: on class A changes it in place under that handle, and every later program() reuses it; the
// etf goes under it
TEST(shaper_qdiscs, a_leftover_cbs_keeps_its_handle_through_later_programs)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = mqprio_root(0x9000'0000, igb_mqprio());
    tc.left(0x9000'0001, 0x8009, "cbs");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.install(3, "enp1s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "show 9000:1",
            "replace 9000:1 cbs 8009: idleslope 375000",
            "show 9000:2",
            "replace 9000:2 cbs 9002: idleslope 375000",
            "show 8009:1",
            "replace 8009:1 etf clockid 11 delta 150000 offload skip_sock_check",
        }));
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 8009: idleslope 19136", "replace 9000:2 cbs 9002: idleslope 730864"}));
}

// A root major near the top of the handle space gives cbs handles below it, never ffff: (ingress)
TEST(shaper_qdiscs, cbs_handles_stay_below_ffff)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = qdisc_info(0, net::tc::ROOT, "mq");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    auto const mqprio = *mqprio_for(igb_layout(), 0xFFFE);
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", mqprio, igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio fffe:",
            "replace fffe:1 cbs fffd: idleslope 375000",
            "replace fffe:2 cbs fffc: idleslope 375000",
        }));
}

// Another handle's mqprio is deleted before ours is laid, its classes back to pfifo_fast first (highest
// queue first), so a cbs it carried is not left on in the hardware.
TEST(shaper_qdiscs, another_handle_s_mqprio_is_cleared_before_it_goes)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    auto theirs = igb_mqprio();
    theirs.major = 0x0001;
    tc.root = mqprio_root(0x0001'0000, theirs);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace 1:4 pfifo_fast",
            "replace 1:3 pfifo_fast",
            "replace 1:2 pfifo_fast",
            "replace 1:1 pfifo_fast",
            "del root 1:",
            "replace root mqprio 9000:",
        }));
}

// Our handle with another layout (a changed queue override) cannot be changed
// in place: mqprio answers EINVAL, the root's classes go back to pfifo_fast
// (highest queue first, so no cbs stays on), and it is deleted and laid again.
TEST(shaper_qdiscs, another_layout_under_our_handle_is_replaced)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    auto other = igb_mqprio();
    other.prio_tc_map[5] = CLASS_A_TC;
    tc.root = mqprio_root(0x9000'0000, other);
    tc.mqprio_error = std::make_error_code(std::errc::invalid_argument);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_FALSE(qdiscs.reused());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:4 pfifo_fast",
            "replace 9000:3 pfifo_fast",
            "replace 9000:2 pfifo_fast",
            "replace 9000:1 pfifo_fast",
            "del root 9000:",
            "replace root mqprio 9000:",
        }));
    // A root that sets no traffic classes is simply replaced.
    RecordingTc foreign{};
    foreign.root = qdisc_info(0x0001'0000, net::tc::ROOT, "fq");
    ShaperQdiscs replacing{foreign, channel.logger()};
    EXPECT_TRUE(replacing.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(foreign.drain() == Requests({"show root", "replace root mqprio 9000:"}));
}

// Another handle's mqprio (an AES67 socket's 9100:, a profile switch ago) or taprio is deleted before ours goes
// on: replaced in place, its destroy would run netdev_set_num_tc(0) after our init and clear our map.
TEST(shaper_qdiscs, another_handle_s_mqprio_or_taprio_is_deleted_before_ours_is_laid)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = qdisc_info(0x9100'0000, net::tc::ROOT, "mqprio");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "eth1", igb_mqprio(), igb_layout()).has_value());
    EXPECT_FALSE(qdiscs.reused());
    EXPECT_TRUE(tc.drain() == Requests({"show root", "del root 9100:", "replace root mqprio 9000:"}));
    // taprio_destroy resets the TCs too (netdev_reset_tc)
    RecordingTc gated{};
    gated.root = qdisc_info(0x0001'0000, net::tc::ROOT, "taprio");
    ShaperQdiscs after_taprio{gated, channel.logger()};
    EXPECT_TRUE(after_taprio.install(3, "eth1", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(gated.drain() == Requests({"show root", "del root 1:", "replace root mqprio 9000:"}));
}

// A driver without cbs offload (EOPNOTSUPP): logged once, then no further
// cbs request; the mqprio steering stays, and uninstall has no cbs to remove.
TEST(shaper_qdiscs, no_cbs_offload_stops_asking)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.cbs_error = std::make_error_code(std::errc::operation_not_supported);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "eth0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());  // a refusal is a state, not an error
    EXPECT_TRUE(qdiscs.cbs_unsupported());
    EXPECT_TRUE(tc.drain() == Requests({"show root", "replace root mqprio 9000:", "replace 9000:1 cbs 9001: idleslope 375000"}));
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain().empty());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:2 pfifo_fast", "replace 9000:1 pfifo_fast", "del root 9000:"}));
}

// A failed cbs request (here EBUSY) is not recorded as programmed: the next
// program() with the same targets sends it again (EntityShaper keeps itself
// dirty and retries from tick()).
TEST(shaper_qdiscs, a_failed_cbs_is_sent_again)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    tc.cbs_error = std::make_error_code(std::errc::device_or_resource_busy);
    EXPECT_FALSE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_FALSE(qdiscs.cbs_unsupported());
    EXPECT_TRUE(qdiscs.class_a().empty() && qdiscs.class_b().empty());
    (void)tc.drain();
    tc.cbs_error = {};
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 375000", "replace 9000:2 cbs 9002: idleslope 375000"}));
}

// Uninstall removes class B's cbs first, then class A's (igc's order), then the
// root, whatever was recorded (a kept root may hold a cbs this run never
// programmed); release leaves everything for the next start.
TEST(shaper_qdiscs, uninstall_and_release)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    (void)tc.drain();
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "replace 9000:2 pfifo_fast",
            "replace 9000:1 pfifo_fast",
            "del root 9000:",
        }));
    EXPECT_FALSE(qdiscs.installed());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.drain().empty());

    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());  // what shutdown parks
    (void)tc.drain();
    qdiscs.release();
    EXPECT_FALSE(qdiscs.installed());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.drain().empty());
}
// Launch time wanted on a driver that honours it (igc): after both cbs, an offloaded etf on class
// A's cbs class (9001:1), the AF_XDP socket's queue, settled once; class B's gets none; admission
// changes then touch the cbs only.
TEST(shaper_launch_time, an_offloaded_etf_goes_under_class_as_cbs_only)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = qdisc_info(0, net::tc::ROOT, "mq");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.install(3, "enp1s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 375000",
            "replace 9000:2 cbs 9002: idleslope 375000",
            "show 9001:1",
            "replace 9001:1 etf clockid 11 delta 150000 offload skip_sock_check",
        }));
    EXPECT_TRUE(qdiscs.launch_time());
    EXPECT_TRUE(qdiscs.launch_time_mode() == LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 19136", "replace 9000:2 cbs 9002: idleslope 730864"}));
}

// A restart onto what an earlier run left: an identical etf stays (etf cannot be changed in place),
// one of another delta is deleted and laid down again; neither touches the cbs above it.
TEST(shaper_launch_time, a_restart_keeps_an_identical_etf_and_replaces_another)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    for (int32_t const left_delta : {DEFAULT_ETF_DELTA_NS, 500'000}) {
        RecordingTc tc{};
        tc.root = mqprio_root(0x9000'0000, igb_mqprio());
        tc.left(0x9000'0001, 0x8001, "cbs");
        tc.left(0x8003, our_etf(0x8001'0001, left_delta));
        tc.left(0x9000'0002, 0x8002, "cbs");
        tc.next_major = 0x8005;
        ShaperQdiscs qdiscs{tc, channel.logger()};
        qdiscs.set_launch_time(LaunchTimeMode::on);
        EXPECT_TRUE(qdiscs.install(3, "enp1s0", igb_mqprio(), igb_layout()).has_value());
        EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
        Requests want{
            "show root",
            "show 9000:1",
            "replace 9000:1 cbs 8001: idleslope 375000",  // the leftover cbs, changed in place
            "show 9000:2",
            "replace 9000:2 cbs 8002: idleslope 375000",
            "show 8001:1",
        };
        if (left_delta != DEFAULT_ETF_DELTA_NS) {
            want.emplace_back("del 8001:1 handle 8003:");
            want.emplace_back("replace 8001:1 etf clockid 11 delta 150000 offload skip_sock_check");
        }
        EXPECT_TRUE(tc.drain() == want);
        EXPECT_TRUE(qdiscs.launch_time());
    }
}

// Launch time not wanted on a driver that could have it: an etf an earlier run left is deleted
// (it would drop AF_PACKET frames); a queue without one is only looked at.
TEST(shaper_launch_time, an_etf_left_behind_goes_when_not_wanted)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = mqprio_root(0x9000'0000, igb_mqprio());
    tc.left(0x9000'0001, 0x8001, "cbs");
    tc.left(0x8003, our_etf(0x8001'0001));
    tc.left(0x9000'0002, 0x8002, "cbs");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::off);
    EXPECT_TRUE(qdiscs.install(3, "enp1s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "show 9000:1",
            "replace 9000:1 cbs 8001: idleslope 375000",
            "show 9000:2",
            "replace 9000:2 cbs 8002: idleslope 375000",
            "show 8001:1",
            "del 8001:1 handle 8003:",
        }));
    EXPECT_FALSE(qdiscs.launch_time());
}

// Launch time enabled at run time (off at install: the clock not yet on the PHC), disabled when the
// clock drifts and enabled again: each program() after a change of mode puts or removes the etf
// under class A's cbs, which stays: no root or cbs change, so no adapter reset.
TEST(shaper_launch_time, enabled_and_disabled_at_run_time_the_etf_follows_under_the_cbs)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = qdisc_info(0, net::tc::ROOT, "mq");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::off);
    EXPECT_TRUE(qdiscs.install(3, "enp1s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    (void)tc.drain();
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain().empty());  // off: settled, nothing more
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"show 9001:1", "replace 9001:1 etf clockid 11 delta 150000 offload skip_sock_check"}));
    EXPECT_TRUE(qdiscs.launch_time());
    qdiscs.set_launch_time(LaunchTimeMode::off);
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"show 9001:1", "del 9001:1 handle 8001:"}));
    EXPECT_FALSE(qdiscs.launch_time());
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"show 9001:1", "replace 9001:1 etf clockid 11 delta 150000 offload skip_sock_check"}));
    EXPECT_TRUE(qdiscs.launch_time());
}

// A driver that honours no AF_XDP launch time (igb), launch time wanted: the requests are the cbs
// alone, etf neither installed nor looked for. Not wanted, it is off: a leftover etf still goes.
TEST(shaper_launch_time, a_driver_without_launch_time_gets_no_etf)
{
    EXPECT_TRUE(launch_time_mode(true, true) == LaunchTimeMode::on);
    EXPECT_TRUE(launch_time_mode(false, true) == LaunchTimeMode::off);
    EXPECT_TRUE(launch_time_mode(true, false) == LaunchTimeMode::unsupported);
    EXPECT_TRUE(launch_time_mode(false, false) == LaunchTimeMode::off);
    // A failed query: wanted, nothing is done; not wanted, a leftover etf still goes.
    EXPECT_TRUE(launch_time_mode(true, failure(std::errc::no_such_file_or_directory)) == LaunchTimeMode::unsupported);
    EXPECT_TRUE(launch_time_mode(false, failure(std::errc::no_such_file_or_directory)) == LaunchTimeMode::off);

    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = qdisc_info(0, net::tc::ROOT, "mq");
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(launch_time_mode(true, false));
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 375000",
            "replace 9000:2 cbs 9002: idleslope 375000",
        }));
    EXPECT_FALSE(qdiscs.launch_time());
}

// A NIC with launch time and no cbs offload: the etf goes on class A's mqprio class itself, once, and the
// uninstall's pfifo_fast graft removes it with the class.
TEST(shaper_launch_time, without_cbs_offload_the_etf_goes_on_class_a_s_queue)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.cbs_error = std::make_error_code(std::errc::operation_not_supported);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.install(3, "eth0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());  // a refusal is a state, not an error
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 375000",
            "show 9000:1",
            "replace 9000:1 etf clockid 11 delta 150000 offload skip_sock_check",
        }));
    EXPECT_TRUE(qdiscs.launch_time());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain().empty());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:2 pfifo_fast", "replace 9000:1 pfifo_fast", "del root 9000:"}));
}

// Without cbs offload and launch time off, an etf an earlier run left on class A's mqprio class gives way
// to pfifo_fast: deleting an mqprio class's child would graft noop_qdisc and drop the class's traffic.
TEST(shaper_launch_time, without_cbs_offload_a_leftover_etf_gives_way_to_pfifo_fast)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.root = mqprio_root(0x9000'0000, igb_mqprio());
    tc.left(0x8003, our_etf(0x9000'0001));
    tc.cbs_error = std::make_error_code(std::errc::operation_not_supported);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::off);
    EXPECT_TRUE(qdiscs.install(3, "eth0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());  // a refusal is a state, not an error
    EXPECT_TRUE(
        tc.drain() ==
        Requests(
            {"show root", "show 9000:1", "replace 9000:1 cbs 9001: idleslope 375000", "show 9000:1", "replace 9000:1 pfifo_fast"}));
    EXPECT_FALSE(qdiscs.launch_time());
}

// Class A's cbs offloaded and class B's refused: the etf still goes under class A's cbs, never in its place.
TEST(shaper_launch_time, a_refused_class_b_cbs_leaves_the_etf_under_class_a_s)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.cbs_refused_on = 0x9000'0002;
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.install(3, "eth0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());  // a refusal is a state, not an error
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 375000",
            "replace 9000:2 cbs 9002: idleslope 375000",
            "show 9001:1",
            "replace 9001:1 etf clockid 11 delta 150000 offload skip_sock_check",
        }));
    EXPECT_TRUE(qdiscs.launch_time());
    EXPECT_FALSE(qdiscs.cbs_unsupported());
    // Class A's cbs still follows its streams; class B's is not asked again.
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 19136"}));
}

// igc refuses a cbs above queue 1 with EINVAL: class B's is refused as with EOPNOTSUPP, once, and the
// etf still settles under class A's cbs.
TEST(shaper_launch_time, an_einval_class_b_cbs_is_a_refusal_too)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.cbs_refused_on = 0x9000'0002;
    tc.cbs_refusal = std::errc::invalid_argument;
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::on);
    EXPECT_TRUE(qdiscs.install(3, "eth0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_FALSE(qdiscs.cbs_unsupported());
    EXPECT_TRUE(qdiscs.launch_time());
    (void)tc.drain();
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 19136"}));
}

// Class B's cbs waits for class A's: a failed class A request (here EBUSY) sends no class B request
// (igc enables cbs on queue 0 first), and the next program() sends both.
TEST(shaper_qdiscs, class_b_s_cbs_waits_for_class_a_s)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    (void)tc.drain();
    tc.cbs_error = std::make_error_code(std::errc::device_or_resource_busy);
    EXPECT_FALSE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 375000"}));
    tc.cbs_error = {};
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 375000", "replace 9000:2 cbs 9002: idleslope 375000"}));
}

// A refused etf offload (EOPNOTSUPP) is recorded and logged once; the cbs stay, program() succeeds
// (nothing for tick() to retry) and no further etf is asked for until the next install().
TEST(shaper_launch_time, a_refused_etf_is_reported_and_not_asked_again)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.etf_error = std::make_error_code(std::errc::operation_not_supported);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    qdiscs.set_launch_time(LaunchTimeMode::on, 200'000);
    EXPECT_TRUE(qdiscs.install(3, "enp1s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 375000",
            "replace 9000:2 cbs 9002: idleslope 375000",
            "show 9001:1",
            "replace 9001:1 etf clockid 11 delta 200000 offload skip_sock_check",
        }));
    EXPECT_FALSE(qdiscs.launch_time());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 19136", "replace 9000:2 cbs 9002: idleslope 730864"}));
    // Uninstall is unchanged: grafting pfifo_fast on each class removes cbs and etf together.
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:2 pfifo_fast", "replace 9000:1 pfifo_fast", "del root 9000:"}));
}

namespace {

/// An igb port of 4 TX queues the PortShaper is told about rather than reads.
auto igb_port() -> ShaperPort
{
    return ShaperPort{.ifindex = 3, .driver = "igb", .tx_queues = 4, .mtu = 1500};
}

/// The AAF + CRF TSpecs of admitted_targets().
auto aaf_and_crf() -> std::array<TSpec, 2>
{
    return {worked_tspec(aaf_8ch_48k_int24()), worked_tspec(crf_one_timestamp())};
}

}  // namespace

// A port's user gives the class A TSpecs itself, before or after install(); the shaper is the
// kit's without the kit (IEEE 802.1Q-2022 Annex L sizing as in admitted_targets()).
TEST(port_shaper, the_tspecs_given_before_install_are_programmed_at_install)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    PortShaper shaper{ShaperConfig{.interface_name = "port-shaper0"}, channel.logger(), tc};
    auto const streams = aaf_and_crf();
    EXPECT_TRUE(shaper.program(streams).has_value());
    EXPECT_TRUE(tc.drain().empty());
    EXPECT_TRUE(shaper.install(igb_port()).has_value());
    EXPECT_TRUE(shaper.installed());
    EXPECT_TRUE(
        tc.drain() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs 9001: idleslope 19136",
        }));
    EXPECT_TRUE(shaper.class_a() == admitted_targets().class_a);
    EXPECT_TRUE(shaper.class_b().empty());
    EXPECT_TRUE(shaper.layout() == igb_layout());
    EXPECT_EQ(shaper.driver(), std::string{"igb"});
}

TEST(port_shaper, tspecs_given_after_install_go_out_on_the_next_tick)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    PortShaper shaper{ShaperConfig{.interface_name = "port-shaper0"}, channel.logger(), tc};
    EXPECT_TRUE(shaper.install(igb_port()).has_value());
    EXPECT_TRUE(shaper.class_a().placeholder);
    (void)tc.drain();
    auto const streams = aaf_and_crf();
    EXPECT_TRUE(shaper.program(streams).has_value());
    EXPECT_TRUE(tc.drain().empty());
    shaper.tick(1'000'000'000);
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 19136"}));
    EXPECT_TRUE(shaper.program({}).has_value());
    shaper.tick(1'010'000'000);
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 100000"}));
    shaper.tick(1'020'000'000);
    EXPECT_TRUE(tc.drain().empty());
}

// A port running class B alone (SrClasses::b): on igb class B uses queue 0, which igc shapes
// first, its PCP alone maps to an SR TC, and only its cbs is put, sized by Annex L L-11 with no class A,
// or a placeholder at CLASS_B_PLACEHOLDER_PERCENT; class A's queue gets nothing.
TEST(port_shaper, a_class_b_port_shapes_class_b_alone_on_queue_0)
{
    auto const layout = make_queue_layout("igb", 4, {}, SrClasses::b);
    EXPECT_TRUE(layout.has_value() && layout->class_b_queue == 0 && layout->class_a_queue == 1);
    EXPECT_FALSE(make_queue_layout("igb", 4, {.class_a = 1, .class_b = 0}, SrClasses::both).has_value());
    EXPECT_FALSE(make_queue_layout("igc", 4, {.class_a = 0, .class_b = 1}, SrClasses::b).has_value());  // unshapeable
    auto const mqprio = layout
        ? mqprio_for(*layout, 0x9000, DEFAULT_CLASS_A_PCP, DEFAULT_CLASS_B_PCP, DEFAULT_PTP_PRIORITY, SrClasses::b)
        : StatusValue<net::tc::MqprioQdisc>{failure(ShaperError::bad_queue_override)};
    EXPECT_TRUE(mqprio.has_value() && mqprio->prio_tc_map[DEFAULT_CLASS_B_PCP] == CLASS_B_TC);
    EXPECT_TRUE(mqprio.has_value() && mqprio->prio_tc_map[DEFAULT_CLASS_A_PCP] == LEGACY_TC);
    EXPECT_TRUE(mqprio.has_value() && mqprio->offset[CLASS_B_TC] == 0);

    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    PortShaper shaper{ShaperConfig{.interface_name = "port-shaper0", .classes = SrClasses::b}, channel.logger(), tc};
    EXPECT_TRUE(shaper.install(igb_port()).has_value());
    EXPECT_TRUE(shaper.class_b().placeholder && shaper.class_a().empty());
    auto const installed = tc.drain();
    EXPECT_TRUE(std::ranges::find(installed, std::string{"replace 9000:1 cbs 9001: idleslope 100000"}) != installed.end());
    EXPECT_TRUE(std::ranges::none_of(installed, [](std::string const& r) { return r.find("9000:2") != std::string::npos; }));
    std::array<TSpec, 2> const class_b{TSpec{.max_frame_size = 329, .max_interval_frames = 1}, TSpec{329, 1}};
    EXPECT_TRUE(shaper.program({}, class_b).has_value());
    shaper.tick(1'000'000'000);
    auto const expected = class_b_targets(class_b, 1'000'000, 1500);
    EXPECT_EQ(expected.class_b.idleslope_kbps, 23744);  // 2 x 371 octets x 8 x 4000 per second
    EXPECT_TRUE(shaper.class_b() == expected.class_b && shaper.class_a().empty());
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 23744"}));
    EXPECT_TRUE(port_targets(SrClasses::both, {}, class_b, 1'000'000, 1500) == cbs_targets({}, class_b, 1'000'000, 1500));
}

// Streams of one frame size fold into one TSpec, sized alike (Annex L sums them); past MAX_PORT_SHAPER_STREAMS
// frame sizes the set is refused
TEST(port_shaper, more_frame_sizes_than_it_sizes_for_are_refused)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    PortShaper shaper{ShaperConfig{.interface_name = "port-shaper0"}, channel.logger(), tc};
    EXPECT_TRUE(shaper.install(igb_port()).has_value());
    (void)tc.drain();
    std::vector<TSpec> const alike(MAX_PORT_SHAPER_STREAMS + 1, TSpec{.max_frame_size = 185, .max_interval_frames = 1});
    EXPECT_TRUE(shaper.program(alike).has_value());
    shaper.tick(1'000'000'000);
    EXPECT_TRUE(shaper.class_a() == class_a_targets(alike, 1'000'000, 1500).class_a);
    (void)tc.drain();
    std::vector<TSpec> many;
    for (size_t i = 0; i <= MAX_PORT_SHAPER_STREAMS; ++i) {
        many.push_back(TSpec{.max_frame_size = static_cast<uint16_t>(100 + i), .max_interval_frames = 1});
    }
    auto const refused = shaper.program(many);
    EXPECT_FALSE(refused.has_value());
    EXPECT_TRUE(!refused.has_value() && refused.error() == make_error_code(ShaperError::too_many_streams));
    shaper.tick(1'010'000'000);
    EXPECT_TRUE(tc.drain().empty());
    EXPECT_TRUE(shaper.class_a() == class_a_targets(alike, 1'000'000, 1500).class_a);  // as it was
}

TEST(port_shaper, a_bad_class_a_priority_installs_nothing)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    PortShaper shaper{ShaperConfig{.interface_name = "port-shaper0"}, channel.logger(), tc, DEFAULT_CLASS_B_PCP};
    auto const status = shaper.install(igb_port());
    EXPECT_TRUE(!status.has_value() && status.error() == make_error_code(ShaperError::bad_priority));
    EXPECT_FALSE(shaper.installed());
    EXPECT_TRUE(tc.drain().empty());
}

// A port runs class A alone (Milan): class B's PCP rides the legacy TC, so its queue carries nothing
// and gets no cbs; a root a two-class run left has another map and is replaced, its cbs with it.
TEST(port_shaper, class_b_maps_to_legacy_and_gets_no_cbs)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    auto const mqprio =
        mqprio_for(igb_layout(), 0x9000, DEFAULT_CLASS_A_PCP, DEFAULT_CLASS_B_PCP, DEFAULT_PTP_PRIORITY, SrClasses::a);
    EXPECT_TRUE(mqprio.has_value() && mqprio->prio_tc_map[DEFAULT_CLASS_B_PCP] == LEGACY_TC);
    EXPECT_TRUE(mqprio.has_value() && mqprio->prio_tc_map[DEFAULT_CLASS_A_PCP] == CLASS_A_TC);
    RecordingTc tc{};
    tc.root = mqprio_root(0x9000'0000, igb_mqprio());
    PortShaper shaper{ShaperConfig{.interface_name = "port-shaper0"}, channel.logger(), tc};
    auto const streams = aaf_and_crf();
    EXPECT_TRUE(shaper.program(streams).has_value());
    EXPECT_TRUE(shaper.install(igb_port()).has_value());
    auto const requests = tc.drain();
    EXPECT_TRUE(std::ranges::find(requests, std::string{"replace root mqprio 9000:"}) != requests.end());
    EXPECT_TRUE(std::ranges::find(requests, std::string{"replace 9000:1 cbs 9001: idleslope 19136"}) != requests.end());
    EXPECT_TRUE(std::ranges::none_of(requests, [](std::string const& r) { return r.find("9000:2") != std::string::npos; }));
    EXPECT_TRUE(shaper.class_b().empty());
}

// shutdown() parks the cbs at the placeholders and leaves the qdiscs, or with restore_qdisc
// removes them (docs/SHAPER.md, "Adapter resets").
TEST(port_shaper, shutdown_parks_or_restores)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    auto const streams = aaf_and_crf();
    RecordingTc tc{};
    PortShaper parked{ShaperConfig{.interface_name = "port-shaper0"}, channel.logger(), tc};
    EXPECT_TRUE(parked.program(streams).has_value());
    EXPECT_TRUE(parked.install(igb_port()).has_value());
    (void)tc.drain();
    parked.shutdown();
    EXPECT_TRUE(tc.drain() == Requests({"replace 9000:1 cbs 9001: idleslope 100000"}));
    EXPECT_FALSE(parked.installed());
    parked.shutdown();
    EXPECT_TRUE(tc.drain().empty());

    RecordingTc restored_tc{};
    PortShaper restored{ShaperConfig{.interface_name = "port-shaper0", .restore_qdisc = true}, channel.logger(), restored_tc};
    EXPECT_TRUE(restored.install(igb_port()).has_value());
    (void)restored_tc.drain();
    restored.shutdown();
    // Class A only: nothing maps to class B's queue, so no cbs is there to remove.
    EXPECT_TRUE(restored_tc.drain() == Requests({"replace 9000:1 pfifo_fast", "del root 9000:"}));
}

#endif

TEST(shaper_errors, messages)
{
    EXPECT_TRUE(make_error_code(ShaperError::not_avb_capable).message().find("3 TX queues") != std::string::npos);
    EXPECT_EQ(std::string{shaper_error_category().name()}, std::string{"statusbar.avb_entity.shaper"});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_shaper_test)
