#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Shared utilities for the ATDECC CLI tool collection

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/status/status.hpp"

#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <print>
#include <string>
#include <string_view>

namespace statusbar::atdecc_tools {

using ieee::Eui48;
using ieee::Eui64;

//
// Tool IDs - each tool in the collection gets a unique discriminator
//
constexpr uint8_t TOOL_ID_DISCOVER = 0x00;
constexpr uint8_t TOOL_ID_CONTROLLER = 0x01;
constexpr uint8_t TOOL_ID_MONITOR = 0x02;
constexpr uint8_t TOOL_ID_DUMP = 0x03;

//
// Entity ID generation from interface MAC
//
// Format: MAC[0]:MAC[1]:MAC[2]:tool_id:instance:MAC[3]:MAC[4]:MAC[5]
//

/// Generate a controller entity ID from interface MAC, tool ID, and instance
inline auto make_controller_entity_id(Eui48 const& mac, uint8_t tool_id, uint8_t instance) -> Eui64
{
    auto const s = mac.span();
    return Eui64{s[0], s[1], s[2], tool_id, instance, s[3], s[4], s[5]};
}

//
// Common CLI configuration
//

struct CommonConfig
{
    std::string interface_name;
    /// A local entity's ATDECC socket (nanoavb_local_port.hpp), instead of an interface.
    std::string socket_path;
    int64_t instance{0};
    Eui64 controller_entity_id{};
    bool entity_id_override{false};
};

/// Parse an EUI-64 string (xx:xx:xx:xx:xx:xx:xx:xx) into an Eui64
/// Returns true on success
inline auto parse_eui64(std::string_view str, Eui64& out) -> bool
{
    // Strip separators and validate
    std::array<uint8_t, 8> bytes{};
    size_t byte_idx = 0;
    size_t nibble = 0;
    uint8_t current = 0;

    for (char c : str) {
        if (c == ':' || c == '-') {
            continue;
        }
        uint8_t val = 0;
        if (c >= '0' && c <= '9') {
            val = static_cast<uint8_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val = static_cast<uint8_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val = static_cast<uint8_t>(c - 'A' + 10);
        } else {
            return false;
        }

        if (nibble == 0) {
            current = static_cast<uint8_t>(val << 4);
            nibble = 1;
        } else {
            current |= val;
            if (byte_idx < 8) {
                bytes[byte_idx++] = current;
            }
            nibble = 0;
        }
    }

    if (byte_idx != 8 || nibble != 0) {
        return false;
    }

    out = Eui64{bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]};
    return true;
}

/// Add common arguments (--interface, --instance, --controller-entity-id) to specs
inline void add_common_arg_specs(args::ArgumentSpecs& specs, CommonConfig& config)
{
    specs.add_device(
        "interface", "Network interface name (e.g., eth0, en0)", "", [&](auto v) { config.interface_name = std::string{v}; });
    specs.add_file(
        "socket",
        "Reach a local entity through its ATDECC socket (AF_UNIX datagram, e.g. /run/mediabridge/atdecc.sock) instead "
        "of an interface; needs no raw-socket privileges",
        "",
        [&](auto v) { config.socket_path = std::string{v}; });
    specs.add<int64_t>("instance", "Instance offset (0-255) for entity ID uniqueness", 0, [&](auto v) { config.instance = v; });
    specs.add<std::string_view>("controller-entity-id", "Override auto-generated controller entity ID (EUI-64)", "", [&](auto v) {
        if (parse_eui64(v, config.controller_entity_id)) {
            config.entity_id_override = true;
        }
    });
}

//
// Signal handling
//

/// A controller entity ID for a tool on a local ATDECC socket (no interface MAC): the local
/// controller prefix 0A:E9:1B, @p tool_id, @p instance and the process ID, so two tools on one
/// socket differ.
inline auto make_local_controller_entity_id(uint8_t tool_id, uint8_t instance) -> Eui64
{
    auto const pid = static_cast<uint32_t>(::getpid());
    return Eui64{
        0x0A,
        0xE9,
        0x1B,
        tool_id,
        instance,
        static_cast<uint8_t>(pid >> 16),
        static_cast<uint8_t>(pid >> 8),
        static_cast<uint8_t>(pid)};
}

/// Resolve the controller entity ID from config or by reading the interface MAC.
///
/// If config.entity_id_override is set, uses config.controller_entity_id directly.
/// With config.socket_path, generates one with make_local_controller_entity_id().
/// Otherwise opens a temporary raw socket on config.interface_name to read the MAC
/// and generates an entity ID using make_controller_entity_id().
///
/// @param config Common CLI config with interface name and optional entity ID override
/// @param tool_id Tool identifier byte for entity ID generation
[[nodiscard]] inline auto resolve_controller_id(CommonConfig const& config, uint8_t tool_id) -> statusbar::StatusValue<Eui64>
{
    if (config.entity_id_override) {
        return statusbar::success(config.controller_entity_id);
    }
    if (!config.socket_path.empty()) {
        return statusbar::success(make_local_controller_entity_id(tool_id, static_cast<uint8_t>(config.instance)));
    }
    net::RawnetContext rawnet;
    auto open_status = rawnet.open(config.interface_name, avtp::AVTP_ETHERTYPE);
    if (!open_status.has_value()) {
        return statusbar::forward_failure(open_status);
    }
    auto id = make_controller_entity_id(rawnet.my_mac(), tool_id, static_cast<uint8_t>(config.instance));
    rawnet.close();
    return statusbar::success(id);
}

}  // namespace statusbar::atdecc_tools
