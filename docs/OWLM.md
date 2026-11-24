<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# OWLM — One-Way Latency Measurement Tool

`statusbar-owlm-tool` measures one-way UDP packet latency between two end
stations using gPTP-synchronized timestamps. Both sides send and
receive; each side maintains per-source latency statistics
(min/mean/p50/p95/p99/max, full histogram, packet loss).

Linux-only. Requires `CAP_NET_RAW + CAP_NET_ADMIN`.

The tool is a thin consumer of the **udptun** framework
(`statusbar/udptun`, see `docs/UDPTUN.md`). It supplies an `OwlmCodec`
adapter for the 32-byte OWLM measurement header and lets udptun own
the TX/RX scaffolding, redundancy tracking, reflector, and report
formatting. Other codecs (e.g. AAF v1 over IEEE 1722 Annex J for AVB
audio over UDP) plug into the same framework.

## Quick start (two nodes)

On node A (192.0.2.1):

```
sudo setcap cap_net_raw,cap_net_admin=ep ./statusbar-owlm-tool
./statusbar-owlm-tool --gptp-interface=eth0 --peer=192.0.2.2 --tx-interval-us=1000
```

On node B (192.0.2.2):

```
./statusbar-owlm-tool --gptp-interface=eth0 --peer=192.0.2.1 --tx-interval-us=1000
```

After ~10 s each side prints summary lines like:

```
[    10.001]
  eui64=00:11:22:FF:FE:33:44:55 count=10000 loss=0.00% \
    min=0.0ms mean=0.0ms p50=0.0ms p95=0.0ms p99=0.0ms max=0.1ms ooo=0 dup=0
  RTT count=10000 min=0.0ms mean=0.0ms p50=0.0ms p95=0.0ms p99=0.0ms max=0.2ms \
    sent=10000 loss=0.00% (0 missing sequence ids) recovered=0 true_loss=0.00% (0)
```

Press Ctrl-C to print the final summary including the full ASCII
histogram per source.

## Redundant dual-stream mode (default)

By default `statusbar-owlm-tool` sends every TX opportunity twice: a *primary*
packet (EUI-64 mid bytes `00:00`) carrying the current sequence and gPTP
timestamp, and a *redundant* packet (mid `00:01`) replaying the
primary packet sent `--temporal-shift-ms` (default 10 ms) earlier. The
reflector echoes both copies. The receiver:

- only feeds primary echoes into the RTT histogram (the redundant
  echoes lag by exactly the temporal shift);
- runs both copies through a per-pair coverage tracker that classifies
  each sequence as *primary received*, *recovered* (primary lost,
  redundant arrived) or *true loss* (both copies lost).

The RTT line gains `recovered=R true_loss=Y% (T)` so you can see the
effective drop rate before and after redundancy.

Caveat: a single-NIC `statusbar-owlm-tool` only protects against temporal
decorrelation (bursty losses up to ~`temporal-shift-ms` wide). For
true path-diversity redundancy (ST 2022-7 style), the two streams must
go over physically separate paths, which this tool does not yet do.

Pass `--no-redundant` to fall back to a single legacy stream (uses
`--id-mid`, no coverage tracker, no extra bandwidth).

## Presentation-time stamping (`--worst-case-latency-ms`)

`statusbar-owlm-tool` stamps each packet with a *presentation time* (PT) rather
than the raw acquisition wall:

    PT = acquisition_wall + worst_case_latency_ms × 1e6

Default is 60 ms. Set it to the deployment's tolerated worst-case
one-way latency. This serves two purposes:

- **Receiver-side reordering is implicit.** The udptun receiver keys
  its primary/redundant coverage tracker by PT. Out-of-order primaries
  land in the slot indexed by their PT regardless of arrival order, so
  there is no seq-gap bookkeeping for redundancy accounting.
- **Lateness is a deadline-relative diagnostic.** The receiver displays
  `lateness = rx_wall - PT`. A deployment with WCL = 60 ms whose actual
  one-way transit is 50 ms shows lateness ≈ −10 ms (10 ms of headroom).
  Lateness > 0 means the packet missed the WCL deadline; lateness ≤ 0
  means it arrived on time. Real one-way transit = lateness + WCL — the
  operator computes it externally because both ends know WCL.

For self-loopback / reflector RTT measurement, the sender adds its own
WCL back to recover real RTT (`rtt = rx_wall - PT + WCL`), so the
displayed RTT is unaffected by the choice of WCL.

Pass `--worst-case-latency-ms=0` to recover the original "PT ==
acquisition wall" semantic (lateness ≡ real one-way transit).

## Running without gPTP (`--time-source=realtime`)

When there's no gPTP grand master on the segment (e.g. the RPi is
plugged straight into a consumer router, or you just want a quick RTT
sanity check), pass `--time-source=realtime`. Behaviour:

- the gPTP slave session is skipped entirely (no PHC, no raw sockets,
  no `CAP_NET_RAW`);
- packet PT (the `tx_gptp_ns` field) and `rx_gptp_ns` are read
  directly from `CLOCK_REALTIME`;
- there is no master clock link to judge, so nothing holds TX/RX back on
  its account; with `--max-utc-error-us` set, the system clock's own error
  figure is the limit and the session stops while it is outside;
- the EUI-64 is built from the MAC of `--owlm-interface` (or
  `--gptp-interface` if the former is empty).

RTT measurement is fully accurate in this mode (same clock on both
the tx and rx stamps). For one-way measurement between two hosts the
two CLOCK_REALTIMEs need to be roughly in agreement — NTP is fine for
ms-scale work; don't expect µs accuracy without gPTP or PTP.

## IPv6 unicast

```
./statusbar-owlm-tool --gptp-interface=eth0 --peer=fe80::1%eth1 --tx-interval-us=1000
```

The local bind family is inferred from `--peer`. For RX-only mode (no
`--peer`), pass `--ipv6` to bind the IPv6 wildcard. IPv6 multicast is
not yet supported.

## Reflect mode (round-trip latency)

For round-trip latency measurements without requiring gPTP synchronization
between the two endpoints — for example, probing a remote server from a
NATed home device — run a stateless reflector on the remote and a normal
sender locally:

On the remote (e.g. `reflector.example.com`):

```
./statusbar-owlm-tool --mode=reflect --local-port=9991
```

On the local sender:

```
./statusbar-owlm-tool --gptp-interface=eth0 --peer=reflector.example.com \
            --peer-port=9991 --tx-interval-us=100000
```

The reflector validates each datagram as a well-formed OWLM packet (magic,
version, reserved-zero, sender-synced) and bounces accepted datagrams
verbatim back to the source IP and port. Random non-OWLM traffic is
counted as `dropped_invalid` and not echoed. The response is the same
size as the request, so the reflector cannot be used as an amplification
vector.

The local sender detects its own EUI-64 in returning packets and records
them as RTT samples in a separate stats bucket from the per-source
one-way stats. Live reports include an extra line:

```
  RTT count=N min=…ns mean=…ns p50=…ns p95=…ns p99=…ns max=…ns
```

and the final summary additionally prints the RTT histogram. Reflect mode
itself does not require gPTP, but the local sender does — RTT timestamps
on the sender side use the same gPTP-rated clock as the one-way path, so
the rate calibration cancels across each round-trip interval.

## Multicast

```
./statusbar-owlm-tool --gptp-interface=eth0 --peer=239.1.2.3 --mcast-ttl=2 \
            --tx-interval-us=1000 --dscp=46
```

`--dscp=46` (Expedited Forwarding) puts packets in the same QoS class
as AVB Class A streams — useful for quantifying queue delay in TSN
networks.

## CLI reference

| Flag                       | Default          | Description                                                |
|----------------------------|------------------|------------------------------------------------------------|
| `--mode {normal,reflect}`  | `normal`         | `reflect` runs a stateless UDP echo for RTT measurement    |
| `--gptp-interface NAME`    | `eth0`           | Interface used for gPTP                                    |
| `--owlm-interface NAME`    | (wildcard)       | Interface for UDP send/recv (`SO_BINDTODEVICE`)            |
| `--peer HOST`              | required if TX   | Destination IPv4 (unicast or multicast) or IPv6 (unicast)  |
| `--peer-port PORT`         | `9991`           | Destination UDP port                                       |
| `--local-port PORT`        | `9991`           | Local UDP bind port                                        |
| `--tx-interval-us US`      | `100000` (10 Hz) | µs between transmits; `0` disables sending (RX-only)       |
| `--report-interval-ms MS`  | `1000` (1 s)     | Live summary cadence                                       |
| `--id-mid HEX`             | `0xFFFE`         | EUI-64 mid bytes (legacy single-stream; ignored if redundant) |
| `--no-redundant`           | off              | Disable the default dual-stream redundancy                 |
| `--temporal-shift-ms MS`   | `10`             | Delay between primary and its redundant copy               |
| `--worst-case-latency-ms MS` | `60`           | Offset between acquisition time and the presentation time stamped on the wire |
| `--time-source {gptp-slave,ptp4l,realtime}` | `gptp-slave` | Where wire time comes from: the built-in gPTP slave on `--gptp-interface`, a PHC that ptp4l keeps in sync (`--ptp.*` options), or `CLOCK_REALTIME` with no grandmaster |
| `--wire-clock {master,gps-tai}` | `master`    | Timeline of the on-wire timestamps: the time source's own, or absolute TAI made from the system clock (needed between sites whose grandmasters are not on TAI) |
| `--tai-minus-utc-ns NS`    | `37000000000`    | TAI − UTC, applied with `--wire-clock=gps-tai`             |
| `--payload-bytes N`        | `0`              | Pad bytes after the 32-byte header                         |
| `--mcast-ttl N`            | `1`              | `IP_MULTICAST_TTL` for multicast peers                     |
| `--dscp N`                 | `46` (EF)        | DSCP value (0–63); pass `-1` to disable. Wi-Fi maps EF→AC_VO |
| `--ipv6`                   | off              | Bind IPv6 in RX-only mode (TX mode infers from `--peer`)   |
| `--max-sources N`          | `32`             | Cap on tracked sources                                     |
| `--max-clock-error-us US`  | `100`            | Largest worst-case error of the master clock link to measure on; negative takes any bounded link, `0` is met by no link. See Accuracy |
| `--max-utc-error-us US`    | `0` (report only)| Largest worst-case error of the system clock to measure on, when the wire timeline is made from it. Here `0` means no limit is enforced |
| `--utc-holdover-drift-ppb N` | `15000`        | How fast the system clock's worst-case error grows from its time daemon's last update |
| `--ptp.holdover_drift_ppb N` | `15000`        | How fast the master link's worst-case error grows without synchronization, for either time source |
| `--utc-fixed-error-ns NS`  | `0`              | An error of the system clock that its own report does not show (a receiver's pulse offset, a path taken as symmetric), added to its worst-case error |
| `--ptp.fixed_error_ns NS`  | `0`              | An error of the master link's hardware that no measurement shows (PHY asymmetry, timestamp granularity), added to its worst-case error, for either time source |
| `--hist-low-ns NS`         | `0`              | Histogram lower bound                                      |
| `--hist-high-ns NS`        | `300000000`      | Histogram upper bound (300 ms)                             |
| `--hist-bucket-ns NS`      | `500000`         | Histogram bucket width (500 µs → 600 bins)                 |
| `--software`               | off              | Software gPTP timestamping (no PHC)                        |
| `--profile {standard,automotive}` | `standard`| gPTP profile                                               |
| `--verbose`                | off              | Per-sync verbose gPTP output                               |

## Accuracy

Userspace timestamps are read from `CLOCK_MONOTONIC_RAW` immediately
before `sendto` and immediately after `recvfrom` returns, then
converted to the gPTP domain via `GptpTimeBridge`. Latency target on
a quiet wired Linux box: **±100 µs**. Sources of jitter:

- Kernel scheduling between `recvfrom` return and our timestamp call
- gPTP bridge offset / rate update granularity (sub-µs typical)
- Sender-side syscall path before the packet reaches the wire

### How well the clocks are known

Every figure owlm prints rests on its master clock link: the gPTP slave's
bridge, or the ptp4l PHC bridge. The link reports a worst-case error that is
refreshed at each synchronization and grows while synchronization is
interrupted, because the link keeps converting with the rate and offset it
had. owlm compares that error with `--max-clock-error-us` (default 100) and
measures and sends only while it is inside. Three missed Syncs add a few
microseconds and change nothing. A link that coasts at the default holdover
figure of 15 ppm leaves a 100 µs limit after about six and a half seconds;
measurement stops there and resumes when synchronization returns. A new
grandmaster is a new timeline: the link is unbounded until it has been
fitted again.

What the figure covers depends on the time source. With
`--time-source=ptp4l` the link is the PHC as ptp4l disciplines it: the bound
says how well the PHC is read, not whether ptp4l still follows its
grandmaster, so a ptp4l that has lost its master is not seen here. A PHC
slewed more than 200 ppm against the local crystal has no bound at all until
the slew is back inside that range. With the built-in gPTP slave the packet
stamps use the bridge's offset alone, which is exact at each Sync and then
moves at the local crystal's rate offset until the next one. The bound
printed is that line's own: a few microseconds while Syncs arrive, growing
at the holdover figure plus the whole rate offset once they stop. A crystal
40 ppm off leaves a 100 µs limit about two seconds after the last Sync. A
new grandmaster has no bound until a rate has been fitted to its own Syncs,
which takes a second of them.

How fast either link's error grows without synchronization is
`--ptp.holdover_drift_ppb` (default 15 ppm, the tolerance NTP assumes of any
clock); a machine whose oscillator is known to be steadier can be told so
and coasts for longer.

Both bounds are built from what the tool can measure. An error that is
constant, such as a PHY's latency asymmetry or a GPS receiver's pulse
offset, is invisible to a fit, so the bound is too small by that amount
unless it is told: `--ptp.fixed_error_ns` for the master link and
`--utc-fixed-error-ns` for the system clock. They are figures of the
hardware at each site and default to 0.

When the wire timeline is made from the system clock (`--wire-clock=gps-tai`
or `--time-source=realtime`), a figure compared across two sites also
depends on how well each site's system clock knows UTC. owlm reads the
operating system's own worst-case error once a second (what chrony or ntpd
last told the kernel) and coasts it through a loss of the time server. It is
shown in the report. It is enforced only when `--max-utc-error-us` is set,
since a round trip or a self-loopback is measured on one clock and does not
depend on it. The kernel grows its figure by half a millisecond every second
between the time daemon's updates, so the first figure owlm sees can be tens
or hundreds of milliseconds; it tightens to the daemon's own rating at the
next update and is coasted from there.

Each live report and the final summary end with one line:

```
  clock: link +/-7.7 us (limit +/-100.0 us, synced 0.3 s ago) excursions=0 outside=0.0 s timeline_changes=0 utc +/-1.5 ms
```

`excursions` counts the times the link left its limit, `outside` the time
spent there, `timeline_changes` the grandmaster changes and time steps seen.
`NOT USABLE` is appended while nothing is being measured or sent. The bounds
are worst cases, rounded up: a one-way figure between two sites is good to
the sum of both sites' bounds, and usually far better.

With `--wire-clock=gps-tai` the mapping from the master timeline to TAI is
learned from pairs of the two clocks, four a second. It is learned only while
the link is inside its limit, and it starts over when the link moves to a new
timeline. The filter widens itself when it finds its samples persistently
outside its gate (a seed taken from a bad reading, a slew of the system
clock it had not allowed for); the clock line then also shows
`tai filter self-corrections N`. One or two at the start of a run are
harmless. A count that keeps climbing means the system clock or the link is
moving in steps.

Hardware timestamping on the UDP NIC is *not* used — its PHC drifts
independently of the gPTP NIC's PHC, and bridging across two PHCs
adds a second layer of conversion error. Future versions may add
`SO_TIMESTAMPING` (kernel software stamping) for a tighter floor.

## Wire format

32-byte fixed header, big-endian:

| Offset | Size | Field            |
|--------|------|------------------|
| 0      | 4    | magic = "OWLM"   |
| 4      | 2    | version = 1      |
| 6      | 2    | flags = 0        |
| 8      | 8    | sender_eui64     |
| 16     | 4    | sequence         |
| 20     | 8    | presentation_time_ns (`tx_gptp_ns` field) |
| 28     | 4    | tx_interval_us   |
| 32     | …    | optional payload pad |

Receivers drop datagrams whose magic, version, flags, length, or
interval are out of range. See `statusbar/owlm/owlm_packet.cpp` for
the canonical validation.

## Smoke test

`scripts/owlm/loopback_smoke.sh` runs two `statusbar-owlm-tool` instances on
`lo` (with separate ports) for a few seconds and asserts packet
counts. It carries the ctest label `functional` and runs with the
functional tests. Hand-run with:

```
OWLM_TEST_IFACE=eth0 ctest -R owlm_loopback
```

## CSV output for offline analysis

Pass `--csv-output=PATH` to `statusbar-owlm-tool` to write a per-packet CSV at
session end. Schema (header row included):

```
rx_gptp_ns,presentation_time_ns,latency_ns,sender_eui64,sequence,interval_us,role
```

- `rx_gptp_ns` — receive-side master-clock timestamp.
- `presentation_time_ns` — tx-side stamped time on the wire.
- `latency_ns` — measured lateness (`rx_gptp − presentation + worst_case_latency [+ peer_tai_offset_ns]`).
- `sender_eui64` — lowercase hex with colons.
- `sequence` — codec wire sequence.
- `interval_us` — `announced_interval_us` from the codec.
- `role` — `remote_primary`, `remote_redundant`, `remote_legacy`,
  `self_primary`, `self_redundant`, `self_legacy`.

Records are streamed to disk continuously by a low-priority writer
thread fed through an SPSC ring (~48 B per record). The realtime
TX/RX path only enqueues into the ring — it never touches disk. The
ring depth (~3 MB / 65536 records) absorbs ~30 s of writer stalls at
2 krec/s; longer stalls (e.g. multi-second SD-card GC pauses) cause
records to be dropped and counted, and the drop count is printed on
stderr at exit when non-zero.

Analyze captured CSVs with `python/owlm/owlm_analyze.py`, installed as `statusbar-owlm-analyze`:

```
statusbar-owlm-analyze join A.csv B.csv [--bin-width-us=250]
statusbar-owlm-analyze plot A.csv [B.csv] -o plot.png [--window-ms=100]
statusbar-owlm-analyze events A.csv \
    [--upper-threshold-ms=5] [--negative-threshold-ms=-1] -o events.csv
```

`events` streams the input (.csv / .csv.gz / .colbin) and emits one
row per outlier (filtered to remote_primary / remote_legacy roles, the
same scope `join`'s `min_ms` / `max_ms` come from). Each row has
`rx_gptp_ns`, a human-readable `rx_pdt`, `presentation_time_ns`,
`latency_ns`, `latency_ms`, `sender_eui64`, `sequence`, `interval_us`,
`role`, and a `direction` tag (`low` or `high`). A short preview is
printed to stdout. The legacy `--threshold-ms` flag is kept as an
alias for `--upper-threshold-ms`.

`plot` accepts optional extra inputs to extend the report with
wan-timer diagnostics and per-packet outlier panels. Each is
independent — pass any subset:

```
statusbar-owlm-analyze plot A.parquet B.parquet -o report.pdf \
    --wake-csv-a A-wake.csv     --wake-csv-b B-wake.csv \
    --duration-csv-a A-dur.csv  --duration-csv-b B-dur.csv \
    --outliers-csv-a A-out.csv  --outliers-csv-b B-out.csv
```

The wake / duration CSVs are the outputs of statusbar-owlm-tool's
`--wan-timer-wake-stats-csv` / `--wan-timer-duration-stats-csv`. Each
adds one bar-plot row (bin centres on x in µs, count on symlog y;
underflow / overflow tail counts shown in the title). The outliers
CSVs are the outputs of `statusbar-owlm-analyze events`. Each adds two
rows per device: a scatter of latency vs session-time coloured by
direction, plus a histogram of outlier latencies split by direction.

The script is `uv`-runnable (PEP 723 inline metadata) — invoke
directly, no `pip install` needed.

## Run-duration and timer-histogram CSV

- `--duration-s=N` schedules a graceful shutdown after `N` wall-clock
  seconds from session start (fractional seconds allowed). The same
  drain, summary, and CSV flush path that runs on SIGINT runs at the
  deadline. `0` (default) keeps the current behavior: run until
  SIGINT / SIGTERM.
- `--wan-timer-wake-stats-csv=PATH` and
  `--wan-timer-duration-stats-csv=PATH` dump the realtime timer's
  wake-error and callback-duration histograms at end of run.
  Collected on every tick regardless of `--enable-tripwire`. Schema:
  ```
  bin_low_ns,bin_high_ns,count
  ```
  Underflow row leaves `bin_low_ns` empty; overflow row leaves
  `bin_high_ns` empty. Bin edges and width come from
  `--wan_timer.error_histogram.*` / `--wan_timer.duration_histogram.*`
  (defaults: −10 µs … +200 µs / 5 µs bins for wake error, 0 … +200 µs
  / 5 µs bins for callback duration). Only populated on the
  realtime-timer code path (gPTP slave or ptp4l with
  `--wan_timer.period_ns > 0`); polling fallback prints a warning and
  skips the CSV.
- `--wan_timer.lock_memory=false` keeps the realtime timer's thread from
  calling `mlockall` when it starts (the default locks the whole
  process). With `--time-source=ptp4l` the PTP setup owns memory
  locking: it locks once, or not at all under `--ptp.no-mlock`, and the
  timer then never locks behind that flag.
