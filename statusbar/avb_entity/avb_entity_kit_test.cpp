// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit's talker streams and the SR class domain: ACMP's picture of each
// stream and the MSRP Talker Advertise carry the VLAN and priority the
// frames are tagged with, configured or adopted from the wire (Milan v1.3
// 4.2.7.2.1), never the headers' defaults.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <chrono>
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
    [[nodiscard]] auto talker_specs() const noexcept -> StreamSpecs const& { return talker_specs_; }

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
        auto const* stream = kit->host().components().acmp_talker.get_stream(spec.index);
        EXPECT_NE(stream, nullptr);
        if (stream != nullptr) {
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
        auto const* stream = kit->host().components().acmp_talker.get_stream(spec.index);
        EXPECT_NE(stream, nullptr);
        if (stream != nullptr) {
            EXPECT_EQ(stream->stream_vlan_id, 5U);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 5U);
        EXPECT_EQ(info.priority, 5U);
    }
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
        EXPECT_EQ(info->features_flags, atdecc::MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING);
        EXPECT_EQ(info->certification_version, atdecc::mvu_version(1, 2));
        EXPECT_EQ(info->specification_version, atdecc::mvu_version(1, 3));
    }

    // Defaults: no certification, Milan 1.3, no features.
    KitUnderTest defaults{make_config(2, "", true)};
    auto const& given = milan_info_of(defaults);
    EXPECT_TRUE(given.has_value());
    if (given) {
        EXPECT_EQ(given->features_flags, 0U);
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

TEST_MAIN(statusbar_avb_entity, avb_entity_kit_test)
