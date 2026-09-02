// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// EntityUdptunBridge facade methods — establishment selection + the media-
// thread source gate/pacing (refactor phase C; the tunnel data plane itself
// lives in UdptunTransport / UdptunIngestPath / UdptunEgressPath).

#include "statusbar/avb_entity/avb_entity_udptun_bridge.hpp"

#include "statusbar/avb_entity/avb_entity_udptun_ingest.hpp"

namespace statusbar::avb_entity {

void EntityUdptunBridge::start()
{
    if (!config_.udptun_rendezvous_server.empty()) {
        // Async punch-RETRY worker (the production STUN path). Build the codec/
        // buffer state up front (no socket) so the entity's local AVB runs
        // immediately; the media thread installs a hole-punched socket the moment
        // the worker stages one. (The worker validates the enable/egress config.)
        if (config_.udptun_enable) {
            ingest_.build_state();
        }
        if (config_.udptun_egress) {
            egress_.build_state();
        }
        (void)transport_.start_punch_worker();
    } else if (config_.udptun_enable && config_.udptun_egress && !config_.udptun_peer_host.empty()) {
        // Bidirectional direct peer: one shared socket so both ends transmitting
        // hole-punches both NAT pinholes without STUN.
        if (transport_.open_shared_socket()) {
            ingest_.build_state();
            egress_.build_state();
        }
    } else {
        if (config_.udptun_enable && !config_.udptun_peer_host.empty() && transport_.open_tx_socket()) {
            ingest_.build_state();
        }
        if (config_.udptun_egress && transport_.open_rx_socket()) {
            egress_.build_state();
        }
    }
}

void EntityUdptunBridge::stop()
{
    transport_.stop_punch_worker();
    egress_.commit_colbin();
}

auto EntityUdptunBridge::should_emit_silence(int64_t const now_tai_ns) const -> bool
{
    if (!ingest_.enabled() || !config_.udptun_silence_source) {
        return false;
    }
    int64_t const now_tai = (now_tai_ns != 0) ? now_tai_ns : realtime_tai_ns(config_.udptun_tai_offset_ns);
    return !udptun_source_streaming(telemetry_.last_real_ingest_tai().load(), now_tai);
}

void EntityUdptunBridge::source_tick(int64_t const pkt_tai_ns, size_t const samples, bool const emit_silence)
{
    // Test-signal mode: the logarithmic sweep IS the tunnel source (silence
    // stands down; the sweep also stamps last_real_ingest so the keepalive
    // stands down too). Without a tunnel clock this wake (pkt_tai_ns == 0) the
    // sweep cannot pace, so fall through to the silence filler like before.
    if (config_.sweep_enable && pkt_tai_ns != 0) {
        ingest_.sweep_tick(pkt_tai_ns);
        return;
    }
    if (emit_silence) {
        ingest_.ingest_silence(samples);
    }
}

}  // namespace statusbar::avb_entity
