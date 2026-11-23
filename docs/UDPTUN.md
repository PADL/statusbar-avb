<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# UDPTUN — UDP-Tunneled Measurement & Streaming Framework

`statusbar/udptun` is a generic scaffolding for sending and receiving
sequence-numbered, gPTP-timestamped UDP datagrams. It provides per-source
latency / loss tracking, optional dual-stream temporal-decorrelation
redundancy, and an opt-in stateless reflector for round-trip latency.
The wire packet format is a customization point: callers supply a
**Codec** that knows how to encode / decode bytes ↔ {sender_id,
sequence, tx_gptp_ns, app_data}, classify primary vs redundant copies,
and validate received bytes for the reflector.

The OWLM measurement tool (`statusbar/owlm`, `statusbar-owlm-tool`) is the
reference consumer. A second consumer — IEEE 1722-2025 AAF version 1
audio carried over IEEE 1722 Annex J UDP encapsulation — lives outside
this repo (in the AVB tooling that wants to reuse the framework).

## Module layout

| File | What it owns |
|---|---|
| `udptun_codec_concept.hpp` | `concept Codec`, `enum class PacketRole` |
| `udptun_session.hpp` | `TxContext`, `RxContext`, `LoopState` (non-templated) plus `send_packet<C>`, `drain_rx<C>`, `process_one_rx_datagram<C>`, `route_self_pair_packet<C>`, `send_redundant_replay<C>`, `drain_inflight_after_stop<C>` (templated, header-only) |
| `udptun_session_helpers.{hpp,cpp}` | Linux helpers — `make_periodic_timerfd`, `bind_to_device`, `is_ipv4_multicast`, `configure_multicast`, `build_pollfds`, `rtt_grace_ns` |
| `udptun_reflect.hpp` | `ReflectStats`, `ReflectConfig`, `print_reflect_summary`, `drain_reflect_rx<C>`, `run_reflect<C>` |
| `udptun_report.{hpp,cpp}` | `TxLossStats`, `RedundancyDisplayStats`, `print_live_report`, `print_final_summary` |
| `udptun_identity.{hpp,cpp}` | `LocalIdentityConfig`, `setup_local_identity`, `TxIdentityPair`, `print_eui64_banner` |
| `udptun_wire_clock.hpp` | `WireClock`, `monotonic_raw_ns`, `realtime_ns` |
| `udptun_stats.{hpp,cpp}` | `LatencyStats`, `percentile_from_histogram` |
| `udptun_per_source_tracker.{hpp,cpp}` | `SourceState`, `PerSourceTracker` |
| `udptun_redundant_rx.{hpp,cpp}` | `RedundantRxTracker` (PT-keyed slot map; backed by `container::PresentationSlotMap<Coverage, 8192>`) |
| `udptun_redundant_tx.hpp` | `RedundantTxBuffer` (1024-entry bounded ring) |
| `udptun_tx_history.hpp` | `TxHistory` (4096-entry bounded ring of `(timestamp, sent_count)`) |
| `udptun_monotonic_ring.hpp` | `MonotonicRing<Payload, N>` template that backs both ring buffers above |

## The Codec contract

`udptun::Codec` is a C++23 concept declared in
`udptun_codec_concept.hpp`. A type satisfies it by providing:

```cpp
struct MyCodec {
    using DecodedPacket = ...;  // codec-specific parsed form

    // Wire size of the header.
    auto header_size() const noexcept -> size_t;

    // Encode a packet header into `buf` at offset 0.
    auto encode(std::span<uint8_t> buf,
                ieee::Eui64 sender_id,
                uint32_t sequence,
                int64_t tx_gptp_ns,
                uint32_t interval_us) const noexcept -> size_t;

    // Decode a received datagram, or nullopt on parse error.
    auto decode(std::span<uint8_t const> bytes) const noexcept
        -> std::optional<DecodedPacket>;

    // Reflector-side validation. Typically: return decode(bytes).has_value();
    auto validate_for_reflect(std::span<uint8_t const> bytes) const noexcept -> bool;

    // Map a decoded packet to one of:
    //   SelfPrimary, SelfRedundant, SelfLegacy,
    //   RemotePrimary, RemoteRedundant, RemoteLegacy.
    auto classify(DecodedPacket const& p, ieee::Eui64 const& my_pair_id) const noexcept
        -> udptun::PacketRole;

    // Accessors on a decoded packet.
    auto sender_id(DecodedPacket const&) const noexcept -> ieee::Eui64;
    auto sender_pair_id(DecodedPacket const&) const noexcept -> ieee::Eui64;
    auto sequence(DecodedPacket const&) const noexcept -> uint32_t;
    auto tx_gptp_ns(DecodedPacket const&) const noexcept -> int64_t;
    auto announced_interval_us(DecodedPacket const&) const noexcept -> uint32_t;
};
```

`sender_pair_id` is the **canonical logical sender** — for codecs that
pair primary and redundant under one logical stream, this collapses
both to the same value so `PerSourceTracker` shows one row per pair.
Codecs without primary/redundant pairing return `sender_id(p)` here.

A static_assert at the bottom of the codec's header pins down
conformance:

```cpp
static_assert(udptun::Codec<MyCodec>);
```

See `statusbar/owlm/owlm_codec.hpp` for the OWLM reference codec
(stateless, 32-byte OWLM measurement header, primary/redundant
distinguished by EUI-64 mid bytes).

## Building a session

A consumer wires up:

1. Resolve the local identity (gPTP slave session if available, MAC
   read otherwise):
   ```cpp
   udptun::LocalIdentityConfig idcfg{ /* no_gptp, fallback_iface, gptp_session_config */ };
   std::optional<gptp::SlaveSession> session;
   auto local_mac = udptun::setup_local_identity(idcfg, session);
   ```

2. Build the TX identity pair (primary and redundant):
   ```cpp
   udptun::TxIdentityPair tx{
       .primary_id = make_my_primary_eui64(*local_mac),
       .redundant_id = make_my_redundant_eui64(*local_mac),
       .redundant_enabled = true,
       .temporal_shift_ns = 10'000'000,  // 10 ms
   };
   ```

3. Set up UDP socket, timerfds, trackers (see `owlm_tool.cpp` for
   the full sequence).

4. Build `TxContext` and `RxContext` referencing the long-lived state.

5. Construct a codec instance and a `LoopState`:
   ```cpp
   MyCodec const codec{ /* codec config */ };
   udptun::LoopState const ls{ session, udp, tx_tfd, report_tfd, tx_ctx, rx_ctx,
                               tx_history, start_mraw, redundancy_enabled };
   ```

6. Dispatch in your main loop:
   ```cpp
   while (!stop.test()) {
       std::array<pollfd, 4> fds{};
       size_t const n_fds = udptun::build_pollfds(session, udp.get(), tx_tfd.get(), report_tfd.get(), fds);
       ::poll(fds.data(), n_fds, 100);
       // ... per-fd dispatch:
       udptun::send_packet(codec, tx_ctx, sequence, synced);
       udptun::drain_rx(codec, rx_ctx, synced);
       // ... and report_tfd handling
   }
   udptun::drain_inflight_after_stop(codec, rx_ctx, synced, drain_ns);
   udptun::print_final_summary(std::cout, "MyTool", tracker, &rtt_stats, &tx_stats, &red_stats);
   ```

## Redundancy semantics (ST 2022-7 style)

When `tx.redundant_enabled` is true, every TX timer expiry sends:

1. A primary packet stamped with `tx.primary_id`, the current sequence,
   and the current gPTP-rated wall time.
2. A redundant packet that **replays** an earlier primary's
   `(sequence, tx_gptp_ns)` from `temporal_shift_ns` ago, but stamped
   with `tx.redundant_id`. Skipped if the buffer doesn't yet hold a
   packet from that far back (i.e. during the first `temporal_shift_ms`
   after startup).

On the receive side, `RedundantRxTracker` tracks per-sequence coverage:
primary received (normal), only redundant arrived (recovered — primary
was lost but redundant rescued the data), or neither (true loss). The
report line gains `recovered=R true_loss=Y% (T)` describing the
effective drop rate before and after redundancy.

Caveat: a single-NIC udptun consumer only protects against **temporal**
decorrelation (bursty losses up to ~`temporal_shift_ms` wide). For true
**path** diversity, the two streams must traverse physically separate
paths, which the framework does not yet arrange itself.

## Allocation discipline

After construction, the framework's hot path does not allocate:

- `RedundantTxBuffer` and `TxHistory` are fixed-size arrays.
- `RedundantRxTracker` is a `container::PresentationSlotMap<Coverage,
  8192>` — a fixed-capacity time-keyed slot ring, ~80 KB always
  resident. Each slot holds `(quantized_PT, primary_received,
  redundant_received)`. Reordering is implicit because the slot index
  derives from the packet's PT, not its arrival order.
- `PerSourceTracker` slots are allocated in the constructor and then
  reused with LRU eviction.
- `tx_buf` is a `std::vector<uint8_t>` whose capacity stabilizes after
  the first send.

This matters even though the framework runs in a userspace tool, not in
an audio RT thread — predictable behavior under load is part of the
design.

## Presentation-time stamping

The framework stamps each TX packet with a **presentation time** (PT):

    PT = wire_ns(monotonic_raw_ns()) + worst_case_latency_ns

`worst_case_latency_ns` is `SessionConfig.worst_case_latency_ns`, set by
the caller (e.g. statusbar-owlm-tool's `--worst-case-latency-ms`). It is the
tolerated worst-case one-way latency for the deployment; on-time
packets land at lateness ≤ 0 and a positive lateness means the packet
missed its WCL deadline. The receiver:

- Uses PT as the slot key in `RedundantRxTracker` —
  primary/redundant copies of the same logical packet share PT and
  therefore the same slot.
- Computes `lateness = rx_wall - PT` as the principal one-way diagnostic.
  When `worst_case_latency_ns == 0`, lateness equals real one-way transit.
  When non-zero, lateness is the delivery delay relative to the WCL
  deadline, and real transit = lateness + WCL (caller knows WCL
  externally).
- For self-loopback echoes (RTT measurement), the sender adds its own
  WCL back to recover real RTT: `rtt = (rx_gptp - PT) + WCL`. This
  happens inside `route_self_pair_packet`.

Sequence numbers are still carried for missing-message accounting in
`PerSourceTracker`, but reordering within an RTT window is handled
automatically by PT-keyed slot lookup — out-of-order primaries land in
the slot indexed by their PT regardless of arrival order.

## Wire-time source

`udptun::WireClock::wire_ns(monotonic_raw_ns)` translates from
`CLOCK_MONOTONIC_RAW` to the on-the-wire timeline:

- With gPTP (`session.has_value()` and `bridge != nullptr`),
  `monotonic_raw` is offset-translated through the gPTP grandmaster's
  clock domain (offset only, no rate extrapolation — see header
  comment for the rationale). Both endpoints stamp packets in the
  same TAI-based domain, so one-way latency is meaningful.
- Without gPTP (`--no-gptp` deployments), `realtime_ns()` is read
  directly. Both endpoints rely on NTP for rough alignment — fine for
  ms-level RTT, acceptable for one-way LAN measurement.

### When the session trusts its clocks

A session does not ask its time source whether it is synchronized. It asks
how large the worst-case error is and compares that with its configuration
(`udptun_clock_gate.hpp`):

| `SessionConfig` field | Default | Clock judged |
|---|---|---|
| `max_clock_error_ns` | 100 000 (100 µs) | the master link: the gPTP slave's bridge or the ptp4l PHC bridge. Negative takes any bounded link |
| `max_utc_error_ns` | 0 (reported, not enforced) | the system clock, when the wire timeline is made from it (`wire_clock_gps_tai`, or the realtime time source) |

Inside its limits the session measures and transmits; outside them it does
neither, exactly as it used to while unsynchronized. The difference is when
that happens: a link that misses a few Syncs keeps its rate and offset and
its error grows slowly, so the session runs on for seconds and stops only
when the error passes the limit. `Session::clock_report()` returns the
current bounds and the record of excursions, and `print_clock_report()`
writes them as the report's last line.

For RTT measurement against a reflector, the sender's wire_ns appears
on both ends of the interval (it stamps tx, then receives its own echo
and computes `rx_wire - tx_wire`), so the rate calibration cancels and
the only error is jitter on the local clock-read syscalls — sub-µs in
practice.

## Reflector

`udptun::run_reflect<Codec>(codec, ReflectConfig)` runs a stateless
validate-then-echo loop. Any datagram for which
`codec.validate_for_reflect(bytes)` returns true is bounced back to the
source verbatim; everything else is counted as `dropped_invalid`. The
response size equals the request size, so the reflector cannot be used
as a UDP amplification vector.

See `docs/OWLM.md` for the OWLM-specific deployment story
(`reflector.example.com`).

## Reference codecs

- **`statusbar/owlm/owlm_codec.hpp`** — `OwlmCodec`. Stateless. 32-byte
  measurement header (magic `OWLM`, version 1, sender EUI-64,
  sequence, tx_gptp_ns, tx_interval_us). Primary/redundant via EUI-64
  mid bytes (`kPrimaryMidBytes = 0x0000`, `kRedundantMidBytes =
  0x0001`); `eui64_pair_id` canonicalizes both to the same logical
  sender.

- **AAF v1 over Annex J** — planned, lives in the AVB tooling. The
  Annex J 32-bit outer sequence drives the framework's transport-level
  loss/redundancy tracking; the AAF v1 inner header carries audio
  metadata (per-stream sequence, 64-bit `avtp_timestamp`, 8-byte
  grandmaster identity). Primary/redundant distinguished by configured
  stream_id pair.

## CSV output

`SessionConfig::csv_output_path` enables per-packet CSV dump at
session end. The columns (defined in
`statusbar/udptun/udptun_csv_record.hpp`) are codec-agnostic — any
udptun consumer (OWLM today, AAF-over-Annex-J in the future) gets
the same schema for free. The realtime path stays IO-free: records
append to a pre-reserved `std::vector<UdpTunCsvRecord>` and the
flush via `statusbar::csv::CsvWriter` happens once at session
teardown.
