// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the Milan talker's connection management responses (Milan
// v1.2 Section 5.5.4, Tables 5.40-5.48), field by field.

#include "statusbar/atdecc/atdecc_acmp_milan_talker.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar;
using namespace statusbar::atdecc;
using ieee::Eui48;
using ieee::Eui64;

namespace {

Eui64 const CONTROLLER{0x00, 0x1B, 0x92, 0xFF, 0xFE, 0x00, 0x00, 0x01};
Eui64 const TALKER{0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x02};
Eui64 const LISTENER{0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x03};
Eui64 const STREAM{0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01};
Eui48 const DEST{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};

auto command(uint8_t const type, uint16_t const flags = 0) -> AcmpCommandResponse
{
    AcmpCommandResponse cmd{};
    cmd.init_command(type);
    cmd.controller_entity_id = CONTROLLER;
    cmd.talker_entity_id = TALKER;
    cmd.listener_entity_id = LISTENER;
    cmd.talker_unique_id = uint16_t{1};
    cmd.listener_unique_id = uint16_t{4};
    cmd.sequence_id = uint16_t{0x1234};
    cmd.connection_count = uint16_t{7};
    cmd.flags = flags;
    return cmd;
}

auto source(bool const dest_mac_valid = true) -> MilanTalkerSource
{
    return MilanTalkerSource{
        .stream_id = STREAM,
        .stream_dest_mac = DEST,
        .stream_vlan_id = 2,
        .avb_interface_index = 0,
        .dest_mac_valid = dest_mac_valid,
        .registering_failed = false};
}

/// The fields every table copies from the command.
void expect_echoed_ids(AcmpCommandResponse const& resp)
{
    EXPECT_EQ(resp.controller_entity_id, CONTROLLER);
    EXPECT_EQ(resp.talker_entity_id, TALKER);
    EXPECT_EQ(resp.talker_unique_id.get(), uint16_t{1});
    EXPECT_EQ(resp.sequence_id.get(), uint16_t{0x1234});
}

}  // namespace

TEST(acmp_milan_talker, probe_success_table_5_43)
{
    auto const src = source();
    auto const reply = milan_talker_respond(
        command(ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT | acmp_flags::CLASS_B),
        &src,
        0);
    EXPECT_TRUE(reply.has_value());
    auto const& resp = reply->response;
    EXPECT_EQ(resp.message_type(), ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    expect_echoed_ids(resp);
    EXPECT_EQ(resp.listener_entity_id, LISTENER);
    EXPECT_EQ(resp.listener_unique_id.get(), uint16_t{4});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{0});
    // FAST_CONNECT and STREAMING_WAIT echoed; REGISTERING_FAILED 0; CLASS_B
    // (Table 5.25) cleared.
    EXPECT_EQ(resp.flags.get(), uint16_t{acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT});
    EXPECT_EQ(resp.stream_id, STREAM);
    EXPECT_EQ(resp.stream_dest_mac, DEST);
    EXPECT_EQ(resp.stream_vlan_id.get(), uint16_t{2});
    EXPECT_TRUE(reply->probe_counts);
}

TEST(acmp_milan_talker, probe_unknown_unique_id_table_5_40)
{
    auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND), nullptr, 0);
    EXPECT_TRUE(reply.has_value());
    EXPECT_EQ(reply->response.message_type(), ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
    EXPECT_EQ(reply->response.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
    expect_echoed_ids(reply->response);
    EXPECT_FALSE(reply->probe_counts);
}

TEST(acmp_milan_talker, probe_wrong_interface_table_5_41)
{
    auto src = source();
    src.avb_interface_index = 1;
    auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND), &src, 0);
    EXPECT_TRUE(reply.has_value());
    EXPECT_EQ(reply->response.status(), ACMP_STATUS_INCOMPATIBLE_REQUEST);
    expect_echoed_ids(reply->response);
    EXPECT_FALSE(reply->probe_counts);
}

TEST(acmp_milan_talker, probe_without_maap_table_5_42)
{
    // Answered (never ignored) before MAAP has an address; the probe still
    // opens the declaration window.
    auto const src = source(false);
    auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND), &src, 0);
    EXPECT_TRUE(reply.has_value());
    EXPECT_EQ(reply->response.message_type(), ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
    EXPECT_EQ(reply->response.status(), ACMP_STATUS_TALKER_DEST_MAC_FAIL);
    expect_echoed_ids(reply->response);
    EXPECT_TRUE(reply->probe_counts);
}

TEST(acmp_milan_talker, disconnect_tx_table_5_45)
{
    auto const src = source();
    auto cmd = command(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT);
    cmd.stream_id = STREAM;
    auto const reply = milan_talker_respond(cmd, &src, 0);
    EXPECT_TRUE(reply.has_value());
    auto const& resp = reply->response;
    EXPECT_EQ(resp.message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    expect_echoed_ids(resp);
    EXPECT_EQ(resp.listener_entity_id, LISTENER);
    EXPECT_EQ(resp.listener_unique_id.get(), uint16_t{4});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{0});
    EXPECT_EQ(resp.flags.get(), uint16_t{0});
    EXPECT_EQ(resp.stream_id, Eui64{});
    EXPECT_EQ(resp.stream_dest_mac, Eui48{});
    EXPECT_EQ(resp.stream_vlan_id.get(), uint16_t{0});
    EXPECT_FALSE(reply->probe_counts);
}

TEST(acmp_milan_talker, disconnect_tx_unknown_unique_id_table_5_44)
{
    auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND), nullptr, 0);
    EXPECT_TRUE(reply.has_value());
    EXPECT_EQ(reply->response.message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
    EXPECT_EQ(reply->response.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(acmp_milan_talker, get_tx_state_table_5_47)
{
    auto src = source();
    auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, acmp_flags::FAST_CONNECT), &src, 0);
    EXPECT_TRUE(reply.has_value());
    auto const& resp = reply->response;
    EXPECT_EQ(resp.message_type(), ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    expect_echoed_ids(resp);
    EXPECT_EQ(resp.listener_entity_id, Eui64{});
    EXPECT_EQ(resp.listener_unique_id.get(), uint16_t{0});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{0});
    EXPECT_EQ(resp.flags.get(), uint16_t{0});
    EXPECT_EQ(resp.stream_id, STREAM);
    EXPECT_EQ(resp.stream_dest_mac, DEST);
    EXPECT_EQ(resp.stream_vlan_id.get(), uint16_t{2});

    // REGISTERING_FAILED while a Listener Asking Failed is registered.
    src.registering_failed = true;
    auto const failed = milan_talker_respond(command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND), &src, 0);
    EXPECT_EQ(failed->response.flags.get(), uint16_t{acmp_flags::REGISTERING_FAILED});
    EXPECT_FALSE(failed->probe_counts);
}

TEST(acmp_milan_talker, get_tx_state_unknown_unique_id_table_5_46)
{
    auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND), nullptr, 0);
    EXPECT_TRUE(reply.has_value());
    EXPECT_EQ(reply->response.message_type(), ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE);
    EXPECT_EQ(reply->response.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(acmp_milan_talker, get_tx_connection_table_5_48)
{
    auto const src = source();
    for (auto const* s : {&src, static_cast<MilanTalkerSource const*>(nullptr)}) {
        auto const reply = milan_talker_respond(command(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND), s, 0);
        EXPECT_TRUE(reply.has_value());
        EXPECT_EQ(reply->response.message_type(), ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE);
        EXPECT_EQ(reply->response.status(), ACMP_STATUS_NOT_SUPPORTED);
        expect_echoed_ids(reply->response);
    }
}

TEST(acmp_milan_talker, ignores_responses_and_listener_commands)
{
    auto const src = source();
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE,
          ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE,
          ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND,
          ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND,
          ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND}) {
        EXPECT_FALSE(milan_talker_respond(command(type), &src, 0).has_value());
    }
}

TEST_MAIN(statusbar_atdecc, atdecc_acmp_milan_talker_test)
