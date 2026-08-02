// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/stun/stun_error.hpp"

#include <string_view>

namespace statusbar::stun {

namespace {

[[nodiscard]] auto stun_error_message(StunError e) -> std::string_view
{
    switch (e) {
        case StunError::DatagramTooShort:
            return "Datagram smaller than STUN header";
        case StunError::InvalidLeadingBits:
            return "STUN message type leading bits are not zero";
        case StunError::InvalidMagicCookie:
            return "STUN magic cookie mismatch";
        case StunError::InvalidMessageLength:
            return "STUN message length not a multiple of four or exceeds buffer";
        case StunError::InvalidAttributeLength:
            return "STUN attribute length is invalid for the attribute type";
        case StunError::AttributeTruncated:
            return "STUN attribute extends beyond message body";
        case StunError::UnknownComprehensionRequiredAttribute:
            return "Unknown comprehension-required attribute encountered";
        case StunError::DuplicateAttribute:
            return "STUN attribute appears more than once";
        case StunError::MissingRequiredAttribute:
            return "Required STUN attribute is missing";
        case StunError::InvalidAddressFamily:
            return "Address family field is malformed";
        case StunError::UnsupportedAddressFamily:
            return "Address family is not IPv4 or IPv6";
        case StunError::BufferTooSmall:
            return "Output buffer too small to encode message";
        case StunError::AuthFailed:
            return "AES-SIV authentication tag did not verify";
        case StunError::AuthenticationMissing:
            return "MIC attribute missing from authenticated message";
        case StunError::InvalidSessionState:
            return "Session state value is not recognized";
        case StunError::InvalidRole:
            return "Role value is not recognized";
        case StunError::SessionFull:
            return "Session already has two peers registered";
        case StunError::SessionNotFound:
            return "Session ID is not known to the server";
        case StunError::SessionExpired:
            return "Session has expired and was dropped";
        case StunError::TransactionMismatch:
            return "Transaction ID in response does not match outstanding request";
        case StunError::UnexpectedClass:
            return "STUN message class is not valid in this context";
        case StunError::UnexpectedMethod:
            return "STUN method is not handled by this peer";
        case StunError::SocketCreateFailed:
            return "Failed to create UDP socket";
        case StunError::SocketBindFailed:
            return "Failed to bind UDP socket";
        case StunError::SocketSendFailed:
            return "Failed to send UDP datagram";
        case StunError::NoServerAddress:
            return "No STUN server address configured";
        case StunError::InvalidErrorCode:
            return "ERROR-CODE attribute is malformed";
    }
    return "Unknown STUN error";
}

}  // namespace

auto StunErrorCategory::message(int ev) const -> std::string
{
    return std::string{stun_error_message(static_cast<StunError>(ev))};
}

auto stun_error_category() noexcept -> std::error_category const&
{
    static StunErrorCategory const instance;
    return instance;
}

auto make_error_code(StunError e) noexcept -> std::error_code
{
    return {static_cast<int>(e), stun_error_category()};
}

}  // namespace statusbar::stun
