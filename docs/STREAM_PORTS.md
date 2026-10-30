<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# Stream ports - the AVTP frame transport

## Overview

`avb_entity::StreamPort` is the link layer under the AVB stream data plane:
`TalkerStreams` transmits through it and `ListenerStreams` receives through it.
`StreamPorts` opens the port an entity's streams use, chosen by
`StreamTransport` (`packet`, `xdp` or `auto`).

| Header | Type | Link |
|---|---|---|
| `avb_entity_stream_port.hpp` | `StreamPort`, `PacketStreamPort` | AF_PACKET (macOS: BPF) |
| `avb_entity_xdp_stream_port.hpp` | `XdpStreamPort` | one AF_XDP socket |
| `avb_entity_test_stream_port.hpp` | `TestStreamPort` | `net::TestPortContext`, in process |
| `avb_entity_stream_ports.hpp` | `StreamPorts` | picks and opens one of the above |
| `avb_entity_slot_guard.hpp` | `SlotGuard` | stream slot shared with the reactor |
| `avb_entity_stream_rx_sink.hpp` | `StreamRxAudioSink`, `StreamRxTimedAudioSink` | where received audio goes |

## Transmit

`tx_begin(capacity)` hands out a window of exactly `capacity` octets at
`TX_PAYLOAD_OFFSET` of the port's frame buffer; the caller writes the AVTPDU
there and `tx_commit` sends a prefix of it. A window that is not committed is
reused by the next `tx_begin`, so a talker that abandons a packet costs nothing.
`begin_cycle`/`end_cycle` bracket one media tick's packets so a port can batch
them.

## AF_XDP

`XdpStreamPort` binds one AF_XDP socket to the NIC queue that carries SR class
A traffic: queue 0 on igb/igc, where non-IP frames land. Its XDP program
redirects the AVTP stream subtypes to the socket and passes everything else to
the kernel, so ATDECC, gPTP and MRP keep using ordinary sockets. Frames are
built in place in UMEM and sent on the same queue. Receive has no descriptor to
poll; it is drained from the media tick.

The UMEM is `UMEM_FRAMES` (2048) frames of `net::XdpConfig`'s default 4096
octets, 8 MiB locked for the socket's life: a frame must fit one NIC receive
buffer (NET_MODULE.md). That is the whole of a default 8 MiB `RLIMIT_MEMLOCK`,
so a process opening it needs `CAP_IPC_LOCK` (the package's setcap grants it to
`statusbar-avb-audio-io`) or a higher limit (`LimitMEMLOCK=` in its unit).

An AF_XDP socket has one consumer, so it carries both directions or neither: when
receive runs on a thread of its own, `StreamPorts` refuses `xdp`, and `auto`
falls back to AF_PACKET.

The transmit pool: `tx_begin` takes one pooled frame and hands the same window
out until it is committed; the commit consumes it and its completion returns it
to the pool. An empty pool or a full TX ring refuses the frame before a sequence
number is taken, and a full ring does not lose the frame from the pool.

Bind modes (`StreamXdpMode`): `auto_` attaches natively, zero-copy when
the driver offers it; `zerocopy` requires it; `copy` attaches generically in copy
mode and does not reset the adapter. Without libxdp, or off Linux, `open()`
fails with `not_supported_on_platform`. Zero-copy behaviour is verified on
hardware, not in unit tests; the AF_XDP tests need root and skip otherwise.

## Launch time

`tx_commit` takes `launch_time_ns`, the gPTP time the frame is due on the
wire (0: untimed). `PacketStreamPort` sends at once (AF_PACKET without
SO_TXTIME); `TestStreamPort` records the time of each frame the link took
(`last_launch_time_ns`).

The host asks the driver once (`net::netdev::launch_time_supported()`:
netdev `tx-launch-time-fifo`, set by igc and stmmac, not igb), checks the
build with `launch_time_support()` (core's `net::launch_time_support`; AF_XDP TX metadata needs libxdp 1.5 or
later) and hands the one answer to the stream ports and the shaper, so they
cannot disagree. `XdpStreamPortOptions::launch_time`
(`StreamPortsOptions::launch_time`, the kit's `stream_launch_time`) then asks
`open()` for TX metadata; a build without it opens without and says why
(`launch_time_error()`, `log_launch_time`). An igb port is exactly as without.

Pooled frames are taken untimed at `begin_cycle`; the commit writes the
frame's time into its metadata (`net::XdpContext::tx_set_launch_time`): two
stores and a load, no syscall. On a socket with launch time every frame is
timed, since igc writes a launch-time context descriptor only for a frame that
asks and may otherwise reuse the previous one: an untimed frame goes at
CLOCK_TAI now (vDSO). igc's launch-time queue is a FIFO, so `net::LaunchOrder`
(core's, shared with the AES67 transport) raises each time to at least the last one committed on the socket; two talkers
whose departures fall a few frames apart wait at most for each other, under a
packet. Talkers that hand packets over on the tick they depart, on a grid of
whole packets, share that grid: in the steady state the clamp fires only on
nanosecond rounding between rates or on a catch-up after a stall. A time more
than `LaunchOrder::STEP_NS` (1 ms) behind the last is a clock step, not
rounding (nor a launch lead, at most half that), and drops the floor, so a
backward PTP step does not hold frames in the FIFO. `launch_order()` counts
frames held by more than `CLAMP_TOLERANCE_NS` (a packet), the largest hold,
steps, and frames committed after their launch time (an overrunning tick: sent
at now, `launched_late` / `max_late_ns`): the design's own rounding is not
counted, so a count means trouble. The NIC reads the time on its PHC, which carries gPTP (TAI) time when
ptp4l disciplines it, and honours it only with the queue in launch-time mode,
which the shaper's offloaded etf sets (SHAPER.md, "Launch time (etf)").

The raw transmits (`transmit_aaf_raw`, `transmit_am824_raw`,
`transmit_crf_raw`) take the time from the caller, which knows when the packet
departs; the float paths pass 0. A caller that hands a packet over on the tick
it departs gives it a lead (a whole tick or more, under its presentation time
offset) on the wire only: the NIC then sends it at departure plus lead, free of
the tick's send jitter, while its presentation time stays departure plus the
offset. So launch time changes no latency and moves no timestamp (nor the CRF
phase); it lets the offset be set to the lead plus the network transit. Launch
times are always domain-0 (PTP, the NIC's PHC) times: a CRF talker in another
clock domain launches at its first event's domain-0 time plus the lead.

igc (`igc_tx_launchtime`) places the time in a 1 s cycle estimated from the
system's CLOCK_TAI, so the system clock must follow the PHC (phc2sys). A time
already past goes out at once; one more than a cycle ahead lands in the wrong
cycle; a time within 5 µs of the cycle's end may not be honoured. None of
this has run on an igc yet.

## SlotGuard

A stream slot is shared by its data-plane thread (the media thread transmitting,
the RX thread receiving) and the reactor that re-shapes it on a
SET_STREAM_FORMAT. The data-plane thread enters the slot for each packet with
`try_enter()`, which never waits: while the reactor re-shapes the slot it fails
and that packet is skipped. The reactor's `lock()` waits for the data-plane
thread to leave (one packet's work) and keeps it out until `unlock()`. One
atomic with acquire/release ordering: what the reactor wrote before `unlock()`
is what the data-plane thread reads after its next `try_enter()`. A receive slot's
kind (AAF or AM824) also changes only under the lock: the RX thread matches a
frame to its slot by kind unlocked, then checks the kind again once entered.

## Receive sinks

`StreamRxAudioSink` receives each accepted packet's payload on the RX thread.
`StreamRxTimedAudioSink` also gets the reconstructed 64-bit AVTP presentation
time and the packet's dimensions; when it is the only consumer of a stream, AAF
payloads are not decoded to float at all. Both must be cheap and non-blocking.

## Testing

`TestStreamPort` runs the whole transmit path and the receive routing in unit
tests: committed frames are captured whole (`pop_tx`), injected frames are
received (`inject_rx`), and flushes, cycles and multicast membership are
recorded.

## See also

- [`AVB_ENTITY_KIT.md`](AVB_ENTITY_KIT.md) - the kit that owns the ports
- [`SHAPER.md`](SHAPER.md) - shaping the frames a port sends
