// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/stun/stun_client.hpp"

#include "statusbar/net/net_socket.hpp"
#include "statusbar/secure_random/secure_random.hpp"
#include "statusbar/stun/stun_error.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <print>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>

namespace statusbar::stun {

namespace {

[[nodiscard]] auto txid_prefix(TransactionId const& tx) -> std::string
{
    char buf[17];
    for (size_t i = 0; i < 8; ++i) {
        std::snprintf(buf + (i * 2), 3, "%02x", tx.bytes[i]);
    }
    return std::string{buf, 16};
}

[[nodiscard]] auto state_label(SessionState s) -> std::string_view
{
    switch (s) {
        case SessionState::Waiting:
            return "WAITING";
        case SessionState::Paired:
            return "PAIRED";
        case SessionState::Expired:
            return "EXPIRED";
        case SessionState::Full:
            return "FULL";
    }
    return "?";
}

void fill_random_transaction_id(TransactionId& out)
{
    // RFC 8489 §5 requires cryptographic randomness for transaction IDs
    // to prevent same-port off-path injection and cross-transaction
    // confusion attacks.
    secure_random_bytes(std::span<uint8_t>{out.bytes.data(), out.bytes.size()});
}

[[nodiscard]] auto open_client_socket(int family, std::error_code& ec_out) -> int
{
    int const fd = ::socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        ec_out = make_error_code(StunError::SocketCreateFailed);
        return -1;
    }
    if (auto status = statusbar::net::set_nonblocking(fd); !status) {
        ::close(fd);
        ec_out = make_error_code(StunError::SocketCreateFailed);
        return -1;
    }
    return fd;
}

}  // namespace

auto StunClientPollable::create(ClientConfig config, std::error_code& ec_out) -> std::unique_ptr<StunClientPollable>
{
    if (!config.server_address.valid()) {
        ec_out = make_error_code(StunError::NoServerAddress);
        return nullptr;
    }
    int const fd = open_client_socket(config.server_address.family(), ec_out);
    if (fd < 0) {
        return nullptr;
    }
    return std::unique_ptr<StunClientPollable>(new StunClientPollable(std::move(config), fd));
}

StunClientPollable::StunClientPollable(ClientConfig config, int fd)
    : config_{std::move(config)}
    , fd_{fd}
{
    ctx_.refresh_interval_ms = default_refresh_interval_ms;
}

StunClientPollable::~StunClientPollable()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

auto StunClientPollable::current_rto_ns() const noexcept -> int64_t
{
    int64_t rto_ms = config_.initial_rto_ms;
    for (int i = 0; i < ctx_.retransmits; ++i) {
        rto_ms *= 2;
    }
    return rto_ms * 1'000'000LL;
}

void StunClientPollable::send_register_request(int64_t now_ns)
{
    fill_random_transaction_id(outstanding_txid_);
    RegisterRequest const req{
        .transaction_id = outstanding_txid_,
        .session_id = config_.session_id,
        .client_eui64 = config_.client_eui64,
        .role = config_.role,
    };

    std::array<uint8_t, max_message_length> buf{};
    size_t written = 0;
    if (auto ec = encode_register_request(req, config_.shared_key, buf, written); ec) {
        emit_failure(ec);
        return;
    }

    ssize_t const sent = ::sendto(fd_, buf.data(), written, 0, config_.server_address.sockaddr(), config_.server_address.length());
    if (sent < 0) {
        emit_failure(std::error_code(errno, std::generic_category()));
        return;
    }
    request_sent_ns_ = now_ns;
    ctx_.request_sent_ns = now_ns;
    if (config_.verbose) {
        std::println(
            stderr,
            "[stun-client] -> REGISTER txid={} retransmit={} bytes={}",
            txid_prefix(outstanding_txid_),
            ctx_.retransmits,
            written);
    }
}

void StunClientPollable::apply_action_flags()
{
    if (!ctx_.emit_send) {
        return;
    }
    ctx_.emit_send = false;
    send_register_request(ctx_.now_ns);
}

void StunClientPollable::on_ready(int64_t now_ns)
{
    while (true) {
        statusbar::net::SocketAddress source{};
        source.reset_length();
        ssize_t const n = ::recvfrom(fd_, rx_buf_.data(), rx_buf_.size(), 0, source.sockaddr(), source.length_ptr());
        if (n <= 0) {
            break;
        }
        handle_datagram(std::span<uint8_t const>{rx_buf_.data(), static_cast<size_t>(n)}, now_ns);
        if (finished_) {
            return;
        }
    }
}

void StunClientPollable::handle_success(RegisterResponseSuccess const& success, int64_t now_ns)
{
    if (config_.verbose) {
        std::println(
            stderr,
            "[stun-client] <- {} reflexive={} refresh={}ms",
            state_label(success.state),
            success.xor_mapped_address.to_string(),
            success.refresh_interval_ms);
    }
    if (success.state == SessionState::Paired) {
        sm_.handle_event(ctx_, ClientDef::Event::ResponsePaired);
        last_paired_response_ns_ = now_ns;
        ctx_.refresh_interval_ms = success.refresh_interval_ms;
        if (!paired_callback_fired_ && config_.on_paired) {
            paired_callback_fired_ = true;
            config_.on_paired(success);
        }
        return;
    }
    if (success.state == SessionState::Waiting) {
        sm_.handle_event(ctx_, ClientDef::Event::ResponseWaiting);
        ctx_.refresh_interval_ms = success.refresh_interval_ms;
        return;
    }
    if (success.state == SessionState::Expired) {
        sm_.handle_event(ctx_, ClientDef::Event::ResponseSessionExpired);
        return;
    }
    sm_.handle_event(ctx_, ClientDef::Event::ResponseError);
}

void StunClientPollable::handle_error(RegisterResponseError const& err)
{
    if (config_.verbose) {
        std::println(stderr, "[stun-client] <- ERROR code={}", err.error_code);
    }
    sm_.handle_event(ctx_, ClientDef::Event::ResponseError);
}

void StunClientPollable::handle_datagram(std::span<uint8_t const> datagram, int64_t now_ns)
{
    bool is_success = false;
    RegisterResponseSuccess success{};
    RegisterResponseError err{};
    if (auto ec = decode_register_response(datagram, config_.shared_key, is_success, success, err); ec) {
        if (config_.verbose) {
            std::println(stderr, "[stun-client] <- dropped reason={}", ec.message());
        }
        return;
    }

    auto const& reply_txid = is_success ? success.transaction_id : err.transaction_id;
    if (reply_txid != outstanding_txid_) {
        if (config_.verbose) {
            std::println(stderr, "[stun-client] <- stale txid={}", txid_prefix(reply_txid));
        }
        return;
    }

    ctx_.now_ns = now_ns;
    if (is_success) {
        handle_success(success, now_ns);
    } else {
        handle_error(err);
    }
    apply_action_flags();
}

void StunClientPollable::emit_failure(std::error_code ec)
{
    if (failure_callback_fired_) {
        finished_ = true;
        return;
    }
    failure_callback_fired_ = true;
    finished_ = true;
    if (config_.on_failure) {
        config_.on_failure(ec);
    }
}

void StunClientPollable::tick(int64_t now_ns)
{
    ctx_.now_ns = now_ns;

    if (sm_.current_state() == ClientDef::State::Idle) {
        sm_.handle_event(ctx_, ClientDef::Event::Start);
        apply_action_flags();
        return;
    }

    if (sm_.current_state() == ClientDef::State::Failed) {
        if (ctx_.emit_failure) {
            ctx_.emit_failure = false;
            emit_failure(make_error_code(StunError::SessionExpired));
        }
        return;
    }

    if (sm_.current_state() == ClientDef::State::Registering) {
        int64_t const elapsed = now_ns - request_sent_ns_;
        if (elapsed >= current_rto_ns()) {
            if (ctx_.retransmits >= config_.max_retransmits) {
                sm_.handle_event(ctx_, ClientDef::Event::RetryBudgetExhausted);
            } else {
                sm_.handle_event(ctx_, ClientDef::Event::Rto);
            }
            apply_action_flags();
        }
        return;
    }

    // While Waiting, poll fast so pairing is discovered quickly after the
    // peer arrives (the server is reactive and never pushes Paired to an
    // idle waiter): re-register at the FASTER of our waiting-poll cap and
    // the server-suggested refresh, so a short server refresh still drives
    // quick re-registration while the cap bounds a long one (e.g. 15s) to
    // ~1s. Once Paired, use the server refresh alone as the NAT keepalive.
    uint32_t interval_ms = ctx_.refresh_interval_ms;
    if (sm_.current_state() == ClientDef::State::Waiting) {
        interval_ms = std::min(config_.waiting_poll_interval_ms, ctx_.refresh_interval_ms);
    }
    int64_t const refresh_ns = static_cast<int64_t>(interval_ms) * 1'000'000LL;
    if (now_ns - request_sent_ns_ >= refresh_ns) {
        sm_.handle_event(ctx_, ClientDef::Event::RefreshTick);
        apply_action_flags();
    }
}

}  // namespace statusbar::stun
