// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Persisted listener fast-connect bindings (kit phase 5b): the text
// serialize/parse round trip, loud rejection of corrupt files, and the
// file wrappers (missing file = empty table; save-then-load round trip).

#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"

#include "statusbar/test/test.hpp"

#include <cstdio>
#include <string>

using namespace statusbar;
using namespace statusbar::avb_entity;

TEST(listener_bindings, serialize_parse_round_trip)
{
    ListenerBindings bindings{};
    (void)bindings.try_push_back(ListenerBinding{
        .listener_unique_id = 0, .talker_entity_id = ieee::Eui64{}.from_uint64(0x0011223344556677ULL), .talker_unique_id = 2});
    (void)bindings.try_push_back(ListenerBinding{
        .listener_unique_id = 5, .talker_entity_id = ieee::Eui64{}.from_uint64(0xFFEEDDCCBBAA9988ULL), .talker_unique_id = 0});

    auto const text = serialize_listener_bindings({bindings.data(), bindings.size()});
    auto const parsed = parse_listener_bindings(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{2});
    EXPECT_EQ((*parsed)[0].listener_unique_id, uint16_t{0});
    EXPECT_EQ((*parsed)[0].talker_entity_id.to_uint64(), 0x0011223344556677ULL);
    EXPECT_EQ((*parsed)[0].talker_unique_id, uint16_t{2});
    EXPECT_EQ((*parsed)[1].listener_unique_id, uint16_t{5});
    EXPECT_EQ((*parsed)[1].talker_entity_id.to_uint64(), 0xFFEEDDCCBBAA9988ULL);
}

TEST(listener_bindings, comments_and_blank_lines_ignored)
{
    std::string const text = std::string{LISTENER_BINDINGS_MAGIC} +
        "\n"
        "# a comment\n"
        "\n"
        "1 00000000000000ab 3\n";
    auto const parsed = parse_listener_bindings(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{1});
    EXPECT_EQ((*parsed)[0].talker_entity_id.to_uint64(), 0xABULL);
    EXPECT_EQ((*parsed)[0].talker_unique_id, uint16_t{3});
}

TEST(listener_bindings, corrupt_files_rejected_loudly)
{
    // Wrong magic.
    EXPECT_TRUE(!parse_listener_bindings("something-else v9\n1 00000000000000ab 3\n").has_value());
    // Empty file (no magic).
    EXPECT_TRUE(!parse_listener_bindings("").has_value());
    // Malformed line (missing fields).
    EXPECT_TRUE(!parse_listener_bindings(std::string{LISTENER_BINDINGS_MAGIC} + "\n1 00000000000000ab\n").has_value());
    // Trailing junk on a line.
    EXPECT_TRUE(!parse_listener_bindings(std::string{LISTENER_BINDINGS_MAGIC} + "\n1 00000000000000ab 3 junk\n").has_value());
}

TEST(listener_bindings, file_round_trip_and_missing_file_is_empty)
{
    std::string const path = "/tmp/statusbar_listener_bindings_test.txt";
    (void)std::remove(path.c_str());

    // Missing file: empty table, not an error (first boot).
    auto const empty = load_listener_bindings(path);
    EXPECT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());

    ListenerBindings bindings{};
    (void)bindings.try_push_back(ListenerBinding{
        .listener_unique_id = 2, .talker_entity_id = ieee::Eui64{}.from_uint64(0x1122334455667788ULL), .talker_unique_id = 1});
    EXPECT_TRUE(save_listener_bindings(path, {bindings.data(), bindings.size()}).has_value());

    auto const loaded = load_listener_bindings(path);
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), size_t{1});
    EXPECT_EQ((*loaded)[0].listener_unique_id, uint16_t{2});
    EXPECT_EQ((*loaded)[0].talker_entity_id.to_uint64(), 0x1122334455667788ULL);
    (void)std::remove(path.c_str());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_bindings_test)
