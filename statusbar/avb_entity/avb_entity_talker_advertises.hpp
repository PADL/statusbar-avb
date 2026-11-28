#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_talker_advertises.hpp
/// @brief The file an entity keeps the Talker Advertise of each of its
///        stream outputs in, as it last declared them.
///
/// IEEE 802.1Q-2022 35.2.2.8: "MSRP does not support changes in any of the
/// FirstValue fields for an existing StreamID. If a Talker wishes to tear an
/// old Stream down and bring a new Stream up, with a different FirstValue,
/// utilizing the same StreamID, there must be at least two LeaveAllTime ...
/// time periods between when the Talker removes the existing Stream
/// registration and declares the new Stream."
///
/// Within one run the MSRP participant keeps that wait. Across a restart it
/// cannot: nothing in a process just started knows what the last one
/// declared. So the entity saves it, and at its next start tells the MSRP
/// handler (MsrpHandler::talker_hold_after_restart()), which then holds back
/// a stream whose destination address, VLAN, TSpec or priority is no longer
/// what the bridges last had registered.
///
/// The file is `talker-advertises` of the entity's state directory
/// (avb_entity_state_dir.hpp):
///
///     statusbar-talker-advertises v1
///     stream 2ccf67fffe000001 91e0f0001234 2 416 1 3
///
/// one line per stream output, `stream <StreamID> <destination address>
/// <VLAN ID> <MaxFrameSize> <MaxIntervalFrames> <priority>`: the first two in
/// hexadecimal, sixteen and twelve digits, the rest decimal.
/// AccumulatedLatency is not kept: it is the one field a Talker may change in
/// place (35.2.2.8.6). Blank lines and lines that begin with `#` are passed
/// over. The functions that read and write the file are called from the
/// reactor thread only.

#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/status/status.hpp"

#include <compare>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::avb_entity {

/// The Talker Advertise of one stream output as it was last declared, without its AccumulatedLatency.
struct SavedTalkerAdvertise
{
    uint64_t stream_id{0};
    uint64_t dest_address{0};  ///< the 48 bits of the destination address
    uint16_t vlan_id{0};
    uint16_t max_frame_size{0};
    uint16_t max_interval_frames{0};
    uint8_t priority{0};

    auto operator<=>(SavedTalkerAdvertise const&) const noexcept = default;
};

/// The saved Talker Advertise of every stream output of an entity.
using SavedTalkerAdvertises = sg14::inplace_vector<SavedTalkerAdvertise, MAX_ENTITY_STREAMS>;

inline constexpr std::string_view TALKER_ADVERTISES_MAGIC = "statusbar-talker-advertises v1";

/// What is kept of @p info: everything of the attribute but its AccumulatedLatency.
[[nodiscard]] inline auto saved_talker_advertise(nanoavb::TalkerStreamSrpInfo const& info) noexcept -> SavedTalkerAdvertise
{
    return SavedTalkerAdvertise{
        .stream_id = info.stream_id.to_uint64(),
        .dest_address = info.dest_address.to_uint64(),
        .vlan_id = info.vlan_id,
        .max_frame_size = info.max_frame_size,
        .max_interval_frames = info.max_interval_frames,
        .priority = info.priority};
}

/// @p saved as the parameters MsrpHandler::talker_hold_after_restart() takes, AccumulatedLatency zero.
[[nodiscard]] inline auto talker_srp_info(SavedTalkerAdvertise const& saved) noexcept -> nanoavb::TalkerStreamSrpInfo
{
    nanoavb::TalkerStreamSrpInfo info{
        .vlan_id = saved.vlan_id,
        .max_frame_size = saved.max_frame_size,
        .max_interval_frames = saved.max_interval_frames,
        .priority = saved.priority};
    info.stream_id.from_uint64(saved.stream_id);
    info.dest_address.from_uint64(saved.dest_address);
    return info;
}

namespace talker_advertises_detail {

inline constexpr std::string_view STREAM_WORD = "stream ";

/// The numbers of a stream line behind its two addresses.
[[nodiscard]] inline auto take_tspec(std::string_view& line, SavedTalkerAdvertise& saved) -> bool
{
    return listener_bindings_detail::take_number(line, saved.vlan_id, 10, false) &&
        listener_bindings_detail::take_number(line, saved.max_frame_size, 10, false) &&
        listener_bindings_detail::take_number(line, saved.max_interval_frames, 10, false) &&
        listener_bindings_detail::take_number(line, saved.priority, 10, true);
}

/// One stream line, nullopt when malformed.
[[nodiscard]] inline auto parse_line(std::string_view line) -> std::optional<SavedTalkerAdvertise>
{
    if (!line.starts_with(STREAM_WORD)) {
        return std::nullopt;
    }
    line.remove_prefix(STREAM_WORD.size());
    SavedTalkerAdvertise saved{};
    bool const ok = listener_bindings_detail::take_number(line, saved.stream_id, 16, false) &&
        listener_bindings_detail::take_number(line, saved.dest_address, 16, false) && take_tspec(line, saved);
    return ok && saved.dest_address <= 0xFFFF'FFFF'FFFFULL ? std::optional{saved} : std::nullopt;
}

/// Take one line that is neither blank nor a comment: the magic first
/// (setting @p seen_magic), a stream after it. errc{} when the line was good.
[[nodiscard]] inline auto take_content_line(std::string_view const line, bool& seen_magic, SavedTalkerAdvertises& streams)
    -> std::errc
{
    if (!seen_magic) {
        seen_magic = line == TALKER_ADVERTISES_MAGIC;
        return seen_magic ? std::errc{} : std::errc::illegal_byte_sequence;
    }
    auto const saved = parse_line(line);
    if (!saved.has_value()) {
        return std::errc::illegal_byte_sequence;
    }
    return streams.try_push_back(*saved) != nullptr ? std::errc{} : std::errc::result_out_of_range;
}

}  // namespace talker_advertises_detail

/// Serialize @p streams to the form of the file.
[[nodiscard]] inline auto serialize_talker_advertises(std::span<SavedTalkerAdvertise const> const streams) -> std::string
{
    std::string out{TALKER_ADVERTISES_MAGIC};
    out.push_back('\n');
    for (auto const& saved : streams) {
        char line[96];
        auto const n = std::snprintf(
            line,
            sizeof(line),
            "stream %016llx %012llx %u %u %u %u\n",
            static_cast<unsigned long long>(saved.stream_id),
            static_cast<unsigned long long>(saved.dest_address),
            static_cast<unsigned>(saved.vlan_id),
            static_cast<unsigned>(saved.max_frame_size),
            static_cast<unsigned>(saved.max_interval_frames),
            static_cast<unsigned>(saved.priority));
        listener_bindings_detail::append_line(out, line, n);
    }
    return out;
}

/// Parse the file's text. An unknown magic, a malformed line or more lines
/// than an entity has streams fail the whole of it: a corrupt file should be
/// noticed, not half honoured.
[[nodiscard]] inline auto parse_talker_advertises(std::string_view text) -> StatusValue<SavedTalkerAdvertises>
{
    SavedTalkerAdvertises streams{};
    bool seen_magic = false;
    while (!text.empty()) {
        auto const line = listener_bindings_detail::take_line(text);
        bool const content = !line.empty() && line.front() != '#';
        auto const verdict = content ? talker_advertises_detail::take_content_line(line, seen_magic, streams) : std::errc{};
        if (verdict != std::errc{}) {
            return failure(verdict);
        }
    }
    if (!seen_magic) {
        return failure(std::errc::illegal_byte_sequence);
    }
    return success(streams);
}

/// Load the Talker Advertises saved in @p path. A missing file is none saved
/// (a first start), not an error; a file that is there and corrupt is one.
[[nodiscard]] inline auto load_talker_advertises(std::string const& path) -> StatusValue<SavedTalkerAdvertises>
{
    auto const text = listener_bindings_detail::read_file(path);
    return text.has_value() ? parse_talker_advertises(*text) : success(SavedTalkerAdvertises{});
}

/// Save @p streams to @p path, atomically.
[[nodiscard]] inline auto save_talker_advertises(std::string const& path, std::span<SavedTalkerAdvertise const> const streams)
    -> Status
{
    return listener_bindings_detail::save_text(path, serialize_talker_advertises(streams));
}

}  // namespace statusbar::avb_entity
