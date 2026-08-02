// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/stun/stun_server.hpp"

#include "statusbar/net/net_socket.hpp"
#include "statusbar/stun/stun_error.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <print>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::stun {

namespace detail {

ServerCore::ServerCore(ServerConfig config, std::span<SessionTableEntry> sessions)
    : config_{std::move(config)}
    , sessions_{sessions}
{}

auto ServerCore::find_session(SessionId const& sid) -> SessionTableEntry*
{
    for (auto& entry : sessions_) {
        if (entry.occupied && entry.session_id == sid) {
            return &entry;
        }
    }
    return nullptr;
}

auto ServerCore::allocate_session(SessionId const& sid) -> SessionTableEntry*
{
    for (auto& entry : sessions_) {
        if (!entry.occupied) {
            entry = SessionTableEntry{};
            entry.occupied = true;
            entry.session_id = sid;
            return &entry;
        }
    }
    return nullptr;
}

namespace {

/// Pick which slot in the (first, second) pair belongs to a registering
/// client. Returns the slot pointer and a flag indicating whether this
/// is a brand-new arrival vs a refresh of an existing entry.
struct PairSlotPick
{
    ServerSessionEntry* slot{nullptr};
    bool is_new_arrival{false};
    bool is_full{false};
};

[[nodiscard]] auto same_address(statusbar::net::SocketAddress const& a, statusbar::net::SocketAddress const& b) -> bool
{
    if (a.family() != b.family() || !a.valid() || !b.valid()) {
        return false;
    }
    if (a.length() != b.length()) {
        return false;
    }
    return std::memcmp(a.sockaddr(), b.sockaddr(), a.length()) == 0;
}

/// Pick which slot in the (first, second) pair belongs to a registering
/// client. Matching is by source `SocketAddress` so two clients that
/// accidentally share an EUI-64 — same lab, hand-typed identifier
/// collision, etc. — still pair correctly. EUI-64 stays in the slot as
/// metadata that gets forwarded to the peer in the Paired response.
[[nodiscard]] auto pick_pair_slot(SessionTableEntry& entry, statusbar::net::SocketAddress const& source) -> PairSlotPick
{
    PairSlotPick pick{};
    if (entry.ctx.first.present && same_address(entry.ctx.first.reflexive_addr, source)) {
        pick.slot = &entry.ctx.first;
        return pick;
    }
    if (entry.ctx.second.present && same_address(entry.ctx.second.reflexive_addr, source)) {
        pick.slot = &entry.ctx.second;
        return pick;
    }
    if (!entry.ctx.first.present) {
        pick.slot = &entry.ctx.first;
        pick.is_new_arrival = true;
        return pick;
    }
    if (!entry.ctx.second.present) {
        pick.slot = &entry.ctx.second;
        pick.is_new_arrival = true;
        return pick;
    }
    pick.is_full = true;
    return pick;
}

void populate_pair_slot(
    ServerSessionEntry& slot, statusbar::ieee::Eui64 const& eui64, statusbar::net::SocketAddress const& reflexive, int64_t now_ns)
{
    slot.eui64 = eui64;
    slot.reflexive_addr = reflexive;
    slot.last_refresh_ns = now_ns;
    slot.present = true;
}

[[nodiscard]] auto event_for_arrival(SessionTableEntry const& entry, bool is_new_arrival) -> ServerDef::Event
{
    if (!is_new_arrival) {
        return ServerDef::Event::DuplicateEui64;
    }
    if (!entry.ctx.first.present || !entry.ctx.second.present) {
        return ServerDef::Event::FirstRegister;
    }
    return ServerDef::Event::SecondRegister;
}

}  // namespace

auto ServerCore::build_success(
    SessionTableEntry const& entry, ServerSessionEntry const& self, ServerSessionEntry const* peer, TransactionId const& txid) const
    -> ReplyPlan
{
    ReplyPlan plan{};
    plan.success.transaction_id = txid;
    plan.success.session_id = entry.session_id;
    plan.success.xor_mapped_address = self.reflexive_addr;
    plan.success.refresh_interval_ms = config_.refresh_interval_ms;

    bool const both_present = entry.ctx.first.present && entry.ctx.second.present;
    if (both_present && peer != nullptr) {
        // Two clients on different address families share no usable
        // reflexive address — they cannot rendezvous P2P. Reject the
        // pair instead of handing each an address it cannot reach.
        if (self.reflexive_addr.family() != peer->reflexive_addr.family()) {
            ReplyPlan err_plan{};
            err_plan.kind = ReplyKind::Error;
            err_plan.error.transaction_id = txid;
            err_plan.error.error_code = 470;
            err_plan.error_reason = std::string_view{"no common address family"};
            return err_plan;
        }
        plan.success.state = SessionState::Paired;
        plan.success.peer_eui64 = peer->eui64;
        plan.success.peer_xor_mapped_address = peer->reflexive_addr;
        plan.kind = ReplyKind::SuccessPaired;
    } else {
        plan.success.state = SessionState::Waiting;
        plan.kind = ReplyKind::SuccessWaiting;
    }
    return plan;
}

auto ServerCore::plan_reply(RegisterRequest const& req, statusbar::net::SocketAddress const& source, int64_t now_ns) -> ReplyPlan
{
    auto* entry = find_session(req.session_id);
    if (entry == nullptr) {
        entry = allocate_session(req.session_id);
        if (entry == nullptr) {
            ReplyPlan plan{};
            plan.kind = ReplyKind::Error;
            plan.error.transaction_id = req.transaction_id;
            plan.error.error_code = 508;  // closest standard meaning: insufficient capacity
            plan.error_reason = std::string_view{"server capacity"};
            return plan;
        }
    }

    auto pick = pick_pair_slot(*entry, source);
    if (pick.is_full) {
        ReplyPlan plan{};
        plan.kind = ReplyKind::Error;
        plan.error.transaction_id = req.transaction_id;
        plan.error.error_code = 486;  // STUN-style "Allocation Quota Reached"
        plan.error_reason = std::string_view{"session full"};
        return plan;
    }

    populate_pair_slot(*pick.slot, req.client_eui64, source, now_ns);
    entry->ctx.now_ns = now_ns;
    entry->ctx.expiry_ns = now_ns + (static_cast<int64_t>(config_.session_expiry_ms) * 1'000'000LL);

    auto const ev = event_for_arrival(*entry, pick.is_new_arrival);
    entry->sm.handle_event(entry->ctx, ev);

    bool const both_present = entry->ctx.first.present && entry->ctx.second.present;
    ServerSessionEntry const* peer = nullptr;
    if (both_present) {
        peer = (pick.slot == &entry->ctx.first) ? &entry->ctx.second : &entry->ctx.first;
    }

    return build_success(*entry, *pick.slot, peer, req.transaction_id);
}

auto ServerCore::encode_plan(ReplyPlan const& plan, std::span<uint8_t> reply_buf, size_t& reply_len_out) const -> bool
{
    if (plan.kind == ReplyKind::Drop) {
        return false;
    }
    if (plan.kind == ReplyKind::Error) {
        return !encode_register_response_error(plan.error, plan.error_reason, config_.shared_key, reply_buf, reply_len_out);
    }
    if (encode_register_response_success(plan.success, config_.shared_key, reply_buf, reply_len_out)) {
        return false;
    }
    return true;
}

namespace {

[[nodiscard]] auto session_prefix(SessionId const& sid) -> std::string
{
    char buf[17];
    for (size_t i = 0; i < 8; ++i) {
        std::snprintf(buf + (i * 2), 3, "%02x", sid.bytes[i]);
    }
    return std::string{buf, 16};
}

[[nodiscard]] auto plan_label(ReplyKind kind) -> std::string_view
{
    switch (kind) {
        case ReplyKind::SuccessWaiting:
            return "WAITING";
        case ReplyKind::SuccessPaired:
            return "PAIRED";
        case ReplyKind::Error:
            return "ERROR";
        case ReplyKind::Drop:
            return "DROP";
    }
    return "?";
}

void log_event(bool verbose, statusbar::net::SocketAddress const& source, RegisterRequest const& req, ReplyPlan const& plan)
{
    if (!verbose) {
        return;
    }
    std::println(
        stderr,
        "[stun-server] REGISTER session={} eui64=0x{:016x} from={} -> {}",
        session_prefix(req.session_id),
        req.client_eui64.to_uint64(),
        source.to_string(),
        plan_label(plan.kind));
}

void log_decode_failure(bool verbose, statusbar::net::SocketAddress const& source, std::error_code ec)
{
    if (!verbose) {
        return;
    }
    std::println(stderr, "[stun-server] dropped from={} reason={}", source.to_string(), ec.message());
}

}  // namespace

auto ServerCore::on_datagram(
    std::span<uint8_t const> datagram,
    statusbar::net::SocketAddress const& source,
    int64_t now_ns,
    std::span<uint8_t> reply_buf,
    size_t& reply_len_out) -> bool
{
    RegisterRequest req{};
    if (auto ec = decode_register_request(datagram, config_.shared_key, req); ec) {
        log_decode_failure(config_.verbose, source, ec);
        return false;
    }
    auto plan = plan_reply(req, source, now_ns);
    log_event(config_.verbose, source, req, plan);
    return encode_plan(plan, reply_buf, reply_len_out);
}

void ServerCore::expire_old(int64_t now_ns)
{
    int64_t const expiry_ns = static_cast<int64_t>(config_.session_expiry_ms) * 1'000'000LL;
    for (auto& entry : sessions_) {
        if (!entry.occupied) {
            continue;
        }
        bool any_live = false;
        if (entry.ctx.first.present && (now_ns - entry.ctx.first.last_refresh_ns) < expiry_ns) {
            any_live = true;
        }
        if (entry.ctx.second.present && (now_ns - entry.ctx.second.last_refresh_ns) < expiry_ns) {
            any_live = true;
        }
        if (!any_live) {
            entry.sm.handle_event(entry.ctx, ServerDef::Event::ExpireTick);
            entry.occupied = false;
        }
    }
}

auto ServerCore::live_session_count() const noexcept -> size_t
{
    size_t n = 0;
    for (auto const& e : sessions_) {
        if (e.occupied) {
            ++n;
        }
    }
    return n;
}

}  // namespace detail

StunSocketPollable::StunSocketPollable(int fd, detail::ServerCore& core, bool const& finished, bool owns_expiry_tick) noexcept
    : fd_{fd}
    , core_{core}
    , finished_{finished}
    , owns_expiry_tick_{owns_expiry_tick}
{}

StunSocketPollable::~StunSocketPollable()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void StunSocketPollable::on_ready(int64_t now_ns)
{
    while (true) {
        statusbar::net::SocketAddress source{};
        source.reset_length();
        ssize_t const n = ::recvfrom(fd_, rx_buf_.data(), rx_buf_.size(), 0, source.sockaddr(), source.length_ptr());
        if (n <= 0) {
            return;
        }
        size_t reply_len = 0;
        if (!core_.on_datagram(
                std::span<uint8_t const>{rx_buf_.data(), static_cast<size_t>(n)},
                source,
                now_ns,
                std::span<uint8_t>{tx_buf_},
                reply_len)) {
            continue;
        }
        ssize_t const sent = ::sendto(fd_, tx_buf_.data(), reply_len, 0, source.sockaddr(), source.length());
        (void)sent;
    }
}

void StunSocketPollable::tick(int64_t now_ns)
{
    if (owns_expiry_tick_) {
        core_.expire_old(now_ns);
    }
}

namespace {

/// Open + bind one wildcard UDP socket for `family` (AF_INET or
/// AF_INET6). Returns -1 on failure (caller decides if that is fatal).
[[nodiscard]] auto open_wildcard_socket(int family, uint16_t port) -> int
{
    auto const addr =
        (family == AF_INET6) ? statusbar::net::SocketAddress::ipv6_any(port) : statusbar::net::SocketAddress::ipv4_any(port);
    auto fd_result = statusbar::net::create_udp_socket(addr, true, -1, /*ipv6_only=*/family == AF_INET6);
    if (!fd_result) {
        return -1;
    }
    int const fd = fd_result->release();
    if (auto status = statusbar::net::set_nonblocking(fd); !status) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Read the actually-bound port of a socket (for the bind_port == 0 case).
[[nodiscard]] auto socket_port(int fd) -> uint16_t
{
    statusbar::net::SocketAddress bound{};
    bound.reset_length();
    if (::getsockname(fd, bound.sockaddr(), bound.length_ptr()) != 0) {
        return 0;
    }
    return bound.port();
}

}  // namespace

template <size_t MaxSessions>
StunServer<MaxSessions>::StunServer(ServerConfig config, uint16_t port)
    : port_{port}
    , core_{std::move(config), std::span<detail::SessionTableEntry>{sessions_}}
{}

template <size_t MaxSessions>
auto StunServer<MaxSessions>::create(ServerConfig config, std::error_code& ec_out) -> std::unique_ptr<StunServer>
{
    uint16_t const requested_port = config.bind_port;

    // Open the IPv4 socket first. When bind_port was 0, its
    // OS-assigned port becomes the shared port the IPv6 socket binds.
    int v4_fd = open_wildcard_socket(AF_INET, requested_port);
    uint16_t resolved_port = requested_port;
    if (v4_fd >= 0 && requested_port == 0) {
        resolved_port = socket_port(v4_fd);
    }

    int v6_fd = open_wildcard_socket(AF_INET6, (v4_fd >= 0) ? resolved_port : requested_port);
    if (v6_fd >= 0 && v4_fd < 0 && requested_port == 0) {
        resolved_port = socket_port(v6_fd);
    }

    if (v4_fd < 0 && v6_fd < 0) {
        ec_out = make_error_code(StunError::SocketBindFailed);
        return nullptr;
    }
    if (v4_fd < 0) {
        std::println(stderr, "[stun-server] IPv4 socket unavailable; running IPv6-only");
    }
    if (v6_fd < 0) {
        std::println(stderr, "[stun-server] IPv6 socket unavailable; running IPv4-only");
    }

    auto server = std::unique_ptr<StunServer>(new StunServer(std::move(config), resolved_port));

    // The first pollable created owns the once-per-tick expiry sweep.
    bool first = true;
    auto add_socket = [&](int fd) {
        if (fd < 0) {
            return;
        }
        server->pollables_.push_back(std::make_unique<StunSocketPollable>(fd, server->core_, server->finished_, first));
        first = false;
    };
    add_socket(v4_fd);
    add_socket(v6_fd);
    return server;
}

template <size_t MaxSessions>
auto StunServer<MaxSessions>::take_pollables() -> std::vector<std::unique_ptr<statusbar::net::Pollable>>
{
    std::vector<std::unique_ptr<statusbar::net::Pollable>> out;
    out.reserve(pollables_.size());
    for (auto& p : pollables_) {
        out.push_back(std::move(p));
    }
    pollables_.clear();
    return out;
}

template class StunServer<default_max_sessions>;

}  // namespace statusbar::stun
