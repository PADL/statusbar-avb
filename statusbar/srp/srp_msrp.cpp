// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_msrp.hpp"

#include <string_view>

namespace statusbar::srp::msrp {

auto attribute_type_name(AttributeType type) noexcept -> std::string_view
{
    switch (type) {
        case AttributeType::TalkerAdvertise:
            return "TalkerAdvertise";
        case AttributeType::TalkerFailed:
            return "TalkerFailed";
        case AttributeType::Listener:
            return "Listener";
        case AttributeType::Domain:
            return "Domain";
        default:
            return "Unknown";
    }
}

auto listener_declaration_name(ListenerDeclaration decl) noexcept -> std::string_view
{
    switch (decl) {
        case ListenerDeclaration::Ignore:
            return "Ignore";
        case ListenerDeclaration::AskingFailed:
            return "AskingFailed";
        case ListenerDeclaration::Ready:
            return "Ready";
        case ListenerDeclaration::ReadyFailed:
            return "ReadyFailed";
        default:
            return "Unknown";
    }
}

auto failure_code_name(FailureCode code) noexcept -> std::string_view
{
    switch (code) {
        case FailureCode::NoFailure:
            return "NoFailure";
        case FailureCode::InsufficientBandwidth:
            return "InsufficientBandwidth";
        case FailureCode::InsufficientBridgeResources:
            return "InsufficientBridgeResources";
        case FailureCode::InsufficientBandwidthForTrafficClass:
            return "InsufficientBandwidthForTrafficClass";
        case FailureCode::StreamIdInUseByAnotherTalker:
            return "StreamIdInUseByAnotherTalker";
        case FailureCode::StreamDestinationAddressAlreadyInUse:
            return "StreamDestinationAddressAlreadyInUse";
        case FailureCode::StreamPreemptedByHigherRank:
            return "StreamPreemptedByHigherRank";
        case FailureCode::ReportedLatencyHasChanged:
            return "ReportedLatencyHasChanged";
        case FailureCode::EgressPortIsNotAvbCapable:
            return "EgressPortIsNotAvbCapable";
        case FailureCode::UseADifferentDestinationAddress:
            return "UseADifferentDestinationAddress";
        case FailureCode::OutOfMsrpResources:
            return "OutOfMsrpResources";
        case FailureCode::OutOfMmrpResources:
            return "OutOfMmrpResources";
        case FailureCode::CannotStoreDestinationAddress:
            return "CannotStoreDestinationAddress";
        case FailureCode::RequestedPriorityIsNotAnSrClass:
            return "RequestedPriorityIsNotAnSrClass";
        case FailureCode::MaxFrameSizeIsTooLargeForMedia:
            return "MaxFrameSizeIsTooLargeForMedia";
        case FailureCode::MsrpMaxFanInPortsLimitHasBeenReached:
            return "MsrpMaxFanInPortsLimitHasBeenReached";
        case FailureCode::ChangesInFirstValueForRegisteredStreamId:
            return "ChangesInFirstValueForRegisteredStreamId";
        case FailureCode::VlanIsBlockedOnThisEgressPort:
            return "VlanIsBlockedOnThisEgressPort";
        case FailureCode::VlanTaggingIsDisabledOnThisEgressPort:
            return "VlanTaggingIsDisabledOnThisEgressPort";
        case FailureCode::SrClassPriorityMismatch:
            return "SrClassPriorityMismatch";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::srp::msrp
