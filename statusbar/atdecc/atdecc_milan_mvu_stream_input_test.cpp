// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Milan v1.3's stream input commands over AECP MVU, in the sink's terms
// (atdecc_milan_mvu_stream_input.hpp): Table 5.22, the statuses of Tables
// 5.28, 5.33, 5.35, 5.39 and 5.41, and the fields of 5.4.4.8.

#include "statusbar/atdecc/atdecc_milan_mvu_stream_input.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_milan_acmp_listener_sm.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>

using namespace statusbar::atdecc;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
using statusbar::ieee::Eui64;

namespace {

constexpr Eui64 LISTENER{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
constexpr Eui64 TALKER{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
constexpr Eui64 CONTROLLER{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

auto header(uint16_t const command, uint16_t const data_length) -> AecpMvuDu
{
    AecpMvuDu du{};
    du.init_command(command, data_length);
    du.common.target_entity_id = LISTENER;
    du.common.controller_entity_id = CONTROLLER;
    du.common.sequence_id = 77;
    return du;
}

auto bind_data(uint16_t const flags) -> MvuBindStream
{
    MvuBindStream bind{};
    bind.flags = flags;
    bind.descriptor_type = DESCRIPTOR_STREAM_INPUT;
    bind.descriptor_index = 2;
    bind.talker_entity_id = TALKER;
    bind.talker_stream_index = 5;
    return bind;
}

}  // namespace

TEST(milan_mvu_stream_input, a_bind_stream_is_the_bind_rx_command_of_table_5_22)
{
    auto const cmd = milan_bind_command_from_mvu(header(MVU_COMMAND_BIND_STREAM, MvuBindStream::LENGTH), bind_data(0));
    EXPECT_EQ(cmd.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(cmd.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(cmd.controller_entity_id, CONTROLLER);
    EXPECT_EQ(cmd.listener_entity_id, LISTENER);  // target_entity_id
    EXPECT_EQ(cmd.listener_unique_id.get(), 2);   // descriptor_index
    EXPECT_EQ(cmd.talker_entity_id, TALKER);
    EXPECT_EQ(cmd.talker_unique_id.get(), 5);  // talker_stream_index
    EXPECT_EQ(cmd.sequence_id.get(), 77);
    EXPECT_EQ(cmd.flags.get(), 0);
    EXPECT_EQ(cmd.stream_id, Eui64{});
}

TEST(milan_mvu_stream_input, streaming_wait_is_the_one_flag_a_bind_stream_carries_over)
{
    // Table 5.19 has one flag. Bits it reserves are not ACMP flags of the same
    // position: 0x0002 would be FAST_CONNECT.
    auto const head = header(MVU_COMMAND_BIND_STREAM, MvuBindStream::LENGTH);
    EXPECT_EQ(
        milan_bind_command_from_mvu(head, bind_data(MVU_BIND_STREAM_FLAG_STREAMING_WAIT)).flags.get(), acmp_flags::STREAMING_WAIT);
    EXPECT_EQ(milan_bind_command_from_mvu(head, bind_data(0xFFFE)).flags.get(), 0);
    EXPECT_EQ(milan_bind_command_from_mvu(head, bind_data(0xFFFF)).flags.get(), acmp_flags::STREAMING_WAIT);
}

TEST(milan_mvu_stream_input, an_unbind_stream_is_the_unbind_rx_command)
{
    MvuStreamInput unbind{};
    unbind.descriptor_type = DESCRIPTOR_STREAM_INPUT;
    unbind.descriptor_index = 1;
    auto const cmd = milan_unbind_command_from_mvu(header(MVU_COMMAND_UNBIND_STREAM, MvuStreamInput::LENGTH), unbind);
    EXPECT_EQ(cmd.message_type(), ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    EXPECT_EQ(cmd.controller_entity_id, CONTROLLER);
    EXPECT_EQ(cmd.listener_entity_id, LISTENER);
    EXPECT_EQ(cmd.listener_unique_id.get(), 1);
    EXPECT_EQ(cmd.talker_entity_id, Eui64{});
    EXPECT_EQ(cmd.sequence_id.get(), 77);
}

TEST(milan_mvu_stream_input, the_sinks_acmp_answer_names_the_mvu_status)
{
    EXPECT_EQ(mvu_status_for_binding_response(ACMP_STATUS_SUCCESS), MVU_STATUS_SUCCESS);                          // 5.35, 5.41
    EXPECT_EQ(mvu_status_for_binding_response(ACMP_STATUS_LISTENER_UNKNOWN_ID), MVU_STATUS_NO_SUCH_DESCRIPTOR);   // 5.28
    EXPECT_EQ(mvu_status_for_binding_response(ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED), MVU_STATUS_ENTITY_LOCKED);  // 5.33, 5.39
    // The sink gives no other; one that came would not be passed off as success.
    EXPECT_EQ(mvu_status_for_binding_response(ACMP_STATUS_LISTENER_MISBEHAVING), MVU_STATUS_ENTITY_MISBEHAVING);
    EXPECT_EQ(mvu_status_for_binding_response(31), MVU_STATUS_ENTITY_MISBEHAVING);
}

TEST(milan_mvu_stream_input, an_unbound_sink_reports_no_talker_and_probing_disabled)
{
    MilanSinkContext ctx{};
    ctx.index = 3;
    auto const info = milan_stream_input_info_ex(ctx);
    EXPECT_EQ(info.descriptor_type.get(), DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(info.descriptor_index.get(), 3);
    EXPECT_EQ(info.talker_entity_id, Eui64{});
    EXPECT_EQ(info.talker_unique_id.get(), 0);
    EXPECT_EQ(info.pbsta(), 0);
    EXPECT_EQ(info.acmpsta(), 0);
    EXPECT_EQ(info.reserved.get(), 0);
    EXPECT_EQ(info.reserved_end.get(), 0);
}

TEST(milan_mvu_stream_input, a_bound_sink_reports_its_talker_and_how_far_probing_has_come)
{
    MilanSinkContext ctx{};
    ctx.index = 1;
    ctx.binding = MilanBinding{
        .listener_unique_id = 1, .talker_entity_id = TALKER, .talker_unique_id = 5, .controller_entity_id = CONTROLLER};
    ctx.probing_status = MilanProbingStatus::Active;
    ctx.acmp_status = ACMP_STATUS_LISTENER_TALKER_TIMEOUT;
    auto const probing = milan_stream_input_info_ex(ctx);
    EXPECT_EQ(probing.talker_entity_id, TALKER);
    EXPECT_EQ(probing.talker_unique_id.get(), 5);
    EXPECT_EQ(probing.pbsta(), 2);
    EXPECT_EQ(probing.acmpsta(), ACMP_STATUS_LISTENER_TALKER_TIMEOUT);

    ctx.probing_status = MilanProbingStatus::Completed;
    ctx.acmp_status = ACMP_STATUS_SUCCESS;
    auto const settled = milan_stream_input_info_ex(ctx);
    EXPECT_EQ(settled.pbsta(), 3);
    EXPECT_EQ(settled.acmpsta(), 0);
}

TEST(milan_mvu_stream_input, what_is_notified_is_the_response_and_a_settled_sinks_registration)
{
    // Table 5.21
    MilanListenerEnv env{};
    MilanTalkerRegistration registers{MilanTalkerRegistration::Advertise};
    MilanSrpParameters asked{};
    env.talker_registration = [&](MilanSrpParameters const& srp) {
        asked = srp;
        return registers;
    };
    MilanSinkContext ctx{};
    ctx.env = &env;
    ctx.index = 1;
    ctx.binding = MilanBinding{
        .listener_unique_id = 1, .talker_entity_id = TALKER, .talker_unique_id = 5, .controller_entity_id = CONTROLLER};
    ctx.probing_status = MilanProbingStatus::Active;

    // Not settled: no SRP parameters to match, so SRP is not asked.
    auto const probing = milan_stream_input_notified(ctx);
    EXPECT_TRUE(probing.info == milan_stream_input_info_ex(ctx));
    EXPECT_TRUE(probing.talker_registration == MilanTalkerRegistration::None);
    EXPECT_TRUE(asked == MilanSrpParameters{});

    // Settled: asked for the parameters of the last probe's answer.
    ctx.probing_status = MilanProbingStatus::Completed;
    ctx.srp = MilanSrpParameters{.stream_id = TALKER, .stream_dest_mac = {0x91, 0xE0, 0xF0, 0, 0, 7}, .stream_vlan_id = 2};
    ctx.srp_active = true;
    auto const settled = milan_stream_input_notified(ctx);
    EXPECT_TRUE(settled.talker_registration == MilanTalkerRegistration::Advertise);
    EXPECT_TRUE(asked == ctx.srp);
    EXPECT_FALSE(settled == probing);

    // The registration alone makes it another: the response is the same.
    registers = MilanTalkerRegistration::Failed;
    auto const failed = milan_stream_input_notified(ctx);
    EXPECT_TRUE(failed.info == settled.info);
    EXPECT_FALSE(failed == settled);

    // A listener that cannot say what SRP registers reads as nothing registered.
    env.talker_registration = nullptr;
    EXPECT_TRUE(milan_stream_input_notified(ctx).talker_registration == MilanTalkerRegistration::None);
    ctx.env = nullptr;
    EXPECT_TRUE(milan_stream_input_notified(ctx).talker_registration == MilanTalkerRegistration::None);
}

TEST_MAIN(statusbar_atdecc, atdecc_milan_mvu_stream_input_test)
