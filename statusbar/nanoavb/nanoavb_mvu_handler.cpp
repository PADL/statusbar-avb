// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>

namespace statusbar::nanoavb {

using namespace atdecc::mvu;  // NOLINT(google-build-using-namespace) MVU_* constants and payloads

namespace {

/// The largest AECPDU: the control header plus the largest control data
constexpr size_t MAX_AECP_PDU_LENGTH = AecpDuCommon::HEADER_LENGTH + AECP_MAX_CONTROL_DATA_LENGTH;

}  // namespace

auto MvuCommandHandler::process_packet(Eui48 const& src_mac, std::span<uint8_t const> const payload, Eui64 const& our_entity_id)
    -> bool
{
    if (!is_mvu_command(payload)) {
        return false;
    }
    MvuDu header{};
    span_load(header, payload.first(MvuDu::LENGTH));
    if (header.common.target_entity_id != our_entity_id) {
        return false;
    }
    // The command as declared: control_data_length counts no padding. A frame
    // shorter than it declares (or declaring more than AECP allows) is
    // malformed and dropped.
    size_t const declared = AecpDuCommon::HEADER_LENGTH + header.common.control_data_length();
    if (declared > payload.size() || declared > MAX_AECP_PDU_LENGTH) {
        return false;
    }
    auto const command = payload.first(declared);

    // Not Milan: no reply to GET_MILAN_INFO (Milan v1.2 Section 5.4.4.1 NOTE), NOT_IMPLEMENTED
    // for the others.
    if (!profile_.milan) {
        if (header.command_type() == MVU_COMMAND_GET_MILAN_INFO) {
            return false;
        }
        return send_not_implemented(src_mac, command);
    }

    switch (header.command_type()) {
        case MVU_COMMAND_GET_MILAN_INFO: {
            // The reserved field is ignored by the receiver (Figure 5.3), so a
            // command without it is answered all the same.
            MvuGetMilanInfoResponse response{};
            response.protocol_version = MVU_PROTOCOL_VERSION;
            // No redundancy (Section 8, one AVB interface), and no mapping
            // changes while a Stream Output is streaming.
            response.features_flags = uint32_t{0};
            response.certification_version = certification_version_;
            return send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
        }

        // Recommended in v1.2 and persisted: they come with the persistence
        // store.
        case MVU_COMMAND_SET_SYSTEM_UNIQUE_ID:
        case MVU_COMMAND_GET_SYSTEM_UNIQUE_ID:
        case MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO:
        case MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO:
        default:
            return send_not_implemented(src_mac, command);
    }
}

auto MvuCommandHandler::send_response(
    Eui48 const& dest_mac, MvuDu const& command, uint8_t const status, std::span<uint8_t const> const body) const -> bool
{
    if (!send_response_) {
        return false;
    }
    std::array<uint8_t, MAX_AECP_PDU_LENGTH> buffer{};
    size_t const length = MvuDu::LENGTH + body.size();
    if (length > buffer.size()) {
        return false;
    }
    MvuDu header = command;
    header.init_response(command.command_type(), status, static_cast<uint16_t>(body.size()));
    span_store(std::span{buffer}.first(MvuDu::LENGTH), header);
    span_copy(std::span{buffer}.subspan(MvuDu::LENGTH, body.size()), body);
    return send_response_(dest_mac, std::span<uint8_t const>{buffer}.first(std::max(length, MVU_MIN_PDU_LENGTH)));
}

auto MvuCommandHandler::send_not_implemented(Eui48 const& dest_mac, std::span<uint8_t const> const command) const -> bool
{
    if (!send_response_) {
        return false;
    }
    std::array<uint8_t, MAX_AECP_PDU_LENGTH> buffer{};
    auto const echo = aecp_echo_response(command, MVU_STATUS_NOT_IMPLEMENTED, buffer);
    if (echo.size() < MvuDu::LENGTH) {
        return false;
    }
    // r is reserved and 0 in every MVU response (Section 5.4.3.2.2)
    MvuDu header{};
    span_load(header, std::span<uint8_t const>{echo}.first(MvuDu::LENGTH));
    header.set_command_type(header.command_type());
    span_store(echo.first(MvuDu::LENGTH), header);
    return send_response_(dest_mac, std::span<uint8_t const>{buffer}.first(std::max(echo.size(), MVU_MIN_PDU_LENGTH)));
}

auto dispatch_aecp(
    AemCommandHandler& aem,
    MvuCommandHandler& mvu,
    Eui48 const& src_mac,
    std::span<uint8_t const> const payload,
    Eui64 const& our_entity_id) -> bool
{
    if (is_mvu_command(payload)) {
        return mvu.process_packet(src_mac, payload, our_entity_id);
    }
    return aem.process_packet(src_mac, payload, our_entity_id);
}

}  // namespace statusbar::nanoavb
