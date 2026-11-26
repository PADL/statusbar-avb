// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"

#include "statusbar/buffer/span_utils.hpp"
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

TEST(aecp_mvu, a_bind_stream_is_laid_out_as_figure_5_10_has_it)
{
    MvuBindStream bind{};
    bind.flags = MVU_BIND_STREAM_FLAG_STREAMING_WAIT;
    bind.descriptor_type = 0x0005;  // STREAM_INPUT
    bind.descriptor_index = 0x0102;
    bind.talker_entity_id = kTarget;
    bind.talker_stream_index = 0x0304;
    std::array<uint8_t, MvuBindStream::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, bind);

    // Octets 30 to 47 of the AECPDU: flags, descriptor, talker, its stream, reserved.
    std::array<uint8_t, MvuBindStream::LENGTH> const golden{
        0x00, 0x01, 0x00, 0x05, 0x01, 0x02, 0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01, 0x03, 0x04, 0x00, 0x00};
    EXPECT_TRUE(bytes == golden);
    EXPECT_TRUE(bind.streaming_wait());

    MvuBindStream back{};
    span_load(back, make_const_span(bytes));
    EXPECT_EQ(back, bind);
    back.flags = 0xFFFE;  // every bit Table 5.19 reserves
    EXPECT_FALSE(back.streaming_wait());
}

TEST(aecp_mvu, a_command_naming_a_stream_input_is_laid_out_as_figure_5_11_has_it)
{
    // UNBIND_STREAM, its response, and the GET_STREAM_INPUT_INFO_EX command.
    MvuStreamInput input{};
    input.descriptor_type = 0x0005;
    input.descriptor_index = 0x0102;
    std::array<uint8_t, MvuStreamInput::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, input);
    std::array<uint8_t, MvuStreamInput::LENGTH> const golden{0x00, 0x00, 0x00, 0x05, 0x01, 0x02};
    EXPECT_TRUE(bytes == golden);

    MvuStreamInput back{};
    span_load(back, make_const_span(bytes));
    EXPECT_EQ(back, input);
}

TEST(aecp_mvu, a_stream_input_info_ex_response_is_laid_out_as_figure_5_12_has_it)
{
    MvuGetStreamInputInfoExResponse info{};
    info.descriptor_type = 0x0005;
    info.descriptor_index = 0x0102;
    info.talker_entity_id = kTarget;
    info.talker_unique_id = 0x0304;
    info.set_status(2, 7);  // PROBING_ACTIVE, LISTENER_TALKER_TIMEOUT
    std::array<uint8_t, MvuGetStreamInputInfoExResponse::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, info);

    // pbsta is the three high bits of octet 46 and acmpsta its five low bits:
    // 010 00111.
    std::array<uint8_t, MvuGetStreamInputInfoExResponse::LENGTH> const golden{
        0x00, 0x00, 0x00, 0x05, 0x01, 0x02, 0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01, 0x03, 0x04, 0x47, 0x00};
    EXPECT_TRUE(bytes == golden);

    MvuGetStreamInputInfoExResponse back{};
    span_load(back, make_const_span(bytes));
    EXPECT_EQ(back, info);
    EXPECT_EQ(back.pbsta(), 2);
    EXPECT_EQ(back.acmpsta(), 7);
}

TEST(aecp_mvu, the_two_statuses_of_a_stream_input_do_not_spill_into_each_other)
{
    MvuGetStreamInputInfoExResponse info{};
    info.set_status(0xFF, 0);
    EXPECT_EQ(info.pbsta(), 7);
    EXPECT_EQ(info.acmpsta(), 0);
    info.set_status(0, 0xFF);
    EXPECT_EQ(info.pbsta(), 0);
    EXPECT_EQ(info.acmpsta(), 31);
}

TEST(aecp_mvu, a_media_clock_reference_info_is_laid_out_as_5_4_4_4_has_it)
{
    MvuMediaClockReferenceInfoPayload info{};
    info.clock_domain_index = 0x0102;
    info.flags = MVU_MCR_FLAGS_SUPPORTED;
    info.default_mcr_prio = MVU_MCR_PRIO_AUDIO_INTERFACE;
    info.user_mcr_prio = 0x40;
    info.media_clock_domain_name = statusbar::atdecc::aem::AtdeccString{MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME};
    std::array<uint8_t, MvuMediaClockReferenceInfoPayload::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, info);

    std::array<uint8_t, 17> const golden{
        0x01, 0x02, 0x03, 0x00, 0xC0, 0x40, 0x00, 0x00, 0x00, 0x00, 'D', 'E', 'F', 'A', 'U', 'L', 'T'};
    EXPECT_TRUE(std::ranges::equal(std::span{bytes}.first(golden.size()), golden));
    EXPECT_TRUE(std::ranges::all_of(std::span{bytes}.subspan(golden.size()), [](uint8_t const b) { return b == 0; }));

    MvuMediaClockReferenceInfoPayload back{};
    span_load(back, make_const_span(bytes));
    EXPECT_EQ(back.user_mcr_prio.get(), 0x40);
    EXPECT_EQ(back.media_clock_domain_name.as_string_view(), MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME);

    MvuGetMediaClockReferenceInfoCommand get{};
    get.clock_domain_index = 0x0102;
    std::array<uint8_t, MvuGetMediaClockReferenceInfoCommand::LENGTH> get_bytes{};
    span_store(std::span<uint8_t>{get_bytes}, get);
    EXPECT_TRUE((get_bytes == std::array<uint8_t, 2>{0x01, 0x02}));
}

TEST(aecp_mvu, a_system_unique_id_is_laid_out_as_figure_5_7_has_it)
{
    MvuSystemUniqueIdPayload id{};
    id.system_unique_id = uint64_t{0x0102'0304'0506'0708};
    id.system_name = statusbar::atdecc::aem::AtdeccString{"Stage"};
    std::array<uint8_t, MvuSystemUniqueIdPayload::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, id);

    std::array<uint8_t, 15> const golden{0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 'S', 't', 'a', 'g', 'e'};
    EXPECT_TRUE(std::ranges::equal(std::span{bytes}.first(golden.size()), golden));
    EXPECT_TRUE(std::ranges::all_of(std::span{bytes}.subspan(golden.size()), [](uint8_t const b) { return b == 0; }));

    MvuSystemUniqueIdPayload back{};
    span_load(back, make_const_span(bytes));
    EXPECT_EQ(back.system_unique_id.get(), uint64_t{0x0102'0304'0506'0708});
    EXPECT_EQ(back.system_name.as_string_view(), std::string_view{"Stage"});
    EXPECT_EQ(MVU_GET_SYSTEM_UNIQUE_ID_COMMAND_LENGTH, size_t{2});
}

TEST_MAIN(statusbar_atdecc, atdecc_aecp_mvu_test)
