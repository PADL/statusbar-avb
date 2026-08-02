#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// gPTP error codes
//
// Failure modes for the slave-role follower. Follows the same pattern
// as TsnError / NanoAvbError so the codes can flow through
// std::error_code / statusbar::Status uniformly.
//

#include "statusbar/status/status.hpp"

#include <string>
#include <string_view>
#include <system_error>

namespace statusbar::gptp {

enum class GptpError : int
{
    Success = 0,

    // Configuration
    InvalidConfiguration,  ///< GptpConfig::validate() failed
    InvalidLogInterval,    ///< log sync/pdelay interval out of range
    InvalidLinkSpeed,      ///< PHY delay lookup failed for unknown link speed

    // Capacity
    PendingSyncTableFull,    ///< too many in-flight Sync -> FollowUp pairs
    PendingPdelayTableFull,  ///< too many in-flight Pdelay exchanges
    ObserverTableFull,       ///< subscribe() failed, slots full

    // Protocol
    SyncReceiptTimeout,       ///< FollowUp did not arrive in time
    PdelayReceiptTimeout,     ///< Pdelay_Resp did not arrive in time
    PdelayFollowUpTimeout,    ///< Pdelay_Resp_Follow_Up did not arrive in time
    AnnounceReceiptTimeout,   ///< no Announce received within timeout window
    PeerMisbehaving,          ///< duplicate Pdelay_Resp from different sources
    NegativeCorrectionField,  ///< corrected precise origin timestamp went negative
    MalformedMessage,         ///< message failed parsing or integrity checks

    // Timestamping
    TxTimestampUnavailable,  ///< HW TX timestamp not obtainable
    RxTimestampMissing,      ///< RX timestamp wasn't supplied with the frame

    // Servo
    NegativeTimeJump,      ///< master time went backwards vs previous sample
    RateOffsetOutOfRange,  ///< computed rate offset exceeds configured ppm limit
};

/// Human-readable name for a GptpError value.
[[nodiscard]] auto gptp_error_name(GptpError e) noexcept -> std::string_view;

/// Error category for gPTP errors.
class GptpErrorCategory : public std::error_category
{
  public:
    [[nodiscard]] auto name() const noexcept -> char const* override { return "statusbar.gptp"; }

    [[nodiscard]] auto message(int ev) const -> std::string override
    {
        return std::string{gptp_error_name(static_cast<GptpError>(ev))};
    }
};

/// Get the gPTP error category singleton.
[[nodiscard]] inline auto gptp_error_category() noexcept -> GptpErrorCategory const&
{
    static GptpErrorCategory const category;
    return category;
}

/// Create an error_code from a GptpError.
[[nodiscard]] inline auto make_error_code(GptpError e) noexcept -> std::error_code
{
    return std::error_code{static_cast<int>(e), gptp_error_category()};
}

}  // namespace statusbar::gptp

template <>
struct std::is_error_code_enum<statusbar::gptp::GptpError> : std::true_type
{};
