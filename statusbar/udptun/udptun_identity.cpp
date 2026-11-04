// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_identity.hpp"

#include "statusbar/ieee/ieee_ethernet_format.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/udptun/udptun_time_source.hpp"

#include <ctime>
#include <iterator>
#include <print>
#include <string>

namespace statusbar::udptun {

auto read_mac_from_interface(std::string const& interface_name) -> std::optional<ieee::Eui48>
{
    if (interface_name.empty()) {
        std::println(stderr, "read_mac_from_interface: empty interface name");
        return std::nullopt;
    }
    auto mac_or = net::read_interface_mac(interface_name);
    if (!mac_or) {
        std::println(stderr, "read_mac_from_interface: failed to read MAC for '{}'", interface_name);
        return std::nullopt;
    }
    return mac_or;
}

namespace {

[[nodiscard]] auto resolve_fallback_iface(LocalIdentityConfig const& cfg) -> std::string const&
{
#if defined(__linux__)
    return cfg.fallback_interface.empty() ? cfg.gptp_session_config.interface : cfg.fallback_interface;
#else
    return cfg.fallback_interface;
#endif
}

}  // namespace

auto setup_local_identity_no_gptp(LocalIdentityConfig const& cfg) -> std::optional<ieee::Eui48>
{
    std::string const& iface = resolve_fallback_iface(cfg);
    auto mac_or = net::read_interface_mac(iface);
    if (!mac_or) {
        std::println(stderr, "no-gptp: failed to read MAC for interface '{}'", iface);
        return std::nullopt;
    }
    std::println(stderr, "no-gptp: skipping gPTP, stamping with CLOCK_REALTIME on iface '{}'", iface);
    return mac_or;
}

#if defined(__linux__)
auto setup_local_identity(LocalIdentityConfig const& cfg, std::optional<gptp::SlaveSession>& session) -> std::optional<ieee::Eui48>
{
    if (cfg.no_gptp) {
        return setup_local_identity_no_gptp(cfg);
    }
    // The wire timeline is CLOCK_MONOTONIC_RAW (monotonic_raw_ns) and the
    // slave's bridge is read with RAW instants everywhere (the wire-clock
    // translator, GptpSlaveClockSource), so the bridge must sample RAW.
    if (cfg.gptp_session_config.bridge_clock != CLOCK_MONOTONIC_RAW) {
        std::println(stderr, "gptp: the bridge clock must be monotonic_raw: the udptun wire timeline runs on CLOCK_MONOTONIC_RAW");
        return std::nullopt;
    }
    session.emplace(cfg.gptp_session_config);
    if (!session->start()) {
        return std::nullopt;
    }
    return session->local_mac();
}
#endif

void print_eui64_banner(std::string_view label, TxIdentityPair const& tx, int64_t temporal_shift_ms)
{
    std::string buf;
    buf = std::string{label} + " primary EUI-64: ";
    ieee::format_to(std::back_inserter(buf), tx.primary_id);
    std::println(stderr, "{}", buf);
    if (tx.redundant_enabled) {
        buf = std::string{label} + " redundant EUI-64: ";
        ieee::format_to(std::back_inserter(buf), tx.redundant_id);
        std::println(stderr, "{}  (temporal_shift={}ms)", buf, temporal_shift_ms);
    }
}

}  // namespace statusbar::udptun
