// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// UdptunIngestPath methods — local stream audio -> AAF-v1/AnnexJ datagrams
// (refactor phase C, split out of EntityUdptunBridge; bodies preserved).

#include "statusbar/avb_entity/avb_entity_udptun_ingest_path.hpp"

#include "statusbar/avb_entity/avb_entity_udptun_ingest.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_transport.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <ctime>

namespace statusbar::avb_entity {

void UdptunIngestPath::build_state()
{
    // Our tunnel stream identity = the entity's EUI-64 with the redundancy flag bit
    // forced CLEAR, so the redundant copy (primary | UDPTUN_REDUN_BIT) is ALWAYS a
    // distinct stream_id even when the entity_id happens to have that bit set, and so
    // the egress can classify primary vs redundant. The bit lives in the EUI-64 b4
    // byte that owlm_analyze masks, so both copies still group as one logical sender.
    stream_id_.from_uint64(config_.entity_id.to_uint64() & ~UDPTUN_REDUN_BIT);
    redundant_id_.from_uint64(stream_id_.to_uint64() | UDPTUN_REDUN_BIT);

    uint16_t const frames = config_.udptun_frames_per_packet > 0 ? config_.udptun_frames_per_packet : 48;
    auto const interval_us = static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1'000'000ULL) / ClassA96k::SAMPLE_RATE);

    udptun::AafV1OverAnnexJCodec::Config cc{};
    cc.stream_id.from_uint64(stream_id_.to_uint64());
    // Tell the codec the real redundant id when redundancy is on (so its classify()/
    // sender_pair_id() distinguish the copies); else leave it equal to primary
    // (has_redundancy() == false -> legacy single-stream).
    cc.redundant_stream_id.from_uint64(config_.udptun_redundant ? redundant_id_.to_uint64() : stream_id_.to_uint64());
    cc.format = ClassA96k::AAF_FORMAT;
    cc.sample_rate = ClassA96k::AAF_SAMPLE_RATE;
    cc.channels = static_cast<uint16_t>(channels_);
    cc.bit_depth = ClassA96k::AAF_BIT_DEPTH;
    cc.samples_per_packet = frames;
    cc.interval_us = interval_us;
    codec_.emplace(cc);

    udptun::AudioIngest<>::Config ic{};
    ic.channels = static_cast<uint16_t>(channels_);
    ic.sample_rate_hz = ClassA96k::SAMPLE_RATE;
    ic.bytes_per_sample = 4;
    ic.tunnel_frames_per_packet = frames;
    ingest_.emplace(ic);

    size_t const datagram = udptun::AafV1OverAnnexJCodec::header_size() + codec_->payload_bytes();
    txbuf_.assign(datagram, 0);
    // Pre-zeroed silence block: up to one media tick (SAMPLES_PER_PACKET+1 frames)
    // of interleaved int32, fed to the ingest by ingest_silence().
    silence_buf_.assign(static_cast<size_t>(ClassA96k::SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    // Scratch for transcoding an AM824 source's MBLA quadlets to int32 before
    // ingest (see ingest_am824_as_int32); generously sized, grows if a
    // packet ever exceeds it. Same byte count per sample (4), so size like silence.
    am824_transcode_buf_.assign(static_cast<size_t>(ClassA96k::SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    // Test-signal sweep: same per-tick sizing as silence; configure the generator
    // from the [sweep] config (logarithmic chirp on one channel).
    sweep_buf_.assign(static_cast<size_t>(ClassA96k::SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    sweep_gen_.configure(
        config_.sweep_f_start_hz,
        config_.sweep_f_end_hz,
        config_.sweep_duration_s,
        static_cast<double>(ClassA96k::SAMPLE_RATE),
        config_.sweep_amplitude);
    seq_ = 0;
    anchored_ = false;
    sweep_tai_anchor_ns_ = 0;
    sweep_frames_emitted_ = 0;
    enable_ = true;

    // Redundancy: size the delay ring to ~temporal_shift worth of packets
    // (interval_us each), pre-sizing each slot's PCM buffer so the replay path never
    // allocates. redundant_id_ was derived above (distinct by UDPTUN_REDUN_BIT).
    if (config_.udptun_redundant) {
        int64_t const shift_us = config_.udptun_temporal_shift_ms * 1000;
        redun_depth_ = std::max<size_t>(1, static_cast<size_t>(shift_us / std::max<uint32_t>(1, interval_us)));
        size_t const ring_sz = redun_depth_ + 4;
        redun_ring_.assign(ring_sz, RedunSlot{});
        size_t const pcm_bytes = codec_->payload_bytes();
        for (auto& s : redun_ring_) {
            s.pcm.assign(pcm_bytes, 0);
        }
        redun_head_ = 0;
        if (ctl_log_) {
            ctl_log_->status(
                "udptun: redundancy temporal_shift {} ms ({} packets), redundant stream_id 0x{:016x}",
                config_.udptun_temporal_shift_ms,
                redun_depth_,
                redundant_id_.to_uint64());
        }
    }
    // 1472 = 1500 MTU - 20 (IPv4) - 8 (UDP). A datagram above this IP-fragments.
    char const* const frag = (datagram > 1472) ? "  *** > MTU: WILL IP-FRAGMENT ***" : "";
    if (ctl_log_) {
        ctl_log_->status(
            "udptun: ingest AAF int32 ({} ch)  ({} frames / {} us, {}-byte datagram{})  TAI = realtime + {} ns",
            channels_,
            frames,
            interval_us,
            datagram,
            logging::static_str(frag),
            config_.udptun_tai_offset_ns);
    }
}

auto UdptunIngestPath::ingest_gps_tai(int64_t const master_ns, bool const rt_caller) -> IngestTai
{
    if (rt_caller) {
        bool const have = rate_tracker_.has_tai_sample();
        return {.have_sample = have, .tai_ns = have ? rate_tracker_.tai_ns(master_ns) : 0};
    }
    auto const snap = tai_snapshot_.consume();  // reactor thread: sole consumer
    return {.have_sample = snap.have_sample, .tai_ns = snap.have_sample ? ptpclient::tai_ns(snap, master_ns) : 0};
}

void UdptunIngestPath::send_encoded(
    ieee::Eui64 const& stream_id, uint32_t const sequence, int64_t const tai_ns, std::span<uint8_t const> const pcm)
{
    if (!transport_.tx_ready() || !codec_) {
        return;
    }
    size_t const hdr = codec_->encode(txbuf_, stream_id, sequence, tai_ns, /*interval_us=*/1000);
    size_t const total = hdr + pcm.size();
    if (total > txbuf_.size()) {
        return;
    }
    span_copy(make_span(txbuf_, {.start = hdr}), pcm);
    transport_.send(std::span<uint8_t const>{txbuf_}.first(total));
    telemetry_.tx_packets().add(1);
}

void UdptunIngestPath::send(int64_t const tai_ns, std::span<uint8_t const> const pcm)
{
    uint32_t const seq = seq_++;
    send_encoded(stream_id_, seq, tai_ns, pcm);

    // Redundancy: store this primary in the delay ring and replay the one from
    // redun_depth_ sends ago as the redundant copy (same seq + TAI, the
    // distinct redundant stream_id, sent ~temporal_shift later).
    if (config_.udptun_redundant && !redun_ring_.empty()) {
        auto& cur = redun_ring_[redun_head_];
        cur.seq = seq;
        cur.tai = tai_ns;
        cur.valid = true;
        if (cur.pcm.size() >= pcm.size()) {
            span_copy(make_span(cur.pcm), pcm);
        }
        size_t const back = (redun_head_ + redun_ring_.size() - redun_depth_) % redun_ring_.size();
        auto const& rep = redun_ring_[back];
        if (rep.valid && rep.pcm.size() >= pcm.size()) {
            send_encoded(redundant_id_, rep.seq, rep.tai, std::span<uint8_t const>{rep.pcm}.first(pcm.size()));
        }
        redun_head_ = (redun_head_ + 1) % redun_ring_.size();
    }
}

void UdptunIngestPath::ingest_audio(std::span<uint8_t const> const audio, bool const real_source, bool const rt_caller)
{
    // `audio` is the raw network-order payload of the received stream packet
    // (4-byte samples: AAF int32, or AM824 24-in-32). It is carried opaquely by
    // the tunnel and re-emitted at the far end, so there is no float round-trip.
    // TAI is anchored to CLOCK_REALTIME + offset on the first packet, then the
    // ingest advances it by exact frame duration (drift-free).
    if (!enable_ || !ingest_) {
        return;
    }
    size_t const frame_bytes = static_cast<size_t>(channels_) * 4;
    if (frame_bytes == 0 || audio.size() < frame_bytes) {
        return;
    }
    auto const n_frames = static_cast<uint16_t>(audio.size() / frame_bytes);
    // Mark that real tunnel audio is flowing so the punch keepalive AND the
    // silence-source gate stand down (the audio itself keeps the pinhole open and
    // is the single producer into the ingest). ONLY real listener audio stamps
    // this -- the silence filler must not, or the gate would read its own silence
    // as "real audio" and suppress itself. CLOCK_REALTIME = same TAI base the media
    // thread's checks read.
    if (real_source) {
        if (int64_t const rx_tai = realtime_tai_ns(config_.udptun_tai_offset_ns); rx_tai != 0) {
            telemetry_.last_real_ingest_tai().publish(rx_tai);
        }
    }

    // Single-producer handoff into the (non-thread-safe) reframer. The media RT
    // thread (rt_caller) try-acquires and skips its filler on contention so it
    // never blocks; the reactor thread spins until real audio gets exclusive
    // access. Everything below up to the matching unlock() is the critical section.
    auto& lock = transport_.tx_lock();
    if (rt_caller) {
        if (!lock.try_lock()) {
            return;  // reactor thread is ingesting real audio; drop this filler tick
        }
    } else {
        lock.lock();  // bounded spin: the RT critical section is a single reframer submit
    }

    // Resolve the GPS-TAI mapping once for this ingest. On the reactor thread this
    // consumes the media-thread-published snapshot (never the live Kalman); on the
    // media thread (rt_caller) it reads the Kalman directly, which is coherent.
    auto const gps_tai = ingest_gps_tai(static_cast<int64_t>(last_gptp_ns_.load(std::memory_order_relaxed)), rt_caller);
    if (!anchored_) {
        // Anchor the ingest timeline to GPS-TAI (gPTP master -> GPS-TAI), the same
        // clock that paces the source and that the egress plays on -- so the ingest
        // avtp_timestamp is drift-free TAI. Fall back to raw CLOCK_REALTIME+offset
        // until the translator has a sample.
        int64_t anchor_tai_ns = 0;
        if (gps_tai.have_sample) {
            anchor_tai_ns = gps_tai.tai_ns;
        } else {
            timespec ts{};
            if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
                lock.unlock();
                return;
            }
            anchor_tai_ns = (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL) + ts.tv_nsec + config_.udptun_tai_offset_ns;
        }
        ingest_->start(anchor_tai_ns);
        anchored_ = true;
    }
    // Discipline the free-running ingest TAI to the live GPS-TAI. The ingest counts
    // frames at the nominal sample rate, which only tracks real time when the SOURCE
    // media clock is GPS-locked. An audio-interface loopback follows the local switch gPTP
    // (not a GPS CRF), ~172 ppm off GPS, so without this the presentation time slides
    // ~500 ms/hour out of the far egress window (the AAF path stays locked because the DSP
    // processor follows our GPS-rate CRF). The slew is gentle, so the emitted timestamps stay
    // smooth. Only when the translator has a global-epoch sample.
    if (gps_tai.have_sample) {
        ingest_->discipline(gps_tai.tai_ns);
    }
    (void)ingest_->submit(
        audio.first(static_cast<size_t>(n_frames) * frame_bytes), n_frames, [this](auto const& pkt) { send(pkt.tai_ns, pkt.pcm); });
    lock.unlock();
}

void UdptunIngestPath::ingest_am824_as_int32(std::span<uint8_t const> const mbla)
{
    // AM824 MBLA data block = one 32-bit quadlet per sample: [label:8][audio:24]
    // big-endian. The inter-site tunnel + the far egress are AAF int32, so emit a
    // genuine int32 sample = the 24-bit audio MSB-aligned with a zero low byte
    // ([b1][b2][b3][0x00] = audio << 8). Dropping the label is exactly what makes
    // the far end NOT read 0x40 as the sample's MSB. Lossless for 24-bit audio.
    if (!enable_) {
        return;
    }
    size_t const need = (mbla.size() / 4) * 4;
    if (am824_transcode_buf_.size() < need) {
        am824_transcode_buf_.resize(need);  // grows once; steady-state no alloc
    }
    // Pure, testable transcode (avb_entity_udptun_ingest.hpp).
    size_t const n = udptun_am824_mbla_to_int32(mbla, std::span<uint8_t>{am824_transcode_buf_}.first(need));
    ingest_audio(std::span<uint8_t const>{am824_transcode_buf_.data(), n});
}

void UdptunIngestPath::ingest_silence(size_t const frames)
{
    // Feed `frames` of zero PCM to the ingest so the entity transmits silence as
    // if its listener source were sending zeros (or its packets were not
    // received). Reuses the normal ingest path (anchor + reframe + send).
    if (!enable_ || frames == 0) {
        return;
    }
    size_t const need = frames * static_cast<size_t>(channels_) * 4;
    if (silence_buf_.size() < need) {
        return;  // pre-sized in build_state; never grow on the hot path
    }
    ingest_audio(std::span<uint8_t const>{silence_buf_}.first(need), /*real_source=*/false, /*rt_caller=*/true);
}

void UdptunIngestPath::ingest_sweep(size_t const frames)
{
    // Generate `frames` of the logarithmic sweep on sweep_channel (silence on all
    // other channels), encode interleaved int32 BIG-ENDIAN (the tunnel codec's
    // network-order AAF int32 format -- same layout as ingest_am824_as_int32),
    // and feed it as the tunnel source. real_source=true so the keepalive/silence
    // paths stand down: the sweep IS the audio that opens/holds the NAT pinhole.
    if (!enable_ || frames == 0) {
        return;
    }
    size_t const need = frames * static_cast<size_t>(channels_) * 4;
    if (sweep_buf_.size() < need) {
        return;  // pre-sized in build_state; never grow on the hot path
    }
    size_t const sweep_ch = (config_.sweep_channel < channels_) ? config_.sweep_channel : 0;
    uint8_t* const out = sweep_buf_.data();
    for (size_t f = 0; f < frames; ++f) {
        float const sample_f = sweep_gen_.next();  // one sample period per frame
        float const clamped = (sample_f > 1.0F) ? 1.0F : ((sample_f < -1.0F) ? -1.0F : sample_f);
        auto const v = static_cast<int32_t>(clamped * 2147483647.0F);
        for (size_t ch = 0; ch < channels_; ++ch) {
            int32_t const s = (ch == sweep_ch) ? v : 0;
            size_t const off = ((f * static_cast<size_t>(channels_)) + ch) * 4;
            span_store(std::span<uint8_t>{out + off, 4}, ieee::quadlet_t{static_cast<uint32_t>(s)});
        }
    }
    ingest_audio(std::span<uint8_t const>{sweep_buf_}.first(need), /*real_source=*/true, /*rt_caller=*/true);
}

void UdptunIngestPath::sweep_tick(int64_t const pkt_tai_ns)
{
    // Pace the sweep by the GPS-TAI tunnel clock: emit the cumulative frame count
    // implied by elapsed TAI (exactly SAMPLE_RATE frames per TAI second), so the
    // ingest avtp_timestamp stays locked to TAI with zero drift and the far egress
    // (playing on the same GPS-TAI clock) never under/over-runs.
    if (!enable_ || pkt_tai_ns == 0) {
        return;
    }
    if (sweep_tai_anchor_ns_ == 0) {
        sweep_tai_anchor_ns_ = pkt_tai_ns;
    }
    // Bound catch-up bursts to a few packets' worth per tick.
    size_t const cap = static_cast<size_t>(ClassA96k::SAMPLES_PER_PACKET) * 4;
    size_t const n = udptun_sweep_frames_due(pkt_tai_ns - sweep_tai_anchor_ns_, sweep_frames_emitted_, ClassA96k::SAMPLE_RATE, cap);
    if (n > 0) {
        ingest_sweep(n);
        sweep_frames_emitted_ += n;
    }
}

void UdptunIngestPath::offer_listener_audio(
    uint16_t const stream_index, StreamAudioFormat const fmt, std::span<uint8_t const> const payload)
{
    // Only forward the listener stream the tunnel is configured to source, and
    // not while the test sweep is replacing the source (it injects its own audio).
    if (stream_index != config_.udptun_source_stream || config_.sweep_enable) {
        return;
    }
    switch (fmt) {
        case StreamAudioFormat::am824_mbla:
            // The tunnel transport + far egress are AAF int32, so the raw MBLA quadlets
            // MUST be transcoded first (sending [0x40 label][24-bit] raw makes the far
            // end read the label as the sample MSB -> +0.5 FS pedestal + crushed audio).
            ingest_am824_as_int32(payload);
            break;
        case StreamAudioFormat::aaf_int32:
            ingest_audio(payload);
            break;
    }
}

}  // namespace statusbar::avb_entity
