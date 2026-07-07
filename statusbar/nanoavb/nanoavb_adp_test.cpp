// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/sm/sm_test_support.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>

using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using statusbar::tsn::ClockIdentity;
namespace sm = statusbar::sm;
namespace sm_test = statusbar::sm::test;
using TimePoint = statusbar::sm::TimePoint;

// Use fully-qualified namespace aliases for capability flags
namespace ent_caps = statusbar::atdecc::entity_capabilities;
namespace tlk_caps = statusbar::atdecc::talker_capabilities;
namespace lst_caps = statusbar::atdecc::listener_capabilities;

//
// Helper Functions
//
static DescriptorEntity create_test_entity_descriptor()
{
    DescriptorEntity entity{};
    entity.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    entity.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    entity.entity_capabilities = ent_caps::AEM_SUPPORTED | ent_caps::CLASS_A_SUPPORTED | ent_caps::GPTP_SUPPORTED;
    entity.talker_stream_sources = 2;
    entity.talker_capabilities = tlk_caps::IMPLEMENTED | tlk_caps::AUDIO_SOURCE;
    entity.listener_stream_sinks = 2;
    entity.listener_capabilities = lst_caps::IMPLEMENTED | lst_caps::AUDIO_SINK;
    entity.entity_name = AtdeccString{"Test Entity"};
    return entity;
}

//
// NanoAvbAdpAdvertiser Construction Tests
//
TEST(nanoavb_adp_advertiser, default_construction)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    EXPECT_EQ(advertiser.state(), AdpAdvertiserState::Stopped);
    EXPECT_EQ(advertiser.available_index(), 0);
    EXPECT_EQ(advertiser.valid_time(), 31);  // Default valid time
}

TEST(nanoavb_adp_advertiser, custom_config)
{
    auto entity = create_test_entity_descriptor();

    AdpAdvertiserConfig config;
    config.valid_time = 15;
    config.reannounce_interval = std::chrono::milliseconds{5000};

    NanoAvbAdpAdvertiser advertiser{entity, {}, config};

    EXPECT_EQ(advertiser.valid_time(), 15);
    EXPECT_EQ(advertiser.config().reannounce_interval, std::chrono::milliseconds{5000});
}

TEST(nanoavb_adp_advertiser, adpdu_populated_from_model)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    auto const& adpdu = advertiser.adpdu();

    // Verify ADPDU fields match entity model
    EXPECT_EQ(adpdu.entity_id, entity.entity_id);
    EXPECT_EQ(adpdu.entity_model_id, entity.entity_model_id);
    EXPECT_EQ(adpdu.entity_capabilities.get(), entity.entity_capabilities.get());
    EXPECT_EQ(adpdu.talker_stream_sources.get(), entity.talker_stream_sources.get());
    EXPECT_EQ(adpdu.talker_capabilities.get(), entity.talker_capabilities.get());
    EXPECT_EQ(adpdu.listener_stream_sinks.get(), entity.listener_stream_sinks.get());
    EXPECT_EQ(adpdu.listener_capabilities.get(), entity.listener_capabilities.get());
}

//
// NanoAvbAdpAdvertiser State Control Tests
//
TEST(nanoavb_adp_advertiser, start_advertising)
{
    auto entity = create_test_entity_descriptor();

    bool sent = false;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const& adpdu) {
        sent = true;
        EXPECT_TRUE(adpdu.is_entity_available());
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    auto result = advertiser.start(TimePoint{});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(advertiser.state(), AdpAdvertiserState::Advertising);
    EXPECT_TRUE(sent);  // Initial Entity Available should be sent
}

TEST(nanoavb_adp_advertiser, start_idempotent)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(send_count, 1);

    // Second start should be idempotent (no extra send)
    auto result = advertiser.start(TimePoint{});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(send_count, 1);  // No additional send
}

TEST(nanoavb_adp_advertiser, stop_advertising)
{
    auto entity = create_test_entity_descriptor();

    bool departing_sent = false;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const& adpdu) {
        if (adpdu.is_entity_departing()) {
            departing_sent = true;
        }
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(advertiser.state(), AdpAdvertiserState::Advertising);

    auto result = advertiser.stop(TimePoint{});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(advertiser.state(), AdpAdvertiserState::Stopped);
    EXPECT_TRUE(departing_sent);
}

TEST(nanoavb_adp_advertiser, departing_available_index_zero)
{
    auto entity = create_test_entity_descriptor();

    AdpDu captured_departing{};
    int available_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const& adpdu) {
        if (adpdu.is_entity_available()) {
            ++available_count;
        }
        if (adpdu.is_entity_departing()) {
            captured_departing = adpdu;
        }
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};
    (void)advertiser.start(TimePoint{});

    // Tick several times to send multiple available announcements (incrementing available_index)
    auto t = TimePoint{} + std::chrono::seconds(65);
    advertiser.tick(t);
    t += std::chrono::seconds(65);
    advertiser.tick(t);
    EXPECT_TRUE(available_count >= 2);

    // Stop should send departing with available_index == 0
    (void)advertiser.stop(t);
    EXPECT_EQ(captured_departing.available_index.get(), 0U);
}

TEST(nanoavb_adp_advertiser, stop_idempotent)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    // Stop when already stopped should succeed
    auto result = advertiser.stop(TimePoint{});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(advertiser.state(), AdpAdvertiserState::Stopped);
}

//
// NanoAvbAdpAdvertiser Entity State Change Tests
//
TEST(nanoavb_adp_advertiser, notify_entity_changed)
{
    auto entity = create_test_entity_descriptor();

    uint32_t notified_index = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.on_available_index_change = [&](uint32_t index) { notified_index = index; };
    callbacks.send_adpdu = [](AdpDu const&) { return true; };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    EXPECT_EQ(advertiser.available_index(), 0);

    // Start advertising first so notify_entity_changed will send
    (void)advertiser.start(TimePoint{});
    // available_index incremented after start() sends Entity Available
    EXPECT_EQ(advertiser.available_index(), 1);
    EXPECT_EQ(notified_index, 1);

    // notify_entity_changed should trigger another send and increment
    advertiser.notify_entity_changed();

    EXPECT_EQ(advertiser.available_index(), 2);
    EXPECT_EQ(notified_index, 2);
    EXPECT_EQ(advertiser.adpdu().available_index.get(), 2);
}

TEST(nanoavb_adp_advertiser, notify_sends_update_when_advertising)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(send_count, 1);

    // Notify should trigger another Entity Available
    advertiser.notify_entity_changed();
    EXPECT_EQ(send_count, 2);
}

TEST(nanoavb_adp_advertiser, notify_no_send_when_stopped)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    // Notify while stopped should not send
    advertiser.notify_entity_changed();
    EXPECT_EQ(send_count, 0);
    // Per IEEE 1722.1, available_index only increments on actual transmission
    EXPECT_EQ(advertiser.available_index(), 0);
}

//
// NanoAvbAdpAdvertiser Discover Response Tests
//
TEST(nanoavb_adp_advertiser, respond_to_broadcast_discover)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const& adpdu) {
        if (adpdu.is_entity_available()) {
            ++send_count;
        }
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(send_count, 1);

    // Receive a broadcast discover (entity_id = 0)
    AdpDu discover{};
    discover.init_entity_discover();

    advertiser.receive_adpdu(discover, TimePoint{});
    EXPECT_EQ(send_count, 2);  // Should respond with Entity Available
}

TEST(nanoavb_adp_advertiser, respond_to_targeted_discover)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(send_count, 1);

    // Receive a targeted discover (entity_id = our ID)
    AdpDu discover{};
    discover.init_entity_discover();
    discover.entity_id = entity.entity_id;

    advertiser.receive_adpdu(discover, TimePoint{});
    EXPECT_EQ(send_count, 2);  // Should respond
}

TEST(nanoavb_adp_advertiser, ignore_discover_for_other_entity)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(send_count, 1);

    // Receive a targeted discover for a different entity
    AdpDu discover{};
    discover.init_entity_discover();
    discover.entity_id = Eui64{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    advertiser.receive_adpdu(discover, TimePoint{});
    EXPECT_EQ(send_count, 1);  // Should NOT respond
}

TEST(nanoavb_adp_advertiser, ignore_discover_when_stopped)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    // Receive discover while stopped
    AdpDu discover{};
    discover.init_entity_discover();

    advertiser.receive_adpdu(discover, TimePoint{});
    EXPECT_EQ(send_count, 0);  // Should NOT respond when stopped
}

//
// NanoAvbAdpAdvertiser Tick/Reannounce Tests
//
TEST(nanoavb_adp_advertiser, tick_reannounce)
{
    auto entity = create_test_entity_descriptor();

    AdpAdvertiserConfig config;
    config.reannounce_interval = std::chrono::milliseconds{100};

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks, config};

    TimePoint now{};
    (void)advertiser.start(now);
    EXPECT_EQ(send_count, 1);

    // Tick immediately - should not reannounce yet
    advertiser.tick(now);
    EXPECT_EQ(send_count, 1);

    // Tick after reannounce interval
    now += std::chrono::milliseconds{100};
    advertiser.tick(now);
    EXPECT_EQ(send_count, 2);
}

TEST(nanoavb_adp_advertiser, tick_no_send_when_stopped)
{
    auto entity = create_test_entity_descriptor();

    int send_count = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++send_count;
        return true;
    };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};

    // Tick while stopped - should not send
    advertiser.tick(TimePoint{});
    EXPECT_EQ(send_count, 0);
}

//
// NanoAvbAdpAdvertiser Configuration Tests
//
TEST(nanoavb_adp_advertiser, set_gptp_info)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    ClockIdentity gm_id{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    advertiser.set_gptp_info(gm_id, 0);

    EXPECT_EQ(advertiser.adpdu().gptp_grandmaster_id, gm_id);
    EXPECT_EQ(advertiser.adpdu().gptp_domain_number, 0);
}

TEST(nanoavb_adp_advertiser, set_interface_index)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    advertiser.set_interface_index(42);

    EXPECT_EQ(advertiser.adpdu().interface_index.get(), 42);
}

TEST(nanoavb_adp_advertiser, set_identify_control_index)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    advertiser.set_identify_control_index(123);

    EXPECT_EQ(advertiser.adpdu().identify_control_index.get(), 123);
    // Advertising the index must also assert AEM_IDENTIFY_CONTROL_INDEX_VALID so
    // controllers know the index is meaningful.
    EXPECT_TRUE(advertiser.adpdu().has_entity_capability(statusbar::atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID));

    // The identify advertisement must survive an ADPDU rebuild from the model
    // (notify_entity_changed runs on startup MAC/entity-id patching) — it is
    // discovered from the descriptor storage, not the ENTITY descriptor.
    advertiser.notify_entity_changed();
    EXPECT_EQ(advertiser.adpdu().identify_control_index.get(), 123);
    EXPECT_TRUE(advertiser.adpdu().has_entity_capability(statusbar::atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID));
}

TEST(nanoavb_adp_advertiser, update_config)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    EXPECT_EQ(advertiser.valid_time(), 31);

    AdpAdvertiserConfig new_config;
    new_config.valid_time = 10;
    new_config.reannounce_interval = std::chrono::milliseconds{2000};

    advertiser.set_config(new_config);

    EXPECT_EQ(advertiser.valid_time(), 10);
    EXPECT_EQ(advertiser.adpdu().valid_time(), 10);
    EXPECT_EQ(advertiser.config().reannounce_interval, std::chrono::milliseconds{2000});
}

//
// NanoAvbAdpDiscovery Tests
//

TEST(nanoavb_adp_discovery, construction)
{
    NanoAvbAdpDiscovery discovery;
    EXPECT_EQ(discovery.entity_count(), 0U);
}

TEST(nanoavb_adp_discovery, receive_available)
{
    std::vector<Eui64> available;
    AdpDiscoveryCallbacks callbacks;
    callbacks.on_entity_available = [&](DiscoveredEntity const& e) { available.push_back(e.adpdu.entity_id); };

    NanoAvbAdpDiscovery discovery{callbacks};

    AdpDu adp{};
    adp.init_entity_available(Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01}, 31);
    adp.available_index = 1;
    Eui48 src_mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    discovery.receive_adpdu(adp, src_mac, sm::TimePoint{});

    EXPECT_EQ(discovery.entity_count(), 1U);
    EXPECT_EQ(available.size(), 1U);

    auto const* found = discovery.find_entity(adp.entity_id);
    EXPECT_TRUE(found != nullptr);

    auto mac = discovery.mac_for_entity(adp.entity_id);
    EXPECT_TRUE(mac.has_value());
    EXPECT_TRUE(*mac == src_mac);
}

TEST(nanoavb_adp_discovery, receive_departing)
{
    std::vector<Eui64> departed;
    AdpDiscoveryCallbacks callbacks;
    callbacks.on_entity_available = [](DiscoveredEntity const&) {};
    callbacks.on_entity_departing = [&](Eui64 id) { departed.push_back(id); };

    NanoAvbAdpDiscovery discovery{callbacks};

    Eui64 entity_id{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01};
    Eui48 src_mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    // Add entity
    AdpDu available{};
    available.init_entity_available(entity_id, 31);
    available.available_index = 1;
    discovery.receive_adpdu(available, src_mac, sm::TimePoint{});
    EXPECT_EQ(discovery.entity_count(), 1U);

    // Depart
    AdpDu departing{};
    departing.init_entity_departing(entity_id);
    discovery.receive_adpdu(departing, src_mac, sm::TimePoint{});

    EXPECT_EQ(discovery.entity_count(), 0U);
    EXPECT_EQ(departed.size(), 1U);
}

TEST(nanoavb_adp_discovery, discover_all_sends)
{
    std::vector<AdpDu> sent;
    AdpDiscoveryCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const& adp) {
        sent.push_back(adp);
        return true;
    };

    NanoAvbAdpDiscovery discovery{callbacks};
    discovery.discover_all();

    EXPECT_EQ(sent.size(), 1U);
    EXPECT_TRUE(sent[0].is_entity_discover());
}

TEST(nanoavb_adp_discovery, tick_expires_entities)
{
    std::vector<Eui64> departed;
    AdpDiscoveryCallbacks callbacks;
    callbacks.on_entity_available = [](DiscoveredEntity const&) {};
    callbacks.on_entity_departing = [&](Eui64 id) { departed.push_back(id); };

    NanoAvbAdpDiscovery discovery{callbacks};

    // Add entity with valid_time=1 (2 seconds)
    AdpDu adp{};
    adp.init_entity_available(Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01}, 1);
    adp.available_index = 1;
    discovery.receive_adpdu(adp, Eui48{}, sm::TimePoint{});
    EXPECT_EQ(discovery.entity_count(), 1U);

    // Tick past expiry
    auto past_expiry = sm::TimePoint{} + std::chrono::seconds(3);
    discovery.tick(past_expiry);

    EXPECT_EQ(discovery.entity_count(), 0U);
    EXPECT_EQ(departed.size(), 1U);
}

// ===========================================================================
// ADP Advertiser State Machine (adp_adv_sm)
// ===========================================================================

namespace adp_adv = statusbar::nanoavb::adp_adv_sm;

TEST(nanoavb_adp_adv_sm, uct_to_off_calls_init)
{
    sm_test::Observed<adp_adv::Def, adp_adv::table> machine;
    adp_adv::Context ctx;
    machine.handle_event(ctx, adp_adv::Def::Event::UCT, TimePoint{});
    EXPECT_EQ(machine.current_state(), adp_adv::Def::State::Off);
    EXPECT_EQ(machine.last_action, "init");
    EXPECT_FALSE(ctx.enabled);
}

TEST(nanoavb_adp_adv_sm, enable_calls_start_adp)
{
    sm_test::Observed<adp_adv::Def, adp_adv::table> machine;
    adp_adv::Context ctx;
    bool called = false;
    ctx.callbacks.start_adp = [&](adp_adv::Context&, TimePoint) { called = true; };
    machine.handle_event(ctx, adp_adv::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Enable, TimePoint{});
    EXPECT_EQ(machine.current_state(), adp_adv::Def::State::Advertising);
    EXPECT_TRUE(ctx.enabled);
    EXPECT_TRUE(called);
}

TEST(nanoavb_adp_adv_sm, disable_calls_stop_adp)
{
    sm_test::Observed<adp_adv::Def, adp_adv::table> machine;
    adp_adv::Context ctx;
    bool called = false;
    ctx.callbacks.start_adp = [](adp_adv::Context&, TimePoint) {};
    ctx.callbacks.stop_adp = [&](adp_adv::Context&, TimePoint) { called = true; };
    machine.handle_event(ctx, adp_adv::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Enable, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Disable, TimePoint{});
    EXPECT_EQ(machine.current_state(), adp_adv::Def::State::Off);
    EXPECT_FALSE(ctx.enabled);
    EXPECT_TRUE(called);
}

TEST(nanoavb_adp_adv_sm, tick_calls_maybe_announce)
{
    sm_test::Observed<adp_adv::Def, adp_adv::table> machine;
    adp_adv::Context ctx;
    bool called = false;
    ctx.callbacks.start_adp = [](adp_adv::Context&, TimePoint) {};
    ctx.callbacks.maybe_announce = [&](adp_adv::Context&, TimePoint) { called = true; };
    machine.handle_event(ctx, adp_adv::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Enable, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Tick, TimePoint{});
    EXPECT_EQ(machine.current_state(), adp_adv::Def::State::Advertising);
    EXPECT_TRUE(called);
}

TEST(nanoavb_adp_adv_sm, error_calls_stop_adp)
{
    sm_test::Observed<adp_adv::Def, adp_adv::table> machine;
    adp_adv::Context ctx;
    bool called = false;
    ctx.callbacks.start_adp = [](adp_adv::Context&, TimePoint) {};
    ctx.callbacks.stop_adp = [&](adp_adv::Context&, TimePoint) { called = true; };
    machine.handle_event(ctx, adp_adv::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Enable, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Error, TimePoint{});
    EXPECT_EQ(machine.current_state(), adp_adv::Def::State::Off);
    EXPECT_TRUE(called);
}

TEST(nanoavb_adp_adv_sm, full_cycle)
{
    sm_test::Observed<adp_adv::Def, adp_adv::table> machine;
    adp_adv::Context ctx;
    int starts = 0, stops = 0;
    ctx.callbacks.start_adp = [&](adp_adv::Context&, TimePoint) { ++starts; };
    ctx.callbacks.stop_adp = [&](adp_adv::Context&, TimePoint) { ++stops; };
    ctx.callbacks.maybe_announce = [](adp_adv::Context&, TimePoint) {};

    machine.handle_event(ctx, adp_adv::Def::Event::UCT, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Enable, TimePoint{});
    EXPECT_EQ(starts, 1);
    machine.handle_event(ctx, adp_adv::Def::Event::Tick, TimePoint{});
    machine.handle_event(ctx, adp_adv::Def::Event::Disable, TimePoint{});
    EXPECT_EQ(stops, 1);
    machine.handle_event(ctx, adp_adv::Def::Event::Enable, TimePoint{});
    EXPECT_EQ(starts, 2);
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_adp_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_adp_test");
    return result;
}
