<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# Milan listener: binding, probing and receive

The Milan listener (`statusbar/atdecc/atdecc_acmp_milan_listener.hpp`) binds a
STREAM_INPUT to a talker stream on a controller's command, probes the talker
until it answers, and settles on the stream once the talker's Talker attribute
is registered (Milan v1.2 Section 5.5.3, Tables 5.27-5.39). The entity's
`avb_entity::ListenerStreams` receives and checks the stream's AVTPDUs.

## The sink state machine

`atdecc_acmp_milan_listener.hpp` keeps one sink state machine per STREAM_INPUT
(Milan v1.2 Table 5.30): UNBOUND, PRB_W_AVAIL, PRB_W_DELAY, PRB_W_RESP,
PRB_W_RESP2, PRB_W_RETRY, SETTLED_NO_RSV and SETTLED_RSV_OK. Its events are the
controller's BIND_RX / UNBIND_RX / GET_RX_STATE, the talker's PROBE_TX_RESPONSE,
the four timers (TMR_NO_RESP 200 ms, TMR_RETRY 4 s, TMR_DELAY random, TMR_NO_TK
10 s), the per-sink ADP discovery state machine (EVT_TK_DISCOVERED /
EVT_TK_DEPARTED, Section 5.6.4) and the registration of the settled stream's
Talker attribute (EVT_TK_REGISTERED / EVT_TK_UNREGISTERED), which the caller
reports.

BIND_RX and UNBIND_RX are CONNECT_RX and DISCONNECT_RX renamed (Section 5.5.2.2:
the same message types), so a classic controller's CONNECT_RX is a bind: it is
answered at once with connection_count 1 and the stream fields zero, and the
probe follows. UNBIND_RX never sends DISCONNECT_TX; the talker learns from MSRP
alone (Section 5.5.2.5).

The machine does no I/O and reads no clock: every event carries its time and
the effects go out through hooks. Each hook is called after the sink's state is
updated, so a hook may feed the machine again (for example, report a
registration from `on_settled`).

The registration the caller reports is matched on stream ID, destination
address and VLAN ID (Section 5.5.3.3). EVT_TK_REGISTERED fires on Not registered
-> Registered and EVT_TK_UNREGISTERED on the reverse; a switch between Advertise
and Failed only changes what GET_RX_STATE reports.

## Format checks on receive

Milan v1.2 Section 4.4.2.2: "A Listener PAAD shall discard the incoming AVTPDUs
that don't respect the format which is configured in the associated
STREAM_INPUT descriptor." `aaf_header_matches_format` checks the AAF header
against the input's current format: the sample format, nominal rate and
channels per frame must match; the payload (stream_data_length) must be the
format's samples per frame when it names them; and the bit depth must be valid
for the sample format's container (1 to its width; any for formats without
one), since some talkers mark INT_32 samples carrying 24 bits as such, with the
same byte layout. A SET_STREAM_FORMAT or SET_SAMPLING_RATE rebuilds the slot's
deserializer, so what the input accepts follows its current format.

## Receive paths

Both AAF receive paths, decoded and raw-only, build their timed packet with
`aaf_timed_packet`, so a timed sink sees the same packet either way. With a
timed sink attached (`set_timed_audio_sink`) and no `StreamConsumeFn`, an AAF
stream is not decoded to float: its timing is taken from the header alone
(`aaf_rx_packet`); an AM824 stream likewise (`am824_timed_packet`,
`am824_rx_packet`). `drain_rx` stamps the frames it drains with the caller's gPTP
"now": the reactor adapter's `current_gptp_ns()`, or a dedicated RX timer's
wake time.

## See also

- [`ACMP_STATE_MACHINES.md`](ACMP_STATE_MACHINES.md) - the IEEE 1722.1 ACMP state machines
- [`MILAN_TALKER.md`](MILAN_TALKER.md) - the talker side
- [`AVB_ENTITY_KIT.md`](AVB_ENTITY_KIT.md) - how the kit wires the listener
- [`SHAPER.md`](SHAPER.md) - the listener VLAN
