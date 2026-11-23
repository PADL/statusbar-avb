// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aaf_stream_input.hpp"

namespace statusbar::avtp {

AafStreamInputContext::AafStreamInputContext(AafFormat fmt, AafSampleRate rate, uint16_t channels, uint8_t depth)
    : format{fmt}
    , sample_rate{rate}
    , channel_count{channels}
    , bit_depth{depth}
    , sample_period_ns{aaf_sample_rate_hz(rate) > 0 ? 1'000'000'000ULL / aaf_sample_rate_hz(rate) : 0}
{}

void AafStreamInputContext::reset() noexcept
{
    last_sequence_num = 0;
    last_pts_ns = 0;
    last_gptp_time_ns = 0;
    samples_since_ts = 0;
    has_valid_timestamp = false;
    packets_received = 0;
    timestamp_updates = 0;
    sequence_gaps = 0;
}

void AafStreamInputContext::update_sequence_num(uint8_t seq_num) noexcept
{
    if (packets_received > 0) {
        uint8_t const expected = static_cast<uint8_t>((last_sequence_num + 1U) & 0xFFU);
        if (seq_num != expected) {
            ++sequence_gaps;
        }
    }
    last_sequence_num = seq_num;
}

void AafStreamInputContext::process_packet_header(AafPdu const& pdu, uint64_t gptp_now_ns) noexcept
{
    process_packet_header(pdu, Nanoseconds{static_cast<int64_t>(gptp_now_ns)});
}

void AafStreamInputContext::process_packet_header(AafPdu const& pdu, Nanoseconds const now) noexcept
{
    update_sequence_num(pdu.get_sequence_num());
    last_gptp_time_ns = now.count();

    if (pdu.tv()) {
        // This packet carries a valid presentation timestamp.
        // Reset the PTS anchor to the reconstructed full timestamp.
        last_pts_ns = reconstruct_avtp_timestamp(pdu.get_avtp_timestamp(), now).count();
        samples_since_ts = 0;
        has_valid_timestamp = true;
        ++timestamp_updates;
    }

    ++packets_received;
}

auto AafStreamInputContext::compute_pts_ns() const noexcept -> uint64_t
{
    return static_cast<uint64_t>(compute_pts().count());
}

auto AafStreamInputContext::compute_pts() const noexcept -> Nanoseconds
{
    if (!has_valid_timestamp) {
        return Nanoseconds::zero();
    }
    // PTS of the first sample in the current packet.
    // samples_since_ts is the number of samples between the anchor and this packet.
    uint64_t const sum = static_cast<uint64_t>(last_pts_ns) + (static_cast<uint64_t>(samples_since_ts) * sample_period_ns);
    return Nanoseconds{static_cast<int64_t>(sum)};  // summed unsigned: no overflow to undefined behaviour
}

void AafStreamInputContext::advance_samples(uint16_t sample_count) noexcept
{
    samples_since_ts += sample_count;
}

}  // namespace statusbar::avtp
