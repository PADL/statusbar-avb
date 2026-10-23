// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

using namespace statusbar::atdecc;
using statusbar::make_const_span;
using statusbar::span_load;
using statusbar::span_store;
using statusbar::ieee::Eui64;

namespace {

Eui64 const kTarget{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01};
Eui64 const kController{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x02};

}  // namespace

TEST(aecp_mvu, names)
{
    EXPECT_EQ(std::string_view{mvu_command_name(MVU_COMMAND_GET_MILAN_INFO)}, "GET_MILAN_INFO");
    EXPECT_EQ(std::string_view{mvu_command_name(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX)}, "GET_STREAM_INPUT_INFO_EX");
    EXPECT_EQ(std::string_view{mvu_command_name(0x0008)}, "Unknown");
    EXPECT_EQ(std::string_view{mvu_status_name(MVU_STATUS_SUCCESS)}, "SUCCESS");
    EXPECT_EQ(std::string_view{mvu_status_name(MVU_STATUS_PAYLOAD_TOO_SHORT)}, "PAYLOAD_TOO_SHORT");
    EXPECT_EQ(std::string_view{mvu_status_name(4)}, "Unknown");
}

TEST(aecp_mvu, a_command_is_laid_out_as_figure_5_5_has_it)
{
    // The golden GET_MILAN_INFO command: vendor-unique command, MVU protocol,
    // command_type 0, two reserved octets, control_data_length 20.
    AecpMvuDu du{};
    du.init_command(MVU_COMMAND_GET_MILAN_INFO, MVU_GET_MILAN_INFO_COMMAND_LENGTH);
    du.common.target_entity_id = kTarget;
    du.common.controller_entity_id = kController;
    du.common.sequence_id = 60;
    std::array<uint8_t, AecpMvuDu::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, du);

    EXPECT_EQ(bytes[0], 0xFB);  // AECP
    EXPECT_EQ(bytes[1], AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 20);  // status SUCCESS, control_data_length 20
    std::array<uint8_t, 6> const protocol{0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00};
    EXPECT_TRUE(std::equal(protocol.begin(), protocol.end(), bytes.begin() + 22));
    EXPECT_EQ(bytes[28], 0x00);
    EXPECT_EQ(bytes[29], 0x00);

    AecpMvuDu back{};
    span_load(back, make_const_span(bytes));
    EXPECT_EQ(back, du);
    EXPECT_TRUE(back.is_valid());
    EXPECT_TRUE(back.is_command());
    EXPECT_FALSE(back.is_response());
    EXPECT_FALSE(back.is_unsolicited());
    EXPECT_EQ(back.command_type(), MVU_COMMAND_GET_MILAN_INFO);
}

TEST(aecp_mvu, a_response_carries_status_the_u_bit_and_the_payload_length)
{
    AecpMvuDu du{};
    du.init_response(MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS, MvuGetMilanInfoResponse::LENGTH);
    EXPECT_TRUE(du.is_response());
    EXPECT_FALSE(du.is_unsolicited());
    EXPECT_EQ(du.common.control_data_length(), 36);  // 18 of header + 18 of data
    EXPECT_EQ(du.common.status(), MVU_STATUS_SUCCESS);

    du.init_response(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_ENTITY_LOCKED, 0, true);
    EXPECT_TRUE(du.is_unsolicited());
    EXPECT_EQ(du.command_type(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID);
    EXPECT_EQ(du.common.status(), MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(du.common.control_data_length(), AecpMvuDu::MVU_DATA_LENGTH);
}

TEST(aecp_mvu, only_a_vendor_unique_message_with_the_mvu_protocol_is_mvu)
{
    AecpMvuDu du{};
    du.init_command(MVU_COMMAND_GET_MILAN_INFO, MVU_GET_MILAN_INFO_COMMAND_LENGTH);
    EXPECT_TRUE(du.is_mvu());

    // Another vendor's protocol on the same message type.
    AecpMvuDu other = du;
    other.protocol_id = Eui48{0x00, 0x1B, 0xC5, 0x0A, 0xC2, 0x00};
    EXPECT_FALSE(other.is_mvu());
    EXPECT_FALSE(other.is_valid());

    // The MVU protocol_id in an AEM message is not an MVU message.
    AecpMvuDu aem = du;
    aem.common.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_FALSE(aem.is_mvu());
    EXPECT_FALSE(aem.is_valid());

    // A control_data_length that stops before command_type.
    AecpMvuDu truncated = du;
    truncated.common.set_control_data_length(AecpMvuDu::MVU_DATA_LENGTH - 1);
    EXPECT_FALSE(truncated.is_valid());
}

TEST(aecp_mvu, the_get_milan_info_response_matches_figure_5_6)
{
    // The golden response data: protocol_version 1, REDUNDANCY|MVU_BINDING,
    // certification 0.0.0.1, specification 1.3.0.0.
    std::array<uint8_t, 18> const golden{
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x01, 0x03, 0x00, 0x00};
    MvuGetMilanInfoResponse info{};
    span_load(info, make_const_span(golden));
    EXPECT_EQ(info.protocol_version.get(), MVU_PROTOCOL_VERSION);
    EXPECT_EQ(info.features_flags.get(), MVU_FEATURE_REDUNDANCY | MVU_FEATURE_MVU_BINDING);
    EXPECT_EQ(info.certification_version.get(), mvu_version(0, 0, 0, 1));
    EXPECT_EQ(info.specification_version.get(), mvu_version(1, 3));

    std::array<uint8_t, 18> bytes{};
    span_store(std::span<uint8_t>{bytes}, info);
    EXPECT_EQ(bytes, golden);
}

TEST(aecp_mvu, dotted_versions_read_and_print)
{
    EXPECT_EQ(mvu_version_from_dotted("1.3"), std::optional<uint32_t>{0x01030000});
    EXPECT_EQ(mvu_version_from_dotted("1.3.0.0"), std::optional<uint32_t>{0x01030000});
    EXPECT_EQ(mvu_version_from_dotted("0"), std::optional<uint32_t>{0});
    EXPECT_EQ(mvu_version_from_dotted("2.0.1.255"), std::optional<uint32_t>{0x020001FF});
    EXPECT_EQ(mvu_version_from_dotted(""), std::nullopt);
    EXPECT_EQ(mvu_version_from_dotted("1."), std::nullopt);
    EXPECT_EQ(mvu_version_from_dotted(".1"), std::nullopt);
    EXPECT_EQ(mvu_version_from_dotted("1.256"), std::nullopt);
    EXPECT_EQ(mvu_version_from_dotted("1.3.0.0.0"), std::nullopt);
    EXPECT_EQ(mvu_version_from_dotted("1.x"), std::nullopt);
    EXPECT_EQ(mvu_version_to_dotted(0x01030000), "1.3.0.0");
    EXPECT_EQ(mvu_version_to_dotted(0), "0.0.0.0");
}

namespace padl_layouts {
using namespace statusbar;

TEST(aecp_mvu, header_layout)
{
    // AecpMvuDu::init_response: protocol_id at 22, r/command_type at 28,
    // control_data_length = 18 + command_specific_data
    AecpMvuDu du{};
    du.init_response(MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_NOT_IMPLEMENTED, 2);
    std::array<uint8_t, AecpMvuDu::LENGTH> bytes{};
    span_store(bytes, du);
    EXPECT_EQ(bytes[0], AvtpSubtype::aecp);
    EXPECT_EQ(bytes[1] & 0x0F, AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_EQ(bytes[2], uint8_t{0x08});  // status 1 in bits 15-11, cdl 20
    EXPECT_EQ(bytes[3], uint8_t{20});
    EXPECT_TRUE(std::ranges::equal(MVU_PROTOCOL_ID.span(), std::span<uint8_t const>{bytes}.subspan(22, 6)));
    EXPECT_EQ(bytes[28], uint8_t{0x00});
    EXPECT_EQ(bytes[29], uint8_t{0x04});

    // r is masked out of command_type and cleared by set_command_type
    du.u_command_type = uint16_t{0x8001};
    EXPECT_EQ(du.command_type(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID);
    du.set_command_type(du.command_type());
    EXPECT_EQ(du.u_command_type.get(), uint16_t{0x0001});
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
    size_t constexpr BASE = AecpMvuDu::LENGTH;  // AVTPDU offset of byte 0
    EXPECT_EQ(bytes[31 - BASE], uint8_t{1});
    EXPECT_EQ(bytes[32 - BASE], uint8_t{0x02});
    EXPECT_EQ(bytes[34 - BASE], uint8_t{192});
    EXPECT_EQ(bytes[35 - BASE], uint8_t{224});
    EXPECT_EQ(bytes[40 - BASE], uint8_t{'D'});
}

TEST(aecp_mvu, is_mvu_command)
{
    AecpMvuDu du{};
    du.init_command(MVU_COMMAND_GET_MILAN_INFO, MVU_GET_MILAN_INFO_COMMAND_LENGTH);
    std::array<uint8_t, AecpMvuDu::LENGTH + MVU_GET_MILAN_INFO_COMMAND_LENGTH> cmd{};
    span_store(cmd, du);
    EXPECT_TRUE(is_mvu_command(cmd));
    // Too short to hold command_type
    EXPECT_FALSE(is_mvu_command(std::span<uint8_t const>{cmd}.first(AecpMvuDu::LENGTH - 1)));
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

}  // namespace padl_layouts

TEST_MAIN(statusbar_atdecc, atdecc_aecp_mvu_test)
