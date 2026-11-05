// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#if defined(__linux__)

#    include "statusbar/gptp/gptp_slave_session.hpp"

#    include "statusbar/gptp/gptp_base_format.hpp"
#    include "statusbar/tsn/tsn.hpp"

#    include <poll.h>
#    include <unistd.h>

#    include <chrono>
#    include <cstring>
#    include <iterator>
#    include <print>
#    include <utility>

namespace statusbar::gptp {

namespace {

[[nodiscard]] auto bridge_clock_name_for(clockid_t clk) -> std::string
{
    if (clk == CLOCK_MONOTONIC) {
        return "CLOCK_MONOTONIC";
    }
    if (clk == CLOCK_MONOTONIC_RAW) {
        return "CLOCK_MONOTONIC_RAW";
    }
    if (clk == CLOCK_REALTIME) {
        return "CLOCK_REALTIME";
    }
    return {};
}

[[nodiscard]] auto build_gptp_config(SlaveSessionConfig const& cfg, ieee::Eui48 mac) -> GptpConfig
{
    GptpConfig out =
        (cfg.profile == Profile::AvnuAutomotive) ? GptpConfig::avnu_automotive_slave_defaults() : GptpConfig::standard_defaults();
    out.local_clock_identity = tsn::ClockIdentity::from_eui48(mac);
    if (cfg.phase_jump_threshold_ns >= 0) {
        out.servo_phase_jump_threshold_ns = cfg.phase_jump_threshold_ns;
    }
    if (cfg.servo_ki >= 0.0) {
        out.servo_integral_gain = cfg.servo_ki;
    }
    if (cfg.servo_kp >= 0.0) {
        out.servo_proportional_gain = cfg.servo_kp;
    }
    if (cfg.manual_peer_delay_ns >= 0) {
        out.pdelay_mode = PdelayMode::Disabled;
        out.manual_peer_delay_ns = cfg.manual_peer_delay_ns;
    }
    return out;
}

void print_interface_banner(std::string const& iface, int if_index, ieee::Eui48 mac)
{
    std::println(
        stderr,
        "Interface: {} (index {}) MAC: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        iface,
        if_index,
        mac.value[0],
        mac.value[1],
        mac.value[2],
        mac.value[3],
        mac.value[4],
        mac.value[5]);
}

void print_running_banner(SlaveSessionConfig const& cfg)
{
    std::println(
        stderr,
        "gPTP slave running on {} (profile: {}, timestamping: {}). Ctrl-C to stop.",
        cfg.interface,
        cfg.profile == Profile::AvnuAutomotive ? "automotive" : "standard",
        cfg.software_timestamping ? "software" : "hardware");
}

}  // namespace

SlaveSession::SlaveSession(SlaveSessionConfig cfg)
    : cfg_{std::move(cfg)}
    , bridge_clock_name_{bridge_clock_name_for(cfg_.bridge_clock)}
{}

SlaveSession::~SlaveSession()
{
    if (phc_fd_ >= 0) {
        ::close(phc_fd_);
        phc_fd_ = -1;
    }
    if (raw_fd_ >= 0) {
        ::close(raw_fd_);
        raw_fd_ = -1;
    }
}

auto SlaveSession::start() -> bool
{
    raw_fd_ = open_raw_gptp_socket();
    if (raw_fd_ < 0) {
        return false;
    }

    auto const ifinfo = get_interface_info(raw_fd_, cfg_.interface.c_str());
    if (ifinfo.if_index < 0) {
        return false;
    }
    mac_ = ifinfo.mac;
    if_index_ = ifinfo.if_index;
    poll_fd_storage_[0] = raw_fd_;

    print_interface_banner(cfg_.interface, if_index_, mac_);

    if (!bind_gptp_socket(raw_fd_, if_index_)) {
        return false;
    }

    if (cfg_.software_timestamping) {
        if (!enable_so_timestamping_sw(raw_fd_)) {
            return false;
        }
        std::println(stderr, "Mode: SOFTWARE timestamping (no PHC) — using SoftClock virtual clock");
        ops_ = make_linux_sw_clock_ops(raw_fd_, soft_clock_, cfg_.interface, if_index_, mac_);
    } else {
        if (!enable_hw_timestamping(raw_fd_, cfg_.interface.c_str())) {
            return false;
        }
        if (!enable_so_timestamping_hw(raw_fd_)) {
            return false;
        }
        phc_fd_ = open_phc_for_interface(raw_fd_, cfg_.interface.c_str());
        if (phc_fd_ < 0) {
            return false;
        }
        if (cfg_.phc_passthrough) {
            std::println(stderr, "Mode: HARDWARE timestamping via PHC (passthrough — virtual offset, PHC not modified)");
            ops_ = make_linux_phc_passthrough_ops(raw_fd_, phc_fd_, soft_clock_, cfg_.interface, if_index_, mac_);
        } else {
            std::println(stderr, "Mode: HARDWARE timestamping via PHC");
            ops_ = make_linux_phc_clock_ops(raw_fd_, phc_fd_, cfg_.interface, if_index_, mac_);
        }
    }

    GptpConfig const gptp_cfg = build_gptp_config(cfg_, mac_);

    // ops_ passed by COPY (not move) so subsequent reads of get_local_time_ns
    // from the bridge keep working (mirrors the pre-refactor lifetime).
    port_ = std::make_unique<GptpSlavePort>(gptp_cfg, ops_);

    Observer obs{};
    obs.on_sync_state_change = [this](bool synced) {
        is_synced_.store(synced, std::memory_order_relaxed);
        std::println(stderr, "[gptp] sync state: {}", synced ? "LOCKED" : "LOST");
    };
    obs.on_grandmaster_change = [](tsn::ClockIdentity const& gm) {
        std::string buf;
        gptp::format_to(std::back_inserter(buf), gm);
        std::println(stderr, "[gptp] grandmaster: {}", buf);
    };
    obs.on_as_capable_change = [](bool capable) { std::println(stderr, "[gptp] asCapable: {}", capable); };

    if (cfg_.bridge_clock >= 0) {
        clockid_t const bclk = cfg_.bridge_clock;
        bridge_.get_gptp_time_ns = [this]() -> int64_t {
            if (cfg_.software_timestamping) {
                struct timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                return soft_clock_.get_ptp_time_ns((ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec);
            }
            return ops_.get_local_time_ns ? ops_.get_local_time_ns() : 0;
        };
        bridge_.get_app_time_ns = [bclk]() -> int64_t {
            struct timespec ts{};
            clock_gettime(bclk, &ts);
            return (ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec;
        };
        bridge_.update();
        std::println(stderr, "Bridge: gPTP <-> {}", bridge_clock_name_);
    }

    bool const verbose = cfg_.verbose;
    bool const show_bridge = (cfg_.bridge_clock >= 0);

    obs.on_sync_update = [this, verbose, show_bridge](int64_t offset_ns, double rate_ratio) {
        // Bridge anchor refresh moved to on_anchor_update so it
        // captures the HW-stamped (local, master) pair instead of
        // sampling get_gptp_time_ns at observer-callback time. This
        // callback now only carries diagnostic / verbose output.
        (void)show_bridge;
        if (!verbose) {
            return;
        }
        if (show_bridge) {
            std::println(
                stderr,
                "[gptp] offset: {:+} ns  rate: {:.12f}  {}_offset: {:+} ns  {}_rate_ppt: {:+}",
                offset_ns,
                rate_ratio,
                bridge_clock_name_,
                bridge_.offset(),
                bridge_clock_name_,
                bridge_.rate_offset_ppt());
        } else {
            std::println(stderr, "[gptp] offset: {:+} ns  rate ratio: {:.12f}", offset_ns, rate_ratio);
        }
    };

    // Bridge anchor refresh from the HW-stamped (local, master) pair.
    // Fires on every successful Sync+FollowUp pairing — earlier in the
    // path than on_sync_update because it carries the wire timestamps
    // directly rather than the user-space-sampled "now" pair.
    obs.on_anchor_update = [this,
                            show_bridge](int64_t /*rx_local_ns*/, int64_t rx_app_ns, int64_t tx_master_ns, double rate_ratio) {
        if (show_bridge) {
            bridge_.update_from_anchor(rx_app_ns, tx_master_ns, rate_ratio);
        }
    };

    obs.on_peer_delay_update = [this, show_bridge](int64_t delay_ns, double neighbor_rate_ratio) {
        if (show_bridge) {
            std::println(
                stderr,
                "[gptp] peer delay: {} ns  neighbor rate ratio: {:.9f}  {}_rate_ppt: {:+}",
                delay_ns,
                neighbor_rate_ratio,
                bridge_clock_name_,
                bridge_.rate_offset_ppt());
        } else {
            std::println(stderr, "[gptp] peer delay: {} ns  neighbor rate ratio: {:.9f}", delay_ns, neighbor_rate_ratio);
        }
    };

    (void)port_->subscribe(std::move(obs));

    auto const now = std::chrono::steady_clock::now();
    port_->start(now, /*link_up=*/true);

    print_running_banner(cfg_);
    return true;
}

auto SlaveSession::poll_fds() const -> std::span<int const>
{
    return std::span<int const>{poll_fd_storage_.data(), poll_fd_storage_.size()};
}

auto SlaveSession::owns_fd(int fd) const -> bool
{
    return fd == raw_fd_ && fd >= 0;
}

void SlaveSession::dispatch(int fd, sm::TimePoint now)
{
    if (!owns_fd(fd) || port_ == nullptr) {
        return;
    }
    int64_t rx_ns = 0;
    bool const prefer_hw = !cfg_.software_timestamping;
    ssize_t const payload_len = recv_gptp_frame(raw_fd_, payload_buf_, rx_ns, prefer_hw);
    if (payload_len <= 0) {
        return;
    }
    // The bridge's app-clock side of a Sync anchor: read once, here, the
    // earliest user-space instant after arrival, and told through the port
    // so the anchor pairs it with the FollowUp's master time.
    int64_t const rx_app_ns = (cfg_.bridge_clock >= 0) ? bridge_.app_now() : 0;
    bool const use_soft_clock_corrections = cfg_.software_timestamping || cfg_.phc_passthrough;
    if (use_soft_clock_corrections && rx_ns != 0) {
        rx_ns = soft_clock_.correct_timestamp(rx_ns);
    }
    port_->receive_frame(std::span<uint8_t const>(payload_buf_.data(), static_cast<size_t>(payload_len)), rx_ns, now, rx_app_ns);
}

void SlaveSession::tick(sm::TimePoint now)
{
    if (port_ != nullptr) {
        port_->tick(now);
    }
}

void SlaveSession::stop()
{
    if (port_ == nullptr) {
        return;
    }
    port_->stop();

    std::println(stderr, "\ngPTP slave stopped.");
    std::println(stderr, "  Last offset: {:+} ns", port_->last_master_offset_ns());
    std::println(stderr, "  Last rate ratio: {:.12f}", port_->last_rate_ratio());
    std::println(stderr, "  Mean link delay: {} ns", port_->mean_link_delay_ns());
    std::println(stderr, "  Current PPM: {:.6f}", port_->current_ppm());
    if (cfg_.software_timestamping) {
        std::println(stderr, "  SoftClock offset: {:+} ns", soft_clock_.offset_ns());
        std::println(stderr, "  SoftClock rate: {:.6f} ppb", soft_clock_.rate_ppb());
    }
    if (cfg_.bridge_clock >= 0) {
        // Refresh only the offset — leave rate_offset_ppt at its last
        // sync-driven value. Calling full update() at exit computes a
        // fresh rate from a tiny app_delta whose timing is determined
        // by Ctrl-C arrival, which amplifies noise and reports a
        // misleading rate.
        bridge_.refresh_offset();
        std::println(
            stderr,
            "  Bridge ({} <-> gPTP) offset: {:+} ns  rate_offset: {:+} ppt  ({:+.3f} ppm)",
            bridge_clock_name_,
            bridge_.offset(),
            bridge_.rate_offset_ppt(),
            static_cast<double>(bridge_.rate_offset_ppt()) / 1e6);
    }
}

}  // namespace statusbar::gptp

#endif  // __linux__
