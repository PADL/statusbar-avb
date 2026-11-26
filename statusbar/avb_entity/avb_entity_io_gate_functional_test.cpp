// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The stereo and AM824 entities' transmit gates: with gating on, the talker
// stream transmits once a controller has connected it (IEEE 1722.1-2021
// Clause 8.2.2.6) and a Listener Ready is registered for it (IEEE 802.1Q-2022
// Clause 35.2.4.4), and stops again at the disconnect. Each entity starts on
// an interface that does not exist; its ACMP and MSRP are fed by hand.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/avb_entity/avb_entity_am824_io.hpp"
#include "statusbar/avb_entity/avb_entity_stereo_io.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/srp/srp_msrp_participant.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/test/test_data.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using TimePoint = statusbar::sm::TimePoint;

namespace {

constexpr Eui64 LISTENER_ID{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
constexpr char const* NO_SUCH_INTERFACE = "no-such-interface-0";

/// The reactor's clock for an entity started in a test: deterministic, 1 ms a read.
struct FakeClock
{
    std::atomic<int64_t>* now_ns;
    auto operator()() noexcept -> int64_t { return now_ns->fetch_add(1'000'000LL); }
};

/// A reactor for the entity to start on; declared before the entity, so it outlives it.
struct TestReactor
{
    itc::StopToken stop{};
    std::atomic<int64_t> now_ns{1};
    net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
};

auto raw(TimePoint const now) -> TalkerGate::RawTimePoint
{
    return TalkerGate::RawTimePoint{now.time_since_epoch()};
}

/// A controller's CONNECT_TX_COMMAND (@p connect) or DISCONNECT_TX_COMMAND for talker stream 0
/// of @p components, received off the wire at @p now.
void controller_tx_command(nanoavb::NanoAvbNetHandlers& handlers, Eui64 const& talker, bool const connect, TimePoint const now)
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.init_command(connect ? atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND : atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    cmd.talker_entity_id = talker;
    cmd.talker_unique_id = 0;
    cmd.listener_entity_id = LISTENER_ID;
    cmd.listener_unique_id = 0;
    std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
    auto const frame = atdecc::acmp_serialize_2016(cmd, buf);
    auto const now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    handlers.atdecc_handler().dispatch_frame(now_ns, Eui48{0x02, 0, 0, 0, 0, 0x09}, frame);
}

/// A bridge port declaring Listener Ready for @p stream_id to @p msrp, from @p now on.
void bridge_listener_ready(nanoavb::MsrpHandler<>& msrp, tsn::StreamId const& stream_id, TimePoint& now)
{
    std::vector<std::vector<uint8_t>> out;
    srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9e};
    bridge.set_send_pdu([&out](std::span<uint8_t const> const pdu) {
        out.emplace_back(pdu.begin(), pdu.end());
        return true;
    });
    bridge.start(now);
    (void)bridge.declare_listener(stream_id, srp::msrp::ListenerDeclaration::Ready, now);
    for (int i = 0; i < 4; ++i) {
        now += std::chrono::milliseconds{250};
        bridge.tick(now);
        for (auto const& pdu : std::exchange(out, {})) {
            msrp.receive_packet(pdu, now);
        }
    }
}

/// Start @p entity on @p reactor and bring it to Ready (Link Up, then a grandmaster): its talker is advertised.
template <class Entity>
void bring_up(Entity& entity, TestReactor& reactor, TimePoint const now)
{
    EXPECT_TRUE(entity.start(reactor.reactor, now).has_value());
    entity.on_link_up(now + std::chrono::seconds{1});
    entity.on_gptp_announce(now + std::chrono::seconds{2}, true);
    EXPECT_TRUE(entity.is_ready());
}

/// The gate's answer for @p entity at each step: Listener Ready alone, connected, disconnected.
template <class Entity>
void expect_gate_follows_the_connection(Entity& entity, Eui64 const& entity_id)
{
    TimePoint now{std::chrono::seconds{3}};
    bridge_listener_ready(entity.components().msrp_handler, entity.talker_srp_info().stream_id, now);
    EXPECT_FALSE(entity.talker_should_transmit(raw(now)));  // no ACMP connection yet

    controller_tx_command(*entity.net_handlers(), entity_id, true, now);
    EXPECT_EQ(entity.components().acmp_talker.connection_count(0), size_t{1});
    EXPECT_TRUE(entity.talker_should_transmit(raw(now)));

    controller_tx_command(*entity.net_handlers(), entity_id, false, now);
    EXPECT_EQ(entity.components().acmp_talker.connection_count(0), size_t{0});
    EXPECT_FALSE(entity.talker_should_transmit(raw(now)));
}

auto load_file(std::filesystem::path const& path) -> std::vector<uint8_t>
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST(avb_entity_stereo_io_gate, a_connected_stream_with_a_listener_ready_transmits)
{
    TestReactor reactor;
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.interface_name = NO_SUCH_INTERFACE;
    EXPECT_TRUE(config.gate_talker_on_listener);
    AvbEntityStereoIO entity{config};
    bring_up(entity, reactor, TimePoint{std::chrono::seconds{0}});
    expect_gate_follows_the_connection(entity, config.entity_id);
    EXPECT_TRUE(entity.stop(TimePoint{std::chrono::seconds{10}}).has_value());
}

TEST(avb_entity_am824_io_gate, a_connected_stream_with_a_listener_ready_transmits)
{
    TestReactor reactor;
    AvbEntityAm824IOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.descriptor_storage_blob = load_file(statusbar::test::functional_test_data(__FILE__, "testdata/simple2.bin"));
    config.interface_name = NO_SUCH_INTERFACE;
    config.gate_talker_on_listener = true;
    auto created = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(created.has_value());
    if (!created.has_value()) {
        return;
    }
    auto& entity = **created;
    bring_up(entity, reactor, TimePoint{std::chrono::seconds{0}});
    expect_gate_follows_the_connection(entity, entity.config().entity_id);
    EXPECT_TRUE(entity.stop(TimePoint{std::chrono::seconds{10}}).has_value());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_io_gate_functional_test)
