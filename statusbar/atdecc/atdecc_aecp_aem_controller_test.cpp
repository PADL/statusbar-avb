// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for AEM Controller State Machine

#include "statusbar/atdecc/atdecc_aecp_aem_controller.hpp"

#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::ieee;

namespace {

Eui64 const CONTROLLER_ID{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01};
Eui64 const TARGET_ID{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

auto test_time(int ms) -> sm::TimePoint
{
    return sm::TimePoint{} + std::chrono::milliseconds(ms);
}

}  // namespace

TEST(aem_controller_sm, initial_state)
{
    AemControllerStateMachine<> sm;
    EXPECT_EQ(sm.current_state(), AemControllerState::Start);
}

TEST(aem_controller_sm, send_command)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;

    std::vector<std::vector<uint8_t>> sent;
    ctx.tx_command = [&](std::span<uint8_t const> packet) {
        sent.emplace_back(packet.begin(), packet.end());
        return true;
    };

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    // Send READ_DESCRIPTOR command
    std::array<uint8_t, 4> payload = {0x00, 0x00, 0x00, 0x00};  // descriptor_type + descriptor_index
    ctx.command_params = {
        .target_entity_id = TARGET_ID,
        .command_code = AEM_COMMAND_READ_DESCRIPTOR,
        .command_data = payload,
    };

    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));

    EXPECT_EQ(sent.size(), 1U);
    EXPECT_EQ(ctx.inflight_count(), 1U);

    // Parse sent header
    AemDu sent_header{};
    span_load(sent_header, make_const_span(sent[0]));
    EXPECT_TRUE(sent_header.target_entity_id == TARGET_ID);
    EXPECT_TRUE(sent_header.controller_entity_id == CONTROLLER_ID);
    EXPECT_EQ(sent_header.command_code(), AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_TRUE(sent_header.is_command());
}

TEST(aem_controller_sm, receive_success_response)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };

    uint8_t response_status = 0xFF;
    ctx.on_response = [&](AemDu const&, std::span<uint8_t const>, std::span<uint8_t const>, uint8_t status) {
        response_status = status;
    };

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    // Send command
    ctx.command_params = {.target_entity_id = TARGET_ID, .command_code = AEM_COMMAND_READ_DESCRIPTOR};
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));

    // Receive SUCCESS response
    AemDu resp_header{};
    resp_header.init_response(AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_SUCCESS, AemDu::AEM_DATA_LENGTH);
    resp_header.controller_entity_id = CONTROLLER_ID;
    resp_header.sequence_id = 0;

    auto idx = ctx.find_inflight(0);
    EXPECT_TRUE(idx < AemControllerContext::MAX_INFLIGHT);
    ctx.rcvd_header = resp_header;
    ctx.rcvd_response_data = {};
    ctx.current_inflight_index = idx;

    sm.handle_event(ctx, AemControllerEvent::RcvdResponse, test_time(10));

    EXPECT_EQ(response_status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(ctx.inflight_count(), 0U);
}

TEST(aem_controller_sm, in_progress_extends_deadline)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };

    int response_count = 0;
    ctx.on_response = [&](AemDu const&, std::span<uint8_t const>, std::span<uint8_t const>, uint8_t) { ++response_count; };

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    ctx.command_params = {.target_entity_id = TARGET_ID, .command_code = AEM_COMMAND_ACQUIRE_ENTITY};
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));

    // Receive IN_PROGRESS at 200ms (before 250ms timeout)
    AemDu in_progress{};
    in_progress.init_response(AEM_COMMAND_ACQUIRE_ENTITY, AEM_STATUS_IN_PROGRESS, AemDu::AEM_DATA_LENGTH);
    in_progress.controller_entity_id = CONTROLLER_ID;
    in_progress.sequence_id = 0;

    auto idx = ctx.find_inflight(0);
    ctx.rcvd_header = in_progress;
    ctx.rcvd_response_data = {};
    ctx.current_inflight_index = idx;
    sm.handle_event(ctx, AemControllerEvent::RcvdResponse, test_time(200));

    // on_response should NOT have been called (IN_PROGRESS is not final)
    EXPECT_EQ(response_count, 0);
    EXPECT_EQ(ctx.inflight_count(), 1U);

    // Original timeout at 250ms should NOT fire (deadline was extended)
    EXPECT_TRUE(ctx.find_timed_out(test_time(260)) >= AemControllerContext::MAX_INFLIGHT);

    // New timeout at 200+250=450ms should fire
    EXPECT_TRUE(ctx.find_timed_out(test_time(460)) < AemControllerContext::MAX_INFLIGHT);
}

TEST(aem_controller_sm, timeout)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };

    bool timed_out = false;
    ctx.on_timeout = [&](AemDu const&) { timed_out = true; };

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    ctx.command_params = {.target_entity_id = TARGET_ID, .command_code = AEM_COMMAND_READ_DESCRIPTOR};
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));

    // Timeout at 260ms
    auto idx = ctx.find_timed_out(test_time(260));
    EXPECT_TRUE(idx < AemControllerContext::MAX_INFLIGHT);

    ctx.current_inflight_index = idx;
    sm.handle_event(ctx, AemControllerEvent::Timeout, test_time(260));

    EXPECT_TRUE(timed_out);
    EXPECT_EQ(ctx.inflight_count(), 0U);
}

TEST(aem_controller_sm, multiple_inflight)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    // Send 3 commands
    for (int i = 0; i < 3; ++i) {
        ctx.command_params = {
            .target_entity_id = TARGET_ID,
            .command_code = AEM_COMMAND_READ_DESCRIPTOR,
        };
        sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(i));
    }

    EXPECT_EQ(ctx.inflight_count(), 3U);
    EXPECT_EQ(ctx.next_sequence_id, 3U);
}

TEST(aem_controller_sm, unknown_sequence_ignored)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;

    auto idx = ctx.find_inflight(99);
    EXPECT_EQ(idx, AemControllerContext::MAX_INFLIGHT);
}

//
// Per-command completions — the typed correlation surface. A completion
// fires exactly once per tracked command: final response, timeout, or
// immediately on send failure. Callers never match sequence IDs.
//

TEST(aem_controller_completion, fires_once_on_final_response)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };

    int fired = 0;
    AemCommandDelivery delivery{};
    uint8_t status = 0xFF;
    uint16_t command_type = 0;
    Eui64 target{};
    std::vector<uint8_t> sent_copy;
    std::vector<uint8_t> response_copy;

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    std::array<uint8_t, 4> const payload = {0x00, 0x1B, 0x00, 0x02};
    ctx.command_params = {
        .target_entity_id = TARGET_ID,
        .command_code = AEM_COMMAND_GET_SIGNAL_SELECTOR,
        .command_data = payload,
        .completion =
            [&](AemCommandResult const& r) {
                ++fired;
                delivery = r.delivery;
                status = r.status;
                command_type = r.command_type;
                target = r.target_entity_id;
                sent_copy.assign(r.sent_payload.begin(), r.sent_payload.end());
                response_copy.assign(r.response.begin(), r.response.end());
            },
    };
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));
    EXPECT_EQ(fired, 0);  // nothing yet — command is inflight

    // An IN_PROGRESS response extends the deadline and must NOT complete.
    AemDu in_progress{};
    in_progress.init_response(AEM_COMMAND_GET_SIGNAL_SELECTOR, AEM_STATUS_IN_PROGRESS, AemDu::AEM_DATA_LENGTH);
    in_progress.controller_entity_id = CONTROLLER_ID;
    in_progress.sequence_id = 0;
    ctx.rcvd_header = in_progress;
    ctx.rcvd_response_data = {};
    ctx.current_inflight_index = ctx.find_inflight(0);
    sm.handle_event(ctx, AemControllerEvent::RcvdResponse, test_time(100));
    EXPECT_EQ(fired, 0);

    // The final response completes with status + payloads.
    AemDu resp{};
    resp.init_response(AEM_COMMAND_GET_SIGNAL_SELECTOR, AEM_STATUS_SUCCESS, AemDu::AEM_DATA_LENGTH);
    resp.controller_entity_id = CONTROLLER_ID;
    resp.sequence_id = 0;
    std::array<uint8_t, 6> const response_data = {0x00, 0x1B, 0x00, 0x02, 0x00, 0x14};
    ctx.rcvd_header = resp;
    ctx.rcvd_response_data = response_data;
    ctx.current_inflight_index = ctx.find_inflight(0);
    sm.handle_event(ctx, AemControllerEvent::RcvdResponse, test_time(150));

    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(delivery == AemCommandDelivery::Responded);
    EXPECT_EQ(status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(command_type, AEM_COMMAND_GET_SIGNAL_SELECTOR);
    EXPECT_TRUE(target == TARGET_ID);
    EXPECT_TRUE(sent_copy == std::vector<uint8_t>(payload.begin(), payload.end()));
    EXPECT_TRUE(response_copy == std::vector<uint8_t>(response_data.begin(), response_data.end()));
    EXPECT_EQ(ctx.inflight_count(), 0U);
}

TEST(aem_controller_completion, fires_on_timeout_with_sent_payload)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };

    int fired = 0;
    AemCommandDelivery delivery{};
    std::vector<uint8_t> sent_copy;

    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    std::array<uint8_t, 2> const payload = {0xAB, 0xCD};
    ctx.command_params = {
        .target_entity_id = TARGET_ID,
        .command_code = AEM_COMMAND_READ_DESCRIPTOR,
        .command_data = payload,
        .completion =
            [&](AemCommandResult const& r) {
                ++fired;
                delivery = r.delivery;
                sent_copy.assign(r.sent_payload.begin(), r.sent_payload.end());
            },
    };
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));

    ctx.current_inflight_index = ctx.find_timed_out(test_time(260));
    EXPECT_TRUE(ctx.current_inflight_index < AemControllerContext::MAX_INFLIGHT);
    sm.handle_event(ctx, AemControllerEvent::Timeout, test_time(260));

    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(delivery == AemCommandDelivery::TimedOut);
    EXPECT_TRUE(sent_copy == std::vector<uint8_t>(payload.begin(), payload.end()));
    EXPECT_EQ(ctx.inflight_count(), 0U);
}

TEST(aem_controller_completion, fires_immediately_on_send_failure)
{
    AemControllerContext ctx;
    ctx.my_id = CONTROLLER_ID;

    // tx failure (e.g. target MAC unknown / interface down).
    ctx.tx_command = [](std::span<uint8_t const>) { return false; };

    int fired = 0;
    AemCommandDelivery delivery{};
    AemControllerStateMachine<> sm;
    sm.handle_event(ctx, AemControllerEvent::UCT, test_time(0));

    ctx.command_params = {
        .target_entity_id = TARGET_ID,
        .command_code = AEM_COMMAND_READ_DESCRIPTOR,
        .completion =
            [&](AemCommandResult const& r) {
                ++fired;
                delivery = r.delivery;
            },
    };
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(delivery == AemCommandDelivery::SendFailed);
    EXPECT_EQ(ctx.inflight_count(), 0U);

    // Inflight table full is also an immediate SendFailed.
    ctx.tx_command = [](std::span<uint8_t const>) { return true; };
    for (size_t i = 0; i < AemControllerContext::MAX_INFLIGHT; ++i) {
        ctx.command_params = {.target_entity_id = TARGET_ID, .command_code = AEM_COMMAND_READ_DESCRIPTOR};
        sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));
    }
    EXPECT_EQ(ctx.inflight_count(), AemControllerContext::MAX_INFLIGHT);
    fired = 0;
    ctx.command_params = {
        .target_entity_id = TARGET_ID,
        .command_code = AEM_COMMAND_READ_DESCRIPTOR,
        .completion =
            [&](AemCommandResult const& r) {
                ++fired;
                delivery = r.delivery;
            },
    };
    sm.handle_event(ctx, AemControllerEvent::DoCommand, test_time(0));
    EXPECT_EQ(fired, 1);
    EXPECT_TRUE(delivery == AemCommandDelivery::SendFailed);
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc, atdecc_aecp_aem_controller_test)
