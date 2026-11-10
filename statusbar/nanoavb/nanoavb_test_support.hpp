#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Test support: an AEM blob made classic by its ENTITY's profile symbol, for the tests of the
/// classic IEEE 1722.1 profile (nanoavb::entity_profile_from_model).

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_entity_profile.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace statusbar::nanoavb {

/// @p blob with the classic profile symbol on its ENTITY, its descriptors and their other symbols
/// kept; empty when @p blob does not parse.
[[nodiscard]] inline auto with_classic_profile(std::span<uint8_t const> const blob) -> std::vector<uint8_t>
{
    auto const storage = atdecc::aem::DescriptorStorage::create(blob);
    if (!storage) {
        return {};
    }
    AemBlobWriter w;
    storage->for_each_descriptor([&](uint16_t const c, uint16_t const t, uint16_t const i, std::span<uint8_t const> const d) {
        w.add(c, t, i, d);
        if (auto const symbol = storage->get_symbol(c, t, i); symbol && t != atdecc::aem::DESCRIPTOR_ENTITY) {
            w.add_symbol(c, t, i, *symbol);
        }
    });
    w.add_symbol(0, atdecc::aem::DESCRIPTOR_ENTITY, 0, entity_profile_symbol_code(ENTITY_PROFILE_CLASSIC_SYMBOL));
    return w.write();
}

/// One ENTITY descriptor with the classic profile symbol: an EntityModel given its storage
/// (EntityModel::set_descriptor_storage) runs classic. Static, as the storage reads it in place.
[[nodiscard]] inline auto classic_profile_storage() -> atdecc::aem::DescriptorStorage
{
    static std::vector<uint8_t> const blob = [] {
        AemBlobWriter w;
        w.add(0, atdecc::aem::DESCRIPTOR_ENTITY, 0, make_const_span(atdecc::aem::DescriptorEntity{}));
        w.add_symbol(0, atdecc::aem::DESCRIPTOR_ENTITY, 0, entity_profile_symbol_code(ENTITY_PROFILE_CLASSIC_SYMBOL));
        return w.write();
    }();
    return *atdecc::aem::DescriptorStorage::create(blob);
}

}  // namespace statusbar::nanoavb
