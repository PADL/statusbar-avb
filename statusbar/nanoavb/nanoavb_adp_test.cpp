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

// IEEE 1722.1-2021 6.2.2.15: available_index "is reset to zero (0) when
// transmitting an ENTITY_DEPARTING". The entity that comes back starts a new
// availability cycle: its first ENTITY_AVAILABLE carries 0 and the count goes
// on from there, and whoever follows the index (the ENTITY descriptor) is
// told of the reset.
TEST(nanoavb_adp_advertiser, an_entity_that_departed_advertises_again_from_available_index_zero)
{
    auto entity = create_test_entity_descriptor();
    std::vector<uint32_t> available{};
    std::vector<uint32_t> told{};
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const& adpdu) {
        if (adpdu.is_entity_available()) {
            available.push_back(adpdu.available_index.get());
        }
        return true;
    };
    callbacks.on_available_index_change = [&](uint32_t const index) { told.push_back(index); };

    NanoAvbAdpAdvertiser advertiser{entity, callbacks};
    auto t = TimePoint{};
    (void)advertiser.start(t);
    t += std::chrono::seconds(65);
    advertiser.tick(t);
    t += std::chrono::seconds(65);
    advertiser.tick(t);
    EXPECT_TRUE(available.size() >= 2 && available.back() + 1 == available.size());
    EXPECT_TRUE(advertiser.available_index() >= 2);

    (void)advertiser.stop(t);
    EXPECT_EQ(advertiser.available_index(), 0U);
    EXPECT_TRUE(!told.empty() && told.back() == 0U);

    size_t const before = available.size();
    (void)advertiser.start(t + std::chrono::seconds(1));
    EXPECT_TRUE(available.size() > before && available[before] == 0U);
    EXPECT_EQ(advertiser.available_index(), 1U);
}

// A departing that could not be sent ends the cycle all the same.
TEST(nanoavb_adp_advertiser, a_departing_that_fails_to_send_still_resets_available_index)
{
    auto entity = create_test_entity_descriptor();
    bool link_up = true;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) { return link_up; };
    NanoAvbAdpAdvertiser advertiser{entity, callbacks};
    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(advertiser.available_index(), 1U);
    link_up = false;
    (void)advertiser.stop(TimePoint{});
    EXPECT_EQ(advertiser.available_index(), 0U);
}

TEST(nanoavb_adp_advertiser, stop_idempotent)
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

TEST(nanoavb_adp_advertiser, a_domain_change_alone_readvertises_after_the_delay)
{
    AdvertiserBench bench;
    ClockIdentity const gm{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0xDD, 0xEE, 0x02};
    bench.advertiser.set_gptp_info(gm, 0, bench.now);
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    bench.run_for(Ms{1'000});
    auto const sent = bench.sent.size();
    auto const changed = bench.now;
    bench.advertiser.set_gptp_info(gm, 3, bench.now);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Delay);
    auto const at = bench.run_to_next_send();
    EXPECT_TRUE(at - changed <= Ms{4'000});
    EXPECT_EQ(bench.sent.size(), sent + 1);
    EXPECT_EQ(bench.sent.back().adpdu.gptp_domain_number.get(), 3);
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

// An entity change while the link is down sends nothing, and a repeated link up while
// advertising neither restarts the delay nor sends again
TEST(nanoavb_adp_advertiser, a_change_while_down_or_a_repeated_link_up_sends_nothing)
{
    AdvertiserBench bench;
    bench.advertiser.link_down(bench.now);
    bench.advertiser.notify_entity_changed(bench.now);
    bench.run_for(Ms{10'000});
    EXPECT_EQ(bench.sent.size(), size_t{0});
    bench.advertiser.link_up(bench.now);
    (void)bench.run_to_next_send();
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
    bench.advertiser.link_up(bench.now);
    EXPECT_TRUE(bench.advertiser.state() == AdpAdvertiserState::Waiting);
    bench.run_for(Ms{4'900});
    EXPECT_EQ(bench.sent.size(), size_t{1});
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
// The Milan advertise state machine (Milan v1.3 5.6.3). Time is told, and
// the random delay is whatever the test says it is.
//

namespace {

using std::chrono::milliseconds;

/// A Milan advertiser with the times of the ENTITY_AVAILABLE and
/// ENTITY_DEPARTING messages it sent, and a TMR_DELAY the test sets.
struct MilanAdvertiser
{
    std::vector<milliseconds> available{};  ///< when each ENTITY_AVAILABLE left
    std::vector<milliseconds> departing{};
    std::vector<milliseconds> asked_longest{};  ///< the longest delay each draw was allowed
    AdpDu last{};
    milliseconds next_delay{0};
    milliseconds now{0};
    NanoAvbAdpAdvertiser advertiser{create_test_entity_descriptor()};

    MilanAdvertiser()
    {
        AdpAdvertiserCallbacks callbacks;
        callbacks.send_adpdu = [this](AdpDu const& adpdu) {
            (adpdu.is_entity_available() ? available : departing).push_back(now);
            last = adpdu;
            return true;
        };
        advertiser.set_callbacks(callbacks);
        advertiser.set_random_delay([this](milliseconds longest) {
            asked_longest.push_back(longest);
            return next_delay;
        });
        advertiser.use_milan_advertise();
    }

    [[nodiscard]] auto at() const -> TimePoint { return TimePoint{} + now; }

    /// Tick every 10 ms up to and including @p until.
    void run_until(milliseconds until)
    {
        for (; now <= until; now += milliseconds{10}) {
            advertiser.tick(at());
        }
        now = until;
    }

    void discover(Eui64 const& target = {})
    {
        AdpDu d{};
        d.init_entity_discover();
        d.entity_id = target;
        advertiser.receive_adpdu(d, at());
    }
};

}  // namespace

TEST(nanoavb_adp_milan, the_adpdu_says_valid_time_10)
{
    MilanAdvertiser m;
    EXPECT_EQ(m.advertiser.valid_time(), 10);
    EXPECT_EQ(m.advertiser.adpdu().valid_time(), 10);
    EXPECT_TRUE(m.advertiser.config().milan_advertise);
    // It stays 10 when the ADPDU is built again from the model.
    m.advertiser.notify_entity_changed();
    EXPECT_EQ(m.advertiser.adpdu().valid_time(), 10);
}

TEST(nanoavb_adp_milan, nothing_is_sent_at_start_but_after_the_startup_delay)
{
    MilanAdvertiser m;
    m.next_delay = milliseconds{1300};
    (void)m.advertiser.start(m.at());
    EXPECT_TRUE(m.available.empty());  // 5.6.3.5.2: DELAY first
    EXPECT_TRUE(m.advertiser.milan_advertise_delayed());
    // At start-up the delay is drawn from 0 to 2 s.
    EXPECT_TRUE(m.asked_longest == (std::vector<milliseconds>{MILAN_ADP_STARTUP_DELAY_MAX}));

    m.run_until(milliseconds{1290});
    EXPECT_TRUE(m.available.empty());
    m.run_until(milliseconds{1300});
    EXPECT_TRUE(m.available == (std::vector<milliseconds>{milliseconds{1300}}));
    EXPECT_FALSE(m.advertiser.milan_advertise_delayed());  // WAITING
    EXPECT_EQ(m.advertiser.available_index(), 1U);
}

TEST(nanoavb_adp_milan, advertisements_are_five_seconds_plus_the_delay_apart)
{
    MilanAdvertiser m;
    m.next_delay = milliseconds{0};
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{0});
    EXPECT_EQ(m.available.size(), size_t{1});

    // TMR_ADVERTISE is 5 s (5.6.3.5.9), then TMR_DELAY of 0 to 4 s (5.6.3.5.5).
    m.next_delay = milliseconds{2500};
    m.run_until(milliseconds{4990});
    EXPECT_FALSE(m.advertiser.milan_advertise_delayed());
    m.run_until(milliseconds{5000});
    EXPECT_TRUE(m.advertiser.milan_advertise_delayed());
    EXPECT_EQ(m.asked_longest.back(), MILAN_ADP_TMR_DELAY_MAX);
    m.run_until(milliseconds{7490});
    EXPECT_EQ(m.available.size(), size_t{1});
    m.run_until(milliseconds{7500});
    EXPECT_TRUE(m.available == (std::vector<milliseconds>{milliseconds{0}, milliseconds{7500}}));

    // And the next: 5 s after that one, plus its own delay.
    m.next_delay = milliseconds{4000};
    m.run_until(milliseconds{16490});
    EXPECT_EQ(m.available.size(), size_t{2});
    m.run_until(milliseconds{16500});
    EXPECT_EQ(m.available.back(), milliseconds{16500});
}

TEST(nanoavb_adp_milan, a_discover_is_answered_after_a_delay_and_restarts_the_five_seconds)
{
    MilanAdvertiser m;
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{0});
    EXPECT_EQ(m.available.size(), size_t{1});

    // WAITING, 2 s in: a discover for everyone (5.6.3.5.4).
    m.run_until(milliseconds{2000});
    m.next_delay = milliseconds{700};
    m.discover();
    EXPECT_EQ(m.available.size(), size_t{1});  // not at once
    EXPECT_TRUE(m.advertiser.milan_advertise_delayed());
    EXPECT_EQ(m.asked_longest.back(), MILAN_ADP_TMR_DELAY_MAX);
    m.run_until(milliseconds{2700});
    EXPECT_EQ(m.available.back(), milliseconds{2700});

    // TMR_ADVERTISE was stopped and started again at that send: nothing at
    // 5 s from the first, the next is 5 s from 2.7 s.
    m.next_delay = milliseconds{0};
    m.run_until(milliseconds{7690});
    EXPECT_EQ(m.available.size(), size_t{2});
    m.run_until(milliseconds{7700});
    EXPECT_EQ(m.available.back(), milliseconds{7700});
}

TEST(nanoavb_adp_milan, a_discover_while_one_is_owed_changes_nothing)
{
    MilanAdvertiser m;
    m.next_delay = milliseconds{1500};
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{500});
    size_t const draws = m.asked_longest.size();

    // DELAY: ignored (Table 5.56). No new draw, the owed one goes out on time.
    m.next_delay = milliseconds{4000};
    m.discover();
    m.discover(create_test_entity_descriptor().entity_id);
    EXPECT_EQ(m.asked_longest.size(), draws);
    m.run_until(milliseconds{1500});
    EXPECT_TRUE(m.available == (std::vector<milliseconds>{milliseconds{1500}}));
}

TEST(nanoavb_adp_milan, a_discover_for_another_entity_is_not_ours)
{
    MilanAdvertiser m;
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{1000});
    m.discover(Eui64{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    EXPECT_FALSE(m.advertiser.milan_advertise_delayed());
    m.discover(create_test_entity_descriptor().entity_id);  // ours by name: answered
    EXPECT_TRUE(m.advertiser.milan_advertise_delayed());
}

TEST(nanoavb_adp_milan, a_new_grandmaster_is_advertised_after_a_delay)
{
    MilanAdvertiser m;
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{1000});
    EXPECT_EQ(m.available.size(), size_t{1});

    // GM_CHANGE in WAITING (5.6.3.5.7): TMR_DELAY, then the new id goes out.
    ClockIdentity const gm{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x09};
    m.next_delay = milliseconds{900};
    m.advertiser.set_gptp_info(gm, 0);
    m.advertiser.notify_entity_changed();
    EXPECT_EQ(m.available.size(), size_t{1});  // nothing at once
    m.run_until(milliseconds{1890});
    EXPECT_EQ(m.available.size(), size_t{1});
    m.run_until(milliseconds{1910});
    EXPECT_EQ(m.available.size(), size_t{2});
    EXPECT_TRUE(m.last.gptp_grandmaster_id == gm);
}

TEST(nanoavb_adp_milan, a_change_while_one_is_owed_rides_on_it)
{
    MilanAdvertiser m;
    m.next_delay = milliseconds{1000};
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{400});
    size_t const draws = m.asked_longest.size();

    ClockIdentity const gm{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x09};
    m.advertiser.set_gptp_info(gm, 0);
    m.advertiser.notify_entity_changed();
    m.run_until(milliseconds{1000});
    EXPECT_EQ(m.asked_longest.size(), draws);  // GM_CHANGE in DELAY: ignored
    EXPECT_EQ(m.available.size(), size_t{1});
    EXPECT_TRUE(m.last.gptp_grandmaster_id == gm);  // and the one owed says it
    // It does not ask for another once that one is out.
    m.run_until(milliseconds{5900});
    EXPECT_EQ(m.available.size(), size_t{1});
}

TEST(nanoavb_adp_milan, departing_goes_out_from_either_state_and_ends_it)
{
    MilanAdvertiser m;
    m.next_delay = milliseconds{1500};
    (void)m.advertiser.start(m.at());
    m.run_until(milliseconds{300});
    (void)m.advertiser.stop(m.at());  // DELAY / SHUTDOWN, 5.6.3.5.11
    EXPECT_EQ(m.departing.size(), size_t{1});
    m.run_until(milliseconds{9000});
    EXPECT_TRUE(m.available.empty());  // the owed one is not sent after a shutdown

    // A start after that is a LINK_UP: the delay is drawn from 0 to 4 s (5.6.3.5.3).
    m.next_delay = milliseconds{100};
    (void)m.advertiser.start(m.at());
    EXPECT_EQ(m.asked_longest.back(), MILAN_ADP_TMR_DELAY_MAX);
    m.run_until(milliseconds{9100});
    EXPECT_EQ(m.available.size(), size_t{1});
    (void)m.advertiser.stop(m.at());  // WAITING / SHUTDOWN, 5.6.3.5.8
    EXPECT_EQ(m.departing.size(), size_t{2});
}

TEST(nanoavb_adp_milan, a_draw_outside_its_range_is_brought_inside)
{
    MilanAdvertiser m;
    m.next_delay = milliseconds{60'000};
    (void)m.advertiser.start(m.at());
    m.run_until(MILAN_ADP_STARTUP_DELAY_MAX);
    EXPECT_TRUE(m.available == (std::vector<milliseconds>{MILAN_ADP_STARTUP_DELAY_MAX}));
}

TEST(nanoavb_adp_milan, our_own_draws_stay_in_range_and_differ_between_entities)
{
    // No set_random_delay(): the generator seeded from the entity ID.
    auto const first_times = [](Eui64 const& id) {
        auto entity = create_test_entity_descriptor();
        entity.entity_id = id;
        std::vector<milliseconds> sent;
        milliseconds now{0};
        AdpAdvertiserCallbacks callbacks;
        callbacks.send_adpdu = [&](AdpDu const& adpdu) {
            if (adpdu.is_entity_available()) {
                sent.push_back(now);
            }
            return true;
        };
        NanoAvbAdpAdvertiser advertiser{entity, callbacks};
        advertiser.use_milan_advertise();
        (void)advertiser.start(TimePoint{});
        for (; now <= milliseconds{60'000}; now += milliseconds{10}) {
            advertiser.tick(TimePoint{} + now);
        }
        return sent;
    };
    auto const a = first_times(Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07});
    auto const b = first_times(Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08});
    EXPECT_TRUE(a.size() >= 6 && b.size() >= 6);
    EXPECT_TRUE(a.front() <= MILAN_ADP_STARTUP_DELAY_MAX);
    for (size_t i = 1; i < a.size(); ++i) {
        auto const gap = a[i] - a[i - 1];
        EXPECT_TRUE(gap >= MILAN_TMR_ADVERTISE);
        EXPECT_TRUE(gap <= MILAN_TMR_ADVERTISE + MILAN_ADP_TMR_DELAY_MAX + milliseconds{20});
    }
    EXPECT_TRUE(a != b);  // two entities do not advertise in step
}

TEST(nanoavb_adp_milan, an_entity_that_does_not_ask_for_it_advertises_as_before)
{
    auto entity = create_test_entity_descriptor();
    int sent = 0;
    AdpAdvertiserCallbacks callbacks;
    callbacks.send_adpdu = [&](AdpDu const&) {
        ++sent;
        return true;
    };
    NanoAvbAdpAdvertiser advertiser{entity, callbacks};
    EXPECT_FALSE(advertiser.config().milan_advertise);
    EXPECT_EQ(advertiser.valid_time(), 31);
    (void)advertiser.start(TimePoint{});
    EXPECT_EQ(sent, 1);  // at once
    EXPECT_FALSE(advertiser.milan_advertise_delayed());
    AdpDu discover{};
    discover.init_entity_discover();
    advertiser.receive_adpdu(discover, TimePoint{});
    EXPECT_EQ(sent, 2);  // at once
    advertiser.notify_entity_changed();
    EXPECT_EQ(sent, 3);  // at once
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
    callbacks.on_entity_departing = [&](Eui64 id, statusbar::sm::TimePoint) { departed.push_back(id); };

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
    callbacks.on_entity_departing = [&](Eui64 id, statusbar::sm::TimePoint) { departed.push_back(id); };

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
