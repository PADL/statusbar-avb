// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ATDECC Enumeration and Control Protocol (AECP) data units

#include "statusbar/atdecc/atdecc_format.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::avtp;
using namespace statusbar::ieee;

//
// AECP message type name lookups
//

TEST(aecp_types, message_type_names)
{
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_AEM_COMMAND)) == "AEM Command");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_AEM_RESPONSE)) == "AEM Response");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND)) == "Address Access Command");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE)) == "Address Access Response");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_AVC_COMMAND)) == "AVC Command");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_AVC_RESPONSE)) == "AVC Response");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND)) == "Vendor Unique Command");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE)) == "Vendor Unique Response");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_HDCP_APM_COMMAND)) == "HDCP APM Command");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE)) == "HDCP APM Response");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_EXTENDED_COMMAND)) == "Extended Command");
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_EXTENDED_RESPONSE)) == "Extended Response");
    EXPECT_TRUE(std::string(aecp_message_type_name(0xFF)) == "Unknown");
}

//
// AECP status name lookups
//

TEST(aecp_types, status_names)
{
    EXPECT_TRUE(std::string(aecp_status_name(AECP_STATUS_SUCCESS)) == "Success");
    EXPECT_TRUE(std::string(aecp_status_name(AECP_STATUS_NOT_IMPLEMENTED)) == "Not Implemented");
    EXPECT_TRUE(std::string(aecp_status_name(0xFF)) == "Unknown");
}

//
// AecpDuCommon construction
//

TEST(aecp_pdu, construction)
{
    AecpDuCommon aecp{};
    EXPECT_EQ(aecp.subtype.get(), 0u);
    EXPECT_EQ(aecp.sv(), false);
    EXPECT_EQ(aecp.version(), 0u);
    EXPECT_EQ(aecp.message_type(), 0u);
    EXPECT_EQ(aecp.status(), 0u);
    EXPECT_EQ(aecp.control_data_length(), 0u);
    EXPECT_EQ(aecp.target_entity_id, Eui64{});
    EXPECT_EQ(aecp.controller_entity_id, Eui64{});
    EXPECT_EQ(aecp.sequence_id.get(), 0u);
}

//
// Controller entity ID accessors
//

TEST(aecp_pdu, controller_entity_id)
{
    AecpDuCommon aecp{};
    Eui64 const ctrl_id(0x00, 0x1b, 0x21, 0xff, 0xfe, 0xAA, 0xBB, 0xCC);
    aecp.controller_entity_id = ctrl_id;
    EXPECT_EQ(aecp.controller_entity_id, ctrl_id);

    Eui64 const target_id(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    aecp.target_entity_id = target_id;
    EXPECT_EQ(aecp.target_entity_id, target_id);
    // Verify controller wasn't clobbered
    EXPECT_EQ(aecp.controller_entity_id, ctrl_id);
}

//
// Init command/response
//

TEST(aecp_pdu, init_command)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 20);

    EXPECT_EQ(aecp.subtype.get(), AvtpSubtype::aecp);
    EXPECT_EQ(aecp.message_type(), AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_EQ(aecp.status(), AECP_STATUS_SUCCESS);
    EXPECT_EQ(aecp.control_data_length(), 20u);
    EXPECT_TRUE(aecp.is_command());
    EXPECT_FALSE(aecp.is_response());
    EXPECT_TRUE(aecp.is_aem());
}

TEST(aecp_pdu, init_response)
{
    AecpDuCommon aecp{};
    aecp.init_response(AECP_MESSAGE_TYPE_AEM_RESPONSE, AECP_STATUS_NOT_IMPLEMENTED, 30);

    EXPECT_EQ(aecp.subtype.get(), AvtpSubtype::aecp);
    EXPECT_EQ(aecp.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(aecp.status(), AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aecp.control_data_length(), 30u);
    EXPECT_FALSE(aecp.is_command());
    EXPECT_TRUE(aecp.is_response());
    EXPECT_TRUE(aecp.is_aem());
}

//
// Message type helpers
//

TEST(aecp_pdu, message_type_helpers)
{
    AecpDuCommon aecp{};

    // AEM
    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_TRUE(aecp.is_aem());
    EXPECT_TRUE(aecp.is_command());
    EXPECT_FALSE(aecp.is_response());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_TRUE(aecp.is_aem());
    EXPECT_FALSE(aecp.is_command());
    EXPECT_TRUE(aecp.is_response());

    // Address Access
    aecp.set_message_type(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND);
    EXPECT_TRUE(aecp.is_address_access());
    EXPECT_FALSE(aecp.is_aem());

    // AVC
    aecp.set_message_type(AECP_MESSAGE_TYPE_AVC_COMMAND);
    EXPECT_TRUE(aecp.is_avc());

    // Vendor Unique
    aecp.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    EXPECT_TRUE(aecp.is_vendor_unique());

    // HDCP APM
    aecp.set_message_type(AECP_MESSAGE_TYPE_HDCP_APM_COMMAND);
    EXPECT_TRUE(aecp.is_hdcp_apm());

    // Extended
    aecp.set_message_type(AECP_MESSAGE_TYPE_EXTENDED_COMMAND);
    EXPECT_TRUE(aecp.is_extended());
}

//
// Validation
//

TEST(aecp_pdu, validation)
{
    // Default-constructed is not valid
    AecpDuCommon aecp{};
    EXPECT_FALSE(aecp.is_valid());

    // Valid after init_command with sufficient cdl
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, AecpDuCommon::COMMON_DATA_LENGTH);
    EXPECT_TRUE(aecp.is_valid());

    // CDL too small
    AecpDuCommon short_aecp{};
    short_aecp.subtype = AvtpSubtype::aecp;
    short_aecp.set_control_data_length(5);
    EXPECT_FALSE(short_aecp.is_valid());

    // CDL too large
    AecpDuCommon large_aecp{};
    large_aecp.subtype = AvtpSubtype::aecp;
    large_aecp.set_control_data_length(AECP_MAX_CONTROL_DATA_LENGTH + 1);
    EXPECT_FALSE(large_aecp.is_valid());

    // Invalid message type (10-13 are not valid)
    AecpDuCommon bad_mt{};
    bad_mt.subtype = AvtpSubtype::aecp;
    bad_mt.set_control_data_length(AecpDuCommon::COMMON_DATA_LENGTH);
    bad_mt.set_message_type(10);
    EXPECT_FALSE(bad_mt.is_valid());
}

//
// Serialization roundtrip
//

TEST(aecp_pdu, serialization_roundtrip)
{
    AecpDuCommon original{};
    original.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 50);
    original.target_entity_id = Eui64(0x00, 0x1b, 0x21, 0xff, 0xfe, 0x00, 0x01, 0x02);
    original.controller_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    original.sequence_id = 0x1234;

    // Serialize to raw bytes
    std::array<uint8_t, AecpDuCommon::LENGTH> wire{};
    span_store(wire, original);

    // Deserialize from raw bytes
    AecpDuCommon restored{};
    span_load(restored, wire);

    EXPECT_EQ(original, restored);
    EXPECT_TRUE(restored.is_valid());
    EXPECT_EQ(restored.target_entity_id, original.target_entity_id);
    EXPECT_EQ(restored.controller_entity_id, original.controller_entity_id);
    EXPECT_EQ(restored.sequence_id.get(), 0x1234u);
    EXPECT_EQ(restored.message_type(), AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_EQ(restored.control_data_length(), 50u);
}

//
// Wire format layout
//

TEST(aecp_pdu, wire_format_layout)
{
    EXPECT_EQ(sizeof(AecpDuCommon), 22u);
    EXPECT_EQ(AecpDuCommon::LENGTH, 22u);
    EXPECT_EQ(AecpDuCommon::HEADER_LENGTH, 12u);
    EXPECT_EQ(AecpDuCommon::COMMON_DATA_LENGTH, 10u);
    EXPECT_EQ(offsetof(AecpDuCommon, subtype), 0u);
    EXPECT_EQ(offsetof(AecpDuCommon, target_entity_id), 4u);
    EXPECT_EQ(offsetof(AecpDuCommon, controller_entity_id), 12u);
    EXPECT_EQ(offsetof(AecpDuCommon, sequence_id), 20u);
}

//
// Status field
//

TEST(aecp_pdu, status_field)
{
    AecpDuCommon aecp{};
    aecp.set_status(AECP_STATUS_SUCCESS);
    EXPECT_EQ(aecp.status(), AECP_STATUS_SUCCESS);

    aecp.set_status(AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aecp.status(), AECP_STATUS_NOT_IMPLEMENTED);

    // Verify status doesn't clobber control_data_length
    aecp.set_control_data_length(100);
    aecp.set_status(AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aecp.status(), AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aecp.control_data_length(), 100u);
}

//
// Format output
//

TEST(aecp_pdu, format_to_command)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 20);
    aecp.target_entity_id = Eui64(0x00, 0x1b, 0x21, 0xff, 0xfe, 0x00, 0x01, 0x02);
    aecp.controller_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    aecp.sequence_id = 42;

    std::string result;
    format_to(std::back_inserter(result), aecp);
    EXPECT_TRUE(result.find("AECP:") != std::string::npos);
    EXPECT_TRUE(result.find("AEM Command") != std::string::npos);
    EXPECT_TRUE(result.find("seq=42") != std::string::npos);
    EXPECT_TRUE(result.find("target=") != std::string::npos);
    EXPECT_TRUE(result.find("controller=") != std::string::npos);
}

TEST(aecp_pdu, format_to_response)
{
    AecpDuCommon aecp{};
    aecp.init_response(AECP_MESSAGE_TYPE_AEM_RESPONSE, AECP_STATUS_SUCCESS, 20);

    std::string result;
    format_to(std::back_inserter(result), aecp);
    EXPECT_TRUE(result.find("AEM Response") != std::string::npos);
    EXPECT_TRUE(result.find("status=") != std::string::npos);
}

//
// aecp_echo_response
//

// The command echoed as its response type with the status, its control data
// as declared (received padding is not echoed); malformed commands give an
// empty span.
TEST(aecp_pdu, echo_response)
{
    std::array<uint8_t, 40> command{};
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND, AecpDuCommon::COMMON_DATA_LENGTH + 4);
    aecp.sequence_id = 7;
    span_store(command, aecp);
    command[22] = 0xAB;
    command[25] = 0xCD;
    std::array<uint8_t, 64> out{};

    auto const response = aecp_echo_response(command, AECP_STATUS_NOT_IMPLEMENTED, out);
    EXPECT_EQ(response.size(), size_t{26});
    EXPECT_EQ(response.data(), out.data());
    AecpDuCommon reply{};
    span_load(reply, std::span<uint8_t const>{response}.first(AecpDuCommon::LENGTH));
    EXPECT_EQ(reply.message_type(), AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE);
    EXPECT_EQ(reply.status(), AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(reply.control_data_length(), uint16_t{14});
    EXPECT_EQ(reply.sequence_id.get(), uint16_t{7});
    EXPECT_EQ(response[22], uint8_t{0xAB});
    EXPECT_EQ(response[25], uint8_t{0xCD});

    // Received shorter than declared: echoed as far as received
    EXPECT_EQ(aecp_echo_response(std::span<uint8_t const>{command}.first(24), AECP_STATUS_NOT_IMPLEMENTED, out).size(), size_t{24});
    // Shorter than the AECP common header, or control_data_length short of the
    // common fields: not echoed
    EXPECT_TRUE(aecp_echo_response(std::span<uint8_t const>{command}.first(21), AECP_STATUS_NOT_IMPLEMENTED, out).empty());
    aecp.set_control_data_length(4);
    span_store(command, aecp);
    EXPECT_TRUE(aecp_echo_response(command, AECP_STATUS_NOT_IMPLEMENTED, out).empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc, atdecc_aecp_test)
