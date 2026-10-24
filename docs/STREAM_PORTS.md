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

## SlotGuard

A stream slot is shared by its data-plane thread (the media thread transmitting,
the RX thread receiving) and the reactor that re-shapes it on a
SET_STREAM_FORMAT. The data-plane thread enters the slot for each packet with
`try_enter()`, which never waits: while the reactor re-shapes the slot it fails
and that packet is skipped. The reactor's `lock()` waits for the data-plane
thread to leave (one packet's work) and keeps it out until `unlock()`. One
atomic with acquire/release ordering: what the reactor wrote before `unlock()`
is what the data-plane thread reads after its next `try_enter()`.

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
