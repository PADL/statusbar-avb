#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Test support: an AEM blob and a descriptor storage for the tests of the classic IEEE 1722.1
/// profile, which an AemCommandHandler runs unless it is told Profile::milan13.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace statusbar::nanoavb {

/// @p blob as a classic entity serves it: its descriptors and symbols unchanged, since the
/// profile is the handler's (AemCommandHandler::set_profile), not the model's.
[[nodiscard]] inline auto with_classic_profile(std::span<uint8_t const> const blob) -> std::vector<uint8_t>
{
    return {blob.begin(), blob.end()};
}

/// One ENTITY descriptor: an EntityModel given its storage (EntityModel::set_descriptor_storage).
/// Static, as the storage reads it in place.
[[nodiscard]] inline auto classic_profile_storage() -> atdecc::aem::DescriptorStorage
{
    static std::vector<uint8_t> const blob = [] {
        AemBlobWriter w;
        w.add(0, atdecc::aem::DESCRIPTOR_ENTITY, 0, make_const_span(atdecc::aem::DescriptorEntity{}));
        return w.write();
    }();
    return *atdecc::aem::DescriptorStorage::create(blob);
}

}  // namespace statusbar::nanoavb
