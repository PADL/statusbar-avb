// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_talker_advertises.hpp"

#include "statusbar/test/test.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

constexpr SavedTalkerAdvertise STREAM_1{
    .stream_id = 0x2ccf67fffe000001ULL,
    .dest_address = 0x91e0f0001234ULL,
    .vlan_id = 2,
    .max_frame_size = 416,
    .max_interval_frames = 1,
    .priority = 3};
constexpr SavedTalkerAdvertise STREAM_2{
    .stream_id = 0x2ccf67fffe000002ULL,
    .dest_address = 0x91e0f0001235ULL,
    .vlan_id = 100,
    .max_frame_size = 36,
    .max_interval_frames = 2,
    .priority = 2};

auto parses(std::string_view const text) -> bool
{
    return parse_talker_advertises(text).has_value();
}

}  // namespace

TEST(talker_advertises, the_text_is_the_magic_and_one_line_per_stream)
{
    SavedTalkerAdvertises const streams{STREAM_1, STREAM_2};
    EXPECT_TRUE(
        serialize_talker_advertises(streams) ==
        "statusbar-talker-advertises v1\n"
        "stream 2ccf67fffe000001 91e0f0001234 2 416 1 3\n"
        "stream 2ccf67fffe000002 91e0f0001235 100 36 2 2\n");
    EXPECT_TRUE(serialize_talker_advertises({}) == "statusbar-talker-advertises v1\n");
}

TEST(talker_advertises, what_was_written_reads_back)
{
    SavedTalkerAdvertises const streams{STREAM_1, STREAM_2};
    auto const read = parse_talker_advertises(serialize_talker_advertises(streams));
    EXPECT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), size_t{2});
    EXPECT_TRUE((*read)[0] == STREAM_1 && (*read)[1] == STREAM_2);
    auto const none = parse_talker_advertises("statusbar-talker-advertises v1\n");
    EXPECT_TRUE(none.has_value() && none->empty());
}

// IEEE 802.1Q 35.2.2.8.6: AccumulatedLatency is the one field a Talker may
// change in place, so it is no part of what is kept, and the parameters made
// from a saved line carry none.
TEST(talker_advertises, what_is_kept_of_a_stream_is_its_attribute_without_the_latency)
{
    nanoavb::TalkerStreamSrpInfo info{};
    info.stream_id.from_uint64(STREAM_1.stream_id);
    info.dest_address.from_uint64(STREAM_1.dest_address);
    info.vlan_id = 2;
    info.max_frame_size = 416;
    info.max_interval_frames = 1;
    info.priority = 3;
    info.accumulated_latency = 2'000'000;
    EXPECT_TRUE(saved_talker_advertise(info) == STREAM_1);

    auto const back = talker_srp_info(STREAM_1);
    EXPECT_EQ(back.stream_id.to_uint64(), STREAM_1.stream_id);
    EXPECT_EQ(back.dest_address.to_uint64(), STREAM_1.dest_address);
    EXPECT_EQ(back.vlan_id, 2);
    EXPECT_EQ(back.max_frame_size, 416);
    EXPECT_EQ(back.max_interval_frames, 1);
    EXPECT_EQ(back.priority, 3);
    EXPECT_EQ(back.accumulated_latency, 0U);
    EXPECT_TRUE(saved_talker_advertise(back) == STREAM_1);
}

TEST(talker_advertises, blank_lines_comments_and_a_carriage_return_are_passed_over)
{
    auto const read = parse_talker_advertises("# what this entity last declared\r\n\nstatusbar-talker-advertises v1\r\n\n"
                                              "# the audio stream\nstream 2ccf67fffe000001 91e0f0001234 2 416 1 3\r\n");
    EXPECT_TRUE(read.has_value() && read->size() == 1 && (*read)[0] == STREAM_1);
}

TEST(talker_advertises, a_text_that_is_not_the_file_is_refused_whole)
{
    EXPECT_FALSE(parses(""));
    EXPECT_FALSE(parses("stream 2ccf67fffe000001 91e0f0001234 2 416 1 3\n"));  // no magic
    EXPECT_FALSE(parses("statusbar-talker-advertises v2\n"));
    std::string const magic{"statusbar-talker-advertises v1\n"};
    EXPECT_FALSE(parses(magic + "stream 2ccf67fffe000001 91e0f0001234 2 416 1\n"));      // a number short
    EXPECT_FALSE(parses(magic + "stream 2ccf67fffe000001 91e0f0001234 2 416 1 3 9\n"));  // one too many
    EXPECT_FALSE(parses(magic + "stream 2ccf67fffe000001 91e0f0001234 2 x 1 3\n"));
    EXPECT_FALSE(parses(magic + "stream 2ccf67fffe000001 1291e0f0001234 2 416 1 3\n"));  // no 48 bit address
    EXPECT_FALSE(parses(magic + "stream 2ccf67fffe000001 91e0f0001234 70000 416 1 3\n"));
    EXPECT_FALSE(parses(magic + "streams 2ccf67fffe000001 91e0f0001234 2 416 1 3\n"));
    // One good line does not carry a bad one.
    EXPECT_FALSE(parses(magic + "stream 2ccf67fffe000001 91e0f0001234 2 416 1 3\nnonsense\n"));
}

TEST(talker_advertises, more_streams_than_an_entity_has_are_refused)
{
    std::string text{"statusbar-talker-advertises v1\n"};
    for (size_t i = 0; i < MAX_ENTITY_STREAMS; ++i) {
        text += "stream 2ccf67fffe000001 91e0f0001234 2 416 1 3\n";
    }
    EXPECT_TRUE(parses(text));
    EXPECT_FALSE(parses(text + "stream 2ccf67fffe000001 91e0f0001234 2 416 1 3\n"));
}

TEST(talker_advertises, a_file_that_is_not_there_is_nothing_saved)
{
    auto const loaded = load_talker_advertises("/no-such-directory-of-statusbar/talker-advertises");
    EXPECT_TRUE(loaded.has_value() && loaded->empty());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_advertises_test)
