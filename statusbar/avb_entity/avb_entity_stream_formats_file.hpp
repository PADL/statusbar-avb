#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The saved current formats of an entity's streams.
///
/// Milan v1.3 5.3.7.1 and 5.3.8.1 say of a stream output and of a stream
/// input: "The current format shall be saved in a non-volatile memory and
/// restored after a power cycle." This is the file that holds them, one of
/// the kinds in an entity's state directory (avb_entity_state_dir.hpp).
///
/// Format (blank lines and '#' comments ignored):
///
///   statusbar-stream-formats v1
///   <input|output> <stream index> <format as 16 hex digits>
///
/// A stream with no line has the format its model authors. The parse and
/// serialize pair is pure (strings only) and unit-tested; load and save do
/// the file I/O and are called from the reactor thread only.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

/// One line of the file: a stream and the format it was using.
struct SavedStreamFormat
{
    uint16_t descriptor_type{atdecc::aem::DESCRIPTOR_STREAM_INPUT};  ///< DESCRIPTOR_STREAM_INPUT or DESCRIPTOR_STREAM_OUTPUT
    uint16_t descriptor_index{0};
    uint64_t format{0};

    auto operator<=>(SavedStreamFormat const&) const noexcept = default;
};

/// Every stream of an entity, both directions.
using SavedStreamFormats = sg14::inplace_vector<SavedStreamFormat, 2 * MAX_ENTITY_STREAMS>;

inline constexpr std::string_view STREAM_FORMATS_MAGIC = "statusbar-stream-formats v1";

namespace stream_formats_detail {

inline constexpr std::string_view INPUT_WORD = "input ";
inline constexpr std::string_view OUTPUT_WORD = "output ";

/// Take the direction word off the front of @p line: the descriptor type it names, nullopt when it is neither.
[[nodiscard]] inline auto take_direction(std::string_view& line) -> std::optional<uint16_t>
{
    bool const input = line.starts_with(INPUT_WORD);
    if (!input && !line.starts_with(OUTPUT_WORD)) {
        return std::nullopt;
    }
    line.remove_prefix(input ? INPUT_WORD.size() : OUTPUT_WORD.size());
    return input ? atdecc::aem::DESCRIPTOR_STREAM_INPUT : atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
}

/// One stream line, nullopt when malformed.
[[nodiscard]] inline auto parse_line(std::string_view line) -> std::optional<SavedStreamFormat>
{
    SavedStreamFormat saved{};
    auto const direction = take_direction(line);
    bool const ok = direction.has_value() && listener_bindings_detail::take_number(line, saved.descriptor_index, 10, false) &&
        listener_bindings_detail::take_number(line, saved.format, 16, true);
    saved.descriptor_type = direction.value_or(saved.descriptor_type);
    return ok ? std::optional{saved} : std::nullopt;
}

/// Take one line that is neither blank nor a comment: the magic first
/// (setting @p seen_magic), a stream after it. errc{} when the line was good.
[[nodiscard]] inline auto take_content_line(std::string_view const line, bool& seen_magic, SavedStreamFormats& formats) -> std::errc
{
    if (!seen_magic) {
        seen_magic = line == STREAM_FORMATS_MAGIC;
        return seen_magic ? std::errc{} : std::errc::illegal_byte_sequence;
    }
    auto const saved = parse_line(line);
    if (!saved.has_value()) {
        return std::errc::illegal_byte_sequence;
    }
    return formats.try_push_back(*saved) != nullptr ? std::errc{} : std::errc::result_out_of_range;
}

}  // namespace stream_formats_detail

/// Serialize @p formats to the form of the file.
[[nodiscard]] inline auto serialize_stream_formats(std::span<SavedStreamFormat const> const formats) -> std::string
{
    std::string out{STREAM_FORMATS_MAGIC};
    out.push_back('\n');
    for (auto const& saved : formats) {
        char line[64];
        auto const n = std::snprintf(
            line,
            sizeof(line),
            "%s %u %016llx\n",
            saved.descriptor_type == atdecc::aem::DESCRIPTOR_STREAM_OUTPUT ? "output" : "input",
            static_cast<unsigned>(saved.descriptor_index),
            static_cast<unsigned long long>(saved.format));
        listener_bindings_detail::append_line(out, line, n);
    }
    return out;
}

/// Parse the file's text. An unknown magic, a malformed line or more lines
/// than an entity has streams fail the whole of it: a corrupt file should be
/// noticed, not half honoured.
[[nodiscard]] inline auto parse_stream_formats(std::string_view text) -> StatusValue<SavedStreamFormats>
{
    SavedStreamFormats formats{};
    bool seen_magic = false;
    while (!text.empty()) {
        auto const line = listener_bindings_detail::take_line(text);
        bool const content = !line.empty() && line.front() != '#';
        auto const verdict = content ? stream_formats_detail::take_content_line(line, seen_magic, formats) : std::errc{};
        if (verdict != std::errc{}) {
            return failure(verdict);
        }
    }
    if (!seen_magic) {
        return failure(std::errc::illegal_byte_sequence);
    }
    return success(formats);
}

/// Load the formats saved in @p path. A missing file is no saved format (a
/// first start), not an error; a file that is there and corrupt is one.
[[nodiscard]] inline auto load_stream_formats(std::string const& path) -> StatusValue<SavedStreamFormats>
{
    auto const text = listener_bindings_detail::read_file(path);
    return text.has_value() ? parse_stream_formats(*text) : success(SavedStreamFormats{});
}

/// Save @p formats to @p path, atomically.
[[nodiscard]] inline auto save_stream_formats(std::string const& path, std::span<SavedStreamFormat const> const formats) -> Status
{
    return listener_bindings_detail::save_text(path, serialize_stream_formats(formats));
}

}  // namespace statusbar::avb_entity
