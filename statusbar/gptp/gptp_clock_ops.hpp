#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// GptpClockOps — user-supplied clock and timestamping operations.
//
// This interface is the sole contact point between the gPTP slave
// follower core and the underlying hardware/OS. The state machines
// depend only on this struct; they never call platform APIs directly.
//
// This layering lets the same core code run on Linux (raw packet
// socket + SO_TIMESTAMPING + PHC via clock_adjtime), on bare metal
// with a custom PTP hardware clock, or on software-only systems for
// testing.
//
// Every field is an inplace_function so the caller can bind lambdas
// capturing their own state (e.g. a PHC fd, a ring buffer of tx
// timestamp requests, a soft-clock driver state).
//

#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <span>

namespace statusbar::gptp {

/// Hardware link speed reported by the NIC, in Mbps. Used by the
/// follower to look up PHY delay compensation in GptpConfig. Return
/// 0 if the platform cannot determine the link speed; the port will
/// fall back to the 1 Gbps PHY delay defaults in that case.
enum class LinkSpeedMbps : uint32_t
{
    Unknown = 0,
    Mbps10 = 10,
    Mbps100 = 100,
    Mbps1000 = 1000,
    Mbps10000 = 10000,
};

/// Result of an attempt to transmit a gPTP frame.
struct TxResult
{
    /// True if the frame was successfully handed to the network and
    /// a hardware TX timestamp was captured.
    bool ok{false};

    /// Hardware TX timestamp in local clock nanoseconds. Valid iff
    /// `ok == true`. If the implementation can't provide a
    /// synchronous TX timestamp, it must block inside `send_frame`
    /// until the timestamp is available, OR set ok=false and arrange
    /// to deliver the timestamp later via
    /// GptpSlavePort::report_tx_timestamp().
    int64_t tx_timestamp_ns{0};
};

/// User-supplied clock / timestamping interface for GptpSlavePort.
///
/// All functions are inplace_function-based to make the callbacks
/// trivially lambda-friendly — the user binds them to whatever
/// underlying implementation (Linux raw socket + PHC, bare-metal
/// hardware timer, software-only for tests, etc.).
///
/// None of the callbacks may throw. All are called from the
/// participant's single event-loop thread.
/// Callback types for GptpClockOps — each is a separate alias so
/// call-sites can override capacity individually if needed.
using GetLocalTimeNsFn = statusbar::sg14::inplace_function<int64_t(), 64>;
using SendGptpFrameFn = statusbar::sg14::inplace_function<TxResult(std::span<uint8_t const>), 64>;
using AdjustPhaseNsFn = statusbar::sg14::inplace_function<void(int64_t), 64>;
using AdjustFrequencyPpbFn = statusbar::sg14::inplace_function<void(double), 64>;
using GetLinkSpeedFn = statusbar::sg14::inplace_function<LinkSpeedMbps(), 64>;

/// User-supplied clock / timestamping interface for GptpSlavePort.
///
/// None of the callbacks may throw. All are called from the
/// participant's single event-loop thread.
struct GptpClockOps
{
    /// Return the current nanosecond timestamp of the local clock
    /// being disciplined by this follower. On Linux this is typically
    /// `clock_gettime(CLOCK_TAI)` on the disciplined system clock, or
    /// a PHC read. On bare metal it's the timer peripheral being
    /// steered by this follower. The unit is nanoseconds since an
    /// implementation-defined epoch; only deltas matter, so the
    /// epoch itself is free.
    GetLocalTimeNsFn get_local_time_ns;

    /// Transmit a gPTP frame (14-byte Ethernet header + gPTP
    /// payload). The implementation is responsible for adding the
    /// Ethernet framing (destination MAC = 01:80:C2:00:00:0E,
    /// source MAC = local NIC address, EtherType 0x88F7).
    ///
    /// The returned TxResult must contain the hardware TX timestamp
    /// of the egress frame in the same nanosecond clock as
    /// get_local_time_ns(). If the TX timestamp cannot be supplied
    /// synchronously, set ok=false and arrange a later call to
    /// GptpSlavePort::report_tx_timestamp(msg_type, seq_id,
    /// tx_ts_ns).
    SendGptpFrameFn send_frame;

    /// Adjust the local clock's phase by `phase_ns` (positive =
    /// advance the clock). Called by the servo when the phase error
    /// exceeds the configured jump threshold for the configured
    /// number of consecutive samples. On Linux this is
    /// clock_adjtime with ADJ_SETOFFSET; on bare metal it's a
    /// hardware timer counter load.
    AdjustPhaseNsFn adjust_phase_ns;

    /// Adjust the local clock's frequency offset. Units: parts per
    /// billion (positive = speed up). The servo clamps to
    /// ±config.servo_ppm_limit × 1000 before calling. On Linux this
    /// is clock_adjtime with ADJ_FREQUENCY on the PHC; on bare metal
    /// it's a clock multiplier/divider control register.
    AdjustFrequencyPpbFn adjust_frequency_ppb;

    /// Current link speed reported by the MAC. Used to look up PHY
    /// delay compensation values in GptpConfig. Returning
    /// LinkSpeedMbps::Unknown causes the port to fall back to the
    /// 1 Gbps PHY delay default.
    GetLinkSpeedFn get_link_speed;
};

/// Validate that all required fields of a GptpClockOps are bound.
/// Returns true if every callback is set; returns false if any
/// required callback is empty (in which case GptpSlavePort
/// construction will fail).
[[nodiscard]] inline auto is_complete(GptpClockOps const& ops) noexcept -> bool
{
    return static_cast<bool>(ops.get_local_time_ns) && static_cast<bool>(ops.send_frame) &&
        static_cast<bool>(ops.adjust_phase_ns) && static_cast<bool>(ops.adjust_frequency_ppb) &&
        static_cast<bool>(ops.get_link_speed);
}

}  // namespace statusbar::gptp
