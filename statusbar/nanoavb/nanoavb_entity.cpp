// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_entity.hpp"

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avtp/avtp_stream_format.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

namespace {

/// The name a SET_NAME command body (an AemNamePayload) names.
[[nodiscard]] auto name_ref_of(std::span<uint8_t const> const command_data) noexcept -> NameRef
{
    atdecc::aem::AemNamePayload payload{};
    span_load(payload, command_data);
    return NameRef{
        .descriptor =
            DescriptorRef{
                .configuration_index = payload.configuration_index.get(),
                .descriptor_type = payload.descriptor_type.get(),
                .descriptor_index = payload.descriptor_index.get()},
        .name_index = payload.name_index.get()};
}

/// @p Payload from the head of @p data, which holds at least its LENGTH octets.
template <typename Payload>
[[nodiscard]] auto load_payload(std::span<uint8_t const> const data) noexcept -> Payload
{
    Payload payload{};
    span_load(payload, data.first(Payload::LENGTH));
    return payload;
}

/// A GET_AS_PATH response (IEEE 1722.1-2021 Clause 7.4.41.2) in @p out_buffer: descriptor_index,
/// count, then count clock identities; NOT_SUPPORTED when it does not fit.
[[nodiscard]] auto as_path_response(
    uint16_t const descriptor_index, AsPath const& path, std::span<uint8_t> const out_buffer) noexcept -> AemCommandResponse
{
    size_t const size = 4 + (path.size() * 8);
    if (out_buffer.size() < size) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    span_store(out_buffer.subspan(0, 2), doublet_t{descriptor_index});
    span_store(out_buffer.subspan(2, 2), doublet_t{static_cast<uint16_t>(path.size())});
    for (size_t i = 0; i < path.size(); ++i) {
        span_copy(out_buffer.subspan(4 + (i * 8), 8), path[i].span());
    }
    return {.status = AEM_STATUS_SUCCESS, .size = size};
}

/// A max_transit_time of stream output @p index (Milan v1.3 Section 5.4.2.30), the layout of
/// GET_MAX_TRANSIT_TIME's response, at the head of @p out.
void store_max_transit_time(std::span<uint8_t> const out, uint16_t const index, uint32_t const offset_ns) noexcept
{
    span_store(
        out.first(AemGetMaxTransitTimeResponsePayload::LENGTH),
        AemGetMaxTransitTimeResponsePayload{
            .descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .descriptor_index = index, .max_transit_time = uint64_t{offset_ns}});
}

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

}  // namespace

auto AemCommandHandler::process_packet(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id) -> bool
{
    // Any other AECP command type addressed to us is answered NOT_IMPLEMENTED rather than
    // dropped (IEEE 1722.1-2021 Clause 9.2.1.1.6); MVU normally goes to dispatch_aecp.
    if (payload.size() >= AecpDuCommon::LENGTH) {
        AecpDuCommon common{};
        span_load(common, payload.first(AecpDuCommon::LENGTH));
        // Only the defined non-AEM command types (IEEE 1722.1-2021 Table 9.1);
        // reserved types (10, 12) are not answered.
        auto const type = common.message_type();
        bool const defined = type == AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND || type == AECP_MESSAGE_TYPE_AVC_COMMAND ||
            type == AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND || type == AECP_MESSAGE_TYPE_HDCP_APM_COMMAND ||
            type == AECP_MESSAGE_TYPE_EXTENDED_COMMAND;
        if (common.subtype.get() == AvtpSubtype::aecp && defined && common.target_entity_id == our_entity_id) {
            return send_not_implemented(src_mac, payload);
        }
    }

    // Validate minimum size
    if (payload.size() < AemDu::LENGTH) {
        return false;
    }

    // Parse header
    AemDu header{};
    span_load(header, payload);

    // Validate header
    if (!header.is_valid()) {
        return false;
    }
    // A response to a command we sent names us as its controller.
    if (header.is_response()) {
        if (header.controller_entity_id == our_entity_id && our_entity_id != Eui64{}) {
            handle_aem_response(header);
        }
        return false;
    }
    if (!header.is_command()) {
        return false;
    }

    // Check target_entity_id matches us
    if (header.target_entity_id != our_entity_id) {
        return false;  // Not for us
    }

    // Keep our id current for unsolicited notifications (their target_entity_id).
    our_entity_id_ = our_entity_id;
    note_controller_command(header.controller_entity_id, src_mac);

    // Stash the source MAC so per-command handlers can retain it if they
    // need to emit an out-of-band late response (e.g. ACQUIRE_ENTITY's
    // CONTROLLER_AVAILABLE handshake). Cleared after dispatch.
    current_src_mac_ = src_mac;

    // Extract command data (after AemDu header)
    auto const command_data = payload.subspan(AemDu::LENGTH);

    // One output buffer per dispatch — handlers write into this and return
    // status + bytes-written, avoiding the per-return 1 KB struct copy that
    // the previous AemCommandResult shape incurred.
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> out_buffer{};
    auto const response = handle_command(header, command_data, out_buffer);

    current_src_mac_ = {};

    // Build and send response
    bool const sent = send_response(src_mac, header, response.status, std::span<uint8_t const>{out_buffer.data(), response.size});
    // Then let the entity save a changed setting (after the response: a
    // write to non-volatile storage must not delay it).
    if (response.status == AEM_STATUS_SUCCESS && is_persistent_setting_command(header.command_code()) && !setting_unchanged_ &&
        callbacks_.on_settings_changed) {
        callbacks_.on_settings_changed(header.command_code());
    }
    return sent;
}

auto AemCommandHandler::handle_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    auto const cmd = header.command_code();
    current_header_ = header;
    setting_unchanged_ = false;

    switch (cmd) {
        case AEM_COMMAND_READ_DESCRIPTOR:
            return handle_read_descriptor(header, command_data, out_buffer);

        case AEM_COMMAND_GET_NAME:
            return handle_get_name(command_data, out_buffer);

        case AEM_COMMAND_SET_NAME:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_name(command_data, out_buffer);

        case AEM_COMMAND_ENTITY_AVAILABLE:
            return handle_entity_available(header);

        case AEM_COMMAND_ACQUIRE_ENTITY: {
            // Notify a change of holder only: a RELEASE when not acquired, or
            // an acquire the holder repeats, tells the other controllers nothing.
            auto const before = std::pair{acquired_, acquiring_controller_};
            auto const r = handle_acquire_entity(header, command_data, out_buffer);
            if (std::pair{acquired_, acquiring_controller_} != before) {
                notify_on_success(AEM_COMMAND_ACQUIRE_ENTITY, r, out_buffer);
            }
            return r;
        }

        case AEM_COMMAND_LOCK_ENTITY: {
            // Likewise an UNLOCK when unlocked, or a lock its holder refreshes.
            auto const before = std::pair{locked_, locking_controller_};
            auto const r = handle_lock_entity(header, command_data, out_buffer);
            if (std::pair{locked_, locking_controller_} != before) {
                notify_on_success(AEM_COMMAND_LOCK_ENTITY, r, out_buffer);
            }
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
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_configuration(command_data, out_buffer);

        case AEM_COMMAND_CONTROLLER_AVAILABLE:
            return handle_controller_available(header);

        case AEM_COMMAND_SET_CONTROL:
            // The IDENTIFY control is exempt from acquire/lock (Milan: identify
            // shall work even when another controller holds the entity) — any
            // controller may ask "which box are you". Every other control honors
            // the exclusive claim.
            if (!targets_identify_control(AEM_COMMAND_SET_CONTROL, command_data)) {
                if (auto const blocked = check_exclusive_access(header)) {
                    return reject_command(*blocked, command_data, out_buffer);
                }
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_CONTROL, command_data, out_buffer);

        case AEM_COMMAND_GET_CONTROL:
            return handle_get_descriptor_value(AEM_COMMAND_GET_CONTROL, command_data, out_buffer);

        case AEM_COMMAND_SET_SIGNAL_SELECTOR:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_SIGNAL_SELECTOR, command_data, out_buffer);

        case AEM_COMMAND_GET_SIGNAL_SELECTOR:
            return handle_get_descriptor_value(AEM_COMMAND_GET_SIGNAL_SELECTOR, command_data, out_buffer);

        case AEM_COMMAND_SET_MATRIX:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_MATRIX, command_data, out_buffer);

        case AEM_COMMAND_GET_MATRIX:
            return handle_get_descriptor_value(AEM_COMMAND_GET_MATRIX, command_data, out_buffer);

        case AEM_COMMAND_SET_MIXER:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_MIXER, command_data, out_buffer);

        case AEM_COMMAND_GET_MIXER:
            return handle_get_descriptor_value(AEM_COMMAND_GET_MIXER, command_data, out_buffer);

        case AEM_COMMAND_SET_CLOCK_SOURCE:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_CLOCK_SOURCE, command_data, out_buffer);

        case AEM_COMMAND_GET_CLOCK_SOURCE:
            return handle_get_descriptor_value(AEM_COMMAND_GET_CLOCK_SOURCE, command_data, out_buffer);

        case AEM_COMMAND_SET_STREAM_FORMAT:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_STREAM_FORMAT, command_data, out_buffer);

        case AEM_COMMAND_SET_SAMPLING_RATE:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_SAMPLING_RATE, command_data, out_buffer);

        case AEM_COMMAND_START_STREAMING:
        case AEM_COMMAND_STOP_STREAMING:
            // Milan v1.2 Sections 5.4.2.19-20 and 5.3.7.3: a stream output
            // streams while it has a Listener Ready; it cannot be stopped.
            if (profile_.milan && command_data.size() >= 2 &&
                ((uint16_t{command_data[0]} << 8U) | command_data[1]) == DESCRIPTOR_STREAM_OUTPUT) {
                return reject_command(AEM_STATUS_NOT_SUPPORTED, command_data, out_buffer);
            }
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(cmd, command_data, out_buffer);

        case AEM_COMMAND_SET_STREAM_INFO:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_stream_info(command_data, out_buffer);

        case AEM_COMMAND_SET_MAX_TRANSIT_TIME:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_max_transit_time(command_data, out_buffer);

        case AEM_COMMAND_GET_MAX_TRANSIT_TIME:
            return handle_get_max_transit_time(command_data, out_buffer);

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

        case AEM_COMMAND_GET_AUDIO_MAP:
            return handle_get_audio_map(header, command_data, out_buffer);

        case AEM_COMMAND_ADD_AUDIO_MAPPINGS:
        case AEM_COMMAND_REMOVE_AUDIO_MAPPINGS:
            // Milan v1.2 Sections 5.4.2.27-28: refused to all but the
            // controller holding the lock.
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_audio_mappings(cmd == AEM_COMMAND_ADD_AUDIO_MAPPINGS, command_data, out_buffer);

        case AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
            return handle_register_unsolicited(header);

        case AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION:
            // Not fanned out: a controller takes an unsolicited DEREGISTER as
            // its own deregistration (Milan v1.2 Table 5.22 sends one only to
            // a controller the entity drops; lib_avb 14c6ee2).
            return handle_deregister_unsolicited(header);

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
    // GET_NAME command body (Clause 7.4.18.1): descriptor_type(2) +
    // descriptor_index(2) + name_index(2) + configuration_index(2).
    constexpr size_t GET_NAME_HEADER = 8;
    if (command_data.size() < GET_NAME_HEADER) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    doublet_t dtype{};
    doublet_t dindex{};
    doublet_t nindex{};
    doublet_t cindex{};
    span_load(dtype, command_data.subspan(0, 2));
    span_load(dindex, command_data.subspan(2, 2));
    span_load(nindex, command_data.subspan(4, 2));
    span_load(cindex, command_data.subspan(6, 2));

    NameRef const ref{
        .descriptor =
            DescriptorRef{.configuration_index = cindex.get(), .descriptor_type = dtype.get(), .descriptor_index = dindex.get()},
        .name_index = nindex.get()};

    // Per-call model; symbols resolve through the handler's storage.
    AemEntityModel const aem_model{*handler_};
    auto const n = aem_model.get_name_for_wire(ref, out_buffer);
    if (n == 0) {
        // The handler serves no name for this (descriptor, name_index) — the
        // conservative default, consistent with the SET path.
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
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
    // mutation; the owning controller (or a free entity) proceeds.
    if (acquired_ && !(acquiring_controller_ == controller_entity_id)) {
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
    size_t size = 0;
    if (out_buffer.size() >= command_data.size()) {
        span_copy(out_buffer, command_data);
        size = command_data.size();
    }
    return {.status = status, .size = size};
}

auto AemCommandHandler::handle_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    return set_name(command_data, out_buffer, current_header_.controller_entity_id);
}

auto AemCommandHandler::apply_local_name(std::span<uint8_t const> const command_body) -> uint8_t
{
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> echo{};
    auto const r = set_name(command_body, echo, std::nullopt);
    if (r.status == AEM_STATUS_SUCCESS && callbacks_.on_settings_changed) {
        callbacks_.on_settings_changed(AEM_COMMAND_SET_NAME);
    }
    return r.status;
}

auto AemCommandHandler::set_name(
    std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer, std::optional<Eui64> const& requester)
    -> AemCommandResponse
{
    // Per-call model; the handler applies the name by (descriptor, name_index,
    // symbol). The response echoes the command (header + 64-byte name).
    AemEntityModel aem_model{*handler_};
    auto const status = aem_model.apply_set_name(command_data);

    size_t size = 0;
    if (status == AEM_STATUS_SUCCESS && out_buffer.size() >= command_data.size()) {
        std::copy(command_data.begin(), command_data.end(), out_buffer.begin());
        size = command_data.size();
        // The name in force, which an empty one (cleared to the configured or
        // authored name) is not: what GET_NAME now answers.
        (void)aem_model.get_name_for_wire(name_ref_of(command_data), out_buffer);
        // A controller renamed a descriptor: notify every registered
        // controller, like handle_set_descriptor_value does for values.
        emit_unsolicited(AEM_COMMAND_SET_NAME, std::span<uint8_t const>{out_buffer.data(), size}, requester);
    }
    return {.status = status, .size = size};
}

auto AemCommandHandler::handle_set_descriptor_value(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // Per-call model (handler_ resolves the descriptor symbol via its storage). The
    // handler applies the value by (command_type, symbol); the response echoes the command.
    AemEntityModel aem_model{*handler_};
    aem_model.set_configuration(current_configuration_);
    auto const r = aem_model.apply_set_descriptor_value(command_type, command_data, out_buffer);

    // A controller changed a control: notify every registered controller (IEEE
    // 1722.1 9.6 unsolicited notifications); the originator too in the
    // classic profile, not in Milan's (emit_unsolicited).
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(command_type, std::span<uint8_t const>{out_buffer.data(), r.size}, current_header_.controller_entity_id);
    }
    if (r.status == AEM_STATUS_IN_PROGRESS) {
        // The hook needs time (e.g. an SRP re-reservation after SET_STREAM_FORMAT):
        // answer IN_PROGRESS now, keep repeating it from tick(), and finish when
        // the application calls complete_pending_command().
        return defer_command(command_type, std::span<uint8_t const>{out_buffer.data(), r.size});
    }
    return {.status = r.status, .size = r.size};
}

auto AemCommandHandler::defer_command(uint16_t const command_type, std::span<uint8_t const> echoed_body) -> AemCommandResponse
{
    (void)command_type;
    if (echoed_body.size() > MAX_PENDING_BODY) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = echoed_body.size()};
    }
    PendingCommand pending{};
    pending.header = current_header_;
    pending.mac = current_src_mac_;
    std::copy(echoed_body.begin(), echoed_body.end(), pending.body.begin());
    pending.body_size = echoed_body.size();
    pending.next_in_progress = event_time_ + std::chrono::milliseconds(AEM_IN_PROGRESS_TIMEOUT_MS);
    if (!pending_commands_.add(pending)) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = echoed_body.size()};
    }
    return {.status = AEM_STATUS_IN_PROGRESS, .size = echoed_body.size()};
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

    auto const body = response_body.empty() ? std::span<uint8_t const>{done.body}.first(done.body_size) : response_body;
    (void)send_response(done.mac, done.header, status, body);
    if (status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(command_type, body, done.header.controller_entity_id);
        if (is_persistent_setting_command(command_type) && callbacks_.on_settings_changed) {
            callbacks_.on_settings_changed(command_type);
        }
    }
    return true;
}

void AemCommandHandler::tick(sm::TimePoint const now)
{
    now_ = now;
    have_now_ = true;
    // A GET_COUNTERS change held back within a second of the last one goes
    // once the second has passed, with the counters as they are then.
    for (auto& n : counters_notifications_) {
        if (n.pending && now >= n.next_allowed) {
            n.pending = false;
            n.next_allowed = now + COUNTERS_NOTIFICATION_INTERVAL;
            (void)send_counters_notification(n.descriptor_type, n.descriptor_index);
        }
    }
    if (locked_ && now >= lock_expiry_time_) {
        release_lock();
        emit_lock_released();
    }
    if (pending_acquire_.active && now >= pending_acquire_.timeout) {
        controller_available_timed_out();
    }
    // IEEE 1722.1 9.2.2.3.1.2: a deferred command repeats IN_PROGRESS every
    // AEM_IN_PROGRESS_TIMEOUT_MS so the controller keeps waiting.
    for (auto& p : pending_commands_) {
        if (now >= p.next_in_progress) {
            (void)send_response(p.mac, p.header, AEM_STATUS_IN_PROGRESS, std::span<uint8_t const>{p.body}.first(p.body_size));
            p.next_in_progress = now + std::chrono::milliseconds(AEM_IN_PROGRESS_TIMEOUT_MS);
        }
    }
    if (profile_.milan) {
        tick_controller_monitor(now);
    }
}

void AemCommandHandler::notify_on_success(
    uint16_t const command_type, AemCommandResponse const& response, std::span<uint8_t const> out_buffer)
{
    if (response.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(command_type, out_buffer.first(response.size), current_header_.controller_entity_id);
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

auto AemCommandHandler::send_counters_notification(uint16_t const descriptor_type, uint16_t const descriptor_index) -> uint8_t
{
    AemGetCountersCommandPayload const cmd{.descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
    std::array<uint8_t, AemGetCountersCommandPayload::LENGTH> request{};
    span_store(request, cmd);
    std::array<uint8_t, sizeof(AemCountersPayload)> out{};
    auto const r = handle_get_counters(AemDu{}, request, out);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_COUNTERS, std::span<uint8_t const>{out}.first(r.size));
    }
    return r.status;
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
    size_t const size = AemAvbInfoPayload::LENGTH + (info.msrp_mappings.size() * MAPPING_LENGTH);
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
    std::array<uint8_t, AemAvbInfoPayload::LENGTH + (AvbInfo::MAX_MSRP_MAPPINGS * 4)> out{};
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
    // Command (7.4.41.1): descriptor_index (AVB_INTERFACE) + reserved, 4 octets.
    if (command_data.size() < 4) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    auto const descriptor_index = static_cast<uint16_t>((uint16_t{command_data[0]} << 8U) | command_data[1]);
    AsPath path{};
    if (!callbacks_.get_as_path(descriptor_index, path)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    return as_path_response(descriptor_index, path, out_buffer);
}

auto AemCommandHandler::notify_as_path_changed(uint16_t const descriptor_index) -> uint8_t
{
    std::array<uint8_t, 4> request{static_cast<uint8_t>(descriptor_index >> 8U), static_cast<uint8_t>(descriptor_index), 0, 0};
    std::array<uint8_t, 4 + (MAX_AS_PATH_LENGTH * 8)> out{};
    auto const r = handle_get_as_path(request, out);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_AS_PATH, std::span<uint8_t const>{out}.first(r.size));
    }
    return r.status;
}

auto AemCommandHandler::notify_stream_info_changed(uint16_t const descriptor_type, uint16_t const descriptor_index) -> uint8_t
{
    AemGetCountersCommandPayload const cmd{.descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
    std::array<uint8_t, AemGetCountersCommandPayload::LENGTH> request{};
    span_store(request, cmd);
    std::array<uint8_t, AEM_STREAM_INFO_MILAN_LENGTH> out{};
    auto const r = handle_get_stream_info(AemDu{}, request, out);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(AEM_COMMAND_GET_STREAM_INFO, std::span<uint8_t const>{out}.first(r.size));
    }
    return r.status;
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

/// The most AEM command-specific data one AECPDU can carry.
constexpr size_t MAX_AEM_BODY = AECP_MAX_CONTROL_DATA_LENGTH - AemDu::AEM_DATA_LENGTH;

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
        // The handler served no value: the entity does not implement this GET (the
        // conservative default, consistent with the SET hook's NOT_IMPLEMENTED).
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
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

namespace {

/// STREAM_PORT port_flags (IEEE 1722.1-2021 Table 7-15): the port converts
/// sample rates (asynchronously or synchronously).
constexpr uint16_t PORT_FLAG_ASYNC_SAMPLE_RATE_CONV = 0x0002;
constexpr uint16_t PORT_FLAG_SYNC_SAMPLE_RATE_CONV = 0x0004;

/// The audio channels and nominal rate of a stream format (AAF, or IEC
/// 61883-6 AM824); none for anything else (a CRF stream has no channels).
struct FormatChannels
{
    uint16_t channels{0};
    uint32_t rate{0};
};

[[nodiscard]] auto format_channels(uint64_t const format) noexcept -> FormatChannels
{
    auto const b = avtp::stream_format_bytes(format);
    if (b[0] == avtp::AvtpSubtype::aaf) {
        auto const aaf = avtp::AafStreamFormat::from_u64(format);
        return {.channels = aaf.channels_per_frame(), .rate = avtp::aaf_nsr_to_hz(aaf.nsr())};
    }
    if (b[0] == avtp::AvtpSubtype::iec_61883_iidc && (b[1] & 0xFEU) == 0xA0U) {
        return {.channels = b[3], .rate = avtp::am824_sfc_to_hz(b[2])};
    }
    return {};
}

/// IEEE 1722.1-2021 Clause 9.2.1.1.7: one AECPDU holds 63 mappings.
constexpr size_t MAPPINGS_PER_PDU = (MAX_AEM_BODY - AemAudioMappingsCommandHeader::LENGTH) / AemAudioMapping::LENGTH;

/// An ADD/REMOVE_AUDIO_MAPPINGS command's header; nullopt when the command is short of it or of
/// the mappings it counts, or counts more than the store holds.
[[nodiscard]] auto mappings_command(std::span<uint8_t const> const command_data) noexcept
    -> std::optional<AemAudioMappingsCommandHeader>
{
    if (command_data.size() < AemAudioMappingsCommandHeader::LENGTH) {
        return std::nullopt;
    }
    auto const cmd = load_payload<AemAudioMappingsCommandHeader>(command_data);
    size_t const count = cmd.number_of_mappings.get();
    if (count * AemAudioMapping::LENGTH > command_data.size() - AemAudioMappingsCommandHeader::LENGTH ||
        count > DynamicAudioMaps::MAX_MAPPINGS) {
        return std::nullopt;
    }
    return cmd;
}

/// The mappings of an ADD/REMOVE_AUDIO_MAPPINGS command body (after its header).
[[nodiscard]] auto parse_mappings(std::span<uint8_t const> const bytes, size_t const count) -> DynamicAudioMaps::Mappings
{
    DynamicAudioMaps::Mappings out{};
    for (size_t i = 0; i < count && out.size() < DynamicAudioMaps::Mappings::capacity(); ++i) {
        AemAudioMapping wire{};
        span_load(wire, bytes.subspan(i * AemAudioMapping::LENGTH, AemAudioMapping::LENGTH));
        out.push_back(
            {.stream_index = wire.stream_index.get(),
             .stream_channel = wire.stream_channel.get(),
             .cluster_offset = wire.cluster_offset.get(),
             .cluster_channel = wire.cluster_channel.get()});
    }
    return out;
}

}  // namespace

template <typename T>
auto AemCommandHandler::read_model_descriptor(uint16_t const type, uint16_t const index) const -> std::optional<T>
{
    std::array<uint8_t, AEM_DESCRIPTOR_SIZE> desc{};
    auto const n = model_descriptor_for_wire(type, index, desc);
    if (n == 0) {
        return std::nullopt;
    }
    T out{};
    span_load_padded(out, std::span<uint8_t const>{desc.data(), n});
    return out;
}

auto AemCommandHandler::model_descriptor_for_wire(uint16_t const type, uint16_t const index, std::span<uint8_t> const out) const
    -> size_t
{
    AemEntityModel const aem_model{*handler_};
    DescriptorRef const ref{.configuration_index = current_configuration_, .descriptor_type = type, .descriptor_index = index};
    return aem_model.get_descriptor_for_wire(ref, out);
}

auto AemCommandHandler::port_model(uint16_t const port_type, uint16_t const port_index) const -> std::optional<PortModel>
{
    if (port_type != DESCRIPTOR_STREAM_PORT_INPUT && port_type != DESCRIPTOR_STREAM_PORT_OUTPUT) {
        return std::nullopt;
    }
    auto const port = read_model_descriptor<DescriptorStreamPort>(port_type, port_index);
    if (!port) {
        return std::nullopt;
    }
    PortModel model{
        .port_type = port_type,
        .base_cluster = port->base_cluster.get(),
        .number_of_clusters = port->number_of_clusters.get(),
        .number_of_maps = port->number_of_maps.get(),
        .port_flags = port->port_flags.get()};
    for (uint16_t offset = 0; offset < model.number_of_clusters && offset < decltype(model.cluster_channel_counts)::capacity();
         ++offset) {
        auto const cluster = read_model_descriptor<DescriptorAudioCluster>(
            DESCRIPTOR_AUDIO_CLUSTER, static_cast<uint16_t>(model.base_cluster + offset));
        uint16_t const channels = cluster ? cluster->channel_count.get() : uint16_t{0};
        model.cluster_channel_counts.push_back(channels);
        model.total_channels += channels;
    }
    return model;
}

auto AemCommandHandler::cluster_channels(PortModel const& port, uint16_t const cluster_offset) -> uint16_t
{
    return cluster_offset < port.cluster_channel_counts.size() ? port.cluster_channel_counts[cluster_offset] : uint16_t{0};
}

auto AemCommandHandler::cluster_ordinal(PortModel const& port, DynamicAudioMapping const& mapping) -> std::optional<uint32_t>
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

auto AemCommandHandler::has_dynamic_input_map(uint16_t const port_type, uint16_t const port_index) const -> bool
{
    auto const port = port_type == DESCRIPTOR_STREAM_PORT_INPUT ? port_model(port_type, port_index) : std::nullopt;
    return port.has_value() && port->number_of_maps == 0;
}

auto AemCommandHandler::has_dynamic_output_map(uint16_t const port_index) const -> bool
{
    auto const port = port_model(DESCRIPTOR_STREAM_PORT_OUTPUT, port_index);
    return port.has_value() && port->number_of_maps == 0;
}

auto AemCommandHandler::check_mapping(
    uint16_t const port_index, PortModel const& port, DynamicAudioMapping const& mapping, bool const check_channel) const -> uint8_t
{
    if (!cluster_ordinal(port, mapping)) {
        return AEM_STATUS_BAD_ARGUMENTS;  // no such cluster channel
    }
    bool const input = port.port_type == DESCRIPTOR_STREAM_PORT_INPUT;
    auto const stream =
        read_model_descriptor<DescriptorStream>(input ? DESCRIPTOR_STREAM_INPUT : DESCRIPTOR_STREAM_OUTPUT, mapping.stream_index);
    if (!stream) {
        return AEM_STATUS_BAD_ARGUMENTS;  // no such stream of the port's direction
    }
    if (!check_channel) {
        return AEM_STATUS_SUCCESS;
    }
    return check_stream_channel(port_index, port, stream->current_format.to_uint64(), mapping.stream_channel);
}

auto AemCommandHandler::check_stream_channel(
    uint16_t const port_index, PortModel const& port, uint64_t const stream_format, uint16_t const stream_channel) const -> uint8_t
{
    // Milan v1.2 Section 5.4.2.27: a channel of the stream's current format...
    auto const format = format_channels(stream_format);
    if (stream_channel >= format.channels) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    // ... at the audio unit's rate, unless the port converts rates (an output's mappings are the
    // entity's own, kept at the rate by it, also through a re-time the unit has not recorded yet).
    bool const converts = (port.port_flags & (PORT_FLAG_ASYNC_SAMPLE_RATE_CONV | PORT_FLAG_SYNC_SAMPLE_RATE_CONV)) != 0;
    if (port.port_type == DESCRIPTOR_STREAM_PORT_INPUT && !converts && !input_unit_rate_is(port_index, format.rate)) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::input_unit_rate_is(uint16_t const port_index, uint32_t const rate) const -> bool
{
    for (uint16_t unit = 0;; ++unit) {
        auto const audio_unit = read_model_descriptor<DescriptorAudioUnit>(DESCRIPTOR_AUDIO_UNIT, unit);
        if (!audio_unit) {
            return true;
        }
        uint32_t const base = audio_unit->base_stream_input_port.get();
        if (port_index >= base && port_index < base + audio_unit->number_of_stream_input_ports.get()) {
            // The rate's base frequency (its top 3 bits are the pull).
            return (audio_unit->current_sampling_rate.get() & 0x1FFFFFFFU) == rate;
        }
    }
}

auto AemCommandHandler::merged_mappings(
    uint16_t const port_index,
    PortModel const& port,
    DynamicAudioMaps::Mappings const& current,
    std::span<DynamicAudioMapping const> const mappings,
    DynamicAudioMaps::Mappings& merged) const -> uint8_t
{
    merged = current;
    for (auto const& mapping : mappings) {
        if (auto const status = check_mapping(port_index, port, mapping, true); status != AEM_STATUS_SUCCESS) {
            return status;
        }
        // Milan v1.2 Section 5.3.10.1: a cluster channel maps from at most one stream channel;
        // two in the command, or one already mapped from another, is BAD_ARGUMENTS.
        auto const same_cluster = [&mapping](DynamicAudioMapping const& m) {
            return m.cluster_offset == mapping.cluster_offset && m.cluster_channel == mapping.cluster_channel;
        };
        auto* const existing = std::ranges::find_if(merged, same_cluster);
        if (existing != merged.end()) {
            if (!(*existing == mapping)) {
                return AEM_STATUS_BAD_ARGUMENTS;
            }
            continue;  // already there (or listed twice)
        }
        if (merged.size() == DynamicAudioMaps::Mappings::capacity()) {
            return AEM_STATUS_NO_RESOURCES;
        }
        merged.push_back(mapping);
    }
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::set_audio_mappings(
    uint16_t const port_type, uint16_t const port_index, std::span<DynamicAudioMapping const> const mappings, bool const apply)
    -> uint8_t
{
    if (port_type != DESCRIPTOR_STREAM_PORT_INPUT) {
        return AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    PortModel port{};
    if (auto const status = dynamic_port(port_type, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    DynamicAudioMaps::Mappings merged{};
    if (auto const status = merged_mappings(port_index, port, {}, mappings, merged); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    return keep_input_mappings(port_index, merged, apply);
}

auto AemCommandHandler::dynamic_port(uint16_t const port_type, uint16_t const port_index, PortModel& port) const -> uint8_t
{
    if (port_type != DESCRIPTOR_STREAM_PORT_INPUT && port_type != DESCRIPTOR_STREAM_PORT_OUTPUT) {
        return AEM_STATUS_NOT_SUPPORTED;
    }
    auto model = port_model(port_type, port_index);
    if (!model) {
        return AEM_STATUS_NO_SUCH_DESCRIPTOR;
    }
    if (model->number_of_maps != 0) {
        return AEM_STATUS_NOT_SUPPORTED;  // static maps: its AUDIO_MAP descriptors
    }
    port = *model;
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::keep_input_mappings(uint16_t const port_index, DynamicAudioMaps::Mappings& merged, bool const apply)
    -> uint8_t
{
    if (apply) {
        if (!callbacks_.on_audio_mappings_changed) {
            return AEM_STATUS_NOT_IMPLEMENTED;
        }
        std::ranges::sort(merged, cluster_order);
        if (auto const status = callbacks_.on_audio_mappings_changed(DESCRIPTOR_STREAM_PORT_INPUT, port_index, merged);
            status != AEM_STATUS_SUCCESS) {
            return status;
        }
    }
    if (!dynamic_maps_.replace(DESCRIPTOR_STREAM_PORT_INPUT, port_index, merged)) {
        if (apply) {
            // Put the data plane back on the mappings in force.
            (void)callbacks_.on_audio_mappings_changed(
                DESCRIPTOR_STREAM_PORT_INPUT, port_index, dynamic_maps_.port(DESCRIPTOR_STREAM_PORT_INPUT, port_index));
        }
        return AEM_STATUS_NO_RESOURCES;
    }
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::mappable_port(uint16_t const port_type, uint16_t const port_index, PortModel& port) const -> uint8_t
{
    if (auto const status = dynamic_port(port_type, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    // An output port: its mappings are static on this entity (Milan v1.2 Section 5.3.9.1 allows it).
    if (port_type != DESCRIPTOR_STREAM_PORT_INPUT) {
        return AEM_STATUS_NOT_SUPPORTED;
    }
    return callbacks_.on_audio_mappings_changed ? AEM_STATUS_SUCCESS : AEM_STATUS_NOT_IMPLEMENTED;
}

auto AemCommandHandler::removed_mappings(
    uint16_t const port_index,
    PortModel const& port,
    DynamicAudioMaps::Mappings const& current,
    std::span<DynamicAudioMapping const> const listed,
    DynamicAudioMaps::Mappings& kept) const -> uint8_t
{
    // Milan v1.2 Section 5.4.2.28: duplicates are ignored, and so is a mapping
    // that is not there; one naming nothing in the model is not a mapping at all.
    for (auto const& mapping : listed) {
        if (auto const status = check_mapping(port_index, port, mapping, false); status != AEM_STATUS_SUCCESS) {
            return status;
        }
    }
    kept.clear();
    for (auto const& mapping : current) {
        if (std::ranges::find(listed, mapping) == listed.end()) {
            kept.push_back(mapping);
        }
    }
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::commit_mappings(
    uint16_t const port_index, DynamicAudioMaps::Mappings const& current, DynamicAudioMaps::Mappings const& next) -> uint8_t
{
    if (!dynamic_maps_.fits(DESCRIPTOR_STREAM_PORT_INPUT, port_index, next.size())) {
        return AEM_STATUS_NO_RESOURCES;  // every port's together
    }
    // All or nothing: the data plane takes the whole new set, or nothing changes.
    if (auto const status = callbacks_.on_audio_mappings_changed(DESCRIPTOR_STREAM_PORT_INPUT, port_index, next);
        status != AEM_STATUS_SUCCESS) {
        return status;
    }
    if (!dynamic_maps_.replace(DESCRIPTOR_STREAM_PORT_INPUT, port_index, next)) {
        // Cannot be: it fits. Put the data plane back.
        (void)callbacks_.on_audio_mappings_changed(DESCRIPTOR_STREAM_PORT_INPUT, port_index, current);
        return AEM_STATUS_NO_RESOURCES;
    }
    return AEM_STATUS_SUCCESS;
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
    uint16_t const port_index = cmd.descriptor_index;
    auto const listed = parse_mappings(command_data.subspan(AemAudioMappingsCommandHeader::LENGTH), cmd.number_of_mappings.get());
    auto const current = dynamic_maps_.port(cmd.descriptor_type, port_index);
    DynamicAudioMaps::Mappings next{};
    if (auto const status = next_mappings(add, port_index, port, current, listed, next); status != AEM_STATUS_SUCCESS) {
        return reject_command(status, command_data, out_buffer);
    }
    return commit_mappings_command(add, cmd, current, next, command_data, out_buffer);
}

auto AemCommandHandler::next_mappings(
    bool const add,
    uint16_t const port_index,
    PortModel const& port,
    DynamicAudioMaps::Mappings const& current,
    std::span<DynamicAudioMapping const> const listed,
    DynamicAudioMaps::Mappings& next) const -> uint8_t
{
    auto const status =
        add ? merged_mappings(port_index, port, current, listed, next) : removed_mappings(port_index, port, current, listed, next);
    if (status == AEM_STATUS_SUCCESS) {
        std::ranges::sort(next, cluster_order);
    }
    return status;
}

auto AemCommandHandler::commit_mappings_command(
    bool const add,
    AemAudioMappingsCommandHeader const& cmd,
    DynamicAudioMaps::Mappings const& current,
    DynamicAudioMaps::Mappings const& next,
    std::span<uint8_t const> const command_data,
    std::span<uint8_t> const out_buffer) -> AemCommandResponse
{
    bool const changed = !std::ranges::equal(next, current);
    if (changed) {
        if (auto const status = commit_mappings(cmd.descriptor_index, current, next); status != AEM_STATUS_SUCCESS) {
            return reject_command(status, command_data, out_buffer);
        }
    }
    // The response echoes the command; the other controllers are told.
    auto const r = reject_command(
        AEM_STATUS_SUCCESS,
        command_data.first(AemAudioMappingsCommandHeader::LENGTH + (cmd.number_of_mappings.get() * AemAudioMapping::LENGTH)),
        out_buffer);
    if (changed) {
        notify_on_success(add ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, r, out_buffer);
    }
    setting_unchanged_ = !changed;  // nothing to save
    return r;
}

auto AemCommandHandler::apply_local_audio_mappings(
    uint16_t const port_index, std::span<DynamicAudioMapping const> const add, std::span<DynamicAudioMapping const> const remove)
    -> uint8_t
{
    if (locked_ || acquired_) {
        return AEM_STATUS_ENTITY_LOCKED;
    }
    auto const current = dynamic_maps_.port(DESCRIPTOR_STREAM_PORT_INPUT, port_index);
    DynamicAudioMaps::Mappings next{};
    if (auto const status = local_mappings(port_index, current, add, remove, next); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    std::ranges::sort(next, cluster_order);
    return commit_local_mappings(port_index, current, next);
}

auto AemCommandHandler::local_mappings(
    uint16_t const port_index,
    DynamicAudioMaps::Mappings const& current,
    std::span<DynamicAudioMapping const> const add,
    std::span<DynamicAudioMapping const> const remove,
    DynamicAudioMaps::Mappings& next) const -> uint8_t
{
    PortModel port{};
    if (auto const status = mappable_port(DESCRIPTOR_STREAM_PORT_INPUT, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    DynamicAudioMaps::Mappings kept{};
    if (auto const status = removed_mappings(port_index, port, current, remove, kept); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    return merged_mappings(port_index, port, kept, add, next);
}

auto AemCommandHandler::commit_local_mappings(
    uint16_t const port_index, DynamicAudioMaps::Mappings const& current, DynamicAudioMaps::Mappings const& next) -> uint8_t
{
    if (std::ranges::equal(next, current)) {
        return AEM_STATUS_SUCCESS;  // nothing to route, tell or save
    }
    if (auto const status = commit_mappings(port_index, current, next); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    // What changed, as a controller would see it: the mappings gone, then those new.
    notify_mappings(false, DESCRIPTOR_STREAM_PORT_INPUT, port_index, current, next);
    notify_mappings(true, DESCRIPTOR_STREAM_PORT_INPUT, port_index, next, current);
    if (callbacks_.on_settings_changed) {
        bool const added = !std::ranges::includes(current, next, cluster_order);
        callbacks_.on_settings_changed(added ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS);
    }
    return AEM_STATUS_SUCCESS;
}

auto AemCommandHandler::set_output_mappings(uint16_t const port_index, std::span<DynamicAudioMapping const> const mappings)
    -> uint8_t
{
    PortModel port{};
    if (auto const status = dynamic_port(DESCRIPTOR_STREAM_PORT_OUTPUT, port_index, port); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    DynamicAudioMaps::Mappings next{};
    if (auto const status = merged_mappings(port_index, port, {}, mappings, next); status != AEM_STATUS_SUCCESS) {
        return status;
    }
    std::ranges::sort(next, cluster_order);
    return keep_output_mappings(port_index, next);
}

auto AemCommandHandler::keep_output_mappings(uint16_t const port_index, DynamicAudioMaps::Mappings const& next) -> uint8_t
{
    auto const current = output_maps_.port(DESCRIPTOR_STREAM_PORT_OUTPUT, port_index);
    if (std::ranges::equal(next, current)) {
        return AEM_STATUS_SUCCESS;
    }
    if (!output_maps_.replace(DESCRIPTOR_STREAM_PORT_OUTPUT, port_index, next)) {
        return AEM_STATUS_NO_RESOURCES;
    }
    notify_mappings(false, DESCRIPTOR_STREAM_PORT_OUTPUT, port_index, current, next);
    notify_mappings(true, DESCRIPTOR_STREAM_PORT_OUTPUT, port_index, next, current);
    return AEM_STATUS_SUCCESS;
}

void AemCommandHandler::notify_mappings(
    bool const add,
    uint16_t const port_type,
    uint16_t const port_index,
    std::span<DynamicAudioMapping const> const of,
    std::span<DynamicAudioMapping const> const but)
{
    std::array<uint8_t, AemAudioMappingsCommandHeader::LENGTH + (MAPPINGS_PER_PDU * AemAudioMapping::LENGTH)> body{};
    size_t count = 0;
    auto const flush = [&] {
        span_store(
            std::span<uint8_t>{body}.first(AemAudioMappingsCommandHeader::LENGTH),
            AemAudioMappingsCommandHeader{
                .descriptor_type = port_type, .descriptor_index = port_index, .number_of_mappings = static_cast<uint16_t>(count)});
        emit_unsolicited(
            add ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS,
            std::span<uint8_t const>{body}.first(AemAudioMappingsCommandHeader::LENGTH + (count * AemAudioMapping::LENGTH)));
        count = 0;
    };
    auto other = but.begin();
    for (auto const& mapping : of) {
        while (other != but.end() && cluster_order(*other, mapping)) {
            ++other;
        }
        if (other != but.end() && *other == mapping) {
            continue;
        }
        AemAudioMapping const wire{
            .stream_index = mapping.stream_index,
            .stream_channel = mapping.stream_channel,
            .cluster_offset = mapping.cluster_offset,
            .cluster_channel = mapping.cluster_channel};
        span_store(
            std::span<uint8_t>{body}.subspan(
                AemAudioMappingsCommandHeader::LENGTH + (count * AemAudioMapping::LENGTH), AemAudioMapping::LENGTH),
            wire);
        if (++count == MAPPINGS_PER_PDU) {
            flush();
        }
    }
    if (count != 0) {
        flush();
    }
}

auto AemCommandHandler::handle_get_audio_map(
    AemDu const& /*header*/, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    if (command_data.size() < AemGetAudioMapCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    auto const cmd = load_payload<AemGetAudioMapCommandPayload>(command_data);
    PortModel port{};
    if (auto const status = dynamic_port(cmd.descriptor_type, cmd.descriptor_index, port); status != AEM_STATUS_SUCCESS) {
        return {.status = status, .size = 0};
    }
    return audio_map_response(cmd, port, out_buffer);
}

auto AemCommandHandler::audio_map_response(
    AemGetAudioMapCommandPayload const& cmd, PortModel const& port, std::span<uint8_t> const out_buffer) const -> AemCommandResponse
{
    // Milan v1.2 Section 5.4.2.26: the port's cluster channels, in order, in
    // fixed partitions of at most 176; number_of_maps is their count whatever
    // is mapped (at least one).
    size_t const maps = std::max<size_t>(1, (port.total_channels + MAX_AUDIO_MAP_MAPPINGS - 1) / MAX_AUDIO_MAP_MAPPINGS);
    if (cmd.map_index.get() >= maps) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }
    auto const mappings = store_audio_map_entries(
        cmd.descriptor_type, cmd.descriptor_index, port, size_t{cmd.map_index.get()} * MAX_AUDIO_MAP_MAPPINGS, out_buffer);
    if (!mappings) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = 0};  // cannot be: 176 fit
    }
    span_store(
        out_buffer,
        AemAudioMapResponseHeader{
            .descriptor_type = cmd.descriptor_type,
            .descriptor_index = cmd.descriptor_index,
            .map_index = cmd.map_index,
            .number_of_maps = static_cast<uint16_t>(maps),
            .number_of_mappings = *mappings});
    return {
        .status = AEM_STATUS_SUCCESS, .size = AemAudioMapResponseHeader::LENGTH + (size_t{*mappings} * AemAudioMapping::LENGTH)};
}

auto AemCommandHandler::store_audio_map_entries(
    uint16_t const port_type,
    uint16_t const port_index,
    PortModel const& port,
    size_t const first,
    std::span<uint8_t> const out_buffer) const -> std::optional<uint16_t>
{
    uint16_t mappings = 0;
    auto const& store = port_type == DESCRIPTOR_STREAM_PORT_INPUT ? dynamic_maps_ : output_maps_;
    for (auto const& mapping : store.port(port_type, port_index)) {
        auto const ordinal = cluster_ordinal(port, mapping);
        if (!ordinal || *ordinal < first || *ordinal >= first + MAX_AUDIO_MAP_MAPPINGS) {
            continue;
        }
        size_t const at = AemAudioMapResponseHeader::LENGTH + (size_t{mappings} * AemAudioMapping::LENGTH);
        if (at + AemAudioMapping::LENGTH > out_buffer.size()) {
            return std::nullopt;
        }
        span_store(
            out_buffer.subspan(at, AemAudioMapping::LENGTH),
            AemAudioMapping{
                .stream_index = mapping.stream_index,
                .stream_channel = mapping.stream_channel,
                .cluster_offset = mapping.cluster_offset,
                .cluster_channel = mapping.cluster_channel});
        ++mappings;
    }
    return mappings;
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
    // No stream-info provider wired => this entity does not implement
    // GET_STREAM_INFO. Checked before argument validation so an entity without
    // streams answers NOT_IMPLEMENTED rather than BAD_ARGUMENTS.
    if (!callbacks_.get_stream_info) {
        return {.status = AEM_STATUS_NOT_IMPLEMENTED, .size = 0};
    }
    // GET_STREAM_INFO command payload (AemGetStreamInfoCommandPayload):
    //   descriptor_type(2) + descriptor_index(2)
    if (command_data.size() < AemGetStreamInfoCommandPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }

    AemGetStreamInfoCommandPayload cmd{};
    span_load(cmd, command_data.first(AemGetStreamInfoCommandPayload::LENGTH));
    uint16_t const descriptor_type = cmd.descriptor_type;
    uint16_t const descriptor_index = cmd.descriptor_index;

    // Echo descriptor_type/index back; the app fills the rest plus the flag bits.
    AemStreamInfoPayload resp{};
    AemStreamInfoMilanExtension ext{};
    resp.descriptor_type = ieee::doublet_t{descriptor_type};
    resp.descriptor_index = ieee::doublet_t{descriptor_index};
    if (!callbacks_.get_stream_info(descriptor_type, descriptor_index, resp, ext)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }

    // Milan v1.2 Section 5.4.2.10 (Figure 5.1): the Milan profile appends
    // flags_ex, pbsta and acmpsta; a classic entity answers IEEE 1722.1-2013's
    // 48 octets.
    size_t const size = profile_.milan ? AEM_STREAM_INFO_MILAN_LENGTH : AemStreamInfoPayload::LENGTH;
    if (out_buffer.size() < size) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    span_store(out_buffer.first(AemStreamInfoPayload::LENGTH), resp);
    if (profile_.milan) {
        span_store(out_buffer.subspan(AemStreamInfoPayload::LENGTH, AemStreamInfoMilanExtension::LENGTH), ext);
    }
    return {.status = AEM_STATUS_SUCCESS, .size = size};
}

auto AemCommandHandler::presentation_offset_target(uint16_t const descriptor_type, uint16_t const descriptor_index) const
    -> std::expected<uint32_t, uint8_t>
{
    if (!callbacks_.get_presentation_offset || !callbacks_.set_presentation_offset) {
        return std::unexpected(AEM_STATUS_NOT_IMPLEMENTED);
    }
    // Milan v1.2 Section 5.4.2.9: "The PAAD-AE shall not implement the
    // SET_STREAM_INFO command on Stream Inputs"; MAX_TRANSIT_TIME is a talker's
    // (Milan v1.3 Section 5.4.2.30).
    if (descriptor_type == DESCRIPTOR_STREAM_INPUT) {
        return std::unexpected(AEM_STATUS_NOT_SUPPORTED);
    }
    if (descriptor_type != DESCRIPTOR_STREAM_OUTPUT) {
        return std::unexpected(AEM_STATUS_NO_SUCH_DESCRIPTOR);  // no stream there
    }
    auto const offset = callbacks_.get_presentation_offset(descriptor_index);
    if (!offset) {
        return std::unexpected(AEM_STATUS_NO_SUCH_DESCRIPTOR);
    }
    return *offset;
}

auto AemCommandHandler::set_presentation_offset(uint16_t const index, uint64_t const offset_ns) -> uint8_t
{
    if (offset_ns > MILAN_PRESENTATION_OFFSET_MAX_NS) {
        return AEM_STATUS_BAD_ARGUMENTS;
    }
    return callbacks_.set_presentation_offset(index, static_cast<uint32_t>(offset_ns));
}

auto AemCommandHandler::handle_set_stream_info(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (command_data.size() < AemStreamInfoPayload::LENGTH || out_buffer.size() < AemStreamInfoPayload::LENGTH) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    auto const cmd = load_payload<AemStreamInfoPayload>(command_data);
    auto const current = presentation_offset_target(cmd.descriptor_type.get(), cmd.descriptor_index.get());
    if (!current) {
        return reject_command(current.error(), command_data.first(AemStreamInfoPayload::LENGTH), out_buffer);
    }
    span_copy(out_buffer.first(AemStreamInfoPayload::LENGTH), command_data.first(AemStreamInfoPayload::LENGTH));
    return set_stream_info(cmd, *current, out_buffer.first(AemStreamInfoPayload::LENGTH));
}

auto AemCommandHandler::set_stream_info(
    AemStreamInfoPayload const& cmd, uint32_t const current, std::span<uint8_t const> const response) -> AemCommandResponse
{
    // "If the PAAD-AE receives a SET_STREAM_INFO command with any unsupported
    // sub-commands, the PAAD-AE shall refuse the entire command by returning
    // the NOT_SUPPORTED error code."
    if ((cmd.flags.get() & stream_info_flags::SET_SUB_COMMANDS & ~stream_info_flags::MSRP_ACC_LAT_VALID) != 0) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = response.size()};
    }
    if ((cmd.flags.get() & stream_info_flags::MSRP_ACC_LAT_VALID) == 0) {
        return {.status = AEM_STATUS_SUCCESS, .size = response.size()};  // nothing asked
    }
    uint32_t const offset = cmd.msrp_accumulated_latency.get();
    auto const status = set_presentation_offset(cmd.descriptor_index.get(), offset);
    // The response echoes the command (MSRP_ACC_LAT_VALID and the offset), and
    // goes to the registered controllers when the offset changed.
    if (status == AEM_STATUS_SUCCESS && offset != current) {
        notify_stream_info_offset(cmd.descriptor_index.get(), offset, response);
    }
    return {.status = status, .size = response.size()};
}

void AemCommandHandler::notify_stream_info_offset(
    uint16_t const index, uint32_t const offset, std::span<uint8_t const> const response)
{
    emit_unsolicited(AEM_COMMAND_SET_STREAM_INFO, response, current_header_.controller_entity_id);
    // A controller following GET_STREAM_INFO or MAX_TRANSIT_TIME (Milan
    // 1.3) sees it too.
    (void)notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, index);
    std::array<uint8_t, AemGetMaxTransitTimeResponsePayload::LENGTH> body{};
    store_max_transit_time(body, index, offset);
    emit_unsolicited(AEM_COMMAND_GET_MAX_TRANSIT_TIME, body);
}

auto AemCommandHandler::handle_set_max_transit_time(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (command_data.size() < AemSetMaxTransitTimePayload::LENGTH || out_buffer.size() < AemSetMaxTransitTimePayload::LENGTH) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    auto const cmd = load_payload<AemSetMaxTransitTimePayload>(command_data);
    auto const current = presentation_offset_target(cmd.descriptor_type.get(), cmd.descriptor_index.get());
    if (!current) {
        return reject_command(current.error(), command_data.first(AemSetMaxTransitTimePayload::LENGTH), out_buffer);
    }
    span_copy(out_buffer.first(AemSetMaxTransitTimePayload::LENGTH), command_data.first(AemSetMaxTransitTimePayload::LENGTH));
    return set_max_transit_time(cmd, *current, out_buffer.first(AemSetMaxTransitTimePayload::LENGTH));
}

auto AemCommandHandler::set_max_transit_time(
    AemSetMaxTransitTimePayload const& cmd, uint32_t const current, std::span<uint8_t const> const response) -> AemCommandResponse
{
    uint16_t const index = cmd.descriptor_index.get();
    uint64_t const requested = cmd.max_transit_time.get();
    auto const status = set_presentation_offset(index, requested);
    if (status == AEM_STATUS_SUCCESS && requested != current) {
        emit_unsolicited(AEM_COMMAND_SET_MAX_TRANSIT_TIME, response, current_header_.controller_entity_id);
        // GET_MAX_TRANSIT_TIME's response has SET's layout: the echoed command.
        emit_unsolicited(AEM_COMMAND_GET_MAX_TRANSIT_TIME, response);
        (void)notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, index);
    }
    return {.status = status, .size = response.size()};
}

auto AemCommandHandler::handle_get_max_transit_time(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    if (command_data.size() < AemGetMaxTransitTimeCommandPayload::LENGTH ||
        out_buffer.size() < AemGetMaxTransitTimeResponsePayload::LENGTH) {
        return reject_command(AEM_STATUS_BAD_ARGUMENTS, command_data, out_buffer);
    }
    auto const cmd = load_payload<AemGetMaxTransitTimeCommandPayload>(command_data);
    auto const current = presentation_offset_target(cmd.descriptor_type.get(), cmd.descriptor_index.get());
    if (!current) {
        return reject_command(current.error(), command_data.first(AemGetMaxTransitTimeCommandPayload::LENGTH), out_buffer);
    }
    store_max_transit_time(out_buffer, cmd.descriptor_index.get(), *current);
    return {.status = AEM_STATUS_SUCCESS, .size = AemGetMaxTransitTimeResponsePayload::LENGTH};
}

auto AemCommandHandler::handle_acquire_entity(
    AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // Milan v1.2 Section 5.4.2.1: ACQUIRE_ENTITY is NOT_SUPPORTED; nothing is acquired, so
    // the controller is not registered for notifications either.
    if (profile_.milan) {
        return reject_command(AEM_STATUS_NOT_SUPPORTED, command_data, out_buffer);
    }

    // ACQUIRE_ENTITY command payload (AemAcquireEntityPayload):
    //   flags(4) + owner_entity_id(8) + descriptor_type(2) + descriptor_index(2)
    if (command_data.size() < AemAcquireEntityPayload::LENGTH) {
        return {.status = AEM_STATUS_BAD_ARGUMENTS, .size = 0};
    }

    AemAcquireEntityPayload cmd{};
    span_load(cmd, command_data);
    uint32_t const flags = cmd.flags;  // host order via wire-type conversion

    bool const release = cmd.is_release();

    if (release) {
        // Release acquisition
        if (acquired_ && acquiring_controller_ == header.controller_entity_id) {
            acquired_ = false;
            acquired_persistent_ = false;
            acquiring_controller_ = {};
            acquiring_controller_mac_ = {};
            return build_acquire_response(
                {.status = AEM_STATUS_SUCCESS, .flags = 0, .entity_id = {}, .command_data = command_data}, out_buffer);
        }
        return build_acquire_response(
            {.status = AEM_STATUS_ENTITY_ACQUIRED,
             .flags = flags,
             .entity_id = acquiring_controller_,
             .command_data = command_data},
            out_buffer);
    }

    // Acquire
    bool const persistent = cmd.is_persistent();
    if (!acquired_) {
        acquired_ = true;
        acquired_persistent_ = persistent;
        acquiring_controller_ = header.controller_entity_id;
        acquiring_controller_mac_ = current_src_mac_;
        return build_acquire_response(
            {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = acquiring_controller_, .command_data = command_data},
            out_buffer);
    }
    if (acquiring_controller_ == header.controller_entity_id) {
        // Already acquired by this controller, which may have moved (a local
        // controller that reconnected): its unsolicited notifications, and
        // forgetting it (forget_controller_at), follow the new address.
        acquiring_controller_mac_ = current_src_mac_;
        return build_acquire_response(
            {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = acquiring_controller_, .command_data = command_data},
            out_buffer);
    }
    // Acquired by another controller - initiate CONTROLLER_AVAILABLE handshake
    // Skip if existing acquisition is persistent or no callback available
    if (!acquired_persistent_ && send_controller_available) {
        // Store pending acquire and send CONTROLLER_AVAILABLE to current owner
        pending_acquire_.requester_header = header;
        pending_acquire_.requester_mac = current_src_mac_;
        pending_acquire_.requester_flags = flags;
        std::copy_n(
            command_data.begin(),
            std::min(command_data.size(), pending_acquire_.command_data_copy.size()),
            pending_acquire_.command_data_copy.begin());
        pending_acquire_.timeout = event_time_ + std::chrono::milliseconds(AEM_TIMEOUT_MS);
        pending_acquire_.active = true;

        send_controller_available(acquiring_controller_);

        // Return IN_PROGRESS - the final response will be sent by
        // controller_available_response_received() or controller_available_timed_out()
        return {.status = AEM_STATUS_IN_PROGRESS, .size = 0};
    }

    // No CONTROLLER_AVAILABLE callback or PERSISTENT - deny immediately
    return build_acquire_response(
        {.status = AEM_STATUS_ENTITY_ACQUIRED, .flags = flags, .entity_id = acquiring_controller_, .command_data = command_data},
        out_buffer);
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
    uint32_t const flags = cmd.flags;  // host order via wire-type conversion

    bool const unlock = cmd.is_unlock();

    if (unlock) {
        // Unlock
        if (locked_ && locking_controller_ == header.controller_entity_id) {
            locked_ = false;
            locking_controller_ = {};
            return build_lock_response(
                {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = {}, .command_data = command_data}, out_buffer);
        }
        if (!locked_) {
            return build_lock_response(
                {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = {}, .command_data = command_data}, out_buffer);
        }
        return build_lock_response(
            {.status = AEM_STATUS_ENTITY_LOCKED, .flags = flags, .entity_id = locking_controller_, .command_data = command_data},
            out_buffer);
    }

    // Lock
    if (!locked_) {
        locked_ = true;
        locking_controller_ = header.controller_entity_id;
        lock_expiry_time_ = event_time_ + std::chrono::milliseconds(AEM_LOCK_TIMEOUT_MS);
        return build_lock_response(
            {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = locking_controller_, .command_data = command_data},
            out_buffer);
    }
    if (locking_controller_ == header.controller_entity_id) {
        // Already locked by this controller - refresh timeout
        lock_expiry_time_ = event_time_ + std::chrono::milliseconds(AEM_LOCK_TIMEOUT_MS);
        return build_lock_response(
            {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = locking_controller_, .command_data = command_data},
            out_buffer);
    }
    // Locked by another controller
    return build_lock_response(
        {.status = AEM_STATUS_ENTITY_LOCKED, .flags = flags, .entity_id = locking_controller_, .command_data = command_data},
        out_buffer);
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
        dynamic_maps_ = {};  // the mappings belong to the configuration's ports
        output_maps_ = {};
    }

    AemSetConfigurationPayload resp{};
    resp.configuration_index = current_configuration_;
    span_store(out_buffer, resp);
    if (switching) {
        emit_unsolicited(
            AEM_COMMAND_SET_CONFIGURATION,
            std::span<uint8_t const>{out_buffer.data(), AemSetConfigurationPayload::LENGTH},
            current_header_.controller_entity_id);
    }
    return {.status = AEM_STATUS_SUCCESS, .size = AemSetConfigurationPayload::LENGTH};
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

auto AemCommandHandler::handle_register_unsolicited(AemDu const& header) -> AemCommandResponse
{
    // Idempotent: if this controller is already registered, return success,
    // notifying it where this command came from (it may have moved: a local
    // controller that reconnected has a new address).
    auto const existing = unsolicited_registrations_.find_if(
        [&header](UnsolicitedRegistration const& r) { return r.controller_entity_id == header.controller_entity_id; });
    if (existing < unsolicited_registrations_.capacity()) {
        auto& reg = unsolicited_registrations_[existing];
        if (reg.controller_mac != current_src_mac_) {
            // A new station: its sequence ids start again (Milan v1.2 Section 5.3.4.2).
            reg = UnsolicitedRegistration{
                .controller_entity_id = header.controller_entity_id,
                .controller_mac = current_src_mac_,
                .monitor_deadline = event_time_ + monitor_period()};
        }
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }

    // Stash the source MAC (set by process_packet) so unsolicited notifications
    // can be unicast back to this controller. Its sequence ids start at 0 and
    // its monitor timer starts now (Milan v1.2 Section 5.4.5.3).
    if (!unsolicited_registrations_.add(UnsolicitedRegistration{
            .controller_entity_id = header.controller_entity_id,
            .controller_mac = current_src_mac_,
            .monitor_deadline = event_time_ + monitor_period()})) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = 0};
    }
    return {.status = AEM_STATUS_SUCCESS, .size = 0};
}

void AemCommandHandler::note_controller_command(Eui64 const& controller_entity_id, Eui48 const& /*controller_mac*/)
{
    // Registrations are kept by entity ID (handle_register_unsolicited), so is the timer.
    for (auto& reg : unsolicited_registrations_) {
        if (reg.controller_entity_id == controller_entity_id) {
            reg.monitor_deadline = event_time_ + monitor_period();
            reg.available_outstanding = false;
        }
    }
}

auto AemCommandHandler::monitor_period() noexcept -> sm::Clock::duration
{
    // Numerical Recipes' LCG: uniform enough to spread the controllers' timers.
    monitor_random_ = (monitor_random_ * 1664525U) + 1013904223U;
    auto const span_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(CONTROLLER_MONITOR_MAX - CONTROLLER_MONITOR_MIN).count();
    auto const offset = std::chrono::milliseconds((monitor_random_ >> 8U) % static_cast<uint32_t>(span_ms + 1));
    return std::chrono::duration_cast<sm::Clock::duration>(CONTROLLER_MONITOR_MIN + offset);
}

void AemCommandHandler::send_controller_available_to(Eui48 const& dest_mac, Eui64 const& controller_id, uint16_t const sequence_id)
{
    if (!callbacks_.send_response) {
        return;
    }
    std::array<uint8_t, AemDu::LENGTH> buffer{};
    span_store(buffer, controller_available_command(controller_id, sequence_id));
    (void)callbacks_.send_response(dest_mac, buffer);
}

auto AemCommandHandler::controller_available_command(Eui64 const& controller_id, uint16_t const sequence_id) const -> AemDu
{
    // We are the controller of this command: the registered controller is its
    // target (IEEE 1722.1-2021 7.4.69, no payload).
    AemDu hdr{};
    hdr.init_command(AEM_COMMAND_CONTROLLER_AVAILABLE, AemDu::AEM_DATA_LENGTH);
    hdr.target_entity_id = controller_id;
    hdr.controller_entity_id = our_entity_id_;
    hdr.sequence_id = doublet_t{sequence_id};
    return hdr;
}

void AemCommandHandler::tick_controller_monitor(sm::TimePoint const now)
{
    for (size_t i = 0; i < unsolicited_registrations_.size();) {
        auto& reg = unsolicited_registrations_[i];
        if (now < reg.monitor_deadline) {
            ++i;
            continue;
        }
        if (!reg.available_outstanding || reg.available_tries < 2) {
            // The command, or its one retry (IEEE 1722.1-2021 9.3.6), with the
            // same sequence_id.
            if (!reg.available_outstanding) {
                reg.available_outstanding = true;
                reg.available_tries = 0;
                reg.available_sequence_id = command_sequence_id_++;
            }
            ++reg.available_tries;
            reg.monitor_deadline = now + std::chrono::milliseconds(AEM_TIMEOUT_MS);
            send_controller_available_to(reg.controller_mac, reg.controller_entity_id, reg.available_sequence_id);
            ++i;
            continue;
        }
        // No answer to either: deregister it and tell it alone (Milan v1.2
        // Section 5.4.5.3, Table 5.22). The last entry moves to i.
        auto const gone = reg;
        unsolicited_registrations_.remove(i);
        send_unsolicited_to(
            gone.controller_mac, gone.controller_entity_id, gone.sequence_id, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, {});
    }
}

void AemCommandHandler::handle_aem_response(AemDu const& header)
{
    if (header.command_code() != AEM_COMMAND_CONTROLLER_AVAILABLE) {
        return;
    }
    // The classic ACQUIRE handshake's question to the holder: it is alive.
    if (pending_acquire_.active && acquired_ && header.target_entity_id == acquiring_controller_) {
        controller_available_response_received();
    }
    // Any status will do (Section 5.4.5.3): the controller is there.
    for (auto& reg : unsolicited_registrations_) {
        if (reg.available_outstanding && reg.controller_entity_id == header.target_entity_id &&
            reg.available_sequence_id == header.sequence_id.get()) {
            reg.available_outstanding = false;
            reg.monitor_deadline = std::max(event_time_, now_) + monitor_period();
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
    // Frame an unsolicited AEM response: our id as target, the controller's id,
    // its sequence_id, and the U-bit set.
    AemDu hdr{};
    hdr.target_entity_id = our_entity_id_;
    hdr.controller_entity_id = controller_id;
    hdr.sequence_id = doublet_t{sequence_id};
    hdr.init_response(
        command_type,
        AEM_STATUS_SUCCESS,
        static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + body.size()),
        /*unsolicited=*/true);

    std::array<uint8_t, AemDu::LENGTH + MAX_AEM_RESPONSE_SIZE> buffer{};
    auto buffer_span = std::span{buffer};
    span_store(buffer_span, hdr);
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

void AemCommandHandler::emit_unsolicited(
    uint16_t const command_type, std::span<uint8_t const> body, std::optional<Eui64> const& requester)
{
    if (!callbacks_.send_response) {
        return;
    }

    // Milan v1.2 Section 5.4.5.1: every registered controller but the requester, each with its
    // own sequence id, incremented whether or not the send succeeds. Classic tells the requester too.
    for (auto& reg : unsolicited_registrations_) {
        if (profile_.milan && requester == reg.controller_entity_id) {
            continue;
        }
        send_unsolicited_to(reg.controller_mac, reg.controller_entity_id, reg.sequence_id++, command_type, body);
    }

    // IEEE 1722.1 7.5.2: the acquiring controller is implicitly registered.
    if (acquired_) {
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

auto AemCommandHandler::apply_local_descriptor_value(uint16_t const command_type, std::span<uint8_t const> command_body) -> uint8_t
{
    // Apply the change exactly as a controller's SET would (same symbol-keyed
    // hook), then fan out unsolicited notifications. This is the entry point for
    // an entity changing its own controls (e.g. a software IDENTIFY button).
    AemEntityModel aem_model{*handler_};
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> echo{};
    auto const r = aem_model.apply_set_descriptor_value(command_type, command_body, echo);
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(command_type, std::span<uint8_t const>{echo.data(), r.size});
        if (is_persistent_setting_command(command_type) && callbacks_.on_settings_changed) {
            callbacks_.on_settings_changed(command_type);
        }
    }
    return r.status;
}

void AemCommandHandler::controller_available_response_received()
{
    if (!pending_acquire_.active) {
        return;
    }

    // Current owner is alive — deny the pending requester with ENTITY_ACQUIRED.
    // The final response is sent here via the send_response callback rather
    // than being returned to the caller, because the IN_PROGRESS path in
    // process_packet() has already returned by the time we get here.
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> out_buffer{};
    auto const original_command_data = std::span<uint8_t const>{pending_acquire_.command_data_copy};
    auto const response = build_acquire_response(
        {.status = AEM_STATUS_ENTITY_ACQUIRED,
         .flags = pending_acquire_.requester_flags,
         .entity_id = acquiring_controller_,
         .command_data = original_command_data},
        out_buffer);
    (void)send_response(
        pending_acquire_.requester_mac,
        pending_acquire_.requester_header,
        response.status,
        std::span<uint8_t const>{out_buffer.data(), response.size});

    pending_acquire_ = {};
}

void AemCommandHandler::controller_available_timed_out()
{
    if (!pending_acquire_.active) {
        return;
    }

    // Current owner is gone — transfer acquisition to the requester and
    // send a SUCCESS response. See controller_available_response_received
    // above for why the response is emitted here rather than returned.
    acquired_ = true;
    acquiring_controller_ = pending_acquire_.requester_header.controller_entity_id;
    acquiring_controller_mac_ = pending_acquire_.requester_mac;
    acquired_persistent_ = (pending_acquire_.requester_flags & 0x01u) != 0u;

    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> out_buffer{};
    auto const original_command_data = std::span<uint8_t const>{pending_acquire_.command_data_copy};
    auto const response = build_acquire_response(
        {.status = AEM_STATUS_SUCCESS,
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

    pending_acquire_ = {};
}

}  // namespace statusbar::nanoavb
