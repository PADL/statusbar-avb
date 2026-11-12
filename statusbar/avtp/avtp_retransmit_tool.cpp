// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVTP Stream Retransmit Tool
///
/// Reads a pcap/pcapng capture containing AVTP streams, parses each packet
/// through the appropriate stream input context, and re-serializes through
/// an output context into a new pcap file. Validates the full round-trip:
/// wire → deserialize → serialize → wire.
///
/// gPTP clock recovery from Sync/Follow_Up messages provides the approximate
/// gPTP time for each captured packet.

#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_common_header.hpp"
#include "statusbar/avtp/avtp_crf_stream_input.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ieee/ieee_ethernet_format.hpp"
#include "statusbar/pcap/pcap.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/tsn/tsn_stream_id_format.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <print>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::gptp;
using namespace statusbar::ieee;
using namespace statusbar::ieee::protocols;
using namespace statusbar::pcap;
using namespace statusbar::tsn;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

struct RetransmitConfig
{
    std::string input_file;
    std::string output_file;
    std::string format{"am824"};
    StreamId stream_id{};
    Eui48 src_mac{};
    Eui48 dst_mac{Eui48{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00}};
    StreamId new_stream_id{};
    uint64_t presentation_offset{2'000'000};
    uint16_t channels{2};
    uint32_t sample_rate{48000};
    uint8_t bit_depth{24};
    std::string aaf_format{"int24"};
    bool verbose{false};
};

auto build_arg_specs(RetransmitConfig& cfg) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;

    specs.add_file("input", "Input pcap/pcapng file", "", [&](auto v) { cfg.input_file = std::string{v}; });
    specs.add_file("output", "Output pcap file", "retransmit.pcap", [&](auto v) { cfg.output_file = std::string{v}; });

    specs.add_choice("format", "Stream format", {"am824", "aaf", "crf"}, "am824", [&](auto v) { cfg.format = std::string{v}; });

    specs.add<std::string_view>("stream-id", "Input stream ID to match (e.g., 00:11:22:33:44:55:0001)", "", [&](auto v) {
        auto r = stream_id_from_string(v);
        if (!r) {
            std::println(stderr, "Warning: invalid stream-id '{}', using default", v);
        }
        cfg.stream_id = r.value_or(StreamId{});
    });

    specs.add<std::string_view>("src-mac", "Source MAC for output packets", "AA:BB:CC:DD:EE:01", [&](auto v) {
        auto r = eui48_from_string(v);
        if (!r) {
            std::println(stderr, "Warning: invalid src-mac '{}', using default", v);
        }
        cfg.src_mac = r.value_or(Eui48{});
    });

    specs.add<std::string_view>("dst-mac", "Destination MAC for output packets", "91:E0:F0:00:FE:00", [&](auto v) {
        auto r = eui48_from_string(v);
        if (!r) {
            std::println(stderr, "Warning: invalid dst-mac '{}', using default", v);
        }
        cfg.dst_mac = r.value_or(Eui48{});
    });

    specs.add<std::string_view>("new-stream-id", "Stream ID for output packets", "", [&](auto v) {
        auto r = stream_id_from_string(v);
        if (!r) {
            std::println(stderr, "Warning: invalid new-stream-id '{}', using default", v);
        }
        cfg.new_stream_id = r.value_or(StreamId{});
    });

    specs.add<uint64_t>(
        "presentation-offset", "Presentation time offset in nanoseconds", 2'000'000, [&](auto v) { cfg.presentation_offset = v; });

    specs.add<uint16_t>("channels", "Number of audio channels", 2, [&](auto v) { cfg.channels = v; });
    specs.add<uint32_t>("sample-rate", "Sample rate in Hz", 48000, [&](auto v) { cfg.sample_rate = v; });
    specs.add<uint8_t>("bit-depth", "Audio bit depth", 24, [&](auto v) { cfg.bit_depth = v; });

    specs.add_choice("aaf-format", "AAF sample format", {"int16", "int24", "int32", "float32"}, "int24", [&](auto v) {
        cfg.aaf_format = std::string{v};
    });

    specs.add_flag("verbose", "Print per-packet statistics", [&](auto v) { cfg.verbose = v; });

    return specs;
}

// ---------------------------------------------------------------------------
// gPTP clock recovery
// ---------------------------------------------------------------------------

struct GptpClockState
{
    bool has_sync{false};
    uint64_t last_sync_capture_us{0};
    uint16_t last_sync_seq{0};

    bool has_offset{false};
    int64_t offset_ns{0};

    uint32_t sync_count{0};

    void process(uint64_t capture_time_us, std::span<uint8_t const> payload)
    {
        if (payload.size() < MessageHeader::LENGTH) {
            return;
        }

        MessageHeader hdr{};
        span_load(hdr, payload);

        if (hdr.is_sync()) {
            last_sync_capture_us = capture_time_us;
            last_sync_seq = hdr.sequence_id;
            has_sync = true;
            ++sync_count;
        } else if (hdr.is_follow_up() && has_sync) {
            if (hdr.sequence_id == last_sync_seq && payload.size() >= FollowUpMessage::LENGTH) {
                FollowUpMessage fup{};
                span_load(fup, payload);
                uint64_t const gptp_ns =
                    (fup.precise_origin_timestamp.seconds() * 1'000'000'000ULL) + fup.precise_origin_timestamp.nanos();
                int64_t const capture_ns = static_cast<int64_t>(last_sync_capture_us) * 1000;
                offset_ns = static_cast<int64_t>(gptp_ns) - capture_ns;
                has_offset = true;
            }
        }
    }

    [[nodiscard]] auto to_gptp_ns(uint64_t capture_time_us) const noexcept -> uint64_t
    {
        if (!has_offset) {
            return capture_time_us * 1000;
        }
        return static_cast<uint64_t>((static_cast<int64_t>(capture_time_us) * 1000) + offset_ns);
    }
};

// ---------------------------------------------------------------------------
// Common pcap loop — reads packets, recovers gPTP clock, dispatches AVTP
// ---------------------------------------------------------------------------

struct PcapLoopStats
{
    uint32_t total{0};
    uint32_t matched{0};
    uint32_t gptp_syncs{0};
};

/// Common pcap reading loop. Parses Ethernet headers, processes gPTP for
/// clock recovery, and calls on_avtp for each AVTP packet matching
/// expected_subtype and stream_id.
///
/// on_avtp receives (payload, gptp_now_ns, capture_us, writer).
template <typename OnAvtp>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) - on_avtp is called, not forwarded
auto run_pcap_loop(RetransmitConfig const& cfg, uint8_t expected_subtype, size_t min_pdu_len, OnAvtp&& on_avtp) -> PcapLoopStats
{
    auto reader_result = PcapngReader::open(cfg.input_file);
    if (!reader_result) {
        std::print(stderr, "error: open pcap '{}': {}\n", cfg.input_file, reader_result.error().message());
        return {};
    }
    auto& reader = *reader_result;

    auto writer_result = FileWriter::open(cfg.output_file);
    if (!writer_result) {
        std::print(stderr, "error: create pcap '{}': {}\n", cfg.output_file, writer_result.error().message());
        return {};
    }
    auto& writer = *writer_result;

    GptpClockState gptp;
    PcapLoopStats stats;

    auto const walk = pcap::for_each_packet(
        reader, [&](uint64_t capture_us, Eui48 const& /*da*/, Eui48 const& /*sa*/, uint16_t ethertype, Packet const& payload) {
            ++stats.total;

            if (ethertype == ETHERTYPE_GPTP) {
                gptp.process(capture_us, std::span<uint8_t const>{payload});
                return;
            }
            if (ethertype != ETHERTYPE_AVTP || payload.size() < min_pdu_len) {
                return;
            }
            if (payload[0] != expected_subtype) {
                return;
            }
            if (!avtp_match_stream_id(std::span<uint8_t const>{payload}, cfg.stream_id)) {
                return;
            }

            ++stats.matched;
            uint64_t const gptp_now = gptp.to_gptp_ns(capture_us);
            on_avtp(std::span<uint8_t const>{payload}, gptp_now, capture_us, writer);
        });
    if (!walk) {
        std::print(stderr, "error: pcap read: {}\n", walk.error().message());
    }

    stats.gptp_syncs = gptp.sync_count;
    return stats;
}

/// Write a retransmitted AVTP frame to the pcap writer.
inline void write_avtp_frame(FileWriter& writer, uint64_t capture_us, RetransmitConfig const& cfg, std::span<uint8_t const> frame)
{
    (void)writer.write_packet(capture_us, cfg.dst_mac, cfg.src_mac, ETHERTYPE_AVTP, frame);
}

// ---------------------------------------------------------------------------
// AM824 retransmit
// ---------------------------------------------------------------------------

struct Am824RetransmitState
{
    Am824StreamInputContext in_ctx;
    Am824StreamOutputContext out_ctx;
    Am824Pdu out_pdu{};
    Am824SampleRate rate;
    uint8_t ch;
    uint32_t pkt_count{0};

    std::array<std::array<float, Am824Pdu::MAX_SAMPLES_PER_PACKET>, Am824Pdu::MAX_CHANNELS> audio_buf{};

    // clang-format off
    // Long mem-init list — clang-19 leaves it inline, clang-22+ rewraps.
    Am824RetransmitState(RetransmitConfig const& cfg)
        : in_ctx{am824_sample_rate_from_hz(cfg.sample_rate).value_or(Am824SampleRate::rate_48_khz), static_cast<uint8_t>(cfg.channels)}
        , out_ctx{cfg.new_stream_id, am824_sample_rate_from_hz(cfg.sample_rate).value_or(Am824SampleRate::rate_48_khz), static_cast<uint8_t>(cfg.channels), cfg.presentation_offset}
        , rate{am824_sample_rate_from_hz(cfg.sample_rate).value_or(Am824SampleRate::rate_48_khz)}
        , ch{static_cast<uint8_t>(cfg.channels)}
    // clang-format on
    {
        out_pdu.init(cfg.new_stream_id, ch, rate);
    }

    /// Decode the input packet into audio_buf; the data blocks it held, 0 when nothing decoded.
    auto decode_input(std::span<uint8_t const> payload, uint64_t gptp_now) -> uint8_t
    {
        Am824Pdu in_pdu{};
        span_load(in_pdu, payload.subspan(0, Am824Pdu::HEADER_LENGTH));
        auto const audio_payload = am824_get_audio_payload(in_pdu, payload);  // declared length, not the padding
        uint8_t sample_count = 0;
        am824_deserialize_mbla(
            in_ctx, in_pdu, audio_payload, gptp_now, [&](uint8_t ch_idx, std::span<float> samples, uint64_t, uint64_t) {
                if (ch_idx < Am824Pdu::MAX_CHANNELS) {
                    size_t const kept = std::min<size_t>(samples.size(), Am824Pdu::MAX_SAMPLES_PER_PACKET);
                    for (size_t i = 0; i < kept; ++i) {
                        audio_buf[ch_idx][i] = samples[i];
                    }
                    sample_count = static_cast<uint8_t>(kept);  // what audio_buf holds, never more
                }
            });
        return sample_count;
    }

    void process_packet(
        std::span<uint8_t const> payload, uint64_t gptp_now, uint64_t capture_us, RetransmitConfig const& cfg, FileWriter& writer)
    {
        uint8_t const sample_count = decode_input(payload, gptp_now);
        if (sample_count == 0) {
            return;
        }

        std::array<uint8_t, Am824Pdu::MAX_CHANNELS * Am824Pdu::MAX_SAMPLES_PER_PACKET * 4> out_payload{};
        size_t const out_bytes = am824_serialize_mbla(
            out_ctx, out_pdu, std::span{out_payload}, sample_count, gptp_now, [&](uint8_t ch_idx, std::span<float> dest) {
                for (size_t i = 0; i < dest.size(); ++i) {
                    dest[i] = (ch_idx < Am824Pdu::MAX_CHANNELS) ? audio_buf[ch_idx][i] : 0.0f;
                }
            });

        if (out_bytes > 0) {
            std::array<uint8_t, Am824Pdu::HEADER_LENGTH + (Am824Pdu::MAX_CHANNELS * Am824Pdu::MAX_SAMPLES_PER_PACKET * 4)> frame{};
            auto const out_frame = span_pack_header_payload(frame, out_pdu, make_const_span(out_payload, {.length = out_bytes}));
            write_avtp_frame(writer, capture_us, cfg, out_frame);

            ++pkt_count;
            if (cfg.verbose && pkt_count % 100 == 0) {
                std::println("  packet {} samples={} dbc={}", pkt_count, sample_count, out_ctx.running_dbc);
            }
        }
    }
};

auto retransmit_am824(RetransmitConfig const& cfg) -> int
{
    Am824RetransmitState state{cfg};

    auto const stats = run_pcap_loop(
        cfg,
        AvtpSubtype::iec_61883_iidc,
        Am824Pdu::HEADER_LENGTH,
        [&](std::span<uint8_t const> payload, uint64_t gptp_now, uint64_t capture_us, FileWriter& writer) {
            state.process_packet(payload, gptp_now, capture_us, cfg, writer);
        });

    std::println(
        "AM824 retransmit: {} packets matched out of {} total ({} gPTP syncs)", stats.matched, stats.total, stats.gptp_syncs);
    if (stats.matched > 0) {
        std::println(
            "  Input:  {} packets, {} timestamp updates, {} sequence gaps",
            state.in_ctx.packets_received,
            state.in_ctx.timestamp_updates,
            state.in_ctx.sequence_gaps);
        std::println("  Output: {} packets, {} timestamp inserts", state.out_ctx.packets_sent, state.out_ctx.timestamp_inserts);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// AAF retransmit
// ---------------------------------------------------------------------------

struct AafRetransmitState
{
    AafStreamInputContext in_ctx;
    AafStreamOutputContext out_ctx;
    AafFormat fmt;
    uint32_t pkt_count{0};

    static constexpr size_t MAX_CH = 64;
    static constexpr size_t MAX_SAMP = 256;
    std::array<std::array<float, MAX_SAMP>, MAX_CH> audio_buf{};

    AafRetransmitState(RetransmitConfig const& cfg)
        : in_ctx{aaf_format_from_name(cfg.aaf_format).value_or(AafFormat::int_24bit),
                 aaf_sample_rate_from_hz(cfg.sample_rate).value_or(AafSampleRate::rate_48_khz),
                 cfg.channels, cfg.bit_depth}
        , out_ctx{cfg.new_stream_id,
                  aaf_format_from_name(cfg.aaf_format).value_or(AafFormat::int_24bit),
                  aaf_sample_rate_from_hz(cfg.sample_rate).value_or(AafSampleRate::rate_48_khz),
                  cfg.channels, cfg.bit_depth, cfg.presentation_offset}
        , fmt{aaf_format_from_name(cfg.aaf_format).value_or(AafFormat::int_24bit)}
    {}

    /// Decode the input packet into audio_buf; the samples per channel it held, 0 when nothing decoded.
    auto decode_input(std::span<uint8_t const> payload, uint64_t gptp_now) -> uint16_t
    {
        AafPdu in_pdu{};
        span_load(in_pdu, payload.subspan(0, AafPdu::HEADER_LENGTH));
        auto const audio_payload = aaf_get_audio_payload(in_pdu, payload);  // declared length, not the padding
        uint16_t sample_count = 0;
        aaf_stream_deserialize(
            in_ctx, in_pdu, audio_payload, gptp_now, [&](uint8_t ch_idx, std::span<float> samples, uint64_t, uint64_t) {
                if (ch_idx < MAX_CH) {
                    size_t const kept = std::min<size_t>(samples.size(), MAX_SAMP);
                    for (size_t i = 0; i < kept; ++i) {
                        audio_buf[ch_idx][i] = samples[i];
                    }
                    sample_count = static_cast<uint16_t>(kept);  // what audio_buf holds, never more
                }
            });
        return sample_count;
    }

    void process_packet(
        std::span<uint8_t const> payload, uint64_t gptp_now, uint64_t capture_us, RetransmitConfig const& cfg, FileWriter& writer)
    {
        uint16_t const sample_count = decode_input(payload, gptp_now);
        if (sample_count == 0) {
            return;
        }

        AafPdu out_pdu{};
        std::vector<uint8_t> out_payload(static_cast<size_t>(sample_count) * cfg.channels * aaf_bytes_per_sample(fmt));
        size_t const out_bytes = aaf_stream_serialize(
            out_ctx, out_pdu, std::span{out_payload}, sample_count, gptp_now, [&](uint8_t ch_idx, std::span<float> dest) {
                for (size_t i = 0; i < dest.size(); ++i) {
                    dest[i] = (ch_idx < MAX_CH) ? audio_buf[ch_idx][i] : 0.0f;
                }
            });

        if (out_bytes > 0) {
            std::vector<uint8_t> frame(AafPdu::HEADER_LENGTH + out_bytes);
            auto const out_frame = span_pack_header_payload(frame, out_pdu, make_const_span(out_payload, {.length = out_bytes}));
            write_avtp_frame(writer, capture_us, cfg, out_frame);

            ++pkt_count;
            if (cfg.verbose && pkt_count % 100 == 0) {
                std::println("  packet {} samples={}", pkt_count, sample_count);
            }
        }
    }
};

auto retransmit_aaf(RetransmitConfig const& cfg) -> int
{
    AafRetransmitState state{cfg};

    auto const stats = run_pcap_loop(
        cfg,
        AvtpSubtype::aaf,
        AafPdu::HEADER_LENGTH,
        [&](std::span<uint8_t const> payload, uint64_t gptp_now, uint64_t capture_us, FileWriter& writer) {
            state.process_packet(payload, gptp_now, capture_us, cfg, writer);
        });

    std::println(
        "AAF retransmit: {} packets matched out of {} total ({} gPTP syncs)", stats.matched, stats.total, stats.gptp_syncs);
    if (stats.matched > 0) {
        std::println(
            "  Input:  {} packets, {} timestamp updates, {} sequence gaps",
            state.in_ctx.packets_received,
            state.in_ctx.timestamp_updates,
            state.in_ctx.sequence_gaps);
        std::println("  Output: {} packets sent", state.out_ctx.packets_sent);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// CRF retransmit
// ---------------------------------------------------------------------------

struct CrfRetransmitState
{
    CrfStreamInputContext in_ctx;
    CrfStreamOutputContext out_ctx;
    bool first_packet{true};
    uint32_t pkt_count{0};

    CrfRetransmitState(RetransmitConfig const& cfg)
        : in_ctx{CrfType::audio_sample, cfg.sample_rate, CrfPull::multiply_1_0, 1}
        , out_ctx{cfg.new_stream_id, CrfType::audio_sample, cfg.sample_rate, CrfPull::multiply_1_0, 1, 6}
    {}

    void process_packet(
        std::span<uint8_t const> payload, uint64_t gptp_now, uint64_t capture_us, RetransmitConfig const& cfg, FileWriter& writer)
    {
        CrfPdu in_pdu{};
        span_load(in_pdu, payload.subspan(0, CrfPdu::HEADER_LENGTH));

        // Auto-configure from first packet
        if (first_packet) {
            out_ctx = CrfStreamOutputContext{
                cfg.new_stream_id,
                in_pdu.get_crf_type(),
                in_pdu.base_frequency(),
                in_pdu.get_pull(),
                in_pdu.timestamp_interval(),
                in_pdu.timestamp_count()};
            in_ctx = CrfStreamInputContext{
                in_pdu.get_crf_type(), in_pdu.base_frequency(), in_pdu.get_pull(), in_pdu.timestamp_interval()};
            first_packet = false;
        }

        auto const ts_data = payload.subspan(CrfPdu::HEADER_LENGTH);
        in_ctx.process_packet(in_pdu, ts_data, [&](uint64_t, uint16_t) {});

        // Anchor output to input timing on first packet
        if (!out_ctx.started) {
            auto first_ts = crf_get_timestamp(ts_data, 0);
            if (first_ts) {
                out_ctx.next_timestamp_ns = *first_ts + cfg.presentation_offset;
                out_ctx.started = true;
            }
        }

        uint16_t const ts_count = in_pdu.timestamp_count();
        CrfPdu out_pdu{};
        std::vector<uint8_t> out_ts(static_cast<size_t>(ts_count) * CrfPdu::TIMESTAMP_SIZE);

        size_t const out_bytes = out_ctx.build_packet(out_pdu, std::span{out_ts}, gptp_now);
        if (out_bytes > 0) {
            std::vector<uint8_t> frame(CrfPdu::HEADER_LENGTH + out_bytes);
            auto const out_frame = span_pack_header_payload(frame, out_pdu, make_const_span(out_ts, {.length = out_bytes}));
            write_avtp_frame(writer, capture_us, cfg, out_frame);

            ++pkt_count;
            if (cfg.verbose && pkt_count % 100 == 0) {
                std::println("  packet {} timestamps={}", pkt_count, ts_count);
            }
        }
    }
};

auto retransmit_crf(RetransmitConfig const& cfg) -> int
{
    CrfRetransmitState state{cfg};

    auto const stats = run_pcap_loop(
        cfg,
        AvtpSubtype::crf,
        CrfPdu::HEADER_LENGTH,
        [&](std::span<uint8_t const> payload, uint64_t gptp_now, uint64_t capture_us, FileWriter& writer) {
            state.process_packet(payload, gptp_now, capture_us, cfg, writer);
        });

    std::println(
        "CRF retransmit: {} packets matched out of {} total ({} gPTP syncs)", stats.matched, stats.total, stats.gptp_syncs);
    if (stats.matched > 0) {
        std::println(
            "  Input:  {} packets, {} total timestamps", state.in_ctx.packets_received, state.in_ctx.total_timestamps_received);
        std::println("  Output: {} packets, {} total timestamps", state.out_ctx.packets_sent, state.out_ctx.total_timestamps_sent);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

auto print_usage(char const* program_name, args::ArgumentSpecs const& specs) -> void
{
    std::println(stderr, "Usage: {} [options]", program_name);
    std::println(stderr, "");
    std::println(stderr, "AVTP Stream Retransmit Tool");
    std::println(stderr, "Reads an AVTP stream from a pcap capture, round-trips through");
    std::println(stderr, "input/output stream contexts, and writes a new pcap file.");
    std::println(stderr, "");
    config::default_print_usage(program_name, specs);
}

auto main(int argc, char* argv[]) -> int
{
    RetransmitConfig cfg;
    auto specs = build_arg_specs(cfg);

    auto const result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avtp-retransmit");
    if (!result) {
        if (config::handled_builtin_command(result)) {
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    }

    if (cfg.input_file.empty()) {
        std::println(stderr, "Error: --input is required");
        return EXIT_FAILURE;
    }

    if (!cfg.stream_id.is_set()) {
        std::println(stderr, "Error: --stream-id is required");
        return EXIT_FAILURE;
    }

    if (!cfg.new_stream_id.is_set()) {
        cfg.new_stream_id = StreamId{cfg.src_mac, 1};
    }

    {
        std::string sid_str, new_sid_str, src_str, dst_str;
        tsn::format_to(std::back_inserter(sid_str), cfg.stream_id);
        tsn::format_to(std::back_inserter(new_sid_str), cfg.new_stream_id);
        ieee::format_to(std::back_inserter(src_str), cfg.src_mac);
        ieee::format_to(std::back_inserter(dst_str), cfg.dst_mac);
        std::println("AVTP Retransmit: format={} stream_id={}", cfg.format, sid_str);
        std::println(
            "  Output: src={} dst={} new_stream_id={} offset={}ns", src_str, dst_str, new_sid_str, cfg.presentation_offset);
    }

    auto const run_result = statusbar::catch_or_status(
        [&]() -> statusbar::StatusValue<int> {
            if (cfg.format == "am824") {
                return retransmit_am824(cfg);
            }
            if (cfg.format == "aaf") {
                return retransmit_aaf(cfg);
            }
            if (cfg.format == "crf") {
                return retransmit_crf(cfg);
            }
            std::println(stderr, "Error: unknown format '{}' (use am824, aaf, or crf)", cfg.format);
            return EXIT_FAILURE;
        },
        std::errc::io_error);
    if (!run_result) {
        std::println(stderr, "Error: {}", run_result.error().message());
        return EXIT_FAILURE;
    }
    return *run_result;
}
