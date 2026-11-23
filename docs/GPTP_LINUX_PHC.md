<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->

# gPTP Linux PHC Factory

This document captures the design for `GptpClockOps::linux_raw_socket_and_phc()`,
the Linux-specific factory that binds the platform-agnostic `GptpClockOps`
interface to Linux kernel PTP hardware clock (PHC) subsystem calls.

**Status**: implemented in `statusbar/gptp/gptp_clock_ops_linux.{hpp,cpp}`.
The factory binds the five `GptpClockOps` lambdas to Linux PHC + raw-socket
system calls. The core gPTP code (`GptpSlavePort`, servo, FSMs) is still
fully platform-agnostic and continues to be exercised on macOS with a
`SoftwareOps` mock.

## Overview

The factory takes two already-open file descriptors and returns a
`GptpClockOps` struct with all five lambdas bound to Linux system calls:

```cpp
auto ops = GptpClockOps::linux_raw_socket_and_phc(raw_sock_fd, phc_fd);
```

The caller is responsible for the privileged setup (socket creation,
NIC timestamping enable, PHC device open) because those steps require
`CAP_NET_RAW` / root and are deployment-specific. The factory only
binds the lambdas.

## File layout

```
statusbar/gptp/
├── gptp_clock_ops_linux.hpp         factory declaration (#if __linux__)
├── gptp_clock_ops_linux.cpp         factory implementation (#if __linux__)
└── gptp_slave_linux_tool.cpp        the slave daemon (statusbar-gptp-slave)
```

## The five GptpClockOps lambdas

### 1. `get_local_time_ns` → `clock_gettime(phc_clock_id)`

The PHC device (`/dev/ptp0`, `/dev/ptp1`, etc.) exposes a kernel
`clockid_t` via the `FD_TO_CLOCKID` macro:

```cpp
// ((~(clockid_t)(fd) << 3) | 3)
clockid_t const phc_clock_id = ((~static_cast<clockid_t>(phc_fd)) << 3) | 3;
```

This pattern already exists in `statusbar/ptpclient/ptpclient_linuxptp.cpp:67-71`.

```cpp
ops.get_local_time_ns = [phc_clock_id]() -> int64_t {
    struct timespec ts{};
    clock_gettime(phc_clock_id, &ts);
    return ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
};
```

This reads the **hardware clock on the NIC** — the same clock that
stamps incoming/outgoing PTP frames. It is the clock being disciplined
by the servo.

### 2. `send_frame` → `sendto()` + `recvmsg(MSG_ERRQUEUE)`

When a gPTP event message is transmitted, the NIC captures the precise
TX timestamp in hardware. The kernel delivers it back to userspace
asynchronously via the socket's **error queue**.

```cpp
ops.send_frame = [raw_sock_fd, dst_addr](std::span<uint8_t const> payload) -> TxResult {
    // 1. Send the frame
    sendto(raw_sock_fd, payload.data(), payload.size(), 0,
           (struct sockaddr*)&dst_addr, sizeof(dst_addr));

    // 2. Poll the error queue for the TX timestamp
    //    (blocks briefly — typically < 1 ms for HW timestamping)
    struct msghdr msg{};
    struct iovec iov{};
    uint8_t ctrl_buf[256]{};
    uint8_t dummy_buf[256]{};
    iov.iov_base = dummy_buf;
    iov.iov_len = sizeof(dummy_buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl_buf;
    msg.msg_controllen = sizeof(ctrl_buf);

    // Retry a few times (HW timestamp may arrive after a short delay)
    for (int retry = 0; retry < 10; ++retry) {
        ssize_t n = recvmsg(raw_sock_fd, &msg, MSG_ERRQUEUE);
        if (n < 0) {
            usleep(100);  // 100 µs between retries
            continue;
        }
        // 3. Extract the HW timestamp from the cmsg
        for (auto* cmsg = CMSG_FIRSTHDR(&msg); cmsg;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET
                && cmsg->cmsg_type == SO_TIMESTAMPING) {
                auto* ts = (struct timespec*)CMSG_DATA(cmsg);
                // ts[0] = software, ts[1] = deprecated,
                // ts[2] = hardware (RAW)
                int64_t hw_ns = ts[2].tv_sec * 1'000'000'000LL
                              + ts[2].tv_nsec;
                return TxResult{.ok = true, .tx_timestamp_ns = hw_ns};
            }
        }
    }
    return TxResult{.ok = false, .tx_timestamp_ns = 0};
};
```

The `ts[2]` slot is the `SOF_TIMESTAMPING_RAW_HARDWARE` timestamp —
the PHC's reading at the instant the frame hit the wire. This is
`t1` in the Pdelay exchange.

**Reference**: OpenAvnu `linux_hal_generic.cpp:283-369`
(`HWTimestamper_txtimestamp`).

### 3. `adjust_frequency_ppb` → `clock_adjtime(ADJ_FREQUENCY)`

The kernel's `clock_adjtime` system call steers the PHC's frequency
synthesizer. The `struct timex` field `freq` is in units of
**ppb × 65536** (shifted fixed-point), not plain ppb:

```cpp
ops.adjust_frequency_ppb = [phc_clock_id](double ppb) {
    struct timex tmx{};
    tmx.modes = ADJ_FREQUENCY;
    // kernel freq units: ppb × 65.536 (i.e. ppb << 16 / 1000)
    tmx.freq = static_cast<long>(ppb * 65.536);
    clock_adjtime(phc_clock_id, &tmx);
};
```

This is the steady-state servo output — called every sync interval
(125 ms standard, 31.25 ms automotive) to slew the hardware clock
toward the grandmaster rate.

**Reference**: OpenAvnu `linux_hal_generic.cpp:216-222` (`Adjust()`
method).

### 4. `adjust_phase_ns` → `clock_adjtime(ADJ_SETOFFSET)`

For large phase corrections (when the servo's phase error exceeds
`servo_phase_jump_threshold_ns` for
`servo_phase_jump_consecutive_samples` consecutive syncs), we step
the PHC clock:

```cpp
ops.adjust_phase_ns = [phc_clock_id](int64_t phase_ns) {
    struct timex tmx{};
    tmx.modes = ADJ_SETOFFSET | ADJ_NANO;
    tmx.time.tv_sec = phase_ns / 1'000'000'000LL;
    // The usec field carries nanoseconds when ADJ_NANO is set.
    tmx.time.tv_usec = phase_ns % 1'000'000'000LL;
    clock_adjtime(phc_clock_id, &tmx);
};
```

### 5. `get_link_speed` → `ioctl(SIOCETHTOOL)`

```cpp
ops.get_link_speed = [raw_sock_fd, ifname]() -> LinkSpeedMbps {
    struct ifreq ifr{};
    struct ethtool_cmd ecmd{};
    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ);
    ecmd.cmd = ETHTOOL_GSET;
    ifr.ifr_data = (char*)&ecmd;
    if (ioctl(raw_sock_fd, SIOCETHTOOL, &ifr) < 0) {
        return LinkSpeedMbps::Unknown;
    }
    switch (ethtool_cmd_speed(&ecmd)) {
        case 10:    return LinkSpeedMbps::Mbps10;
        case 100:   return LinkSpeedMbps::Mbps100;
        case 1000:  return LinkSpeedMbps::Mbps1000;
        case 10000: return LinkSpeedMbps::Mbps10000;
        default:    return LinkSpeedMbps::Unknown;
    }
};
```

Used by the port to look up PHY delay compensation values
(`GptpConfig::phy_delay_1g`, etc.).

## Caller setup (before calling the factory)

The factory takes already-open fds because the setup involves
privileged operations the caller controls:

```cpp
// 1. Open raw AF_PACKET socket for gPTP
int raw_fd = socket(AF_PACKET, SOCK_RAW, htons(0x88F7));

// 2. Bind to the network interface
struct sockaddr_ll addr{};
addr.sll_family = AF_PACKET;
addr.sll_protocol = htons(0x88F7);
addr.sll_ifindex = if_nametoindex("eth0");
bind(raw_fd, (struct sockaddr*)&addr, sizeof(addr));

// 3. Join PTP multicast group
struct packet_mreq mr{};
mr.mr_ifindex = addr.sll_ifindex;
mr.mr_type = PACKET_MR_MULTICAST;
mr.mr_alen = 6;
memcpy(mr.mr_address, "\x01\x80\xc2\x00\x00\x0e", 6);
setsockopt(raw_fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr));

// 4. Enable HW timestamping on the NIC via SIOCSHWTSTAMP
struct hwtstamp_config hwcfg{};
hwcfg.tx_type = HWTSTAMP_TX_ON;
hwcfg.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
struct ifreq ifr{};
strncpy(ifr.ifr_name, "eth0", IFNAMSIZ);
ifr.ifr_data = (char*)&hwcfg;
ioctl(raw_fd, SIOCSHWTSTAMP, &ifr);

// 5. Set SO_TIMESTAMPING on the socket
int flags = SOF_TIMESTAMPING_TX_HARDWARE
          | SOF_TIMESTAMPING_RX_HARDWARE
          | SOF_TIMESTAMPING_RAW_HARDWARE;
setsockopt(raw_fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));

// 6. Find the PHC device index via ethtool
struct ethtool_ts_info ts_info{};
ts_info.cmd = ETHTOOL_GET_TS_INFO;
ifr.ifr_data = (char*)&ts_info;
ioctl(raw_fd, SIOCETHTOOL, &ifr);
int phc_index = ts_info.phc_index;

// 7. Open the PHC device
char phc_path[32];
snprintf(phc_path, sizeof(phc_path), "/dev/ptp%d", phc_index);
int phc_fd = open(phc_path, O_RDWR);

// 8. Create the GptpClockOps and the port
auto ops = GptpClockOps::linux_raw_socket_and_phc(raw_fd, phc_fd);
auto cfg = GptpConfig::avnu_automotive_slave_defaults();
GptpSlavePort port{cfg, std::move(ops)};
port.start(now, /*link_up=*/true);
```

Steps 1-2 are already handled by `statusbar/net/net_rawnet.cpp:46-106`
(`RawnetContext`). The factory adds steps 3-7 on top.

## RX timestamp flow

When the event loop calls `recvmsg()` on the raw socket to receive
a gPTP frame, the kernel attaches the HW RX timestamp as a
`SO_TIMESTAMPING` cmsg — same cmsg parsing logic as the TX error
queue path but from the regular receive instead of `MSG_ERRQUEUE`.

The event loop extracts the timestamp and passes it to:

```cpp
port.receive_frame(gptp_payload, rx_hw_timestamp_ns, now);
```

The port applies PHY delay compensation internally
(`rx_adjusted = rx_hw_timestamp_ns - phy_delay_rx`).

**Reference**: OpenAvnu `linux_hal_generic.cpp:134-151` (RX timestamp
extraction from cmsg in the regular receive path).

## Cross-timestamp support (optional)

For applications that need to correlate PHC time with
`CLOCK_REALTIME` or `CLOCK_MONOTONIC` (e.g., for NTP SHM output),
the Linux PHC driver may support precise cross-timestamping:

```cpp
struct ptp_sys_offset_precise offset{};
ioctl(phc_fd, PTP_SYS_OFFSET_PRECISE, &offset);
// offset.device = PHC time
// offset.sys_realtime = CLOCK_REALTIME at the same instant
```

Or the multi-sample fallback:

```cpp
struct ptp_sys_offset offset{};
offset.n_samples = 5;
ioctl(phc_fd, PTP_SYS_OFFSET, &offset);
// Pick the sample pair with the smallest bracketing interval.
```

This is not part of the core `GptpClockOps` interface (it's a
diagnostic / output concern), but could be exposed via a separate
helper for applications that want to write NTP SHM or feed
`CLOCK_TAI` from the disciplined PHC.

**Reference**: OpenAvnu `linux_hal_generic.cpp:450-501`
(`HWTimestamper_gettime`).

## Required Linux headers

```cpp
#include <linux/net_tstamp.h>     // SOF_TIMESTAMPING_*, hwtstamp_config
#include <linux/sockios.h>        // SIOCSHWTSTAMP, SIOCETHTOOL
#include <linux/ethtool.h>        // ethtool_ts_info, ETHTOOL_GET_TS_INFO
#include <linux/ptp_clock.h>      // PTP_SYS_OFFSET, PTP_CLOCK_GETCAPS
#include <sys/timex.h>            // struct timex, ADJ_FREQUENCY, ADJ_SETOFFSET
#include <sys/socket.h>           // AF_PACKET, SOL_SOCKET, SO_TIMESTAMPING
#include <linux/if_packet.h>      // sockaddr_ll, PACKET_ADD_MEMBERSHIP
```

None of these are available on macOS, which is why this phase is
deferred to a Linux build host.

## Existing codebase references

| What | Where | Notes |
|------|-------|-------|
| AF_PACKET socket setup | `statusbar/net/net_rawnet.cpp:46-106` | Already binds to interface + ethertype |
| FD_TO_CLOCKID | `statusbar/ptpclient/ptpclient_linuxptp.cpp:67-71` | Same macro we'll use |
| NTP SHM reader | `statusbar/gptp/gptp_ntpshm.hpp/.cpp` | For ptp4l interop (reader, not writer) |
| BPF device Linux | `statusbar/bpf/bpf_device_linux.hpp` | Platform-specific raw packet capture |
| OpenAvnu HW timestamping | `stash/avnu/gptp/linux/src/linux_hal_generic.cpp` | Full reference for all PHC operations |
| OpenAvnu socket setup | `stash/avnu/gptp/linux/src/linux_hal_common.cpp:1061-1117` | AF_PACKET + PTP multicast join |

## Example daemon outline

```
examples/gptp_slave_linux.cpp:
  - Parse CLI args (interface name, profile, optional manual peer delay)
  - Open raw socket, bind, enable SIOCSHWTSTAMP, SO_TIMESTAMPING
  - Open PHC via ethtool TS info
  - Build GptpClockOps from factory
  - Build GptpConfig from args (standard_defaults or avnu_automotive)
  - Create GptpSlavePort
  - Subscribe observer that prints sync state to stderr
  - Event loop: poll(raw_fd, timeout=next_deadline)
    - on readable: recvmsg, extract RX cmsg timestamp, receive_frame
    - on timeout: tick(now)
  - On SIGTERM: port.stop(), close fds
```

## Testing plan

1. **Unit tests** (macOS, already done): SoftwareOps mock drives the
   full GptpSlavePort through Sync+FollowUp+Pdelay and verifies
   observer callbacks, servo convergence, and state transitions.

2. **Integration test on Linux host**: run the example daemon on a
   NIC with PHC support (e.g., Intel i210/i225, or a Raspberry Pi 5
   with the RPi Ethernet MAC's HW timestamping). Verify:
   - PHC time converges to grandmaster time within ±1 µs
   - Frequency adjustment stays within ±100 ppb at steady state
   - Automotive Profile with Pdelay disabled + manual peer delay works
   - Link down / link up recovery re-acquires sync

3. **Interop test**: run against `ptp4l` as grandmaster on the same
   LAN segment and verify convergence matches `ptp4l`'s own slave
   implementation.
