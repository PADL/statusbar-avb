# statusbar-avb

C++23 implementation of the AVB/TSN (Audio Video Bridging / Time-Sensitive
Networking) protocol family: IEEE 1722 (AVTP audio transport), IEEE 1722.1
(ATDECC device discovery, control, and connection management), IEEE 802.1AS
(gPTP time synchronization), and IEEE 802.1Q MSRP/MVRP (stream reservation).
Intended for audio device makers building AVB endpoints, AVDECC controller
authors, gPTP slave implementations on Linux, and anyone needing to parse,
emit, or route AVTP streams.

Includes an experimental AVTP-layer crypto extension (`avtp_crypto`) for
authenticating AVTP control traffic.

Also includes AVB/TSN-adjacent networking and measurement tooling: a
gPTP-timestamped UDP framework (`udptun`), one-way latency measurement
(`owlm`), a STUN client and server (RFC 8489, `stun`), AVB/AVTP-aware packet
capture and dump (`netdump`), and a real-time AVTP latency tester.

Version 1.3.0.

> Portions of this repository were developed with assistance from Claude,
> an AI model by Anthropic. All reference material used in this process
> came from my own original open source implementations of these AVB/TSN and
> networking protocol modules, with Claude assisting in refactoring and in
> validating conformance against IEEE 1722, IEEE 1722.1, IEEE 802.1AS,
> IEEE 802.1Q MSRP/MVRP, STUN (RFC 8489), and the pcap/pcapng capture
> formats. All architectural decisions, final implementations, and
> engineering judgments are my own, and any errors are mine alone.

## Overview

`statusbar-avb` is a C++23 implementation of the AVB/TSN protocol family for
time-synchronised, reservation-backed audio streaming over standard Ethernet.
It covers IEEE 1722 AVTP audio transport, IEEE 1722.1 ATDECC device discovery,
control, and connection management, IEEE 802.1AS gPTP time synchronisation,
and IEEE 802.1Q MSRP/MVRP stream reservation — plus an experimental
AVTP-layer crypto extension for authenticating control traffic.

The package is organised into protocol modules (`avtp`, `atdecc`, `gptp`,
`srp`, and the integrated `nanoavb` entity stack) and ships a broad set of
command-line tools: ATDECC controllers and monitors, a Linux gPTP slave and
NTP-SHM bridge, AVTP capture / replay / decode utilities, and turn-key AVB
audio entities.

It also provides AVB/TSN-adjacent networking and measurement modules: the
`udptun` gPTP-timestamped UDP framework, `owlm` one-way latency measurement, a
`stun` STUN client/server (RFC 8489), and `netdump` packet capture — with
their command-line tools and a real-time AVTP latency tester.

It is built for audio device makers building AVB endpoints, AVDECC controller
authors, and gPTP implementers on Linux. It depends on `statusbar-core`,
`statusbar-crypto`, and `statusbar-audio`.

## Quick start

```bash
# Local build + unit tests (Clang+libc++ toolchain is mandatory; applied automatically)
./local-build.sh && ctest --test-dir build

# Reproducible Debian .deb in ../deb-output/ (statusbar-core + statusbar-crypto + statusbar-audio .debs must already be there)
./container-build.sh

# Sanitizers (mutually exclusive — use separate build dirs)
./local-build.sh -DENABLE_ASAN=ON     # AddressSanitizer
./local-build.sh -DENABLE_UBSAN=ON    # UndefinedBehaviorSanitizer
./local-build.sh -DENABLE_TSAN=ON     # ThreadSanitizer

# Fuzzing (libFuzzer harnesses)
./local-build.sh -DENABLE_FUZZING=ON
cmake --build build --target fuzz-smoke   # quick CI pass
cmake --build build --target fuzz-all     # per-harness libFuzzer campaign
```

Depends on `statusbar-core`, `statusbar-crypto`, and `statusbar-audio`. See the sections below for details.

## What ships

### Key command-line tools

Full reference with examples: **[`docs/TOOLS.md`](docs/TOOLS.md)** — every
installed binary, grouped by purpose. Highlights:

- **AVB endpoints** (Linux): `statusbar-avb-audio-io` (dual-format
  AM824 + AAF + CRF), `statusbar-avb-am824-io`, `statusbar-avb-stereo-io`,
  `statusbar-nanoavb`. `--interface` is **required**.
- **ATDECC**: `statusbar-atdecc-ctl` (scriptable connect / clock-source /
  batch), `statusbar-acmp-controller` (one-shot connect),
  `statusbar-atdecc-controller` (interactive TUI), `statusbar-atdecc-monitor`,
  `statusbar-aem-get-counters`, `statusbar-aem-set-clock-source`,
  `statusbar-aemxml`, `statusbar-descriptor-storage`,
  `statusbar-aecp-aa-analyzer`.
- **Capture / audio**: `statusbar-pcap-dump`, `statusbar-bpf-dump`,
  `statusbar-avtp-to-wav`, `statusbar-avtp-retransmit`,
  `statusbar-rttest-send-avtp-tool` (Linux).
- **Timing** (Linux): `statusbar-gptp-slave`, `statusbar-gptp-ntpshm`,
  `statusbar-ptpclient-wake`, `statusbar-gps-ratio-tracker`.
- **Inter-site / NAT**: `owlm_tool` (Linux), `stun_client_tool`,
  `stun_server_tool`. **SRP**: `statusbar-msrp-functional-test`.
- **Crypto**: `statusbar-avtp-crypto-tool`.
- **State-machine diagrams** (DOT / Markdown):
  `statusbar-{atdecc,avtp,gptp,srp,nanoavb}-sm`, `stun_sm_tool`.

Every tool supports `--help`; facility-based tools also support
`--completion` and a TOML config cascade (`--config-load` / `--config-save`).
The Python `owlm_analyze` (numpy / pandas / matplotlib) post-processes `owlm`
CSV output into plots and stats.

### Modules

- **`avtp`** — IEEE 1722 transport: AAF (v0 + v1), AM824, CRF, NTSCF/TSCF,
  AEF, ESCF/EECF, MAAP, IP encapsulation, and stream input/output state
  machines.
- **`avtp_crypto`** — AVTP control-plane crypto: key exchange, keychain,
  AEM-frame authentication, P-256 wire helpers.
- **`avtp_tools`** — higher-level helpers atop `avtp` (audio stream decoder,
  channel interleaver, AVTP-to-WAV).
- **`atdecc`** — IEEE 1722.1: ADP discovery, ACMP connection management,
  AECP including AEM and Address Access; descriptor storage and AEM control
  value/type accessors.
- **`atdecc_tools`** — interactive ATDECC controller driver and TUI.
- **`gptp`** — IEEE 802.1AS generalised PTP: messages, slave port + session
  state machines, link delay, servo, Linux clock/socket integration, and an
  NTP-SHM publisher.
- **`ptpclient`** — application-facing PTP client abstraction with a bridge
  to `linuxptp` and a wake/timer interface.
- **`srp`** — IEEE 802.1Q SRP: MRP (applicant / registrar / leave-all /
  periodic state machines), plus MSRP (stream reservation) and MVRP (VLAN
  registration) participants.
- **`nanoavb`** — small, integrated AVB entity stack tying ADP, ACMP, AECP,
  MSRP, MVRP, and gPTP together behind a supervisor state machine.
- **`avb_entity`** — turn-key AVB audio entities (stereo PCM, AM824) built
  on top of `nanoavb` and `ptpclient`.
- **`udptun`** — gPTP-timestamped UDP framework with redundancy, per-source
  tracking, and CSV / columnar telemetry.
- **`owlm`** — one-way latency measurement over a gPTP-locked link.
- **`stun`** — STUN (RFC 8489) client/server with a private REGISTER extension
  for reflexive-address discovery and peer rendezvous.
- **`netdump`** — AVB/AVTP-aware packet capture and frame-by-frame dump
  (pcap/pcapng).

## Building

This package depends on other statusbar packages. Export every package
into one directory so the trees sit side by side, then build the
dependencies first — each builds on its own (see its README) — in this
order:

    statusbar-core -> statusbar-crypto -> statusbar-audio -> statusbar-avb

This package then finds each dependency through its **local
build tree** — nothing is installed, so your system stays clean.

**Quick path:** run `./local-build.sh`. It looks for each dependency's
build dir in a sibling checkout (`../<dep>/build/`), threads the `-Dstatusbar-<dep>_DIR` flags into
cmake automatically, and falls back to `$STATUSBAR_<DEP>_DIR` when the
layout differs. Extra args are forwarded to cmake configure
(`./local-build.sh -DENABLE_ASAN=ON`). The script echoes its final cmake
invocation via `set -x` so users configuring an IDE can copy the exact
flags.

**Manual / IDE configuration.** The explicit cmake invocation is:

```
cmake -S . -B build -G Ninja --toolchain cmake/toolchain-clang.cmake \
  -Dstatusbar-core_DIR=../core/build \
  -Dstatusbar-crypto_DIR=../crypto/build \
  -Dstatusbar-audio_DIR=../audio/build
cmake --build build
ctest --test-dir build
```

Each `-Dstatusbar-<dep>_DIR=<path>` points `find_package` at a
dependency's build directory; the package exports its build tree,
so it need not be installed. The `../<dep>/build` paths
assume the packages were exported as siblings — use absolute paths
otherwise. In an IDE's CMake settings, add one cache entry per
dependency (`statusbar-core_DIR`, `statusbar-crypto_DIR`,
`statusbar-audio_DIR`) pointing at each dep's build directory; this
is exactly what the script generates.

## Benchmarks

This package ships no benchmark executables of its own. An outer
aggregate build's `cmake --build build --target run-all-benches` runs
every bench across `core` (buffer, ieee), `crypto`, and `audio` (dsp,
engine) in one pass.

## Coverage

Line/region coverage is wired through LLVM source-based profiling
(`-fprofile-instr-generate -fcoverage-mapping`). Enable via
`-DENABLE_COVERAGE=ON`, ideally in a separate build directory:

```
cmake -S . -B build-cov -G Ninja --toolchain cmake/toolchain-clang.cmake \
  -DENABLE_COVERAGE=ON
```

Three CMake custom targets become available after configure (they
require `llvm-profdata` and `llvm-cov` on `PATH`):

| Target | What it does |
|---|---|
| `coverage-collect` | Builds and runs `statusbar_test`, merges `.profraw` into `combined.profdata` |
| `coverage-report` | Prints a text line-coverage report |
| `coverage-html`   | Generates an HTML report under `build-cov/coverage/html/` |

Each target depends on the previous, so a single invocation runs the
whole pipeline:

```
cmake --build build-cov --target coverage-html
xdg-open build-cov/coverage/html/index.html   # or `open` on macOS
```

`*_test.cpp` / `test.hpp` files are excluded by default. Override with
`-DSTATUSBAR_COVERAGE_IGNORE='<regex>'` if you want a different filter.

When this package is built as part of an outer aggregate build, the
same three targets run against the unified `statusbar_test` covering
every aggregated package.

## Fuzz testing

Every byte parser that consumes untrusted input has a libFuzzer harness
(`*_fuzzer.cpp`). When the compiler is Clang (the toolchain default),
`-DENABLE_FUZZING=ON` is automatic; the harnesses build with
`-fsanitize=fuzzer,address,undefined`.

Two aggregate targets sweep every `*_fuzzer` executable in the build tree:

| Target | What it does |
|---|---|
| `fuzz-smoke` | Runs each fuzzer once with a 256-byte random seed. Quick check; CI-friendly. |
| `fuzz-all`   | Extended run (Linux: 30 s of libFuzzer mutation per fuzzer with a persistent corpus; macOS: 1000 random inputs per fuzzer via the standalone driver). |

```
cmake --build build                          # builds the fuzzers
cmake --build build --target fuzz-smoke      # smoke pass
cmake --build build --target fuzz-all        # extended pass
```

Or run a single fuzzer directly with any libFuzzer arguments:

```
./build/.../some_fuzzer -max_total_time=60 -max_len=4096
```

Persistent corpora live under `build/fuzz/corpus/<fuzzer_name>/`.

To disable: `-DENABLE_FUZZING=OFF`.

## Sanitizers

`cmake/sanitizers.cmake` exposes three mutually exclusive sanitizer options.
Use a **separate build directory per sanitizer** — the instrumentation
flags change ABI / runtime expectations:

```
cmake -S . -B build-asan -G Ninja --toolchain cmake/toolchain-clang.cmake \
  -DENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan
```

| Option | Adds | Use for |
|---|---|---|
| `-DENABLE_ASAN=ON`  | `-fsanitize=address`                       | Out-of-bounds, use-after-free, leaks |
| `-DENABLE_UBSAN=ON` | `-fsanitize=undefined -fno-sanitize-recover` | Integer overflow, alignment, UB |
| `-DENABLE_TSAN=ON`  | `-fsanitize=thread`                        | Data races (useful for SPSC + triple-buffer code) |

Enabling more than one of the three at configure time is a hard error.

## Static analysis (clang-tidy)

Two modes are available:

**In-compile** — `clang-tidy` runs as part of every translation unit:

```
cmake -S . -B build-tidy -G Ninja --toolchain cmake/toolchain-clang.cmake \
  -DENABLE_CLANG_TIDY=ON
cmake --build build-tidy
```

Slow (every TU lints), but findings show up alongside the compile output.

**Separate aggregate run** — runs `clang-tidy` in parallel against the
existing build's `compile_commands.json`:

```
cmake --build build                         # normal build first
cmake --build build --target clang-tidy     # runs against build/compile_commands.json
```

The `clang-tidy` target uses `cmake/run-clang-tidy-all.sh` to fan out
across cores (8 jobs by default), skips `*_test.cpp` / `*_tool.cpp` /
`*_example.cpp` / `*_fuzzer.cpp`, and writes combined findings to
`build/clang-tidy-findings.txt`. The `.clang-tidy` config at the
submodule root drives check selection.

## Installing

```
cmake --install build --prefix <prefix>
```

## Packaging

A standalone build configures CPack — TGZ and ZIP archives on every
platform, plus DEB and (when `rpmbuild` is present) RPM on Linux.
Run `cpack` from the build directory:

```
cd build && cpack
```

This produces `statusbar-avb` (runtime: tools) and `statusbar-avb-dev` / `-devel` (headers, static libraries, CMake config).

### Reproducible Debian packages

`./container-build.sh` builds this package's `.deb`s inside a Debian
container — no local toolchain needed. Output lands in `../deb-output/`
(override with `DEB_OUTPUT`); the base image is configurable with
`DEBIAN_VERSION=...`.

The `statusbar-core`, `statusbar-crypto`, and `statusbar-audio` `.deb`s
must already be present in `../deb-output/` — build each by running
`./container-build.sh` in a sibling clone of the corresponding repo
first, in that order.
