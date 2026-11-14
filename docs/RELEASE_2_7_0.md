<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->
<!-- SPDX-License-Identifier: MIT -->

# statusbar 2.7.0

Released 2026-11-12. What the release contains, what changed on the wire
and in the public APIs, how it was built and verified, and where the
per-repository detail is.

2.7.0 is the code-review release. A whole-tree read of the four libraries
at 2.6.0 (core, crypto, audio, avb; about 190 k lines, eight independent
read-only passes briefed with the project's rules: told the time, ordered
IEEE types, no allocation or blocking on hot paths, fixed-capacity tables)
produced 5 high, 39 medium and about 60 low findings. This release fixes
all 5 highs and 38 of the 39 mediums, each as one commit with a test that
pins the fix. The remaining medium is an interoperability decision and is
listed under "Not changed". The lows are deferred. Beside the review
fixes, the core CMake export takes an `INCLUDE_DIR` so a dependent tree
can install headers from a non-standard root.

Nothing in this release changes a wire format. Several things change what
goes on the wire: the MVRP LeaveAll bit is now actually transmitted, the
Talker Advertise carries the VLAN and priority the stream frames carry, a
MAAP defender keeps its range against a higher MAC, the ACMP listener
answers a DISCONNECT in the order of Figure 8-3, and the AEM controller
retries once. All of these are the standard as written.

## The tags

| Repository       | Tag(s)                        | Commit    | Previous |
|------------------|-------------------------------|-----------|----------|
| core             | `2.7.0`                       | `49ab671` | `d8878a0` (2.6.0) |
| crypto           | `2.7.0`                       | `462fdf5` | `670ec17` (2.6.0) |
| audio            | `2.7.0`                       | `3c6bb98` | `5b4202f` (2.6.0) |
| avb              | `2.7.0`, `wireshark-v2.7.0`   | `95ee30b` | `8d2d9cf` (2.6.0) |
| atdecc-control   | `2.7.0`                       | `ccc80a9` | `5f4dfd3` (2.6.0) |
| umbrella         | `2.7.0`                       | `b7894dd` | `97c2602` (2.6.0) |

atdecc-control carries only the version bump. The mains and the tags are
on the staging remote and on all three public mirrors (opensource,
Codeberg, GitHub), verified ref-for-ref after the push.

## What changed

Each item names the finding, the clause where one applies, and the commit
on main. Every commit carries the test that pins it.

### core (2.6.0 to 2.7.0, 14 commits including the version bump)

High:

- **A timer thread that left on its own was never joined** (`72c6b0d`).
  `TimerBase::stop()` only joined when `running_` was still set, so a
  thread that exited through `prepare_thread()` returning false, a failed
  `wait_until_ready()` or a tripwire exception was left joinable and
  `std::thread`'s destructor called `std::terminate`. `stop()` now joins
  whenever the thread is joinable, and the base destructor calls `stop()`.

Medium, `net`:

- **PACKET_MMAP RX read `tp_len`, not `tp_snaplen`** (`a5a234f`). A frame
  longer than the slot produced a span past the slot.
- **The mmap ring handshake had no ordering** (`82ad8e1`). `tp_status` is
  loaded with acquire and stored with release through `std::atomic_ref`;
  the `__sync_synchronize` fence is gone.
- **The mmap RX stamp was CLOCK_REALTIME** (`442d4b5`) against the
  `MONOTONIC_RAW` contract of the RX slot. The slot is now stamped with
  the told RX time, as the XDP backend already did; the contract comments
  in `net_ethernet_port.hpp` and `net_any_ethernet_port.hpp` say so.
- **mmap `tx_commit()` did not bound the frame** (`349c608`). An
  over-MTU frame got EMSGSIZE, the slot stayed `TP_STATUS_WRONG_FORMAT` at
  the head of the ring and nothing after it was ever sent. `open()` reads
  the interface MTU; `tx_start()` hands out `max_tx_frame_length()` bytes;
  a longer `tx_commit()` fails with `MmapError::frame_too_long` and gives
  the slot back the `tx_cancel()` way. `mmap_max_tx_frame_length(mtu)` is
  constexpr and tested on every platform.
- **A receiving rawnet socket was not bound to its interface at `open()`**
  (`8d4a640`); only `join_multicast()` bound it, so a port that never
  joined heard that ethertype from every NIC. Transmit-only sockets stay
  unbound.
- **XDP ring paths on a closed context dereferenced null libxdp rings**
  (`fc68457`). `rx_start`, `tx_commit`, `tx_complete` and `end_cycle`
  answer `XdpError::not_open` first, as the other backends do.

Medium, other:

- **The duration tripwire subtracted across clock domains** (`edf1a7b`).
  It took `read_monotonic_ns()` minus a schedule-clock value, so for a
  gPTP-scheduled timer it could never fire, and it read a clock on the hot
  path for a value the timer already had. `TimerEvent` gains `wake_raw`,
  the RAW instant the timer read as it entered the callback; the bridge
  measures from that on RAW with one RAW read at the end.
- **Triple-buffer exchanges were release-only and acquire-only**
  (`5aa2c95`); there was no happens-before between the consumer's read of
  a slot and the publisher's next write into it. Both are `acq_rel`, as
  the LatestWins pipe already was.
- **pcapng ignored `if_tsresol`** (`b093510`); a nanosecond capture read
  as microseconds. IDB options are walked, decimal and binary resolutions
  decoded into `InterfaceInfo::ts_resol`, and the tick-to-microsecond
  conversion no longer overflows on epoch-scale nanosecond counts.
- **A backpressured HTTP connection spun the level-triggered reactor**
  (`3b54be2`). `TcpConnectionPool::set_read_interest()` turns reads off
  while a response is pending and back on for the next request; a held
  async request with bytes queued behind it costs one poll, not a busy
  loop.

Build:

- `statusbar_package_install_export` takes the header root as
  `INCLUDE_DIR` (`0b6fac8`, `8c607ae`), so a dependent tree whose headers
  are not under `statusbar/` installs them from where they are.

### crypto (2.6.0 to 2.7.0, 2 commits including the version bump)

- **ECDSA verify never rejected r or s at or above n** (`b365f40`). Both
  P-256 backends checked the range by a to-bytes/from-bytes round trip,
  and neither scalar decoder reduces, so the check never failed: s + n
  verified like s, about one signature in 2^32 of the time, and r or s
  equal to n passed. Both backends now use the constant-time compare
  against the group order that `p256_keypair_from_scalar` used, and that
  function shares the helper. The test takes a key built so that (G.x, 1)
  signs "sample", checks s = 1 passes and s = 1 + n, s = n, r = n fail.
  (n − s is the standard mirror signature and remains valid.)

### audio (2.6.0 to 2.7.0, 7 commits including the version bump)

High:

- **The Linux restart published the old holder's null** (`a57691d`).
  Assigning a new `CallbackHolder` over the old one ran the new
  constructor's publish before the old destructor's clear, so the slot
  ended null and the device went silent. The holder now lives in
  `audio_callback_holder.hpp`; `replace_callback_holder()` resets the old
  one before constructing the new one.

Medium:

- **A 29.97 fps LTC frame was 1602 samples, not 1601.6** (`2232afc`).
  The generator carries the fractional remainder frame to frame (frames
  alternate 1601 and 1602 at 48 kHz), `samples_for_frames(n)` gives the
  exact total for a WAV header, and `reset()` clears the carry. 30 and 25
  fps are unchanged.
- **An SMF data byte with the top bit set indexed a 128-entry array**
  (`0d96ceb`). The reader rejects it with `MidiError::unexpected_byte` on
  both the status and running-status paths; `MidiMatrix` ignores notes
  at or above 128.
- **The LTC servo callback called `localtime_r()` and allocated**
  (`3b63dbc`). The UTC offset is read on the control thread and refreshed
  each second; local time is arithmetic in the callback; the sample
  buffer is prepared to the period (at least four periods) and a longer
  period renders silence past capacity and counts `short_periods`.
- **A stale producer could write a drained presentation-queue slot**
  (`b006639`). A producer that loaded `center` before the consumer
  advanced it could CAS a drained slot and write a sample that was read
  one window later; it could also release a slot the consumer had skipped
  as busy. Each slot now carries the presentation time it holds; the
  producer re-checks `center` after acquiring and gives a slot back
  clean; the consumer treats a time mismatch as never written, drains it
  and counts `ReadResult::frames_stale`; a producer with a later time
  takes over a stale full slot, an earlier one never displaces a real
  sample.
- **A Darwin `stop()` from another thread left the AudioUnit running**
  (`8733241`). `stop()` detected the render thread by `in_callback > 0`,
  which is also true when another thread stops during a callback.
  `RenderThreadMarker` records the render thread's id from the CoreAudio
  trampolines, and `stop()` defers teardown only on that thread.

### avb (2.6.0 to 2.7.0, 26 commits including the version bump)

High:

- **The milan13 requester outlived its command** (`df3a5eb`). Under the
  Milan 1.3 profile the controller that issued a SET is left out of the
  unsolicited fan-out (Milan v1.3 5.4.5.3), but `requester_` was never
  cleared, so the next unsolicited notification from a local change or a
  lock expiry skipped a controller that had nothing to do with it.
  The requester is now scoped to one dispatch and one deferred answer,
  and the unchanged-SET check that re-enters `handle_command` for its GET
  twin finds the outer requester restored.
- **Talker Advertise and ACMP ignored the configured VLAN** (`464ee94`).
  ACMP streams were configured with the Class A default VID, the Talker
  Advertise carried fixed VLAN and priority, and a Domain adoption under
  milan13 moved the frames but not the declaration. `configure_stream()`
  takes the configured VID, `make_talker_srp_info()` reads VLAN and
  priority from the MSRP domain in force, and a Domain adoption
  reconfigures each ACMP stream and re-declares. Pinned by the new
  `avb_entity_kit_test` over the tone-generator entity blob.
- **The MVRP LeaveAll never reached the wire** (`57025d7`). The LeaveAll
  timer set `tx_leaveall_pending` and nothing read it; peers never saw a
  LeaveAll from an MVRP participant. The encoder sets the bit on the
  first vector and sends a LeaveAll-only vector when nothing is declared,
  as the MSRP participant already did.

Medium, ATDECC (IEEE Std 1722.1-2021):

- **DISCONNECT_RX kept the sink until the talker answered** (`e4760e1`;
  8.1.2, Figure 8-3). The listener now resets the sink when it sends
  DISCONNECT_TX, retries that command once as it does CONNECT_TX, and
  relays the talker's status. A dead talker no longer leaves a sink that
  cannot be freed over ACMP.
- **The AEM controller never retried a timed-out command** (`333a093`;
  9.3.6, Figure 9-4). One retry with the same sequence_id, then
  `TimedOut`. A lost frame now reports after two timeouts rather than one.
- **Responses matched inflight commands by sequence_id alone**
  (`7bc2fcd`). `find_inflight(AemDu const&)` matches sequence_id, target
  entity and command code; the simple controller no longer caches another
  entity's bytes under the wrong entity.
- **The entity-detail builder counted any READ_DESCRIPTOR** (`4aa413d`);
  it now completes on the set of descriptors it asked for.
- **The descriptor-count cap was per type only** (`1a6d237`); a
  CONFIGURATION can enqueue at most 100000 reads.
- **GET_STREAM_FORMAT answers were keyed on the echoed type and index**
  (`ca95ac7`), which entities zero on error; keyed on the request now, so
  an error clears the query instead of re-sending it every second.
- **connect and disconnect succeeded on any SUCCESS naming the sink**
  (`e519161`), including the talker's TX leg and other controllers'
  transactions. A new `AcmpCommandCompletedEvent` reports our own
  command's response or timeout; the ctl tool confirms on our RX response
  only.
- **The enumerate walk's accounting was asymmetric** (`9004a72`). The
  enumerator is its own component behind `EnumeratorHooks`, issues every
  READ_DESCRIPTOR itself, and counts out one slot per response or timeout
  it counted in.

Medium, AVTP (IEEE Std 1722-2025):

- **A MAAP defender always yielded** (`e2b1ff8`; Table B.7, B.3.6.4). A
  conflicting DEFEND or ANNOUNCE in DEFEND state now runs compare_MAC
  first; the lower MAC keeps the range. The old test pinned the
  non-conforming behaviour and was replaced.
- **AM824 classification was subtype-only** (`aaa36dd`). The tools
  require `Am824Pdu::is_valid()`; 61883-4 and SPH streams are not decoded
  as audio.
- **AM824 and AAF decode took the frame tail, not `stream_data_length`**
  (`f12fdc3`) in the decoder, the retransmit tool and the production
  listener, so minimum-size Ethernet padding decoded as silent samples and
  DBC and presentation time drifted. `am824_get_audio_payload(pdu, frame)`
  and `aaf_get_audio_payload(pdu, frame)` bound all three.
- **Packets above the template default were dropped before the header**
  (`e78d584`); a 192 kHz class B AM824 stream decoded nothing and the
  sample count was eight bits. The bound is the scratch capacity,
  `Am824Pdu::sample_count()` is 16-bit, the retransmit tool clamps copies
  to its buffers.
- **The retransmit tool skipped 802.1Q-tagged frames and opened only
  pcapng** (`b9a845e`). `avtp_capture_frame.hpp` strips the tag and
  sniffs the capture format; classic pcap opens as the help promised.
- **No ESCF verify applied the time window or any replay state**
  (`6ecbd77`). `escf_verify_frame_at(frame, key, now_ptp_ns, guard)`
  applies the ±100 ms window and a fixed-capacity per-signer replay guard
  before the signature check and returns an `EscfVerdict`; the raw verify
  remains for callers that gate themselves.

Medium, entity and nanoavb:

- **An ACMP controller timeout named the wrong command** (`3958a98`);
  `SlotTable::remove()` swaps the last entry into the index. The command
  is copied before `step(Timeout)`.
- **The RX thread read the ACMP listener's record with no hand-off**
  (`aef37c5`). The reactor publishes the connected stream id through
  `itc::Published`; the drain matches frames on that alone.
- **A deferred command took a second slot on retransmit and had no
  deadline** (`01725bf`). A retransmit (same controller, sequence_id and
  command) is answered IN_PROGRESS from its slot; a slot nobody completes
  is answered NO_RESOURCES with the value in force after 60 s.
- **A contested ACQUIRE during a CONTROLLER_AVAILABLE handshake
  overwrote the pending request** (`cb8884b`; 7.4.1). The handshake stays
  its first contender's: the same contender gets IN_PROGRESS, another gets
  ENTITY_ACQUIRED naming the owner, and the owner releasing hands the
  entity to the waiting contender.

Medium, gPTP, SRP and udptun:

- **A zero RX timestamp was a time of zero** (`671ad56`). A Sync,
  Pdelay_Req or Pdelay_Resp without a hardware stamp is counted
  (`rx_timestamps_missing()`) and dropped; the servo and the pdelay state
  never see it and receipt timeouts treat it as a lost message.
- **The time bridge used the servo's master/PHC ratio as its
  gPTP-to-RAW slope** (`3677ce9`), so conversions sawtoothed by a few
  microseconds per Sync interval. The bridge fits its own rate over a
  128-anchor window after a 1 s baseline and restarts the window on a
  100 ms step.
- **An MSRP message skipped for space lost its Leave** (`40e145d`;
  802.1Q 10.7.7, Table 10-3). tx! is dispatched as each message is built;
  a message that does not fit rolls its Applicants back; a skip repeats
  the pass with the optional JoinIn and In encodings shed, and restarts
  the join timer if still skipped.
- **The udptun reporter scanned RT structures live** (`ef1ddb5`). The
  packet thread publishes a `LossSnapshot` through a triple buffer at
  20 Hz and the report reads that.

## Downstream: what a tree built on 2.6.0 must change

Nothing on the wire. API changes, all additive or in signatures with no
external caller:

- core `TimerEvent` gained a trailing `wake_raw` and `wake_raw_ns()`;
  designated initialisers still compile; rebuild dependents.
- core `MmapError::frame_too_long`, `MmapContext::max_tx_frame_length()`,
  `mmap_max_tx_frame_length(mtu)`; a rawnet receiving socket is bound at
  `open()`; XDP ring paths return `not_open` on a closed context.
- core `TcpConnectionPool::set_read_interest()` / `read_interest()`;
  pcapng `InterfaceInfo::ts_resol`.
- audio `AudioCallbackFn`, `AudioCallbackRT`, `CallbackHolder` and
  `replace_callback_holder()` live in `audio_callback_holder.hpp`;
  `PresentationQueue::try_acquire_for_write(idx, time)` (protected) and
  `ReadResult::frames_stale`; `LtcGenerator::samples_for_frames()`;
  `MidiError::unexpected_byte`; `RenderThreadMarker` in
  `audio_render_thread.hpp`.
- avb `NanoAvbAcmpTalker::configure_stream(..., vlan_id)`;
  `AvbEntityKit::make_talker_srp_info()` is public;
  `GptpTimeBridge::update_from_anchor(sync_rx_app_ns, tx_master_ns)` lost
  its `rate_ratio` parameter; `GptpSlavePort::rx_timestamps_missing()`;
  `Am824Pdu::sample_count()` is `uint16_t`; `am824_get_audio_payload` and
  `aaf_get_audio_payload` overloads on (pdu, frame);
  `escf_verify_frame_at`, `EscfVerdict`, `EscfReplayGuard`;
  `ControllerEvent` has a new alternative `AcmpCommandCompletedEvent`
  (visitors with a generic lambda or `get_if` are unaffected);
  `AemInflightCommand::retried`; `ControllerSimple::
  MAX_DESCRIPTORS_PER_CONFIGURATION`; `atdecc_enumerator.hpp`;
  `avtp_capture_frame.hpp`; `udptun_loss_snapshot.hpp`.
- Behaviour: the AEM controller reports a lost frame after two timeouts;
  a MAAP defender with the lower MAC no longer yields; a Talker Advertise
  carries the configured or adopted VLAN and priority; a deferred AEM
  command expires after 60 s.

## Not changed

- **AM824 SYT.** The talker writes a computed SYT when `tv` is set where
  IEEE 1722-2025 5.4.4.4 has the talker set FFFF and the listener ignore
  it. Our listener ignores it; the only question is receivers that want a
  SYT for clock recovery, which the code comment says exist. Left as is
  pending an interoperability decision.
- The ~60 low findings from the same review.
- Noted during the fixes, not acted on: the macOS BPF reader hands on the
  kernel's wall-clock `bh_tstamp` as the old mmap path did; the MVRP
  participant shares the dispatch-then-build shape that MSRP lost; a
  waiting ACQUIRE contender is not sent IN_PROGRESS at 120 ms (9.3.2.6);
  a DISCONNECT_RX for a sink that is not connected still answers SUCCESS
  where Figure 8-3 says NOT_CONNECTED.

## Build, test and verify

- Each fix landed through the CI branch flow (`ci/<name>` built and
  tested on Linux arm64 by the cron, cherry-picked onto main, result tag
  per branch): one branch for the five highs, eight branches for the
  mediums (one per review area), then the five version bumps. Every run
  passed.
- Before push, each branch was formatted with the pinned clang-format,
  gated by the block-statement plugin (one NOLINT in the round, on a flat
  field copy), scrubbed, built and tested on macOS arm64 against Homebrew
  LLVM 23.1.5. The two core branches and the four avb branches were also
  merged into scratch trees and tested together (core 86, avb 175 tests,
  avb built against the merged core), and the whole set was compiled for
  Linux arm64 in podman including the Linux-only net, udptun and audio
  files.
- Suites at the tags (macOS counts): core 86, crypto 44, audio 21,
  avb 175, atdecc-control 35.
- Packages and fleet: eleven arm64 Debian packages (six runtime, five dev)
  cross-built with podman as 2.7.0-20261113051036 and installed on all five
  Raspberry Pi nodes (jdk01a, jdk01b, jdk01c, jdk01d, jdk01f); the
  services on jdk01c and the tone generators were restarted and verified
  active.

## Where the detail is

- The review itself, every finding with file and line, the suggested
  order, and what was not covered, is kept with the umbrella's release
  records (`CODE_REVIEW_2026-11-12.md`).
- `core/docs/TIME_POLICY.md`: the told-the-time rule the tripwire,
  mmap-stamp and gPTP zero-stamp fixes apply.
- `avb/docs/IEEE_1722_CRYPTO_AMENDMENT.md`: the ESCF verify entry noted
  on the A3 row.
- `RELEASE_2_5_0.md` and `RELEASE_2_4_0.md` (this folder): the previous
  releases.
