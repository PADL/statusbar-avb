// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AECP Address Access & Memory Object Upload Analyzer Tool
///
/// Reads a pcap/pcapng capture and analyzes AECP Address Access conversations
/// and Memory Object Upload sessions (Annex D). Reports response times, errors,
/// retransmits, upload state machine tracking, and transfer integrity.

#include "statusbar/atdecc/atdecc_aecp_aa_analysis.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/pcap/pcap.hpp"
#include "statusbar/stats/stats.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <print>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::ieee;
using namespace statusbar::pcap;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct AnalyzerConfig
{
    std::string input_file;
    Eui64 controller_filter{};
    Eui64 target_filter{};
    bool has_controller_filter{false};
    bool has_target_filter{false};
    bool verbose{false};
};

static auto build_arg_specs(AnalyzerConfig& cfg) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    specs.add_file("input", "Input pcap/pcapng file", "", [&](auto v) { cfg.input_file = std::string{v}; });
    specs.add<std::string>("controller", "Filter by controller entity ID", "", [&](std::string v) {
        if (!v.empty()) {
            if (auto parsed = eui64_from_string(v); parsed.has_value()) {
                cfg.controller_filter = *parsed;
                cfg.has_controller_filter = true;
            }
        }
    });
    specs.add<std::string>("target", "Filter by target entity ID", "", [&](std::string v) {
        if (!v.empty()) {
            if (auto parsed = eui64_from_string(v); parsed.has_value()) {
                cfg.target_filter = *parsed;
                cfg.has_target_filter = true;
            }
        }
    });
    specs.add_flag("verbose", "Print per-packet details", [&](auto v) { cfg.verbose = v; });
    return specs;
}

// ---------------------------------------------------------------------------
// Report formatting (tool-specific)
// ---------------------------------------------------------------------------

// Use ieee::to_string() directly for Eui64/Eui48 formatting

[[nodiscard]] static auto is_contiguous_transfer(AddressTracker const& trk) -> bool
{
    return trk.gap_count == 0 && trk.backward_count == 0 && trk.overlap_count == 0;
}

static void print_address_tracker(char const* mode, AddressTracker const& trk)
{
    if (!trk.has_first) {
        return;
    }
    std::println("\n  {} address transfer:", mode);
    std::println("    Address range:       0x{:X}..0x{:X}", trk.first_address, trk.expected_next);
    std::println("    Total bytes:         {}", trk.total_bytes);
    std::println("    Gaps:                {}", trk.gap_count);
    std::println("    Backward moves:      {}", trk.backward_count);
    std::println("    Overlaps:            {}", trk.overlap_count);
    if (is_contiguous_transfer(trk)) {
        std::println("    Transfer:            CONTIGUOUS (no gaps or backward moves)");
    }
    for (auto const& issue : trk.issues) {
        std::println("      {}", issue);
    }
}

static void print_conversation_report(ConversationStats const& conv, uint64_t capture_start_us)
{
    std::println(
        "\n--- AA Conversation: controller={} -> target={} ---\n",
        ieee::to_string(conv.controller).view(),
        ieee::to_string(conv.target).view());

    std::println("  Summary:");
    std::println("    Commands sent:       {}", conv.commands_sent);
    std::println("    Responses received:  {}", conv.responses_received);
    std::println("    Unanswered:          {}", conv.unanswered.size());
    std::println("    Retransmits:         {}", conv.retransmits);
    std::println("    Duplicate payloads:  {}", conv.duplicate_payloads);

    if (!conv.status_counts.empty()) {
        std::println("\n  Status breakdown:");
        for (auto const& [status, count] : conv.status_counts) {
            std::println("    {:<24} {}", aa_status_name(status), count);
        }
    }

    std::println("\n  TLV mode breakdown:");
    std::println("    READ:                {}", conv.read_count);
    std::println("    WRITE:               {}", conv.write_count);
    std::println("    EXECUTE:             {}", conv.execute_count);

    print_address_tracker("READ", conv.read_tracker);
    print_address_tracker("WRITE", conv.write_tracker);

    if (!conv.rtt_us.empty()) {
        std::vector<double> sorted(conv.rtt_us.begin(), conv.rtt_us.end());
        auto ds = stats::analyze(std::move(sorted));
        std::println("\n  Response time (microseconds):");
        std::println("    Min:       {:>10.0f}", ds.min);
        std::println("    Max:       {:>10.0f}", ds.max);
        std::println("    Mean:      {:>10.0f}", ds.mean);
        std::println("    Median:    {:>10.0f}", ds.median);
        std::println("    P95:       {:>10.0f}", ds.p95);
        std::println("    P99:       {:>10.0f}", ds.p99);
        std::println("    Std Dev:   {:>10.0f}", ds.stddev);
        std::vector<double> rtt_copy(conv.rtt_us.begin(), conv.rtt_us.end());
        auto hist = stats::histogram(rtt_copy, stats::log_scale_us_buckets());
        if (hist.total_count > 0) {
            std::string bar_chart;
            bar_chart.reserve(512);
            stats::format_histogram_to(std::back_inserter(bar_chart), hist);
            std::println("\n  Response time histogram:");
            std::print("{}", bar_chart);
        }
    }

    if (!conv.retransmit_details.empty()) {
        std::println("\n  Retransmit details:");
        for (auto const& r : conv.retransmit_details) {
            double const t = static_cast<double>(r.time_us - capture_start_us) / 1e6;
            std::println(
                "    seq={} at {:.3f}s (retransmitted {}x, response after {:.1f}us)",
                r.sequence_id,
                t,
                r.retransmit_count,
                r.response_time_us);
        }
    }

    if (!conv.errors.empty()) {
        std::println("\n  Errors:");
        for (auto const& e : conv.errors) {
            double const t = static_cast<double>(e.time_us - capture_start_us) / 1e6;
            std::println("    seq={} at {:.3f}s {} {}", e.sequence_id, t, aa_status_name(e.status), e.detail);
        }
    }

    if (!conv.unanswered.empty()) {
        std::println("\n  Unanswered commands:");
        for (auto const& u : conv.unanswered) {
            double const t = static_cast<double>(u.send_time_us - capture_start_us) / 1e6;
            std::println("    seq={} at {:.3f}s ({})", u.sequence_id, t, format_tlv_detail(u));
        }
    }
}

static void print_upload_session_report(UploadSession const& session, size_t index)
{
    std::println(
        "\n--- Upload Session #{}: controller={} -> target={}, descriptor_index={} ---\n",
        index + 1,
        ieee::to_string(session.controller).view(),
        ieee::to_string(session.target).view(),
        session.descriptor_index);

    std::println("  Phase:               {}", upload_phase_name(session.phase));

    if (!session.timeline.empty()) {
        std::println("\n  Phase timeline:");
        for (auto const& entry : session.timeline) {
            std::println("    {}", entry);
        }
    }

    if (session.phase >= UploadPhase::Transferring) {
        std::println("\n  Upload summary:");
        std::println("    Upload operation_id: {}", session.upload_operation_id);
        if (session.write_count > 0) {
            std::println("    Bytes transferred:   {}", session.bytes_transferred);
            std::println("    Write commands:      {}", session.write_count);
            if (session.last_write_time_us > session.first_write_time_us) {
                double const xfer = static_cast<double>(session.last_write_time_us - session.first_write_time_us) / 1e6;
                std::println("    Transfer time:       {:.3f}s", xfer);
                std::println(
                    "    Transfer rate:       {:.1f} KiB/s", static_cast<double>(session.bytes_transferred) / 1024.0 / xfer);
            }
        }
        if (session.store_operation_id != 0) {
            std::println("    Store operation_id:  {}", session.store_operation_id);
            std::println("    Store type:          {}", operation_type::name(session.store_operation_type));
        }
        if (session.status_update_count > 0) {
            std::println("    Status updates:      {}", session.status_update_count);
            std::println("    Last progress:       {:.1f}%", static_cast<double>(session.last_percent_complete) / 10.0);
        }
        if (session.store_complete_time_us > 0 && session.store_request_time_us > 0) {
            double const s = static_cast<double>(session.store_complete_time_us - session.store_request_time_us) / 1e6;
            std::println("    Store time:          {:.3f}s", s);
        }
        if (session.store_complete_time_us > 0 && session.upload_start_time_us > 0) {
            double const s = static_cast<double>(session.store_complete_time_us - session.upload_start_time_us) / 1e6;
            std::println("    Total time:          {:.3f}s", s);
        }
    }

    print_address_tracker("WRITE", session.write_tracker);

    if (session.issues.empty()) {
        std::println("\n  Issues: NONE");
    } else {
        std::println("\n  Issues:");
        for (auto const& issue : session.issues) {
            std::println("    {}", issue);
        }
    }
}

// ---------------------------------------------------------------------------
// Packet dispatch helpers
// ---------------------------------------------------------------------------

static void handle_aa_packet(AnalysisState& state, std::span<uint8_t const> payload, uint64_t capture_us, bool verbose)
{
    AecpAaDu pdu{};
    span_load(pdu, payload.subspan(0, AecpAaDu::LENGTH));

    ConversationKey const ck{pdu.common.controller_entity_id, pdu.common.target_entity_id};
    auto& conv = state.conversations[ck];
    conv.controller = pdu.common.controller_entity_id;
    conv.target = pdu.common.target_entity_id;

    if (pdu.common.is_command()) {
        process_aa_command(conv, state, pdu, payload, capture_us);
    } else {
        process_aa_response(conv, state, pdu, capture_us);
    }

    if (verbose) {
        uint16_t const seq = pdu.common.sequence_id.get();
        if (pdu.common.is_command()) {
            std::println("  [{:.6f}] AA CMD seq={} tlvs={}", static_cast<double>(capture_us) / 1e6, seq, pdu.tlv_count.get());
        } else {
            std::println(
                "  [{:.6f}] AA RSP seq={} status={}",
                static_cast<double>(capture_us) / 1e6,
                seq,
                aa_status_name(pdu.common.status()));
        }
    }
}

/// Returns true if an AEM memory-object operation packet was processed.
static auto handle_aem_packet(AnalysisState& state, std::span<uint8_t const> payload, uint64_t capture_us, bool verbose) -> bool
{
    AemDu aem{};
    span_load(aem, payload.subspan(0, AemDu::LENGTH));

    constexpr size_t OP_PAYLOAD_LEN = 8;
    if (payload.size() < AemDu::LENGTH + OP_PAYLOAD_LEN) {
        return false;
    }

    aem::AemStartOperationCommandPayload op_payload{};
    span_load(op_payload, payload.subspan(AemDu::LENGTH, OP_PAYLOAD_LEN));
    if (op_payload.descriptor_type.get() != DESCRIPTOR_MEMORY_OBJECT) {
        return false;
    }

    uint16_t const desc_index = op_payload.descriptor_index.get();
    uint16_t const op_id = op_payload.operation_id.get();
    uint16_t const op_field = op_payload.operation_type.get();
    uint16_t const cmd_code = aem.command_code();
    uint16_t const seq = aem.sequence_id.get();

    AemOperationFields fields{};
    fields.controller = aem.controller_entity_id;
    fields.target = aem.target_entity_id;
    fields.desc_index = desc_index;
    fields.seq = seq;
    fields.op_id = op_id;
    fields.op_type_or_pct = op_field;
    fields.status = aem.status();
    fields.is_command = aem.is_command();

    if (cmd_code == AEM_COMMAND_START_OPERATION && aem.is_command()) {
        process_aem_start_operation_command(state, fields, capture_us);
    } else if (cmd_code == AEM_COMMAND_START_OPERATION && aem.is_response()) {
        process_aem_start_operation_response(state, fields, capture_us);
    } else if (cmd_code == AEM_COMMAND_ABORT_OPERATION) {
        process_aem_abort_operation(state, fields, capture_us);
    } else if (cmd_code == AEM_COMMAND_OPERATION_STATUS && aem.is_unsolicited()) {
        process_aem_operation_status(state, fields, capture_us);
    }

    if (verbose) {
        std::println("  [{:.6f}] AEM {} seq={}", static_cast<double>(capture_us) / 1e6, aem_command_name(cmd_code), seq);
    }
    return true;
}

static void print_report(
    std::string_view input_file,
    AnalysisState const& state,
    uint32_t total_packets,
    uint32_t aa_packets,
    uint32_t aem_packets,
    uint64_t last_time_us)
{
    double const duration_s = static_cast<double>(last_time_us - state.capture_start_us) / 1e6;
    std::println("=== AECP Address Access & Upload Analysis ===\n");
    std::println(
        "Capture: {} ({} packets, {:.3f}s duration, {} AA packets, {} AEM operation packets)",
        input_file,
        total_packets,
        duration_s,
        aa_packets,
        aem_packets);

    size_t idx = 0;
    for (auto const& [key, session] : state.upload_sessions) {
        print_upload_session_report(session, idx++);
    }
    for (auto const& [key, conv] : state.conversations) {
        print_conversation_report(conv, state.capture_start_us);
    }
    if (state.conversations.empty() && state.upload_sessions.empty()) {
        std::println("\nNo Address Access or Memory Object Upload packets found.");
    }
    std::println("");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

static void print_usage(char const* program_name, args::ArgumentSpecs const& specs)
{
    config::default_print_usage(
        program_name,
        specs,
        "AECP Address Access & Memory Object Upload Analyzer. "
        "Analyzes AA conversations and firmware upload sessions in pcap captures.");
}

auto main(int argc, char* argv[]) -> int
{
    AnalyzerConfig cfg;
    auto specs = build_arg_specs(cfg);
    auto result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-aecp-aa-analyzer");
    if (!result) {
        return config::handled_builtin_command(result) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (cfg.input_file.empty()) {
        std::println(stderr, "Error: --input is required");
        return EXIT_FAILURE;
    }

    auto reader_result = PcapngReader::open(cfg.input_file);
    if (!reader_result) {
        std::println(stderr, "Error: open pcap '{}': {}", cfg.input_file, reader_result.error().message());
        return EXIT_FAILURE;
    }
    auto& reader = *reader_result;

    AnalysisState state;
    uint32_t total_packets = 0;
    uint32_t aa_packets = 0;
    uint32_t aem_packets = 0;
    uint64_t last_time_us = 0;

    auto walk_status = for_each_packet(
        reader, [&](uint64_t capture_us, Eui48 const& da, Eui48 const& sa, uint16_t ethertype, Packet const& payload) {
            (void)da;
            (void)sa;
            ++total_packets;
            if (state.capture_start_us == 0) {
                state.capture_start_us = capture_us;
            }
            last_time_us = capture_us;

            if (ethertype != protocols::ETHERTYPE_AVTP) {
                return;
            }
            if (payload.empty() || payload[0] != avtp::AvtpSubtype::aecp) {
                return;
            }
            if (payload.size() < AecpDuCommon::LENGTH) {
                return;
            }

            AecpDuCommon common{};
            span_load(common, make_const_span(payload).first<AecpDuCommon::LENGTH>());

            if (cfg.has_controller_filter && common.controller_entity_id != cfg.controller_filter) {
                return;
            }
            if (cfg.has_target_filter && common.target_entity_id != cfg.target_filter) {
                return;
            }

            if (common.is_address_access() && payload.size() >= AecpAaDu::LENGTH) {
                ++aa_packets;
                handle_aa_packet(state, payload, capture_us, cfg.verbose);
            } else if (common.is_aem() && payload.size() >= AemDu::LENGTH) {
                if (handle_aem_packet(state, payload, capture_us, cfg.verbose)) {
                    ++aem_packets;
                }
            }
        });
    if (!walk_status) {
        std::println(stderr, "Error: pcap read failed: {}", walk_status.error().message());
        return EXIT_FAILURE;
    }

    finalize_pending(state);
    print_report(cfg.input_file, state, total_packets, aa_packets, aem_packets, last_time_us);
    return EXIT_SUCCESS;
}
