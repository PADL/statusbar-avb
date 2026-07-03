// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// EntityUdptunBridge methods — the inter-site WAN tunnel data plane, moved out
// of avb_entity_audio_io.cpp (god-object phase 2, step 5). Bodies unchanged:
// the bridge holds same-named references (config_/rate_tracker_/audio_buffer_/
// channels_/last_gptp_ns_) into the owning entity, so only the member-access
// pointer (udptun_->) and the method qualifier changed.

#include "statusbar/avb_entity/avb_entity_udptun_bridge.hpp"

#include "statusbar/avb_entity/avb_entity_udptun_egress.hpp"
#include "statusbar/avb_entity/avb_entity_udptun_ingest.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/net/net_util.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/stun/stun_rendezvous.hpp"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <span>
#include <utility>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::avb_entity {

auto EntityUdptunBridge::setup_udptun_ingest() -> bool
{
    if (!config_.udptun_enable || config_.udptun_peer_host.empty()) {
        return false;
    }
    auto addr = net::SocketAddress::from_string(config_.udptun_peer_host, std::to_string(config_.udptun_peer_port));
    if (!addr) {
        std::print(stderr, "[udptun] cannot resolve peer {}:{}\n", config_.udptun_peer_host, config_.udptun_peer_port);
        return false;
    }
    peer_ = *addr;
    int const fd = ::socket(peer_.family(), SOCK_DGRAM, 0);
    if (fd < 0) {
        std::print(stderr, "[udptun] socket() failed\n");
        return false;
    }
    fd_ = net::FileDescriptor{fd};
    if (auto const s = net::set_nonblocking(fd); !s) {
        std::print(stderr, "[udptun] set_nonblocking failed: {}\n", s.error().message());
        fd_ = net::FileDescriptor{};
        return false;
    }

    build_udptun_ingest_state();
    return true;
}

auto EntityUdptunBridge::setup_udptun_direct_shared() -> bool
{
    // Direct-peer bidirectional: ONE socket bound to udptun_listen_port does both
    // ingest TX (sendto peer:peer_port) and egress RX. Both ends binding the same
    // port and both transmitting opens both NAT pinholes -- the same hole-punch
    // owlm uses in --time-source direct-peer mode -- so no STUN is needed. Used
    // when direct mode has BOTH ingest and egress enabled.
    auto addr = net::SocketAddress::from_string(config_.udptun_peer_host, std::to_string(config_.udptun_peer_port));
    if (!addr) {
        std::print(stderr, "[udptun] cannot resolve peer {}:{}\n", config_.udptun_peer_host, config_.udptun_peer_port);
        return false;
    }
    peer_ = *addr;
    int const fd = ::socket(peer_.family(), SOCK_DGRAM, 0);
    if (fd < 0) {
        std::print(stderr, "[udptun] shared socket() failed\n");
        return false;
    }
    fd_ = net::FileDescriptor{fd};
    if (auto const s = net::set_reuse_addr(fd); !s) {
        std::print(stderr, "[udptun] set_reuse_addr failed (continuing): {}\n", s.error().message());
    }
    auto const bind_addr = (peer_.family() == AF_INET6) ? net::SocketAddress::ipv6_any(config_.udptun_listen_port)
                                                        : net::SocketAddress::ipv4_any(config_.udptun_listen_port);
    if (::bind(fd, bind_addr.sockaddr(), bind_addr.length()) != 0) {
        std::print(stderr, "[udptun] shared bind :{} failed\n", config_.udptun_listen_port);
        fd_ = net::FileDescriptor{};
        return false;
    }
    if (auto const s = net::set_nonblocking(fd); !s) {
        std::print(stderr, "[udptun] set_nonblocking failed: {}\n", s.error().message());
        fd_ = net::FileDescriptor{};
        return false;
    }
    shared_socket_ = true;
    direct_shared_mode_ = true;
    // Arm udptun_punch_service()'s keepalive: a DIRECT-SHARED node that is
    // momentarily idle (no ingest audio) must still send the ~1 ms keepalive to
    // hold its NAT pinhole open, or the peer's packets are dropped and the tunnel
    // is one-directional until this node happens to transmit. The STUN punch only
    // armed this via the worker's socket hand-off, which never happens here -- so
    // set the install timestamp ourselves. (The STUN-only teardown/re-punch paths
    // in that service are gated off by direct_shared_mode_.)
    timespec ts{};
    int64_t const tai = (clock_gettime(CLOCK_REALTIME, &ts) == 0)
        ? ((static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL) + ts.tv_nsec + config_.udptun_tai_offset_ns)
        : 1;
    install_tai_ns_ = tai;
    last_rx_ns_ = tai;
    rx_baseline_ = telemetry_->any_rx.load();
    saw_data_ = true;  // shared socket is "up" at bind; no STUN first-data grace
    std::print(
        "Inter-site UDPTUN: direct-peer shared socket :{} <-> {} (bidirectional hole-punch, no STUN)\n",
        config_.udptun_listen_port,
        peer_.to_string());
    build_udptun_ingest_state();
    build_udptun_egress_state();
    return true;
}

void EntityUdptunBridge::udptun_ingest_silence(size_t const frames)
{
    // Feed `frames` of zero PCM to the ingest so the entity transmits silence as
    // if its listener source were sending zeros (or its packets were not
    // received). Reuses the normal ingest path (anchor + reframe + send).
    if (!enable_ || frames == 0) {
        return;
    }
    size_t const need = frames * static_cast<size_t>(channels_) * 4;
    if (silence_buf_.size() < need) {
        return;  // pre-sized in build_udptun_ingest_state; never grow on the hot path
    }
    udptun_ingest_audio(std::span<uint8_t const>{silence_buf_}.first(need), /*real_source=*/false, /*rt_caller=*/true);
}

void EntityUdptunBridge::udptun_ingest_sweep(size_t const frames)
{
    // Generate `frames` of the logarithmic sweep on sweep_channel (silence on all
    // other channels), encode interleaved int32 BIG-ENDIAN (the tunnel codec's
    // network-order AAF int32 format -- same layout as udptun_ingest_am824_as_int32),
    // and feed it as the tunnel source. real_source=true so the keepalive/silence
    // paths stand down: the sweep IS the audio that opens/holds the NAT pinhole.
    if (!enable_ || frames == 0) {
        return;
    }
    size_t const need = frames * static_cast<size_t>(channels_) * 4;
    if (sweep_buf_.size() < need) {
        return;  // pre-sized in build_udptun_ingest_state; never grow on the hot path
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
            out[off + 0] = static_cast<uint8_t>((static_cast<uint32_t>(s) >> 24) & 0xFFU);  // big-endian (MSB first)
            out[off + 1] = static_cast<uint8_t>((static_cast<uint32_t>(s) >> 16) & 0xFFU);
            out[off + 2] = static_cast<uint8_t>((static_cast<uint32_t>(s) >> 8) & 0xFFU);
            out[off + 3] = static_cast<uint8_t>(static_cast<uint32_t>(s) & 0xFFU);
        }
    }
    udptun_ingest_audio(std::span<uint8_t const>{sweep_buf_}.first(need), /*real_source=*/true, /*rt_caller=*/true);
}

void EntityUdptunBridge::build_udptun_ingest_state()
{
    // Our tunnel stream identity = the entity's EUI-64 with the redundancy flag bit
    // forced CLEAR, so the redundant copy (primary | UDPTUN_REDUN_BIT) is ALWAYS a
    // distinct stream_id even when the entity_id happens to have that bit set, and so
    // the egress can classify primary vs redundant. The bit lives in the EUI-64 b4
    // byte that owlm_analyze masks, so both copies still group as one logical sender.
    stream_id_.from_uint64(config_.entity_id.to_uint64() & ~UDPTUN_REDUN_BIT);
    redundant_id_.from_uint64(stream_id_.to_uint64() | UDPTUN_REDUN_BIT);

    uint16_t const frames = config_.udptun_frames_per_packet > 0 ? config_.udptun_frames_per_packet : 48;
    auto const interval_us = static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1'000'000ULL) / SAMPLE_RATE);

    udptun::AafV1OverAnnexJCodec::Config cc{};
    cc.stream_id.from_uint64(stream_id_.to_uint64());
    // Tell the codec the real redundant id when redundancy is on (so its classify()/
    // sender_pair_id() distinguish the copies); else leave it equal to primary
    // (has_redundancy() == false -> legacy single-stream).
    cc.redundant_stream_id.from_uint64(config_.udptun_redundant ? redundant_id_.to_uint64() : stream_id_.to_uint64());
    cc.format = avtp::AafFormat::int_32bit;
    cc.sample_rate = avtp::AafSampleRate::rate_96_khz;
    cc.channels = static_cast<uint16_t>(channels_);
    cc.bit_depth = AAF_BIT_DEPTH;
    cc.samples_per_packet = frames;
    cc.interval_us = interval_us;
    codec_.emplace(cc);

    udptun::AudioIngest<>::Config ic{};
    ic.channels = static_cast<uint16_t>(channels_);
    ic.sample_rate_hz = SAMPLE_RATE;
    ic.bytes_per_sample = 4;
    ic.tunnel_frames_per_packet = frames;
    ingest_.emplace(ic);

    size_t const datagram = udptun::AafV1OverAnnexJCodec::header_size() + codec_->payload_bytes();
    txbuf_.assign(datagram, 0);
    // Pre-zeroed silence block: up to one media tick (SAMPLES_PER_PACKET+1 frames)
    // of interleaved int32, fed to the ingest by udptun_ingest_silence().
    silence_buf_.assign(static_cast<size_t>(SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    // Scratch for transcoding an AM824 source's MBLA quadlets to int32 before
    // ingest (see udptun_ingest_am824_as_int32); generously sized, grows if a
    // packet ever exceeds it. Same byte count per sample (4), so size like silence.
    am824_transcode_buf_.assign(static_cast<size_t>(SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    // Test-signal sweep: same per-tick sizing as silence; configure the generator
    // from the [sweep] config (logarithmic chirp on one channel).
    sweep_buf_.assign(static_cast<size_t>(SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    sweep_gen_.configure(
        config_.sweep_f_start_hz,
        config_.sweep_f_end_hz,
        config_.sweep_duration_s,
        static_cast<double>(SAMPLE_RATE),
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
        redun_ring_.assign(ring_sz, EntityUdptunBridge::RedunSlot{});
        size_t const pcm_bytes = codec_->payload_bytes();
        for (auto& s : redun_ring_) {
            s.pcm.assign(pcm_bytes, 0);
        }
        redun_head_ = 0;
        std::print(
            "Inter-site UDPTUN redundancy: temporal_shift {} ms ({} packets), redundant stream_id 0x{:016x}\n",
            config_.udptun_temporal_shift_ms,
            redun_depth_,
            redundant_id_.to_uint64());
    }
    // 1472 = 1500 MTU - 20 (IPv4) - 8 (UDP). A datagram above this IP-fragments.
    char const* const frag = (datagram > 1472) ? "  *** > MTU: WILL IP-FRAGMENT ***" : "";
    std::print(
        "Inter-site UDPTUN ingest: AAF int32 ({} ch)  ({} frames / {} us, {}-byte datagram{})  TAI = realtime + {} ns\n",
        channels_,
        frames,
        interval_us,
        datagram,
        frag,
        config_.udptun_tai_offset_ns);
}

void EntityUdptunBridge::udptun_send_encoded(
    ieee::Eui64 const& stream_id, uint32_t const sequence, int64_t const tai_ns, std::span<uint8_t const> const pcm)
{
    if (fd_.get() < 0 || !codec_) {
        return;
    }
    size_t const hdr = codec_->encode(txbuf_, stream_id, sequence, tai_ns, /*interval_us=*/1000);
    size_t const total = hdr + pcm.size();
    if (total > txbuf_.size()) {
        return;
    }
    std::memcpy(txbuf_.data() + hdr, pcm.data(), pcm.size());
    (void)::sendto(fd_.get(), txbuf_.data(), total, MSG_DONTWAIT, peer_.sockaddr(), peer_.length());
    telemetry_->tx_packets.add(1);
}

void EntityUdptunBridge::udptun_send(int64_t const tai_ns, std::span<uint8_t const> const pcm)
{
    uint32_t const seq = seq_++;
    udptun_send_encoded(stream_id_, seq, tai_ns, pcm);

    // Redundancy: store this primary in the delay ring and replay the one from
    // redun_depth_ sends ago as the redundant copy (same seq + TAI, the
    // distinct redundant stream_id, sent ~temporal_shift later).
    if (config_.udptun_redundant && !redun_ring_.empty()) {
        auto& cur = redun_ring_[redun_head_];
        cur.seq = seq;
        cur.tai = tai_ns;
        cur.valid = true;
        if (cur.pcm.size() >= pcm.size()) {
            std::memcpy(cur.pcm.data(), pcm.data(), pcm.size());
        }
        size_t const back = (redun_head_ + redun_ring_.size() - redun_depth_) % redun_ring_.size();
        auto const& rep = redun_ring_[back];
        if (rep.valid && rep.pcm.size() >= pcm.size()) {
            udptun_send_encoded(redundant_id_, rep.seq, rep.tai, std::span<uint8_t const>{rep.pcm}.first(pcm.size()));
        }
        redun_head_ = (redun_head_ + 1) % redun_ring_.size();
    }
}

auto EntityUdptunBridge::setup_udptun_egress() -> bool
{
    if (!config_.udptun_egress) {
        return false;
    }
    int const fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::print(stderr, "[udptun] egress socket() failed\n");
        return false;
    }
    rx_fd_ = net::FileDescriptor{fd};
    if (auto const s = net::set_reuse_addr(fd); !s) {
        std::print(stderr, "[udptun] egress set_reuse_addr failed (continuing): {}\n", s.error().message());
    }
    auto const bind_addr = net::SocketAddress::ipv4_any(config_.udptun_listen_port);
    if (::bind(fd, bind_addr.sockaddr(), bind_addr.length()) != 0) {
        std::print(stderr, "[udptun] egress bind :{} failed\n", config_.udptun_listen_port);
        rx_fd_ = net::FileDescriptor{};
        return false;
    }
    if (auto const s = net::set_nonblocking(fd); !s) {
        std::print(stderr, "[udptun] egress set_nonblocking failed: {}\n", s.error().message());
        rx_fd_ = net::FileDescriptor{};
        return false;
    }

    build_udptun_egress_state();
    return true;
}

void EntityUdptunBridge::build_udptun_egress_state()
{
    uint16_t const frames = config_.udptun_frames_per_packet > 0 ? config_.udptun_frames_per_packet : 44;
    udptun::AudioEgress<>::Config ec{};
    ec.channels = static_cast<uint16_t>(channels_);
    ec.sample_rate_hz = SAMPLE_RATE;
    ec.bytes_per_sample = 4;
    ec.frames_per_packet = frames;
    ec.wcl_ns = config_.udptun_wcl_ns;
    egress_.emplace(ec);

    udptun::AafV1OverAnnexJCodec::Config dc{};
    dc.format = avtp::AafFormat::int_32bit;
    dc.sample_rate = avtp::AafSampleRate::rate_96_khz;
    dc.channels = static_cast<uint16_t>(channels_);
    dc.bit_depth = AAF_BIT_DEPTH;
    dc.samples_per_packet = frames;
    egress_codec_.emplace(dc);

    rxbuf_.assign(2048, 0);
    egress_pcm_.assign(static_cast<size_t>(SAMPLES_PER_PACKET + 1) * channels_ * 4, 0);
    egress_active_ = true;

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
            std::print(
                stderr, "[udptun] egress colbin '{}' open failed: {}\n", config_.udptun_egress_colbin_path, w.error().message());
        }
    }
    std::print(
        "Inter-site UDPTUN egress: WCL {} ns  ({} frames/packet) -> local AVB talkers{}\n",
        config_.udptun_wcl_ns,
        frames,
        egress_colbin_ ? "  [+colbin timing]" : "");
}

auto EntityUdptunBridge::setup_udptun_rendezvous() -> bool
{
    if (config_.udptun_rendezvous_server.empty()) {
        return false;
    }
    if (!config_.udptun_enable && !config_.udptun_egress) {
        std::print(stderr, "[udptun] rendezvous set but neither --udptun.enable nor --udptun.egress -- skipping\n");
        return false;
    }
    statusbar::crypto::Aes128SivKey key{};
    if (!net::parse_hex_into(config_.udptun_rendezvous_key, std::span<uint8_t>{key.data})) {
        std::print(stderr, "[udptun] rendezvous-key must be 64 hex chars\n");
        return false;
    }
    stun::SessionId session_id{};
    if (!net::parse_hex_into(config_.udptun_rendezvous_session_id, std::span<uint8_t>{session_id.bytes})) {
        std::print(stderr, "[udptun] rendezvous-session-id must be 32 hex chars\n");
        return false;
    }
    std::string host{};
    std::string port{};
    if (!net::split_host_port(config_.udptun_rendezvous_server, host, port)) {
        std::print(stderr, "[udptun] rendezvous-server must be HOST:PORT\n");
        return false;
    }
    auto server_addr = net::SocketAddress::from_string(host, port, net::SocketDatagram);
    if (!server_addr) {
        std::print(stderr, "[udptun] cannot resolve rendezvous server '{}:{}'\n", host, port);
        return false;
    }
    bool const responder = (config_.udptun_rendezvous_role == "responder");
    // Placeholder EUI-64 from session id + role byte; the server only needs the
    // two peers' EUI-64s to differ (mirrors owlm's perform_rendezvous_into).
    ieee::Eui64 client_eui{};
    auto eui_span = client_eui.span();
    for (size_t i = 0; i < eui_span.size(); ++i) {
        eui_span[i] = session_id.bytes[i];
    }
    eui_span[0] = responder ? 0x02U : 0x01U;

    stun::RendezvousConfig rcfg{
        .server_address = *server_addr,
        .session_id = session_id,
        .client_eui64 = client_eui,
        .role = responder ? stun::Role::Responder : stun::Role::Initiator,
        .shared_key = key,
        // Ephemeral local port (0), NOT udptun_listen_port. In rendezvous mode the
        // hole-punched socket is SHARED for TX+RX and the peer endpoint is learned
        // from the STUN XOR-MAPPED-ADDRESS, so the local port is immaterial to the
        // data plane. Pinning a fixed port (e.g. 17220 -- also used by the direct
        // and egress paths) invites an endpoint-dependent / reused NAT mapping whose
        // STUN-observed external port differs from the peer-facing pinhole, breaking
        // the punch. An ephemeral port gets a fresh mapping and reports the true
        // reflexive (matches the proven stun-client tool, which binds ephemeral).
        .local_port = 0,
        .local_interface = config_.interface_name,
        .timeout_ms = 60'000,
    };
    std::print(
        "Inter-site UDPTUN: STUN rendezvous against {} as {}...\n", server_addr->to_string(), config_.udptun_rendezvous_role);
    auto result = stun::perform_rendezvous(rcfg);
    if (!result) {
        std::print(stderr, "[udptun] rendezvous failed: {}\n", result.error().message());
        return false;
    }
    // Adopt the hole-punched socket as the SHARED TX+RX socket and the peer's
    // reflexive address as the data peer.
    fd_ = std::move(result->socket);
    if (auto const s = net::set_nonblocking(fd_.get()); !s) {
        std::print(stderr, "[udptun] rendezvous set_nonblocking failed: {}\n", s.error().message());
        fd_ = net::FileDescriptor{};
        return false;
    }
    peer_ = result->peer_reflexive_address;
    rendezvous_active_ = true;
    shared_socket_ = true;
    std::print(
        "Inter-site UDPTUN: rendezvous done. local={} my reflexive={} peer={}\n",
        result->local_address.to_string(),
        result->my_reflexive_address.to_string(),
        peer_.to_string());

    if (config_.udptun_enable) {
        build_udptun_ingest_state();
    }
    if (config_.udptun_egress) {
        build_udptun_egress_state();
    }
    return true;
}

auto EntityUdptunBridge::start_udptun_punch_worker() -> bool
{
    if (config_.udptun_rendezvous_server.empty()) {
        return false;
    }
    if (!config_.udptun_enable && !config_.udptun_egress) {
        std::print(stderr, "[udptun] punch: rendezvous set but neither enable nor egress -- skipping\n");
        return false;
    }
    // Build codec/buffer state up front (no socket) so the entity's local AVB runs
    // immediately; the media thread installs a hole-punched socket the moment the
    // worker stages one.
    if (config_.udptun_enable) {
        build_udptun_ingest_state();
    }
    if (config_.udptun_egress) {
        build_udptun_egress_state();
    }
    punch_run_.publish(true);
    punch_thread_ = std::thread([this] { statusbar::run_guarded("udptun-punch", [this] { udptun_punch_loop(); }); });
    std::print(
        "Inter-site UDPTUN: STUN punch-retry worker started ({} as {}); local AVB runs now, tunnel comes up async\n",
        config_.udptun_rendezvous_server,
        config_.udptun_rendezvous_role);
    return true;
}

void EntityUdptunBridge::stop_udptun_punch_worker()
{
    punch_run_.publish(false);
    if (punch_thread_.joinable()) {
        punch_thread_.join();
    }
}

void EntityUdptunBridge::udptun_punch_loop()
{
    using namespace std::chrono_literals;

    // Parse the static rendezvous inputs once.
    stun::SessionId base{};
    if (!net::parse_hex_into(config_.udptun_rendezvous_session_id, std::span<uint8_t>{base.bytes})) {
        std::print(stderr, "[udptun] punch: rendezvous-session-id must be 32 hex chars\n");
        return;
    }
    statusbar::crypto::Aes128SivKey key{};
    if (!net::parse_hex_into(config_.udptun_rendezvous_key, std::span<uint8_t>{key.data})) {
        std::print(stderr, "[udptun] punch: rendezvous-key must be 64 hex chars\n");
        return;
    }
    std::string host{};
    std::string port{};
    if (!net::split_host_port(config_.udptun_rendezvous_server, host, port)) {
        std::print(stderr, "[udptun] punch: rendezvous-server must be HOST:PORT\n");
        return;
    }
    auto server_addr = net::SocketAddress::from_string(host, port, net::SocketDatagram);
    if (!server_addr) {
        std::print(stderr, "[udptun] punch: cannot resolve rendezvous server '{}:{}'\n", host, port);
        return;
    }
    bool const responder = (config_.udptun_rendezvous_role == "responder");

    // Both peers attempt the SAME session id within each WINDOW, derived from the
    // shared GPS-NTP TAI clock (window counter overwrites the id's high 8 bytes).
    // Aligning attempts to window boundaries lands both nodes in the handshake
    // together with no external orchestration, and rotating the id every window
    // avoids stale STUN-server state -- exactly what owlm's harness achieves with a
    // fresh id per retry. The tunnel, once up, persists; the worker only re-punches
    // when the media-thread watchdog (udptun_punch_service) reports lost data.
    constexpr int64_t WINDOW_NS = 15'000'000'000LL;  // 15 s rendezvous window

    auto tai_now = [this]() -> int64_t {
        timespec ts{};
        if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
            return 0;
        }
        return (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL) + ts.tv_nsec + config_.udptun_tai_offset_ns;
    };

    while (punch_run_.load()) {
        int64_t const now = tai_now();
        int64_t const window = (now > 0) ? now / WINDOW_NS : 0;
        int64_t const into_window = now - (window * WINDOW_NS);
        int64_t const remain_ms = (WINDOW_NS - into_window) / 1'000'000LL;

        stun::SessionId sid = base;
        for (int i = 0; i < 8; ++i) {
            sid.bytes[i] = static_cast<uint8_t>((window >> (8 * (7 - i))) & 0xFF);
        }
        ieee::Eui64 client_eui{};
        auto eui_span = client_eui.span();
        for (size_t i = 0; i < eui_span.size(); ++i) {
            eui_span[i] = sid.bytes[i];
        }
        eui_span[0] = responder ? 0x02U : 0x01U;

        stun::RendezvousConfig rcfg{
            .server_address = *server_addr,
            .session_id = sid,
            .client_eui64 = client_eui,
            .role = responder ? stun::Role::Responder : stun::Role::Initiator,
            .shared_key = key,
            .local_port = 0,
            .local_interface = config_.interface_name,
            .timeout_ms = static_cast<uint32_t>(std::clamp<int64_t>(remain_ms - 1000, 3000, 12000)),
        };

        auto result = stun::perform_rendezvous(rcfg);
        if (!punch_run_.load()) {
            break;
        }
        if (result) {
            // Make the hole-punched socket non-blocking here on the (non-RT) worker
            // thread, before handing it to the media thread — so the RT path never
            // touches socket options.
            if (auto const s = net::set_nonblocking(result->socket.get()); !s) {
                std::print(stderr, "[udptun] set_nonblocking(staged socket) failed: {}\n", s.error().message());
            }
            {
                std::scoped_lock const lk(stage_mutex_);
                staged_fd_ = std::move(result->socket);
                staged_peer_ = result->peer_reflexive_address;
                staged_ready_ = true;
            }
            punch_retry_.publish(false);
            std::print(
                "[udptun] punch: paired (window {}) peer={} -- handing socket to media thread\n",
                window,
                result->peer_reflexive_address.to_string());
            // Hold while the tunnel is up; the media thread sets punch_retry_
            // if RX never starts or a live stream stalls, prompting a fresh punch.
            while (punch_run_.load() && !punch_retry_.load()) {
                std::this_thread::sleep_for(250ms);
            }
            punch_retry_.publish(false);
        } else {
            std::print(stderr, "[udptun] punch: window {} no pair ({}) -- retry next window\n", window, result.error().message());
        }

        // Align the next attempt to the next window boundary so both peers fire
        // together; sleep in small chunks for responsive shutdown.
        int64_t const t2 = tai_now();
        int64_t const next_boundary = (((t2 > 0 ? t2 : 0) / WINDOW_NS) + 1) * WINDOW_NS;
        int64_t sleep_ns = next_boundary - t2;
        while (punch_run_.load() && sleep_ns > 0) {
            int64_t const chunk = std::min<int64_t>(sleep_ns, 200'000'000LL);
            std::this_thread::sleep_for(std::chrono::nanoseconds(chunk));
            sleep_ns -= chunk;
        }
    }
}

void EntityUdptunBridge::udptun_punch_service(int64_t now_tai_ns)
{
    // 1) Install a staged hole-punched socket (worker -> media handoff). Only this
    // (media) thread ever assigns fd_ during operation.
    {
        std::scoped_lock const lk(stage_mutex_);
        if (staged_ready_) {
            // Exclude the reactor-thread send while we swap the socket + peer.
            // Lock order stage_mutex_ -> ingest_lock_ is the only site taking
            // both; the send takes only ingest_lock_, staging only stage_mutex_.
            std::scoped_lock const ig(ingest_lock_);
            fd_ = std::move(staged_fd_);
            peer_ = staged_peer_;
            staged_ready_ = false;
            // (socket was already set non-blocking by the worker before staging)
            shared_socket_ = true;
            rendezvous_active_ = true;
            anchored_ = false;
            sweep_tai_anchor_ns_ = 0;
            sweep_frames_emitted_ = 0;  // re-anchor ingest TAI on the fresh socket
            install_tai_ns_ = now_tai_ns;
            rx_baseline_ = telemetry_->any_rx.load();
            last_rx_ns_ = now_tai_ns;
            saw_data_ = false;
            // Re-anchor the egress on the fresh tunnel: drop any stale timeline so a
            // re-punch never leaves it stuck emitting silence (it re-anchors on the
            // first packet of the new stream). Reset the egress-silence watchdog too.
            if (egress_) {
                egress_->reset();
            }
            egress_play_baseline_ = egress_real_frames_;
            egress_last_play_ns_ = now_tai_ns;
            // No std::print on the RT media thread: the non-RT punch worker already
            // logs "paired ... peer=" just before handing the socket over.
        }
    }

    if (install_tai_ns_ == 0 || fd_.get() < 0) {
        return;  // no active tunnel socket
    }

    // 1b) Keepalive: a silence_source node already streams continuously, but a node
    // whose tunnel source is a (possibly-disconnected) AVB listener sends nothing
    // when idle -- so its NAT pinhole never opens and the punch can't complete until
    // audio happens to start. Emit a tiny raw datagram (NOT through the codec, so no
    // cross-thread ingest race) straight to the peer while idle, so both pinholes
    // open and stay warm; real audio takes over seamlessly when it arrives. The far
    // end drops the bad decode but still counts it as tunnel liveness (any_rx).
    if (!config_.udptun_silence_source) {
        int64_t const last_audio = telemetry_->last_real_ingest_tai.load();
        bool const streaming = (last_audio != 0) && (now_tai_ns - last_audio < 100'000'000LL);
        // ~1 ms cadence (matches owlm's proven punch rate + the ~2000 pkt/s real
        // audio that opened the pinhole on hardware). A slow keepalive (e.g. 50 ms)
        // refreshes a mapping but does NOT reliably open one on these home NATs.
        if (!streaming && (now_tai_ns - last_keepalive_ns_ > 1'000'000LL)) {
            last_keepalive_ns_ = now_tai_ns;
            std::array<uint8_t, 16> ka{};  // bare keepalive; opens/holds the NAT pinhole
            (void)::sendto(fd_.get(), ka.data(), ka.size(), MSG_DONTWAIT, peer_.sockaddr(), peer_.length());
        }
    }

    // 2) Watchdog liveness via ANY received datagram (audio OR keepalive) -- the far
    // end's packets prove the punch actually opened (STUN pairing alone does not).
    // Counting keepalives keeps an idle-but-open pinhole warm, so when real audio
    // starts it flows immediately instead of waiting on a fresh punch. No first
    // packet within the grace window, or a live tunnel going silent, tears the
    // socket down and asks the worker to re-punch.
    uint64_t const rx = telemetry_->any_rx.load();
    if (rx != rx_baseline_) {
        rx_baseline_ = rx;
        last_rx_ns_ = now_tai_ns;
        saw_data_ = true;
    }

    // 2b) Egress-anchor self-heal: distinct from the socket teardown below (which
    // fires only when data STOPS). This catches DECODED tunnel AUDIO arriving while
    // the egress emits no real audio for a sustained window -- i.e. the playout
    // timeline is stale (the punch->egress startup race that can leave a node silent until
    // a 2nd restart). Reset() it so it re-anchors on the next packet. Gated on real
    // audio (the telemetry rx_packets counter, decoded) not keepalives, so an idle tunnel doesn't
    // trip it; the ~22 ms WCL startup gap is far shorter than the 2 s window.
    if (egress_) {
        uint64_t const audio_rx = telemetry_->rx_packets.load();
        if (audio_rx != egress_audio_rx_baseline_) {
            egress_audio_rx_baseline_ = audio_rx;
            egress_last_audio_ns_ = now_tai_ns;
        }
        if (egress_real_frames_ != egress_play_baseline_) {
            egress_play_baseline_ = egress_real_frames_;  // egress producing audio -> healthy
            egress_last_play_ns_ = now_tai_ns;
            egress_reset_streak_ = 0;  // recovered -> clear the escalation streak
        } else if (
            (now_tai_ns - egress_last_audio_ns_ < 1'000'000'000LL) && (now_tai_ns - egress_last_play_ns_ > 2'000'000'000LL)) {
            egress_last_play_ns_ = now_tai_ns;  // grace before re-checking
            // A bare anchor reset recovers the common punch->egress startup race in
            // one shot. But if decoded audio keeps arriving and the egress STILL
            // plays nothing after several resets, the timeline itself is unworkable
            // -- e.g. the far end restarted onto a higher-latency re-punched path, so
            // transit now exceeds WCL and the read position permanently sits ahead of
            // the newest arrived packet. No anchor reset can fix that; only a fresh
            // tunnel can (a lower-latency re-punch, or the far end settling). The
            // plain stall teardown below never fires here because data is still
            // flowing -- so escalate to a re-punch ourselves, which is what a manual
            // restart did by hand (observed: 1090 fruitless resets, then a restart
            // fixed it). constexpr threshold ~= reset cadence (2 s) * count.
            // RT path: never std::print here (stderr I/O can block/alloc on the media
            // thread). Bump an atomic counter; print_state() surfaces it off-thread.
            // Escalate to a re-punch only on the STUN tunnel -- a DIRECT-SHARED
            // socket has no punch worker to re-create it, so tearing it down would
            // strand the tunnel. There the continuous keepalive + a fresh anchor
            // reset are the recovery (when the peer resumes, both pinholes re-open).
            constexpr int kEgressResetEscalate = 3;
            if (!direct_shared_mode_ && ++egress_reset_streak_ >= kEgressResetEscalate) {
                egress_reset_streak_ = 0;
                telemetry_->egress_repunch_count.add(1);
                {
                    std::scoped_lock const ig(ingest_lock_);  // no close while the reactor may sendto
                    fd_ = net::FileDescriptor{};       // close -> forces a fresh punch
                }
                shared_socket_ = false;
                rendezvous_active_ = false;
                install_tai_ns_ = 0;
                punch_retry_.publish(true);  // wake the worker for a fresh window punch
                return;                      // socket gone; nothing else to service this tick
            }
            telemetry_->egress_reset_count.add(1);
            egress_->reset();
        }
    }

    // A residential-NAT hole-punch can take tens of seconds to actually open even
    // when it will succeed, so give first data a generous window before giving up
    // (tearing down too early kills a punch that was about to come through). Once a
    // stream is live, a much shorter stall triggers a re-punch.
    constexpr int64_t GRACE_NS = 45'000'000'000LL;  // first data must arrive within 45 s of install
    constexpr int64_t STALL_NS = 8'000'000'000LL;   // a live stream silent this long -> re-punch
    int64_t const since_install = now_tai_ns - install_tai_ns_;
    int64_t const since_rx = now_tai_ns - last_rx_ns_;
    bool const dead = (!saw_data_ && since_install > GRACE_NS) || (saw_data_ && since_rx > STALL_NS);
    // Only the STUN tunnel tears down + re-punches on a stall; DIRECT-SHARED has no
    // worker to re-punch, and its keepalive keeps the pinhole warm so it re-converges
    // on its own when the peer comes back.
    if (dead && !direct_shared_mode_) {
        // RT path: count, don't print (the worker logs the ensuing re-punch).
        telemetry_->egress_repunch_count.add(1);
        {
            std::scoped_lock const ig(ingest_lock_);  // no close while the reactor may sendto
            fd_ = net::FileDescriptor{};        // close
        }
        shared_socket_ = false;
        rendezvous_active_ = false;
        install_tai_ns_ = 0;
        punch_retry_.publish(true);  // wake the worker for a fresh window punch
    }
}

void EntityUdptunBridge::udptun_egress_drain_rx()
{
    int const rx_fd = udptun_rx_fd();
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
            timespec ts{};
            if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
                int64_t const rx_tai =
                    (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL) + ts.tv_nsec + config_.udptun_tai_offset_ns;
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
                    .interval_us = static_cast<uint32_t>((static_cast<uint64_t>(nf) * 1'000'000ULL) / SAMPLE_RATE),
                    .role = static_cast<uint8_t>(role),
                    ._pad = {},
                };
                std::array<uint8_t, sizeof(udptun::UdpTunCsvRecord)> row{};
                std::memcpy(row.data(), &rec, sizeof(rec));
                (void)egress_colbin_->write_row(row);
            }
        }
    }
}

void EntityUdptunBridge::udptun_egress_fill(int64_t const now_tai_ns, size_t const samples)
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
    egress_real_frames_ +=
        egress_->playout(now_tai_ns, std::span<uint8_t>{egress_pcm_}.first(need), static_cast<uint16_t>(samples));
    // Interleaved int32 (network byte order) -> float audio_buffer_. Missing
    // frames were zero-filled by playout(), so underrun becomes silence. The
    // conversion is a pure, testable seam (avb_entity_udptun_egress.hpp) that
    // takes the destination as an out-span, so the egress move (god-object
    // phase 2) won't reach into audio_buffer_ directly.
    udptun_egress_deinterleave_to_float(
        std::span<uint8_t const>{egress_pcm_}.first(need), std::span<float>{audio_buffer_}, channels_, samples);
}

void EntityUdptunBridge::udptun_ingest_am824_as_int32(std::span<uint8_t const> const mbla)
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
    udptun_ingest_audio(std::span<uint8_t const>{am824_transcode_buf_.data(), n});
}

void EntityUdptunBridge::udptun_ingest_audio(std::span<uint8_t const> const audio, bool const real_source, bool const rt_caller)
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
        timespec rts{};
        if (clock_gettime(CLOCK_REALTIME, &rts) == 0) {
            telemetry_->last_real_ingest_tai.publish(
                (static_cast<int64_t>(rts.tv_sec) * 1'000'000'000LL) + rts.tv_nsec + config_.udptun_tai_offset_ns);
        }
    }

    // Single-producer handoff into the (non-thread-safe) reframer. The media RT
    // thread (rt_caller) try-acquires and skips its filler on contention so it
    // never blocks; the reactor thread spins until real audio gets exclusive
    // access. Everything below up to the matching unlock() is the critical section.
    if (rt_caller) {
        if (!ingest_lock_.try_lock()) {
            return;  // reactor thread is ingesting real audio; drop this filler tick
        }
    } else {
        ingest_lock_.lock();  // bounded spin: the RT critical section is a single reframer submit
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
                ingest_lock_.unlock();
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
    (void)ingest_->submit(audio.first(static_cast<size_t>(n_frames) * frame_bytes), n_frames, [this](auto const& pkt) {
        udptun_send(pkt.tai_ns, pkt.pcm);
    });
    ingest_lock_.unlock();
}

void EntityUdptunBridge::on_listener_audio(
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
            udptun_ingest_am824_as_int32(payload);
            break;
        case StreamAudioFormat::aaf_int32:
            udptun_ingest_audio(payload);
            break;
    }
}

}  // namespace statusbar::avb_entity
