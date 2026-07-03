#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// MDPdelayReq — IEEE 802.1AS-2020 Clause 11.2.15.
//
// Drives the 4-timestamp peer delay exchange that produces
// meanLinkDelay and neighborRateRatio. Exposes callbacks for the
// three Pdelay messages it expects to receive (Pdelay_Resp,
// Pdelay_Resp_Follow_Up, and their TX timestamp feedback for our
// own Pdelay_Req), plus hooks for Pdelay interval timer expiration
// and receipt timeouts.
//
// Three modes via config.pdelay_mode:
//   - Active      — full exchange (initiate + respond)
//   - RespondOnly — never initiate; FSM remains in NotEnabled forever
//   - Disabled    — never initiate, never respond
//
// The NotEnabled variant is handled by the participant: if
// pdelay_mode != Active, it simply never constructs / drives this
// class. The config-supplied manual_peer_delay_ns and
// manual_neighbor_rate_ratio are used directly by the participant
// as the meanLinkDelay / neighborRateRatio.
//

#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/gptp/gptp_link_delay.hpp"
#include "statusbar/gptp/gptp_messages.hpp"

#include <cstdint>
#include <optional>

namespace statusbar::gptp {

/// Result of a successful Pdelay exchange, returned to the caller
/// after the Pdelay_Resp_Follow_Up completes the round-trip.
struct MDPdelayMeasurement
{
    uint16_t sequence_id{0};

    /// Computed meanLinkDelay in local clock nanoseconds.
    int64_t mean_link_delay_ns{0};

    /// Current best estimate of neighborRateRatio
    /// (= local_freq / neighbor_freq). On the first completed
    /// exchange this is 1.0 (bootstrap); from the second onward it
    /// is the ratio computed from two successive exchanges.
    double neighbor_rate_ratio{1.0};

    /// Raw timestamps of the exchange, for diagnostic observers.
    PdelayExchange exchange{};
};

/// MDPdelayReq driver. Plain class rather than a statusbar::sm FSM —
/// state transitions are driven by messages whose content the SM
/// framework can't carry through its void(Context,TimePoint) action
/// signature. The explicit state enum matches Clause 11.2.15.3.
class MDPdelayReq
{
  public:
    /// Slave-side Pdelay requestor states per Clause 11.2.15.3.
    enum class State : uint8_t
    {
        NotEnabled,                     ///< pdelay disabled or port down
        Initial,                        ///< bootstrap; will send first request
        WaitingForPdelayResp,           ///< sent request, awaiting response
        WaitingForPdelayRespFollowUp,   ///< got response, awaiting follow-up
        WaitingForPdelayIntervalTimer,  ///< idle between exchanges
    };

    /// Configuration snapshot taken at construction time.
    struct Config
    {
        /// Port number to put in our outgoing Pdelay_Req. Filled in
        /// from GptpConfig::local_port_number.
        uint16_t local_port_number{1};

        /// Consecutive Pdelay_Resp mismatches / losses allowed before
        /// declaring asCapable=false. From
        /// GptpConfig::seq_id_as_capable_threshold.
        uint8_t seq_id_mismatch_threshold{2};

        /// Consecutive missing Pdelay_Resps allowed before asCapable
        /// is dropped. From GptpConfig::lost_pdelay_resp_threshold.
        uint16_t lost_response_threshold{3};

        /// Rate ratio ±ppm clamp, from GptpConfig::servo_ppm_limit.
        double max_rate_ratio_ppm{250.0};

        /// Max mean link delay (ns) for the peer to be asCapable (802.1AS 11.2.2).
        /// A successful exchange whose measured delay exceeds this still drops
        /// asCapable. 0 = disabled. From GptpConfig::neighbor_prop_delay_threshold_ns.
        int64_t neighbor_prop_delay_threshold_ns{0};
    };

    explicit MDPdelayReq(Config const& config) noexcept
        : config_{config}
    {}

    [[nodiscard]] auto state() const noexcept -> State { return state_; }

    /// Enable the FSM. Called once at startup when the port comes up
    /// and config.pdelay_mode == Active. After this, the participant
    /// is expected to call pdelay_interval_timer_expired() to kick
    /// off the first exchange.
    void enable() noexcept;

    /// Disable the FSM. Called on link down or administrative
    /// disable. Any in-flight state is dropped.
    void disable() noexcept;

    /// Called when the Pdelay interval timer expires and we should
    /// send the next Pdelay_Req. The caller is responsible for
    /// actually building and transmitting the frame; this method
    /// only advances the FSM state. Returns the sequence id to use
    /// on the outgoing Pdelay_Req. Returns nullopt if the FSM is
    /// not in a state that can initiate.
    [[nodiscard]] auto pdelay_interval_timer_expired() noexcept -> std::optional<uint16_t>;

    /// Called once the participant has the TX hardware timestamp of
    /// the Pdelay_Req frame it just sent (this may be synchronous
    /// inside the send_frame callback or deferred).
    void on_pdelay_req_tx_timestamp(uint16_t sequence_id, int64_t tx_ts_local_ns) noexcept;

    /// Called when a Pdelay_Resp arrives for a sequence id we sent.
    void on_pdelay_resp(PdelayRespMessage const& msg, int64_t rx_ts_local_ns) noexcept;

    /// Called when the matching Pdelay_Resp_Follow_Up arrives. If
    /// the exchange is now complete, returns a valid measurement
    /// (the caller uses it to update the port's link delay / rate
    /// ratio state).
    [[nodiscard]] auto on_pdelay_resp_follow_up(PdelayRespFollowUpMessage const& msg) noexcept
        -> std::optional<MDPdelayMeasurement>;

    /// Called when the Pdelay_Resp receipt timeout fires (no
    /// Pdelay_Resp arrived within the expected window).
    void on_pdelay_resp_receipt_timeout() noexcept;

    /// Called when the Pdelay_Resp_Follow_Up receipt timeout fires.
    void on_pdelay_resp_follow_up_receipt_timeout() noexcept;

    /// True if the FSM believes the peer is responsive enough to
    /// consider the port asCapable for sync purposes. The
    /// participant ANDs this with its own asCapable policy (e.g.
    /// the Automotive pre-asCapable flag).
    [[nodiscard]] auto is_as_capable() const noexcept -> bool { return as_capable_; }

    /// Consecutive lost Pdelay_Resp counter.
    [[nodiscard]] auto lost_response_count() const noexcept -> uint16_t { return lost_responses_; }

  private:
    Config config_;
    State state_{State::NotEnabled};

    /// Sequence id of the next Pdelay_Req we'll send. Monotonically
    /// incremented each exchange.
    uint16_t next_sequence_id_{1};

    /// In-flight exchange, built up as Pdelay_Req TX / Pdelay_Resp /
    /// Pdelay_Resp_Follow_Up events arrive.
    struct InFlight
    {
        uint16_t sequence_id{0};
        PdelayExchange ex{};
        bool have_t1{false};  // our Pdelay_Req TX timestamp
        bool have_t2{false};  // peer's Pdelay_Req RX (from Pdelay_Resp)
        bool have_t4{false};  // our Pdelay_Resp RX timestamp
    };
    InFlight in_flight_{};

    /// Previous completed exchange, for neighbor rate ratio calculation.
    std::optional<PdelayExchange> previous_completed_{};

    /// Latest neighborRateRatio estimate.
    double neighbor_rate_ratio_{1.0};

    /// Consecutive lost-response counter.
    uint16_t lost_responses_{0};

    /// Current asCapable state.
    bool as_capable_{false};

    /// Reset in-flight tracking for the current exchange.
    void reset_in_flight() noexcept;

    /// Advance to WaitingForPdelayIntervalTimer (exchange done) and
    /// either promote to asCapable or update the lost-response
    /// counter.
    void complete_exchange_success() noexcept;

    /// Exchange failed; bump lost-response counter and potentially
    /// drop asCapable.
    void complete_exchange_failure() noexcept;
};

}  // namespace statusbar::gptp
