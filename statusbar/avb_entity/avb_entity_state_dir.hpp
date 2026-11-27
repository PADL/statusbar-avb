#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Where an entity keeps what must outlast a restart: its power-on state.
///
/// A machine has one state directory, which the installation makes
/// (/var/lib/statusbar-avb from the package). Each entity keeps its own in a
/// directory inside it named by its entity ID, sixteen lowercase hex digits
/// with no separators, so entities of different programs on one machine
/// never share a file:
///
///     <state.dir>/<entity ID>/clock-sources
///     <state.dir>/<entity ID>/listener-bindings
///     <state.dir>/<entity ID>/maap-range
///     <state.dir>/<entity ID>/stream-formats
///
/// One file per kind of saved state; a new kind is a new file name here.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

/// The state directory of the installed tools; the package creates it.
inline constexpr std::string_view DEFAULT_ENTITY_STATE_DIR{"/var/lib/statusbar-avb"};

/// The files of an entity's state directory, one per kind of saved state.
namespace entity_state_file {
/// The clock domains' chosen clock sources (Milan v1.3 5.3.11.1), in the form avb_entity_clock_sources.hpp reads.
inline constexpr std::string_view CLOCK_SOURCES{"clock-sources"};
/// The stream inputs' bindings (Milan v1.3 5.3.8.3), in the form avb_entity_listener_bindings.hpp reads.
inline constexpr std::string_view LISTENER_BINDINGS{"listener-bindings"};
/// The MAAP block last defended, probed first at the next start.
inline constexpr std::string_view MAAP_RANGE{"maap-range"};
/// The streams' current formats (Milan v1.3 5.3.7.1, 5.3.8.1), in the form avb_entity_stream_formats_file.hpp reads.
inline constexpr std::string_view STREAM_FORMATS{"stream-formats"};
}  // namespace entity_state_file

/// The name of an entity's own directory: its entity ID as sixteen lowercase hex digits.
[[nodiscard]] inline auto entity_state_dir_name(ieee::Eui64 const& entity_id) -> std::string
{
    return std::format("{:016x}", entity_id.to_uint64());
}

/// @p entity_id's directory inside @p state_dir, named and not made: where
/// the entity's state is when it has saved any.
[[nodiscard]] inline auto entity_state_dir_path(std::string const& state_dir, ieee::Eui64 const& entity_id) -> std::string
{
    return (std::filesystem::path{state_dir} / entity_state_dir_name(entity_id)).string();
}

/// @p entity_id's directory inside @p state_dir, made when it is not there yet.
/// @p state_dir itself is the installation's to make: with none there this
/// fails with no_such_file_or_directory and makes nothing. Other failures are
/// the directory's own (permission_denied for one the caller may not write).
[[nodiscard]] inline auto ensure_entity_state_dir(std::string const& state_dir, ieee::Eui64 const& entity_id)
    -> StatusValue<std::string>
{
    std::error_code error;
    if (state_dir.empty() || !std::filesystem::is_directory(state_dir, error)) {
        return failure(std::errc::no_such_file_or_directory);
    }
    auto const dir = std::filesystem::path{state_dir} / entity_state_dir_name(entity_id);
    std::filesystem::create_directory(dir, error);
    if (error) {
        return failure(error);
    }
    return dir.string();
}

/// The path of the file @p name in the entity's directory @p entity_dir.
[[nodiscard]] inline auto entity_state_file_path(std::string const& entity_dir, std::string_view const name) -> std::string
{
    return (std::filesystem::path{entity_dir} / name).string();
}

}  // namespace statusbar::avb_entity
