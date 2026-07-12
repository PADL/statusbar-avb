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

    auto send_aem_command(Eui64 const& target, uint16_t command_code, std::span<uint8_t const>, atdecc::AemCommandCompletion c)
        -> bool override
    {
        calls_.push_back({.op = "send", .target = target, .a = command_code, .b = 0, .completion = std::move(c)});
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

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc_tools, atdecc_controller_service_test)
