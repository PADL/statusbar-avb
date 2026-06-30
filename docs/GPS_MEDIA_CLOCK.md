<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# GPS-frequency-locked distributed media clock (multi-site AVB over WAN)

Design for frequency-locking media clocks across geographically separate AVB
LANs (Site A / Site B / Site C) to a common GPS reference, while keeping
**low-jitter local AVTP timing**, and bridging audio between sites over the public
internet with `UDPTUN` (see [`UDPTUN.md`](UDPTUN.md), owlm).

## Problem

A GPS time server is a ~microsecond-class PTP
grandmaster — **±500 ns jitter** with intermittent ±500–700 ns spikes (its own
datasheet/accuracy report; see [`LINUXPTP.md`](LINUXPTP.md) "Grandmaster sync
quality"). Relaying it through a switch boundary clock and recovering a **media
clock** from it directly gives ~700–800 ns of wander at endpoints — enough for a
DSP processor to flag "AVB sync" yellow and produce audible 1-sample slips
("snats"). A **free-running AVB switch** as gPTP GM, by contrast, gives ~20–30 ns
locally — but is not frequency-locked to GPS, so two sites' free-running switches
drift relative to each other (±ppm) and can't stay sample-correlated over a WAN.

We need BOTH: low local jitter AND a GPS-common frequency across sites.

## Key idea: separate frequency (syntonization) from phase (synchronization)

The media clock fundamentally needs **frequency** accuracy (so samples don't slip),
not absolute phase. GPS frequency is rock-stable (~1e-12); the ±500 ns is just
timestamp *noise* on top. So:

- **Local phase/timestamps** come from the **free-running switch gPTP GM** — clean
  (~30 ns), per-LAN. This is the AVTP timebase.
- **Frequency** comes from **GPS via a long-time-constant servo** that averages the
  ±500 ns noise down to **sub-ppb** frequency — delivered on a *separate* path that
  never touches the local gPTP.

Result: media-clock **jitter ≈ local gPTP (~30 ns)**, media-clock **frequency =
GPS**. Best of both. This is the classic telecom syntonization-vs-synchronization
split, and how broadcast genlock-over-IP / AES67-over-WAN with a common reference
works.

## Per-Pi5 clock map

Two intentionally-decoupled clocks on each Pi5 (supersedes the old
`REALTIME = gPTP` design in [[ptp-bridge-clock-domain-design]]):

| Clock | Disciplined by | Equals | Jitter | Used for |
|-------|----------------|--------|--------|----------|
| **PHC** (`/dev/ptp0`) | `ptp4l` (L2 gPTP slave to local switch, HW timestamps) | local switch time (free-runs vs GPS) | ~30 ns | AVTP `avtp_timestamp` / AM824 CIP SYT |
| **CLOCK_REALTIME** | `chronyd` ← GPS grandmaster NTP (GPS, Stratum-1) | GPS time | ~tens of µs phase, sub-ppb freq | GPS time-of-day for the WAN tunnel; frequency reference |

- **Stop `phc2sys`** (and `systemd-timesyncd`) — it previously forced
  CLOCK_REALTIME to follow the PHC. The two clocks are now different on purpose.
- **`r = switch_rate / GPS_rate`** and the GPS↔switch offset are read with the
  **`PTP_SYS_OFFSET_PRECISE` ioctl** on `/dev/ptp0` (atomically-correlated
  PHC + CLOCK_REALTIME), tracked over time. This needs no extra PTP daemon.

### Why NTP (chronyd) for the GPS path, not a 2nd ptp4l
We need GPS *frequency* (excellent via chronyd) + *coarse* GPS time-of-day (~tens of
µs is fine; the local switch already provides the tight timebase). NTP's larger
per-sample jitter washes out in the long-servo frequency extraction — the ±500 ns
PTP jitter was a *phase* problem, irrelevant to the *frequency* we want here. NTP
also **avoids the PHC-contention trap**: a second HW-timestamping ptp4l would also
reference the PHC, which the gPTP slave already owns; chronyd disciplines
CLOCK_REALTIME with no contention. (If HW-timestamped frequency is ever needed: run
a 2nd ptp4l on UDP in `free_running` measure-only mode and feed its GPS-vs-PHC
offset to a custom servo. More code; overkill for frequency-only.)

## Media-clock generation (the marriage)

The Pi5 **produces audio samples at the measured GPS rate** but **stamps
`avtp_timestamp` / CIP SYT in PHC (switch) time**. Concretely: sample N is placed at
the PHC time corresponding to `(GPS_time_0 + N/Fs)`, converted through `r`. Because
the avtp_timestamps advance at GPS-rate (expressed in switch ticks) and are placed
smoothly (r from the long servo is smooth), the local receiver:

- recovers the **media clock = GPS frequency** (from the avtp_timestamp deltas), and
- interprets those timestamps via its own clean lock to the **local switch gPTP**,

so the receiver's recovered media clock is **GPS-frequency-locked with ~30 ns
jitter** — exactly the goal. The switch's absolute frequency offset from GPS is
transparent because both the Pi5's stamping and the receiver's recovery use the
same local switch time; the GPS rate lives in the timestamp deltas.

## Inter-site (WAN) via UDPTUN

- The tunnel carries **GPS timestamps** (CLOCK_REALTIME). These need only arrive "in
  time"; they do **not** need low jitter — they establish/maintain the slowly-varying
  GPS↔local mapping, which is heavily averaged.
- Each site maps **GPS time ↔ local switch time** continuously via its tracked `r`.
  Site B plays site A's samples at the correct GPS instant expressed in *current*
  switch_B time.
- A **~25 ms presentation buffer** absorbs internet latency variation. Because the
  media is GPS-frequency-locked, there is no systematic fill/drain; inter-channel
  correlation holds even if absolute offset is off by a few samples.

### Subtlety: the inter-site mapping is *tracked*, not *fixed*
The avtp timebase is the **local switch, which free-runs vs GPS** (only the media
*clock* is GPS-locked, not the switch). So the numeric avtp_timestamp offset between
sites **drifts at the switches' oscillator offset (±ppm)** — a one-time-latched delta
would traverse the 25 ms buffer in **~80 min** at ±5 ppm. The fix (already implied by
"tunnel carries GPS timestamps"): **anchor playout on GPS time and continuously
re-map GPS↔local at each site.** The *media-sample* correlation IS effectively fixed
(media is GPS-locked); only the numeric offset slowly tracks, and the GPS anchoring
follows it. Do not cache the delta and walk away.

## Implementation notes / gotchas

1. **GPS path is passive w.r.t. the PHC.** chronyd owns CLOCK_REALTIME; ptp4l owns
   the PHC. Never let two loops steer the same clock.
2. **Robust long servo.** The GPS grandmaster throws ±500–700 ns spikes — use
   median/Huber/step-rejection so a spike doesn't bias the frequency. chronyd already
   does outlier rejection; a custom servo must too.
3. **Media-clock generator** = produce at GPS rate, stamp avtp/SYT in PHC time. `r`
   (switch/GPS) is the key state; for AM824 this is the CIP SYT (see
   [[am824-cip-syt-missing]]), for AAF the per-packet avtp_timestamp.
4. **Verify** the Pi5 NIC HW-timestamps the L2 gPTP fine (it does, ~30 ns); the GPS
   path via chronyd doesn't need NIC HW timestamps.

## Refinements / future

- **1PPS hardware frequency reference.** The GPS module's **1PPS is ±20 ns** (25×
  cleaner than its ±500 ns PTP). Feeding 1PPS to a Pi5 GPIO (Linux PPS API), or using
  a dedicated 10 MHz / 1PPS GPS reference, gives a far better frequency input and
  converges in seconds. Costs a wire; the UDP/NTP path works without it.
- **Pi5 as the GPS-disciplined gPTP GM** (instead of free-running switch): if the
  Pi5's long-servo-smoothed GPS clock *is* the LAN gPTP GM, the avtp timebase becomes
  GPS-locked and the inter-site mapping is truly fixed (no re-anchoring) — at the cost
  of Pi5-as-GM jitter vs the hardware switch. Worth an A/B; switch-as-GM is a good v1.
- **CRF (IEEE 1722 Clock Reference Format)** is the right long-term vehicle to hand a
  receiver (the DSP processor) the GPS-locked media clock as a dedicated stream, decoupled from
  the AAF audio.

## Frequency-ratio estimators (`statusbar/ptpclient`)

`r = switch/GPS` and the PHC-GPS offset are tracked by
`ptpclient/ptpclient_freq_ratio.hpp` — two portable, unit-tested estimators
(`ptpclient_freq_ratio_test.cpp`) — driven by the `gps_ratio_tracker` tool
(`statusbar-gps-ratio-tracker`; Linux: reads `/dev/ptp0` vs `CLOCK_REALTIME`,
bracketed). Both consume `(offset, elapsed-GPS-seconds)` — `d(offset)/d(gps) = r−1`
by definition — and emit `r`, offset, `freq_uncertainty_ppb`, and (Kalman) drift.

| | `OlsRatioTracker` | `KalmanRatioTracker` (3-state: phase, freq, drift) |
|---|---|---|
| method | sliding-window OLS slope | recursive clock filter |
| thermal-ramp lag | window/2 (~7.5 min @ 15 min) | **zero** (drift state absorbs it) |
| acquisition | needs the full window | **~5 s** (large initial P) |
| outlier rejection | none | innovation gate (drops GPS grandmaster ±500 ns spikes) |
| uncertainty out | slope variance | covariance (both fill `freq_uncertainty_ppb`) |
| GPS holdover | freezes last window avg | **coasts on freq + drift** |
| code | ~80 lines | ~120 lines; scalar measurement ⇒ no matrix inversion |

Both validated on hardware and against each other: Site A switch **+42 ppm**,
Site B **+45.7 ppm** vs GPS (OLS and Kalman agree). For the 25 ms buffer the OLS
ramp-lag is harmless, so OLS is a fine v1; the Kalman earns its keep for
**GPS-holdover coasting** and tighter buffers.

**Tuning the Kalman** (`--meas-noise-ns` = √R, `--jerk-psd` = q): R is the
per-sample read jitter (~50–100 ns idle, ~1 µs under load — estimate from the
bracket spread). q sets the bandwidth; tune it by **whitening the innovations**
(the printed `innov/σ` should look unit-variance and white). These are the only
soak-tuning knobs; the algebra is fixed. Startup is hardened — the frequency is
**seeded from the first two samples** with a conservative covariance and the
update uses the **Joseph form** — so it can't run away the way an `f=0` +
huge-initial-P start could (which diverged on a loaded box at R=400). Covered by
the `robust_noisy_startup` regression test.

## Status

**All four nodes across three sites are converged on the decoupled design** —
`phc2sys` removed, PHC on a clean free-running local switch GM, CLOCK_REALTIME
GPS-disciplined by chrony, all on `linuxptp4avb 0.3.2`:

| Node | Site | PHC (local gPTP GM) | CLOCK_REALTIME (chrony ← GPS grandmaster NTP) |
|------|------|---------------------|----------------------------------------|
| node-a | Site A | AVB switch, ~20–30 ns | `192.0.2.90` |
| node-d | Site A | AVB switch, ~20–30 ns | `192.0.2.90` |
| node-b | Site B | AVB switch, ~30 ns | `192.0.2.20` |
| node-e | Site C | AVB switch, ~18 ns | `192.0.2.20` (see note) |

- Per-site GPS grandmaster reconfigured off L2 802.1AS (UDP 1588 / NTP), so the local
  switch free-runs as gPTP GM and ptp4l slaves the PHC to it (clean ~20–30 ns).
- **Site C note:** that GPS grandmaster refused to leave its static `192.0.2.20` (DHCP and a
  `198.51.100.x` static both failed to apply), so node-e reaches it **cross-subnet
  over the shared L2** via a persistent secondary address `192.0.2.250/24`
  (systemd unit `avb-site-c-secondary-ip.service`). Functional and reboot-persistent;
  collapses to the clean same-subnet form if that unit ever accepts a `.4.x` IP.
- Ratio estimators + tool implemented and validated on hardware
  (`statusbar/ptpclient`; Site A +42 ppm, Site B +45.7 ppm vs GPS).

### the DSP processor listener: GET_STREAM_INFO compliance fix

While running the node-e → the DSP processor 440 Hz AAF talker test, the DSP processor would
ACMP-connect (FAST_CONNECT, a saved connection) and then immediately self-
disconnect. A `pcap` of the AECP exchange showed the DSP processor querying
`GET_STREAM_INFO (0x000f)` on our talker's `STREAM_OUTPUT` right before the
`Disconnect TX` — and our entity answering `status=1 (NOT_IMPLEMENTED)`. A Milan
listener queries `GET_STREAM_INFO` to verify the talker's stream identity/format
after connecting and tears the connection down when the talker can't answer.

`GET_STREAM_INFO` is now implemented (IEEE 1722.1 Clause 7.4.16):
`nanoavb::AemCommandHandler` gained a `get_stream_info` callback (mirroring
`get_counters`), and `AvbEntityAudioIO` serves it from the live ACMP stream
identity (`stream_id`, `stream_dest_mac`, `stream_vlan_id`), the `STREAM_OUTPUT`
descriptor's `current_format`, and the live connection state (`CONNECTED` flag).
Unit-tested in `nanoavb_entity_test`; deployed to node-e (transient unit
`avb440-site-c.service` via `systemd-run`, logs in `journalctl -u avb440-site-c`).

### the DSP processor talker -> node-e listener: ACMP connect + the multicast-join gap

Connecting node-e's AAF *listener* sink to the DSP processor's *talker* (the direction
that streams cleanly) is a controller-issued CONNECT_RX:

```
statusbar-acmp-controller --interface=eth0 --action=CONNECT \
  --talker-entity-id=00:11:22:ff:fe:33:44:55 --talker-uid=0 \
  --listener-entity-id=70:b3:d5:ed:cf:00:00:02 --listener-uid=1
```

node-e's listener then sends CONNECT_TX to the DSP processor, declares MSRP Listener
Ready, and the DSP processor streams. node-e's AAF sink (`STREAM_INPUT[1]`, format
`02 07 02 20 02 00 c0 00`) is byte-identical to the DSP processor's talker outputs
[0,1,2,3,7,8,9,10]. **Verified glitchless: AAF `rx_bad=0` over 2.6M packets /
31.6M samples.**

**Resolved — the entity now auto-joins the talker's multicast group.** The
listener data plane previously joined only its own *talker* groups
(`fe:00`/`fe:01`) at startup and never the remote talker's stream group, so the
NIC filtered the frames out and a manual `ip maddr add 91:e0:f0:00:9a:6X dev eth0`
was needed. **The the DSP processor allocates a NEW dest MAC on every connection** (`9a:60`,
then `9a:61` on reconnect), so it can't be preconfigured. The listener
connect/disconnect callback (`avb_entity_audio_io.cpp`, `set_connection_callbacks`)
now `join_multicast(dest_mac)`s on the StreamRxHandler socket at connect and
`leave_multicast`es at disconnect (the latter via the new
`RawnetContext::leave_multicast`, core). Connecting a listener is now nothing but
*start the entity + send the ACMP CONNECT_RX* — no `ip maddr` step. The connect
log line reports `mcast join ok`.

### GPS-rate media-clock generator (DONE)

The talker now generates `avtp_timestamp` deterministically from a free-running
sample counter pinned to the GPS frequency ratio `r`, instead of basing it on the
media timer's (jittery) wake time. `ptpclient::MediaClockGenerator` is a phase
accumulator: presentation time advances by exactly one `nominal_period_ns * r`
sample period; the wake time only paces the integer sample count (nominal ±1,
GPS-rate), never the timestamp. `r = switch/GPS` comes from the ratio Kalman,
sampled in-entity from `CLOCK_REALTIME`(GPS) vs gPTP (`AvbEntityAudioIO::
update_gps_ratio`). The presentation offset is 1 ms.

Why it mattered — diagnosed on a Pi → the audio interface → Pi loopback. The the audio interface recovers its
entire media clock from `avtp_timestamp` (its CIP SYT is a constant placeholder),
so the talker's ~±20–80 ns wake jitter became audible warble. With the deterministic generator,
node-a's outgoing `avtp_timestamp` deltas are exactly **166669 ns (±1 ns)** (vs
±20–40 ns before), with GPS-rate pacing visible as a DBC histogram of mostly 12
with some 11/13. Through the audio interface **digital** passthrough the round-trip is
**+51.8 dB SNR** — pristine to the 24-bit floor, no drops, no wander. (The earlier
**analog** loopback wander was the audio interface's own D/A→A/D converter clock, not our
timestamps.) Decoders for these captures live in
[`pcap-analysis/`](pcap-analysis/).

Sequencing the clean GM also matters: lock every endpoint to the local gPTP
grandmaster (a switch free-running as GM is clean ~4–20 ns; a GPS unit relayed
*through* a boundary-clock switch injects ~650–800 ns of correction jitter — take
the GPS grandmaster off 802.1AS, domain≠0 / priority 255, so the switch is GM).

- Next: GPS-anchored UDPTUN inter-site playout (consume the same `r`).
