// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// End-to-end test of perform_rendezvous: spin up a real server in a
/// background thread on a loopback port, run a client through the
/// helper, verify it returns a paired result with a valid fd.

#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/stun/stun_rendezvous.hpp"
#include "statusbar/stun/stun_server.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <chrono>
#include <thread>

#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::stun;

namespace {

auto make_test_key() -> statusbar::crypto::Aes128SivKey
{
    statusbar::crypto::Aes128SivKey k{};
    for (size_t i = 0; i < k.data.size(); ++i) {
        k.data[i] = static_cast<uint8_t>(0x90U + i);
    }
    return k;
}

auto make_session_id() -> SessionId
{
    SessionId s{};
    for (size_t i = 0; i < SESSION_ID_SIZE; ++i) {
        s.bytes[i] = static_cast<uint8_t>(0xE0U + i);
    }
    return s;
}

}  // namespace

TEST(stun_rendezvous, two_clients_through_helper_pair_and_release_fds)
{
    auto const key = make_test_key();
    auto const sid = make_session_id();

    std::error_code ec{};
    ServerConfig server_cfg{
        .bind_port = 0,
        .shared_key = key,
        .refresh_interval_ms = 50,
        .session_expiry_ms = 5'000,
    };
    auto server = StunServer<>::create(server_cfg, ec);
    EXPECT_TRUE(server != nullptr);
    if (!server) {
        return;
    }
    auto const server_port = server->port();

    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, statusbar::net::monotonic_ns, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }

    std::thread server_thread{[&] {
        for (int i = 0; i < 2'000 && !stop.stop_requested(); ++i) {
            reactor.poll_once(5);
        }
    }};

    auto server_addr = statusbar::net::SocketAddress::ipv4_loopback(server_port);

    RendezvousConfig cfg_a{
        .server_address = server_addr,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0x11, 0x22, 0x33, 0xFF, 0xFE, 0x00, 0x00, 0xA1},
        .role = Role::Initiator,
        .shared_key = key,
        .timeout_ms = 5'000,
    };
    RendezvousConfig cfg_b = cfg_a;
    cfg_b.client_eui64 = ieee::Eui64{0x44, 0x55, 0x66, 0xFF, 0xFE, 0x00, 0x00, 0xB2};
    cfg_b.role = Role::Responder;

    std::expected<RendezvousResult, std::error_code> result_a{};
    std::expected<RendezvousResult, std::error_code> result_b{};
    std::thread client_a_thread{[&] { result_a = perform_rendezvous(cfg_a); }};
    std::thread client_b_thread{[&] { result_b = perform_rendezvous(cfg_b); }};

    client_a_thread.join();
    client_b_thread.join();
    stop.request_stop();
    server_thread.join();

    EXPECT_TRUE(result_a.has_value());
    EXPECT_TRUE(result_b.has_value());
    if (!result_a.has_value() || !result_b.has_value()) {
        return;
    }

    // Each client got back a usable, distinct fd.
    EXPECT_TRUE(result_a->socket.valid());
    EXPECT_TRUE(result_b->socket.valid());
    EXPECT_NE(result_a->socket.get(), result_b->socket.get());

    // The peer reflexive address each one learned matches the other's
    // local address (modulo loopback IP encoding, both are 127.0.0.1).
    EXPECT_EQ(result_a->peer_reflexive_address.port(), result_b->local_address.port());
    EXPECT_EQ(result_b->peer_reflexive_address.port(), result_a->local_address.port());

    // Peer EUI-64 cross-check.
    EXPECT_EQ(result_a->peer_eui64.to_uint64(), cfg_b.client_eui64.to_uint64());
    EXPECT_EQ(result_b->peer_eui64.to_uint64(), cfg_a.client_eui64.to_uint64());
}

TEST(stun_rendezvous, timeout_when_no_peer)
{
    auto const key = make_test_key();
    std::error_code ec{};
    ServerConfig server_cfg{.bind_port = 0, .shared_key = key, .refresh_interval_ms = 30};
    auto server = StunServer<>::create(server_cfg, ec);
    EXPECT_TRUE(server != nullptr);
    if (!server) {
        return;
    }
    auto const server_port = server->port();

    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, statusbar::net::monotonic_ns, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }
    std::thread server_thread{[&] {
        for (int i = 0; i < 200 && !stop.stop_requested(); ++i) {
            reactor.poll_once(5);
        }
    }};

    RendezvousConfig cfg{
        .server_address = statusbar::net::SocketAddress::ipv4_loopback(server_port),
        .session_id = make_session_id(),
        .client_eui64 = ieee::Eui64{1, 2, 3, 0xFF, 0xFE, 4, 5, 6},
        .role = Role::Initiator,
        .shared_key = key,
        .timeout_ms = 200,
    };
    auto const result = perform_rendezvous(cfg);
    stop.request_stop();
    server_thread.join();

    EXPECT_FALSE(result.has_value());
}

TEST_MAIN(statusbar_stun, stun_rendezvous_functional_test)
