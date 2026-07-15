#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_egress_path.hpp
/// @brief UdptunEgressPath — the inter-site tunnel's EGRESS concern
/// (refactor phase C, split out of EntityUdptunBridge).
///
/// Peer datagrams -> local AVB talkers: decode AAF-v1/AnnexJ, submit into the
/// WCL-buffered AudioEgress, play out onto the entity's audio buffer at the
/// TAI playout clock, record per-packet timing to the optional colbin, and
/// self-heal a stale playout anchor. The RX socket is borrowed from
/// UdptunTransport (rx_fd()); the egress never owns it. Media-thread only
/// (drain + fill + self-heal all run on the media timer).

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_telemetry.hpp"
#include "statusbar/colbin/colbin_writer.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/udptun/udptun_aaf_v1_codec.hpp"
#include "statusbar/udptun/udptun_audio_egress.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

namespace statusbar::avb_entity {

class UdptunTransport;

class UdptunEgressPath
{
  public:
    UdptunEgressPath(
        AvbEntityAudioIOConfig const& config,
        size_t const& channels,
        std::pmr::vector<float>& audio_buffer,
        UdptunTransport& transport,
        std::shared_ptr<UdptunTelemetry> telemetry) noexcept
        : config_{config}
        , channels_{channels}
        , audio_buffer_{audio_buffer}
        , transport_{transport}
        , telemetry_{std::move(telemetry)}
    {}

    UdptunEgressPath(UdptunEgressPath const&) = delete;
    auto operator=(UdptunEgressPath const&) -> UdptunEgressPath& = delete;
    UdptunEgressPath(UdptunEgressPath&&) = delete;
    auto operator=(UdptunEgressPath&&) -> UdptunEgressPath& = delete;
    ~UdptunEgressPath() = default;

    /// Inject the main/reactor-thread logger (setup lines).
    void set_ctl_logger(logging::Logger const log) noexcept { ctl_log_ = log; }

    /// Build the codec / egress / buffer state (+ optional colbin) and arm the
    /// egress. Socket-free (the punch-worker path builds state long before a
    /// socket exists).
    void build_state();
    [[nodiscard]] auto active() const noexcept -> bool { return active_; }

    /// Drain the tunnel RX socket: decode each datagram and submit its PCM into
    /// the WCL playout buffer; tally liveness + timing telemetry.
    void drain_rx();
    /// Play out @p samples frames due at TAI @p now_tai_ns into the entity's
    /// float audio buffer (missing frames become silence).
    void fill(int64_t now_tai_ns, size_t samples);

    /// Fresh tunnel socket (transport install hook): drop any stale playout
    /// timeline so a re-punch never leaves the egress stuck emitting silence
    /// (it re-anchors on the first packet of the new stream), and reset the
    /// egress-silence watchdog baselines.
    void on_fresh_tunnel(int64_t now_tai_ns) noexcept
    {
        if (egress_) {
            egress_->reset();
        }
        play_baseline_ = real_frames_;
        last_play_ns_ = now_tai_ns;
    }

    /// Egress-anchor self-heal, run per media wake by the transport service.
    /// Detects DECODED tunnel AUDIO arriving while the egress emits no real
    /// audio for a sustained window — a stale playout timeline (the
    /// punch->egress startup race that can leave a node silent until a 2nd
    /// restart) — and reset()s it so it re-anchors on the next packet. Returns
    /// true to ESCALATE: several resets did not recover while audio kept
    /// arriving, so only a fresh tunnel can fix it (@p allow_escalate is false
    /// on a DIRECT-SHARED socket, which has no punch worker to re-create one).
    [[nodiscard]] auto self_heal(int64_t now_tai_ns, bool allow_escalate) -> bool;

    /// Commit + close the optional per-packet timing recorder (entity stop()).
    void commit_colbin();

  private:
    AvbEntityAudioIOConfig const& config_;
    size_t const& channels_;
    std::pmr::vector<float>& audio_buffer_;
    UdptunTransport& transport_;
    std::shared_ptr<UdptunTelemetry> telemetry_;
    std::optional<logging::Logger> ctl_log_{};

    bool active_{false};
    std::optional<udptun::AudioEgress<>> egress_{};
    std::optional<udptun::AafV1OverAnnexJCodec> egress_codec_{};
    std::vector<uint8_t> rxbuf_{};       ///< one-datagram recv scratch
    std::vector<uint8_t> egress_pcm_{};  ///< per-tick playout scratch (int32)
    std::optional<statusbar::colbin::Writer> egress_colbin_{};

    // --- Watchdog state (media thread) -----------------------------------------
    uint64_t real_frames_{0};        ///< cumulative real (non-concealed) egress frames
    uint64_t play_baseline_{0};      ///< watchdog snapshot of the above
    int64_t last_play_ns_{0};        ///< last time real egress frames advanced
    uint64_t audio_rx_baseline_{0};  ///< watchdog snapshot of telemetry rx_packets
    int64_t last_audio_ns_{0};       ///< last time decoded tunnel audio arrived
    int reset_streak_{0};            ///< consecutive resets without recovery
};

}  // namespace statusbar::avb_entity
