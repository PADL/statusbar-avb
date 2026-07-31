// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp_aa_analysis.hpp"

#include <format>
#include <string_view>

namespace statusbar::atdecc {

using namespace statusbar::atdecc::aem;

// ---------------------------------------------------------------------------
// UploadSession methods
// ---------------------------------------------------------------------------

void UploadSession::log_timeline(uint64_t time_us, uint64_t capture_start_us, std::string msg)
{
    double const t = static_cast<double>(time_us - capture_start_us) / 1e6;
    timeline.push_back(std::format("{:.6f}s  {}", t, msg));
}

void UploadSession::log_issue(uint64_t time_us, uint64_t capture_start_us, std::string msg)
{
    double const t = static_cast<double>(time_us - capture_start_us) / 1e6;
    issues.push_back(std::format("{:.3f}s  {}", t, msg));
}

// ---------------------------------------------------------------------------
// upload_phase_name
// ---------------------------------------------------------------------------

auto upload_phase_name(UploadPhase p) -> std::string_view
{
    switch (p) {
        case UploadPhase::Idle:
            return "IDLE";
        case UploadPhase::UploadRequested:
            return "UPLOAD_REQUESTED";
        case UploadPhase::UploadAccepted:
            return "UPLOAD_ACCEPTED";
        case UploadPhase::Transferring:
            return "TRANSFERRING";
        case UploadPhase::StoreRequested:
            return "STORE_REQUESTED";
        case UploadPhase::Storing:
            return "STORING";
        case UploadPhase::Complete:
            return "COMPLETE";
        case UploadPhase::Aborted:
            return "ABORTED";
        case UploadPhase::Failed:
            return "FAILED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// update_address_tracker
// ---------------------------------------------------------------------------

static void record_backward(
    AddressTracker& trk, uint16_t seq_id, uint64_t time_us, uint64_t capture_start_us, std::string_view mode_name, uint64_t address)
{
    double const time_s = static_cast<double>(time_us - capture_start_us) / 1e6;
    trk.issues.push_back(std::format(
        "seq={} at {:.3f}s: {} backward 0x{:X} -> 0x{:X} (delta -{})",
        seq_id,
        time_s,
        mode_name,
        trk.last_address,
        address,
        trk.last_address - address));
}

static void record_overlap(
    AddressTracker& trk, uint16_t seq_id, uint64_t time_us, uint64_t capture_start_us, std::string_view mode_name, uint64_t address)
{
    double const time_s = static_cast<double>(time_us - capture_start_us) / 1e6;
    trk.issues.push_back(std::format(
        "seq={} at {:.3f}s: {} overlap at 0x{:X} (expected 0x{:X}, overlap {} bytes)",
        seq_id,
        time_s,
        mode_name,
        address,
        trk.expected_next,
        trk.expected_next - address));
}

static void record_gap(
    AddressTracker& trk, uint16_t seq_id, uint64_t time_us, uint64_t capture_start_us, std::string_view mode_name, uint64_t address)
{
    double const time_s = static_cast<double>(time_us - capture_start_us) / 1e6;
    trk.issues.push_back(std::format(
        "seq={} at {:.3f}s: {} gap 0x{:X}..0x{:X} ({} bytes)",
        seq_id,
        time_s,
        mode_name,
        trk.expected_next,
        address,
        address - trk.expected_next));
}

void update_address_tracker(
    AddressTracker& trk,
    uint64_t address,
    uint16_t length,
    uint16_t seq_id,
    uint64_t time_us,
    uint64_t capture_start_us,
    std::string_view mode_name)
{
    trk.total_bytes += length;
    if (!trk.has_first) {
        trk.first_address = address;
        trk.last_address = address;
        trk.last_length = length;
        trk.expected_next = address + length;
        trk.has_first = true;
        return;
    }
    if (address < trk.last_address) {
        trk.backward_count++;
        record_backward(trk, seq_id, time_us, capture_start_us, mode_name, address);
    } else if (address < trk.expected_next) {
        trk.overlap_count++;
        record_overlap(trk, seq_id, time_us, capture_start_us, mode_name, address);
    } else if (address > trk.expected_next) {
        trk.gap_count++;
        record_gap(trk, seq_id, time_us, capture_start_us, mode_name, address);
    }
    trk.last_address = address;
    trk.last_length = length;
    trk.expected_next = address + length;
}

// ---------------------------------------------------------------------------
// format_tlv_detail
// ---------------------------------------------------------------------------

auto format_tlv_detail(PendingCommand const& cmd) -> std::string
{
    std::string detail;
    for (size_t i = 0; i < cmd.modes.size(); ++i) {
        if (i > 0) {
            detail += ", ";
        }
        detail += std::format("{} 0x{:016X}", aa_mode_name(cmd.modes[i]), cmd.addresses[i]);
    }
    return detail;
}

// ---------------------------------------------------------------------------
// process_aa_tlv
// ---------------------------------------------------------------------------

static void process_aa_tlv_read(
    ConversationStats& conv, uint64_t address, uint16_t tlv_len, uint16_t seq, uint64_t capture_us, uint64_t capture_start_us)
{
    conv.read_count++;
    update_address_tracker(conv.read_tracker, address, tlv_len, seq, capture_us, capture_start_us, "READ");
}

static void process_aa_tlv_write_duplicate_check(ConversationStats& conv, uint64_t address, std::span<uint8_t const> data)
{
    if (address == conv.last_write_address && !data.empty() && data.size() == conv.last_write_data.size() &&
        std::equal(data.begin(), data.end(), conv.last_write_data.begin())) {
        conv.duplicate_payloads++;
    }
    conv.last_write_address = address;
    conv.last_write_data.assign(data.begin(), data.end());
}

static void process_aa_tlv_write_upload_tracking(
    AnalysisState& state,
    Eui64 controller,
    Eui64 target,
    uint64_t address,
    std::span<uint8_t const> data,
    uint16_t tlv_len,
    uint16_t seq,
    uint64_t capture_us)
{
    for (auto& [skey, session] : state.upload_sessions) {
        if (skey.controller != controller || skey.target != target) {
            continue;
        }
        if (session.phase != UploadPhase::UploadAccepted && session.phase != UploadPhase::Transferring) {
            continue;
        }
        if (session.phase == UploadPhase::UploadAccepted) {
            session.phase = UploadPhase::Transferring;
            session.first_write_time_us = capture_us;
        }
        session.last_write_time_us = capture_us;
        session.write_count++;
        session.bytes_transferred += data.size();
        update_address_tracker(session.write_tracker, address, tlv_len, seq, capture_us, state.capture_start_us, "WRITE");
        break;
    }
}

static void process_aa_tlv_write(
    PendingCommand& cmd,
    ConversationStats& conv,
    AnalysisState& state,
    uint64_t address,
    std::span<uint8_t const> data,
    uint16_t tlv_len,
    uint16_t seq,
    uint64_t capture_us)
{
    conv.write_count++;
    update_address_tracker(conv.write_tracker, address, tlv_len, seq, capture_us, state.capture_start_us, "WRITE");
    process_aa_tlv_write_duplicate_check(conv, address, data);
    process_aa_tlv_write_upload_tracking(state, cmd.controller, cmd.target, address, data, tlv_len, seq, capture_us);
}

void process_aa_tlv(
    PendingCommand& cmd,
    ConversationStats& conv,
    AnalysisState& state,
    uint8_t mode,
    uint64_t address,
    std::span<uint8_t const> data,
    uint16_t seq,
    uint64_t capture_us)
{
    cmd.modes.push_back(mode);
    cmd.addresses.push_back(address);
    cmd.total_data_bytes += static_cast<uint32_t>(data.size());
    auto const tlv_len = static_cast<uint16_t>(data.size());

    switch (mode) {
        case AA_MODE_READ:
            process_aa_tlv_read(conv, address, tlv_len, seq, capture_us, state.capture_start_us);
            break;
        case AA_MODE_WRITE:
            process_aa_tlv_write(cmd, conv, state, address, data, tlv_len, seq, capture_us);
            break;
        case AA_MODE_EXECUTE:
            conv.execute_count++;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// process_aa_command
// ---------------------------------------------------------------------------

void process_aa_command(
    ConversationStats& conv, AnalysisState& state, AecpAaDu const& pdu, std::span<uint8_t const> payload, uint64_t capture_us)
{
    uint16_t const seq = pdu.common.sequence_id.get();
    PendingKey const pkey{.controller = pdu.common.controller_entity_id, .sequence_id = seq};

    // Check for retransmit
    if (auto it = state.pending.find(pkey); it != state.pending.end()) {
        it->second.retransmit_count++;
        it->second.send_time_us = capture_us;
        conv.retransmits++;
        return;
    }

    // New command
    PendingCommand cmd{};
    cmd.send_time_us = capture_us;
    cmd.sequence_id = seq;
    cmd.target = pdu.common.target_entity_id;
    cmd.controller = pdu.common.controller_entity_id;
    cmd.tlv_count = pdu.tlv_count.get();

    auto const tlv_payload = payload.subspan(AecpAaDu::LENGTH);
    (void)aa_parse_tlvs(
        pdu.tlv_count.get(), tlv_payload, [&](uint16_t, uint8_t mode, uint64_t address, std::span<uint8_t const> data) {
            process_aa_tlv(cmd, conv, state, mode, address, data, seq, capture_us);
        });

    state.pending[pkey] = std::move(cmd);
    conv.commands_sent++;
}

// ---------------------------------------------------------------------------
// process_aa_response
// ---------------------------------------------------------------------------

static void check_near_timeout(AnalysisState& state, PendingCommand const& cmd, double rtt, uint64_t capture_us)
{
    for (auto& [skey, session] : state.upload_sessions) {
        if (skey.controller == cmd.controller && skey.target == cmd.target) {
            session.log_issue(
                capture_us,
                state.capture_start_us,
                std::format("WARNING: AA WRITE response took {:.1f}ms (near 250ms timeout)", rtt / 1000.0));
        }
    }
}

static void record_error_in_uploads(AnalysisState& state, PendingCommand const& cmd, uint8_t status, uint64_t capture_us)
{
    for (auto& [skey, session] : state.upload_sessions) {
        if (skey.controller == cmd.controller && skey.target == cmd.target) {
            session.log_issue(
                capture_us, state.capture_start_us, std::format("ERROR: AA response status={}", aa_status_name(status)));
            session.phase = UploadPhase::Failed;
        }
    }
}

void process_aa_response(ConversationStats& conv, AnalysisState& state, AecpAaDu const& pdu, uint64_t capture_us)
{
    uint16_t const seq = pdu.common.sequence_id.get();
    PendingKey const pkey{.controller = pdu.common.controller_entity_id, .sequence_id = seq};

    auto it = state.pending.find(pkey);
    if (it == state.pending.end()) {
        return;
    }

    auto const rtt = static_cast<double>(capture_us - it->second.send_time_us);
    uint8_t const status = pdu.common.status();

    conv.responses_received++;
    conv.rtt_us.push_back(rtt);
    conv.status_counts[status]++;

    if (rtt > 240000.0) {
        check_near_timeout(state, it->second, rtt, capture_us);
    }

    if (status != AA_STATUS_SUCCESS) {
        std::string detail = format_tlv_detail(it->second);
        conv.errors.push_back(
            ErrorRecord{.time_us = capture_us, .sequence_id = seq, .status = status, .detail = std::move(detail)});
        record_error_in_uploads(state, it->second, status, capture_us);
    }

    if (it->second.retransmit_count > 0) {
        conv.retransmit_details.push_back(RetransmitRecord{
            .time_us = it->second.send_time_us,
            .sequence_id = seq,
            .retransmit_count = it->second.retransmit_count,
            .response_time_us = rtt});
    }

    state.pending.erase(it);
}

// ---------------------------------------------------------------------------
// process_aem_start_operation_command
// ---------------------------------------------------------------------------

static void handle_upload_command(
    AnalysisState& state, UploadSessionKey const& skey, AemOperationFields const& fields, uint64_t capture_us)
{
    auto& session = state.upload_sessions[skey];
    session.controller = fields.controller;
    session.target = fields.target;
    session.descriptor_index = fields.desc_index;
    session.phase = UploadPhase::UploadRequested;
    session.upload_start_time_us = capture_us;
    session.log_timeline(capture_us, state.capture_start_us, std::format("UPLOAD START_OPERATION command (seq={})", fields.seq));
}

static void handle_store_command(
    AnalysisState& state, UploadSessionKey const& skey, AemOperationFields const& fields, uint64_t capture_us)
{
    auto it = state.upload_sessions.find(skey);
    if (it == state.upload_sessions.end()) {
        return;
    }
    it->second.phase = UploadPhase::StoreRequested;
    it->second.store_request_time_us = capture_us;
    it->second.store_operation_type = fields.op_type_or_pct;
    it->second.log_timeline(
        capture_us,
        state.capture_start_us,
        std::format("{} START_OPERATION command (seq={})", operation_type::name(fields.op_type_or_pct), fields.seq));
}

void process_aem_start_operation_command(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us)
{
    UploadSessionKey const skey{.controller = fields.controller, .target = fields.target, .descriptor_index = fields.desc_index};

    if (fields.op_type_or_pct == operation_type::UPLOAD) {
        handle_upload_command(state, skey, fields, capture_us);
    } else if (fields.op_type_or_pct == operation_type::STORE || fields.op_type_or_pct == operation_type::STORE_AND_REBOOT) {
        handle_store_command(state, skey, fields, capture_us);
    }
}

// ---------------------------------------------------------------------------
// process_aem_start_operation_response
// ---------------------------------------------------------------------------

static void handle_upload_response_success(UploadSession& session, uint16_t op_id, uint64_t capture_us, uint64_t capture_start_us)
{
    session.phase = UploadPhase::UploadAccepted;
    session.upload_accepted_time_us = capture_us;
    session.upload_operation_id = op_id;
    session.log_timeline(
        capture_us, capture_start_us, std::format("UPLOAD START_OPERATION response SUCCESS (operation_id={})", op_id));
}

static void handle_upload_response_failure(UploadSession& session, uint8_t status, uint64_t capture_us, uint64_t capture_start_us)
{
    session.phase = UploadPhase::Failed;
    session.log_timeline(
        capture_us, capture_start_us, std::format("UPLOAD START_OPERATION response FAILED status={}", aem_status_name(status)));
    session.log_issue(capture_us, capture_start_us, std::format("ERROR: UPLOAD rejected with status={}", aem_status_name(status)));
}

static void handle_store_response_success(
    UploadSession& session, uint16_t op_id, uint16_t op_type, uint64_t capture_us, uint64_t capture_start_us)
{
    session.phase = UploadPhase::Storing;
    session.store_operation_id = op_id;
    session.log_timeline(
        capture_us,
        capture_start_us,
        std::format("{} START_OPERATION response SUCCESS (operation_id={})", operation_type::name(op_type), op_id));
}

static void handle_store_response_failure(
    UploadSession& session, uint16_t op_type, uint8_t status, uint64_t capture_us, uint64_t capture_start_us)
{
    session.phase = UploadPhase::Failed;
    session.log_timeline(
        capture_us,
        capture_start_us,
        std::format("{} START_OPERATION response FAILED status={}", operation_type::name(op_type), aem_status_name(status)));
    session.log_issue(
        capture_us,
        capture_start_us,
        std::format("ERROR: {} rejected with status={}", operation_type::name(op_type), aem_status_name(status)));
}

void process_aem_start_operation_response(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us)
{
    UploadSessionKey const skey{.controller = fields.controller, .target = fields.target, .descriptor_index = fields.desc_index};
    auto it = state.upload_sessions.find(skey);
    if (it == state.upload_sessions.end()) {
        return;
    }

    if (fields.op_type_or_pct == operation_type::UPLOAD) {
        if (fields.status == AEM_STATUS_SUCCESS) {
            handle_upload_response_success(it->second, fields.op_id, capture_us, state.capture_start_us);
        } else {
            handle_upload_response_failure(it->second, fields.status, capture_us, state.capture_start_us);
        }
    } else if (fields.op_type_or_pct == operation_type::STORE || fields.op_type_or_pct == operation_type::STORE_AND_REBOOT) {
        if (fields.status == AEM_STATUS_SUCCESS) {
            handle_store_response_success(it->second, fields.op_id, fields.op_type_or_pct, capture_us, state.capture_start_us);
        } else {
            handle_store_response_failure(it->second, fields.op_type_or_pct, fields.status, capture_us, state.capture_start_us);
        }
    }
}

// ---------------------------------------------------------------------------
// process_aem_abort_operation
// ---------------------------------------------------------------------------

void process_aem_abort_operation(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us)
{
    UploadSessionKey const skey{.controller = fields.controller, .target = fields.target, .descriptor_index = fields.desc_index};
    auto it = state.upload_sessions.find(skey);
    if (it == state.upload_sessions.end()) {
        return;
    }
    it->second.phase = UploadPhase::Aborted;
    it->second.log_timeline(
        capture_us,
        state.capture_start_us,
        std::format("ABORT_OPERATION {} (operation_id={})", fields.is_command ? "command" : "response", fields.op_id));
    it->second.log_issue(capture_us, state.capture_start_us, std::format("ABORT_OPERATION (operation_id={})", fields.op_id));
}

// ---------------------------------------------------------------------------
// process_aem_operation_status
// ---------------------------------------------------------------------------

void process_aem_operation_status(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us)
{
    double const pct = static_cast<double>(fields.op_type_or_pct) / 10.0;

    for (auto& [sk, session] : state.upload_sessions) {
        if (session.store_operation_id != fields.op_id && session.upload_operation_id != fields.op_id) {
            continue;
        }
        session.status_update_count++;
        session.last_percent_complete = fields.op_type_or_pct;
        session.log_timeline(
            capture_us, state.capture_start_us, std::format("OPERATION_STATUS {:.1f}% (operation_id={})", pct, fields.op_id));

        if (fields.op_type_or_pct >= 1000) {
            session.store_complete_time_us = capture_us;
            session.phase = UploadPhase::Complete;
            session.log_timeline(capture_us, state.capture_start_us, "Upload COMPLETE");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// finalize_pending
// ---------------------------------------------------------------------------

void finalize_pending(AnalysisState& state)
{
    for (auto& [key, cmd] : state.pending) {
        ConversationKey const ck{.controller = cmd.controller, .target = cmd.target};
        state.conversations[ck].unanswered.push_back(std::move(cmd));
    }
}

}  // namespace statusbar::atdecc
