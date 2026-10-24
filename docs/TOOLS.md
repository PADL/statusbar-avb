<!-- SPDX-License-Identifier: MIT -->
# AVB command-line tools

Every program the `statusbar-avb` package installs, grouped by purpose, plus
the service units, configuration files and data the package puts beside them.
This is a reference index: each tool also documents itself.

- `--help` prints usage and the full option list (generated from the tool's
  argument specs, so it never drifts). Four tools take their arguments
  another way and are marked below; each still answers `--help`:
  `statusbar-descriptor-storage` and `statusbar-gptp-ntpshm` take one
  positional argument, `statusbar-gps-ratio-tracker` space-separated
  `--flag value` pairs, and `statusbar-avtp-crypto-tool` takes
  `<command> [hex...]` and prints its command list for `--help` or no
  arguments.
- Tools built on the shared config facility additionally support shell
  `--completion=bash|zsh|fish` and a TOML config cascade
  (`--config-load=FILE`, `--config-save=FILE`, `--config-dump`, and a default
  `~/.config/<tool>/config.toml`). Every flag has the same name as a TOML key,
  so a unit file can carry its whole configuration in one `--config-load`.
- Every installed name is `statusbar-<target>` with underscores turned into
  dashes; the build applies the prefix uniformly (`statusbar_prefix_tool_names`
  in the shared CMake), so a build-tree binary has the same name as the
  installed one.
- **Platform** is `all` unless marked `Linux-only` (those depend on raw
  `AF_PACKET` sockets, PHC/`/dev/ptp0`, NTP SHM, or `PACKET_MMAP`/XDP, and are
  gated off non-Linux builds in CMake).

Examples use `eth0`; substitute your interface. Live AVB and raw-socket tools
need `CAP_NET_RAW` (plus `CAP_NET_ADMIN` for some). The Debian package's
postinst grants `cap_net_raw,cap_net_admin,cap_sys_nice,cap_ipc_lock` to every
installed `statusbar-*` ELF binary with `setcap`, so on a packaged system the
tools run unprivileged; from a build tree, run as root or `setcap` by hand.

---

## AVB endpoints — run a live entity

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-avb-audio-io` | Dual-format AVB entity: one AM824 and one AAF stream pair plus a CRF media clock. Streams a built-in test tone when nothing feeds it; optional inter-site UDP tunnel (`--udptun.*`). The entity the `statusbar-avb-entity` service runs. | Linux-only |
| `statusbar-avb-tone-generator` | Talker-only entity: 8 channels of continuous sine tones (white piano keys from C4 by default) as AAF plus a CRF media clock, or AM824 + AAF + CRF, locked to gPTP. The entity the `statusbar-avb-tone-generator` service runs. | Linux-only |
| `statusbar-avb-am824-io` | AVB entity: one N-ch 48 kHz AM824 listener/talker pair with a configurable DSP biquad (peak EQ). | Linux-only |
| `statusbar-avb-stereo-io` | AVB entity: one stereo 48 kHz AM824 listener/talker pair with a configurable DSP biquad (descriptors built in). | Linux-only |
| `statusbar-nanoavb` | Demonstration entity wiring the nanoavb components (ADP/ACMP/MVRP/MSRP/gPTP) under a state-machine supervisor. | all |

```sh
# The packaged entity models live in /usr/local/share/statusbar-avb/; audio-io
# and the tone generator default to them, am824-io needs one named.
statusbar-avb-audio-io       --interface=eth0
statusbar-avb-audio-io       --interface=eth0 --descriptor-storage=my_entity.bin --net.backend=xdp
statusbar-avb-tone-generator --interface=eth0 --streams=aaf+crf --tone.base_note=60
statusbar-avb-am824-io       --interface=eth0 --descriptor-storage=entity.bin
statusbar-avb-stereo-io      --interface=eth0 --ptp.driver=linuxptp --ptp.device=/dev/ptp0
statusbar-nanoavb            --interface=eth0 --ptp.driver=system

# Compile your own model from its JSON source first:
statusbar-aemxml json2bin examples/dual.json my_entity.bin
```

`--interface` is **required** on all five (no default). `--descriptor-storage`
defaults to the packaged blob on `avb-audio-io` (`entity_audio.bin`) and the
tone generator (`entity_tone*.bin`, picked by `--streams=all|aaf|aaf+crf`) and
is required on `avb-am824-io`. Useful flags on the entity tools:
`--ptp.driver=linuxptp|ntpshm|system`, `--ptp.device=/dev/ptp0`, `--vlan_id`,
`--entity.id`/`--entity.name`, `--atdecc.version=2016|2021` (empty, the
default, takes the profile's: 2021 under Milan, 2016 otherwise),
`--atdecc.profile=classic|milan13` and `--entity.milan` (a Milan model
answers GET_MILAN_INFO, takes `milan13` as its default profile, and under
that profile answers BIND_STREAM, UNBIND_STREAM and
GET_STREAM_INPUT_INFO_EX; see `ENTITY_CONSTRUCTION_KIT.md`), `--presentation_offset_ns` (0, the default,
takes the profile's: 2 ms under Milan, 1 ms otherwise),
`--net.backend=rawnet|mmap|xdp`
(+ `--net.xdp_queue`), `--filter.freq_hz`/`--filter.gain_db`/`--filter.q`.
`avb-audio-io` adds `--udptun.*` (peer/egress/listen/wcl_ns/rendezvous/max_tai_error_us),
`--tx_pcap.path`, `--gate.talker_on_listener`, `--media.*`, `--tone.*`,
`--srp.*` workarounds and `--stream.address_mode=static|maap` (empty, the
default, takes the profile's: `maap` under Milan, `static` otherwise; under `maap`
the first ENTITY_AVAILABLE after a Link Up waits until the MAAP block is
defended, so a listener that discovers the entity is never told
TALKER_DEST_MAC_FAIL for an address still being probed; with
`--stream.maap_range_file=<path>` the block defended is saved there and
probed first at the next start, so a restart keeps its destination
addresses and a bridge still holding the StreamIDs does not refuse the
new Talker Advertise for tens of seconds; with no file named it is
`maap-range` in the entity's state directory). `--state.dir=<dir>`
(default `/var/lib/statusbar-avb`, made by the package) is where each
entity keeps its saved state, in `<dir>/<entity ID as 16 hex digits>/`:
see "Saved state" in `ENTITY_CONSTRUCTION_KIT.md`.

## ATDECC — control & inspection

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-atdecc-ctl` | Scriptable controller: resolve entity names → connect/disconnect, set/get clock source and signal selector, probe RX/TX state, identify, validate models, run TOML batches, supervise connections. | all |
| `statusbar-acmp-controller` | One-shot ACMP CONNECT/DISCONNECT (RX, to the listener) or CONNECT_TX/DISCONNECT_TX (direct to the talker) to set up or tear down a stream. | all |
| `statusbar-atdecc-controller` | Interactive terminal-UI controller (ADP discovery, AEM descriptor read, ACMP connection management), on an interface or a local entity's ATDECC socket. | all |
| `statusbar-atdecc-monitor` | Passively monitor ADP/AEM/ACMP from a live interface, a local entity's ATDECC socket, or a pcap; optional active descriptor-tree enumeration of one target. | all |
| `statusbar-aem-control` | GET or SET one CONTROL descriptor's values on a target (`--set=` comma-separated values; omit to GET). | all |
| `statusbar-aem-get-counters` | Poll a target's descriptor counters (GET_COUNTERS) at an interval, printing values and deltas. | all |
| `statusbar-aem-set-clock-source` | Discover a target and SET (default) or GET its CLOCK_DOMAIN media-clock source. | all |
| `statusbar-atdecc-mvu` | Send one Milan vendor-unique command to a target and print its answer: `milan-info` (GET_MILAN_INFO), `bind` (BIND_STREAM), `unbind` (UNBIND_STREAM), `stream-input-info` (GET_STREAM_INPUT_INFO_EX). Exits 0 for SUCCESS, 1 for another status, 2 for no response. | all |
| `statusbar-aecp-aa-analyzer` | Offline analysis of AECP Address-Access / Memory-Object firmware-upload sessions in a capture. | all |
| `statusbar-descriptor-storage` | Dump a `.aem` / `.bin` DescriptorStorage blob (configurations, descriptors, string symbols). One positional `<file>`. | all |
| `statusbar-aemxml` | Compile and convert AEM entity models (Python): `json2bin` builds the descriptor-storage blob the entity tools load from a declarative `examples/*.json` model; `json2xml`, `xml2json`, `xml2bin`, `bin2xml`, `bin2json`, `validate`, `dump`, `upgrade` (2013 → 2021 AEMXML schema) and `downgrade`. | all |
| `statusbar-aemxml-cross-check` | Python: compile an example model to a blob, read it back with the C++ `statusbar-descriptor-storage` reader (`--tool=` or `--build-dir=`) and check the two agree. | all |

The controller and `statusbar-atdecc-monitor --enumerate` read an entity's
model the same way: ENTITY, its CONFIGURATION, every descriptor the
configuration's descriptor_counts list, and from each descriptor read the
ones it owns. IEEE 1722.1-2021 7.2.2 has the counts list the top level
descriptors only, so a unit's stream ports, a port's clusters and maps, a
LOCALE's STRINGS and a PTP_INSTANCE's PTP_PORTs are found through the base
index and count their owner gives (`atdecc_aem_children.hpp`). An entity
that counts them as well has each read once.

```sh
statusbar-atdecc-ctl --interface=eth0 --command=list
statusbar-atdecc-ctl --interface=eth0 --command=connect --talker=node-a:0 --listener=node-d:0 --trace
statusbar-atdecc-ctl --interface=eth0 --command=set-clock-source --entity=MyDevice --clock-domain=0 --clock-source=1
statusbar-atdecc-ctl --interface=eth0 --command=set-signal-selector --entity=MyDevice --descriptor=0 --signal-type=AUDIO_CLUSTER --signal-index=2
statusbar-atdecc-ctl --interface=eth0 --command=identify --entity=MyDevice --state=on
statusbar-acmp-controller --interface=eth0 --action=CONNECT \
  --talker-entity-id=00:11:22:33:44:55:66:77 --talker-uid=0 \
  --listener-entity-id=aa:bb:cc:dd:ee:ff:00:11 --listener-uid=1
statusbar-atdecc-controller --interface=eth0
statusbar-atdecc-monitor --interface=eth0 --show-descriptors
statusbar-atdecc-monitor --socket=/run/avb-entity/atdecc.sock --enumerate
statusbar-atdecc-monitor --pcap=capture.pcapng --enumerate
statusbar-aem-control --interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --descriptor-index=1          # GET
statusbar-aem-control --interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --descriptor-index=1 --set=-3.0
statusbar-aem-get-counters --interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --descriptor-index=0
statusbar-aem-set-clock-source --interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --clock-domain-index=0 --clock-source-index=5
statusbar-aecp-aa-analyzer --input=capture.pcapng --verbose
statusbar-descriptor-storage entity.aem
statusbar-aemxml json2bin examples/dual.json entity.bin
statusbar-aemxml dump entity.bin
```

`--socket=PATH` (monitor and controller) reaches one entity on this host
through its local ATDECC socket (an entity started with
`--atdecc.socket=PATH`) instead of
`--interface`: no raw-socket privileges, only that entity, and only its own
traffic (see NANOAVB_ARCHITECTURE.md, "Local ATDECC socket"). The tools send
an ENTITY_DISCOVER every 5 s while otherwise idle.

`atdecc-ctl --command` accepts `list`, `validate`, `connect`, `disconnect`,
`get-rx-state`, `get-tx-state`, `set-clock-source`, `get-clock-source`,
`set-signal-selector`, `get-signal-selector`, `identify` (`--state=on|off`),
`batch` (TOML `--file`) and `supervise` (`--file`, `--watch-ms`).
`--backend=raw|avb` picks the controller's own state machines over the raw
socket (default) or, on macOS, the system AVB framework (needed to reach the
Mac's own virtual entities); `statusbar-atdecc-controller` takes the same
`--backend`.

Every `atdecc-ctl` command starts with an ENTITY_DISCOVER and a discovery
window (1.5 s unless `--discover-ms` says otherwise). An entity may answer
later than that: IEEE 1722.1-2021 6.2.4.2.2 has it wait a random time up to 1/5 of its valid time,
which is up to 4 s for a Milan device and 12.4 s at most. So an entity that a
command names (`--talker`, `--listener`, `--entity`, or an operation of a
`batch` file) and that has not been seen yet is waited for, until 13 s after
the start, and the command goes on as soon as it is there. `list` without
`--entity` names nothing that could be waited for, so its window is 13 s unless
`--discover-ms` says otherwise: by then every entity has answered. `supervise`
resolves its names once a pass and does not wait.

`--trace` on a connect or a disconnect prints what was seen of the handshake's
four messages and a verdict. With an IEEE 1722.1 listener the verdict names the
first leg that broke. A Milan listener answers the controller by itself (Milan
v1.3 5.5.2.4 and 5.5.2.5): it probes the talker after a bind, with the same two
messages, and sends the talker nothing at an unbind. The trace watches up to
1.5 s more for that probe and its answer, and its verdict then says bound or
unbound, and whether the talker has answered a probe.

## AVTP, capture & audio

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-avtp-retransmit` | Round-trip an AVTP stream (AM824/AAF/CRF) from a pcap through deserialize→serialize, writing a new pcap (wire round-trip check); can rewrite addresses, stream id, presentation offset and format on the way. | all |
| `statusbar-avtp-to-wav` | Decode an AVTP AAF or AM824/MBLA audio stream from a pcap into a 32-bit-float BWF wav. | all |
| `statusbar-pcap-dump` | Print each packet's timestamp and decoded frame from a PCAP/PCAPng file. | all |
| `statusbar-bpf-dump` | Live-capture frames of one EtherType off an interface through core's BPF reader and print the decoded contents. | all |
| `statusbar-rttest-send-avtp-tool` | Realtime-timer-driven AVTP/AAF transmitter for TSN egress testing (mmap/bpf/xdp backends, optional PTP scheduling, wake and duration histograms). | Linux-only |

```sh
statusbar-avtp-retransmit --input=in.pcap --stream-id=00:11:22:33:44:55:0001 --format=am824 --output=out.pcap
statusbar-avtp-to-wav --input=capture.pcap --output=out.wav
statusbar-pcap-dump --file=capture.pcapng
statusbar-bpf-dump --device=eth0 --ethertype=0x22f0
statusbar-rttest-send-avtp-tool --interface=eth0 --backend=mmap
```

## Timing — gPTP / PTP / media clock

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-gptp-slave` | gPTP slave-only follower daemon; disciplines to a grandmaster and reports the gPTP↔CLOCK_MONOTONIC_RAW offset and rate. | Linux-only |
| `statusbar-gptp-ntpshm` | Read the current PTP time and local offset from linuxptp's NTP SHM segment. One positional argument, the segment number (default 0). | Linux-only |
| `statusbar-ptpclient-wake` | Demonstrate precision periodic wake timing disciplined to a PTP clock; prints wake-error stats. | Linux-only |
| `statusbar-gps-ratio-tracker` | Track the frequency ratio r = switch_gPTP_rate / GPS_rate (PHC vs GPS-disciplined REALTIME) for the GPS-rate media clock. Space-separated `--flag value` arguments (`--help` lists them). | Linux-only |

```sh
statusbar-gptp-slave --interface=eth0 --profile=standard --verbose
statusbar-gptp-ntpshm 0
statusbar-ptpclient-wake --ptp.driver=linuxptp --ptp.device=/dev/ptp0 --ptp.period=1000000
sudo statusbar-gps-ratio-tracker --device /dev/ptp0 --filter kalman
```

`gptp-slave` options: `--profile=standard|automotive`, `--manual-peer-delay=<ns>`,
`--phase-jump-threshold=<ns>`, `--servo-kp`/`--servo-ki`, `--software`,
`--no-bridge` (the gPTP time bridge is kept against CLOCK_MONOTONIC_RAW, the
application's one timing reference, and its offset and rate are printed; this
turns it off), `--holdover-drift-ppb=<n>` (how fast the bridge's worst-case
error grows while no Sync arrives, default 15000), `--fixed-error-ns=<ns>`
(an error of this port's hardware and link that no measurement shows, added
to the bridge's worst-case error, default 0).

`ptpclient-wake` takes the `--ptp.*` options every PTP-timed tool takes, among
them `--ptp.spin_ns` (the busy-spin window before each wake) and
`--ptp.holdover_drift_ppb` (how fast the PHC link's worst-case error grows
while the PHC cannot be read) and `--ptp.fixed_error_ns` (an error of this
machine's hardware that no fit shows, added to that worst case: the PHC's
offset from its master, PHY asymmetry, timestamp granularity). Both are
figures of the hardware the tool runs on, so they are set where it is
deployed. When it stops it prints the wake-error
statistics and how much of the spin window was left at each wake; see
[PTP_WAKE_TIMING.md](PTP_WAKE_TIMING.md).

## SRP / MSRP

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-msrp-functional-test` | Offline harness: replay a pcap's peer frames through MSRP/MVRP and check the emitted PDUs and events against optional TOML expectations. | all |

```sh
statusbar-msrp-functional-test --input=capture.pcapng --peer-mac=a8:20:66:3c:f8:6e --our-mac=11:22:33:44:55:66
```

## Inter-site tunnel & NAT rendezvous

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-owlm-tool` | One-way latency measurement: stamp small UDP datagrams in the gPTP domain and track per-source latency stats; `--mode=reflect` for a remote reflector; `--csv-output`/`--bin-output` for offline analysis. | Linux-only |
| `statusbar-owlm-analyze` | Python (numpy / pandas / matplotlib): `summarize`, `plot`, `join`, `events` and `gaps` over the CSV or `.colbin` files `statusbar-owlm-tool` writes. | all |
| `statusbar-stun-client-tool` | STUN rendezvous client: REGISTER to a server, learn the reflexive address, pair with a peer, NAT keepalive. | all |
| `statusbar-stun-server-tool` | STUN rendezvous server: pair two clients in a session and return each one's reflexive and peer address. | all |

```sh
statusbar-owlm-tool --gptp-interface=eth0 --owlm-interface=eth1 --peer=10.0.0.5 --tx-interval-us=1000
statusbar-owlm-tool --gptp-interface=eth0 --peer=10.0.0.5 --tx-interval-us=1000 --csv-output=a.csv
statusbar-owlm-analyze summarize a.csv -o a.summary
statusbar-stun-server-tool --port=3478 --key=<64-hex>
statusbar-stun-client-tool --server=stun.example.com:3478 --key=<64-hex> --eui64=AA:BB:CC:DD:EE:FF:00:11
```

## Crypto

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-avtp-crypto-tool` | Hex-in/hex-out front end to the crypto primitives (AES, SIV/GCM-SIV, SHA/HMAC, HKDF, Ed25519/X25519, P-256 ECDSA/ECDH, ECIES, HPKE, the IEEE 1722 EECF/ESCF payloads). | all |

```sh
statusbar-avtp-crypto-tool sha256 <hex-message>
statusbar-avtp-crypto-tool aes128_encrypt <key-hex> <plaintext-hex>
```

Invoked as `<command> [hex_args...]` (not flags); `--help` or no arguments
prints the command list. Commands include `aes128_encrypt`/`aes256_encrypt`,
`aes128_siv_encrypt`, `aes256_gcm_siv_encrypt`, `sha256`/`sha512`/`sha256_hmac`,
`hkdf`/`hkdf_extract`/`hkdf_expand`, `ed25519_sign`/`ed25519_verify`,
`x25519`, `p256_ecdsa_sign`/`p256_ecdh`, `ecies_encrypt` (the IEEE 1363a-2004
triple as V||C||T), `eecf_ecc1_encrypt`/`eecf_ecc1_decrypt` (the IEEE
1722-2025 17.3.2 EECF encrypted_payload, V||T||C) and
`eecf_ecc2_encrypt`/`eecf_ecc2_decrypt` and `eecf_ecc3_encrypt`/
`eecf_ecc3_decrypt` (the HPKE-based ECC2 and ECC3 encrypted_payloads of the
IEEE 1722 security amendment, enc||C||T, with the 12-octet EECF header as
aad), `hpke_x25519_keypair` (RFC 9180 DeriveKeyPair) and `escf_sign`/
`escf_verify` (the ESCF signature_value for sig 00, ECSSA_DSA_EMSA1_SHA256,
or 01, ED25519; see `IEEE_1722_CRYPTO_AMENDMENT.md`).

## Diagnostics

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-adp-l2send-probe` | Raw-socket ADPDU send probe (optionally `PACKET_QDISC_BYPASS`) that reads driver TX counters to localize where frames are dropped before the wire; `--listen` counts what arrives. | Linux-only |

```sh
statusbar-adp-l2send-probe --interface=eth0 --compare
```

## State-machine documentation generators

Each emits the protocol state machines as Graphviz **DOT** or **Markdown**
(pipe DOT through `dot -Tpng`/`-Tsvg`). With no `--machine` they emit all
machines; `--output-dir=DIR` writes one file per machine. The committed
`docs/sm/` tree is their output: the build's `docs-sm` target regenerates it
(Markdown plus SVG, `dot` required), and it is regenerated whenever a state
machine changes.

| Tool | State machines | Platform |
|---|---|---|
| `statusbar-atdecc-sm` | ACMP controller/talker/listener, ADP discovery, AECP AEM controller | all |
| `statusbar-avtp-sm` | AVTP MAAP (multicast address acquisition) | all |
| `statusbar-gptp-sm` | gPTP slave-role port state machine | all |
| `statusbar-srp-sm` | MRP applicant / registrar / leaveall / periodic | all |
| `statusbar-nanoavb-sm` | NanoAVB supervisor, gPTP, MVRP, ADP, ACMP, MSRP, talker/listener engines | all |
| `statusbar-stun-sm-tool` | STUN client + server-session | all |

```sh
statusbar-atdecc-sm  --format=markdown --machine=acmp_controller_sm
statusbar-nanoavb-sm --format=dot --machine=supervisor_sm | dot -Tpng -o supervisor.png
statusbar-srp-sm     --output-dir=docs/sm/srp --format=markdown   # all machines as .md
```

## What else the package installs

The Debian runtime package (`statusbar-avb`) ships, beside the binaries above:

| Path | What it is |
|---|---|
| `/lib/systemd/system/statusbar-avb-entity.service` | Runs `statusbar-avb-audio-io --config-load=/etc/statusbar-avb/entity.toml`. Shipped **disabled**; `systemctl enable --now statusbar-avb-entity` after editing the conffile. |
| `/lib/systemd/system/statusbar-avb-tone-generator.service` | Runs `statusbar-avb-tone-generator --config-load=/etc/statusbar-avb/tone-generator.toml`. Shipped **disabled**, enabled the same way. |
| `/etc/statusbar-avb/entity.toml`, `/etc/statusbar-avb/tone-generator.toml` | The two services' configuration; dpkg conffiles, so site edits survive upgrades. Every key is the tool's flag of the same name. The shipped files hold generic defaults only (`interface = "eth0"`, the packaged model, the test tone); peers, keys and CPU pinning are left commented out. |
| `/usr/local/share/statusbar-avb/entity_audio.bin`, `entity_tone.bin`, `entity_tone_aaf.bin`, `entity_tone_aaf_crf.bin` | The packaged AEM models, compiled at build time from `examples/*.json` by `statusbar-aemxml json2bin`; the defaults of `--descriptor-storage` on the two service entities. |
| `/lib/udev/rules.d/60-statusbar-ptp.rules` | Makes `/dev/ptp*` world-readable so an unprivileged talker timestamps in the gPTP domain instead of falling back to the system clock. |
| `/usr/lib/python3/dist-packages/aemxml/` | The Python AEM model package behind `statusbar-aemxml`. |

The postinst grants the capabilities named at the top to every `statusbar-*`
ELF binary, reloads udev, and across an upgrade restarts each of the two
services that was running. The `-dev` package installs the headers, the static
libraries, the CMake package, this `docs/` tree (including `docs/sm/`) under
`/usr/local/share/doc/statusbar-avb/`, and `statusbar-avb-functional-test`
with its data under `/usr/local/share/statusbar-avb/functional-test-data/`:
the package's own tests that need real sockets and a real kernel, to be run on
a target (`-A` runs all of them; `STATUSBAR_FUNCTIONAL_TEST_DATA_DIR` points
it at other data). It is a different program from
`statusbar-msrp-functional-test` above. No man pages are shipped; each unit's
`Documentation=` line points at its conffile, which names the tool and its
`--help`.

---

See also: [`ENTITY_CONSTRUCTION_KIT.md`](ENTITY_CONSTRUCTION_KIT.md) for the
entity tools' configuration keys, [`OWLM.md`](OWLM.md), [`UDPTUN.md`](UDPTUN.md),
[`GPS_MEDIA_CLOCK.md`](GPS_MEDIA_CLOCK.md), the `*_STATE_MACHINES.md` docs, and
[`AEM_ENTITY_MODEL.md`](AEM_ENTITY_MODEL.md) for the concepts behind these tools.
