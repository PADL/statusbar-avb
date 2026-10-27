// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_local_port.hpp"

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/buffer/buffer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

namespace statusbar::nanoavb {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = MSG_DONTWAIT;
#endif

[[nodiscard]] auto errno_failure(int const err = errno) -> std::unexpected<std::error_code>
{
    return failure(std::error_code{err, std::system_category()});
}

/// Non-blocking and close-on-exec, portably (no SOCK_NONBLOCK on macOS).
[[nodiscard]] auto make_nonblocking(int const fd) noexcept -> bool
{
    int const flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

[[nodiscard]] auto make_address(std::string_view const path, sockaddr_un& address, socklen_t& length) noexcept -> bool
{
    // A path the length of sun_path would lack its terminating zero.
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return false;
    }
    address = sockaddr_un{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    return true;
}

/// The peer has gone: nothing is bound to its address any more, or (EPERM)
/// its socket is now connected to some other socket.
[[nodiscard]] constexpr auto peer_is_absent(int const err) noexcept -> bool
{
    return err == ECONNREFUSED || err == ENOENT || err == ENOTCONN || err == EPIPE || err == ECONNRESET || err == EPERM;
}

/// Whether the socket file at @p address may be replaced: 0 when nothing serves it, else
/// EADDRINUSE (a live socket answers) or the error that leaves it alone.
[[nodiscard]] auto socket_replaceable(sockaddr_un const& address, socklen_t const length) noexcept -> int
{
    int const probe = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (probe < 0) {
        return errno;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    int const result = ::connect(probe, reinterpret_cast<sockaddr const*>(&address), length) == 0 ? EADDRINUSE : errno;
    ::close(probe);
    return result == ECONNREFUSED || result == ENOENT ? 0 : result;
}

/// The directory part of @p path ("." for none).
[[nodiscard]] auto directory_of(std::string const& path) -> std::string
{
    auto const slash = path.rfind('/');
    if (slash == std::string::npos) {
        return ".";
    }
    return slash == 0 ? std::string{"/"} : path.substr(0, slash);
}

/// Close @p fd after a failed call, keeping that call's errno.
[[nodiscard]] auto close_failure(int const fd) -> std::unexpected<std::error_code>
{
    int const err = errno;
    ::close(fd);
    return errno_failure(err);
}

/// A non-blocking, close-on-exec datagram socket.
[[nodiscard]] auto nonblocking_socket() -> StatusValue<int>
{
    int const fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return errno_failure();
    }
    if (!make_nonblocking(fd)) {
        return close_failure(fd);
    }
    return success(fd);
}

/// mkdtemp(@p pattern), then the path @p leaf names in the new directory.
[[nodiscard]] auto private_socket_path(std::string pattern, std::string_view const leaf) -> StatusValue<std::string>
{
    if (::mkdtemp(pattern.data()) == nullptr) {
        return errno_failure();
    }
    return success(pattern.append(leaf));
}

/// Remove the socket file at @p path, if it is there, and its private directory.
void remove_private_socket(std::string const& path)
{
    (void)::unlink(path.c_str());
    (void)::rmdir(directory_of(path).c_str());
}

/// A non-blocking datagram socket bound at @p address.
[[nodiscard]] auto bound_socket(sockaddr_un const& address, socklen_t const length) -> StatusValue<int>
{
    auto const fd = nonblocking_socket();
    if (!fd) {
        return fd;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    if (::bind(*fd, reinterpret_cast<sockaddr const*>(&address), length) != 0) {
        return close_failure(*fd);
    }
    // Room for a burst to every controller; the kernel caps it (wmem_max).
    int const sndbuf = 1 << 20;
    (void)::setsockopt(*fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return fd;
}

/// A socket bound at @p private_path with @p mode and renamed to @p path (its identity in @p st).
[[nodiscard]] auto bind_and_rename(std::string const& private_path, std::string const& path, mode_t const mode, struct stat& st)
    -> StatusValue<int>
{
    sockaddr_un address{};
    socklen_t length = 0;
    if (!make_address(private_path, address, length)) {
        return failure(std::errc::filename_too_long);
    }
    return bound_socket(address, length).and_then([&](int const fd) -> StatusValue<int> {
        if (::chmod(private_path.c_str(), mode) != 0 || ::lstat(private_path.c_str(), &st) != 0 ||
            ::rename(private_path.c_str(), path.c_str()) != 0) {
            return close_failure(fd);
        }
        return success(fd);
    });
}

/// A non-blocking datagram socket bound at @p path with @p mode (its identity in @p st), bound
/// in a private 0700 directory and renamed into place. Replaces whatever is at @p path.
[[nodiscard]] auto bind_in_place(std::string const& path, mode_t const mode, struct stat& st) -> StatusValue<int>
{
    std::string const dir = directory_of(path);
    // Short: it must fit sun_path.
    auto const private_path = private_socket_path((dir == "/" ? std::string{} : dir) + "/.XXXXXX", "/s");
    if (!private_path) {
        return forward_failure(private_path);
    }
    auto fd = bind_and_rename(*private_path, path, mode, st);
    remove_private_socket(*private_path);  // the socket only if the rename did not happen
    return fd;
}

/// One entity per path: a lock beside it, held while the port is open, so two
/// starting at once cannot both find the path free. The lock's descriptor.
[[nodiscard]] auto lock_beside(std::string const& path) -> StatusValue<int>
{
    std::string const lock_path = path + ".lock";
    int const lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        return errno_failure();
    }
    if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        int const err = errno == EWOULDBLOCK ? EADDRINUSE : errno;
        ::close(lock_fd);
        return errno_failure(err);
    }
    return success(lock_fd);
}

/// The socket bound at @p config's path, replacing a socket left by an entity that did not
/// exit cleanly, but never one something still serves, nor anything else at the path.
[[nodiscard]] auto claim_path(
    AtdeccLocalPort::Config const& config, sockaddr_un const& address, socklen_t const length, struct stat& st) -> StatusValue<int>
{
    if (::lstat(config.path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            return errno_failure(EEXIST);
        }
        if (int const err = socket_replaceable(address, length); err != 0) {
            return errno_failure(err);
        }
    }
    return bind_in_place(config.path, static_cast<mode_t>(config.mode) & 0777U, st);
}

/// Whether @p address is an unnamed sender's (an unbound socket).
[[nodiscard]] auto is_unnamed([[maybe_unused]] sockaddr_un const& address, socklen_t const length) noexcept -> bool
{
#ifdef __linux__
    return length <= static_cast<socklen_t>(offsetof(sockaddr_un, sun_path));
#else
    // No abstract namespace: an unbound sender may come back with an empty path rather than none.
    return length <= static_cast<socklen_t>(offsetof(sockaddr_un, sun_path)) || address.sun_path[0] == '\0';
#endif
}

#ifndef __linux__
/// Bind @p fd at a path of our own in a private directory under the temporary directory,
/// which nobody else can bind or replace; the path. Closes @p fd on failure.
[[nodiscard]] auto bind_client_path(int const fd) -> StatusValue<std::string>
{
    char const* tmpdir = std::getenv("TMPDIR");
    auto const own_path =
        private_socket_path(std::string{tmpdir != nullptr && *tmpdir != '\0' ? tmpdir : "/tmp"} + "/atdecc-client-XXXXXX", "/c");
    if (!own_path) {
        return close_failure(fd);
    }
    sockaddr_un own{};
    socklen_t own_length = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    if (!make_address(*own_path, own, own_length) || ::bind(fd, reinterpret_cast<sockaddr const*>(&own), own_length) != 0) {
        int const err = errno != 0 ? errno : ENAMETOOLONG;
        ::close(fd);
        (void)::rmdir(directory_of(*own_path).c_str());
        return errno_failure(err);
    }
    return own_path;
}
#endif

/// The value of the integer socket option @p option of @p fd.
[[nodiscard]] auto socket_option(int const fd, int const option) -> StatusValue<int>
{
    int value = 0;
    socklen_t len = sizeof(value);
    if (::getsockopt(fd, SOL_SOCKET, option, &value, &len) != 0) {
        return errno_failure();
    }
    return success(value);
}

}  // namespace

//
// AtdeccLocalPort
//

auto AtdeccLocalPort::open(Config const& config) -> StatusValue<std::unique_ptr<AtdeccLocalPort>>
{
    sockaddr_un address{};
    socklen_t length = 0;
    if (!make_address(config.path, address, length)) {
        return failure(std::errc::filename_too_long);
    }
    auto const lock_fd = lock_beside(config.path);
    if (!lock_fd) {
        return forward_failure(lock_fd);
    }
    return open_locked(config, address, length, *lock_fd);
}

auto AtdeccLocalPort::open_locked(Config const& config, sockaddr_un const& address, socklen_t const length, int const lock_fd)
    -> StatusValue<std::unique_ptr<AtdeccLocalPort>>
{
    struct stat st{};
    auto const fd = claim_path(config, address, length, st);
    if (!fd) {
        ::close(lock_fd);
        return forward_failure(fd);
    }
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) - false positive: the unique_ptr owns it
    std::unique_ptr<AtdeccLocalPort> port{new AtdeccLocalPort{*fd, false, config}};
    port->own_path(config.path, st.st_dev, st.st_ino, lock_fd);
    return success(std::move(port));
}

void AtdeccLocalPort::own_path(std::string const& path, dev_t const dev, ino_t const ino, int const lock_fd)
{
    path_ = path;
    path_dev_ = dev;
    path_ino_ = ino;
    lock_fd_ = lock_fd;
}

auto AtdeccLocalPort::adopt(int const fd) -> StatusValue<std::unique_ptr<AtdeccLocalPort>>
{
#ifdef SO_DOMAIN
    auto const domain = socket_option(fd, SO_DOMAIN);
#else
    StatusValue<int> const domain{AF_UNIX};
#endif
    auto const type = socket_option(fd, SO_TYPE);
    if (!domain || !type) {
        return forward_failure(!domain ? domain : type);
    }
    if (*domain != AF_UNIX || *type != SOCK_DGRAM) {
        return failure(std::errc::wrong_protocol_type);
    }
    if (!make_nonblocking(fd)) {
        return errno_failure();
    }
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks) - false positive: the unique_ptr owns it
    return success(std::unique_ptr<AtdeccLocalPort>{new AtdeccLocalPort{fd, true, Config{}}});
}

AtdeccLocalPort::AtdeccLocalPort(int const fd, bool const connected, Config config)
    : fd_{fd}
    , connected_{connected}
    , config_{std::move(config)}
{}

AtdeccLocalPort::~AtdeccLocalPort()
{
    // Whatever the callback reaches may already be gone (the reactor that
    // owns this port can outlive the entity, or the other way round).
    client_gone_ = {};
    close();
}

void AtdeccLocalPort::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!path_.empty()) {
        // Only our own socket file: a newer entity may have bound the path.
        struct stat st{};
        if (::lstat(path_.c_str(), &st) == 0 && st.st_dev == path_dev_ && st.st_ino == path_ino_) {
            (void)::unlink(path_.c_str());
        }
        path_.clear();
    }
    if (lock_fd_ >= 0) {
        // The lock file stays: removing it would let a newcomer lock a new
        // file while an old one still holds the unlinked one.
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
    for (auto& client : clients_) {
        client.gone = true;
    }
    reap();
}

auto AtdeccLocalPort::client_count() const noexcept -> size_t
{
    return static_cast<size_t>(std::ranges::count_if(clients_, [](Client const& c) { return !c.gone; }));
}

auto AtdeccLocalPort::find_client(sockaddr_un const& address, socklen_t const length) -> Client*
{
    for (auto& client : clients_) {
        if (!client.gone && client.address_length == length && std::memcmp(&client.address, &address, length) == 0) {
            return &client;
        }
    }
    return nullptr;
}

auto AtdeccLocalPort::next_mac() -> ieee::Eui48
{
    // Fresh MACs in turn, so state the entity still holds for a forgotten
    // controller's MAC (a deferred AECP response) never reaches a new one.
    for (;;) {
        uint16_t const suffix = next_mac_suffix_++;
        ieee::Eui48 const mac{
            LOCAL_CONTROLLER_MAC.value[0],
            LOCAL_CONTROLLER_MAC.value[1],
            LOCAL_CONTROLLER_MAC.value[2],
            LOCAL_CONTROLLER_MAC.value[3],
            static_cast<uint8_t>(suffix >> 8),
            static_cast<uint8_t>(suffix & 0xFF)};
        if (std::ranges::none_of(clients_, [&mac](Client const& c) { return c.mac == mac; })) {
            return mac;
        }
    }
}

auto AtdeccLocalPort::add_client(sockaddr_un const& address, socklen_t const length, int64_t const now_ns) -> Client*
{
    if (clients_.size() == MAX_CLIENTS) {
        auto* oldest = std::ranges::min_element(clients_, {}, &Client::last_heard_ns);
        oldest->gone = true;
        reap();
    }
    auto& client = clients_.emplace_back(Client{.address_length = length, .mac = next_mac(), .last_heard_ns = now_ns});
    std::memcpy(&client.address, &address, length);
    if (logger_) {
        logger_->status("local ATDECC controller joined ({} connected)", client_count());
    }
    return &client;
}

void AtdeccLocalPort::reap()
{
    for (size_t i = 0; i < clients_.size();) {
        if (!clients_[i].gone) {
            ++i;
            continue;
        }
        ieee::Eui48 const mac = clients_[i].mac;
        clients_.erase(clients_.begin() + static_cast<ptrdiff_t>(i));
        ++counters_.clients_forgotten;
        if (logger_) {
            logger_->status("local ATDECC controller left ({} connected)", client_count());
        }
        if (client_gone_) {
            client_gone_(mac);
        }
    }
}

void AtdeccLocalPort::on_ready(int64_t const now_ns)
{
    for (size_t n = 0; n < MAX_DATAGRAMS_PER_READY && fd_ >= 0; ++n) {
        // A safe point to tell the entity about controllers found gone while
        // it was sending: it is not in the middle of anything now.
        reap();
        sockaddr_un address{};
        socklen_t length = sizeof(address);
        ssize_t const received = receive_datagram(address, length);
        if (received < 0) {
            return;
        }
        if (received > 0) {
            deliver(address, length, static_cast<size_t>(received), now_ns);
        }
    }
}

auto AtdeccLocalPort::receive_datagram(sockaddr_un& address, socklen_t& length) -> ssize_t
{
    // MSG_TRUNC: the datagram's real length, so oversize is seen as such.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    ssize_t const received =
        ::recvfrom(fd_, buffer_.data(), buffer_.size(), MSG_TRUNC | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&address), &length);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    length = std::min<socklen_t>(length, sizeof(address));  // a truncated address is reported at full length
    if (received < 0) {
        if (errno == EINTR) {
            return 0;
        }
        // EAGAIN: drained. An adopted connected socket whose peer has
        // closed reports that once (ECONNRESET) and is then useless.
        if (connected_ && peer_is_absent(errno)) {
            close();
        }
        return -1;
    }
    if (std::cmp_less(received, LOCAL_PORT_MIN_DATAGRAM) || std::cmp_greater(received, LOCAL_PORT_MAX_DATAGRAM)) {
        ++counters_.rx_dropped;
        return 0;
    }
    return received;
}

void AtdeccLocalPort::deliver(sockaddr_un const& address, socklen_t const length, size_t const size, int64_t const now_ns)
{
    bool const unnamed = is_unnamed(address, length);
    // An unnamed peer can be answered only on a connected socket.
    if (unnamed && !connected_) {
        ++counters_.rx_dropped;
        return;
    }
    ieee::Eui48 const mac =
        heard_from(address, unnamed ? 0 : length, now_ns).mac;  // the receiver may add no clients, but copy anyway
    ++counters_.rx_frames;
    if (receive_) {
        receive_(now_ns, mac, std::span<uint8_t const>{buffer_.data(), size});
    }
}

auto AtdeccLocalPort::heard_from(sockaddr_un const& address, socklen_t const length, int64_t const now_ns) -> Client&
{
    Client* client = find_client(address, length);
    if (client == nullptr) {
        client = add_client(address, length, now_ns);
    }
    client->last_heard_ns = now_ns;
    return *client;
}

void AtdeccLocalPort::tick(int64_t const now_ns)
{
    if (config_.idle_timeout_ns > 0) {
        for (auto& client : clients_) {
            if (now_ns - client.last_heard_ns > config_.idle_timeout_ns) {
                client.gone = true;
            }
        }
    }
    reap();
}

auto AtdeccLocalPort::send_to(Client& client, std::span<uint8_t const> const frame) -> bool
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    auto const* address = client.address_length != 0 ? reinterpret_cast<sockaddr const*>(&client.address) : nullptr;
    for (;;) {
        ssize_t const sent = ::sendto(fd_, frame.data(), frame.size(), SEND_FLAGS, address, client.address_length);
        if (sent >= 0) {
            ++counters_.tx_frames;
            client.send_failures = 0;
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (peer_is_absent(errno)) {
            // Forgotten at the next safe point (reap()), not here: the
            // entity may be iterating its own state to send this.
            client.gone = true;
        } else if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) && ++client.send_failures >= MAX_SEND_FAILURES) {
            // The frame is lost rather than the reactor blocking for it; a
            // controller that keeps not reading is forgotten (it rejoins
            // with its next datagram).
            client.gone = true;
        }
        ++counters_.tx_dropped;
        return false;
    }
}

auto AtdeccLocalPort::send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> const frame) -> bool
{
    if (fd_ < 0 || frame.size() > LOCAL_PORT_MAX_DATAGRAM) {
        return false;
    }
    bool delivered = false;
    if (dest_mac.is_multicast()) {
        for (auto& client : clients_) {
            if (!client.gone) {
                delivered = send_to(client, frame) || delivered;
            }
        }
        return delivered;
    }
    for (auto& client : clients_) {
        if (!client.gone && client.mac == dest_mac) {
            return send_to(client, frame);
        }
    }
    return false;
}

//
// AtdeccLocalClient
//

AtdeccLocalClient::AtdeccLocalClient(AtdeccLocalClient&& other) noexcept
    : fd_{std::exchange(other.fd_, -1)}
    , entity_{other.entity_}
    , entity_length_{other.entity_length_}
    , bound_path_{std::move(other.bound_path_)}
    , last_send_ns_{other.last_send_ns_}
    , sent_since_tick_{other.sent_since_tick_}
{
    other.bound_path_.clear();
}

auto AtdeccLocalClient::operator=(AtdeccLocalClient&& other) noexcept -> AtdeccLocalClient&
{
    if (this != &other) {  // NOLINT(statusbar-block-statements): a member-wise move
        close();
        fd_ = std::exchange(other.fd_, -1);
        entity_ = other.entity_;
        entity_length_ = other.entity_length_;
        bound_path_ = std::move(other.bound_path_);
        other.bound_path_.clear();
        last_send_ns_ = other.last_send_ns_;
        sent_since_tick_ = other.sent_since_tick_;
    }
    return *this;
}

auto AtdeccLocalClient::open(std::string_view const path) -> Status
{
    close();
    if (!make_address(path, entity_, entity_length_)) {
        return failure(std::errc::filename_too_long);
    }
    auto const fd = nonblocking_socket();
    if (!fd) {
        return forward_failure(fd);
    }
    return bind_and_connect(*fd);
}

auto AtdeccLocalClient::bind_and_connect(int const fd) -> Status
{
    // An unbound socket has no address for the entity to reply to.
#ifdef __linux__
    sockaddr_un own{};
    own.sun_family = AF_UNIX;
    // Autobind: an abstract address of the kernel's choosing.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    if (::bind(fd, reinterpret_cast<sockaddr const*>(&own), sizeof(sa_family_t)) != 0) {
        return close_failure(fd);
    }
#else
    auto bound = bind_client_path(fd);
    if (!bound) {
        return forward_failure(bound);
    }
    bound_path_ = std::move(*bound);
#endif
    fd_ = fd;
    // The entity may not be listening yet; the first send connects then.
    (void)connect_to_entity();
    return success();
}

void AtdeccLocalClient::close() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!bound_path_.empty()) {
        remove_private_socket(bound_path_);
        bound_path_.clear();
    }
}

auto AtdeccLocalClient::connect_to_entity() noexcept -> bool
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
    return ::connect(fd_, reinterpret_cast<sockaddr const*>(&entity_), entity_length_) == 0;
}

auto AtdeccLocalClient::send(ieee::Eui48 const* /*dest_mac*/, std::span<uint8_t const> const payload) noexcept
    -> StatusValue<ssize_t>
{
    if (fd_ < 0) {
        return failure(std::errc::bad_file_descriptor);
    }
    if (payload.size() > LOCAL_PORT_MAX_DATAGRAM) {
        return failure(std::errc::message_size);
    }
    bool reconnected = false;
    for (;;) {
        ssize_t const sent = ::send(fd_, payload.data(), payload.size(), SEND_FLAGS);
        if (sent >= 0) {
            sent_since_tick_ = true;
            return success(sent);
        }
        int const err = errno;
        if (err == EINTR) {
            continue;
        }
        // The entity has gone, and may be bound to the path again.
        if (peer_is_absent(err) && !reconnected && connect_to_entity()) {
            reconnected = true;
            continue;
        }
        return errno_failure(err);
    }
}

auto AtdeccLocalClient::recv(ieee::Eui48* const src_mac, ieee::Eui48* const dest_mac, std::span<uint8_t> const payload_buf) noexcept
    -> StatusValue<ssize_t>
{
    if (fd_ < 0) {
        return failure(std::errc::bad_file_descriptor);
    }
    for (;;) {
        ssize_t const received = ::recv(fd_, payload_buf.data(), payload_buf.size(), MSG_TRUNC | MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno_failure();
        }
        auto const size = static_cast<size_t>(received);
        if (size < LOCAL_PORT_MIN_DATAGRAM || size > LOCAL_PORT_MAX_DATAGRAM || size > payload_buf.size()) {
            continue;
        }
        if (src_mac != nullptr) {
            *src_mac = LOCAL_ENTITY_MAC;
        }
        if (dest_mac != nullptr) {
            *dest_mac = LOCAL_CONTROLLER_MAC;
        }
        return success(received);
    }
}

void AtdeccLocalClient::tick(int64_t const now_ns) noexcept
{
    if (fd_ < 0) {
        return;
    }
    if (sent_since_tick_) {
        sent_since_tick_ = false;
        last_send_ns_ = now_ns;
        return;
    }
    if (now_ns - last_send_ns_ < KEEPALIVE_NS) {
        return;
    }
    atdecc::AdpDu discover{};
    discover.init_entity_discover();
    (void)send(nullptr, make_const_span(discover));
}

}  // namespace statusbar::nanoavb
