// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_state_dir.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <filesystem>
#include <string>
#include <system_error>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

constexpr ieee::Eui64 ENTITY{0x2C, 0xCF, 0x67, 0xFF, 0xFE, 0x0A, 0x0B, 0x0C};

/// An empty directory of this test's own under the temporary directory.
auto fresh_dir(char const* const name) -> std::filesystem::path
{
    auto const dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST(avb_entity_state_dir, the_directory_name_is_the_entity_id_as_sixteen_hex_digits)
{
    EXPECT_EQ(entity_state_dir_name(ENTITY), std::string{"2ccf67fffe0a0b0c"});
    // Leading zeros are kept: every entity's name is sixteen digits long.
    EXPECT_EQ(entity_state_dir_name(ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0A}), std::string{"000102030405060a"});
    EXPECT_EQ(entity_state_dir_name(ieee::Eui64{}), std::string{"0000000000000000"});
}

TEST(avb_entity_state_dir, the_entity_directory_is_made_inside_a_state_directory_that_is_there)
{
    auto const state = fresh_dir("statusbar-state-dir-test");
    auto const made = ensure_entity_state_dir(state.string(), ENTITY);
    EXPECT_TRUE(made.has_value());
    EXPECT_EQ(*made, (state / "2ccf67fffe0a0b0c").string());
    EXPECT_TRUE(std::filesystem::is_directory(*made));

    // Asked again, the directory that is there is the answer.
    auto const again = ensure_entity_state_dir(state.string(), ENTITY);
    EXPECT_TRUE(again.has_value());
    EXPECT_EQ(*again, *made);

    // Another entity on the machine has its own.
    auto const other = ensure_entity_state_dir(state.string(), ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0A});
    EXPECT_TRUE(other.has_value());
    EXPECT_NE(*other, *made);
    std::filesystem::remove_all(state);
}

TEST(avb_entity_state_dir, nothing_is_made_where_the_state_directory_is_not_there)
{
    auto const state = fresh_dir("statusbar-state-dir-absent-test");
    std::filesystem::remove_all(state);
    auto const made = ensure_entity_state_dir(state.string(), ENTITY);
    EXPECT_FALSE(made.has_value());
    EXPECT_TRUE(made.error() == std::errc::no_such_file_or_directory);
    EXPECT_FALSE(std::filesystem::exists(state));

    // No state directory named is the same answer.
    EXPECT_TRUE(ensure_entity_state_dir(std::string{}, ENTITY).error() == std::errc::no_such_file_or_directory);
}

TEST(avb_entity_state_dir, a_file_of_a_kind_of_state_is_in_the_entity_directory)
{
    auto const dir = (std::filesystem::path{"/var/lib/statusbar-avb"} / "2ccf67fffe0a0b0c").string();
    EXPECT_EQ(
        entity_state_file_path(dir, entity_state_file::LISTENER_BINDINGS),
        std::string{"/var/lib/statusbar-avb/2ccf67fffe0a0b0c/listener-bindings"});
    EXPECT_EQ(
        entity_state_file_path(dir, entity_state_file::MAAP_RANGE),
        std::string{"/var/lib/statusbar-avb/2ccf67fffe0a0b0c/maap-range"});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_state_dir_test)
