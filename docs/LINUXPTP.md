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


## ptpclient: grandmaster status from ptp4l

`ptpclient::PtpManagementClient` asks ptp4l which grandmaster the local clock
follows, as `pmc -u` does. ptp4l answers IEEE 1588 management messages on a
datagram socket (`uds_address`, `/var/run/ptp4l` by default), and only in its
own domain: a message whose domainNumber differs is ignored, so
`PtpManagementConfig::domain` must be ptp4l's (pmc `-d` semantics), else the
client sees no answer and reports unreachable.

The client is a `net::Pollable` for the reactor thread. On its first tick, and
every `period_ns` after, it sends GET PARENT_DATA_SET, GET CURRENT_DATA_SET and
GET TIME_PROPERTIES_DATA_SET, parses the responses as they arrive, and calls
its listener when the status or grandmaster changes. With `port_interface` set
it also sends linuxptp's GET PORT_PROPERTIES_NP (each port's interface name, so
the port identity is learnt, not configured) and GET PORT_DATA_SET (15.5.3.6.1),
and reports that port's `peerMeanPathDelay` in ns: on a gPTP port it is
802.1AS-2020's neighborPropDelay, measured whatever the port's role (11.2.2), the
`propagation_delay` of IEEE 1722.1-2021 Clause 7.4.40. Its own socket is a path
beside ptp4l's, as pmc's is (`dirname(uds_address)/statusbar-ptpmgmt.<pid>`),
removed on destruction.

The message layout is IEEE 1588-2019 Clause 15: the common header (13.3, as
`gptp::MessageHeader`), the management header (15.4.1), the MANAGEMENT TLV
(15.5.2) and the data sets (15.5.3). `build_management_get()` and
`parse_management_message()` are pure and portable, so the wire format is unit
tested without a ptp4l.

### Notifications and grandmaster settings

With `PtpManagementConfig::events` set, the client sends linuxptp's SET
SUBSCRIBE_EVENTS_NP at its first query and renews it at half
`subscription_duration_s` (180 s by default, as phc2sys). ptp4l then pushes, to
the client's socket, RESPONSE messages of the data set an event concerns: a port's
PORT_DATA_SET on each state change (`port_state`), PARENT_DATA_SET when the
grandmaster changes (`parent_data_set`) and TIME_STATUS_NP on each clock update
(`time_sync`). They are parsed as the answers to GETs are, so a port leaving
SLAVE reaches the listener without waiting for the next period; TIME_STATUS_NP is
kept in `state().time_status` and reports nothing by itself.

`set_grandmaster_settings()` sends SET GRANDMASTER_SETTINGS_NP: the clockQuality
(clockClass, clockAccuracy, offsetScaledLogVariance), currentUtcOffset, the time
flags and the time source that ptp4l announces while it is grandmaster. This is
linuxptp's external-grandmaster path: a `serverOnly 1`, `free_running 1` ptp4l
timestamps and announces while something else steers its PHC (`PhcServo`
below), and the steering process states its quality, such as clockClass 13 when
locked to an application-specific source and 248 when not (IEEE 1588-2019
Table 4). ptp4l's answer lands in `state().grandmaster_settings`, a
MANAGEMENT_ERROR_STATUS in `state().refused`. A restarted ptp4l has forgotten
both the settings and the subscription, so each period's queries add GETs of
GRANDMASTER_SETTINGS_NP and SUBSCRIBE_EVENTS_NP, and the client sends either again
when ptp4l holds something else, and also when ptp4l answers after silence. Both
need ptp4l's read-write socket (`uds_address`, not `uds_ro_address`).

## ptpclient: waking on a PTP deadline

`PtpTimeBridge::sleep_until_ptp` converts the PTP deadline to CLOCK_MONOTONIC,
sleeps with `clock_nanosleep` to `wake_refine_spin_ns` (plus a slew term)
before it, then busy-waits on CLOCK_MONOTONIC_RAW (a vDSO read, no syscall)
with a CPU pause hint to the deadline itself. The spin calls `sched_yield` only
every 20 us and not in the last 10 us: a yield lets an equal-priority
SCHED_FIFO thread sharing the core run during a long spin, but a yield per
iteration costs a syscall round trip each (about 1 us) and coarsens where the
wake lands.

`wake_refine_spin_ns` must cover the worst OS wake latency on the timer's core.
The default 50 us absorbs stray kernel work on a shared core; on an isolated
core (isolcpus/nohz_full, IRQs and kworkers moved off, deep C-states held off)
15-20 us is enough and halves the spin's CPU.

The optional `spin_headroom_ns` result is how far before the deadline the sleep
returned, less the slew term: what was left of the spin window, so the OS wake
latency is the window minus it. It is negative when the sleep overran the
deadline, and set only on success when the call slept (not when the deadline was
already inside the window). `PtpTimer::wake_headroom()` aggregates it; its
`min_ns` is the closest call, and a negative minimum shows up as a late wake in
`stats()`.
