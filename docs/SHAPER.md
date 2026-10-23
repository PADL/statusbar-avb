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
3. keeps an offloaded cbs on the class A queue (`parent <major>:<queue + 1>`)
   whose idleSlope is the sum of the admitted streams' reservations, and puts
   the default qdisc (pfifo_fast) back when none is admitted.

A software cbs would do nothing here: bypass frames never enter it. Only the
offloaded cbs (`offload 1`), which programs the NIC's own shaper on the queue,
shapes them. Without a shaper, `SO_PRIORITY` alone still lands the streams on
the SR-class queue once some mqprio map is in place.

Removing a cbs grafts pfifo_fast rather than deleting the child: deleting an
mqprio child leaves `noop_qdisc` on the queue, which drops every non-bypass
frame steered there. Either way the cbs is destroyed and its offload disabled.
`uninstall()` deletes the mqprio root (its children go with it) and the
kernel restores the default root. `install()` first tries to replace the root;
if that fails with EINVAL, which is what mqprio answers when the root is
already an mqprio with our handle (a "change", which it refuses), it deletes
the root with our handle and tries once more. So a root left by a run that did
not uninstall is replaced, and a foreign mqprio that happens to hold our
handle (`ShaperConfig::qdisc_major`, the bridge's `[avb] qdisc_handle`,
default 0x9000) is taken over. Nothing of ours is left installed when
`install()` fails.

## Threading

The shaper is a tick-only `net::Pollable` on the reactor. `install()` registers
the kit's admission hook, which only records the new admitted set and marks the
shaper dirty: no netlink request runs inside the kit's ACMP/MSRP callbacks.
`tick()` re-reads the link speed at most once a second and calls `reprogram()`
when the admitted set or the link speed changed, so a burst of admission
changes costs one reprogramming and a renegotiated link is corrected within a
second. `shutdown()` unregisters the hook and, when `restore_qdisc` is set,
uninstalls; the destructor unregisters too. The kit must outlive the shaper.

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
queue. The offloaded cbs on an i210 has not been exercised.
