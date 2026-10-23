// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the AECP Milan Vendor Unique wire layouts (Milan v1.2
// Section 5.4.3-5.4.4)

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::mvu;

TEST(aecp_mvu, header_layout)
{
    // MvuDu::init_response: protocol_id at 22, r/command_type at 28,
    // control_data_length = 18 + command_specific_data
    MvuDu du{};
    du.init_response(MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_NOT_IMPLEMENTED, 2);
    std::array<uint8_t, MvuDu::LENGTH> bytes{};
    span_store(bytes, du);
    EXPECT_EQ(bytes[0], AvtpSubtype::aecp);
    EXPECT_EQ(bytes[1] & 0x0F, AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_EQ(bytes[2], uint8_t{0x08});  // status 1 in bits 15-11, cdl 20
    EXPECT_EQ(bytes[3], uint8_t{20});
    EXPECT_TRUE(std::equal(MVU_PROTOCOL_ID.begin(), MVU_PROTOCOL_ID.end(), bytes.begin() + 22));
    EXPECT_EQ(bytes[28], uint8_t{0x00});
    EXPECT_EQ(bytes[29], uint8_t{0x04});

    // r is masked out of command_type and cleared by set_command_type
    du.r_command_type = uint16_t{0x8001};
    EXPECT_EQ(du.command_type(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID);
    du.set_command_type(du.command_type());
    EXPECT_EQ(du.r_command_type.get(), uint16_t{0x0001});
}

// Figure 5.4: protocol_version, features_flags and certification_version at
// AVTPDU offsets 32, 36 and 40
TEST(aecp_mvu, get_milan_info_response_layout)
{
    MvuGetMilanInfoResponse info{};
    info.protocol_version = MVU_PROTOCOL_VERSION;
    info.features_flags = MVU_FEATURES_REDUNDANCY | MVU_FEATURES_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING;
    info.certification_version = milan_certification_version(1, 2, 0, 0);
    std::array<uint8_t, MvuGetMilanInfoResponse::LENGTH> bytes{};
    span_store(bytes, info);
    std::array<uint8_t, 14> const expected{
        0x00,
        0x00,  // reserved (30)
        0x00,
        0x00,
        0x00,
        0x01,  // protocol_version (32)
        0x00,
        0x00,
        0x00,
        0x03,  // features_flags (36)
        0x01,
        0x02,
        0x00,
        0x00,  // certification_version (40): 1.2.0.0
    };
    EXPECT_TRUE(bytes == expected);
}

// Figure 5.6: flags at 32, the priorities at 34-35, the name at 40-103
TEST(aecp_mvu, media_clock_reference_info_layout)
{
    MvuMediaClockReferenceInfoPayload mcr{};
    mcr.clock_domain_index = uint16_t{1};
    mcr.flags = MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID;
    mcr.default_mcr_prio = uint8_t{192};
    mcr.user_mcr_prio = uint8_t{224};
    mcr.media_clock_domain_name[0] = 'D';
    std::array<uint8_t, MvuMediaClockReferenceInfoPayload::LENGTH> bytes{};
    span_store(bytes, mcr);
    size_t constexpr BASE = MvuDu::LENGTH;  // AVTPDU offset of byte 0
    EXPECT_EQ(bytes[31 - BASE], uint8_t{1});
    EXPECT_EQ(bytes[32 - BASE], uint8_t{0x02});
    EXPECT_EQ(bytes[34 - BASE], uint8_t{192});
    EXPECT_EQ(bytes[35 - BASE], uint8_t{224});
    EXPECT_EQ(bytes[40 - BASE], uint8_t{'D'});
}

TEST(aecp_mvu, is_mvu_command)
{
    MvuDu du{};
    du.init_command(MVU_COMMAND_GET_MILAN_INFO, MvuGetMilanInfoCommand::LENGTH);
    std::array<uint8_t, MvuDu::LENGTH + MvuGetMilanInfoCommand::LENGTH> cmd{};
    span_store(cmd, du);
    EXPECT_TRUE(is_mvu_command(cmd));
    // Too short to hold command_type
    EXPECT_FALSE(is_mvu_command(std::span<uint8_t const>{cmd}.first(MvuDu::LENGTH - 1)));
    // Another vendor unique protocol
    auto other = cmd;
    other[27] = 0x01;
    EXPECT_FALSE(is_mvu_command(other));
    // A response
    auto response = cmd;
    response[1] = static_cast<uint8_t>((response[1] & 0xF0) | AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_FALSE(is_mvu_command(response));
    // control_data_length short of protocol_id + command_type
    auto short_cdl = cmd;
    short_cdl[3] = 16;
    EXPECT_FALSE(is_mvu_command(short_cdl));
    // Not AECP
    auto acmp = cmd;
    acmp[0] = AvtpSubtype::acmp;
    EXPECT_FALSE(is_mvu_command(acmp));
}

TEST_MAIN(statusbar_atdecc, atdecc_aecp_mvu_test)
