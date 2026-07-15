#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Persisted listener fast-connect bindings (kit phase 5b).
///
/// A tiny text file mirroring NanoAvbAcmpListener's fast-connect goals so a
/// restarted entity re-connects its stream inputs without a controller —
/// the fix for the field-observed half-open deadlock where a talker kept
/// its MSRP attach while the restarted listener had lost all ACMP state.
///
/// Format (one binding per line; blank lines and '#' comments ignored):
///
///   statusbar-listener-bindings v1
///   <stream_index> <talker_entity_id as 16 hex digits> <talker_unique_id>
///
/// The parse/serialize pair is pure (string-only) and unit-tested; the
/// load/save wrappers do the file I/O and are called from the reactor
/// thread only (never the media thread).

#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

using ListenerBinding = nanoavb::NanoAvbAcmpListener::FastConnectGoal;
using ListenerBindings = sg14::inplace_vector<ListenerBinding, nanoavb::NanoAvbAcmpListener::MAX_FAST_CONNECT_GOALS>;

inline constexpr std::string_view LISTENER_BINDINGS_MAGIC = "statusbar-listener-bindings v1";

/// Serialize bindings to the on-disk text form.
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
        if (n > 0) {
            out.append(line, static_cast<size_t>(n));
        }
    }
    return out;
}

/// Parse the on-disk text form. Unknown magic, malformed lines or
/// over-capacity tables fail loudly (a corrupt bindings file should be
/// noticed, not half-honored).
[[nodiscard]] inline auto parse_listener_bindings(std::string_view text) -> StatusValue<ListenerBindings>
{
    ListenerBindings bindings{};
    bool saw_magic = false;
    while (!text.empty()) {
        auto const eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = (eol == std::string_view::npos) ? std::string_view{} : text.substr(eol + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!saw_magic) {
            if (line != LISTENER_BINDINGS_MAGIC) {
                return failure(std::errc::illegal_byte_sequence);
            }
            saw_magic = true;
            continue;
        }
        // <index> <talker hex16> <uid>
        uint16_t index = 0;
        uint64_t talker = 0;
        uint16_t uid = 0;
        char const* p = line.data();
        char const* const end = line.data() + line.size();
        auto r = std::from_chars(p, end, index, 10);
        if (r.ec != std::errc{} || r.ptr == end || *r.ptr != ' ') {
            return failure(std::errc::illegal_byte_sequence);
        }
        r = std::from_chars(r.ptr + 1, end, talker, 16);
        if (r.ec != std::errc{} || r.ptr == end || *r.ptr != ' ') {
            return failure(std::errc::illegal_byte_sequence);
        }
        r = std::from_chars(r.ptr + 1, end, uid, 10);
        if (r.ec != std::errc{} || r.ptr != end) {
            return failure(std::errc::illegal_byte_sequence);
        }
        if (bindings.try_push_back(ListenerBinding{
                .listener_unique_id = index, .talker_entity_id = ieee::Eui64{}.from_uint64(talker), .talker_unique_id = uid}) ==
            nullptr) {
            return failure(std::errc::result_out_of_range);
        }
    }
    if (!saw_magic) {
        return failure(std::errc::illegal_byte_sequence);
    }
    return success(bindings);
}

/// Load bindings from @p path. A missing file is an empty table (first
/// boot), not an error; a present-but-corrupt file is an error.
[[nodiscard]] inline auto load_listener_bindings(std::string const& path) -> StatusValue<ListenerBindings>
{
    std::FILE* const f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return success(ListenerBindings{});
    }
    std::string text;
    char buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
    }
    (void)std::fclose(f);
    return parse_listener_bindings(text);
}

/// Save bindings to @p path (atomically: write a .tmp sibling, then
/// rename over the target so a crash never leaves a torn file).
[[nodiscard]] inline auto save_listener_bindings(std::string const& path, std::span<ListenerBinding const> const bindings) -> Status
{
    std::string const tmp = path + ".tmp";
    std::FILE* const f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return failure(std::errc::permission_denied);
    }
    auto const text = serialize_listener_bindings(bindings);
    bool const ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    bool const closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        (void)std::remove(tmp.c_str());
        return failure(std::errc::io_error);
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        (void)std::remove(tmp.c_str());
        return failure(std::errc::io_error);
    }
    return success();
}

}  // namespace statusbar::avb_entity
