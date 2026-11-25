// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
//
// GptpAnnounceHandler: the grandmaster in force, as the Announces name it and
// as their silence takes it away. MaapNetHandler: the reactor port that owns
// the MAAP handler, and how it retires. The two ACMP implementations: which
// one the ATDECC handler feeds, and ACMP's picture of a stream from either.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;

namespace {

constexpr int64_t ONE_SECOND_NS = 1'000'000'000;

auto announce_from(gptp::ClockIdentity const& gm, int8_t const log_interval = 0) -> gptp::AnnounceMessage
{
    gptp::AnnounceMessage ann{};
    ann.init(1);
    ann.grandmaster_identity = gm;
    ann.header.log_message_interval = static_cast<uint8_t>(log_interval);
    return ann;
}

/// What the callbacks were told. The handler under test is on no interface:
/// its socket never opens, observe_announce() and tick() work all the same.
struct Observed
{
    std::vector<gptp::ClockIdentity> changes;
    std::optional<gptp::ClockIdentity> last_announce_gm_at_change;
    int losses{0};
};

gptp::ClockIdentity const kGmA{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04};

}  // namespace

TEST(gptp_announce_handler, a_callback_sees_the_announce_that_named_the_grandmaster)
{
    Observed seen;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_callbacks(GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&](int64_t, gptp::ClockIdentity const& gm, gptp::AnnounceMessage const&) {
                seen.changes.push_back(gm);
                if (auto const* last = handler.last_announce()) {
                    seen.last_announce_gm_at_change = last->grandmaster_identity;
                }
            },
        .grandmaster_lost = [&](int64_t) { ++seen.losses; }});
    EXPECT_FALSE(handler.has_grandmaster());

    handler.observe_announce(ONE_SECOND_NS, announce_from(kGmA));
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(seen.changes.size(), size_t{1});
    EXPECT_TRUE(seen.last_announce_gm_at_change.has_value());
    EXPECT_TRUE(seen.last_announce_gm_at_change.has_value() && *seen.last_announce_gm_at_change == kGmA);

    // The same grandmaster again: nothing to report.
    handler.observe_announce(2 * ONE_SECOND_NS, announce_from(kGmA));
    EXPECT_EQ(seen.changes.size(), size_t{1});
    EXPECT_EQ(seen.losses, 0);
}

TEST(gptp_announce_handler, silence_for_the_receipt_timeout_loses_the_grandmaster)
{
    Observed seen;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_callbacks(GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&](int64_t, gptp::ClockIdentity const& gm, gptp::AnnounceMessage const&) { seen.changes.push_back(gm); },
        .grandmaster_lost = [&](int64_t) { ++seen.losses; }});

    // logMessageInterval 0: one-second Announces, held for three.
    handler.observe_announce(10 * ONE_SECOND_NS, announce_from(kGmA, 0));
    EXPECT_EQ(handler.announce_receipt_timeout_ns(), 3 * ONE_SECOND_NS);
    handler.tick((13 * ONE_SECOND_NS) - 1);
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(seen.losses, 0);
    handler.tick(13 * ONE_SECOND_NS);
    EXPECT_FALSE(handler.has_grandmaster());
    EXPECT_EQ(seen.losses, 1);
    // The last Announce is still readable (its domain, for the reports).
    EXPECT_TRUE(handler.last_announce() != nullptr);
    // Silence after the loss reports nothing more.
    handler.tick(20 * ONE_SECOND_NS);
    EXPECT_EQ(seen.losses, 1);

    // Announces return: the grandmaster is in force again, as a change.
    handler.observe_announce(21 * ONE_SECOND_NS, announce_from(kGmA, -3));
    EXPECT_TRUE(handler.has_grandmaster());
    EXPECT_EQ(seen.changes.size(), size_t{2});
    // logMessageInterval -3: 125 ms Announces, held for 375 ms.
    EXPECT_EQ(handler.announce_receipt_timeout_ns(), (3 * ONE_SECOND_NS) / 8);
}

TEST(maap_net_handler, retire_releases_the_range_and_finishes_the_port)
{
    using S = avtp::maap_sm::Def::State;
    // On no interface: the socket never opens, the handler it owns works all
    // the same, and nothing leaves the test.
    auto handler = std::make_unique<avtp::MaapHandler>(ieee::Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, 7);
    MaapNetHandler port{"no-such-interface-0", std::move(handler)};
    EXPECT_FALSE(port.valid());
    EXPECT_FALSE(port.finished());

    port.handler().acquire(2, ONE_SECOND_NS);
    EXPECT_EQ(port.handler().state(), S::Probe);
    EXPECT_TRUE(port.next_deadline_ns().has_value());

    // The entity stops: Table B.7 Release!, and the reactor is to drop the port.
    port.retire(2 * ONE_SECOND_NS);
    EXPECT_TRUE(port.finished());
    EXPECT_EQ(port.handler().state(), S::Initial);
    EXPECT_FALSE(port.next_deadline_ns().has_value());

    // A tick that still reaches the retired port drives nothing.
    port.tick(100 * ONE_SECOND_NS);
    EXPECT_EQ(port.handler().state(), S::Initial);
    EXPECT_FALSE(port.next_deadline_ns().has_value());
}

//
// The two ACMP implementations and the choice between them
//

namespace {

constexpr ieee::Eui64 ENTITY{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
constexpr ieee::Eui64 CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
constexpr ieee::Eui64 REMOTE_TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr ieee::Eui64 STREAM{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01};
constexpr ieee::Eui48 DEST{0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x81};
constexpr ieee::Eui48 PEER_MAC{0x02, 0x00, 0x00, 0x00, 0x00, 0x09};

auto two_stream_components() -> std::unique_ptr<NanoAvbComponents>
{
    EntityModel model{EntityModelConfig{.max_configurations = 1, .max_stream_inputs = 2, .max_stream_outputs = 2}};
    atdecc::aem::DescriptorEntity entity{};
    entity.entity_id = ENTITY;
    model.set_entity(entity);
    auto built = NanoAvbComponentsBuilder{}
                     .with_entity_model(std::move(model))
                     .with_acmp_talker_streams(2, 4)
                     .with_acmp_listener_streams(2)
                     .build(sm::TimePoint{});
    EXPECT_TRUE(built.has_value());
    return std::move(*built);
}

/// An ACMP command as its 56 octets on the wire.
auto acmp_frame(uint8_t const type, ieee::Eui64 const& talker, ieee::Eui64 const& listener) -> std::vector<uint8_t>
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.init_command(type);
    cmd.controller_entity_id = CONTROLLER;
    cmd.talker_entity_id = talker;
    cmd.listener_entity_id = listener;
    std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
    auto const wire = atdecc::acmp_serialize_2016(cmd, buf);
    return {wire.begin(), wire.end()};
}

/// The ATDECC handler of @p components on no interface, the Milan pair's
/// PDUs recorded. Frames are handed to dispatch_frame().
struct AtdeccOnNoInterface
{
    AtdeccNetHandler handler;
    std::vector<atdecc::AcmpCommandResponse> milan_sent{};

    explicit AtdeccOnNoInterface(NanoAvbComponents& components)
        : handler{
              "no-such-interface-0",
              components.adp_advertiser,
              components.acmp_talker,
              components.acmp_listener,
              components.aem_handler}
    {
        handler.set_milan_acmp(components.milan_acmp_talker.get(), components.milan_acmp_listener.get());
        auto record = [this](atdecc::AcmpCommandResponse const& pdu) {
            milan_sent.push_back(pdu);
            return true;
        };
        if (components.acmp_is_milan()) {
            components.milan_acmp_talker->set_tx_response(record);
            components.milan_acmp_listener->set_senders(record, record);
        }
    }

    void receive(std::vector<uint8_t> const& frame, int64_t const now_ns = ONE_SECOND_NS)
    {
        handler.dispatch_frame(now_ns, PEER_MAC, frame);
    }
};

}  // namespace

TEST(nanoavb_components_acmp, the_ieee_pair_runs_unless_milan_is_chosen)
{
    auto components = two_stream_components();
    EXPECT_FALSE(components->acmp_is_milan());
    EXPECT_TRUE(components->milan_acmp_talker == nullptr);
    EXPECT_TRUE(components->milan_acmp_listener == nullptr);

    components->use_milan_acmp();
    EXPECT_TRUE(components->acmp_is_milan());
    EXPECT_EQ(components->milan_acmp_talker->entity_id(), ENTITY);
    EXPECT_EQ(components->milan_acmp_listener->entity_id(), ENTITY);
    EXPECT_EQ(components->milan_acmp_talker->max_streams(), 2U);
    EXPECT_EQ(components->milan_acmp_listener->max_streams(), 2U);
    auto const* talker = components->milan_acmp_talker.get();
    components->use_milan_acmp();  // once is enough: the same objects stay
    EXPECT_TRUE(components->milan_acmp_talker.get() == talker);
}

TEST(nanoavb_components_acmp, a_streams_parameters_go_to_and_come_from_the_pair_that_runs)
{
    auto ieee_components = two_stream_components();
    EXPECT_TRUE(ieee_components->configure_talker_stream(1, STREAM, DEST, 7).has_value());
    EXPECT_EQ(ieee_components->acmp_talker.get_stream(1)->stream_id, STREAM);
    EXPECT_EQ(ieee_components->talker_stream(1)->stream_dest_mac, DEST);
    EXPECT_EQ(ieee_components->talker_stream(1)->stream_vlan_id, 7);
    EXPECT_FALSE(ieee_components->talker_stream(2).has_value());
    EXPECT_FALSE(ieee_components->listener_sink(0)->connected);
    EXPECT_FALSE(ieee_components->listener_sink(2).has_value());

    auto milan_components = two_stream_components();
    milan_components->use_milan_acmp();
    EXPECT_TRUE(milan_components->configure_talker_stream(1, STREAM, DEST, 7).has_value());
    EXPECT_EQ(milan_components->milan_acmp_talker->get_stream(1)->stream_id, STREAM);
    EXPECT_EQ(milan_components->acmp_talker.get_stream(1)->stream_id, ieee::Eui64{});  // the idle pair is told nothing
    EXPECT_EQ(milan_components->talker_stream(1)->stream_id, STREAM);
    EXPECT_EQ(milan_components->talker_stream(1)->stream_vlan_id, 7);
    EXPECT_FALSE(milan_components->configure_talker_stream(2, STREAM, DEST, 7).has_value());
    EXPECT_FALSE(milan_components->talker_stream(2).has_value());
    EXPECT_FALSE(milan_components->listener_sink(0)->connected);
    EXPECT_FALSE(milan_components->listener_sink(2).has_value());
}

TEST(nanoavb_components_acmp, a_milan_sinks_picture_is_its_binding_and_its_probed_parameters)
{
    auto components = two_stream_components();
    components->use_milan_acmp();
    AtdeccOnNoInterface net{*components};
    net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY));
    EXPECT_EQ(net.milan_sent.size(), 2U);  // the BIND_RX_RESPONSE, then the PROBE_TX_COMMAND
    auto bound = components->listener_sink(0);
    EXPECT_FALSE(bound->connected);
    EXPECT_EQ(bound->talker_entity_id, REMOTE_TALKER);
    EXPECT_EQ(bound->stream_id, ieee::Eui64{});

    auto answer = net.milan_sent.back();
    answer.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    answer.stream_id = STREAM;
    answer.stream_dest_mac = DEST;
    answer.stream_vlan_id = 2;
    std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
    auto const wire = atdecc::acmp_serialize_2016(answer, buf);
    net.receive({wire.begin(), wire.end()});
    auto settled = components->listener_sink(0);
    EXPECT_TRUE(settled->connected);
    EXPECT_EQ(settled->stream_id, STREAM);
    EXPECT_EQ(settled->stream_dest_mac, DEST);
    EXPECT_EQ(settled->stream_vlan_id, 2);
}

TEST(nanoavb_components_acmp, acmp_frames_go_to_the_milan_pair_once_it_is_chosen_and_to_the_ieee_pair_otherwise)
{
    // IEEE 1722.1: a CONNECT_RX_COMMAND is relayed to the talker and the
    // controller waits. Milan: it is a BIND_RX_COMMAND, answered at once.
    auto ieee_components = two_stream_components();
    AtdeccOnNoInterface ieee_net{*ieee_components};
    ieee_net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY));
    EXPECT_TRUE(ieee_components->acmp_listener.has_pending());

    auto milan_components = two_stream_components();
    milan_components->use_milan_acmp();
    AtdeccOnNoInterface milan_net{*milan_components};
    milan_net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY));
    EXPECT_FALSE(milan_components->acmp_listener.has_pending());  // the IEEE 1722.1 listener never saw it
    EXPECT_TRUE(milan_components->milan_acmp_listener->is_bound(0));
    EXPECT_EQ(milan_net.milan_sent.front().message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(milan_net.milan_sent.front().status(), atdecc::ACMP_STATUS_SUCCESS);

    // And the talker side: a GET_TX_CONNECTION_COMMAND is not supported under Milan.
    milan_net.milan_sent.clear();
    milan_net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, ENTITY, REMOTE_TALKER));
    EXPECT_EQ(milan_net.milan_sent.size(), 1U);
    EXPECT_EQ(milan_net.milan_sent.front().status(), atdecc::ACMP_STATUS_NOT_SUPPORTED);
}

TEST(nanoavb_components_acmp, the_milan_listener_hears_adp_and_is_ticked_through_the_handler)
{
    auto components = two_stream_components();
    components->use_milan_acmp();
    AtdeccOnNoInterface net{*components};
    auto& listener = *components->milan_acmp_listener;
    listener.set_random_delay([]() { return std::chrono::milliseconds{50}; });
    EXPECT_TRUE(listener.restore_binding(atdecc::MilanBinding{.listener_unique_id = 1, .talker_entity_id = REMOTE_TALKER}, {}));

    atdecc::AdpDu adp{};
    adp.init_entity_available(REMOTE_TALKER, 5);
    adp.available_index = 1;
    std::array<uint8_t, atdecc::AdpDu::LENGTH> buf{};
    span_store(std::span<uint8_t>{buf}, adp);
    net.receive({buf.begin(), buf.end()}, ONE_SECOND_NS);
    EXPECT_EQ(listener.sink(1)->state(), atdecc::MilanSinkState::PrbWDelay);

    net.handler.tick(ONE_SECOND_NS + 50'000'000);
    EXPECT_EQ(listener.sink(1)->state(), atdecc::MilanSinkState::PrbWResp);
    EXPECT_EQ(net.milan_sent.size(), 1U);
    EXPECT_EQ(net.milan_sent.front().message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(net.milan_sent.front().talker_entity_id, REMOTE_TALKER);
}

TEST(nanoavb_components_acmp, a_milan_bind_finds_its_talker_discovered_when_adp_discovery_heard_it_first)
{
    auto components = two_stream_components();
    components->use_milan_acmp();
    AtdeccOnNoInterface net{*components};
    NanoAvbAdpDiscovery discovery{};
    net.handler.set_adp_discovery(&discovery);
    auto const& sink = *components->milan_acmp_listener->sink(0);

    // The talker advertises while nothing is bound to it: only ADP discovery keeps it.
    atdecc::AdpDu adp{};
    adp.init_entity_available(REMOTE_TALKER, 5);
    adp.available_index = 1;
    std::array<uint8_t, atdecc::AdpDu::LENGTH> buf{};
    span_store(std::span<uint8_t>{buf}, adp);
    net.receive({buf.begin(), buf.end()}, ONE_SECOND_NS);
    EXPECT_EQ(sink.discovery_state(), atdecc::MilanDiscoveryState::Stopped);

    // A controller binds a second later: the sink probes and has its talker discovered.
    net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY), 2 * ONE_SECOND_NS);
    EXPECT_EQ(sink.state(), atdecc::MilanSinkState::PrbWResp);
    EXPECT_EQ(sink.discovery_state(), atdecc::MilanDiscoveryState::TkDiscovered);

    // The advertisement lapses ten seconds after it was heard, not after the bind.
    net.handler.tick(11 * ONE_SECOND_NS);
    EXPECT_EQ(sink.discovery_state(), atdecc::MilanDiscoveryState::TkNotDiscovered);
}

TEST(nanoavb_components_acmp, a_handler_that_goes_away_takes_its_senders_back)
{
    auto components = two_stream_components();
    components->use_milan_acmp();
    {
        AtdeccNetHandler handler{
            "no-such-interface-0",
            components->adp_advertiser,
            components->acmp_talker,
            components->acmp_listener,
            components->aem_handler};
        handler.set_milan_acmp(components->milan_acmp_talker.get(), components->milan_acmp_listener.get());
    }
    // Nothing left to send through: the command is taken and its answer goes nowhere.
    atdecc::AcmpCommandResponse cmd{};
    cmd.init_command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND);
    cmd.listener_entity_id = ENTITY;
    EXPECT_TRUE(components->milan_acmp_listener->receive_controller_command(cmd, {}));
}

TEST(nanoavb_components_acmp, an_earlier_handler_going_away_leaves_a_later_ones_senders)
{
    // A control plane started again makes a second handler for the same
    // pair while the reactor may still hold the first.
    auto components = two_stream_components();
    components->use_milan_acmp();
    auto first = std::make_unique<AtdeccOnNoInterface>(*components);
    AtdeccOnNoInterface second{*components};
    first.reset();
    second.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, ENTITY, REMOTE_TALKER));
    EXPECT_EQ(second.milan_sent.size(), 1U);
}

TEST(nanoavb_net_handlers, a_handler_whose_socket_did_not_open_can_still_be_reached)
{
    // On no interface nothing opens and the reactor is given nothing; the
    // accessors must answer for the handlers all the same.
    auto components = two_stream_components();
    itc::StopToken stop{};
    net::MessageReactor reactor{stop, []() noexcept -> int64_t { return 0; }, 5};
    NanoAvbNetHandlers handlers{"no-such-interface-0", *components};
    handlers.add_to_reactor(reactor);
    EXPECT_FALSE(handlers.atdecc_handler().valid());
    EXPECT_FALSE(handlers.gptp_handler().valid());
    EXPECT_FALSE(handlers.mvrp_handler().valid());
    EXPECT_FALSE(handlers.msrp_handler().valid());
    EXPECT_EQ(handlers.atdecc_handler().tick_count(), 0);
}

TEST_MAIN(statusbar_nanoavb, nanoavb_components_functional_test)
