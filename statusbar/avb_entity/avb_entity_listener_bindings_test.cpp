// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Persisted listener bindings (kit phase 5b): the text serialize/parse
// round trip of both forms, each read by either listener, loud rejection of
// corrupt files, and the file wrappers (missing file = empty table;
// save-then-load round trip).

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

//
// The v2 form: a Milan listener's binding parameters (Milan v1.3 5.3.8.3)
//

namespace {

auto milan_binding(uint16_t const sink, uint64_t const talker, uint16_t const source, uint64_t const controller, bool const wait)
    -> SavedListenerBinding
{
    return SavedListenerBinding{
        .listener_unique_id = sink,
        .talker_entity_id = ieee::Eui64{}.from_uint64(talker),
        .talker_unique_id = source,
        .controller_entity_id = ieee::Eui64{}.from_uint64(controller),
        .streaming_wait = wait};
}

}  // namespace

TEST(listener_bindings, a_milan_binding_round_trips_with_its_controller_and_streaming_wait)
{
    SavedListenerBindings bindings{};
    bindings.push_back(milan_binding(0, 0x0011223344556677ULL, 2, 0x8899AABBCCDDEEFFULL, true));
    bindings.push_back(milan_binding(5, 0xFFEEDDCCBBAA9988ULL, 0, 0x0000000000000001ULL, false));
    auto const text = serialize_milan_listener_bindings({bindings.data(), bindings.size()});
    EXPECT_EQ(
        text,
        std::string{"statusbar-listener-bindings v2\n"
                    "0 0011223344556677 2 8899aabbccddeeff 1\n"
                    "5 ffeeddccbbaa9988 0 0000000000000001 0\n"});
    auto const parsed = parse_saved_listener_bindings(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{2});
    EXPECT_TRUE((*parsed)[0] == bindings[0]);
    EXPECT_TRUE((*parsed)[1] == bindings[1]);
}

TEST(listener_bindings, a_file_the_ieee_listener_wrote_gives_a_milan_listener_its_bindings)
{
    // An entity moved to the Milan profile keeps what it was connected to:
    // the binding has no controller and STREAMING_WAIT clear.
    std::string const text = std::string{LISTENER_BINDINGS_MAGIC} + "\n1 00000000000000ab 3\n";
    auto const parsed = parse_saved_listener_bindings(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{1});
    EXPECT_TRUE((*parsed)[0] == milan_binding(1, 0xAB, 3, 0, false));
}

TEST(listener_bindings, a_file_the_milan_listener_wrote_gives_the_ieee_listener_its_goals)
{
    std::string const text = std::string{LISTENER_BINDINGS_MAGIC_V2} + "\n# moved back\n1 00000000000000ab 3 00000000000000cd 1\n";
    auto const parsed = parse_listener_bindings(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), size_t{1});
    EXPECT_EQ((*parsed)[0].listener_unique_id, uint16_t{1});
    EXPECT_EQ((*parsed)[0].talker_entity_id.to_uint64(), 0xABULL);
    EXPECT_EQ((*parsed)[0].talker_unique_id, uint16_t{3});
}

TEST(listener_bindings, a_line_must_be_of_its_files_version)
{
    std::string const v1{LISTENER_BINDINGS_MAGIC};
    std::string const v2{LISTENER_BINDINGS_MAGIC_V2};
    // Five fields under the v1 magic, three under the v2.
    EXPECT_TRUE(!parse_saved_listener_bindings(v1 + "\n1 00000000000000ab 3 00000000000000cd 1\n").has_value());
    EXPECT_TRUE(!parse_saved_listener_bindings(v2 + "\n1 00000000000000ab 3\n").has_value());
    // The fourth field missing its fifth, STREAMING_WAIT other than 0 or 1, junk after it.
    EXPECT_TRUE(!parse_saved_listener_bindings(v2 + "\n1 00000000000000ab 3 00000000000000cd\n").has_value());
    EXPECT_TRUE(!parse_saved_listener_bindings(v2 + "\n1 00000000000000ab 3 00000000000000cd 2\n").has_value());
    EXPECT_TRUE(!parse_saved_listener_bindings(v2 + "\n1 00000000000000ab 3 00000000000000cd 1 junk\n").has_value());
    EXPECT_TRUE(!parse_saved_listener_bindings("statusbar-listener-bindings v3\n").has_value());
    // The magic alone is a file with no bindings.
    EXPECT_TRUE(parse_saved_listener_bindings(v2 + "\n")->empty());
}

// The IEEE listener keeps a fast-connect goal for each of its 32 streams, as many as Milan sinks.
TEST(listener_bindings, a_binding_for_every_stream_is_read_for_both_listeners)
{
    SavedListenerBindings bindings{};
    for (uint16_t sink = 0; sink < nanoavb::NanoAvbMilanAcmpListener::MAX_STREAMS; ++sink) {
        bindings.push_back(milan_binding(sink, 0xAB, 0, 0xCD, false));
    }
    auto const text = serialize_milan_listener_bindings({bindings.data(), bindings.size()});
    EXPECT_EQ(parse_saved_listener_bindings(text)->size(), size_t{32});
    EXPECT_EQ(parse_listener_bindings(text)->size(), size_t{32});
}

TEST(listener_bindings, a_milan_file_round_trips_and_a_missing_one_is_empty)
{
    std::string const path = "/tmp/statusbar_milan_listener_bindings_test.txt";
    (void)std::remove(path.c_str());
    auto const empty = load_saved_listener_bindings(path);
    EXPECT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());

    SavedListenerBindings bindings{};
    bindings.push_back(milan_binding(2, 0x1122334455667788ULL, 1, 0x99AABBCCDDEEFF00ULL, true));
    EXPECT_TRUE(save_milan_listener_bindings(path, {bindings.data(), bindings.size()}).has_value());
    auto const loaded = load_saved_listener_bindings(path);
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->size(), size_t{1});
    EXPECT_TRUE((*loaded)[0] == bindings[0]);
    (void)std::remove(path.c_str());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_bindings_test)
