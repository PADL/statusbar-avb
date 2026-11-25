// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// An MVU command as a controller sends it and the reading of its answer
// (atdecc_mvu_request.hpp). No sockets, no time.

#include "statusbar/atdecc/atdecc_mvu_request.hpp"

#include "statusbar/atdecc/atdecc_acmp_types.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace statusbar::atdecc;
using statusbar::make_const_span;
using statusbar::span_load;
using statusbar::span_store;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

constexpr Eui64 TARGET{0x2C, 0xCF, 0x67, 0xFF, 0xFE, 0x00, 0x00, 0x0D};
constexpr Eui64 CONTROLLER{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01};
constexpr Eui64 TALKER{0x2C, 0xCF, 0x67, 0xFF, 0xFE, 0x00, 0x00, 0x0A};
constexpr MvuParties PARTIES{.target_entity_id = TARGET, .controller_entity_id = CONTROLLER, .sequence_id = 0x1234};

auto pdu_of(MvuRequest const& request) -> std::vector<uint8_t>
{
    std::array<uint8_t, MvuRequest::MAX_PDU> buffer{};
    auto const pdu = mvu_request_pdu(request, buffer);
    return {pdu.begin(), pdu.end()};
}

/// The response a target gives @p request: @p status and @p data behind the header.
template <typename Data>
auto response_to(MvuRequest const& request, uint8_t const status, Data const& data) -> std::vector<uint8_t>
{
    AecpMvuDu header{};
    header.init_response(request.command_type, status, static_cast<uint16_t>(Data::LENGTH));
    header.common.target_entity_id = request.parties.target_entity_id;
    header.common.controller_entity_id = request.parties.controller_entity_id;
    header.common.sequence_id = request.parties.sequence_id;
    std::vector<uint8_t> pdu(AecpMvuDu::LENGTH + Data::LENGTH);
    span_store(std::span<uint8_t>{pdu}.first(AecpMvuDu::LENGTH), header);
    span_store(std::span<uint8_t>{pdu}.subspan(AecpMvuDu::LENGTH), data);
    return pdu;
}

/// @p pdu with its MVU header changed by @p change.
template <typename Change>
auto with_header(std::vector<uint8_t> pdu, Change const& change) -> std::vector<uint8_t>
{
    AecpMvuDu header{};
    span_load(header, std::span<uint8_t const>{pdu}.first(AecpMvuDu::LENGTH));
    change(header);
    span_store(std::span<uint8_t>{pdu}.first(AecpMvuDu::LENGTH), header);
    return pdu;
}

auto settled_info() -> MvuGetStreamInputInfoExResponse
{
    MvuGetStreamInputInfoExResponse info{};
    info.descriptor_type = 0x0005;
    info.descriptor_index = 1;
    info.talker_entity_id = TALKER;
    info.talker_unique_id = 2;
    info.set_status(3, 0);
    return info;
}

}  // namespace

TEST(mvu_request, a_bind_stream_goes_out_as_figure_5_10_has_it)
{
    auto const request = mvu_bind_stream_request(
        PARTIES, MvuBindTarget{.stream_input = 1, .talker_entity_id = TALKER, .talker_stream_index = 2, .streaming_wait = true});
    auto const pdu = pdu_of(request);
    std::array<uint8_t, 48> const golden{0xFB, 0x06, 0x00, 0x24,  // AECP, VENDOR_UNIQUE_COMMAND, SUCCESS, control_data_length 36
                                         0x2C, 0xCF, 0x67, 0xFF, 0xFE, 0x00, 0x00, 0x0D,  // target_entity_id
                                         0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01,  // controller_entity_id
                                         0x12, 0x34,                                      // sequence_id
                                         0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00,              // protocol_id
                                         0x00, 0x05,                                      // u clear, BIND_STREAM
                                         0x00, 0x01,                                      // flags: STREAMING_WAIT
                                         0x00, 0x05, 0x00, 0x01,                          // STREAM_INPUT 1
                                         0x2C, 0xCF, 0x67, 0xFF, 0xFE, 0x00, 0x00, 0x0A,  // talker_entity_id
                                         0x00, 0x02, 0x00, 0x00};                         // talker_stream_index, reserved
    EXPECT_EQ(pdu.size(), golden.size());
    EXPECT_TRUE(std::ranges::equal(pdu, golden));
}

TEST(mvu_request, the_other_three_commands_carry_their_own_data)
{
    auto const info = pdu_of(mvu_get_milan_info_request(PARTIES));
    EXPECT_EQ(info.size(), size_t{32});  // two reserved octets
    EXPECT_EQ(info[3], 20);
    EXPECT_EQ(info[29], MVU_COMMAND_GET_MILAN_INFO);
    EXPECT_EQ(info[30], 0);
    EXPECT_EQ(info[31], 0);

    std::array<uint8_t, 6> const stream_input_3{0x00, 0x00, 0x00, 0x05, 0x00, 0x03};
    auto const unbind = pdu_of(mvu_unbind_stream_request(PARTIES, 3));
    EXPECT_EQ(unbind.size(), size_t{36});
    EXPECT_EQ(unbind[29], MVU_COMMAND_UNBIND_STREAM);
    EXPECT_TRUE(std::ranges::equal(std::span{unbind}.subspan(30), stream_input_3));

    auto const query = pdu_of(mvu_get_stream_input_info_ex_request(PARTIES, 3));
    EXPECT_EQ(query[29], MVU_COMMAND_GET_STREAM_INPUT_INFO_EX);
    EXPECT_TRUE(std::ranges::equal(std::span{query}.subspan(30), stream_input_3));

    // A bind that does not ask for STREAMING_WAIT has no flag set.
    auto const started = pdu_of(mvu_bind_stream_request(PARTIES, MvuBindTarget{.talker_entity_id = TALKER}));
    EXPECT_EQ(started[30], 0);
    EXPECT_EQ(started[31], 0);
}

TEST(mvu_request, a_buffer_too_short_for_the_pdu_gets_nothing)
{
    auto const request = mvu_unbind_stream_request(PARTIES, 0);
    std::array<uint8_t, 35> small{};
    EXPECT_TRUE(mvu_request_pdu(request, small).empty());
    std::array<uint8_t, 36> exact{};
    EXPECT_EQ(mvu_request_pdu(request, exact).size(), size_t{36});
}

TEST(mvu_request, the_answer_is_the_targets_response_with_our_sequence_and_command)
{
    auto const request = mvu_get_stream_input_info_ex_request(PARTIES, 1);
    auto const pdu = response_to(request, MVU_STATUS_SUCCESS, settled_info());
    auto const answer = mvu_answer_to(request, pdu);
    EXPECT_TRUE(answer.has_value());
    if (!answer) {
        return;
    }
    EXPECT_EQ(answer->status, MVU_STATUS_SUCCESS);
    EXPECT_EQ(answer->data.size(), MvuGetStreamInputInfoExResponse::LENGTH);
    MvuGetStreamInputInfoExResponse info{};
    span_load(info, answer->data);
    EXPECT_EQ(info, settled_info());

    auto const refused = response_to(request, MVU_STATUS_NO_SUCH_DESCRIPTOR, settled_info());
    EXPECT_EQ(mvu_answer_to(request, refused)->status, MVU_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(mvu_request, nothing_else_on_the_wire_is_taken_for_the_answer)
{
    auto const request = mvu_get_stream_input_info_ex_request(PARTIES, 1);
    auto const good = response_to(request, MVU_STATUS_SUCCESS, settled_info());
    EXPECT_TRUE(mvu_answer_to(request, good).has_value());

    // Another exchange's response, or one for another controller or from another entity.
    EXPECT_FALSE(mvu_answer_to(request, with_header(good, [](AecpMvuDu& h) { h.common.sequence_id = 0x1235; })).has_value());
    EXPECT_FALSE(mvu_answer_to(request, with_header(good, [](AecpMvuDu& h) { h.common.controller_entity_id = TALKER; })));
    EXPECT_FALSE(mvu_answer_to(request, with_header(good, [](AecpMvuDu& h) { h.common.target_entity_id = TALKER; })));
    // The response to another command, a notification of the same one (5.4.3.2.2), another vendor's protocol.
    EXPECT_FALSE(mvu_answer_to(request, with_header(good, [](AecpMvuDu& h) { h.set_command_type(MVU_COMMAND_UNBIND_STREAM); })));
    EXPECT_FALSE(mvu_answer_to(
        request, with_header(good, [](AecpMvuDu& h) { h.set_command_type(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, true); })));
    EXPECT_FALSE(
        mvu_answer_to(request, with_header(good, [](AecpMvuDu& h) { h.protocol_id = Eui48{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00}; })));
    // Our own command, as another station on the segment hears it, and a frame cut short.
    EXPECT_FALSE(mvu_answer_to(request, pdu_of(request)).has_value());
    EXPECT_FALSE(mvu_answer_to(request, std::span<uint8_t const>{good}.first(AecpMvuDu::LENGTH - 1)).has_value());
}

TEST(mvu_request, the_answers_data_is_what_control_data_length_covers)
{
    // An UNBIND_STREAM response is 36 octets and arrives in a 46-octet
    // payload: the padding is not data.
    auto const request = mvu_unbind_stream_request(PARTIES, 1);
    MvuStreamInput input{};
    input.descriptor_type = 0x0005;
    input.descriptor_index = 1;
    auto padded = response_to(request, MVU_STATUS_SUCCESS, input);
    padded.resize(46, 0xEE);
    EXPECT_EQ(mvu_answer_to(request, padded)->data.size(), MvuStreamInput::LENGTH);

    // A length that claims more than arrived gives what arrived.
    auto const claimed = with_header(response_to(request, MVU_STATUS_SUCCESS, input), [](AecpMvuDu& h) {
        h.init_response(MVU_COMMAND_UNBIND_STREAM, MVU_STATUS_SUCCESS, 200);
        h.common.target_entity_id = TARGET;
        h.common.controller_entity_id = CONTROLLER;
        h.common.sequence_id = 0x1234;
    });
    EXPECT_EQ(mvu_answer_to(request, claimed)->data.size(), MvuStreamInput::LENGTH);
}

TEST(mvu_request, an_answer_reads_as_one_line_with_every_field_named)
{
    auto const info = settled_info();
    MvuAnswer const settled{.status = MVU_STATUS_SUCCESS, .data = make_const_span(info)};
    EXPECT_EQ(
        mvu_answer_text(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, settled),
        std::string{"GET_STREAM_INPUT_INFO_EX SUCCESS: descriptor_type=5 stream_input=1 talker=2c:cf:67:ff:fe:00:00:0a "
                    "talker_unique_id=2 pbsta=3 (PROBING_COMPLETED) acmpsta=0 (Success)"});

    MvuGetMilanInfoResponse milan{};
    milan.features_flags = MVU_FEATURE_MVU_BINDING | MVU_FEATURE_REDUNDANCY;
    milan.specification_version = mvu_version(1, 3);
    MvuAnswer const milan_info{.status = MVU_STATUS_SUCCESS, .data = make_const_span(milan)};
    EXPECT_EQ(
        mvu_answer_text(MVU_COMMAND_GET_MILAN_INFO, milan_info),
        std::string{"GET_MILAN_INFO SUCCESS: protocol_version=1 features_flags=0x00000005 (REDUNDANCY MVU_BINDING) "
                    "certification_version=0.0.0.0 specification_version=1.3.0.0"});

    MvuBindStream bind{};
    bind.flags = MVU_BIND_STREAM_FLAG_STREAMING_WAIT;
    bind.descriptor_type = 0x0005;
    bind.descriptor_index = 1;
    bind.talker_entity_id = TALKER;
    bind.talker_stream_index = 2;
    MvuAnswer const locked{.status = MVU_STATUS_ENTITY_LOCKED, .data = make_const_span(bind)};
    EXPECT_EQ(
        mvu_answer_text(MVU_COMMAND_BIND_STREAM, locked),
        std::string{"BIND_STREAM ENTITY_LOCKED: descriptor_type=5 stream_input=1 talker=2c:cf:67:ff:fe:00:00:0a "
                    "talker_stream=2 streaming_wait=1"});

    MvuStreamInput input{};
    input.descriptor_type = 0x0005;
    input.descriptor_index = 7;
    MvuAnswer const unbound{.status = MVU_STATUS_NO_SUCH_DESCRIPTOR, .data = make_const_span(input)};
    EXPECT_EQ(
        mvu_answer_text(MVU_COMMAND_UNBIND_STREAM, unbound),
        std::string{"UNBIND_STREAM NO_SUCH_DESCRIPTOR: descriptor_type=5 stream_input=7"});
}

TEST(mvu_request, data_shorter_than_the_format_is_counted_and_not_read)
{
    // NOT_IMPLEMENTED gives the command's six octets back where the response has eighteen.
    std::array<uint8_t, 6> const echoed{0x00, 0x00, 0x00, 0x05, 0x00, 0x01};
    MvuAnswer const not_implemented{.status = MVU_STATUS_NOT_IMPLEMENTED, .data = echoed};
    EXPECT_EQ(
        mvu_answer_text(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, not_implemented),
        std::string{"GET_STREAM_INPUT_INFO_EX NOT_IMPLEMENTED: 6 octets of data"});
    EXPECT_EQ(
        mvu_answer_text(MVU_COMMAND_GET_MILAN_INFO, MvuAnswer{.status = MVU_STATUS_PAYLOAD_TOO_SHORT}),
        std::string{"GET_MILAN_INFO PAYLOAD_TOO_SHORT: 0 octets of data"});
    EXPECT_EQ(
        mvu_answer_text(0x0008, MvuAnswer{.status = 1, .data = echoed}), std::string{"Unknown NOT_IMPLEMENTED: 6 octets of data"});
}

TEST(mvu_request, a_probing_status_has_the_name_table_5_5_gives_it)
{
    EXPECT_EQ(mvu_probing_status_name(0), std::string_view{"PROBING_DISABLED"});
    EXPECT_EQ(mvu_probing_status_name(1), std::string_view{"PROBING_PASSIVE"});
    EXPECT_EQ(mvu_probing_status_name(2), std::string_view{"PROBING_ACTIVE"});
    EXPECT_EQ(mvu_probing_status_name(3), std::string_view{"PROBING_COMPLETED"});
    EXPECT_EQ(mvu_probing_status_name(4), std::string_view{"Unknown"});
}

TEST_MAIN(statusbar_atdecc, atdecc_mvu_request_test)
