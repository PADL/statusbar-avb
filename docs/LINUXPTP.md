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

The client is a `net::Pollable` for the reactor thread, event-driven: one fetch
of the data sets it follows, then ptp4l's notifications. While ptp4l is
unreachable it sends GET PARENT_DATA_SET every `retry_period_ns`, once with
transportSpecific 0 and once with 1, since ptp4l drops a request whose
transportSpecific is not its UDS port's (the global `transportSpecific`, 1 in a
gPTP configuration, unless `ignore_transport_specific`). The first answer gives
the UDS port's value, used from then on (a port's notifications carry that port's
own, which is not used), and starts the fetch: GET PARENT_DATA_SET,
CURRENT_DATA_SET, TIME_PROPERTIES_DATA_SET and every port's PORT_DATA_SET
(15.5.3.6.1); with `port_interface` set also linuxptp's PORT_PROPERTIES_NP (each
port's interface name, so the port identity is learnt, not configured), before the
PORT_DATA_SET; the grandmaster settings,
if any; and SET SUBSCRIBE_EVENTS_NP. The status stays unreachable until
PARENT_DATA_SET says which grandmaster ptp4l follows. The header's
minorVersionPTP is not checked (linuxptp sends 0x12 by default). The client
reports that port's
`peerMeanPathDelay` in ns: on a gPTP port it is 802.1AS-2020's neighborPropDelay,
measured whatever the port's role (11.2.2), the `propagation_delay` of IEEE
1722.1-2021 Clause 7.4.40. Its own socket is autobound to an abstract address
(Linux `bind()` with only the address family), unique to each client and gone
with the socket; ptp4l answers to the sender's address (`recvfrom`, then
`sendto`), so nothing is created in the filesystem. Each client has its own source
port identity (the pid and a per-client port number), which ptp4l keys a
subscription by.

The message layout is IEEE 1588-2019 Clause 15: the common header (13.3, as
`gptp::MessageHeader`), the management header (15.4.1), the MANAGEMENT TLV
(15.5.2) and the data sets (15.5.3), as packed structs
(`ptpclient_management_format.hpp`, with linuxptp's implementation-specific data
sets as its `tlv.h` lays them out). `build_management_message()` and
`parse_management_message()` are pure and portable, so the wire format is unit
tested without a ptp4l, and fuzzed.

Every port's portState is kept in `state().port_states` (by portNumber, the
first `MAX_PTP4L_PORTS`), from each PORT_DATA_SET answered or notified, and a
change is reported. `clock_synchronized(state)` reads them: ptp4l's clock is
synchronised to its domain while it follows a grandmaster with a port in SLAVE,
or is the grandmaster with a port in MASTER and none UNCALIBRATED (linuxptp
reports its PS_GRAND_MASTER as MASTER). linuxptp moves a port from SLAVE to
UNCALIBRATED when its servo jumps or unlocks, and back once it locks again.
ptp4l steps the clock before it dispatches the fault, so a host learns of a step
just after it happens.

### Notifications, polling and grandmaster settings

The client always subscribes to `port_state` and `parent_data_set`, and to the
further events in `PtpManagementConfig::events` (`time_sync`). ptp4l pushes, to
the socket that last subscribed with the client's source port identity, an
unsolicited RESPONSE carrying the whole data set an event concerns: a port's
PORT_DATA_SET on each state change, PARENT_DATA_SET when it changes (linuxptp
4.2) and TIME_STATUS_NP on each clock update. They are parsed as the answers to
GETs are. A new parent also fetches CURRENT_DATA_SET and TIME_PROPERTIES_DATA_SET
again; TIME_STATUS_NP is kept in `state().time_status` and reports nothing by
itself. ptp4l notifies nothing for TIME_PROPERTIES_DATA_SET or for a port's
peerMeanPathDelay, so those are polled every `poll_period_ns`, with
PARENT_DATA_SET too while ptp4l holds no subscription (it refuses one on the
read-only socket; that refusal is not reported in `state().refused`).

The subscription lasts `subscription_duration_s` (6 s by default; 65535 would
never expire and is refused) and is renewed three times per duration, as phc2sys
does (pmc_agent.c), so one renewal can be lost before ptp4l prunes it
(clock.c `clock_prune_subscriptions`). A renewal is a GET then a SET of
SUBSCRIBE_EVENTS_NP, with a GET of GRANDMASTER_SETTINGS_NP when the client set
some: an answer to the GET without our events is a restarted ptp4l, so the client
fetches everything again and sends its settings, and settings other than ours are
sent again. It is also the keepalive: two renewals without any answer from ptp4l
make the client unreachable, and so does a send ptp4l's socket refuses (ENOENT,
ECONNREFUSED: it exited or is restarting); everything learnt is then forgotten and
the probes resume. A client being destroyed unsubscribes (an empty bitmask).

`set_grandmaster_settings()` sends SET GRANDMASTER_SETTINGS_NP: the clockQuality
(clockClass, clockAccuracy, offsetScaledLogVariance), currentUtcOffset, the time
flags and the time source that ptp4l announces while it is grandmaster. This is
linuxptp's external-grandmaster path: a `serverOnly 1`, `free_running 1` ptp4l
timestamps and announces while something else steers its PHC (`PhcServo`
below), and the steering process states its quality, such as clockClass 13 when
locked to an application-specific source and 248 when not (IEEE 1588-2019
Table 4). ptp4l's answer lands in `state().grandmaster_settings`, a
MANAGEMENT_ERROR_STATUS in `state().refused`; an answer with other settings than
ours sends ours again. The settings and the subscription need ptp4l's read-write
socket (`uds_address`, not `uds_ro_address`).

## ptpclient: PHC cross-timestamps and the PHC servo

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

## ptpclient: pulse lock

`PulseLock` decides whether a PHC is locked to another clock's 1 PPS from the PHC's own
external timestamps of it: the pulses ts2phc steers a sink by, with the source
emitting at its whole seconds. Each event is checked against the whole second nearest
it. An event further than `edge_window_ns` (250 ms) from one is the pulse's other
edge, which ts2phc also stamps (`extts_polarity both`), and is ignored. A pulse is
good when it lies within `max_offset_ns` (25 µs) of its second, and when the
reference clock, read as the event is handled, lies between half a second before
and a second after that second. The second test catches a sink stepped by whole seconds, such as one
an adapter reset set to the system time, which a pulse alone cannot show.

The lock holds after `settle_pulses` (3) good pulses in a row. It goes at one bad
pulse, or, through `expire()`, after `max_silence_ns` (3 s) without one: an adapter
reset that disarms the channel, or ts2phc gone. It is event-driven: `pulse()` per
event and `expire()` when the host's deadline passes.

`ExttsReader` reads one channel's events on a descriptor of its own: a reactor
`Pollable` over the `/dev/ptpN`. Since Linux 6.7 each open file has its own event
queue (`ptp_open()`), and `PTP_MASK_CLEAR_ALL` / `PTP_MASK_EN_SINGLE` keep only the
channel wanted. The channel stays its owner's (ts2phc enables it with
`PTP_EXTTS_REQUEST`); the reader never enables or disables it. On a kernel without
the mask ioctls one queue is shared, and reading it would steal the owner's events,
so `open()` refuses with `not_supported`. One `read()` per readiness:
`ptp_read()` blocks on an empty queue whatever `O_NONBLOCK` says.

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

## The media timer's busy-spin window

`ptp.spin_ns` is how long before each wake the timer stops sleeping and busy-polls. The
default 50 µs covers kernel work stalling a shared core; with the timer's core isolated
(isolcpus/nohz_full, IRQs and kworkers off it, deep C-states disabled) 15000-20000 ns is enough.
