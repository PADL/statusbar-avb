// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp.hpp"

#include <string_view>

namespace statusbar::atdecc {

auto aecp_message_type_name(uint8_t const type) noexcept -> std::string_view
{
    switch (type) {
        case AECP_MESSAGE_TYPE_AEM_COMMAND:
            return "AEM Command";
        case AECP_MESSAGE_TYPE_AEM_RESPONSE:
            return "AEM Response";
        case AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND:
            return "Address Access Command";
        case AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE:
            return "Address Access Response";
        case AECP_MESSAGE_TYPE_AVC_COMMAND:
            return "AVC Command";
        case AECP_MESSAGE_TYPE_AVC_RESPONSE:
            return "AVC Response";
        case AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND:
            return "Vendor Unique Command";
        case AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE:
            return "Vendor Unique Response";
        case AECP_MESSAGE_TYPE_HDCP_APM_COMMAND:
            return "HDCP APM Command";
        case AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE:
            return "HDCP APM Response";
        case AECP_MESSAGE_TYPE_EXTENDED_COMMAND:
            return "Extended Command";
        case AECP_MESSAGE_TYPE_EXTENDED_RESPONSE:
            return "Extended Response";
        default:
            return "Unknown";
    }
}

auto aecp_status_name(uint8_t const status) noexcept -> std::string_view
{
    switch (status) {
        case AECP_STATUS_SUCCESS:
            return "Success";
        case AECP_STATUS_NOT_IMPLEMENTED:
            return "Not Implemented";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::atdecc
