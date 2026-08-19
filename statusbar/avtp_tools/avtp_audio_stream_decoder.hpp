#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AvtpAudioStreamDecoder — single-stream, sink-agnostic decoder for AVTP
// audio payloads (AAF or simple AM824 MBLA). Caller filters input by
// stream id / VLAN / source as needed and feeds matching payloads in;
// the decoder figures out the format from the first packet, then
// dispatches subsequent packets through the appropriate stream-input
// deserializer and pushes interleaved float samples to the caller's
// sink callback.
//
// The decoder owns no I/O. The output sink is an inplace_function that the
// caller wires to wherever the samples should land — Bw64Writer for the
// avtp-to-wav tool, an AudioStream for live monitoring, a network
// pipe for retransmit, etc. Keeping the decoder free of any one sink is
// the whole point: the same decoding logic is reusable across tools
// rather than tied to a single output path.
//
// Single-stream by design: caller is responsible for stream-id and VLAN
// filtering before feeding payloads in. To handle multiple streams from
// a single capture, instantiate one AvtpAudioStreamDecoder per stream id.

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_am824_stream_input.hpp"
#include "statusbar/avtp_tools/avtp_audio_detect.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace statusbar::avtp_tools {

// Format descriptor — what the decoder learned from the first packet.
// Caller reads this (after format() reports kind != unknown) to set up
// its sink: open a WAV file with the right rate/channels, configure an
// audio stream, etc.
struct AvtpAudioFormat
{
    StreamKind kind{StreamKind::unknown};
    uint64_t stream_id{0};
    uint32_t sample_rate_hz{0};
    uint16_t channel_count{0};

    // Set when kind == aaf:
    avtp::AafFormat aaf_format{avtp::AafFormat::user_specified};
    avtp::AafSampleRate aaf_rate{avtp::AafSampleRate::user_specified};
    uint8_t aaf_bit_depth{0};

    // Set when kind == am824_mbla:
    avtp::Am824SampleRate am824_rate{avtp::Am824SampleRate::rate_48_khz};
};

// One decoded packet's worth of interleaved samples handed to the sink.
// The span is valid only for the duration of the sink call — the
// decoder reuses an internal scratch buffer between packets.
struct AvtpAudioSamples
{
    std::span<float const> interleaved;  // size == frames * channel_count
    size_t frames{0};
    uint64_t gptp_pts_ns{0};  // presentation timestamp of the first sample
};

enum class FeedStatus : uint8_t
{
    ok,            // packet processed, samples delivered to sink (or none if empty)
    wrong_kind,    // packet kind doesn't match initialized kind
    format_error,  // first packet had an unusable format (zero rate/channels)
    sink_error,    // sink callback returned false
};

struct FeedResult
{
    FeedStatus status{FeedStatus::ok};
    bool sequence_gap_observed{false};
    bool samples_delivered{false};
};

class AvtpAudioStreamDecoder
{
  public:
    // Sink: receives interleaved float samples for one packet. Return
    // true to continue, false to abort (decoder reports sink_error and
    // refuses further work until reset).
    using SamplesSink = statusbar::sg14::inplace_function<bool(AvtpAudioSamples const&), 64>;

    // Diagnostic: optional. Called for non-fatal events (sequence gaps,
    // first-packet format problems). Caller decides whether to log.
    using Diagnostic = statusbar::sg14::inplace_function<void(std::string_view message), 64>;

    AvtpAudioStreamDecoder() = default;
    AvtpAudioStreamDecoder(AvtpAudioStreamDecoder const&) = delete;
    auto operator=(AvtpAudioStreamDecoder const&) -> AvtpAudioStreamDecoder& = delete;

    void set_sink(SamplesSink sink) noexcept { sink_ = std::move(sink); }
    void set_diagnostic(Diagnostic diag) noexcept { diagnostic_ = std::move(diag); }

    // Feed one AVTP audio payload (already filtered to AAF or AM824 by
    // the caller via payload_is_aaf / payload_is_am824). Returns details
    // of what happened.
    //
    // gptp_now_ns is forwarded to the underlying stream-input
    // deserializer; defaulting to 0 means "no presentation-time gating",
    // which is correct for offline pcap replay. Live consumers should
    // pass a monotonic gPTP nanosecond timestamp.
    [[nodiscard]] auto feed(std::span<uint8_t const> payload, uint64_t gptp_now_ns = 0) -> FeedResult;

    [[nodiscard]] auto initialized() const noexcept -> bool { return format_.kind != StreamKind::unknown; }
    [[nodiscard]] auto format() const noexcept -> AvtpAudioFormat const& { return format_; }

    // Counters
    [[nodiscard]] auto packets_decoded() const noexcept -> uint64_t { return packets_decoded_; }
    [[nodiscard]] auto sample_frames_delivered() const noexcept -> uint64_t { return sample_frames_delivered_; }
    [[nodiscard]] auto sequence_gaps_observed() const noexcept -> uint64_t { return sequence_gaps_observed_; }
    [[nodiscard]] auto first_gptp_pts_ns() const noexcept -> uint64_t { return first_gptp_pts_ns_; }

  private:
    [[nodiscard]] auto initialize_from_payload(std::span<uint8_t const> payload) -> bool;
    [[nodiscard]] auto initialize_aaf(avtp::AafPdu const& pdu) -> bool;
    [[nodiscard]] auto initialize_am824(avtp::Am824Pdu const& pdu) -> bool;
    [[nodiscard]] auto decode_aaf_payload(std::span<uint8_t const> payload, uint64_t gptp_now_ns) -> FeedResult;
    [[nodiscard]] auto decode_am824_payload(std::span<uint8_t const> payload, uint64_t gptp_now_ns) -> FeedResult;

    AvtpAudioFormat format_{};

    std::optional<avtp::AafStreamInputContext> aaf_ctx_;
    std::optional<avtp::Am824StreamInputContext> am824_ctx_;

    SamplesSink sink_;
    Diagnostic diagnostic_;

    std::vector<float> scratch_;

    uint64_t packets_decoded_{0};
    uint64_t sample_frames_delivered_{0};
    uint64_t sequence_gaps_observed_{0};
    uint64_t first_gptp_pts_ns_{0};
};

}  // namespace statusbar::avtp_tools
