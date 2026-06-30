// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// End-to-end test: server + two clients on UDP loopback. Runs them all
/// in a single MessageReactor for a bounded number of cycles and asserts
/// that both clients reach Paired and learn each other's addresses.

#include "statusbar/crypto/aes_siv/aes128_siv.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/stun/stun_client.hpp"
#include "statusbar/stun/stun_register.hpp"
#include "statusbar/stun/stun_server.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>

using namespace statusbar;
using namespace statusbar::stun;

namespace {

auto make_test_key() -> statusbar::crypto::Aes128SivKey
{
    statusbar::crypto::Aes128SivKey k{};
    for (size_t i = 0; i < k.data.size(); ++i) {
        k.data[i] = static_cast<uint8_t>(0x10U + i);
    }
    return k;
}

auto make_session_id(uint8_t seed) -> SessionId
{
    SessionId s{};
    for (size_t i = 0; i < SESSION_ID_SIZE; ++i) {
        s.bytes[i] = static_cast<uint8_t>(seed + i);
    }
    return s;
}

/// Synthesize a deterministic monotonic clock so the test does not depend
/// on real time — each call advances by 1 ms.
struct FakeClock
{
    std::atomic<int64_t>* now_ns;
    auto operator()() noexcept -> int64_t { return now_ns->fetch_add(1'000'000LL); }
};

}  // namespace

TEST(stun_loopback, two_clients_pair)
{
    auto const sid = make_session_id(0x60);
    auto const key = make_test_key();

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
    auto server_address = statusbar::net::SocketAddress::ipv4_loopback(server_port);

    std::optional<RegisterResponseSuccess> a_paired;
    std::optional<RegisterResponseSuccess> b_paired;

    ClientConfig a_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xAA, 0xAA, 0xAA, 0xFF, 0xFE, 0x00, 0x00, 0x01},
        .role = Role::Initiator,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { a_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };
    ClientConfig b_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xBB, 0xBB, 0xBB, 0xFF, 0xFE, 0x00, 0x00, 0x02},
        .role = Role::Responder,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { b_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };

    auto client_a = StunClientPollable::create(a_cfg, ec);
    EXPECT_TRUE(client_a != nullptr);
    auto client_b = StunClientPollable::create(b_cfg, ec);
    EXPECT_TRUE(client_b != nullptr);

    std::atomic<int64_t> now_ns{1};
    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }
    reactor.add(std::move(client_a));
    reactor.add(std::move(client_b));

    constexpr int max_cycles = 200;
    for (int i = 0; i < max_cycles; ++i) {
        reactor.poll_once(5);
        if (a_paired.has_value() && b_paired.has_value()) {
            break;
        }
    }

    EXPECT_TRUE(a_paired.has_value());
    EXPECT_TRUE(b_paired.has_value());
    if (a_paired.has_value() && b_paired.has_value()) {
        EXPECT_TRUE(a_paired->peer_eui64.has_value());
        EXPECT_TRUE(b_paired->peer_eui64.has_value());
        EXPECT_EQ(a_paired->peer_eui64->to_uint64(), b_cfg.client_eui64.to_uint64());
        EXPECT_EQ(b_paired->peer_eui64->to_uint64(), a_cfg.client_eui64.to_uint64());
    }
}

TEST(stun_loopback, duplicate_eui64_still_pairs_when_addresses_differ)
{
    // Regression: server used to key its slot table by EUI-64, so two
    // clients passing the same EUI-64 would silently overwrite each
    // other's slot and never pair. Now slot picking is by source
    // SocketAddress; duplicate EUI-64s pair fine as long as the two
    // sockets bind to distinct ephemeral ports (which is the kernel
    // default).
    auto const sid = make_session_id(0x70);
    auto const key = make_test_key();

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
    auto const server_address = statusbar::net::SocketAddress::ipv4_loopback(server->port());

    ieee::Eui64 const shared_eui{0x77, 0x77, 0x77, 0xFF, 0xFE, 0x00, 0x00, 0xAA};

    std::optional<RegisterResponseSuccess> a_paired;
    std::optional<RegisterResponseSuccess> b_paired;
    ClientConfig a_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = shared_eui,
        .role = Role::Initiator,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { a_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };
    ClientConfig b_cfg = a_cfg;  // same EUI-64
    b_cfg.role = Role::Responder;
    b_cfg.on_paired = [&](RegisterResponseSuccess const& r) { b_paired = r; };

    auto client_a = StunClientPollable::create(a_cfg, ec);
    auto client_b = StunClientPollable::create(b_cfg, ec);

    std::atomic<int64_t> now_ns{1};
    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }
    reactor.add(std::move(client_a));
    reactor.add(std::move(client_b));

    for (int i = 0; i < 200; ++i) {
        reactor.poll_once(5);
        if (a_paired.has_value() && b_paired.has_value()) {
            break;
        }
    }

    EXPECT_TRUE(a_paired.has_value());
    EXPECT_TRUE(b_paired.has_value());
    if (a_paired.has_value() && b_paired.has_value()) {
        // EUI-64s match between peers — but the addresses differ.
        EXPECT_TRUE(a_paired->peer_xor_mapped_address.has_value());
        EXPECT_TRUE(b_paired->peer_xor_mapped_address.has_value());
        EXPECT_NE(a_paired->xor_mapped_address.port(), b_paired->xor_mapped_address.port());
        EXPECT_EQ(a_paired->peer_xor_mapped_address->port(), b_paired->xor_mapped_address.port());
        EXPECT_EQ(b_paired->peer_xor_mapped_address->port(), a_paired->xor_mapped_address.port());
    }
}

TEST(stun_loopback, two_ipv6_clients_pair)
{
    auto const sid = make_session_id(0x80);
    auto const key = make_test_key();

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

    auto const server_address = statusbar::net::SocketAddress::ipv6_loopback(server->port());

    std::optional<RegisterResponseSuccess> a_paired;
    std::optional<RegisterResponseSuccess> b_paired;

    ClientConfig a_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xAA, 0xAA, 0xAA, 0xFF, 0xFE, 0x00, 0x00, 0x03},
        .role = Role::Initiator,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { a_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };
    ClientConfig b_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xBB, 0xBB, 0xBB, 0xFF, 0xFE, 0x00, 0x00, 0x04},
        .role = Role::Responder,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { b_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };

    auto client_a = StunClientPollable::create(a_cfg, ec);
    EXPECT_TRUE(client_a != nullptr);
    auto client_b = StunClientPollable::create(b_cfg, ec);
    EXPECT_TRUE(client_b != nullptr);

    std::atomic<int64_t> now_ns{1};
    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }
    reactor.add(std::move(client_a));
    reactor.add(std::move(client_b));

    constexpr int max_cycles = 200;
    for (int i = 0; i < max_cycles; ++i) {
        reactor.poll_once(5);
        if (a_paired.has_value() && b_paired.has_value()) {
            break;
        }
    }

    EXPECT_TRUE(a_paired.has_value());
    EXPECT_TRUE(b_paired.has_value());
    if (a_paired.has_value() && b_paired.has_value()) {
        EXPECT_TRUE(a_paired->peer_xor_mapped_address.has_value());
        EXPECT_TRUE(b_paired->peer_xor_mapped_address.has_value());
        EXPECT_EQ(a_paired->xor_mapped_address.family(), AF_INET6);
        EXPECT_EQ(b_paired->xor_mapped_address.family(), AF_INET6);
        EXPECT_EQ(a_paired->peer_xor_mapped_address->family(), AF_INET6);
        EXPECT_EQ(b_paired->peer_xor_mapped_address->family(), AF_INET6);
    }
}

TEST(stun_loopback, mixed_family_pair_is_rejected)
{
    auto const sid = make_session_id(0x90);
    auto const key = make_test_key();

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

    auto const v4_address = statusbar::net::SocketAddress::ipv4_loopback(server->port());
    auto const v6_address = statusbar::net::SocketAddress::ipv6_loopback(server->port());

    std::optional<RegisterResponseSuccess> a_paired;
    std::optional<RegisterResponseSuccess> b_paired;
    std::optional<std::error_code> a_failure;
    std::optional<std::error_code> b_failure;

    ClientConfig a_cfg{
        .server_address = v4_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xCC, 0xCC, 0xCC, 0xFF, 0xFE, 0x00, 0x00, 0x05},
        .role = Role::Initiator,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { a_paired = r; },
        .on_failure = [&](std::error_code e) { a_failure = e; },
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };
    ClientConfig b_cfg{
        .server_address = v6_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xDD, 0xDD, 0xDD, 0xFF, 0xFE, 0x00, 0x00, 0x06},
        .role = Role::Responder,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { b_paired = r; },
        .on_failure = [&](std::error_code e) { b_failure = e; },
        .initial_rto_ms = 20,
        .max_retransmits = 8,
    };

    auto client_a = StunClientPollable::create(a_cfg, ec);
    EXPECT_TRUE(client_a != nullptr);
    auto client_b = StunClientPollable::create(b_cfg, ec);
    EXPECT_TRUE(client_b != nullptr);

    std::atomic<int64_t> now_ns{1};
    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }
    reactor.add(std::move(client_a));
    reactor.add(std::move(client_b));

    constexpr int max_cycles = 200;
    for (int i = 0; i < max_cycles; ++i) {
        reactor.poll_once(5);
        if (a_failure.has_value() || b_failure.has_value()) {
            break;
        }
    }

    // Neither client should have paired.
    EXPECT_FALSE(a_paired.has_value());
    EXPECT_FALSE(b_paired.has_value());
    // At least the second registrant (the one whose family conflicts with
    // the first) must have received a failure response.
    EXPECT_TRUE(a_failure.has_value() || b_failure.has_value());
}

// Regression test for the staggered-start rendezvous-timing bug: the
// first client to arrive must discover pairing within its short
// waiting-poll interval after the second client shows up, NOT have to
// wait a full (long) server refresh interval. The server is reactive and
// never pushes Paired to an idle waiter, so a waiter only learns of
// pairing on its own next REGISTER. A waiter that only re-registered
// every refresh_interval_ms would not pair until 15s of sim time elapsed; here the server refresh is 15s but the
// client waiting-poll is 50ms, so pairing must complete far under 15s.
TEST(stun_loopback, staggered_start_pairs_within_waiting_poll)
{
    auto const sid = make_session_id(0x90);
    auto const key = make_test_key();

    std::error_code ec{};
    ServerConfig server_cfg{
        .bind_port = 0,
        .shared_key = key,
        .refresh_interval_ms = 15'000,  // long: old code would block here
        .session_expiry_ms = 60'000,    // first client stays live across the stagger
    };
    auto server = StunServer<>::create(server_cfg, ec);
    EXPECT_TRUE(server != nullptr);
    if (!server) {
        return;
    }

    auto const server_port = server->port();
    auto server_address = statusbar::net::SocketAddress::ipv4_loopback(server_port);

    std::optional<RegisterResponseSuccess> a_paired;
    std::optional<RegisterResponseSuccess> b_paired;

    ClientConfig a_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xAA, 0xAA, 0xAA, 0xFF, 0xFE, 0x00, 0x00, 0x01},
        .role = Role::Initiator,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { a_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
        .waiting_poll_interval_ms = 50,  // poll fast while waiting
    };
    ClientConfig b_cfg{
        .server_address = server_address,
        .session_id = sid,
        .client_eui64 = ieee::Eui64{0xBB, 0xBB, 0xBB, 0xFF, 0xFE, 0x00, 0x00, 0x02},
        .role = Role::Responder,
        .shared_key = key,
        .on_paired = [&](RegisterResponseSuccess const& r) { b_paired = r; },
        .on_failure = [](std::error_code) {},
        .initial_rto_ms = 20,
        .max_retransmits = 8,
        .waiting_poll_interval_ms = 50,
    };

    auto client_a = StunClientPollable::create(a_cfg, ec);
    EXPECT_TRUE(client_a != nullptr);
    auto client_b = StunClientPollable::create(b_cfg, ec);
    EXPECT_TRUE(client_b != nullptr);

    std::atomic<int64_t> now_ns{1};
    statusbar::itc::StopToken stop;
    statusbar::net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    for (auto& p : server->take_pollables()) {
        reactor.add(std::move(p));
    }

    // Client A arrives first and registers; with no peer it stays Waiting,
    // re-registering every 50ms of sim time.
    reactor.add(std::move(client_a));
    for (int i = 0; i < 60; ++i) {
        reactor.poll_once(5);
    }
    EXPECT_FALSE(a_paired.has_value());  // alone: no pairing yet

    // Client B arrives (staggered). A must now discover pairing on its
    // next short waiting-poll, well before the 15s server refresh.
    reactor.add(std::move(client_b));
    constexpr int64_t refresh_ns = 15'000LL * 1'000'000LL;
    constexpr int64_t cutoff_ns = refresh_ns / 2;  // 7.5s: << one server refresh
    while (now_ns.load() < cutoff_ns) {
        reactor.poll_once(5);
        if (a_paired.has_value() && b_paired.has_value()) {
            break;
        }
    }

    EXPECT_TRUE(a_paired.has_value());
    EXPECT_TRUE(b_paired.has_value());
    // Pairing completed in far less than one server refresh interval —
    // the whole point of the waiting-poll fix.
    EXPECT_TRUE(now_ns.load() < cutoff_ns);
    if (a_paired.has_value() && b_paired.has_value()) {
        EXPECT_TRUE(a_paired->peer_eui64.has_value());
        EXPECT_EQ(a_paired->peer_eui64->to_uint64(), b_cfg.client_eui64.to_uint64());
    }
}

TEST_MAIN(statusbar_stun, stun_loopback_test)
