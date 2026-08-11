// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// stun_server_tool — STUN rendezvous server.
///
/// Listens on both IPv4 and IPv6 wildcard UDP sockets for REGISTER messages
/// from a pair of clients sharing a session id. Responds with each client's
/// reflexive address and, once both have registered, the peer's address.

#include "statusbar/args/args_spec.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/crypto/aes_siv/aes128_siv.hpp"
#include "statusbar/crypto/util/crypto_backend.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/stun/stun_server.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

using namespace statusbar;

namespace {

struct Config
{
    uint16_t port{stun::default_stun_port};
    std::string key_hex{};
    int64_t refresh_interval_ms{stun::default_refresh_interval_ms};
    int64_t session_expiry_ms{stun::default_session_expiry_ms};
    bool verbose{false};
};

auto build_arg_specs(Config& c) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs s;
    s.add<uint16_t>("port", "UDP port (both IPv4 and IPv6 wildcards)", stun::default_stun_port, [&](auto v) { c.port = v; });
    s.add<std::string>(
        "key",
        "64 hex chars = 32-byte AES-128-SIV pre-shared key. Generate with `openssl rand -hex 32`.",
        std::string{},
        [&](auto v) { c.key_hex = v; });
    s.add<int64_t>(
        "refresh-interval-ms", "Refresh interval the server advertises to clients", stun::default_refresh_interval_ms, [&](auto v) {
            c.refresh_interval_ms = v;
        });
    s.add<int64_t>(
        "session-expiry-ms", "Drop sessions whose last refresh is older than this", stun::default_session_expiry_ms, [&](auto v) {
            c.session_expiry_ms = v;
        });
    s.add_flag("verbose", "Print one stderr line per accepted REGISTER", [&](auto v) { c.verbose = v; });
    return s;
}

void print_usage(char const* prog, statusbar::args::ArgumentSpecs const& specs)
{
    statusbar::config::default_print_usage(prog, specs, "STUN rendezvous server. --port and --key are required.");
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    Config cli;
    auto specs = build_arg_specs(cli);
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "stun_server_tool");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (cli.key_hex.empty()) {
        std::println(stderr, "error: --key is required");
        return 1;
    }
    statusbar::crypto::Aes128SivKey key{};
    if (!statusbar::net::parse_hex_into(cli.key_hex, std::span<uint8_t>{key.data})) {
        std::println(stderr, "error: --key must be {} hex characters", key.data.size() * 2);
        return 1;
    }

    stun::ServerConfig server_cfg{
        .bind_port = cli.port,
        .shared_key = key,
        .refresh_interval_ms = static_cast<uint32_t>(cli.refresh_interval_ms),
        .session_expiry_ms = static_cast<uint32_t>(cli.session_expiry_ms),
        .verbose = cli.verbose,
    };

    std::error_code ec{};
    auto server = stun::StunServer<>::create(server_cfg, ec);
    if (!server) {
        std::println(stderr, "error: failed to start server: {}", ec.message());
        return 1;
    }

    std::println(stderr, "STUN rendezvous server listening on port {}", server->port());
    // One-line downgrade canary: a "sw" where "hw" is expected means the
    // platform is hiding CPU crypto features from this process.
    std::println(stderr, "Crypto backends: {}", statusbar::crypto::crypto_backend_summary());
    std::println(
        stderr, "Refresh interval: {}ms, session expiry: {}ms", server_cfg.refresh_interval_ms, server_cfg.session_expiry_ms);
    std::println(stderr, "Press Ctrl+C to stop");

    auto& stop = statusbar::itc::install_stop_signal();
    statusbar::net::MessageReactor reactor{stop, statusbar::net::monotonic_ns, 100};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }
    reactor.run();

    std::println(stderr, "\nShutting down...");
    return 0;
}
