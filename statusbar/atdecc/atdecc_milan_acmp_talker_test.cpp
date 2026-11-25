// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The Milan v1.3 talker's responses, section 5.5.4: Tables 5.45 to 5.53.

#include "statusbar/atdecc/atdecc_milan_acmp_talker.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <optional>

using namespace statusbar;
using namespace statusbar::atdecc;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

constexpr Eui64 LISTENER{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr Eui64 TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr Eui64 CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
constexpr MilanTalkerStream STREAM{
    .stream_id = Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x05},
    .stream_dest_mac = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34},
    .stream_vlan_id = 2};

/// A talker command as a listener or controller sends it, the fields the
/// talker must overwrite filled with something else.
auto command(uint8_t const type, uint16_t const flags = 0) -> AcmpCommandResponse
{
    AcmpCommandResponse cmd{};
    cmd.init_command(type);
    cmd.controller_entity_id = CONTROLLER;
    cmd.talker_entity_id = TALKER;
    cmd.listener_entity_id = LISTENER;
    cmd.talker_unique_id = 5;
    cmd.listener_unique_id = 3;
    cmd.sequence_id = 4242;
    cmd.flags = flags;
    cmd.connection_count = 9;
    cmd.stream_id = Eui64{1, 2, 3, 4, 5, 6, 7, 8};
    cmd.stream_dest_mac = Eui48{1, 2, 3, 4, 5, 6};
    cmd.stream_vlan_id = 99;
    return cmd;
}

/// What every response repeats from its command.
void expect_echo(AcmpCommandResponse const& resp, AcmpCommandResponse const& cmd)
{
    EXPECT_EQ(resp.message_type(), cmd.message_type() + 1);
    EXPECT_EQ(resp.controller_entity_id, CONTROLLER);
    EXPECT_EQ(resp.talker_entity_id, TALKER);
    EXPECT_EQ(resp.talker_unique_id.get(), 5);
    EXPECT_EQ(resp.sequence_id.get(), 4242);
}

void expect_stream(AcmpCommandResponse const& resp, MilanTalkerStream const& stream)
{
    EXPECT_EQ(resp.stream_id, stream.stream_id);
    EXPECT_EQ(resp.stream_dest_mac, stream.stream_dest_mac);
    EXPECT_EQ(resp.stream_vlan_id.get(), stream.stream_vlan_id);
}

constexpr MilanTalkerSource KNOWN{.stream = &STREAM};
constexpr MilanTalkerSource UNKNOWN{};

}  // namespace

TEST(milan_talker, a_probe_for_a_source_the_talker_has_gets_its_parameters)
{
    // 5.5.4.1 step 4, Table 5.48
    uint16_t const asked = acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT | acmp_flags::CLASS_B;
    auto const cmd = command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, asked);
    auto const resp = milan_probe_tx_response(cmd, KNOWN);
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.listener_entity_id, LISTENER);
    EXPECT_EQ(resp.listener_unique_id.get(), 3);
    EXPECT_EQ(resp.connection_count.get(), 0);                                           // a Milan talker counts no listeners
    EXPECT_EQ(resp.flags.get(), acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT);  // as asked; REGISTERING_FAILED 0
    expect_stream(resp, STREAM);
}

TEST(milan_talker, a_probe_for_an_unknown_source_is_talker_unknown_id)
{
    // 5.5.4.1 step 1, Table 5.45
    auto const cmd = command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    auto const resp = milan_probe_tx_response(cmd, UNKNOWN);
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
    EXPECT_EQ(resp.listener_entity_id, LISTENER);
}

TEST(milan_talker, a_probe_while_no_destination_address_is_valid_is_talker_dest_mac_failed)
{
    // 5.5.4.1 step 3, Table 5.47
    auto const cmd = command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    auto const resp = milan_probe_tx_response(cmd, MilanTalkerSource{.stream = &STREAM, .dest_mac_valid = false});
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_TALKER_DEST_MAC_FAIL);
    // An unknown source is said first, whatever the addresses.
    auto const unknown = milan_probe_tx_response(cmd, MilanTalkerSource{.stream = nullptr, .dest_mac_valid = false});
    EXPECT_EQ(unknown.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(milan_talker, a_disconnect_succeeds_with_everything_zero)
{
    // 5.5.4.2, Tables 5.49 and 5.50
    auto const cmd = command(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT);
    auto const resp = milan_disconnect_tx_response(cmd, KNOWN);
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.listener_entity_id, LISTENER);
    EXPECT_EQ(resp.listener_unique_id.get(), 3);
    EXPECT_EQ(resp.connection_count.get(), 0);
    EXPECT_EQ(resp.flags.get(), 0);
    expect_stream(resp, MilanTalkerStream{});

    auto const unknown = milan_disconnect_tx_response(cmd, UNKNOWN);
    expect_echo(unknown, cmd);
    EXPECT_EQ(unknown.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(milan_talker, get_tx_state_names_no_listener_and_reports_registering_failed)
{
    // 5.5.4.3, Tables 5.51 and 5.52
    auto const cmd = command(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, acmp_flags::FAST_CONNECT | acmp_flags::STREAMING_WAIT);
    for (bool const failed : {false, true}) {
        auto const resp = milan_get_tx_state_response(cmd, MilanTalkerSource{.stream = &STREAM, .registering_failed = failed});
        expect_echo(resp, cmd);
        EXPECT_EQ(resp.status(), ACMP_STATUS_SUCCESS);
        EXPECT_EQ(resp.listener_entity_id, Eui64{});
        EXPECT_EQ(resp.listener_unique_id.get(), 0);
        EXPECT_EQ(resp.connection_count.get(), 0);
        EXPECT_EQ(resp.flags.get(), failed ? acmp_flags::SRP_REGISTRATION_FAILED : 0);
        expect_stream(resp, STREAM);
    }
    auto const unknown = milan_get_tx_state_response(cmd, UNKNOWN);
    expect_echo(unknown, cmd);
    EXPECT_EQ(unknown.status(), ACMP_STATUS_TALKER_UNKNOWN_ID);
}

TEST(milan_talker, get_tx_connection_is_not_supported)
{
    // 5.5.4.4, Table 5.53: whatever the source.
    auto const cmd = command(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND);
    auto const resp = milan_get_tx_connection_response(cmd);
    expect_echo(resp, cmd);
    EXPECT_EQ(resp.status(), ACMP_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(milan_talker_response(cmd, UNKNOWN)->status(), ACMP_STATUS_NOT_SUPPORTED);
}

TEST(milan_talker, each_talker_command_gets_its_own_response_and_nothing_else_gets_one)
{
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND,
          ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND,
          ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND,
          ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND}) {
        auto const resp = milan_talker_response(command(type), KNOWN);
        EXPECT_TRUE(resp.has_value());
        EXPECT_EQ(resp->message_type(), type + 1);
    }
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND,
          ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND,
          ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND,
          ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE,
          ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE}) {
        EXPECT_FALSE(milan_talker_response(command(type), KNOWN).has_value());
    }
}

TEST_MAIN(statusbar_atdecc, atdecc_milan_acmp_talker_test)
