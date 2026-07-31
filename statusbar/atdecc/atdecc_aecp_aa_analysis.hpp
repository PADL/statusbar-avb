#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AECP Address Access Analysis Library
///
/// Reusable types and processing functions for analyzing AECP Address Access
/// conversations and Memory Object Upload sessions (Annex D).
/// Extracted from the analyzer tool for use by other analysis consumers.

#include "statusbar/atdecc/atdecc_aecp_aa.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"

#include <cstdint>
#include <map>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::atdecc {

using ieee::Eui64;

// ---------------------------------------------------------------------------
// Pending command tracking
// ---------------------------------------------------------------------------

/// Tracks an in-flight AA command awaiting its response.
struct PendingCommand
{
    uint64_t send_time_us{0};
    uint16_t sequence_id{0};
    Eui64 target{};
    Eui64 controller{};
    uint16_t tlv_count{0};
    uint32_t total_data_bytes{0};
    uint32_t retransmit_count{0};
    std::pmr::vector<uint8_t> modes;
    std::pmr::vector<uint64_t> addresses;
};

/// Record of an error response.
struct ErrorRecord
{
    uint64_t time_us{0};
    uint16_t sequence_id{0};
    uint8_t status{0};
    std::string detail;
};

/// Record of a retransmitted command that eventually received a response.
struct RetransmitRecord
{
    uint64_t time_us{0};
    uint16_t sequence_id{0};
    uint32_t retransmit_count{0};
    double response_time_us{0};
};

/// Map key for pending commands: (controller, sequence_id).
struct PendingKey
{
    Eui64 controller;
    uint16_t sequence_id;
    auto operator<=>(PendingKey const&) const = default;
};

// ---------------------------------------------------------------------------
// Address tracker
// ---------------------------------------------------------------------------

/// Tracks sequential address access patterns and detects gaps, overlaps,
/// and backward moves.
struct AddressTracker
{
    bool has_first{false};
    uint64_t first_address{0};
    uint64_t last_address{0};
    uint16_t last_length{0};
    uint64_t expected_next{0};
    uint32_t backward_count{0};
    uint32_t gap_count{0};
    uint32_t overlap_count{0};
    uint64_t total_bytes{0};
    std::pmr::vector<std::string> issues;
};

// ---------------------------------------------------------------------------
// Conversation statistics
// ---------------------------------------------------------------------------

/// Per-conversation (controller->target) statistics for AA traffic.
struct ConversationStats
{
    Eui64 controller{};
    Eui64 target{};

    uint32_t commands_sent{0};
    uint32_t responses_received{0};
    uint32_t retransmits{0};
    uint32_t duplicate_payloads{0};

    std::map<uint8_t, uint32_t> status_counts;

    uint32_t read_count{0};
    uint32_t write_count{0};
    uint32_t execute_count{0};

    std::pmr::vector<double> rtt_us;

    std::pmr::vector<ErrorRecord> errors;
    std::pmr::vector<RetransmitRecord> retransmit_details;
    std::pmr::vector<PendingCommand> unanswered;

    uint64_t last_write_address{0};
    std::pmr::vector<uint8_t> last_write_data;

    AddressTracker read_tracker;
    AddressTracker write_tracker;
};

/// Map key for conversations: (controller, target).
struct ConversationKey
{
    Eui64 controller;
    Eui64 target;
    auto operator<=>(ConversationKey const&) const = default;
};

// ---------------------------------------------------------------------------
// Upload session state machine (Annex D)
// ---------------------------------------------------------------------------

/// Phase of a Memory Object upload session.
enum class UploadPhase : uint8_t
{
    Idle,
    UploadRequested,
    UploadAccepted,
    Transferring,
    StoreRequested,
    Storing,
    Complete,
    Aborted,
    Failed,
};

/// Tracks the full lifecycle of a Memory Object upload session.
struct UploadSession
{
    Eui64 controller{};
    Eui64 target{};
    uint16_t descriptor_index{0};

    UploadPhase phase{UploadPhase::Idle};

    uint16_t upload_operation_id{0};
    uint16_t store_operation_id{0};
    uint16_t store_operation_type{0};

    uint64_t upload_start_time_us{0};
    uint64_t upload_accepted_time_us{0};
    uint64_t first_write_time_us{0};
    uint64_t last_write_time_us{0};
    uint64_t store_request_time_us{0};
    uint64_t store_complete_time_us{0};

    uint32_t segment_number{0};
    uint64_t bytes_transferred{0};
    uint32_t write_count{0};

    AddressTracker write_tracker;

    uint16_t last_percent_complete{0};
    uint32_t status_update_count{0};

    std::pmr::vector<std::string> timeline;
    std::pmr::vector<std::string> issues;

    /// Append a timestamped entry to the phase timeline.
    void log_timeline(uint64_t time_us, uint64_t capture_start_us, std::string msg);

    /// Append a timestamped issue.
    void log_issue(uint64_t time_us, uint64_t capture_start_us, std::string msg);
};

/// Map key for upload sessions: (controller, target, descriptor_index).
struct UploadSessionKey
{
    Eui64 controller;
    Eui64 target;
    uint16_t descriptor_index;
    auto operator<=>(UploadSessionKey const&) const = default;
};

// ---------------------------------------------------------------------------
// Aggregate analysis state
// ---------------------------------------------------------------------------

/// Aggregate state for AA/upload analysis. Passed by reference to all processing functions.
struct AnalysisState
{
    std::map<ConversationKey, ConversationStats> conversations;
    std::map<PendingKey, PendingCommand> pending;
    std::map<UploadSessionKey, UploadSession> upload_sessions;
    uint64_t capture_start_us{0};
};

// ---------------------------------------------------------------------------
// Parsed AEM operation fields
// ---------------------------------------------------------------------------

/// Parsed fields from an AEM START_OPERATION / ABORT_OPERATION / OPERATION_STATUS payload.
struct AemOperationFields
{
    Eui64 controller{};
    Eui64 target{};
    uint16_t desc_index{0};
    uint16_t seq{0};
    uint16_t op_id{0};
    uint16_t op_type_or_pct{0};
    uint8_t status{0};
    bool is_command{false};
};

// ---------------------------------------------------------------------------
// Function declarations
// ---------------------------------------------------------------------------

/// Get human-readable name for an upload phase.
auto upload_phase_name(UploadPhase p) -> std::string_view;

/// Update an address tracker with a new access, detecting gaps/overlaps/backward moves.
void update_address_tracker(
    AddressTracker& trk,
    uint64_t address,
    uint16_t length,
    uint16_t seq_id,
    uint64_t time_us,
    uint64_t capture_start_us,
    std::string_view mode_name);

/// Build a human-readable string from a PendingCommand's modes and addresses.
/// Example output: "READ 0x0000000000001000, WRITE 0x0000000000002000"
auto format_tlv_detail(PendingCommand const& cmd) -> std::string;

/// Process a single AA TLV from a command packet.
/// Updates the PendingCommand being built, conversation statistics,
/// and any active upload session write tracking.
void process_aa_tlv(
    PendingCommand& cmd,
    ConversationStats& conv,
    AnalysisState& state,
    uint8_t mode,
    uint64_t address,
    std::span<uint8_t const> data,
    uint16_t seq,
    uint64_t capture_us);

/// Process a complete AA command packet.
/// Handles retransmit detection, TLV parsing, and pending command tracking.
void process_aa_command(
    ConversationStats& conv, AnalysisState& state, AecpAaDu const& pdu, std::span<uint8_t const> payload, uint64_t capture_us);

/// Process a complete AA response packet.
/// Matches to pending command, calculates RTT, records errors and retransmit details.
void process_aa_response(ConversationStats& conv, AnalysisState& state, AecpAaDu const& pdu, uint64_t capture_us);

/// Process an AEM START_OPERATION command for a Memory Object.
/// Creates or updates upload sessions for UPLOAD, STORE, and STORE_AND_REBOOT operations.
void process_aem_start_operation_command(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us);

/// Process an AEM START_OPERATION response for a Memory Object.
/// Transitions upload sessions on success or failure.
void process_aem_start_operation_response(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us);

/// Process an AEM ABORT_OPERATION command or response for a Memory Object.
void process_aem_abort_operation(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us);

/// Process an AEM OPERATION_STATUS unsolicited response.
/// Updates progress and marks sessions complete at 100%.
void process_aem_operation_status(AnalysisState& state, AemOperationFields const& fields, uint64_t capture_us);

/// Move remaining pending commands into their conversations' unanswered lists.
void finalize_pending(AnalysisState& state);

}  // namespace statusbar::atdecc
