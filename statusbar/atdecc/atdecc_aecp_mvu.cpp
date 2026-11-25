// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"

#include <charconv>
#include <format>
#include <string>
#include <string_view>

namespace statusbar::atdecc {

auto mvu_command_name(uint16_t const command_type) noexcept -> std::string_view
{
    switch (command_type) {
        case MVU_COMMAND_GET_MILAN_INFO:
            return "GET_MILAN_INFO";
        case MVU_COMMAND_SET_SYSTEM_UNIQUE_ID:
            return "SET_SYSTEM_UNIQUE_ID";
        case MVU_COMMAND_GET_SYSTEM_UNIQUE_ID:
            return "GET_SYSTEM_UNIQUE_ID";
        case MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO:
            return "SET_MEDIA_CLOCK_REFERENCE_INFO";
        case MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO:
            return "GET_MEDIA_CLOCK_REFERENCE_INFO";
        case MVU_COMMAND_BIND_STREAM:
            return "BIND_STREAM";
        case MVU_COMMAND_UNBIND_STREAM:
            return "UNBIND_STREAM";
        case MVU_COMMAND_GET_STREAM_INPUT_INFO_EX:
            return "GET_STREAM_INPUT_INFO_EX";
        default:
            return "Unknown";
    }
}

auto mvu_status_name(uint8_t const status) noexcept -> std::string_view
{
    switch (status) {
        case MVU_STATUS_SUCCESS:
            return "SUCCESS";
        case MVU_STATUS_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";
        case MVU_STATUS_NO_SUCH_DESCRIPTOR:
            return "NO_SUCH_DESCRIPTOR";
        case MVU_STATUS_ENTITY_LOCKED:
            return "ENTITY_LOCKED";
        case MVU_STATUS_BAD_ARGUMENTS:
            return "BAD_ARGUMENTS";
        case MVU_STATUS_ENTITY_MISBEHAVING:
            return "ENTITY_MISBEHAVING";
        case MVU_STATUS_PAYLOAD_TOO_SHORT:
            return "PAYLOAD_TOO_SHORT";
        default:
            return "Unknown";
    }
}

auto mvu_probing_status_name(uint8_t const pbsta) noexcept -> std::string_view
{
    switch (pbsta) {
        case 0:
            return "PROBING_DISABLED";
        case 1:
            return "PROBING_PASSIVE";
        case 2:
            return "PROBING_ACTIVE";
        case 3:
            return "PROBING_COMPLETED";
        default:
            return "Unknown";
    }
}

namespace {

/// One decimal number of at most 255 from the front of @p text, consumed
/// along with the dot after it, if any. nullopt for no digits, a value over
/// 255, or a character that is neither digit nor dot.
[[nodiscard]] auto take_version_number(std::string_view& text) noexcept -> std::optional<uint8_t>
{
    unsigned value = 0;
    auto const [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || value > 255 || end == text.data()) {
        return std::nullopt;
    }
    text.remove_prefix(static_cast<size_t>(end - text.data()));
    if (!text.empty()) {
        if (text.front() != '.' || text.size() == 1) {
            return std::nullopt;
        }
        text.remove_prefix(1);
    }
    return static_cast<uint8_t>(value);
}

}  // namespace

auto mvu_version_from_dotted(std::string_view text) noexcept -> std::optional<uint32_t>
{
    if (text.empty()) {
        return std::nullopt;
    }
    uint32_t version = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        if (text.empty()) {
            return version;
        }
        auto const number = take_version_number(text);
        if (!number) {
            return std::nullopt;
        }
        version |= static_cast<uint32_t>(*number) << shift;
    }
    return text.empty() ? std::optional<uint32_t>{version} : std::nullopt;
}

auto mvu_version_to_dotted(uint32_t const version) -> std::string
{
    return std::format("{}.{}.{}.{}", (version >> 24) & 0xFF, (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF);
}

}  // namespace statusbar::atdecc
