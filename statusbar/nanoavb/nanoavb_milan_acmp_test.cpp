// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The entity's Milan v1.3 ACMP talker and listener (nanoavb_milan_acmp.hpp):
// which PDUs each takes, what it asks of the rest of the entity, what it
// tells it. Synthetic time, no sockets.

#include "statusbar/nanoavb/nanoavb_milan_acmp.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using statusbar::atdecc::AcmpCommandResponse;
using statusbar::atdecc::AdpDu;
using statusbar::atdecc::MilanBinding;
using statusbar::atdecc::MilanSinkState;
using statusbar::atdecc::MilanTalkerRegistration;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using TimePoint = statusbar::sm::TimePoint;

namespace {

constexpr Eui64 LISTENER{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr Eui64 TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr Eui64 SOMEONE_ELSE{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x22};
constexpr Eui64 CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
constexpr Eui64 LOCK_HOLDER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x99};
constexpr Eui64 STREAM{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x05};
constexpr Eui48 DEST{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
constexpr tsn::ClockIdentity GRANDMASTER{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x00, 0x01};

auto at(int const ms) -> TimePoint
{
    return TimePoint{std::chrono::milliseconds{ms}};
}

auto command(uint8_t const type, Eui64 const& talker, uint16_t const source, Eui64 const& listener, uint16_t const sink)
    -> AcmpCommandResponse
{
    AcmpCommandResponse cmd{};
    cmd.init_command(type);
    cmd.controller_entity_id = CONTROLLER;
    cmd.talker_entity_id = talker;
    cmd.talker_unique_id = source;
    cmd.listener_entity_id = listener;
    cmd.listener_unique_id = sink;
    cmd.sequence_id = 9;
    return cmd;
}

auto available(Eui64 const& talker, uint32_t const available_index) -> AdpDu
{
    AdpDu adp{};
    adp.init_entity_available(talker, 5);
    adp.available_index = available_index;
    adp.gptp_grandmaster_id = GRANDMASTER;
    return adp;
}

/// A Milan talker with two stream outputs and a record of what it did, in order.
struct TalkerRig
{
    std::vector<std::string> log{};
    std::vector<AcmpCommandResponse> responses{};
    bool dest_mac_valid{true};
    bool registering_failed{false};
    NanoAvbMilanAcmpTalker talker{TALKER, AcmpTalkerCallbacks{}, 2};

    TalkerRig()
    {
        talker.set_tx_response([this](AcmpCommandResponse const& pdu) {
            log.emplace_back("response");
            responses.push_back(pdu);
            return true;
        });
        talker.set_on_probe([this](uint16_t const index, TimePoint) { log.push_back("probe " + std::to_string(index)); });
        talker.set_dest_mac_valid([this](uint16_t) { return dest_mac_valid; });
        talker.set_registering_failed([this](uint16_t const index) { return registering_failed && index == 1; });
        EXPECT_TRUE(talker.configure_stream(0, Eui64{1, 1, 1, 1, 1, 1, 0, 0}, Eui48{0x91, 0xE0, 0xF0, 0, 0, 0}, 2).has_value());
        EXPECT_TRUE(talker.configure_stream(1, STREAM, DEST, 2).has_value());
    }
    TalkerRig(TalkerRig const&) = delete;
    auto operator=(TalkerRig const&) -> TalkerRig& = delete;

    auto receive(uint8_t const type, uint16_t const source, Eui64 const& talker_id = TALKER) -> bool
    {
        return talker.receive_command(command(type, talker_id, source, LISTENER, 0), at(1000));
    }
};

/// A Milan listener with three stream inputs and a record of what it did.
struct ListenerRig
{
    std::vector<std::string> log{};
    std::vector<AcmpCommandResponse> probes{};
    std::vector<AcmpCommandResponse> responses{};
    std::vector<uint16_t> connected{};
    std::vector<uint16_t> disconnected{};
    Eui64 connected_stream{};
    Eui48 connected_dest{};
    Eui64 lock_holder{};
    atdecc::MilanGptpState gptp{.grandmaster_id = GRANDMASTER, .domain_number = 0};
    MilanTalkerRegistration registration{MilanTalkerRegistration::None};
    NanoAvbMilanAcmpListener listener{LISTENER, AcmpListenerCallbacks{}, 3};

    ListenerRig()
    {
        listener.set_senders(
            [this](AcmpCommandResponse const& pdu) {
                log.emplace_back("probe");
                probes.push_back(pdu);
                return true;
            },
            [this](AcmpCommandResponse const& pdu) {
                log.emplace_back("response");
                responses.push_back(pdu);
                return true;
            });
        listener.set_connection_callbacks(
            [this](uint16_t const index, Eui64 const& stream_id, Eui48 const dest, TimePoint) {
                connected.push_back(index);
                connected_stream = stream_id;
                connected_dest = dest;
            },
            [this](uint16_t const index, TimePoint) { disconnected.push_back(index); });
        listener.set_locked_out([this](Eui64 const& controller) { return lock_holder != Eui64{} && lock_holder != controller; });
        listener.set_gptp_state([this]() { return gptp; });
        listener.set_talker_registration([this](atdecc::MilanSrpParameters const&) { return registration; });
        listener.set_random_delay([]() { return std::chrono::milliseconds{100}; });
        listener.set_on_bindings_changed([this]() { log.emplace_back("saved"); });
    }
    ListenerRig(ListenerRig const&) = delete;
    auto operator=(ListenerRig const&) -> ListenerRig& = delete;

    auto bind(uint16_t const sink, TimePoint const now, Eui64 const& talker = TALKER) -> bool
    {
        return listener.receive_controller_command(
            command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, talker, 1, LISTENER, sink), now);
    }

    /// The talker's answer to probe @p probe, with its stream's parameters.
    static auto answer_to(AcmpCommandResponse const& probe) -> AcmpCommandResponse
    {
        AcmpCommandResponse resp = probe;
        resp.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
        resp.stream_id = STREAM;
        resp.stream_dest_mac = DEST;
        resp.stream_vlan_id = 2;
        return resp;
    }

    [[nodiscard]] auto state(uint16_t const sink) const -> MilanSinkState { return listener.sink(sink)->state(); }

    /// The stream inputs tell_stream_input_changes() tells of now, in order.
    [[nodiscard]] auto changed() -> std::vector<uint16_t>
    {
        std::vector<uint16_t> told{};
        listener.tell_stream_input_changes([&told](uint16_t const index) { told.push_back(index); });
        return told;
    }
};

}  // namespace

//
// Talker
//

TEST(nanoavb_milan_talker, a_probe_is_reported_before_it_is_answered)
{
    // Milan v1.3 4.3.3.1 counts the command: the SRP handler must hear of it
    // first, so the declaration is under way when the listener reads the answer.
    TalkerRig rig;
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1));
    EXPECT_EQ(rig.log.size(), 2U);
    EXPECT_EQ(rig.log[0], std::string{"probe 1"});
    EXPECT_EQ(rig.log[1], std::string{"response"});
    auto const& resp = rig.responses.front();
    EXPECT_EQ(resp.message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_EQ(resp.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.stream_id, STREAM);
    EXPECT_EQ(resp.stream_dest_mac, DEST);
    EXPECT_EQ(resp.stream_vlan_id.get(), 2);
    EXPECT_EQ(resp.connection_count.get(), 0);
}

TEST(nanoavb_milan_talker, any_number_of_listeners_may_probe_one_stream)
{
    // No list of listeners to fill: the fifth distinct listener is answered like the first.
    TalkerRig rig;
    for (uint8_t n = 0; n < 40; ++n) {
        auto cmd = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, TALKER, 1, Eui64{0x10, 0, 0, 0, 0, 0, 0, n}, n);
        EXPECT_TRUE(rig.talker.receive_command(cmd, at(n)));
        EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_SUCCESS);
        EXPECT_EQ(rig.responses.back().connection_count.get(), 0);
    }
}

TEST(nanoavb_milan_talker, a_probe_for_a_stream_it_does_not_have_is_answered_and_not_reported)
{
    TalkerRig rig;
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 2));
    EXPECT_EQ(rig.log.size(), 1U);
    EXPECT_EQ(rig.responses.front().status(), atdecc::ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(nanoavb_milan_talker, a_command_for_another_entity_or_for_a_listener_is_left_alone)
{
    TalkerRig rig;
    EXPECT_FALSE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1, SOMEONE_ELSE));
    EXPECT_FALSE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, 1));
    EXPECT_FALSE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, 1));
    EXPECT_TRUE(rig.log.empty());
}

TEST(nanoavb_milan_talker, without_a_valid_address_a_probe_is_refused_and_still_reported)
{
    // 5.5.4.1 step 3. The probe is still a probe: whether it leads to a
    // declaration is the SRP handler's to decide, which knows the address is gone.
    TalkerRig rig;
    rig.dest_mac_valid = false;
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1));
    EXPECT_EQ(rig.responses.front().status(), atdecc::ACMP_STATUS_TALKER_DEST_MAC_FAIL);
    EXPECT_EQ(rig.log[0], std::string{"probe 1"});
}

TEST(nanoavb_milan_talker, the_other_commands_change_nothing_and_get_milans_answers)
{
    TalkerRig rig;
    rig.registering_failed = true;
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, 1));
    EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(rig.responses.back().stream_id, Eui64{});
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 1));
    EXPECT_EQ(rig.responses.back().stream_id, STREAM);
    EXPECT_EQ(rig.responses.back().flags.get(), atdecc::acmp_flags::SRP_REGISTRATION_FAILED);
    EXPECT_EQ(rig.responses.back().listener_entity_id, Eui64{});
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 0));
    EXPECT_EQ(rig.responses.back().flags.get(), 0);
    EXPECT_TRUE(rig.receive(atdecc::ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, 1));
    EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(rig.log.size(), 4U);  // four responses, no probe reported
    EXPECT_EQ(rig.talker.get_stream(1)->stream_id, STREAM);
}

TEST(nanoavb_milan_talker, streams_beyond_its_count_cannot_be_configured)
{
    TalkerRig rig;
    EXPECT_EQ(rig.talker.max_streams(), 2U);
    EXPECT_FALSE(rig.talker.configure_stream(2, STREAM, DEST, 2).has_value());
    EXPECT_TRUE(rig.talker.get_stream(2) == nullptr);
    NanoAvbMilanAcmpTalker wide{TALKER, AcmpTalkerCallbacks{}, 1000};
    EXPECT_EQ(wide.max_streams(), NanoAvbMilanAcmpTalker::MAX_STREAMS);
}

//
// Listener
//

TEST(nanoavb_milan_listener, a_bind_is_saved_then_answered_then_probed)
{
    // 5.5.3.6.3 steps 2, 3 and 5, in that order.
    ListenerRig rig;
    EXPECT_TRUE(rig.bind(1, at(0)));
    EXPECT_EQ(rig.log.size(), 3U);
    EXPECT_EQ(rig.log[0], std::string{"saved"});
    EXPECT_EQ(rig.log[1], std::string{"response"});
    EXPECT_EQ(rig.log[2], std::string{"probe"});
    EXPECT_EQ(rig.responses.front().message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(rig.responses.front().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(rig.probes.front().listener_entity_id, LISTENER);
    EXPECT_EQ(rig.probes.front().listener_unique_id.get(), 1);
    EXPECT_TRUE(rig.listener.is_bound(1));
    EXPECT_FALSE(rig.listener.is_settled(1));
    EXPECT_FALSE(rig.listener.is_bound(0));
}

TEST(nanoavb_milan_listener, a_command_for_a_sink_it_does_not_have_is_listener_unknown_id)
{
    // 5.5.3.1 step 1, Table 5.27
    ListenerRig rig;
    for (uint8_t const type :
         {atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND,
          atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND,
          atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND}) {
        EXPECT_TRUE(rig.listener.receive_controller_command(command(type, TALKER, 1, LISTENER, 3), at(0)));
        EXPECT_EQ(rig.responses.back().message_type(), type + 1);
        EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_LISTENER_UNKNOWN_ID);
    }
    EXPECT_TRUE(rig.probes.empty());
}

TEST(nanoavb_milan_listener, every_other_acmp_message_is_ignored)
{
    // 5.5.3.1: "The listener shall ignore all other ACMP messages."
    ListenerRig rig;
    EXPECT_FALSE(rig.listener.receive_controller_command(
        command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, SOMEONE_ELSE, 1), at(0)));
    for (uint8_t const type :
         {atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND,
          atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND,
          atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND,
          atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE}) {
        EXPECT_FALSE(rig.listener.receive_controller_command(command(type, TALKER, 1, LISTENER, 1), at(0)));
    }
    EXPECT_FALSE(rig.listener.receive_talker_response(
        command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, TALKER, 1, LISTENER, 1), at(0)));
    EXPECT_TRUE(rig.log.empty());
}

TEST(nanoavb_milan_listener, the_lock_shuts_another_controller_out_of_bind_and_unbind)
{
    // 5.5.2.4 and 5.5.2.5
    ListenerRig rig;
    rig.lock_holder = LOCK_HOLDER;
    EXPECT_TRUE(rig.bind(0, at(0)));
    EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_FALSE(rig.listener.is_bound(0));
    EXPECT_TRUE(rig.probes.empty());

    rig.lock_holder = CONTROLLER;  // the controller that sends the commands holds the lock
    EXPECT_TRUE(rig.bind(0, at(1)));
    EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(rig.listener.is_bound(0));

    rig.lock_holder = LOCK_HOLDER;
    auto const unbind = command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, 1, LISTENER, 0);
    EXPECT_TRUE(rig.listener.receive_controller_command(unbind, at(2)));
    EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_TRUE(rig.listener.is_bound(0));
    // A query is not shut out.
    auto const query = command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER, 1, LISTENER, 0);
    EXPECT_TRUE(rig.listener.receive_controller_command(query, at(3)));
    EXPECT_EQ(rig.responses.back().status(), atdecc::ACMP_STATUS_SUCCESS);
}

TEST(nanoavb_milan_listener, a_settled_sink_connects_and_an_unbound_one_disconnects)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.bind(2, at(0)));
    EXPECT_TRUE(rig.listener.receive_talker_response(ListenerRig::answer_to(rig.probes.back()), at(5)));
    EXPECT_EQ(rig.state(2), MilanSinkState::SettledNoRsv);
    EXPECT_TRUE(rig.listener.is_settled(2));
    EXPECT_EQ(rig.connected.size(), 1U);
    EXPECT_EQ(rig.connected.front(), 2);
    EXPECT_EQ(rig.connected_stream, STREAM);
    EXPECT_EQ(rig.connected_dest, DEST);

    rig.registration = MilanTalkerRegistration::Advertise;
    rig.listener.on_srp_changed(at(300));
    EXPECT_EQ(rig.state(2), MilanSinkState::SettledRsvOk);

    auto const unbind = command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, 1, LISTENER, 2);
    EXPECT_TRUE(rig.listener.receive_controller_command(unbind, at(400)));
    EXPECT_EQ(rig.state(2), MilanSinkState::Unbound);
    EXPECT_EQ(rig.disconnected.size(), 1U);
    EXPECT_EQ(rig.disconnected.front(), 2);
    EXPECT_TRUE(rig.listener.bindings().empty());
}

TEST(nanoavb_milan_listener, a_response_reaches_only_the_sink_it_names)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.bind(0, at(0)));
    EXPECT_TRUE(rig.bind(1, at(0)));
    EXPECT_TRUE(rig.bind(2, at(0)));
    EXPECT_EQ(rig.responses.size(), 3U);  // a burst of binds: each answered, each probing by itself
    EXPECT_EQ(rig.probes.size(), 3U);
    EXPECT_NE(rig.probes[0].sequence_id.get(), rig.probes[1].sequence_id.get());

    EXPECT_TRUE(rig.listener.receive_talker_response(ListenerRig::answer_to(rig.probes[1]), at(5)));
    EXPECT_EQ(rig.state(0), MilanSinkState::PrbWResp);
    EXPECT_EQ(rig.state(1), MilanSinkState::SettledNoRsv);
    EXPECT_EQ(rig.state(2), MilanSinkState::PrbWResp);

    auto for_another_listener = ListenerRig::answer_to(rig.probes[0]);
    for_another_listener.listener_entity_id = SOMEONE_ELSE;
    EXPECT_FALSE(rig.listener.receive_talker_response(for_another_listener, at(6)));
    auto for_no_sink = ListenerRig::answer_to(rig.probes[0]);
    for_no_sink.listener_unique_id = 7;
    EXPECT_FALSE(rig.listener.receive_talker_response(for_no_sink, at(6)));  // 5.5.3.1: ignored
    EXPECT_EQ(rig.state(0), MilanSinkState::PrbWResp);
}

TEST(nanoavb_milan_listener, adp_reaches_the_sinks_bound_to_its_sender_on_our_gptp_domain)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.listener.restore_binding(MilanBinding{.listener_unique_id = 0, .talker_entity_id = TALKER}, at(0)));
    EXPECT_TRUE(rig.listener.restore_binding(MilanBinding{.listener_unique_id = 1, .talker_entity_id = SOMEONE_ELSE}, at(0)));
    EXPECT_TRUE(rig.listener.restore_binding(MilanBinding{.listener_unique_id = 2, .talker_entity_id = TALKER}, at(0)));
    EXPECT_FALSE(rig.listener.restore_binding(MilanBinding{.listener_unique_id = 3, .talker_entity_id = TALKER}, at(0)));
    EXPECT_FALSE(rig.listener.next_deadline().has_value());

    rig.gptp.domain_number = 1;
    rig.listener.receive_adpdu(available(TALKER, 1), at(100));
    EXPECT_EQ(rig.state(0), MilanSinkState::PrbWAvail);  // the talker is on another gPTP domain

    rig.gptp.domain_number = 0;
    rig.listener.receive_adpdu(available(TALKER, 2), at(200));
    EXPECT_EQ(rig.state(0), MilanSinkState::PrbWDelay);
    EXPECT_EQ(rig.state(1), MilanSinkState::PrbWAvail);
    EXPECT_EQ(rig.state(2), MilanSinkState::PrbWDelay);
    EXPECT_EQ(*rig.listener.next_deadline(), at(300));

    rig.listener.tick(at(300));
    EXPECT_EQ(rig.probes.size(), 2U);
    EXPECT_EQ(rig.state(0), MilanSinkState::PrbWResp);
    EXPECT_EQ(rig.state(2), MilanSinkState::PrbWResp);
    EXPECT_EQ(*rig.listener.next_deadline(), at(300) + atdecc::MILAN_TMR_NO_RESP);
}

TEST(nanoavb_milan_listener, a_bind_is_given_what_was_heard_of_its_talker_before_it)
{
    using atdecc::MilanDiscoveryState;
    using atdecc::MilanHeardAdvertisement;
    auto const bind = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 0);
    MilanHeardAdvertisement const heard{.adp = available(TALKER, 4), .valid_until = at(9000)};

    // Heard on our grandmaster and still good: the talker is discovered from the bind on.
    ListenerRig rig;
    EXPECT_TRUE(rig.listener.receive_controller_command(bind, at(1000), heard));
    EXPECT_EQ(rig.listener.sink(0)->discovery_state(), MilanDiscoveryState::TkDiscovered);
    EXPECT_EQ(rig.state(0), MilanSinkState::PrbWResp);
    EXPECT_EQ(rig.probes.size(), 1U);

    // Heard on another grandmaster, or nothing heard: the sink waits for an advertisement.
    ListenerRig elsewhere;
    elsewhere.gptp.domain_number = 1;
    EXPECT_TRUE(elsewhere.listener.receive_controller_command(bind, at(1000), heard));
    EXPECT_EQ(elsewhere.listener.sink(0)->discovery_state(), MilanDiscoveryState::TkNotDiscovered);
    ListenerRig unheard;
    EXPECT_TRUE(unheard.listener.receive_controller_command(bind, at(1000)));
    EXPECT_EQ(unheard.listener.sink(0)->discovery_state(), MilanDiscoveryState::TkNotDiscovered);

    // Only a bind takes it: a query of a sink that waits for its talker leaves it waiting.
    ListenerRig waiting;
    EXPECT_TRUE(waiting.listener.restore_binding(MilanBinding{.listener_unique_id = 0, .talker_entity_id = TALKER}, at(0)));
    auto const query = command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER, 1, LISTENER, 0);
    EXPECT_TRUE(waiting.listener.receive_controller_command(query, at(1000), heard));
    EXPECT_EQ(waiting.state(0), MilanSinkState::PrbWAvail);
}

//
// BIND_STREAM and UNBIND_STREAM (AECP MVU) - Milan v1.3 5.5.3.2
//

TEST(nanoavb_milan_listener, an_mvu_bind_is_saved_then_answered_to_its_caller_then_probed)
{
    // The same three steps of 5.5.3.6.3, with the answer handed to whoever
    // brought the command where the ACMP response would have been sent.
    ListenerRig rig;
    std::vector<AcmpCommandResponse> answers{};
    NanoAvbMilanAcmpListener::Respond const respond = [&](AcmpCommandResponse const& pdu) {
        rig.log.emplace_back("mvu answer");
        answers.push_back(pdu);
    };
    auto const bind = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 1);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(bind, at(0), respond));
    EXPECT_EQ(rig.log.size(), 3U);
    EXPECT_EQ(rig.log[0], std::string{"saved"});
    EXPECT_EQ(rig.log[1], std::string{"mvu answer"});
    EXPECT_EQ(rig.log[2], std::string{"probe"});
    EXPECT_TRUE(rig.responses.empty());  // nothing on the ACMP multicast
    EXPECT_EQ(answers.size(), 1U);
    EXPECT_EQ(answers.front().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(rig.state(1), MilanSinkState::PrbWResp);
    EXPECT_EQ(rig.listener.sink(1)->context().binding->controller_entity_id, CONTROLLER);
}

TEST(nanoavb_milan_listener, an_mvu_bind_runs_the_same_sink_as_an_acmp_one)
{
    // One machine: a sink bound over ACMP is bound again, to the same
    // source, over MVU (5.5.3.6.6: answered, no new probe), and unbound over MVU.
    ListenerRig rig;
    std::vector<AcmpCommandResponse> answers{};
    NanoAvbMilanAcmpListener::Respond const respond = [&](AcmpCommandResponse const& pdu) { answers.push_back(pdu); };
    EXPECT_TRUE(rig.bind(0, at(0)));
    EXPECT_EQ(rig.probes.size(), 1U);

    auto const again = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 0);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(again, at(10), respond));
    EXPECT_EQ(answers.back().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(rig.probes.size(), 1U);

    auto const unbind = command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, Eui64{}, 0, LISTENER, 0);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(unbind, at(20), respond));
    EXPECT_EQ(answers.size(), 2U);
    EXPECT_EQ(answers.back().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_FALSE(rig.listener.is_bound(0));
    EXPECT_EQ(rig.responses.size(), 1U);  // the ACMP bind's, and no other
}

TEST(nanoavb_milan_listener, an_mvu_bind_is_refused_as_an_acmp_one_is)
{
    // A sink that does not exist (Table 5.28) and a controller the lock
    // shuts out (Tables 5.33, 5.39): the caller gets the sink's ACMP status
    // to name its own with, and nothing changes.
    ListenerRig rig;
    std::vector<AcmpCommandResponse> answers{};
    NanoAvbMilanAcmpListener::Respond const respond = [&](AcmpCommandResponse const& pdu) { answers.push_back(pdu); };
    auto const nowhere = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 3);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(nowhere, at(0), respond));
    EXPECT_EQ(answers.back().status(), atdecc::ACMP_STATUS_LISTENER_UNKNOWN_ID);

    rig.lock_holder = LOCK_HOLDER;
    auto const bind = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 0);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(bind, at(1), respond));
    EXPECT_EQ(answers.back().status(), atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_FALSE(rig.listener.is_bound(0));
    EXPECT_EQ(answers.size(), 2U);
    EXPECT_TRUE(rig.log.empty());  // nothing saved, sent or probed
}

TEST(nanoavb_milan_listener, only_a_bind_or_unbind_for_this_entity_is_an_mvu_binding_command)
{
    ListenerRig rig;
    size_t answered = 0;
    NanoAvbMilanAcmpListener::Respond const respond = [&](AcmpCommandResponse const&) { ++answered; };
    EXPECT_FALSE(rig.listener.receive_mvu_binding_command(
        command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, SOMEONE_ELSE, 0), at(0), respond));
    // GET_RX_STATE has no MVU form: GET_STREAM_INPUT_INFO_EX is not a sink event.
    EXPECT_FALSE(rig.listener.receive_mvu_binding_command(
        command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER, 1, LISTENER, 0), at(0), respond));
    EXPECT_EQ(answered, 0U);
    EXPECT_TRUE(rig.log.empty());
}

TEST(nanoavb_milan_listener, after_an_mvu_command_the_sink_answers_acmp_commands_on_acmp_again)
{
    // The caller's answer is for one command. A GET_RX_STATE that follows,
    // and whatever the sink sends a controller later, go out as ACMP.
    ListenerRig rig;
    size_t answered = 0;
    NanoAvbMilanAcmpListener::Respond const respond = [&](AcmpCommandResponse const&) { ++answered; };
    auto const bind = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 2);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(bind, at(0), respond));
    auto const query = command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER, 1, LISTENER, 2);
    EXPECT_TRUE(rig.listener.receive_controller_command(query, at(1)));
    EXPECT_EQ(answered, 1U);
    EXPECT_EQ(rig.responses.size(), 1U);
    EXPECT_EQ(rig.responses.back().message_type(), atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);
    EXPECT_EQ(rig.responses.back().talker_entity_id, TALKER);
}

TEST(nanoavb_milan_listener, an_mvu_bind_is_given_what_was_heard_of_its_talker_too)
{
    // As with the ACMP command: the talker advertised before the bind, its
    // first probe is refused, and the retry does not wait for another ADP.
    ListenerRig rig;
    NanoAvbMilanAcmpListener::Respond const respond = [](AcmpCommandResponse const&) {};
    atdecc::MilanHeardAdvertisement const heard{.adp = available(TALKER, 1), .valid_until = at(10000)};
    auto const bind = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 1, LISTENER, 0);
    EXPECT_TRUE(rig.listener.receive_mvu_binding_command(bind, at(0), respond, heard));
    EXPECT_EQ(rig.listener.sink(0)->discovery_state(), atdecc::MilanDiscoveryState::TkDiscovered);
}

TEST(nanoavb_milan_listener, a_started_or_stopped_change_is_saved_as_a_binding_is)
{
    // START_STREAMING and STOP_STREAMING reach a sink through the listener
    // (Milan v1.3 5.4.2.19, 5.4.2.20); the state is saved with its binding.
    ListenerRig rig;
    EXPECT_FALSE(rig.listener.set_streaming_wait(1, true));  // not bound: no effect
    EXPECT_FALSE(rig.listener.set_streaming_wait(3, true));  // no such stream input
    EXPECT_TRUE(rig.log.empty());

    EXPECT_TRUE(rig.bind(1, at(0)));
    rig.log.clear();
    EXPECT_TRUE(rig.listener.set_streaming_wait(1, true));
    EXPECT_EQ(rig.log.size(), 1U);
    EXPECT_EQ(rig.log[0], std::string{"saved"});
    EXPECT_TRUE(rig.listener.bindings().front().streaming_wait);
    EXPECT_FALSE(rig.listener.set_streaming_wait(1, true));  // already stopped
    EXPECT_EQ(rig.log.size(), 1U);
    EXPECT_TRUE(rig.listener.set_streaming_wait(1, false));
    EXPECT_FALSE(rig.listener.bindings().front().streaming_wait);
}

TEST(nanoavb_milan_listener, the_bindings_are_what_was_bound_and_survive_a_restart)
{
    ListenerRig rig;
    auto bind = command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER, 4, LISTENER, 2);
    bind.flags = atdecc::acmp_flags::STREAMING_WAIT;
    EXPECT_TRUE(rig.listener.receive_controller_command(bind, at(0)));
    EXPECT_TRUE(rig.listener.receive_talker_response(ListenerRig::answer_to(rig.probes.back()), at(5)));
    auto const saved = rig.listener.bindings();
    EXPECT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved.front().listener_unique_id, 2);
    EXPECT_EQ(saved.front().talker_entity_id, TALKER);
    EXPECT_EQ(saved.front().talker_unique_id, 4);
    EXPECT_EQ(saved.front().controller_entity_id, CONTROLLER);
    EXPECT_TRUE(saved.front().streaming_wait);

    rig.listener.restart(at(1000));
    EXPECT_EQ(rig.state(2), MilanSinkState::PrbWAvail);
    EXPECT_EQ(rig.disconnected.size(), 1U);
    EXPECT_TRUE(rig.listener.bindings().front() == saved.front());

    // Another listener starting from the saved bindings is where this one is.
    ListenerRig next;
    for (auto const& binding : saved) {
        EXPECT_TRUE(next.listener.restore_binding(binding, at(0)));
    }
    EXPECT_EQ(next.state(2), MilanSinkState::PrbWAvail);
    EXPECT_TRUE(next.log.empty());  // restoring is not a change to save, and answers nobody
    EXPECT_TRUE(next.listener.bindings().front() == saved.front());
}

//
// What Table 5.21 has notified
//

TEST(nanoavb_milan_listener, a_fresh_listener_has_no_stream_input_to_tell_of)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.changed().empty());
    rig.listener.tick(at(5000));
    EXPECT_TRUE(rig.changed().empty());
}

TEST(nanoavb_milan_listener, a_bind_a_settle_and_an_unbind_are_each_told_once_for_their_input)
{
    ListenerRig rig;
    // Bound: the talker is named and probing begins (Probing status).
    EXPECT_TRUE(rig.bind(1, at(1000)));
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{1}));
    EXPECT_TRUE(rig.changed().empty());
    EXPECT_EQ(rig.probes.size(), 1U);
    if (rig.probes.empty()) {
        return;
    }

    // The probe is answered: settled (Probing status again).
    EXPECT_TRUE(rig.listener.receive_talker_response(ListenerRig::answer_to(rig.probes.back()), at(1010)));
    EXPECT_TRUE(rig.listener.is_settled(1));
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{1}));
    EXPECT_TRUE(rig.changed().empty());

    // Unbound: no talker, probing disabled.
    EXPECT_TRUE(rig.listener.receive_controller_command(
        command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, 1, LISTENER, 1), at(2000)));
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{1}));
    EXPECT_TRUE(rig.changed().empty());
}

TEST(nanoavb_milan_listener, the_talker_attributes_registration_is_told_for_a_settled_input_alone)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.bind(0, at(1000)));
    EXPECT_TRUE(rig.bind(2, at(1000)));
    EXPECT_EQ(rig.probes.size(), 2U);
    if (rig.probes.size() != 2) {
        return;
    }
    // Input 0 settles, input 2 goes on waiting for its answer.
    EXPECT_TRUE(rig.listener.receive_talker_response(ListenerRig::answer_to(rig.probes.front()), at(1010)));
    EXPECT_TRUE(rig.listener.is_settled(0));
    EXPECT_FALSE(rig.listener.is_settled(2));
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{0, 2}));

    // SRP registers the Talker attribute: nothing in the response moves, and it is told.
    rig.registration = MilanTalkerRegistration::Advertise;
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{0}));
    EXPECT_TRUE(rig.changed().empty());
    // Advertise to Failed is another registration state, and so is its going.
    rig.registration = MilanTalkerRegistration::Failed;
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{0}));
    rig.registration = MilanTalkerRegistration::None;
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{0}));
    EXPECT_TRUE(rig.changed().empty());
}

TEST(nanoavb_milan_listener, an_input_back_as_it_was_between_two_looks_is_not_told)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.bind(1, at(1000)));
    EXPECT_TRUE(rig.listener.receive_controller_command(
        command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, 1, LISTENER, 1), at(1001)));
    EXPECT_TRUE(rig.changed().empty());
}

TEST(nanoavb_milan_listener, a_binding_restored_at_start_is_told_at_the_first_look)
{
    ListenerRig rig;
    EXPECT_TRUE(rig.listener.restore_binding(
        MilanBinding{
            .listener_unique_id = 2, .talker_entity_id = TALKER, .talker_unique_id = 1, .controller_entity_id = CONTROLLER},
        at(0)));
    EXPECT_TRUE((rig.changed() == std::vector<uint16_t>{2}));
}

//
// Milan v1.3 4.4.1: the VLANs of the settled sinks
//

namespace {

/// A ListenerRig that keeps what on_sink_vlans_changed is told, and when.
struct VlanRig
{
    ListenerRig rig{};
    std::vector<std::vector<uint16_t>> told{};
    std::vector<TimePoint> when{};

    VlanRig()
    {
        rig.listener.set_on_sink_vlans_changed([this](std::span<uint16_t const> const vlans, TimePoint const now) {
            told.emplace_back(vlans.begin(), vlans.end());
            when.push_back(now);
        });
    }
    VlanRig(VlanRig const&) = delete;
    auto operator=(VlanRig const&) -> VlanRig& = delete;

    /// Bind @p sink, and its talker answers the probe with a stream on @p vlan.
    void settle(uint16_t const sink, uint16_t const vlan, TimePoint const now)
    {
        EXPECT_TRUE(rig.bind(sink, now));
        auto answer = ListenerRig::answer_to(rig.probes.back());
        answer.stream_vlan_id = vlan;
        EXPECT_TRUE(rig.listener.receive_talker_response(answer, now));
    }

    void unbind(uint16_t const sink, TimePoint const now)
    {
        EXPECT_TRUE(rig.listener.receive_controller_command(
            command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER, 1, LISTENER, sink), now));
    }
};

}  // namespace

TEST(nanoavb_milan_listener, the_vlans_of_the_settled_sinks_are_told_when_the_list_changes)
{
    VlanRig vlans;
    EXPECT_TRUE(vlans.rig.listener.sink_vlans().empty());
    vlans.settle(0, 7, at(5));
    EXPECT_TRUE((vlans.told == std::vector<std::vector<uint16_t>>{{7}}));
    EXPECT_EQ(vlans.when.back(), at(5));

    // A second sink on the same VLAN: the list is as it was.
    vlans.settle(1, 7, at(10));
    EXPECT_EQ(vlans.told.size(), 1U);
    vlans.settle(2, 9, at(20));
    EXPECT_TRUE((vlans.told.back() == std::vector<uint16_t>{7, 9}));

    // One of the two on VLAN 7 goes and 7 stays; the other goes and 7 goes.
    vlans.unbind(0, at(30));
    EXPECT_EQ(vlans.told.size(), 2U);
    vlans.unbind(1, at(40));
    EXPECT_TRUE((vlans.told.back() == std::vector<uint16_t>{9}));
    EXPECT_EQ(vlans.when.back(), at(40));
    vlans.unbind(2, at(50));
    EXPECT_TRUE(vlans.told.back().empty());
    EXPECT_EQ(vlans.told.size(), 4U);
    EXPECT_TRUE(vlans.rig.listener.sink_vlans().empty());
}

TEST(nanoavb_milan_listener, a_sink_that_is_bound_and_not_settled_adds_no_vlan)
{
    VlanRig vlans;
    EXPECT_TRUE(vlans.rig.bind(0, at(0)));
    EXPECT_TRUE(vlans.told.empty());
    EXPECT_TRUE(vlans.rig.listener.sink_vlans().empty());
}

TEST(nanoavb_milan_listener, a_talker_that_names_no_vlan_adds_none)
{
    // IEEE 1722.1-2021 8.2.1.17: 0 is the VLAN of the SRP Domain, which the
    // entity declares for itself.
    VlanRig vlans;
    vlans.settle(0, 0, at(0));
    EXPECT_TRUE(vlans.rig.listener.is_settled(0));
    EXPECT_TRUE(vlans.told.empty());
    EXPECT_TRUE(vlans.rig.listener.sink_vlans().empty());
}

TEST(nanoavb_milan_listener, a_restart_takes_the_sinks_vlans_away)
{
    VlanRig vlans;
    vlans.settle(0, 7, at(0));
    vlans.rig.listener.restart(at(1000));
    EXPECT_EQ(vlans.told.size(), 2U);
    EXPECT_TRUE(vlans.told.back().empty());
    EXPECT_EQ(vlans.when.back(), at(1000));
}

TEST(nanoavb_milan_listener, the_vlan_is_told_before_the_connect_and_taken_away_after_the_disconnect)
{
    VlanRig vlans;
    std::vector<std::string> order{};
    vlans.rig.listener.set_connection_callbacks(
        [&order](uint16_t, Eui64 const&, Eui48, TimePoint) { order.emplace_back("connect"); },
        [&order](uint16_t, TimePoint) { order.emplace_back("disconnect"); });
    vlans.rig.listener.set_on_sink_vlans_changed(
        [&order](std::span<uint16_t const> const list, TimePoint) { order.emplace_back(list.empty() ? "no vlan" : "vlan"); });
    vlans.settle(0, 7, at(0));
    vlans.unbind(0, at(10));
    EXPECT_TRUE((order == std::vector<std::string>{"vlan", "connect", "disconnect", "no vlan"}));
}

TEST(nanoavb_milan_listener, its_own_random_delay_stays_within_a_second_and_varies)
{
    NanoAvbMilanAcmpListener a{LISTENER, AcmpListenerCallbacks{}, 1};
    NanoAvbMilanAcmpListener b{SOMEONE_ELSE, AcmpListenerCallbacks{}, 1};
    int differing = 0;
    std::optional<TimePoint> first{};
    bool varied = false;
    for (int round = 0; round < 16; ++round) {
        auto const now = at(round * 20'000);
        for (auto* listener : {&a, &b}) {
            EXPECT_TRUE(listener->restore_binding(MilanBinding{.listener_unique_id = 0, .talker_entity_id = TALKER}, now));
            listener->set_gptp_state([]() { return atdecc::MilanGptpState{.grandmaster_id = GRANDMASTER}; });
            listener->receive_adpdu(available(TALKER, 1), now);
            EXPECT_EQ(listener->sink(0)->state(), MilanSinkState::PrbWDelay);
            auto const delay = *listener->sink(0)->context().timer - now;
            EXPECT_TRUE(delay >= std::chrono::milliseconds{0});
            EXPECT_TRUE(delay <= atdecc::MILAN_TMR_DELAY_MAX);
        }
        auto const delay_a = *a.sink(0)->context().timer - now;
        differing += delay_a != *b.sink(0)->context().timer - now ? 1 : 0;
        varied = varied || (first.has_value() && TimePoint{delay_a} != *first);
        first = first.has_value() ? first : std::optional{TimePoint{delay_a}};
    }
    EXPECT_TRUE(differing >= 12);  // two entities do not draw the same delays
    EXPECT_TRUE(varied);           // nor one entity the same delay every time
}

TEST_MAIN(statusbar_nanoavb, nanoavb_milan_acmp_test)
