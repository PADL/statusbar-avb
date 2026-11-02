<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->
<!-- SPDX-License-Identifier: MIT -->

# statusbar-avb 2.3.0: what is new, and how it behaved on a Raspberry Pi 5

A reference for the entity data-path work that reached `main` between the
2.1.0 release and 2.3.0, with the measurements taken on the Raspberry Pi 5
rig. `statusbar-core/docs/REPORT_2_3_0_PI5.md` covers the Ethernet port,
AF_XDP and launch-time work this builds on; the umbrella's
`docs/REPORT_2_3_0_PI5.md` describes the rig and how it was driven.

## The measurements, in one table

Two Pi 5 nodes, each running `statusbar-avb-audio-io` with three talker and
three listener streams (AM824 at 96 kHz, AAF, CRF), connected to each other
in both directions through a MOTU AVB switch by `statusbar-acmp-controller`,
roughly 7,800 frames/s per stream, runs of 60 to 300 s. "Media tick" is the
125 µs media-timer callback; "margin" is `avtp_timestamp - gPTP now` at
receive, so a late frame is a negative margin.

| Configuration                                   | Late frames          | Media tick (avg / max)      | Margin, AM824 / AAF (avg)   |
|-------------------------------------------------|----------------------|-----------------------------|-----------------------------|
| rawnet, reactor RX, kick in the media tick      | 0.008–0.018 %, late by 316–377 µs avg, 1.0–1.24 ms max | –           | +931 µs / +809 µs, sd 42–52 |
| rawnet, RT RX timer (`stream_rx.rt_timer`)      | 0 of ~1.2 M per input | –                          | +820 µs / +702 µs, sd 38–41 |
| mmap, RT RX timer, kick in the media tick       | 0                    | 17.3 µs / 45 µs             | unchanged                   |
| mmap, RT RX timer, kicker thread (`stream_tx.kicker`) | 0              | 5.4–5.7 µs / 19 µs          | unchanged                   |
| xdp, RT RX timer, kick in the media tick        | 0                    | 15.2–15.8 µs / 64–73 µs     | unchanged                   |
| xdp, RT RX timer, kicker thread                 | 0                    | 5.5 µs / 31–51 µs           | unchanged                   |
| xdp, RT RX timer, kick from the RX timer's tick (`stream_tx.kick_in_rx_timer`, the default) | 0 | 5.9–6.0 µs / 66–82 µs | unchanged      |

Every run: `MEDIA_LOCKED=1` on every connected input, `SEQ_NUM_MISMATCH=0`,
no early frames, the RX timer and the kicker with 0 wake errors, 0 recoveries
and 0 missed ticks, and a clean stop on SIGTERM with the full report printed.
The kick itself (`sendto` on the ring) costs 2.9–3.5 µs on average, 32–44 µs
at worst.

What the table says: the late frames in reactor mode were never marginal
misses. They are a stall tail of 1–2 ms events in the reactor's scheduling,
and the RT RX timer removes them entirely. The mean margin reads about
110 µs lower under the RT timer because its "now" is fresh, where the
reactor's "now" is the last media-tick wake, stale by up to a tick. Moving
the ring kick off the media thread cuts the media tick from 15–17 µs to
about 6 µs, and folding it into the RX timer's own tick gets the same result
on a rig with one spare isolated core.

## New in the entity kit

### The streams go through one Ethernet port

Every entity tool (tone generator, audio-io, AM824 I/O, stereo I/O) sends and
receives its AVTP streams through `avb_entity::StreamPort`
(`avb_entity_stream_port.hpp`), a thin owner of core's `AnyEthernetPort`. The
backend is a config key: `net.backend = rawnet | mmap | xdp` and
`net.xdp_queue`, with `rawnet` the default. The entity logs
`stream port: <describe>` at start. On a ring backend TX and RX share state,
so `StreamPort::Guard` is a spin-lock taken only when the port says the two
are not independent; `TalkerStreams::flush_tx()` runs once per media tick;
the listener drain opens a port cycle, because XDP hands out at most 32
frames per cycle and receive used to stop at the first budget.

### Presentation-margin timing per STREAM_INPUT

`StreamInputCounters` carry `margin` and `lateness` as lock-free
`AtomicTimeStats` and a 128-bin `AtomicHistogram` sized by the presentation
offset (bins of 15.625 µs at a 1 ms offset, covering −offset/2 to
+1.5·offset). The tally is split into continuity, presentation margin and
media lock, and media lock is a steady inter-timestamp step rather than a
hard-coded per-packet value, so AM824's SYT cadence locks as AAF does.
Entities print the margin, the lateness and the histogram per input in the
exit banner; `statusbar-avb-audio-io --stream-input-margin-csv` writes the
histograms out.

### The TX kick off the media thread

`stream_tx.kicker` (with `stream_tx.cpu` and `stream_tx.period_us`, default
core 2 at 50 µs) runs the ring kick on its own SCHED_FIFO PTP timer; the
media tick only fills and ends its cycle. `stream_tx.kick_in_rx_timer` folds
the kick into the RT RX timer's tick instead, which is what the Pi rig wants.
The runner records the media-timer callback duration and prints it as
"Media Timer Callback Duration" in the exit banner; the kicker prints its own
final line with `via=rx-timer` or `via=kicker-timer`.

### Defaults, and the tone generator's exception

`stream_rx.rt_timer` defaults on for audio-io, AM824 I/O and stereo I/O, and
`stream_tx.kick_in_rx_timer` defaults on for audio-io. The tone generator
runs no timers, so it pins both off and keeps its reactor RX and in-tick
kick; the kit's `rx_to_reactor()` and `kick_from_media_tick()` are real
fallbacks the tools take when a timer fails to start.

### Shutdown order

A tool's PTP timers stop before the bridge they sleep on:
`EntityRunnerHooks::stop_timers` runs after the media timer stops and before
the context guard, the timer loop treats a stopped bridge as "no sync", and
`PtpTimeBridge::sleep_until_ptp()` takes the timer's running flag and does
not sleep at all for a stopped caller. This was found as a hang at exit on
one node, where the RX timer and kicker outlived the bridge.

### Packaging and versioning

The Debian postinst restarts a running unit across an upgrade; a tree names
itself after a release tag only when it is exactly at one; a snapped TX
capture records how long the frame really was.

## How it was verified

- **Backends, one node:** TX measured at the NIC's own counter, 8,514
  frames/s on each of rawnet, mmap and xdp; RX by injecting 104,000 frames
  from a Mac over a direct link, 104,000 received with 0 sequence errors on
  each backend. The Mac's own AVB stack never shows stream frames to BPF and
  auto-connects its virtual entity to everything, so the ACMP connections are
  made by entity id and the counters are read on the Pi.
- **Pair, two nodes through the switch:** the table above. Scripts drive both
  nodes over ssh, start the entities, connect the six streams, run for a
  fixed time, send SIGTERM to the entity process and collect the exit
  banners.
- **Service node:** the tone generator service ran the review-fix build with
  the bridge healthy, transmitting at about 8,000 frames/s, its CRF input
  connected.

## Known gaps

- The CRF listener input never reports `FRAMES_RX` or `MEDIA_LOCKED` through
  GET_COUNTERS: the CRF branch feeds clock recovery only. CRF frames do arrive
  (about 340/s on the wire). Pre-existing, identical on every backend.
- `net.backend = xdp` under systemd needs `CAP_BPF` and `CAP_NET_ADMIN` in
  the unit; the rig's tone-generator unit does not carry `CAP_BPF` yet, so the
  XDP pair runs were started by hand as root.
- `macb` attaches XDP in generic mode with copy; there is no zero-copy path
  on the Pi 5's on-board port.
