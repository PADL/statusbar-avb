<!-- SPDX-License-Identifier: MIT -->
# AVB command-line tools

Every binary the `statusbar-avb` package installs, grouped by purpose. This is a
reference index — each tool also documents itself:

- `--help` prints usage and the full option list (generated from the tool's
  argument specs, so it never drifts).
- Tools built on the shared config facility additionally support shell
  `--completion=bash|zsh|fish` and a TOML config cascade
  (`--config-load=FILE`, `--config-save=FILE`, and a default
  `~/.config/<tool>/config.toml`).
- **Platform** is `all` unless marked `Linux-only` (those depend on raw
  `AF_PACKET` sockets, PHC/`/dev/ptp0`, NTP SHM, or `PACKET_MMAP`/XDP, and are
  gated off non-Linux builds in CMake).

Examples use `eth0`; substitute your interface. Live AVB/raw-socket tools need
`CAP_NET_RAW` (+ `CAP_NET_ADMIN` for some) — run as root or grant caps with
`setcap`.

---

## AVB endpoints — run a live entity

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-avb-audio-io` | Dual-format AVB entity (AM824 + AAF stream pair + CRF media clock) streaming an N-ch 96 kHz tone, with optional inter-site UDP tunnel. | Linux-only |
| `statusbar-avb-am824-io` | AVB entity: one N-ch 48 kHz AM824 listener/talker pair with a configurable DSP biquad (peak EQ). | Linux-only |
| `statusbar-avb-stereo-io` | AVB entity: one stereo 48 kHz AM824 listener/talker pair with a configurable DSP biquad (descriptors built in). | Linux-only |
| `statusbar-nanoavb` | Demonstration entity wiring the nanoavb components (ADP/ACMP/MVRP/MSRP/gPTP) under a state-machine supervisor. | Linux-only |

```sh
# Build the descriptor model first, then run the dual-format entity.
statusbar-aem-entity-blob --dual --channels 8 --sample-rate 96000 --out entity_audio.bin
statusbar-avb-audio-io --interface=eth0 --descriptor-storage=entity_audio.bin

statusbar-avb-am824-io --interface=eth0 --descriptor-storage=entity.bin
statusbar-avb-stereo-io --interface=eth0 --ptp.driver=linuxptp --ptp.device=/dev/ptp0
statusbar-nanoavb       --interface=eth0 --ptp.driver=system
```

`--interface` is **required** on all four (no default). `avb-audio-io` and
`avb-am824-io` also require `--descriptor-storage=<path>`. Useful flags:
`--ptp.driver=linuxptp|system`, `--ptp.device=/dev/ptp0`, `--vlan_id`,
`--filter.freq_hz`/`--filter.gain_db`/`--filter.q`, `--entity.id`/`--entity.name`;
`avb-audio-io` adds `--udptun.*` (peer/egress/listen/wcl_ns), `--tx_pcap.path`,
`--gate.talker_on_listener`, `--media.*`, `--tone.*`.

## ATDECC — control & inspection

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-atdecc-ctl` | Scriptable controller: resolve entity names → connect/disconnect, set/get clock source, probe RX/TX state, validate models, run TOML batches. | all |
| `statusbar-acmp-controller` | One-shot ACMP CONNECT/DISCONNECT (RX-to-listener or TX-direct) to set up/tear down a stream. | all |
| `statusbar-atdecc-controller` | Interactive terminal-UI controller (ADP discovery, AEM descriptor read, ACMP connection management). | all |
| `statusbar-atdecc-monitor` | Passively monitor ADP/AEM/ACMP from a live interface or a pcap; optional active descriptor-tree enumeration. | all |
| `statusbar-aem-get-counters` | Poll a target's descriptor counters (GET_COUNTERS) at an interval, printing values + deltas. | all |
| `statusbar-aem-set-clock-source` | Discover a target and SET/GET its CLOCK_DOMAIN media-clock source. | all |
| `statusbar-aecp-aa-analyzer` | Offline analysis of AECP Address-Access / Memory-Object firmware-upload sessions in a capture. | all |
| `statusbar-descriptor-storage` | Dump a `.aem` DescriptorStorage blob (configs, descriptors, string symbols). Positional `<file.aem>`. | all |
| `statusbar-aem-entity-blob` | Generate the in-tree AEM descriptor-storage blob consumed by the entity tools. | all |

```sh
statusbar-atdecc-ctl --interface=eth0 --command=list
statusbar-atdecc-ctl --interface=eth0 --command=connect --talker=node-a:0 --listener=node-d:0
statusbar-atdecc-ctl --interface=eth0 --command=set-clock-source --entity=MyDevice --clock-domain=0 --clock-source=1
statusbar-acmp-controller --interface=eth0 --action=CONNECT \
  --talker-entity-id=00:11:22:33:44:55:66:77 --talker-uid=0 \
  --listener-entity-id=aa:bb:cc:dd:ee:ff:00:11 --listener-uid=1
statusbar-atdecc-controller --interface=eth0
statusbar-atdecc-monitor --interface=eth0 --show-descriptors
statusbar-atdecc-monitor --pcap=capture.pcapng --enumerate
statusbar-aem-get-counters --interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --descriptor-index=0
statusbar-aem-set-clock-source --interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --clock-domain-index=0 --clock-source-index=5
statusbar-aecp-aa-analyzer --input=capture.pcapng --verbose
statusbar-descriptor-storage entity.aem
statusbar-aem-entity-blob --dual --channels 8 --sample-rate 96000 --out entity.bin
```

`atdecc-ctl --command` accepts `list`, `validate`, `connect`, `disconnect`,
`get-rx-state`, `get-tx-state`, `set-clock-source`, `get-clock-source`, `batch`
(TOML `--file`), and `supervise`.

## AVTP, capture & audio

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-avtp-retransmit` | Round-trip an AVTP stream (AM824/AAF/CRF) from a pcap through deserialize→serialize, writing a new pcap (wire round-trip check). | all |
| `statusbar-avtp-to-wav` | Decode an AVTP AAF or AM824/MBLA audio stream from a pcap into a 32-bit-float BWF wav. | all |
| `statusbar-pcap-dump` | Print each packet's timestamp + decoded frame from a PCAP/PCAPng file. | all |
| `statusbar-bpf-dump` | Live-capture frames of a given EtherType off an interface (BPF) and print decoded contents. | all |
| `statusbar-rttest-send-avtp-tool` | Realtime-timer-driven AVTP/AAF transmitter for TSN egress testing (mmap/bpf/xdp backends, optional PTP scheduling). | Linux-only |

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
| `statusbar-gptp-slave` | gPTP slave-only follower daemon; disciplines to a grandmaster and reports gPTP↔bridge-clock offset/rate. | Linux-only |
| `statusbar-gptp-ntpshm` | Read current PTP time + local offset from linuxptp's NTP SHM segment. | Linux-only |
| `statusbar-ptpclient-wake` | Demonstrate precision periodic wake timing disciplined to a PTP clock; prints wake-error stats. | Linux-only |
| `statusbar-gps-ratio-tracker` | Track the frequency ratio r = switch_gPTP_rate / GPS_rate (PHC vs GPS-disciplined REALTIME) for the GPS-rate media clock. | Linux-only |

```sh
statusbar-gptp-slave --interface=eth0 --profile=standard --verbose
statusbar-gptp-ntpshm 0
statusbar-ptpclient-wake --ptp.driver=linuxptp --ptp.device=/dev/ptp0 --ptp.period=1000000
statusbar-gps-ratio-tracker --device /dev/ptp0 --filter kalman
```

`gptp-slave` options: `--profile=standard|automotive`, `--manual-peer-delay=<ns>`,
`--servo-kp`/`--servo-ki`, `--software`, `--bridge-clock=monotonic_raw|...`.
(`gps-ratio-tracker` uses a hand-rolled parser — space-separated `--flag value`.)

## SRP / MSRP

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-msrp-functional-test` | Offline harness: replay a pcap's peer frames through MSRP/MVRP and check emitted PDUs/events against optional TOML expectations. | all |

```sh
statusbar-msrp-functional-test --input=capture.pcapng --peer-mac=a8:20:66:3c:f8:6e --our-mac=11:22:33:44:55:66
```

## Inter-site tunnel & NAT rendezvous

| Tool | Purpose | Platform |
|---|---|---|
| `owlm_tool` | One-way latency measurement: stamp small UDP datagrams in the gPTP domain and track per-source latency stats. | Linux-only |
| `stun_client_tool` | STUN rendezvous client — REGISTER to a server, learn the reflexive address, pair with a peer, NAT keepalive. | all |
| `stun_server_tool` | STUN rendezvous server — pair two clients in a session and return each one's reflexive + peer address. | all |

```sh
owlm_tool --gptp-interface=eth0 --owlm-interface=eth1 --peer=10.0.0.5 --tx-interval-us=1000
stun_server_tool --port=3478 --key=<64-hex>
stun_client_tool --server=reflex.example.com:3478 --key=<64-hex> --eui64=AA:BB:CC:DD:EE:FF:00:11
```

## Crypto

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-avtp-crypto-tool` | Hex-in/hex-out front end to the crypto primitives (AES, SIV/GCM-SIV, SHA/HMAC, HKDF, Ed25519/X25519, P-256 ECDSA/ECDH, ECIES). | all |

```sh
statusbar-avtp-crypto-tool sha256 <hex-message>
statusbar-avtp-crypto-tool aes128_encrypt <key-hex> <plaintext-hex>
```

Invoked as `<command> [hex_args...]` (not flags). Commands include
`aes128_encrypt`/`aes256_encrypt`, `aes128_siv_encrypt`,
`aes256_gcm_siv_encrypt`, `sha256`/`sha512`/`sha256_hmac`,
`hkdf`/`hkdf_extract`/`hkdf_expand`, `ed25519_sign`/`ed25519_verify`,
`x25519`, `p256_ecdsa_sign`/`p256_ecdh`, `ecies_encrypt`.

## Diagnostics

| Tool | Purpose | Platform |
|---|---|---|
| `statusbar-adp-l2send-probe` | Raw-socket ADPDU send probe (optionally `PACKET_QDISC_BYPASS`) that reads driver TX counters to localize where frames are dropped before the wire. | Linux-only |

```sh
statusbar-adp-l2send-probe --interface=eth0 --compare
```

## State-machine documentation generators

Each emits the protocol state machines as Graphviz **DOT** or **Markdown**
(pipe DOT through `dot -Tpng`/`-Tsvg`). With no `--machine` they emit all
machines; `--output-dir=DIR` writes one file per machine.

| Tool | State machines | Platform |
|---|---|---|
| `statusbar-atdecc-sm` | ACMP controller/talker/listener, ADP discovery, AECP AEM controller | all |
| `statusbar-avtp-sm` | AVTP MAAP (multicast address acquisition) | all |
| `statusbar-gptp-sm` | gPTP slave-role port state machine | all |
| `statusbar-srp-sm` | MRP applicant / registrar / leaveall / periodic | all |
| `statusbar-nanoavb-sm` | NanoAVB supervisor, gPTP, MVRP, ADP, ACMP, MSRP, talker/listener engines | all |
| `stun_sm_tool` | STUN client + server-session | all |

```sh
statusbar-atdecc-sm  --format=markdown --machine=acmp_controller_sm
statusbar-nanoavb-sm --format=dot --machine=supervisor_sm | dot -Tpng -o supervisor.png
statusbar-srp-sm     --output-dir=docs/sm --format=markdown   # all machines as .md
```

---

See also: [`OWLM.md`](OWLM.md), [`UDPTUN.md`](UDPTUN.md),
[`GPS_MEDIA_CLOCK.md`](GPS_MEDIA_CLOCK.md), the `*_STATE_MACHINES.md` docs, and
[`AEM_ENTITY_MODEL.md`](AEM_ENTITY_MODEL.md) for the concepts behind these tools.
