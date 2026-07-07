// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Integration tests: NanoAvbAemController ↔ NanoAvbEntity via LoopbackPort.
/// Tests the full ADP discovery → AEM READ_DESCRIPTOR pipeline in-process
/// without any network access.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_loopback_port.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using statusbar::net::EthernetTxSlot;
using statusbar::net::LoopbackPortContext;
using TimePoint = statusbar::sm::TimePoint;

namespace ent_caps = statusbar::atdecc::entity_capabilities;
namespace tlk_caps = statusbar::atdecc::talker_capabilities;
namespace lst_caps = statusbar::atdecc::listener_capabilities;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Eui64 const ENTITY_ID{0x00, 0x01, 0x02, 0xFF, 0xFE, 0x05, 0x06, 0x07};
static Eui64 const CONTROLLER_ID{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0x00, 0x00, 0x01};
static Eui48 const ENTITY_MAC{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
static Eui48 const CONTROLLER_MAC{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};

static auto create_test_entity_model() -> EntityModel
{
    EntityModelConfig cfg;
    cfg.max_configurations = 1;
    cfg.max_stream_inputs = 2;
    cfg.max_stream_outputs = 2;
    cfg.max_avb_interfaces = 1;
    cfg.max_clock_sources = 1;
    cfg.max_clock_domains = 1;

    EntityModel model{cfg};

    DescriptorEntity entity{};
    entity.entity_id = ENTITY_ID;
    entity.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    entity.entity_capabilities = ent_caps::AEM_SUPPORTED | ent_caps::CLASS_A_SUPPORTED | ent_caps::GPTP_SUPPORTED;
    entity.talker_stream_sources = 2;
    entity.talker_capabilities = tlk_caps::IMPLEMENTED | tlk_caps::AUDIO_SOURCE;
    entity.listener_stream_sinks = 2;
    entity.listener_capabilities = lst_caps::IMPLEMENTED | lst_caps::AUDIO_SINK;
    entity.entity_name = AtdeccString{"Integration Test Entity"};
    entity.firmware_version = AtdeccString{"1.0.0"};
    entity.serial_number = AtdeccString{"SN-12345"};
    entity.configurations_count = 1;
    model.set_entity(entity);

    DescriptorConfiguration config{};
    config.object_name = AtdeccString{"Default Config"};
    (void)model.add_configuration(config);

    DescriptorStream stream_out0{};
    stream_out0.object_name = AtdeccString{"Output 1"};
    (void)model.add_stream_output(stream_out0);

    DescriptorStream stream_out1{};
    stream_out1.object_name = AtdeccString{"Output 2"};
    (void)model.add_stream_output(stream_out1);

    DescriptorStream stream_in0{};
    stream_in0.object_name = AtdeccString{"Input 1"};
    (void)model.add_stream_input(stream_in0);

    DescriptorStream stream_in1{};
    stream_in1.object_name = AtdeccString{"Input 2"};
    (void)model.add_stream_input(stream_in1);

    return model;
}

/// Send a frame via a LoopbackPort (build Ethernet-like packet from raw payload).
static auto send_raw(LoopbackPortContext& port, std::span<uint8_t const> payload) -> bool
{
    auto slot_result = port.tx_start();
    if (!slot_result.has_value()) {
        return false;
    }
    auto slot = *slot_result;
    auto const len = std::min(payload.size(), slot.buffer.size());
    std::memcpy(slot.buffer.data(), payload.data(), len);
    return port.tx_commit(slot.handle, len).has_value();
}

/// Drain all frames from one port's TX captures and dispatch them to
/// the receiver's appropriate handler.
static auto relay_entity_to_controller(LoopbackPortContext& entity_port, NanoAvbAemController& controller, int64_t now_ns) -> size_t
{
    size_t count = 0;
    while (auto cap = entity_port.pop_tx()) {
        if (cap->frame.empty()) {
            continue;
        }
        auto const subtype = cap->frame[0];
        if (subtype == avtp::AvtpSubtype::adp && cap->frame.size() >= sizeof(AdpDu)) {
            AdpDu adp{};
            span_load(adp, std::span<uint8_t const>{cap->frame});
            controller.receive_adp(adp, ENTITY_MAC, now_ns);
        } else if (subtype == avtp::AvtpSubtype::aecp) {
            controller.receive_aecp(cap->frame, now_ns);
        }
        ++count;
    }
    return count;
}

static auto relay_controller_to_entity(LoopbackPortContext& controller_port, AemCommandHandler& aem_handler, Eui64 const& entity_id)
    -> size_t
{
    size_t count = 0;
    while (auto cap = controller_port.pop_tx()) {
        if (cap->frame.size() >= AemDu::LENGTH) {
            (void)aem_handler.process_packet(CONTROLLER_MAC, cap->frame, entity_id);
        }
        ++count;
    }
    return count;
}

/// A test fixture wiring up entity ↔ controller via loopback ports.
struct IntegrationFixture
{
    EntityModel entity_model;
    AemCommandHandler aem_handler;
    NanoAvbAdpAdvertiser adp_advertiser;
    NanoAvbAemController controller;
    LoopbackPortContext entity_port;
    LoopbackPortContext controller_port;

    // Tracking
    std::vector<Eui64> discovered_entities;
    std::vector<std::tuple<Eui64, uint16_t, uint8_t>> aem_responses;  // target, cmd, status

    IntegrationFixture()
        : entity_model{create_test_entity_model()}
        , aem_handler{entity_model}
        , adp_advertiser{entity_model.get_entity()}
        , controller{CONTROLLER_ID}
    {
        auto [a, b] = net::create_loopback_pair({
            .mac_a = ENTITY_MAC,
            .mac_b = CONTROLLER_MAC,
            .frame_pool_size = 128,
        });
        entity_port = std::move(a);
        controller_port = std::move(b);

        // Wire entity AEM responses → entity port TX
        aem_handler.set_callbacks({
            .send_response = [this](Eui48 const& /*dest*/, std::span<uint8_t const> response) -> bool {
                return send_raw(entity_port, response);
            },
        });

        // Wire entity ADP advertisements → entity port TX
        adp_advertiser.set_callbacks({
            .send_adpdu = [this](AdpDu const& adp) -> bool { return send_raw(entity_port, make_const_span(adp)); },
        });

        // Wire controller TX → controller port TX
        controller.set_callbacks({
            .send_atdecc_multicast = [this](std::span<uint8_t const> packet) -> bool { return send_raw(controller_port, packet); },
            .send_atdecc_unicast = [this](Eui48 const& /*dest*/, std::span<uint8_t const> packet) -> bool {
                return send_raw(controller_port, packet);
            },
            .on_entity_available = [this](DiscoveredEntity const& e) { discovered_entities.push_back(e.adpdu.entity_id); },
            .on_entity_updated = [](DiscoveredEntity const&) {},
            .on_entity_departing = [](Eui64) {},
            .on_aem_response =
                [this](Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> /*sent*/, std::span<uint8_t const>) {
                    aem_responses.emplace_back(target, cmd, status);
                },
            .on_aem_timeout = [](Eui64, uint16_t) {},
            .on_acmp_response = [](AcmpCommandResponse const&) {},
            .on_acmp_timeout = [](AcmpCommandResponse const&) {},
        });
        controller.start();
    }

    /// Run one round of entity advertise + relay + tick.
    void run_discovery_round(int64_t now_ns)
    {
        adp_advertiser.tick(sm::TimePoint{std::chrono::nanoseconds{now_ns}});
        relay_entity_to_controller(entity_port, controller, now_ns);
        controller.tick(now_ns);
    }

    /// Send a command from controller and run the request/response relay.
    void run_command_round(int64_t now_ns)
    {
        // Controller TX → entity RX (AEM command)
        relay_controller_to_entity(controller_port, aem_handler, ENTITY_ID);
        // Entity TX → controller RX (AEM response)
        relay_entity_to_controller(entity_port, controller, now_ns);
        controller.tick(now_ns);
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(nanoavb_integration, discovery_via_adp)
{
    IntegrationFixture f;

    // Entity starts advertising
    (void)f.adp_advertiser.start(TimePoint{});
    f.run_discovery_round(1'000'000'000);

    EXPECT_EQ(f.discovered_entities.size(), 1u);
    EXPECT_EQ(f.discovered_entities[0], ENTITY_ID);
    EXPECT_EQ(f.controller.entity_count(), 1u);
    EXPECT_TRUE(f.controller.find_entity(ENTITY_ID) != nullptr);
}

TEST(nanoavb_integration, read_entity_descriptor)
{
    IntegrationFixture f;

    // Discovery
    (void)f.adp_advertiser.start(TimePoint{});
    f.run_discovery_round(1'000'000'000);

    // Send READ_DESCRIPTOR for ENTITY
    EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, DESCRIPTOR_ENTITY, 0));
    f.run_command_round(1'100'000'000);

    EXPECT_EQ(f.aem_responses.size(), 1u);
    auto const& [target, cmd, status] = f.aem_responses[0];
    EXPECT_EQ(target, ENTITY_ID);
    EXPECT_EQ(cmd, AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_EQ(status, AEM_STATUS_SUCCESS);
}

TEST(nanoavb_integration, read_stream_descriptors)
{
    IntegrationFixture f;

    (void)f.adp_advertiser.start(TimePoint{});
    f.run_discovery_round(1'000'000'000);

    // Read all 4 stream descriptors (2 outputs + 2 inputs)
    EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, DESCRIPTOR_STREAM_OUTPUT, 0));
    f.run_command_round(1'100'000'000);
    EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, DESCRIPTOR_STREAM_OUTPUT, 1));
    f.run_command_round(1'200'000'000);
    EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, DESCRIPTOR_STREAM_INPUT, 0));
    f.run_command_round(1'300'000'000);
    EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, DESCRIPTOR_STREAM_INPUT, 1));
    f.run_command_round(1'400'000'000);

    EXPECT_EQ(f.aem_responses.size(), 4u);
    for (auto const& [target, cmd, status] : f.aem_responses) {
        EXPECT_EQ(target, ENTITY_ID);
        EXPECT_EQ(cmd, AEM_COMMAND_READ_DESCRIPTOR);
        EXPECT_EQ(status, AEM_STATUS_SUCCESS);
    }
}

TEST(nanoavb_integration, read_nonexistent_descriptor_returns_error)
{
    IntegrationFixture f;

    (void)f.adp_advertiser.start(TimePoint{});
    f.run_discovery_round(1'000'000'000);

    // Read a stream index that doesn't exist
    EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, DESCRIPTOR_STREAM_OUTPUT, 99));
    f.run_command_round(1'100'000'000);

    EXPECT_EQ(f.aem_responses.size(), 1u);
    auto const& [target, cmd, status] = f.aem_responses[0];
    EXPECT_EQ(cmd, AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_NE(status, AEM_STATUS_SUCCESS);
}

TEST(nanoavb_integration, identify_command)
{
    IntegrationFixture f;

    (void)f.adp_advertiser.start(TimePoint{});
    f.run_discovery_round(1'000'000'000);

    // Without AEM_IDENTIFY_CONTROL_INDEX_VALID in the ADPDU the controller
    // refuses to guess a control index.
    EXPECT_FALSE(f.controller.set_identify(ENTITY_ID, true));

    // Identify is implemented as SET_CONTROL on the CONTROL descriptor at
    // the index advertised by the ADPDU's identify_control_index field.
    f.adp_advertiser.set_identify_control_index(0);
    f.adp_advertiser.notify_entity_changed();
    f.run_discovery_round(1'050'000'000);
    EXPECT_TRUE(f.controller.set_identify(ENTITY_ID, true));
    f.run_command_round(1'100'000'000);

    EXPECT_EQ(f.aem_responses.size(), 1u);
    auto const& [target, cmd, status] = f.aem_responses[0];
    EXPECT_EQ(cmd, AEM_COMMAND_SET_CONTROL);
    // Entity may return SUCCESS or NOT_IMPLEMENTED depending on model
}

TEST(nanoavb_integration, multiple_descriptor_reads_sequentially)
{
    IntegrationFixture f;

    (void)f.adp_advertiser.start(TimePoint{});
    f.run_discovery_round(1'000'000'000);

    // Read ENTITY, CONFIGURATION, STREAM_OUTPUT(0), STREAM_INPUT(0)
    uint16_t const desc_types[] = {DESCRIPTOR_ENTITY, DESCRIPTOR_CONFIGURATION, DESCRIPTOR_STREAM_OUTPUT, DESCRIPTOR_STREAM_INPUT};
    int64_t t = 1'100'000'000;
    for (auto dt : desc_types) {
        EXPECT_TRUE(f.controller.read_descriptor(ENTITY_ID, dt, 0));
        f.run_command_round(t);
        t += 100'000'000;
    }

    EXPECT_EQ(f.aem_responses.size(), 4u);
    for (auto const& [target, cmd, status] : f.aem_responses) {
        EXPECT_EQ(cmd, AEM_COMMAND_READ_DESCRIPTOR);
        EXPECT_EQ(status, AEM_STATUS_SUCCESS);
    }
}

TEST_MAIN(statusbar_nanoavb, nanoavb_integration_test)
