// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The shaper's pure parts (queue layout, mqprio, IEEE 802.1Q Annex L cbs parameters, placeholders)
// and ShaperQdiscs against a recording TcControl; EntityShaper needs rtnetlink and is not run here.
// The arithmetic is worked in docs/SHAPER.md ("Worked examples").

#include "statusbar/avb_entity/avb_entity_shaper.hpp"

#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
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
    // The TSpecs the kit advertises (srp_max_frame_size, Milan v1.2 Table 4.4):
    //   AAF 8 ch INT_24 48 kHz: 24-byte AAF header + 6 samples x 8 ch x 3 B + 1 = 18*8 + 24 + 1 = 169
    //   CRF, one timestamp a PDU: 20-byte CRF header + 1 x 8 + 1 = 29
    auto const aaf = AvbEntityKit::talker_tspec(aaf_8ch_48k_int24());
    auto const crf = AvbEntityKit::talker_tspec(crf_one_timestamp());
    EXPECT_EQ(aaf.max_frame_size, uint16_t{169});
    EXPECT_EQ(aaf.max_interval_frames, uint16_t{1});
    EXPECT_EQ(crf.max_frame_size, uint16_t{29});
    // On the wire (Table 4.4 notes): max(MFS + 22, 68) + 20
    //   AAF: 169 + 22 = 191 (>= 68), + 20 = 211
    //   CRF: 29 + 22 = 51 -> 68, + 20 = 88
    EXPECT_EQ(wire_frame_size(169), uint32_t{211});
    EXPECT_EQ(wire_frame_size(29), uint32_t{88});
    // Class A: 8000 frames/s. AAF 211 x 8 x 8000 / 1000 = 13504 kbit/s
    // (Table 4.4: 1152*8 + 4288); CRF 88 x 8 x 8000 / 1000 = 5632 (Table 4.4).
    EXPECT_EQ(stream_bandwidth_kbps(aaf, CLASS_A_INTERVALS_PER_SEC), uint32_t{13504});
    EXPECT_EQ(stream_bandwidth_kbps(crf, CLASS_A_INTERVALS_PER_SEC), uint32_t{5632});
    // Class B: 4000 frames/s: 211 x 8 x 4000 / 1000 = 6752 kbit/s
    EXPECT_EQ(stream_bandwidth_kbps(aaf, CLASS_B_INTERVALS_PER_SEC), uint32_t{6752});
    // MFS 100 -> 142 octets; 3 frames per 250 us: 142 x 8 x 3 x 4000 / 1000 = 13632.
    EXPECT_EQ(
        stream_bandwidth_kbps(TSpec{.max_frame_size = 100, .max_interval_frames = 3}, CLASS_B_INTERVALS_PER_SEC), uint32_t{13632});
    // Rounds up: one 142-octet frame a second is 142 x 8 / 1000 = 1.136 -> 2 kbit/s
    EXPECT_EQ(stream_bandwidth_kbps(TSpec{.max_frame_size = 100, .max_interval_frames = 1}, 1), uint32_t{2});
}

// Milan v1.2 Section 4.3.3.2, Table 4.4, every row: MaxFrameSize and the
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
            uint32_t const mfs = (row.bytes_per_channel * n) + 24 + 1;
            if (mfs > 1500) {
                continue;  // past the MTU: not a stream the model allows
            }
            auto const tspec = AvbEntityKit::talker_tspec(spec);
            EXPECT_EQ(uint32_t{tspec.max_frame_size}, mfs);
            EXPECT_EQ(tspec.max_interval_frames, uint16_t{1});
            // The column's linear formula holds while the frame is past the
            // 68-octet minimum; below it the notes' rule 2 pads the frame.
            uint32_t const expected = (mfs + 22 < 68) ? 5632 : (row.kbps_per_channel * n) + 4288;
            EXPECT_EQ(stream_bandwidth_kbps(tspec, CLASS_A_INTERVALS_PER_SEC), expected);
        }
    }
    auto const crf = AvbEntityKit::talker_tspec(crf_one_timestamp());
    EXPECT_EQ(crf.max_frame_size, uint16_t{28 + 1});
    EXPECT_EQ(stream_bandwidth_kbps(crf, CLASS_A_INTERVALS_PER_SEC), uint32_t{5632});
}

TEST(shaper_cbs, class_a_aaf_and_crf_on_a_gigabit_link)
{
    // One 8-channel 48 kHz INT_24 AAF stream and the CRF, 1 Gbit/s, MTU 1500 interfering:
    // idleSlope 19136, sendSlope -980864, hiCredit 29, loCredit -206 (docs/SHAPER.md).
    std::array<TSpec, 2> const streams{
        AvbEntityKit::talker_tspec(aaf_8ch_48k_int24()), AvbEntityKit::talker_tspec(crf_one_timestamp())};
    auto const a = class_a_cbs(streams, 1'000'000, 1500);
    EXPECT_EQ(a.idleslope_kbps, int32_t{19136});
    EXPECT_EQ(a.sendslope_kbps, int32_t{-980864});
    EXPECT_EQ(a.hicredit, int32_t{29});
    EXPECT_EQ(a.locredit, int32_t{-206});
    EXPECT_EQ(a.max_frame_size, uint32_t{211});
    EXPECT_FALSE(a.empty());

    // The AAF alone at 100 Mbit/s: idleSlope 13504, sendSlope -86496, hiCredit 203, loCredit -182.
    auto const slow = class_a_cbs(std::span{streams}.first(1), 100'000, 1500);
    EXPECT_EQ(slow.idleslope_kbps, int32_t{13504});
    EXPECT_EQ(slow.sendslope_kbps, int32_t{-86496});
    EXPECT_EQ(slow.hicredit, int32_t{203});
    EXPECT_EQ(slow.locredit, int32_t{-182});

    // Nothing admitted: nothing to shape
    EXPECT_TRUE(class_a_cbs({}, 1'000'000, 1500).empty());
    EXPECT_TRUE(class_a_cbs({}, 1'000'000, 1500) == CbsParameters{});
}

TEST(shaper_cbs, class_b_sees_class_a_as_interference)
{
    std::array<TSpec, 2> const a_streams{
        AvbEntityKit::talker_tspec(aaf_8ch_48k_int24()), AvbEntityKit::talker_tspec(crf_one_timestamp())};
    auto const a = class_a_cbs(a_streams, 1'000'000, 1500);
    // One AAF stream in class B (6752 kbit/s), class A as above: sendSlopeB -993248,
    // hiCreditB 12 (L-11), loCreditB -209 (docs/SHAPER.md).
    std::array<TSpec, 1> const b_streams{AvbEntityKit::talker_tspec(aaf_8ch_48k_int24())};
    auto const b = class_b_cbs(b_streams, a, 1'000'000, 1500);
    EXPECT_EQ(b.idleslope_kbps, int32_t{6752});
    EXPECT_EQ(b.sendslope_kbps, int32_t{-993248});
    EXPECT_EQ(b.hicredit, int32_t{12});
    EXPECT_EQ(b.locredit, int32_t{-209});
    // With no class A: hiCreditB = ceil(6752 x 1500 / 1000000) = ceil(10.128) = 11
    auto const b_alone = class_b_cbs(b_streams, CbsParameters{}, 1'000'000, 1500);
    EXPECT_EQ(b_alone.hicredit, int32_t{11});
    EXPECT_TRUE(class_b_cbs({}, a, 1'000'000, 1500).empty());
}

// The kit's bound (MAX_ENTITY_AUDIO_STREAMS 32-channel 48 kHz INT_24 AAF talkers and the CRF,
// gigabit) fits within 802.1Q's default 75% SR budget; the same at INT_32 does not.
// Arithmetic in docs/SHAPER.md ("Worked examples").
TEST(shaper_cbs, class_a_at_the_kits_stream_bound)
{
    auto aaf = aaf_8ch_48k_int24();
    aaf.format.channels = 32;
    EXPECT_EQ(AvbEntityKit::talker_tspec(aaf).max_frame_size, uint16_t{601});
    EXPECT_EQ(stream_bandwidth_kbps(AvbEntityKit::talker_tspec(aaf), CLASS_A_INTERVALS_PER_SEC), uint32_t{41152});
    std::vector<TSpec> streams(MAX_ENTITY_AUDIO_STREAMS, AvbEntityKit::talker_tspec(aaf));
    streams.push_back(AvbEntityKit::talker_tspec(crf_one_timestamp()));
    EXPECT_EQ(streams.size(), MAX_ENTITY_STREAMS);
    auto const a = class_a_cbs(streams, 1'000'000, 1500);
    EXPECT_EQ(a.idleslope_kbps, int32_t{664064});
    EXPECT_EQ(a.sendslope_kbps, int32_t{-335936});
    EXPECT_EQ(a.hicredit, int32_t{997});
    EXPECT_EQ(a.locredit, int32_t{-216});
    EXPECT_EQ(a.max_frame_size, uint32_t{643});
    EXPECT_TRUE(a.idleslope_kbps < 750'000);

    aaf.format.aaf_format = avtp::AafFormat::int_32bit;
    std::vector<TSpec> wide(MAX_ENTITY_AUDIO_STREAMS, AvbEntityKit::talker_tspec(aaf));
    wide.push_back(AvbEntityKit::talker_tspec(crf_one_timestamp()));
    EXPECT_EQ(class_a_cbs(wide, 1'000'000, 1500).idleslope_kbps, int32_t{860672});
}

// The placeholder at half the 75% SR budget, 1 Gbit/s, MTU 1500 (wire frame 1542): idleSlope
// 375000, sendSlope -625000, hiCredit 563 (L-10), loCredit -963 (L-12).
TEST(shaper_cbs, placeholders)
{
    auto const idle = placeholder_cbs(1'000'000, 1500, 375'000);
    EXPECT_EQ(idle.idleslope_kbps, int32_t{375'000});
    EXPECT_EQ(idle.sendslope_kbps, int32_t{-625'000});
    EXPECT_EQ(idle.hicredit, int32_t{563});
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
    // The same at 100 Mbit/s: 37500 each, hiCredit 563, loCredit -963.
    auto const slow = cbs_targets({}, {}, 100'000, 1500);
    EXPECT_EQ(slow.class_a.idleslope_kbps, int32_t{37'500});
    EXPECT_EQ(slow.class_a.hicredit, int32_t{563});
    EXPECT_EQ(slow.class_a.locredit, int32_t{-963});
    // The AAF and the CRF admitted (19136 kbit/s): class B's placeholder takes the rest of the
    // budget: idleSlope 730864, sendSlope -269136, hiCredit 1097, loCredit -415.
    std::array<TSpec, 2> const streams{
        AvbEntityKit::talker_tspec(aaf_8ch_48k_int24()), AvbEntityKit::talker_tspec(crf_one_timestamp())};
    auto const some = cbs_targets(streams, {}, 1'000'000, 1500);
    EXPECT_TRUE(some.class_a == class_a_cbs(streams, 1'000'000, 1500));
    EXPECT_FALSE(some.class_a.placeholder);
    EXPECT_TRUE(some.class_b.placeholder);
    EXPECT_EQ(some.class_b.idleslope_kbps, int32_t{730'864});
    EXPECT_EQ(some.class_b.sendslope_kbps, int32_t{-269'136});
    EXPECT_EQ(some.class_b.hicredit, int32_t{1097});
    EXPECT_EQ(some.class_b.locredit, int32_t{-415});
    EXPECT_EQ(some.class_a.idleslope_kbps + some.class_b.idleslope_kbps, int32_t{750'000});
    // At the kit's INT_32 bound class A alone exceeds the budget: class B's
    // placeholder falls to its 1% floor.
    auto aaf = aaf_8ch_48k_int24();
    aaf.format.channels = 32;
    aaf.format.aaf_format = avtp::AafFormat::int_32bit;
    std::vector<TSpec> wide(MAX_ENTITY_AUDIO_STREAMS, AvbEntityKit::talker_tspec(aaf));
    EXPECT_EQ(cbs_targets(wide, {}, 1'000'000, 1500).class_b.idleslope_kbps, int32_t{10'000});
}

#if defined(__linux__)
namespace {

/// A TcControl that records each request as `tc` would spell it and answers
/// from a canned root and canned errors.
class RecordingTc final : public net::tc::TcControl
{
  public:
    std::vector<std::string> requests{};
    std::optional<net::tc::QdiscInfo> root{};  ///< get(ROOT)'s answer (none: ENOENT)
    std::error_code mqprio_error{};            ///< replace_mqprio's first answer
    std::error_code cbs_error{};               ///< every replace_cbs's answer

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
        requests.push_back(
            std::format("replace {:x}:{:x} cbs idleslope {}", q.parent >> 16U, q.parent & 0xFFFFU, q.idleslope_kbps));
        if (cbs_error) {
            return failure(cbs_error);
        }
        return success();
    }
    auto replace_kind(int /*ifindex*/, uint32_t const parent, std::string_view const kind) -> Status override
    {
        requests.push_back(std::format("replace {:x}:{:x} {}", parent >> 16U, parent & 0xFFFFU, kind));
        return success();
    }
    auto remove(int /*ifindex*/, uint32_t const handle, uint32_t /*parent*/) -> Status override
    {
        requests.push_back(std::format("del root {:x}:", handle >> 16U));
        return success();
    }
    auto get(int /*ifindex*/, uint32_t const parent) -> StatusValue<net::tc::QdiscInfo> override
    {
        requests.push_back(parent == net::tc::ROOT ? std::string{"show root"} : std::format("show {:x}", parent));
        if (parent == net::tc::ROOT && root) {
            return *root;
        }
        return failure(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    [[nodiscard]] auto last_message() const noexcept -> std::string const& override { return message_; }

    /// The requests since the last take, cleared.
    auto take() -> std::vector<std::string> { return std::exchange(requests, {}); }

  private:
    std::string message_{};
};

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
    std::array<TSpec, 2> const streams{
        AvbEntityKit::talker_tspec(aaf_8ch_48k_int24()), AvbEntityKit::talker_tspec(crf_one_timestamp())};
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
    tc.root = net::tc::QdiscInfo{.handle = 0, .parent = net::tc::ROOT, .kind = "mq"};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.installed() && !qdiscs.reused());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.take() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "replace 9000:1 cbs idleslope 375000",
            "replace 9000:2 cbs idleslope 375000",
        }));
    EXPECT_TRUE(qdiscs.class_a().placeholder && qdiscs.class_b().placeholder);
    // Nothing changed: nothing sent.
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.take().empty());
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
    (void)tc.take();

    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.take() ==
        Requests({
            "replace 9000:1 cbs idleslope 19136",
            "replace 9000:2 cbs idleslope 730864",
        }));
    EXPECT_FALSE(qdiscs.class_a().placeholder);

    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.take() ==
        Requests({
            "replace 9000:1 cbs idleslope 375000",
            "replace 9000:2 cbs idleslope 375000",
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
    tc.root = net::tc::QdiscInfo{.handle = 0x9000'0000, .parent = net::tc::ROOT, .kind = "mqprio", .mqprio = igb_mqprio()};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.reused());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(
        tc.take() ==
        Requests({
            "show root",
            "replace 9000:1 cbs idleslope 375000",
            "replace 9000:2 cbs idleslope 375000",
        }));
}

// Our handle with another layout (a changed queue override) cannot be changed
// in place: mqprio answers EINVAL, the root is deleted and laid down again.
TEST(shaper_qdiscs, another_layout_under_our_handle_is_replaced)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    auto other = igb_mqprio();
    other.prio_tc_map[5] = CLASS_A_TC;
    tc.root = net::tc::QdiscInfo{.handle = 0x9000'0000, .parent = net::tc::ROOT, .kind = "mqprio", .mqprio = other};
    tc.mqprio_error = std::make_error_code(std::errc::invalid_argument);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_FALSE(qdiscs.reused());
    EXPECT_TRUE(
        tc.take() ==
        Requests({
            "show root",
            "replace root mqprio 9000:",
            "del root 9000:",
            "replace root mqprio 9000:",
        }));
    // And a foreign root of another handle is simply replaced.
    RecordingTc foreign{};
    foreign.root = net::tc::QdiscInfo{.handle = 0x0001'0000, .parent = net::tc::ROOT, .kind = "mqprio", .mqprio = igb_mqprio()};
    ShaperQdiscs taken{foreign, channel.logger()};
    EXPECT_TRUE(taken.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(foreign.take() == Requests({"show root", "replace root mqprio 9000:"}));
}

// A driver without cbs offload (EOPNOTSUPP): logged once, then no further
// cbs request; the mqprio steering stays, and uninstall has no cbs to take.
TEST(shaper_qdiscs, no_cbs_offload_stops_asking)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    tc.cbs_error = std::make_error_code(std::errc::operation_not_supported);
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "eth0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_FALSE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(qdiscs.cbs_unsupported());
    EXPECT_TRUE(tc.take() == Requests({"show root", "replace root mqprio 9000:", "replace 9000:1 cbs idleslope 375000"}));
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(tc.take().empty());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.take() == Requests({"replace 9000:2 pfifo_fast", "replace 9000:1 pfifo_fast", "del root 9000:"}));
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
    (void)tc.take();
    tc.cbs_error = {};
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());
    EXPECT_TRUE(tc.take() == Requests({"replace 9000:1 cbs idleslope 375000", "replace 9000:2 cbs idleslope 375000"}));
}

// Uninstall takes class B's cbs first, then class A's (igc's order), then the
// root, whatever was recorded (a kept root may hold a cbs this run never
// programmed); release leaves everything for the next start.
TEST(shaper_qdiscs, uninstall_and_release)
{
    logging::LogChannel<64> channel{logging::lit("test")};
    RecordingTc tc{};
    ShaperQdiscs qdiscs{tc, channel.logger()};
    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    (void)tc.take();
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(
        tc.take() ==
        Requests({
            "replace 9000:2 pfifo_fast",
            "replace 9000:1 pfifo_fast",
            "del root 9000:",
        }));
    EXPECT_FALSE(qdiscs.installed());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.take().empty());

    EXPECT_TRUE(qdiscs.install(3, "enp8s0", igb_mqprio(), igb_layout()).has_value());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(qdiscs.program(idle_targets(), 1000).has_value());  // what shutdown parks
    (void)tc.take();
    qdiscs.release();
    EXPECT_FALSE(qdiscs.installed());
    EXPECT_TRUE(qdiscs.program(admitted_targets(), 1000).has_value());
    EXPECT_TRUE(qdiscs.uninstall().has_value());
    EXPECT_TRUE(tc.take().empty());
}
#endif

TEST(shaper_errors, messages)
{
    EXPECT_TRUE(make_error_code(ShaperError::not_avb_capable).message().find("3 TX queues") != std::string::npos);
    EXPECT_EQ(std::string{shaper_error_category().name()}, std::string{"statusbar.avb_entity.shaper"});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_shaper_test)
