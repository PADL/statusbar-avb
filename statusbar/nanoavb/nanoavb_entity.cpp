// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_entity.hpp"

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"

#include <cstring>
#include <span>

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
            out[FORMATS_OFFSET_POS] = static_cast<uint8_t>((L2016 >> 8) & 0xFFU);
            out[FORMATS_OFFSET_POS + 1] = static_cast<uint8_t>(L2016 & 0xFFU);
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
    // Validate minimum size
    if (payload.size() < AemDu::LENGTH) {
        return false;
    }

    // Parse header
    AemDu header{};
    span_load(header, payload);

    // Validate header
    if (!header.is_valid() || !header.is_command()) {
        return false;
    }

    // Check target_entity_id matches us
    if (header.target_entity_id != our_entity_id) {
        return false;  // Not for us
    }

    // Keep our id current for unsolicited notifications (their target_entity_id).
    our_entity_id_ = our_entity_id;

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
    return send_response(src_mac, header, response.status, std::span<uint8_t const>{out_buffer.data(), response.size});
}

auto AemCommandHandler::handle_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
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
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_name(command_data, out_buffer);

        case AEM_COMMAND_ENTITY_AVAILABLE:
            return handle_entity_available(header);

        case AEM_COMMAND_ACQUIRE_ENTITY:
            return handle_acquire_entity(header, command_data, out_buffer);

        case AEM_COMMAND_LOCK_ENTITY:
            return handle_lock_entity(header, command_data, out_buffer);

        case AEM_COMMAND_GET_CONFIGURATION:
            return handle_get_configuration(header, out_buffer);

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

        case AEM_COMMAND_SET_CLOCK_SOURCE:
            if (auto const blocked = check_exclusive_access(header)) {
                return reject_command(*blocked, command_data, out_buffer);
            }
            return handle_set_descriptor_value(AEM_COMMAND_SET_CLOCK_SOURCE, command_data, out_buffer);

        case AEM_COMMAND_GET_CLOCK_SOURCE:
            return handle_get_descriptor_value(AEM_COMMAND_GET_CLOCK_SOURCE, command_data, out_buffer);

        case AEM_COMMAND_GET_COUNTERS:
            return handle_get_counters(header, command_data, out_buffer);

        case AEM_COMMAND_GET_STREAM_INFO:
            return handle_get_stream_info(header, command_data, out_buffer);

        case AEM_COMMAND_GET_STREAM_FORMAT:
            return handle_get_stream_format(header, command_data, out_buffer);

        case AEM_COMMAND_GET_SAMPLING_RATE:
            return handle_get_sampling_rate(header, command_data, out_buffer);

        case AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
            return handle_register_unsolicited(header);

        case AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION:
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
    // ENTITY_ACQUIRED takes precedence over ENTITY_LOCKED (an acquisition is the
    // stronger claim). Either held by a controller OTHER than the sender blocks the
    // mutation; the owning controller (or a free entity) proceeds.
    if (acquired_ && !(acquiring_controller_ == header.controller_entity_id)) {
        return AEM_STATUS_ENTITY_ACQUIRED;
    }
    if (locked_ && !(locking_controller_ == header.controller_entity_id)) {
        return AEM_STATUS_ENTITY_LOCKED;
    }
    return std::nullopt;
}

auto AemCommandHandler::reject_command(uint8_t const status, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
    -> AemCommandResponse
{
    size_t size = 0;
    if (out_buffer.size() >= command_data.size()) {
        std::copy(command_data.begin(), command_data.end(), out_buffer.begin());
        size = command_data.size();
    }
    return {.status = status, .size = size};
}

auto AemCommandHandler::handle_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // Per-call model; the handler applies the name by (descriptor, name_index,
    // symbol). The response echoes the command (header + 64-byte name).
    AemEntityModel aem_model{*handler_};
    auto const status = aem_model.apply_set_name(command_data);

    size_t size = 0;
    if (status == AEM_STATUS_SUCCESS && out_buffer.size() >= command_data.size()) {
        std::copy(command_data.begin(), command_data.end(), out_buffer.begin());
        size = command_data.size();
    }
    return {.status = status, .size = size};
}

auto AemCommandHandler::handle_set_descriptor_value(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    // Per-call model (handler_ resolves the descriptor symbol via its storage). The
    // handler applies the value by (command_type, symbol); the response echoes the command.
    AemEntityModel aem_model{*handler_};
    auto const r = aem_model.apply_set_descriptor_value(command_type, command_data, out_buffer);

    // A controller changed a control: notify every registered controller (IEEE
    // 1722.1 9.6 unsolicited notifications). All subscribers are notified,
    // including the originator — the U-bit + distinct sequence_id let a
    // controller tell its own unsolicited echo from the direct response.
    if (r.status == AEM_STATUS_SUCCESS) {
        emit_unsolicited(command_type, std::span<uint8_t const>{out_buffer.data(), r.size});
    }
    return {.status = r.status, .size = r.size};
}

auto AemCommandHandler::handle_get_descriptor_value(
    uint16_t const command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse
{
    AemEntityModel const aem_model{*handler_};
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
    DescriptorRef const ref{.configuration_index = 0, .descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
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
    DescriptorRef const ref{.configuration_index = 0, .descriptor_type = descriptor_type, .descriptor_index = descriptor_index};
    auto const n = aem_model.get_descriptor_for_wire(ref, desc);
    constexpr size_t SR_OFFSET = offsetof(DescriptorAudioUnit, current_sampling_rate);  // 136
    if (n < SR_OFFSET + 4) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }
    AemSamplingRatePayload resp{};
    resp.descriptor_type = descriptor_type;
    resp.descriptor_index = descriptor_index;
    // current_sampling_rate is a network-order quadlet in the wire descriptor.
    resp.sampling_rate = static_cast<uint32_t>(
        (static_cast<uint32_t>(desc[SR_OFFSET]) << 24) | (static_cast<uint32_t>(desc[SR_OFFSET + 1]) << 16) |
        (static_cast<uint32_t>(desc[SR_OFFSET + 2]) << 8) | static_cast<uint32_t>(desc[SR_OFFSET + 3]));
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
    resp.descriptor_type = ieee::doublet_t{descriptor_type};
    resp.descriptor_index = ieee::doublet_t{descriptor_index};
    if (!callbacks_.get_stream_info(descriptor_type, descriptor_index, resp)) {
        return {.status = AEM_STATUS_NO_SUCH_DESCRIPTOR, .size = 0};
    }

    if (out_buffer.size() < AemStreamInfoPayload::LENGTH) {
        return {.status = AEM_STATUS_NOT_SUPPORTED, .size = 0};
    }
    span_store(out_buffer.first(AemStreamInfoPayload::LENGTH), resp);
    return {.status = AEM_STATUS_SUCCESS, .size = AemStreamInfoPayload::LENGTH};
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
    uint32_t const flags = cmd.flags;  // host order via wire-type conversion

    bool const release = cmd.is_release();

    if (release) {
        // Release acquisition
        if (acquired_ && acquiring_controller_ == header.controller_entity_id) {
            acquired_ = false;
            acquired_persistent_ = false;
            acquiring_controller_ = {};
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
        return build_acquire_response(
            {.status = AEM_STATUS_SUCCESS, .flags = flags, .entity_id = acquiring_controller_, .command_data = command_data},
            out_buffer);
    }
    if (acquiring_controller_ == header.controller_entity_id) {
        // Already acquired by this controller
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
    // Idempotent: if this controller is already registered, return success.
    auto const existing = unsolicited_registrations_.find_if(
        [&header](UnsolicitedRegistration const& r) { return r.controller_entity_id == header.controller_entity_id; });
    if (existing < unsolicited_registrations_.capacity()) {
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }

    // Stash the source MAC (set by process_packet) so unsolicited notifications
    // can be unicast back to this controller.
    if (!unsolicited_registrations_.add(
            UnsolicitedRegistration{.controller_entity_id = header.controller_entity_id, .controller_mac = current_src_mac_})) {
        return {.status = AEM_STATUS_NO_RESOURCES, .size = 0};
    }
    return {.status = AEM_STATUS_SUCCESS, .size = 0};
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

auto AemCommandHandler::unsolicited_registration_count() const noexcept -> size_t
{
    return unsolicited_registrations_.size();
}

void AemCommandHandler::send_unsolicited_to(
    Eui48 const& dest_mac, Eui64 const& controller_id, uint16_t const command_type, std::span<uint8_t const> body)
{
    // Frame an unsolicited AEM response: our id as target, the controller's id,
    // a fresh sequence_id, and the U-bit set.
    AemDu hdr{};
    hdr.target_entity_id = our_entity_id_;
    hdr.controller_entity_id = controller_id;
    hdr.sequence_id = doublet_t{unsolicited_sequence_id_++};
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
    std::array<uint8_t, 5> const body{
        static_cast<uint8_t>((DESCRIPTOR_CONTROL >> 8) & 0xFF),
        static_cast<uint8_t>(DESCRIPTOR_CONTROL & 0xFF),
        static_cast<uint8_t>((identify_control_index_ >> 8) & 0xFF),
        static_cast<uint8_t>(identify_control_index_ & 0xFF),
        on ? uint8_t{0xFF} : uint8_t{0x00}};
    return apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body);
}

auto AemCommandHandler::targets_identify_control(uint16_t const command_type, std::span<uint8_t const> body) const noexcept -> bool
{
    // Body header: descriptor_type(2) + descriptor_index(2), big-endian.
    if (!identify_control_index_valid_ || command_type != AEM_COMMAND_SET_CONTROL || body.size() < 4) {
        return false;
    }
    auto const descriptor_type = static_cast<uint16_t>((static_cast<uint16_t>(body[0]) << 8) | body[1]);
    auto const descriptor_index = static_cast<uint16_t>((static_cast<uint16_t>(body[2]) << 8) | body[3]);
    return descriptor_type == DESCRIPTOR_CONTROL && descriptor_index == identify_control_index_;
}

void AemCommandHandler::emit_unsolicited(uint16_t const command_type, std::span<uint8_t const> body)
{
    if (!callbacks_.send_response) {
        return;
    }

    for (auto const& reg : unsolicited_registrations_) {
        send_unsolicited_to(reg.controller_mac, reg.controller_entity_id, command_type, body);
    }

    // An IDENTIFY-control change is additionally announced to the IDENTIFY
    // multicast (IEEE 1722.1) so any controller — even one not registered for
    // unsolicited notifications — observes the identify. No specific controller,
    // so controller_entity_id is left zero.
    if (targets_identify_control(command_type, body)) {
        send_unsolicited_to(atdecc::ATDECC_IDENTIFY_MULTICAST_MAC, Eui64{}, command_type, body);
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

    pending_acquire_ = {};
}

}  // namespace statusbar::nanoavb
