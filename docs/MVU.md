<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# Milan Vendor Unique (MVU) commands - Milan v1.2 Sections 5.4.3-5.4.4

## Overview

An MVU command is an AECP VENDOR_UNIQUE command or response (IEEE 1722.1-2021
Clause 9.2.1.5) whose protocol_id is Avnu's MVU protocol, 00-1B-C5-0A-C1-00.
`statusbar/atdecc/atdecc_aecp_mvu.hpp` holds the PDU layouts and
`statusbar/nanoavb/nanoavb_mvu_handler.hpp` answers them for a nanoavb entity,
beside the AEM handler.

## Wire format

Milan v1.2 Figure 5.2:

```
Bytes 0-21:  AECP common header (AecpDuCommon)
Bytes 22-27: protocol_id (00-1B-C5-0A-C1-00)
Bytes 28-29: r (bit 15, reserved, 0) | command_type (bits 14-0)
Bytes 30+:   command_specific_data (per command, Section 5.4.4)
```

A frame shorter than the Ethernet minimum is padded; the padding is not
counted in control_data_length (Section 5.4.3.2).

The layouts are Milan v1.2's. Milan v1.3 changes some of them:
GET_MILAN_INFO gains specification_version, SYSTEM_UNIQUE_ID becomes a u64
plus a name, and PAYLOAD_TOO_SHORT and other statuses are added. A v1.3
variant would sit beside the v1.2 layouts, selected by the entity's Milan
version.

### SYSTEM_UNIQUE_ID

The v1.2 text calls system_unique_id "64 bit" but Figure 5.5 draws one
quadlet. The figure is followed, as GenAVB's v1.2 layout and la_avdecc (whose
1.2 payload is 6 octets, the id a u32) do. v1.3 widens it
(`MvuSystemUniqueIdPayloadV13`, v1.3 Figure 5.7: a u64 number and a 64-octet
name). la_avdecc (Hive) always sends the v1.3 form, so a v1.2 entity takes its
number when it fits the u32 and answers in the v1.2 layout, which la_avdecc
tells apart by its length.

## The handler

`MvuCommandHandler` does no packet I/O: the caller hands it frames and a
`send_response` callback. `dispatch_aecp` routes one AECPDU addressed to the
entity: an MVU command to the MVU handler, anything else to the AEM handler
(which answers AEM commands, and NOT_IMPLEMENTED for the other AECP command
types and vendor unique protocols). Replies are synchronous, well inside the
240 ms of Section 5.4.3.4. It is single-threaded, driven by the reactor
thread that drives the AEM handler. `NanoAvbComponents` wires it to the AEM
handler, whose lock and acquisition gate the SETs and whose
unsolicited-notification registrations receive its notifications; without
one, nothing is locked and nobody is notified.

Implemented (Milan profile only):

- GET_MILAN_INFO (Section 5.4.4.1), v1.2 layout (Figure 5.4: reserved,
  protocol_version 1, features_flags 0, certification_version, no
  specification_version). A command without its reserved field is answered
  too (the receiver ignores it).
- SET/GET_SYSTEM_UNIQUE_ID (Sections 5.4.4.2-3), the v1.2 u32 (default 0; a
  SET of 0 is refused BAD_ARGUMENTS). A v1.3-form SET is taken when its
  number fits the u32.
- SET/GET_MEDIA_CLOCK_REFERENCE_INFO (Sections 5.4.4.4-5, 7.6) per
  CLOCK_DOMAIN: default_mcr_prio, user_mcr_prio and media_clock_domain_name
  ("DEFAULT"). The default priority is MVU_MCR_PRIO_NETWORK_INTERFACE (192,
  Milan v1.3 Table 7.3), set for every domain or for one with
  `set_default_mcr_priority`; a user priority still at the old default follows
  it (Section 7.6.1.2). A SET changes only the fields its flags mark, and the
  response carries every supported flag. Milan v1.3 Section 5.4.4.4 makes the
  default part of the entity model: an entity whose default changes reports a
  different entity_model_id.

SETs are refused ENTITY_LOCKED while another controller holds the AEM lock or
acquisition (Section 5.4.4.4). A SET that changes something is sent, u bit
set, to the other controllers registered for unsolicited notifications (the
v1.3 Section 5.4.5.2 mechanism; v1.2 has none), each with the registration's
MVU sequence id, and reported to `set_on_settings_changed` (the values are
non-volatile). An MVU command restarts the controller's monitor timer
(Section 5.4.5.3).

Answered NOT_IMPLEMENTED: every other MVU command in the classic profile, and
reserved command types.

Not answered:

- GET_MILAN_INFO in the classic profile: an entity whose active
  configuration is not Milan compliant should not reply (Section 5.4.4.1
  NOTE; a requirement in a later revision), so it never claims Milan;
- a frame shorter than its control_data_length declares (malformed).

## See also

- [`NANOAVB_ARCHITECTURE.md`](NANOAVB_ARCHITECTURE.md) - the AEM handler beside the MVU handler
- [`AEM_ENTITY_MODEL.md`](AEM_ENTITY_MODEL.md) - the entity model
