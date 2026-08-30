// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for NanoAvbAemController

#include "statusbar/nanoavb/nanoavb_controller.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

Eui64 const CONTROLLER_ID{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01};
Eui64 const TARGET_ID{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
Eui48 const TARGET_MAC{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

auto test_time(int ms) -> sm::TimePoint
{
    return sm::TimePoint{} + std::chrono::milliseconds(ms);
}

}  // namespace

TEST(nanoavb_controller, construction)
{
    NanoAvbAemController controller{CONTROLLER_ID};
    EXPECT_EQ(controller.entity_id(), CONTROLLER_ID);
    EXPECT_EQ(controller.entity_count(), 0U);
    EXPECT_EQ(controller.acmp_inflight_count(), 0U);
    EXPECT_EQ(controller.aem_inflight_count(), 0U);
}

TEST(nanoavb_controller, discover_all_sends)
{
    std::vector<std::vector<uint8_t>> sent;
    AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_multicast = [&](std::span<uint8_t const> packet) {
        sent.emplace_back(packet.begin(), packet.end());
        return true;
    };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};
    controller.discover_all();

    EXPECT_EQ(sent.size(), 1U);
}

TEST(nanoavb_controller, receive_adp_available)
{
    std::vector<Eui64> available;
    AemControllerEntityCallbacks callbacks;
    callbacks.on_entity_available = [&](DiscoveredEntity const& e) { available.push_back(e.adpdu.entity_id); };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};

    AdpDu adp{};
    adp.init_entity_available(TARGET_ID, 31);
    adp.available_index = 1;

    controller.receive_adp(adp, TARGET_MAC, test_time(0));

    EXPECT_EQ(controller.entity_count(), 1U);
    EXPECT_EQ(available.size(), 1U);

    auto mac = controller.mac_for_entity(TARGET_ID);
    EXPECT_TRUE(mac.has_value());
    EXPECT_TRUE(*mac == TARGET_MAC);
}

TEST(nanoavb_controller, connect_stream_sends_acmp)
{
    std::vector<std::vector<uint8_t>> sent;
    AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_multicast = [&](std::span<uint8_t const> packet) {
        sent.emplace_back(packet.begin(), packet.end());
        return true;
    };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};
    controller.start();

    Eui64 const talker{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10};
    Eui64 const listener{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x20};

    bool ok = controller.connect_stream(talker, 0, listener, 0);
    EXPECT_TRUE(ok);
    EXPECT_EQ(controller.acmp_inflight_count(), 1U);
}

// Regression: the controller MUST emit ACMP commands in the pre-2021 56-byte
// (control_data_length=44) "2016" short form on L2. Per IEEE 1722.1 a receiver
// must accept the 96-byte 2021 extended form and ignore the extra bytes, but
// several shipping devices (e.g. the DSP processor) silently DROP the oversized PDU -- so a
// controller that emits the 2021 form on L2 silently fails to connect them. The
// bug was tx_command serializing the whole struct via make_const_span (84-byte
// cdl) instead of acmp_serialize_2016 (the L2 short form, matching the entity
// talker/listener TX).
TEST(nanoavb_controller, connect_command_is_2016_short_form)
{
    std::vector<std::vector<uint8_t>> sent;
    AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_multicast = [&](std::span<uint8_t const> packet) {
        sent.emplace_back(packet.begin(), packet.end());
        return true;
    };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};
    controller.start();

    Eui64 const talker{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10};
    Eui64 const listener{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x20};
    EXPECT_TRUE(controller.connect_stream(talker, 0, listener, 0));

    EXPECT_EQ(sent.size(), 1U);
    if (!sent.empty()) {
        auto const& pkt = sent.front();
        // 56-byte 2016 short form on the wire -- NOT the 96-byte 2021 extended form.
        EXPECT_EQ(pkt.size(), AcmpDu::LENGTH);
        // control_data_length == 44 (2016), not 84 (2021). Byte 2 low 3 bits + byte 3.
        if (pkt.size() >= 4) {
            uint16_t const cdl = static_cast<uint16_t>(((pkt[2] & 0x07) << 8) | pkt[3]);
            EXPECT_EQ(cdl, AcmpDu::DATA_LENGTH);
            EXPECT_EQ(pkt[1] & 0x0F, ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
        }
    }
}

TEST(nanoavb_controller, send_aem_needs_discovered_mac)
{
    std::vector<std::vector<uint8_t>> unicast_sent;
    AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_unicast = [&](Eui48 const&, std::span<uint8_t const> packet) {
        unicast_sent.emplace_back(packet.begin(), packet.end());
        return true;
    };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};
    controller.start();

    // AEM command to unknown entity - should fail (no MAC)
    // The command will be queued but tx_command returns false
    auto ok = controller.read_descriptor(TARGET_ID, 0x0000, 0x0000);
    // Command was accepted internally but send failed
    EXPECT_TRUE(ok);
    EXPECT_EQ(unicast_sent.size(), 0U);  // No packet sent because MAC unknown
}

TEST(nanoavb_controller, send_aem_after_discovery)
{
    std::vector<std::vector<uint8_t>> unicast_sent;
    AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_unicast = [&](Eui48 const&, std::span<uint8_t const> packet) {
        unicast_sent.emplace_back(packet.begin(), packet.end());
        return true;
    };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};
    controller.start();

    // Discover entity first
    AdpDu adp{};
    adp.init_entity_available(TARGET_ID, 31);
    adp.available_index = 1;
    controller.receive_adp(adp, TARGET_MAC, test_time(0));

    // Now AEM command should succeed
    controller.read_descriptor(TARGET_ID, 0x0000, 0x0000);
    EXPECT_EQ(unicast_sent.size(), 1U);
    EXPECT_EQ(controller.aem_inflight_count(), 1U);
}

TEST(nanoavb_controller, unsolicited_response_reaches_callback)
{
    std::vector<uint8_t> body_seen;
    Eui64 from{};
    uint16_t cmd_seen = 0;
    uint8_t status_seen = 0xFF;
    AemControllerEntityCallbacks callbacks;
    callbacks.on_unsolicited = [&](Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> data) {
        from = target;
        cmd_seen = cmd;
        status_seen = status;
        body_seen.assign(data.begin(), data.end());
    };
    bool solicited_response_seen = false;
    callbacks.on_aem_response = [&](Eui64, uint16_t, uint8_t, std::span<uint8_t const>, std::span<uint8_t const>) {
        solicited_response_seen = true;
    };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};
    controller.start();

    // An unsolicited SET_CONTROL response: the entity's own sequence
    // counter (no matching in-flight command), U bit set.
    AemDu du{};
    du.init_response(AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, 20, /*unsolicited=*/true);
    du.controller_entity_id = CONTROLLER_ID;
    du.target_entity_id = TARGET_ID;
    du.sequence_id = 0x1234;
    std::array<uint8_t, 6> const body{0x00, 0x1A, 0x00, 0x03, 0xAB, 0xCD};

    std::vector<uint8_t> frame;
    auto const header = make_const_span(du);
    frame.assign(header.begin(), header.end());
    frame.insert(frame.end(), body.begin(), body.end());
    controller.receive_aecp(frame, test_time(10));

    EXPECT_TRUE(from == TARGET_ID);
    EXPECT_EQ(cmd_seen, AEM_COMMAND_SET_CONTROL);
    EXPECT_EQ(status_seen, AEM_STATUS_SUCCESS);
    EXPECT_EQ(body_seen.size(), body.size());
    EXPECT_TRUE(!body_seen.empty() && body_seen[4] == 0xAB);
    // It must not masquerade as a solicited response.
    EXPECT_FALSE(solicited_response_seen);

    // A NON-unsolicited response with an unknown sequence stays dropped.
    body_seen.clear();
    du.init_response(AEM_COMMAND_SET_CONTROL, AEM_STATUS_SUCCESS, 20, /*unsolicited=*/false);
    du.controller_entity_id = CONTROLLER_ID;
    du.target_entity_id = TARGET_ID;
    du.sequence_id = 0x4321;
    auto const header2 = make_const_span(du);
    frame.assign(header2.begin(), header2.end());
    frame.insert(frame.end(), body.begin(), body.end());
    controller.receive_aecp(frame, test_time(20));
    EXPECT_TRUE(body_seen.empty());
    EXPECT_FALSE(solicited_response_seen);
}

TEST(nanoavb_controller, tick_expires_entities)
{
    std::vector<Eui64> departed;
    AemControllerEntityCallbacks callbacks;
    callbacks.on_entity_available = [](DiscoveredEntity const&) {};
    callbacks.on_entity_departing = [&](Eui64 id) { departed.push_back(id); };

    NanoAvbAemController controller{CONTROLLER_ID, callbacks};

    AdpDu adp{};
    adp.init_entity_available(TARGET_ID, 1);  // valid_time=1 -> 2 seconds
    adp.available_index = 1;
    controller.receive_adp(adp, TARGET_MAC, test_time(0));
    EXPECT_EQ(controller.entity_count(), 1U);

    controller.tick(test_time(3000));
    EXPECT_EQ(controller.entity_count(), 0U);
    EXPECT_EQ(departed.size(), 1U);
}

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_controller_test)
