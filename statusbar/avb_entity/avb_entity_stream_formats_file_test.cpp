// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_stream_formats_file.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <string>
#include <string_view>
#include <system_error>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

constexpr SavedStreamFormat INPUT_1{
    .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT, .descriptor_index = 1, .format = 0x020702200080C000ULL};
constexpr SavedStreamFormat OUTPUT_0{
    .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, .descriptor_index = 0, .format = 0x00A0040860000800ULL};

}  // namespace

TEST(stream_formats_file, the_text_is_the_magic_and_one_line_per_stream)
{
    std::array const formats{INPUT_1, OUTPUT_0};
    EXPECT_EQ(
        serialize_stream_formats(formats),
        std::string{"statusbar-stream-formats v1\ninput 1 020702200080c000\noutput 0 00a0040860000800\n"});
    EXPECT_EQ(serialize_stream_formats({}), std::string{"statusbar-stream-formats v1\n"});
}

TEST(stream_formats_file, what_was_written_reads_back)
{
    std::array const formats{INPUT_1, OUTPUT_0};
    auto const parsed = parse_stream_formats(serialize_stream_formats(formats));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{2});
    EXPECT_TRUE(parsed->at(0) == INPUT_1);
    EXPECT_TRUE(parsed->at(1) == OUTPUT_0);

    auto const none = parse_stream_formats("statusbar-stream-formats v1\n");
    EXPECT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
}

TEST(stream_formats_file, blank_lines_comments_and_a_carriage_return_are_passed_over)
{
    auto const parsed =
        parse_stream_formats("# saved formats\n\nstatusbar-stream-formats v1\r\n# one input\ninput 1 020702200080C000\r\n");
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{1});
    EXPECT_TRUE(parsed->at(0) == INPUT_1);
}

TEST(stream_formats_file, a_text_that_is_not_the_file_is_refused_whole)
{
    constexpr std::string_view MAGIC{"statusbar-stream-formats v1\n"};
    for (std::string_view const bad :
         {std::string_view{""},
          std::string_view{"input 1 020702200080c000\n"},           // no magic
          std::string_view{"statusbar-stream-formats v2\n"},        // another version
          std::string_view{"statusbar-listener-bindings v1\n"}}) {  // another kind of file
        EXPECT_EQ(parse_stream_formats(bad).error(), make_error_code(std::errc::illegal_byte_sequence));
    }
    for (std::string_view const line :
         {std::string_view{"sideways 1 020702200080c000\n"},
          std::string_view{"input one 020702200080c000\n"},
          std::string_view{"input 1\n"},
          std::string_view{"input 1 020702200080c000 extra\n"},
          std::string_view{"input 1 02070220zz80c000\n"},
          std::string_view{"input 70000 020702200080c000\n"}}) {
        auto const text = std::string{MAGIC} + std::string{line};
        EXPECT_EQ(parse_stream_formats(text).error(), make_error_code(std::errc::illegal_byte_sequence));
    }
}

TEST(stream_formats_file, more_lines_than_an_entity_has_streams_are_refused)
{
    std::string text{"statusbar-stream-formats v1\n"};
    for (size_t i = 0; i < SavedStreamFormats{}.capacity(); ++i) {
        text += "input 0 020702200080c000\n";
    }
    EXPECT_TRUE(parse_stream_formats(text).has_value());
    text += "input 0 020702200080c000\n";
    EXPECT_EQ(parse_stream_formats(text).error(), make_error_code(std::errc::result_out_of_range));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_formats_file_test)
