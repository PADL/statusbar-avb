// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_fingerprint.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/crypto/sha/sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <tuple>

namespace statusbar::nanoavb {

namespace {

void zero_range(std::vector<std::uint8_t>& bytes, std::size_t offset, std::size_t length)
{
    if (offset >= bytes.size()) {
        return;
    }
    auto n = std::min(length, bytes.size() - offset);
    std::fill_n(bytes.begin() + long(offset), n, std::uint8_t(0));
}

/// Zeroes the per-unit volatile fields (§6.2) in a copy of the bytes.
std::vector<std::uint8_t> canonical_bytes(AemRawDescriptor const& d)
{
    using namespace statusbar::atdecc::aem;
    std::vector<std::uint8_t> bytes = d.bytes;
    if (d.descriptor_type == DESCRIPTOR_ENTITY) {
        zero_range(bytes, offsetof(DescriptorEntity, entity_id), 8);
        zero_range(bytes, offsetof(DescriptorEntity, available_index), 4);
        zero_range(bytes, offsetof(DescriptorEntity, association_id), 8);
        zero_range(bytes, offsetof(DescriptorEntity, entity_name), AtdeccString::LENGTH);
        zero_range(bytes, offsetof(DescriptorEntity, group_name), AtdeccString::LENGTH);
        zero_range(bytes, offsetof(DescriptorEntity, current_configuration), 2);
    } else if (d.descriptor_type == DESCRIPTOR_AVB_INTERFACE) {
        zero_range(bytes, offsetof(DescriptorAvbInterface, mac_address), 6);
        zero_range(bytes, offsetof(DescriptorAvbInterface, interface_flags), 2);
        zero_range(bytes, offsetof(DescriptorAvbInterface, clock_identity), 8);
    } else if (d.descriptor_type == DESCRIPTOR_SIGNAL_SELECTOR) {
        // current_signal is runtime state (which source is selected NOW),
        // served back through READ_DESCRIPTOR by entities that track it in
        // their descriptor state — the GALAXY proxy's live input modes made
        // two crawls of one unit fingerprint differently.
        zero_range(bytes, offsetof(DescriptorSignalSelector, current_signal), SignalSource::LENGTH);
    }
    return bytes;
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(std::uint8_t(v >> 8));
    out.push_back(std::uint8_t(v & 0xFF));
}

}  // namespace

std::string aem_fingerprint(std::span<AemRawDescriptor const> descriptors)
{
    std::vector<AemRawDescriptor const*> order;
    order.reserve(descriptors.size());
    for (auto const& d : descriptors) {
        order.push_back(&d);
    }
    std::sort(order.begin(), order.end(), [](AemRawDescriptor const* a, AemRawDescriptor const* b) {
        return std::tuple(a->configuration, a->descriptor_type, a->descriptor_index) <
            std::tuple(b->configuration, b->descriptor_type, b->descriptor_index);
    });

    std::vector<std::uint8_t> canonical;
    for (auto const* d : order) {
        auto bytes = canonical_bytes(*d);
        append_u16(canonical, d->configuration);
        append_u16(canonical, d->descriptor_type);
        append_u16(canonical, d->descriptor_index);
        append_u16(canonical, std::uint16_t(bytes.size()));
        canonical.insert(canonical.end(), bytes.begin(), bytes.end());
    }

    auto digest = crypto::sha256_sw(canonical);
    std::string out;
    out.reserve(digest.size() * 2);
    for (auto b : digest) {
        static char const HEX[] = "0123456789abcdef";
        out.push_back(HEX[b >> 4]);
        out.push_back(HEX[b & 0xF]);
    }
    return out;
}

}  // namespace statusbar::nanoavb
