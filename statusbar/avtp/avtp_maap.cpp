// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_maap.hpp"

#include <string_view>

namespace statusbar::avtp {

auto maap_message_type_name(uint8_t const type) noexcept -> std::string_view
{
    switch (type) {
        case MAAP_MESSAGE_TYPE_PROBE:
            return "Probe";
        case MAAP_MESSAGE_TYPE_DEFEND:
            return "Defend";
        case MAAP_MESSAGE_TYPE_ANNOUNCE:
            return "Announce";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::avtp
