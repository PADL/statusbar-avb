// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include "statusbar/atdecc/atdecc_utf8.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

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

/// The MVU status of a binding answered @p acmp_status by the Milan listener (Milan v1.3 Tables
/// 5.33-5.41: a locked entity is ENTITY_LOCKED).
[[nodiscard]] constexpr auto binding_status(uint8_t const acmp_status) noexcept -> uint8_t
{
    switch (acmp_status) {
        case atdecc::ACMP_STATUS_SUCCESS:
            return MVU_STATUS_SUCCESS;
        case atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED:
            return MVU_STATUS_ENTITY_LOCKED;
        default:
            return MVU_STATUS_ENTITY_MISBEHAVING;
    }
}

/// What sink @p s reports of its binding, to tell whether a command changed it.
struct BindingState
{
    bool bound{false};
    atdecc::MilanSinkBinding binding{};

    auto operator==(BindingState const&) const -> bool = default;
};

[[nodiscard]] auto binding_state(atdecc::MilanSink const* const s) noexcept -> BindingState
{
    return s == nullptr ? BindingState{} : BindingState{.bound = s->bound(), .binding = s->binding};
}

/// A binding command's payload T from @p command, its reserved field cleared for the echo; nullopt when short.
template <typename T>
[[nodiscard]] auto binding_payload(std::span<uint8_t const> const command) -> std::optional<T>
{
    auto payload = load_payload<T>(command.subspan(MvuDu::LENGTH));
    if (payload) {
        payload->reserved = uint16_t{0};
    }
    return payload;
}

/// BIND_STREAM @p p from @p controller as the Milan listener receives it (Milan v1.3 Table 5.22).
[[nodiscard]] auto bind_request(Eui64 const& controller, MvuBindStreamPayload const& p) noexcept -> atdecc::MilanBindRequest
{
    return atdecc::MilanBindRequest{
        .controller_entity_id = controller,
        .talker_entity_id = p.talker_entity_id,
        .talker_unique_id = p.talker_stream_index.get(),
        .streaming_wait = (p.flags.get() & MVU_BIND_FLAGS_STREAMING_WAIT) != 0};
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

/// Whether the media_clock_domain_name @p request sets (its flag) is well-formed UTF-8 (an ATDECC string).
[[nodiscard]] auto mcr_name_well_formed(MvuMediaClockReferenceInfoPayload const& request) noexcept -> bool
{
    return (request.flags.get() & MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID) == 0 ||
        atdecc::utf8_well_formed(atdecc::atdecc_string_text(request.media_clock_domain_name));
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

auto MvuCommandHandler::process_packet(
    Eui48 const& src_mac, std::span<uint8_t const> const payload, Eui64 const& our_entity_id, sm::TimePoint const now) -> bool
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
    return dispatch_command(src_mac, header, command, now);
}

auto MvuCommandHandler::dispatch_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command, sm::TimePoint const now) -> bool
{  // NOLINT(statusbar-block-statements): one case per command type of Milan v1.3 Table 5.15
    std::array<uint8_t, MvuSystemUniqueIdPayloadV13::LENGTH> body{};
    switch (header.command_type()) {
        case MVU_COMMAND_GET_MILAN_INFO:
            // The reserved field is ignored by the receiver (Figure 5.5), so a
            // command without it is answered all the same.
            return send_response(src_mac, header, MVU_STATUS_SUCCESS, milan_info_payload(body));

        case MVU_COMMAND_SET_SYSTEM_UNIQUE_ID:
            return set_system_unique_id_command(src_mac, header, command);

        case MVU_COMMAND_GET_SYSTEM_UNIQUE_ID:
            // The reserved field is ignored, as for GET_MILAN_INFO.
            return send_response(src_mac, header, MVU_STATUS_SUCCESS, system_unique_id_payload(body));

        case MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO:
            return set_mcr_info_command(src_mac, header, command);

        case MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO:
            return get_mcr_info_command(src_mac, header, command);

        // Milan v1.3 Sections 5.4.4.6-8.
        case MVU_COMMAND_BIND_STREAM:
            return bind_stream_command(src_mac, header, command, now);

        case MVU_COMMAND_UNBIND_STREAM:
            return unbind_stream_command(src_mac, header, command, now);

        case MVU_COMMAND_GET_STREAM_INPUT_INFO_EX:
            return stream_input_info_ex_command(src_mac, header, command);

        default:
            return send_not_implemented(src_mac, command);
    }
}

auto MvuCommandHandler::features_flags() const noexcept -> uint32_t
{
    // What the application adds, and MVU binding wherever there are sinks to bind (Milan v1.3 Table 5.17).
    return extra_features_ | (bindable(0) ? MVU_FEATURES_MVU_BINDING : 0U);
}

auto MvuCommandHandler::milan_info_payload(std::span<uint8_t> const out) const noexcept -> std::span<uint8_t const>
{
    MvuGetMilanInfoResponseV13 const response{
        .info =
            {.protocol_version = quadlet_t{MVU_PROTOCOL_VERSION},
             .features_flags = quadlet_t{features_flags()},
             .certification_version = quadlet_t{certification_version_}},
        .specification_version = quadlet_t{MILAN_SPECIFICATION_VERSION}};
    span_store(out.first(MvuGetMilanInfoResponseV13::LENGTH), response);
    return out.first(MvuGetMilanInfoResponseV13::LENGTH);
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

auto MvuCommandHandler::system_unique_id_payload(std::span<uint8_t> const out) const noexcept -> std::span<uint8_t const>
{
    MvuSystemUniqueIdPayloadV13 const payload{.number = system_unique_id_, .name = system_unique_name_};
    span_store(out.first(MvuSystemUniqueIdPayloadV13::LENGTH), payload);
    return out.first(MvuSystemUniqueIdPayloadV13::LENGTH);
}

auto MvuCommandHandler::set_system_unique_id_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command) -> bool
{
    // Milan v1.3 Figure 5.7: a u64 number and a 64-octet name.
    auto const requested = load_payload<MvuSystemUniqueIdPayloadV13>(command.subspan(MvuDu::LENGTH));
    if (!requested) {
        return send_response(src_mac, header, MVU_STATUS_PAYLOAD_TOO_SHORT, {});
    }
    return apply_system_unique_id(src_mac, header, requested->number.get(), requested->name);
}

auto MvuCommandHandler::apply_system_unique_id(
    Eui48 const& src_mac, MvuDu const& header, uint64_t const number, std::array<char, 64> const& name) -> bool
{
    std::array<uint8_t, MvuSystemUniqueIdPayloadV13::LENGTH> body{};
    if (locked_against(header)) {
        return send_response(src_mac, header, MVU_STATUS_ENTITY_LOCKED, system_unique_id_payload(body));
    }
    if (!atdecc::utf8_well_formed(atdecc::atdecc_string_text(name))) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, system_unique_id_payload(body));
    }
    bool const changed = replace_system_unique_id(number, name);
    return respond_and_notify(src_mac, header, system_unique_id_payload(body), changed);
}

auto MvuCommandHandler::replace_system_unique_id(uint64_t const number, std::array<char, 64> const& name) noexcept -> bool
{
    auto const before = std::pair{system_unique_id_, system_unique_name_};
    set_system_unique_id(number, name);
    return std::pair{system_unique_id_, system_unique_name_} != before;
}

//
// Binding - Milan v1.3 Sections 5.4.4.6-8, 5.5.3.2
//

auto MvuCommandHandler::stream_input_refusal(uint16_t const type, uint16_t const index) const noexcept -> std::optional<uint8_t>
{
    if (type != atdecc::aem::DESCRIPTOR_STREAM_INPUT) {
        return MVU_STATUS_BAD_ARGUMENTS;
    }
    if (!bindable(index)) {
        return MVU_STATUS_NO_SUCH_DESCRIPTOR;
    }
    return std::nullopt;
}

auto MvuCommandHandler::bindable(uint16_t const index) const noexcept -> bool
{
    bool const described = !stream_input_count_ || index < *stream_input_count_;
    return listener_ != nullptr && listener_->sink(index) != nullptr && described;
}

auto MvuCommandHandler::answer_binding(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const body, uint8_t const acmp_status, bool const changed)
    -> bool
{
    uint8_t const status = binding_status(acmp_status);
    bool const sent = send_response(src_mac, header, status, body);
    // Milan v1.3 Section 5.4.5.3: a response that changed the binding notifies the others.
    if (status == MVU_STATUS_SUCCESS && changed) {
        send_unsolicited(header.command_type(), header.common.controller_entity_id, body);
    }
    return sent;
}

auto MvuCommandHandler::run_binding(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const body, uint16_t const index, BindingFn const& binding)
    -> bool
{
    auto const before = binding_state(listener_->sink(index));
    std::optional<bool> sent{};
    (void)binding([&](uint8_t const acmp_status) {
        bool const changed = !(binding_state(listener_->sink(index)) == before);
        sent = answer_binding(src_mac, header, body, acmp_status, changed);
    });
    return sent.has_value() ? *sent : send_response(src_mac, header, MVU_STATUS_ENTITY_MISBEHAVING, body);
}

template <typename Payload, typename Bind>
auto MvuCommandHandler::binding_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command, sm::TimePoint const now, Bind const& bind)
    -> bool
{
    auto const request = binding_payload<Payload>(command);
    if (!request) {
        return send_response(src_mac, header, MVU_STATUS_PAYLOAD_TOO_SHORT, {});
    }
    uint16_t const index = request->descriptor_index.get();
    if (auto const refusal = stream_input_refusal(request->descriptor_type.get(), index)) {
        return send_response(src_mac, header, *refusal, make_const_span(*request));
    }
    // The listener answers before it probes (Section 5.5.3.6.3 steps 3 and 5).
    return run_binding(src_mac, header, make_const_span(*request), index, [&](atdecc::MilanListener::ReplyFn const& reply) {
        return bind(index, *request, now, reply);
    });
}

auto MvuCommandHandler::bind_stream_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command, sm::TimePoint const now) -> bool
{
    return binding_command<MvuBindStreamPayload>(
        src_mac,
        header,
        command,
        now,
        [&](uint16_t const index, MvuBindStreamPayload const& request, sm::TimePoint const at, auto const& reply) {
            return listener_->bind_stream(index, bind_request(header.common.controller_entity_id, request), at, reply);
        });
}

auto MvuCommandHandler::unbind_stream_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command, sm::TimePoint const now) -> bool
{
    return binding_command<MvuStreamInputPayload>(
        src_mac,
        header,
        command,
        now,
        [&](uint16_t const index, MvuStreamInputPayload const&, sm::TimePoint const at, auto const& reply) {
            return listener_->unbind_stream(index, header.common.controller_entity_id, at, reply);
        });
}

auto MvuCommandHandler::stream_input_info_ex(uint16_t const index) const noexcept -> MvuStreamInputInfoExPayload
{
    auto const& s = *listener_->sink(index);
    MvuStreamInputInfoExPayload payload{
        .input = {.descriptor_type = doublet_t{atdecc::aem::DESCRIPTOR_STREAM_INPUT}, .descriptor_index = doublet_t{index}}};
    if (s.bound()) {
        payload.talker_entity_id = s.binding.talker_entity_id;
        payload.talker_unique_id = s.binding.talker_unique_id;
    }
    payload.set_probing(s.probing_status, s.acmp_status);
    return payload;
}

auto MvuCommandHandler::stream_input_info_ex_command(
    Eui48 const& src_mac, MvuDu const& header, std::span<uint8_t const> const command) -> bool
{
    auto const request = binding_payload<MvuStreamInputPayload>(command);
    if (!request) {
        return send_response(src_mac, header, MVU_STATUS_PAYLOAD_TOO_SHORT, {});
    }
    if (auto const refusal = stream_input_refusal(request->descriptor_type.get(), request->descriptor_index.get())) {
        MvuStreamInputInfoExPayload const refused{.input = *request};
        return send_response(src_mac, header, *refusal, make_const_span(refused));
    }
    auto const response = stream_input_info_ex(request->descriptor_index.get());
    return send_response(src_mac, header, MVU_STATUS_SUCCESS, make_const_span(response));
}

void MvuCommandHandler::notify_stream_input_info_ex(uint16_t const index)
{
    if (!profile_.milan || listener_ == nullptr || listener_->sink(index) == nullptr) {
        return;
    }
    auto const payload = stream_input_info_ex(index);
    send_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, Eui64{}, make_const_span(payload));
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
        return send_response(src_mac, header, MVU_STATUS_PAYLOAD_TOO_SHORT, {});
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
        return send_response(src_mac, header, MVU_STATUS_PAYLOAD_TOO_SHORT, data);
    }
    if (request->clock_domain_index.get() >= clock_domain_count_) {
        return send_response(src_mac, header, MVU_STATUS_NO_SUCH_DESCRIPTOR, make_const_span(*request));
    }
    if (!mcr_name_well_formed(*request)) {
        return send_response(src_mac, header, MVU_STATUS_BAD_ARGUMENTS, make_const_span(*request));
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
    send_unsolicited(command_type, requester, body);
    if (on_settings_changed_) {
        on_settings_changed_();
    }
}

void MvuCommandHandler::send_unsolicited(uint16_t const command_type, Eui64 const& requester, std::span<uint8_t const> const body)
{
    if (aem_ != nullptr && send_response_ && MvuDu::LENGTH + body.size() <= MAX_AECP_PDU_LENGTH) {
        MvuDu unsolicited{};
        unsolicited.init_response(command_type, MVU_STATUS_SUCCESS, static_cast<uint16_t>(body.size()));
        unsolicited.common.target_entity_id = our_entity_id_;
        unsolicited.set_unsolicited(true);
        // Milan v1.3 Sections 5.4.5.1-2: one to each registered controller but the one that asked,
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
    Eui64 const& our_entity_id,
    sm::TimePoint const now) -> bool
{
    aem.set_event_time(now);  // a LOCK or ACQUIRE deadline set by this command runs from now
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
        return mvu.process_packet(src_mac, payload, our_entity_id, now);
    }
    return aem.process_packet(src_mac, payload, our_entity_id);
}

}  // namespace statusbar::nanoavb
