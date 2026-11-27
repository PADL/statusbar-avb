#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_clock_sources.hpp
/// @brief What a model's CLOCK_SOURCE descriptors are to an entity, and the
///        file its chosen clock sources are saved in.
///
/// A clock domain lists the clock sources a controller may choose between
/// (IEEE 1722.1-2021 7.2.9, 7.2.32). An entity runs its media clock from the
/// one in force, so it needs to know what each is: its own oscillator, a CRF
/// stream input, gPTP (Milan v1.3 7.5), or some other external source only
/// that entity knows how to follow.
///
/// Milan v1.3 5.3.11.1: "The current clock source shall be saved in a
/// non-volatile memory and restored after a power cycle." The file is
/// `clock-sources` of the entity's state directory (avb_entity_state_dir.hpp):
///
///     statusbar-clock-sources v1
///     domain 0 1
///
/// one line per clock domain, `domain <CLOCK_DOMAIN index> <CLOCK_SOURCE
/// index>`, both decimal. Blank lines and lines that begin with `#` are
/// passed over. The functions that read and write the file are called from
/// the reactor thread only.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

/// What a CLOCK_SOURCE descriptor is to the entity that runs from it.
enum class ClockSourceKind : uint8_t
{
    /// INTERNAL: the entity's own oscillator.
    Internal,
    /// INPUT_STREAM: a stream input, a CRF Media Clock Input among them.
    InputStream,
    /// EXTERNAL located at a TIMING descriptor: gPTP (Milan v1.3 7.5.2).
    Gptp,
    /// Any other source: one the entity itself has to know how to follow.
    External,
};

/// The most CLOCK_SOURCE descriptors of a configuration an entity classifies.
inline constexpr size_t MAX_CLOCK_SOURCES = 16;

/// The kind of each CLOCK_SOURCE of a configuration, by descriptor index.
using ClockSourceKinds = sg14::inplace_vector<ClockSourceKind, MAX_CLOCK_SOURCES>;

/// The kind of the clock source @p source describes.
[[nodiscard]] constexpr auto clock_source_kind_of(atdecc::aem::DescriptorClockSource const& source) noexcept -> ClockSourceKind
{
    if (source.clock_source_type == atdecc::aem::CLOCK_SOURCE_TYPE_INTERNAL) {
        return ClockSourceKind::Internal;
    }
    if (source.clock_source_type == atdecc::aem::CLOCK_SOURCE_TYPE_INPUT_STREAM) {
        return ClockSourceKind::InputStream;
    }
    bool const at_timing = source.clock_source_type == atdecc::aem::CLOCK_SOURCE_TYPE_EXTERNAL &&
        source.clock_source_location_type == atdecc::aem::DESCRIPTOR_TIMING;
    return at_timing ? ClockSourceKind::Gptp : ClockSourceKind::External;
}

/// The kinds of configuration 0's CLOCK_SOURCE descriptors in @p storage,
/// from index 0 up to the first index it lacks (MAX_CLOCK_SOURCES at most).
[[nodiscard]] inline auto clock_source_kinds(atdecc::aem::DescriptorStorage const& storage) -> ClockSourceKinds
{
    ClockSourceKinds kinds{};
    for (uint16_t index = 0; index < MAX_CLOCK_SOURCES; ++index) {
        auto const blob = storage.get_descriptor(0, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, index);
        if (!blob.has_value()) {
            break;
        }
        atdecc::aem::DescriptorClockSource source{};
        span_load_padded(source, *blob);
        kinds.push_back(clock_source_kind_of(source));
    }
    return kinds;
}

/// One line of the file: a clock domain and the clock source it was using.
struct SavedClockSource
{
    uint16_t clock_domain{0};
    uint16_t clock_source{0};

    auto operator<=>(SavedClockSource const&) const noexcept = default;
};

/// The most clock domains whose source is saved: as many as the descriptor
/// storage handler keeps a chosen source for.
inline constexpr size_t MAX_SAVED_CLOCK_SOURCES = 4;

/// The saved clock source of every clock domain of an entity.
using SavedClockSources = sg14::inplace_vector<SavedClockSource, MAX_SAVED_CLOCK_SOURCES>;

inline constexpr std::string_view CLOCK_SOURCES_MAGIC = "statusbar-clock-sources v1";

namespace clock_sources_detail {

inline constexpr std::string_view DOMAIN_WORD = "domain ";

/// One clock domain line, nullopt when malformed.
[[nodiscard]] inline auto parse_line(std::string_view line) -> std::optional<SavedClockSource>
{
    if (!line.starts_with(DOMAIN_WORD)) {
        return std::nullopt;
    }
    line.remove_prefix(DOMAIN_WORD.size());
    SavedClockSource saved{};
    bool const ok = listener_bindings_detail::take_number(line, saved.clock_domain, 10, false) &&
        listener_bindings_detail::take_number(line, saved.clock_source, 10, true);
    return ok ? std::optional{saved} : std::nullopt;
}

/// Take one line that is neither blank nor a comment: the magic first
/// (setting @p seen_magic), a clock domain after it. errc{} when the line was good.
[[nodiscard]] inline auto take_content_line(std::string_view const line, bool& seen_magic, SavedClockSources& sources) -> std::errc
{
    if (!seen_magic) {
        seen_magic = line == CLOCK_SOURCES_MAGIC;
        return seen_magic ? std::errc{} : std::errc::illegal_byte_sequence;
    }
    auto const saved = parse_line(line);
    if (!saved.has_value()) {
        return std::errc::illegal_byte_sequence;
    }
    return sources.try_push_back(*saved) != nullptr ? std::errc{} : std::errc::result_out_of_range;
}

}  // namespace clock_sources_detail

/// Serialize @p sources to the form of the file.
[[nodiscard]] inline auto serialize_clock_sources(std::span<SavedClockSource const> const sources) -> std::string
{
    std::string out{CLOCK_SOURCES_MAGIC};
    out.push_back('\n');
    for (auto const& saved : sources) {
        char line[48];
        auto const n = std::snprintf(
            line,
            sizeof(line),
            "domain %u %u\n",
            static_cast<unsigned>(saved.clock_domain),
            static_cast<unsigned>(saved.clock_source));
        listener_bindings_detail::append_line(out, line, n);
    }
    return out;
}

/// Parse the file's text. An unknown magic, a malformed line or more lines
/// than there are clock domains kept fail the whole of it: a corrupt file
/// should be noticed, not half honoured.
[[nodiscard]] inline auto parse_clock_sources(std::string_view text) -> StatusValue<SavedClockSources>
{
    SavedClockSources sources{};
    bool seen_magic = false;
    while (!text.empty()) {
        auto const line = listener_bindings_detail::take_line(text);
        bool const content = !line.empty() && line.front() != '#';
        auto const verdict = content ? clock_sources_detail::take_content_line(line, seen_magic, sources) : std::errc{};
        if (verdict != std::errc{}) {
            return failure(verdict);
        }
    }
    if (!seen_magic) {
        return failure(std::errc::illegal_byte_sequence);
    }
    return success(sources);
}

/// Load the clock sources saved in @p path. A missing file is no saved clock
/// source (a first start), not an error; a file that is there and corrupt is one.
[[nodiscard]] inline auto load_clock_sources(std::string const& path) -> StatusValue<SavedClockSources>
{
    auto const text = listener_bindings_detail::read_file(path);
    return text.has_value() ? parse_clock_sources(*text) : success(SavedClockSources{});
}

/// Save @p sources to @p path, atomically.
[[nodiscard]] inline auto save_clock_sources(std::string const& path, std::span<SavedClockSource const> const sources) -> Status
{
    return listener_bindings_detail::save_text(path, serialize_clock_sources(sources));
}

}  // namespace statusbar::avb_entity
