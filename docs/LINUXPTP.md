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

