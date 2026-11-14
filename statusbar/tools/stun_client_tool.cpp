// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// stun_client_tool — STUN rendezvous client.
///
/// Sends REGISTER messages to a server, receives the reflexive address,
/// and prints peer info once the session is paired. After pairing the
/// client keeps refreshing as a NAT keepalive until interrupted.

#include "statusbar/args/args_spec.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/crypto/aes_siv/aes128_siv.hpp"
#include "statusbar/crypto/util/crypto_backend.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_socket.hpp"
#include "statusbar/secure_random/secure_random.hpp"
#include "statusbar/stun/stun_client.hpp"
#include "statusbar/stun/stun_register.hpp"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

using namespace statusbar;

namespace {

struct Config
{
    std::string server{};
    std::string key_hex{};
    std::string session_id_hex{};
    std::string eui64{};
    std::string role{"initiator"};
    bool verbose{false};
};

void random_session_id(stun::SessionId& out)
{
    secure_random_bytes(std::span<uint8_t>{out.bytes.data(), out.bytes.size()});
}

[[nodiscard]] auto parse_eui64(std::string_view s, statusbar::ieee::Eui64& out) -> bool
{
    std::array<uint8_t, 8> bytes{};
    size_t byte_idx = 0;
    int nibble = -1;
    for (char c : s) {
        if (c == ':' || c == '-') {
            continue;
        }
        auto const n = statusbar::ieee::parse_hex_digit(c);
        if (n == 255) {
            return false;
        }
        if (nibble < 0) {
            nibble = static_cast<int>(n);
        } else {
            if (byte_idx >= bytes.size()) {
                return false;
            }
            bytes[byte_idx++] = static_cast<uint8_t>((nibble << 4) | n);
            nibble = -1;
        }
    }
    if (byte_idx != bytes.size() || nibble != -1) {
        return false;
    }
    out = statusbar::ieee::Eui64{bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]};
    return true;
}

// The server, the key and the session: what the rendezvous is made of.
void add_session_args(statusbar::args::ArgumentSpecs& s, Config& c)
{
    s.add<std::string>(
        "server", "STUN server as host:port (e.g. stun.example.com:3478)", std::string{}, [&](auto v) { c.server = v; });
    s.add<std::string>(
        "key", "64 hex chars = 32-byte AES-128-SIV pre-shared key. Must match the server's --key.", std::string{}, [&](auto v) {
            c.key_hex = v;
        });
    s.add<std::string>(
        "session-id",
        "32 hex chars = 16-byte session id. Must match the peer's value. Random if omitted.",
        std::string{},
        [&](auto v) { c.session_id_hex = v; });
}

// Who this client is in the session, and how loudly it reports.
void add_identity_args(statusbar::args::ArgumentSpecs& s, Config& c)
{
    s.add<std::string>("eui64", "8-byte identifier (e.g. AA:BB:CC:DD:EE:FF:00:11)", std::string{}, [&](auto v) { c.eui64 = v; });
    s.add_choice("role", "Role hint", {"initiator", "responder"}, "initiator", [&](auto v) { c.role = std::string{v}; });
    s.add_flag("verbose", "Trace each send / recv / refresh / retransmit", [&](auto v) { c.verbose = v; });
}

auto build_arg_specs(Config& c) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs s;
    add_session_args(s, c);
    add_identity_args(s, c);
    return s;
}

void print_usage(char const* prog, statusbar::args::ArgumentSpecs const& specs)
{
    statusbar::config::default_print_usage(prog, specs, "STUN rendezvous client. --server, --key, and --eui64 are required.");
}

// Parses the command line into cli. Returns the exit code when the run ends here (a
// built-in command handled, an argument error, a required option missing); nullopt to go on.
auto parse_arguments(int argc, char** argv, Config& cli) -> std::optional<int>
{
    auto specs = build_arg_specs(cli);
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-stun-client-tool");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (cli.server.empty() || cli.key_hex.empty() || cli.eui64.empty()) {
        std::println(stderr, "error: --server, --key, and --eui64 are required");
        return 1;
    }
    return std::nullopt;
}

// The command line, checked and decoded: what the client is built from.
struct Resolved
{
    statusbar::net::SocketAddress server_address{};
    statusbar::crypto::Aes128SivKey key{};
    statusbar::ieee::Eui64 eui64{};
    stun::SessionId session_id{};
    stun::Role role{stun::Role::Initiator};
    bool verbose{false};
};

// Each resolver prints its own error and answers false.
auto resolve_server_address(std::string const& host, std::string const& port, Resolved& r) -> bool
{
    auto server_addr = statusbar::net::SocketAddress::from_string(host, port, statusbar::net::SocketDatagram);
    if (!server_addr) {
        std::println(stderr, "error: cannot resolve '{}:{}'", host, port);
        return false;
    }
    r.server_address = *server_addr;
    return true;
}

auto resolve_server(std::string const& server, Resolved& r) -> bool
{
    std::string host{};
    std::string port{};
    if (!statusbar::net::split_host_port(server, host, port)) {
        std::println(stderr, "error: --server must be HOST:PORT");
        return false;
    }
    return resolve_server_address(host, port, r);
}

auto resolve_key_and_eui64(Config const& cli, Resolved& r) -> bool
{
    if (!statusbar::net::parse_hex_into(cli.key_hex, std::span<uint8_t>{r.key.data})) {
        std::println(stderr, "error: --key must be exactly {} hex characters", r.key.data.size() * 2);
        return false;
    }
    if (!parse_eui64(cli.eui64, r.eui64)) {
        std::println(stderr, "error: --eui64 must be 8 hex bytes (e.g. AA:BB:CC:DD:EE:FF:00:11)");
        return false;
    }
    return true;
}

// The session id given, or a fresh random one, announced so the peer can be given it.
auto resolve_session_id(std::string const& hex, stun::SessionId& out) -> bool
{
    if (!hex.empty()) {
        if (!statusbar::net::parse_hex_into(hex, std::span<uint8_t>{out.bytes})) {
            std::println(stderr, "error: --session-id must be exactly {} hex characters", stun::SESSION_ID_SIZE * 2);
            return false;
        }
        return true;
    }
    random_session_id(out);
    std::print(stderr, "Generated session-id: ");
    for (auto b : out.bytes) {
        std::print(stderr, "{:02x}", b);
    }
    std::println(stderr, "");
    return true;
}

auto resolve(Config const& cli) -> std::optional<Resolved>
{
    Resolved r{
        .role = (cli.role == "responder") ? stun::Role::Responder : stun::Role::Initiator,
        .verbose = cli.verbose,
    };
    if (!resolve_server(cli.server, r)) {
        return std::nullopt;
    }
    if (!resolve_key_and_eui64(cli, r)) {
        return std::nullopt;
    }
    if (!resolve_session_id(cli.session_id_hex, r.session_id)) {
        return std::nullopt;
    }
    return r;
}

void print_peer(stun::RegisterResponseSuccess const& r)
{
    if (r.peer_xor_mapped_address.has_value()) {
        std::println(stderr, "  Peer reflexive address: {}", r.peer_xor_mapped_address->to_string());
    }
    if (r.peer_eui64.has_value()) {
        std::println(stderr, "  Peer EUI-64: 0x{:016x}", r.peer_eui64->to_uint64());
    }
}

void print_paired(stun::RegisterResponseSuccess const& r)
{
    std::println(stderr, "");
    std::println(stderr, "PAIRED.");
    std::println(stderr, "  My reflexive address: {}", r.xor_mapped_address.to_string());
    print_peer(r);
    std::println(stderr, "");
    std::println(stderr, "Continuing to refresh as NAT keepalive. Ctrl+C to stop.");
}

auto make_client_config(Resolved const& r, std::atomic<bool>& failed) -> stun::ClientConfig
{
    return stun::ClientConfig{
        .server_address = r.server_address,
        .session_id = r.session_id,
        .client_eui64 = r.eui64,
        .role = r.role,
        .shared_key = r.key,
        .on_paired = [](stun::RegisterResponseSuccess const& response) { print_paired(response); },
        .on_failure =
            [&failed](std::error_code ec) {
                std::println(stderr, "Client failed: {}", ec.message());
                failed.store(true);
            },
        .verbose = r.verbose,
    };
}

void announce(Resolved const& r)
{
    // One-line downgrade canary: a "sw" where "hw" is expected means the
    // platform is hiding CPU crypto features from this process.
    std::println(stderr, "Crypto backends: {}", statusbar::crypto::crypto_backend_summary());
    std::println(
        stderr,
        "STUN client \xe2\x86\x92 {} as role={}",
        r.server_address.to_string(),
        r.role == stun::Role::Initiator ? "initiator" : "responder");
    std::println(stderr, "Waiting for peer to register with the same session-id...");
}

// Runs the client on the reactor until the stop signal; 1 when the client reported a failure.
auto serve(std::unique_ptr<stun::StunClientPollable> client, std::atomic<bool> const& failed) -> int
{
    auto& stop = statusbar::itc::install_stop_signal();
    statusbar::net::MessageReactor reactor{stop, statusbar::net::monotonic_ns, 100};
    reactor.add(std::move(client));
    reactor.run();
    std::println(stderr, "\nShutting down...");
    return failed.load() ? 1 : 0;
}

auto run_client(Resolved const& r, std::atomic<bool>& failed) -> int
{
    auto const client_cfg = make_client_config(r, failed);
    std::error_code ec{};
    auto client = stun::StunClientPollable::create(client_cfg, ec);
    if (!client) {
        std::println(stderr, "error: failed to create client: {}", ec.message());
        return 1;
    }
    announce(r);
    return serve(std::move(client), failed);
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    Config cli;
    if (auto const done = parse_arguments(argc, argv, cli)) {
        return *done;
    }
    auto const resolved = resolve(cli);
    if (!resolved) {
        return 1;
    }
    std::atomic<bool> failed{false};
    return run_client(*resolved, failed);
}
