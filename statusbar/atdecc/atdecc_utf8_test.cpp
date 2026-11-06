// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the UTF-8 well-formedness of ATDECC strings (RFC 3629 Section 4)

#include "statusbar/atdecc/atdecc_utf8.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <string_view>

using namespace statusbar::atdecc;

TEST(atdecc_utf8, each_sequence_length_is_read_from_its_lead_octet)
{
    EXPECT_EQ(utf8_sequence_length("a"), size_t{1});
    EXPECT_EQ(utf8_sequence_length("\xC3\xA9"), size_t{2});
    EXPECT_EQ(utf8_sequence_length("\xE2\x82\xAC"), size_t{3});
    EXPECT_EQ(utf8_sequence_length("\xF0\x9F\x8E\xB5"), size_t{4});
    EXPECT_EQ(utf8_sequence_length(""), size_t{0});
}

TEST(atdecc_utf8, malformed_sequences_have_no_length)
{
    EXPECT_EQ(utf8_sequence_length("\x80"), size_t{0});              // a lone continuation octet
    EXPECT_EQ(utf8_sequence_length("\xC3"), size_t{0});              // cut
    EXPECT_EQ(utf8_sequence_length("\xE2\x82"), size_t{0});          // cut
    EXPECT_EQ(utf8_sequence_length("\xC0\xAF"), size_t{0});          // overlong
    EXPECT_EQ(utf8_sequence_length("\xE0\x80\xAF"), size_t{0});      // overlong
    EXPECT_EQ(utf8_sequence_length("\xED\xA0\x80"), size_t{0});      // a surrogate
    EXPECT_EQ(utf8_sequence_length("\xF4\x90\x80\x80"), size_t{0});  // past U+10FFFF
    EXPECT_EQ(utf8_sequence_length("\xF5\x80\x80\x80"), size_t{0});
}

TEST(atdecc_utf8, the_prefix_ends_at_the_first_malformed_sequence)
{
    EXPECT_EQ(utf8_prefix("Caf\xC3\xA9 \x80 sale"), std::string_view{"Caf\xC3\xA9 "});
    EXPECT_EQ(utf8_prefix("A\xE2\x82"), std::string_view{"A"});
    EXPECT_EQ(utf8_prefix("\xED\xA0\x80x"), std::string_view{});
    EXPECT_EQ(utf8_prefix("\xF0\x9F\x8E\xB5 ok"), std::string_view{"\xF0\x9F\x8E\xB5 ok"});
    EXPECT_TRUE(utf8_well_formed("\xF0\x9F\x8E\xB5 ok"));
    EXPECT_TRUE(utf8_well_formed(""));
    EXPECT_FALSE(utf8_well_formed("Caf\xC3\xA9 \x80 sale"));
}

TEST(atdecc_utf8, an_atdecc_string_is_read_to_its_first_nul)
{
    std::array<char, 64> field{};
    field[0] = 'o';
    field[1] = 'k';
    field[3] = 'x';  // past the NUL: not the string's
    EXPECT_EQ(atdecc_string_text(field), std::string_view{"ok"});
    std::array<char, 4> const full{'a', 'b', 'c', 'd'};
    EXPECT_EQ(atdecc_string_text(full), std::string_view{"abcd"});
}

TEST_MAIN(statusbar_atdecc, atdecc_utf8_test)
