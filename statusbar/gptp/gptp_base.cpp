// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/gptp/gptp_base.hpp"

#include <string_view>

namespace statusbar::gptp {

auto message_type_name(uint8_t msg_type) noexcept -> std::string_view
{
    switch (msg_type) {
        case MESSAGE_TYPE_SYNC:
            return "Sync";
        case MESSAGE_TYPE_DELAY_REQ:
            return "Delay_Req";
        case MESSAGE_TYPE_PDELAY_REQ:
            return "Pdelay_Req";
        case MESSAGE_TYPE_PDELAY_RESP:
            return "Pdelay_Resp";
        case MESSAGE_TYPE_FOLLOW_UP:
            return "Follow_Up";
        case MESSAGE_TYPE_DELAY_RESP:
            return "Delay_Resp";
        case MESSAGE_TYPE_PDELAY_RESP_FOLLOW_UP:
            return "Pdelay_Resp_Follow_Up";
        case MESSAGE_TYPE_ANNOUNCE:
            return "Announce";
        case MESSAGE_TYPE_SIGNALING:
            return "Signaling";
        case MESSAGE_TYPE_MANAGEMENT:
            return "Management";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::gptp
