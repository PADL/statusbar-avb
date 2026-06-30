#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_audio_io_config.hpp
/// @brief Configuration for AvbEntityAudioIO, in its own header so the
/// EntityUdptunBridge can hold a config_ ref without an include cycle.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/udptun/udptun_csv_record.hpp"  // udptun::default_colbin_capacity_bytes

#include <cstdint>
#include <string>
#include <vector>

namespace statusbar::avb_entity {

/// Configuration for the dual-format AVB Audio I/O Entity
struct AvbEntityAudioIOConfig
{
    /// Entity ID (EUI-64) - unique identifier for this entity
    ieee::Eui64 entity_id{};

    /// Entity Model ID (EUI-64) - identifies the entity model/class
    ieee::Eui64 entity_model_id{};

    /// Descriptor storage blob (serialized AEM descriptors; must declare 2
    /// stream inputs and 2 stream outputs)
    std::vector<uint8_t> descriptor_storage_blob{};

    /// Network interface name (e.g., "en0" on macOS, "eth0" on Linux)
    std::string interface_name{"eth0"};

    // Destination multicast MACs for the talker streams. NOTE: the IEEE-1722
    // AVTP multicast OUI 91:E0:F0 (00:00:00..00:FD:FF) is the MAAP dynamic
    // allocation pool and must NOT be hardcoded -- only used once claimed via
    // MAAP. Until MAAP is implemented we default to the JDKS OUI-36 (70:B3:D5:ED:C)
    // with the I/G (multicast) bit set (0x70->0x71), assuming one such device per
    // LAN; the top of that range (bottom 12 bits set) is reserved for the CRF.
    /// Destination multicast MAC for stream 0 (AM824 talker)
    ieee::Eui48 am824_talker_dest_mac{0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFD};

    /// Destination multicast MAC for stream 1 (AAF talker)
    ieee::Eui48 aaf_talker_dest_mac{0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFE};

    /// Destination multicast MAC for stream 2 (CRF media-clock talker) -- the
    /// highest address in the JDKS OUI-36 multicast range.
    ieee::Eui48 crf_talker_dest_mac{0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFF};

    /// CRF media-clock cadence (IEEE 1722-2016 §10). Matches the Milan CRF
    /// stream-input format (0x041060010000bb80): CRF_BASE_FREQUENCY=48 kHz base,
    /// timestamp_interval=96 events between timestamps, 1 timestamp per PDU -> 48000/96
    /// = 500 PDU/s, each timestamp spaced 96/48000 = 2 ms (= 192 of our 96 kHz audio
    /// samples). Receivers reject a mismatched interval/ts-per-PDU as UNSUPPORTED_FORMAT
    /// even when the base matches, so these must equal what the listener advertises.
    uint16_t crf_timestamp_interval{96};
    uint16_t crf_timestamps_per_packet{1};

    /// Gate talker stream transmission on listener readiness (IEEE 802.1Q SRP):
    /// a talker emits its AVTP stream only when a downstream listener has
    /// declared MSRP Listener Ready (listener_permits_transmit) for that stream
    /// OR has an active ACMP connection. With no listener / no Listener-Ready the
    /// talker stays silent instead of streaming into the void. The CRF media
    /// clock follows the audio talkers (emitted whenever any audio stream is
    /// transmitting). Default true; set false to stream unconditionally from
    /// link-up (the legacy behavior).
    bool gate_talker_on_listener{true};

    // Inter-site UDPTUN ingest (TX). When enabled and a peer is set, audio
    // received on the AAF listener (stream 1, interleaved int32) is reframed into
    // 1 ms packets and sent to the peer as AAF-v1 over IEEE-1722 Annex J UDP. Each
    // packet's avtp_timestamp is the sample's TAI presentation time, sourced as
    // CLOCK_REALTIME + udptun_tai_offset_ns (no PTP/PHC involvement); the far end
    // presents at tai + its worst-case-latency. The local AVB streams are
    // unchanged. See statusbar/udptun/udptun_audio_ingest.hpp.
    /// Enable the inter-site UDPTUN ingest TX path.
    bool udptun_enable{false};
    /// Far-site peer host/IP (empty disables the tunnel even if udptun_enable).
    std::string udptun_peer_host{};
    /// Far-site UDP port (IEEE 1722 Annex J continuous default 17220).
    uint16_t udptun_peer_port{17220};
    /// TAI − UTC offset (ns) applied to CLOCK_REALTIME to form the TAI timeline.
    int64_t udptun_tai_offset_ns{37'000'000'000};

    /// Which listener stream feeds the tunnel: 0 = AM824 (stream 0), 1 = AAF
    /// (stream 1, default). Both carry 4-byte samples (AM824 is 24-in-32), so the
    /// reframer handles either; the payload is forwarded opaquely.
    uint16_t udptun_source_stream{1};

    /// Frames (samples per channel) per inter-site packet. Default 44 = 458 us @
    /// 96 kHz, the largest size that keeps the datagram within one Ethernet MTU at
    /// 8 ch × int32 (32 B/frame): header (44 B) + 44×32 (1408 B) = 1452 B ≤ 1472
    /// (1500 − 20 IPv4 − 8 UDP), so NO IP fragmentation. 48 frames = a round 500 us
    /// but 1580 B → fragments into 2. Verified on the wire (node-b, 2026-06-08).
    uint16_t udptun_frames_per_packet{44};

    /// Inter-site UDPTUN egress (RX/playout). When enabled, the entity binds a UDP
    /// socket on udptun_listen_port, receives AAF-v1/AnnexJ packets from the far
    /// site, reclocks them onto the local media clock (play out at TAI + WCL with
    /// drop-to-0 concealment), and emits the de-tunneled audio on the local AVB
    /// talkers in place of the test oscillator. See AudioEgress.
    bool udptun_egress{false};
    /// UDP port the egress binds to receive far-site packets (Annex J default).
    uint16_t udptun_listen_port{17220};
    /// Worst-case-latency / playout delay (ns) for the egress de-jitter buffer.
    /// MUST be <= the AudioEgress ring depth (SlotCapacity*frames_per_packet/Fs ~=
    /// 29 ms) or the playout cursor reads behind where packets are retained and the
    /// egress permanently underruns. Default 20 ms: comfortably > real inter-site
    /// latency (6-9 ms measured) + jitter, and within the ring.
    int64_t udptun_wcl_ns{20'000'000};

    /// The ingest emits zero-PCM (silence) frames at the media-clock packet cadence
    /// whenever its listener source is absent or not delivering AVTP -- so the
    /// inter-site UDP transmission is NEVER gated on the AVB connection or incoming
    /// AVTP packets. Both ends therefore transmit continuously (silence or real
    /// audio), which keeps both NAT pinholes open and the tunnel solid + bidirectional
    /// regardless of source state; real audio simply replaces the silence the instant
    /// it arrives, with no tunnel re-establishment. DEFAULT TRUE: a tunnel node should
    /// not depend on a live source to hold its tunnel up. Set false only to suppress
    /// silence TX on a node that must stay quiet when idle.
    bool udptun_silence_source{true};

    /// Optional .colbin path: the egress records one row per received tunnel
    /// packet -- rx_TAI (CLOCK_REALTIME + tai_offset at drain), the packet's TAI
    /// presentation time, latency = rx_TAI - PT, and sequence -- in the owlm
    /// UdpTunCsvRecord schema, so scripts/owlm/owlm_analyze reads/plots it
    /// directly. Empty disables.
    std::string udptun_egress_colbin_path{};

    /// Pre-allocated capacity (bytes) for the egress colbin. The recorder maps
    /// this in full at start and NEVER grows -- no mid-run mremap to stall the
    /// real-time data plane (the growing mmap was the cause of the doubling
    /// latency spikes at t=base*2^k). Recording stops with capacity_exceeded if
    /// the run outlasts it. Default = 15 min @ 96 kHz (see
    /// udptun::default_colbin_capacity_bytes); raise via udptun.egress_colbin_max_mb.
    uint64_t udptun_egress_colbin_max_bytes{udptun::default_colbin_capacity_bytes};

    /// Send a temporally-shifted redundant copy of every ingest packet (owlm-
    /// style). The redundant carries the SAME sequence + TAI presentation time
    /// but a distinct stream_id (primary | UDPTUN_REDUN_BIT), sent
    /// udptun_temporal_shift_ms later. The far egress keys playout by TAI, so a
    /// redundant fills the slot of a lost primary (audio recovery); the colbin
    /// labels primary (RemotePrimary) vs redundant (RemoteRedundant) so
    /// owlm_analyze computes recovered / true_loss. Doubles the wire bandwidth.
    bool udptun_redundant{false};
    /// Delay (ms) between a primary and its redundant copy (default 5).
    int64_t udptun_temporal_shift_ms{5};

    /// STUN rendezvous for NAT traversal. When udptun_rendezvous_server is set
    /// (HOST:PORT), the entity performs a STUN REGISTER handshake at start() to
    /// discover the peer's reflexive address and obtain a hole-punched UDP socket,
    /// which becomes the SHARED socket for both ingest TX and egress RX (so the NAT
    /// mapping stays consistent). --udptun.peer/.listen are then ignored. Both
    /// peers must use the same session_id + key; roles are initiator/responder.
    std::string udptun_rendezvous_server{};           ///< STUN server HOST:PORT (empty = direct)
    std::string udptun_rendezvous_key{};              ///< 64-hex (32-byte) AES-128-SIV shared key
    std::string udptun_rendezvous_session_id{};       ///< 32-hex (16-byte) session id (same on both)
    std::string udptun_rendezvous_role{"initiator"};  ///< "initiator" or "responder"

    /// ATDECC wire version: "2016" emits descriptors at their IEEE 1722.1-2013/2016
    /// lengths (truncating the 2021-only descriptor tail fields) for controllers
    /// that reject 2021-length descriptors; "2021" emits the
    /// full 2021 forms. The ADPDU (68B) and the L2/short-form ACMPDU (56B) we use
    /// are identical across both standards; the 2021 IP-extended ACMPDU form
    /// (96B, control_data_length=84, carries the connected stream's IP address) is
    /// NOT implemented -- it is only needed to connect IP-transported streams, and
    /// is deferred until there is an IP-stream use case. Default "2016".
    std::string atdecc_version{"2016"};

    /// Stream destination address source. "static" (default) uses the
    /// *_talker_dest_mac fields above. "maap" acquires a contiguous block of
    /// addresses from the IEEE 1722 dynamic pool via MAAP at start() and assigns
    /// one per talker stream (block+0=AM824, +1=AAF, +2=CRF), then defends them for
    /// the entity's lifetime. Acquisition is synchronous at startup (~2-3 s of
    /// probing); on failure the entity falls back to the static MACs with a warning.
    std::string stream_address_mode{"static"};

    /// VLAN ID for AVB streams (default: SR Class A VLAN 2)
    uint16_t vlan_id{2};

    /// Sticky-Listener MSRP workaround. When true, the MSRP participant
    /// re-declares REGISTERED Listener attributes on every periodic/LeaveAll
    /// pass (not just our own declarations), echoing a downstream listener's
    /// Listener-Ready back toward the bridge so the bridge keeps the forwarding
    /// path to that listener warm. Needed when feeding a downstream listener
    /// through an AVB switch, where the strict end-station behaviour (never
    /// re-declare registered attributes) caused the switch to stop forwarding
    /// our stream to that listener. Safe for the Listener type only (no
    /// two-source / TalkerFailed-19 conflict).
    /// Default OFF (strict, spec-compliant); enable per host that needs it.
    bool redeclare_registered_listeners{false};

    /// Suppress-LeaveAll MSRP workaround. When true, the MSRP participant never
    /// originates a periodic LeaveAll -- it only re-asserts its declarations via
    /// the periodic timer and never releases them. This reproduces an earlier
    /// era that fed a downstream listener through an AVB switch reliably; the
    /// switch appears to drop installed stream forwarding when our LeaveAll
    /// drives the reservation through Leaving, making the downstream audio blink.
    /// We still honour received peer LeaveAlls; we just don't send our own.
    /// Default OFF (spec-compliant); enable per host talking to such a bridge.
    bool suppress_leaveall{false};

    /// TX stream-capture (diagnostic). When non-empty, the entity records its OWN
    /// transmitted stream frames (AAF / AM824 / CRF -- normally invisible because
    /// the TX socket is PACKET_QDISC_BYPASS) to this libpcap file, timestamped
    /// with the gPTP time at transmit, for `tx_pcap_seconds` from the first frame.
    /// Lets us inspect what we really put on the wire -- e.g. the AVTP presentation
    /// timestamps (the media clock handed to a listener) -- without a mirror port.
    /// Empty = disabled.
    std::string tx_pcap_path{};

    /// Capture window in seconds for tx_pcap_path (from the first transmitted
    /// frame). Default 10.
    uint32_t tx_pcap_seconds{10};

    /// Memory budget (bytes) for the TX capture ring. At 8000 pkt/s/stream and
    /// ~256 B/frame, 10 s of one stream is ~20 MB; default 64 MB covers three
    /// streams for 10 s. The window also stops if this fills.
    size_t tx_pcap_max_bytes{size_t{64} * 1024 * 1024};

    /// Test-signal generator: a repeating logarithmic sine sweep (chirp) on ONE
    /// channel of the UDP tunnel source. When enabled, the sweep REPLACES the
    /// tunnel's normal source (the AVB listener stream / silence keepalive) so the
    /// peer site receives the sweep -- e.g. node-b generates it, it crosses the
    /// tunnel to node-a's audio interface channel `sweep_channel` out, and a physical patch
    /// the audio interface ch1 -> ch2 returns it through the tunnel for a round-trip latency/
    /// continuity test. Does NOT affect the local talker (the local the audio interface still gets
    /// the tone/de-tunneled audio). Disabled by default.
    bool sweep_enable{false};
    double sweep_f_start_hz{20.0};
    double sweep_f_end_hz{1000.0};
    double sweep_duration_s{5.0};
    uint16_t sweep_channel{0};  // first channel; others carry silence
    float sweep_amplitude{0.5F};

    /// 802.1Q priority code point (PCP) for AVB stream frames. AVTP stream frames
    /// must be VLAN-tagged with the SR class priority so bridges classify them
    /// into the right SR class / credit-based shaper. Default 3 = SR Class A.
    uint8_t stream_pcp{3};

    /// Presentation-time offset in nanoseconds added to the gPTP time to form the
    /// AVTP timestamp of transmitted stream packets (how far in the future the
    /// listener should present the samples). Must exceed the worst-case talker +
    /// network transit time or the listener flags LATE_TIMESTAMP. Default 1 ms
    /// (the deterministic GPS-rate media clock owns this lead; see
    /// ptpclient::MediaClockGenerator).
    uint64_t presentation_offset_ns{1'000'000};

    /// Number of stream packets emitted per stream per media-timer wake. Default 1
    /// = strict SR Class A (one 12-sample/125 us packet per 125 us wake, evenly
    /// shaped). Setting N>1 wakes every N*125 us and emits N back-to-back packets
    /// per stream per wake (each still a 12-sample/125 us Class A frame, avtp
    /// timestamps advanced one packet interval apart), so the on-wire packet rate
    /// (8000/s/stream) and frame format are UNCHANGED, but there are N x fewer
    /// timer deadlines to miss. The burst-of-N egress is NOT strict Class A
    /// traffic shaping; it is absorbed at the listener by presentation_offset_ns,
    /// which must exceed N*125 us (it does at the 2 ms default). Non-default.
    size_t packets_per_wake{1};

    /// Pin the media-clock rate ratio r to exactly 1.0 (media clock = gPTP) instead
    /// of tracking r = gPTP/CLOCK_REALTIME via the GPS ratio Kalman. Use when the
    /// gPTP grandmaster IS the reference the listener recovers against (e.g. the
    /// listener is itself the GM, or the GM and listener share one timebase) and
    /// CLOCK_REALTIME is NOT GPS-disciplined (NTP), so r would otherwise wander and
    /// wobble the recovered clock. Skips the CLOCK_REALTIME sampling entirely.
    /// Default false (GPS-rate-pinned, for multi-site agreement). Diagnostic/test.
    bool media_lock_to_gptp{false};

    /// MEDIA_LOCKED detector tolerance (ns). A received stream locks when 8
    /// consecutive avtp_timestamp deltas fall within +/- this of the expected
    /// samples_per_ch/SAMPLE_RATE interval (125 us @ 12 samples). Default 5000
    /// (5 us). Widen to tolerate talker media-timer jitter without flapping the
    /// lock (e.g. when the talker batches packets per wake or runs hot).
    uint32_t lock_tolerance_ns{5'000};

    /// DSP filter center frequency (Hz)
    double filter_freq_hz{1000.0};

    /// DSP filter gain (dB) - negative for cut, positive for boost
    double filter_gain_db{0.0};

    /// DSP filter Q factor
    double filter_q{0.707};

    /// Stream test-tone frequency (Hz) generated per channel. Default 96000/7.
    double tone_freq_hz{96000.0 / 7.0};

    /// Stream test-tone amplitude (0..1 linear).
    float tone_amplitude{0.5F};

    /// Entity name (displayed in ATDECC controllers)
    std::string entity_name{"AVB Audio IO"};

    /// Firmware version string
    std::string firmware_version{"1.0.0"};
};

}  // namespace statusbar::avb_entity
