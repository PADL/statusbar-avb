<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->
<!-- SPDX-License-Identifier: MIT -->

# statusbar 2.8.0

Released 2026-11-13 (mirrored 2026-11-14). What the release contains, what changed on the wire
and in the public APIs, how it was built and verified, and where the
per-repository detail is.

2.8.0 is the Milan device release. An entity built from a Milan model now
answers GET_MILAN_INFO, the command a Milan controller uses to tell a
Milan device from any other ATDECC entity, and takes the Milan profile
by default. Beside it, the SRP side of the entity kit follows Milan v1.3
4.2.7.2.1 on startup and Link Up, the SRP participants start from BEGIN
on every run, and two AEM/ACMP answers follow IEEE 1722.1-2021 as
written. core gains the network plumbing an AVB receiver on a multi-queue
NIC needs: a netlink request/reply socket, a netdev generic-netlink
client, ethtool RX flow rules with a guard that puts them back, and a
handful of socket helpers; the XDP launch-time probe is rebuilt on the
new client. crypto, audio and atdecc-control carry only the version bump.

One wire format is new: the AECP vendor-unique message with Avnu's MVU
protocol_id (Milan v1.3 5.4.3), carrying GET_MILAN_INFO. Several things
change what goes on the wire: an unsolicited response is never sent back
to the controller that caused it under either profile, a DISCONNECT_RX
for a sink that is not connected answers NOT_CONNECTED, a Link Up
declares the configured Class A Domain again, a Milan Talker Advertise
carries the exact TSpec of Table 4.4, and a kit started after a stop
registers the VLAN its Domain names. All of these are the standards as
written.

## The tags

| Repository       | Tag(s)                        | Commit    | Previous |
|------------------|-------------------------------|-----------|----------|
| core             | `2.8.0`                       | `f55793a` | `49ab671` (2.7.0) |
| crypto           | `2.8.0`                       | `2f036ea` | `462fdf5` (2.7.0) |
| audio            | `2.8.0`                       | `0416ad4` | `3c6bb98` (2.7.0) |
| avb              | `2.8.0`, `wireshark-v2.8.0`   | `24115fc` | `95ee30b` (2.7.0) |
| atdecc-control   | `2.8.0`                       | `f8c0e24` | `ccc80a9` (2.7.0) |
| umbrella         | `2.8.0`                       | `cda19a2` | `b7894dd` (2.7.0) |

crypto, audio and atdecc-control carry only the version bump. The mains
and the tags are on the staging remote and on all three public mirrors
(opensource, Codeberg, GitHub), verified ref-for-ref after the push.

## What changed

Each item names the clause where one applies and the commit on main.
Every commit carries the tests that pin it.

### core (2.7.0 to 2.8.0, 14 commits including the version bump)

Socket helpers, `net`:

- **`errno_code()` takes a saved errno** (`098874d`). A failure path
  that closes a descriptor or logs before building its error code had
  errno overwritten by then; the caller can now save it first.
- **Unicast egress and priority** (`55c34b8`). `set_unicast_interface()`
  pins the egress interface by index (IP_UNICAST_IF / IP_BOUND_IF),
  `set_unicast_hops()` the TTL / hop limit, `set_unicast_send()` both;
  `set_priority()` sets SO_PRIORITY, the value an mqprio root maps to a
  traffic class, and must follow `set_dscp()` since IP_TOS on Linux
  rewrites it.
- **Source-specific groups** (`e96a444`; RFC 4607, RFC 3376 4.2,
  RFC 3678 5.2). `is_source_specific_group()` names 232/8,
  `join_source_group()` makes the INCLUDE join, and
  `open_source_group_membership()` is the membership an AF_XDP receiver
  holds so the switch keeps forwarding a group the kernel's IP stack
  never sees.

Launch times, `net`:

- **LaunchOrder counts late commits** (`a5bded3`). `launched_late()` and
  `max_late_ns()` count the timed frames whose wanted departure had
  passed when the clock was told; a 0 `now` counts nothing and a gap of a
  second or more is a clock step, not a late frame.
- **`phc_now_ns()`** (`3a65499`) reads a NIC's PHC for the launch times
  the NIC reads in that clock, 0 without a usable clock so the commit
  goes untimed rather than timed against the wrong clock.
- **Busy-polled zero-copy queues are not kicked** (`fd2d2a0`).
  `XdpContext::set_napi_busy_poll()`: once a kick has got through, a
  polling queue drains its own ring and `tx_flush()` skips the syscall;
  a queue that stops making progress with descriptors pending is kicked
  regardless, counted in `tx_flush_stalled`. Copy mode is unchanged.

Netlink and ethtool, `net`:

- **`netlink::Socket`** (`17155f0`; `net_netlink.hpp`). One request and
  its replies up to the acknowledgement, with extended-ack reasons,
  retransmission on truncation, and framing readers that never read past
  what a message holds. Tested against hand-laid byte vectors, against
  RTM_GETLINK on Linux, and fuzzed.
- **A netdev family client** (`d46b2cc`; `net_netdev_features.hpp`).
  `NetdevClient::features(ifindex)` asks the kernel's netdev
  generic-netlink family for a device's xdp, rx-metadata and xsk
  features; `launch_time_supported()` is the one-shot form, true where
  NETDEV_XSK_FLAGS_TX_LAUNCH_TIME_FIFO (Linux 6.15) is set. Tested
  against the kernel's reply layouts and fuzzed.
- **`ethtool_ioctl()`** (`21a626e`) is the one SIOCETHTOOL helper the RSS
  table and the flow rules share.
- **RX flow rules** (`d8ab1ed`; `net_rx_flow.hpp`). ethtool's RX
  classifier wrapped for the two matches an AVB receiver wants, a whole
  EtherType or the VLAN priority bits, each steered to one queue;
  `RxFlowPlan` and `check_rx_flow_plan()` say what a steerer owns;
  `RxFlowRecorder` is the shipped test double; `rx_queue_count()` is
  public.
- **`RxFlowSteering`** (`29e9cda`) installs a plan's rules, spreads the
  RSS table over the queues the rules leave alone, and puts both back
  from its destructor; a refusal part way undoes what got in; ntuple
  filters are turned on when needed and never off again (igb and igc
  reset the adapter for it).
- **The XDP launch-time probe asks the netdev client** (`74aab8a`). Some
  two hundred lines of hand-rolled generic netlink inside the XDP
  context are gone; the probe keeps its contract (no netdev family is
  the launch-time refusal, any other failure is the kernel's own).

Build:

- **No C++ module scanning** (`3056025`). Both toolchain files set
  `CMAKE_CXX_SCAN_FOR_MODULES OFF`: nothing in the tree is a module and
  nothing imports std, and CMake 4.5 otherwise builds an `import std`
  target ahead of each try_compile check on a libc++ whose modules.json
  it can read, failing the configure where that libc++'s std.compat does
  not build.

### crypto (2.7.0 to 2.8.0, 1 commit)

- Version bump only.

### audio (2.7.0 to 2.8.0, 1 commit)

- Version bump only.

### avb (2.7.0 to 2.8.0, 10 commits including the version bump)

ATDECC, Milan:

- **Milan vendor-unique messages** (`c297a04`; Milan v1.3 5.4.3, 5.4.4.1;
  `atdecc_aecp_mvu.hpp`). `AecpMvuDu` is the 30-octet header of Figure
  5.4 with Avnu's protocol_id and the u/command_type word;
  `MvuGetMilanInfoResponse` is Figure 5.6; the command types of Table
  5.15, the status codes of Table 5.16, the features flags of Table 5.17
  and the dotted version words are named. The AEM handler takes a
  `MilanInfo`; with one set, GET_MILAN_INFO is answered with
  protocol_version 1, every other MVU command NOT_IMPLEMENTED with its
  data given back, and a short GET_MILAN_INFO PAYLOAD_TOO_SHORT. Without
  one the entity is not a Milan device and lets every MVU command pass
  unanswered, as 5.4.4.1 recommends.
- **A Milan model is a Milan device, and the profile's default**
  (`32c02c6`). `atdecc.profile` now defaults to empty, which resolves to
  `milan13` for a model with `entity.milan` set and `classic` otherwise;
  an explicit value still wins. A Milan model answers GET_MILAN_INFO from
  `milan.features_flags` (none by default), `milan.certification_version`
  (`0.0.0.0`) and `milan.specification_version` (`1.3.0.0`); a version
  that is not dotted numbers falls back to its default and is logged.
- **Milan TSpec frame sizes** (`aaf7d06`; Milan v1.3 4.3.3.2, Table 4.4).
  `milan_srp_max_frame_size()` sizes MaxFrameSize exactly (AVTPDU header
  plus nominal payload); the kit's Talker Advertise uses it under
  `milan13` and keeps the one-sample headroom under `classic`.

ATDECC, IEEE 1722.1-2021:

- **The requester never gets its own unsolicited copy** (`30bff3e`;
  7.5.2). The sentence is unconditional, so both profiles leave the
  requester out, the acquiring controller included when it is the
  requester; it still hears what other controllers do.
- **DISCONNECT_RX on a sink that is not connected** (`06d845c`; Figure
  8-3, 8.2.4.2.3, Table 8-3). Answers NOT_CONNECTED, also for a command
  naming a different talker, with no talker exchange and the live
  connection left alone.

SRP and the entity kit:

- **The shared callback setup installs only the sender** (`56a4521`).
  `MvrpHandler` and `MsrpHandler` gain `set_send_packet()`; the setup
  used `set_callbacks()`, which replaced the kit's `on_domain_adopted`
  hook (Milan v1.3 4.2.7.2.1) once the control plane came up, so an
  adopted Class A Domain changed the MSRP declaration while the frames,
  ACMP, MVRP and GET_AVB_INFO stayed on the configured VLAN.
- **start registers the VLAN in force** (`d4e09ca`). After a stop and
  start MVRP registered the configured VLAN while MSRP declared the
  adopted one; `start()` now registers `current_vlan_id()`.
- **Link Up and start return to the configured Domain** (`a62c794`;
  Milan v1.3 4.2.7.2.1). `MsrpHandler::return_to_configured_domain()`
  forgets the peers' registered Domains and declares the configured one
  again, the adopted value leaving first (802.1Q 10.7); the kit follows
  the hook (`follow_domain()`) back to the configured VLAN and priority
  everywhere, and the host's Link Up path declares it at once,
  independent of gPTP state.
- **The SRP participants BEGIN again on start** (`2bae0e6`; IEEE
  802.1Q-2022 10.7.5.1). `withdraw_all()` ends the run and the next use
  starts the participant again; `PortState::start()` resets both FSMs
  before the UCT chain, so a stopped-then-started port originates its
  LeaveAll. The attribute database is kept across the two.

### atdecc-control (2.7.0 to 2.8.0, 1 commit)

- Version bump only.

## Downstream: what a tree built on 2.7.0 must change

One new wire format (MVU GET_MILAN_INFO), answered only by an entity
given a `MilanInfo`. API changes, all additive except where noted:

- core `errno_code(int err = errno)`; `set_unicast_interface()`,
  `set_unicast_hops()`, `set_unicast_send()`, `set_priority()`;
  `is_source_specific_group()`, `join_source_group()`,
  `open_source_group_membership()`; `LaunchOrder::launched_late()` /
  `max_late_ns()`; `phc_now_ns()`; `XdpContext::set_napi_busy_poll()`
  and the `tx_flush_stalled` counter; new headers `net_netlink.hpp`,
  `net_netdev_features.hpp`, `net_rx_flow.hpp`,
  `net_rx_flow_recorder.hpp`; `ethtool_ioctl()` in `net_posix_util.hpp`;
  `rx_queue_count()` public in `net_rss`.
- core, removed: the `detail::genl_family_id_in()` and
  `detail::xsk_features_in()` parsers in `net_linux_xdp.hpp` (use
  `netdev::parse_family_id()` / `parse_dev()`).
- core build: a tree that configures with the shared toolchain files
  gets `CMAKE_CXX_SCAN_FOR_MODULES OFF`; a tree that adopts modules must
  remove the line.
- avb `atdecc_aecp_mvu.hpp` (included by `atdecc.hpp`);
  `MilanInfo`, `set_milan_info()`, `milan_info()` on the nanoavb entity;
  `MvrpHandler::set_send_packet()`, `MsrpHandler::set_send_packet()`,
  `MsrpHandler::return_to_configured_domain()`, both handlers'
  `started()`; `AvbEntityKit::milan_profile()`;
  `milan_srp_max_frame_size()`; the kit's `adopt_domain` hook follower is
  `follow_domain()`.
- avb configuration: `atdecc.profile` defaults to empty (the model
  decides); new keys `entity.milan`, `milan.features_flags`,
  `milan.certification_version`, `milan.specification_version`. A
  deployment that wants a Milan device sets `entity.milan = true`; a
  configuration that set `atdecc.profile = milan13` explicitly keeps the
  profile but is not a Milan device until it does.
- Behaviour: no unsolicited copy to the requester under `classic`;
  DISCONNECT_RX NOT_CONNECTED; a Link Up returns to the configured
  Class A Domain; SRP participants restart from BEGIN on every run.

## Not changed

- **One adoption per Link Up.** Milan v1.3 4.2.7.2.1 has a PAAD adopt
  every differing Class A Domain it hears, and a compliant peer returns
  to priority 3 / VLAN 2 on Link Up itself, so no guard against two
  adopting stations handing a Domain back and forth is added.
- **Optional MVU commands.** SET/GET_SYSTEM_UNIQUE_ID, the media-clock
  reference info commands and the binding commands (Milan v1.3 5.4.4.2
  onward) answer NOT_IMPLEMENTED; `milan.features_flags` is 0 by default
  because none of Table 5.17 is implemented.
- **AM824 SYT** and the ~60 low findings carried from 2.7.0.
- Noted, not acted on: the Lua dissector's SET_SYSTEM_UNIQUE_ID payload
  renders the id only, where Milan v1.3 Figure 5.7 also carries a
  64-octet name.

## Build, test and verify

- Each change landed through the CI branch flow (`ci/<name>` built and
  tested on Linux arm64 by the cron, cherry-picked onto main, result tag
  per branch): three core branches and two avb branches for the work
  since 2.7.0, one core and one avb branch for the Milan device and
  probe work, then the five version bumps. Every run passed.
- Before push, each branch was formatted with the pinned clang-format,
  gated by the block-statement plugin, scrubbed, built and tested on
  macOS arm64 against Homebrew LLVM 23.1.5, and the core work compiled
  for Linux arm64 in podman (Debian trixie, clang 19, -Werror) including
  the Linux-only net files.
- The module-scan line was verified under CMake 4.4 and by reading the
  4.5 sources; CMake 4.5 itself was not run locally.
- Suites at the tags (macOS counts): core 89, crypto 44, audio 21,
  avb 176, atdecc-control 35.
- Packages and fleet: eleven arm64 Debian packages (six runtime, five dev)
  cross-built with podman as 2.8.0-20261114064316 and installed on all five
  Raspberry Pi nodes (jdk01a, jdk01b, jdk01c, jdk01d, jdk01f); the
  services on jdk01c and the tone generators were restarted and verified
  active.

## Where the detail is

- `avb/docs/ENTITY_CONSTRUCTION_KIT.md`: the profile default rule and
  the Milan device paragraph.
- `core/docs/NET_MODULE.md`: the netlink and netdev headers and
  when to ask them.
- `RELEASE_2_7_0.md`, `RELEASE_2_5_0.md` and `RELEASE_2_4_0.md` (this
  folder): the previous releases.
