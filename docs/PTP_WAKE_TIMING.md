<!-- Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com> -->


# PTP Wake Timing Guide

This document covers the `ptpclient_wake_example` tool and performance tuning recommendations for achieving sub-microsecond precision wake timing using PTP hardware clocks on Linux.

## Overview

The PTP Wake Example demonstrates precision wake timing by:
1. Reading the PTP hardware clock via `/dev/ptp*` devices
2. Fitting PTP time against the local timing reference, `CLOCK_MONOTONIC_RAW`, with a continuously sampled linear mapping (`PtpTimeBridge`)
3. Waking at precise PTP-synchronized times: each PTP deadline becomes a `CLOCK_MONOTONIC_RAW` deadline, the thread sleeps early on `CLOCK_MONOTONIC` through the RAW↔MONOTONIC line (`realtime::MonotonicRawLine`, the only clock `clock_nanosleep()` can target), then spins on RAW to the exact instant
4. Telling the wake to the callback on both clocks (`TimerWakeInfo`: PTP nanoseconds and the typed RAW instant), so nothing the callback runs reads a clock

## Command Line Usage

```bash
Usage: ptpclient_wake_example [options]

Options:
  --ptp.driver=CHOICE   PTP driver (linuxptp or system)
                        (default: linuxptp on Linux, system on macOS)
  --ptp.device=FILE     PTP device path (default: /dev/ptp0)
  --ptp.period=INTEGER  Wake period in nanoseconds (default: 1000000 = 1ms)
  --ptp.compensation=INTEGER
                        Compensation offset in ns (default: 0, negative = wake earlier)
  --ptp.cpu=INTEGER     CPU core affinity (default: 3)
  --ptp.no-rt           Disable realtime priority
  --ptp.no-mlock        Disable memory locking
  --ptp.quiet           Suppress verbose output
  --help                Show help message
  --config-load=FILE    Load configuration from TOML file
  --config-save=FILE    Save current configuration to TOML file
```

### Examples

```bash
# Basic usage with defaults (1ms period, no compensation, with RT priority)
sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0

# 1ms period with -3µs compensation to account for wake overhead
sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0 \
    --ptp.period=1000000 --ptp.compensation=-3000

# Without realtime priority (for testing without CAP_SYS_NICE)
sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.period=1000000 --ptp.compensation=-3000 --ptp.no-rt

# 500µs period (2kHz wake rate)
sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.period=500000 --ptp.compensation=-3000

# Using a TOML configuration file
sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --config-load=ptp_config.toml
```

### Configuration File Example

Settings can be stored in a TOML file:
```toml
[ptp]
driver = "linuxptp"
device = "/dev/ptp0"
period = 1000000
compensation = -3000
cpu = 3
no-rt = false
```

## Prerequisites

### PTP Hardware Clock Setup

The PTP hardware clock must be synchronized before use:

```bash
# Check PTP device exists
ls -la /dev/ptp*

# Read current PHC time
sudo phc_ctl /dev/ptp0 get

# Sync system clock to PHC (if PHC is authoritative)
sudo phc2sys -s /dev/ptp0 -c CLOCK_REALTIME -O 0

# Or sync PHC to system clock (if system clock is authoritative)
sudo phc2sys -s CLOCK_REALTIME -c /dev/ptp0 -O 0
```

### Required Capabilities

For realtime priority (SCHED_FIFO) and memory locking, the program needs either:
- Root privileges (`sudo`)
- `CAP_SYS_NICE` capability (for SCHED_FIFO)
- `CAP_IPC_LOCK` capability (for mlockall)

## Performance Tuning

### Measured Results

On a Raspberry Pi 5 with stock PREEMPT kernel (6.12.47), SCHED_FIFO, and `mlockall()`:

| Configuration | Average Error | Std Deviation | Max Error |
|--------------|---------------|---------------|-----------|
| No tuning (no RT, no compensation) | ~3-5 µs | ~1-2 µs | ~20 µs |
| SCHED_FIFO + mlockall, no load | -81 ns | 542 ns | 119 µs |
| SCHED_FIFO + mlockall, heavy build | +7.1 µs | 6.8 µs | 5 ms |
| **isolcpus + SCHED_FIFO + mlockall, no load** | **-493 ns** | **460 ns** | **6.3 µs** |
| **isolcpus + SCHED_FIFO + mlockall, heavy build** | **-43 ns** | **1.1 µs** | **39 µs** |

The `isolcpus` option provides the most dramatic improvement under load, reducing max latency from 5ms to 39µs (128x improvement).

### Compensation Value

The compensation parameter accounts for the overhead between the target wake time and when your code actually runs. To find the optimal value:

1. Run first without compensation to measure average error:
   ```bash
   sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0 \
       --ptp.period=1000000 --ptp.compensation=0
   ```

2. Note the "Average error" from the output (e.g., +3000 ns)

3. Apply compensation to subtract this offset:
   ```bash
   sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0 \
       --ptp.period=1000000 --ptp.compensation=-3000
   ```

4. The program will suggest an optimal compensation value on exit if the average error is significant.

### Memory Locking (mlockall)

The program automatically calls `mlockall(MCL_CURRENT | MCL_FUTURE)` to lock all memory pages. This prevents page faults during the realtime wake loop, which can cause multi-millisecond latency spikes under memory pressure (e.g., during heavy builds).

Without memory locking, running a parallel build caused 5-7ms max latency spikes. With `mlockall()`, the same load produced only 39µs max latency (when combined with `isolcpus`).

### CPU Isolation (isolcpus)

Dedicate a CPU core exclusively to the wake timing process by preventing the scheduler from running other tasks on it. This is the **most important optimization** for handling system load.

1. Edit boot parameters (Raspberry Pi: `/boot/firmware/cmdline.txt`):
   ```
   isolcpus=3
   ```

2. Reboot and verify:
   ```bash
   cat /sys/devices/system/cpu/isolated
   # Should show: 3
   ```

3. Run the program on the isolated core (use `--ptp.cpu=3` for affinity, taskset optional):
   ```bash
   sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0 \
       --ptp.period=1000000 --ptp.compensation=-2900 --ptp.cpu=3
   ```

**Note**: `taskset` pins the entire process to the specified CPU. The PtpTimeBridge sampling thread will also run on this CPU. For optimal performance, the sampling thread could be moved to a non-isolated CPU, but in practice the current design works well since the sampling thread is lightweight.

### IRQ Affinity

Move interrupts away from the isolated CPU to reduce jitter:

```bash
# List IRQs and their CPU affinity
cat /proc/interrupts

# Move all IRQs away from CPU 3 (set affinity to CPUs 0-2)
for irq in /proc/irq/*/smp_affinity; do
    echo 7 | sudo tee $irq 2>/dev/null
done
```

### PREEMPT_RT Kernel

For the tightest timing bounds, use a fully preemptible kernel:

- **Raspberry Pi**: PREEMPT kernel is available by default
- **Desktop Linux**: Install `linux-image-rt-*` packages or build with `CONFIG_PREEMPT_RT`

Check your kernel's preemption model:
```bash
cat /proc/version
# Look for "PREEMPT" or "PREEMPT_RT"

# Or check config
zcat /proc/config.gz | grep PREEMPT
```

### Complete High-Performance Setup

For best results, combine all optimizations:

```bash
# Boot with isolated CPU
# /boot/firmware/cmdline.txt: ... isolcpus=3 nohz_full=3

# After boot, set IRQ affinity
for irq in /proc/irq/*/smp_affinity; do
    echo 7 | sudo tee $irq 2>/dev/null
done

# Run on isolated core with RT priority and compensation
sudo ./build/build-Debug/statusbar/ptpclient/ptpclient_wake_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0 \
    --ptp.period=1000000 --ptp.compensation=-2900 --ptp.cpu=3
```

### Optimization Summary

| Optimization | Effect | Improvement |
|-------------|--------|-------------|
| `mlockall()` | Prevents page fault latency | Eliminates multi-ms spikes from memory pressure |
| `SCHED_FIFO` | Realtime scheduling priority | Reduces average latency and jitter |
| `isolcpus` | Dedicated CPU core | **128x reduction** in max latency under load |
| IRQ affinity | Move interrupts away | Further reduces occasional spikes |
| `nohz_full` | Tickless kernel on isolated CPU | Eliminates timer tick interrupts |
| PREEMPT_RT kernel | Fully preemptible kernel | Tightest latency bounds |

For most applications, `mlockall()` + `SCHED_FIFO` + `isolcpus` provides excellent results (sub-40µs worst case under heavy load) without requiring a custom kernel.

## Interpreting Output

### Startup Phase

```
PTP Wake Example
  Driver: linuxptp
  Device: /dev/ptp0
  Period: 1000000 ns (1.000 ms)
  Compensation: -3000 ns (-3.000 us)
  Realtime priority: enabled
  PTP client opened successfully
  Current PTP time:       1735689600123456789 ns (1735689600.123 seconds)
  Current MONOTONIC_RAW:  12345678901234 ns (12345.679 seconds)
  Difference (PTP-raw):   1735677254444555555 ns
  Bridge sampling started
  Waiting for bridge to become healthy...
```

### Running Statistics

```
Wakes:     1000  Avg:    +27.1 ns  Min:   -1536 ns  Max:  +10752 ns  StdDev:   608.5 ns
```

- **Avg**: Average error (positive = late, negative = early)
- **Min/Max**: Worst-case timing bounds
- **StdDev**: Standard deviation (lower = more consistent)

### Final Report

```
=== Final Wake Statistics ===
  Total wakes:     60772
  Compensation:    -3000 ns (-3.000 us)
  Average error:   +27.1 ns (+0.027 us)
  Minimum error:   -1536 ns (-1.536 us)
  Maximum error:   +10752 ns (+10.752 us)
  Std deviation:   608.5 ns (0.608 us)

  Suggested compensation: -3027 ns (-3.027 us)
```

## Troubleshooting

### "PTP hardware clock appears uninitialized"

The PHC time is near zero, indicating it hasn't been synchronized:
```bash
sudo phc2sys -s CLOCK_REALTIME -c /dev/ptp0 -O 0
```

### "Could not set realtime priority"

Run with `sudo` or grant `CAP_SYS_NICE`:
```bash
sudo setcap cap_sys_nice+ep ./ptpclient_wake_example
```

Or use `--ptp.no-rt` to run without realtime priority.

### "Could not lock memory"

Run with `sudo` or grant `CAP_IPC_LOCK`:
```bash
sudo setcap cap_ipc_lock+ep ./ptpclient_wake_example
```

Memory locking is strongly recommended to prevent page fault latency spikes.

### High jitter despite tuning

Check for:
- Background processes (`top`, `htop`)
- Thermal throttling (`vcgencmd measure_temp` on RPi)
- Power management (`cpupower frequency-set -g performance`)
- Competing IRQs on the same CPU

## Implementation Notes

### Clock Types

The bridge's reference clock is `CLOCK_MONOTONIC_RAW`, the timing reference
for all of statusbar (core's [`TIME_POLICY.md`](../../core/docs/TIME_POLICY.md)):
the raw hardware counter, immune to the frequency slew NTP or phc2sys apply
to `CLOCK_MONOTONIC`. The PHC is fitted against RAW, deadlines are RAW, and
wake error is measured on RAW.

`clock_nanosleep()` cannot target `CLOCK_MONOTONIC_RAW`, so a sleep goes
through the RAW↔MONOTONIC line (`realtime::MonotonicRawLine`,
`monotonic = rate * raw + offset`, published by the bridge's sampler): the
RAW deadline becomes a coarse `CLOCK_MONOTONIC` target, the thread wakes
early by a guard sized from the line's rate, then spins on RAW to the
deadline. `CLOCK_MONOTONIC` is for sleeping, never for timing.

The startup lines above print one `CLOCK_MONOTONIC_RAW` reading and the
PTP-minus-RAW difference for orientation; the mapping the timer runs on is
the continuously sampled RAW fit.

### The wake is told on both clocks

`TimerWakeInfo` carries `scheduled_time_ns` / `actual_time_ns` / `error_ns`
in PTP nanoseconds and `actual_raw`, the same wake as a typed
`realtime::MonotonicRawClock::time_point`. A callback tells PTP time to what
stamps on PTP (AVTP presentation) and the RAW instant to what stamps on RAW
(an RX drain, a transmit gate); it reads no clock of its own. See
[`TIME_POLICY.md`](../../core/docs/TIME_POLICY.md) for the rule.

### FD_TO_CLOCKID

The Linux kernel converts PTP device file descriptors to clock IDs using:
```c
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | CLOCKFD)
// where CLOCKFD = 3
```

This allows `clock_gettime()` to read the PTP hardware clock directly.

### Linear Regression Mapping

The PtpTimeBridge maintains a linear mapping between the PHC and
`CLOCK_MONOTONIC_RAW`:
```
ptp_ns = raw_ns * rate + offset
```

The mapping is continuously updated using samples to track clock drift.
`to_raw_ns()` / `from_raw_ns()` are the primary conversions; the
`to_monotonic_ns()` / `from_monotonic_ns()` pair goes through the
RAW↔MONOTONIC line as well and exists for sleeping.
