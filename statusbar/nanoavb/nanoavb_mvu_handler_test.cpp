// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the AECP Milan Vendor Unique handler (Milan v1.2 Section
// 5.4.3-5.4.4)

#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_entity_profile.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::mvu;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

Eui64 const US{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x03};
Eui64 const CONTROLLER{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
Eui48 const SRC{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

/// An MVU command AVTPDU: the 30-byte MVU header with @p command_type and
/// @p data as command_specific_data, then @p padding zero octets that
/// control_data_length does not count (an Ethernet-padded frame).
auto mvu_command(uint16_t const command_type, std::span<uint8_t const> const data, size_t const padding = 0) -> std::vector<uint8_t>
{
    MvuDu du{};
    du.init_command(command_type, static_cast<uint16_t>(data.size()));
    du.common.target_entity_id = US;
    du.common.controller_entity_id = CONTROLLER;
    du.common.sequence_id = 0x1234;
    std::vector<uint8_t> out(MvuDu::LENGTH + data.size() + padding, 0);
    span_store(std::span<uint8_t>{out}.first(MvuDu::LENGTH), du);
    std::ranges::copy(data, out.begin() + MvuDu::LENGTH);
    return out;
}

auto get_milan_info_command(size_t const padding = 0) -> std::vector<uint8_t>
{
    std::array<uint8_t, MvuGetMilanInfoCommand::LENGTH> const reserved{};
    return mvu_command(MVU_COMMAND_GET_MILAN_INFO, reserved, padding);
}

/// A handler whose responses land in @p sent
auto make_handler(std::vector<std::vector<uint8_t>>& sent, bool const milan = true) -> MvuCommandHandler
{
    MvuCommandHandler handler{};
    handler.set_profile(EntityProfile{.milan = milan});
    handler.set_send_response([&sent](Eui48 const&, std::span<uint8_t const> const resp) {
        sent.emplace_back(resp.begin(), resp.end());
        return true;
    });
    return handler;
}

auto reply_header(std::vector<uint8_t> const& reply) -> MvuDu
{
    MvuDu du{};
    span_load(du, std::span<uint8_t const>{reply}.first(MvuDu::LENGTH));
    return du;
}

/// The response is a VENDOR_UNIQUE_RESPONSE with the MVU protocol_id, r = 0,
/// @p status, the command's identity, and @p command_type
void expect_mvu_response(std::vector<uint8_t> const& reply, uint16_t const command_type, uint8_t const status)
{
    EXPECT_TRUE(reply.size() >= MvuDu::LENGTH);
    auto const du = reply_header(reply);
    EXPECT_EQ(du.common.subtype.get(), AvtpSubtype::aecp);
    EXPECT_EQ(du.common.message_type(), AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_EQ(du.common.status(), status);
    EXPECT_TRUE(du.common.target_entity_id == US);
    EXPECT_TRUE(du.common.controller_entity_id == CONTROLLER);
    EXPECT_EQ(du.common.sequence_id.get(), uint16_t{0x1234});
    EXPECT_TRUE(du.is_mvu_protocol());
    EXPECT_EQ(du.r_command_type.get(), command_type);
}

}  // namespace

// GET_MILAN_INFO (Milan v1.2 Section 5.4.4.1, Figure 5.4), padded to the Ethernet minimum;
// control_data_length counts no padding.
TEST(nanoavb_mvu, get_milan_info_reply_bytes)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    // A command as received: Ethernet-padded beyond its control_data_length
    EXPECT_TRUE(handler.process_packet(SRC, get_milan_info_command(14), US));
    EXPECT_EQ(sent.size(), size_t{1});
    auto const& reply = sent[0];
    expect_mvu_response(reply, MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(reply).common.control_data_length(), uint16_t{32});
    EXPECT_EQ(reply.size(), MVU_MIN_PDU_LENGTH);
    EXPECT_EQ(MVU_MIN_PDU_LENGTH, size_t{46});  // 64-octet frame less header and FCS
    std::array<uint8_t, 14> const expected{
        0x00,
        0x00,  // reserved
        0x00,
        0x00,
        0x00,
        0x01,  // protocol_version
        0x00,
        0x00,
        0x00,
        0x00,  // features_flags
        0x00,
        0x00,
        0x00,
        0x00,  // certification_version
    };
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), reply.begin() + MvuDu::LENGTH));
    // The padding is zero
    EXPECT_TRUE(std::all_of(reply.begin() + MvuDu::LENGTH + 14, reply.end(), [](uint8_t const b) { return b == 0; }));

    MvuGetMilanInfoResponse info{};
    span_load(info, std::span<uint8_t const>{reply}.subspan(MvuDu::LENGTH, MvuGetMilanInfoResponse::LENGTH));
    EXPECT_EQ(info.protocol_version.get(), MVU_PROTOCOL_VERSION);
    EXPECT_EQ(info.features_flags.get() & MVU_FEATURES_REDUNDANCY, uint32_t{0});
    EXPECT_EQ(info.features_flags.get() & MVU_FEATURES_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING, uint32_t{0});
}

TEST(nanoavb_mvu, get_milan_info_certification_version)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    handler.set_certification_version(milan_certification_version(1, 2, 0, 0));
    EXPECT_TRUE(handler.process_packet(SRC, get_milan_info_command(), US));
    EXPECT_EQ(sent.size(), size_t{1});
    std::array<uint8_t, 4> const expected{0x01, 0x02, 0x00, 0x00};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), sent[0].begin() + 40));
}

// A command whose r bit is set is still answered, with r = 0
TEST(nanoavb_mvu, response_clears_r)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    auto cmd = get_milan_info_command();
    cmd[28] |= 0x80;
    EXPECT_TRUE(handler.process_packet(SRC, cmd, US));
    auto cmd_unknown = mvu_command(0x0100, {});
    cmd_unknown[28] |= 0x80;
    EXPECT_TRUE(handler.process_packet(SRC, cmd_unknown, US));
    EXPECT_EQ(sent.size(), size_t{2});
    expect_mvu_response(sent[0], MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);
    expect_mvu_response(sent[1], 0x0100, MVU_STATUS_NOT_IMPLEMENTED);
}

// The classic profile must not claim Milan: GET_MILAN_INFO gets no reply at
// all (Section 5.4.4.1 NOTE), the other MVU commands NOT_IMPLEMENTED.
TEST(nanoavb_mvu, classic_profile)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent, false);
    EXPECT_FALSE(handler.process_packet(SRC, get_milan_info_command(14), US));
    EXPECT_TRUE(sent.empty());

    std::array<uint8_t, MvuGetMilanInfoCommand::LENGTH> const reserved{};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, reserved, 14), US));
    EXPECT_EQ(sent.size(), size_t{1});
    expect_mvu_response(sent[0], MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, MVU_STATUS_NOT_IMPLEMENTED);
    // The command echoed (cdl 20: common 10 + protocol_id 6 + command_type 2 +
    // reserved 2), padded; the received padding is not echoed as data
    EXPECT_EQ(reply_header(sent[0]).common.control_data_length(), uint16_t{20});
    EXPECT_EQ(sent[0].size(), MVU_MIN_PDU_LENGTH);
}

// SET/GET_SYSTEM_UNIQUE_ID and SET/GET_MEDIA_CLOCK_REFERENCE_INFO wait for the
// persistence store: NOT_IMPLEMENTED, the command echoed
TEST(nanoavb_mvu, deferred_commands_not_implemented)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    std::array<uint8_t, MvuSystemUniqueIdPayload::LENGTH> const unique_id{0x00, 0x00, 0x12, 0x34, 0x56, 0x78};
    std::array<uint8_t, MvuGetMilanInfoCommand::LENGTH> const reserved{};
    std::array<uint8_t, MvuMediaClockReferenceInfoPayload::LENGTH> mcr{};
    mcr[2] = MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID;
    std::array<uint8_t, MvuGetMediaClockReferenceInfoCommand::LENGTH> const domain{};

    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, unique_id), US));
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, reserved), US));
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, mcr), US));
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, domain), US));
    EXPECT_EQ(sent.size(), size_t{4});
    expect_mvu_response(sent[0], MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(reply_header(sent[0]).common.control_data_length(), uint16_t{MvuDu::BASE_CONTROL_DATA_LENGTH + 6});
    EXPECT_TRUE(std::equal(unique_id.begin(), unique_id.end(), sent[0].begin() + MvuDu::LENGTH));
    expect_mvu_response(sent[1], MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, MVU_STATUS_NOT_IMPLEMENTED);
    expect_mvu_response(sent[2], MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_NOT_IMPLEMENTED);
    // Longer than the minimum: echoed whole, no padding
    EXPECT_EQ(reply_header(sent[2]).common.control_data_length(), uint16_t{MvuDu::BASE_CONTROL_DATA_LENGTH + 74});
    EXPECT_EQ(sent[2].size(), MvuDu::LENGTH + MvuMediaClockReferenceInfoPayload::LENGTH);
    expect_mvu_response(sent[3], MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_NOT_IMPLEMENTED);
}

// A reserved command type (v1.2 Table 5.18: 0x0005-0x7FFF, which v1.3 uses
// for BIND_STREAM and others) is NOT_IMPLEMENTED
TEST(nanoavb_mvu, unknown_command_type_not_implemented)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    std::array<uint8_t, 20> const bind{};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(0x0005, bind), US));
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(0x7FFF, {}), US));
    EXPECT_EQ(sent.size(), size_t{2});
    expect_mvu_response(sent[0], 0x0005, MVU_STATUS_NOT_IMPLEMENTED);
    expect_mvu_response(sent[1], 0x7FFF, MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(reply_header(sent[1]).common.control_data_length(), MvuDu::BASE_CONTROL_DATA_LENGTH);
    EXPECT_EQ(sent[1].size(), MVU_MIN_PDU_LENGTH);
}

// A GET_MILAN_INFO without its reserved field (cdl 18) is answered: the
// receiver ignores reserved. A frame shorter than its control_data_length, or
// declaring more than AECP allows, is dropped.
TEST(nanoavb_mvu, short_payload)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    // Declared and received without the reserved field (Ethernet padding
    // present, but not declared)
    auto const no_reserved = mvu_command(MVU_COMMAND_GET_MILAN_INFO, {}, 16);
    EXPECT_TRUE(handler.process_packet(SRC, no_reserved, US));
    EXPECT_EQ(sent.size(), size_t{1});
    expect_mvu_response(sent[0], MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(sent[0]).common.control_data_length(), uint16_t{32});
    EXPECT_EQ(sent[0][35], uint8_t{0x01});  // protocol_version

    // Declares the reserved field but the frame stops before it: malformed
    auto truncated = get_milan_info_command();
    truncated.resize(MvuDu::LENGTH + 1);
    EXPECT_FALSE(handler.process_packet(SRC, truncated, US));
    // Declares more than AECP allows (600 > 524), all of it received
    auto oversized = get_milan_info_command(600);
    oversized[2] = 0x02;
    oversized[3] = 0x58;
    EXPECT_FALSE(handler.process_packet(SRC, oversized, US));
    EXPECT_EQ(sent.size(), size_t{1});
}

// Commands for another entity, and responses, are not answered
TEST(nanoavb_mvu, ignores_other_targets_and_responses)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    EXPECT_FALSE(handler.process_packet(SRC, get_milan_info_command(), Eui64{0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09}));
    auto response = get_milan_info_command();
    response[1] = static_cast<uint8_t>((response[1] & 0xF0) | AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_FALSE(handler.process_packet(SRC, response, US));
    EXPECT_TRUE(sent.empty());
}

// dispatch_aecp: MVU commands reach the MVU handler; an unknown vendor unique
// protocol_id and AEM commands reach the AEM handler (NOT_IMPLEMENTED for the
// former, unchanged from before the MVU handler existed)
TEST(nanoavb_mvu, dispatch_aecp)
{
    DescriptorEntity entity{};
    entity.entity_id = US;
    entity.configurations_count = 1;
    auto model = EntityModelBuilder{}.entity(entity).configuration(DescriptorConfiguration{}).build();
    AemCommandHandler aem{model};
    std::vector<std::vector<uint8_t>> aem_sent;
    aem.set_send_response([&aem_sent](Eui48 const&, std::span<uint8_t const> const resp) {
        aem_sent.emplace_back(resp.begin(), resp.end());
        return true;
    });
    std::vector<std::vector<uint8_t>> mvu_sent;
    auto mvu = make_handler(mvu_sent);

    EXPECT_TRUE(dispatch_aecp(aem, mvu, SRC, get_milan_info_command(), US));
    EXPECT_EQ(mvu_sent.size(), size_t{1});
    EXPECT_TRUE(aem_sent.empty());
    expect_mvu_response(mvu_sent[0], MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);

    // Unknown protocol_id: the AEM handler's NOT_IMPLEMENTED echo
    auto other = get_milan_info_command();
    other[27] = 0x01;
    EXPECT_TRUE(dispatch_aecp(aem, mvu, SRC, other, US));
    EXPECT_EQ(mvu_sent.size(), size_t{1});
    EXPECT_EQ(aem_sent.size(), size_t{1});
    AecpDuCommon reply{};
    span_load(reply, std::span<uint8_t const>{aem_sent[0]}.first(AecpDuCommon::LENGTH));
    EXPECT_EQ(reply.message_type(), AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_EQ(reply.status(), AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aem_sent[0][27], uint8_t{0x01});

    // An AEM command still reaches the AEM handler
    AemDu read{};
    read.init_command(AEM_COMMAND_GET_CONFIGURATION, AemDu::AEM_DATA_LENGTH);
    read.target_entity_id = US;
    std::array<uint8_t, AemDu::LENGTH> bytes{};
    span_store(bytes, read);
    EXPECT_TRUE(dispatch_aecp(aem, mvu, SRC, bytes, US));
    EXPECT_EQ(aem_sent.size(), size_t{2});
    EXPECT_EQ(mvu_sent.size(), size_t{1});
}

TEST_MAIN(statusbar_nanoavb, nanoavb_mvu_handler_test)
