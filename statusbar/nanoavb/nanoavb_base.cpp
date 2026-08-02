// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_base.hpp"

#include <string_view>

namespace statusbar::nanoavb {

auto nanoavb_error_name(NanoAvbError e) noexcept -> std::string_view
{
    switch (e) {
        case NanoAvbError::Success:
            return "Success";
        case NanoAvbError::InvalidDescriptorType:
            return "Invalid Descriptor Type";
        case NanoAvbError::InvalidDescriptorIndex:
            return "Invalid Descriptor Index";
        case NanoAvbError::DescriptorNotFound:
            return "Descriptor Not Found";
        case NanoAvbError::DescriptorStorageFull:
            return "Descriptor Storage Full";
        case NanoAvbError::DescriptorTooLarge:
            return "Descriptor Too Large";
        case NanoAvbError::InvalidConfiguration:
            return "Invalid Configuration";
        case NanoAvbError::EntityNotInitialized:
            return "Entity Not Initialized";
        case NanoAvbError::CommandNotSupported:
            return "Command Not Supported";
        case NanoAvbError::InvalidStreamIndex:
            return "Invalid Stream Index";
        case NanoAvbError::StreamNotConnected:
            return "Stream Not Connected";
        case NanoAvbError::StreamAlreadyConnected:
            return "Stream Already Connected";
        case NanoAvbError::SrpRegistrationFailed:
            return "SRP Registration Failed";
        case NanoAvbError::InvalidVlanId:
            return "Invalid VLAN ID";
        case NanoAvbError::VlanTableFull:
            return "VLAN Table Full";
        default:
            return "Unknown Error";
    }
}

}  // namespace statusbar::nanoavb
