// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The AVB entity's persistent state file: the text round trip, malformed
// files, the atomic write, and the load outcomes (corrupt file moved aside,
// another model's file discarded, the old listener bindings file imported).

#include "statusbar/avb_entity/avb_entity_persistent_state.hpp"

#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

/// A fresh, empty directory for one test
auto temp_dir(std::string const& name) -> std::filesystem::path
{
    auto dir = std::filesystem::temp_directory_path() / ("avb-state-test-" + std::to_string(::getpid()) + "-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

auto read_text(std::filesystem::path const& path) -> std::string
{
    std::ifstream in{path};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void write_text(std::filesystem::path const& path, std::string const& text)
{
    std::ofstream out{path};
    out << text;
}

constexpr uint64_t MODEL = 0x001B92FFFE000001ULL;

auto sample_state() -> PersistentState
{
    PersistentState state{};
    state.entity_model_id = MODEL;
    state.configured_entity_name = "Bridge";
    state.configured_presentation_offset_ns = 2000000;
    state.names.push_back({.descriptor_type = 0, .descriptor_index = 0, .name_index = 0, .value = "FOH \"left\" \\ bridge\t\xC3\xA9"});
    state.names.push_back({.descriptor_type = 0x1A, .descriptor_index = 3, .name_index = 0, .value = std::string(64, 'x')});
    state.stream_formats.push_back({.descriptor_type = 5, .descriptor_index = 1, .format = 0x0205022002006000ULL});
    state.clock_sources.push_back({.clock_domain = 0, .clock_source = 1});
    state.presentation_offsets.push_back({.stream_output = 2, .ns = 1500000});
    state.listener_bindings.push_back({.stream_input = 0, .talker_entity_id = 0xFFEEDDCCBBAA9988ULL, .talker_unique_id = 3, .format = 0x0205022002006000ULL});
    state.listener_bindings.push_back({.stream_input = 1, .talker_entity_id = 0x01, .talker_unique_id = 0});
    // A Milan input's binding parameters: its controller, stopped
    state.listener_bindings.push_back(
        {.stream_input = 2, .talker_entity_id = 0x02, .talker_unique_id = 1, .controller_entity_id = 0x001B92FFFE000003ULL, .started = false});
    state.system_unique_id = 0xDEADBEEF;
    state.media_clock_references.push_back({.clock_domain = 0, .user_priority = 200, .domain_name = "Stage"});
    // A port's dynamic mappings (one swapped, one fanned out), and a
    // port with every mapping removed.
    state.audio_maps.push_back(
        {.descriptor_type = 14,
         .descriptor_index = 0,
         .mappings = {
             {.stream_index = 0, .stream_channel = 1, .cluster_offset = 0, .cluster_channel = 0},
             {.stream_index = 0, .stream_channel = 0, .cluster_offset = 1, .cluster_channel = 0},
             {.stream_index = 0, .stream_channel = 0, .cluster_offset = 2, .cluster_channel = 0}}});
    state.audio_maps.push_back({.descriptor_type = 14, .descriptor_index = 1, .mappings = {}});
    return state;
}

}  // namespace

TEST(avb_persistent_state, round_trip)
{
    auto const state = sample_state();
    auto const text = serialize_persistent_state(state);
    auto const parsed = parse_persistent_state(text);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(*parsed == state);

    // The empty state too (no system unique id line, no tables)
    PersistentState empty{};
    empty.entity_model_id = MODEL;
    auto const reparsed = parse_persistent_state(serialize_persistent_state(empty));
    EXPECT_TRUE(reparsed.has_value());
    EXPECT_TRUE(*reparsed == empty);
}

// Unknown keys are ignored (a later version's additions); a missing version
// or model id, a mistyped or out-of-range value, an over-long name, or TOML
// that does not parse are malformed; another version is not supported
TEST(avb_persistent_state, malformed_rejected)
{
    std::string const head = "version = 1\nentity_model_id = \"0x001b92fffe000001\"\n";
    auto const extra = parse_persistent_state(head + "future = 3\n[[listener_binding]]\nstream_input = 1\n"
                                                     "talker_entity_id = \"0x01\"\ntalker_unique_id = 0\nstarted = true\n");
    EXPECT_TRUE(extra.has_value());
    EXPECT_EQ(extra->listener_bindings.size(), size_t{1});

    auto const is_malformed = [](std::string const& text) {
        auto const r = parse_persistent_state(text);
        return !r.has_value() && r.error() == std::errc::illegal_byte_sequence;
    };
    EXPECT_TRUE(is_malformed("entity_model_id = \"0x01\"\n"));
    EXPECT_TRUE(is_malformed("version = 1\n"));
    EXPECT_TRUE(is_malformed("version = 1\nentity_model_id = 5\n"));
    EXPECT_TRUE(is_malformed(head + "system_unique_id = -1\n"));
    EXPECT_TRUE(is_malformed(head + "[[clock_source]]\nclock_domain = 0\n"));
    EXPECT_TRUE(is_malformed(head + "[[name]]\ndescriptor_type = 0\ndescriptor_index = 0\nname_index = 0\nvalue = \"" +
                             std::string(65, 'n') + "\"\n"));
    EXPECT_TRUE(is_malformed(head + "[[media_clock_reference]]\nclock_domain = 0\nuser_priority = 256\ndomain_name = \"\"\n"));
    EXPECT_TRUE(is_malformed(head + "name = 3\n"));
    EXPECT_TRUE(is_malformed(head + "[[listener_binding]]\nstream_input = 1\ntalker_entity_id = \"0x01\"\ntalker_unique_id = 0\n"
                                    "started = 1\n"));
    EXPECT_TRUE(is_malformed(head + "[[listener_binding]]\nstream_input = 1\ntalker_entity_id = \"0x01\"\ntalker_unique_id = 0\n"
                                    "controller_entity_id = 3\n"));
    EXPECT_TRUE(is_malformed(head + "[[audio_map]]\ndescriptor_type = 14\ndescriptor_index = 0\nmappings = [[0, 1, 0]]\n"));
    EXPECT_TRUE(is_malformed(head + "[[audio_map]]\ndescriptor_type = 14\ndescriptor_index = 0\nmappings = [[0, 1, 0, -1]]\n"));
    EXPECT_TRUE(is_malformed(head + "[[audio_map]]\ndescriptor_type = 14\ndescriptor_index = 0\n"));
    EXPECT_TRUE(is_malformed("version = = 1\n"));
    EXPECT_TRUE(is_malformed(""));

    auto const v2 = parse_persistent_state("version = 2\nentity_model_id = \"0x01\"\n");
    EXPECT_FALSE(v2.has_value());
    EXPECT_TRUE(v2.error() == std::errc::not_supported);
}

// An older file's [[sampling_rate]] tables load and are ignored: the kit keeps
// no unit rate, and the rewritten file has none
TEST(avb_persistent_state, a_saved_sampling_rate_is_ignored)
{
    std::string const text = "version = 1\nentity_model_id = \"0x001b92fffe000001\"\n"
                             "[[sampling_rate]]\naudio_unit = 0\nrate = 96000\n"
                             "[[clock_source]]\nclock_domain = 0\nclock_source = 1\n";
    auto const parsed = parse_persistent_state(text);
    EXPECT_TRUE(parsed.has_value());
    if (parsed) {
        EXPECT_EQ(parsed->clock_sources.size(), size_t{1});
        EXPECT_EQ(serialize_persistent_state(*parsed).find("sampling_rate"), std::string::npos);
    }
}

// The write is whole and atomic: a temporary file beside the target renamed
// over it, none left behind; a missing state directory is created
TEST(avb_persistent_state, atomic_write)
{
    auto const dir = temp_dir("atomic");
    auto const path = (dir / "state" / "avb-state.toml").string();
    auto state = sample_state();
    EXPECT_TRUE(save_persistent_state(path, state).has_value());
    EXPECT_TRUE(read_text(path) == serialize_persistent_state(state));

    // A stale temporary file (a write cut short) is replaced, and the new
    // state replaces the old whole
    write_text(path + ".tmp", "torn");
    state.names.clear();
    state.system_unique_id = 7;
    EXPECT_TRUE(save_persistent_state(path, state).has_value());
    EXPECT_TRUE(read_text(path) == serialize_persistent_state(state));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    auto const loaded = load_persistent_state(path, MODEL, {});
    EXPECT_TRUE(loaded.outcome == PersistentStateOutcome::loaded);
    EXPECT_TRUE(loaded.state == state);

    // An unwritable place fails and leaves the old file alone
    EXPECT_FALSE(save_persistent_state((dir / "no" / "such" / "dir" / "f.toml").string(), state).has_value());
    std::filesystem::remove_all(dir);
}

// A later version's file is left alone (a downgrade must not destroy it)
TEST(avb_persistent_state, newer_version_left_alone)
{
    auto const dir = temp_dir("newer");
    auto const path = (dir / "avb-state.toml").string();
    std::string const text = "version = 2\nentity_model_id = \"0x001b92fffe000001\"\n";
    write_text(path, text);
    auto const loaded = load_persistent_state(path, MODEL, {});
    EXPECT_TRUE(loaded.outcome == PersistentStateOutcome::newer_version);
    EXPECT_TRUE(read_text(path) == text);
    EXPECT_FALSE(std::filesystem::exists(path + ".bad"));
    std::filesystem::remove_all(dir);
}

TEST(avb_persistent_state, missing_file_is_fresh)
{
    auto const dir = temp_dir("fresh");
    auto const loaded = load_persistent_state((dir / "avb-state.toml").string(), MODEL, (dir / "no-bindings").string());
    EXPECT_TRUE(loaded.outcome == PersistentStateOutcome::fresh);
    EXPECT_EQ(loaded.state.entity_model_id, MODEL);
    EXPECT_TRUE(loaded.state.names.empty());
    EXPECT_TRUE(loaded.state.listener_bindings.empty());
    std::filesystem::remove_all(dir);
}

// A corrupt file is moved to <path>.bad and the entity starts fresh
TEST(avb_persistent_state, corrupt_file_kept_aside)
{
    auto const dir = temp_dir("corrupt");
    auto const path = (dir / "avb-state.toml").string();
    write_text(path, "version = 1\nentity_model_id = [\n");
    auto const loaded = load_persistent_state(path, MODEL, {});
    EXPECT_TRUE(loaded.outcome == PersistentStateOutcome::corrupt);
    EXPECT_TRUE(loaded.error == std::errc::illegal_byte_sequence);
    EXPECT_TRUE(loaded.bad_copy == path + ".bad");
    EXPECT_TRUE(read_text(path + ".bad") == "version = 1\nentity_model_id = [\n");
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(loaded.state.entity_model_id, MODEL);
    EXPECT_TRUE(loaded.state.names.empty());
    std::filesystem::remove_all(dir);
}

// A file written for another entity model is discarded (Milan v1.2 Section
// 5.5.1.4)
TEST(avb_persistent_state, model_change_discards)
{
    auto const dir = temp_dir("model");
    auto const path = (dir / "avb-state.toml").string();
    EXPECT_TRUE(save_persistent_state(path, sample_state()).has_value());
    auto const loaded = load_persistent_state(path, MODEL + 1, {});
    EXPECT_TRUE(loaded.outcome == PersistentStateOutcome::model_changed);
    EXPECT_EQ(loaded.discarded_model_id, MODEL);
    EXPECT_EQ(loaded.state.entity_model_id, MODEL + 1);
    EXPECT_TRUE(loaded.state.names.empty());
    EXPECT_TRUE(loaded.state.listener_bindings.empty());
    EXPECT_EQ(loaded.state.system_unique_id, uint32_t{0});
    std::filesystem::remove_all(dir);
}

// No state file yet: the old listener bindings file's bindings are imported
// (once: with the state file present, the old file is not read)
TEST(avb_persistent_state, bindings_imported_once)
{
    auto const dir = temp_dir("import");
    auto const path = (dir / "avb-state.toml").string();
    auto const legacy = (dir / "avb-bindings").string();
    write_text(legacy, std::string{LISTENER_BINDINGS_MAGIC} + "\n0 0011223344556677 2\n3 ffeeddccbbaa9988 0\n");
    auto const imported = load_persistent_state(path, MODEL, legacy);
    EXPECT_TRUE(imported.outcome == PersistentStateOutcome::imported);
    EXPECT_EQ(imported.state.listener_bindings.size(), size_t{2});
    EXPECT_TRUE(
        (imported.state.listener_bindings[0] ==
         PersistedListenerBinding{.stream_input = 0, .talker_entity_id = 0x0011223344556677ULL, .talker_unique_id = 2}));
    EXPECT_TRUE(
        (imported.state.listener_bindings[1] ==
         PersistedListenerBinding{.stream_input = 3, .talker_entity_id = 0xFFEEDDCCBBAA9988ULL, .talker_unique_id = 0}));

    EXPECT_TRUE(save_persistent_state(path, imported.state).has_value());
    write_text(legacy, std::string{LISTENER_BINDINGS_MAGIC} + "\n1 0000000000000001 1\n");
    auto const reloaded = load_persistent_state(path, MODEL, legacy);
    EXPECT_TRUE(reloaded.outcome == PersistentStateOutcome::loaded);
    EXPECT_TRUE(reloaded.state.listener_bindings == imported.state.listener_bindings);

    // An empty or corrupt old file imports nothing
    std::filesystem::remove(path);
    write_text(legacy, "garbage\n");
    EXPECT_TRUE(load_persistent_state(path, MODEL, legacy).outcome == PersistentStateOutcome::fresh);
    std::filesystem::remove_all(dir);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_persistent_state_test)
