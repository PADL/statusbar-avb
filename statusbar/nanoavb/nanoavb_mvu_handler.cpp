// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

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
    our_entity_id_ = our_entity_id;

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

        case MVU_COMMAND_SET_SYSTEM_UNIQUE_ID:
            return set_system_unique_id_command(src_mac, header, command);

        case MVU_COMMAND_GET_SYSTEM_UNIQUE_ID: {
            // The reserved field is ignored, as for GET_MILAN_INFO.
            MvuSystemUniqueIdPayload response{};
            response.system_unique_id = system_unique_id_;
            return send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
        }

        case MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO:
            return set_mcr_info_command(src_mac, header, command);

        case MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO:
            return get_mcr_info_command(src_mac, header, command);

        default:
            return send_not_implemented(src_mac, command);
    }
}

void MvuCommandHandler::set_clock_domain_count(size_t const count) noexcept
{
    clock_domain_count_ = std::min(count, MAX_CLOCK_DOMAINS);
}

void MvuCommandHandler::set_default_mcr_priority(uint8_t const prio) noexcept
{
    for (auto& info : mcr_) {
        if (info.user_mcr_prio == default_mcr_prio_) {
            info.user_mcr_prio = prio;
        }
    }
    default_mcr_prio_ = prio;
}

auto MvuCommandHandler::mcr_info(size_t const index) const noexcept -> McrInfo const*
{
    return index < clock_domain_count_ ? &mcr_[index] : nullptr;
}

auto MvuCommandHandler::set_mcr_info(size_t const index, McrInfo const& info) noexcept -> bool
{
    if (index >= clock_domain_count_) {
        return false;
    }
    mcr_[index] = info;
    return true;
}

auto MvuCommandHandler::locked_against(MvuDu const& header) const noexcept -> bool
{
    return aem_ != nullptr && aem_->exclusive_access_status(header.common.controller_entity_id).has_value();
}

auto MvuCommandHandler::set_system_unique_id_command(Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command)
    -> bool
{
    auto const data = command.subspan(MvuDu::LENGTH);
    uint64_t requested = 0;
    // The full v1.3 payload (a u64 number where v1.2 has its u32, and a name,
    // not kept); anything shorter that holds the v1.2 id is v1.2.
    if (data.size() >= MvuSystemUniqueIdPayloadV13::LENGTH) {
        ieee::octlet_t number{0};
        span_load(number, data.subspan(offsetof(MvuSystemUniqueIdPayloadV13, number), sizeof(number)));
        requested = number.get();
    } else if (data.size() >= MvuSystemUniqueIdPayload::LENGTH) {
        MvuSystemUniqueIdPayload v12{};
        span_load(v12, data.first(MvuSystemUniqueIdPayload::LENGTH));
        requested = v12.system_unique_id.get();
    } else {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, {});
    }
    MvuSystemUniqueIdPayload response{};
    if (locked_against(header)) {
        response.system_unique_id = system_unique_id_;
        return send_response(src_mac, header, MVU_STATUS_ENTITY_LOCKED, make_const_span(response));
    }
    // Section 5.4.4.2: the new id differs from the default, 0; and it is a
    // v1.2 u32.
    if (requested == 0 || requested > UINT32_MAX) {
        response.system_unique_id = system_unique_id_;
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, make_const_span(response));
    }
    bool const changed = system_unique_id_ != static_cast<uint32_t>(requested);
    system_unique_id_ = static_cast<uint32_t>(requested);
    response.system_unique_id = system_unique_id_;
    bool const sent = send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
    if (changed) {
        notify_changed(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, header.common.controller_entity_id, make_const_span(response));
    }
    return sent;
}

auto MvuCommandHandler::mcr_payload(uint16_t const index) const noexcept -> MvuMediaClockReferenceInfoPayload
{
    MvuMediaClockReferenceInfoPayload payload{};
    payload.clock_domain_index = index;
    payload.flags = MVU_MCR_FLAGS_SUPPORTED;
    payload.default_mcr_prio = default_mcr_prio_;
    if (auto const* info = mcr_info(index); info != nullptr) {
        payload.user_mcr_prio = info->user_mcr_prio;
        payload.media_clock_domain_name = info->domain_name;
    }
    return payload;
}

auto MvuCommandHandler::get_mcr_info_command(Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command)
    -> bool
{
    auto const data = command.subspan(MvuDu::LENGTH);
    if (data.size() < MvuGetMediaClockReferenceInfoCommand::LENGTH) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, {});
    }
    MvuGetMediaClockReferenceInfoCommand request{};
    span_load(request, data.first(MvuGetMediaClockReferenceInfoCommand::LENGTH));
    uint16_t const index = request.clock_domain_index.get();
    if (index >= clock_domain_count_) {
        return send_response(src_mac, header, MVU_STATUS_NO_SUCH_DESCRIPTOR, make_const_span(request));
    }
    auto const response = mcr_payload(index);
    return send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
}

auto MvuCommandHandler::set_mcr_info_command(Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command)
    -> bool
{
    auto const data = command.subspan(MvuDu::LENGTH);
    if (data.size() < MvuMediaClockReferenceInfoPayload::LENGTH) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, data);
    }
    MvuMediaClockReferenceInfoPayload request{};
    span_load(request, data.first(MvuMediaClockReferenceInfoPayload::LENGTH));
    uint16_t const index = request.clock_domain_index.get();
    if (index >= clock_domain_count_) {
        return send_response(src_mac, header, MVU_STATUS_NO_SUCH_DESCRIPTOR, make_const_span(request));
    }
    if (locked_against(header)) {
        auto const current = mcr_payload(index);
        return send_response(src_mac, header, MVU_STATUS_ENTITY_LOCKED, make_const_span(current));
    }
    // Only the fields the flags mark valid change; default_mcr_prio is
    // read-only (ignored in a command).
    McrInfo info = mcr_[index];
    if ((request.flags.get() & MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID) != 0) {
        info.user_mcr_prio = request.user_mcr_prio.get();
    }
    if ((request.flags.get() & MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID) != 0) {
        info.domain_name = request.media_clock_domain_name;
    }
    bool const changed = !(info == mcr_[index]);
    mcr_[index] = info;
    auto const response = mcr_payload(index);
    bool const sent = send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
    if (changed) {
        notify_changed(MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, header.common.controller_entity_id, make_const_span(response));
    }
    return sent;
}

void MvuCommandHandler::notify_changed(uint16_t const command_type, Eui64 const& requester, std::span<uint8_t const> const body)
{
    if (aem_ != nullptr && send_response_) {
        std::array<uint8_t, MAX_AECP_PDU_LENGTH> buffer{};
        size_t const length = MvuDu::LENGTH + body.size();
        if (length <= buffer.size()) {
            // Milan v1.2 Section 5.4.5.1: one to each registered controller
            // but the one that asked, each with its own controller id and a
            // fresh sequence_id.
            aem_->for_each_unsolicited_registration([&](Eui48 const& mac, Eui64 const& controller) {
                if (controller == requester) {
                    return;
                }
                MvuDu header{};
                header.init_response(command_type, MVU_STATUS_SUCCESS, static_cast<uint16_t>(body.size()));
                header.common.target_entity_id = our_entity_id_;
                header.common.controller_entity_id = controller;
                header.common.sequence_id = unsolicited_sequence_id_++;
                header.set_unsolicited(true);
                span_store(std::span{buffer}.first(MvuDu::LENGTH), header);
                span_copy(std::span{buffer}.subspan(MvuDu::LENGTH, body.size()), body);
                (void)send_response_(mac, std::span<uint8_t const>{buffer}.first(std::max(length, MVU_MIN_PDU_LENGTH)));
            });
        }
    }
    if (on_settings_changed_) {
        on_settings_changed_();
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
