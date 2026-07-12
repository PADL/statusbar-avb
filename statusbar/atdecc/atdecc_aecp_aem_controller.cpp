// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEM Controller State Machine action implementations

#include "statusbar/atdecc/atdecc_aecp_aem_controller.hpp"

#include <algorithm>
#include <array>

namespace statusbar::atdecc::aem_controller_actions {

namespace {

/// Report a command that never made it onto the wire, so a completion-based
/// caller is not stranded waiting for a response that can never arrive.
void report_send_failure(AemCommandParams& params)
{
    if (params.completion) {
        params.completion(AemCommandResult{
            .delivery = AemCommandDelivery::SendFailed,
            .status = 0,
            .command_type = params.command_code,
            .target_entity_id = params.target_entity_id,
            .sent_payload = params.command_data,
            .response = {}});
        params.completion = {};
    }
}

}  // namespace

void send_aem_command(AemControllerContext& ctx, sm::TimePoint event_time)
{
    auto& params = ctx.command_params;

    // Refuse to send if there's no free inflight slot. Otherwise the packet
    // would go on the wire untracked and its response would be silently
    // dropped, stranding any queued descriptor read that was relying on it.
    if (ctx.inflight.is_full()) {
        report_send_failure(params);
        return;
    }

    // Build AemDu header
    AemDu header{};
    auto const payload_size = std::min(params.command_data.size(), AemInflightCommand::MAX_PAYLOAD);
    auto const cdl = static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + payload_size);
    header.init_command(params.command_code, cdl);
    header.target_entity_id = params.target_entity_id;
    header.controller_entity_id = ctx.my_id;
    header.sequence_id = ctx.next_sequence_id++;

    // Serialize header + payload into send buffer
    std::array<uint8_t, AemDu::LENGTH + AemInflightCommand::MAX_PAYLOAD> send_buffer{};
    auto const out_frame = span_pack_header_payload(send_buffer, header, params.command_data.first(payload_size));

    // Try to send
    if (ctx.tx_command && ctx.tx_command(out_frame)) {
        // We checked is_full() above; in a single-threaded context the
        // table cannot have filled between then and now.
        [[maybe_unused]] bool const added =
            ctx.add_inflight(header, params.command_data.first(payload_size), event_time, std::move(params.completion));
        params.completion = {};
    } else {
        report_send_failure(params);
    }
}

void handle_aem_response(AemControllerContext& ctx, sm::TimePoint event_time)
{
    auto const idx = ctx.current_inflight_index;
    auto* entry = ctx.inflight.get(idx);
    if (entry == nullptr) {
        return;
    }

    auto const status = ctx.rcvd_header.status();

    // Check if IN_PROGRESS - extends deadline
    auto tracker_result = entry->tracker.receive_response(status, ctx.rcvd_header.sequence_id.get(), event_time);

    if (tracker_result == AemCommandTracker::Result::Pending) {
        // Still waiting - deadline extended
        return;
    }

    // Final response — also pass the original request payload (some entities
    // zero out the response's echoed descriptor_type/index on errors).
    std::span<uint8_t const> const sent_payload{entry->payload.data(), entry->payload.size()};
    if (ctx.on_response) {
        ctx.on_response(ctx.rcvd_header, sent_payload, ctx.rcvd_response_data, status);
    }
    if (entry->completion) {
        entry->completion(AemCommandResult{
            .delivery = AemCommandDelivery::Responded,
            .status = status,
            .command_type = entry->sent_header.command_code(),
            .target_entity_id = entry->sent_header.target_entity_id,
            .sent_payload = sent_payload,
            .response = ctx.rcvd_response_data});
    }
    ctx.remove_inflight(idx);
}

void handle_aem_timeout(AemControllerContext& ctx, sm::TimePoint /*event_time*/)
{
    auto const idx = ctx.current_inflight_index;
    auto* entry = ctx.inflight.get(idx);
    if (entry == nullptr) {
        return;
    }

    if (ctx.on_timeout) {
        ctx.on_timeout(entry->sent_header);
    }
    if (entry->completion) {
        entry->completion(AemCommandResult{
            .delivery = AemCommandDelivery::TimedOut,
            .status = 0,
            .command_type = entry->sent_header.command_code(),
            .target_entity_id = entry->sent_header.target_entity_id,
            .sent_payload = std::span<uint8_t const>{entry->payload.data(), entry->payload.size()},
            .response = {}});
    }
    ctx.remove_inflight(idx);
}

}  // namespace statusbar::atdecc::aem_controller_actions
