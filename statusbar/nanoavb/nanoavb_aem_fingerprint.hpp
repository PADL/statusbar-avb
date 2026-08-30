#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Model fingerprints: a hash over the descriptor bytes
// in canonical order with the per-unit volatile fields zeroed first —
// entity_id, entity_name, group_name, current_configuration,
// association_id (plus available_index, equally volatile), and
// AVB_INTERFACE's MAC address, interface_flags (link state), and
// clock_identity (MAC-derived). Miss any of those and every physical unit
// fingerprints differently, collapsing dedup back into per-device
// enumeration. The hash runs over a normalized canonical form — sorted
// (configuration, type, index) with an explicit length prefix per
// descriptor — so blob padding and authoring-tool quirks do not leak into
// identity.

namespace statusbar::nanoavb {

/// One raw descriptor as read off the wire or out of a blob: the full
/// READ_DESCRIPTOR payload starting at descriptor_type, trailers included.
struct AemRawDescriptor
{
    std::uint16_t configuration = 0;
    std::uint16_t descriptor_type = 0;
    std::uint16_t descriptor_index = 0;
    std::vector<std::uint8_t> bytes;
};

/// The model fingerprint as 64 lowercase hex characters (SHA-256).
std::string aem_fingerprint(std::span<AemRawDescriptor const> descriptors);

}  // namespace statusbar::nanoavb
