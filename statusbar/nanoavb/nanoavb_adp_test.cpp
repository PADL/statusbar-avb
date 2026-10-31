// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/sm/sm_test_support.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <system_error>
#include <utility>
#include <vector>

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

    EXPECT_TRUE(advertiser.state() == AdpAdvertiserState::Start);
    EXPECT_EQ(advertiser.available_index(), 0);
    EXPECT_EQ(advertiser.valid_time(), 10);  // Milan v1.2 Section 5.6.2
    EXPECT_EQ(advertiser.config().reannounce_interval, std::chrono::milliseconds{5000});
    // Milan v1.2 Section 5.6.2: AEM_INTERFACE_INDEX_VALID set, interface_index 0.
    EXPECT_TRUE(advertiser.adpdu().has_entity_capability(statusbar::atdecc::entity_capabilities::AEM_INTERFACE_INDEX_VALID));
    EXPECT_EQ(advertiser.adpdu().interface_index.get(), 0);
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
    // The model's capabilities, plus AEM_INTERFACE_INDEX_VALID (the advertiser
    // always populates interface_index, Milan v1.2 Section 5.6.2).
    EXPECT_EQ(
        adpdu.entity_capabilities.get(),
        entity.entity_capabilities.get() | statusbar::atdecc::entity_capabilities::AEM_INTERFACE_INDEX_VALID);
    EXPECT_EQ(adpdu.talker_stream_sources.get(), entity.talker_stream_sources.get());
    EXPECT_EQ(adpdu.talker_capabilities.get(), entity.talker_capabilities.get());
    EXPECT_EQ(adpdu.listener_stream_sinks.get(), entity.listener_stream_sinks.get());
    EXPECT_EQ(adpdu.listener_capabilities.get(), entity.listener_capabilities.get());
}

//
// NanoAvbAdpAdvertiser State Machine Tests - Milan v1.2 Section 5.6.3
//
namespace {

using Ms = std::chrono::milliseconds;
using AdvEvent = adp_adv_sm::Def::Event;

constexpr Eui48 TEST_MAC{0x00, 0x1B, 0x92, 0x00, 0x00, 0x01};

/// An advertiser on a fake 1 ms clock with a fixed seed, recording what it sends.
struct AdvertiserBench
{
    struct Sent
    {
        TimePoint at;
        AdpDu adpdu;
    };

    explicit AdvertiserBench(uint64_t const seed = 1)
        : advertiser{create_test_entity_descriptor(), AdpAdvertiserCallbacks{.send_adpdu = [this](AdpDu const& du) {
                         sent.push_back({now, du});
                         return true;
                     }}}
    {
        advertiser.seed_random(TEST_MAC, seed);
    }

    void run_for(Ms const span)
    {
        auto const until = now + span;
        while (now < until) {
            now += Ms{1};
            advertiser.tick(now);
        }
    }

    /// Run until the next frame is sent (at most @p limit); returns its time.
    auto run_to_next_send(Ms const limit = Ms{20'000}) -> TimePoint
    {
        auto const before = sent.size();
        auto const until = now + limit;
        while (sent.size() == before && now < until) {
            now += Ms{1};
            advertiser.tick(now);
        }
        return sent.size() > before ? sent.back().at : TimePoint::max();
    }

    [[nodiscard]] auto since(TimePoint const t) const -> Ms { return std::chrono::duration_cast<Ms>(now - t); }

    void discover(Eui64 const& id = Eui64{})
    {
        AdpDu du{};
        du.init_entity_discover();
        du.entity_id = id;
        advertiser.receive_adpdu(du, now);
    }

    [[nodiscard]] auto count(bool const departing) const -> size_t
    {
        return static_cast<size_t>(
            std::ranges::count_if(sent, [&](Sent const& s) { return s.adpdu.is_entity_departing() == departing; }));
    }

    std::vector<Sent> sent;
    TimePoint now{std::chrono::seconds{100}};
    NanoAvbAdpAdvertiser advertiser;
};

}  // namespace

TEST(nanoavb_adp_advertiser, nothing_is_sent_before_the_link_is_up)
{
    AdvertiserBench bench;
    bench.run_for(Ms{30'000});
    bench.discover();
    bench.run_for(Ms{5'000});
    EXPECT_EQ(bench.sent.size(), size_t{0});
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Start);
}

TEST(nanoavb_adp_advertiser, startup_with_the_link_up_advertises_within_two_seconds)
{
    Ms lo{Ms::max()};
    Ms hi{0};
    for (uint64_t seed = 1; seed <= 64; ++seed) {
        AdvertiserBench bench{seed};
        auto const start = bench.now;
        bench.advertiser.link_up(start);
        EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Delay);
        auto const at = bench.run_to_next_send();
        auto const delay = std::chrono::duration_cast<Ms>(at - start);
        EXPECT_TRUE(delay <= Ms{2'000});
        lo = std::min(lo, delay);
        hi = std::max(hi, delay);
        EXPECT_TRUE(bench.sent.front().adpdu.is_entity_available());
    }
    EXPECT_TRUE(lo < Ms{500});
    EXPECT_TRUE(hi > Ms{1'500});
}

TEST(nanoavb_adp_advertiser, link_up_after_down_advertises_within_four_seconds)
{
    Ms hi{0};
    for (uint64_t seed = 1; seed <= 64; ++seed) {
        AdvertiserBench bench{seed};
        bench.advertiser.link_down(bench.now);  // started with the link down
        EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Down);
        auto const start = bench.now;
        bench.advertiser.link_up(start);
        auto const delay = std::chrono::duration_cast<Ms>(bench.run_to_next_send() - start);
        EXPECT_TRUE(delay <= Ms{4'000});
        hi = std::max(hi, delay);
    }
    EXPECT_TRUE(hi > Ms{3'000});
}

TEST(nanoavb_adp_advertiser, the_period_is_five_seconds_plus_zero_to_four)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    auto last = bench.run_to_next_send();
    Ms lo{Ms::max()};
    Ms hi{0};
    for (int i = 0; i < 100; ++i) {
        auto const at = bench.run_to_next_send();
        auto const gap = std::chrono::duration_cast<Ms>(at - last);
        EXPECT_TRUE(gap >= Ms{5'000} && gap <= Ms{9'000});
        lo = std::min(lo, gap);
        hi = std::max(hi, gap);
        last = at;
    }
    EXPECT_TRUE(lo < Ms{5'500});
    EXPECT_TRUE(hi > Ms{8'500});
}

TEST(nanoavb_adp_advertiser, a_discover_is_answered_within_four_seconds_and_restarts_the_period)
{
    for (uint64_t seed = 1; seed <= 32; ++seed) {
        AdvertiserBench bench{seed};
        bench.advertiser.link_up(bench.now);
        (void)bench.run_to_next_send();
        bench.run_for(Ms{4'900});  // TMR_ADVERTISE is 100 ms away
        auto const asked = bench.now;
        bench.discover();
        EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Delay);
        auto const reply = bench.run_to_next_send();
        EXPECT_TRUE(reply - asked <= Ms{4'000});
        auto const next = bench.run_to_next_send();
        EXPECT_TRUE(next - reply >= Ms{5'000} && next - reply <= Ms{9'000});
    }
}

TEST(nanoavb_adp_advertiser, a_discover_for_us_by_id_is_answered_another_is_not)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    bench.discover(Eui64{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
    bench.discover(create_test_entity_descriptor().entity_id);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Delay);
}

TEST(nanoavb_adp_advertiser, discovers_are_ignored_in_delay)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    bench.run_for(Ms{1'000});
    bench.discover();
    auto const sent = bench.sent.size();
    for (int i = 0; i < 20 && bench.advertiser.state() == AdpAdvertiserState::Delay; ++i) {
        bench.discover();
        bench.run_for(Ms{1});
    }
    (void)bench.run_to_next_send();
    EXPECT_EQ(bench.sent.size(), sent + 1);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
}

TEST(nanoavb_adp_advertiser, a_grandmaster_change_readvertises_after_the_delay_with_its_domain)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    bench.run_for(Ms{1'000});
    ClockIdentity const gm{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0x01};
    auto const changed = bench.now;
    bench.advertiser.set_gptp_info(gm, 7, bench.now);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Delay);
    auto const at = bench.run_to_next_send();
    EXPECT_TRUE(at - changed <= Ms{4'000});
    EXPECT_TRUE(bench.sent.back().adpdu.gptp_grandmaster_id == gm);
    EXPECT_EQ(bench.sent.back().adpdu.gptp_domain_number.get(), 7);
    // The same grandmaster again is no GM_CHANGE.
    bench.advertiser.set_gptp_info(gm, 7, bench.now);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
}

TEST(nanoavb_adp_advertiser, a_grandmaster_change_in_delay_rides_the_pending_send)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    ClockIdentity const gm{0x01, 0x02, 0x03, 0xFF, 0xFE, 0x04, 0x05, 0x06};
    bench.advertiser.set_gptp_info(gm, 0, bench.now);
    (void)bench.run_to_next_send();
    EXPECT_EQ(bench.sent.size(), size_t{1});
    EXPECT_TRUE(bench.sent.back().adpdu.gptp_grandmaster_id == gm);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
}

TEST(nanoavb_adp_advertiser, an_entity_change_readvertises_after_the_delay)
{
    AdvertiserBench bench;
    bench.advertiser.notify_entity_changed(bench.now);  // link not up: nothing
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    bench.run_for(Ms{1'000});
    auto const changed = bench.now;
    bench.advertiser.notify_entity_changed(bench.now);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Delay);
    EXPECT_TRUE(bench.run_to_next_send() - changed <= Ms{4'000});
}

TEST(nanoavb_adp_advertiser, link_down_sends_no_departing)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    bench.advertiser.link_down(bench.now);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Down);
    bench.run_for(Ms{30'000});
    EXPECT_EQ(bench.sent.size(), size_t{1});
    EXPECT_EQ(bench.count(true), size_t{0});
    // From DELAY too: the pending send is dropped.
    bench.advertiser.link_up(bench.now);
    bench.advertiser.link_down(bench.now);
    bench.run_for(Ms{30'000});
    EXPECT_EQ(bench.sent.size(), size_t{1});
}

TEST(nanoavb_adp_advertiser, available_index_advances_once_per_entity_available)
{
    uint32_t notified = 0;
    AdvertiserBench bench;
    auto callbacks = AdpAdvertiserCallbacks{
        .send_adpdu =
            [&bench](AdpDu const& du) {
                bench.sent.push_back({bench.now, du});
                return true;
            },
        .on_available_index_change = [&notified](uint32_t const index) { notified = index; }};
    bench.advertiser.set_callbacks(std::move(callbacks));
    bench.advertiser.link_up(bench.now);
    for (int i = 0; i < 4; ++i) {
        (void)bench.run_to_next_send();
    }
    bench.discover();
    (void)bench.run_to_next_send();
    EXPECT_EQ(bench.sent.size(), size_t{5});
    for (size_t i = 0; i < bench.sent.size(); ++i) {
        EXPECT_EQ(bench.sent[i].adpdu.available_index.get(), static_cast<uint32_t>(i));
    }
    EXPECT_EQ(bench.advertiser.available_index(), 5U);
    EXPECT_EQ(notified, 5U);
}

TEST(nanoavb_adp_advertiser, a_reply_outside_the_state_machine_advances_available_index)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    std::vector<uint32_t> replied;
    EXPECT_TRUE(bench.advertiser.reply_available([&](AdpDu const& du) {
        replied.push_back(du.available_index.get());
        return du.is_entity_available();
    }));
    EXPECT_FALSE(bench.advertiser.reply_available([](AdpDu const&) { return false; }));  // not sent: no advance
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
    (void)bench.run_to_next_send();
    EXPECT_EQ(replied.size(), size_t{1});
    EXPECT_EQ(replied.front(), 1U);
    EXPECT_EQ(bench.sent.back().adpdu.available_index.get(), 2U);
}

TEST(nanoavb_adp_advertiser, a_reply_leaves_the_network_failure_count_alone)
{
    AdvertiserBench bench;
    bench.advertiser.set_callbacks(AdpAdvertiserCallbacks{.send_adpdu = [](AdpDu const&) { return false; }});
    bench.advertiser.link_up(bench.now);
    bench.run_for(Ms{2'001});
    EXPECT_EQ(bench.advertiser.consecutive_send_failures(), 1U);
    EXPECT_TRUE(bench.advertiser.reply_available([](AdpDu const&) { return true; }));
    EXPECT_EQ(bench.advertiser.consecutive_send_failures(), 1U);  // a sent reply clears no streak
    EXPECT_FALSE(bench.advertiser.reply_available([](AdpDu const&) { return false; }));
    EXPECT_EQ(bench.advertiser.consecutive_send_failures(), 1U);  // a gone local client is no failure
    EXPECT_EQ(bench.advertiser.available_index(), 1U);
}

TEST(nanoavb_adp_advertiser, shutdown_departs_and_resets_available_index)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    (void)bench.run_to_next_send();
    EXPECT_EQ(bench.advertiser.available_index(), 2U);
    EXPECT_TRUE(bench.advertiser.shutdown(bench.now));
    EXPECT_EQ(bench.count(true), size_t{1});
    EXPECT_EQ(bench.sent.back().adpdu.available_index.get(), 0U);
    EXPECT_EQ(bench.advertiser.available_index(), 0U);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Start);
    // Restarted: the next ENTITY_AVAILABLE starts again at 0, after the startup delay.
    auto const start = bench.now;
    bench.advertiser.link_up(bench.now);
    EXPECT_TRUE(bench.run_to_next_send() - start <= Ms{2'000});
    EXPECT_EQ(bench.sent.back().adpdu.available_index.get(), 0U);
}

TEST(nanoavb_adp_advertiser, shutdown_from_delay_departs_without_the_pending_send)
{
    AdvertiserBench bench;
    bench.advertiser.link_up(bench.now);
    EXPECT_TRUE(bench.advertiser.shutdown(bench.now));
    bench.run_for(Ms{10'000});
    EXPECT_EQ(bench.sent.size(), size_t{1});
    EXPECT_EQ(bench.count(true), size_t{1});
}

TEST(nanoavb_adp_advertiser, shutdown_with_the_link_down_sends_nothing)
{
    AdvertiserBench bench;
    bench.advertiser.link_down(bench.now);
    EXPECT_FALSE(bench.advertiser.shutdown(bench.now));
    EXPECT_EQ(bench.sent.size(), size_t{0});
    // Ignored while down: the next link up is from DOWN, 0-4 s (Section 5.6.3.5.3).
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Down);
}

TEST(nanoavb_adp_advertiser, a_failed_send_does_not_advance_available_index)
{
    AdvertiserBench bench;
    bench.advertiser.set_callbacks(AdpAdvertiserCallbacks{.send_adpdu = [](AdpDu const&) { return false; }});
    bench.advertiser.link_up(bench.now);
    bench.run_for(Ms{2'001});
    EXPECT_EQ(bench.advertiser.available_index(), 0U);
    EXPECT_EQ(bench.advertiser.consecutive_send_failures(), 1U);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
}

TEST(nanoavb_adp_advertiser, the_delay_range_follows_valid_time)
{
    AdpAdvertiserConfig config{};
    EXPECT_TRUE(config.delay_max() == Ms{4'000});  // Milan v1.2 Table 5.50
    config.valid_time = 31;
    EXPECT_TRUE(config.delay_max() == Ms{12'400});  // IEEE 1722.1-2021 Clause 6.2.4.2.2
}

TEST(nanoavb_adp_delay_random, a_seed_from_the_mac_and_clock_fixes_the_sequence)
{
    adp_adv_sm::DelayRandom a{};
    adp_adv_sm::DelayRandom b{};
    adp_adv_sm::DelayRandom c{};
    a.seed(TEST_MAC, 1'700'000'000'000'000'000ULL);
    b.seed(TEST_MAC, 1'700'000'000'000'000'000ULL);
    c.seed(Eui48{0x00, 0x1B, 0x92, 0x00, 0x00, 0x02}, 1'700'000'000'000'000'000ULL);
    int same = 0;
    for (int i = 0; i < 16; ++i) {
        auto const x = a.next(Ms{4'000});
        EXPECT_TRUE(x == b.next(Ms{4'000}));
        same += x == c.next(Ms{4'000}) ? 1 : 0;
    }
    EXPECT_TRUE(same < 4);
}

TEST(nanoavb_adp_delay_random, delays_stay_within_the_range)
{
    adp_adv_sm::DelayRandom r{};
    r.seed(TEST_MAC, 0);
    for (int i = 0; i < 1000; ++i) {
        auto const d = r.next(Ms{4'000});
        EXPECT_TRUE(d >= Ms{0} && d <= Ms{4'000});
    }
    EXPECT_TRUE(r.next(Ms{0}) == Ms{0});
}

TEST(nanoavb_adp_adv_sm, table_5_51_ignores_and_impossible_events_leave_the_state)
{
    sm_test::Observed<adp_adv_sm::Def, adp_adv_sm::table> machine;
    adp_adv_sm::Context ctx;
    machine.handle_event(ctx, AdvEvent::LinkDown, TimePoint{});
    EXPECT_TRUE(machine.current_state() == AdpAdvertiserState::Down);
    for (auto const e : {AdvEvent::RcvDiscover, AdvEvent::GmChange, AdvEvent::Shutdown, AdvEvent::TmrDelay}) {
        machine.handle_event(ctx, e, TimePoint{});
        EXPECT_TRUE(machine.current_state() == AdpAdvertiserState::Down);
    }
    EXPECT_FALSE(ctx.do_advertise || ctx.do_terminate);
}

//
// NanoAvbAdpAdvertiser Configuration Tests
//
TEST(nanoavb_adp_advertiser, set_gptp_info)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    ClockIdentity gm_id{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    advertiser.set_gptp_info(gm_id, 0, TimePoint{});

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
    advertiser.notify_entity_changed(TimePoint{});
    EXPECT_EQ(advertiser.adpdu().identify_control_index.get(), 123);
    EXPECT_TRUE(advertiser.adpdu().has_entity_capability(statusbar::atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID));
}

TEST(nanoavb_adp_advertiser, update_config)
{
    auto entity = create_test_entity_descriptor();
    NanoAvbAdpAdvertiser advertiser{entity};

    EXPECT_EQ(advertiser.valid_time(), 10);

    AdpAdvertiserConfig new_config;
    new_config.valid_time = 15;
    new_config.reannounce_interval = std::chrono::milliseconds{2000};

    advertiser.set_config(new_config);

    EXPECT_EQ(advertiser.valid_time(), 15);
    EXPECT_EQ(advertiser.adpdu().valid_time(), 15);
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
