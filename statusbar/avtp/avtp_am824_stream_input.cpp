// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_am824_stream_input.hpp"

namespace statusbar::avtp {

Am824StreamInputContext::Am824StreamInputContext(Am824SampleRate rate, uint8_t channels)
    : config{rate, channels}
{}

void Am824StreamInputContext::reset() noexcept
{
    last_dbc = 0;
    last_sequence_num = 0;
    last_avtp_timestamp = 0;
    last_anchor_pts_ns = 0;
    last_gptp_time_ns = 0;
    last_anchor_dbc = 0;
    running_dbc = 0;
    last_sample_count = 0;
    has_valid_anchor = false;
    packets_received = 0;
    timestamp_updates = 0;
    sequence_gaps = 0;
    dbc_discontinuities = 0;
}

void Am824StreamInputContext::update_dbc(uint8_t dbc) noexcept
{
    if (packets_received == 0) {
        running_dbc = dbc;
    } else {
        uint8_t const delta = static_cast<uint8_t>(dbc - last_dbc);
        running_dbc += delta;
    }
    last_dbc = dbc;
}

void Am824StreamInputContext::update_timestamp(bool tv, uint32_t avtp_timestamp, uint8_t dbc, uint64_t gptp_now_ns) noexcept
{
    last_gptp_time_ns = gptp_now_ns;

    if (tv && config.syt_interval > 0) {
        // IEEE 1722-2016 Equation (7):
        // index = mod(SYT_INTERVAL - mod(DBC, SYT_INTERVAL), SYT_INTERVAL)
        uint8_t const dbc_mod = dbc % config.syt_interval;
        uint8_t const index = (config.syt_interval - dbc_mod) % config.syt_interval;

        last_avtp_timestamp = avtp_timestamp;
        last_anchor_pts_ns = reconstruct_avtp_timestamp(avtp_timestamp, gptp_now_ns);
        last_anchor_dbc = running_dbc + index;
        has_valid_anchor = true;
        ++timestamp_updates;
    }
}

auto Am824StreamInputContext::compute_base_pts_ns() const noexcept -> uint64_t
{
    if (!has_valid_anchor) {
        return 0;
    }
    int64_t const dbc_delta = static_cast<int64_t>(running_dbc) - static_cast<int64_t>(last_anchor_dbc);
    return last_anchor_pts_ns + static_cast<uint64_t>(dbc_delta * static_cast<int64_t>(config.sample_period_ns));
}

void Am824StreamInputContext::update_sequence_num(uint8_t seq_num) noexcept
{
    if (packets_received > 0) {
        uint8_t const expected = static_cast<uint8_t>((last_sequence_num + 1U) & 0xFFU);
        if (seq_num != expected) {
            ++sequence_gaps;
        }
    }
    last_sequence_num = seq_num;
}

void Am824StreamInputContext::process_packet_header(Am824Pdu const& pdu, uint8_t sample_count, uint64_t gptp_now_ns) noexcept
{
    uint8_t const dbc = pdu.data_block_count();
    update_sequence_num(pdu.sequence_num());
    if (packets_received > 0 && dbc != static_cast<uint8_t>(last_dbc + last_sample_count)) {
        ++dbc_discontinuities;
    }
    last_sample_count = sample_count;
    update_dbc(dbc);
    update_timestamp(pdu.tv(), pdu.avtp_timestamp(), dbc, gptp_now_ns);
    ++packets_received;
}

}  // namespace statusbar::avtp
