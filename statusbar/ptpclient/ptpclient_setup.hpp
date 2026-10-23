#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP Client Setup Utilities
/// High-level setup functions for PTP client and bridge initialization
/// Provides diagnostic output and error handling for common setup tasks

#include "statusbar/config/config.hpp"
#include "statusbar/ptpclient/ptpclient_base.hpp"
#include "statusbar/ptpclient/ptpclient_bridge.hpp"
#include "statusbar/ptpclient/ptpclient_linuxptp.hpp"
#include "statusbar/ptpclient/ptpclient_ntpshm.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace statusbar::ptpclient {

using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;

/// Result of PTP client and bridge setup
struct PtpSetupResult
{
    std::unique_ptr<PtpClientBase> client;
    std::unique_ptr<PtpTimeBridge> bridge;

    // Make move-constructible for StatusValue
    ~PtpSetupResult() = default;
    PtpSetupResult() = default;
    PtpSetupResult(PtpSetupResult&&) noexcept = default;
    auto operator=(PtpSetupResult&&) noexcept -> PtpSetupResult& = default;
    PtpSetupResult(PtpSetupResult const&) = delete;
    auto operator=(PtpSetupResult const&) -> PtpSetupResult& = delete;
};

/// Configuration for PTP setup
struct PtpSetupConfig
{
#if defined(__linux__)
    std::string_view driver_name = "linuxptp";
    std::string_view device_path = "/dev/ptp0";
#else
    std::string_view driver_name = "system";
    std::string_view device_path = "system_clock";
#endif
    bool verbose = true;  // Print diagnostic messages
};

/// Format PTP client information for diagnostics
/// @param out Output iterator
/// @param client The PTP client to describe
/// @return Updated output iterator
template <typename OutputIt>
auto format_client_info_to(OutputIt out, PtpClientBase const& client) -> OutputIt
{
#if defined(__linux__)
    if (auto const* linux_client = dynamic_cast<LinuxPtpClient const*>(&client)) {
        out = std::format_to(
            out,
            "  FD: {}, clock_id: {} (0x{:x})\n",
            linux_client->fd(),
            linux_client->clock_id(),
            static_cast<unsigned>(linux_client->clock_id()));
    }
#else
    (void)client;
#endif
    return out;
}

/// Format current clock times for diagnostics
/// @param out Output iterator
/// @param client The PTP client to read from
/// @return Updated output iterator
template <typename OutputIt>
auto format_clock_times_to(OutputIt out, PtpClientBase& client) -> OutputIt
{
    auto ptp_now = client.get_time_ns();
    if (ptp_now) {
        out = std::format_to(
            out, "  Current PTP time:       {} ns ({:.3f} seconds)\n", *ptp_now, static_cast<double>(*ptp_now) / 1e9);
    } else {
        out = std::format_to(out, "  Error reading PTP time: {}\n", ptp_now.error().message());
    }

    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t const mono_now = (ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec;
    out = std::format_to(out, "  Current MONOTONIC:      {} ns ({:.3f} seconds)\n", mono_now, static_cast<double>(mono_now) / 1e9);

    if (ptp_now) {
        int64_t const diff = *ptp_now - mono_now;
        out = std::format_to(out, "  Difference (PTP-mono):  {} ns ({:.3f} seconds)\n", diff, static_cast<double>(diff) / 1e9);
    }

    return out;
}

/// Check if PTP clock appears uninitialized and format warning
/// @param out Output iterator
/// @param client The PTP client to check
/// @return Updated output iterator, warning text added if clock is uninitialized
template <typename OutputIt>
auto format_clock_warning_to(OutputIt out, PtpClientBase& client) -> OutputIt
{
    auto ptp_now = client.get_time_ns();
    if (ptp_now && *ptp_now < 1'000'000'000LL) {  // Less than 1 second
        out = std::format_to(out, "\n  WARNING: PTP hardware clock appears uninitialized (time near 0)!\n");
        out = std::format_to(out, "  The PTP hardware clock needs to be synchronized before the bridge can work.\n");
        out = std::format_to(out, "  Common solutions:\n");
        out = std::format_to(out, "    1. Run phc2sys to sync hardware clock: sudo phc2sys -s CLOCK_REALTIME -c /dev/ptp0 -O 0\n");
        out = std::format_to(out, "    2. Or sync system to hardware clock:   sudo phc2sys -s /dev/ptp0 -c CLOCK_REALTIME -O 0\n");
        out = std::format_to(out, "    3. Check ptp4l config uses hardware timestamping\n");
        out = std::format_to(out, "    4. Verify with: sudo phc_ctl /dev/ptp0 get\n\n");
    }
    return out;
}

/// Format bridge mapping status for diagnostics
/// @param out Output iterator
/// @param mapping The current mapping from the bridge
/// @return Updated output iterator
template <typename OutputIt>
auto format_mapping_status_to(OutputIt out, TimeMapping const& mapping) -> OutputIt
{
    return std::format_to(
        out,
        "    samples={}, rms={}ns, bracket={}ns, epoch={}, rate={:.9f}, offset={}ns\n",
        mapping.sample_count,
        mapping.rms_residual_ns,
        mapping.worst_bracket_ns,
        mapping.epoch,
        mapping.rate,
        mapping.offset_ns);
}

/// Create and open a PTP client
/// @param config Setup configuration
/// @return PTP client on success, or error
[[nodiscard]] auto create_client(PtpSetupConfig const& config) -> StatusValue<std::unique_ptr<PtpClientBase>>;

/// Create PTP client and bridge with full diagnostic output
/// @param config Setup configuration
/// @return PtpSetupResult containing client and bridge on success
[[nodiscard]] auto setup_ptp(PtpSetupConfig const& config) -> StatusValue<PtpSetupResult>;

/// Wait for bridge to become healthy with optional progress output
/// @param bridge The bridge to wait for
/// @param is_shutdown Callable returning true when shutdown is requested
/// @param progress_callback Optional callback for progress updates (receives TimeMapping)
/// @param timeout Maximum time to wait (0 = no timeout)
/// @return true if bridge became healthy, false if shutdown or timeout
template <typename ShutdownCheck>
[[nodiscard]] auto wait_for_healthy(
    PtpTimeBridge& bridge,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) - is_shutdown is called multiple times in loop
    ShutdownCheck&& is_shutdown,
    statusbar::sg14::inplace_function<void(TimeMapping const&), 64> const& progress_callback = nullptr,
    std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) -> bool
{
    auto const start = std::chrono::steady_clock::now();
    int wait_count = 0;

    while (!is_shutdown()) {
        if (bridge.is_healthy()) {
            return true;
        }

        // Check timeout
        if (timeout.count() > 0) {
            auto const elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                return false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++wait_count;

        // Call progress callback every second
        if (progress_callback && wait_count % 10 == 0) {
            progress_callback(bridge.get_mapping());
        }
    }

    return false;  // Shutdown requested
}

//
// PTP Application Configuration and Context
//

/// Configuration for a complete PTP application
/// Combines device, timer, sampling, and runtime settings
struct PtpAppConfig
{
    // PTP device configuration
#if defined(__linux__)
    std::string driver_name = "linuxptp";
    std::string device_path = "/dev/ptp0";
#else
    std::string driver_name = "system";
    std::string device_path = "system_clock";
#endif

    // Timer configuration
    int64_t period_ns = 1'000'000;  // 1ms default
    int64_t compensation_ns = 0;
    int cpu_affinity = 3;
    bool enable_realtime = true;
    int64_t threshold = 50'000;

    // Sampling parameters
    BridgeSamplingParams sampling{};

    // Behavior flags
    bool verbose = true;
    bool lock_memory = true;
};

/// Ready-to-use PTP application context
/// Contains initialized client, bridge, and active sampling guard
struct PtpAppContext
{
    std::unique_ptr<PtpClientBase> client;
    std::unique_ptr<PtpTimeBridge> bridge;
    SamplingGuard guard;

    // Configuration values for timer creation
    int64_t period_ns{1'000'000};
    int64_t compensation_ns{0};
    int cpu_affinity{3};
    bool enable_realtime{true};
    int64_t threshold{50'000};

    // Move only
    ~PtpAppContext() = default;
    PtpAppContext() = default;
    PtpAppContext(PtpAppContext&&) noexcept = default;
    auto operator=(PtpAppContext&&) noexcept -> PtpAppContext& = default;
    PtpAppContext(PtpAppContext const&) = delete;
    auto operator=(PtpAppContext const&) -> PtpAppContext& = delete;
};

/// Ready-to-use gPTP context with clock adapter
/// Contains initialized PTP context plus a ClockAdapter for a specific gPTP domain.
/// The adapter references the bridge owned by this context.
///
/// @tparam DomainId The gPTP domain ID (typically 0)
template <int DomainId = 0>
struct GptpContext
{
    using Clock = realtime::GptpClock<DomainId>;
    using Adapter = realtime::ClockAdapter<Clock, PtpTimeBridge>;

    PtpAppContext ptp;  ///< Underlying PTP context (owns client, bridge, guard)
    Adapter adapter;    ///< Clock adapter referencing ptp.bridge

    /// Construct from a PtpAppContext, creating the adapter
    /// @param ctx PTP application context to take ownership of
    explicit GptpContext(PtpAppContext&& ctx)
        : ptp{std::move(ctx)}
        , adapter{*ptp.bridge}
    {}

    // Move only - adapter holds pointer to bridge, so moves must update it
    GptpContext(GptpContext&& other) noexcept
        : ptp{std::move(other.ptp)}
        , adapter{*ptp.bridge}  // Re-bind adapter to our bridge after move
    {}

    auto operator=(GptpContext&& other) noexcept -> GptpContext&
    {
        if (this != &other) {
            ptp = std::move(other.ptp);
            adapter = Adapter{*ptp.bridge};  // Re-bind adapter to our bridge
        }
        return *this;
    }

    ~GptpContext() = default;
    GptpContext(GptpContext const&) = delete;
    auto operator=(GptpContext const&) -> GptpContext& = delete;
};

/// Load PTP application configuration from config::Config
/// Extracts common PTP options with "ptp." prefix:
///   --ptp.driver, --ptp.device, --ptp.period, --ptp.compensation, --ptp.cpu, --ptp.no-rt
/// @param toml_config The parsed TOML configuration
/// @return Populated PtpAppConfig with values from config or defaults
[[nodiscard]] auto load_ptp_app_config(config::Config const& toml_config) -> PtpAppConfig;

/// Build argument specifications for PTP CLI options with lambda binding
/// The Config type must have PtpAppConfig fields (driver_name, device_path, period_ns, etc.)
/// or be a class that inherits from / embeds PtpAppConfig.
/// @tparam Config Configuration struct with PTP fields
/// @param specs The ArgumentSpecs collection to add PTP options to
/// @param config Reference to configuration struct whose fields are bound to the options
/// @return Reference to specs for chaining
template <typename Config>
auto add_ptp_arg_specs(args::ArgumentSpecs& specs, Config& config) -> args::ArgumentSpecs&
{
#if defined(__linux__)
    specs.add_choice("ptp.driver", "PTP driver", {"linuxptp", "ntpshm", "system"}, "linuxptp", [&](std::string_view v) -> void {
        config.driver_name = std::string{v};
    });
    specs.add<std::string_view>(
        "ptp.device", "PTP device path", "/dev/ptp0", [&](std::string_view v) -> void { config.device_path = std::string{v}; });
#else
    specs.add_choice(
        "ptp.driver", "PTP driver", {"system"}, "system", [&](std::string_view v) -> void { config.driver_name = std::string{v}; });
    specs.add<std::string_view>("ptp.device", "Device name for display", "system_clock", [&](std::string_view v) -> void {
        config.device_path = std::string{v};
    });
#endif
    specs.add<int64_t>("ptp.period", "Wake period in nanoseconds", 1'000'000, [&](int64_t v) -> void { config.period_ns = v; });
    specs.add<int64_t>("ptp.compensation", "Compensation offset in ns (negative = wake earlier)", 0, [&](int64_t v) -> void {
        config.compensation_ns = v;
    });
    specs.add<int64_t>(
        "ptp.cpu", "Timer thread CPU core affinity", 3, [&](int64_t v) -> void { config.cpu_affinity = static_cast<int>(v); });
    specs.add_flag("ptp.no-rt", "Disable realtime priority", [&](bool v) -> void { config.enable_realtime = !v; });
    specs.add_flag("ptp.no-mlock", "Disable memory locking", [&](bool v) -> void { config.lock_memory = !v; });
    specs.add_flag("ptp.quiet", "Suppress verbose output", [&](bool v) -> void { config.verbose = !v; });
    specs.add<int64_t>("ptp.threshold", "PTP sync threshold ns", 50'000, [&](int64_t v) -> void { config.threshold = v; });
    // Busy-spin window before each wake. The timer clock_nanosleep()s to ~this far
    // before the deadline then busy-polls the rest for sub-us precision. Spinning
    // ~40% of every 125us period is heat-intensive; set 0 to drop the spin (just
    // clock_nanosleep, looser wakes, far lower CPU/heat -- useful on thermally
    // marginal nodes). The default 50us covers kernel work stalling a shared
    // core; with the timer's core isolated (isolcpus/nohz_full, IRQs and
    // kworkers off it, deep C-states disabled) 15000-20000 is enough.
    specs.add<int64_t>(
        "ptp.spin_ns",
        "Busy-spin refinement window before each wake in ns (0 = no busy-wait)",
        config.sampling.wake_refine_spin_ns,
        [&](int64_t v) -> void { config.sampling.wake_refine_spin_ns = v; });
    return specs;
}

/// Print standard PTP CLI options help text
/// @tparam OutputIt Output iterator type
/// @param out Output iterator to write help text to
/// @return Updated output iterator
template <typename OutputIt>
auto format_ptp_options_help_to(OutputIt out) -> OutputIt
{
    out = std::format_to(out, "PTP Options:\n");
#if defined(__linux__)
    out = std::format_to(out, "  --ptp.driver=NAME       PTP driver: linuxptp, ntpshm, system (default: linuxptp)\n");
    out = std::format_to(out, "  --ptp.device=PATH       PTP device path (default: /dev/ptp0)\n");
#else
    out = std::format_to(out, "  --ptp.driver=NAME       PTP driver: system (default: system)\n");
    out = std::format_to(out, "  --ptp.device=PATH       Device name for display (default: system_clock)\n");
#endif
    out = std::format_to(out, "  --ptp.period=NS         Wake period in nanoseconds (default: 1000000 = 1ms)\n");
    out = std::format_to(out, "  --ptp.compensation=NS   Compensation offset in ns (default: 0, negative = wake earlier)\n");
    out = std::format_to(out, "  --ptp.cpu=N             CPU core affinity (default: 3)\n");
    out = std::format_to(out, "  --ptp.no-rt             Disable realtime priority\n");
    out = std::format_to(out, "  --ptp.no-mlock          Disable memory locking\n");
    out = std::format_to(out, "  --ptp.quiet             Suppress verbose output\n");
    return out;
}

/// Print PTP configuration summary
/// @param out Output iterator
/// @param config The configuration to summarize
/// @param title Optional title (printed first if non-empty)
/// @return Updated output iterator
template <typename OutputIt>
auto format_ptp_config_to(OutputIt out, PtpAppConfig const& config, std::string_view title = "") -> OutputIt
{
    if (!title.empty()) {
        out = std::format_to(out, "{}\n", title);
    }
    out = std::format_to(out, "  Driver: {}\n", config.driver_name);
    out = std::format_to(out, "  Device: {}\n", config.device_path);
    out = std::format_to(out, "  Period: {} ns ({:.3f} ms)\n", config.period_ns, static_cast<double>(config.period_ns) / 1e6);
    out = std::format_to(
        out, "  Compensation: {} ns ({:+.3f} us)\n", config.compensation_ns, static_cast<double>(config.compensation_ns) / 1000.0);
    out = std::format_to(out, "  Realtime priority: {}\n", config.enable_realtime ? "enabled" : "disabled");
    out = std::format_to(out, "  CPU affinity: {}\n", config.cpu_affinity);
    return out;
}

/// Setup complete PTP application context
/// Creates client, bridge, starts sampling, and waits for bridge to become healthy.
/// Prints diagnostic output if verbose is enabled.
///
/// @param config Application configuration
/// @param is_shutdown Callable returning true when shutdown is requested
/// @return Ready-to-use PtpAppContext or error
template <typename ShutdownCheck>
// NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) - is_shutdown is called multiple times, not forwarded
[[nodiscard]] auto setup_ptp_app(PtpAppConfig const& config, ShutdownCheck&& is_shutdown) -> StatusValue<PtpAppContext>
{
    // Create PTP client
    PtpSetupConfig setup_config;
    setup_config.driver_name = config.driver_name;
    setup_config.device_path = config.device_path;
    setup_config.verbose = config.verbose;

    auto client_result = create_client(setup_config);
    if (!client_result) {
        return forward_failure(client_result);
    }

    PtpAppContext ctx;
    ctx.client = std::move(*client_result);
    ctx.bridge = std::make_unique<PtpTimeBridge>();
    ctx.period_ns = config.period_ns;
    ctx.compensation_ns = config.compensation_ns;
    ctx.cpu_affinity = config.cpu_affinity;
    ctx.enable_realtime = config.enable_realtime;
    ctx.threshold = config.threshold;

    if (config.verbose) {
        std::print("  PTP client opened successfully\n");

        // Print client info
        std::string info;
        format_client_info_to(std::back_inserter(info), *ctx.client);
        if (!info.empty()) {
            std::print("{}", info);
        }

        // Print clock times
        std::string times;
        format_clock_times_to(std::back_inserter(times), *ctx.client);
        std::print("{}", times);

        // Check for uninitialized clock warning
        std::string warning;
        format_clock_warning_to(std::back_inserter(warning), *ctx.client);
        if (!warning.empty()) {
            std::print("{}", warning);
        }
    }

    // Start sampling
    auto guard_result = ctx.bridge->start_sampling(*ctx.client, config.sampling);
    if (!guard_result) {
        return forward_failure(guard_result);
    }
    ctx.guard = std::move(*guard_result);

    if (config.verbose) {
        std::print("  Bridge sampling started\n");
        std::print("  Waiting for bridge to become healthy...\n");
    }

    // Wait for bridge to become healthy
    auto progress = config.verbose
        ? statusbar::sg14::inplace_function<void(TimeMapping const&), 64>([](TimeMapping const& m) -> void {
              std::string status;
              format_mapping_status_to(std::back_inserter(status), m);
              std::print("{}", status);
          })
        : nullptr;

    if (!wait_for_healthy(*ctx.bridge, is_shutdown, progress)) {
        if (is_shutdown()) {
            // Shutdown requested - not an error, but caller should check
            return failure(PtpError::bridge_not_healthy);
        }
        return failure(PtpError::bridge_not_healthy);
    }

    if (config.verbose) {
        auto mapping = ctx.bridge->get_mapping();
        std::print("  Bridge healthy: {} samples, RMS residual {} ns\n", mapping.sample_count, mapping.rms_residual_ns);
    }

    // Lock memory if requested
    if (config.enable_realtime && config.lock_memory) {
        if (realtime::lock_memory()) {
            if (config.verbose) {
                std::print("  Memory locked (mlockall) successfully\n");
            }
        } else {
            if (config.verbose) {
                std::print("  Warning: Could not lock memory (run as root or with CAP_IPC_LOCK)\n");
            }
        }
    }

    if (config.verbose && config.enable_realtime) {
        std::print("  Realtime priority will be set on timer thread\n");
    } else if (config.verbose) {
        std::print("  Realtime priority disabled\n");
    }

    return success(std::move(ctx));
}

/// Setup PTP application with automatic fallback to system clock
/// If the configured PTP driver fails (e.g., /dev/ptp0 not found), automatically
/// falls back to using the system clock. Updates config in-place to reflect the
/// actual driver used.
///
/// @param config Application configuration (may be modified on fallback)
/// @param is_shutdown Callable returning true when shutdown is requested
/// @return Ready-to-use PtpAppContext or error
template <typename ShutdownCheck>
[[nodiscard]] auto setup_ptp_app_with_fallback(PtpAppConfig& config, ShutdownCheck&& is_shutdown) -> StatusValue<PtpAppContext>
{
    auto ctx_result = setup_ptp_app(config, is_shutdown);

    if (!ctx_result) {
        // If shutdown was requested, propagate the error
        if (is_shutdown()) {
            return ctx_result;
        }

        // If we weren't already using the system clock, fall back to it
        if (config.driver_name != "system") {
            if (config.verbose) {
                std::print(stderr, "Warning: PTP setup failed ({}), falling back to system clock\n", ctx_result.error().message());
            }
            config.driver_name = "system";
            config.device_path = "system_clock";
            ctx_result = setup_ptp_app(config, std::forward<ShutdownCheck>(is_shutdown));
        }
    }

    return ctx_result;
}

/// Setup gPTP application context with clock adapter
/// Creates PTP client, bridge, waits for healthy, and creates a ClockAdapter
/// for the specified gPTP domain.
///
/// @tparam DomainId The gPTP domain ID (default 0)
/// @param config Application configuration
/// @param is_shutdown Callable returning true when shutdown is requested
/// @return Ready-to-use GptpContext or error
template <int DomainId = 0, typename ShutdownCheck>
[[nodiscard]] auto setup_gptp_app(PtpAppConfig const& config, ShutdownCheck&& is_shutdown) -> StatusValue<GptpContext<DomainId>>
{
    auto ptp_result = setup_ptp_app(config, std::forward<ShutdownCheck>(is_shutdown));
    if (!ptp_result) {
        return failure(ptp_result.error());
    }

    return success(GptpContext<DomainId>{std::move(*ptp_result)});
}

}  // namespace statusbar::ptpclient
