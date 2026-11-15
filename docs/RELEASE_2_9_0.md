<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->
<!-- SPDX-License-Identifier: MIT -->

# statusbar 2.9.0

Released 2026-11-14. What the release contains, what changed on the wire
and in the public APIs, how it was built and verified, and where the
per-repository detail is.

2.9.0 is the MAAP conformance and memory-locking release. The entity
kit's MAAP acquisition now follows IEEE 1722-2025 Annex B as written:
DEFENDs go to the prober with the right fields, every MAAP PDU carries a
zero stream_id, the ANNOUNCE follows the last PROBE in the same tick, a
stop releases the range, a Link Up re-probes the range held, and the
address sequence is seeded from the clock as well as the MAC. core's
real-time timers no longer lock the whole process unless asked, and a
new `MappedFile` gives a real-time reader a mapped file with page-range
locking; audio gains a reader for RIFF WAVE, RF64 and BW64 files to go
with its writer. The avb tools answer `--help` throughout, the tools
index matches the package, and `statusbar-bpf-dump` is in the package.
crypto and atdecc-control carry only the version bump.

Things that change what goes on the wire: a MAAP DEFEND is unicast to
the PROBE's source with the PROBE's range in the requested fields and
our overlap in the conflict fields; stream_id is zero in every MAAP
PDU; the ANNOUNCE is sent in the tick of the final PROBE; a stopped kit
sends no MAAP frames; a Link Up starts a probe sequence on the held
range (TX gated while it runs). All of these are the standard as
written.

## The tags

| Repository       | Tag(s)                        | Commit    | Previous |
|------------------|-------------------------------|-----------|----------|
| core             | `2.9.0`                       | `7d833e2` | `f55793a` (2.8.0) |
| crypto           | `2.9.0`                       | `5ecc449` | `2f036ea` (2.8.0) |
| audio            | `2.9.0`                       | `0a64cef` | `0416ad4` (2.8.0) |
| avb              | `2.9.0`, `wireshark-v2.9.0`   | `8d6ef55` | `24115fc` (2.8.0) |
| atdecc-control   | `2.9.0`                       | `8a829ab` | `f8c0e24` (2.8.0) |
| umbrella         | `2.9.0`                       | `75769d9` | `cda19a2` (2.8.0) |

crypto and atdecc-control carry only the version bump. The mains and the
tags are on the staging remote and on all three public mirrors
(opensource, Codeberg, GitHub), verified ref-for-ref after the push.

## What changed

Each item names the clause where one applies and the commit on main.
Every commit carries the tests that pin it.

### core (2.8.0 to 2.9.0, 3 commits including the version bump)

- **`MappedFile`** (`ba856d5`; `mem_mapped_file.hpp`, new
  `docs/MEM_MODULE.md`). A read-only, move-only mapping of a whole
  regular file (`map(fd)` or `open(dir_fd, path)`; PROT_READ,
  MAP_SHARED) with `lock_range()` / `unlock_range()` to keep a window
  resident while a real-time thread reads inside it, `advise()` for
  posix_madvise and `page_span()` as the page arithmetic the three
  share. A zero-length file is a valid empty mapping; every error is the
  errno as `Status`. The mem module gains its first source file and a
  dependency on status.
- **`TimerConfig::lock_memory`** (`021a3ef`; `realtime_timer_config.hpp`,
  `REALTIME_MODULE.md`). `prepare_thread()` called `mlockall(MCL_CURRENT)`
  on every timer start, locking every mapping of the moment, a large
  mapped file included, and overriding the process's own page-range
  locking. `lock_memory` is true by default so existing timers are
  unchanged; false leaves locking to the process. `build_timer_arg_specs()`
  exposes it as `--<timer>.lock_memory` and the TOML key. The CPU
  preparation and affinity move out of `prepare_thread()` into
  `prepare_cpu()` on the way. Pinned through VmLck on Linux.

### crypto (2.8.0 to 2.9.0, 1 commit)

- Version bump only.

### audio (2.8.0 to 2.9.0, 2 commits including the version bump)

- **`parse_bw64()`** (`e6ddfe4`; `audio_bw64_reader.hpp`; EBU Tech 3285,
  EBU Tech 3306, ITU-R BS.2088). Reads a RIFF WAVE, RF64 or BW64 file held
  in a byte span, a mapped file typically, without copying or allocating:
  the data chunk's offset, declared and available size and whole frames;
  the fmt chunk, WAVE_FORMAT_EXTENSIBLE resolved through its SubFormat,
  valid bits and channel mask, as one of the layouts the package converts
  (16, 24 and 32-bit PCM, 32-bit float); bext field by field with its
  64-bit TimeReference; chna, axml and iXML as raw bodies; sizes of
  0xFFFFFFFF read from ds64. A data chunk cut short by the end of the span
  reads as far as it goes with `complete()` false, so a file still being
  written is readable. A structure that is not a WAVE's is the new
  `AudioError::MalformedFile`; 8-bit PCM, 64-bit float, A-law, mu-law,
  compressed tags and SubFormats outside the KSDATAFORMAT family are
  `AudioError::UnsupportedFormat`. `bw64_channel_label()` names a channel
  from the EXTENSIBLE mask. A fuzzer feeds the reader arbitrary bytes.

### avb (2.8.0 to 2.9.0, 12 commits including the version bump)

MAAP, IEEE 1722-2025 Annex B:

- **DEFENDs go to the prober with its range and the overlap, and every
  PDU carries a zero stream_id** (`8ce4217`; B.2.1, B.2.4 to B.2.8,
  B.3.6.3, B.3.6.6). The DEFEND went to the MAAP multicast address with
  our range in the requested fields and the PROBE's in the conflict
  fields; it is now unicast to the PROBE's source MAC, echoes the PROBE's
  requested_start_address and requested_count, and names the first of our
  addresses the PROBE covers and how many (`conflict_range()`). The
  handler's send callback takes the destination (`SendFn(dest, bytes)`).
  Every PDU carried the first talker stream's id; the handler no longer
  takes a stream id and writes zero. The ANNOUNCE came one probe interval
  after the last PROBE; the tick whose decrement empties probeCount now
  raises ProbeCount! and sends the ANNOUNCE in the same tick.
- **MAAP stops with the kit, re-probes on Link Up, and seeds from the
  clock** (`98f941a`; Table B.7, B.3.5.2 Release!, B.3.5.9
  PortOperational!, B.3.6.1). A stop never released the range and left
  the reactor port standing with a reference to a handler the next start
  destroyed (PROBEs and ANNOUNCEs after the stop; a start after a stop was
  a use-after-free with a second socket). `MaapNetHandler` now owns the
  handler (`unique_ptr`), `retire()` releases the range and the reactor
  drops the port as finished; the kit's `stop()` does this. PortOperational!
  was never raised: `MaapHandler::port_operational()` runs Table B.7, from
  INITIAL beginning acquisition and from PROBE or DEFEND restarting the
  probe sequence on the range held (note a), so a range nobody took is
  defended again and one that was taken is found out. The kit raises it on
  every Link Up; the first Link Up begins the acquisition, as for ADP and
  the MSRP Domain, and the TX gate is closed while re-probing. The seed is
  the sum of the MAC and the local real-time clock read once at set-up
  (`maap_seed()`), so a station no longer draws the same ranges after
  every boot.

owlm and udptun:

- **`--ptp.no-mlock` reaches the WAN timer** (`e47124c`). Under
  `--time-source=ptp4l` the PTP setup owns memory locking; the session's
  realtime timer locked the process again on its own, undoing the flag.
  The timer's `lock_memory` now follows the ptp4l config's under that
  source (`wan_timer_locks_memory()`); the gPTP and realtime sources have
  no such step, so `--wan_timer.lock_memory` stands there. The session
  header includes the realtime headers it names on every platform.

Tools and documentation:

- **The tools index matches the package** (`8c44def`; `docs/TOOLS.md`).
  Every installed binary listed under its installed `statusbar-*` name,
  the Python helpers included; a new section on the systemd units and
  conffiles, packaged models, the `/dev/ptp*` udev rule, the postinst
  capabilities and the `-dev` package. OWLM.md and UDPTUN.md use the
  installed names and example.com hosts; the units' `Documentation=`
  lines point at the conffile each loads; `docs/sm/` regenerated.
- **Tools name themselves as installed** (`3ae0a14`).
  `statusbar-avtp-crypto-tool` prints its installed name; the stun
  client's `--server` example is an example.com name.
- **`statusbar-bpf-dump` is built wherever core's bpf module is**
  (`629839b`). The tool was gated on core's own configure option and
  never reached a package; the gate is now the `statusbar::bpf` target.
- **`--help` on every tool** (`a8eb334`, `805ca78`, `7a4d777`).
  `statusbar-descriptor-storage`, `statusbar-gptp-ntpshm` and
  `statusbar-gps-ratio-tracker` print usage for `--help`/`-h` and exit 0
  instead of treating it as a file, a segment or a PTP device;
  `avtp-to-wav`, `msrp-functional-test` and `rttest-send-avtp` open their
  help with what they do and example invocations.
- **The stun tools read their config under their installed names**
  (`57e78a4`). `~/.config/statusbar-stun-client-tool` and
  `~/.config/statusbar-stun-server-tool` replace the CMake target names;
  a configuration under the old directory is no longer read and should
  be moved.
- **The 2.7.0 and 2.8.0 release notes** join `avb/docs/` (`c581c72`).

### atdecc-control (2.8.0 to 2.9.0, 1 commit)

- Version bump only.

## Downstream: what a tree built on 2.8.0 must change

No new wire format. API changes:

- core, added: `mem::MappedFile` (`mem_mapped_file.hpp`; the mem module
  now links status); `realtime::TimerConfig::lock_memory` and its
  `--<timer>.lock_memory` flag / TOML key; `TimerBase::prepare_cpu()`.
  A process that maps files and locks ranges of its own should set
  `lock_memory = false` on its timers.
- audio, added: `parse_bw64()`, `Bw64Info`, `Bw64Format`, `Bw64Bext`,
  `bw64_channel_label()` (`audio_bw64_reader.hpp`, included by
  `audio.hpp`); `AudioError::MalformedFile`.
- avb `avtp::MaapHandler`, changed: `SendFn` is
  `bool(Eui48 const& dest, std::span<uint8_t const>)`; the stream-id
  parameter is gone; `acquire(count, now)` is joined by `prepare(count)`
  and `port_operational(now)`.
- avb `nanoavb::MaapNetHandler`, changed: constructed from
  `unique_ptr<MaapHandler>`, exposes `handler()`, `retire(now)` and
  `finished()`.
- avb `avb_entity::MaapAddressAcquirer`, changed: `setup(...)` replaces
  `acquire(...)`; `port_operational(now)`, `stop(now)`; `maap_seed()` is
  a free function. `AvbEntityKit::setup_maap_addresses()` replaces
  `acquire_maap_addresses()`; `on_link_up()` raises PortOperational!.
- avb udptun: `wan_timer_locks_memory()` in `udptun_session_helpers.hpp`;
  `--wan_timer.lock_memory` documented in OWLM.md.
- avb configuration: the stun client and server read
  `~/.config/statusbar-stun-client-tool` / `statusbar-stun-server-tool`.
- Packaging: `statusbar-bpf-dump` is now installed by the avb package.
- Behaviour: MAAP DEFEND unicast with the standard's fields, zero
  stream_id, ANNOUNCE in the final PROBE's tick, Release! on stop,
  PortOperational! on Link Up with the TX gate closed while re-probing,
  MAC-plus-clock seed.

## Not changed

- **One MAAP range per kit.** The acquirer still asks for one contiguous
  range sized to the talker streams; per-stream ranges are not needed by
  any shipped model.
- **AM824 SYT** and the ~60 low findings carried from 2.7.0.
- The timer's `lock_memory` default stays true; only owlm under ptp4l
  and callers that opt out change behaviour.

## Build, test and verify

- Each change landed through the CI branch flow (`ci/<name>` built and
  tested on Linux arm64 by the cron, cherry-picked onto main, result tag
  per branch): two core branches, one audio branch and eight avb branches
  since 2.8.0, then the five version bumps. Every run passed.
- Before push, each branch was formatted with the pinned clang-format,
  gated by the block-statement plugin, scrubbed, and built and tested on
  macOS arm64 against Homebrew LLVM 23.1.5; the Linux-only paths (VmLck,
  mlock, ptp4l) ran on the CI host.
- Suites at the tags (macOS counts): core 90, crypto 44,
  audio 22, avb 176, atdecc-control 35.
- Packages and fleet: eleven arm64 Debian packages (six runtime, five dev)
  cross-built with podman as 2.9.0-20261115055826 and installed on all five
  Raspberry Pi nodes (jdk01a, jdk01b, jdk01c, jdk01d, jdk01f); the postinst
  restarted the tone generators, the galaxy proxy, Nadia CP proxy and
  widget bridge units on jdk01c were restarted and verified active.

## Where the detail is

- `core/docs/MEM_MODULE.md` and `core/docs/REALTIME_MODULE.md`: the
  mapped file and when a timer should not lock memory.
- `audio/docs/AUDIO_MODULE.md`: the BW64 reader.
- `avb/docs/TOOLS.md`: every installed tool; `avb/docs/OWLM.md`: the WAN
  timer's locking.
- `RELEASE_2_8_0.md`, `RELEASE_2_7_0.md`, `RELEASE_2_5_0.md` and
  `RELEASE_2_4_0.md` (this folder): the previous releases.
