# Talker traffic shaping: mqprio, cbs and SO_PRIORITY

`avb_entity::EntityShaper` (`statusbar/avb_entity/avb_entity_shaper.hpp`) puts
an entity's talker streams on a hardware credit-based shaper. It needs
CAP_NET_ADMIN, Linux and a NIC with at least 3 TX queues. The rtnetlink
requests are `net::tc::TcClient` (`core/statusbar/net/net_tc_qdisc.hpp`).

## Why bypass + mqprio + SO_PRIORITY works

The kit sends stream frames on a `PACKET_QDISC_BYPASS` socket
(`TalkerStreams::open_tx`). Bypass frames skip every software qdisc and go
straight to `dev_direct_xmit` on a TX queue that `packet_pick_tx_queue` picks:
the driver's `ndo_select_queue` if it has one (igb and igc do not), otherwise
`netdev_pick_tx`, which maps `skb->priority` through the netdev's
priority-to-traffic-class map to that class's queue range. An AF_PACKET
socket's `skb->priority` is its `SO_PRIORITY`.

An mqprio root installs that map (`netdev_set_num_tc`, `netdev_set_prio_tc_map`,
`netdev_set_tc_queue`) even in software mode (`hw 0`), so bypass frames follow
it although they never meet mqprio itself. The shaper therefore:

1. replaces the root with mqprio, 3 traffic classes: TC0 legacy, TC1 class B,
   TC2 class A; priority 3 (class A's PCP) maps to TC2, priority 2 (class B's)
   to TC1, every other priority to TC0; one TX queue per SR class, the rest
   to TC0;
2. relies on the stream TX socket's `SO_PRIORITY` being the class A PCP:
   `open_tx` sets it to the kit's `stream_pcp` (best effort: priority 7 needs
   CAP_NET_ADMIN, and on failure the kit logs a warning and the socket stays
   open on the default queue), so the steering holds whoever installs the
   map: SwiftMRP's mrpd, a hand-run `tc`, or this shaper. The shaper keys
   class A on that same PCP (`kit.config().stream_pcp`) and class B on the
   802.1Q default 2, and refuses a PCP of 8 or more or one PCP for both
   (`ShaperError::bad_priority`);
3. keeps an offloaded cbs on both SR-class queues (`parent <major>:<queue + 1>`)
   from install on: class A's idleSlope is the sum of the admitted streams'
   reservations, and a class with none admitted holds a placeholder (see
   "Adapter resets" below). Admission only changes the parameters in place.

A software cbs would do nothing here: bypass frames never enter it. Only the
offloaded cbs (`offload 1`), which programs the NIC's own shaper on the queue,
shapes them. Without a shaper, `SO_PRIORITY` alone still lands the streams on
the SR-class queue once some mqprio map is in place.

`uninstall()` grafts pfifo_fast on class B's queue, then class A's (both,
whatever this run recorded: a kept root may carry a cbs it never programmed),
rather than deleting the children (deleting an mqprio child leaves `noop_qdisc` on
the queue, which drops every non-bypass frame steered there; and igc refuses
to drop class A's cbs while class B's is on), then deletes the mqprio root and
the kernel restores the default root.

`install()` first reads the root (`TcClient::get`: an `RTM_GETQDISC` dump,
filtered to the interface's root; a single get would also notify every
`tc monitor`). A root
that already is our mqprio, with our handle and exactly our layout (a run that
left it at shutdown, or crashed), is kept with its cbs children, whose
parameters the first reprogramming changes in place. Anything else is
replaced; if that fails with EINVAL, which is what mqprio answers when the
root is already an mqprio with our handle (a "change", which it refuses: our
handle with another layout), it deletes the root with our handle and tries
once more. So a foreign mqprio that happens to hold our handle
(`ShaperConfig::qdisc_major`, the bridge's `[avb] qdisc_handle`, default
0x9000) is taken over. Nothing of ours is left installed when `install()`
fails.

## Adapter resets (igb, igc)

igb (i210) switches its transmit mode to FQTSS (Qav) when the first offloaded
cbs or etf is added and back when the last is removed (`igb_offload_apply` ->
`enable_fqtss`), and each switch schedules `reset_task`: "Reset adapter", the
link down for about 4 s, gPTP lost, and the PHC's SDP pin functions cleared
(ts2phc's EXTTS input on the port stops). igc on an i225 does the same
(`igc_tsn_offload_apply`). A cbs replacing a cbs is a change (`cbs_change`:
the driver reprograms the queue's idleSlope and credits only), and mqprio is
never offloaded on igb (`hw 0`: installing or deleting it touches no
hardware); only the first cbs and the last one cost a reset.

So the shaper puts a cbs on both class queues at install and never takes the
last one away while running:

| Event | Requests | Resets (igb) |
| --- | --- | --- |
| first start (default root) | mqprio; cbs A, cbs B (placeholders) | 1 (the first cbs; the second finds FQTSS on) |
| a stream admitted, withdrawn, link speed change | cbs A and/or B changed in place | 0 |
| shutdown, `restore_qdisc = false` (default) | both cbs set to their placeholders; root left | 0 |
| restart onto that root | the root kept; both cbs changed in place | 0 |
| shutdown, `restore_qdisc = true` | pfifo_fast on B, on A; root deleted | 1 (the last cbs) |
| start after that, or after a reboot | as a first start | 1 |
| our handle with another layout (a changed queue override) | root deleted and replaced, cbs added | 1 or 2 (the driver's reset work may coalesce the off and on switches) |

The placeholder (`placeholder_cbs`) must keep the queue shaped without
throttling what still reaches it. On class A's queue that is only the gated
stream frames, which can run for the moment between the talker gate opening
and the next tick, before the cbs follows the admitted set. On class B's queue
(skb priority 2 on our map, `map 0 0 1 2 0 ...`) it is any AF_PACKET socket
with SO_PRIORITY 2 and any IPv4 socket whose IP_TOS the kernel maps to
TC_PRIO_BULK (`rt_tos2priority`: the AFx1 DSCPs, e.g. OpenSSH's default AF21
for interactive sessions) when the AVB port carries IP. The bridge's kit sets
no priority but the stream socket's, ptp4l's default `socket_priority` is 0,
and the bench box's AVB port has no address, so nothing reaches class B there
today. A small idleSlope would cap that traffic at a trickle; instead the
placeholders share what the admitted streams leave of the SR budget, 75% of
the link (802.1Q 34.3.1's default deltaBandwidth): all of it for the one
empty class, half each when both are empty, with Annex L credits for an MTU
frame (1 Gbit/s, MTU 1500, nothing admitted: each idleslope 375000, sendslope
-625000, hicredit 563, locredit -963). So the two SR queues together never
claim more than 75% and legacy traffic keeps a quarter of the link even with
both busy (on igb, SP_WAIT_SR holds the strict-priority queues while an SR
queue has credit). A placeholder never falls under 1% of the link; only when
admitted streams leave less than that does the total pass the budget.

A driver without cbs offload (EOPNOTSUPP: igb on anything but an i210, a
driver with no cbs `ndo_setup_tc`) is logged once and the shaper sends no
further cbs request; the mqprio map, which is what steers the
bypass frames, stays. Other errors (EINVAL for an igb queue above 1, EBUSY)
leave the class unprogrammed and the shaper dirty: `tick()` retries after
1 s, doubling to 32 s, each attempt logged, and a new admitted set goes out
at once. On igb/igc an override putting class A on queue 1 and class B on
queue 0 is refused (`bad_queue_override`): queue 0 outranks queue 1, and igc
takes queue 1's cbs only after queue 0's.

## Threading

The shaper is a tick-only `net::Pollable` on the reactor. `install()` registers
the kit's admission hook, which only records the new admitted set and marks the
shaper dirty: no netlink request runs inside the kit's ACMP/MSRP callbacks.
`tick()` re-reads the link speed at most once a second and calls `reprogram()`
when the admitted set or the link speed changed, so a burst of admission
changes costs one reprogramming and a renegotiated link is corrected within a
second. `shutdown()` unregisters the hook and, when `restore_qdisc` is set,
uninstalls; otherwise (the default) it parks both cbs at their placeholders
and leaves the qdiscs for the next start. The destructor unregisters too. The
kit must outlive the shaper. The requests themselves are `ShaperQdiscs`, over
`net::tc::TcControl`; its tests record them.

`netdev_set_num_tc` resets any XPS maps; do not configure XPS on the AVB port
afterwards (XPS is consulted before the TC hash in `netdev_pick_tx`).

## Queue order per driver

| Driver | Class A | Class B | Legacy (TC0) | Why |
| --- | --- | --- | --- | --- |
| `igb` (i210), `igc` (i225/i226) | queue 0 | queue 1 | [2, N) | Qav mode: the lowest queue has the highest priority, and cbs offload is honoured only on queues 0 and 1 (`igb_offload_cbs` refuses others) |
| anything else (e.g. mv88e6xxx switch ports) | N-1 | N-2 | [0, N-2) | the highest queue has the highest priority |

`ShaperConfig::queues` (the bridge's `[avb] class_a_queue` / `class_b_queue`)
overrides either queue; the queues left must form one range, and fewer than 3
TX queues is refused (`ShaperError::not_avb_capable`): an AVB port must keep
its queues (no `ethtool -L <if> combined 1`). SwiftMRP's README notes the same
igb inversion ("pass `--class-a-qdisc-handle 1 --class-b-qdisc-handle 2`").

The igb layout maps TCs to queues in reverse order (TC0 on 2-3, TC2 on 0);
mqprio accepts that since Linux 6.5 ("allow reverse TC:TXQ mappings"); older
kernels refuse it with EINVAL.

## The cbs parameters (IEEE 802.1Q Annex L, as SwiftMRP)

Per admitted talker stream, from the TSpec the kit advertises
(`srp_max_frame_size(format)`, MaxIntervalFrames 1):

```
wire frame    = max(MaxFrameSize + 4 (VLAN) + 18 (L2), 68) + 20 (L1) + 1 (clock drift)
bandwidth     = ceil(wire frame * 8 * MaxIntervalFrames * intervals/s / 1000)   kbit/s
                intervals/s: 8000 for class A (125 us), 4000 for class B (250 us)
idleSlope     = sum of the class's bandwidths                                  kbit/s
sendSlope     = idleSlope - link speed                                         kbit/s
hiCredit (A)  = ceil(idleSlope * maxInterferingFrame / link)                   (L-10)
loCredit      = ceil(sendSlope * maxFrameSize / link)                          (L-12)
hiCredit (B)  = ceil(idleSlopeB * (maxInterferingFrame / (link - idleSlopeA)
                                   + maxFrameSizeA / link))                    (L-11)
```

maxFrameSize is the class's largest wire frame; maxInterferingFrame is the
port MTU, as SwiftMRP passes it. The link speed comes from
`/sys/class/net/<if>/speed`, re-read once a second. When the kernel has no
speed (EINVAL or -1: link down, or a NIC that never reports one) the last
known speed is kept, so a flap does not churn the shaper; before any is known,
1 Gbit/s is assumed. igb computes its own credits from idleSlope; the others
matter to drivers that take them.

Example (the bridge's default model: one 8-channel 48 kHz INT_24 AAF talker
plus the CRF, 1 Gbit/s, MTU 1500): AAF MaxFrameSize 192, wire 235, 15040
kbit/s; CRF MaxFrameSize 28, wire 89, 5696 kbit/s; idleSlope 20736, sendSlope
-979264, hiCredit 32, loCredit -230 (`avb_entity_shaper_test.cpp` shows the
arithmetic).

## Admission

The shaper sizes class A by the kit's admitted set
(`AvbEntityKit::admitted_talker_streams()`, reported through
`set_on_admission_changed`): a Started talker stream for which an MSRP
Listener Ready or Listener Ready Failed is registered, or was within the
transmit gate's 5 s grace window (`TalkerGate::admitted`). That is the
Milan 1.2 5.3.7.3 streaming condition; Listener Asking Failed does not
admit, and ACMP connections play no part (a Milan listener binds with
PROBE_TX and makes none). The grace window runs from the last moment the
declaration was known registered, so the LeaveAll re-registration blip
never takes bandwidth from a stream still on the wire, and a listener that
really leaves gives its bandwidth back 5 s later. The kit re-evaluates the
set on every MSRP listener event and on every `EntityUpkeep` tick, which is
what notices a grace expiry. The transmit gate is the same predicate (the
kit's MAAP and STOP_STREAMING checks only narrow it), and the reactor
evaluates it later than the media thread does, so the admitted set never
drops a stream before it stops transmitting. Withdrawing our Talker
Advertise closes every stream at once. With
`gate_talker_on_listener = false` every talker stream is admitted. Every kit talker stream is class A today (the kit
declares only the class A SR domain); class B is laid out but stays empty.

## Verifying on the box

```
tc qdisc show dev <if>
#  qdisc mqprio 9000: root tc 3 map 0 0 1 2 0 0 0 0 0 0 0 0 0 0 0 0
#               queues:(2:3) (1:1) (0:0)
#  qdisc cbs 8001: parent 9000:1 hicredit 32 locredit -230 sendslope -979264 idleslope 20736 offload 1
#  qdisc cbs 8002: parent 9000:2 hicredit 1094 locredit -417 sendslope -270736 idleslope 729264 offload 1
tc -s class show dev <if>        # per-queue counters (non-bypass traffic only)
ethtool -S <if> | grep tx_queue  # per-queue packet counters, bypass frames included
```

Bypass frames do not appear in `tc -s` (they skip the qdiscs); the driver's
per-queue counters (`tx_queue_0_packets` on igb) show the streams on class A's
queue. The entity's ctl log records each install, cbs change and removal with
the values programmed, or the errno and the kernel's extended-ack text.

What has been verified without hardware: the request bytes (unit tests,
hand-checked against `linux/rtnetlink.h` and `linux/pkt_sched.h`), and, in an
unprivileged user and network namespace on Linux 7.0 with a 4-queue dummy
interface, that the kernel accepts the mqprio layout, refuses a same-handle
replace with EINVAL (the takeover path), accepts a software cbs and its
in-place change, the pfifo_fast graft and the root delete, refuses `offload 1`
on a device without it (EOPNOTSUPP), and steers SO_PRIORITY 3 onto class A's
queue. In the same setup `ShaperQdiscs` over a real `TcClient` read back an
mqprio and a cbs laid down by `tc`, kept the identical root, stopped asking
after EOPNOTSUPP, uninstalled, and took over our handle holding another
layout (EINVAL, delete, replace). The offloaded cbs on an i210 has not been
exercised by these tests; the reset behaviour above is from the driver source
and the bench box's dmesg.
