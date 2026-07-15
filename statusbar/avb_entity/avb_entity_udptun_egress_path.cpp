// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// UdptunEgressPath methods — peer datagrams -> local AVB talkers (refactor
// phase C, split out of EntityUdptunBridge; bodies preserved).

#include "statusbar/avb_entity/avb_entity_udptun_egress_path.hpp"

#include "statusbar/avb_entity/avb_entity_udptun_egress.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_format.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_transport.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <array>
#include <span>

#include <sys/socket.h>

namespace statusbar::avb_entity {

void UdptunEgressPath::build_state()
{
    uint16_t const frames = config_.udptun_frames_per_packet > 0 ? config_.udptun_frames_per_packet : 44;
    udptun::AudioEgress<>::Config ec{};
    ec.channels = static_cast<uint16_t>(channels_);
    ec.sample_rate_hz = ClassA96k::SAMPLE_RATE;
    ec.bytes_per_sample = 4;
    ec.frames_per_packet = frames;
    ec.wcl_ns = config_.udptun_wcl_ns;
    egress_.emplace(ec);

    udptun::AafV1OverAnnexJCodec::Config dc{};
    dc.format = ClassA96k::AAF_FORMAT;
    dc.sample_rate = ClassA96k::AAF_SAMPLE_RATE;
    dc.channels = static_cast<uint16_t>(channels_);
    dc.bit_depth = ClassA96k::AAF_BIT_DEPTH;
    dc.samples_per_packet = frames;
    egress_codec_.emplace(dc);

    rxbuf_.assign(2048, 0);
    egress_pcm_.assign(static_cast<size_t>(ClassA96k::SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    active_ = true;

    // Optional per-packet timing recorder (owlm UdpTunCsvRecord colbin schema).
    // Fully pre-allocated (never grows mid-run, so no mremap stalls the RT data
    // plane); sized by udptun.egress_colbin_max_mb (default 15 min @ 96 kHz).
    if (!config_.udptun_egress_colbin_path.empty()) {
        statusbar::colbin::WriterConfig const colbin_cfg{
            .max_capacity_bytes = config_.udptun_egress_colbin_max_bytes, .preallocate = true};
        auto w = statusbar::colbin::Writer::create(config_.udptun_egress_colbin_path, udptun::udptun_colbin_schema(), colbin_cfg);
        if (w) {
            egress_colbin_.emplace(std::move(*w));
        } else {
            if (ctl_log_) {
                ctl_log_->error(
                    "udptun: egress colbin '{}' open failed: errno {}",
                    logging::embed<40>(config_.udptun_egress_colbin_path),
                    w.error().value());
            }
        }
    }
    if (ctl_log_) {
        ctl_log_->status(
            "udptun: egress WCL {} ns  ({} frames/packet) -> local AVB talkers{}",
            config_.udptun_wcl_ns,
            frames,
            egress_colbin_ ? logging::lit("  [+colbin timing]") : logging::lit(""));
    }
}

void UdptunEgressPath::drain_rx()
{
    int const rx_fd = transport_.rx_fd();
    if (rx_fd < 0 || !egress_ || !egress_codec_) {
        return;
    }
    size_t const frame_bytes = static_cast<size_t>(channels_) * 4;
    if (frame_bytes == 0) {
        return;
    }
    for (int guard = 0; guard < 512; ++guard) {  // bound the drain per tick
        ssize_t const n = ::recv(rx_fd, rxbuf_.data(), rxbuf_.size(), MSG_DONTWAIT);
        if (n <= 0) {
            break;
        }
        // Count EVERY datagram (decodable audio or a bare keepalive) for the punch
        // watchdog's tunnel-liveness check -- a keepalive proves the pinhole is open.
        telemetry_->any_rx.add(1);
        auto const dec = egress_codec_->decode(std::span<uint8_t const>{rxbuf_.data(), static_cast<size_t>(n)});
        if (!dec) {
            continue;
        }
        size_t const pcm_bytes = dec->audio.size();
        if (pcm_bytes < frame_bytes) {
            continue;
        }
        auto const nf = static_cast<uint16_t>(pcm_bytes / frame_bytes);
        int64_t const pt_ns = egress_codec_->tx_gptp_ns(*dec);
        (void)egress_->submit(pt_ns, dec->audio.first(static_cast<size_t>(nf) * frame_bytes), nf);
        telemetry_->rx_packets.add(1);

        // Per-packet timing: latency = local rx TAI - the packet's TAI
        // presentation time. Same TAI basis (CLOCK_REALTIME + tai_offset) the
        // ingest stamps with, so both ends share one absolute timeline.
        if (egress_colbin_) {
            if (int64_t const rx_tai = realtime_tai_ns(config_.udptun_tai_offset_ns); rx_tai != 0) {
                // Classify primary vs redundant by the REDUN_BIT; record the
                // PRIMARY-form stream_id for both so owlm_analyze groups them as
                // one logical stream (role distinguishes them for recovery accounting).
                uint64_t const sid_u64 = dec->pdu.stream_id().to_uint64();
                bool const is_redun = config_.udptun_redundant && ((sid_u64 & UDPTUN_REDUN_BIT) != 0);
                ieee::Eui64 sender{};
                sender.from_uint64(sid_u64 & ~UDPTUN_REDUN_BIT);
                auto const role = !config_.udptun_redundant
                    ? udptun::PacketRole::RemoteLegacy
                    : (is_redun ? udptun::PacketRole::RemoteRedundant : udptun::PacketRole::RemotePrimary);
                udptun::UdpTunCsvRecord rec{
                    .rx_gptp_ns = rx_tai,
                    .presentation_time_ns = pt_ns,
                    .latency_ns = rx_tai - pt_ns,
                    .sender_id = sender,
                    .sequence = dec->pdu.get_sequence_num(),
                    .interval_us = static_cast<uint32_t>((static_cast<uint64_t>(nf) * 1'000'000ULL) / ClassA96k::SAMPLE_RATE),
                    .role = static_cast<uint8_t>(role),
                    ._pad = {},
                };
                std::array<uint8_t, sizeof(udptun::UdpTunCsvRecord)> row{};
                span_store(row, rec);
                (void)egress_colbin_->write_row(row);
            }
        }
    }
}

void UdptunEgressPath::fill(int64_t const now_tai_ns, size_t const samples)
{
    if (!egress_) {
        return;
    }
    size_t const frame_bytes = static_cast<size_t>(channels_) * 4;
    size_t const need = samples * frame_bytes;
    if (egress_pcm_.size() < need) {
        return;
    }
    // Count frames the egress filled with REAL audio (vs zero-fill concealment) so
    // the punch-service watchdog can tell "tunnel delivering but egress silent"
    // (stale anchor) from "tunnel genuinely idle". Media-thread only -> plain add.
    real_frames_ += egress_->playout(now_tai_ns, std::span<uint8_t>{egress_pcm_}.first(need), static_cast<uint16_t>(samples));
    // Interleaved int32 (network byte order) -> float audio_buffer_. Missing
    // frames were zero-filled by playout(), so underrun becomes silence. The
    // conversion is a pure, testable seam (avb_entity_udptun_egress.hpp) that
    // takes the destination as an out-span, so this path never touches the
    // byte order itself.
    udptun_egress_deinterleave_to_float(
        std::span<uint8_t const>{egress_pcm_}.first(need), std::span<float>{audio_buffer_}, channels_, samples);
}

auto UdptunEgressPath::self_heal(int64_t const now_tai_ns, bool const allow_escalate) -> bool
{
    if (!egress_) {
        return false;
    }
    uint64_t const audio_rx = telemetry_->rx_packets.load();
    if (audio_rx != audio_rx_baseline_) {
        audio_rx_baseline_ = audio_rx;
        last_audio_ns_ = now_tai_ns;
    }
    if (real_frames_ != play_baseline_) {
        play_baseline_ = real_frames_;  // egress producing audio -> healthy
        last_play_ns_ = now_tai_ns;
        reset_streak_ = 0;  // recovered -> clear the escalation streak
    } else if ((now_tai_ns - last_audio_ns_ < 1'000'000'000LL) && (now_tai_ns - last_play_ns_ > 2'000'000'000LL)) {
        last_play_ns_ = now_tai_ns;  // grace before re-checking
        // This catches DECODED tunnel AUDIO arriving while the egress emits no real
        // audio for a sustained window -- i.e. the playout timeline is stale (the
        // punch->egress startup race that can leave a node silent until a 2nd
        // restart). Gated on real audio (the telemetry rx_packets counter, decoded)
        // not keepalives, so an idle tunnel doesn't trip it; the ~22 ms WCL startup
        // gap is far shorter than the 2 s window.
        //
        // A bare anchor reset recovers the common punch->egress startup race in
        // one shot. But if decoded audio keeps arriving and the egress STILL
        // plays nothing after several resets, the timeline itself is unworkable
        // -- e.g. the far end restarted onto a higher-latency re-punched path, so
        // transit now exceeds WCL and the read position permanently sits ahead of
        // the newest arrived packet. No anchor reset can fix that; only a fresh
        // tunnel can (a lower-latency re-punch, or the far end settling). The
        // plain stall teardown in the transport never fires here because data is
        // still flowing -- so escalate to a re-punch ourselves, which is what a
        // manual restart did by hand (observed: 1090 fruitless resets, then a
        // restart fixed it). constexpr threshold ~= reset cadence (2 s) * count.
        // RT path: never std::print here (stderr I/O can block/alloc on the media
        // thread). The transport bumps an atomic counter; print_state() surfaces
        // it off-thread.
        constexpr int kEgressResetEscalate = 3;
        if (allow_escalate && ++reset_streak_ >= kEgressResetEscalate) {
            reset_streak_ = 0;
            return true;  // transport tears down + re-punches
        }
        telemetry_->egress_reset_count.add(1);
        egress_->reset();
    }
    return false;
}

void UdptunEgressPath::commit_colbin()
{
    if (egress_colbin_) {
        (void)egress_colbin_->commit();
        egress_colbin_.reset();
    }
}

}  // namespace statusbar::avb_entity
