#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The entity's ATDECC profile, read by every module that switches Milan-only behaviour; the
/// default comes from the model (an ENTITY symbol, else Milan) and a runtime setting
/// overrides it. See docs/NANOAVB_ARCHITECTURE.md.

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace statusbar::nanoavb {

/// The Milan revision a Milan entity implements: it selects every layout and rule that differs
/// between them (GET_MILAN_INFO's specification_version, Milan v1.3 Section 5.4.4.1).
enum class MilanVersion : uint8_t
{
    v1_2,  ///< Milan v1.2 (2023-11-29)
    v1_3,  ///< Milan v1.3 (2025-08-21)
};

/// GET_MILAN_INFO's specification_version for @p version: major, minor, 0, 0 (Milan v1.3 Section 5.4.4.1).
[[nodiscard]] constexpr auto milan_specification_version(MilanVersion const version) noexcept -> uint32_t
{
    return version == MilanVersion::v1_3 ? 0x01030000U : 0x01020000U;
}

struct EntityProfile
{
    bool milan{true};                                ///< Milan; false: classic IEEE 1722.1
    MilanVersion milan_version{MilanVersion::v1_3};  ///< meaningful when milan

    /// A Milan entity at v1.3 or later.
    [[nodiscard]] constexpr auto milan_v1_3() const noexcept -> bool { return milan && milan_version >= MilanVersion::v1_3; }

    auto operator==(EntityProfile const&) const noexcept -> bool = default;
};

/// The ENTITY descriptor's well-known profile symbols (aemxml flatten.py).
inline constexpr std::string_view ENTITY_PROFILE_MILAN_SYMBOL = "entity_profile:milan";
inline constexpr std::string_view ENTITY_PROFILE_CLASSIC_SYMBOL = "entity_profile:classic";

/// A symbol name's 32-bit code: zlib/IEEE 802.3 CRC-32, as aemxml's
/// symbol_to_code (and avb_entity::symbol_code).
[[nodiscard]] constexpr auto entity_profile_symbol_code(std::string_view const name) noexcept -> uint32_t
{
    uint32_t crc = 0xFFFFFFFFU;
    for (char const c : name) {
        crc ^= static_cast<uint8_t>(c);
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/// The profile: @p milan_override when set, else what the model's ENTITY symbol
/// says, else Milan; Milan v1.3 either way (NanoAvbComponents::set_milan_version changes it).
[[nodiscard]] inline auto entity_profile_from_model(
    atdecc::aem::DescriptorStorage const* const storage, std::optional<bool> const milan_override = std::nullopt) noexcept
    -> EntityProfile
{
    if (milan_override.has_value()) {
        return EntityProfile{.milan = *milan_override};
    }
    if (storage != nullptr) {
        if (auto const symbol = storage->get_symbol(0, atdecc::aem::DESCRIPTOR_ENTITY, 0)) {
            if (*symbol == entity_profile_symbol_code(ENTITY_PROFILE_CLASSIC_SYMBOL)) {
                return EntityProfile{.milan = false};
            }
        }
    }
    return EntityProfile{};
}

}  // namespace statusbar::nanoavb
