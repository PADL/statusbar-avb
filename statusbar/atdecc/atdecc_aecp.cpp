// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <span>
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

auto aecp_echo_response(std::span<uint8_t const> const command, uint8_t const status, std::span<uint8_t> const out) noexcept
    -> std::span<uint8_t>
{
    if (command.size() < AecpDuCommon::LENGTH) {
        return {};
    }
    AecpDuCommon header{};
    span_load(header, command.first(AecpDuCommon::LENGTH));
    size_t const declared = AecpDuCommon::HEADER_LENGTH + header.control_data_length();
    size_t const size = std::min({declared, command.size(), out.size()});
    // A command whose control_data_length does not even cover the AECP common
    // fields (controller_entity_id, sequence_id) is malformed: not echoed.
    if (size < AecpDuCommon::LENGTH) {
        return {};
    }
    auto const response = out.first(size);
    span_copy(response, command.first(size));
    header.set_message_type(static_cast<uint8_t>(header.message_type() | 1U));
    header.set_status(status);
    header.set_control_data_length(static_cast<uint16_t>(size - AecpDuCommon::HEADER_LENGTH));
    span_store(response.first(AecpDuCommon::LENGTH), header);
    return response;
}

}  // namespace statusbar::atdecc
