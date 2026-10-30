// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace statusbar::nanoavb {

using namespace atdecc::mvu;  // NOLINT(google-build-using-namespace) MVU_* constants and payloads

namespace {

/// The largest AECPDU: the control header plus the largest control data
constexpr size_t MAX_AECP_PDU_LENGTH = AecpDuCommon::HEADER_LENGTH + AECP_MAX_CONTROL_DATA_LENGTH;

/// The MVU command in @p payload addressed to @p our_entity_id (its header in @p header), as
/// declared: control_data_length counts no padding. Empty when it is not one, or is malformed
/// (shorter than it declares, or declaring more than AECP allows).
[[nodiscard]] auto addressed_command(std::span<uint8_t const> const payload, Eui64 const& our_entity_id, MvuDu& header)
    -> std::span<uint8_t const>
{
    if (!is_mvu_command(payload)) {
        return {};
    }
    span_load(header, payload.first(MvuDu::LENGTH));
    if (header.common.target_entity_id != our_entity_id) {
        return {};
    }
    size_t const declared = AecpDuCommon::HEADER_LENGTH + header.common.control_data_length();
    if (declared > payload.size() || declared > MAX_AECP_PDU_LENGTH) {
        return {};
    }
    return payload.first(declared);
}

/// The requested id of a SET_SYSTEM_UNIQUE_ID's @p data; nullopt when too short. The full v1.3
/// payload (a u64 number where v1.2 has its u32, and a name, not kept); anything shorter that
/// holds the v1.2 id is v1.2.
[[nodiscard]] auto requested_system_unique_id(std::span<uint8_t const> const data) -> std::optional<uint64_t>
{
    if (data.size() >= MvuSystemUniqueIdPayloadV13::LENGTH) {
        ieee::octlet_t number{0};
        span_load(number, data.subspan(offsetof(MvuSystemUniqueIdPayloadV13, number), sizeof(number)));
        return number.get();
    }
    if (data.size() >= MvuSystemUniqueIdPayload::LENGTH) {
        MvuSystemUniqueIdPayload v12{};
        span_load(v12, data.first(MvuSystemUniqueIdPayload::LENGTH));
        return v12.system_unique_id.get();
    }
    return std::nullopt;
}

/// @p info with the fields @p request's flags mark valid; default_mcr_prio is read-only
/// (ignored in a command).
[[nodiscard]] auto merged_mcr_info(MvuCommandHandler::McrInfo info, MvuMediaClockReferenceInfoPayload const& request)
    -> MvuCommandHandler::McrInfo
{
    if ((request.flags.get() & MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID) != 0) {
        info.user_mcr_prio = request.user_mcr_prio.get();
    }
    if ((request.flags.get() & MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID) != 0) {
        info.domain_name = request.media_clock_domain_name;
    }
    return info;
}

/// Clear r, reserved and 0 in every MVU response (Section 5.4.3.2.2), in @p pdu's header.
void clear_reserved_r(std::span<uint8_t> const pdu)
{
    MvuDu header{};
    span_load(header, std::span<uint8_t const>{pdu}.first(MvuDu::LENGTH));
    header.set_command_type(header.command_type());
    span_store(pdu.first(MvuDu::LENGTH), header);
}

}  // namespace

auto MvuCommandHandler::process_packet(Eui48 const& src_mac, std::span<uint8_t const> const payload, Eui64 const& our_entity_id)
    -> bool
{
    MvuDu header{};
    auto const command = addressed_command(payload, our_entity_id, header);
    if (command.empty()) {
        return false;
    }
    our_entity_id_ = our_entity_id;
    // Not Milan: no reply to GET_MILAN_INFO (Milan v1.2 Section 5.4.4.1 NOTE), NOT_IMPLEMENTED
    // for the others.
    if (!profile_.milan) {
        if (header.command_type() == MVU_COMMAND_GET_MILAN_INFO) {
            return false;
        }
        return send_not_implemented(src_mac, command);
    }
    return dispatch_command(src_mac, header, command);
}

auto MvuCommandHandler::dispatch_command(Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command) -> bool
{
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
            auto const response = system_unique_id_payload();
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
    for (size_t index = 0; index < MAX_CLOCK_DOMAINS; ++index) {
        apply_default_mcr_priority(index, prio);
    }
}

auto MvuCommandHandler::set_default_mcr_priority(size_t const index, uint8_t const prio) noexcept -> bool
{
    if (index >= clock_domain_count_) {
        return false;
    }
    apply_default_mcr_priority(index, prio);
    return true;
}

void MvuCommandHandler::apply_default_mcr_priority(size_t const index, uint8_t const prio) noexcept
{
    if (mcr_[index].user_mcr_prio == default_mcr_prio_[index]) {
        mcr_[index].user_mcr_prio = prio;
    }
    default_mcr_prio_[index] = prio;
}

auto MvuCommandHandler::default_mcr_priority(size_t const index) const noexcept -> uint8_t
{
    return index < MAX_CLOCK_DOMAINS ? default_mcr_prio_[index] : MVU_MCR_PRIO_DEFAULT;
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

auto MvuCommandHandler::system_unique_id_payload() const noexcept -> MvuSystemUniqueIdPayload
{
    MvuSystemUniqueIdPayload payload{};
    payload.system_unique_id = system_unique_id_;
    return payload;
}

auto MvuCommandHandler::set_system_unique_id_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command) -> bool
{
    auto const requested = requested_system_unique_id(command.subspan(MvuDu::LENGTH));
    if (!requested) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, {});
    }
    return apply_system_unique_id(src_mac, header, *requested);
}

auto MvuCommandHandler::apply_system_unique_id(Eui48 const& src_mac, MvuDu const& header, uint64_t const requested) -> bool
{
    auto const current = system_unique_id_payload();
    if (locked_against(header)) {
        return send_response(src_mac, header, MVU_STATUS_ENTITY_LOCKED, make_const_span(current));
    }
    // Section 5.4.4.2: the new id differs from the default, 0; and it is a v1.2 u32.
    if (requested == 0 || requested > UINT32_MAX) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, make_const_span(current));
    }
    bool const changed = system_unique_id_ != static_cast<uint32_t>(requested);
    system_unique_id_ = static_cast<uint32_t>(requested);
    return respond_and_notify(src_mac, header, make_const_span(system_unique_id_payload()), changed);
}

auto MvuCommandHandler::mcr_payload(uint16_t const index) const noexcept -> MvuMediaClockReferenceInfoPayload
{
    MvuMediaClockReferenceInfoPayload payload{};
    payload.clock_domain_index = index;
    payload.flags = MVU_MCR_FLAGS_SUPPORTED;
    payload.default_mcr_prio = default_mcr_priority(index);
    if (auto const* info = mcr_info(index); info != nullptr) {
        payload.user_mcr_prio = info->user_mcr_prio;
        payload.media_clock_domain_name = info->domain_name;
    }
    return payload;
}

auto MvuCommandHandler::get_mcr_info_command(Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command)
    -> bool
{
    auto const request = load_payload<MvuGetMediaClockReferenceInfoCommand>(command.subspan(MvuDu::LENGTH));
    if (!request) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, {});
    }
    uint16_t const index = request->clock_domain_index.get();
    if (index >= clock_domain_count_) {
        return send_response(src_mac, header, MVU_STATUS_NO_SUCH_DESCRIPTOR, make_const_span(*request));
    }
    auto const response = mcr_payload(index);
    return send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
}

auto MvuCommandHandler::set_mcr_info_command(Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command)
    -> bool
{
    auto const data = command.subspan(MvuDu::LENGTH);
    auto const request = load_payload<MvuMediaClockReferenceInfoPayload>(data);
    if (!request) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, data);
    }
    uint16_t const index = request->clock_domain_index.get();
    if (index >= clock_domain_count_) {
        return send_response(src_mac, header, MVU_STATUS_NO_SUCH_DESCRIPTOR, make_const_span(*request));
    }
    return apply_mcr_info(src_mac, header, *request);
}

auto MvuCommandHandler::apply_mcr_info(Eui48 const& src_mac, MvuDu const& header, MvuMediaClockReferenceInfoPayload const& request)
    -> bool
{
    uint16_t const index = request.clock_domain_index.get();
    if (locked_against(header)) {
        auto const current = mcr_payload(index);
        return send_response(src_mac, header, MVU_STATUS_ENTITY_LOCKED, make_const_span(current));
    }
    McrInfo const info = merged_mcr_info(mcr_[index], request);
    bool const changed = !(info == mcr_[index]);
    mcr_[index] = info;
    return respond_and_notify(src_mac, header, make_const_span(mcr_payload(index)), changed);
}

auto MvuCommandHandler::respond_and_notify(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const body, bool const changed) -> bool
{
    bool const sent = send_response(src_mac, header, MVU_STATUS_SUCCESS, body);
    if (changed) {
        notify_changed(header.command_type(), header.common.controller_entity_id, body);
    }
    return sent;
}

void MvuCommandHandler::notify_changed(uint16_t const command_type, Eui64 const& requester, std::span<uint8_t const> const body)
{
    if (aem_ != nullptr && send_response_ && MvuDu::LENGTH + body.size() <= MAX_AECP_PDU_LENGTH) {
        MvuDu unsolicited{};
        unsolicited.init_response(command_type, MVU_STATUS_SUCCESS, static_cast<uint16_t>(body.size()));
        unsolicited.common.target_entity_id = our_entity_id_;
        unsolicited.set_unsolicited(true);
        // Milan v1.2 Section 5.4.5.1: one to each registered controller but the one that asked,
        // each with its own controller id and its own next MVU sequence_id.
        aem_->for_each_unsolicited_registration([&](Eui48 const& mac, Eui64 const& controller, uint16_t& sequence_id) {
            if (controller == requester) {
                return;
            }
            MvuDu header = unsolicited;
            header.common.controller_entity_id = controller;
            header.common.sequence_id = sequence_id++;
            (void)send_pdu(mac, header, body);
        });
    }
    if (on_settings_changed_) {
        on_settings_changed_();
    }
}

auto MvuCommandHandler::send_pdu(Eui48 const& dest_mac, MvuDu const& header, std::span<uint8_t const> const body) const -> bool
{
    std::array<uint8_t, MAX_AECP_PDU_LENGTH> buffer{};
    size_t const length = MvuDu::LENGTH + body.size();
    if (length > buffer.size()) {
        return false;
    }
    span_store(std::span{buffer}.first(MvuDu::LENGTH), header);
    span_copy(std::span{buffer}.subspan(MvuDu::LENGTH, body.size()), body);
    return send_response_(dest_mac, std::span<uint8_t const>{buffer}.first(std::max(length, MVU_MIN_PDU_LENGTH)));
}

auto MvuCommandHandler::send_response(
    Eui48 const& dest_mac, MvuDu const& command, uint8_t const status, std::span<uint8_t const> const body) const -> bool
{
    if (!send_response_) {
        return false;
    }
    MvuDu header = command;
    header.init_response(command.command_type(), status, static_cast<uint16_t>(body.size()));
    return send_pdu(dest_mac, header, body);
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
    clear_reserved_r(echo);
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
        // A valid AECP command from a registered controller restarts its
        // monitor timer (Milan v1.2 Section 5.4.5.3).
        if (payload.size() >= AecpDuCommon::LENGTH) {
            AecpDuCommon common{};
            span_load(common, payload.first(AecpDuCommon::LENGTH));
            if (common.target_entity_id == our_entity_id) {
                aem.note_controller_command(common.controller_entity_id, src_mac);
            }
        }
        return mvu.process_packet(src_mac, payload, our_entity_id);
    }
    return aem.process_packet(src_mac, payload, our_entity_id);
}

}  // namespace statusbar::nanoavb
