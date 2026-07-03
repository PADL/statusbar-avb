// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/gptp/gptp_md_pdelay_req.hpp"

#include "statusbar/gptp/gptp_time_util.hpp"

namespace statusbar::gptp {

void MDPdelayReq::enable() noexcept
{
    state_ = State::Initial;
    next_sequence_id_ = 1;
    reset_in_flight();
    previous_completed_.reset();
    neighbor_rate_ratio_ = 1.0;
    lost_responses_ = 0;
    as_capable_ = false;
}

void MDPdelayReq::disable() noexcept
{
    state_ = State::NotEnabled;
    reset_in_flight();
    as_capable_ = false;
}

auto MDPdelayReq::pdelay_interval_timer_expired() noexcept -> std::optional<uint16_t>
{
    if (state_ == State::NotEnabled) {
        return std::nullopt;
    }
    // A stale in-flight exchange means we missed something; treat
    // that as a lost response before starting the next one.
    if (state_ == State::WaitingForPdelayResp || state_ == State::WaitingForPdelayRespFollowUp) {
        complete_exchange_failure();
    }
    reset_in_flight();
    in_flight_.sequence_id = next_sequence_id_++;
    state_ = State::WaitingForPdelayResp;
    return in_flight_.sequence_id;
}

void MDPdelayReq::on_pdelay_req_tx_timestamp(uint16_t sequence_id, int64_t tx_ts_local_ns) noexcept
{
    if (state_ == State::NotEnabled) {
        return;
    }
    if (sequence_id != in_flight_.sequence_id) {
        return;  // stale TX timestamp — ignore
    }
    in_flight_.ex.t1_req_tx_local_ns = tx_ts_local_ns;
    in_flight_.have_t1 = true;
}

void MDPdelayReq::on_pdelay_resp(PdelayRespMessage const& msg, int64_t rx_ts_local_ns) noexcept
{
    if (state_ != State::WaitingForPdelayResp) {
        return;
    }
    if (msg.header.sequence_id != in_flight_.sequence_id) {
        return;  // sequence id mismatch — likely stale or peer misbehaving
    }
    in_flight_.ex.t2_req_rx_peer_ns = ts_to_ns(msg.request_receipt_timestamp);
    in_flight_.have_t2 = true;
    in_flight_.ex.t4_resp_rx_local_ns = rx_ts_local_ns;
    in_flight_.have_t4 = true;
    state_ = State::WaitingForPdelayRespFollowUp;
}

auto MDPdelayReq::on_pdelay_resp_follow_up(PdelayRespFollowUpMessage const& msg) noexcept -> std::optional<MDPdelayMeasurement>
{
    if (state_ != State::WaitingForPdelayRespFollowUp) {
        return std::nullopt;
    }
    if (msg.header.sequence_id != in_flight_.sequence_id) {
        return std::nullopt;
    }
    // Complete the exchange.
    in_flight_.ex.t3_resp_tx_peer_ns = ts_to_ns(msg.response_origin_timestamp);
    auto const have_all_timestamps = in_flight_.have_t1 && in_flight_.have_t2 && in_flight_.have_t4;
    if (!have_all_timestamps) {
        // Missing a timestamp — can't compute; fail the exchange.
        complete_exchange_failure();
        return std::nullopt;
    }

    // Compute neighborRateRatio if we have a prior exchange.
    if (previous_completed_.has_value()) {
        double const raw_ratio = compute_neighbor_rate_ratio(*previous_completed_, in_flight_.ex);
        neighbor_rate_ratio_ = clamp_rate_ratio_ppm(raw_ratio, config_.max_rate_ratio_ppm);
    }

    // Compute link delay using the (possibly updated) rate ratio.
    int64_t const link_delay = compute_link_delay_ns(in_flight_.ex, neighbor_rate_ratio_);

    // Build the measurement.
    MDPdelayMeasurement meas{};
    meas.sequence_id = in_flight_.sequence_id;
    meas.mean_link_delay_ns = link_delay;
    meas.neighbor_rate_ratio = neighbor_rate_ratio_;
    meas.exchange = in_flight_.ex;

    // Record for next rate-ratio computation.
    previous_completed_ = in_flight_.ex;
    complete_exchange_success();
    // 802.1AS 11.2.2: a link longer than the neighbor-prop-delay threshold is not
    // AVB-usable even though the exchange succeeded -- drop asCapable. 0 = disabled.
    if (config_.neighbor_prop_delay_threshold_ns > 0 && link_delay > config_.neighbor_prop_delay_threshold_ns) {
        as_capable_ = false;
    }
    return meas;
}

void MDPdelayReq::on_pdelay_resp_receipt_timeout() noexcept
{
    if (state_ != State::WaitingForPdelayResp) {
        return;
    }
    complete_exchange_failure();
}

void MDPdelayReq::on_pdelay_resp_follow_up_receipt_timeout() noexcept
{
    if (state_ != State::WaitingForPdelayRespFollowUp) {
        return;
    }
    complete_exchange_failure();
}

void MDPdelayReq::reset_in_flight() noexcept
{
    in_flight_ = InFlight{};
}

void MDPdelayReq::complete_exchange_success() noexcept
{
    lost_responses_ = 0;
    as_capable_ = true;
    state_ = State::WaitingForPdelayIntervalTimer;
    reset_in_flight();
}

void MDPdelayReq::complete_exchange_failure() noexcept
{
    if (lost_responses_ < UINT16_MAX) {
        ++lost_responses_;
    }
    if (lost_responses_ >= config_.lost_response_threshold) {
        as_capable_ = false;
    }
    state_ = State::WaitingForPdelayIntervalTimer;
    reset_in_flight();
}

}  // namespace statusbar::gptp
