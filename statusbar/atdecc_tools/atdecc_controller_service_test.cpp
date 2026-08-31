// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ControllerService seam tests: ControllerSimple driven by a FAKE backend
// (no socket, no state machines) — proving the business logic depends only
// on the ControllerService contract, the property that lets another
// platform's backend slot in.

#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_simple.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc_tools;
using namespace statusbar::ieee;

namespace {

Eui64 const ENTITY_A{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01};
Eui64 const ENTITY_B{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x02};

/// A ControllerService with no transport at all: records every operation,
/// exposes the sink so tests can inject events, and completes commands on
/// demand. pollable() is nullptr, like a framework-driven backend.
class FakeControllerService final : public ControllerService
{
  public:
    struct AemCall
    {
        std::string op;
        Eui64 target{};
        uint16_t a{0};
        uint16_t b{0};
        atdecc::AemCommandCompletion completion{};
        std::vector<uint8_t> payload{};
    };

    void set_sink(ControllerServiceSink sink) override { sink_ = std::move(sink); }
    void start() override { started_ = true; }
    void tick(int64_t /*now_ns*/) override {}
    [[nodiscard]] auto pollable() noexcept -> net::Pollable* override { return nullptr; }

    void discover_all() override { ++discovers_; }
    [[nodiscard]] auto find_entity(Eui64 const& id) const -> DiscoveredEntity const* override
    {
        for (auto const& e : entities_) {
            if (e.adpdu.entity_id == id) {
                return &e;
            }
        }
        return nullptr;
    }

    auto send_aem_command(
        Eui64 const& target, uint16_t command_code, std::span<uint8_t const> payload, atdecc::AemCommandCompletion c)
        -> bool override
    {
        calls_.push_back(
            {.op = "send",
             .target = target,
             .a = command_code,
             .b = 0,
             .completion = std::move(c),
             .payload = {payload.begin(), payload.end()}});
        return true;
    }

    auto register_unsolicited(Eui64 const& target, atdecc::AemCommandCompletion c) -> bool override
    {
        calls_.push_back(
            {.op = "register_unsolicited", .target = target, .a = 0, .b = 0, .completion = std::move(c), .payload = {}});
        return true;
    }
    [[nodiscard]] auto aem_inflight_count() const -> size_t override { return 0; }

    auto read_descriptor(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        calls_.push_back({.op = "read_descriptor", .target = target, .a = desc_type, .b = desc_index, .completion = {}});
        return true;
    }
    auto set_identify(Eui64 const& target, bool on, atdecc::AemCommandCompletion c) -> bool override
    {
        calls_.push_back(
            {.op = "set_identify", .target = target, .a = static_cast<uint16_t>(on ? 1 : 0), .b = 0, .completion = std::move(c)});
        return true;
    }
    auto get_counters(Eui64 const&, uint16_t, uint16_t) -> bool override { return true; }
    auto set_stream_format(Eui64 const&, uint16_t, uint16_t, uint64_t) -> bool override { return true; }
    auto start_streaming(Eui64 const&, uint16_t, uint16_t) -> bool override { return true; }
    auto stop_streaming(Eui64 const&, uint16_t, uint16_t) -> bool override { return true; }
    auto set_clock_source(Eui64 const&, uint16_t, uint16_t, atdecc::AemCommandCompletion) -> bool override { return true; }
    auto get_clock_source(Eui64 const&, uint16_t, atdecc::AemCommandCompletion) -> bool override { return true; }
    auto set_signal_selector(
        Eui64 const& target, uint16_t desc_index, uint16_t signal_type, uint16_t, uint16_t, atdecc::AemCommandCompletion c)
        -> bool override
    {
        calls_.push_back(
            {.op = "set_signal_selector", .target = target, .a = desc_index, .b = signal_type, .completion = std::move(c)});
        return true;
    }
    auto get_signal_selector(Eui64 const&, uint16_t, atdecc::AemCommandCompletion) -> bool override { return true; }

    auto connect_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto disconnect_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto connect_tx_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto disconnect_tx_stream(Eui64 const&, uint16_t, Eui64 const&, uint16_t) -> bool override { return true; }
    auto get_rx_state(Eui64 const& listener, uint16_t uid) -> bool override
    {
        calls_.push_back({.op = "get_rx_state", .target = listener, .a = uid, .b = 0, .completion = {}});
        return true;
    }
    auto get_tx_state(Eui64 const&, uint16_t) -> bool override { return true; }

    // Test-side controls.
    void add_entity(Eui64 const& id)
    {
        DiscoveredEntity e{};
        e.adpdu.entity_id = id;
        e.valid = true;
        entities_.push_back(e);
        if (sink_.on_entity_added) {
            sink_.on_entity_added(entities_.back());
        }
    }

    [[nodiscard]] auto sink() -> ControllerServiceSink& { return sink_; }
    [[nodiscard]] auto calls() -> std::vector<AemCall>& { return calls_; }
    [[nodiscard]] auto started() const -> bool { return started_; }
    [[nodiscard]] auto discovers() const -> int { return discovers_; }

  private:
    ControllerServiceSink sink_{};
    std::vector<DiscoveredEntity> entities_;
    std::vector<AemCall> calls_;
    bool started_{false};
    int discovers_{0};
};

/// Build a ControllerSimple on a fake backend; returns the fake (owned by
/// the ControllerSimple) for driving the test.
struct Harness
{
    FakeControllerService* fake{};
    std::unique_ptr<ControllerSimple> ctrl;

    Harness()
    {
        auto svc = std::make_unique<FakeControllerService>();
        fake = svc.get();
        ctrl = std::make_unique<ControllerSimple>(std::move(svc));
    }
};

auto make_acmp_response(uint8_t message_type, uint8_t status) -> AcmpDu
{
    AcmpDu acmp{};
    acmp.init_response(message_type, status);
    acmp.talker_entity_id = ENTITY_A;
    acmp.talker_unique_id = 0;
    acmp.listener_entity_id = ENTITY_B;
    acmp.listener_unique_id = 1;
    return acmp;
}

}  // namespace

TEST(controller_service_seam, construction_starts_backend_and_discovers)
{
    Harness h;
    EXPECT_TRUE(h.fake->started());
    EXPECT_EQ(h.fake->discovers(), 1);
    // A backend without an OS event source (framework-style) yields no fd;
    // the consumer must tolerate that.
    EXPECT_EQ(h.ctrl->fd(), -1);
}

TEST(controller_service_seam, entity_added_seeds_descriptor_crawl)
{
    Harness h;
    h.fake->add_entity(ENTITY_A);
    // The ENTITY descriptor read is queued on discovery and drained by tick.
    h.ctrl->tick(1'000'000);
    auto& calls = h.fake->calls();
    bool found = false;
    for (auto const& c : calls) {
        if (c.op == "read_descriptor" && c.target == ENTITY_A && c.a == aem::DESCRIPTOR_ENTITY && c.b == 0) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(controller_service_seam, acmp_observation_produces_connection_events)
{
    Harness h;
    auto& sink = h.fake->sink();
    EXPECT_TRUE(static_cast<bool>(sink.on_acmp_observed));

    sink.on_acmp_observed(make_acmp_response(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS));
    sink.on_acmp_observed(make_acmp_response(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS));

    auto const events = h.ctrl->drain_events();
    bool added = false;
    bool removed = false;
    for (auto const& ev : events) {
        if (auto const* a = std::get_if<ConnectionAddedEvent>(&ev)) {
            added = a->connection.talker_entity_id == ENTITY_A && a->connection.listener_unique_id == 1;
        }
        if (auto const* r = std::get_if<ConnectionRemovedEvent>(&ev)) {
            removed = r->listener_entity_id == ENTITY_B;
        }
    }
    EXPECT_TRUE(added);
    EXPECT_TRUE(removed);
}

TEST(controller_service_seam, dispatched_command_completes_as_typed_event)
{
    Harness h;
    h.fake->add_entity(ENTITY_A);
    (void)h.ctrl->drain_events();

    ControllerAction action{};
    action.kind = ControllerActionKind::SetSignalSelector;
    action.request.talker_entity_id = ENTITY_A;
    action.request.desc_index = 2;
    action.request.signal_type = aem::DESCRIPTOR_AUDIO_CLUSTER;
    action.request.signal_index = 1;
    h.ctrl->dispatch(action, 0);

    // The backend recorded the operation with a live completion.
    auto& calls = h.fake->calls();
    FakeControllerService::AemCall* sel = nullptr;
    for (auto& c : calls) {
        if (c.op == "set_signal_selector") {
            sel = &c;
        }
    }
    EXPECT_TRUE(sel != nullptr);
    if (sel == nullptr) {
        return;
    }
    EXPECT_TRUE(sel->target == ENTITY_A);
    EXPECT_EQ(sel->a, 2);
    EXPECT_TRUE(static_cast<bool>(sel->completion));

    // Completing the command surfaces the typed CommandCompletedEvent.
    std::array<uint8_t, 12> const response{0x00, 0x1B, 0x00, 0x02, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    sel->completion(atdecc::AemCommandResult{
        .delivery = atdecc::AemCommandDelivery::Responded,
        .status = AEM_STATUS_SUCCESS,
        .command_type = AEM_COMMAND_SET_SIGNAL_SELECTOR,
        .target_entity_id = ENTITY_A,
        .sent_payload = {},
        .response = response});

    bool completed = false;
    for (auto const& ev : h.ctrl->drain_events()) {
        if (auto const* c = std::get_if<CommandCompletedEvent>(&ev)) {
            completed = c->ok() && c->command_type == AEM_COMMAND_SET_SIGNAL_SELECTOR && c->entity_id == ENTITY_A &&
                c->response.size() == response.size();
        }
    }
    EXPECT_TRUE(completed);
}

TEST(controller_service_seam, get_and_set_control_actions)
{
    Harness h;
    h.fake->add_entity(ENTITY_A);
    (void)h.ctrl->drain_events();

    ControllerAction get{};
    get.kind = ControllerActionKind::GetControl;
    get.request.talker_entity_id = ENTITY_A;
    get.request.desc_index = 3;
    h.ctrl->dispatch(get, 0);

    ControllerAction set{};
    set.kind = ControllerActionKind::SetControl;
    set.request.talker_entity_id = ENTITY_A;
    set.request.desc_index = 3;
    set.request.control_values.assign({0x12, 0x34});
    h.ctrl->dispatch(set, 0);

    FakeControllerService::AemCall* get_call = nullptr;
    FakeControllerService::AemCall* set_call = nullptr;
    for (auto& c : h.fake->calls()) {
        if (c.op == "send" && c.a == AEM_COMMAND_GET_CONTROL) {
            get_call = &c;
        }
        if (c.op == "send" && c.a == AEM_COMMAND_SET_CONTROL) {
            set_call = &c;
        }
    }
    EXPECT_TRUE(get_call != nullptr && set_call != nullptr);
    if (get_call == nullptr || set_call == nullptr) {
        return;
    }
    // GET: just the 4-byte control header (type CONTROL, index 3).
    std::vector<uint8_t> const get_payload{0x00, 0x1A, 0x00, 0x03};
    EXPECT_TRUE(get_call->payload == get_payload);
    // SET: header + the caller's encoded values.
    std::vector<uint8_t> const set_payload{0x00, 0x1A, 0x00, 0x03, 0x12, 0x34};
    EXPECT_TRUE(set_call->payload == set_payload);
    EXPECT_TRUE(static_cast<bool>(set_call->completion));

    // The SET completion surfaces as a typed CommandCompletedEvent.
    std::array<uint8_t, 6> const response{0x00, 0x1A, 0x00, 0x03, 0x12, 0x34};
    set_call->completion(atdecc::AemCommandResult{
        .delivery = atdecc::AemCommandDelivery::Responded,
        .status = AEM_STATUS_SUCCESS,
        .command_type = AEM_COMMAND_SET_CONTROL,
        .target_entity_id = ENTITY_A,
        .sent_payload = {},
        .response = response});
    bool completed = false;
    for (auto const& ev : h.ctrl->drain_events()) {
        if (auto const* c = std::get_if<CommandCompletedEvent>(&ev)) {
            completed = c->ok() && c->command_type == AEM_COMMAND_SET_CONTROL && c->response.size() == response.size();
        }
    }
    EXPECT_TRUE(completed);
}

TEST(controller_service_seam, matrix_actions_pass_region_payloads_through)
{
    Harness h;
    h.fake->add_entity(ENTITY_A);
    (void)h.ctrl->drain_events();

    // GET: header {MATRIX, 2} + a 12-byte region request (whole region:
    // column 0, row 0, 16x32, count 0, offset 0).
    ControllerAction get{};
    get.kind = ControllerActionKind::GetMatrix;
    get.request.talker_entity_id = ENTITY_A;
    get.request.desc_index = 2;
    get.request.control_values.assign({0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00});
    h.ctrl->dispatch(get, 0);

    // SET: header + region (1x1 at column 4, row 2, count 1) + one float.
    ControllerAction set{};
    set.kind = ControllerActionKind::SetMatrix;
    set.request.talker_entity_id = ENTITY_A;
    set.request.desc_index = 0;
    set.request.control_values.assign(
        {0x00, 0x04, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xC0, 0x40, 0x00, 0x00});
    h.ctrl->dispatch(set, 0);

    FakeControllerService::AemCall* get_call = nullptr;
    FakeControllerService::AemCall* set_call = nullptr;
    for (auto& c : h.fake->calls()) {
        if (c.op == "send" && c.a == AEM_COMMAND_GET_MATRIX) {
            get_call = &c;
        }
        if (c.op == "send" && c.a == AEM_COMMAND_SET_MATRIX) {
            set_call = &c;
        }
    }
    EXPECT_TRUE(get_call != nullptr && set_call != nullptr);
    if (get_call == nullptr || set_call == nullptr) {
        return;
    }
    // MATRIX descriptor type 0x001d in the header, region verbatim after.
    std::vector<uint8_t> const get_head{0x00, 0x1D, 0x00, 0x02};
    EXPECT_TRUE(get_call->payload.size() == 16 && std::equal(get_head.begin(), get_head.end(), get_call->payload.begin()));
    std::vector<uint8_t> const set_head{0x00, 0x1D, 0x00, 0x00};
    EXPECT_TRUE(set_call->payload.size() == 20 && std::equal(set_head.begin(), set_head.end(), set_call->payload.begin()));
    EXPECT_TRUE(set_call->payload.back() == 0x00 && set_call->payload[16] == 0xC0);
    EXPECT_TRUE(static_cast<bool>(get_call->completion) && static_cast<bool>(set_call->completion));
}

TEST(controller_service_seam, unsolicited_responses_surface_as_events)
{
    Harness h;
    h.fake->add_entity(ENTITY_A);
    (void)h.ctrl->drain_events();

    // Registration flows through the dedicated backend hook.
    ControllerAction reg{};
    reg.kind = ControllerActionKind::RegisterUnsolicited;
    reg.request.talker_entity_id = ENTITY_A;
    h.ctrl->dispatch(reg, 0);
    bool registered = false;
    for (auto const& c : h.fake->calls()) {
        registered = registered || (c.op == "register_unsolicited" && c.target == ENTITY_A);
    }
    EXPECT_TRUE(registered);

    // An unsolicited SET_CONTROL response fans out as UnsolicitedEvent.
    auto& sink = h.fake->sink();
    EXPECT_TRUE(static_cast<bool>(sink.on_unsolicited));
    std::array<uint8_t, 6> const body{0x00, 0x1A, 0x00, 0x03, 0xAB, 0xCD};
    sink.on_unsolicited(ENTITY_A, AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, body);

    bool surfaced = false;
    for (auto const& ev : h.ctrl->drain_events()) {
        if (auto const* u = std::get_if<UnsolicitedEvent>(&ev)) {
            surfaced = u->entity_id == ENTITY_A && u->command_type == AEM_COMMAND_SET_CONTROL &&
                u->aem_status == AEM_STATUS_SUCCESS && u->response.size() == body.size() && u->response[4] == 0xAB;
        }
    }
    EXPECT_TRUE(surfaced);
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc_tools, atdecc_controller_service_test)
