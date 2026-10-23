#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// A local ATDECC port: controllers on the same host reach the entity over an AF_UNIX datagram
/// socket (la_avdecc's local protocol interface), and AtdeccLocalClient is the controller end.
/// Reactor thread only. See docs/NANOAVB_ARCHITECTURE.md "Local ATDECC socket".

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

namespace statusbar::nanoavb {

/// The controller end's well-known MAC (la_avdecc's local protocol interface):
/// the first local controller's frames appear to come from it.
inline constexpr ieee::Eui48 LOCAL_CONTROLLER_MAC{0x0A, 0xE9, 0x1B, 0x01, 0x01, 0x01};
/// The entity end's well-known MAC: the source a controller gives every frame
/// it receives over the socket.
inline constexpr ieee::Eui48 LOCAL_ENTITY_MAC{0x0A, 0xE9, 0x1B, 0xFF, 0xFF, 0xFF};

/// The largest datagram either end sends or accepts.
inline constexpr size_t LOCAL_PORT_MAX_DATAGRAM = 1500;
/// The smallest acceptable datagram: an AVTP control header.
inline constexpr size_t LOCAL_PORT_MIN_DATAGRAM = 12;

/// The entity end of a local ATDECC socket.
class AtdeccLocalPort final : public net::Pollable
{
  public:
    static constexpr size_t MAX_CLIENTS = 16;
    /// Datagrams handled per on_ready(), so a busy controller cannot starve
    /// the rest of the reactor.
    static constexpr size_t MAX_DATAGRAMS_PER_READY = 32;
    /// Consecutive sends to a controller that may fail for want of buffer
    /// before it is forgotten.
    static constexpr uint32_t MAX_SEND_FAILURES = 64;

    struct Config
    {
        std::string path{};
        /// The socket file's mode (like the control socket's unix_mode).
        int mode{0660};
        /// Forget a controller not heard from for this long; 0 never does.
        int64_t idle_timeout_ns{0};
    };

    /// A frame from a controller, with its synthetic source MAC.
    using ReceiveFn = sg14::inplace_function<void(int64_t now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> frame), 32>;
    /// A controller has been forgotten: drop any state held for @p mac.
    /// Called from on_ready()/tick()/close(), never from inside send().
    using ClientGoneFn = sg14::inplace_function<void(ieee::Eui48 const& mac), 32>;

    /// Bind @p config.path with its mode (at most 0777) and hold "<path>.lock": a stale socket
    /// file is replaced, a live one fails EADDRINUSE, anything else EEXIST.
    [[nodiscard]] static auto open(Config const& config) -> StatusValue<std::unique_ptr<AtdeccLocalPort>>;
    /// Use @p fd, a connected AF_UNIX datagram socket (one end of a
    /// socketpair, or one passed by a service manager), whose peer may be
    /// unnamed. The port owns and closes it.
    [[nodiscard]] static auto adopt(int fd) -> StatusValue<std::unique_ptr<AtdeccLocalPort>>;

    AtdeccLocalPort(AtdeccLocalPort const&) = delete;
    auto operator=(AtdeccLocalPort const&) -> AtdeccLocalPort& = delete;
    AtdeccLocalPort(AtdeccLocalPort&&) = delete;
    auto operator=(AtdeccLocalPort&&) -> AtdeccLocalPort& = delete;
    ~AtdeccLocalPort() override;

    void set_receiver(ReceiveFn fn) { receive_ = std::move(fn); }
    void set_on_client_gone(ClientGoneFn fn) { client_gone_ = std::move(fn); }
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    /// Whether @p mac has the form of a local controller's synthetic MAC: a
    /// unicast to it goes to this port, never onto the network.
    [[nodiscard]] static constexpr auto is_local_mac(ieee::Eui48 const& mac) noexcept -> bool
    {
        return mac.value[0] == 0x0A && mac.value[1] == 0xE9 && mac.value[2] == 0x1B && mac.value[3] == 0x01;
    }

    /// Send @p frame to the controllers @p dest_mac addresses: every one for a
    /// multicast MAC, the one it names for a local MAC, none otherwise.
    /// Returns whether it went to at least one controller.
    auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> frame) -> bool;

    /// Stop: close the socket, remove the socket file if it is still ours,
    /// and forget every controller. finished() is then true.
    void close();

    [[nodiscard]] auto client_count() const noexcept -> size_t;
    [[nodiscard]] auto path() const noexcept -> std::string const& { return path_; }

    struct Counters
    {
        uint64_t rx_frames{0};
        uint64_t rx_dropped{0};  ///< empty, short, oversize, or from an address we cannot reply to
        uint64_t tx_frames{0};
        uint64_t tx_dropped{0};  ///< a controller's queue was full, or another send error
        uint64_t clients_forgotten{0};
    };
    [[nodiscard]] auto counters() const noexcept -> Counters const& { return counters_; }

    // -- Pollable --
    [[nodiscard]] auto fd() const noexcept -> int override { return fd_; }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return fd_ < 0; }

  private:
    struct Client
    {
        sockaddr_un address{};
        socklen_t address_length{0};
        ieee::Eui48 mac{};
        int64_t last_heard_ns{0};
        uint32_t send_failures{0};  ///< consecutive sends that failed for want of buffer
        bool gone{false};
    };

    AtdeccLocalPort(int fd, bool connected, Config config);

    [[nodiscard]] auto find_client(sockaddr_un const& address, socklen_t length) -> Client*;
    [[nodiscard]] auto add_client(sockaddr_un const& address, socklen_t length, int64_t now_ns) -> Client*;
    [[nodiscard]] auto next_mac() -> ieee::Eui48;
    /// Send one datagram to @p client, marking it gone if it has gone.
    auto send_to(Client& client, std::span<uint8_t const> frame) -> bool;
    /// Remove the clients marked gone, telling client_gone_.
    void reap();

    int fd_{-1};
    bool connected_{false};  ///< adopted connected socket: unnamed peers can be replied to
    Config config_{};
    std::string path_{};
    dev_t path_dev_{0};
    ino_t path_ino_{0};
    int lock_fd_{-1};
    sg14::inplace_vector<Client, MAX_CLIENTS> clients_{};
    uint16_t next_mac_suffix_{0x0101};
    ReceiveFn receive_{};
    ClientGoneFn client_gone_{};
    std::optional<logging::Logger> logger_{};
    Counters counters_{};
    std::array<uint8_t, LOCAL_PORT_MAX_DATAGRAM + 1> buffer_{};
};

/// The controller end of a local ATDECC socket, shaped like RawnetContext.
/// Sends ignore the destination MAC (there is one peer); received frames have
/// LOCAL_ENTITY_MAC as source and LOCAL_CONTROLLER_MAC as destination.
class AtdeccLocalClient
{
  public:
    /// How often tick() sends an ADP ENTITY_DISCOVER when nothing else has
    /// been sent, so an entity with an idle timeout keeps the client.
    static constexpr int64_t KEEPALIVE_NS = 5'000'000'000;

    AtdeccLocalClient() = default;
    AtdeccLocalClient(AtdeccLocalClient const&) = delete;
    auto operator=(AtdeccLocalClient const&) -> AtdeccLocalClient& = delete;
    AtdeccLocalClient(AtdeccLocalClient&& other) noexcept;
    auto operator=(AtdeccLocalClient&& other) noexcept -> AtdeccLocalClient&;
    ~AtdeccLocalClient() { close(); }

    /// Bind an address of our own (Linux: autobound abstract) and connect to
    /// the entity at @p path. The entity need not be there yet: a send that
    /// finds it absent connects again.
    [[nodiscard]] auto open(std::string_view path) -> Status;
    void close() noexcept;

    [[nodiscard]] auto valid() const noexcept -> bool { return fd_ >= 0; }
    [[nodiscard]] auto fd() const noexcept -> int { return fd_; }

    [[nodiscard]] auto send(ieee::Eui48 const* dest_mac, std::span<uint8_t const> payload) noexcept -> StatusValue<ssize_t>;
    /// One frame, or failure (EAGAIN when none is waiting). Oversize and
    /// short datagrams are skipped.
    [[nodiscard]] auto recv(ieee::Eui48* src_mac, ieee::Eui48* dest_mac, std::span<uint8_t> payload_buf) noexcept
        -> StatusValue<ssize_t>;

    /// Send an ENTITY_DISCOVER if nothing has been sent for KEEPALIVE_NS.
    void tick(int64_t now_ns) noexcept;

  private:
    [[nodiscard]] auto connect_to_entity() noexcept -> bool;

    int fd_{-1};
    sockaddr_un entity_{};
    socklen_t entity_length_{0};
    std::string bound_path_{};  ///< non-Linux: the path we bound, removed on close
    int64_t last_send_ns_{0};
    bool sent_since_tick_{false};
};

}  // namespace statusbar::nanoavb
