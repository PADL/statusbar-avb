#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Persisted listener bindings (kit phase 5b).
///
/// A tiny text file holding what the entity's ACMP listener remembers about
/// its stream inputs, so a restarted entity takes them up again without a
/// controller. Either ACMP listener writes it:
///
///  - the IEEE 1722.1 listener its fast-connect goals — the fix for the
///    field-observed half-open deadlock where a talker kept its MSRP attach
///    while the restarted listener had lost all ACMP state;
///  - the Milan v1.3 listener its binding parameters (5.3.8.3), which Milan
///    requires in non-volatile memory.
///
/// Format (one binding per line; blank lines and '#' comments ignored):
///
///   statusbar-listener-bindings v1
///   <stream_index> <talker_entity_id as 16 hex digits> <talker_unique_id>
///
///   statusbar-listener-bindings v2
///   <stream_index> <talker_entity_id> <talker_unique_id> <controller_entity_id as 16 hex digits> <streaming_wait 0|1>
///
/// The IEEE 1722.1 listener writes v1, as it always has; the Milan listener
/// writes v2, which adds the two binding parameters v1 has no place for.
/// Both read both: a v1 file gives a Milan listener a binding with no
/// controller and STREAMING_WAIT clear, and the IEEE 1722.1 listener takes
/// the first three fields of a v2 line.
///
/// The parse/serialize pairs are pure (string-only) and unit-tested; the
/// load/save wrappers do the file I/O and are called from the reactor
/// thread only (never the media thread).

#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_milan_acmp.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

using ListenerBinding = nanoavb::NanoAvbAcmpListener::FastConnectGoal;
using ListenerBindings = sg14::inplace_vector<ListenerBinding, nanoavb::NanoAvbAcmpListener::MAX_FAST_CONNECT_GOALS>;

/// One line of the file, whichever version: a Milan binding, of which the
/// IEEE 1722.1 listener uses the first three fields.
using SavedListenerBinding = atdecc::MilanBinding;
using SavedListenerBindings = nanoavb::NanoAvbMilanAcmpListener::Bindings;

inline constexpr std::string_view LISTENER_BINDINGS_MAGIC = "statusbar-listener-bindings v1";
inline constexpr std::string_view LISTENER_BINDINGS_MAGIC_V2 = "statusbar-listener-bindings v2";

namespace listener_bindings_detail {

inline void append_line(std::string& out, char const* const line, int const length)
{
    if (length > 0) {
        out.append(line, static_cast<size_t>(length));
    }
}

/// Take the next line off the front of @p text, without its trailing CR and spaces.
[[nodiscard]] inline auto take_line(std::string_view& text) -> std::string_view
{
    auto const eol = text.find('\n');
    std::string_view line = text.substr(0, eol);
    text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    return line;
}

/// Take one number in @p base off the front of @p rest: followed by a space
/// (taken too), or when @p last by the end of the line. False when it is not there.
template <typename T>
[[nodiscard]] inline auto take_number(std::string_view& rest, T& value, int const base, bool const last) -> bool
{
    char const* const end = rest.data() + rest.size();
    auto const parsed = std::from_chars(rest.data(), end, value, base);
    bool const terminated = last ? parsed.ptr == end : (parsed.ptr != end && *parsed.ptr == ' ');
    if (parsed.ec != std::errc{} || !terminated) {
        return false;
    }
    rest.remove_prefix(static_cast<size_t>(parsed.ptr - rest.data()) + (last ? 0U : 1U));
    return true;
}

/// The two fields a v2 line adds: <controller hex16> <streaming_wait 0|1>.
[[nodiscard]] inline auto take_controller_and_wait(std::string_view& rest, SavedListenerBinding& binding) -> bool
{
    uint64_t controller = 0;
    unsigned wait = 0;
    bool const ok = take_number(rest, controller, 16, false) && take_number(rest, wait, 10, true) && wait <= 1;
    binding.controller_entity_id = ieee::Eui64{}.from_uint64(controller);
    binding.streaming_wait = wait == 1;
    return ok;
}

/// One binding line of a v1 (@p v2 false) or v2 file, nullopt when malformed.
[[nodiscard]] inline auto parse_line(std::string_view line, bool const v2) -> std::optional<SavedListenerBinding>
{
    SavedListenerBinding binding{};
    uint64_t talker = 0;
    bool ok = take_number(line, binding.listener_unique_id, 10, false) && take_number(line, talker, 16, false) &&
        take_number(line, binding.talker_unique_id, 10, !v2);
    binding.talker_entity_id = ieee::Eui64{}.from_uint64(talker);
    ok = ok && (!v2 || take_controller_and_wait(line, binding));
    return ok ? std::optional{binding} : std::nullopt;
}

/// The version a magic line names: false v1, true v2, nullopt neither.
[[nodiscard]] inline auto magic_is_v2(std::string_view const line) -> std::optional<bool>
{
    if (line == LISTENER_BINDINGS_MAGIC_V2) {
        return true;
    }
    return line == LISTENER_BINDINGS_MAGIC ? std::optional{false} : std::nullopt;
}

/// Take one line that is neither blank nor a comment: the magic first
/// (setting @p v2), a binding after it. errc{} when the line was good.
[[nodiscard]] inline auto take_content_line(std::string_view const line, std::optional<bool>& v2, SavedListenerBindings& bindings)
    -> std::errc
{
    if (!v2.has_value()) {
        v2 = magic_is_v2(line);
        return v2.has_value() ? std::errc{} : std::errc::illegal_byte_sequence;
    }
    auto const binding = parse_line(line, *v2);
    if (!binding.has_value()) {
        return std::errc::illegal_byte_sequence;
    }
    return bindings.try_push_back(*binding) != nullptr ? std::errc{} : std::errc::result_out_of_range;
}

/// Everything left to read in @p f.
[[nodiscard]] inline auto read_rest(std::FILE* const f) -> std::string
{
    std::string text;
    char buf[512];
    for (size_t n = std::fread(buf, 1, sizeof(buf), f); n > 0; n = std::fread(buf, 1, sizeof(buf), f)) {
        text.append(buf, n);
    }
    return text;
}

/// The whole of @p path, nullopt when it cannot be opened.
[[nodiscard]] inline auto read_file(std::string const& path) -> std::optional<std::string>
{
    std::FILE* const f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return std::nullopt;
    }
    std::string text = read_rest(f);
    (void)std::fclose(f);
    return text;
}

/// Write @p text to @p tmp. False when any of it did not get there.
[[nodiscard]] inline auto write_file(std::string const& tmp, std::string const& text) -> std::optional<bool>
{
    std::FILE* const f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return std::nullopt;
    }
    bool const ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    bool const closed = std::fclose(f) == 0;
    return ok && closed;
}

/// Save @p text to @p path atomically: write a .tmp sibling, then rename
/// over the target so a crash never leaves a torn file.
[[nodiscard]] inline auto save_text(std::string const& path, std::string const& text) -> Status
{
    std::string const tmp = path + ".tmp";
    auto const written = write_file(tmp, text);
    if (!written.has_value()) {
        return failure(std::errc::permission_denied);
    }
    if (!*written || std::rename(tmp.c_str(), path.c_str()) != 0) {
        (void)std::remove(tmp.c_str());
        return failure(std::errc::io_error);
    }
    return success();
}

}  // namespace listener_bindings_detail

/// Serialize the IEEE 1722.1 listener's fast-connect goals to the v1 form.
[[nodiscard]] inline auto serialize_listener_bindings(std::span<ListenerBinding const> const bindings) -> std::string
{
    std::string out{LISTENER_BINDINGS_MAGIC};
    out.push_back('\n');
    for (auto const& b : bindings) {
        char line[64];
        auto const n = std::snprintf(
            line,
            sizeof(line),
            "%u %016llx %u\n",
            static_cast<unsigned>(b.listener_unique_id),
            static_cast<unsigned long long>(b.talker_entity_id.to_uint64()),
            static_cast<unsigned>(b.talker_unique_id));
        listener_bindings_detail::append_line(out, line, n);
    }
    return out;
}

/// Serialize the Milan listener's binding parameters to the v2 form.
[[nodiscard]] inline auto serialize_milan_listener_bindings(std::span<SavedListenerBinding const> const bindings) -> std::string
{
    std::string out{LISTENER_BINDINGS_MAGIC_V2};
    out.push_back('\n');
    for (auto const& b : bindings) {
        char line[96];
        auto const n = std::snprintf(
            line,
            sizeof(line),
            "%u %016llx %u %016llx %u\n",
            static_cast<unsigned>(b.listener_unique_id),
            static_cast<unsigned long long>(b.talker_entity_id.to_uint64()),
            static_cast<unsigned>(b.talker_unique_id),
            static_cast<unsigned long long>(b.controller_entity_id.to_uint64()),
            b.streaming_wait ? 1U : 0U);
        listener_bindings_detail::append_line(out, line, n);
    }
    return out;
}

/// Parse either form of the file into full bindings. Unknown magic,
/// malformed lines or over-capacity tables fail loudly (a corrupt bindings
/// file should be noticed, not half-honored).
[[nodiscard]] inline auto parse_saved_listener_bindings(std::string_view text) -> StatusValue<SavedListenerBindings>
{
    SavedListenerBindings bindings{};
    std::optional<bool> v2{};
    while (!text.empty()) {
        auto const line = listener_bindings_detail::take_line(text);
        bool const content = !line.empty() && line.front() != '#';
        auto const verdict = content ? listener_bindings_detail::take_content_line(line, v2, bindings) : std::errc{};
        if (verdict != std::errc{}) {
            return failure(verdict);
        }
    }
    if (!v2.has_value()) {
        return failure(std::errc::illegal_byte_sequence);
    }
    return success(bindings);
}

/// Parse either form of the file into the IEEE 1722.1 listener's
/// fast-connect goals. Fails as parse_saved_listener_bindings() does, and
/// when there are more bindings than that listener has goals.
[[nodiscard]] inline auto parse_listener_bindings(std::string_view const text) -> StatusValue<ListenerBindings>
{
    auto const saved = parse_saved_listener_bindings(text);
    if (!saved.has_value()) {
        return failure(saved.error());
    }
    ListenerBindings bindings{};
    for (auto const& b : *saved) {
        if (bindings.try_push_back(ListenerBinding{
                .listener_unique_id = b.listener_unique_id,
                .talker_entity_id = b.talker_entity_id,
                .talker_unique_id = b.talker_unique_id}) == nullptr) {
            return failure(std::errc::result_out_of_range);
        }
    }
    return success(bindings);
}

/// Load bindings from @p path. A missing file is an empty table (first
/// boot), not an error; a present-but-corrupt file is an error.
[[nodiscard]] inline auto load_listener_bindings(std::string const& path) -> StatusValue<ListenerBindings>
{
    auto const text = listener_bindings_detail::read_file(path);
    return text.has_value() ? parse_listener_bindings(*text) : success(ListenerBindings{});
}

/// Load full bindings from @p path, as load_listener_bindings() does.
[[nodiscard]] inline auto load_saved_listener_bindings(std::string const& path) -> StatusValue<SavedListenerBindings>
{
    auto const text = listener_bindings_detail::read_file(path);
    return text.has_value() ? parse_saved_listener_bindings(*text) : success(SavedListenerBindings{});
}

/// Save the IEEE 1722.1 listener's goals to @p path, atomically.
[[nodiscard]] inline auto save_listener_bindings(std::string const& path, std::span<ListenerBinding const> const bindings) -> Status
{
    return listener_bindings_detail::save_text(path, serialize_listener_bindings(bindings));
}

/// Save the Milan listener's binding parameters to @p path, atomically.
[[nodiscard]] inline auto save_milan_listener_bindings(
    std::string const& path, std::span<SavedListenerBinding const> const bindings) -> Status
{
    return listener_bindings_detail::save_text(path, serialize_milan_listener_bindings(bindings));
}

}  // namespace statusbar::avb_entity
