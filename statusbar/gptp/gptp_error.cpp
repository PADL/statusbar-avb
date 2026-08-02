// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/gptp/gptp_error.hpp"

#include <string_view>

namespace statusbar::gptp {

auto gptp_error_name(GptpError e) noexcept -> std::string_view
{
    switch (e) {
        case GptpError::Success:
            return "success";
        case GptpError::InvalidConfiguration:
            return "invalid gPTP configuration";
        case GptpError::InvalidLogInterval:
            return "log interval out of range";
        case GptpError::InvalidLinkSpeed:
            return "unknown link speed";
        case GptpError::PendingSyncTableFull:
            return "pending Sync table full";
        case GptpError::PendingPdelayTableFull:
            return "pending PDelay table full";
        case GptpError::ObserverTableFull:
            return "observer table full";
        case GptpError::SyncReceiptTimeout:
            return "Sync receipt timeout";
        case GptpError::PdelayReceiptTimeout:
            return "Pdelay_Resp receipt timeout";
        case GptpError::PdelayFollowUpTimeout:
            return "Pdelay_Resp_Follow_Up receipt timeout";
        case GptpError::AnnounceReceiptTimeout:
            return "Announce receipt timeout";
        case GptpError::PeerMisbehaving:
            return "peer misbehaving";
        case GptpError::NegativeCorrectionField:
            return "negative correction field rejected";
        case GptpError::MalformedMessage:
            return "malformed gPTP message";
        case GptpError::TxTimestampUnavailable:
            return "hardware TX timestamp unavailable";
        case GptpError::RxTimestampMissing:
            return "hardware RX timestamp missing";
        case GptpError::NegativeTimeJump:
            return "negative time jump detected";
        case GptpError::RateOffsetOutOfRange:
            return "rate offset exceeds configured limit";
    }
    return "unknown gPTP error";
}

}  // namespace statusbar::gptp
