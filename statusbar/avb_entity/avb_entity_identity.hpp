#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Per-node identity defaults for AVB entities. Centralizes the "make every
/// node on the wire unique" logic the entity tools otherwise hand-roll (or
/// forget — a hardcoded entity_id makes two nodes running the same descriptor
/// blob collide).

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/net/net_posix_util.hpp"

#include <unistd.h>

#include <array>
#include <print>
#include <string>
#include <string_view>

namespace statusbar::avb_entity {

/// Fill per-node identity defaults in place:
///  - `entity_id` (when unset): the `interface_name` MAC as a modified EUI-64
///    so every node is unique; falls back to `fallback_id` if the MAC can't be
///    read (with a warning).
///  - `entity_name` (when empty): the host's name, so a controller can address
///    the node by name; falls back to `fallback_name`.
///
/// Idempotent: an already-set id / non-empty name (e.g. from `--entity.id` /
/// `--entity.name`) is left untouched.
/// Globally-unique IEEE 1722 stream_id: the talker's NIC MAC (unique per
/// interface) in the high 6 bytes plus the per-stream index in the low 16
/// bits. MAC-based — NOT entity_id-based — so two entities whose ids share
/// their high 48 bits still get distinct stream_ids (identical stream_ids
/// are indistinguishable to a listener's RX demux and MSRP reservations).
[[nodiscard]] inline auto stream_id_for(ieee::Eui48 const& base_mac, uint16_t const index) -> ieee::Eui64
{
    ieee::Eui64 sid{};
    span_copy(sid.span().first(6), make_const_span(base_mac).first(6));
    sid.span()[6] = 0;
    sid.span()[7] = static_cast<uint8_t>(index & 0xFFU);
    return sid;
}

/// The NIC MAC for @p interface_name, falling back to @p entity_id's high
/// 6 bytes when the interface can't be read (the stream-id / MAAP base).
[[nodiscard]] inline auto stream_base_mac_for(std::string_view const interface_name, ieee::Eui64 const& entity_id) -> ieee::Eui48
{
    if (auto const mac = net::read_interface_mac(interface_name)) {
        return *mac;
    }
    ieee::Eui48 fallback{};
    span_copy(fallback.span(), make_const_span(entity_id).first(6));
    return fallback;
}

inline void apply_node_identity_defaults(
    std::string_view interface_name,
    ieee::Eui64& entity_id,
    std::string& entity_name,
    ieee::Eui64 const& fallback_id,
    std::string_view fallback_name)
{
    if (!entity_id.is_set()) {
        if (auto const mac = net::read_interface_mac(interface_name)) {
            entity_id = mac->to_modified_eui64();
        } else {
            std::print(stderr, "Warning: could not read MAC of {}; using fallback entity ID\n", interface_name);
            entity_id = fallback_id;
        }
    }
    if (entity_name.empty()) {
        std::array<char, 256> host{};
        entity_name = (::gethostname(host.data(), host.size() - 1) == 0) ? std::string{host.data()} : std::string{fallback_name};
    }
}

}  // namespace statusbar::avb_entity
