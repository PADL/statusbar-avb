// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for JDKS vendor-specific ATDECC extensions
// Tests log control, IPv4 parameters, and packet generation/parsing

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::jdks;
using namespace statusbar::avtp;
using namespace statusbar::ieee;

//
// Tests: JDKS Constants
//

TEST(jdks_const, notifications_controller_id)
{
    // 70:B3:D5:FF:FF:ED:CF:FF
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[0], 0x70);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[1], 0xB3);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[2], 0xD5);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[3], 0xFF);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[4], 0xFF);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[5], 0xED);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[6], 0xCF);
    EXPECT_EQ(NOTIFICATIONS_CONTROLLER_ENTITY_ID.span()[7], 0xFF);
}

TEST(jdks_const, multicast_log_address)
{
    // 71:B3:D5:ED:CF:FF
    EXPECT_EQ(MULTICAST_LOG.value[0], 0x71);
    EXPECT_EQ(MULTICAST_LOG.value[1], 0xB3);
    EXPECT_EQ(MULTICAST_LOG.value[2], 0xD5);
    EXPECT_EQ(MULTICAST_LOG.value[3], 0xED);
    EXPECT_EQ(MULTICAST_LOG.value[4], 0xCF);
    EXPECT_EQ(MULTICAST_LOG.value[5], 0xFF);
}

TEST(jdks_const, control_log_text)
{
    // 70:B3:D5:ED:C0:00:00:00
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[0], 0x70);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[1], 0xB3);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[2], 0xD5);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[3], 0xED);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[4], 0xC0);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[5], 0x00);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[6], 0x00);
    EXPECT_EQ(CONTROL_LOG_TEXT.span()[7], 0x00);
}

TEST(jdks_const, control_ipv4_parameters)
{
    // 70:B3:D5:ED:C0:00:00:01
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[0], 0x70);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[1], 0xB3);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[2], 0xD5);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[3], 0xED);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[4], 0xC0);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[5], 0x00);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[6], 0x00);
    EXPECT_EQ(CONTROL_IPV4_PARAMETERS.span()[7], 0x01);
}

TEST(jdks_const, blob_sizes)
{
    EXPECT_EQ(BLOB_MAX_SIZE, 362U);
    EXPECT_EQ(LOG_MAX_TEXT_LEN, 360U);
}

//
// Tests: Log Priority Levels
//

TEST(jdks_priority, values)
{
    EXPECT_EQ(log_priority::ERROR, 0);
    EXPECT_EQ(log_priority::WARNING, 1);
    EXPECT_EQ(log_priority::INFO, 2);
    EXPECT_EQ(log_priority::DEBUG1, 3);
    EXPECT_EQ(log_priority::DEBUG2, 4);
    EXPECT_EQ(log_priority::DEBUG3, 5);
    EXPECT_EQ(log_priority::CONSOLE, 0xFF);
}

TEST(jdks_priority, names)
{
    EXPECT_TRUE(std::string(log_priority_name(log_priority::ERROR)) == "Error");
    EXPECT_TRUE(std::string(log_priority_name(log_priority::WARNING)) == "Warning");
    EXPECT_TRUE(std::string(log_priority_name(log_priority::INFO)) == "Info");
    EXPECT_TRUE(std::string(log_priority_name(log_priority::DEBUG1)) == "Debug1");
    EXPECT_TRUE(std::string(log_priority_name(log_priority::DEBUG2)) == "Debug2");
    EXPECT_TRUE(std::string(log_priority_name(log_priority::DEBUG3)) == "Debug3");
    EXPECT_TRUE(std::string(log_priority_name(log_priority::CONSOLE)) == "Console");
    EXPECT_TRUE(std::string(log_priority_name(100)) == "Unknown");
}

//
// Tests: LogBlobHeader Structure
//

TEST(jdks_log_blob, size)
{
    EXPECT_EQ(sizeof(LogBlobHeader), 14U);
    EXPECT_EQ(LogBlobHeader::LENGTH, 14U);
}

TEST(jdks_log_blob, offsets)
{
    EXPECT_EQ(offsetof(LogBlobHeader, vendor_eui64), 0U);
    EXPECT_EQ(offsetof(LogBlobHeader, blob_size), 8U);
    EXPECT_EQ(offsetof(LogBlobHeader, log_detail), 12U);
    EXPECT_EQ(offsetof(LogBlobHeader, reserved), 13U);
}

TEST(jdks_log_blob, default_constructor)
{
    LogBlobHeader blob{};
    EXPECT_EQ(static_cast<uint32_t>(blob.blob_size), 0U);
    EXPECT_EQ(static_cast<uint8_t>(blob.log_detail), 0);
    EXPECT_EQ(static_cast<uint8_t>(blob.reserved), 0);
}

TEST(jdks_log_blob, set_values)
{
    LogBlobHeader blob{};
    blob.vendor_eui64 = CONTROL_LOG_TEXT;
    blob.blob_size = 100;
    blob.log_detail = log_priority::WARNING;
    blob.reserved = 0;

    EXPECT_TRUE(blob.vendor_eui64 == CONTROL_LOG_TEXT);
    EXPECT_EQ(static_cast<uint32_t>(blob.blob_size), 100U);
    EXPECT_EQ(static_cast<uint8_t>(blob.log_detail), log_priority::WARNING);
}

//
// Tests: Ipv4ParamsBlob Structure
//

TEST(jdks_ipv4_blob, size)
{
    EXPECT_EQ(sizeof(Ipv4ParamsBlob), 32U);
    EXPECT_EQ(Ipv4ParamsBlob::LENGTH, 32U);
}

TEST(jdks_ipv4_blob, default_constructor)
{
    Ipv4ParamsBlob params{};
    EXPECT_EQ(static_cast<uint16_t>(params.interface_descriptor_type), 0U);
    EXPECT_EQ(static_cast<uint16_t>(params.interface_descriptor_index), 0U);
    EXPECT_EQ(static_cast<uint32_t>(params.flags), 0U);
}

TEST(jdks_ipv4_blob, flags)
{
    EXPECT_EQ(ipv4_flags::STATIC_ENABLE, 0x00000001U);
    EXPECT_EQ(ipv4_flags::LINK_LOCAL_ENABLE, 0x00000002U);
    EXPECT_EQ(ipv4_flags::DHCP_ENABLE, 0x00000004U);
    EXPECT_EQ(ipv4_flags::IPV4_ADDRESS_VALID, 0x00000008U);
    EXPECT_EQ(ipv4_flags::IPV4_NETMASK_VALID, 0x00000010U);
    EXPECT_EQ(ipv4_flags::IPV4_GATEWAY_VALID, 0x00000020U);
    EXPECT_EQ(ipv4_flags::IPV4_BROADCAST_VALID, 0x00000040U);
    EXPECT_EQ(ipv4_flags::DNSSERVER1_VALID, 0x00000080U);
    EXPECT_EQ(ipv4_flags::DNSSERVER2_VALID, 0x00000100U);
}

TEST(jdks_ipv4_blob, flag_accessors)
{
    Ipv4ParamsBlob params{};
    params.flags = ipv4_flags::STATIC_ENABLE | ipv4_flags::IPV4_ADDRESS_VALID | ipv4_flags::IPV4_NETMASK_VALID;

    EXPECT_TRUE(params.is_static_enabled());
    EXPECT_FALSE(params.is_link_local_enabled());
    EXPECT_FALSE(params.is_dhcp_enabled());
    EXPECT_TRUE(params.is_address_valid());
    EXPECT_TRUE(params.is_netmask_valid());
    EXPECT_FALSE(params.is_gateway_valid());
    EXPECT_FALSE(params.is_broadcast_valid());
}

TEST(jdks_ipv4_blob, set_address)
{
    Ipv4ParamsBlob params{};
    // 192.0.2.100 = 0xC0000264
    params.ipv4_address = 0xC0000264U;
    params.ipv4_netmask = 0xFFFFFF00U;  // 255.255.255.0
    params.ipv4_gateway = 0xC0000201U;  // 192.0.2.1
    params.flags = ipv4_flags::IPV4_ADDRESS_VALID | ipv4_flags::IPV4_NETMASK_VALID | ipv4_flags::IPV4_GATEWAY_VALID;

    EXPECT_EQ(static_cast<uint32_t>(params.ipv4_address), 0xC0000264U);
    EXPECT_EQ(static_cast<uint32_t>(params.ipv4_netmask), 0xFFFFFF00U);
    EXPECT_EQ(static_cast<uint32_t>(params.ipv4_gateway), 0xC0000201U);
    EXPECT_TRUE(params.is_address_valid());
    EXPECT_TRUE(params.is_netmask_valid());
    EXPECT_TRUE(params.is_gateway_valid());
}

//
// Tests: generate_log_response
//

TEST(jdks_generate_log, basic)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    ctx.descriptor_index = 0;
    uint16_t seq_id = 42;

    std::string_view text = "Test log message";

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::INFO, text);

    // Ethernet (14) + AEM (24) + Control header (4) + Blob header (14) + text (16)
    EXPECT_EQ(len, 14U + 24U + 4U + 14U + 16U);
    EXPECT_EQ(seq_id, 43U);  // Should be incremented
}

TEST(jdks_generate_log, buffer_too_small)
{
    std::array<uint8_t, 20> buffer{};  // Too small
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    uint16_t seq_id = 42;

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::INFO, "Hello");

    EXPECT_EQ(len, 0U);      // Should fail
    EXPECT_EQ(seq_id, 42U);  // Should NOT be incremented
}

TEST(jdks_generate_log, empty_text)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    ctx.descriptor_index = 5;
    uint16_t seq_id = 1;

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::ERROR, "");

    // Ethernet (14) + AEM (24) + Control header (4) + Blob header (14) + text (0)
    EXPECT_EQ(len, 14U + 24U + 4U + 14U);
    EXPECT_EQ(seq_id, 2U);
}

TEST(jdks_generate_log, dest_mac_is_multicast)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    uint16_t seq_id = 1;

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::INFO, "Test");
    EXPECT_TRUE(len > 0);

    // Check destination MAC is MULTICAST_LOG
    Eui48 dest_mac{};
    span_copy(dest_mac.value, make_const_span(buffer).first(6));
    EXPECT_TRUE(dest_mac == MULTICAST_LOG);
}

//
// Tests: generate_console_command
//

TEST(jdks_generate_console, basic)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.dest_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.src_mac = Eui48(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    ctx.my_entity_id = Eui64(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    ctx.target_entity_id = Eui64(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18);
    ctx.descriptor_index = 0;
    uint16_t seq_id = 100;

    std::string_view text = "help";

    auto len = generate_console_command(buffer, ctx, seq_id, log_priority::CONSOLE, text);

    // Ethernet (14) + AEM (24) + Control header (4) + Blob header (14) + text (4)
    EXPECT_EQ(len, 14U + 24U + 4U + 14U + 4U);
    EXPECT_EQ(seq_id, 101U);
}

TEST(jdks_generate_console, buffer_too_small)
{
    std::array<uint8_t, 10> buffer{};  // Too small
    ConsoleCommandContext ctx{};
    ctx.dest_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.src_mac = Eui48(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    ctx.my_entity_id = Eui64(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    ctx.target_entity_id = Eui64(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18);
    uint16_t seq_id = 100;

    auto len = generate_console_command(buffer, ctx, seq_id, log_priority::CONSOLE, "x");

    EXPECT_EQ(len, 0U);
    EXPECT_EQ(seq_id, 100U);  // Should NOT be incremented
}

//
// Tests: is_log_response
//

TEST(jdks_is_log_response, valid)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    uint16_t seq_id = 1;

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::INFO, "Test message");
    EXPECT_TRUE(len > 0);

    LogBlobHeader blob{};
    bool result = is_log_response(make_const_span(buffer).first(len), &blob);

    EXPECT_TRUE(result);
    EXPECT_TRUE(blob.vendor_eui64 == CONTROL_LOG_TEXT);
    EXPECT_EQ(static_cast<uint8_t>(blob.log_detail), log_priority::INFO);
}

TEST(jdks_is_log_response, too_short)
{
    std::array<uint8_t, 10> buffer{};
    bool result = is_log_response(buffer);
    EXPECT_FALSE(result);
}

TEST(jdks_is_log_response, wrong_subtype)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    uint16_t seq_id = 1;

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::INFO, "Test");
    EXPECT_TRUE(len > 0);

    // Corrupt the subtype
    buffer[14] = 0x00;  // Wrong subtype (should be 0xFB for AECP)

    bool result = is_log_response(make_const_span(buffer).first(len));
    EXPECT_FALSE(result);
}

//
// Tests: is_console_command
//

TEST(jdks_is_console_command, valid)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.dest_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.src_mac = Eui48(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    ctx.my_entity_id = Eui64(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    ctx.target_entity_id = Eui64(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18);
    uint16_t seq_id = 1;

    auto len = generate_console_command(buffer, ctx, seq_id, log_priority::CONSOLE, "status");
    EXPECT_TRUE(len > 0);

    LogBlobHeader blob{};
    bool result = is_console_command(make_const_span(buffer).first(len), &blob);

    EXPECT_TRUE(result);
    EXPECT_TRUE(blob.vendor_eui64 == CONTROL_LOG_TEXT);
    EXPECT_EQ(static_cast<uint8_t>(blob.log_detail), log_priority::CONSOLE);
}

TEST(jdks_is_console_command, too_short)
{
    std::array<uint8_t, 10> buffer{};
    bool result = is_console_command(buffer);
    EXPECT_FALSE(result);
}

//
// Tests: parse_log_message
//

TEST(jdks_parse_log, roundtrip)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.src_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.my_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    ctx.descriptor_index = 5;
    uint16_t seq_id = 42;

    std::string_view original_text = "Hello JDKS World!";

    auto len = generate_log_response(buffer, ctx, seq_id, log_priority::WARNING, original_text);
    EXPECT_TRUE(len > 0);

    LogMessage msg{};
    bool result = parse_log_message(make_const_span(buffer).first(len), msg);

    EXPECT_TRUE(result);
    EXPECT_TRUE(msg.target_entity_id == ctx.my_entity_id);
    EXPECT_TRUE(msg.controller_entity_id == NOTIFICATIONS_CONTROLLER_ENTITY_ID);
    EXPECT_EQ(msg.descriptor_index, ctx.descriptor_index);
    EXPECT_EQ(msg.sequence_id, 42U);
    EXPECT_EQ(msg.log_detail, log_priority::WARNING);
    EXPECT_TRUE(msg.text == original_text);
}

TEST(jdks_parse_log, console_command_roundtrip)
{
    std::array<uint8_t, 512> buffer{};
    ConsoleCommandContext ctx{};
    ctx.dest_mac = Eui48(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    ctx.src_mac = Eui48(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    ctx.my_entity_id = Eui64(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    ctx.target_entity_id = Eui64(0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18);
    ctx.descriptor_index = 3;
    uint16_t seq_id = 99;

    std::string_view original_text = "list devices";

    auto len = generate_console_command(buffer, ctx, seq_id, log_priority::CONSOLE, original_text);
    EXPECT_TRUE(len > 0);

    LogMessage msg{};
    bool result = parse_log_message(make_const_span(buffer).first(len), msg);

    EXPECT_TRUE(result);
    EXPECT_TRUE(msg.target_entity_id == ctx.target_entity_id);
    EXPECT_TRUE(msg.controller_entity_id == ctx.my_entity_id);
    EXPECT_EQ(msg.descriptor_index, ctx.descriptor_index);
    EXPECT_EQ(msg.sequence_id, 99U);
    EXPECT_EQ(msg.log_detail, log_priority::CONSOLE);
    EXPECT_TRUE(msg.text == original_text);
}

TEST(jdks_parse_log, invalid_frame)
{
    std::array<uint8_t, 100> buffer{};
    buffer.fill(0);

    LogMessage msg{};
    bool result = parse_log_message(buffer, msg);
    EXPECT_FALSE(result);
}

//
// Tests: LogMessage Structure
//

TEST(jdks_log_message, default_constructor)
{
    LogMessage msg{};
    EXPECT_EQ(msg.descriptor_index, 0U);
    EXPECT_EQ(msg.sequence_id, 0U);
    EXPECT_EQ(msg.log_detail, 0U);
    EXPECT_TRUE(msg.text.empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc, atdecc_jdks_test)