// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aaf_v1_stream_input.hpp"

namespace statusbar::avtp {

AafV1StreamInputContext::AafV1StreamInputContext(AafFormat fmt, AafSampleRate rate, uint16_t channels, uint8_t depth)
    : format{fmt}
    , sample_rate{rate}
    , channel_count{channels}
    , bit_depth{depth}
    , sample_period_ns{aaf_sample_rate_hz(rate) > 0 ? 1'000'000'000ULL / aaf_sample_rate_hz(rate) : 0}
{}

void AafV1StreamInputContext::reset() noexcept
{
    last_sequence_num = 0;
    last_pts_ns = 0;
    last_gptp_time_ns = 0;
    samples_since_ts = 0;
    has_valid_timestamp = false;
    last_grandmaster = ClockIdentity{};
    grandmaster_changed = false;
    packets_received = 0;
    timestamp_updates = 0;
    sequence_gaps = 0;
}

void AafV1StreamInputContext::process_packet_header(AafV1Pdu const& pdu, uint64_t gptp_now_ns) noexcept
{
    process_packet_header(pdu, Nanoseconds{static_cast<int64_t>(gptp_now_ns)});
}

void AafV1StreamInputContext::process_packet_header(AafV1Pdu const& pdu, Nanoseconds const now) noexcept
{
    update_sequence_num(pdu.get_sequence_num());
    last_gptp_time_ns = now.count();
    if (pdu.tv()) {
        take_timestamp(pdu);
    }
    ++packets_received;
}

void AafV1StreamInputContext::update_sequence_num(uint32_t const seq_num) noexcept
{
    // 32-bit sequence gap detection
    if (packets_received > 0 && seq_num != last_sequence_num + 1U) {
        ++sequence_gaps;
    }
    last_sequence_num = seq_num;
}

void AafV1StreamInputContext::take_timestamp(AafV1Pdu const& pdu) noexcept
{
    // V1: 64-bit timestamp used directly — no reconstruction needed
    last_pts_ns = static_cast<int64_t>(pdu.get_avtp_timestamp());
    samples_since_ts = 0;
    has_valid_timestamp = true;
    ++timestamp_updates;
    note_grandmaster(pdu.get_ptp_grandmaster_identity());
}

void AafV1StreamInputContext::note_grandmaster(ClockIdentity const& gm) noexcept
{
    // Track grandmaster changes
    if (packets_received > 0 && gm != last_grandmaster) {
        grandmaster_changed = true;
    }
    last_grandmaster = gm;
}

auto AafV1StreamInputContext::compute_pts_ns() const noexcept -> uint64_t
{
    return static_cast<uint64_t>(compute_pts().count());
}

auto AafV1StreamInputContext::compute_pts() const noexcept -> Nanoseconds
{
    if (!has_valid_timestamp) {
        return Nanoseconds::zero();
    }
    // The stored time is the 64-bit wire field as it came. The sum is made on
    // unsigned counts, where it cannot overflow into undefined behaviour
    // whatever a talker sent; for any true time the result is the same.
    uint64_t const sum = static_cast<uint64_t>(last_pts_ns) + (static_cast<uint64_t>(samples_since_ts) * sample_period_ns);
    return Nanoseconds{static_cast<int64_t>(sum)};
}

void AafV1StreamInputContext::advance_samples(uint16_t sample_count) noexcept
{
    samples_since_ts += sample_count;
}

}  // namespace statusbar::avtp
