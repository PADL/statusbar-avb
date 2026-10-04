<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# IEEE 1722.1 Implementation Gap Analysis

**Date:** 2026-03-31
**Scope:** `statusbar/atdecc`, `statusbar/nanoavb`, `statusbar/avb_entity` vs `standards/ieee1722.1/*.rst`

This analysis focuses on what IS implemented and identifies gaps within those implementations.
It does not flag sub-protocols that were intentionally not implemented.

> **Status update (2026-06-20):** this is a point-in-time snapshot from 2026-03-31.
> Several High-severity (P0) gaps below have since been **resolved**; the original
> entries are kept for history and annotated inline as `[RESOLVED 2026-06-20]`:
> - AEM `cr` bit is now parsed — `command_code()` masks `0x3FFF` (`atdecc_aecp_aem.hpp`).
> - ADP `available_index` is now zeroed on `ENTITY_DEPARTING` (`nanoavb_adp.cpp`).
> - Listener SM enforces `listenerIsConnected` → `LISTENER_EXCLUSIVE` (`atdecc_acmp_listener_sm.cpp`).
> - Unsolicited notification subsystem is implemented — register/deregister + dispatch
>   (`nanoavb_entity.cpp`).
>
> **Status update (2026-10-04, IEEE 1722.1-2021/Cor 1-2025 review):** the
> nanoavb entity now notifies on ACQUIRE_ENTITY, LOCK_ENTITY (and lock expiry),
> DEREGISTER_UNSOLICITED_NOTIFICATION, REBOOT and SET_MEMORY_OBJECT_LENGTH, and
> implicitly registers the acquiring controller (7.5.2); it exposes
> `notify_counters_changed()` / `notify_stream_info_changed()` for the SRP, gPTP
> and stream-reception driven GET_COUNTERS / GET_STREAM_INFO notifications;
> implements GET_DYNAMIC_INFO with the corrigendum's fixed-size GET list, REBOOT
> (via `on_reboot`) and SET/GET_MEMORY_OBJECT_LENGTH (via handler hooks); and
> sends entity-side IN_PROGRESS: a SET hook returning `AEM_STATUS_IN_PROGRESS`
> (e.g. SET_STREAM_FORMAT awaiting an SRP re-reservation, Cor 1 7.4.9.3) is
> answered IN_PROGRESS, repeated every 120 ms from `tick()`, and finished by
> `complete_pending_command()`. ACQUIRE_ENTITY's CONTROLLER_AVAILABLE handshake
> and LOCK_ENTITY's timeout were already in place.

---

## Executive Summary

The implementation is strong at the **wire format level** -- all 36 descriptor structs are complete
with correct field layouts, ACMPDU formats are complete, and protocol constants (message types,
status codes, flags, timeouts) are nearly complete. The main gaps are in **behavioral logic**:
state machine functions that enforce protocol rules (authorization checks, retry logic,
SRP integration hooks), the unsolicited notification subsystem, and the AEM `cr` bit.

### By Severity

**High (protocol correctness):**
- ~~AEM `cr` bit not parsed -- `command_type` mask is 0x7FFF instead of 0x3FFF~~ **[RESOLVED 2026-06-20]**
- ~~ADP `available_index` sent as current value on ENTITY_DEPARTING instead of zero~~ **[RESOLVED 2026-06-20]**
- ~~Listener SM missing `listenerIsConnected` check -- no LISTENER_EXCLUSIVE rejection~~ **[RESOLVED 2026-06-20]**
- Listener SM missing retry on first TX timeout
- ACQUIRE_ENTITY missing CONTROLLER_AVAILABLE handshake
- LOCK_ENTITY missing timeout expiration

**Medium (missing functionality within implemented features):**
- ~~No unsolicited notification subsystem (registration, dispatch)~~ **[RESOLVED 2026-06-20]**
- No IN_PROGRESS send/receive logic (constants defined but unused)
- No acquired/locked authorization checks in ACMP state machines
- ADP missing 8 entity capability flag constants (bits 6-13)
- ADP missing `current_configuration_index` field in AdpDu
- No Address Access entity-side handler

**Low (minor/naming):**
- `backedup_talker_unique` missing `_id` suffix
- `CLOCK_SOURCE_TYPE_MEDIA_CLOCK_STREAM` not in standard (0x0003 is reserved)
- `SET_PTP_PORT_CURRENT_INTERVALS` (0x005A) is reserved in standard
- Memory object type constants incomplete (6 of 15)

---

## Clause 6: ADP (Discovery Protocol)

### AdpDu Struct Fields

| Finding | Severity |
|---------|----------|
| `current_configuration_index` field missing -- absorbed into oversized `reserved0` (3 bytes instead of 1) | **Missing** |
| All other ADPDU fields present with correct offsets (verified by static_assert) | OK |

### Capability Flags

| Finding | Severity |
|---------|----------|
| Entity capability bits 6-13 missing (8 flags): ACMP_ACQUIRE_WITH_AEM, ACMP_AUTHENTICATE_WITH_AEM, SUPPORTS_UDPv4/v6_ATDECC/STREAMING, MULTIPLE_PTP_INSTANCES, AEM_CONFIGURATION_INDEX_VALID | **Missing** |
| `format_entity_capabilities_to()` does not decode bits 6-13 | **Missing** |
| Talker capabilities (8 flags): Complete | OK |
| Listener capabilities (8 flags): Complete | OK |
| Controller capabilities: Complete | OK |
| All 3 message types defined | OK |

### State Machines

| Finding | Severity |
|---------|----------|
| Advertise Entity SM: `randomDeviceDelay()` not implemented (RST requires uniform random 0 to valid_time/5) | **Missing** |
| Advertise Entity SM: reannounce timer uses config value instead of RST formula `MAX(valid_time/2 - 2, 1)` | **Divergent** |
| Advertise Interface SM: not implemented as separate per-interface SM | **Missing** |
| Discovery SM: entirely absent (needed for controller/discovery functionality) | **Missing** |
| Discovery Interface SM: entirely absent | **Missing** |
| `available_index` on ENTITY_DEPARTING: ~~sends current value instead of zero~~ now zeroed | **Fixed (2026-06-20)** |
| `adp_adv_sm` state topology: Off/Advertising vs RST's Advertise/Waiting | **Divergent** |

---

## Clause 7: AEM (Entity Model)

### Descriptor Structs (7.2)

All 36 descriptor types have C++ structs with correct field layouts. **Excellent coverage.**

| Finding | Severity |
|---------|----------|
| All 36 descriptor type constants match RST values exactly | OK |
| All 36 descriptor structs have complete fixed-field layouts matching RST | OK |
| Variable-length data (sampling_rates, formats, mappings, etc.) correctly excluded from fixed-size structs | OK |
| 2021-edition fields present (redundant_offset, timing, aes3, transcoder_type, etc.) | OK |
| `DescriptorStream::backedup_talker_unique` missing `_id` suffix vs RST `backedup_talker_unique_id` | **Divergent** (minor) |
| `CLOCK_SOURCE_TYPE_MEDIA_CLOCK_STREAM` (0x0003) not in RST -- 0x0003 is reserved | **Divergent** |
| Memory object type constants: 6 of 15 defined (missing SVG/PNG/DAE variants 0x0006-0x000e) | **Incomplete** |

### EntityModel / EntityModelBuilder (nanoavb)

17 of 36 descriptor types are storable/buildable via EntityModel. The remaining 19 (video/sensor
units, signal processing descriptors, timing/PTP) have complete structs but no EntityModel storage.
This is a reasonable design choice for an audio-focused AVB entity.

### Commands and Responses (7.4)

| Finding | Severity |
|---------|----------|
| `cr` bit in AemDu: ~~`command_code()` masks with 0x7FFF (15 bits)~~ now masks 0x3FFF (14 bits) | **Fixed (2026-06-20)** |
| AUTH_GET_NONCE (0x0067) and AUTH_ADD_KEY_NONCE (0x0068) command codes not defined | **Missing** |
| SET_PTP_PORT_CURRENT_INTERVALS (0x005A) defined but RST says reserved | **Divergent** |
| All 13 AEM status codes match RST exactly | OK |
| All timeout constants correct (AEM 250ms, IN_PROGRESS 120ms, LOCK 60s, AA 250ms) | OK |

### Command Payload Structs

| Finding | Severity |
|---------|----------|
| AemAcquireEntityPayload: complete (16 bytes) | OK |
| AemLockEntityPayload: complete (16 bytes) | OK |
| AemReadDescriptorCommandPayload / ResponsePayload: complete | OK |
| AemStreamFormatPayload: complete (12 bytes) | OK |
| AemStreamInfoPayload: complete (48 bytes) | OK |
| AemNamePayload: complete (72 bytes) | OK |
| AemCountersPayload: complete (136 bytes) | OK |
| AemAvbInfoPayload: has 8 extra reserved bytes possibly misaligning variable data; missing AVTP_DOWN and AVTP_DOWN_VALID flags | **Incomplete** |

### NanoAVB AemCommandHandler

| Command | Status |
|---------|--------|
| READ_DESCRIPTOR | Fully implemented |
| ACQUIRE_ENTITY | **Incomplete** -- missing CONTROLLER_AVAILABLE handshake and IN_PROGRESS flow |
| LOCK_ENTITY | **Incomplete** -- missing timeout expiration and acquired-controller-only check |
| GET_CONFIGURATION | Implemented |
| ENTITY_AVAILABLE | Implemented |
| CONTROLLER_AVAILABLE | Implemented |
| SET_CONTROL / GET_CONTROL / GET_COUNTERS | Stub (returns NOT_IMPLEMENTED) |
| All others | Returns NOT_IMPLEMENTED (by design) |

### Notifications (7.5)

| Finding | Severity |
|---------|----------|
| ~~Entire unsolicited notification subsystem missing (registration, dispatch, identification)~~ now implemented | **Fixed (2026-06-20)** |
| ~~REGISTER/DEREGISTER_UNSOLICITED_NOTIFICATION: command codes defined, no handler~~ handlers added | **Fixed (2026-06-20)** |
| No registered controller list maintained | **Missing** |
| AemDu has `is_unsolicited()` / `set_unsolicited()` accessors -- wire format ready | OK |
| No payload struct for REGISTER_UNSOLICITED_NOTIFICATION flags | **Missing** |

### IN_PROGRESS Handling

| Finding | Severity |
|---------|----------|
| `AEM_STATUS_IN_PROGRESS = 9` defined | OK |
| `AEM_IN_PROGRESS_TIMEOUT_MS = 120` defined | OK |
| ~~No code ever sends IN_PROGRESS responses~~ entity side: ACQUIRE_ENTITY handshake and deferred SET commands (`complete_pending_command`) | **Fixed (2026-10-04)** |
| No controller-side handling of received IN_PROGRESS (reset timeout, wait for final) | **Missing** |

---

## Clause 8: ACMP (Connection Management)

### Wire Format and Constants

| Finding | Severity |
|---------|----------|
| AcmpDu (56 bytes) and AcmpDu2021 (96 bytes): all fields complete | OK |
| All 14 message types (0-13) defined and match RST | OK |
| All 20 status codes match RST | OK |
| All 10 flag bits match RST | OK |
| All 7 timeout values match RST | OK |
| ListenerStreamInfo, ListenerPair, TalkerStreamInfo, InflightCommand, ACMPCommandParams: complete | OK |

### Controller State Machine (8.2.3)

| Finding | Severity |
|---------|----------|
| States and events: complete | OK |
| Timeout handler does not call `processResponse` -- caller never learns about final timeout | **Incomplete** |
| `cancelTimeout` + `removeInflight` merged (functionally equivalent) | OK |
| `makeCommand` + `txCommand` merged (functionally equivalent) | OK |

### Listener State Machine (8.2.7)

| Finding | Severity |
|---------|----------|
| States and events: complete | OK |
| `listenerIsConnected()` ~~not implemented -- no LISTENER_EXCLUSIVE check on CONNECT_RX~~ now enforced | **Fixed (2026-06-20)** |
| `listenerIsConnectedTo()` not implemented -- no reconnection handling (disconnect-before-reconnect) | **Missing** |
| `listenerIsAcquiredOrLockedByOther()` not implemented -- no CONTROLLER_NOT_AUTHORIZED check | **Missing** |
| No retry on first TX timeout (immediately sends LISTENER_TALKER_TIMEOUT) | **Incomplete** |
| `txCommand` ignores send failure (no COULD_NOT_SEND_MESSAGE response) | **Incomplete** |
| `connectListener` does not hook into SRP registration | **Incomplete** |
| `disconnectListener` does not hook into SRP de-registration | **Incomplete** |
| Uses single-slot pending command (correct for one-stream-at-a-time) | OK |

### Talker State Machine (8.2.5)

| Finding | Severity |
|---------|----------|
| States and events: complete | OK |
| `talkerIsAcquiredOrLockedByOther()` not implemented -- no CONTROLLER_NOT_AUTHORIZED check | **Missing** |
| `connectTalker` does not hook into SRP/MAAP registration | **Incomplete** |
| `connectTalker` returns TALKER_NO_STREAM_INDEX when listener array full (should be TALKER_EXCLUSIVE for single-listener case) | **Incomplete** |
| `disconnectTalker` does not hook into SRP de-registration when connection_count reaches zero | **Incomplete** |
| `getConnection` sets response `connection_count` to actual stream count instead of echoing command value | **Divergent** |

### NanoAVB ACMP Wrappers

| Finding | Severity |
|---------|----------|
| `NanoAvbAcmpTalker::on_connect` / `on_disconnect` callbacks defined but never called | **Missing** (wiring bug) |
| `NanoAvbAcmpListener::on_connect` / `on_disconnect` callbacks defined but never called | **Missing** (wiring bug) |

---

## Clause 9: AECP (Enumeration and Control)

### AECP Common

| Finding | Severity |
|---------|----------|
| AecpDuCommon header: complete, all fields match RST | OK |
| All message types defined | OK |
| AECP_MAX_CONTROL_DATA_LENGTH = 524 correct | OK |

### Address Access (9.2.1.3)

| Finding | Severity |
|---------|----------|
| Wire format (AecpAaDu, TLV parsing, TLV builder): complete | OK |
| All 3 TLV modes (READ, WRITE, EXECUTE) defined | OK |
| All 8 AA status codes defined | OK |
| AA timeout 250ms correct | OK |
| No entity-side AA handler (no AaCommandHandler equivalent) | **Missing** |
| Analysis and pcap tools implemented | OK |

---

## Prioritized Action Items

### P0 -- Protocol Correctness
1. Fix AemDu `command_type` field mask to 0x3FFF and add `cr` bit accessor
2. Fix ADP `available_index` to send zero on ENTITY_DEPARTING
3. Add `listenerIsConnected()` check to Listener SM (LISTENER_EXCLUSIVE)
4. Add Listener SM retry on first TX timeout before sending LISTENER_TALKER_TIMEOUT

### P1 -- Important Gaps
5. Add `current_configuration_index` field to AdpDu (split `reserved0` into 1 byte + 2 byte field)
6. Add missing entity capability flags (bits 6-13)
7. Add acquired/locked authorization checks to ACMP Talker and Listener SMs
8. Fix `getConnection` response `connection_count` to echo command value
9. Implement LOCK_ENTITY timeout expiration
10. Wire NanoAVB ACMP `on_connect`/`on_disconnect` callbacks

### P2 -- Feature Completeness
11. Implement unsolicited notification subsystem (registration + dispatch)
12. Implement IN_PROGRESS send/receive flow
13. Implement ACQUIRE_ENTITY CONTROLLER_AVAILABLE handshake
14. Add SRP integration hooks to ACMP connectListener/disconnectListener/connectTalker/disconnectTalker
15. Add AemAvbInfoPayload AVTP_DOWN / AVTP_DOWN_VALID flags

### P3 -- Minor / Cleanup
16. Fix `backedup_talker_unique` field name to `backedup_talker_unique_id`
17. Add missing memory object type constants (0x0006-0x000e)
18. Add AUTH_GET_NONCE (0x0067) and AUTH_ADD_KEY_NONCE (0x0068) command codes
19. Review SET_PTP_PORT_CURRENT_INTERVALS (0x005A) -- reserved in current standard
20. Review CLOCK_SOURCE_TYPE_MEDIA_CLOCK_STREAM (0x0003) -- reserved in standard