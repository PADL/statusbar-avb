#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_audio_io_config.hpp
/// @brief Configuration for AvbEntityAudioIO, in its own header so the
/// EntityUdptunBridge can hold a config_ ref without an include cycle.

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_linux_xdp.hpp"
#include "statusbar/udptun/udptun_csv_record.hpp"  // udptun::default_colbin_capacity_bytes

#include <charconv>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace statusbar::avb_entity {

/// The default limit on the system clock's worst-case error for following
/// its rate: one second. A rate is as good from a clock 30 ms off as from an
/// exact one, and the operating system's own figure climbs by half a
/// millisecond every second between its time daemon's updates (32 ms over a
/// 64 s poll), so the limit sits well above that. It tells a disciplined
/// clock from one that never synchronized, or that has had no server for
/// most of a day.
inline constexpr int64_t DEFAULT_MAX_UTC_ERROR_NS = 1'000'000'000;

/// The default limit on the system clock's worst-case error for vouching for
/// a TAI timestamp made from it: three milliseconds. Between two sites across
/// the internet the relative phase does not need the accuracy a gPTP network
/// keeps; what the tunnel needs is an accurate rate at both ends, and a phase
/// error well under its worst-case latency (tens of milliseconds). A clock
/// disciplined by a GPS receiver on the local network is rated between one
/// and a half and two and a half milliseconds, so it sits inside this with
/// little to spare: one that has lost its server coasts at the default 15 ppm
/// for a minute or so before it passes it. A deployment that knows its
/// oscillator sets utc_holdover_drift_ppb lower and coasts for longer.
inline constexpr int64_t DEFAULT_MAX_TAI_ERROR_NS = 3'000'000;

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

    /// The interface's MAC address, told by a caller that already knows it.
    /// Left empty, the entity asks the operating system for the address of
    /// interface_name when it is created. Telling it keeps the entity's
    /// construction free of any call to the system (a test does this).
    std::optional<ieee::Eui48> interface_mac{};

    /// How the stream frames reach the wire (net::ETHERNET_BACKEND_CHOICES):
    /// "rawnet" (default) is a transmit socket and a receive socket, plain
    /// AF_PACKET, one syscall per frame, on any platform; "mmap" is
    /// PACKET_MMAP rings on one socket (Linux); "xdp" is AF_XDP on one NIC
    /// queue with the stream subtypes steered into it (Linux, needs the XDP
    /// program loadable, zero-copy only on a NIC with a native driver). The
    /// control plane stays on its own raw socket whichever this is.
    std::string net_backend{"rawnet"};

    /// The NIC queue the xdp backend binds to.
    uint32_t net_xdp_queue{0};
    /// The xdp backend's bind (zero-copy or copy) and attach (driver or generic) modes.
    net::XdpBindMode net_xdp_bind_mode{net::XdpBindMode::auto_};
    net::XdpAttachMode net_xdp_attach_mode{net::XdpAttachMode::native};
    /// Where the xdp backend pins its program and maps (bpffs); empty: not pinned.
    std::string net_xdp_pin_path{};
    /// Open rawnet when the xdp backend does not open, the reason kept
    /// (AvbEntityKit::stream_xdp_error()); false: no stream port then.
    bool net_xdp_fallback{false};
    /// Give each stream frame a launch time (AF_XDP TX metadata, the NIC's
    /// etf/TBS), in the sending PHC's time; xdp backend only.
    bool stream_launch_time{false};

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
    ///
    /// Both have to be values a CRF stream can have (crf_cadence_fits()):
    /// AvbEntityAudioIO::create() refuses a configuration where one is not.
    uint16_t crf_timestamp_interval{96};
    uint16_t crf_timestamps_per_packet{1};

    /// Gate each talker stream on SR-class admission: it transmits only when Started
    /// and an MSRP Listener Ready (Failed) is registered (Milan v1.2 Section 5.3.7.3).
    /// Default true; false streams unconditionally (bench only). docs/AVB_ENTITY_KIT.md.
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
    /// is deferred until there is an IP-stream use case. Empty (the default):
    /// the profile decides, see effective_atdecc_version() - "2021" under the
    /// Milan profile, whose descriptors are those of 1722.1-2021 (Milan v1.3
    /// 5.3.3), "2016" otherwise.
    std::string atdecc_version{};

    /// Let any controller's SET_CONTROL of the IDENTIFY control through while
    /// another controller holds the entity acquired or locked. Off by default,
    /// as IEEE 1722.1-2021 7.4.25.2 and Milan v1.3 5.4.2.17 have it: a held
    /// entity refuses every other controller's SET_CONTROL, IDENTIFY included.
    bool identify_bypasses_lock{false};

    /// Grant an ACQUIRE_ENTITY and shut no other controller out with it
    /// (AemCommandHandler::AcquireMode::shared): the acquirer is answered
    /// SUCCESS and stays the owner the entity reports, and every other
    /// controller's SET and LOCK_ENTITY goes on as before. Off by default, as
    /// IEEE 1722.1-2021 7.4.1 has it: an acquired entity answers the others
    /// ENTITY_ACQUIRED. It is for the classic profile; under the Milan one
    /// no acquisition is granted at all (Milan v1.3 5.4.2.1).
    bool acquire_shared{false};

    /// The specification the entity answers to where IEEE 1722.1-2021 and
    /// Milan v1.3 differ: "classic" reads 1722.1 as written; "milan13" applies
    /// Milan v1.3 where it says otherwise (a SET to the value already in force
    /// is not notified, 5.4.5.3). Empty (the default), the model decides:
    /// "milan13" for a Milan model (`milan`), "classic" for any other.
    std::string atdecc_profile{};

    /// The model is a Milan PAAD-AE. The entity then answers the Milan
    /// vendor-unique commands: GET_MILAN_INFO (Milan v1.3 5.4.4.1) with the
    /// three fields below; under the Milan profile BIND_STREAM,
    /// UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX (5.4.4.6 to 5.4.4.8); the
    /// others NOT_IMPLEMENTED. An empty atdecc_profile then means "milan13".
    /// Off, the entity is a plain 1722.1 entity and lets every MVU command
    /// pass unanswered (5.4.4.1, Note).
    bool milan{false};
    /// GET_MILAN_INFO features_flags, Table 5.17 (REDUNDANCY 1,
    /// TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING 2, TALKER_SIGNAL_PRESENCE 8).
    /// None by default: none is implemented. MVU_BINDING (4) is not set
    /// here: the entity sets it under the Milan profile, where it answers
    /// BIND_STREAM and UNBIND_STREAM, and clears it under any other.
    uint32_t milan_features_flags{0};
    /// The Milan certification the device has passed, as dotted numbers
    /// ("1.2", "1.3.0.0"); "0.0.0.0" for none.
    std::string milan_certification_version{"0.0.0.0"};
    /// The Milan specification the device supports, as dotted numbers.
    std::string milan_specification_version{"1.3.0.0"};

    /// Each CLOCK_DOMAIN's Default Media Clock Reference Priority (Milan v1.3 Section 7.6.1.1), by
    /// index; the last also serves the domains after it, extras are ignored. Empty keeps the MVU
    /// handler's MVU_MCR_PRIO_NETWORK_INTERFACE (192). Read at start().
    std::vector<uint8_t> default_mcr_priorities{};

    /// Stream destination address source. "static" (default) uses the
    /// *_talker_dest_mac fields above. "maap" acquires a contiguous block of
    /// addresses from the IEEE 1722 dynamic pool via MAAP at start() and assigns
    /// one per talker stream (block+0=AM824, +1=AAF, +2=CRF), then defends them for
    /// the entity's lifetime. Acquisition is synchronous at startup (~2-3 s of
    /// probing); on failure the entity falls back to the static MACs with a warning.
    /// Empty (the default): the profile decides, see
    /// effective_stream_address_mode() - "maap" under the Milan profile, which
    /// has its talkers take their addresses from MAAP (Milan v1.3 4.3.3.1),
    /// "static" otherwise.
    std::string stream_address_mode{};

    /// Under "maap": where the block defended is saved, and read back at the
    /// next start to be probed first, so a restart keeps its destination
    /// addresses under the same StreamIDs - a bridge still holding the old
    /// registration refuses a new address for them (802.1Q-2018 35.2.2.8,
    /// Talker Failed code 16) until it lapses, tens of seconds. A range taken
    /// by somebody else while the entity was down is found out by the probe
    /// and a fresh one drawn. Empty (the default): every start draws afresh.
    std::string stream_maap_range_file{};

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

    /// Persisted listener bindings (kit phase 5b). When set, every
    /// controller-made stream-input connection is remembered in this file.
    /// IEEE 1722.1 ACMP: on start the entity fast-connects (8.2.2.1.1) each
    /// remembered talker — retrying until it answers — so connections
    /// survive an entity or talker restart without a controller; a
    /// controller DISCONNECT removes the binding; empty = disabled. Milan
    /// ACMP: the file is the non-volatile memory of Milan v1.3 5.3.8.3,
    /// written at every bind and unbind, and each saved stream input starts
    /// bound; empty, the bindings last only as long as the process.
    std::string listener_bindings_path{};

    /// The persistent state TOML file (docs/PERSISTENT_STATE.md), loaded at start()
    /// and rewritten on every change; supersedes listener_bindings_path, importing
    /// it once. Empty = off.
    std::string persistent_state_path{};

    /// Local ATDECC socket (nanoavb_local_port.hpp): an AF_UNIX datagram socket for
    /// controllers on this host, without raw-socket privileges. Empty = off.
    std::string atdecc_socket_path{};
    /// The local ATDECC socket file's mode.
    int atdecc_socket_mode{0660};

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

    /// Presentation-time offset (ns) added to gPTP time for transmitted AVTP
    /// timestamps; must exceed talker + network transit. Default 2 ms (Milan v1.2
    /// Section 5.3.7.6); a controller may change it per output.
    uint64_t presentation_offset_ns{2'000'000};

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

    /// Drain the AM824/AAF stream RX on a dedicated SCHED_FIFO timer thread (its own,
    /// ideally isolated, core) instead of the shared reactor. The reactor also runs the
    /// control plane (gPTP/MSRP/ACMP/MAAP), so under load or media-timer preemption it
    /// services RX with jitter; queued frames are then batch-tallied against a now-later
    /// clock and read as spurious LATE_TIMESTAMP. A ~50 us RT drain keeps frames tallied
    /// within one tick of arrival, against the timer's own fresh gPTP wake time. On by
    /// default (measured 2026-11-01 on the Pi 5 pair: the reactor path's late frames,
    /// a stall tail of 0.3 to 1.2 ms, went to zero). Off = reactor path. Wants a spare
    /// core; on the Pi5 rig core 3 is the media timer, so the RX timer takes isolated
    /// core 2. A core that does not exist only costs the pinning, logged.
    bool stream_rx_rt_timer{true};

    /// CPU core for the RX timer thread when stream_rx_rt_timer is set (should be an
    /// isolated core, distinct from the media timer's).
    int stream_rx_cpu_affinity{2};

    /// RX drain tick period in microseconds when stream_rx_rt_timer is set. 50 us
    /// (2.5x the 125 us packet interval) keeps the per-tick backlog to 0-3 frames.
    uint32_t stream_rx_period_us{50};

    /// Kick the stream port's TX ring (the wake-up syscall the mmap and xdp
    /// backends need) from a dedicated SCHED_FIFO timer instead of at the end
    /// of each media tick, so the media thread never enters the kernel for
    /// transmit: the syscall's latency is interference on the realtime core.
    /// The media thread still fills the ring and reclaims completions. Free
    /// on rawnet (each frame is its own syscall there).
    bool stream_tx_kicker{false};

    /// CPU core for the TX kicker thread when stream_tx_kicker is set (an
    /// isolated core; it may share the RX timer's).
    int stream_tx_cpu_affinity{2};

    /// TX kick period in microseconds when stream_tx_kicker is set. Frames
    /// committed in a tick leave within one period of it.
    uint32_t stream_tx_period_us{50};

    /// Issue the TX kick from the RX drain timer's callback (stream_rx.rt_timer)
    /// instead of a timer of its own: one SCHED_FIFO thread on the spare core
    /// drains then kicks, so there is no second timer for it to contend with.
    /// Takes the kick off the media tick like stream_tx.kicker, which it
    /// overrides; nothing without stream_rx.rt_timer. On by default: it cut
    /// the media tick from 16 to 6 us on mmap and xdp with no second timer
    /// to contend with (2026-11-01, Pi 5 pair).
    bool stream_tx_kick_in_rx_timer{true};

    /// Pin the media-clock rate ratio r to exactly 1.0 (media clock = gPTP) instead
    /// of tracking r = gPTP/CLOCK_REALTIME via the GPS ratio Kalman. Use when the
    /// gPTP grandmaster IS the reference the listener recovers against (e.g. the
    /// listener is itself the GM, or the GM and listener share one timebase) and
    /// CLOCK_REALTIME is NOT GPS-disciplined (NTP), so r would otherwise wander and
    /// wobble the recovered clock. The system clock is still read and judged (its
    /// error figure decides the tunnel's tu bit); only its rate is not followed.
    /// Default false (GPS-rate-pinned, for multi-site agreement). Diagnostic/test.
    bool media_lock_to_gptp{false};

    /// The largest worst-case error of the system clock (the operating
    /// system's own figure, set by the time daemon and coasted here through a
    /// loss of its server) at which the entity still follows that clock's
    /// rate. Inside it the media-clock ratio and the tunnel's TAI mapping are
    /// trained from the clock; outside it they hold what they had. Negative
    /// takes any bounded clock.
    int64_t max_utc_error_ns{DEFAULT_MAX_UTC_ERROR_NS};

    /// The largest worst-case error of the system clock at which the tunnel's
    /// talker vouches for the TAI timestamps it makes from that clock. Outside
    /// it the packets carry the tu (timestamp uncertain) bit; the audio and
    /// the timestamps are sent as before. Negative takes any bounded clock.
    int64_t udptun_max_tai_error_ns{DEFAULT_MAX_TAI_ERROR_NS};

    /// How fast the system clock's error may grow while its time daemon has
    /// no server: the deployment's oscillator and temperature figure.
    uint32_t utc_holdover_drift_ppb{realtime::DEFAULT_HOLDOVER_DRIFT_PPB};

    /// An error of the system clock that its own report does not show, in
    /// nanoseconds, added to its bound at every update: what lies between
    /// the time source and this clock that the time daemon does not measure
    /// (a receiver's pulse offset and cable, a path taken as symmetric).
    int64_t utc_fixed_error_ns{0};

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

    /// This unit's ENTITY serial_number (IEEE 1722.1-2021 Clause 7.2.1); empty serves the blob's.
    std::string serial_number{};
};

/// @p text as AvbEntityAudioIOConfig::default_mcr_priorities: comma-separated values 0-255 ("224",
/// "224,192"), empty for none; nullopt when an item is not one.
[[nodiscard]] inline auto parse_default_mcr_priorities(std::string_view const text) -> std::optional<std::vector<uint8_t>>
{
    std::vector<uint8_t> priorities;
    for (auto const field : std::views::split(text, ',')) {
        std::string_view const item{field.begin(), field.end()};
        uint8_t value = 0;
        auto const [end, ec] = std::from_chars(item.data(), item.data() + item.size(), value);
        if (ec != std::errc{} || end != item.data() + item.size()) {
            return std::nullopt;
        }
        priorities.push_back(value);
    }
    return priorities;
}

/// The ATDECC wire version the entity's descriptors are sent in: what the
/// configuration gives, or the profile's when it gives none. Milan v1.3 5.3.3
/// has the descriptors of IEEE 1722.1-2021; an entity held to 1722.1 alone
/// keeps the 2013/2016 lengths that older controllers expect.
[[nodiscard]] inline auto effective_atdecc_version(AvbEntityAudioIOConfig const& config) -> std::string_view
{
    if (!config.atdecc_version.empty()) {
        return config.atdecc_version;
    }
    return is_milan_profile(config) ? "2021" : "2016";
}

/// Where the talker streams' destination addresses come from: what the
/// configuration gives, or the profile's when it gives none. Milan v1.3
/// 4.3.3.1 has a talker's addresses allocated by MAAP; an entity held to
/// 1722.1 alone keeps its configured ones.
[[nodiscard]] inline auto effective_stream_address_mode(AvbEntityAudioIOConfig const& config) -> std::string_view
{
    if (!config.stream_address_mode.empty()) {
        return config.stream_address_mode;
    }
    return is_milan_profile(config) ? "maap" : "static";
}

}  // namespace statusbar::avb_entity
