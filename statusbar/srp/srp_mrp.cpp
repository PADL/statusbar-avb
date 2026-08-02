// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_mrp.hpp"

#include <string_view>

namespace statusbar::srp::mrp {

auto attribute_event_name(AttributeEvent event) noexcept -> std::string_view
{
    switch (event) {
        case AttributeEvent::New:
            return "New";
        case AttributeEvent::JoinIn:
            return "JoinIn";
        case AttributeEvent::In:
            return "In";
        case AttributeEvent::JoinMt:
            return "JoinMt";
        case AttributeEvent::Mt:
            return "Mt";
        case AttributeEvent::Lv:
            return "Lv";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::srp::mrp
