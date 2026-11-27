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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
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

TEST(gptp_announce_handler, the_path_trace_follows_the_announces_and_is_emptied_on_loss)
{
    int changes = 0;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_on_path_trace_changed([&](int64_t) { ++changes; });
    AsPath through_bridge{};
    through_bridge.push_back(kGmA.to_eui64());
    through_bridge.push_back(ieee::Eui64{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x02});

    handler.observe_announce(ONE_SECOND_NS, announce_from(kGmA), through_bridge);
    EXPECT_EQ(changes, 1);
    EXPECT_TRUE(std::ranges::equal(handler.path_trace(), through_bridge));
    handler.observe_announce(2 * ONE_SECOND_NS, announce_from(kGmA), through_bridge);
    EXPECT_EQ(changes, 1);
    // An Announce without the TLV names its grandmaster alone.
    handler.observe_announce(3 * ONE_SECOND_NS, announce_from(kGmA));
    EXPECT_EQ(changes, 2);
    std::array const gm_alone{kGmA.to_eui64()};
    EXPECT_TRUE(std::ranges::equal(handler.path_trace(), gm_alone));
    handler.tick(6 * ONE_SECOND_NS);
    EXPECT_FALSE(handler.has_grandmaster());
    EXPECT_TRUE(handler.path_trace().empty());
    EXPECT_EQ(changes, 3);
}

TEST(gptp_announce_handler, replacing_the_callbacks_keeps_the_path_trace_observer)
{
    int changes = 0;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_on_path_trace_changed([&](int64_t) { ++changes; });
    handler.set_callbacks(GptpAnnounceCallbacks{.grandmaster_lost = [](int64_t) {}});
    handler.observe_announce(ONE_SECOND_NS, announce_from(kGmA));
    EXPECT_EQ(changes, 1);
}

TEST(gptp_announce_handler, the_grandmaster_observer_is_told_each_change_whatever_the_callbacks)
{
    // A change to another grandmaster, the loss at the receipt timeout and the return, each once,
    // with has_grandmaster() already saying which; the callbacks replaced in between do not silence it.
    std::vector<std::pair<int64_t, bool>> told;
    GptpAnnounceHandler handler{"no-such-interface-0"};
    handler.set_on_grandmaster_changed([&](int64_t const now_ns) { told.emplace_back(now_ns, handler.has_grandmaster()); });
    gptp::ClockIdentity const gm_b{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x05};

    handler.observe_announce(ONE_SECOND_NS, announce_from(kGmA));
    handler.observe_announce(2 * ONE_SECOND_NS, announce_from(kGmA));  // the same: not a change
    handler.set_callbacks(GptpAnnounceCallbacks{.grandmaster_lost = [](int64_t) {}});
    handler.observe_announce(3 * ONE_SECOND_NS, announce_from(gm_b));
    handler.tick(6 * ONE_SECOND_NS);
    handler.tick(7 * ONE_SECOND_NS);  // still none: not a change
    handler.observe_announce(8 * ONE_SECOND_NS, announce_from(gm_b));

    std::vector<std::pair<int64_t, bool>> const expected{
        {ONE_SECOND_NS, true}, {3 * ONE_SECOND_NS, true}, {6 * ONE_SECOND_NS, false}, {8 * ONE_SECOND_NS, true}};
    EXPECT_TRUE(told == expected);
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

TEST(nanoavb_components_acmp, a_milan_entity_advertises_as_milan_asks)
{
    // IEEE 1722.1 as it was: the configured valid time, sent at once.
    auto components = two_stream_components();
    EXPECT_FALSE(components->adp_advertiser.config().milan_advertise);
    EXPECT_TRUE(components->adp_advertiser.adpdu().valid_time() != MILAN_ADP_VALID_TIME);

    // Milan v1.3 5.6.2 and 5.6.3: valid_time 10, and the first
    // ENTITY_AVAILABLE waits out its random delay.
    components->use_milan_acmp();
    auto& adp = components->adp_advertiser;
    EXPECT_TRUE(adp.config().milan_advertise);
    EXPECT_EQ(adp.adpdu().valid_time(), MILAN_ADP_VALID_TIME);
    EXPECT_TRUE(adp.start(TimePoint{std::chrono::seconds{1}}).has_value());
    EXPECT_TRUE(adp.milan_advertise_delayed());
    EXPECT_EQ(adp.available_index(), 0U);
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

TEST(nanoavb_components_acmp, a_settled_milan_sinks_vlan_is_declared_by_mvrp_until_the_sink_is_unbound)
{
    // Milan v1.3 4.4.1. The builder has declared the entity's own VLAN, 2.
    auto components = two_stream_components();
    components->use_milan_acmp();
    AtdeccOnNoInterface net{*components};
    auto const& mvrp = components->mvrp_handler;
    EXPECT_EQ(mvrp.get_vlan_state(2), VlanState::Pending);

    net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY));
    EXPECT_EQ(mvrp.get_vlan_state(7), VlanState::Unregistered);  // bound, not settled
    auto answer = net.milan_sent.back();
    answer.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    answer.stream_id = STREAM;
    answer.stream_dest_mac = DEST;
    answer.stream_vlan_id = 7;
    std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
    auto const wire = atdecc::acmp_serialize_2016(answer, buf);
    net.receive({wire.begin(), wire.end()});
    EXPECT_TRUE(components->listener_sink(0)->connected);
    EXPECT_EQ(mvrp.get_vlan_state(7), VlanState::Pending);
    EXPECT_EQ(mvrp.sink_vlans().size(), 1U);

    net.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY));
    EXPECT_FALSE(components->listener_sink(0)->connected);
    EXPECT_EQ(mvrp.get_vlan_state(7), VlanState::Unregistered);
    EXPECT_EQ(mvrp.get_vlan_state(2), VlanState::Pending);
    EXPECT_TRUE(mvrp.sink_vlans().empty());
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

//
// BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX through the handler
//

namespace {

/// An MVU command for ENTITY as its octets on the wire: @p data behind the header.
template <typename Data>
auto mvu_frame(uint16_t const command, Data const& data, uint16_t const sequence_id = 1) -> std::vector<uint8_t>
{
    atdecc::AecpMvuDu du{};
    du.init_command(command, static_cast<uint16_t>(Data::LENGTH));
    du.common.target_entity_id = ENTITY;
    du.common.controller_entity_id = CONTROLLER;
    du.common.sequence_id = sequence_id;
    std::vector<uint8_t> frame(atdecc::AecpMvuDu::LENGTH + Data::LENGTH);
    span_store(std::span<uint8_t>{frame}.first(atdecc::AecpMvuDu::LENGTH), du);
    span_store(std::span<uint8_t>{frame}.subspan(atdecc::AecpMvuDu::LENGTH), data);
    return frame;
}

auto bind_stream_frame(uint16_t const sink, ieee::Eui64 const& talker) -> std::vector<uint8_t>
{
    atdecc::MvuBindStream bind{};
    bind.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    bind.descriptor_index = sink;
    bind.talker_entity_id = talker;
    bind.talker_stream_index = 3;
    return mvu_frame(atdecc::MVU_COMMAND_BIND_STREAM, bind);
}

auto stream_input_frame(uint16_t const command, uint16_t const sink) -> std::vector<uint8_t>
{
    atdecc::MvuStreamInput input{};
    input.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    input.descriptor_index = sink;
    return mvu_frame(command, input);
}

/// A Milan device's AEM handler and Milan pair behind one ATDECC handler,
/// with what each sent kept in one list, in order: "mvu <command> <status>"
/// for an MVU response and "acmp <message type>" for an ACMP PDU.
struct MilanDeviceOnNoInterface
{
    std::unique_ptr<NanoAvbComponents> components{two_stream_components()};
    std::vector<std::string> sent{};
    std::vector<uint8_t> last_mvu{};
    std::optional<AtdeccNetHandler> handler{};

    explicit MilanDeviceOnNoInterface(bool const milan_acmp = true)
    {
        components->aem_handler.set_milan_info(AemCommandHandler::MilanInfo{});
        if (milan_acmp) {
            components->use_milan_acmp();
        }
        handler.emplace(
            "no-such-interface-0",
            components->adp_advertiser,
            components->acmp_talker,
            components->acmp_listener,
            components->aem_handler);
        handler->set_milan_acmp(components->milan_acmp_talker.get(), components->milan_acmp_listener.get());
        record_what_is_sent(milan_acmp);
    }

    void record_what_is_sent(bool const milan_acmp)
    {
        components->aem_handler.set_send_response([this](ieee::Eui48 const&, std::span<uint8_t const> const response) {
            note_mvu(response);
            return true;
        });
        auto acmp = [this](atdecc::AcmpCommandResponse const& pdu) {
            sent.push_back("acmp " + std::to_string(pdu.message_type()));
            return true;
        };
        if (milan_acmp) {
            components->milan_acmp_listener->set_senders(acmp, acmp);
        }
    }

    void note_mvu(std::span<uint8_t const> const response)
    {
        atdecc::AecpMvuDu du{};
        span_load(du, response.first(atdecc::AecpMvuDu::LENGTH));
        sent.push_back(
            "mvu " + std::string{atdecc::mvu_command_name(du.command_type())} + " " +
            std::string{atdecc::mvu_status_name(du.common.status())});
        last_mvu.assign(response.begin(), response.end());
    }

    void receive(std::vector<uint8_t> const& frame, int64_t const now_ns = ONE_SECOND_NS)
    {
        handler->dispatch_frame(now_ns, PEER_MAC, frame);
    }

    /// The stream input's info, as GET_STREAM_INPUT_INFO_EX answers it now.
    auto info_of(uint16_t const sink) -> atdecc::MvuGetStreamInputInfoExResponse
    {
        receive(stream_input_frame(atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, sink));
        atdecc::MvuGetStreamInputInfoExResponse info{};
        if (last_mvu.size() == atdecc::AecpMvuDu::LENGTH + atdecc::MvuGetStreamInputInfoExResponse::LENGTH) {
            span_load(info, std::span<uint8_t const>{last_mvu}.subspan(atdecc::AecpMvuDu::LENGTH));
        }
        return info;
    }
};

}  // namespace

TEST(nanoavb_components_mvu, a_bind_stream_binds_its_sink_and_is_answered_before_the_probe)
{
    // Milan v1.3 5.5.3.2 and 5.5.3.6.3: the response of step 3 is an MVU
    // one, to the controller, and the probe of step 5 follows it on ACMP.
    MilanDeviceOnNoInterface device;
    device.receive(bind_stream_frame(1, REMOTE_TALKER));
    EXPECT_EQ(device.sent.size(), 2U);
    if (device.sent.size() != 2) {
        return;
    }
    EXPECT_EQ(device.sent[0], std::string{"mvu BIND_STREAM SUCCESS"});
    EXPECT_EQ(device.sent[1], "acmp " + std::to_string(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND));
    auto const& sink = *device.components->milan_acmp_listener->sink(1);
    EXPECT_EQ(sink.state(), atdecc::MilanSinkState::PrbWResp);
    EXPECT_EQ(sink.context().binding->talker_entity_id, REMOTE_TALKER);
    EXPECT_EQ(sink.context().binding->talker_unique_id, 3);
    EXPECT_EQ(sink.context().binding->controller_entity_id, CONTROLLER);
    EXPECT_FALSE(device.components->milan_acmp_listener->is_bound(0));
}

TEST(nanoavb_components_mvu, get_stream_input_info_ex_follows_a_sink_from_unbound_to_probing_and_back)
{
    MilanDeviceOnNoInterface device;
    auto const unbound = device.info_of(1);
    EXPECT_EQ(device.sent.back(), std::string{"mvu GET_STREAM_INPUT_INFO_EX SUCCESS"});
    EXPECT_EQ(unbound.descriptor_type.get(), atdecc::aem::DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(unbound.descriptor_index.get(), 1);
    EXPECT_EQ(unbound.talker_entity_id, ieee::Eui64{});
    EXPECT_EQ(unbound.pbsta(), 0);  // PROBING_DISABLED

    device.receive(bind_stream_frame(1, REMOTE_TALKER));
    auto const probing = device.info_of(1);
    EXPECT_EQ(probing.talker_entity_id, REMOTE_TALKER);
    EXPECT_EQ(probing.talker_unique_id.get(), 3);
    EXPECT_EQ(probing.pbsta(), 2);  // PROBING_ACTIVE
    EXPECT_EQ(probing.acmpsta(), 0);
    EXPECT_EQ(device.info_of(0).pbsta(), 0);  // the other sink is as it was

    // Two probes go unanswered: the sink waits to retry, and says why (5.5.3.6.25).
    device.handler->tick(ONE_SECOND_NS + 200'000'000);
    device.handler->tick(ONE_SECOND_NS + 400'000'000);
    auto const retrying = device.info_of(1);
    EXPECT_EQ(retrying.pbsta(), 2);
    EXPECT_EQ(retrying.acmpsta(), atdecc::ACMP_STATUS_LISTENER_TALKER_TIMEOUT);

    device.receive(stream_input_frame(atdecc::MVU_COMMAND_UNBIND_STREAM, 1));
    EXPECT_EQ(device.sent.back(), std::string{"mvu UNBIND_STREAM SUCCESS"});
    EXPECT_FALSE(device.components->milan_acmp_listener->is_bound(1));
    auto const after = device.info_of(1);
    EXPECT_EQ(after.talker_entity_id, ieee::Eui64{});
    EXPECT_EQ(after.pbsta(), 0);
}

TEST(nanoavb_components_mvu, a_stream_input_the_entity_does_not_have_is_no_such_descriptor)
{
    // Table 5.28, and 5.4.4.8.
    MilanDeviceOnNoInterface device;
    device.receive(bind_stream_frame(2, REMOTE_TALKER));
    EXPECT_EQ(device.sent.back(), std::string{"mvu BIND_STREAM NO_SUCH_DESCRIPTOR"});
    device.receive(stream_input_frame(atdecc::MVU_COMMAND_UNBIND_STREAM, 2));
    EXPECT_EQ(device.sent.back(), std::string{"mvu UNBIND_STREAM NO_SUCH_DESCRIPTOR"});
    device.receive(stream_input_frame(atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, 2));
    EXPECT_EQ(device.sent.back(), std::string{"mvu GET_STREAM_INPUT_INFO_EX NO_SUCH_DESCRIPTOR"});
    EXPECT_EQ(device.sent.size(), 3U);  // and nothing on ACMP
}

TEST(nanoavb_components_mvu, a_bind_stream_from_a_controller_the_lock_shuts_out_is_entity_locked)
{
    // Tables 5.33 and 5.39. The lock is asked of whoever wired the listener.
    MilanDeviceOnNoInterface device;
    device.components->milan_acmp_listener->set_locked_out([](ieee::Eui64 const& controller) { return controller == CONTROLLER; });
    device.receive(bind_stream_frame(0, REMOTE_TALKER));
    EXPECT_EQ(device.sent.back(), std::string{"mvu BIND_STREAM ENTITY_LOCKED"});
    device.receive(stream_input_frame(atdecc::MVU_COMMAND_UNBIND_STREAM, 0));
    EXPECT_EQ(device.sent.back(), std::string{"mvu UNBIND_STREAM ENTITY_LOCKED"});
    EXPECT_FALSE(device.components->milan_acmp_listener->is_bound(0));
    EXPECT_EQ(device.sent.size(), 2U);
}

TEST(nanoavb_components_mvu, a_bind_stream_finds_its_talker_discovered_when_adp_discovery_heard_it_first)
{
    // As with a BIND_RX_COMMAND: the talker's last advertisement goes with the bind.
    MilanDeviceOnNoInterface device;
    NanoAvbAdpDiscovery discovery{};
    device.handler->set_adp_discovery(&discovery);
    atdecc::AdpDu adp{};
    adp.init_entity_available(REMOTE_TALKER, 5);
    adp.available_index = 1;
    std::array<uint8_t, atdecc::AdpDu::LENGTH> buf{};
    span_store(std::span<uint8_t>{buf}, adp);
    device.receive({buf.begin(), buf.end()}, ONE_SECOND_NS);

    device.receive(bind_stream_frame(0, REMOTE_TALKER), 2 * ONE_SECOND_NS);
    auto const& sink = *device.components->milan_acmp_listener->sink(0);
    EXPECT_EQ(sink.state(), atdecc::MilanSinkState::PrbWResp);
    EXPECT_EQ(sink.discovery_state(), atdecc::MilanDiscoveryState::TkDiscovered);
}

TEST(nanoavb_components_mvu, an_entity_on_ieee_connection_management_does_not_implement_the_three)
{
    // It answers MVU (GET_MILAN_INFO) and has no sink machine behind it.
    MilanDeviceOnNoInterface device{false};
    device.receive(bind_stream_frame(0, REMOTE_TALKER));
    device.receive(stream_input_frame(atdecc::MVU_COMMAND_UNBIND_STREAM, 0));
    device.receive(stream_input_frame(atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, 0));
    EXPECT_EQ(device.sent.size(), 3U);
    if (device.sent.size() != 3) {
        return;
    }
    EXPECT_EQ(device.sent[0], std::string{"mvu BIND_STREAM NOT_IMPLEMENTED"});
    EXPECT_EQ(device.sent[1], std::string{"mvu UNBIND_STREAM NOT_IMPLEMENTED"});
    EXPECT_EQ(device.sent[2], std::string{"mvu GET_STREAM_INPUT_INFO_EX NOT_IMPLEMENTED"});
    EXPECT_FALSE(device.components->acmp_listener.has_pending());
}

TEST(nanoavb_components_mvu, a_handler_that_goes_away_takes_the_stream_inputs_back)
{
    // The AEM handler outlives it and must not call into a handler that is gone.
    MilanDeviceOnNoInterface device;
    device.handler.reset();
    auto const frame = stream_input_frame(atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, 0);
    EXPECT_TRUE(device.components->aem_handler.process_packet(PEER_MAC, frame, ENTITY));
    EXPECT_EQ(device.sent.back(), std::string{"mvu GET_STREAM_INPUT_INFO_EX NOT_IMPLEMENTED"});
}

//
// The notifications of Milan v1.3 5.4.5.3 and Table 5.21 through the handler
//

namespace {

constexpr ieee::Eui64 WATCHER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x88};
constexpr ieee::Eui48 WATCHER_MAC{0x02, 0x00, 0x00, 0x00, 0x00, 0x0A};

/// REGISTER_UNSOLICITED_NOTIFICATION for ENTITY from @p controller, as its octets on the wire.
auto register_frame(ieee::Eui64 const& controller) -> std::vector<uint8_t>
{
    atdecc::AemDu du{};
    du.init_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, atdecc::AemDu::AEM_DATA_LENGTH);
    du.target_entity_id = ENTITY;
    du.controller_entity_id = controller;
    std::vector<uint8_t> frame(atdecc::AemDu::LENGTH);
    span_store(std::span<uint8_t>{frame}, du);
    return frame;
}

/// A Milan device with two registered controllers: CONTROLLER at PEER_MAC,
/// which sends the commands, and WATCHER at WATCHER_MAC. Every MVU
/// notification the device sends is kept with where it went.
struct WatchedMilanDevice
{
    struct Told
    {
        ieee::Eui48 dest{};
        uint16_t command{0};
        std::vector<uint8_t> data{};
    };

    MilanDeviceOnNoInterface device{};
    std::vector<Told> told{};

    WatchedMilanDevice()
    {
        device.components->aem_handler.set_send_response([this](ieee::Eui48 const& dest, std::span<uint8_t const> const pdu) {
            note(dest, pdu);
            return true;
        });
        device.handler->dispatch_frame(ONE_SECOND_NS, PEER_MAC, register_frame(CONTROLLER));
        device.handler->dispatch_frame(ONE_SECOND_NS, WATCHER_MAC, register_frame(WATCHER));
    }
    WatchedMilanDevice(WatchedMilanDevice const&) = delete;
    auto operator=(WatchedMilanDevice const&) -> WatchedMilanDevice& = delete;

    void note(ieee::Eui48 const& dest, std::span<uint8_t const> const pdu)
    {
        if (pdu.size() < atdecc::AecpMvuDu::LENGTH) {
            return;  // an AEM response
        }
        atdecc::AecpMvuDu du{};
        span_load(du, pdu.first(atdecc::AecpMvuDu::LENGTH));
        if (du.is_mvu() && du.is_unsolicited()) {
            auto const data = pdu.subspan(atdecc::AecpMvuDu::LENGTH);
            told.push_back(Told{.dest = dest, .command = du.command_type(), .data = {data.begin(), data.end()}});
        }
    }

    /// The notifications of @p command sent to @p dest, in order.
    [[nodiscard]] auto told_to(ieee::Eui48 const& dest, uint16_t const command) const -> std::vector<Told>
    {
        std::vector<Told> found{};
        std::ranges::copy_if(
            told, std::back_inserter(found), [&](Told const& each) { return each.dest == dest && each.command == command; });
        return found;
    }

    /// The stream input info of the last GET_STREAM_INPUT_INFO_EX notification sent to @p dest.
    [[nodiscard]] auto last_info_to(ieee::Eui48 const& dest) const -> atdecc::MvuGetStreamInputInfoExResponse
    {
        atdecc::MvuGetStreamInputInfoExResponse info{};
        auto const found = told_to(dest, atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX);
        if (!found.empty() && found.back().data.size() == atdecc::MvuGetStreamInputInfoExResponse::LENGTH) {
            span_load(info, std::span<uint8_t const>{found.back().data});
        }
        return info;
    }

    [[nodiscard]] auto infos_to(ieee::Eui48 const& dest) const -> size_t
    {
        return told_to(dest, atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX).size();
    }
};

}  // namespace

TEST(nanoavb_components_mvu_notify, a_bind_stream_is_told_to_the_other_controller_and_the_input_to_both_at_the_tick)
{
    WatchedMilanDevice watched;
    watched.device.receive(bind_stream_frame(1, REMOTE_TALKER));
    // 5.4.5.3: the command's notification, to the registered controllers but its sender.
    EXPECT_EQ(watched.told_to(WATCHER_MAC, atdecc::MVU_COMMAND_BIND_STREAM).size(), 1U);
    EXPECT_TRUE(watched.told_to(PEER_MAC, atdecc::MVU_COMMAND_BIND_STREAM).empty());
    EXPECT_EQ(watched.told.size(), 1U);

    // Table 5.21: the input has a talker and a Probing status now. Found at the tick, told to both.
    watched.device.handler->tick(ONE_SECOND_NS + 10'000'000);
    EXPECT_EQ(watched.infos_to(PEER_MAC), 1U);
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 1U);
    auto const probing = watched.last_info_to(WATCHER_MAC);
    EXPECT_EQ(probing.descriptor_index.get(), 1);
    EXPECT_EQ(probing.talker_entity_id, REMOTE_TALKER);
    EXPECT_EQ(probing.pbsta(), 2);  // PROBING_ACTIVE
    EXPECT_TRUE(watched.last_info_to(PEER_MAC) == probing);

    // Nothing changed: nothing more is said.
    watched.device.handler->tick(ONE_SECOND_NS + 20'000'000);
    EXPECT_EQ(watched.told.size(), 3U);
}

TEST(nanoavb_components_mvu_notify, the_input_is_told_again_when_its_acmp_status_changes_and_when_it_is_unbound)
{
    WatchedMilanDevice watched;
    watched.device.receive(bind_stream_frame(1, REMOTE_TALKER));
    watched.device.handler->tick(ONE_SECOND_NS + 10'000'000);
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 1U);

    // The first probe times out and is sent again: the input reads as before.
    watched.device.handler->tick(ONE_SECOND_NS + 200'000'000);
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 1U);
    // The second times out too: the ACMP status says so (5.5.3.6.25).
    watched.device.handler->tick(ONE_SECOND_NS + 400'000'000);
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 2U);
    EXPECT_EQ(watched.infos_to(PEER_MAC), 2U);
    EXPECT_EQ(watched.last_info_to(WATCHER_MAC).acmpsta(), atdecc::ACMP_STATUS_LISTENER_TALKER_TIMEOUT);

    // Unbound: the command's notification at once, the input's at the tick.
    watched.device.receive(stream_input_frame(atdecc::MVU_COMMAND_UNBIND_STREAM, 1));
    EXPECT_EQ(watched.told_to(WATCHER_MAC, atdecc::MVU_COMMAND_UNBIND_STREAM).size(), 1U);
    EXPECT_TRUE(watched.told_to(PEER_MAC, atdecc::MVU_COMMAND_UNBIND_STREAM).empty());
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 2U);
    watched.device.handler->tick(ONE_SECOND_NS + 410'000'000);
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 3U);
    EXPECT_EQ(watched.last_info_to(WATCHER_MAC).pbsta(), 0);  // PROBING_DISABLED
    EXPECT_EQ(watched.last_info_to(WATCHER_MAC).talker_entity_id, ieee::Eui64{});
}

TEST(nanoavb_components_mvu_notify, a_bind_over_acmp_is_told_as_the_changed_input_and_not_as_a_bind_stream)
{
    // BIND_RX_COMMAND is no MVU command, so 5.4.5.3 asks for no BIND_STREAM
    // notification; Table 5.21 has every registered controller told of the input.
    WatchedMilanDevice watched;
    watched.device.receive(acmp_frame(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, REMOTE_TALKER, ENTITY));
    EXPECT_TRUE(watched.device.components->milan_acmp_listener->is_bound(0));
    EXPECT_TRUE(watched.told.empty());
    watched.device.handler->tick(ONE_SECOND_NS + 10'000'000);
    EXPECT_EQ(watched.infos_to(PEER_MAC), 1U);
    EXPECT_EQ(watched.infos_to(WATCHER_MAC), 1U);
    EXPECT_EQ(watched.told.size(), 2U);
    EXPECT_EQ(watched.last_info_to(WATCHER_MAC).descriptor_index.get(), 0);
    EXPECT_EQ(watched.last_info_to(WATCHER_MAC).talker_entity_id, REMOTE_TALKER);
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
