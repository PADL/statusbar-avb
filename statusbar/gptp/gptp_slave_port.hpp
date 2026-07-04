#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// GptpSlavePort — per-NIC IEEE 802.1AS slave-role follower.
//
// Orchestrates the five protocol components:
//   - PortStateSM          (gptp_port_state_sm.hpp)
//   - MDSyncReceive        (gptp_md_sync_receive.hpp)
//   - MDPdelayReq          (gptp_md_pdelay_req.hpp) — only when pdelay_mode == Active
//   - PortAnnounceReceive  (gptp_port_announce_receive.hpp)
//   - ServoLoop            (gptp_servo.hpp)
//
// Exposes the complete slave-follower API: start/stop, observer
// subscription, frame ingress, link up/down, tick / next_deadline,
// and query methods. Follows the same in-process pattern as
// MsrpParticipant: config-driven fixed capacities, zero-allocation
// steady state, std::function-based Observer callbacks.
//

#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/gptp/gptp_clock_ops.hpp"
#include "statusbar/gptp/gptp_config.hpp"
#include "statusbar/gptp/gptp_error.hpp"
#include "statusbar/gptp/gptp_md_pdelay_req.hpp"
#include "statusbar/gptp/gptp_md_sync_receive.hpp"
#include "statusbar/gptp/gptp_messages.hpp"
#include "statusbar/gptp/gptp_port_announce_receive.hpp"
#include "statusbar/gptp/gptp_port_state_sm.hpp"
#include "statusbar/gptp/gptp_servo.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace statusbar::gptp {

using sm::TimePoint;

/// Sync freshness / quality telemetry. Deliberately reports raw
/// freshness rather than a library-side pass/fail verdict — the
/// application converts time-since-last-valid-sync plus its known
/// local oscillator ppm into a worst-case accuracy bound and decides
/// its own tolerance band (e.g. < 1 µs for 96 kHz audio, ≤ 200 µs for
/// many show-control uses). The library never drops asCapable or
/// declares sync "lost" based on this struct; it only reports.
///
/// GM identity / clockClass / clockAccuracy / variance are not
/// duplicated here — query grandmaster_info() for those. Only
/// gm_time_base_indicator is included because it rides the FollowUp
/// Information TLV, not Announce.
struct SyncQuality
{
    /// Monotonic TimePoint at which the most recent Sync+FollowUp
    /// pair was successfully processed. std::nullopt until the first
    /// successful pairing. Monotonic (not PTP time) so the app can
    /// compute freshness as (now - *last_valid_sync_local) robustly
    /// across PTP clock step adjustments.
    std::optional<TimePoint> last_valid_sync_local{};

    /// Latest master-to-local phase error in ns (positive = local behind).
    int64_t last_master_offset_ns{0};

    /// Latest master-to-local rate ratio estimate.
    double last_rate_ratio{1.0};

    /// From the last FollowUp Information TLV. Changes when the GM's
    /// local time base discontinuously jumps.
    uint16_t gm_time_base_indicator{0};

    /// Count of SyncReceiptTimeout firings since the last successful
    /// Sync+FollowUp pair. Reset to 0 on each successful pair.
    uint32_t consecutive_missed_syncs{0};
};

/// Nanoseconds elapsed since the last successful Sync+FollowUp pair.
/// Returns std::nullopt if no valid sync has ever been observed.
[[nodiscard]] inline auto time_since_last_valid_sync_ns(SyncQuality const& q, TimePoint now) noexcept -> std::optional<int64_t>
{
    if (!q.last_valid_sync_local.has_value()) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now - *q.last_valid_sync_local).count();
}

/// Observer callback surface. Every callback is a nullable
/// std::function so subscribers can opt in to only the events they
/// care about. Called synchronously from within receive_frame() or
/// tick() on the caller's thread.
struct Observer
{
    /// Fires on transitions between synchronized and unsynchronized.
    /// Payload is the new state (true = sync locked, false = sync lost).
    statusbar::sg14::inplace_function<void(bool synchronized), 64> on_sync_state_change;

    /// Fires on every successful Sync+FollowUp pair once the servo
    /// has run. master_offset_ns is the master-to-local phase error
    /// (positive = local is behind), rate_ratio is the master-to-local
    /// clock rate ratio estimate.
    statusbar::sg14::inplace_function<void(int64_t master_offset_ns, double rate_ratio), 64> on_sync_update;

    /// Fires on every successful Sync+FollowUp pair WITH the precise
    /// hardware-stamped anchor pair. `rx_local_ns` is the local-clock
    /// HW timestamp at Sync arrival (already RX-PHY corrected, raw PHC
    /// ns in passthrough mode, master-disciplined PHC in legacy mode).
    /// `tx_master_ns` is the grandmaster's preciseOriginTimestamp from
    /// the FollowUp body, with the correctionField applied (master TAI
    /// at Sync egress). `rate_ratio` is the same servo estimate as
    /// on_sync_update. Consumers that need the exact (local, master)
    /// pair (e.g. for a bridge anchor used in HW-precise time-domain
    /// conversion) read it here instead of sampling get_local_time_ns
    /// at callback time.
    statusbar::sg14::inplace_function<void(int64_t rx_local_ns, int64_t tx_master_ns, double rate_ratio), 64> on_anchor_update;

    /// Fires when the grandmaster identity in the most recent Announce
    /// differs from the previously latched identity (or when the
    /// first Announce is received). The passed identity is the
    /// grandmaster's ClockIdentity; an all-zero value indicates the
    /// grandmaster was lost (announce receipt timeout).
    statusbar::sg14::inplace_function<void(tsn::ClockIdentity const& gm_identity), 64> on_grandmaster_change;

    /// Fires when the computed meanLinkDelay or neighborRateRatio
    /// updates (after a successful Pdelay exchange).
    statusbar::sg14::inplace_function<void(int64_t mean_link_delay_ns, double neighbor_rate_ratio), 64> on_peer_delay_update;

    /// Fires on transitions between asCapable and !asCapable.
    statusbar::sg14::inplace_function<void(bool as_capable), 64> on_as_capable_change;

    /// Fires on every SyncQuality update — i.e. on each successful
    /// Sync+FollowUp pair AND on each SyncReceiptTimeout expiry.
    /// This is the primary telemetry for applications that want to
    /// apply their own accuracy / freshness policy rather than react
    /// to the library's asCapable / synchronized booleans.
    statusbar::sg14::inplace_function<void(SyncQuality const& quality), 64> on_sync_quality;
};

class GptpSlavePort
{
  public:
    using SubscriptionId = uint32_t;

    /// Construct with an explicit config and a set of clock
    /// operations. Both are validated; invalid inputs throw
    /// std::system_error with a GptpError code.
    /// @param memory_resource Memory resource for the observer slot
    ///        vector. nullptr is treated as std::pmr::get_default_resource().
    GptpSlavePort(GptpConfig const& config, GptpClockOps ops, std::pmr::memory_resource* memory_resource = nullptr);

    /// Start the port. Fires the PortStateSM's LinkUp event if
    /// `link_up` is true. Arms the Pdelay interval timer (in Active
    /// mode) and the sync-receipt timeout monitoring.
    void start(TimePoint now, bool link_up = true);

    /// Stop the port. All FSMs transition to Disabled / Discard.
    void stop() noexcept;

    // -------------------- Observer subscription --------------------

    [[nodiscard]] auto subscribe(Observer obs) -> SubscriptionId;
    void unsubscribe(SubscriptionId id);

    // -------------------- Link state --------------------

    void on_link_up(TimePoint now);
    void on_link_down(TimePoint now);

    // -------------------- Ingress --------------------

    /// Parse and process an incoming gPTP frame.
    ///
    /// @param gptp_payload      The gPTP payload (after the Ethernet
    ///                          header; starts with the common PTP
    ///                          message header at byte 0).
    /// @param rx_hw_timestamp_ns Hardware RX timestamp of the frame in
    ///                          the same clock as GptpClockOps::get_local_time_ns.
    /// @param now               Monotonic TimePoint for timer scheduling.
    void receive_frame(std::span<uint8_t const> gptp_payload, int64_t rx_hw_timestamp_ns, TimePoint now);

    /// Deferred-path for hardware that cannot return the TX
    /// timestamp synchronously from GptpClockOps::send_frame.
    /// Feed back a previously-sent Pdelay_Req's TX timestamp here
    /// once the hardware provides it.
    void report_tx_timestamp(uint8_t message_type, uint16_t sequence_id, int64_t tx_ts_ns, TimePoint now);

    // -------------------- Event loop --------------------

    /// Check timers and process any expired ones. Call from the
    /// owning event loop after next_deadline() has been reached.
    void tick(TimePoint now);

    /// Earliest upcoming timer deadline across all scheduled timers
    /// on this port. Returns TimePoint::max() if nothing is scheduled.
    [[nodiscard]] auto next_deadline() const noexcept -> TimePoint;

    // -------------------- Query API --------------------

    [[nodiscard]] auto is_synchronized() const noexcept -> bool;
    [[nodiscard]] auto last_master_offset_ns() const noexcept -> int64_t { return last_master_offset_ns_; }
    [[nodiscard]] auto last_rate_ratio() const noexcept -> double { return last_rate_ratio_; }
    [[nodiscard]] auto mean_link_delay_ns() const noexcept -> int64_t { return mean_link_delay_ns_; }
    [[nodiscard]] auto neighbor_rate_ratio() const noexcept -> double { return neighbor_rate_ratio_; }
    [[nodiscard]] auto as_capable() const noexcept -> bool;
    [[nodiscard]] auto grandmaster_identity() const noexcept -> tsn::ClockIdentity;
    [[nodiscard]] auto grandmaster_info() const noexcept -> std::optional<GrandmasterInfo> const&;
    [[nodiscard]] auto current_port_state() const noexcept -> port_state_sm::Def::State;
    [[nodiscard]] auto current_ppm() const noexcept -> double { return servo_.current_ppm(); }
    [[nodiscard]] auto sync_quality() const noexcept -> SyncQuality const& { return sync_quality_; }

  private:
    //
    // Observer slot for fixed-capacity vector (mirrors the MRP style).
    //
    struct ObserverSlot
    {
        SubscriptionId id{0};
        Observer obs{};
    };

    // --- Member layout note ---------------------------------------
    // Fields are ordered to minimize padding per
    // clang-analyzer-optin.performance.Padding. Init-order constraint:
    // md_pdelay_req_ and servo_ both read from config_ in their
    // constructor initializers, so config_ must come first.
    //
    GptpConfig config_;
    GptpClockOps ops_;

    //
    // Core components
    //
    port_state_sm::Machine port_state_sm_{};
    port_state_sm::Context port_state_ctx_{};

    MDSyncReceive md_sync_receive_{};
    MDPdelayReq md_pdelay_req_;
    PortAnnounceReceive port_announce_receive_{};
    ServoLoop servo_;

    //
    // Observers (fixed capacity, slot-based)
    //
    std::pmr::vector<ObserverSlot> observers_;

    //
    // Timer deadlines (std::optional == not scheduled)
    //
    std::optional<TimePoint> sync_receipt_timeout_{};
    std::optional<TimePoint> pdelay_interval_timer_{};
    std::optional<TimePoint> pdelay_receipt_timeout_{};
    std::optional<TimePoint> announce_receipt_timeout_{};

    //
    // Latched measurements (read via query API)
    //
    int64_t last_master_offset_ns_{0};
    double last_rate_ratio_{1.0};
    int64_t mean_link_delay_ns_{0};
    double neighbor_rate_ratio_{1.0};
    SyncQuality sync_quality_{};

    //
    // Small scalars packed together at the end to avoid padding gaps.
    //
    SubscriptionId next_subscription_id_{1};  // uint32_t
    uint16_t next_pdelay_seq_id_{1};
    bool as_capable_published_{false};
    bool synchronized_published_{false};

    // =========================================================
    // Internal message handlers
    // =========================================================

    void handle_sync_message(SyncMessage const& msg, int64_t rx_hw_timestamp_ns, TimePoint now);
    void handle_follow_up_message(FollowUpMessage const& msg, std::span<uint8_t const> trailing_bytes, TimePoint now);
    void handle_pdelay_req_message(PdelayReqMessage const& msg, int64_t rx_hw_timestamp_ns, TimePoint now);
    void handle_pdelay_resp_message(PdelayRespMessage const& msg, int64_t rx_hw_timestamp_ns, TimePoint now);
    void handle_pdelay_resp_follow_up_message(PdelayRespFollowUpMessage const& msg, TimePoint now);
    /// Dispatch the two AsCapableAcquired events the PortStateSM needs to traverse
    /// Initializing -> Listening -> Uncalibrated on a false->true asCapable edge. A
    /// single event only reaches Listening; a second is required or the port sticks
    /// there. Named + centralized so the two call sites don't look like a stray
    /// duplicate handle_event().
    void drive_as_capable_acquired(TimePoint now);
    /// Reconcile the PortStateSM to MDPdelayReq's asCapable after a Pdelay event:
    /// given the value BEFORE the event, dispatch AsCapableAcquired (x2) on a
    /// false->true edge or AsCapableLost on true->false. No-op if unchanged.
    void sync_as_capable(bool prev_as_capable, TimePoint now);
    void handle_announce_message(AnnounceMessage const& msg, TimePoint now);
    void handle_signaling_message(SignalingMessage const& msg, std::span<uint8_t const> trailing_bytes, TimePoint now);
    /// True if a Sync/FollowUp from @p src should be accepted: verification off, no
    /// grandmaster latched yet, or @p src matches the latched grandmaster's source
    /// port. Drops Sync/FollowUp injected by a rogue/second master (config
    /// verify_source_port_identity).
    [[nodiscard]] auto source_is_grandmaster(SourcePortIdentity const& src) const noexcept -> bool;

    // =========================================================
    // Timer helpers
    // =========================================================

    void arm_pdelay_interval_timer(TimePoint now);
    void arm_sync_receipt_timeout(TimePoint now, int8_t log_sync_interval);
    void arm_announce_receipt_timeout(TimePoint now);

    void on_pdelay_interval_expired(TimePoint now);
    void on_sync_receipt_timeout_expired(TimePoint now);
    void on_announce_receipt_timeout_expired(TimePoint now);

    // =========================================================
    // PHY delay lookup
    // =========================================================

    [[nodiscard]] auto get_phy_rx_delay_ns() const noexcept -> int64_t;
    [[nodiscard]] auto get_phy_tx_delay_ns() const noexcept -> int64_t;

    // =========================================================
    // Observer helpers
    // =========================================================

    void notify_sync_state_change(bool synchronized);
    void notify_sync_update(int64_t master_offset_ns, double rate_ratio);
    void notify_anchor_update(int64_t rx_local_ns, int64_t tx_master_ns, double rate_ratio);
    void notify_grandmaster_change(tsn::ClockIdentity const& gm_identity);
    void notify_peer_delay_update(int64_t link_delay_ns, double rate_ratio);
    void notify_as_capable_change(bool as_capable);
    void notify_sync_quality(SyncQuality const& quality);

    // Recompute & publish asCapable / synced changes.
    void publish_state_changes();
};

}  // namespace statusbar::gptp
