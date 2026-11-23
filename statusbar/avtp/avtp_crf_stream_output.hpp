#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// CRF stream output (talker/transmitter) — clock reference generation
///
/// Provides CrfStreamOutputContext for generating CRF packets with timestamps
/// derived from the current gPTP time and the configured media clock frequency.
///
/// References:
///   IEEE Std 1722-2016 Section 10 (Clock Reference Format)

#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"

#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// Per-stream state for CRF serialization (talker/transmitter side).
/// One instance per transmitted CRF stream.
///
/// Generates CRF packets with evenly-spaced timestamps based on the configured
/// media clock frequency. The caller provides the current gPTP time; the context
/// computes the timestamp values.
struct CrfStreamOutputContext
{
    // Config
    CrfType type;
    uint32_t base_frequency;
    CrfPull pull;
    uint16_t timestamp_interval;
    uint16_t timestamps_per_packet;
    StreamId stream_id;

    // Running state
    uint8_t sequence_num{0};
    uint64_t next_timestamp_ns{0};  ///< Next timestamp value to emit
    bool started{false};

    // Statistics
    uint32_t packets_sent{0};
    uint64_t total_timestamps_sent{0};

    /// Construct with CRF parameters
    /// @param sid          The stream ID for this CRF talker stream
    /// @param t            CRF type (audio_sample, video_frame, etc.)
    /// @param base_freq    Base frequency in Hz
    /// @param p            Pull multiplier
    /// @param ts_interval  Timestamp interval (media clock events between timestamps)
    /// @param ts_per_pkt   Number of timestamps per packet
    CrfStreamOutputContext(StreamId sid, CrfType t, uint32_t base_freq, CrfPull p, uint16_t ts_interval, uint16_t ts_per_pkt);

    /// Reset running state while preserving configuration
    void reset() noexcept;

    /// Get the nominal clock period in nanoseconds
    [[nodiscard]] auto nominal_period_ns() const noexcept -> double;

    /// Get the timestamp spacing in nanoseconds (period * timestamp_interval)
    [[nodiscard]] auto timestamp_spacing_ns() const noexcept -> double;

    /// Build a CRF packet with timestamps.
    ///
    /// Fills in the PDU header and writes timestamps_per_packet timestamps
    /// to the payload buffer, evenly spaced at the configured frequency.
    /// On the first call, anchors the timestamp sequence to gptp_now_ns.
    ///
    /// @param pdu            PDU header to fill
    /// @param timestamp_data Output buffer for timestamp payload (must hold timestamps_per_packet * 8 bytes)
    /// @param gptp_now_ns    Current gPTP time
    /// @return Number of bytes written to timestamp_data
    auto build_packet(CrfPdu& pdu, std::span<uint8_t> timestamp_data, uint64_t gptp_now_ns) noexcept -> size_t;

    /// build_packet() told the time as a signed Nanoseconds value.
    ///
    /// @p first_timestamp is used by the first call only: it is the first CRF
    /// timestamp written, and every later one is counted on from it by the
    /// timestamp spacing. A caller that wants a presentation offset in the
    /// stream adds it before the first call. A CRF timestamp is 64 bits
    /// unsigned on the wire and takes the count's bits as they are.
    auto build_packet(CrfPdu& pdu, std::span<uint8_t> timestamp_data, Nanoseconds first_timestamp) noexcept -> size_t
    {
        return build_packet(pdu, timestamp_data, static_cast<uint64_t>(first_timestamp.count()));
    }
};

}  // namespace statusbar::avtp
