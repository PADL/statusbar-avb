<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# Milan Vendor Unique (MVU) commands - Milan v1.3 Sections 5.4.3-5.4.4

## Overview

An MVU command is an AECP VENDOR_UNIQUE command or response (IEEE 1722.1-2021
Clause 9.2.1.5) whose protocol_id is Avnu's MVU protocol, 00-1B-C5-0A-C1-00.
`statusbar/atdecc/atdecc_aecp_mvu.hpp` holds the PDU layouts and the AEM
handler (`nanoavb::AemCommandHandler`) answers them for a nanoavb entity.

A Milan entity implements Milan v1.3, and GET_MILAN_INFO says so
(specification_version 1.3.0.0). Milan v1.3 Section 5.4.4.1 forbids changing
GET_MILAN_INFO's fields at run time: set the features before the entity is
advertised.

## Wire format

Milan v1.3 Figure 5.4:

```
Bytes 0-21:  AECP common header (AecpDuCommon)
Bytes 22-27: protocol_id (00-1B-C5-0A-C1-00)
Bytes 28-29: u (bit 15) | command_type (bits 14-0)
Bytes 30+:   command_specific_data (per command, Section 5.4.4)
```

u is set in an unsolicited response (Section 5.4.3.2.2). A frame shorter than the Ethernet minimum is padded; the padding is
not counted in control_data_length (Section 5.4.3.2).

| Command | Layout |
|---|---|
| GET_MILAN_INFO response | 18 octets, with specification_version (Figure 5.6) |
| SET/GET_SYSTEM_UNIQUE_ID | u64 number and 64-octet name, 74 octets (Figure 5.7) |
| SET/GET_MEDIA_CLOCK_REFERENCE_INFO | 74 octets (Figure 5.8) |
| BIND_STREAM | 18 octets (Figure 5.10) |
| UNBIND_STREAM, GET_STREAM_INPUT_INFO_EX command | 6 octets (Figure 5.11) |
| GET_STREAM_INPUT_INFO_EX response | 18 octets (Figure 5.12) |

`atdecc_aecp_mvu.hpp` keeps Milan v1.2's shorter GET_MILAN_INFO and
SYSTEM_UNIQUE_ID layouts for controllers that read them.

### SYSTEM_UNIQUE_ID

The entity keeps the number and the name, answers in the 74-octet form, and
refuses a shorter SET PAYLOAD_TOO_SHORT (Milan v1.2's 6-octet one among them);
0 with an empty name is its factory default and may be set.

### GET_STREAM_INPUT_INFO_EX

pbsta (3 bits) and acmpsta (5 bits) share one octet, as in Milan v1.2's
GET_STREAM_INFO extension; the talker fields are zero for an unbound input.

## The handler

`AemCommandHandler` answers MVU beside AEM, on the reactor thread, its lock and
acquisition gating the SETs, its unsolicited-notification registrations receiving
the notifications. Replies are synchronous, well inside the 240 ms of Section
5.4.3.4. BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX go to the Milan
ACMP listener's sinks: see [`MILAN_ACMP.md`](MILAN_ACMP.md) ("Binding over AECP MVU").

Implemented (Milan profile only):

- GET_MILAN_INFO (Section 5.4.4.1): specification_version 1.3.0.0 and the
  features_flags the entity gives (`set_milan_info`; the kit sets MVU_BINDING under
  the Milan profile and TALKER_SIGNAL_PRESENCE when the application reports it).
- SET/GET_SYSTEM_UNIQUE_ID (Sections 5.4.4.2-3), as above; a SET is refused
  ENTITY_LOCKED while another controller holds the lock, BAD_ARGUMENTS for a name
  that is not UTF-8.
- SET/GET_MEDIA_CLOCK_REFERENCE_INFO (Sections 5.4.4.4-5, 7.6) per CLOCK_DOMAIN.
  The default priority is MVU_MCR_PRIO_DEFAULT (128, Milan v1.3 Table 7.3) until the
  entity sets its own (`set_default_mcr_priority`; the kit's `mcr.default_priority`).
  Milan v1.3 Section 5.4.4.4 makes the default part of the entity model: an entity
  whose default changes reports a different entity_model_id.
- BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX (Sections 5.4.4.6-8).

A SET that changes something is sent, u bit set, to the other registered controllers
(Section 5.4.5.3) and reported to `set_on_mvu_settings_changed` (the values are
non-volatile: the kit saves them in its persistent state).

Answered NOT_IMPLEMENTED: every MVU command in the classic profile, and command
types Milan v1.3 Table 5.15 does not define.

## Notes

- The response tables for BIND_STREAM and UNBIND_STREAM (Milan v1.3 Tables
  5.33, 5.35, 5.39, 5.41) give message_type VENDOR_UNIQUE_COMMAND; a response
  is sent as VENDOR_UNIQUE_RESPONSE (IEEE 1722.1-2021 Clause 9.2.1.1.5).

## See also

- [`MILAN_ACMP.md`](MILAN_ACMP.md) - the sinks MVU binding drives
- [`NANOAVB_ARCHITECTURE.md`](NANOAVB_ARCHITECTURE.md) - the AEM handler
- [`AEM_ENTITY_MODEL.md`](AEM_ENTITY_MODEL.md) - the entity model
