// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_adp.hpp"

#include <string_view>

namespace statusbar::atdecc {

auto adp_message_type_name(uint8_t const type) noexcept -> std::string_view
{
    switch (type) {
        case ADP_MESSAGE_TYPE_ENTITY_AVAILABLE:
            return "Entity Available";
        case ADP_MESSAGE_TYPE_ENTITY_DEPARTING:
            return "Entity Departing";
        case ADP_MESSAGE_TYPE_ENTITY_DISCOVER:
            return "Entity Discover";
        default:
            return "Unknown";
    }
}

void AdpDu::init_entity_available(Eui64 const& eid, uint8_t const valid_time_2s) noexcept
{
    subtype = AvtpSubtype::adp;
    sv_version_msgtype = ADP_MESSAGE_TYPE_ENTITY_AVAILABLE;
    set_valid_time(valid_time_2s);
    set_control_data_length(DATA_LENGTH);
    entity_id = eid;
}

void AdpDu::init_entity_departing(Eui64 const& eid) noexcept
{
    subtype = AvtpSubtype::adp;
    sv_version_msgtype = ADP_MESSAGE_TYPE_ENTITY_DEPARTING;
    set_valid_time(0);
    set_control_data_length(DATA_LENGTH);
    entity_id = eid;
    available_index = quadlet_t{0};
}

void AdpDu::init_entity_discover() noexcept
{
    subtype = AvtpSubtype::adp;
    sv_version_msgtype = ADP_MESSAGE_TYPE_ENTITY_DISCOVER;
    set_valid_time(0);
    set_control_data_length(DATA_LENGTH);
    entity_id = Eui64{};
}

}  // namespace statusbar::atdecc
