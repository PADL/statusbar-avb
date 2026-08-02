// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_mvrp.hpp"

#include <string_view>

namespace statusbar::srp::mvrp {

auto attribute_type_name(AttributeType type) noexcept -> std::string_view
{
    switch (type) {
        case AttributeType::VlanIdentifier:
            return "VlanIdentifier";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::srp::mvrp
