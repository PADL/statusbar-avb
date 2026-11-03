<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# Stream ports - the AVTP frame transport

## Overview

`avb_entity::StreamPort` is the link layer under the AVB stream data plane, a
thin owner of core's `net::AnyEthernetPort`: `TalkerStreams` transmits through
it and `ListenerStreams` receives through it. The backend is a configuration
key, `net.backend = rawnet | mmap | xdp`; the ATDECC control plane keeps its own
raw socket whichever it is. `StreamPort::Guard` serialises transmit against
receive only when the backend says the two are not independent.

| Header | Type | What |
|---|---|---|
| `avb_entity_stream_port.hpp` | `StreamPort`, `StreamPortConfig` | the port, its backend and xdp options |
| `avb_entity_stream_rx_sink.hpp` | `StreamRxAudioSink`, `StreamRxTimedAudioSink` | where received audio goes |

## Transmit

`TalkerStreams` builds each AVTPDU in its own buffer, then
`send_stream_frame()` takes a TX slot (`tx_start`), writes the tagged header and
the payload into it (`build_stream_frame`) and commits it. `flush_tx()` once per
media tick kicks the ring backends and ends the port's cycle; with an external
kick (`set_external_kick`) the kicker thread calls `kick_tx()` and `flush_tx()`
only ends the cycle. It fails with the kick's error, else the cycle's.

## AF_XDP

The xdp backend binds one AF_XDP socket to the NIC queue that carries SR class
A traffic (`net.xdp_queue`). Its XDP program redirects the AVTP stream subtypes
to the socket and passes everything else to the kernel, so ATDECC, gPTP and MRP
keep using ordinary sockets. `net.xdp_bind` (`auto`, `zero_copy`, `copy`) and
`net.xdp_attach` (`native`, `generic`) choose the bind and attach modes, and
`net.xdp_pin_path` pins the program and its maps in bpffs. With
`net.xdp_fallback` a socket that does not open gives way to rawnet, and
`AvbEntityKit::stream_xdp_error()` keeps the reason. `StreamPort::xdp()` gives
the socket itself (counters, busy poll) and `reset_counters()` zeroes its
counters and the launch-time order at the next `end_cycle()`. Zero-copy
behaviour is verified on hardware; the AF_XDP tests need root and skip
otherwise.

## Launch time

With `stream.launch_time` the xdp socket opens with TX metadata and each frame
carries a launch time (Documentation/networking/xsk-tx-metadata.rst), in the
sending PHC's own time. The host gives the port that clock with
`set_phc_clock(net::ClockFn)` on the control thread before the media thread
runs; without one, frames go untimed. `send_stream_frame()` takes the time the
caller wants a frame on the wire (0: now) and `launch_time()` orders it after
the previous one (`net::LaunchOrder`), counting holds, clock steps and frames
committed late (`launch_order()`). When to use a lead is the host's decision;
the NIC's etf offload is the shaper's (SHAPER.md, "Launch time (etf)").

## SlotGuard

A stream slot is shared by its data-plane thread (the media thread transmitting,
the RX thread receiving) and the reactor that re-shapes it on a
SET_STREAM_FORMAT or, on a receive slot, replaces its consumer (`set_consume`,
`set_crf`). Each slot has a core `itc::SpinLock`. The data-plane thread takes it
for each packet with `try_lock()`, which never waits: while the reactor holds it
that packet is skipped. The reactor's `lock()` spins until the data-plane thread
has finished its packet (one packet's work) and keeps it out until `unlock()`;
what the reactor wrote before `unlock()` is what the data-plane thread reads
after its next `try_lock()`. A receive slot's kind (AAF or AM824) also changes
only under the lock: the RX thread matches a frame to its slot by kind unlocked,
then checks the kind again once it holds the lock.

## Receive sinks

`StreamRxAudioSink` receives each accepted packet's payload on the RX thread.
`StreamRxTimedAudioSink` also gets the reconstructed 64-bit AVTP presentation
time and the packet's dimensions; when it is the only consumer of a stream, AAF
payloads are not decoded to float at all. Both must be cheap and non-blocking.

## Testing

`StreamPort::open_test()` opens core's in-process test port
(`net::AnyEthernetPort::open_test()`): frames are captured with their launch
times, frames are injected, multicast groups are recorded. The stream port,
talker, listener, kit and tone generator tests run their whole data plane on
it, without a socket or privileges; a kit keeps a port opened before start().

## See also

- [`AVB_ENTITY_KIT.md`](AVB_ENTITY_KIT.md) - the kit that owns the ports
- [`SHAPER.md`](SHAPER.md) - shaping the frames a port sends
