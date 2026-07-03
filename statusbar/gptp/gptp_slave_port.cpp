// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/gptp/gptp_slave_port.hpp"

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/gptp/gptp_time_util.hpp"
#include "statusbar/gptp/gptp_tlv.hpp"
#include "statusbar/status/throw_or_abort.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <system_error>

namespace statusbar::gptp {

namespace {

/// Convert log2(seconds) interval to std::chrono::nanoseconds.
/// E.g.: log=-3 → 125 ms = 125'000'000 ns.
auto log2_interval_to_ns(int8_t log_interval) -> std::chrono::nanoseconds
{
    // Clamp to the valid range first: config values are pre-validated, but a value
    // taken from a received Sync header is not, and an out-of-range exponent would
    // push the int64 cast into UB (large positive) or arm a 0-ns timeout (negative).
    double const seconds = std::pow(2.0, static_cast<double>(clamp_log_interval(log_interval)));
    return std::chrono::nanoseconds(static_cast<int64_t>(seconds * 1e9));
}

}  // namespace

// =============================================================
// Construction / lifecycle
// =============================================================

GptpSlavePort::GptpSlavePort(GptpConfig const& config, GptpClockOps ops, std::pmr::memory_resource* memory_resource)
    : config_{config}
    , ops_{std::move(ops)}
    , md_pdelay_req_{MDPdelayReq::Config{
          .local_port_number = config_.local_port_number,
          .seq_id_mismatch_threshold = config_.seq_id_as_capable_threshold,
          .lost_response_threshold = config_.lost_pdelay_resp_threshold,
          .max_rate_ratio_ppm = config_.servo_ppm_limit,
          .neighbor_prop_delay_threshold_ns = config_.neighbor_prop_delay_threshold_ns,
      }}
    , servo_{config_}
    , observers_{memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource()}
{
    if (auto const valid = config_.validate(); !valid) {
        statusbar::throw_or_abort(valid.error());
    }
    if (!is_complete(ops_)) {
        statusbar::throw_or_abort(make_error_code(GptpError::InvalidConfiguration));
    }
    // Pre-populate the PortStateSM context with the config's
    // as_capable_initial flag.
    port_state_ctx_.as_capable_initial = config_.as_capable_initial;

    // Reserve observer slots.
    observers_.resize(config_.max_observers);

    // If pdelay is not Active, apply manual values immediately.
    if (config_.pdelay_mode != PdelayMode::Active) {
        mean_link_delay_ns_ = config_.manual_peer_delay_ns;
        neighbor_rate_ratio_ = config_.manual_neighbor_rate_ratio;
    }
}

void GptpSlavePort::start(TimePoint now, bool link_up)
{
    // Fire the PortState SM's UCT chain (Start -> Disabled).
    port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::UCT, now);

    // Enable the sub-components.
    md_sync_receive_.enable();
    port_announce_receive_.enable();

    if (config_.pdelay_mode == PdelayMode::Active) {
        md_pdelay_req_.enable();
    }

    if (link_up) {
        on_link_up(now);
    }
}

void GptpSlavePort::stop() noexcept
{
    port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::AdministrativeDisable, TimePoint{});
    md_sync_receive_.disable();
    md_pdelay_req_.disable();
    port_announce_receive_.disable();

    sync_receipt_timeout_.reset();
    pdelay_interval_timer_.reset();
    pdelay_receipt_timeout_.reset();
    announce_receipt_timeout_.reset();
}

// =============================================================
// Observer subscription (slot-based, fixed capacity)
// =============================================================

auto GptpSlavePort::subscribe(Observer obs) -> SubscriptionId
{
    for (auto& slot : observers_) {
        if (slot.id == 0) {
            auto const id = next_subscription_id_++;
            if (next_subscription_id_ == 0) {
                next_subscription_id_ = 1;
            }
            slot.id = id;
            slot.obs = std::move(obs);
            return id;
        }
    }
    return 0;  // table full
}

void GptpSlavePort::unsubscribe(SubscriptionId id)
{
    if (id == 0) {
        return;
    }
    for (auto& slot : observers_) {
        if (slot.id == id) {
            slot.id = 0;
            slot.obs = Observer{};
            return;
        }
    }
}

// =============================================================
// Link state
// =============================================================

void GptpSlavePort::on_link_up(TimePoint now)
{
    port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::LinkUp, now);

    // In Automotive Profile with pre-asCapable, the port jumps
    // straight past Listening to Uncalibrated. The PortStateSM needs
    // two AsCapableAcquired events: one to move Initializing →
    // Listening, another to move Listening → Uncalibrated.
    if (config_.as_capable_initial) {
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::AsCapableAcquired, now);
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::AsCapableAcquired, now);
    }

    // Arm the first Pdelay interval (if Active).
    if (config_.pdelay_mode == PdelayMode::Active) {
        arm_pdelay_interval_timer(now);
    }

    // Arm the sync receipt timeout so we detect "no sync at all".
    arm_sync_receipt_timeout(now, config_.initial_log_sync_interval);
    arm_announce_receipt_timeout(now);
    publish_state_changes();
}

void GptpSlavePort::on_link_down(TimePoint now)
{
    port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::LinkDown, now);
    md_sync_receive_.disable();
    md_pdelay_req_.disable();
    servo_.reset();
    sync_receipt_timeout_.reset();
    pdelay_interval_timer_.reset();
    pdelay_receipt_timeout_.reset();
    announce_receipt_timeout_.reset();
    publish_state_changes();
}

// =============================================================
// Ingress
// =============================================================

void GptpSlavePort::receive_frame(std::span<uint8_t const> gptp_payload, int64_t rx_hw_timestamp_ns, TimePoint now)
{
    auto const parsed = parse_gptp(gptp_payload);
    if (!parsed.has_value()) {
        return;
    }

    // PHY delay compensation on the RX side.
    int64_t const rx_adjusted = rx_hw_timestamp_ns - get_phy_rx_delay_ns();

    std::visit(
        [&](auto const& msg) {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, SyncMessage>) {
                handle_sync_message(msg, rx_adjusted, now);
            } else if constexpr (std::is_same_v<T, FollowUpMessage>) {
                auto const trailing = (gptp_payload.size() > FollowUpMessage::LENGTH)
                    ? gptp_payload.subspan(FollowUpMessage::LENGTH)
                    : std::span<uint8_t const>{};
                handle_follow_up_message(msg, trailing, now);
            } else if constexpr (std::is_same_v<T, PdelayReqMessage>) {
                handle_pdelay_req_message(msg, rx_adjusted, now);
            } else if constexpr (std::is_same_v<T, PdelayRespMessage>) {
                handle_pdelay_resp_message(msg, rx_adjusted, now);
            } else if constexpr (std::is_same_v<T, PdelayRespFollowUpMessage>) {
                handle_pdelay_resp_follow_up_message(msg, now);
            } else if constexpr (std::is_same_v<T, AnnounceMessage>) {
                handle_announce_message(msg, now);
            } else if constexpr (std::is_same_v<T, SignalingMessage>) {
                auto const trailing = (gptp_payload.size() > SignalingMessage::FIXED_LENGTH)
                    ? gptp_payload.subspan(SignalingMessage::FIXED_LENGTH)
                    : std::span<uint8_t const>{};
                handle_signaling_message(msg, trailing, now);
            }
            // GptpTruncated, MessageHeader: silently ignored.
        },
        *parsed);
}

void GptpSlavePort::report_tx_timestamp(uint8_t message_type, uint16_t sequence_id, int64_t tx_ts_ns, TimePoint /*now*/)
{
    int64_t const tx_adjusted = tx_ts_ns + get_phy_tx_delay_ns();

    if (message_type == MESSAGE_TYPE_PDELAY_REQ) {
        md_pdelay_req_.on_pdelay_req_tx_timestamp(sequence_id, tx_adjusted);
    }
    // Future: handle other message types if needed.
}

// =============================================================
// Event loop
// =============================================================

auto GptpSlavePort::next_deadline() const noexcept -> TimePoint
{
    auto earliest = TimePoint::max();
    auto consider = [&](std::optional<TimePoint> const& tp) {
        if (tp.has_value() && *tp < earliest) {
            earliest = *tp;
        }
    };
    consider(sync_receipt_timeout_);
    consider(pdelay_interval_timer_);
    consider(pdelay_receipt_timeout_);
    consider(announce_receipt_timeout_);
    return earliest;
}

void GptpSlavePort::tick(TimePoint now)
{
    if (pdelay_interval_timer_.has_value() && *pdelay_interval_timer_ <= now) {
        pdelay_interval_timer_.reset();
        on_pdelay_interval_expired(now);
    }
    if (pdelay_receipt_timeout_.has_value() && *pdelay_receipt_timeout_ <= now) {
        pdelay_receipt_timeout_.reset();
        bool const prev_as_capable = md_pdelay_req_.is_as_capable();
        if (md_pdelay_req_.state() == MDPdelayReq::State::WaitingForPdelayResp) {
            md_pdelay_req_.on_pdelay_resp_receipt_timeout();
        } else if (md_pdelay_req_.state() == MDPdelayReq::State::WaitingForPdelayRespFollowUp) {
            md_pdelay_req_.on_pdelay_resp_follow_up_receipt_timeout();
        }
        // A crossed lost-response threshold drops asCapable -> tell the SM.
        sync_as_capable(prev_as_capable, now);
        // Re-arm the interval timer so the next Pdelay_Req is scheduled.
        // Without this a single lost Pdelay_Resp/Follow_Up permanently
        // halts the Pdelay engine (no retries; asCapable frozen), since
        // the interval timer is otherwise only re-armed on a successful
        // Follow_Up. The MDPdelayReq SM is in WaitingForPdelayIntervalTimer
        // after either timeout handler and expects the interval to tick on.
        if (config_.pdelay_mode == PdelayMode::Active) {
            arm_pdelay_interval_timer(now);
        }
    }
    if (sync_receipt_timeout_.has_value() && *sync_receipt_timeout_ <= now) {
        sync_receipt_timeout_.reset();
        on_sync_receipt_timeout_expired(now);
    }
    if (announce_receipt_timeout_.has_value() && *announce_receipt_timeout_ <= now) {
        announce_receipt_timeout_.reset();
        on_announce_receipt_timeout_expired(now);
    }
    publish_state_changes();
}

// =============================================================
// Query API
// =============================================================

auto GptpSlavePort::is_synchronized() const noexcept -> bool
{
    return port_state_ctx_.synced;
}

auto GptpSlavePort::as_capable() const noexcept -> bool
{
    return port_state_ctx_.as_capable;
}

auto GptpSlavePort::grandmaster_identity() const noexcept -> tsn::ClockIdentity
{
    return port_announce_receive_.current_grandmaster_identity();
}

auto GptpSlavePort::grandmaster_info() const noexcept -> std::optional<GrandmasterInfo> const&
{
    return port_announce_receive_.current();
}

auto GptpSlavePort::current_port_state() const noexcept -> port_state_sm::Def::State
{
    return port_state_sm_.current_state();
}

// =============================================================
// Message handlers
// =============================================================

void GptpSlavePort::handle_sync_message(SyncMessage const& msg, int64_t rx_hw_timestamp_ns, TimePoint now)
{
    md_sync_receive_.on_sync(msg, rx_hw_timestamp_ns);
    // Rearm the sync receipt timeout from the advertised interval.
    arm_sync_receipt_timeout(now, static_cast<int8_t>(msg.header.log_message_interval));
}

void GptpSlavePort::handle_follow_up_message(FollowUpMessage const& msg, std::span<uint8_t const> trailing_bytes, TimePoint now)
{
    auto const indication = md_sync_receive_.on_follow_up(msg, trailing_bytes);
    if (!indication.has_value()) {
        return;
    }
    // Feed the servo.
    auto const out = servo_.process(*indication, mean_link_delay_ns_, neighbor_rate_ratio_);

    // Apply clock corrections.
    if (out.phase_jump_ns.has_value() && ops_.adjust_phase_ns) {
        ops_.adjust_phase_ns(*out.phase_jump_ns);
    }
    if (out.frequency_adjust_ppb.has_value() && ops_.adjust_frequency_ppb) {
        ops_.adjust_frequency_ppb(*out.frequency_adjust_ppb);
    }

    // Update latched query values.
    last_master_offset_ns_ = out.master_offset_ns;
    last_rate_ratio_ = out.rate_ratio;

    // Update SyncQuality telemetry on successful pairing.
    sync_quality_.last_valid_sync_local = now;
    sync_quality_.last_master_offset_ns = out.master_offset_ns;
    sync_quality_.last_rate_ratio = out.rate_ratio;
    sync_quality_.gm_time_base_indicator = indication->gm_time_base_indicator;
    sync_quality_.consecutive_missed_syncs = 0;

    // If this is the first successful servo pass, promote the port to Slave.
    if (!port_state_ctx_.synced && port_state_sm_.current_state() == port_state_sm::Def::State::Uncalibrated) {
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::FirstSyncLocked, now);
    }

    // Observer callbacks.
    notify_sync_update(out.master_offset_ns, out.rate_ratio);
    notify_anchor_update(indication->sync_rx_local_ns, out.corrected_master_ns, out.rate_ratio);
    notify_sync_quality(sync_quality_);
    publish_state_changes();
}

void GptpSlavePort::handle_pdelay_req_message(PdelayReqMessage const& msg, int64_t rx_hw_timestamp_ns, TimePoint /*now*/)
{
    if (config_.pdelay_mode == PdelayMode::Disabled) {
        return;
    }

    // Convert the HW RX timestamp into a PTP Timestamp for the response.
    uint64_t const rx_secs = static_cast<uint64_t>(rx_hw_timestamp_ns / 1'000'000'000LL);
    uint32_t const rx_nsec = static_cast<uint32_t>(rx_hw_timestamp_ns % 1'000'000'000LL);

    // -- Step 1: Send Pdelay_Resp with twoStep flag --
    PdelayRespMessage resp{};
    resp.init(msg.header.sequence_id);
    resp.header.flags.set_flag(0x0200);  // twoStep
    resp.header.source_port_identity = SourcePortIdentity{config_.local_clock_identity, config_.local_port_number};
    resp.header.log_message_interval = 0x7F;  // logMessageInterval = 0x7F per 802.1AS
    resp.request_receipt_timestamp = Timestamp{rx_secs, rx_nsec};
    resp.requesting_port_identity = msg.header.source_port_identity;

    std::array<uint8_t, PdelayRespMessage::LENGTH> resp_buf{};
    (void)store_unchecked(std::span<uint8_t>(resp_buf), resp);

    TxResult resp_tx{};
    if (ops_.send_frame) {
        resp_tx = ops_.send_frame(std::span<uint8_t const>(resp_buf));
    }

    // -- Step 2: Send Pdelay_Resp_Follow_Up with the TX timestamp --
    PdelayRespFollowUpMessage fup{};
    fup.init(msg.header.sequence_id);
    fup.header.source_port_identity = SourcePortIdentity{config_.local_clock_identity, config_.local_port_number};
    fup.header.log_message_interval = 0x7F;
    fup.requesting_port_identity = msg.header.source_port_identity;

    if (resp_tx.ok) {
        int64_t const tx_adjusted = resp_tx.tx_timestamp_ns + get_phy_tx_delay_ns();
        uint64_t const tx_secs = static_cast<uint64_t>(tx_adjusted / 1'000'000'000LL);
        uint32_t const tx_nsec = static_cast<uint32_t>(tx_adjusted % 1'000'000'000LL);
        fup.response_origin_timestamp = Timestamp{tx_secs, tx_nsec};
    }

    std::array<uint8_t, PdelayRespFollowUpMessage::LENGTH> fup_buf{};
    (void)store_unchecked(std::span<uint8_t>(fup_buf), fup);

    if (ops_.send_frame) {
        (void)ops_.send_frame(std::span<uint8_t const>(fup_buf));
    }
}

void GptpSlavePort::handle_pdelay_resp_message(PdelayRespMessage const& msg, int64_t rx_hw_timestamp_ns, TimePoint now)
{
    if (config_.pdelay_mode != PdelayMode::Active) {
        return;
    }
    md_pdelay_req_.on_pdelay_resp(msg, rx_hw_timestamp_ns);
    // Arm the Pdelay_Resp_Follow_Up receipt timeout (same duration
    // as the overall response timeout).
    pdelay_receipt_timeout_ = now + log2_interval_to_ns(config_.initial_log_pdelay_interval);
}

void GptpSlavePort::handle_pdelay_resp_follow_up_message(PdelayRespFollowUpMessage const& msg, TimePoint now)
{
    if (config_.pdelay_mode != PdelayMode::Active) {
        return;
    }
    bool const prev_as_capable = md_pdelay_req_.is_as_capable();
    auto const meas = md_pdelay_req_.on_pdelay_resp_follow_up(msg);
    if (meas.has_value()) {
        int64_t const prev_delay = mean_link_delay_ns_;

        mean_link_delay_ns_ = meas->mean_link_delay_ns;
        neighbor_rate_ratio_ = meas->neighbor_rate_ratio;

        // Notify if link delay or asCapable changed.
        if (mean_link_delay_ns_ != prev_delay || neighbor_rate_ratio_ != meas->neighbor_rate_ratio) {
            notify_peer_delay_update(mean_link_delay_ns_, neighbor_rate_ratio_);
        }

    }
    // Reconcile the PortStateSM to any asCapable change from this exchange:
    // acquired on the first success, or lost if the measured link delay exceeded
    // the neighbor-prop-delay threshold (802.1AS 11.2.2).
    sync_as_capable(prev_as_capable, now);
    // Cancel the receipt timeout — exchange completed (or failed cleanly).
    pdelay_receipt_timeout_.reset();
    // Re-arm the interval timer for the next exchange.
    arm_pdelay_interval_timer(now);
    publish_state_changes();
}

void GptpSlavePort::sync_as_capable(bool const prev_as_capable, TimePoint const now)
{
    bool const now_as_capable = md_pdelay_req_.is_as_capable();
    if (now_as_capable == prev_as_capable) {
        return;
    }
    if (now_as_capable) {
        // false -> true: two events to traverse Initializing -> Listening ->
        // Uncalibrated (the SM needs both so it does not get stuck in Listening).
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::AsCapableAcquired, now);
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::AsCapableAcquired, now);
    } else {
        // true -> false: too many lost Pdelay responses, or the measured link
        // delay exceeded the neighbor-prop-delay threshold (802.1AS 11.2.2).
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::AsCapableLost, now);
    }
}

void GptpSlavePort::handle_announce_message(AnnounceMessage const& msg, TimePoint now)
{
    bool const gm_changed = port_announce_receive_.on_announce(msg);
    arm_announce_receipt_timeout(now);
    if (gm_changed) {
        notify_grandmaster_change(port_announce_receive_.current_grandmaster_identity());
    }
}

void GptpSlavePort::handle_signaling_message(
    SignalingMessage const& /*msg*/, std::span<uint8_t const> trailing_bytes, TimePoint /*now*/)
{
    // Parse the MessageIntervalRequest TLV if present (Automotive
    // Profile). Interval changes are captured for diagnostic
    // reporting but do NOT dynamically alter our config — the
    // intervals are fixed at construction.
    if (trailing_bytes.size() >= MessageIntervalRequestTLV::LENGTH) {
        MessageIntervalRequestTLV tlv{};
        (void)load_unchecked(trailing_bytes.first(MessageIntervalRequestTLV::LENGTH), &tlv);
        if (tlv.is_valid()) {
            // Future: apply interval changes if dynamic signaling
            // is enabled (GptpConfig flag). For now: informational.
        }
    }
}

// =============================================================
// Timer helpers
// =============================================================

void GptpSlavePort::arm_pdelay_interval_timer(TimePoint now)
{
    pdelay_interval_timer_ = now + log2_interval_to_ns(config_.oper_log_pdelay_interval);
}

void GptpSlavePort::arm_sync_receipt_timeout(TimePoint now, int8_t log_sync_interval)
{
    auto const interval = log2_interval_to_ns(log_sync_interval);
    sync_receipt_timeout_ = now + interval * config_.sync_receipt_timeout_multiplier;
}

void GptpSlavePort::arm_announce_receipt_timeout(TimePoint now)
{
    auto const interval = log2_interval_to_ns(config_.initial_log_announce_interval);
    announce_receipt_timeout_ = now + interval * config_.announce_receipt_timeout_multiplier;
}

void GptpSlavePort::on_pdelay_interval_expired(TimePoint now)
{
    // A stale in-flight exchange is failed here (may cross the lost-response
    // threshold and drop asCapable), so reconcile the SM afterwards.
    bool const prev_as_capable = md_pdelay_req_.is_as_capable();
    auto const seq_opt = md_pdelay_req_.pdelay_interval_timer_expired();
    sync_as_capable(prev_as_capable, now);
    if (!seq_opt.has_value()) {
        return;
    }
    // Build and send a Pdelay_Req.
    PdelayReqMessage req{};
    req.init(*seq_opt);
    req.header.source_port_identity = SourcePortIdentity{config_.local_clock_identity, config_.local_port_number};

    std::array<uint8_t, PdelayReqMessage::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), req);

    if (ops_.send_frame) {
        auto const result = ops_.send_frame(std::span<uint8_t const>(buf));
        if (result.ok) {
            int64_t const tx_adjusted = result.tx_timestamp_ns + get_phy_tx_delay_ns();
            md_pdelay_req_.on_pdelay_req_tx_timestamp(*seq_opt, tx_adjusted);
        }
    }

    // Arm a receipt timeout in case the peer never responds.
    pdelay_receipt_timeout_ = now + log2_interval_to_ns(config_.oper_log_pdelay_interval);
}

void GptpSlavePort::on_sync_receipt_timeout_expired(TimePoint now)
{
    md_sync_receive_.on_sync_receipt_timeout();
    // Bump freshness telemetry. Note: last_valid_sync_local is
    // intentionally NOT cleared — the app computes (now - last_valid)
    // itself and decides whether the gap is still acceptable for its
    // domain (see SyncQuality docs).
    if (sync_quality_.consecutive_missed_syncs < UINT32_MAX) {
        ++sync_quality_.consecutive_missed_syncs;
    }
    // Signal sync loss if we were synchronized.
    if (port_state_ctx_.synced) {
        port_state_sm_.handle_event(port_state_ctx_, port_state_sm::Def::Event::SyncLost, now);
        servo_.reset();
    }
    // Re-arm so we keep detecting sync recovery.
    arm_sync_receipt_timeout(now, config_.oper_log_sync_interval);
    notify_sync_quality(sync_quality_);
    publish_state_changes();
}

void GptpSlavePort::on_announce_receipt_timeout_expired(TimePoint now)
{
    bool const had_gm = port_announce_receive_.on_announce_receipt_timeout();
    if (had_gm) {
        notify_grandmaster_change(tsn::ClockIdentity{});
    }
    arm_announce_receipt_timeout(now);
}

// =============================================================
// PHY delay lookup
// =============================================================

auto GptpSlavePort::get_phy_rx_delay_ns() const noexcept -> int64_t
{
    if (!ops_.get_link_speed) {
        return config_.phy_delay_1g.rx_ns;
    }
    switch (ops_.get_link_speed()) {
        case LinkSpeedMbps::Mbps10:
            return config_.phy_delay_10m.rx_ns;
        case LinkSpeedMbps::Mbps100:
            return config_.phy_delay_100m.rx_ns;
        case LinkSpeedMbps::Mbps1000:
            return config_.phy_delay_1g.rx_ns;
        default:
            return config_.phy_delay_1g.rx_ns;
    }
}

auto GptpSlavePort::get_phy_tx_delay_ns() const noexcept -> int64_t
{
    if (!ops_.get_link_speed) {
        return config_.phy_delay_1g.tx_ns;
    }
    switch (ops_.get_link_speed()) {
        case LinkSpeedMbps::Mbps10:
            return config_.phy_delay_10m.tx_ns;
        case LinkSpeedMbps::Mbps100:
            return config_.phy_delay_100m.tx_ns;
        case LinkSpeedMbps::Mbps1000:
            return config_.phy_delay_1g.tx_ns;
        default:
            return config_.phy_delay_1g.tx_ns;
    }
}

// =============================================================
// Observer helpers
// =============================================================

void GptpSlavePort::notify_sync_state_change(bool synchronized)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_sync_state_change) {
            slot.obs.on_sync_state_change(synchronized);
        }
    }
}

void GptpSlavePort::notify_sync_update(int64_t master_offset_ns, double rate_ratio)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_sync_update) {
            slot.obs.on_sync_update(master_offset_ns, rate_ratio);
        }
    }
}

void GptpSlavePort::notify_anchor_update(int64_t rx_local_ns, int64_t tx_master_ns, double rate_ratio)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_anchor_update) {
            slot.obs.on_anchor_update(rx_local_ns, tx_master_ns, rate_ratio);
        }
    }
}

void GptpSlavePort::notify_grandmaster_change(tsn::ClockIdentity const& gm_identity)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_grandmaster_change) {
            slot.obs.on_grandmaster_change(gm_identity);
        }
    }
}

void GptpSlavePort::notify_peer_delay_update(int64_t link_delay_ns, double rate_ratio)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_peer_delay_update) {
            slot.obs.on_peer_delay_update(link_delay_ns, rate_ratio);
        }
    }
}

void GptpSlavePort::notify_as_capable_change(bool as_capable)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_as_capable_change) {
            slot.obs.on_as_capable_change(as_capable);
        }
    }
}

void GptpSlavePort::notify_sync_quality(SyncQuality const& quality)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_sync_quality) {
            slot.obs.on_sync_quality(quality);
        }
    }
}

void GptpSlavePort::publish_state_changes()
{
    bool const now_synced = port_state_ctx_.synced;
    bool const now_as_capable = port_state_ctx_.as_capable;

    if (now_synced != synchronized_published_) {
        synchronized_published_ = now_synced;
        notify_sync_state_change(now_synced);
    }
    if (now_as_capable != as_capable_published_) {
        as_capable_published_ = now_as_capable;
        notify_as_capable_change(now_as_capable);
    }
}

}  // namespace statusbar::gptp
