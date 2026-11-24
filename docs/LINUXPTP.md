<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# linuxptp config and command line for raspberry pi 5


    ptp4l -f /etc/linuxptp/gptp-slave.conf -i eth0 -m



    [global]
    gmCapable              1
    priority1              248
    priority2              248
    domainNumber           0
    logAnnounceInterval    0
    logSyncInterval        -3
    logMinPdelayReqInterval 0
    syncReceiptTimeout     3
    neighborPropDelayThresh 800
    min_neighbor_prop_delay -20000000
    clock_servo            pi
    step_threshold         0.0
    time_stamping          hardware
    assume_two_step        1
    path_trace_enabled     1
    follow_up_info         1
    transportSpecific      0x1
    ptp_dst_mac            01:80:C2:00:00:0E
    network_transport      L2
    delay_mechanism        P2P
    summary_interval       0


## Grandmaster sync quality — GPS grandmaster clock (±500 ns / 3 µs class)

The lab gPTP grandmaster at each site is a **GPS grandmaster clock** GPS time
server (OUI `90:06:f2`). It is a **~microsecond-class** PTP grandmaster, not a
telecom-grade one. This matters for Milan/AVB media sync.

**Datasheet numbers** (from the grandmaster's manual and its
"Time Server Accuracy" report):

- Datasheet **PTP Server Time Precision: "better than 3 µs + network jitter."**
- Accuracy report best case (GPS + HW timestamping, vs a NIST Microsemi TP-2700):
  **within ~200 ns of reference, jitter ±500 ns.**
- The GPS module's raw 1PPS is **±20 ns** (§8.2); the ~µs of PTP jitter is added in
  the GPS grandmaster's PTP packet-generation/timestamping path, *not* the GPS.
- Without GPS (software timestamping) jitter is **~10× worse (±~5 µs).**

**Measured on this bench (2026-06-07), Pi5 slaves:**

| Path | lock rms | peak \|offset\| |
|---|---|---|
| Pi5 → GPS grandmaster **via a switch boundary clock** (2 hops) | ~700–800 ns | ~1.6–1.7 µs |
| Pi5 → **free-running AVB switch as GM** (own oscillator) | ~20–30 ns | ~50 ns |

The ~1.7 µs through the relay is **within the GPS grandmaster's 3 µs spec** — the device is
performing to spec, not broken. The same AVB switch delivers ~20–30 ns
when it free-runs as GM, so the **boundary clocks are not the problem**: a boundary
clock is a slave on its upstream port and can only relay its lock to the GM. The
~±500 ns originates *at the GPS grandmaster*; AVB switches at both sites (Site C and
Site B) reproduce the same degradation, so it is **switch-brand-independent.**
802.1AS conformance is otherwise clean (`gmTimeBaseIndicator`/`lastGmPhaseChange`
stable = no GM phase jumps; large boundary-clock residence ~1.15–1.77 ms is legal).
A DSP-processor endpoint shows a **yellow "AVB sync" LED + audible sample slips
("snats")** on the relayed GPS grandmaster, and **green + clean** when the switch free-runs.

**GPS-grandmaster web-admin jitter levers** (its PTP Config page), to push toward
the ±500 ns best case:

1. **Time Stamping Source = Hardware (GPS)** — Software = 10× worse jitter.
2. **Transport Specific Field = 1** (default `0`; gPTP needs majorSdoId 1).
3. **Update Method = One Step** — HW stamp at egress, less slop than Two Step.
4. **Log Sync Interval = −3** (8/s) or faster — more samples → tighter relay tracking.
5. Confirm **Packet Output = 802.1AS (gPTP)**, **Delay Mechanism = Peer to Peer**,
   **Multicast**, **802.1AS Capable = True**, **Include Followup Information = Enable**.
6. Antenna: outdoor, 360° sky view, more satellites/SNR → tighter 1PPS = less wander.
7. **Save OCXO Correction** after ~2 h of 3D lock (holdover/startup stability).

Ceiling even when fully tuned is ~µs / ±500 ns. For tightest **media** jitter, let
the AVB switch be GM on its own oscillator (lose GPS-absolute time, win short-term
stability — which is what audio needs). For GPS-traceable **and** tight, use a
telecom-grade GM (Microsemi TP-2700 / Orolia / Meinberg, sub-100 ns).

**Lab grandmaster inventory:**

| Site | Pi5 nodes | GPS grandmaster web IP | gPTP clock id |
|---|---|---|---|
| Site A | node-a, node-d | `192.0.2.90` | `0200de.fffe.000001` |
| Site B | node-b | `192.0.2.20` | `0200de.fffe.000002` |
| Site C | node-e | (AVB switch segment) | `0200de.fffe.000003` |

Note: at Site A the GPS grandmaster has a 3D GPS lock but is **not** the active gPTP GM
(the AVB switch free-runs as GM); fix is on the GPS grandmaster's PTP Config page
(`Packet Output = 802.1AS`).

## ptpclient: PHC cross-timestamps, the PHC servo and the follower

`PhcDevice` opens a `/dev/ptpN` and reads it against CLOCK_MONOTONIC_RAW by the
best method the device and kernel offer: `PTP_SYS_OFFSET_PRECISE` (a hardware
cross-timestamp over PCIe PTM, as on an I226) when the capabilities say so;
`PTP_SYS_OFFSET_EXTENDED` with `clockid` CLOCK_MONOTONIC_RAW (Linux 6.11 and
later; an older kernel refuses the field with EINVAL); else a software bracket of
`clock_gettime` RAW, PHC, RAW. Each read uses `samples` brackets (9 by default)
and keeps the narrowest, its midpoint as the instant. A failed hardware
cross-timestamp (a PTM timeout) falls back to the next method for that read.
`frequency_ppb()` needs a writable device, as the kernel refuses
`clock_adjtime` on a read-only PHC descriptor.

`PhcCrossTimestamp` pairs two PHCs, as phc2sys's `sysoff_measure` does but with
RAW in place of the system clock: read PHC0, PHC_d, PHC0, and interpolate PHC0 at
PHC_d's instant (over tens of microseconds, where the clocks' rate difference
is negligible). A sample's `uncertainty_ns` is half PHC_d's bracket plus half
PHC0's mean bracket. The two may be the same device, which checks the
arithmetic: the pair then agrees to within its brackets.

`PhcServo` steers a PHC to a target given per sample, as `error_ns = t_phc -
t_target`: linuxptp's `pi.c` with its hardware-timestamping constants (kp =
0.7 × interval^-0.3, at most 0.7 / interval; ki = 0.3 × interval^0.4, at most
0.3 / interval; at 16 updates a second kp = 1.61, ki = 0.099), the first
frequency estimate after 0.016 / ki seconds, and an anti-windup clamp at
`max_frequency_ppb` (give the device's `max_frequency_ppb()`). It steps only on
that first estimate and only before its first lock, past
`first_step_threshold_ns` (20 µs, linuxptp's `first_step_threshold`); afterwards
it slews. It is locked after 16 samples under 100 ns and unlocked after 16 over
1 µs. `freeze()` holds the frequency (the PHC free-runs on its last correction)
until `thaw()`; `reset()` starts over from the frequency in force and allows a
step again. A known target rate goes in as `feed_forward_ppb`, so the integrator
learns only the PHC's own oscillator error. The servo is arithmetic only;
`apply()` sets the frequency (ADJ_FREQUENCY) and steps (ADJ_SETOFFSET).

`PhcFollower` is the loop that steers one PHC to follow another, or a line over
it, as phc2sys does between two PHCs but over a `PhcCrossTimestamp` pair. Each
`sample(line)` reads one pair, computes the error `phc_d - line.at(phc0)`, feeds
it to a `PhcServo` at the pair's own CLOCK_MONOTONIC_RAW instant (`raw_ns`) with
the line's rate as its feed-forward, and applies the action: the frequency, then
any step. A `PhcLine` is a target time at a PHC0 anchor and a rate offset in
ppb (finite, else the sample is refused); `PhcLine::offset_from_phc0(offset_ns)`
follows PHC0 itself with a constant calibration. A caller whose line is not
running passes its rate as 0, or freezes the follower. The servo starts from the
followed PHC's frequency and is clamped to the smaller of its parameters' range
and the device's. `freeze()` steers nothing (the pair is still measured) until
`thaw()`, for a line that stopped running; after `thaw()` the servo estimates
its rate again. An optional restep rule starts the servo over, so it may step
again, after `restep_count` samples past `restep_threshold_ns` once the servo's
rate estimate is done (`PhcServo::estimated()`): a timescale change of PHC0,
before or after a lock, or a line that resumed elsewhere after a freeze. The
samples it counts are held, not fed to the servo, so a glitch does not wind it
up, and the follower reports itself unlocked meanwhile. A sample reports what
failed (`PhcFollowerStage`) and its error, and carries the pair it measured: a
failed measure or a refused frequency leaves the follower as it was, so a step
it wanted is still owed; a refused step after the frequency went through starts
the servo over from that frequency, so device and servo agree and the step is
owed again; refused twice running (a device without ADJ_SETOFFSET), it slews
from then on, and the restep rule stays off until the caller's `reset()`. A
line's rate at or past the servo's clamp (the smaller of its parameters' and the
device's range) is refused: the servo would have no headroom to follow it. The pair is a `PhcFollowerPort` (measure, frequency, step,
range): `open_phc_follower_port(phc0, followed)` is the device pair, one
`PhcCrossTimestamp` opened with PHC_d writable (`PhcCrossAccess::phcd_writable`)
and adjusted through it; a test gives the loop a fake.

The tests that touch real clocks skip unless the devices open. On a box with
PHCs, as root:

```
STATUSBAR_PHC0=/dev/ptp0 STATUSBAR_PHCD=/dev/ptp1 STATUSBAR_PHC_SERVO_DEVICE=/dev/ptp1 \
    ctest --test-dir build -R 'ptpclient_phc' -V
```

`STATUSBAR_PHC_SERVO_DEVICE` names a spare PHC the servo test may steer for ten
seconds onto a line 1 ms ahead of its start at +100 ppm over PHC0. It restores the
frequency afterwards, not the time, so never name a PHC that ptp4l or phc2sys
disciplines.
