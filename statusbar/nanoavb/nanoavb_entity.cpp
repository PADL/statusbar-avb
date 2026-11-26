// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_entity.hpp"

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_utf8.hpp"
#include "statusbar/avtp/avtp_stream_format.hpp"
#include "statusbar/avtp/avtp_types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

namespace {

/// Rewrite a freshly serialized IEEE 1722.1-2021 descriptor (`out[0..n)`) to
/// its 1722.1-2013/2016 wire form in place, returning the 2016 length. Only
/// the descriptor types whose fixed length grew in 2021 are altered; every
/// other type is returned unchanged. Used when the entity is configured for
/// `atdecc.version = "2016"` so controllers that reject 2021-length
/// descriptors can still enumerate the model.
[[nodiscard]] auto downgrade_descriptor_to_legacy_2016(uint16_t type, std::span<uint8_t> out, size_t n) -> size_t
{
    using namespace atdecc::aem;
    switch (type) {
        case DESCRIPTOR_STREAM_INPUT:
        case DESCRIPTOR_STREAM_OUTPUT: {
            // 2021 wire: [0..138) header + stream_formats[138..n). 2016 wire:
            // [0..132) header + formats[132..]. The three 2021-only fields
            // (redundant_offset / number_of_redundant_streams / timing) sit at
            // bytes 132..137 -- BEFORE the formats trailer -- so drop them and
            // slide the trailer down 6 bytes, and rewrite formats_offset
            // (big-endian, bytes 82..83) from 138 to 132.
            constexpr size_t L2021 = DescriptorStream::LENGTH;          // 138
            constexpr size_t L2016 = DescriptorStream::MINIMUM_LENGTH;  // 132
            constexpr size_t FORMATS_OFFSET_POS = 82;
            if (n < L2021) {
                return n;
            }
            size_t const trailer = n - L2021;
            span_store(out.subspan(FORMATS_OFFSET_POS, 2), doublet_t{L2016});
            if (trailer != 0) {
                std::memmove(out.data() + L2016, out.data() + L2021, trailer);
            }
            return L2016 + trailer;
        }
        // Tail-only 2021 additions, no variable trailer -> plain truncate.
        case DESCRIPTOR_AVB_INTERFACE:
            return (n >= DescriptorAvbInterface::LENGTH) ? DescriptorAvbInterface::MINIMUM_LENGTH : n;
        case DESCRIPTOR_AUDIO_CLUSTER:
            return (n >= DescriptorAudioCluster::LENGTH) ? DescriptorAudioCluster::MINIMUM_LENGTH : n;
        case DESCRIPTOR_CONTROL_BLOCK:
            return (n >= DescriptorControlBlock::LENGTH) ? DescriptorControlBlock::MINIMUM_LENGTH : n;
        case DESCRIPTOR_SIGNAL_TRANSCODER:
            return (n >= DescriptorSignalTranscoder::LENGTH) ? DescriptorSignalTranscoder::MINIMUM_LENGTH : n;
        default:
            return n;
    }
}

/// Whether @p payload is an AECP VENDOR_UNIQUE command: its common header says so.
[[nodiscard]] auto is_vendor_unique_command(std::span<uint8_t const> const payload) noexcept -> bool
{
    if (payload.size() < AecpDuCommon::LENGTH) {
        return false;
    }
    AecpDuCommon common{};
    span_load(common, payload);
    return common.message_type() == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND;
}

/// Whether @p payload is a defined non-AEM, non-vendor-unique AECP command type (IEEE 1722.1-2021
/// Table 9.1) addressed to @p our_entity_id; reserved types (10, 12) are not.
[[nodiscard]] auto is_other_aecp_command_for(std::span<uint8_t const> const payload, Eui64 const& our_entity_id) noexcept -> bool
{
    if (payload.size() < AecpDuCommon::LENGTH) {
        return false;
    }
    AecpDuCommon common{};
    span_load(common, payload.first(AecpDuCommon::LENGTH));
    auto const type = common.message_type();
    bool const defined = type == AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND || type == AECP_MESSAGE_TYPE_AVC_COMMAND ||
        type == AECP_MESSAGE_TYPE_HDCP_APM_COMMAND || type == AECP_MESSAGE_TYPE_EXTENDED_COMMAND;
    return common.subtype.get() == AvtpSubtype::aecp && defined && common.target_entity_id == our_entity_id;
}

/// GET_STREAM_INFO's octets after the first 48 in @p layout: Milan v1.2's @p ext, or IEEE
/// 1722.1-2021's IP fields, zero without IP transport (Clause 7.4.15.1).
void store_stream_info_tail(
    StreamInfoLayout const layout, AemStreamInfoMilanExtension const& ext, std::span<uint8_t> const tail) noexcept
{
    if (layout == StreamInfoLayout::milan_v1_2) {
        span_store(tail, ext);
        return;
    }
    if (layout == StreamInfoLayout::ieee_2021) {
        span_store(tail, AemStreamInfoIpExtension{});
    }
}

/// What SET_STREAM_INFO with sub-commands @p flags answers before setting the presentation
/// time offset, or nullopt to set it (Milan v1.2 Section 5.4.2.9).
[[nodiscard]] auto set_stream_info_refusal(uint32_t const flags) noexcept -> std::optional<uint8_t>
{
    if ((flags & stream_info_flags::SET_SUB_COMMANDS & ~stream_info_flags::MSRP_ACC_LAT_VALID) != 0) {
        return AEM_STATUS_NOT_SUPPORTED;  // any unsupported sub-command refuses the whole command
    }
    if ((flags & stream_info_flags::MSRP_ACC_LAT_VALID) == 0) {
        return AEM_STATUS_SUCCESS;  // nothing asked
    }
    return std::nullopt;
}

/// SET_STREAM_INFO's response length in @p layout: 84 octets in IEEE 1722.1-2021's, else 48
/// (Milan v1.2's extension is GET_STREAM_INFO's only).
[[nodiscard]] constexpr auto set_stream_info_length(StreamInfoLayout const layout) noexcept -> size_t
{
    return layout == StreamInfoLayout::ieee_2021 ? AEM_STREAM_INFO_2021_LENGTH : AemStreamInfoPayload::LENGTH;
}

/// SET_STREAM_INFO's response in @p layout's form at the head of @p out: @p command echoed,
/// zero-padded to 84 octets in IEEE 1722.1-2021's, else its 48. Empty when @p out is short.
[[nodiscard]] auto echo_stream_info(
    StreamInfoLayout const layout, std::span<uint8_t const> const command, std::span<uint8_t> const out) -> std::span<uint8_t const>
{
    size_t const size = set_stream_info_length(layout);
    if (out.size() < size) {
        return {};
    }
    std::ranges::fill(out.first(size), uint8_t{0});
    size_t const echoed = std::min(size, command.size());
    span_copy(out.first(echoed), command.first(echoed));
    return out.first(size);
}

/// The AEM header in @p payload when it is a valid AEM command addressed to
/// @p our_entity_id; nullopt for a response, another target, or a short or
/// malformed header.
[[nodiscard]] auto aem_command_for(std::span<uint8_t const> const payload, Eui64 const& our_entity_id) noexcept
    -> std::optional<AemDu>
{
    if (payload.size() < AemDu::LENGTH) {
        return std::nullopt;
    }
    AemDu header{};
    span_load(header, payload);
    if (!header.is_valid() || !header.is_command() || header.target_entity_id != our_entity_id) {
        return std::nullopt;
    }
    return header;
}

/// The AEM header in @p payload when it is a response to a command this entity
/// sent as a controller (its controller_entity_id is ours); nullopt otherwise.
[[nodiscard]] auto aem_response_for(std::span<uint8_t const> const payload, Eui64 const& our_entity_id) noexcept
    -> std::optional<AemDu>
{
    if (payload.size() < AemDu::LENGTH || our_entity_id == Eui64{}) {
        return std::nullopt;
    }
    AemDu header{};
    span_load(header, payload);
    if (!header.is_valid() || !header.is_response() || header.controller_entity_id != our_entity_id) {
        return std::nullopt;
    }
    return header;
}

/// A GET_AS_PATH response (IEEE 1722.1-2021 Clause 7.4.41.2) for @p descriptor_index in
/// @p out_buffer: the header, then @p path's clock identities; NOT_SUPPORTED when it does not fit.
[[nodiscard]] auto as_path_response(
    uint16_t const descriptor_index, AsPath const& path, std::span<uint8_t> const out_buffer) noexcept -> AemCommandResponse
{
    size_t const size = AemAsPathResponseHeader::LENGTH + (path.size() * Eui64::LENGTH);
    if (out_buffer.size() < size) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    span_store(
        out_buffer, AemAsPathResponseHeader{.descriptor_index = descriptor_index, .count = static_cast<uint16_t>(path.size())});
    for (size_t i = 0; i < path.size(); ++i) {
        span_store(out_buffer.subspan(AemAsPathResponseHeader::LENGTH + (i * Eui64::LENGTH)), path[i]);
    }
    return {.status = AEM_STATUS_SUCCESS, .size = size};
}

/// The leading T of @p bytes, nullopt when they are too few.
template <typename T>
[[nodiscard]] auto load_payload(std::span<uint8_t const> const bytes) noexcept -> std::optional<T>
{
    if (bytes.size() < T::LENGTH) {
        return std::nullopt;
    }
    T value{};
    span_load(value, bytes);
    return value;
}

/// The command data of the AEM command @p header in @p payload: as long as its
/// control_data_length says (IEEE 1722.1-2021 Clause 9.2.1.1.7), not the octets behind it.
[[nodiscard]] auto aem_command_data(AemDu const& header, std::span<uint8_t const> const payload) noexcept
    -> std::span<uint8_t const>
{
    size_t const declared = size_t{header.control_data_length()} - AemDu::AEM_DATA_LENGTH;
    return payload.subspan(AemDu::LENGTH).first(std::min(payload.size() - AemDu::LENGTH, declared));
}

}  // namespace

auto AemCommandHandler::answer_other_than_aem(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id)
    -> std::optional<bool>
{
    // A vendor-unique command is Milan's to answer, or nobody's (9.6).
    if (is_vendor_unique_command(payload)) {
        return process_mvu_packet(src_mac, payload, our_entity_id);
    }
    // Any other AECP command type addressed to us is answered NOT_IMPLEMENTED
    // rather than dropped (IEEE 1722.1-2021 Clause 9.2.1.1.6).
    if (is_other_aecp_command_for(payload, our_entity_id)) {
        return send_not_implemented(src_mac, payload);
    }
    return std::nullopt;
}

auto AemCommandHandler::process_packet(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id) -> bool
{
    // A vendor-unique or other non-AEM command: the AEM header check would only drop it.
    if (auto const answered = answer_other_than_aem(src_mac, payload, our_entity_id)) {
        return *answered;
    }
    if (auto const response = aem_response_for(payload, our_entity_id)) {
        handle_aem_response(*response);
        return false;
    }
    auto const header = aem_command_for(payload, our_entity_id);
    if (!header) {
        return false;
    }
    restart_controller_monitor(header->controller_entity_id);
    return process_aem_command(src_mac, *header, aem_command_data(*header, payload), our_entity_id);
}

auto AemCommandHandler::process_aem_command(
    Eui48 const& src_mac, AemDu const& header, std::span<uint8_t const> const command_data, Eui64 const& our_entity_id) -> bool
{
    // Keep our id current for unsolicited notifications (their target_entity_id).
    our_entity_id_ = our_entity_id;
    // One output buffer per dispatch — handlers write into this and return
    // status + bytes-written, avoiding the per-return 1 KB struct copy that
    // the previous AemCommandResult shape incurred.
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> out_buffer{};
    auto const response = dispatch_aem_command(src_mac, header, command_data, out_buffer);
    return answer_aem_command(src_mac, header, response, std::span<uint8_t const>{out_buffer.data(), response.size});
}

auto AemCommandHandler::dispatch_aem_command(
    Eui48 const& src_mac, AemDu const& header, std::span<uint8_t const> const command_data, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{  // NOLINT(statusbar-block-statements) the per-dispatch state, set and cleared around the one call
    // Stash the source MAC so per-command handlers can retain it if they
    // need to emit an out-of-band late response (e.g. ACQUIRE_ENTITY's
    // CONTROLLER_AVAILABLE handshake). Cleared after dispatch.
    current_src_mac_ = src_mac;
    setting_unchanged_ = false;  // here, not in handle_command: a read_back() dispatches too
    dispatching_ = true;
    auto const response = handle_command(header, command_data, out_buffer);
    dispatching_ = false;
    current_src_mac_ = {};
    return response;
}

auto AemCommandHandler::answer_aem_command(
    Eui48 const& src_mac, AemDu const& header, AemCommandResponse const& response, std::span<uint8_t const> const body) -> bool
{
    // The response, then the notifications the command raised (Milan v1.3 Section 5.4.5.3).
    bool const sent = send_response(src_mac, header, response.status, body);
    flush_notifications();
    run_after_response(header.controller_entity_id);
    // Then let the entity save a changed setting (after the response: a
    // write to non-volatile storage must not delay it).
    if (response.status == AEM_STATUS_SUCCESS && !setting_unchanged_) {
        report_setting_changed(header.command_code());
    }
    return sent;
}

void AemCommandHandler::run_after_response(Eui64 const& requester)
{
    // As the command's dispatch would: its controller is left out of what the work notifies.
    Eui64 const outer = std::exchange(requester_, requester);
    for (auto& fn : std::exchange(after_response_, {})) {
        fn();
    }
    requester_ = outer;
}

void AemCommandHandler::call_after_response(AfterResponseFn fn)
{
    if (!dispatching_ || after_response_.size() == MAX_AFTER_RESPONSE) {
        fn();
        return;
    }
    after_response_.push_back(std::move(fn));
}

auto AemCommandHandler::handle_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    // The command's controller is the requester for the unsolicited fan-out
    // this dispatch makes (IEEE 1722.1-2021 7.5.2), and for that alone: what the
    // entity notifies later on its own (a lock expiring, a front-panel change,
    // a timer) is for every registered controller, this one included. A
    // dispatch nested in another (read_back's GET twin) hands back the
    // requester it found, so the outer command's fan-out still leaves out its own.
    Eui64 const outer_requester = requester_;
    current_header_ = header;
    requester_ = header.controller_entity_id;
    auto const response = dispatch_command(header, command_data, out_buffer);
    requester_ = outer_requester;
    return response;
}

auto AemCommandHandler::dispatch_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = header.command_code();

    switch (cmd) {
        case AEM_COMMAND_READ_DESCRIPTOR:
            return handle_read_descriptor(header, command_data, out_buffer);

        case AEM_COMMAND_GET_NAME:
            return handle_get_name(command_data, out_buffer);

        case AEM_COMMAND_SET_NAME:
            if (auto const blocked = check_exclusive_access(header)) {
                return refused_set(*blocked, AEM_COMMAND_SET_NAME, command_data, out_buffer);
            }
            return handle_set_name(command_data, out_buffer);

        case AEM_COMMAND_ENTITY_AVAILABLE:
            return handle_entity_available(header);

        case AEM_COMMAND_ACQUIRE_ENTITY: {
            auto const r = handle_acquire_entity(header, command_data, out_buffer);
            notify_on_success(AEM_COMMAND_ACQUIRE_ENTITY, r, out_buffer);
            return r;
        }

        case AEM_COMMAND_LOCK_ENTITY: {
            auto const r = handle_lock_entity(header, command_data, out_buffer);
            notify_on_success(AEM_COMMAND_LOCK_ENTITY, r, out_buffer);
            return r;
        }

        case AEM_COMMAND_REBOOT: {
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            auto const r = handle_reboot(command_data, out_buffer);
            notify_on_success(AEM_COMMAND_REBOOT, r, out_buffer);
            return r;
        }

        case AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH: {
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            auto const r = handle_set_memory_object_length(command_data, out_buffer);
            notify_on_success(AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH, r, out_buffer);
            return r;
        }

        case AEM_COMMAND_GET_MEMORY_OBJECT_LENGTH:
            return handle_get_memory_object_length(command_data, out_buffer);

        case AEM_COMMAND_GET_DYNAMIC_INFO:
            return handle_get_dynamic_info(header, command_data, out_buffer);

        case AEM_COMMAND_GET_CONFIGURATION:
            return handle_get_configuration(header, out_buffer);

        case AEM_COMMAND_SET_CONFIGURATION:
            if (auto const blocked = check_exclusive_access(header)) {
                return refused_set(*blocked, AEM_COMMAND_SET_CONFIGURATION, command_data, out_buffer);
            }
            return answer_set_configuration(command_data, out_buffer);

        case AEM_COMMAND_CONTROLLER_AVAILABLE:
            return handle_controller_available(header);

        case AEM_COMMAND_SET_CONTROL:
            // IDENTIFY is a SET_CONTROL like any other under 7.4.25.2: a held
            // entity refuses it from every other controller, unless the kit was
            // asked to let identify through (set_identify_bypasses_exclusive_access).
            if (identify_bypasses_exclusive_access_ && targets_identify_control(AEM_COMMAND_SET_CONTROL, command_data)) {
                return handle_set_descriptor_value(AEM_COMMAND_SET_CONTROL, command_data, out_buffer);
            }
            return guarded_set(header, AEM_COMMAND_SET_CONTROL, command_data, out_buffer);

        case AEM_COMMAND_GET_CONTROL:
            return handle_get_descriptor_value(AEM_COMMAND_GET_CONTROL, command_data, out_buffer);

        case AEM_COMMAND_SET_SIGNAL_SELECTOR:
            return guarded_set(header, AEM_COMMAND_SET_SIGNAL_SELECTOR, command_data, out_buffer);

        case AEM_COMMAND_GET_SIGNAL_SELECTOR:
            return handle_get_descriptor_value(AEM_COMMAND_GET_SIGNAL_SELECTOR, command_data, out_buffer);

        case AEM_COMMAND_SET_MATRIX:
            return guarded_set(header, AEM_COMMAND_SET_MATRIX, command_data, out_buffer);

        case AEM_COMMAND_GET_MATRIX:
            return handle_get_descriptor_value(AEM_COMMAND_GET_MATRIX, command_data, out_buffer);

        case AEM_COMMAND_SET_MIXER:
            return guarded_set(header, AEM_COMMAND_SET_MIXER, command_data, out_buffer);

        case AEM_COMMAND_GET_MIXER:
            return handle_get_descriptor_value(AEM_COMMAND_GET_MIXER, command_data, out_buffer);

        case AEM_COMMAND_SET_CLOCK_SOURCE:
            return guarded_set(header, AEM_COMMAND_SET_CLOCK_SOURCE, command_data, out_buffer);

        case AEM_COMMAND_GET_CLOCK_SOURCE:
            return handle_get_descriptor_value(AEM_COMMAND_GET_CLOCK_SOURCE, command_data, out_buffer);

        case AEM_COMMAND_SET_STREAM_FORMAT:
            return guarded_set(header, AEM_COMMAND_SET_STREAM_FORMAT, command_data, out_buffer);

        case AEM_COMMAND_SET_SAMPLING_RATE:
            return guarded_set(header, AEM_COMMAND_SET_SAMPLING_RATE, command_data, out_buffer);

        case AEM_COMMAND_START_STREAMING:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_START_STREAMING, command_data, out_buffer);

        case AEM_COMMAND_STOP_STREAMING:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_STOP_STREAMING, command_data, out_buffer);

        case AEM_COMMAND_SET_STREAM_INFO:
            if (auto const blocked = check_exclusive_access(header)) {
                return refused_set(*blocked, AEM_COMMAND_SET_STREAM_INFO, command_data, out_buffer);
            }
            return stream_info_set_answer(handle_set_stream_info(command_data, out_buffer), command_data, out_buffer);

        case AEM_COMMAND_SET_MAX_TRANSIT_TIME:
            if (auto const blocked = check_exclusive_access(header)) {
                return refused_set(*blocked, AEM_COMMAND_SET_MAX_TRANSIT_TIME, command_data, out_buffer);
            }
            return handle_set_max_transit_time(command_data, out_buffer);

        case AEM_COMMAND_GET_MAX_TRANSIT_TIME:
            return handle_get_max_transit_time(command_data, out_buffer);

        case AEM_COMMAND_GET_AUDIO_MAP:
            return handle_get_audio_map(command_data, out_buffer);

        case AEM_COMMAND_ADD_AUDIO_MAPPINGS:
        case AEM_COMMAND_REMOVE_AUDIO_MAPPINGS:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_audio_mappings(cmd == AEM_COMMAND_ADD_AUDIO_MAPPINGS, command_data, out_buffer);

        case AEM_COMMAND_GET_COUNTERS:
            return handle_get_counters(header, command_data, out_buffer);

        case AEM_COMMAND_GET_AVB_INFO:
            return handle_get_avb_info(command_data, out_buffer);

        case AEM_COMMAND_GET_AS_PATH:
            return handle_get_as_path(command_data, out_buffer);

        case AEM_COMMAND_GET_STREAM_INFO:
            return handle_get_stream_info(header, command_data, out_buffer);

        case AEM_COMMAND_GET_STREAM_FORMAT:
            return handle_get_stream_format(header, command_data, out_buffer);

        case AEM_COMMAND_GET_SAMPLING_RATE:
            return handle_get_sampling_rate(header, command_data, out_buffer);

        case AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
            return handle_register_unsolicited(header);

        case AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION: {
            // IEEE 1722.1-2021 7.5.2 lists the command, so under classic the
            // remaining registered controllers learn that this one left. Not
            // under Milan: v1.3 5.4.5.1 addresses every copy with the
            // recipient's own controller_entity_id, so a copy of A's
            // deregistration reaches B as a successful DEREGISTER of B's own
            // and B drops its subscription while the entity still holds it;
            // Table 5.20 sends a deregistration only to the controller removed.
            auto const r = handle_deregister_unsolicited(header);
            if (profile_ != Profile::milan13) {
                notify_on_success(AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, r, out_buffer);
            }
            return r;
        }

        default:
            return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
}

auto AemCommandHandler::handle_read_descriptor(
    AemDu const& /*header*/, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // READ_DESCRIPTOR command payload (AemReadDescriptorCommandPayload):
    //   configuration_index(2) + reserved(2) + descriptor_type(2) + descriptor_index(2)
    if (command_data.size() < AemReadDescriptorCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }

    AemReadDescriptorCommandPayload cmd{};
    span_load(cmd, command_data);
    uint16_t const config_index = cmd.configuration_index;   // doublet_t -> host order
    uint16_t const descriptor_type = cmd.descriptor_type;    // doublet_t -> host order
    uint16_t const descriptor_index = cmd.descriptor_index;  // doublet_t -> host order

    // Response payload (IEEE 1722.1 Clause 7.4.5.2): a fixed
    // AemReadDescriptorResponsePayload header (configuration_index + reserved),
    // then the descriptor. The descriptor's OWN first fields are
    // descriptor_type + descriptor_index, which get_descriptor_for_wire() emits
    // — so we must NOT write type/index here as well. Doing so would shift the
    // descriptor body 4 bytes (entity_id at offset 12 instead of 8,
    // configurations_count reading as 0, and so on).

    // Need room for the header plus at least a minimal descriptor payload.
    if (out_buffer.size() <= AemReadDescriptorResponsePayload::LENGTH) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }

    // Echo back the configuration index; reserved stays zero.
    AemReadDescriptorResponsePayload const resp{.configuration_index = config_index};
    span_store(out_buffer, resp);

    // Dispatch to a locally-constructed AemEntityModel, which routes by
    // descriptor_type through a constexpr table to the appropriate
    // AemEntityHandler method. The handler_ pointer resolves to either
    // an internally-owned EntityModelAdapter (when this object was built
    // from a legacy EntityModel) or a caller-supplied AemEntityHandler.
    // Constructing the model per-call is free — it is two pointers plus
    // an optional — and avoids a dangling self-reference across moves.
    AemEntityModel const aem_model{*handler_};
    DescriptorRef const ref{
        .configuration_index = config_index, .descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
    auto const desc_out = out_buffer.subspan(AemReadDescriptorResponsePayload::LENGTH);
    auto bytes_written = aem_model.get_descriptor_for_wire(ref, desc_out);
    if (bytes_written == 0) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }

    // Optionally downgrade 2021-length descriptors to their 2013/2016 wire
    // sizes for controllers that reject the 2021 forms (atdecc.version=2016).
    if (legacy_2016_) {
        bytes_written = downgrade_descriptor_to_legacy_2016(descriptor_type, desc_out, bytes_written);
    }

    return {.status = AEM_STATUS_SUCCESS, .size = AemReadDescriptorResponsePayload::LENGTH + bytes_written};
}

auto AemCommandHandler::handle_get_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    auto const ref = name_ref_of(command_data);
    if (!ref) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    // Per-call model; symbols resolve through the handler's storage.
    AemEntityModel const aem_model{*handler_};
    auto const n = aem_model.get_name_for_wire(*ref, out_buffer);
    if (n == 0) {
        // No name for this (descriptor, name_index): because there is no such
        // descriptor, or because the handler has none to give for it.
        return {.status = unserved_get_status(ref->descriptor), .size = 0};
    }
    return {.status = AEM_STATUS_SUCCESS, .size = n};
}

auto AemCommandHandler::check_exclusive_access(AemDu const& header) const noexcept -> std::optional<uint8_t>
{
    return exclusive_access_status(header.controller_entity_id);
}

auto AemCommandHandler::exclusive_access_status(Eui64 const& controller_entity_id) const noexcept -> std::optional<uint8_t>
{
    // ENTITY_ACQUIRED takes precedence over ENTITY_LOCKED (an acquisition is the
    // stronger claim). Either held by a controller OTHER than the sender blocks the
    // mutation; the owning controller (or a free entity) proceeds. An
    // acquisition shuts nobody out under AcquireMode::shared.
    if (acquisition_shuts_out(controller_entity_id)) {
        return AEM_STATUS_ENTITY_ACQUIRED;
    }
    if (locked_ && !(locking_controller_ == controller_entity_id)) {
        return AEM_STATUS_ENTITY_LOCKED;
    }
    return std::nullopt;
}

auto AemCommandHandler::reject_command(uint8_t const status, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    return {.status = status, .size = echo_command(command_data, out_buffer)};
}

auto AemCommandHandler::echo_command(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> size_t
{
    if (out_buffer.size() < command_data.size()) {
        return 0;
    }
    span_copy(out_buffer.first(command_data.size()), command_data);
    return command_data.size();
}

auto AemCommandHandler::handle_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    setting_unchanged_ = name_in_force(command_data);  // a rename to the name in force: nothing to save
    // Per-call model; the handler applies the name by (descriptor, name_index,
    // symbol). A name that was set is echoed back; one that was not is answered
    // with the name in force (7.4.17.1). Either is zero-filled after its first NUL.
    std::array<uint8_t, MAX_AEM_BODY> zero_filled{};
    auto const command = name_zero_filled(command_data, zero_filled);
    if (auto const as_is = unchanged_set_answer(AEM_COMMAND_SET_NAME, command, out_buffer)) {
        return *as_is;
    }
    auto const status = AemEntityModel{*handler_}.apply_set_name(command);
    if (status != AEM_STATUS_SUCCESS) {
        return refused_set(status, AEM_COMMAND_SET_NAME, command, out_buffer);
    }
    return applied_set_name(command, out_buffer);
}

auto AemCommandHandler::applied_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // A controller renamed a descriptor: notify every registered controller,
    // like handle_set_descriptor_value does for values, with the name in force
    // (an empty one is the configured or authored name again).
    size_t const size = echo_command(command_data, out_buffer);
    if (auto const ref = name_ref_of(command_data)) {
        AemEntityModel const aem_model{*handler_};
        (void)aem_model.get_name_for_wire(*ref, out_buffer);
    }
    emit_unsolicited(AEM_COMMAND_SET_NAME, out_buffer.first(size));
    return {.status = AEM_STATUS_SUCCESS, .size = size};
}

auto AemCommandHandler::answered_before_hook(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> std::optional<AemCommandResponse>
{
    setting_unchanged_ = is_persistent_setting_command(command_type) && value_is_in_force(command_type, command_data);
    // A controller that heard nothing for 250 ms sends its command again
    // (9.2.2.3.1.2) under the same sequence_id: the one already deferred is
    // answered IN_PROGRESS once more from its slot, its hook not run twice.
    if (auto const* repeat = repeated_pending(command_type)) {
        std::copy_n(repeat->body.begin(), repeat->body_size, out_buffer.begin());
        return AemCommandResponse{.status = AEM_STATUS_IN_PROGRESS, .size = repeat->body_size};
    }
    // A hook that answers IN_PROGRESS needs a pending slot to be finished from,
    // so the slot is spoken for before the hook runs: with none free the SET is
    // refused NO_RESOURCES here, and no work is begun that nothing could report.
    if (pending_commands_.size() >= pending_commands_.capacity()) {
        return refused_set(AEM_STATUS_NO_RESOURCES, command_type, command_data, out_buffer);
    }
    if (auto const refusal = mappings_refusal(command_type, command_data)) {
        return refused_set(*refusal, command_type, command_data, out_buffer);
    }
    return unchanged_set_answer(command_type, command_data, out_buffer);
}

auto AemCommandHandler::unchanged_set_answer(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> std::optional<AemCommandResponse>
{
    if (!set_leaves_value_as_is(command_type, command_data)) {
        return std::nullopt;
    }
    return AemCommandResponse{.status = AEM_STATUS_SUCCESS, .size = echo_command(command_data, out_buffer)};
}

auto AemCommandHandler::guarded_set(
    AemDu const& header, uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (auto const blocked = check_exclusive_access(header)) {
        return refused_set(*blocked, command_type, command_data, out_buffer);
    }
    return handle_set_descriptor_value(command_type, command_data, out_buffer);
}

auto AemCommandHandler::set_leaves_value_as_is(uint16_t const command_type, std::span<uint8_t const> command_data) -> bool
{
    if (profile_ != Profile::milan13) {
        return false;
    }
    // The GET twin's response has the SET body's shape (the descriptor head and
    // the value, the region header too for a matrix), so the two compare whole.
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> in_force{};
    auto const n = value_in_force(command_type, command_data, in_force);
    return n == command_data.size() && std::equal(command_data.begin(), command_data.end(), in_force.begin());
}

auto AemCommandHandler::refused_set(
    uint8_t const status, uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    return {.status = status, .size = failed_set_body(command_type, command_data, out_buffer)};
}

auto AemCommandHandler::failed_set_body(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> size_t
{
    // 7.4.9.1 and its siblings: a SET's response "always contains the current
    // value, that is ... the old value if it fails". The value in force is read
    // the way GET reads it; a descriptor that cannot be read back is answered
    // with the command echoed, so the response keeps the command's shape.
    auto const n = value_in_force(command_type, command_data, out_buffer);
    return n > 0 ? n : echo_command(command_data, out_buffer);
}

auto AemCommandHandler::get_twin_of(uint16_t const set_command_type) noexcept -> std::optional<uint16_t>
{
    constexpr std::array<std::pair<uint16_t, uint16_t>, 11> twins{
        {{AEM_COMMAND_SET_CONFIGURATION, AEM_COMMAND_GET_CONFIGURATION},
         {AEM_COMMAND_SET_NAME, AEM_COMMAND_GET_NAME},
         {AEM_COMMAND_SET_CONTROL, AEM_COMMAND_GET_CONTROL},
         {AEM_COMMAND_SET_STREAM_FORMAT, AEM_COMMAND_GET_STREAM_FORMAT},
         {AEM_COMMAND_SET_SAMPLING_RATE, AEM_COMMAND_GET_SAMPLING_RATE},
         {AEM_COMMAND_SET_CLOCK_SOURCE, AEM_COMMAND_GET_CLOCK_SOURCE},
         {AEM_COMMAND_SET_SIGNAL_SELECTOR, AEM_COMMAND_GET_SIGNAL_SELECTOR},
         {AEM_COMMAND_SET_MIXER, AEM_COMMAND_GET_MIXER},
         {AEM_COMMAND_SET_MATRIX, AEM_COMMAND_GET_MATRIX},
         {AEM_COMMAND_SET_STREAM_INFO, AEM_COMMAND_GET_STREAM_INFO},  // 7.4.15.1, cut to the SET's length
         {AEM_COMMAND_SET_MAX_TRANSIT_TIME, AEM_COMMAND_GET_MAX_TRANSIT_TIME}}};
    auto const it = std::find_if(twins.begin(), twins.end(), [=](auto const& pair) { return pair.first == set_command_type; });
    return it == twins.end() ? std::nullopt : std::optional<uint16_t>{it->second};
}

auto AemCommandHandler::get_twin_request_size(uint16_t const set_command_type) noexcept -> size_t
{
    // The GET twin asks with the SET command's head: descriptor_type/index for
    // most, plus the name_index/configuration_index of a name (7.4.18.1) or the
    // region header of a matrix (7.4.34.1). GET_CONFIGURATION asks with nothing (7.4.8.1).
    if (set_command_type == AEM_COMMAND_SET_CONFIGURATION) {
        return 0;
    }
    if (set_command_type == AEM_COMMAND_SET_NAME) {
        return aem::AemNameCommandPayload::LENGTH;
    }
    if (set_command_type == AEM_COMMAND_SET_MATRIX) {
        return aem::AemMatrixPayloadHeader::LENGTH;
    }
    return aem::AemControlPayloadHeader::LENGTH;
}

auto AemCommandHandler::value_in_force(
    uint16_t const set_command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> size_t
{
    auto const twin = get_twin_of(set_command_type);
    size_t const request_size = get_twin_request_size(set_command_type);
    if (!twin || command_data.size() < request_size) {
        return 0;
    }
    auto const n = read_back(*twin, command_data.first(request_size), out_buffer);
    // GET_STREAM_INFO's Milan v1.2 extension is not SET_STREAM_INFO's.
    return set_command_type == AEM_COMMAND_SET_STREAM_INFO ? std::min(n, set_stream_info_length(stream_info_layout())) : n;
}

auto AemCommandHandler::value_is_in_force(uint16_t const command_type, std::span<uint8_t const> command_data) -> bool
{
    // The GET twin's response has the SET body's shape (the descriptor head and
    // the value, the region header too for a matrix), so the two compare whole.
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> in_force{};
    auto const n = value_in_force(command_type, command_data, in_force);
    return n == command_data.size() && std::equal(command_data.begin(), command_data.end(), in_force.begin());
}

auto AemCommandHandler::name_in_force(std::span<uint8_t const> const command_data) const -> bool
{
    auto const ref = name_ref_of(command_data);
    if (!ref) {
        return false;
    }
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> current{};
    AemEntityModel const aem_model{*handler_};
    size_t const n = aem_model.get_name_for_wire(*ref, current);
    return n != 0 && n <= command_data.size() && std::ranges::equal(std::span{current}.first(n), command_data.first(n));
}

void AemCommandHandler::report_setting_changed(uint16_t const command_type) const
{
    if (is_persistent_setting_command(command_type) && callbacks_.on_settings_changed) {
        callbacks_.on_settings_changed(command_type);
    }
}

auto AemCommandHandler::refuse_unless_success(
    uint16_t const set_command, AemCommandResponse const r, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (r.status == AEM_STATUS_SUCCESS) {
        return r;
    }
    // Nothing holds a command answered here for a final response (Table 7-141): no IN_PROGRESS.
    uint8_t const status = r.status == AEM_STATUS_IN_PROGRESS ? AEM_STATUS_ENTITY_MISBEHAVING : r.status;
    return refused_set(status, set_command, command_data, out_buffer);
}

auto AemCommandHandler::read_back(uint16_t const get_command_type, std::span<uint8_t const> request, std::span<uint8_t> out_buffer)
    -> size_t
{
    // The GET runs through the ordinary dispatch so every path that serves a
    // GET serves this too; the header of the command being answered is kept.
    AemDu const saved_header = current_header_;
    AemDu get_header = saved_header;
    get_header.init_command(get_command_type, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + request.size()));
    auto const r = handle_command(get_header, request, out_buffer);
    current_header_ = saved_header;
    return r.status == AEM_STATUS_SUCCESS ? r.size : 0;
}

auto AemCommandHandler::handle_set_descriptor_value(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    if (auto const early = answered_before_hook(command_type, command_data, out_buffer)) {
        return *early;
    }
    // Per-call model (handler_ resolves the descriptor symbol via its storage). The
    // handler applies the value by (command_type, symbol); the response echoes the command.
    AemEntityModel aem_model{*handler_};
    aem_model.set_configuration(current_configuration_);
    auto const r = aem_model.apply_set_descriptor_value(command_type, command_data, out_buffer);
    if (r.status == AEM_STATUS_IN_PROGRESS) {
        // The hook needs time (e.g. an SRP re-reservation after SET_STREAM_FORMAT):
        // answer IN_PROGRESS now, keep repeating it from tick(), and finish when
        // the application calls complete_pending_command().
        return defer_command(command_type, std::span<uint8_t const>{out_buffer.data(), r.size});
    }
    return settle_set(command_type, r.status, command_data, out_buffer.first(r.size), out_buffer);
}

auto AemCommandHandler::settle_set(
    uint16_t const command_type,
    uint8_t const status,
    std::span<uint8_t const> command_data,
    std::span<uint8_t const> echoed,
    std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    if (status != AEM_STATUS_SUCCESS) {
        return refused_set(status, command_type, command_data, out_buffer);
    }
    // A controller changed a value: notify every registered controller (IEEE
    // 1722.1 9.6 unsolicited notifications). All subscribers are notified,
    // including the originator — the U-bit + distinct sequence_id let a
    // controller tell its own unsolicited echo from the direct response.
    emit_unsolicited(command_type, echoed);
    return {.status = status, .size = echoed.size()};
}

auto AemCommandHandler::defer_command(uint16_t const command_type, std::span<uint8_t const> echoed_body) -> AemCommandResponse
{
    (void)command_type;
    // handle_set_descriptor_value() saw a free slot before the hook ran, so the
    // add below cannot fail for want of one; a body longer than an AECPDU carries,
    // which no command read off the wire has, is the one refusal left here.
    if (echoed_body.size() > MAX_PENDING_BODY) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = echoed_body.size()};
    }
    PendingCommand pending{
        .header = current_header_,
        .mac = current_src_mac_,
        .body = {},
        .body_size = echoed_body.size(),
        .next_in_progress = event_time_ + std::chrono::milliseconds(AEM_IN_PROGRESS_TIMEOUT_MS),
        .deadline = event_time_ + std::chrono::milliseconds(PENDING_COMMAND_DEADLINE_MS)};
    std::copy(echoed_body.begin(), echoed_body.end(), pending.body.begin());
    if (!pending_commands_.add(pending)) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = echoed_body.size()};
    }
    return {.status = AEM_STATUS_IN_PROGRESS, .size = echoed_body.size()};
}

auto AemCommandHandler::repeated_pending(uint16_t const command_type) const noexcept -> PendingCommand const*
{
    auto const idx = pending_commands_.find_if([&](PendingCommand const& p) {
        return p.header.controller_entity_id == current_header_.controller_entity_id &&
            p.header.sequence_id == current_header_.sequence_id && p.header.command_code() == command_type;
    });
    return pending_commands_.get(idx);
}

void AemCommandHandler::expire_pending_commands(sm::TimePoint const now)
{
    // One at a time from the table: answering one may not be interleaved with
    // walking it (SlotTable::remove moves the last entry into the freed index).
    auto idx = pending_commands_.find_if([now](PendingCommand const& p) { return now >= p.deadline; });
    while (idx < pending_commands_.capacity()) {
        PendingCommand const done = pending_commands_[idx];
        pending_commands_.remove(idx);
        answer_completed(done, PENDING_COMMAND_EXPIRED_STATUS, {});
        idx = pending_commands_.find_if([now](PendingCommand const& p) { return now >= p.deadline; });
    }
}

auto AemCommandHandler::complete_pending_command(
    uint16_t const command_type,
    uint16_t const descriptor_type,
    uint16_t const descriptor_index,
    uint8_t const status,
    std::span<uint8_t const> response_body) -> bool
{
    auto const idx = pending_commands_.find_if([&](PendingCommand const& p) {
        if (p.header.command_code() != command_type || p.body_size < 4) {
            return false;
        }
        doublet_t dtype{};
        doublet_t dindex{};
        span_load(dtype, std::span<uint8_t const>{p.body}.subspan(0, 2));
        span_load(dindex, std::span<uint8_t const>{p.body}.subspan(2, 2));
        return dtype.get() == descriptor_type && dindex.get() == descriptor_index;
    });
    if (idx >= pending_commands_.capacity()) {
        return false;
    }
    PendingCommand const done = pending_commands_[idx];
    pending_commands_.remove(idx);
    answer_completed(done, status, response_body);
    return true;
}

void AemCommandHandler::answer_completed(PendingCommand const& done, uint8_t const status, std::span<uint8_t const> response_body)
{
    auto const command_type = done.header.command_code();
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> in_force{};
    auto const body = completion_body(done, status, response_body, in_force);
    (void)send_response(done.mac, done.header, status, body);
    if (status == AEM_STATUS_SUCCESS) {
        emit_unsolicited_for(done.header.controller_entity_id, command_type, body);
        report_setting_changed(command_type);
    }
}

void AemCommandHandler::emit_unsolicited_for(Eui64 const& requester, uint16_t const command_type, std::span<uint8_t const> body)
{
    // The requester is left out of this fan-out alone (handle_command says
    // why); it is nobody again once the fan-out is done.
    requester_ = requester;
    emit_unsolicited(command_type, body);
    requester_ = {};
}

auto AemCommandHandler::completion_body(
    PendingCommand const& done, uint8_t const status, std::span<uint8_t const> response_body, std::span<uint8_t> in_force)
    -> std::span<uint8_t const>
{
    // The application may say what the response carries. Otherwise a SET that
    // succeeded echoes what was asked, and one that failed answers the value in
    // force (7.4.9.1 and its siblings), read back now.
    auto const requested = std::span<uint8_t const>{done.body}.first(done.body_size);
    if (!response_body.empty()) {
        return response_body;
    }
    if (status == AEM_STATUS_SUCCESS) {
        return requested;
    }
    return std::span<uint8_t const>{in_force}.first(failed_set_body(done.header.command_code(), requested, in_force));
}

void AemCommandHandler::tick(sm::TimePoint const now)
{  // NOLINT(statusbar-block-statements) one timer of the handler after another
    event_time_ = std::max(event_time_, now);
    have_now_ = true;
    tick_notifications(now);
    if (locked_ && now >= lock_expiry_time_) {
        release_lock();
        emit_lock_released();
    }
    if (pending_acquire_.active && now >= pending_acquire_.timeout) {
        controller_available_timed_out();
    }
    // A deferred command nobody completed is answered and let go of first, so
    // it is not told IN_PROGRESS once more on its way out.
    expire_pending_commands(now);
    // IEEE 1722.1 9.2.2.3.1.2: a deferred command repeats IN_PROGRESS every
    // AEM_IN_PROGRESS_TIMEOUT_MS so the controller keeps waiting.
    for (auto& p : pending_commands_) {
        if (now >= p.next_in_progress) {
            (void)send_response(p.mac, p.header, AEM_STATUS_IN_PROGRESS, std::span<uint8_t const>{p.body}.first(p.body_size));
            p.next_in_progress = now + std::chrono::milliseconds(AEM_IN_PROGRESS_TIMEOUT_MS);
        }
    }
    if (profile_ == Profile::milan13) {
        tick_controller_monitor(now);
    }
}

void AemCommandHandler::notify_on_success(
    uint16_t const command_type, AemCommandResponse const& response, std::span<uint8_t const> out_buffer)
{
    if (response.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(command_type, out_buffer.first(response.size));
    }
}

void AemCommandHandler::emit_lock_released()
{
    AemLockEntityPayload body{};
    body.set_unlock(true);
    std::array<uint8_t, AemLockEntityPayload::LENGTH> bytes{};
    span_store(bytes, body);
    emit_unsolicited(AEM_COMMAND_LOCK_ENTITY, bytes);
}

auto AemCommandHandler::notify_counters_changed(uint16_t const descriptor_type, uint16_t const descriptor_index) -> uint8_t
{
    // Milan v1.2 Table 5.22: at most one GET_COUNTERS notification per descriptor per second,
    // sent from tick(); before the first tick there is no clock, so send at once.
    if (!have_now_) {
        return send_counters_notification(descriptor_type, descriptor_index);
    }
    if (auto const status = counters_of(descriptor_type, descriptor_index).response.status; status != AEM_STATUS_SUCCESS) {
        return status;  // no counters for it (no callback, no such descriptor): nothing held
    }
    auto at = counters_notifications_.find_if([&](CountersNotification const& n) {
        return n.descriptor_type == descriptor_type && n.descriptor_index == descriptor_index;
    });
    if (at >= counters_notifications_.capacity()) {
        if (!counters_notifications_.add(
                CountersNotification{.descriptor_type = descriptor_type, .descriptor_index = descriptor_index})) {
            // More descriptors than any entity has: unlimited.
            return send_counters_notification(descriptor_type, descriptor_index);
        }
        at = counters_notifications_.size() - 1;
    }
    counters_notifications_[at].pending = true;
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::counters_of(uint16_t const descriptor_type, uint16_t const descriptor_index) -> CountersResponse
{
    std::array<uint8_t, AemGetCountersCommandPayload::LENGTH> request{};
    span_store(request, AemGetCountersCommandPayload{.descriptor_type = descriptor_type, .descriptor_index = descriptor_index});
    CountersResponse r{};
    r.response = handle_get_counters(AemDu{}, request, r.payload);
    return r;
}

auto AemCommandHandler::send_counters_notification(uint16_t const descriptor_type, uint16_t const descriptor_index) -> uint8_t
{
    auto const r = counters_of(descriptor_type, descriptor_index);
    if (r.response.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_COUNTERS, std::span<uint8_t const>{r.payload}.first(r.response.size));
    }
    return r.response.status;
}

void AemCommandHandler::tick_notifications(sm::TimePoint const now)
{
    // A GET_COUNTERS change held back within a second of the last one goes
    // once the second has passed, with the counters as they are then.
    for (auto& n : counters_notifications_) {
        if (n.pending && now >= n.next_allowed) {
            n.pending = false;
            n.next_allowed = now + COUNTERS_NOTIFICATION_INTERVAL;
            (void)send_counters_notification(n.descriptor_type, n.descriptor_index);
        }
    }
}

auto AemCommandHandler::handle_get_avb_info(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (!callbacks_.get_avb_info) {
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    if (command_data.size() < AemGetAvbInfoCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemGetAvbInfoCommandPayload cmd{};
    span_load(cmd, command_data.first(AemGetAvbInfoCommandPayload::LENGTH));
    AvbInfo info{};
    if (cmd.descriptor_type.get() != DESCRIPTOR_AVB_INTERFACE || !callbacks_.get_avb_info(cmd.descriptor_index.get(), info)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    // Response (7.4.40.2): the fixed payload then one 4-octet mapping per SR
    // class: traffic_class, priority, vlan_id.
    constexpr size_t MAPPING_LENGTH = 4;
    size_t const size = AemAvbInfoPayload::LENGTH + info.msrp_mappings.size() * MAPPING_LENGTH;
    if (out_buffer.size() < size) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    AemAvbInfoPayload resp{};
    resp.descriptor_type = cmd.descriptor_type;
    resp.descriptor_index = cmd.descriptor_index;
    resp.gptp_grandmaster_id = tsn::ClockIdentity{info.gptp_grandmaster_id};
    resp.propagation_delay = info.propagation_delay;
    resp.gptp_domain_number = info.gptp_domain_number;
    resp.flags = info.flags;
    resp.msrp_mappings_count = static_cast<uint16_t>(info.msrp_mappings.size());
    span_store(out_buffer.first(AemAvbInfoPayload::LENGTH), resp);
    size_t at = AemAvbInfoPayload::LENGTH;
    for (auto const& m : info.msrp_mappings) {
        out_buffer[at] = m.traffic_class;
        out_buffer[at + 1] = m.priority;
        doublet_t const vlan{m.vlan_id};
        span_store(out_buffer.subspan(at + 2, 2), vlan);
        at += MAPPING_LENGTH;
    }
    return {.status = AEM_STATUS_SUCCESS, .size = size};
}

auto AemCommandHandler::notify_avb_info_changed(uint16_t const descriptor_index) -> uint8_t
{
    AemGetAvbInfoCommandPayload const cmd{.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .descriptor_index = descriptor_index};
    std::array<uint8_t, AemGetAvbInfoCommandPayload::LENGTH> request{};
    span_store(request, cmd);
    std::array<uint8_t, AemAvbInfoPayload::LENGTH + AvbInfo::MAX_MSRP_MAPPINGS * 4> out{};
    auto const r = handle_get_avb_info(request, out);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_AVB_INFO, std::span<uint8_t const>{out}.first(r.size));
    }
    return r.status;
}

auto AemCommandHandler::handle_get_as_path(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (!callbacks_.get_as_path) {
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    auto const cmd = load_payload<AemGetAsPathCommandPayload>(command_data);
    if (!cmd) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AsPath path{};
    if (!callbacks_.get_as_path(cmd->descriptor_index.get(), path)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    return as_path_response(cmd->descriptor_index.get(), path, out_buffer);
}

auto AemCommandHandler::notify_as_path_changed(uint16_t const descriptor_index) -> uint8_t
{
    std::array<uint8_t, AemGetAsPathCommandPayload::LENGTH> request{};
    span_store(request, AemGetAsPathCommandPayload{.descriptor_index = descriptor_index});
    std::array<uint8_t, AemAsPathResponseHeader::LENGTH + (MAX_AS_PATH_LENGTH * Eui64::LENGTH)> out{};
    auto const r = handle_get_as_path(request, out);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_AS_PATH, std::span<uint8_t const>{out}.first(r.size));
    }
    return r.status;
}

auto AemCommandHandler::notify_stream_info_changed(uint16_t const descriptor_type, uint16_t const descriptor_index) -> uint8_t
{
    std::array<uint8_t, AEM_STREAM_INFO_2021_LENGTH> out{};
    auto const r = answer_stream_info({.descriptor_type = descriptor_type, .descriptor_index = descriptor_index}, out);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_STREAM_INFO, std::span<uint8_t const>{out}.first(r.size));
    }
    return r.status;
}

auto AemCommandHandler::handle_set_max_transit_time(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = load_payload<AemSetMaxTransitTimePayload>(command_data);
    if (!cmd) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    auto const body = command_data.first(AemSetMaxTransitTimePayload::LENGTH);
    if (auto const status = max_transit_time_refusal(*cmd); status != AEM_STATUS_SUCCESS) {
        return refused_set(status, AEM_COMMAND_SET_MAX_TRANSIT_TIME, body, out_buffer);
    }
    // A repeat of one deferred, no slot to defer to, or (Milan v1.3 Section 5.4.5.3) the time in force.
    if (auto const early = answered_before_hook(AEM_COMMAND_SET_MAX_TRANSIT_TIME, body, out_buffer)) {
        return *early;
    }
    return set_max_transit_time(*cmd, body, out_buffer);
}

auto AemCommandHandler::max_transit_time_refusal(AemSetMaxTransitTimePayload const& cmd) const -> uint8_t
{
    uint32_t current = 0;
    if (auto const status = max_transit_time_target(cmd.descriptor_type.get(), cmd.descriptor_index.get(), current);
        status != AEM_STATUS_SUCCESS) {
        return status;
    }
    return cmd.max_transit_time.get() > MILAN_PRESENTATION_OFFSET_MAX_NS ? AEM_STATUS_BAD_ARGUMENTS : AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::set_max_transit_time(
    AemSetMaxTransitTimePayload const& cmd, std::span<uint8_t const> const body, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{
    auto const echoed = out_buffer.first(echo_command(body, out_buffer));
    auto const status =
        callbacks_.set_presentation_offset(cmd.descriptor_index.get(), static_cast<uint32_t>(cmd.max_transit_time.get()));
    if (status == AEM_STATUS_IN_PROGRESS) {
        return defer_command(AEM_COMMAND_SET_MAX_TRANSIT_TIME, echoed);  // finished by complete_pending_command()
    }
    // The response echoes the command; a success is notified as it to the others (7.5.2).
    return settle_set(AEM_COMMAND_SET_MAX_TRANSIT_TIME, status, body, echoed, out_buffer);
}

auto AemCommandHandler::handle_get_max_transit_time(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = load_payload<AemGetMaxTransitTimeCommandPayload>(command_data);
    if (!cmd) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    return answer_max_transit_time(*cmd, command_data, out_buffer);
}

auto AemCommandHandler::answer_max_transit_time(
    AemGetMaxTransitTimeCommandPayload const& cmd, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    uint32_t current = 0;
    if (auto const status = max_transit_time_target(cmd.descriptor_type.get(), cmd.descriptor_index.get(), current);
        status != AEM_STATUS_SUCCESS) {
        return reject_command(status, command_data, out_buffer);
    }
    span_store(
        out_buffer,
        AemGetMaxTransitTimeResponsePayload{
            .descriptor_type = cmd.descriptor_type,
            .descriptor_index = cmd.descriptor_index,
            .max_transit_time = uint64_t{current}});
    return {.status = AEM_STATUS_SUCCESS, .size = AemGetMaxTransitTimeResponsePayload::LENGTH};
}

auto AemCommandHandler::max_transit_time_target(
    uint16_t const descriptor_type, uint16_t const descriptor_index, uint32_t& current) const -> uint8_t
{
    if (!callbacks_.get_presentation_offset || !callbacks_.set_presentation_offset) {
        return AEM_STATUS_NOT_IMPLEMENTED;
    }
    if (descriptor_type != DESCRIPTOR_STREAM_OUTPUT) {
        return descriptor_type == DESCRIPTOR_STREAM_INPUT ? AEM_STATUS_NOT_SUPPORTED : AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    auto const offset = callbacks_.get_presentation_offset(descriptor_index);
    current = offset.value_or(0);
    return offset ? AEM_STATUS_SUCCESS : AEM_STATUS_NO_SUCH_DESCRIPTOR;
}

auto AemCommandHandler::handle_reboot(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    if (!callbacks_.on_reboot) {
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    if (command_data.size() < AemRebootPayload::LENGTH || out_buffer.size() < AemRebootPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemRebootPayload cmd{};
    span_load(cmd, command_data.first(AemRebootPayload::LENGTH));
    auto const status = callbacks_.on_reboot(cmd.descriptor_type.get(), cmd.descriptor_index.get());
    span_store(out_buffer.first(AemRebootPayload::LENGTH), cmd);
    return {.status = status, .size = AemRebootPayload::LENGTH};
}

auto AemCommandHandler::memory_object_id(uint16_t const configuration_index, uint16_t const descriptor_index) const -> DescriptorId
{
    AemEntityModel aem_model{*handler_};
    aem_model.set_configuration(configuration_index);
    DescriptorRef const ref{
        .configuration_index = configuration_index,
        .descriptor_type = DESCRIPTOR_MEMORY_OBJECT,
        .descriptor_index = descriptor_index};
    return DescriptorId{.ref = ref, .symbol = aem_model.symbol_for(ref)};
}

auto AemCommandHandler::handle_set_memory_object_length(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (command_data.size() < AemMemoryObjectLengthPayload::LENGTH || out_buffer.size() < AemMemoryObjectLengthPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemMemoryObjectLengthPayload cmd{};
    span_load(cmd, command_data.first(AemMemoryObjectLengthPayload::LENGTH));
    auto const status = handler_->on_set_memory_object_length(
        memory_object_id(cmd.configuration_index.get(), cmd.descriptor_index.get()), cmd.length.get());
    span_store(out_buffer.first(AemMemoryObjectLengthPayload::LENGTH), cmd);
    return {.status = status, .size = AemMemoryObjectLengthPayload::LENGTH};
}

auto AemCommandHandler::handle_get_memory_object_length(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (command_data.size() < AemGetMemoryObjectLengthCommandPayload::LENGTH ||
        out_buffer.size() < AemMemoryObjectLengthPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemGetMemoryObjectLengthCommandPayload cmd{};
    span_load(cmd, command_data.first(AemGetMemoryObjectLengthCommandPayload::LENGTH));
    uint64_t length = 0;
    if (!handler_->on_get_memory_object_length(
            memory_object_id(cmd.configuration_index.get(), cmd.descriptor_index.get()), length)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    AemMemoryObjectLengthPayload resp{};
    resp.descriptor_index = cmd.descriptor_index;
    resp.configuration_index = cmd.configuration_index;
    resp.length = length;
    span_store(out_buffer.first(AemMemoryObjectLengthPayload::LENGTH), resp);
    return {.status = AEM_STATUS_SUCCESS, .size = AemMemoryObjectLengthPayload::LENGTH};
}

namespace {

/// dynamic_info entry header (Figure 7-94): info_command_specific_data_length(2),
/// reserved(2), info_status in the top five bits of a doublet(2), info_command_type(2).
constexpr size_t DYNAMIC_INFO_HEADER_LENGTH = 8;

/// Fixed-size GET commands GET_DYNAMIC_INFO may carry (7.4.76.2 as amended by
/// IEEE 1722.1-2021/Cor 1-2025).
constexpr std::array<uint16_t, 24> DYNAMIC_INFO_COMMANDS{
    AEM_COMMAND_GET_CONFIGURATION,
    AEM_COMMAND_GET_STREAM_FORMAT,
    AEM_COMMAND_GET_VIDEO_FORMAT,
    AEM_COMMAND_GET_SENSOR_FORMAT,
    AEM_COMMAND_GET_STREAM_INFO,
    AEM_COMMAND_GET_NAME,
    AEM_COMMAND_GET_ASSOCIATION_ID,
    AEM_COMMAND_GET_SAMPLING_RATE,
    AEM_COMMAND_GET_CLOCK_SOURCE,
    AEM_COMMAND_GET_SIGNAL_SELECTOR,
    AEM_COMMAND_GET_COUNTERS,
    AEM_COMMAND_GET_MEMORY_OBJECT_LENGTH,
    AEM_COMMAND_GET_STREAM_BACKUP,
    AEM_COMMAND_GET_MAX_TRANSIT_TIME,
    AEM_COMMAND_GET_SAMPLING_RATE_RANGE,
    AEM_COMMAND_GET_PTP_INSTANCE_INFO,
    AEM_COMMAND_GET_PTP_INSTANCE_EXTENDED_INFO,
    AEM_COMMAND_GET_PTP_INSTANCE_GRANDMASTER_INFO,
    AEM_COMMAND_GET_PTP_PORT_INITIAL_INTERVALS,
    AEM_COMMAND_GET_PTP_PORT_CURRENT_INTERVALS,
    AEM_COMMAND_GET_PTP_PORT_REMOTE_INTERVALS,
    AEM_COMMAND_GET_PTP_PORT_INFO,
    AEM_COMMAND_GET_PTP_PORT_OVERRIDES,
    AEM_COMMAND_GET_PATH_LATENCY,
};

auto read_u16(std::span<uint8_t const> bytes, size_t const at) -> uint16_t
{
    doublet_t v{};
    span_load(v, bytes.subspan(at, 2));
    return v.get();
}

void write_u16(std::span<uint8_t> bytes, size_t const at, uint16_t const value)
{
    doublet_t const v{value};
    span_store(bytes.subspan(at, 2), v);
}

}  // namespace

auto AemCommandHandler::name_ref_of(std::span<uint8_t const> command_data) -> std::optional<NameRef>
{
    // GET_NAME command body (Clause 7.4.18.1): descriptor_type(2) +
    // descriptor_index(2) + name_index(2) + configuration_index(2).
    constexpr size_t GET_NAME_HEADER = 8;
    if (command_data.size() < GET_NAME_HEADER) {
        return std::nullopt;
    }
    return NameRef{
        .descriptor =
            DescriptorRef{
                .configuration_index = read_u16(command_data, 6),
                .descriptor_type = read_u16(command_data, 0),
                .descriptor_index = read_u16(command_data, 2)},
        .name_index = read_u16(command_data, 4)};
}

auto AemCommandHandler::descriptor_ref_of(std::span<uint8_t const> command_data) const -> std::optional<DescriptorRef>
{
    if (command_data.size() < 4) {
        return std::nullopt;
    }
    return DescriptorRef{
        .configuration_index = current_configuration_,
        .descriptor_type = read_u16(command_data, 0),
        .descriptor_index = read_u16(command_data, 2)};
}

auto AemCommandHandler::unserved_get_status(DescriptorRef const& ref) const -> uint8_t
{
    // Whether the entity has the descriptor at all is what the model answers
    // when asked for its wire form; the bytes themselves are not wanted.
    std::array<uint8_t, atdecc::aem::AEM_DESCRIPTOR_SIZE> scratch{};
    AemEntityModel const aem_model{*handler_};
    return aem_model.get_descriptor_for_wire(ref, scratch) == 0 ? AEM_STATUS_NO_SUCH_DESCRIPTOR : AEM_STATUS_NOT_IMPLEMENTED;
}

auto AemCommandHandler::handle_get_dynamic_info(
    AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // Pass 1 (7.4.76.2): every entry must parse and name a fixed-size GET, or the
    // whole command is BAD_ARGUMENTS with the request echoed and nothing processed.
    auto const echo_request = [&](uint8_t const status) -> AemCommandResponse {
        if (out_buffer.size() < command_data.size()) {
            return {.status = status, .size = 0};
        }
        span_copy(out_buffer.first(command_data.size()), command_data);
        return {.status = status, .size = command_data.size()};
    };
    for (size_t at = 0; at < command_data.size();) {
        if (at + DYNAMIC_INFO_HEADER_LENGTH > command_data.size()) {
            return echo_request(AEM_STATUS_BAD_ARGUMENTS);
        }
        size_t const length = read_u16(command_data, at);
        auto const info_command = static_cast<uint16_t>(read_u16(command_data, at + 6) & 0x3FFFU);
        if (at + DYNAMIC_INFO_HEADER_LENGTH + length > command_data.size() ||
            std::find(DYNAMIC_INFO_COMMANDS.begin(), DYNAMIC_INFO_COMMANDS.end(), info_command) == DYNAMIC_INFO_COMMANDS.end()) {
            return echo_request(AEM_STATUS_BAD_ARGUMENTS);
        }
        at += DYNAMIC_INFO_HEADER_LENGTH + length;
    }

    // Pass 2: dispatch each entry as its own command. The largest fixed-size GET
    // response is GET_COUNTERS (136 octets), so a quarter of the dispatch buffer
    // is plenty for one entry.
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE / 4> entry_out{};
    size_t const out_limit = std::min(out_buffer.size(), MAX_AEM_BODY);
    size_t out_at = 0;
    AemDu const saved_header = current_header_;
    for (size_t at = 0; at < command_data.size();) {
        size_t const length = read_u16(command_data, at);
        auto const info_command = static_cast<uint16_t>(read_u16(command_data, at + 6) & 0x3FFFU);
        auto const request = command_data.subspan(at + DYNAMIC_INFO_HEADER_LENGTH, length);

        AemDu sub_header = header;
        sub_header.init_command(info_command, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + length));
        auto const r = handle_command(sub_header, request, entry_out);
        // A failed entry echoes its request, as the standalone command would.
        auto const body = r.size > 0 ? std::span<uint8_t const>{entry_out}.first(r.size) : request;

        // An entry that would overflow the AECPDU is dropped and the rest continue.
        size_t const need = DYNAMIC_INFO_HEADER_LENGTH + body.size();
        if (out_at + need <= out_limit) {
            auto entry = out_buffer.subspan(out_at, need);
            write_u16(entry, 0, static_cast<uint16_t>(body.size()));
            write_u16(entry, 2, 0);
            write_u16(entry, 4, static_cast<uint16_t>(static_cast<uint16_t>(r.status) << 11U));
            write_u16(entry, 6, info_command);
            span_copy(entry.subspan(DYNAMIC_INFO_HEADER_LENGTH), body);
            out_at += need;
        }
        at += DYNAMIC_INFO_HEADER_LENGTH + length;
    }
    current_header_ = saved_header;
    return {.status = AEM_STATUS_SUCCESS, .size = out_at};
}

auto AemCommandHandler::handle_get_descriptor_value(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    AemEntityModel aem_model{*handler_};
    aem_model.set_configuration(current_configuration_);
    auto const n = aem_model.get_descriptor_value_for_wire(command_type, command_data, out_buffer);
    if (n == 0) {
        // The handler served no value: there is no such descriptor, or the
        // entity does not implement this GET for the one there is. A body too
        // short to name a descriptor keeps the latter answer.
        auto const ref = descriptor_ref_of(command_data);
        return {.status = ref ? unserved_get_status(*ref) : AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    return {.status = AEM_STATUS_SUCCESS, .size = n};
}

auto AemCommandHandler::handle_get_stream_format(
    AemDu const& /*header*/, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    using namespace atdecc::aem;
    if (command_data.size() < AemGetStreamFormatCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemGetStreamFormatCommandPayload cmd{};
    span_load(cmd, command_data);
    uint16_t const descriptor_type = cmd.descriptor_type;
    uint16_t const descriptor_index = cmd.descriptor_index;
    if (descriptor_type != DESCRIPTOR_STREAM_INPUT && descriptor_type != DESCRIPTOR_STREAM_OUTPUT) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    if (out_buffer.size() < AemStreamFormatPayload::LENGTH) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    // Read the STREAM descriptor (current configuration) and lift its current_format.
    std::array<uint8_t, AEM_DESCRIPTOR_SIZE> desc{};
    AemEntityModel const aem_model{*handler_};
    DescriptorRef const ref{
        .configuration_index = current_configuration_, .descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
    auto const n = aem_model.get_descriptor_for_wire(ref, desc);
    constexpr size_t FORMAT_OFFSET = offsetof(DescriptorStream, current_format);  // 74
    if (n < FORMAT_OFFSET + 8) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    AemStreamFormatPayload resp{};
    resp.descriptor_type = descriptor_type;
    resp.descriptor_index = descriptor_index;
    std::copy_n(desc.data() + FORMAT_OFFSET, resp.stream_format.size(), resp.stream_format.begin());
    span_store(out_buffer, resp);
    return {.status = AEM_STATUS_SUCCESS, .size = AemStreamFormatPayload::LENGTH};
}

auto AemCommandHandler::handle_get_sampling_rate(
    AemDu const& /*header*/, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    using namespace atdecc::aem;
    if (command_data.size() < AemGetSamplingRateCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemGetSamplingRateCommandPayload cmd{};
    span_load(cmd, command_data);
    uint16_t const descriptor_type = cmd.descriptor_type;
    uint16_t const descriptor_index = cmd.descriptor_index;
    // GET_SAMPLING_RATE targets AUDIO_UNIT (VIDEO_CLUSTER / SENSOR_CLUSTER not modeled).
    if (descriptor_type != DESCRIPTOR_AUDIO_UNIT) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    if (out_buffer.size() < AemSamplingRatePayload::LENGTH) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    std::array<uint8_t, AEM_DESCRIPTOR_SIZE> desc{};
    AemEntityModel const aem_model{*handler_};
    DescriptorRef const ref{
        .configuration_index = current_configuration_, .descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
    auto const n = aem_model.get_descriptor_for_wire(ref, desc);
    constexpr size_t SR_OFFSET = offsetof(DescriptorAudioUnit, current_sampling_rate);  // 136
    if (n < SR_OFFSET + 4) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    AemSamplingRatePayload resp{};
    resp.descriptor_type = descriptor_type;
    resp.descriptor_index = descriptor_index;
    // current_sampling_rate is a network-order quadlet in the wire descriptor.
    span_load(resp.sampling_rate, make_const_span(desc, {.start = SR_OFFSET, .length = 4}));
    span_store(out_buffer, resp);
    return {.status = AEM_STATUS_SUCCESS, .size = AemSamplingRatePayload::LENGTH};
}

auto AemCommandHandler::handle_get_counters(
    AemDu const& /*header*/, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // No counter provider wired => this entity does not implement GET_COUNTERS.
    // Checked before argument validation so an entity without counters answers
    // NOT_IMPLEMENTED rather than BAD_ARGUMENTS.
    if (!callbacks_.get_counters) {
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    // GET_COUNTERS command payload (AemGetCountersCommandPayload):
    //   descriptor_type(2) + descriptor_index(2)
    if (command_data.size() < AemGetCountersCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }

    AemGetCountersCommandPayload cmd{};
    span_load(cmd, command_data.first(AemGetCountersCommandPayload::LENGTH));
    uint16_t const descriptor_type = cmd.descriptor_type;
    uint16_t const descriptor_index = cmd.descriptor_index;

    uint32_t counters_valid = 0;
    std::array<uint32_t, 32> counters{};
    if (!callbacks_.get_counters(descriptor_type, descriptor_index, counters_valid, counters)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }

    // Response (Clause 7.4.42.2): descriptor_type + descriptor_index + counters_valid
    // (32-bit bitmap) + 32 counter values (4 bytes each).
    if (out_buffer.size() < sizeof(AemCountersPayload)) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    AemCountersPayload resp{};
    resp.descriptor_type = ieee::doublet_t{descriptor_type};
    resp.descriptor_index = ieee::doublet_t{descriptor_index};
    resp.counters_valid = ieee::quadlet_t{counters_valid};
    for (size_t i = 0; i < counters.size(); ++i) {
        resp.counters[i] = ieee::quadlet_t{counters[i]};
    }
    span_store(out_buffer.first(sizeof(AemCountersPayload)), resp);
    return {.status = AEM_STATUS_SUCCESS, .size = sizeof(AemCountersPayload)};
}

auto AemCommandHandler::handle_get_stream_info(
    AemDu const& /*header*/, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // An entity without streams answers NOT_IMPLEMENTED rather than BAD_ARGUMENTS.
    auto const cmd = load_payload<AemGetStreamInfoCommandPayload>(command_data);
    if (!cmd && callbacks_.get_stream_info) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    return answer_stream_info(cmd.value_or(AemGetStreamInfoCommandPayload{}), out_buffer);
}

auto AemCommandHandler::answer_stream_info(AemGetStreamInfoCommandPayload const& cmd, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{
    if (!callbacks_.get_stream_info) {
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    // Echo descriptor_type/index back; the app fills the rest plus the flag bits.
    AemStreamInfoPayload resp{.descriptor_type = cmd.descriptor_type, .descriptor_index = cmd.descriptor_index};
    AemStreamInfoMilanExtension ext{};
    if (!callbacks_.get_stream_info(cmd.descriptor_type.get(), cmd.descriptor_index.get(), resp, ext)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    return store_stream_info(resp, ext, out_buffer);
}

auto AemCommandHandler::store_stream_info(
    AemStreamInfoPayload const& resp, AemStreamInfoMilanExtension const& ext, std::span<uint8_t> const out_buffer) const
    -> AemCommandResponse
{
    auto const layout = stream_info_layout();
    size_t const size = stream_info_length(layout);
    if (out_buffer.size() < size) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    std::ranges::fill(out_buffer.first(size), uint8_t{0});
    span_store(out_buffer.first(AemStreamInfoPayload::LENGTH), resp);
    store_stream_info_tail(layout, ext, out_buffer.first(size).subspan(AemStreamInfoPayload::LENGTH));
    return {.status = AEM_STATUS_SUCCESS, .size = size};
}

auto AemCommandHandler::handle_set_stream_info(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = load_payload<AemStreamInfoPayload>(command_data);
    auto const response = echo_stream_info(stream_info_layout(), command_data, out_buffer);
    if (!cmd || response.empty()) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    uint32_t current = 0;
    if (auto const status = stream_info_target(*cmd, current); status != AEM_STATUS_SUCCESS) {
        return {.status = status, .size = response.size()};
    }
    return set_stream_info(*cmd, current, response);
}

auto AemCommandHandler::stream_info_set_answer(
    AemCommandResponse const r, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    if (r.status == AEM_STATUS_SUCCESS || r.status == AEM_STATUS_IN_PROGRESS) {
        return r;  // a deferred SET is answered from its slot when it completes
    }
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> in_force{};
    if (value_in_force(AEM_COMMAND_SET_STREAM_INFO, command_data, in_force) == 0) {
        return r;  // unreadable: the handler's own echo
    }
    return refuse_unless_success(AEM_COMMAND_SET_STREAM_INFO, r, command_data, out_buffer);
}

auto AemCommandHandler::stream_info_target(AemStreamInfoPayload const& cmd, uint32_t& current) const -> uint8_t
{
    // Milan v1.3 Section 5.4.2.9: SET_MAX_TRANSIT_TIME sets the offset in this layout.
    bool const latency = (cmd.flags.get() & stream_info_flags::MSRP_ACC_LAT_VALID) != 0;
    if (latency && stream_info_layout() == StreamInfoLayout::ieee_2021) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    // Milan v1.2 Section 5.4.2.9: "The PAAD-AE shall not implement the SET_STREAM_INFO command on Stream Inputs".
    return max_transit_time_target(cmd.descriptor_type.get(), cmd.descriptor_index.get(), current);
}

auto AemCommandHandler::set_stream_info(
    AemStreamInfoPayload const& cmd, uint32_t const current, std::span<uint8_t const> const response) -> AemCommandResponse
{
    if (auto const refused = set_stream_info_refusal(cmd.flags.get())) {
        setting_unchanged_ = true;  // nothing asked, or refused: nothing to save
        return {.status = *refused, .size = response.size()};
    }
    uint32_t const offset = cmd.msrp_accumulated_latency.get();
    if (auto const early = stream_info_offset_answered_before_hook(offset, response.size())) {
        return *early;
    }
    return apply_stream_info_offset(cmd.descriptor_index.get(), offset, current, response);
}

auto AemCommandHandler::stream_info_offset_answered_before_hook(uint32_t const offset, size_t const size) const
    -> std::optional<AemCommandResponse>
{
    if (offset > MILAN_PRESENTATION_OFFSET_MAX_NS) {
        return AemCommandResponse{.status = AEM_STATUS_BAD_ARGUMENTS, .size = size};
    }
    // As SET_MAX_TRANSIT_TIME's (answered_before_hook): a repeat of one deferred is IN_PROGRESS again,
    // its hook not run twice (the echo is the slot's); with no slot to defer to, NO_RESOURCES.
    if (repeated_pending(AEM_COMMAND_SET_STREAM_INFO) != nullptr) {
        return AemCommandResponse{.status = AEM_STATUS_IN_PROGRESS, .size = size};
    }
    if (pending_commands_.size() >= pending_commands_.capacity()) {
        return AemCommandResponse{.status = AEM_STATUS_NO_RESOURCES, .size = size};
    }
    return std::nullopt;
}

auto AemCommandHandler::apply_stream_info_offset(
    uint16_t const index, uint32_t const offset, uint32_t const current, std::span<uint8_t const> const response)
    -> AemCommandResponse
{
    auto const status = callbacks_.set_presentation_offset(index, offset);
    setting_unchanged_ = offset == current;
    if (status == AEM_STATUS_IN_PROGRESS) {
        return defer_command(AEM_COMMAND_SET_STREAM_INFO, response);  // finished by complete_pending_command()
    }
    // The response echoes the command (MSRP_ACC_LAT_VALID and the offset); a change goes to the
    // other registered controllers as that response (Milan v1.2 Section 5.4.5.2).
    if (status == AEM_STATUS_SUCCESS && offset != current) {
        emit_unsolicited(AEM_COMMAND_SET_STREAM_INFO, response);
    }
    return {.status = status, .size = response.size()};
}

//
// Dynamic audio mappings - IEEE 1722.1-2021 Clauses 7.4.44-7.4.46
//

namespace {

/// The base frequency of an AUDIO_UNIT sampling rate, without its pull (IEEE 1722.1-2021 Clause 7.3.1).
constexpr uint32_t SAMPLING_RATE_BASE_FREQUENCY_MASK = 0x1FFF'FFFFU;

/// The leading octets of an IEC 61883-6 AM824 stream format code, as decode_am824_stream_format() reads them.
struct Am824FormatHead
{
    ieee::octet_t subtype{0};
    ieee::octet_t sf_fmt{0};  ///< sf in bit 7, fmt in bits 6-1
    ieee::octet_t sfc{0};     ///< sampling frequency code
    ieee::octet_t dbs{0};     ///< data block size: the channels
    ieee::quadlet_t rest{0};
};
static_assert(sizeof(Am824FormatHead) == 8, "Am824FormatHead must overlay the 8-octet format code");

/// sf = 1 with fmt = 0x10 (IEC 61883-6) in Am824FormatHead::sf_fmt, under AM824_SF_FMT_MASK.
constexpr uint8_t AM824_SF_FMT_MASK = 0xFEU;
constexpr uint8_t AM824_SF_FMT = 0xA0U;

/// The audio channels and nominal rate of a stream format (AAF, or IEC 61883-6 AM824); none for
/// anything else (a CRF stream has no channels).
struct FormatChannels
{
    uint16_t channels{0};
    uint32_t rate{0};
};

[[nodiscard]] auto format_channels(uint64_t const format) noexcept -> FormatChannels
{
    auto const aaf = avtp::AafStreamFormat::from_u64(format);
    if (aaf.subtype.get() == avtp::AvtpSubtype::aaf) {
        return {.channels = aaf.channels_per_frame(), .rate = avtp::aaf_nsr_to_hz(aaf.nsr())};
    }
    auto const am824 = std::bit_cast<Am824FormatHead>(ieee::octlet_t{format});
    if (am824.subtype.get() == avtp::AvtpSubtype::iec_61883_iidc && (am824.sf_fmt.get() & AM824_SF_FMT_MASK) == AM824_SF_FMT) {
        return {.channels = am824.dbs.get(), .rate = avtp::am824_sfc_to_hz(am824.sfc.get())};
    }
    return {};
}

[[nodiscard]] auto to_wire(DynamicAudioMapping const& mapping) noexcept -> AemAudioMapping
{
    return {
        .stream_index = mapping.stream_index,
        .stream_channel = mapping.stream_channel,
        .cluster_offset = mapping.cluster_offset,
        .cluster_channel = mapping.cluster_channel};
}

/// The mappings one notification carries: as many as an IEEE 1722.1 AECPDU holds (Clause 9.2.1.1.7),
/// (512 - 8) / 8 after the 8-octet ADD/REMOVE header; a GET_AUDIO_MAP response's 12 leave 62.
constexpr size_t MAPPINGS_PER_PDU = (MAX_AEM_BODY - AemAudioMappingsCommandHeader::LENGTH) / AemAudioMapping::LENGTH;
static_assert(MAPPINGS_PER_PDU == 63);

/// The mappings a command is read with at most: as many as MAX_AEM_RESPONSE_SIZE holds.
constexpr size_t MAX_COMMAND_MAPPINGS = (MAX_AEM_RESPONSE_SIZE - AemAudioMappingsCommandHeader::LENGTH) / AemAudioMapping::LENGTH;
using CommandMappings = sg14::inplace_vector<DynamicAudioMapping, MAX_COMMAND_MAPPINGS>;

[[nodiscard]] auto from_wire(AemAudioMapping const& wire) noexcept -> DynamicAudioMapping
{
    return {
        .stream_index = wire.stream_index.get(),
        .stream_channel = wire.stream_channel.get(),
        .cluster_offset = wire.cluster_offset.get(),
        .cluster_channel = wire.cluster_channel.get()};
}

/// Whether @p a and @p b map the same channel of a port of @p port_type: on an input, the cluster
/// channel they feed; on an output, the stream channel (Milan v1.3 Section 5.4.2.27).
[[nodiscard]] auto same_mapped_channel(
    uint16_t const port_type, DynamicAudioMapping const& a, DynamicAudioMapping const& b) noexcept -> bool
{
    if (port_type == DESCRIPTOR_STREAM_PORT_INPUT) {
        return a.cluster_offset == b.cluster_offset && a.cluster_channel == b.cluster_channel;
    }
    return a.stream_index == b.stream_index && a.stream_channel == b.stream_channel;
}

/// Whether AUDIO_UNIT @p unit holds stream port (@p port_type, @p port_index).
[[nodiscard]] auto unit_holds_port(DescriptorAudioUnit const& unit, uint16_t const port_type, uint16_t const port_index) noexcept
    -> bool
{
    bool const input = port_type == DESCRIPTOR_STREAM_PORT_INPUT;
    uint32_t const base = input ? unit.base_stream_input_port.get() : unit.base_stream_output_port.get();
    uint32_t const count = input ? unit.number_of_stream_input_ports.get() : unit.number_of_stream_output_ports.get();
    return port_index >= base && port_index < base + count;
}

/// An ADD/REMOVE_AUDIO_MAPPINGS command's header; nullopt when the command is short of it or of
/// the mappings it counts, or counts more than MAX_COMMAND_MAPPINGS.
[[nodiscard]] auto mappings_command(std::span<uint8_t const> const command_data) noexcept
    -> std::optional<AemAudioMappingsCommandHeader>
{
    auto const cmd = load_payload<AemAudioMappingsCommandHeader>(command_data);
    if (!cmd) {
        return std::nullopt;
    }
    size_t const count = cmd->number_of_mappings.get();
    bool const listed = count * AemAudioMapping::LENGTH <= command_data.size() - AemAudioMappingsCommandHeader::LENGTH;
    return listed && count <= MAX_COMMAND_MAPPINGS ? cmd : std::nullopt;
}

/// The @p count mappings of an ADD/REMOVE_AUDIO_MAPPINGS command body after its header.
[[nodiscard]] auto parse_mappings(std::span<uint8_t const> const bytes, size_t const count) noexcept -> CommandMappings
{
    CommandMappings out{};
    for (size_t i = 0; i < count; ++i) {
        AemAudioMapping wire{};
        span_load(wire, bytes.subspan(i * AemAudioMapping::LENGTH, AemAudioMapping::LENGTH));
        out.push_back(from_wire(wire));
    }
    return out;
}

}  // namespace

template <typename T>
auto AemCommandHandler::read_model_descriptor(uint16_t const type, uint16_t const index) const -> std::optional<T>
{
    std::array<uint8_t, AEM_DESCRIPTOR_SIZE> desc{};
    DescriptorRef const ref{.configuration_index = current_configuration_, .descriptor_type = type, .descriptor_index = index};
    auto const n = AemEntityModel{*handler_}.get_descriptor_for_wire(ref, desc);
    T out{};
    span_load_padded(out, std::span<uint8_t const>{desc}.first(n));
    return n == 0 ? std::nullopt : std::optional<T>{out};
}

auto AemCommandHandler::port_model(uint16_t const port_type, uint16_t const port_index, PortModel& model) const -> bool
{
    auto const port = read_model_descriptor<DescriptorStreamPort>(port_type, port_index);
    if (!port) {
        return false;
    }
    model = PortModel{
        .port_type = port_type,
        .base_cluster = port->base_cluster.get(),
        .number_of_clusters = port->number_of_clusters.get(),
        .number_of_maps = port->number_of_maps.get(),
        .port_flags = port->port_flags.get()};
    read_clusters(model);
    if (port_type == DESCRIPTOR_STREAM_PORT_OUTPUT) {
        read_output_streams(model);
    }
    return true;
}

void AemCommandHandler::read_clusters(PortModel& model) const
{
    for (uint16_t offset = 0; offset < model.number_of_clusters && offset < decltype(model.cluster_channel_counts)::capacity();
         ++offset) {
        auto const cluster = read_model_descriptor<DescriptorAudioCluster>(
            DESCRIPTOR_AUDIO_CLUSTER, static_cast<uint16_t>(model.base_cluster + offset));
        model.cluster_channel_counts.push_back(cluster ? cluster->channel_count.get() : uint16_t{0});
        model.total_channels += model.cluster_channel_counts.back();
    }
}

void AemCommandHandler::read_output_streams(PortModel& model) const
{
    for (uint16_t index = 0; index < MAX_PARTITIONED_STREAMS; ++index) {
        auto const stream = read_model_descriptor<DescriptorStream>(DESCRIPTOR_STREAM_OUTPUT, index);
        if (!stream) {
            return;
        }
        uint16_t most = format_channels(stream->current_format.to_uint64()).channels;
        for (auto const& format : stream->used_stream_formats()) {
            most = std::max(most, format_channels(format.to_uint64()).channels);
        }
        model.stream_channel_counts.push_back(most);
    }
}

auto AemCommandHandler::cluster_channels(PortModel const& port, uint16_t const cluster_offset) noexcept -> uint16_t
{
    return cluster_offset < port.cluster_channel_counts.size() ? port.cluster_channel_counts[cluster_offset] : uint16_t{0};
}

auto AemCommandHandler::cluster_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) noexcept
    -> std::optional<uint32_t>
{
    if (mapping.cluster_offset >= port.number_of_clusters ||
        mapping.cluster_channel >= cluster_channels(port, mapping.cluster_offset)) {
        return std::nullopt;
    }
    uint32_t ordinal = mapping.cluster_channel;
    for (uint16_t offset = 0; offset < mapping.cluster_offset; ++offset) {
        ordinal += cluster_channels(port, offset);
    }
    return ordinal;
}

auto AemCommandHandler::stream_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) noexcept
    -> std::optional<uint32_t>
{
    auto const& counts = port.stream_channel_counts;
    if (mapping.stream_index >= counts.size() || mapping.stream_channel >= counts[mapping.stream_index]) {
        return std::nullopt;
    }
    auto const before = std::span{counts}.first(mapping.stream_index);
    return std::ranges::fold_left(before, uint32_t{mapping.stream_channel}, std::plus<>{});
}

auto AemCommandHandler::partition_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) noexcept
    -> std::optional<uint32_t>
{
    return port.port_type == DESCRIPTOR_STREAM_PORT_INPUT ? cluster_ordinal(port, mapping) : stream_ordinal(port, mapping);
}

auto AemCommandHandler::partitioned_channels(PortModel const& port) noexcept -> uint32_t
{
    if (port.port_type == DESCRIPTOR_STREAM_PORT_INPUT) {
        return port.total_channels;
    }
    return std::ranges::fold_left(port.stream_channel_counts, uint32_t{0}, std::plus<>{});
}

auto AemCommandHandler::dynamic_port(uint16_t const port_type, uint16_t const port_index, PortModel& port) const -> uint8_t
{
    if (port_type != DESCRIPTOR_STREAM_PORT_INPUT && port_type != DESCRIPTOR_STREAM_PORT_OUTPUT) {
        return AEM_STATUS_NOT_SUPPORTED;
    }
    if (!port_model(port_type, port_index, port)) {
        return AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    return port.number_of_maps == 0 ? AEM_STATUS_SUCCESS : AEM_STATUS_NOT_SUPPORTED;  // AUDIO_MAPs: static
}

auto AemCommandHandler::has_dynamic_map(uint16_t const port_type, uint16_t const port_index) const -> bool
{
    PortModel port{};
    return dynamic_port(port_type, port_index, port) == AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::handle_get_audio_map(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = load_payload<AemGetAudioMapCommandPayload>(command_data);
    if (!cmd || audio_maps_ == nullptr) {
        return {.status = cmd ? AEM_STATUS_NOT_IMPLEMENTED : AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    PortModel port{};
    if (auto const status = dynamic_port(cmd->descriptor_type, cmd->descriptor_index, port); status != AEM_STATUS_SUCCESS) {
        return {.status = status, .size = 0};
    }
    return audio_map_response(*cmd, port, out_buffer);
}

auto AemCommandHandler::audio_map_partition() const noexcept -> size_t
{
    return profile_ == Profile::milan13 ? MILAN_AUDIO_MAP_MAPPINGS : AUDIO_MAP_MAPPINGS;
}

auto AemCommandHandler::audio_map_response(
    AemGetAudioMapCommandPayload const& cmd, PortModel const& port, std::span<uint8_t> const out_buffer) const -> AemCommandResponse
{
    // Milan v1.3 Section 5.4.2.26: an input's cluster channels, an output's stream channels, in
    // order, in fixed partitions; number_of_maps counts them whatever is mapped (at least one).
    size_t const maps = std::max<size_t>(1, (partitioned_channels(port) + audio_map_partition() - 1) / audio_map_partition());
    if (cmd.map_index.get() >= maps || out_buffer.size() < AemAudioMapResponseHeader::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    auto const mappings = store_audio_map_entries(
        port, cmd.descriptor_index.get(), cmd.map_index.get(), out_buffer.subspan(AemAudioMapResponseHeader::LENGTH));
    span_store(
        out_buffer,
        AemAudioMapResponseHeader{
            .descriptor_type = cmd.descriptor_type,
            .descriptor_index = cmd.descriptor_index,
            .map_index = cmd.map_index,
            .number_of_maps = static_cast<uint16_t>(maps),
            .number_of_mappings = mappings});
    return {.status = AEM_STATUS_SUCCESS, .size = AemAudioMapResponseHeader::LENGTH + (size_t{mappings} * AemAudioMapping::LENGTH)};
}

auto AemCommandHandler::store_audio_map_entries(
    PortModel const& port, uint16_t const port_index, size_t const map_index, std::span<uint8_t> const entries) const -> uint16_t
{
    // A partition holds at most one mapping per channel it covers, so its mappings fit a response.
    uint16_t count = 0;
    for (auto const& mapping : audio_maps_->port(port.port_type, port_index)) {
        auto const ordinal = partition_ordinal(port, mapping);
        if (!ordinal || *ordinal / audio_map_partition() != map_index || (count + 1U) * AemAudioMapping::LENGTH > entries.size()) {
            continue;
        }
        span_store(entries.subspan(size_t{count} * AemAudioMapping::LENGTH, AemAudioMapping::LENGTH), to_wire(mapping));
        ++count;
    }
    return count;
}

auto AemCommandHandler::check_mapping(
    uint16_t const port_index, PortModel const& port, DynamicAudioMapping const& mapping, bool const check_channel) const -> uint8_t
{
    if (!cluster_ordinal(port, mapping)) {
        return AEM_STATUS_BAD_ARGUMENTS;  // no such cluster channel
    }
    auto const stream = read_model_descriptor<DescriptorStream>(
        port.port_type == DESCRIPTOR_STREAM_PORT_INPUT ? DESCRIPTOR_STREAM_INPUT : DESCRIPTOR_STREAM_OUTPUT, mapping.stream_index);
    if (!stream) {
        return AEM_STATUS_BAD_ARGUMENTS;  // no such stream of the port's direction
    }
    return check_channel ? check_stream_channel(port_index, port, stream->current_format.to_uint64(), mapping.stream_channel)
                         : AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::check_stream_channel(
    uint16_t const port_index, PortModel const& port, uint64_t const stream_format, uint16_t const stream_channel) const -> uint8_t
{
    // Milan v1.3 Section 5.4.2.27: a channel of the stream's current format, at the audio unit's
    // rate unless the port converts rates.
    auto const format = format_channels(stream_format);
    if (stream_channel >= format.channels) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    bool const converts = (port.port_flags & (PORT_FLAG_ASYNC_SAMPLE_RATE_CONV | PORT_FLAG_SYNC_SAMPLE_RATE_CONV)) != 0 ||
        (port.port_type == DESCRIPTOR_STREAM_PORT_OUTPUT && output_mappings_follow_streams_);
    return converts || unit_rate_is(port.port_type, port_index, format.rate) ? AEM_STATUS_SUCCESS : AEM_STATUS_BAD_ARGUMENTS;
}

auto AemCommandHandler::unit_rate_is(uint16_t const port_type, uint16_t const port_index, uint32_t const rate) const -> bool
{
    for (uint16_t unit = 0; unit < UINT16_MAX; ++unit) {
        auto const audio_unit = read_model_descriptor<DescriptorAudioUnit>(DESCRIPTOR_AUDIO_UNIT, unit);
        if (!audio_unit) {
            return true;
        }
        if (unit_holds_port(*audio_unit, port_type, port_index)) {
            return (audio_unit->current_sampling_rate.get() & SAMPLING_RATE_BASE_FREQUENCY_MASK) == rate;
        }
    }
    return true;
}

auto AemCommandHandler::mappable_port(uint16_t const port_type, uint16_t const port_index, PortModel& port) const -> uint8_t
{
    return callbacks_.on_audio_mappings_changed ? stored_port(port_type, port_index, port) : AEM_STATUS_NOT_IMPLEMENTED;
}

auto AemCommandHandler::stored_port(uint16_t const port_type, uint16_t const port_index, PortModel& port) const -> uint8_t
{
    return audio_maps_ != nullptr ? dynamic_port(port_type, port_index, port) : AEM_STATUS_NOT_IMPLEMENTED;
}

auto AemCommandHandler::handle_audio_mappings(bool const add, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = mappings_command(command_data);
    if (!cmd) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    PortModel port{};
    if (auto const status = mappable_port(cmd->descriptor_type, cmd->descriptor_index, port); status != AEM_STATUS_SUCCESS) {
        return reject_command(status, command_data, out_buffer);
    }
    return change_audio_mappings(add, *cmd, port, command_data, out_buffer);
}

auto AemCommandHandler::change_audio_mappings(
    bool const add,
    AemAudioMappingsCommandHeader const& cmd,
    PortModel const& port,
    std::span<uint8_t const> const command_data,
    std::span<uint8_t> const out_buffer) -> AemCommandResponse
{
    auto const listed = parse_mappings(command_data.subspan(AemAudioMappingsCommandHeader::LENGTH), cmd.number_of_mappings.get());
    if (auto const status = next_mappings(add, cmd.descriptor_index, port, listed); status != AEM_STATUS_SUCCESS) {
        return reject_command(status, command_data, out_buffer);
    }
    return commit_mappings_command(add, port.port_type, cmd, command_data, out_buffer);
}

auto AemCommandHandler::next_mappings(
    bool const add, uint16_t const port_index, PortModel const& port, std::span<DynamicAudioMapping const> const listed) -> uint8_t
{
    auto const current = audio_maps_->port(port.port_type, port_index);
    if (add) {
        audio_maps_->next().assign(current.begin(), current.end());
        return add_mappings(port_index, port, listed);
    }
    // IEEE 1722.1-2021 Clause 7.4.46.1: a REMOVE listing a mapping not held removes nothing.
    auto const held = [current](DynamicAudioMapping const& m) { return std::ranges::binary_search(current, m, cluster_order); };
    return std::ranges::all_of(listed, held) ? remove_mappings(port_index, port, current, listed) : AEM_STATUS_BAD_ARGUMENTS;
}

auto AemCommandHandler::commit_mappings_command(
    bool const add,
    uint16_t const port_type,
    AemAudioMappingsCommandHeader const& cmd,
    std::span<uint8_t const> const command_data,
    std::span<uint8_t> const out_buffer) -> AemCommandResponse
{
    bool const changed = !std::ranges::equal(audio_maps_->next(), audio_maps_->port(port_type, cmd.descriptor_index));
    setting_unchanged_ = !changed;  // nothing to save
    if (auto const status = changed ? admit_and_keep(port_type, cmd.descriptor_index, true) : AEM_STATUS_SUCCESS;
        status != AEM_STATUS_SUCCESS) {
        return reject_command(status, command_data, out_buffer);
    }
    // The response echoes the command; a change is notified to the other controllers as it.
    auto const r = reject_command(
        AEM_STATUS_SUCCESS,
        command_data.first(AemAudioMappingsCommandHeader::LENGTH + (cmd.number_of_mappings.get() * AemAudioMapping::LENGTH)),
        out_buffer);
    if (changed) {
        notify_on_success(add ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, r, out_buffer);
    }
    return r;
}

auto AemCommandHandler::add_mappings(
    uint16_t const port_index, PortModel const& port, std::span<DynamicAudioMapping const> const mappings) -> uint8_t
{
    for (auto const& mapping : mappings) {
        if (auto const status = add_mapping(port_index, port, mapping); status != AEM_STATUS_SUCCESS) {
            return status;
        }
    }
    std::ranges::sort(audio_maps_->next(), cluster_order);
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::add_mapping(uint16_t const port_index, PortModel const& port, DynamicAudioMapping const& mapping) -> uint8_t
{
    if (auto const status = check_mapping(port_index, port, mapping, true); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    // Milan v1.3 Section 5.4.2.27: an input's cluster channel is fed from one stream channel, an
    // output's stream channel from one cluster channel; another, held or listed, is BAD_ARGUMENTS.
    auto& next = audio_maps_->next();
    auto const* const existing =
        std::ranges::find_if(next, [&](DynamicAudioMapping const& m) { return same_mapped_channel(port.port_type, m, mapping); });
    if (existing != next.end()) {
        return *existing == mapping ? AEM_STATUS_SUCCESS : AEM_STATUS_BAD_ARGUMENTS;
    }
    return next.try_push_back(mapping) != nullptr ? AEM_STATUS_SUCCESS : AEM_STATUS_NO_RESOURCES;
}

auto AemCommandHandler::remove_mappings(
    uint16_t const port_index,
    PortModel const& port,
    std::span<DynamicAudioMapping const> const current,
    std::span<DynamicAudioMapping const> const listed) -> uint8_t
{
    // Milan v1.3 Section 5.4.2.28: a mapping listed twice is ignored; one naming nothing in the
    // model is not a mapping at all.
    for (auto const& mapping : listed) {
        if (auto const status = check_mapping(port_index, port, mapping, false); status != AEM_STATUS_SUCCESS) {
            return status;
        }
    }
    auto& next = audio_maps_->next();
    next.clear();
    std::ranges::copy_if(current, std::back_inserter(next), [listed](DynamicAudioMapping const& m) {
        return std::ranges::find(listed, m) == listed.end();
    });
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::admit_mappings(uint16_t const port_type, uint16_t const port_index, bool const apply) -> uint8_t
{
    auto const& next = audio_maps_->next();
    if (!audio_maps_->fits(port_type, port_index, next.size())) {
        return AEM_STATUS_NO_RESOURCES;  // every port's together
    }
    if (!apply) {
        return AEM_STATUS_SUCCESS;
    }
    // All or nothing: the data plane puts the whole new set in force, or nothing changes.
    return callbacks_.on_audio_mappings_changed ? callbacks_.on_audio_mappings_changed(port_type, port_index, next)
                                                : AEM_STATUS_NOT_IMPLEMENTED;
}

auto AemCommandHandler::admit_and_keep(uint16_t const port_type, uint16_t const port_index, bool const apply) -> uint8_t
{
    if (auto const status = admit_mappings(port_type, port_index, apply); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    (void)audio_maps_->replace(port_type, port_index, audio_maps_->next());  // admitted: it fits
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::mappings_hold() const -> bool
{
    if (audio_maps_ == nullptr) {
        return true;
    }
    bool holds = true;
    uint32_t read_for = UINT32_MAX;
    PortModel port{};
    audio_maps_->for_each([&](uint16_t const type, uint16_t const index, DynamicAudioMapping const& mapping) {
        if (std::exchange(read_for, (uint32_t{type} << 16U) | index) != read_for && !port_model(type, index, port)) {
            port = PortModel{};  // a port the model lacks: nothing on it holds
        }
        holds = holds && check_mapping(index, port, mapping, true) == AEM_STATUS_SUCCESS;
    });
    return holds;
}

auto AemCommandHandler::mappings_refusal(uint16_t const command_type, std::span<uint8_t const> const command_body) const
    -> std::optional<uint8_t>
{
    if (audio_maps_ == nullptr || audio_maps_->size() == 0) {
        return std::nullopt;
    }
    if (command_type == AEM_COMMAND_SET_STREAM_FORMAT) {
        auto const cmd = load_payload<AemStreamFormatPayload>(command_body);
        return cmd ? format_change_refusal(*cmd) : std::nullopt;  // a short SET is refused as it is
    }
    if (command_type == AEM_COMMAND_SET_SAMPLING_RATE) {
        auto const cmd = load_payload<AemSamplingRatePayload>(command_body);
        return cmd ? rate_change_refusal(*cmd) : std::nullopt;
    }
    return std::nullopt;
}

auto AemCommandHandler::format_change_refusal(AemStreamFormatPayload const& cmd) const -> std::optional<uint8_t>
{
    // Milan v1.3 Section 5.4.2.7: every channel a mapping names stays; and, as Section 5.4.2.27
    // checks an ADD, at the unit's rate unless the port converts rates.
    bool const input = cmd.descriptor_type.get() == DESCRIPTOR_STREAM_INPUT;
    uint16_t const port_type = input ? DESCRIPTOR_STREAM_PORT_INPUT : DESCRIPTOR_STREAM_PORT_OUTPUT;
    auto const format = format_channels(std::bit_cast<ieee::octlet_t>(cmd.stream_format).get());
    bool holds = true;
    audio_maps_->for_each([&](uint16_t const type, uint16_t const index, DynamicAudioMapping const& mapping) {
        if (type == port_type && mapping.stream_index == cmd.descriptor_index.get()) {
            holds = holds && mapping.stream_channel < format.channels &&
                ((!input && output_mappings_follow_streams_) || port_converts(type, index) ||
                 unit_rate_is(type, index, format.rate));
        }
    });
    return holds ? std::nullopt : std::optional<uint8_t>{AEM_STATUS_BAD_ARGUMENTS};
}

auto AemCommandHandler::rate_change_refusal(AemSamplingRatePayload const& cmd) const -> std::optional<uint8_t>
{
    // Milan v1.3 Section 5.4.2.13: a mapping whose stream runs at another rate, on a port without
    // the SRC bits, may refuse the rate: the mapping held would no longer hold (Section 5.4.2.27).
    auto const unit = cmd.descriptor_type.get() == DESCRIPTOR_AUDIO_UNIT
        ? read_model_descriptor<DescriptorAudioUnit>(DESCRIPTOR_AUDIO_UNIT, cmd.descriptor_index)
        : std::nullopt;
    if (!unit) {
        return std::nullopt;
    }
    uint32_t const rate = cmd.sampling_rate.get() & SAMPLING_RATE_BASE_FREQUENCY_MASK;
    bool holds = true;
    audio_maps_->for_each([&](uint16_t const type, uint16_t const index, DynamicAudioMapping const& mapping) {
        bool const rate_free = type == DESCRIPTOR_STREAM_PORT_OUTPUT && output_mappings_follow_streams_;
        if (!rate_free && unit_holds_port(*unit, type, index)) {
            holds = holds && (port_converts(type, index) || stream_rate(type, mapping.stream_index) == rate);
        }
    });
    return holds ? std::nullopt : std::optional<uint8_t>{AEM_STATUS_NOT_SUPPORTED};
}

auto AemCommandHandler::port_converts(uint16_t const port_type, uint16_t const port_index) const -> bool
{
    auto const port = read_model_descriptor<DescriptorStreamPort>(port_type, port_index);
    return port && (port->port_flags.get() & (PORT_FLAG_ASYNC_SAMPLE_RATE_CONV | PORT_FLAG_SYNC_SAMPLE_RATE_CONV)) != 0;
}

auto AemCommandHandler::stream_rate(uint16_t const port_type, uint16_t const stream_index) const -> uint32_t
{
    auto const stream = read_model_descriptor<DescriptorStream>(
        port_type == DESCRIPTOR_STREAM_PORT_INPUT ? DESCRIPTOR_STREAM_INPUT : DESCRIPTOR_STREAM_OUTPUT, stream_index);
    return stream ? format_channels(stream->current_format.to_uint64()).rate : 0;
}

auto AemCommandHandler::seed_audio_mappings(
    uint16_t const port_type, uint16_t const port_index, std::span<DynamicAudioMapping const> const mappings) -> uint8_t
{
    PortModel port{};
    if (auto const status = stored_port(port_type, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    audio_maps_->next().clear();
    if (auto const status = add_mappings(port_index, port, mappings); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    return admit_and_keep(port_type, port_index, false);
}

auto AemCommandHandler::follow_local_audio_mappings(
    uint16_t const port_type, uint16_t const port_index, std::span<DynamicAudioMapping const> const mappings) -> uint8_t
{
    PortModel port{};
    if (auto const status = stored_port(port_type, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    audio_maps_->next().clear();
    if (auto const status = add_mappings(port_index, port, mappings); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    tell_and_keep(port_type, port_index, audio_maps_->port(port_type, port_index));
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::apply_local_audio_mappings(
    uint16_t const port_type,
    uint16_t const port_index,
    std::span<DynamicAudioMapping const> const add,
    std::span<DynamicAudioMapping const> const remove) -> uint8_t
{
    if (auto const refusal = local_change_refusal()) {
        return *refusal;
    }
    PortModel port{};
    if (auto const status = mappable_port(port_type, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    if (auto const status = local_mappings(port_index, port, add, remove); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    return keep_own_mappings(port_type, port_index);
}

auto AemCommandHandler::local_mappings(
    uint16_t const port_index,
    PortModel const& port,
    std::span<DynamicAudioMapping const> const add,
    std::span<DynamicAudioMapping const> const remove) -> uint8_t
{
    auto const current = audio_maps_->port(port.port_type, port_index);
    if (auto const status = remove_mappings(port_index, port, current, remove); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    return add_mappings(port_index, port, add);
}

auto AemCommandHandler::keep_own_mappings(uint16_t const port_type, uint16_t const port_index) -> uint8_t
{
    auto const current = audio_maps_->port(port_type, port_index);
    if (std::ranges::equal(audio_maps_->next(), current)) {
        return AEM_STATUS_SUCCESS;  // nothing to put in force or tell
    }
    if (auto const status = admit_mappings(port_type, port_index, true); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    tell_and_keep(port_type, port_index, current);
    return AEM_STATUS_SUCCESS;
}

void AemCommandHandler::tell_and_keep(
    uint16_t const port_type, uint16_t const port_index, std::span<DynamicAudioMapping const> const current)
{
    // What changed, as a controller would see it: the mappings gone, then those new, to every
    // registered controller (nobody asked over ATDECC). The store keeps @p current until then.
    Eui64 const outer_requester = std::exchange(requester_, Eui64{});
    notify_mappings(false, port_type, port_index, current, audio_maps_->next());
    notify_mappings(true, port_type, port_index, audio_maps_->next(), current);
    requester_ = outer_requester;
    (void)audio_maps_->replace(port_type, port_index, audio_maps_->next());  // admitted: it fits
}

void AemCommandHandler::notify_mappings(
    bool const add,
    uint16_t const port_type,
    uint16_t const port_index,
    std::span<DynamicAudioMapping const> const of,
    std::span<DynamicAudioMapping const> const but)
{
    std::array<uint8_t, MAX_AEM_BODY> body{};
    size_t count = 0;
    for (auto const& mapping : of) {
        if (std::ranges::binary_search(but, mapping, cluster_order)) {
            continue;
        }
        span_store(
            std::span{body}.subspan(
                AemAudioMappingsCommandHeader::LENGTH + (count * AemAudioMapping::LENGTH), AemAudioMapping::LENGTH),
            to_wire(mapping));
        if (++count == MAPPINGS_PER_PDU) {
            emit_mappings_pdu(add, port_type, port_index, body, std::exchange(count, 0));
        }
    }
    if (count != 0) {
        emit_mappings_pdu(add, port_type, port_index, body, count);
    }
}

void AemCommandHandler::emit_mappings_pdu(
    bool const add, uint16_t const port_type, uint16_t const port_index, std::span<uint8_t> const body, size_t const count)
{
    span_store(
        body.first(AemAudioMappingsCommandHeader::LENGTH),
        AemAudioMappingsCommandHeader{
            .descriptor_type = port_type, .descriptor_index = port_index, .number_of_mappings = static_cast<uint16_t>(count)});
    emit_unsolicited(
        add ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS,
        body.first(AemAudioMappingsCommandHeader::LENGTH + (count * AemAudioMapping::LENGTH)));
}

auto AemCommandHandler::handle_acquire_entity(
    AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // ACQUIRE_ENTITY command payload (AemAcquireEntityPayload):
    //   flags(4) + owner_entity_id(8) + descriptor_type(2) + descriptor_index(2)
    if (command_data.size() < AemAcquireEntityPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemAcquireEntityPayload cmd{};
    span_load(cmd, command_data);

    // 7.4.1.1: an entity shall support acquiring ENTITY and may support other
    // descriptor types; one that does not answers NOT_SUPPORTED. This entity is
    // acquired whole or not at all, release included. Under Milan v1.3 it is
    // not acquired at all: 5.4.2.1 has a Milan entity answer NOT_SUPPORTED to
    // every ACQUIRE_ENTITY, the lock being the one exclusive access it grants.
    if (profile_ == Profile::milan13 || cmd.descriptor_type.get() != DESCRIPTOR_ENTITY) {
        return build_acquire_response(
            {.status = AEM_STATUS_NOT_SUPPORTED,
             .flags = cmd.flags,
             .entity_id = acquiring_controller_,
             .command_data = command_data},
            out_buffer);
    }
    auto const status = cmd.is_release() ? release_acquisition(header) : take_acquisition(header, cmd, command_data);
    return acquire_outcome(status, cmd, command_data, out_buffer);
}

auto AemCommandHandler::release_acquisition(AemDu const& header) -> uint8_t
{
    if (!(acquired_ && acquiring_controller_ == header.controller_entity_id)) {
        return AEM_STATUS_ENTITY_ACQUIRED;
    }
    release_acquisition();
    // The owner letting go answers what the handshake was asking it: the
    // entity is free, and the contender waiting on that answer takes it now
    // rather than at the handshake's timeout, or never.
    if (pending_acquire_.active) {
        grant_pending_acquire();
    }
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::take_acquisition(
    AemDu const& header, AemAcquireEntityPayload const& cmd, std::span<uint8_t const> command_data) -> uint8_t
{
    if (!acquired_) {
        acquired_ = true;
        acquired_persistent_ = cmd.is_persistent();
        acquiring_controller_ = header.controller_entity_id;
        acquiring_controller_mac_ = current_src_mac_;
        return AEM_STATUS_SUCCESS;
    }
    if (acquiring_controller_ == header.controller_entity_id) {
        // Already this controller's, which may have moved (a local controller that
        // reconnected): its notifications and forget_controller_at follow the new address.
        acquiring_controller_mac_ = current_src_mac_;
        return AEM_STATUS_SUCCESS;
    }
    // While the owner is being asked on one contender's behalf the entity is
    // still the owner's: that contender asking again (its retransmit) is told
    // IN_PROGRESS once more, anyone else ENTITY_ACQUIRED, and the handshake
    // runs on for the one it was begun for.
    if (pending_acquire_.active) {
        bool const same_contender = pending_acquire_.requester_header.controller_entity_id == header.controller_entity_id;
        return same_contender ? AEM_STATUS_IN_PROGRESS : AEM_STATUS_ENTITY_ACQUIRED;
    }
    // Acquired by another controller: ask it whether it is still there, unless
    // its acquisition is PERSISTENT or nobody can carry the question. The final
    // answer then comes from controller_available_response_received() (deny) or
    // controller_available_timed_out() (grant).
    if (!acquired_persistent_ && (send_controller_available || callbacks_.send_response)) {
        begin_controller_available_handshake(header, cmd.flags, command_data);
        return AEM_STATUS_IN_PROGRESS;
    }
    return AEM_STATUS_ENTITY_ACQUIRED;
}

void AemCommandHandler::begin_controller_available_handshake(
    AemDu const& header, uint32_t const flags, std::span<uint8_t const> command_data)
{
    pending_acquire_ = PendingAcquire{
        .requester_header = header,
        .requester_mac = current_src_mac_,
        .requester_flags = flags,
        .command_data_copy = {},
        .timeout = event_time_ + std::chrono::milliseconds(AEM_TIMEOUT_MS),
        .sequence_id = 0,
        .framed = false,
        .active = true};
    std::copy_n(
        command_data.begin(),
        std::min(command_data.size(), pending_acquire_.command_data_copy.size()),
        pending_acquire_.command_data_copy.begin());
    ask_owner_available();
}

void AemCommandHandler::ask_owner_available()
{
    if (send_controller_available) {
        (void)send_controller_available(acquiring_controller_);
        return;
    }
    pending_acquire_.sequence_id = command_sequence_id_++;
    pending_acquire_.framed = true;
    send_controller_available_to(acquiring_controller_mac_, acquiring_controller_, pending_acquire_.sequence_id);
}

auto AemCommandHandler::answers_pending_acquire(AemDu const& header) const noexcept -> bool
{
    if (!pending_acquire_.active || !acquired_ || header.target_entity_id != acquiring_controller_) {
        return false;
    }
    return !pending_acquire_.framed || header.sequence_id.get() == pending_acquire_.sequence_id;
}

auto AemCommandHandler::acquire_outcome(
    uint8_t const status, AemAcquireEntityPayload const& cmd, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (status == AEM_STATUS_IN_PROGRESS) {
        return {.status = AEM_STATUS_IN_PROGRESS, .size = 0};
    }
    // A release that succeeded answers no flags; everything else echoes the
    // flags. Every answer names the owner: zero when the entity is free, the
    // contender a release handed it to, else whoever holds it.
    uint32_t const flags = (cmd.is_release() && status == AEM_STATUS_SUCCESS) ? 0U : uint32_t{cmd.flags};
    return build_acquire_response(
        {.status = status, .flags = flags, .entity_id = acquiring_controller_, .command_data = command_data}, out_buffer);
}

auto AemCommandHandler::build_acquire_response(AcquireLockResponseParams const& params, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    // ACQUIRE_ENTITY response shares the command layout (AemAcquireEntityPayload):
    //   flags(4) + owner_entity_id(8) + descriptor_type(2) + descriptor_index(2)
    AemAcquireEntityPayload resp{};
    resp.flags = params.flags;
    resp.owner_entity_id = params.entity_id;
    // Echo descriptor type/index from the command, if present.
    if (params.command_data.size() >= AemAcquireEntityPayload::LENGTH) {
        AemAcquireEntityPayload cmd{};
        span_load(cmd, params.command_data);
        resp.descriptor_type = cmd.descriptor_type;
        resp.descriptor_index = cmd.descriptor_index;
    }
    span_store(out_buffer, resp);

    return {.status = params.status, .size = AemAcquireEntityPayload::LENGTH};
}

auto AemCommandHandler::handle_lock_entity(
    AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // LOCK_ENTITY command payload (AemLockEntityPayload):
    //   flags(4) + locked_entity_id(8) + descriptor_type(2) + descriptor_index(2)
    if (command_data.size() < AemLockEntityPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemLockEntityPayload cmd{};
    span_load(cmd, command_data);
    // The response names whoever holds the lock once the command has been
    // applied: zero after an unlock, the holder after a lock or a refusal.
    auto const refused = lock_refusal(header, cmd);
    auto const status = refused ? *refused : (cmd.is_unlock() ? apply_unlock(header) : apply_lock(header));
    return build_lock_response(
        {.status = status, .flags = cmd.flags, .entity_id = locking_controller_, .command_data = command_data}, out_buffer);
}

auto AemCommandHandler::lock_refusal(AemDu const& header, AemLockEntityPayload const& cmd) const noexcept -> std::optional<uint8_t>
{
    // This entity is locked whole or not at all: a command naming any other
    // descriptor type is NOT_SUPPORTED (the ACQUIRE_ENTITY rule of 7.4.1.1,
    // which Milan asks of LOCK_ENTITY as well), unlock included.
    if (cmd.descriptor_type.get() != DESCRIPTOR_ENTITY) {
        return AEM_STATUS_NOT_SUPPORTED;
    }
    // 7.4.2.2: while acquired, only the acquirer may lock or unlock; every other
    // controller is told ENTITY_ACQUIRED. Under AcquireMode::shared anyone may.
    if (acquisition_shuts_out(header.controller_entity_id)) {
        return AEM_STATUS_ENTITY_ACQUIRED;
    }
    return std::nullopt;
}

auto AemCommandHandler::apply_lock(AemDu const& header) noexcept -> uint8_t
{
    if (locked_ && !(locking_controller_ == header.controller_entity_id)) {
        return AEM_STATUS_ENTITY_LOCKED;
    }
    // A fresh lock, or the holder keeping its lock alive (7.4.2).
    locked_ = true;
    locking_controller_ = header.controller_entity_id;
    lock_expiry_time_ = event_time_ + std::chrono::milliseconds(AEM_LOCK_TIMEOUT_MS);
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::apply_unlock(AemDu const& header) noexcept -> uint8_t
{
    if (locked_ && !(locking_controller_ == header.controller_entity_id)) {
        return AEM_STATUS_ENTITY_LOCKED;
    }
    // The holder letting go, or nothing to let go of: both succeed.
    locked_ = false;
    locking_controller_ = {};
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::build_lock_response(AcquireLockResponseParams const& params, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    // LOCK_ENTITY response shares the command layout (AemLockEntityPayload):
    //   flags(4) + locked_entity_id(8) + descriptor_type(2) + descriptor_index(2)
    AemLockEntityPayload resp{};
    resp.flags = params.flags;
    resp.locked_entity_id = params.entity_id;
    // Echo descriptor type/index from the command, if present.
    if (params.command_data.size() >= AemLockEntityPayload::LENGTH) {
        AemLockEntityPayload cmd{};
        span_load(cmd, params.command_data);
        resp.descriptor_type = cmd.descriptor_type;
        resp.descriptor_index = cmd.descriptor_index;
    }
    span_store(out_buffer, resp);

    return {.status = params.status, .size = AemLockEntityPayload::LENGTH};
}

auto AemCommandHandler::handle_get_configuration(AemDu const& /*header*/, std::span<uint8_t> out_buffer) const -> AemCommandResponse
{
    // GET_CONFIGURATION response shares the SET_CONFIGURATION layout
    // (AemSetConfigurationPayload): reserved(2) + configuration_index(2).
    AemSetConfigurationPayload resp{};
    resp.configuration_index = current_configuration_;
    span_store(out_buffer, resp);

    return {.status = AEM_STATUS_SUCCESS, .size = AemSetConfigurationPayload::LENGTH};
}

auto AemCommandHandler::handle_set_configuration(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    using namespace atdecc::aem;
    if (command_data.size() < AemSetConfigurationPayload::LENGTH || out_buffer.size() < AemSetConfigurationPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    AemSetConfigurationPayload cmd{};
    span_load(cmd, command_data);
    uint16_t const requested = cmd.configuration_index;

    // Validate against the ENTITY descriptor's configurations_count.
    std::array<uint8_t, AEM_DESCRIPTOR_SIZE> desc{};
    AemEntityModel const aem_model{*handler_};
    auto const n = aem_model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, make_span(desc));
    constexpr size_t CFG_COUNT_OFFSET = offsetof(DescriptorEntity, configurations_count);
    uint16_t configurations_count = 1;
    if (n >= CFG_COUNT_OFFSET + 2) {
        doublet_t count{};
        span_load(count, make_const_span(desc, {.start = CFG_COUNT_OFFSET, .length = 2}));
        configurations_count = count.get();
    }
    if (requested >= configurations_count) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }

    bool const switching = requested != current_configuration_;
    if (switching) {
        // Without an application hook the data plane cannot actually
        // re-shape itself; refuse the switch rather than claim a
        // configuration the streams are not built for.
        if (!on_configuration_changed_) {
            return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
        }
        if (auto const status = on_configuration_changed_(requested); status != AEM_STATUS_SUCCESS) {
            return {.status = status, .size = 0};
        }
        current_configuration_ = requested;
    }

    AemSetConfigurationPayload resp{};
    resp.configuration_index = current_configuration_;
    span_store(out_buffer, resp);
    if (switching) {
        emit_unsolicited(
            AEM_COMMAND_SET_CONFIGURATION, std::span<uint8_t const>{out_buffer.data(), AemSetConfigurationPayload::LENGTH});
    }
    return {.status = AEM_STATUS_SUCCESS, .size = AemSetConfigurationPayload::LENGTH};
}

auto AemCommandHandler::answer_set_configuration(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    uint16_t const before = current_configuration_;
    auto const r = refuse_unless_success(
        AEM_COMMAND_SET_CONFIGURATION, handle_set_configuration(command_data, out_buffer), command_data, out_buffer);
    if (current_configuration_ != before) {
        forget_configuration_state();
    }
    return r;
}

void AemCommandHandler::forget_configuration_state()
{
    mcr_info_ = {};  // the new configuration's CLOCK_DOMAINs start from their defaults
    if (audio_maps_ != nullptr) {
        audio_maps_->clear();  // they named the old configuration's ports
    }
}

auto AemCommandHandler::send_response(
    Eui48 const& dest_mac, AemDu const& cmd_header, uint8_t status, std::span<uint8_t const> response_body) const -> bool
{
    if (!callbacks_.send_response) {
        return false;
    }

    // Build response packet: AemDu header + response data
    std::array<uint8_t, AemDu::LENGTH + MAX_AEM_RESPONSE_SIZE> response_buffer{};

    // Copy and modify header for response
    AemDu response_header = cmd_header;
    response_header.init_response(
        cmd_header.command_code(),
        status,
        static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + response_body.size()),
        cmd_header.is_unsolicited());

    // Copy header to buffer
    auto buffer_span = std::span{response_buffer};
    span_store(buffer_span, response_header);

    // Copy response body
    if (!response_body.empty()) {
        span_copy(buffer_span.subspan(AemDu::LENGTH), response_body);
    }

    size_t const total_size = AemDu::LENGTH + response_body.size();
    return callbacks_.send_response(dest_mac, {response_buffer.data(), total_size});
}

// ──────────────────────────────────────────────
// Milan vendor unique (MVU) - Milan v1.3 5.4.3, 5.4.4
// ──────────────────────────────────────────────

namespace {

/// The command-specific data of the MVU command @p header in @p payload: what
/// its control_data_length covers, not the Ethernet padding behind a short
/// frame (5.4.3.2).
[[nodiscard]] auto mvu_command_data(AecpMvuDu const& header, std::span<uint8_t const> const payload) noexcept
    -> std::span<uint8_t const>
{
    size_t const declared = header.common.control_data_length() - AecpMvuDu::MVU_DATA_LENGTH;
    return payload.subspan(AecpMvuDu::LENGTH, std::min(declared, payload.size() - AecpMvuDu::LENGTH));
}

/// The header of the direct response to @p cmd_header: its fields with the
/// response message type, @p status and a length covering @p body_size octets
/// of data; u stays clear on a direct response (5.4.3.2.2).
[[nodiscard]] auto mvu_response_header(AecpMvuDu const& cmd_header, uint8_t const status, size_t const body_size) noexcept
    -> AecpMvuDu
{
    AecpMvuDu response = cmd_header;
    response.init_response(cmd_header.command_type(), status, static_cast<uint16_t>(body_size));
    return response;
}

/// The header of an MVU notification of @p command_type, carrying @p body_size
/// octets of data, from @p entity_id to @p controller_id: a response with the
/// u bit set (5.4.3.2.2).
[[nodiscard]] auto mvu_unsolicited_header(
    Eui64 const& entity_id,
    Eui64 const& controller_id,
    uint16_t const sequence_id,
    uint16_t const command_type,
    size_t const body_size) noexcept -> AecpMvuDu
{
    AecpMvuDu header{};
    header.init_response(command_type, MVU_STATUS_SUCCESS, static_cast<uint16_t>(body_size), /*unsolicited=*/true);
    header.common.target_entity_id = entity_id;
    header.common.controller_entity_id = controller_id;
    header.common.sequence_id = doublet_t{sequence_id};
    return header;
}

/// The header of an unsolicited AEM response (u bit set) to @p controller_id from @p entity_id,
/// with @p sequence_id, for @p command_type and a body of @p body_size octets.
[[nodiscard]] auto aem_unsolicited_header(
    Eui64 const& entity_id,
    Eui64 const& controller_id,
    uint16_t const sequence_id,
    uint16_t const command_type,
    size_t const body_size) noexcept -> AemDu
{
    AemDu header{};
    header.target_entity_id = entity_id;
    header.controller_entity_id = controller_id;
    header.sequence_id = doublet_t{sequence_id};
    header.init_response(
        command_type, AEM_STATUS_SUCCESS, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + body_size), /*unsolicited=*/true);
    return header;
}

/// CONTROLLER_AVAILABLE (IEEE 1722.1-2021 Clause 7.4.4, no payload) to @p controller_id with
/// @p sequence_id: @p entity_id is the command's controller, the registered controller its target.
[[nodiscard]] auto controller_available_command(
    Eui64 const& entity_id, Eui64 const& controller_id, uint16_t const sequence_id) noexcept -> AemDu
{
    AemDu command{};
    command.init_command(AEM_COMMAND_CONTROLLER_AVAILABLE, AemDu::AEM_DATA_LENGTH);
    command.target_entity_id = controller_id;
    command.controller_entity_id = entity_id;
    command.sequence_id = doublet_t{sequence_id};
    return command;
}

/// The response a command gets when the entity does not implement it
/// (Table 5.16): the command's data goes back as it came, as an AEM
/// response to an unimplemented command carries its command's.
[[nodiscard]] auto mvu_not_implemented(std::span<uint8_t const> const data, std::span<uint8_t> const out_buffer) noexcept
    -> AemCommandResponse
{
    size_t const echoed = std::min(data.size(), out_buffer.size());
    span_copy(out_buffer.first(echoed), data.first(echoed));
    return {.status = MVU_STATUS_NOT_IMPLEMENTED, .size = echoed};
}

/// A BIND_STREAM or UNBIND_STREAM command as read from its data: the ACMP
/// command it equals (Table 5.22) and the data its response carries, which
/// is the command's with the reserved field cleared.
struct MvuBinding
{
    uint16_t descriptor_type{0};
    atdecc::AcmpCommandResponse as_acmp{};
    std::array<uint8_t, MvuBindStream::LENGTH> response{};
    size_t response_size{0};

    [[nodiscard]] auto response_data() const noexcept -> std::span<uint8_t const>
    {
        return std::span{response}.first(response_size);
    }
};

[[nodiscard]] auto mvu_bind_stream(AecpMvuDu const& header, std::span<uint8_t const> const data) noexcept -> MvuBinding
{
    MvuBindStream bind{};
    span_load(bind, data.first(MvuBindStream::LENGTH));
    bind.reserved = 0;
    MvuBinding binding{
        .descriptor_type = bind.descriptor_type.get(),
        .as_acmp = atdecc::milan_bind_command_from_mvu(header, bind),
        .response_size = MvuBindStream::LENGTH};
    span_store(std::span{binding.response}, bind);
    return binding;
}

[[nodiscard]] auto mvu_unbind_stream(AecpMvuDu const& header, std::span<uint8_t const> const data) noexcept -> MvuBinding
{
    MvuStreamInput unbind{};
    span_load(unbind, data.first(MvuStreamInput::LENGTH));
    unbind.reserved = 0;
    MvuBinding binding{
        .descriptor_type = unbind.descriptor_type.get(),
        .as_acmp = atdecc::milan_unbind_command_from_mvu(header, unbind),
        .response_size = MvuStreamInput::LENGTH};
    span_store(std::span{binding.response}, unbind);
    return binding;
}

/// The binding command in @p data, nullopt when it is shorter than its format.
[[nodiscard]] auto mvu_binding_for(AecpMvuDu const& header, std::span<uint8_t const> const data) noexcept
    -> std::optional<MvuBinding>
{
    bool const bind = header.command_type() == MVU_COMMAND_BIND_STREAM;
    if (data.size() < (bind ? MvuBindStream::LENGTH : MvuStreamInput::LENGTH)) {
        return std::nullopt;
    }
    return bind ? mvu_bind_stream(header, data) : mvu_unbind_stream(header, data);
}

/// The STREAM_INPUT a GET_STREAM_INPUT_INFO_EX command names, its reserved
/// field ignored (5.4.4.8).
[[nodiscard]] auto mvu_stream_input_asked(std::span<uint8_t const> const data) noexcept -> MvuStreamInput
{
    MvuStreamInput asked{};
    span_load(asked, data.first(MvuStreamInput::LENGTH));
    asked.reserved = 0;
    return asked;
}

/// A GET_STREAM_INPUT_INFO_EX response that refuses with @p status: the
/// response's own format (Figure 5.12), so that a controller which checks a
/// response's length before its status still reads it, with the descriptor
/// @p asked named and every other field zero.
[[nodiscard]] auto mvu_stream_input_refused(uint8_t const status, MvuStreamInput const& asked, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{
    MvuGetStreamInputInfoExResponse refused{};
    refused.descriptor_type = asked.descriptor_type;
    refused.descriptor_index = asked.descriptor_index;
    span_store(out_buffer, refused);
    return {.status = status, .size = MvuGetStreamInputInfoExResponse::LENGTH};
}

}  // namespace

auto AemCommandHandler::mvu_command_for(std::span<uint8_t const> const payload, Eui64 const& our_entity_id) noexcept
    -> std::optional<MvuCommand>
{
    if (payload.size() < AecpMvuDu::LENGTH) {
        return std::nullopt;
    }
    MvuCommand command{};
    span_load(command.header, payload);
    if (!command.header.is_valid() || !command.header.is_command() || command.header.common.target_entity_id != our_entity_id) {
        return std::nullopt;
    }
    command.data = mvu_command_data(command.header, payload);
    return command;
}

auto AemCommandHandler::process_mvu_packet(Eui48 const& src_mac, std::span<uint8_t const> const payload, Eui64 const& our_entity_id)
    -> bool
{
    // Not a Milan device: no answer, as 5.4.4.1 recommends of a Configuration
    // that is not Milan, and as the other MVU commands then have no meaning.
    if (!milan_info_) {
        return false;
    }
    auto const command = mvu_command_for(payload, our_entity_id);
    if (!command) {
        return false;
    }
    our_entity_id_ = our_entity_id;  // the target_entity_id of the notifications it raises
    restart_controller_monitor(command->header.common.controller_entity_id);
    return dispatch_mvu_command(src_mac, *command);
}

auto AemCommandHandler::dispatch_mvu_command(Eui48 const& src_mac, MvuCommand const& command) -> bool
{
    // As handle_command() does for AEM: the requester is left out of the fan-out (5.4.5.2).
    Eui64 const outer_requester = std::exchange(requester_, command.header.common.controller_entity_id);
    bool const sent =
        is_mvu_binding_command(command) ? answer_mvu_binding_and_notify(src_mac, command) : answer_mvu_command(src_mac, command);
    requester_ = outer_requester;
    return sent;
}

auto AemCommandHandler::answer_mvu_command(Eui48 const& src_mac, MvuCommand const& command) -> bool
{
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> out_buffer{};
    auto const response = handle_mvu_command(command, out_buffer);
    auto const body = std::span<uint8_t const>{out_buffer.data(), response.size};
    bool const sent = send_mvu_response(src_mac, command.header, response.status, body);
    if (std::exchange(mvu_changed_, false)) {
        emit_mvu_unsolicited(command.header.command_type(), body);  // after the response (5.4.5.3)
        if (callbacks_.on_mvu_settings_changed) {
            callbacks_.on_mvu_settings_changed(command.header.command_type());
        }
    }
    return sent;
}

auto AemCommandHandler::handle_mvu_command(MvuCommand const& command, std::span<uint8_t> const out_buffer) -> AemCommandResponse
{
    switch (command.header.command_type()) {
        case MVU_COMMAND_GET_MILAN_INFO:
            return handle_mvu_get_milan_info(command, out_buffer);
        case MVU_COMMAND_SET_SYSTEM_UNIQUE_ID:
            return handle_mvu_set_system_unique_id(command, out_buffer);
        case MVU_COMMAND_GET_SYSTEM_UNIQUE_ID:
            return handle_mvu_get_system_unique_id(out_buffer);
        case MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO:
            return handle_mvu_set_mcr_info(command, out_buffer);
        case MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO:
            return handle_mvu_get_mcr_info(command, out_buffer);
        case MVU_COMMAND_GET_STREAM_INPUT_INFO_EX:
            return handle_mvu_get_stream_input_info_ex(command, out_buffer);
        default:
            return mvu_not_implemented(command.data, out_buffer);
    }
}

auto AemCommandHandler::handle_mvu_get_milan_info(MvuCommand const& command, std::span<uint8_t> const out_buffer) const
    -> AemCommandResponse
{
    if (command.data.size() < MVU_GET_MILAN_INFO_COMMAND_LENGTH) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    span_store(out_buffer, milan_info_response());
    return {.status = MVU_STATUS_SUCCESS, .size = MvuGetMilanInfoResponse::LENGTH};
}

auto AemCommandHandler::handle_mvu_get_system_unique_id(MvuCommand const& command, std::span<uint8_t> const out_buffer) const
    -> AemCommandResponse
{
    if (command.data.size() < MVU_GET_SYSTEM_UNIQUE_ID_COMMAND_LENGTH) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    span_store(out_buffer, system_unique_id());
    return {.status = MVU_STATUS_SUCCESS, .size = MvuSystemUniqueIdPayload::LENGTH};
}

auto AemCommandHandler::handle_mvu_set_system_unique_id(MvuCommand const& command, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{
    auto const request = load_payload<MvuSystemUniqueIdPayload>(command.data);
    if (!request) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    uint8_t const status = system_unique_id_set_status(command.header.common.controller_entity_id, *request);
    if (status == MVU_STATUS_SUCCESS) {
        apply_system_unique_id(*request);
    }
    span_store(out_buffer, system_unique_id());
    return {.status = status, .size = MvuSystemUniqueIdPayload::LENGTH};
}

void AemCommandHandler::apply_system_unique_id(MvuSystemUniqueIdPayload const& request) noexcept
{
    AtdeccString const name{request.system_name.as_string_view()};  // the octets past its end zeroed
    mvu_changed_ = request.system_unique_id.get() != system_unique_id_ || std::is_neq(name <=> system_name_);
    system_unique_id_ = request.system_unique_id.get();
    system_name_ = name;
    if (mvu_changed_ && on_system_unique_id_changed_) {
        on_system_unique_id_changed_(system_unique_id());
    }
}

auto AemCommandHandler::restore_system_unique_id(uint64_t const id, AtdeccString const& name) noexcept -> bool
{
    if (!atdecc::utf8_well_formed(name.as_string_view())) {
        return false;
    }
    system_unique_id_ = id;
    system_name_ = AtdeccString{name.as_string_view()};
    return true;
}

auto AemCommandHandler::system_unique_id_set_status(Eui64 const& controller, MvuSystemUniqueIdPayload const& request) const
    -> uint8_t
{
    // The lock first, as for SET_MEDIA_CLOCK_REFERENCE_INFO: a locked-out controller learns nothing of its arguments.
    if (exclusive_access_status(controller)) {
        return MVU_STATUS_ENTITY_LOCKED;
    }
    return atdecc::utf8_well_formed(request.system_name.as_string_view()) ? MVU_STATUS_SUCCESS : MVU_STATUS_BAD_ARGUMENTS;
}

auto AemCommandHandler::handle_mvu_get_mcr_info(MvuCommand const& command, std::span<uint8_t> const out_buffer) const
    -> AemCommandResponse
{
    auto const request = load_payload<MvuGetMediaClockReferenceInfoCommand>(command.data);
    if (!request) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    uint16_t const index = request->clock_domain_index.get();
    if (!has_clock_domain(index)) {
        span_store(out_buffer, *request);
        return {.status = MVU_STATUS_NO_SUCH_DESCRIPTOR, .size = MvuGetMediaClockReferenceInfoCommand::LENGTH};
    }
    span_store(out_buffer, media_clock_reference_info(index));
    return {.status = MVU_STATUS_SUCCESS, .size = MvuMediaClockReferenceInfoPayload::LENGTH};
}

auto AemCommandHandler::handle_mvu_set_mcr_info(MvuCommand const& command, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{
    auto const request = load_payload<MvuMediaClockReferenceInfoPayload>(command.data);
    if (!request) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    uint8_t const status = mcr_set_status(command.header.common.controller_entity_id, *request);
    if (status == MVU_STATUS_SUCCESS) {
        apply_mcr_info(*request);
    }
    span_store(out_buffer, mcr_info_answer(*request));
    return {.status = status, .size = MvuMediaClockReferenceInfoPayload::LENGTH};
}

auto AemCommandHandler::system_unique_id_payload() const noexcept -> MvuSystemUniqueIdPayloadV13
{
    return MvuSystemUniqueIdPayloadV13{.number = system_unique_id_, .name = system_unique_name_};
}

auto AemCommandHandler::handle_mvu_get_system_unique_id(std::span<uint8_t> const out_buffer) const -> AemCommandResponse
{
    // The reserved field is ignored, as for GET_MILAN_INFO.
    span_store(out_buffer, system_unique_id_payload());
    return {.status = MVU_STATUS_SUCCESS, .size = MvuSystemUniqueIdPayloadV13::LENGTH};
}

auto AemCommandHandler::handle_mvu_set_system_unique_id(MvuCommand const& command, std::span<uint8_t> const out_buffer)
    -> AemCommandResponse
{
    // Milan v1.3 Figure 5.7: a u64 number and a 64-octet name.
    auto const request = load_payload<MvuSystemUniqueIdPayloadV13>(command.data);
    if (!request) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    uint8_t status = MVU_STATUS_SUCCESS;
    if (exclusive_access_status(command.header.common.controller_entity_id)) {
        status = MVU_STATUS_ENTITY_LOCKED;
    } else if (!atdecc::utf8_well_formed(request->name.as_string_view())) {
        status = MVU_STATUS_BAD_ARGUMENTS;
    } else {
        mvu_changed_ = request->number.get() != system_unique_id_ || request->name.value != system_unique_name_.value;
        set_system_unique_id(request->number.get(), request->name);
    }
    span_store(out_buffer, system_unique_id_payload());
    return {.status = status, .size = MvuSystemUniqueIdPayloadV13::LENGTH};
}

auto AemCommandHandler::mcr_info_answer(MvuMediaClockReferenceInfoPayload const& request) const -> MvuMediaClockReferenceInfoPayload
{
    // The settings in force, as a failed AEM SET answers its value; the command for no domain.
    uint16_t const index = request.clock_domain_index.get();
    return has_clock_domain(index) ? media_clock_reference_info(index) : request;
}

auto AemCommandHandler::mcr_set_status(Eui64 const& controller, MvuMediaClockReferenceInfoPayload const& request) const -> uint8_t
{
    // The lock first, as for every SET; MVU has no ENTITY_ACQUIRED, so an acquisition answers the same.
    if (exclusive_access_status(controller)) {
        return MVU_STATUS_ENTITY_LOCKED;
    }
    if (!has_clock_domain(request.clock_domain_index.get())) {
        return MVU_STATUS_NO_SUCH_DESCRIPTOR;
    }
    bool const naming = request.flags.has_flag(MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID);
    bool const well_formed = !naming || atdecc::utf8_well_formed(request.media_clock_domain_name.as_string_view());
    return well_formed ? MVU_STATUS_SUCCESS : MVU_STATUS_BAD_ARGUMENTS;
}

void AemCommandHandler::apply_mcr_info(MvuMediaClockReferenceInfoPayload const& request)
{
    uint16_t const index = request.clock_domain_index.get();
    auto const before = media_clock_reference_info(index);
    auto& info = mcr_info_[index];
    if (request.flags.has_flag(MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID)) {
        info.user_mcr_prio = request.user_mcr_prio.get();
    }
    if (request.flags.has_flag(MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID)) {
        info.domain_name = aem::AtdeccString{request.media_clock_domain_name.as_string_view()};
    }
    mvu_changed_ = !std::ranges::equal(make_const_span(before), make_const_span(media_clock_reference_info(index)));
}

auto AemCommandHandler::has_clock_domain(uint16_t const index) const -> bool
{
    if (index >= MAX_CLOCK_DOMAINS) {
        return false;
    }
    std::array<uint8_t, atdecc::aem::AEM_DESCRIPTOR_SIZE> scratch{};
    AemEntityModel const aem_model{*handler_};
    DescriptorRef const ref{
        .configuration_index = current_configuration_, .descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .descriptor_index = index};
    return aem_model.get_descriptor_for_wire(ref, scratch) != 0;
}

auto AemCommandHandler::media_clock_reference_info(uint16_t const clock_domain_index) const noexcept
    -> MvuMediaClockReferenceInfoPayload
{
    bool const held = clock_domain_index < MAX_CLOCK_DOMAINS;
    uint8_t const default_prio = held ? default_mcr_prio_[clock_domain_index] : MVU_MCR_PRIO_DEFAULT;
    McrInfo const kept = held ? mcr_info_[clock_domain_index] : McrInfo{};
    return MvuMediaClockReferenceInfoPayload{
        .clock_domain_index = clock_domain_index,
        .flags = MVU_MCR_FLAGS_SUPPORTED,
        .default_mcr_prio = default_prio,
        .user_mcr_prio = kept.user_mcr_prio.value_or(default_prio),
        .media_clock_domain_name = kept.domain_name};
}

auto AemCommandHandler::restore_media_clock_reference_info(
    uint16_t const clock_domain_index, uint8_t const user_mcr_prio, aem::AtdeccString const& domain_name) noexcept -> bool
{
    if (clock_domain_index >= MAX_CLOCK_DOMAINS) {
        return false;
    }
    mcr_info_[clock_domain_index] = McrInfo{.user_mcr_prio = user_mcr_prio, .domain_name = domain_name};
    return true;
}

auto AemCommandHandler::set_default_mcr_priority(uint16_t const clock_domain_index, uint8_t const prio) noexcept -> bool
{
    if (clock_domain_index >= MAX_CLOCK_DOMAINS) {
        return false;
    }
    default_mcr_prio_[clock_domain_index] = prio;
    return true;
}

auto AemCommandHandler::handle_mvu_get_stream_input_info_ex(MvuCommand const& command, std::span<uint8_t> const out_buffer) const
    -> AemCommandResponse
{
    if (!mvu_stream_inputs_.stream_input_info) {
        return mvu_not_implemented(command.data, out_buffer);
    }
    if (command.data.size() < MvuStreamInput::LENGTH) {
        return {.status = MVU_STATUS_PAYLOAD_TOO_SHORT, .size = 0};
    }
    return mvu_stream_input_info_ex(mvu_stream_input_asked(command.data), out_buffer);
}

auto AemCommandHandler::mvu_stream_input_info_ex(MvuStreamInput const& asked, std::span<uint8_t> const out_buffer) const
    -> AemCommandResponse
{
    if (asked.descriptor_type.get() != DESCRIPTOR_STREAM_INPUT) {
        return mvu_stream_input_refused(MVU_STATUS_BAD_ARGUMENTS, asked, out_buffer);
    }
    auto const info = mvu_stream_inputs_.stream_input_info(asked.descriptor_index.get());
    if (!info.has_value()) {
        return mvu_stream_input_refused(MVU_STATUS_NO_SUCH_DESCRIPTOR, asked, out_buffer);
    }
    span_store(out_buffer, *info);
    return {.status = MVU_STATUS_SUCCESS, .size = MvuGetStreamInputInfoExResponse::LENGTH};
}

auto AemCommandHandler::is_mvu_binding_command(MvuCommand const& command) const noexcept -> bool
{
    // With no Milan connection management attached the two are commands
    // this entity does not implement, and are answered as any such command.
    uint16_t const type = command.header.command_type();
    return mvu_stream_inputs_.binding_command && (type == MVU_COMMAND_BIND_STREAM || type == MVU_COMMAND_UNBIND_STREAM);
}

auto AemCommandHandler::answer_mvu_binding_command(Eui48 const& src_mac, MvuCommand const& command) const -> bool
{
    auto const binding = mvu_binding_for(command.header, command.data);
    if (!binding.has_value()) {
        return send_mvu_response(src_mac, command.header, MVU_STATUS_PAYLOAD_TOO_SHORT, {});
    }
    if (binding->descriptor_type != DESCRIPTOR_STREAM_INPUT) {
        return send_mvu_response(src_mac, command.header, MVU_STATUS_BAD_ARGUMENTS, binding->response_data());
    }
    return give_mvu_binding_to_its_sink(src_mac, command.header, binding->as_acmp, binding->response_data());
}

auto AemCommandHandler::give_mvu_binding_to_its_sink(
    Eui48 const& src_mac,
    AecpMvuDu const& header,
    atdecc::AcmpCommandResponse const& as_acmp,
    std::span<uint8_t const> const response_data) const -> bool
{
    // The sink answers from inside its treatment (5.5.3.6.3 step 3 comes
    // before the probe of step 5), and its ACMP status names the MVU one.
    std::optional<bool> sent{};
    MvuStreamInputCallbacks::Respond const respond = [&](atdecc::AcmpCommandResponse const& response) {
        uint8_t const status = atdecc::mvu_status_for_binding_response(response.status());
        sent = send_mvu_response(src_mac, header, status, response_data);
    };
    mvu_stream_inputs_.binding_command(as_acmp, respond);
    return sent.has_value() ? *sent : send_mvu_response(src_mac, header, MVU_STATUS_ENTITY_MISBEHAVING, response_data);
}

auto AemCommandHandler::mvu_binding_in_force(MvuCommand const& command) const -> std::optional<atdecc::MilanBinding>
{
    auto const named = mvu_binding_for(command.header, command.data);
    if (!named.has_value() || !mvu_stream_inputs_.binding) {
        return std::nullopt;
    }
    return mvu_stream_inputs_.binding(named->as_acmp.listener_unique_id.get());
}

auto AemCommandHandler::answer_mvu_binding_and_notify(Eui48 const& src_mac, MvuCommand const& command) -> bool
{
    // Milan v1.3 5.4.5.3: a BIND_STREAM or UNBIND_STREAM answered SUCCESS
    // that changed the entity's state is notified to the registered
    // controllers but the one that sent it, after its response. A sink
    // changes its binding parameters (5.3.8.3) only for a command it takes,
    // so a binding that is not what it was says both. The notification
    // carries what the response carried.
    auto const before = mvu_binding_in_force(command);
    bool const sent = answer_mvu_binding_command(src_mac, command);
    auto const named = mvu_binding_for(command.header, command.data);
    if (named.has_value() && mvu_binding_in_force(command) != before) {
        emit_mvu_unsolicited(command.header.command_type(), named->response_data());
    }
    return sent;
}

auto AemCommandHandler::milan_info_response() const noexcept -> MvuGetMilanInfoResponse
{
    MvuGetMilanInfoResponse info{};
    info.protocol_version = MVU_PROTOCOL_VERSION;
    info.features_flags = milan_info_->features_flags;
    info.certification_version = milan_info_->certification_version;
    info.specification_version = milan_info_->specification_version;
    return info;
}

auto AemCommandHandler::send_mvu_response(
    Eui48 const& dest_mac, AecpMvuDu const& cmd_header, uint8_t const status, std::span<uint8_t const> const response_body) const
    -> bool
{
    return send_mvu_pdu(dest_mac, mvu_response_header(cmd_header, status, response_body.size()), response_body);
}

auto AemCommandHandler::send_mvu_pdu(Eui48 const& dest_mac, AecpMvuDu const& header, std::span<uint8_t const> const body) const
    -> bool
{
    if (!callbacks_.send_response || body.size() > MAX_AEM_RESPONSE_SIZE) {
        return false;
    }
    std::array<uint8_t, AecpMvuDu::LENGTH + MAX_AEM_RESPONSE_SIZE> buffer{};
    auto const bytes = std::span{buffer};
    span_store(bytes, header);
    span_copy(bytes.subspan(AecpMvuDu::LENGTH, body.size()), body);
    return callbacks_.send_response(dest_mac, bytes.first(AecpMvuDu::LENGTH + body.size()));
}

void AemCommandHandler::emit_mvu_unsolicited(uint16_t const command_type, std::span<uint8_t const> const body)
{
    if (!milan_info_ || !callbacks_.send_response) {
        return;  // a plain 1722.1 entity has no MVU to notify with; nothing is sent, no id spent
    }
    for (auto& reg : unsolicited_registrations_) {
        if (reg.controller_entity_id == requester_) {
            continue;
        }
        auto const header =
            mvu_unsolicited_header(our_entity_id_, reg.controller_entity_id, reg.mvu_sequence_id++, command_type, body.size());
        (void)send_mvu_pdu(reg.controller_mac, header, body);
    }
}

auto AemCommandHandler::notify_stream_input_info_ex(uint16_t const stream_input_index) -> bool
{
    auto const& ask = mvu_stream_inputs_.stream_input_info;
    auto const info = ask ? ask(stream_input_index) : std::nullopt;
    if (!info.has_value()) {
        return false;
    }
    emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, make_const_span(*info));
    return true;
}

auto AemCommandHandler::handle_register_unsolicited(AemDu const& header) -> AemCommandResponse
{
    // The source MAC (set by process_packet) is where its notifications are unicast; its
    // sequence ids start at zero (Milan v1.3 Section 5.4.2.21). Registering again is idempotent
    // from the same station, and starts afresh from another one.
    UnsolicitedRegistration const fresh{
        .controller_entity_id = header.controller_entity_id,
        .controller_mac = current_src_mac_,
        .monitor_deadline = event_time_ + monitor_period()};
    auto const existing = unsolicited_registrations_.find_if(
        [&header](UnsolicitedRegistration const& r) { return r.controller_entity_id == header.controller_entity_id; });
    if (existing < unsolicited_registrations_.capacity()) {
        auto& reg = unsolicited_registrations_[existing];
        if (reg.controller_mac != current_src_mac_) {
            reg = fresh;
        }
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }
    if (!unsolicited_registrations_.add(fresh)) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = 0};
    }
    return {.status = AEM_STATUS_SUCCESS, .size = 0};
}

void AemCommandHandler::restart_controller_monitor(Eui64 const& controller_entity_id)
{
    auto const idx = unsolicited_registrations_.find_if(
        [&](UnsolicitedRegistration const& r) { return r.controller_entity_id == controller_entity_id; });
    if (auto* const reg = unsolicited_registrations_.get(idx)) {
        reg->monitor_deadline = event_time_ + monitor_period();
        reg->available_tries = 0;
    }
}

auto AemCommandHandler::monitor_period() noexcept -> sm::Clock::duration
{
    // xorshift32: spreading the controllers' timers is all the period asks of it.
    monitor_random_ ^= monitor_random_ << 13U;
    monitor_random_ ^= monitor_random_ >> 17U;
    monitor_random_ ^= monitor_random_ << 5U;
    auto const span = std::chrono::duration_cast<std::chrono::milliseconds>(CONTROLLER_MONITOR_MAX - CONTROLLER_MONITOR_MIN);
    auto const offset = std::chrono::milliseconds{monitor_random_ % (static_cast<uint32_t>(span.count()) + 1U)};
    return std::chrono::duration_cast<sm::Clock::duration>(CONTROLLER_MONITOR_MIN + offset);
}

void AemCommandHandler::tick_controller_monitor(sm::TimePoint const now)
{
    // An entry removed has the last one moved into its slot (SlotTable::remove): i stays.
    for (size_t i = 0; i < unsolicited_registrations_.size();) {
        if (keep_monitoring(unsolicited_registrations_[i], now)) {
            ++i;
            continue;
        }
        // Neither answered: deregistered, and told so alone (Milan v1.3 Section 5.4.5.4).
        auto const gone = unsolicited_registrations_[i];
        unsolicited_registrations_.remove(i);
        send_unsolicited_to(
            gone.controller_mac, gone.controller_entity_id, gone.sequence_id, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, {});
    }
}

auto AemCommandHandler::keep_monitoring(UnsolicitedRegistration& reg, sm::TimePoint const now) -> bool
{
    if (now < reg.monitor_deadline) {
        return true;
    }
    if (reg.available_tries >= 2) {
        return false;
    }
    ask_controller_available(reg, now);
    return true;
}

void AemCommandHandler::ask_controller_available(UnsolicitedRegistration& reg, sm::TimePoint const now)
{
    // The command, then its one retry with the same sequence_id.
    reg.available_sequence_id = reg.available_tries == 0 ? command_sequence_id_++ : reg.available_sequence_id;
    ++reg.available_tries;
    reg.monitor_deadline = now + std::chrono::milliseconds(AEM_TIMEOUT_MS);
    send_controller_available_to(reg.controller_mac, reg.controller_entity_id, reg.available_sequence_id);
}

void AemCommandHandler::send_controller_available_to(Eui48 const& dest_mac, Eui64 const& controller_id, uint16_t const sequence_id)
{
    if (!callbacks_.send_response) {
        return;
    }
    std::array<uint8_t, AemDu::LENGTH> buffer{};
    span_store(buffer, controller_available_command(our_entity_id_, controller_id, sequence_id));
    (void)callbacks_.send_response(dest_mac, buffer);
}

void AemCommandHandler::handle_aem_response(AemDu const& header)
{
    if (header.command_code() != AEM_COMMAND_CONTROLLER_AVAILABLE) {
        return;
    }
    // The acquire handshake asked the holder whether it is still there: it is.
    if (pending_acquire_.active && acquired_ && header.target_entity_id == acquiring_controller_) {
        controller_available_response_received();
    }
    // The monitor's question, answered with any status (Milan v1.3 Section 5.4.5.4).
    for (auto& reg : unsolicited_registrations_) {
        if (reg.available_tries > 0 && reg.controller_entity_id == header.target_entity_id &&
            reg.available_sequence_id == header.sequence_id.get()) {
            reg.available_tries = 0;
            reg.monitor_deadline = event_time_ + monitor_period();
        }
    }
}

auto AemCommandHandler::handle_deregister_unsolicited(AemDu const& header) -> AemCommandResponse
{
    auto const idx = unsolicited_registrations_.find_if(
        [&header](UnsolicitedRegistration const& r) { return r.controller_entity_id == header.controller_entity_id; });
    if (idx < unsolicited_registrations_.capacity()) {
        unsolicited_registrations_.remove(idx);
    }
    // Deregister is idempotent — always return success, even for unknown controllers.
    return {.status = AEM_STATUS_SUCCESS, .size = 0};
}

auto AemCommandHandler::remove_unsolicited_registrations_for(Eui64 controller_entity_id) noexcept -> size_t
{
    size_t removed = 0;
    for (;;) {
        auto const idx = unsolicited_registrations_.find_if(
            [&](UnsolicitedRegistration const& r) { return r.controller_entity_id == controller_entity_id; });
        if (idx >= unsolicited_registrations_.capacity()) {
            break;
        }
        unsolicited_registrations_.remove(idx);
        ++removed;
    }
    return removed;
}

auto AemCommandHandler::remove_unsolicited_registrations_for_mac(Eui48 const& controller_mac) noexcept -> size_t
{
    size_t removed = 0;
    for (;;) {
        auto const idx = unsolicited_registrations_.find_if(
            [&](UnsolicitedRegistration const& r) { return r.controller_mac == controller_mac; });
        if (idx >= unsolicited_registrations_.capacity()) {
            break;
        }
        unsolicited_registrations_.remove(idx);
        ++removed;
    }
    return removed;
}

void AemCommandHandler::forget_controller_at(Eui48 const& controller_mac)
{
    (void)remove_unsolicited_registrations_for_mac(controller_mac);
    if (pending_acquire_.active && pending_acquire_.requester_mac == controller_mac) {
        pending_acquire_ = {};
    }
    if (!acquired_ || acquiring_controller_mac_ != controller_mac) {
        return;
    }
    if (pending_acquire_.active) {
        // Someone is waiting for the owner to answer CONTROLLER_AVAILABLE: it
        // never will, so the waiting controller gets the entity now.
        controller_available_timed_out();
        return;
    }
    drop_acquisition();
}

void AemCommandHandler::drop_acquisition() noexcept
{
    acquired_ = false;
    acquired_persistent_ = false;
    acquiring_controller_ = {};
    acquiring_controller_mac_ = {};
}

auto AemCommandHandler::send_not_implemented(Eui48 const& dest_mac, std::span<uint8_t const> const command) const -> bool
{
    if (!callbacks_.send_response) {
        return false;
    }
    // The command echoed back (as much as was received) as its response type, NOT_IMPLEMENTED.
    // A command whose control_data_length is short of the AECP common fields is dropped.
    std::array<uint8_t, AemDu::LENGTH + MAX_AEM_RESPONSE_SIZE> response_buffer{};
    auto const out = aecp_echo_response(command, AECP_STATUS_NOT_IMPLEMENTED, response_buffer);
    if (out.empty()) {
        return false;
    }
    return callbacks_.send_response(dest_mac, out);
}

auto AemCommandHandler::unsolicited_registration_count() const noexcept -> size_t
{
    return unsolicited_registrations_.size();
}

void AemCommandHandler::send_unsolicited_to(
    Eui48 const& dest_mac,
    Eui64 const& controller_id,
    uint16_t const sequence_id,
    uint16_t const command_type,
    std::span<uint8_t const> body)
{
    std::array<uint8_t, AemDu::LENGTH + MAX_AEM_RESPONSE_SIZE> buffer{};
    auto buffer_span = std::span{buffer};
    span_store(buffer_span, aem_unsolicited_header(our_entity_id_, controller_id, sequence_id, command_type, body.size()));
    if (!body.empty()) {
        span_copy(buffer_span.subspan(AemDu::LENGTH), body);
    }
    (void)callbacks_.send_response(dest_mac, {buffer.data(), AemDu::LENGTH + body.size()});
}

auto AemCommandHandler::set_local_identify(bool const on) -> uint8_t
{
    if (!identify_control_index_valid_) {
        return AEM_STATUS_NOT_IMPLEMENTED;
    }
    aem::AemControlPayloadHeader const header{.descriptor_type = DESCRIPTOR_CONTROL, .descriptor_index = identify_control_index_};
    std::array<uint8_t, aem::AemControlPayloadHeader::LENGTH + 1> body{};
    span_store(body, header);
    body[aem::AemControlPayloadHeader::LENGTH] = on ? uint8_t{0xFF} : uint8_t{0x00};
    return apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body);
}

auto AemCommandHandler::targets_identify_control(uint16_t const command_type, std::span<uint8_t const> body) const noexcept -> bool
{
    if (!identify_control_index_valid_ || command_type != AEM_COMMAND_SET_CONTROL ||
        body.size() < aem::AemControlPayloadHeader::LENGTH) {
        return false;
    }
    aem::AemControlPayloadHeader header{};
    span_load(header, body);
    return header.descriptor_type.get() == DESCRIPTOR_CONTROL && header.descriptor_index.get() == identify_control_index_;
}

void AemCommandHandler::emit_unsolicited(uint16_t const command_type, std::span<uint8_t const> body)
{
    if (!callbacks_.send_response) {
        return;
    }
    if (dispatching_ && queue_notification(command_type, body)) {
        return;  // sent after the command's response
    }

    for (auto& reg : unsolicited_registrations_) {
        // IEEE 1722.1-2021 7.5.2: the unsolicited response goes to the acquired
        // and every registered controller "without sending the unsolicited
        // response back to the ATDECC Controller which executed the command",
        // under either profile (Milan v1.3 5.4.5.3 asks the same). Each
        // registration's sequence id advances whether or not the send succeeds (5.4.5.1).
        if (reg.controller_entity_id == requester_) {
            continue;
        }
        send_unsolicited_to(reg.controller_mac, reg.controller_entity_id, reg.sequence_id++, command_type, body);
    }

    // IEEE 1722.1 7.5.2: the acquiring controller is implicitly registered,
    // and like any registered controller hears nothing of its own command.
    if (acquired_ && acquiring_controller_ != requester_) {
        auto const registered = unsolicited_registrations_.find_if(
            [&](UnsolicitedRegistration const& r) { return r.controller_entity_id == acquiring_controller_; });
        if (registered >= unsolicited_registrations_.capacity()) {
            send_unsolicited_to(acquiring_controller_mac_, acquiring_controller_, unsolicited_sequence_id_++, command_type, body);
        }
    }

    // An IDENTIFY-control change is additionally announced to the IDENTIFY
    // multicast (IEEE 1722.1) so any controller — even one not registered for
    // unsolicited notifications — observes the identify. No specific controller,
    // so controller_entity_id is left zero.
    if (targets_identify_control(command_type, body)) {
        send_unsolicited_to(atdecc::ATDECC_IDENTIFY_MULTICAST_MAC, Eui64{}, unsolicited_sequence_id_++, command_type, body);
        if (callbacks_.identify_changed) {
            // SET_CONTROL body: descriptor_type(2) + descriptor_index(2) + values.
            // The IDENTIFY control is LINEAR_UINT8: byte 4 is the new value.
            callbacks_.identify_changed(body.size() > 4 && body[4] != 0);
        }
    }
}

auto AemCommandHandler::queue_notification(uint16_t const command_type, std::span<uint8_t const> body) -> bool
{
    if (body.size() > MAX_AEM_RESPONSE_SIZE) {
        release_side_effects();  // the held ones go first, so the order on the wire is the order raised
        return false;
    }
    if (make_room(command_type)) {
        hold_notification(command_type, body);
        return true;
    }
    if (is_commands_own(command_type)) {
        spend_notification(command_type, body);  // never ahead of the response
        return true;
    }
    return false;
}

void AemCommandHandler::spend_notification(uint16_t const command_type, std::span<uint8_t const> const body)
{
    // Milan v1.3 Section 5.4.5.1: sent as a send that fails, each controller's id spent, so each sees the gap.
    auto sender = std::exchange(callbacks_.send_response, [](Eui48 const&, std::span<uint8_t const>) { return false; });
    dispatching_ = false;
    emit_unsolicited(command_type, body);
    dispatching_ = true;
    callbacks_.send_response = std::move(sender);
}

void AemCommandHandler::hold_notification(uint16_t const command_type, std::span<uint8_t const> const body)
{
    auto& queued = queued_notifications_.emplace_back();
    queued.command_type = command_type;
    queued.requester = requester_;
    queued.body_size = body.size();
    span_copy(std::span{queued.body}.first(body.size()), body);
}

auto AemCommandHandler::is_commands_own(uint16_t const command_type) const noexcept -> bool
{
    return command_type == current_header_.command_code();
}

auto AemCommandHandler::make_room(uint16_t const command_type) -> bool
{
    // A side effect may not use the last slot, which is kept for the command's own notification.
    auto const fits = [&] {
        return queued_notifications_.size() + (is_commands_own(command_type) ? 0U : 1U) < MAX_QUEUED_NOTIFICATIONS;
    };
    if (fits()) {
        return true;
    }
    release_side_effects();
    return fits();
}

void AemCommandHandler::release_side_effects()
{
    dispatching_ = false;  // send them rather than hold them again
    for (auto const& n : queued_notifications_) {
        if (!is_commands_own(n.command_type)) {
            emit_held(n);
        }
    }
    dispatching_ = true;
    auto const sent = std::ranges::remove_if(
        queued_notifications_, [this](QueuedNotification const& n) { return !is_commands_own(n.command_type); });
    queued_notifications_.erase(sent.begin(), sent.end());
}

void AemCommandHandler::flush_notifications()
{
    for (auto const& n : queued_notifications_) {
        emit_held(n);
    }
    queued_notifications_.clear();
}

void AemCommandHandler::emit_held(QueuedNotification const& n)
{
    Eui64 const requester = std::exchange(requester_, n.requester);  // left out as when it was raised
    emit_unsolicited(n.command_type, std::span<uint8_t const>{n.body}.first(n.body_size));
    requester_ = requester;
}

auto AemCommandHandler::apply_local_descriptor_value(uint16_t const command_type, std::span<uint8_t const> command_body) -> uint8_t
{
    if (auto const refusal = local_change_refusal()) {
        return *refusal;
    }
    if (auto const refusal = mappings_refusal(command_type, command_body)) {
        return *refusal;
    }
    requester_ = {};  // nobody asked over ATDECC: every registered controller is told
    return apply_and_notify(command_type, command_body);
}

auto AemCommandHandler::apply_local_name(std::span<uint8_t const> const command_body) -> uint8_t
{
    if (auto const refusal = local_change_refusal()) {
        return *refusal;
    }
    requester_ = {};  // nobody asked over ATDECC: every registered controller is told
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> echo{};
    auto const r = handle_set_name(command_body, echo);
    if (r.status == AEM_STATUS_SUCCESS && !setting_unchanged_) {
        report_setting_changed(AEM_COMMAND_SET_NAME);  // a rename to the name in force has nothing to save
    }
    return r.status;
}

auto AemCommandHandler::follow_local_descriptor_value(uint16_t const command_type, std::span<uint8_t const> command_body) -> uint8_t
{
    requester_ = {};  // nobody asked over ATDECC: every registered controller is told
    return apply_and_notify(command_type, command_body);
}

auto AemCommandHandler::apply_and_notify(uint16_t const command_type, std::span<uint8_t const> command_body) -> uint8_t
{
    // Apply the change exactly as a controller's SET would (same symbol-keyed
    // hook), then fan out unsolicited notifications. This is the entry point for
    // an entity changing its own controls (e.g. a software IDENTIFY button).
    AemEntityModel aem_model{*handler_};
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> echo{};
    auto const r = aem_model.apply_set_descriptor_value(command_type, command_body, echo);
    notify_on_success(command_type, {.status = r.status, .size = r.size}, echo);
    if (r.status == AEM_STATUS_SUCCESS) {
        report_setting_changed(command_type);
    }
    return r.status;
}

void AemCommandHandler::controller_available_response_received()
{
    if (!pending_acquire_.active) {
        return;
    }
    // Current owner is alive: deny the pending requester with ENTITY_ACQUIRED.
    // The final response is sent here via the send_response callback rather
    // than being returned to the caller, because the IN_PROGRESS path in
    // process_packet() has already returned by the time we get here.
    answer_pending_acquire(AEM_STATUS_ENTITY_ACQUIRED);
    pending_acquire_ = {};
}

void AemCommandHandler::controller_available_timed_out()
{
    if (pending_acquire_.active) {
        grant_pending_acquire();
    }
}

void AemCommandHandler::grant_pending_acquire()
{
    // The owner is gone or has let go: transfer acquisition to the requester
    // and send a SUCCESS response. See controller_available_response_received
    // above for why the response is emitted here rather than returned.
    acquired_ = true;
    acquiring_controller_ = pending_acquire_.requester_header.controller_entity_id;
    acquiring_controller_mac_ = pending_acquire_.requester_mac;
    acquired_persistent_ = (pending_acquire_.requester_flags & 0x01u) != 0u;
    answer_pending_acquire(AEM_STATUS_SUCCESS);
    pending_acquire_ = {};
}

void AemCommandHandler::answer_pending_acquire(uint8_t const status)
{
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> out_buffer{};
    auto const original_command_data = std::span<uint8_t const>{pending_acquire_.command_data_copy};
    auto const response = build_acquire_response(
        {.status = status,
         .flags = pending_acquire_.requester_flags,
         .entity_id = acquiring_controller_,
         .command_data = original_command_data},
        out_buffer);
    (void)send_response(
        pending_acquire_.requester_mac,
        pending_acquire_.requester_header,
        response.status,
        std::span<uint8_t const>{out_buffer.data(), response.size});
    notify_on_success(AEM_COMMAND_ACQUIRE_ENTITY, response, out_buffer);
}

}  // namespace statusbar::nanoavb
