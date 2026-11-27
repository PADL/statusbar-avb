// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit's talker streams and the SR class domain: ACMP's picture of each
// stream and the MSRP Talker Advertise carry the VLAN and priority the
// frames are tagged with, configured or adopted from the wire (Milan v1.3
// 4.2.7.2.1), never the headers' defaults.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_test_port.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using statusbar::nanoavb::DomainInfo;
using TimePoint = statusbar::sm::TimePoint;

namespace {

auto load_file(std::filesystem::path const& path) -> std::vector<uint8_t>
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Talker-only descriptor blob (committed fixture; source: examples/tone.json).
auto entity_tone_bin_path() -> std::filesystem::path
{
    return std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone.bin";
}

/// The reactor's clock for a kit started in a test: deterministic, 1 ms a read.
struct FakeClock
{
    std::atomic<int64_t>* now_ns;
    auto operator()() noexcept -> int64_t { return now_ns->fetch_add(1'000'000LL); }
};

/// A kit over the tone blob, the way the tone generator builds its own, with
/// the collaborators the kit binds declared before it. start() brings up its
/// control plane on an interface that does not exist: every socket fails to
/// open (the handlers say so and carry on), so no frame leaves the test, and
/// the protocol handlers are fed by hand.
class KitUnderTest
{
  public:
    explicit KitUnderTest(AvbEntityAudioIOConfig config)
        : config_{std::move(config)}
        , storage_{*atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{config_.descriptor_storage_blob})}
        , handler_{storage_, config_.entity_id, config_.entity_model_id, "1.0", "kit test", std::nullopt, true}
        , talker_specs_{*talker_stream_specs(storage_, 0)}
        , listener_specs_{*listener_stream_specs(storage_, 0)}
        , kit_{
              config_,
              handler_,
              talker_specs_,
              listener_specs_,
              48000,
              0,
              std::nullopt,
              media_clock_,
              last_gptp_ns_,
              nullptr,
              std::pmr::get_default_resource()}
    {}

    auto operator->() noexcept -> AvbEntityKit* { return &kit_; }

    /// The ENTITY descriptor's entity_capabilities, as a controller reads them.
    [[nodiscard]] auto entity_capabilities() -> uint32_t
    {
        atdecc::aem::DescriptorEntity entity{};
        nanoavb::DescriptorId const id{
            .ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0},
            .symbol = 0};
        return handler_.on_get_entity(id, entity) ? entity.entity_capabilities.get() : 0;
    }
    [[nodiscard]] auto talker_specs() const noexcept -> StreamSpecs const& { return talker_specs_; }
    /// The media clock the kit's talker timestamps from, driven by hand as the media thread would.
    [[nodiscard]] auto media_clock() noexcept -> ptpclient::MediaClockGenerator& { return media_clock_; }

    /// Bring the control plane up (or up again), as the tool does at @p now.
    [[nodiscard]] auto start(TimePoint const now) -> bool { return kit_.start(reactor_, now).has_value(); }
    [[nodiscard]] auto stop(TimePoint const now) -> bool { return kit_.stop(now).has_value(); }

  private:
    AvbEntityAudioIOConfig config_;
    itc::StopToken stop_{};
    std::atomic<int64_t> now_ns_{1};
    net::MessageReactor reactor_{stop_, FakeClock{&now_ns_}, 5};
    atdecc::aem::DescriptorStorage storage_;
    EntityIdentityDescriptorHandler handler_;
    StreamSpecs talker_specs_;
    StreamSpecs listener_specs_;
    ptpclient::MediaClockGenerator media_clock_;
    std::atomic<int64_t> last_gptp_ns_{0};
    AvbEntityKit kit_;
};

auto make_config(uint16_t const vlan_id, std::string_view const profile, bool const milan = false) -> AvbEntityAudioIOConfig
{
    AvbEntityAudioIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.descriptor_storage_blob = load_file(entity_tone_bin_path());
    config.vlan_id = vlan_id;
    config.atdecc_profile = std::string{profile};
    config.milan = milan;
    config.interface_name = "no-such-interface-0";
    return config;
}

using Profile = nanoavb::AemCommandHandler::Profile;

auto profile_of(KitUnderTest& kit) -> Profile
{
    return kit->host().components().aem_handler.profile();
}

/// The entity_capabilities @p kit advertises in ADP.
auto advertised_capabilities(KitUnderTest& kit) -> uint32_t
{
    return kit->host().components().adp_advertiser.adpdu().entity_capabilities.get();
}

auto milan_info_of(KitUnderTest& kit) -> std::optional<nanoavb::AemCommandHandler::MilanInfo> const&
{
    return kit->host().components().aem_handler.milan_info();
}

/// A bridge on the wire declaring a Class A Domain, its PDUs handed to the
/// kit's MSRP handler.
void bridge_declares_domain(KitUnderTest& kit, DomainInfo const& domain)
{
    std::vector<std::vector<uint8_t>> out;
    nanoavb::MsrpHandler<> bridge{
        statusbar::srp::msrp::MsrpConfig{}, nanoavb::MsrpCallbacks{.send_packet = [&out](std::span<uint8_t const> p) {
            out.emplace_back(p.begin(), p.end());
            return true;
        }}};
    bridge.set_domain(domain);
    TimePoint now{std::chrono::seconds{1}};
    (void)bridge.declare_domain(now);
    for (int round = 0; round < 6; ++round) {
        now += std::chrono::milliseconds{120};
        bridge.tick(now);
        kit->host().components().msrp_handler.tick(now);
        for (auto const& p : out) {
            kit->host().components().msrp_handler.receive_packet(p, now);
        }
        out.clear();
    }
}

}  // namespace

TEST(avb_entity_kit_domain, acmp_and_the_talker_advertise_carry_the_configured_vlan)
{
    // VLAN 7, not the headers' default of 2: what ACMP answers (and
    // GET_STREAM_INFO shows) and what the Talker Advertise declares.
    KitUnderTest kit{make_config(7, "classic")};
    EXPECT_FALSE(kit.talker_specs().empty());
    for (auto const& spec : kit.talker_specs()) {
        auto const stream = kit->host().components().talker_stream(spec.index);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
            EXPECT_EQ(stream->stream_vlan_id, 7U);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 7U);
        EXPECT_EQ(info.priority, 3U);
    }
    EXPECT_EQ(kit->talker().vlan_id(), 7U);
}

TEST(avb_entity_kit_domain, an_adopted_domain_moves_acmp_and_the_talker_advertise_with_the_frames)
{
    // Milan v1.3 4.2.7.2.1: the bridge's Class A Domain (priority 5, VLAN 5)
    // replaces ours. The frames moved already; ACMP's picture and the
    // Talker Advertise must say the same, or a listener reserves on VLAN 7
    // priority 3 for frames that arrive on VLAN 5 priority 5.
    KitUnderTest kit{make_config(7, "milan13")};
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});

    auto const& domain = kit->host().components().msrp_handler.domain();
    EXPECT_EQ(domain.sr_class_vid, 5U);
    EXPECT_EQ(domain.sr_class_priority, 5U);
    EXPECT_EQ(kit->talker().vlan_id(), 5U);
    EXPECT_EQ(kit->talker().stream_pcp(), 5U);
    for (auto const& spec : kit.talker_specs()) {
        auto const stream = kit->host().components().talker_stream(spec.index);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
            EXPECT_EQ(stream->stream_vlan_id, 5U);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 5U);
        EXPECT_EQ(info.priority, 5U);
    }
}

// A data plane beside the port follows each stream's frames: a Domain adopted from the wire (Milan v1.3
// 4.2.7.2.1) moves every stream's tag, a MAAP block (IEEE 1722-2025 Annex B) the destinations it gives.
TEST(avb_entity_kit_domain, an_adopted_domain_and_a_maap_block_are_told_as_changes_of_the_frames)
{
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    std::vector<uint16_t> told;
    kit->talker().set_on_frames_changed([&told](uint16_t const index) { told.push_back(index); });
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    std::vector<uint16_t> every{};
    for (auto const& spec : kit.talker_specs()) {
        every.push_back(spec.index);
    }
    EXPECT_TRUE(told == every);

    told.clear();
    std::array<uint16_t, 1> const indices{every.front()};
    Eui48 const block{0x91, 0xE0, 0xF0, 0x00, 0x56, 0x78};
    MaapAddressAcquirer::publish_block(kit->host().components(), kit->talker(), indices, block);
    EXPECT_TRUE(told == std::vector<uint16_t>{every.front()});
    MaapAddressAcquirer::publish_block(kit->host().components(), kit->talker(), indices, block);  // the same block
    EXPECT_EQ(told.size(), size_t{1});
    kit->talker().set_on_frames_changed({});
}

TEST(avb_entity_kit_domain, a_classic_kit_keeps_its_configured_domain)
{
    KitUnderTest kit{make_config(7, "classic")};
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    EXPECT_EQ(kit->talker().vlan_id(), 7U);
    for (auto const& spec : kit.talker_specs()) {
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 7U);
        EXPECT_EQ(info.priority, 3U);
    }
}

TEST(avb_entity_kit_maap, the_range_file_holds_one_line_and_reads_back_what_was_written)
{
    SavedMaapRange const range{.start = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34}, .count = 3};
    EXPECT_EQ(encode_maap_range(range), std::string{"91e0f0001234 3\n"});
    EXPECT_TRUE(decode_maap_range("91e0f0001234 3\n") == std::optional{range});
    EXPECT_TRUE(decode_maap_range("91e0f0001234 3") == std::optional{range});  // the newline is a courtesy
    EXPECT_FALSE(decode_maap_range("").has_value());
    EXPECT_FALSE(decode_maap_range("91e0f0001234").has_value());      // no count
    EXPECT_FALSE(decode_maap_range("91e0f0001234 0\n").has_value());  // a count of 0
    EXPECT_FALSE(decode_maap_range("91e0f0001234 x\n").has_value());
    EXPECT_FALSE(decode_maap_range("91e0f000123g 3\n").has_value());  // not hex
    EXPECT_FALSE(decode_maap_range("001122334455 3\n").has_value());  // outside the dynamic pool
    EXPECT_FALSE(decode_maap_range("91e0f000fdff 2\n").has_value());  // runs off the pool's end
    EXPECT_TRUE(decode_maap_range("91e0f000fdff 1\n").has_value());

    auto const dir = std::filesystem::temp_directory_path() / "statusbar-maap-range-test";
    std::filesystem::create_directories(dir);
    auto const path = (dir / "range").string();
    std::filesystem::remove(path);
    EXPECT_FALSE(load_maap_range(path).has_value());  // no file: a fresh draw
    EXPECT_TRUE(save_maap_range(path, range).has_value());
    EXPECT_TRUE(load_maap_range(path) == std::optional{range});
    SavedMaapRange const moved{.start = Eui48{0x91, 0xE0, 0xF0, 0x00, 0xAB, 0xCD}, .count = 2};
    EXPECT_TRUE(save_maap_range(path, moved).has_value());  // replaced, not appended
    EXPECT_TRUE(load_maap_range(path) == std::optional{moved});
    {
        std::ofstream damaged{path, std::ios::trunc};
        damaged << "not a range\n";
    }
    EXPECT_FALSE(load_maap_range(path).has_value());
    std::filesystem::remove_all(dir);
}

// IEEE 1722-2025 Annex B.3: the streams have their addresses while the block is defended; each change of
// that is told on the reactor, and the addresses of a block defended are the streams' before it is told.
TEST(avb_entity_kit_maap, ready_is_told_at_each_change_and_only_then)
{
    constexpr int64_t SEC = 1'000'000'000;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    itc::StopToken stop{};
    std::atomic<int64_t> now_ns{1};
    net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    Eui48 const mac{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    std::array<uint16_t, 1> const indices{1};
    MaapAddressAcquirer maap;
    std::vector<bool> told;
    maap.set_on_ready_changed([&told, &kit](bool const ready) {
        told.push_back(ready);
        auto const dest = kit->talker().slot_for(1)->dest_mac.to_uint64();
        bool const in_pool = dest >= avtp::MAAP_DYNAMIC_POOL_START.to_uint64() && dest <= avtp::MAAP_DYNAMIC_POOL_END.to_uint64();
        if (ready) {
            EXPECT_TRUE(in_pool);
        }
    });
    // No MAAP socket here: the static addresses stand and the streams stay ready, which is no change.
    EXPECT_TRUE(maap.setup(
                        reactor,
                        "no-such-interface-0",
                        mac,
                        kit->host().components(),
                        kit->talker(),
                        indices,
                        kit->host().ctl_log(),
                        {},
                        TimePoint{})
                    .has_value());
    EXPECT_TRUE(told.empty());

    avtp::MaapHandler handler{mac, 1};
    handler.set_send([](Eui48 const&, std::span<uint8_t const>) { return true; });
    maap.wire_handler(handler, kit->host().components(), kit->talker());
    handler.prepare(1);
    auto defend = [&handler](int64_t const from_ns) {
        handler.port_operational(from_ns);
        for (int k = 1; k <= 12 && !handler.is_acquired(); ++k) {
            handler.tick(from_ns + (k * SEC));
        }
    };
    defend(SEC);
    EXPECT_TRUE(told.empty());  // defended while ready: no change
    handler.release(20 * SEC);
    EXPECT_TRUE(told == std::vector<bool>{false});
    EXPECT_FALSE(maap.ready());
    defend(30 * SEC);
    EXPECT_TRUE((told == std::vector<bool>{false, true}));

    maap.set_on_ready_changed({});
    handler.release(60 * SEC);
    EXPECT_EQ(told.size(), size_t{2});
}

TEST(avb_entity_kit_maap, the_seed_mixes_the_mac_with_the_real_time_clock)
{
    // IEEE 1722-2025 B.3.6.1: a station draws a different sequence after every
    // boot, and two stations booted at one instant draw different ones.
    Eui48 const mac_a{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    Eui48 const mac_b{0x00, 0x11, 0x22, 0x33, 0x44, 0x56};
    EXPECT_NE(maap_seed(mac_a, 1'700'000'000'000'000'000LL), maap_seed(mac_a, 1'700'000'001'000'000'000LL));
    EXPECT_NE(maap_seed(mac_a, 1'700'000'000'000'000'000LL), maap_seed(mac_b, 1'700'000'000'000'000'000LL));
    EXPECT_EQ(maap_seed(mac_a, 0), mac_a.to_uint64());
    // Never 0, which the handler would read as "the MAC alone".
    EXPECT_EQ(maap_seed(Eui48{}, 0), 1U);
}

TEST(avb_entity_kit_tspec, a_milan_kit_declares_table_4_4_frame_sizes_and_a_classic_kit_its_headroom)
{
    // Milan v1.3 Table 4.4 sizes the Talker Advertise's TSpec exactly; the
    // classic profile keeps one sample of headroom per channel.
    KitUnderTest milan{make_config(2, "milan13")};
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_FALSE(milan.talker_specs().empty());
    for (auto const& spec : milan.talker_specs()) {
        auto const info = milan->make_talker_srp_info(spec);
        EXPECT_EQ(info.max_frame_size, milan_srp_max_frame_size(spec.format));
        EXPECT_EQ(info.max_interval_frames, 1U);
    }
    for (auto const& spec : classic.talker_specs()) {
        auto const info = classic->make_talker_srp_info(spec);
        EXPECT_EQ(info.max_frame_size, srp_max_frame_size(spec.format));
        if (spec.format.kind != StreamKind::crf) {  // CRF is exact under both
            EXPECT_NE(info.max_frame_size, milan_srp_max_frame_size(spec.format));
        }
    }
}

TEST(avb_entity_kit_profile, a_milan_kit_runs_mrp_on_table_4_3_and_a_classic_kit_on_table_10_7)
{
    // Milan v1.3 Table 4.3: joinTime 200 ms, LeaveTime 5000 ms, a leavealltimer
    // of 10 to 15 s, a 1000 ms periodictimer, on MSRP and MVRP alike. Any
    // other profile keeps IEEE 802.1Q Table 10-7, whose LeaveTime is 1000 ms.
    KitUnderTest milan{make_config(2, "milan13")};
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_TRUE(milan->host().components().msrp_handler.mrp_timers() == nanoavb::MILAN_MRP_TIMERS);
    EXPECT_TRUE(milan->host().components().mvrp_handler.mrp_timers() == nanoavb::MILAN_MRP_TIMERS);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.join_time.count(), 200);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.leave_time.count(), 5000);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.leave_all_time_min.count(), 10000);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.leave_all_time_max.count(), 15000);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.periodic_time.count(), 1000);
    EXPECT_TRUE(classic->host().components().msrp_handler.mrp_timers() == srp::mrp::TimerConfig{});
    EXPECT_TRUE(classic->host().components().mvrp_handler.mrp_timers() == srp::mrp::TimerConfig{});
}

TEST(avb_entity_kit_profile, the_loop_does_not_sleep_past_an_mrp_timer)
{
    // The reactor loop waits at most 100 ms a cycle. An MRP jointimer falls
    // due 200 ms after a declaration: the wait is cut to what is left of it,
    // so its message goes out on time and not up to 100 ms late.
    KitUnderTest kit{make_config(2, "classic")};
    auto& msrp = kit->host().components().msrp_handler;
    EXPECT_FALSE(msrp.next_deadline_ns().has_value());  // not running yet
    statusbar::tsn::StreamId sid{ieee::Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 1};
    int64_t const start_ns = 5'000'000'000;
    EXPECT_TRUE(msrp.listener_ready(sid, TimePoint{std::chrono::nanoseconds{start_ns}}).has_value());
    EXPECT_EQ(msrp.next_deadline_ns().value_or(0), start_ns + 200'000'000);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns), 100);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + 150'000'000), 50);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + 199'500'000), 1);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + 200'000'000), 0);
}

TEST(avb_entity_kit_counters, the_loop_wakes_for_the_end_of_an_observation_interval)
{
    // The counters' interval is at most a second (Milan v1.3 5.3.8.10), and here half of one: the
    // loop's wait is cut to what is left of it, not up to 100 ms past it.
    constexpr int64_t interval = AvbEntityKit::COUNTERS_INTERVAL_NS;
    constexpr int64_t ms = 1'000'000;
    static_assert(interval <= 1'000'000'000);
    KitUnderTest kit{make_config(2, "classic")};
    int64_t const start_ns = 5'000'000'000;
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns), 100);  // no interval begun
    kit->end_cycle(start_ns);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + interval - (50 * ms)), 50);
    kit->end_cycle(start_ns + interval - ms);  // not yet over
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + interval - ms), 1);
    kit->end_cycle(start_ns + interval);  // over: the next one begins
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + (2 * interval) - (50 * ms)), 50);
}

TEST(avb_entity_kit_profile, a_talker_given_no_presentation_offset_takes_its_profiles)
{
    // Milan v1.3 5.3.7.6: "By default, the PAAD-AE shall use a 2ms Presentation
    // time offset for all its Stream Outputs." An entity held to 1722.1 alone
    // keeps 1 ms. A value the configuration gives is used under either profile.
    auto milan = make_config(2, "milan13");
    auto classic = make_config(2, "classic");
    auto milan_model = make_config(2, "", true);
    EXPECT_EQ(milan.presentation_offset_ns, 0U);
    EXPECT_EQ(effective_presentation_offset_ns(milan), 2'000'000U);
    EXPECT_EQ(effective_presentation_offset_ns(classic), 1'000'000U);
    EXPECT_EQ(effective_presentation_offset_ns(milan_model), 2'000'000U);

    milan.presentation_offset_ns = 1'000'000;
    classic.presentation_offset_ns = 3'000'000;
    EXPECT_EQ(effective_presentation_offset_ns(milan), 1'000'000U);
    EXPECT_EQ(effective_presentation_offset_ns(classic), 3'000'000U);
}

TEST(avb_entity_kit_profile, the_wire_version_and_the_address_mode_given_by_nobody_are_the_profiles)
{
    // Milan v1.3 5.3.3 has the descriptors of IEEE 1722.1-2021, and 4.3.3.1
    // has a talker's addresses come from MAAP. An entity held to 1722.1 alone
    // keeps the 2013/2016 lengths and its configured addresses. What the
    // configuration says is used under either profile.
    auto milan = make_config(2, "milan13");
    auto classic = make_config(2, "classic");
    auto milan_model = make_config(2, "", true);
    auto pinned = make_config(2, "classic", true);
    EXPECT_TRUE(milan.atdecc_version.empty());
    EXPECT_TRUE(milan.stream_address_mode.empty());
    EXPECT_EQ(effective_atdecc_version(milan), std::string_view{"2021"});
    EXPECT_EQ(effective_atdecc_version(classic), std::string_view{"2016"});
    EXPECT_EQ(effective_atdecc_version(milan_model), std::string_view{"2021"});
    EXPECT_EQ(effective_atdecc_version(pinned), std::string_view{"2016"});
    EXPECT_EQ(effective_stream_address_mode(milan), std::string_view{"maap"});
    EXPECT_EQ(effective_stream_address_mode(classic), std::string_view{"static"});
    EXPECT_EQ(effective_stream_address_mode(milan_model), std::string_view{"maap"});
    EXPECT_EQ(effective_stream_address_mode(pinned), std::string_view{"static"});

    milan.atdecc_version = "2016";
    milan.stream_address_mode = "static";
    classic.atdecc_version = "2021";
    classic.stream_address_mode = "maap";
    EXPECT_EQ(effective_atdecc_version(milan), std::string_view{"2016"});
    EXPECT_EQ(effective_stream_address_mode(milan), std::string_view{"static"});
    EXPECT_EQ(effective_atdecc_version(classic), std::string_view{"2021"});
    EXPECT_EQ(effective_stream_address_mode(classic), std::string_view{"maap"});
}

TEST(avb_entity_kit_profile, a_kit_sends_its_descriptors_in_its_profiles_wire_version)
{
    KitUnderTest milan{make_config(2, "milan13")};
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_FALSE(milan->host().components().aem_handler.legacy_2016());
    EXPECT_TRUE(classic->host().components().aem_handler.legacy_2016());

    auto asked = make_config(2, "milan13");
    asked.atdecc_version = "2016";
    KitUnderTest held{asked};
    EXPECT_TRUE(held->host().components().aem_handler.legacy_2016());
}

TEST(avb_entity_kit_profile, a_milan_entity_says_vendor_unique_and_interface_index_valid)
{
    // Milan v1.3 5.3.3.1 (ENTITY descriptor) and 5.6.2 (ADPDU): both bits are
    // 1. The model authors neither: they follow the entity answering MVU,
    // which a Milan model does under either profile and a plain one never.
    using namespace atdecc::entity_capabilities;
    uint32_t const milan_bits = VENDOR_UNIQUE_SUPPORTED | AEM_INTERFACE_INDEX_VALID;
    // What the tone model has: its three flags, and the IDENTIFY control its generator names.
    uint32_t const authored = AEM_SUPPORTED | CLASS_A_SUPPORTED | GPTP_SUPPORTED | AEM_IDENTIFY_CONTROL_INDEX_VALID;

    KitUnderTest plain{make_config(2, "")};
    EXPECT_EQ(plain.entity_capabilities(), authored);
    EXPECT_EQ(advertised_capabilities(plain), authored);

    KitUnderTest milan{make_config(2, "", true)};
    EXPECT_EQ(milan.entity_capabilities(), authored | milan_bits);
    EXPECT_EQ(advertised_capabilities(milan), authored | milan_bits);

    KitUnderTest pinned{make_config(2, "classic", true)};
    EXPECT_EQ(pinned.entity_capabilities(), authored | milan_bits);
    EXPECT_EQ(advertised_capabilities(pinned), authored | milan_bits);

    KitUnderTest opted_in{make_config(2, "milan13")};
    EXPECT_EQ(opted_in.entity_capabilities(), authored);
    EXPECT_EQ(advertised_capabilities(opted_in), authored);
}

TEST(avb_entity_kit_profile, an_empty_profile_is_the_models_and_an_explicit_one_wins)
{
    // Nothing said: a plain 1722.1 model answers to 1722.1 and has no Milan info.
    KitUnderTest plain{make_config(2, "")};
    EXPECT_FALSE(plain->milan_profile());
    EXPECT_EQ(profile_of(plain), Profile::classic);
    EXPECT_FALSE(milan_info_of(plain).has_value());

    // Nothing said of a Milan model: Milan v1.3, and GET_MILAN_INFO to answer.
    KitUnderTest milan{make_config(2, "", true)};
    EXPECT_TRUE(milan->milan_profile());
    EXPECT_EQ(profile_of(milan), Profile::milan13);
    EXPECT_TRUE(milan_info_of(milan).has_value());

    // An explicit profile wins; the model stays a Milan device for GET_MILAN_INFO.
    KitUnderTest pinned{make_config(2, "classic", true)};
    EXPECT_FALSE(pinned->milan_profile());
    EXPECT_EQ(profile_of(pinned), Profile::classic);
    EXPECT_TRUE(milan_info_of(pinned).has_value());

    KitUnderTest opted_in{make_config(2, "milan13")};
    EXPECT_TRUE(opted_in->milan_profile());
    EXPECT_EQ(profile_of(opted_in), Profile::milan13);
    EXPECT_FALSE(milan_info_of(opted_in).has_value());
}

TEST(avb_entity_kit_profile, the_milan_info_comes_from_the_configuration)
{
    auto config = make_config(2, "", true);
    config.milan_features_flags = atdecc::MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING;
    config.milan_certification_version = "1.2";
    config.milan_specification_version = "1.3.0.0";
    KitUnderTest kit{config};
    auto const& info = milan_info_of(kit);
    EXPECT_TRUE(info.has_value());
    if (info) {
        EXPECT_EQ(
            info->features_flags, atdecc::MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING | atdecc::MVU_FEATURE_MVU_BINDING);
        EXPECT_EQ(info->certification_version, atdecc::mvu_version(1, 2));
        EXPECT_EQ(info->specification_version, atdecc::mvu_version(1, 3));
    }

    // Defaults: no certification, Milan 1.3, and the one feature the Milan profile brings.
    KitUnderTest defaults{make_config(2, "", true)};
    auto const& given = milan_info_of(defaults);
    EXPECT_TRUE(given.has_value());
    if (given) {
        EXPECT_EQ(given->features_flags, atdecc::MVU_FEATURE_MVU_BINDING);
        EXPECT_EQ(given->certification_version, 0U);
        EXPECT_EQ(given->specification_version, atdecc::mvu_version(1, 3));
    }

    // A version that is not dotted numbers falls back to the default, and is logged.
    auto bad = make_config(2, "", true);
    bad.milan_certification_version = "one point two";
    bad.milan_specification_version = "1.3.";
    KitUnderTest fallback{bad};
    auto const& fell = milan_info_of(fallback);
    EXPECT_TRUE(fell.has_value());
    if (fell) {
        EXPECT_EQ(fell->certification_version, 0U);
        EXPECT_EQ(fell->specification_version, atdecc::mvu_version(1, 3));
    }
}

TEST(avb_entity_kit_profile, mvu_binding_is_advertised_when_bind_stream_is_answered_and_only_then)
{
    // Milan v1.3 5.4.4.6: the flag says the commands are implemented. They
    // are under the Milan profile, whatever the configuration leaves out,
    // and are not under IEEE 1722.1 connection management, whatever it claims.
    auto claimed = make_config(2, "classic", true);
    claimed.milan_features_flags = atdecc::MVU_FEATURE_MVU_BINDING | atdecc::MVU_FEATURE_REDUNDANCY;
    KitUnderTest classic{claimed};
    EXPECT_FALSE(classic->host().components().acmp_is_milan());
    EXPECT_EQ(milan_info_of(classic)->features_flags, atdecc::MVU_FEATURE_REDUNDANCY);

    KitUnderTest milan{make_config(2, "", true)};
    EXPECT_TRUE(milan->host().components().acmp_is_milan());
    EXPECT_EQ(milan_info_of(milan)->features_flags, atdecc::MVU_FEATURE_MVU_BINDING);
}

//
// SET_SAMPLING_RATE - IEEE 1722.1-2021 Clause 7.4.21
//

namespace {

constexpr uint64_t AAF_48K_8CH = 0x0205022002006000ULL;  // AAF 48k INT_32 8ch 6 samples/frame
constexpr uint64_t AAF_96K_8CH = 0x020702200200C000ULL;  // AAF 96k INT_32 8ch 12 samples/frame
constexpr uint64_t CRF_48K = 0x041060010000BB80ULL;
Eui64 const RATE_ENTITY{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};  // make_config's
Eui64 const RATE_CONTROLLER_A{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0xA1};
Eui64 const RATE_CONTROLLER_B{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0xB2};
Eui48 const RATE_MAC_A{0x02, 0x00, 0x00, 0x00, 0x00, 0xA1};
Eui48 const RATE_MAC_B{0x02, 0x00, 0x00, 0x00, 0x00, 0xB2};

/// A model whose AUDIO_UNIT 0 lists 48, 96 and 192 kHz, with an AAF stream input and output listing
/// their format at 48 and 96 kHz, a CRF output, and dynamic stream ports (source: entity_rates.json).
auto make_rates_config(std::string_view const profile) -> AvbEntityAudioIOConfig
{
    auto config = make_config(2, profile);
    config.descriptor_storage_blob = load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_rates.bin");
    return config;
}

/// One frame the kit's AEM handler sent: where to, and the AECPDU.
struct SentFrame
{
    Eui48 dest{};
    std::vector<uint8_t> frame;

    [[nodiscard]] auto header() const -> atdecc::AemDu
    {
        atdecc::AemDu du{};
        span_load(du, std::span<uint8_t const>{frame});
        return du;
    }
    [[nodiscard]] auto body() const -> std::span<uint8_t const>
    {
        return std::span<uint8_t const>{frame}.subspan(atdecc::AemDu::LENGTH);
    }
};

/// The kit's AEM handler as two controllers see it: commands go in through process_packet(),
/// every frame it sends is kept.
struct RateControllers
{
    KitUnderTest& kit;
    std::vector<SentFrame> sent{};
    uint16_t sequence_id{0};

    explicit RateControllers(KitUnderTest& under_test)
        : kit{under_test}
    {
        kit->host().components().aem_handler.set_send_response([this](Eui48 const& dest, std::span<uint8_t const> const frame) {
            sent.push_back({.dest = dest, .frame = {frame.begin(), frame.end()}});
            return true;
        });
    }

    /// @p controller at @p mac sends AEM @p command with @p body; its response's status.
    auto command(Eui64 const& controller, Eui48 const& mac, uint16_t const command, std::span<uint8_t const> const body) -> uint8_t
    {
        atdecc::AemDu header{};
        header.init_command(command, static_cast<uint16_t>(atdecc::AemDu::AEM_DATA_LENGTH + body.size()));
        header.target_entity_id = RATE_ENTITY;
        header.controller_entity_id = controller;
        header.sequence_id = ++sequence_id;
        std::vector<uint8_t> frame(atdecc::AemDu::LENGTH + body.size());
        span_store(std::span{frame}, header);
        std::ranges::copy(body, frame.begin() + atdecc::AemDu::LENGTH);
        size_t const before = sent.size();
        (void)kit->host().components().aem_handler.process_packet(mac, frame, header.target_entity_id);
        auto const response = std::find_if(sent.begin() + static_cast<std::ptrdiff_t>(before), sent.end(), [](SentFrame const& f) {
            return !f.header().is_unsolicited();
        });
        return response != sent.end() ? response->header().status() : uint8_t{0xFF};
    }

    auto set_rate(uint32_t const rate) -> uint8_t
    {
        std::array<uint8_t, atdecc::aem::AemSamplingRatePayload::LENGTH> body{};
        span_store(
            std::span{body},
            atdecc::aem::AemSamplingRatePayload{
                .descriptor_type = atdecc::aem::DESCRIPTOR_AUDIO_UNIT, .descriptor_index = 0, .sampling_rate = rate});
        return command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_SET_SAMPLING_RATE, body);
    }

    /// The rate GET_SAMPLING_RATE answers for AUDIO_UNIT 0.
    auto rate() -> uint32_t
    {
        std::array<uint8_t, 4> const body{0x00, 0x02, 0x00, 0x00};
        (void)command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_GET_SAMPLING_RATE, body);
        atdecc::aem::AemSamplingRatePayload answer{};
        span_load(answer, sent.back().body());
        return answer.sampling_rate.get();
    }

    /// The format GET_STREAM_FORMAT answers for stream (@p type, @p index).
    auto format(uint16_t const type, uint16_t const index) -> uint64_t
    {
        std::array<uint8_t, 4> const body{0x00, static_cast<uint8_t>(type), 0x00, static_cast<uint8_t>(index)};
        (void)command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_GET_STREAM_FORMAT, body);
        ieee::octlet_t word{};
        span_load(word, sent.back().body().subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format)));
        return word.get();
    }

    void register_a()
    {
        (void)command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, {});
        sent.clear();
    }

    void register_b()
    {
        (void)command(RATE_CONTROLLER_B, RATE_MAC_B, atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, {});
        sent.clear();
    }

    /// The unsolicited @p command responses in sent to @p dest.
    [[nodiscard]] auto notifications_to(Eui48 const& dest, uint16_t const command) const -> size_t
    {
        return static_cast<size_t>(std::ranges::count_if(sent, [&](SentFrame const& f) {
            return f.dest == dest && f.header().is_unsolicited() && f.header().command_code() == command;
        }));
    }

    /// The unsolicited SET_STREAM_FORMATs in sent, to @p dest or to anyone: (type, index, format).
    [[nodiscard]] auto format_notifications(std::optional<Eui48> const dest = {}) const
        -> std::vector<std::tuple<uint16_t, uint16_t, uint64_t>>
    {
        std::vector<std::tuple<uint16_t, uint16_t, uint64_t>> found;
        for (auto const& f : sent) {
            if (dest && f.dest != *dest) {
                continue;
            }
            if (f.header().is_unsolicited() && f.header().command_code() == atdecc::AEM_COMMAND_SET_STREAM_FORMAT) {
                atdecc::aem::AemStreamFormatPayload payload{};
                span_load(payload, f.body());
                ieee::octlet_t word{};
                span_load(word, f.body().subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format)));
                found.emplace_back(payload.descriptor_type.get(), payload.descriptor_index.get(), word.get());
            }
        }
        return found;
    }
};

/// What the entity's data plane was asked: each rate with its streams.
struct UnitRateRecorder
{
    std::vector<std::pair<uint32_t, std::vector<AvbEntityKit::RetimedFormat>>> asked{};
    uint8_t answer{atdecc::AEM_STATUS_SUCCESS};

    void install(KitUnderTest& kit)
    {
        kit->set_on_unit_rate_changed([this](uint32_t const rate, std::span<AvbEntityKit::RetimedFormat const> const streams) {
            asked.emplace_back(rate, std::vector<AvbEntityKit::RetimedFormat>{streams.begin(), streams.end()});
            return answer;
        });
    }
};

}  // namespace

TEST(avb_entity_kit_rate, without_a_unit_rate_hook_only_the_rate_in_force_is_accepted)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    EXPECT_EQ(controllers.set_rate(48'000), atdecc::AEM_STATUS_SUCCESS);
    // The data plane keeps its rate, so the unit does not report another (IEEE 1722.1-2021 Clause 7.4.21).
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(controllers.rate(), 48'000U);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0), AAF_48K_8CH);
    EXPECT_EQ(kit->sample_rate(), 48'000U);
}

TEST(avb_entity_kit_rate, a_unit_rate_hook_retimes_the_units_streams_notified_after_the_response)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    controllers.register_a();
    controllers.register_b();

    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_SUCCESS);
    // The AAF output and input, their layout kept; the CRF output stays.
    EXPECT_EQ(data_plane.asked.size(), size_t{1});
    if (data_plane.asked.size() == 1) {
        auto const& [rate, streams] = data_plane.asked.front();
        EXPECT_EQ(rate, 96'000U);
        EXPECT_EQ(streams.size(), size_t{2});
        EXPECT_TRUE(std::ranges::all_of(streams, [](auto const& s) { return s.from == AAF_48K_8CH && s.to == AAF_96K_8CH; }));
    }
    // The response first, to A; the rate's notification to B alone; then each stream's to both, A
    // included: a change beyond what it requested (Milan v1.3 Section 5.4.5.3).
    EXPECT_FALSE(controllers.sent.empty());
    EXPECT_FALSE(controllers.sent.front().header().is_unsolicited());
    EXPECT_EQ(controllers.sent.front().dest, RATE_MAC_A);
    EXPECT_EQ(controllers.sent.size(), size_t{6});
    EXPECT_EQ(controllers.notifications_to(RATE_MAC_A, atdecc::AEM_COMMAND_SET_SAMPLING_RATE), size_t{0});
    EXPECT_EQ(controllers.notifications_to(RATE_MAC_B, atdecc::AEM_COMMAND_SET_SAMPLING_RATE), size_t{1});
    using Notice = std::tuple<uint16_t, uint16_t, uint64_t>;
    std::vector<Notice> const retimed{
        {atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0, AAF_96K_8CH}, {atdecc::aem::DESCRIPTOR_STREAM_INPUT, 0, AAF_96K_8CH}};
    EXPECT_TRUE(controllers.format_notifications(RATE_MAC_A) == retimed);
    EXPECT_TRUE(controllers.format_notifications(RATE_MAC_B) == retimed);

    // The descriptors report what the data plane runs.
    EXPECT_EQ(controllers.rate(), 96'000U);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0), AAF_96K_8CH);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_INPUT, 0), AAF_96K_8CH);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 1), CRF_48K);
    // The kit's specs and engines keep the rate they were built with: the media thread reads them.
    EXPECT_EQ(kit->sample_rate(), 48'000U);
    EXPECT_EQ(kit->talker_specs().front().format.sample_rate_hz, 48'000U);
    EXPECT_EQ(kit->talker_specs().front().format_word, AAF_48K_8CH);
}

TEST(avb_entity_kit_rate, a_refused_or_unlisted_rate_moves_nothing)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    controllers.register_b();

    // The unit lists 192 kHz, its AAF streams do not: the data plane is not asked.
    EXPECT_EQ(controllers.set_rate(192'000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(data_plane.asked.empty());
    // The data plane refuses: its status, and the rate and formats in force.
    data_plane.answer = atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(data_plane.asked.size(), size_t{1});
    EXPECT_TRUE(controllers.format_notifications().empty());
    EXPECT_EQ(controllers.rate(), 48'000U);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_INPUT, 0), AAF_48K_8CH);
    EXPECT_EQ(kit->sample_rate(), 48'000U);
}

TEST(avb_entity_kit_rate, a_bound_milan_input_keeps_the_rate)
{
    KitUnderTest kit{make_rates_config("milan13")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    auto& sinks = *kit->host().components().milan_acmp_listener;
    EXPECT_TRUE(sinks.restore_binding(
        atdecc::MilanBinding{.listener_unique_id = 0, .talker_entity_id = RATE_CONTROLLER_B}, TimePoint{std::chrono::seconds{1}}));

    // Milan v1.3 Section 5.4.2.7: as the input's own SET_STREAM_FORMAT would be.
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_TRUE(data_plane.asked.empty());
    EXPECT_EQ(controllers.rate(), 48'000U);
}

// Milan v1.3 Section 5.4.2.13: a mapping held on a stream at the rate in force refuses a re-time,
// the data plane not asked; once it is removed the unit re-times.
TEST(avb_entity_kit_rate, a_held_mapping_refuses_a_retime)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    auto& aem = kit->host().components().aem_handler;
    auto const maps = std::make_unique<nanoavb::DynamicAudioMaps>();
    aem.set_dynamic_audio_maps(maps.get());
    aem.set_on_audio_mappings_changed(
        [](uint16_t, uint16_t, std::span<nanoavb::DynamicAudioMapping const>) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    auto const mapping = [&controllers](uint16_t const command) {
        std::array<uint8_t, atdecc::aem::AemAudioMappingsCommandHeader::LENGTH + atdecc::aem::AemAudioMapping::LENGTH> body{};
        span_store(
            std::span{body},
            atdecc::aem::AemAudioMappingsCommandHeader{
                .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, .descriptor_index = 0, .number_of_mappings = 1});
        span_store(
            std::span{body}.subspan(atdecc::aem::AemAudioMappingsCommandHeader::LENGTH),
            atdecc::aem::AemAudioMapping{.stream_index = 0, .stream_channel = 0, .cluster_offset = 0, .cluster_channel = 0});
        return controllers.command(RATE_CONTROLLER_A, RATE_MAC_A, command, body);
    };
    EXPECT_EQ(mapping(atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(data_plane.asked.empty());
    EXPECT_EQ(controllers.rate(), 48'000U);

    EXPECT_EQ(mapping(atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(data_plane.asked.size(), size_t{1});
    EXPECT_TRUE(aem.mappings_hold());
    aem.set_dynamic_audio_maps(nullptr);
}

// IEEE 1722-2016 Clause 4.4.4.3: a re-timed stream output's media clock restarts, so its mr
// toggles; a refused rate restarts nothing.
TEST(avb_entity_kit_rate, a_retimed_stream_output_toggles_mr)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    auto& talker = kit->talker();
    auto const& aaf = kit->talker_specs().front();
    EXPECT_TRUE(talker.open_stream(aaf, {}, {}).has_value());  // as start() opens it
    auto* const slot = talker.slot_for(aaf.index);
    EXPECT_TRUE(slot != nullptr);

    data_plane.answer = atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_FALSE(talker.next_mr(*slot));
    data_plane.answer = atdecc::AEM_STATUS_SUCCESS;
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(talker.next_mr(*slot));
}

namespace {

/// A RAW instant the kit's gate is told in the admission tests.
constexpr auto ADMIT_T0 = AvbEntityKit::RawTimePoint{std::chrono::seconds{100}};

/// The end of a reactor cycle the loop told at reactor-clock @p now_ns and @p raw on the gate's clock.
void end_cycle_at(KitUnderTest& kit, int64_t const now_ns, AvbEntityKit::RawTimePoint const raw)
{
    kit->tell_cycle_time(now_ns, raw);
    kit->end_cycle(now_ns);
}

/// Talker stream @p idx's StreamID as MSRP reports it.
auto talker_sid(KitUnderTest& kit, uint16_t const idx) -> nanoavb::StreamId
{
    nanoavb::StreamId sid{};
    auto const stream = kit->host().components().talker_stream(idx);
    EXPECT_TRUE(stream.has_value());
    sid.from_uint64(stream.has_value() ? stream->stream_id.to_uint64() : 0);
    return sid;
}

/// A controller's CONNECT_TX_COMMAND (or DISCONNECT_TX_COMMAND) for talker stream @p idx.
void acmp_tx_command(KitUnderTest& kit, uint8_t const type, uint16_t const idx)
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.set_message_type(type);
    cmd.talker_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};  // make_config's
    cmd.talker_unique_id = idx;
    cmd.listener_entity_id = Eui64{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    cmd.listener_unique_id = idx;
    cmd.sequence_id = 1;
    (void)kit->host().components().acmp_talker.receive_command(cmd, TimePoint{});
}

/// Each admitted set the kit's hook is told.
struct AdmissionRecorder
{
    std::vector<std::vector<uint16_t>> told{};

    void install(KitUnderTest& kit)
    {
        kit->set_on_admission_changed(
            [this](std::span<uint16_t const> const admitted) { told.emplace_back(admitted.begin(), admitted.end()); });
    }
};

}  // namespace

// IEEE 1722.1-2021 Clause 8.2.1: a classic talker stream is admitted while it is connected and its
// Listener Ready registered; the hook hears each change of the set, not the set at install.
TEST(avb_entity_kit_admission, the_hook_follows_the_listener_and_the_acmp_connection)
{
    using V = std::vector<uint16_t>;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    EXPECT_TRUE(kit->admitted_talker_streams(ADMIT_T0) == AvbEntityKit::AdmittedStreams{1});
    AdmissionRecorder first;
    first.install(kit);
    EXPECT_TRUE(first.told.empty());  // the set in force at install is not told

    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 2);
    EXPECT_TRUE(first.told.empty());  // connected, no Listener Ready: no change
    kit->notify_listener_ready(talker_sid(kit, 2), true, ADMIT_T0);
    EXPECT_TRUE((first.told == std::vector<V>{V{1, 2}}));
    EXPECT_TRUE(kit->talker_should_transmit(2, ADMIT_T0));

    AdmissionRecorder second;
    second.install(kit);  // replaces the first
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, 1);
    EXPECT_TRUE(second.told == std::vector<V>{V{2}});
    EXPECT_EQ(first.told.size(), size_t{1});
    EXPECT_FALSE(kit->talker_should_transmit(1, ADMIT_T0));
    kit->set_on_admission_changed({});
}

namespace {

/// A bridge that speaks MSRP to a kit, and the loop that drives the kit's reactor as these tests see
/// it: each cycle the kit is told the pair of clocks, then handed what the bridge sent.
struct BridgeOverMsrp
{
    KitUnderTest& kit;
    TimePoint now{std::chrono::seconds{2}};
    std::vector<std::vector<uint8_t>> out{};
    nanoavb::MsrpHandler<> bridge{
        statusbar::srp::msrp::MsrpConfig{}, nanoavb::MsrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) {
            out.emplace_back(p.begin(), p.end());
            return true;
        }}};

    /// The gate's clock at reactor time @p at: one hundred seconds ahead of it.
    [[nodiscard]] static auto raw_of(TimePoint const at) -> AvbEntityKit::RawTimePoint
    {
        return AvbEntityKit::RawTimePoint{at.time_since_epoch() + std::chrono::seconds{100}};
    }
    [[nodiscard]] static auto ns_of(TimePoint const at) -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
    }

    /// One reactor cycle 120 ms on: the kit told its time, the bridge's PDUs received, the timers run.
    void cycle()
    {
        now += std::chrono::milliseconds{120};
        kit->tell_cycle_time(ns_of(now), raw_of(now));
        bridge.tick(now);
        auto& msrp = kit->host().components().msrp_handler;
        for (auto const& p : std::exchange(out, {})) {
            msrp.receive_packet(p, now);
        }
        msrp.tick(now);
    }

    /// Cycles until @p done or @p rounds have gone by.
    template <typename Done>
    void cycles_until(int const rounds, Done done)
    {
        for (int round = 0; round < rounds && !done(); ++round) {
            cycle();
        }
    }
};

}  // namespace

// IEEE 802.1Q-2022 Clause 35.2.4.4: a Listener Ready a bridge declares over MSRP reaches the gate through
// the host, known at the instant the loop told the kit for that reactor cycle, and admits the connected
// stream.
TEST(avb_entity_kit_admission, a_listener_ready_over_msrp_admits_the_connected_stream)
{
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    uint16_t const idx = kit.talker_specs()[0].index;
    BridgeOverMsrp wire{kit};
    auto& msrp = kit->host().components().msrp_handler;
    EXPECT_TRUE(msrp.talker_advertise(kit->make_talker_srp_info(kit.talker_specs()[0]), wire.now).has_value());
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, idx);
    AdmissionRecorder recorder;
    recorder.install(kit);
    EXPECT_TRUE(wire.bridge.listener_ready(talker_sid(kit, idx), wire.now).has_value());
    wire.cycles_until(6, [&recorder] { return !recorder.told.empty(); });
    EXPECT_TRUE(recorder.told == std::vector<std::vector<uint16_t>>{{idx}});
    EXPECT_TRUE(kit->talker_should_transmit(idx, BridgeOverMsrp::raw_of(wire.now)));
    kit->set_on_admission_changed({});
}

// The kit reads no clock. A Listener Ready withdrawn over MSRP is stamped with the instant told for
// the cycle that saw it go, the grace and the admission deadline run from that instant, and a loop that
// holds the reactor clock alone is given its wait from the pair carried forward.
TEST(avb_entity_kit_admission, a_withdrawal_over_msrp_is_known_at_the_cycle_the_loop_told)
{
    using std::chrono::milliseconds;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    uint16_t const idx = kit.talker_specs()[0].index;
    BridgeOverMsrp wire{kit};
    auto& msrp = kit->host().components().msrp_handler;
    msrp.set_suppress_leaveall(true);
    wire.bridge.set_suppress_leaveall(true);
    EXPECT_TRUE(msrp.talker_advertise(kit->make_talker_srp_info(kit.talker_specs()[0]), wire.now).has_value());
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, idx);
    EXPECT_TRUE(wire.bridge.listener_ready(talker_sid(kit, idx), wire.now).has_value());
    wire.cycles_until(6, [&] { return kit->admitted_talker_streams() == AvbEntityKit::AdmittedStreams{idx}; });
    EXPECT_TRUE(kit->admitted_talker_streams() == AvbEntityKit::AdmittedStreams{idx});
    EXPECT_FALSE(kit->admission_deadline().has_value());  // registered: no grace runs

    // The bridge withdraws. The kit's registrar lets go a LeaveTime later, in one of the cycles told.
    EXPECT_TRUE(wire.bridge.listener_withdraw(talker_sid(kit, idx), wire.now).has_value());
    auto const before = BridgeOverMsrp::raw_of(wire.now);
    wire.cycles_until(100, [&] { return kit->admission_deadline().has_value(); });
    auto const deadline = kit->admission_deadline();
    EXPECT_TRUE(deadline.has_value());
    auto const stamped = BridgeOverMsrp::raw_of(wire.now);  // the cycle that saw it go
    EXPECT_TRUE(stamped > before);
    EXPECT_TRUE(deadline.value_or(AvbEntityKit::RawTimePoint{}) == stamped + TalkerGate::GRACE);
    EXPECT_TRUE(kit->admitted_talker_streams() == AvbEntityKit::AdmittedStreams{idx});  // within the grace

    // 40 ms further into the cycle, by the reactor clock alone.
    int64_t const later_ns = BridgeOverMsrp::ns_of(wire.now + milliseconds{40});
    EXPECT_TRUE(kit->raw_at(later_ns) == stamped + milliseconds{40});
    EXPECT_TRUE(kit->poll_timeout_ms(60'000, later_ns) <= 4960);  // the deadline is 4960 ms off, or a timer sooner
    EXPECT_EQ(kit->poll_timeout_ms(60'000, later_ns), kit->poll_timeout_ms(60'000, later_ns, stamped + milliseconds{40}));
    int64_t const at_deadline_ns = BridgeOverMsrp::ns_of(wire.now) + 5'000'000'000;
    EXPECT_EQ(kit->poll_timeout_ms(60'000, at_deadline_ns), 0);  // due: no wait until end_cycle() has run

    // A cycle told at the deadline: the set asked for with no instant is the set of that cycle.
    AdmissionRecorder recorder;
    recorder.install(kit);
    kit->tell_cycle_time(at_deadline_ns, stamped + TalkerGate::GRACE);
    EXPECT_TRUE(kit->admitted_talker_streams().empty());
    kit->end_cycle(at_deadline_ns);
    EXPECT_TRUE(recorder.told == std::vector<std::vector<uint16_t>>{{}});
    EXPECT_FALSE(kit->admission_deadline().has_value());
    kit->set_on_admission_changed({});
}

// An ACMP connection is a gate input with no time of its own: the set is evaluated at the instant
// told for its cycle, so one that arrives after a grace has run out finds it over.
TEST(avb_entity_kit_admission, an_acmp_connection_is_evaluated_at_the_cycle_the_loop_told)
{
    using std::chrono::seconds;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{seconds{1}}));
    auto connect_from = [&kit](uint8_t const listener) {
        atdecc::AcmpCommandResponse cmd{};
        cmd.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
        cmd.talker_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};  // make_config's
        cmd.talker_unique_id = 1;
        cmd.listener_entity_id = Eui64{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, listener};
        (void)kit->host().components().acmp_talker.receive_command(cmd, TimePoint{});
    };
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    connect_from(0x01);
    kit->notify_listener_ready(talker_sid(kit, 1), false, ADMIT_T0 + seconds{1});  // the grace runs to T0 + 1 s + GRACE
    AdmissionRecorder recorder;
    recorder.install(kit);

    kit->tell_cycle_time(1, ADMIT_T0 + seconds{2});  // a cycle inside the grace
    connect_from(0x02);
    EXPECT_TRUE(recorder.told.empty());
    EXPECT_EQ(kit->host().components().acmp_talker.connection_count(1), size_t{2});

    kit->tell_cycle_time(2, ADMIT_T0 + seconds{1} + TalkerGate::GRACE);  // a cycle at its end
    connect_from(0x03);
    EXPECT_TRUE(recorder.told == std::vector<std::vector<uint16_t>>{{}});
    kit->set_on_admission_changed({});
}

// Told no pair, the kit has no instant on the gate's clock: raw_at() says so, and the loop's wait is
// the control plane's alone.
TEST(avb_entity_kit_admission, a_kit_told_no_time_has_no_instant_on_the_gates_clock)
{
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    EXPECT_TRUE(kit->raw_at(5'000'000'000) == AvbEntityKit::RawTimePoint{});
    EXPECT_FALSE(kit->admission_deadline().has_value());
    EXPECT_EQ(kit->poll_timeout_ms(7, 5'000'000'000), kit->poll_timeout_ms(7, 5'000'000'000, ADMIT_T0));
}

// Milan v1.3 Section 5.5.4.1: a Milan talker streams while its Listener is registered, with no
// connection and no grace, so the stream leaves the admitted set at the Listener's withdrawal.
TEST(avb_entity_kit_admission, a_milan_stream_closes_at_the_listener_leave)
{
    using V = std::vector<uint16_t>;
    KitUnderTest kit{make_config(2, "milan13")};
    AdmissionRecorder recorder;
    recorder.install(kit);
    kit->notify_listener_ready(talker_sid(kit, 0), true, ADMIT_T0);
    EXPECT_TRUE(recorder.told == std::vector<V>{V{0}});
    kit->notify_listener_ready(talker_sid(kit, 0), false, ADMIT_T0 + std::chrono::seconds{1});
    EXPECT_TRUE(recorder.told == (std::vector<V>{V{0}, V{}}));
    EXPECT_TRUE(kit->admitted_talker_streams(ADMIT_T0 + std::chrono::seconds{1}).empty());
    EXPECT_FALSE(kit->admission_deadline().has_value());  // no grace: nothing to wait for
    kit->set_on_admission_changed({});
}

// A classic stream's grace runs out with no MSRP or ACMP event: the kit arms its deadline at the
// grace's end, re-evaluates admission there, tells the hook, and disarms.
TEST(avb_entity_kit_admission, the_deadline_is_armed_at_the_grace_end_and_disarmed_after)
{
    using V = std::vector<uint16_t>;
    using std::chrono::nanoseconds;
    constexpr auto grace = TalkerGate::GRACE;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    AdmissionRecorder recorder;
    recorder.install(kit);
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    EXPECT_FALSE(kit->admission_deadline().has_value());  // registered: no grace runs

    kit->notify_listener_ready(talker_sid(kit, 1), false, ADMIT_T0 + nanoseconds{10});
    auto const ends = ADMIT_T0 + nanoseconds{10} + grace;
    EXPECT_TRUE(kit->admission_deadline() == ends);
    end_cycle_at(kit, 1, ends - nanoseconds{1});
    EXPECT_TRUE(recorder.told == std::vector<V>{V{1}});  // still within the grace
    EXPECT_TRUE(kit->talker_should_transmit(1, ends - nanoseconds{1}));
    end_cycle_at(kit, 2, ends);
    EXPECT_TRUE((recorder.told == std::vector<V>{V{1}, V{}}));
    EXPECT_FALSE(kit->talker_should_transmit(1, ends));
    EXPECT_FALSE(kit->admission_deadline().has_value());
    end_cycle_at(kit, 3, ends + 10 * grace);
    EXPECT_EQ(recorder.told.size(), size_t{2});

    kit->notify_listener_ready(talker_sid(kit, 1), true, ends + 11 * grace);  // registered again: back in
    EXPECT_TRUE((recorder.told == std::vector<V>{V{1}, V{}, V{1}}));
    kit->set_on_admission_changed({});
}

// The loop does not sleep past a grace's end: its wait is cut to what is left of the deadline on
// the gate's clock, as it is for an MRP timer, and is 0 from the deadline until end_cycle() runs.
TEST(avb_entity_kit_admission, the_loop_wakes_for_the_admission_deadline)
{
    using std::chrono::milliseconds;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    auto const control = kit->next_control_deadline_ns();
    int64_t const now_ns = control.has_value() ? *control - 1'000'000'000 : 0;  // no other deadline within 100 ms
    EXPECT_EQ(kit->poll_timeout_ms(100, now_ns, ADMIT_T0), 100);                // nothing armed
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    kit->notify_listener_ready(talker_sid(kit, 1), false, ADMIT_T0 + milliseconds{50});
    auto const ends = ADMIT_T0 + milliseconds{50} + TalkerGate::GRACE;
    EXPECT_EQ(kit->poll_timeout_ms(100, now_ns, ends - milliseconds{50}), 50);
    EXPECT_EQ(kit->poll_timeout_ms(100, now_ns, ends - milliseconds{150}), 100);
    EXPECT_EQ(kit->poll_timeout_ms(100, now_ns, ends), 0);
    end_cycle_at(kit, now_ns, ends);
    EXPECT_EQ(kit->poll_timeout_ms(100, now_ns, ends), 100);  // disarmed
}

// end_cycle() finds the gate's clock as poll_timeout_ms() does, from the pair told for the cycle
// carried forward by the reactor clock, and runs the deadline once that reaches it.
TEST(avb_entity_kit_admission, the_end_of_a_cycle_runs_the_deadline_on_the_pair_carried_forward)
{
    using std::chrono::milliseconds;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    kit->notify_listener_ready(talker_sid(kit, 1), false, ADMIT_T0);  // the grace runs to T0 + GRACE
    AdmissionRecorder recorder;
    recorder.install(kit);
    int64_t const told_ns = 9'000'000'000;
    kit->tell_cycle_time(told_ns, ADMIT_T0 + TalkerGate::GRACE - milliseconds{40});
    kit->end_cycle(told_ns + 39'000'000);
    EXPECT_TRUE(recorder.told.empty());  // 1 ms short of the grace's end
    EXPECT_TRUE(kit->admission_deadline().has_value());
    kit->end_cycle(told_ns + 40'000'000);
    EXPECT_TRUE(recorder.told == std::vector<std::vector<uint16_t>>{{}});
    EXPECT_FALSE(kit->admission_deadline().has_value());
    kit->set_on_admission_changed({});
}

// Two streams' graces that end apart: the timer at the first end closes that stream and arms the
// second end, where the second closes and the deadline is disarmed.
TEST(avb_entity_kit_admission, the_timer_at_one_grace_end_arms_the_next)
{
    using V = std::vector<uint16_t>;
    using std::chrono::seconds;
    constexpr auto grace = TalkerGate::GRACE;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{seconds{1}}));
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 2);
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    kit->notify_listener_ready(talker_sid(kit, 2), true, ADMIT_T0);
    AdmissionRecorder recorder;
    recorder.install(kit);
    kit->notify_listener_ready(talker_sid(kit, 2), false, ADMIT_T0 + seconds{2});
    kit->notify_listener_ready(talker_sid(kit, 1), false, ADMIT_T0 + seconds{3});
    EXPECT_TRUE(kit->admission_deadline() == ADMIT_T0 + seconds{2} + grace);
    end_cycle_at(kit, 1, ADMIT_T0 + seconds{2} + grace);
    EXPECT_TRUE(recorder.told == std::vector<V>{V{1}});
    EXPECT_TRUE(kit->admission_deadline() == ADMIT_T0 + seconds{3} + grace);
    end_cycle_at(kit, 2, ADMIT_T0 + seconds{3} + grace);
    EXPECT_TRUE((recorder.told == std::vector<V>{V{1}, V{}}));
    EXPECT_FALSE(kit->admission_deadline().has_value());
    kit->set_on_admission_changed({});
}

// An entity that forwards poll_timeout_ms() forwards end_cycle(), or its loop would wake at once
// from the admission deadline on: the runner calls end_cycle() only where it exists.
template <typename Entity>
concept EndsCycle = requires(Entity& entity) { entity.end_cycle(int64_t{}); };
static_assert(EndsCycle<AvbEntityAudioIO>);
static_assert(EndsCycle<AvbEntityToneGenerator>);

// An entity whose gate is stamped from an MSRP hook is told the reactor cycle's time by its loop
// (entity_reactor_clock): the kit's two entities forward it to the kit.
template <typename Entity>
concept ToldCycleTime = requires(Entity& entity) { entity.tell_cycle_time(int64_t{}, AvbEntityKit::RawTimePoint{}); };
static_assert(ToldCycleTime<AvbEntityAudioIO>);
static_assert(ToldCycleTime<AvbEntityToneGenerator>);

// With the gate off every talker stream transmits from the start: that is the set in force, so
// nothing is told until a stream leaves it, which none does.
TEST(avb_entity_kit_admission, a_kit_without_a_gate_admits_every_stream_and_tells_nothing)
{
    auto config = make_config(2, "classic");
    config.gate_talker_on_listener = false;
    KitUnderTest kit{std::move(config)};
    AdmissionRecorder recorder;
    recorder.install(kit);
    kit->notify_listener_ready(talker_sid(kit, 0), true, ADMIT_T0);
    EXPECT_EQ(kit->admitted_talker_streams(ADMIT_T0).size(), kit.talker_specs().size());
    EXPECT_TRUE(recorder.told.empty());
}

//
// Stream port
//

namespace {

/// The 802.1Q-tagged Ethernet header build_stream_frame writes ahead of each AVTPDU.
constexpr size_t TAGGED_HEADER_LENGTH = 18;
Eui48 const TEST_PORT_MAC{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

/// @p kit started at @p now on an in-process test port opened before the start: that port, or
/// nullptr when the start did not keep it.
auto start_on_test_port(KitUnderTest& kit, TimePoint const now) -> net::TestPortContext*
{
    EXPECT_TRUE(kit->stream_port().open_test({.hardware_address = TEST_PORT_MAC}).has_value());
    EXPECT_TRUE(kit.start(now));
    return kit->stream_port().test_port();
}

}  // namespace

// The host's port is the streams': the start keeps it, the talker's frames reach it tagged with
// the configured VLAN, and the stop closes it as it would one the kit opened.
TEST(avb_entity_kit_stream_port, a_port_opened_before_the_start_carries_the_talkers_frames)
{
    KitUnderTest kit{make_config(7, "classic")};
    auto* const wire = start_on_test_port(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    EXPECT_EQ(kit->stream_port().generation(), 1U);  // not opened again
    auto* const slot = kit->talker().slot_of(StreamKind::aaf);
    EXPECT_TRUE(slot != nullptr);
    if (wire == nullptr || slot == nullptr) {
        return;
    }
    std::vector<float> const audio(size_t{slot->aaf_samples} * slot->spec.format.channels);
    kit->talker().transmit_aaf(*slot, kit->talker().presentation_for(0), slot->aaf_samples, audio);
    auto const sent = wire->pop_tx();
    EXPECT_TRUE(sent.has_value());
    if (sent.has_value()) {
        std::array<uint8_t, TAGGED_HEADER_LENGTH> header{};
        EXPECT_EQ(build_stream_frame(header, slot->dest_mac, TEST_PORT_MAC, 7, 3, {}), TAGGED_HEADER_LENGTH);
        EXPECT_TRUE(std::ranges::equal(std::span<uint8_t const>{sent->frame}.first(TAGGED_HEADER_LENGTH), header));
        EXPECT_TRUE(avtp::aaf_parse_header(std::span<uint8_t const>{sent->frame}.subspan(TAGGED_HEADER_LENGTH)).has_value());
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
    EXPECT_FALSE(kit->stream_port().valid());
}

//
// AVTP tu on the wire - IEEE 1722-2016 Clause 4.4.4.7, Milan v1.3 Section 4.3.5.2
//

namespace {

/// The tu and mr of one AVTPDU captured on the wire.
struct StreamBits
{
    bool tu{false};
    bool mr{false};

    auto operator==(StreamBits const&) const -> bool = default;
};

/// The tu and mr of the AVTPDU in captured @p frame of a @p kind stream; nullopt when it holds none.
auto bits_of(StreamKind const kind, std::span<uint8_t const> const frame) -> std::optional<StreamBits>
{
    auto const pdu = frame.size() > TAGGED_HEADER_LENGTH ? frame.subspan(TAGGED_HEADER_LENGTH) : std::span<uint8_t const>{};
    switch (kind) {
        case StreamKind::am824: {
            auto const p = avtp::am824_parse_header(pdu);
            return p ? std::optional{StreamBits{.tu = p->stream_header.tu(), .mr = p->stream_header.mr()}} : std::nullopt;
        }
        case StreamKind::aaf: {
            auto const p = avtp::aaf_parse_header(pdu);
            return p ? std::optional{StreamBits{.tu = p->tu(), .mr = p->mr()}} : std::nullopt;
        }
        case StreamKind::crf: {
            auto const p = avtp::crf_parse_header(pdu);
            return p ? std::optional{StreamBits{.tu = p->tu(), .mr = p->mr()}} : std::nullopt;
        }
        case StreamKind::other:
        default:
            return std::nullopt;
    }
}

/// One AVTPDU of @p slot, sent as the media thread sends one of its kind.
void transmit_one(TalkerStreams& talker, TalkerStreamSlot& slot)
{
    constexpr uint32_t AM824_SAMPLES = 6;
    std::vector<float> const audio(size_t{avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET} * slot.spec.format.channels);
    switch (slot.spec.format.kind) {
        case StreamKind::am824:
            talker.transmit_am824(slot, talker.presentation_for(0), AM824_SAMPLES, audio);
            return;
        case StreamKind::aaf:
            talker.transmit_aaf(slot, talker.presentation_for(0), slot.aaf_samples, audio);
            return;
        case StreamKind::crf:
            talker.transmit_crf(slot, 0);
            return;
        case StreamKind::other:
        default:
            return;
    }
}

using RawTimePoint = TalkerStreams::RawTimePoint;

/// The tu and mr of @p count AVTPDUs the kit's @p kind output sends in a media wake at RAW
/// instant @p wake, as captured on @p wire. The kit is asked about the stream first, as an
/// entity asks it each wake, which tells the talker the wake's instant.
auto send(KitUnderTest& kit, net::TestPortContext& wire, StreamKind const kind, size_t const count, RawTimePoint const wake)
    -> std::vector<StreamBits>
{
    std::vector<StreamBits> captured;
    auto* const slot = kit->talker().slot_of(kind);
    for (size_t i = 0; slot != nullptr && i < count; ++i) {
        (void)kit->talker_should_transmit(slot->spec.index, wake);
        transmit_one(kit->talker(), *slot);
        auto const frame = wire.pop_tx();
        auto const bits = frame ? bits_of(kind, frame->frame) : std::nullopt;
        if (bits.has_value()) {
            captured.push_back(*bits);
        }
    }
    return captured;
}

/// @p count AVTPDUs of one tu and one mr.
auto all(size_t const count, bool const tu, bool const mr) -> std::vector<StreamBits>
{
    return std::vector<StreamBits>(count, StreamBits{.tu = tu, .mr = mr});
}

/// The talker's three kinds of stream, each the tone model's STREAM_OUTPUT of its kind.
constexpr std::array<StreamKind, 3> ALL_KINDS{StreamKind::am824, StreamKind::aaf, StreamKind::crf};

/// The tone kit started on a test port, its media clock anchored at a told gPTP instant (CRF needs one).
auto start_streaming(KitUnderTest& kit, TimePoint const now) -> net::TestPortContext*
{
    auto* const wire = start_on_test_port(kit, now);
    (void)kit.media_clock().advance(1'000'000'000, 1.0, 6);
    return wire;
}

/// GET_COUNTERS of the kit's STREAM_OUTPUT of @p kind, as a controller reads them.
auto output_counters(KitUnderTest& kit, StreamKind const kind) -> atdecc::aem::AemCountersPayload
{
    auto const* const slot = kit->talker().slot_of(kind);
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_GET_COUNTERS, atdecc::aem::AemGetCountersCommandPayload::LENGTH);
    header.controller_entity_id = RATE_CONTROLLER_A;
    atdecc::aem::AemGetCountersCommandPayload const asked{
        .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT,
        .descriptor_index = slot != nullptr ? slot->spec.index : uint16_t{0xFFFF}};
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const result = kit->host().components().aem_handler.handle_command(header, make_const_span(asked), out);
    EXPECT_EQ(result.status, atdecc::AEM_STATUS_SUCCESS);
    atdecc::aem::AemCountersPayload counters{};
    span_load(counters, std::span<uint8_t const>{out}.first(atdecc::aem::AemCountersPayload::LENGTH));
    return counters;
}

/// What GET_COUNTERS answers for the kit's @p kind output at @p bit (IEEE 1722.1-2021 Table 7-158).
auto output_counter(KitUnderTest& kit, StreamKind const kind, unsigned const bit) -> uint32_t
{
    return output_counters(kit, kind).counters.at(bit).get();
}

/// The RAW instant of the reactor cycle an Announce arrives in, as the loop tells the kit it.
constexpr RawTimePoint CYCLE{std::chrono::seconds{500}};
/// The gPTP handler's time of that Announce.
constexpr int64_t ANNOUNCE_NS = 2'000'000'000;
/// A media wake @p after the cycle the Announce arrived in.
constexpr auto wake_after(std::chrono::nanoseconds const after) -> RawTimePoint
{
    return CYCLE + after;
}
constexpr auto WINDOW = std::chrono::nanoseconds{TalkerStreams::TU_AFTER_GM_CHANGE_NS};

/// A grandmaster announced to @p kit in the reactor cycle at CYCLE, as the loop tells it; its handler.
auto announce_grandmaster(KitUnderTest& kit) -> nanoavb::GptpAnnounceHandler&
{
    auto& announces = kit->host().net_handlers()->gptp_handler();
    gptp::AnnounceMessage announce{};
    announce.init(1);
    announce.grandmaster_identity = gptp::ClockIdentity{Eui64{0x00, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55}};
    kit->tell_cycle_time(ANNOUNCE_NS, CYCLE);
    announces.observe_announce(ANNOUNCE_NS, announce);
    return announces;
}

}  // namespace

namespace {

/// Each (stream, open) the kit's gate hook is told.
struct GateRecorder
{
    std::vector<std::pair<uint16_t, bool>> told{};

    void install(KitUnderTest& kit)
    {
        kit->set_on_talker_gate_changed([this](uint16_t const idx, bool const open) { told.emplace_back(idx, open); });
    }
};

/// A controller's START_STREAMING or STOP_STREAMING (@p command) for STREAM_OUTPUT @p index: the status.
auto output_streaming_command(KitUnderTest& kit, uint16_t const command, uint16_t const index) -> uint8_t
{
    atdecc::AemDu header{};
    header.init_command(command, atdecc::aem::AemStreamingPayload::LENGTH);
    header.controller_entity_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    atdecc::aem::AemStreamingPayload payload{};
    payload.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
    payload.descriptor_index = index;
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    return kit->host().components().aem_handler.handle_command(header, make_const_span(payload), out).status;
}

}  // namespace

// A start knows no grandmaster: every AVTPDU of each kind carries tu, and does again once the
// grandmaster announced is lost; TIMESTAMP_UNCERTAIN counts each of them.
TEST(avb_entity_kit_tu, every_kind_of_avtpdu_carries_tu_while_no_grandmaster_is_in_force)
{
    KitUnderTest kit{make_config(2, "classic")};
    auto* const wire = start_streaming(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send(kit, *wire, kind, 3, wake_after(std::chrono::seconds{1})) == all(3, true, false));
    }

    auto& announces = announce_grandmaster(kit);
    announces.tick(ANNOUNCE_NS + announces.announce_receipt_timeout_ns() + 1);  // the Announces stopped
    EXPECT_FALSE(announces.has_grandmaster());
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send(kit, *wire, kind, 2, wake_after(std::chrono::seconds{10})) == all(2, true, false));
        EXPECT_EQ(output_counter(kit, kind, stream_output_timestamp_uncertain_bit), 5U);
        EXPECT_EQ(output_counter(kit, kind, stream_output_frames_tx_bit), 5U);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{3}}));
}

// Milan v1.3 Annex B.1.1: an Announce opens the 0.25 s window at the reactor cycle it arrives in;
// every AVTPDU of a wake inside it carries tu, none of a wake at its end or past it.
TEST(avb_entity_kit_tu, every_kind_of_avtpdu_carries_tu_inside_the_window_after_an_announce_and_not_past_it)
{
    using std::chrono::nanoseconds;
    KitUnderTest kit{make_config(2, "classic")};
    auto* const wire = start_streaming(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    (void)announce_grandmaster(kit);
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send(kit, *wire, kind, 1, wake_after(nanoseconds{0})) == all(1, true, false));
        EXPECT_TRUE(send(kit, *wire, kind, 1, wake_after(WINDOW - nanoseconds{1})) == all(1, true, false));
        EXPECT_TRUE(send(kit, *wire, kind, 1, wake_after(WINDOW)) == all(1, false, false));
        EXPECT_TRUE(send(kit, *wire, kind, 2, wake_after(std::chrono::seconds{1})) == all(2, false, false));
        EXPECT_EQ(output_counter(kit, kind, stream_output_timestamp_uncertain_bit), 2U);
        EXPECT_EQ(output_counter(kit, kind, stream_output_frames_tx_bit), 5U);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

//
// AVTP mr on the wire - IEEE 1722-2016 Clauses 4.4.4.3, 10.4.3
//

namespace {

/// The mr of each AVTPDU in @p captured.
auto mrs(std::vector<StreamBits> const& captured) -> std::vector<bool>
{
    std::vector<bool> out;
    std::ranges::transform(captured, std::back_inserter(out), [](StreamBits const& b) { return b.mr; });
    return out;
}

/// The mr of @p count AVTPDUs the kit's @p kind output sends in a wake past the tu window, as
/// captured on @p wire.
auto send_mr(KitUnderTest& kit, net::TestPortContext& wire, StreamKind const kind, size_t const count) -> std::vector<bool>
{
    return mrs(send(kit, wire, kind, count, wake_after(std::chrono::seconds{1})));
}

/// Runs of mr: each (count, value) in turn.
auto runs(std::initializer_list<std::pair<size_t, bool>> const parts) -> std::vector<bool>
{
    std::vector<bool> out;
    for (auto const& [count, value] : parts) {
        out.insert(out.end(), count, value);
    }
    return out;
}

/// The toggles of mr from one AVTPDU to the next in @p sent: what MEDIA_RESET counts of them.
auto toggles(std::vector<bool> const& sent) -> uint32_t
{
    uint32_t count = 0;
    for (size_t i = 1; i < sent.size(); ++i) {
        count += sent[i] != sent[i - 1] ? 1U : 0U;
    }
    return count;
}

constexpr size_t HOLD = TalkerStreams::MR_HOLD_PDUS;

/// The tone kit started on a test port with a grandmaster announced, so the AVTPDUs send_mr()
/// sends carry tu clear and a frame's bits are its mr alone.
auto start_certain(KitUnderTest& kit, TimePoint const now) -> net::TestPortContext*
{
    auto* const wire = start_streaming(kit, now);
    (void)announce_grandmaster(kit);
    return wire;
}

/// SET_CLOCK_SOURCE of clock domain 0 to @p source, as a controller asks it; the status answered.
auto set_clock_source(KitUnderTest& kit, uint16_t const source) -> uint8_t
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, atdecc::aem::AemClockSourcePayload::LENGTH);
    header.controller_entity_id = RATE_CONTROLLER_A;
    atdecc::aem::AemClockSourcePayload const asked{
        .descriptor_type = atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, .descriptor_index = 0, .clock_source_index = source};
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    return kit->host().components().aem_handler.handle_command(header, make_const_span(asked), out).status;
}

}  // namespace

// A restart toggles mr on the next AVTPDU, which keeps it for eight; one asked inside the hold
// toggles it back on the first after. MEDIA_RESET counts the toggles the frames carry.
TEST(avb_entity_kit_mr, a_media_clock_restart_toggles_mr_on_the_wire_and_holds_it_for_eight_avtpdus)
{
    KitUnderTest kit{make_config(2, "classic")};
    auto* const wire = start_certain(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    for (auto const kind : ALL_KINDS) {
        auto const index = kit->talker().slot_of(kind)->spec.index;
        auto sent = send_mr(kit, *wire, kind, 2);
        EXPECT_TRUE(kit->talker().toggle_media_clock_restart(index).has_value());
        auto const first = send_mr(kit, *wire, kind, 1);
        EXPECT_TRUE(kit->talker().toggle_media_clock_restart(index).has_value());
        auto const rest = send_mr(kit, *wire, kind, HOLD - 1 + HOLD + 1);
        sent.insert(sent.end(), first.begin(), first.end());
        sent.insert(sent.end(), rest.begin(), rest.end());
        EXPECT_TRUE(sent == runs({{2, false}, {HOLD, true}, {HOLD + 1, false}}));
        EXPECT_EQ(output_counter(kit, kind, stream_output_media_reset_bit), toggles(sent));
        EXPECT_EQ(output_counter(kit, kind, stream_output_frames_tx_bit), sent.size());
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

// The media clock re-anchored on a gPTP timeline stepped seconds ahead restarts every stream it
// times: each kind's next AVTPDU toggles mr and holds it. One that keeps time toggles nothing.
TEST(avb_entity_kit_mr, a_stepped_re_anchor_toggles_mr_on_every_kind_of_avtpdu)
{
    KitUnderTest kit{make_config(2, "classic")};
    auto* const wire = start_certain(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, 1) == runs({{1, false}}));  // the first AVTPDU knows the clock's steps
    }
    (void)kit.media_clock().advance(1'000'125'000, 1.0, 6);  // on time
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, 1) == runs({{1, false}}));
    }
    (void)kit.media_clock().advance(9'000'000'000, 1.0, 6);
    EXPECT_EQ(kit.media_clock().step_count(), 1U);
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, HOLD) == runs({{HOLD, true}}));
        EXPECT_EQ(output_counter(kit, kind, stream_output_media_reset_bit), 1U);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

// SET_CLOCK_SOURCE to another source of clock domain 0 restarts the media clock of the domain's
// outputs: mr toggles on each kind's next AVTPDU. The source in force named again restarts nothing.
TEST(avb_entity_kit_mr, another_clock_source_toggles_mr_on_every_kind_of_avtpdu_and_the_same_one_does_not)
{
    KitUnderTest kit{make_config(2, "classic")};
    auto* const wire = start_certain(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, 2) == runs({{2, false}}));
    }
    EXPECT_EQ(set_clock_source(kit, 1), atdecc::AEM_STATUS_SUCCESS);
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, HOLD) == runs({{HOLD, true}}));
    }
    EXPECT_EQ(set_clock_source(kit, 1), atdecc::AEM_STATUS_SUCCESS);
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, 2) == runs({{2, true}}));
        EXPECT_EQ(output_counter(kit, kind, stream_output_media_reset_bit), 1U);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

// An accepted SET_SAMPLING_RATE re-times the AAF output, whose next AVTPDU toggles mr; the CRF
// output it does not move, and a rate the data plane refuses, toggle nothing.
TEST(avb_entity_kit_mr, an_accepted_sampling_rate_toggles_mr_on_the_retimed_output_alone)
{
    KitUnderTest kit{make_rates_config("classic")};
    auto* const wire = start_certain(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    RateControllers controllers{kit};  // after the start, which gives the AEM handler its socket
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    EXPECT_TRUE(send_mr(kit, *wire, StreamKind::aaf, 2) == runs({{2, false}}));
    data_plane.answer = atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_TRUE(send_mr(kit, *wire, StreamKind::aaf, 2) == runs({{2, false}}));
    data_plane.answer = atdecc::AEM_STATUS_SUCCESS;
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(send_mr(kit, *wire, StreamKind::aaf, HOLD) == runs({{HOLD, true}}));
    EXPECT_TRUE(send_mr(kit, *wire, StreamKind::crf, 2) == runs({{2, false}}));
    EXPECT_EQ(output_counter(kit, StreamKind::aaf, stream_output_media_reset_bit), 1U);
    EXPECT_EQ(output_counter(kit, StreamKind::crf, stream_output_media_reset_bit), 0U);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

// A stop ends the streams: the next start's first AVTPDUs carry mr clear, whatever the last run
// sent or asked, and MEDIA_RESET counts no toggle at the reopen.
TEST(avb_entity_kit_mr, a_stream_reopened_after_a_stop_starts_with_mr_clear)
{
    KitUnderTest kit{make_config(2, "classic")};
    auto* wire = start_certain(kit, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    for (auto const kind : ALL_KINDS) {
        auto const index = kit->talker().slot_of(kind)->spec.index;
        EXPECT_TRUE(kit->talker().toggle_media_clock_restart(index).has_value());
        EXPECT_TRUE(send_mr(kit, *wire, kind, 3) == runs({{3, true}}));
        EXPECT_TRUE(kit->talker().toggle_media_clock_restart(index).has_value());  // inside the hold: owed at the stop
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));

    wire = start_certain(kit, TimePoint{std::chrono::seconds{3}});
    EXPECT_TRUE(wire != nullptr);
    if (wire == nullptr) {
        return;
    }
    for (auto const kind : ALL_KINDS) {
        EXPECT_TRUE(send_mr(kit, *wire, kind, HOLD + 1) == runs({{HOLD + 1, false}}));
        EXPECT_EQ(output_counter(kit, kind, stream_output_media_reset_bit), 0U);
        EXPECT_EQ(output_counter(kit, kind, stream_output_frames_tx_bit), 3U + HOLD + 1);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{4}}));
}

// A classic stream streams while it is admitted and not stopped: the hook is told each edge the media
// thread's transmit_if_due() finds, STOP_STREAMING's included, which leaves the admitted set as it is.
TEST(avb_entity_kit_talker_gate, the_hook_follows_admission_and_stop_streaming)
{
    using G = std::vector<std::pair<uint16_t, bool>>;
    KitUnderTest kit{make_config(2, "classic")};
    GateRecorder recorder;
    recorder.install(kit);
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    EXPECT_TRUE(recorder.told.empty());  // nothing admitted yet
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    EXPECT_TRUE((recorder.told == G{{1, true}}));
    EXPECT_TRUE(kit->talker_gate_open(1));

    EXPECT_EQ(output_streaming_command(kit, atdecc::AEM_COMMAND_STOP_STREAMING, 1), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE((recorder.told == G{{1, true}, {1, false}}));
    EXPECT_TRUE(kit->admitted_talker_streams(ADMIT_T0) == AvbEntityKit::AdmittedStreams{1});  // the slot stops, not the gate
    EXPECT_EQ(output_streaming_command(kit, atdecc::AEM_COMMAND_STOP_STREAMING, 1), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(recorder.told.size(), size_t{2});  // stopped again: no change
    EXPECT_EQ(output_streaming_command(kit, atdecc::AEM_COMMAND_START_STREAMING, 1), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE((recorder.told == G{{1, true}, {1, false}, {1, true}}));
    kit->set_on_talker_gate_changed({});
}

// A grace that runs out with no event closes the gate at the admission deadline, and stop() closes
// every gate still open; a later install replaces the earlier hook.
TEST(avb_entity_kit_talker_gate, the_deadline_and_stop_close_the_gate)
{
    using G = std::vector<std::pair<uint16_t, bool>>;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    acmp_tx_command(kit, atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 2);
    kit->notify_listener_ready(talker_sid(kit, 1), true, ADMIT_T0);
    kit->notify_listener_ready(talker_sid(kit, 2), true, ADMIT_T0);
    GateRecorder first;
    first.install(kit);
    EXPECT_TRUE(first.told.empty());  // the gates in force at install are not told
    EXPECT_TRUE(kit->talker_gate_open(1) && kit->talker_gate_open(2));

    kit->notify_listener_ready(talker_sid(kit, 1), false, ADMIT_T0 + std::chrono::seconds{1});
    EXPECT_TRUE(first.told.empty());  // within the grace
    end_cycle_at(kit, 1, ADMIT_T0 + std::chrono::seconds{1} + TalkerGate::GRACE);
    EXPECT_TRUE((first.told == G{{1, false}}));

    GateRecorder second;
    second.install(kit);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
    EXPECT_TRUE((second.told == G{{2, false}}));
    EXPECT_EQ(first.told.size(), size_t{1});
    EXPECT_FALSE(kit->talker_gate_open(2));
    kit->set_on_talker_gate_changed({});
}

// With the gate off every talker stream streams from start() to stop(): start() tells each one open,
// stop() each one closed.
TEST(avb_entity_kit_talker_gate, a_kit_without_a_gate_opens_every_stream_at_start_and_closes_it_at_stop)
{
    auto config = make_config(2, "classic");
    config.gate_talker_on_listener = false;
    KitUnderTest kit{std::move(config)};
    GateRecorder recorder;
    recorder.install(kit);
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    std::vector<std::pair<uint16_t, bool>> opened;
    std::vector<std::pair<uint16_t, bool>> closed;
    for (auto const& spec : kit.talker_specs()) {
        opened.emplace_back(spec.index, true);
        closed.emplace_back(spec.index, false);
    }
    EXPECT_TRUE(recorder.told == opened);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
    opened.insert(opened.end(), closed.begin(), closed.end());
    EXPECT_TRUE(recorder.told == opened);
    kit->set_on_talker_gate_changed({});
}

// IEEE 1722-2025 Annex B.3: in "maap" address mode a stream streams only while its block is defended. The
// gate closes when the block is lost and opens when it is defended again, as the media thread finds it.
TEST(avb_entity_kit_talker_gate, the_gate_follows_the_maap_block)
{
    constexpr int64_t SEC = 1'000'000'000;
    auto config = make_config(2, "classic");
    config.gate_talker_on_listener = false;
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));  // no MAAP socket: the static addresses stand
    std::vector<std::pair<uint16_t, bool>> closed;
    std::vector<std::pair<uint16_t, bool>> opened;
    for (auto const& spec : kit.talker_specs()) {
        closed.emplace_back(spec.index, false);
        opened.emplace_back(spec.index, true);
    }
    uint16_t const idx = kit.talker_specs().front().index;
    auto agree = [&kit, idx] { return kit->talker_gate_open(idx) == kit->talker_should_transmit(idx, ADMIT_T0); };
    EXPECT_TRUE(kit->talker_gate_open(idx));
    EXPECT_TRUE(agree());
    GateRecorder recorder;
    recorder.install(kit);

    avtp::MaapHandler handler{Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}, 1};
    handler.set_send([](Eui48 const&, std::span<uint8_t const>) { return true; });
    kit->maap().wire_handler(handler, kit->host().components(), kit->talker());
    handler.prepare(static_cast<uint16_t>(kit.talker_specs().size()));
    auto defend = [&handler](int64_t const from_ns) {
        handler.port_operational(from_ns);
        for (int k = 1; k <= 12 && !handler.is_acquired(); ++k) {
            handler.tick(from_ns + (k * SEC));
        }
    };
    defend(SEC);
    EXPECT_TRUE(recorder.told.empty());  // ready already: no change
    handler.release(20 * SEC);
    EXPECT_TRUE(recorder.told == closed);
    EXPECT_FALSE(kit->talker_gate_open(idx));
    EXPECT_TRUE(agree());
    defend(30 * SEC);
    closed.insert(closed.end(), opened.begin(), opened.end());
    EXPECT_TRUE(recorder.told == closed);
    EXPECT_TRUE(kit->talker_gate_open(idx));
    EXPECT_TRUE(agree());
    kit->set_on_talker_gate_changed({});
}

// atdecc.acquire_shared: the kit hands the configured mode to the AEM handler; under the Milan
// profile an ACQUIRE_ENTITY is refused whatever the mode (Milan v1.3 5.4.2.1).
TEST(avb_entity_kit_acquire, the_configured_acquire_mode_reaches_the_handler_and_milan_grants_none)
{
    using AcquireMode = nanoavb::AemCommandHandler::AcquireMode;
    KitUnderTest by_default{make_config(2, "classic")};
    EXPECT_TRUE(by_default->host().components().aem_handler.acquire_mode() == AcquireMode::exclusive);

    auto shared_config = make_config(2, "classic");
    shared_config.acquire_shared = true;
    KitUnderTest shared{std::move(shared_config)};
    EXPECT_TRUE(shared->host().components().aem_handler.acquire_mode() == AcquireMode::shared);

    auto milan_config = make_config(2, "milan13", true);
    milan_config.acquire_shared = true;
    KitUnderTest milan{std::move(milan_config)};
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_ACQUIRE_ENTITY, atdecc::aem::AemAcquireEntityPayload::LENGTH);
    header.controller_entity_id = RATE_CONTROLLER_A;
    atdecc::aem::AemAcquireEntityPayload const asked{};
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const answered = milan->host().components().aem_handler.handle_command(header, make_const_span(asked), out);
    EXPECT_EQ(answered.status, atdecc::AEM_STATUS_NOT_SUPPORTED);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_kit_test)
