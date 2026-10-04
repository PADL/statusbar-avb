#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overloads for ATDECC AEM command/response payloads defined in
/// atdecc_aem_command.hpp. Split out so consumers that only need the data
/// structures do not pay the compile-time cost of <format>.

#include "statusbar/atdecc/atdecc_aem_command.hpp"

#include <format>

namespace statusbar::atdecc::aem {

/// Format SET_MAX_TRANSIT_TIME payload to an output iterator
/// @param out Output iterator to write formatted text to
/// @param payload SET_MAX_TRANSIT_TIME payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemSetMaxTransitTimePayload const& payload) -> OutputIt
{
    return std::format_to(
        out,
        "MAX_TRANSIT_TIME: desc_type={:#06x} desc_idx={} max_transit_time={} ns",
        payload.descriptor_type.get(),
        payload.descriptor_index.get(),
        payload.max_transit_time.get());
}

/// Format GET_MAX_TRANSIT_TIME command payload to an output iterator
/// @param out Output iterator to write formatted text to
/// @param payload GET_MAX_TRANSIT_TIME command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetMaxTransitTimeCommandPayload const& payload) -> OutputIt
{
    return std::format_to(
        out, "GET_MAX_TRANSIT_TIME: desc_type={:#06x} desc_idx={}", payload.descriptor_type.get(), payload.descriptor_index.get());
}

/// Format a single audio mapping entry
/// @param out Output iterator to write formatted text to
/// @param m Audio mapping entry to format
template <typename OutputIt>
auto format_to(OutputIt out, AemAudioMapping const& m) -> OutputIt
{
    return std::format_to(
        out,
        "stream_index={} stream_channel={} cluster_offset={} cluster_channel={}",
        m.stream_index.get(),
        m.stream_channel.get(),
        m.cluster_offset.get(),
        m.cluster_channel.get());
}

/// Format SET_CONTROL / GET_CONTROL / INCREMENT_CONTROL / DECREMENT_CONTROL
/// header only (the variable-length values region requires the full payload
/// span; see format_control in atdecc_aem_format.hpp for the full dump).
/// @param out Output iterator to write formatted text to
/// @param p Control payload header to format
template <typename OutputIt>
auto format_to(OutputIt out, AemControlPayloadHeader const& p) -> OutputIt
{
    return std::format_to(
        out, "        CONTROL header: desc_type={:#06x} desc_idx={}\n", p.descriptor_type.get(), p.descriptor_index.get());
}

/// Format READ_DESCRIPTOR response header (fixed portion only).
/// The variable-length descriptor body is rendered by the format_descriptor
/// dispatcher in atdecc_aem_format.hpp once the caller has the full payload
/// span.
/// @param out Output iterator to write formatted text to
/// @param p READ_DESCRIPTOR response payload header to format
template <typename OutputIt>
auto format_to(OutputIt out, AemReadDescriptorResponsePayload const& p) -> OutputIt
{
    return std::format_to(out, "        config_index={}\n", p.configuration_index.get());
}

/// Format SET_PTP_PORT_INFO / GET_PTP_PORT_INFO command payload (2021)
/// @param out Output iterator to write formatted text to
/// @param p PTP port info command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemPtpPortInfoCommandPayload const& p) -> OutputIt
{
    out = std::format_to(
        out, "        PTP_PORT_INFO command: desc_type={:#06x} desc_idx={}\n", p.descriptor_type.get(), p.descriptor_index.get());
    out = std::format_to(out, "          flags: {:#06x}\n", p.flags.get());
    out = std::format_to(
        out,
        "          delay_mechanism={:#04x} announce_to={} sync_to={} port_flags={:#04x}\n",
        static_cast<uint8_t>(p.delay_mechanism),
        static_cast<uint8_t>(p.announce_receipt_timeout),
        static_cast<uint8_t>(p.sync_receipt_timeout),
        static_cast<uint8_t>(p.port_flags));
    out = std::format_to(
        out,
        "          mean_link_delay_threshold={} delay_asymmetry={}\n",
        p.mean_link_delay_threshold.get(),
        p.delay_asymmetry.get());
    out = std::format_to(
        out,
        "          allowed_lost_responses={} allowed_faults={} gptp_capable_to={}\n",
        static_cast<uint8_t>(p.allowed_lost_responses),
        static_cast<uint8_t>(p.allowed_faults),
        static_cast<uint8_t>(p.gptp_capable_receipt_timeout));
    return out;
}

/// Format GET_PTP_PORT_INFO response payload (Figure 7-124, Cor 1-2025)
/// @param out Output iterator to write formatted text to
/// @param p GET_PTP_PORT_INFO response payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemGetPtpPortInfoResponsePayload const& p) -> OutputIt
{
    out = std::format_to(
        out, "        PTP_PORT_INFO response: desc_type={:#06x} desc_idx={}\n", p.descriptor_type.get(), p.descriptor_index.get());
    out = std::format_to(out, "          flags: {:#06x}\n", p.flags.get());
    out = std::format_to(
        out,
        "          delay_mechanism={:#04x} announce_to={} sync_to={} port_flags={:#04x}\n",
        static_cast<uint8_t>(p.delay_mechanism),
        static_cast<uint8_t>(p.announce_receipt_timeout),
        static_cast<uint8_t>(p.sync_receipt_timeout),
        static_cast<uint8_t>(p.port_flags));
    out = std::format_to(
        out,
        "          mean_link_delay_threshold={} delay_asymmetry={}\n",
        p.mean_link_delay_threshold.get(),
        p.delay_asymmetry.get());
    out = std::format_to(
        out,
        "          allowed_lost_responses={} allowed_faults={} gptp_capable_to={} port_state={:#04x}\n",
        static_cast<uint8_t>(p.allowed_lost_responses),
        static_cast<uint8_t>(p.allowed_faults),
        static_cast<uint8_t>(p.gptp_capable_receipt_timeout),
        static_cast<uint8_t>(p.port_state));
    out = std::format_to(
        out,
        "          mean_link_delay={} neighbor_rate_ratio={} ({:#018x})\n",
        p.mean_link_delay.get(),
        p.neighbor_rate_ratio_value(),
        p.neighbor_rate_ratio.get());
    out = std::format_to(
        out,
        "          version: major={} minor={} ext_port_flags={:#04x}\n",
        static_cast<uint8_t>(p.major_version),
        static_cast<uint8_t>(p.minor_version),
        static_cast<uint8_t>(p.ext_port_flags));
    return out;
}

/// Format START_OPERATION command payload (header only — variable-length
/// operation_data is dumped by format_start_operation in
/// atdecc_aem_format.hpp when the full payload span is available).
/// @param out Output iterator to write formatted text to
/// @param p START_OPERATION command payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemStartOperationCommandPayload const& p) -> OutputIt
{
    return std::format_to(
        out,
        "        START_OPERATION command: desc_type={:#06x} desc_idx={} operation_id={} operation_type={:#06x}\n",
        p.descriptor_type.get(),
        p.descriptor_index.get(),
        p.operation_id.get(),
        p.operation_type.get());
}

/// Format START_OPERATION response payload
/// @param out Output iterator to write formatted text to
/// @param p START_OPERATION response payload to format
template <typename OutputIt>
auto format_to(OutputIt out, AemStartOperationResponsePayload const& p) -> OutputIt
{
    return std::format_to(
        out,
        "        START_OPERATION response: desc_type={:#06x} desc_idx={} operation_id={} operation_type={:#06x}\n",
        p.descriptor_type.get(),
        p.descriptor_index.get(),
        p.operation_id.get(),
        p.operation_type.get());
}

}  // namespace statusbar::atdecc::aem
