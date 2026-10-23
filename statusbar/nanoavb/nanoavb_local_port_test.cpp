// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The local ATDECC socket: controllers reach the entity's ADP, AECP and ACMP handlers through
// NanoAvbNetHandlers with no raw socket (the ATDECC handler runs tick-only).

#include "statusbar/nanoavb/nanoavb_local_port.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

namespace ent_caps = statusbar::atdecc::entity_capabilities;

Eui64 const ENTITY_ID{0x00, 0x01, 0x02, 0xFF, 0xFE, 0x05, 0x06, 0x07};
Eui64 const CONTROLLER_A{0x0A, 0xE9, 0x1B, 0x02, 0x00, 0x00, 0x00, 0x01};
Eui64 const CONTROLLER_B{0x0A, 0xE9, 0x1B, 0x02, 0x00, 0x00, 0x00, 0x02};

using Frame = std::vector<uint8_t>;

/// A socket path short enough for sun_path, unique to this process. Its
/// "<path>.lock" (which the port leaves behind by design) goes at exit.
auto temporary_socket_path() -> std::string
{
    struct LockFiles
    {
        std::vector<std::string> paths;
        ~LockFiles()
        {
            for (auto const& p : paths) {
                ::unlink((p + ".lock").c_str());
            }
        }
    };
    static LockFiles lock_files;
    static int counter = 0;
    lock_files.paths.push_back(std::format("/tmp/nanoavb-local-{}-{}.sock", ::getpid(), counter++));
    return lock_files.paths.back();
}

auto make_model() -> EntityModel
{
    EntityModelConfig cfg;
    cfg.max_configurations = 1;
    cfg.max_stream_inputs = 2;
    cfg.max_stream_outputs = 2;
    EntityModel model{cfg};
    DescriptorEntity entity{};
    entity.entity_id = ENTITY_ID;
    entity.entity_capabilities = ent_caps::AEM_SUPPORTED;
    entity.talker_stream_sources = 2;
    entity.listener_stream_sinks = 2;
    entity.entity_name = AtdeccString{"Local Port Test"};
    entity.configurations_count = 1;
    model.set_entity(entity);
    (void)model.add_configuration(DescriptorConfiguration{});
    (void)model.add_stream_output(DescriptorStream{});
    (void)model.add_stream_output(DescriptorStream{});
    (void)model.add_stream_input(DescriptorStream{});
    (void)model.add_stream_input(DescriptorStream{});
    return model;
}

/// An entity whose only way in is the local socket at path.
struct Bench
{
    explicit Bench(bool const milan = true, AtdeccLocalPort::Config config = {.path = temporary_socket_path()})
        : components{make_model(), AdpAdvertiserConfig{}, 2, 4, 2, milan}
        , handlers{"nanoavb-none0", components}
        , reactor{stop, [this] { return now_ns; }, 0}
        , path{config.path}
    {
        handlers.set_local_port_config(std::move(config));
        handlers.add_to_reactor(reactor);
        setup_nanoavb_callbacks(components, handlers);
        (void)components.adp_advertiser.start(sm::TimePoint{std::chrono::nanoseconds{now_ns}});
    }

    /// A few reactor cycles, 1 ms apart.
    void pump()
    {
        for (int i = 0; i < 3; ++i) {
            now_ns += 1'000'000;
            (void)reactor.poll_once(0);
        }
    }

    [[nodiscard]] auto port() -> AtdeccLocalPort& { return *handlers.local_port(); }

    itc::StopToken stop{};
    int64_t now_ns{1'000'000'000};
    NanoAvbComponents components;
    NanoAvbNetHandlers handlers;
    net::MessageReactor reactor;
    std::string path;
};

auto open_client(std::string const& path) -> AtdeccLocalClient
{
    AtdeccLocalClient client{};
    EXPECT_TRUE(client.open(path).has_value());
    return client;
}

/// Every frame waiting for @p client.
auto drain(AtdeccLocalClient& client) -> std::vector<Frame>
{
    std::vector<Frame> frames;
    std::array<uint8_t, 2048> buf{};
    Eui48 src{};
    Eui48 dst{};
    while (auto n = client.recv(&src, &dst, buf)) {
        frames.emplace_back(buf.begin(), buf.begin() + *n);
        EXPECT_TRUE(src == LOCAL_ENTITY_MAC);
    }
    return frames;
}

auto subtype_of(Frame const& f) -> uint8_t
{
    return f[0];
}
auto message_type_of(Frame const& f) -> uint8_t
{
    return f[1] & 0x0F;
}

auto count_of(std::vector<Frame> const& frames, uint8_t const subtype, uint8_t const message_type) -> size_t
{
    return static_cast<size_t>(std::ranges::count_if(
        frames, [&](Frame const& f) { return subtype_of(f) == subtype && message_type_of(f) == message_type; }));
}

auto entity_discover() -> Frame
{
    AdpDu du{};
    du.init_entity_discover();
    auto const bytes = make_const_span(du);
    return {bytes.begin(), bytes.end()};
}

auto aem_command(
    Eui64 const& controller, uint16_t const command, std::span<uint8_t const> const body = {}, uint16_t const sequence = 1) -> Frame
{
    AemDu du{};
    du.init_command(command, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + body.size()));
    du.target_entity_id = ENTITY_ID;
    du.controller_entity_id = controller;
    du.sequence_id = sequence;
    Frame out(AemDu::LENGTH + body.size());
    span_store(std::span<uint8_t>{out}.first(AemDu::LENGTH), du);
    std::ranges::copy(body, out.begin() + AemDu::LENGTH);
    return out;
}

auto read_entity_descriptor(Eui64 const& controller, uint16_t const sequence = 1) -> Frame
{
    std::array<uint8_t, 8> const body{0, 0, 0, 0, 0, 0, 0, 0};  // config 0, ENTITY 0
    return aem_command(controller, AEM_COMMAND_READ_DESCRIPTOR, body, sequence);
}

auto aem_header(Frame const& f) -> AemDu
{
    AemDu du{};
    span_load(du, std::span<uint8_t const>{f}.first(AemDu::LENGTH));
    return du;
}

/// The AEM responses among @p frames that answer a command (U bit clear).
auto solicited(std::vector<Frame> const& frames) -> std::vector<AemDu>
{
    std::vector<AemDu> out;
    for (auto const& f : frames) {
        if (subtype_of(f) == avtp::AvtpSubtype::aecp && f.size() >= AemDu::LENGTH && !aem_header(f).is_unsolicited()) {
            out.push_back(aem_header(f));
        }
    }
    return out;
}

auto send(AtdeccLocalClient& client, Frame const& frame) -> bool
{
    return client.send(nullptr, frame).has_value();
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(nanoavb_local_port, discover_is_answered_with_entity_available)
{
    Bench bench;
    auto client = open_client(bench.path);
    EXPECT_TRUE(send(client, entity_discover()));
    bench.pump();
    auto const frames = drain(client);
    EXPECT_EQ(count_of(frames, avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{1});
    AdpDu adp{};
    span_load(adp, std::span<uint8_t const>{frames.front()});
    EXPECT_TRUE(adp.entity_id == ENTITY_ID);
    EXPECT_EQ(bench.port().client_count(), size_t{1});
}

TEST(nanoavb_local_port, a_local_discover_is_answered_to_that_client_alone)
{
    Bench bench;
    auto a = open_client(bench.path);
    auto b = open_client(bench.path);
    EXPECT_TRUE(send(b, entity_discover()));
    bench.pump();
    (void)drain(b);
    EXPECT_TRUE(send(a, entity_discover()));
    bench.pump();
    EXPECT_EQ(count_of(drain(a), avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{1});
    EXPECT_EQ(drain(b).size(), size_t{0});
    // The entity's own advertisements reach every client.
    bench.components.adp_advertiser.notify_entity_changed();
    bench.pump();
    EXPECT_EQ(count_of(drain(a), avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{1});
    EXPECT_EQ(count_of(drain(b), avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{1});
}

TEST(nanoavb_local_port, a_local_discover_is_answered_while_the_network_is_down)
{
    Bench bench;
    (void)bench.components.adp_advertiser.stop(sm::TimePoint{});  // e.g. link down
    auto client = open_client(bench.path);
    EXPECT_TRUE(send(client, entity_discover()));
    bench.pump();
    auto const frames = drain(client);
    EXPECT_EQ(count_of(frames, avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{1});
}

TEST(nanoavb_local_port, aem_response_goes_only_to_the_requester)
{
    Bench bench;
    auto a = open_client(bench.path);
    auto b = open_client(bench.path);
    EXPECT_TRUE(send(b, entity_discover()));  // b is known too
    bench.pump();
    (void)drain(a);
    (void)drain(b);

    EXPECT_TRUE(send(a, read_entity_descriptor(CONTROLLER_A, 7)));
    bench.pump();
    auto const to_a = drain(a);
    auto const to_b = drain(b);
    EXPECT_EQ(to_a.size(), size_t{1});
    EXPECT_EQ(to_b.size(), size_t{0});
    auto const header = aem_header(to_a.front());
    EXPECT_TRUE(header.is_response());
    EXPECT_EQ(header.status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(header.command_code(), AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_EQ(header.sequence_id.get(), uint16_t{7});
    EXPECT_TRUE(header.controller_entity_id == CONTROLLER_A);
}

TEST(nanoavb_local_port, two_clients_each_get_their_own_responses)
{
    Bench bench;
    auto a = open_client(bench.path);
    auto b = open_client(bench.path);
    EXPECT_TRUE(send(a, read_entity_descriptor(CONTROLLER_A, 1)));
    EXPECT_TRUE(send(b, read_entity_descriptor(CONTROLLER_B, 2)));
    bench.pump();
    auto const to_a = drain(a);
    auto const to_b = drain(b);
    EXPECT_EQ(to_a.size(), size_t{1});
    EXPECT_EQ(to_b.size(), size_t{1});
    EXPECT_EQ(aem_header(to_a.front()).sequence_id.get(), uint16_t{1});
    EXPECT_EQ(aem_header(to_b.front()).sequence_id.get(), uint16_t{2});
}

TEST(nanoavb_local_port, get_milan_info_reaches_the_mvu_handler)
{
    Bench bench;
    auto client = open_client(bench.path);
    mvu::MvuDu du{};
    du.init_command(mvu::MVU_COMMAND_GET_MILAN_INFO, static_cast<uint16_t>(mvu::MvuGetMilanInfoCommand::LENGTH));
    du.common.target_entity_id = ENTITY_ID;
    du.common.controller_entity_id = CONTROLLER_A;
    Frame command(mvu::MvuDu::LENGTH + mvu::MvuGetMilanInfoCommand::LENGTH, 0);
    span_store(std::span<uint8_t>{command}.first(mvu::MvuDu::LENGTH), du);
    EXPECT_TRUE(send(client, command));
    bench.pump();
    auto const frames = drain(client);
    EXPECT_EQ(count_of(frames, avtp::AvtpSubtype::aecp, AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE), size_t{1});
    EXPECT_EQ(frames.front()[2] >> 3, AEM_STATUS_SUCCESS);  // the AECP status field
}

TEST(nanoavb_local_port, acmp_response_is_multicast_to_every_client)
{
    Bench bench;
    auto a = open_client(bench.path);
    auto b = open_client(bench.path);
    EXPECT_TRUE(send(b, entity_discover()));
    bench.pump();
    (void)drain(a);
    (void)drain(b);

    AcmpDu acmp{};
    acmp.init_command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND);
    acmp.controller_entity_id = CONTROLLER_A;
    acmp.talker_entity_id = ENTITY_ID;
    acmp.talker_unique_id = 0;
    acmp.sequence_id = 9;
    auto const bytes = make_const_span(acmp);
    EXPECT_TRUE(send(a, Frame{bytes.begin(), bytes.end()}));
    bench.pump();
    EXPECT_EQ(count_of(drain(a), avtp::AvtpSubtype::acmp, ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE), size_t{1});
    EXPECT_EQ(count_of(drain(b), avtp::AvtpSubtype::acmp, ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE), size_t{1});
}

TEST(nanoavb_local_port, unsolicited_notifications_reach_each_registered_client)
{
    Bench bench;
    auto a = open_client(bench.path);
    auto b = open_client(bench.path);
    auto c = open_client(bench.path);  // not registered
    EXPECT_TRUE(send(a, aem_command(CONTROLLER_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
    EXPECT_TRUE(send(b, aem_command(CONTROLLER_B, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
    EXPECT_TRUE(send(c, entity_discover()));
    bench.pump();
    (void)drain(a);
    (void)drain(b);
    (void)drain(c);
    EXPECT_EQ(bench.components.aem_handler.unsolicited_registration_count(), size_t{2});

    std::array<uint8_t, 4> const body{0, 0, 0, 0};
    bench.components.aem_handler.emit_unsolicited(AEM_COMMAND_SET_NAME, body);
    auto const to_a = drain(a);
    auto const to_b = drain(b);
    EXPECT_EQ(to_a.size(), size_t{1});
    EXPECT_EQ(to_b.size(), size_t{1});
    EXPECT_EQ(drain(c).size(), size_t{0});
    EXPECT_TRUE(aem_header(to_a.front()).is_unsolicited());
    EXPECT_TRUE(aem_header(to_a.front()).controller_entity_id == CONTROLLER_A);
    EXPECT_TRUE(aem_header(to_b.front()).controller_entity_id == CONTROLLER_B);
}

TEST(nanoavb_local_port, a_closed_client_is_forgotten_with_its_registration)
{
    Bench bench;
    auto b = open_client(bench.path);
    {
        auto a = open_client(bench.path);
        EXPECT_TRUE(send(a, aem_command(CONTROLLER_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
        EXPECT_TRUE(send(b, entity_discover()));
        bench.pump();
        EXPECT_EQ(bench.port().client_count(), size_t{2});
        EXPECT_EQ(bench.components.aem_handler.unsolicited_registration_count(), size_t{1});
    }
    // The next multicast finds a gone (ECONNREFUSED); a reactor cycle later
    // it is forgotten, and its registration with it.
    bench.components.adp_advertiser.notify_entity_changed();
    bench.pump();
    EXPECT_EQ(bench.port().client_count(), size_t{1});
    EXPECT_EQ(bench.port().counters().clients_forgotten, uint64_t{1});
    EXPECT_EQ(bench.components.aem_handler.unsolicited_registration_count(), size_t{0});
    EXPECT_EQ(count_of(drain(b), avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{2});
}

TEST(nanoavb_local_port, a_closed_client_releases_its_acquisition)
{
    Bench bench{/*milan=*/false};             // classic: ACQUIRE_ENTITY works
    std::array<uint8_t, 16> const acquire{};  // flags 0: acquire; ENTITY 0
    auto b = open_client(bench.path);
    {
        auto a = open_client(bench.path);
        EXPECT_TRUE(send(a, aem_command(CONTROLLER_A, AEM_COMMAND_ACQUIRE_ENTITY, acquire)));
        EXPECT_TRUE(send(b, entity_discover()));
        bench.pump();
        auto const to_a = solicited(drain(a));  // (and the acquire's notification)
        EXPECT_EQ(to_a.size(), size_t{1});
        EXPECT_EQ(to_a.front().status(), AEM_STATUS_SUCCESS);
    }
    bench.components.adp_advertiser.notify_entity_changed();
    bench.pump();
    EXPECT_EQ(bench.port().client_count(), size_t{1});
    (void)drain(b);
    EXPECT_TRUE(send(b, aem_command(CONTROLLER_B, AEM_COMMAND_ACQUIRE_ENTITY, acquire)));
    bench.pump();
    auto const to_b = solicited(drain(b));
    EXPECT_EQ(to_b.size(), size_t{1});
    EXPECT_EQ(to_b.front().status(), AEM_STATUS_SUCCESS);
}

TEST(nanoavb_local_port, a_reconnected_controller_is_notified_at_its_new_address)
{
    Bench bench;
    {
        auto old_client = open_client(bench.path);
        EXPECT_TRUE(send(old_client, aem_command(CONTROLLER_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
        bench.pump();
    }
    // Registered again from a new socket before the old one is noticed gone.
    auto client = open_client(bench.path);
    EXPECT_TRUE(send(client, aem_command(CONTROLLER_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
    bench.pump();
    (void)drain(client);
    std::array<uint8_t, 4> const body{0, 0, 0, 0};
    bench.components.aem_handler.emit_unsolicited(AEM_COMMAND_SET_NAME, body);
    EXPECT_EQ(drain(client).size(), size_t{1});
}

TEST(nanoavb_local_port, short_oversize_and_unnamed_datagrams_are_dropped)
{
    Bench bench;
    auto client = open_client(bench.path);
    EXPECT_FALSE(send(client, Frame(LOCAL_PORT_MAX_DATAGRAM + 1, 0)));  // the client refuses

    // A raw socket of our own gets past the client's checks.
    int const raw = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    sockaddr_un own{};
    own.sun_family = AF_UNIX;
    EXPECT_EQ(::bind(raw, reinterpret_cast<sockaddr const*>(&own), sizeof(sa_family_t)), 0);
    sockaddr_un entity{};
    entity.sun_family = AF_UNIX;
    std::memcpy(entity.sun_path, bench.path.data(), bench.path.size());
    EXPECT_EQ(::connect(raw, reinterpret_cast<sockaddr const*>(&entity), sizeof(entity)), 0);
    Frame const oversize(LOCAL_PORT_MAX_DATAGRAM + 1, 0);
    Frame const short_frame(LOCAL_PORT_MIN_DATAGRAM - 1, 0);
    EXPECT_TRUE(::send(raw, oversize.data(), oversize.size(), 0) > 0);
    EXPECT_TRUE(::send(raw, short_frame.data(), short_frame.size(), 0) > 0);
    EXPECT_EQ(::send(raw, nullptr, 0, 0), 0);

    // An unbound socket: the entity could not reply, so it is not a client.
    int const unnamed = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    auto const discover = entity_discover();
    EXPECT_TRUE(
        ::sendto(unnamed, discover.data(), discover.size(), 0, reinterpret_cast<sockaddr const*>(&entity), sizeof(entity)) > 0);
    bench.pump();
    EXPECT_EQ(bench.port().counters().rx_dropped, uint64_t{4});
    EXPECT_EQ(bench.port().counters().rx_frames, uint64_t{0});
    EXPECT_EQ(bench.port().client_count(), size_t{0});
    ::close(raw);
    ::close(unnamed);
}

TEST(nanoavb_local_port, an_adopted_socketpair_answers_its_unnamed_peer)
{
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds.data()), 0);
    auto port = AtdeccLocalPort::adopt(fds[0]);
    EXPECT_TRUE(port.has_value());
    std::vector<Frame> received;
    Eui48 from{};
    (*port)->set_receiver([&](int64_t, Eui48 const& src, std::span<uint8_t const> frame) {
        from = src;
        received.emplace_back(frame.begin(), frame.end());
    });
    auto const discover = entity_discover();
    EXPECT_TRUE(::send(fds[1], discover.data(), discover.size(), 0) > 0);
    (*port)->on_ready(0);
    EXPECT_EQ(received.size(), size_t{1});
    EXPECT_TRUE(from == LOCAL_CONTROLLER_MAC);  // the first client has the well-known MAC
    EXPECT_TRUE((*port)->send(from, discover));
    std::array<uint8_t, 2048> buf{};
    EXPECT_EQ(::recv(fds[1], buf.data(), buf.size(), MSG_DONTWAIT), static_cast<ssize_t>(discover.size()));
    ::close(fds[1]);
}

TEST(nanoavb_local_port, unicast_routing_by_synthetic_mac)
{
    Bench bench;
    auto a = open_client(bench.path);
    auto b = open_client(bench.path);
    Eui48 first{};
    Eui48 second{};
    std::vector<Eui48> seen;
    bench.port().set_receiver([&](int64_t, Eui48 const& src, std::span<uint8_t const>) { seen.push_back(src); });
    EXPECT_TRUE(send(a, entity_discover()));
    EXPECT_TRUE(send(b, entity_discover()));
    bench.pump();
    EXPECT_EQ(seen.size(), size_t{2});
    first = seen[0];
    second = seen[1];
    EXPECT_TRUE(first == LOCAL_CONTROLLER_MAC);
    EXPECT_TRUE(first != second);
    EXPECT_TRUE(AtdeccLocalPort::is_local_mac(second));
    auto const frame = entity_discover();
    EXPECT_TRUE(bench.port().send(second, frame));
    EXPECT_EQ(drain(a).size(), size_t{0});
    EXPECT_EQ(drain(b).size(), size_t{1});
    // A unicast to anyone else is not ours to send.
    EXPECT_FALSE(bench.port().send(Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, frame));
}

TEST(nanoavb_local_port, idle_timeout_and_a_full_table_forget_clients)
{
    AtdeccLocalPort::Config config{.path = temporary_socket_path(), .idle_timeout_ns = 10'000'000'000};
    auto port = AtdeccLocalPort::open(config);
    EXPECT_TRUE(port.has_value());
    std::vector<Eui48> gone;
    (*port)->set_on_client_gone([&](Eui48 const& mac) { gone.push_back(mac); });
    std::vector<AtdeccLocalClient> clients;
    auto const discover = entity_discover();
    for (size_t i = 0; i < AtdeccLocalPort::MAX_CLIENTS + 1; ++i) {
        clients.push_back(open_client(config.path));
        EXPECT_TRUE(send(clients.back(), discover));
        (*port)->on_ready(static_cast<int64_t>(i));
    }
    // The least recently heard made room for the last.
    EXPECT_EQ((*port)->client_count(), AtdeccLocalPort::MAX_CLIENTS);
    EXPECT_EQ(gone.size(), size_t{1});
    EXPECT_TRUE(gone.front() == LOCAL_CONTROLLER_MAC);
    (*port)->tick(5'000'000'000);
    EXPECT_EQ((*port)->client_count(), AtdeccLocalPort::MAX_CLIENTS);
    (*port)->tick(20'000'000'000);
    EXPECT_EQ((*port)->client_count(), size_t{0});
    EXPECT_EQ(gone.size(), AtdeccLocalPort::MAX_CLIENTS + 1);
}

TEST(nanoavb_local_port, socket_file_lifecycle)
{
    auto const path = temporary_socket_path();
    {
        // A socket file nobody holds (an entity that did not exit cleanly)
        // is replaced.
        int const stale = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.data(), path.size());
        EXPECT_EQ(::bind(stale, reinterpret_cast<sockaddr const*>(&address), sizeof(address)), 0);
        ::close(stale);
        auto first = AtdeccLocalPort::open({.path = path, .mode = 0600});
        EXPECT_TRUE(first.has_value());
        struct stat st{};
        EXPECT_EQ(::stat(path.c_str(), &st), 0);
        EXPECT_EQ(st.st_mode & 0777U, 0600U);
        // One a live entity serves is not.
        auto second = AtdeccLocalPort::open({.path = path});
        EXPECT_FALSE(second.has_value());
        EXPECT_TRUE(second.error() == std::errc::address_in_use);
        EXPECT_FALSE((*first)->finished());
        (*first)->close();
        EXPECT_TRUE((*first)->finished());
        EXPECT_TRUE(::stat(path.c_str(), &st) != 0);  // removed its own

        // Closing leaves someone else's file alone (ours was removed under
        // us, and the path bound again).
        auto old_entity = AtdeccLocalPort::open({.path = path});
        EXPECT_TRUE(old_entity.has_value());
        ::unlink(path.c_str());
        int const other = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        EXPECT_EQ(::bind(other, reinterpret_cast<sockaddr const*>(&address), sizeof(address)), 0);
        (*old_entity)->close();
        EXPECT_EQ(::stat(path.c_str(), &st), 0);
        ::close(other);
        ::unlink(path.c_str());
    }

    // Anything but a socket at the path is left alone.
    FILE* file = std::fopen(path.c_str(), "w");
    EXPECT_TRUE(file != nullptr);
    std::fclose(file);
    EXPECT_FALSE(AtdeccLocalPort::open({.path = path}).has_value());
    ::unlink(path.c_str());

    EXPECT_FALSE(AtdeccLocalPort::open({.path = std::string(200, 'x')}).has_value());
}

TEST(nanoavb_local_port, client_keepalive_and_reconnect)
{
    auto const path = temporary_socket_path();
    AtdeccLocalClient client{};
    EXPECT_TRUE(client.open(path).has_value());  // before the entity is there
    auto port = AtdeccLocalPort::open({.path = path});
    EXPECT_TRUE(port.has_value());
    size_t received = 0;
    (*port)->set_receiver([&](int64_t, Eui48 const&, std::span<uint8_t const>) { ++received; });

    // Nothing sent for KEEPALIVE_NS: an ENTITY_DISCOVER, which also connects.
    client.tick(AtdeccLocalClient::KEEPALIVE_NS);
    (*port)->on_ready(0);
    EXPECT_EQ(received, size_t{1});
    client.tick(AtdeccLocalClient::KEEPALIVE_NS + 1);  // records the send
    client.tick(AtdeccLocalClient::KEEPALIVE_NS + 2);
    (*port)->on_ready(0);
    EXPECT_EQ(received, size_t{1});

    // The entity restarts on the same path: the client finds it again.
    port->reset();
    port = AtdeccLocalPort::open({.path = path});
    EXPECT_TRUE(port.has_value());
    (*port)->set_receiver([&](int64_t, Eui48 const&, std::span<uint8_t const>) { ++received; });
    EXPECT_TRUE(send(client, entity_discover()));
    (*port)->on_ready(0);
    EXPECT_EQ(received, size_t{2});
}

TEST(nanoavb_local_port, closing_detaches_from_the_entity)
{
    Bench bench;
    auto client = open_client(bench.path);
    EXPECT_TRUE(send(client, entity_discover()));
    bench.pump();
    (void)drain(client);
    bench.handlers.close_local_port();
    EXPECT_TRUE(bench.handlers.local_port() == nullptr);
    bench.pump();  // the reactor drops the finished port
    // The entity's multicast no longer has anywhere local to go.
    bench.components.adp_advertiser.notify_entity_changed();
    bench.pump();
    EXPECT_EQ(drain(client).size(), size_t{0});
    struct stat st{};
    EXPECT_TRUE(::stat(bench.path.c_str(), &st) != 0);
}

TEST(nanoavb_local_port, a_controller_that_does_not_read_is_forgotten)
{
    Bench bench;
    auto healthy = open_client(bench.path);
    {
        auto stalled = open_client(bench.path);
        EXPECT_TRUE(send(stalled, aem_command(CONTROLLER_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
        bench.pump();
        // What it does not read stays charged to the entity's socket until
        // sends fail; after MAX_SEND_FAILURES in a row it is forgotten.
        AdpDu du{};
        du.init_entity_available(ENTITY_ID, 10);
        for (size_t i = 0; i < 4000; ++i) {
            (void)bench.port().send(ATDECC_MULTICAST_MAC, make_const_span(du));
            (void)drain(healthy);
        }
        bench.pump();
        EXPECT_TRUE(bench.port().counters().clients_forgotten >= 1);
        EXPECT_EQ(bench.components.aem_handler.unsolicited_registration_count(), size_t{0});
    }
    // Once it has gone the buffer is free again, and a controller forgotten
    // with it rejoins with its next datagram.
    (void)drain(healthy);
    EXPECT_TRUE(send(healthy, entity_discover()));
    bench.pump();
    EXPECT_EQ(count_of(drain(healthy), avtp::AvtpSubtype::adp, ADP_MESSAGE_TYPE_ENTITY_AVAILABLE), size_t{1});
}

TEST(nanoavb_local_port, a_local_mac_never_reaches_the_network)
{
    Bench bench;
    std::array<uint8_t, 4> const body{0, 0, 0, 0};
    // A forgotten (or never known) controller's MAC: unreachable, not sent
    // on the AVB network.
    auto const r = bench.handlers.atdecc_handler().send(Eui48{0x0A, 0xE9, 0x1B, 0x01, 0x77, 0x77}, body);
    EXPECT_FALSE(r.has_value());
    EXPECT_TRUE(r.error() == std::errc::host_unreachable);
}

TEST(nanoavb_local_port, a_reacquire_from_a_new_address_moves_the_acquisition)
{
    Bench bench{/*milan=*/false};
    std::array<uint8_t, 16> const acquire{};
    {
        auto first = open_client(bench.path);
        EXPECT_TRUE(send(first, aem_command(CONTROLLER_A, AEM_COMMAND_ACQUIRE_ENTITY, acquire)));
        bench.pump();
        (void)drain(first);
    }
    auto restarted = open_client(bench.path);  // the same controller
    EXPECT_TRUE(send(restarted, aem_command(CONTROLLER_A, AEM_COMMAND_ACQUIRE_ENTITY, acquire)));
    bench.pump();
    auto const r = solicited(drain(restarted));
    EXPECT_EQ(r.size(), size_t{1});
    EXPECT_EQ(r.front().status(), AEM_STATUS_SUCCESS);
    bench.components.adp_advertiser.notify_entity_changed();  // the first is found gone
    bench.pump();
    // Forgetting the old address did not release the controller's acquisition.
    auto b = open_client(bench.path);
    EXPECT_TRUE(send(b, aem_command(CONTROLLER_B, AEM_COMMAND_ACQUIRE_ENTITY, acquire)));
    bench.pump();
    auto const rb = solicited(drain(b));
    EXPECT_EQ(rb.size(), size_t{1});
    EXPECT_NE(rb.front().status(), AEM_STATUS_SUCCESS);
}

TEST(nanoavb_local_port, closing_the_port_forgets_its_controllers_in_the_entity)
{
    Bench bench;
    auto client = open_client(bench.path);
    EXPECT_TRUE(send(client, aem_command(CONTROLLER_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION)));
    bench.pump();
    EXPECT_EQ(bench.components.aem_handler.unsolicited_registration_count(), size_t{1});
    bench.handlers.close_local_port();
    EXPECT_EQ(bench.components.aem_handler.unsolicited_registration_count(), size_t{0});
}

TEST(nanoavb_local_port, a_path_in_use_is_refused)
{
    auto const path = temporary_socket_path();
    // A live socket we cannot connect to (mode 0000) is not taken for stale.
    auto first = AtdeccLocalPort::open({.path = path, .mode = 0000});
    EXPECT_TRUE(first.has_value());
    EXPECT_FALSE(AtdeccLocalPort::open({.path = path}).has_value());
    // Nor is a path whose lock another entity holds, socket file or not.
    ::unlink(path.c_str());
    auto second = AtdeccLocalPort::open({.path = path});
    EXPECT_FALSE(second.has_value());
    EXPECT_TRUE(second.error() == std::errc::address_in_use);
    first->reset();
    EXPECT_TRUE(AtdeccLocalPort::open({.path = path}).has_value());
}

TEST(nanoavb_local_port, a_symlink_at_the_path_is_left_alone)
{
    auto const path = temporary_socket_path();
    auto const target = path + ".target";
    FILE* file = std::fopen(target.c_str(), "w");
    EXPECT_TRUE(file != nullptr);
    std::fclose(file);
    EXPECT_EQ(::chmod(target.c_str(), 0600), 0);
    EXPECT_EQ(::symlink(target.c_str(), path.c_str()), 0);
    EXPECT_FALSE(AtdeccLocalPort::open({.path = path, .mode = 0666}).has_value());
    struct stat st{};
    EXPECT_EQ(::stat(target.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777U, 0600U);
    ::unlink(path.c_str());
    ::unlink(target.c_str());
}

TEST_MAIN(statusbar_nanoavb, nanoavb_local_port_test)
