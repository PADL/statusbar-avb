<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# Milan Vendor Unique (MVU) commands - Milan v1.3 Sections 5.4.3-5.4.4

## Overview

An MVU command is an AECP VENDOR_UNIQUE command or response (IEEE 1722.1-2021
Clause 9.2.1.5) whose protocol_id is Avnu's MVU protocol, 00-1B-C5-0A-C1-00.
`statusbar/atdecc/atdecc_aecp_mvu.hpp` holds the PDU layouts and
`statusbar/nanoavb/nanoavb_mvu_handler.hpp` answers them for a nanoavb entity,
beside the AEM handler.

The entity implements one Milan version, `nanoavb::MilanVersion` in its
`EntityProfile` (default v1.3; `NanoAvbComponents::set_milan_version`, the
kit's `AvbEntityAudioIOConfig::milan_version`). It selects every MVU layout
and rule that differs between v1.2 and v1.3, and GET_MILAN_INFO says which
(specification_version, v1.3 only). Milan v1.3 Section 5.4.4.1 forbids
changing GET_MILAN_INFO's fields at run time: set the version and the features
before the entity is advertised.

## Wire format

Milan v1.3 Figure 5.4:

```
Bytes 0-21:  AECP common header (AecpDuCommon)
Bytes 22-27: protocol_id (00-1B-C5-0A-C1-00)
Bytes 28-29: u (bit 15) | command_type (bits 14-0)
Bytes 30+:   command_specific_data (per command, Section 5.4.4)
```

u is set in an unsolicited response (Section 5.4.3.2.2; v1.2 calls the bit r,
reserved). A frame shorter than the Ethernet minimum is padded; the padding is
not counted in control_data_length (Section 5.4.3.2).

| Command | v1.3 layout | v1.2 layout |
|---|---|---|
| GET_MILAN_INFO response | 18 octets, with specification_version (Figure 5.6) | 14 octets (v1.2 Figure 5.4) |
| SET/GET_SYSTEM_UNIQUE_ID | u64 number and 64-octet name, 74 octets (Figure 5.7) | one quadlet, 6 octets (v1.2 Figure 5.5) |
| SET/GET_MEDIA_CLOCK_REFERENCE_INFO | 74 octets (Figure 5.8) | the same |
| BIND_STREAM | 18 octets (Figure 5.10) | reserved command type |
| UNBIND_STREAM, GET_STREAM_INPUT_INFO_EX command | 6 octets (Figure 5.11) | reserved |
| GET_STREAM_INPUT_INFO_EX response | 18 octets (Figure 5.12) | reserved |

### SYSTEM_UNIQUE_ID

The v1.2 text calls system_unique_id "64 bit" but its Figure 5.5 draws one
quadlet. The figure is followed, as la_avdecc (whose 1.2 payload is 6
octets, the id a u32) does. la_avdecc (Hive) always sends the v1.3 form, so a
v1.2 entity takes its number when it fits the u32 and answers in the v1.2
layout, which la_avdecc tells apart by its length. A v1.3 entity keeps the
number and the name, answers in the 74-octet form, and refuses a shorter SET
PAYLOAD_TOO_SHORT; 0 with an empty name is its factory default and may be set.

### GET_STREAM_INPUT_INFO_EX

pbsta (3 bits) and acmpsta (5 bits) share one octet, as in v1.2's
GET_STREAM_INFO extension; the talker fields are zero for an unbound input.

## The handler

`MvuCommandHandler` does no packet I/O: the caller hands it frames and a
`send_response` callback. `dispatch_aecp` routes one AECPDU addressed to the
entity: an MVU command to the MVU handler, anything else to the AEM handler
(which answers AEM commands, and NOT_IMPLEMENTED for the other AECP command
types and vendor unique protocols). Replies are synchronous, well inside the
240 ms of Section 5.4.3.4. It is single-threaded, driven by the reactor
thread that drives the AEM handler. `NanoAvbComponents` wires it to the AEM
handler, whose lock and acquisition gate the SETs and bindings, whose
unsolicited-notification registrations receive its notifications; and to the
ACMP listener's Milan sinks (`atdecc::MilanListener`), which BIND_STREAM and
UNBIND_STREAM drive at the time the caller gives `dispatch_aecp` (the
command's, as ACMP's `receive` is told its time).

Implemented (Milan profile only):

- GET_MILAN_INFO (Section 5.4.4.1): protocol_version 1, certification_version
  (`set_certification_version`, 0 until certified), and in v1.3
  specification_version 1.3.0.0 and features_flags: MVU_BINDING when there
  are sinks to bind, plus what `set_extra_features` adds (the kit adds
  TALKER_SIGNAL_PRESENCE when the application reports signal presence). No
  REDUNDANCY, no TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING. A command without its
  reserved field is answered too (the receiver ignores it).
- SET/GET_SYSTEM_UNIQUE_ID (Sections 5.4.4.2-3), as above; v1.2 refuses a SET
  of 0 BAD_ARGUMENTS.
- SET/GET_MEDIA_CLOCK_REFERENCE_INFO (Sections 5.4.4.4-5, 7.6) per
  CLOCK_DOMAIN: default_mcr_prio, user_mcr_prio and media_clock_domain_name
  ("DEFAULT"). The default priority is MVU_MCR_PRIO_NETWORK_INTERFACE (192,
  Milan v1.3 Table 7.3), set for every domain or for one with
  `set_default_mcr_priority`; a user priority still at the old default follows
  it (Section 7.6.1.2). A SET changes only the fields its flags mark, and the
  response carries every supported flag. Milan v1.3 Section 5.4.4.4 makes the
  default part of the entity model: an entity whose default changes reports a
  different entity_model_id.
- BIND_STREAM and UNBIND_STREAM (Sections 5.4.4.6-7, v1.3): the sink's
  RCV_BIND_RX_CMD and RCV_UNBIND_RX_CMD (Section 5.5.3.2), answered in MVU
  form before the sink probes (Section 5.5.3.6.3 steps 3 and 5): a locked
  entity ENTITY_LOCKED, an unknown STREAM_INPUT NO_SUCH_DESCRIPTOR, another
  descriptor type BAD_ARGUMENTS. The binding is saved as an ACMP one is.
- GET_STREAM_INPUT_INFO_EX (Section 5.4.4.8, v1.3): the input's talker, pbsta
  and acmpsta.

SETs and bindings are refused ENTITY_LOCKED while another controller holds the
AEM lock or acquisition (Section 5.4.4.4). A SET or binding that changes
something is sent, u bit set, to the other controllers registered for
unsolicited notifications (Section 5.4.5.3), each with the registration's own
MVU sequence id (Section 5.4.5.2); a SET is also reported to
`set_on_settings_changed` (the values are non-volatile). In v1.3, every change
of what GET_STREAM_INPUT_INFO_EX reports, or of the talker's registration, is
sent to every registered controller (Table 5.21; the listener's
`set_on_info_ex_changed`). An MVU command restarts the controller's monitor
timer (Section 5.4.5.4).

Answered NOT_IMPLEMENTED: every MVU command in the classic profile, the
binding commands and GET_STREAM_INPUT_INFO_EX below v1.3, and reserved
command types.

Not answered:

- GET_MILAN_INFO in the classic profile: an entity whose active
  configuration is not Milan compliant should not reply (Section 5.4.4.1
  NOTE; a requirement in a later revision), so it never claims Milan;
- a frame shorter than its control_data_length declares (malformed).

## Notes

- The response tables for BIND_STREAM and UNBIND_STREAM (Milan v1.3 Tables
  5.33, 5.35, 5.39, 5.41) give message_type VENDOR_UNIQUE_COMMAND; a response
  is sent as VENDOR_UNIQUE_RESPONSE (IEEE 1722.1-2021 Clause 9.2.1.1.5).

## See also

- [`MILAN_LISTENER.md`](MILAN_LISTENER.md) - the sinks MVU binding drives
- [`NANOAVB_ARCHITECTURE.md`](NANOAVB_ARCHITECTURE.md) - the AEM handler beside the MVU handler
- [`AEM_ENTITY_MODEL.md`](AEM_ENTITY_MODEL.md) - the entity model
