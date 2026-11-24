// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit's talker streams and the SR class domain: ACMP's picture of each
// stream and the MSRP Talker Advertise carry the VLAN and priority the
// frames are tagged with, configured or adopted from the wire (Milan v1.3
// 4.2.7.2.1), never the headers' defaults.

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/test/test_data.hpp"

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
    return statusbar::test::functional_test_data(__FILE__, "testdata/entity_tone.bin");
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

/// What the kit says the Class A VLAN is, from every place it is said:
/// the SRP handler's Domain, the frames, ACMP's picture of each stream.
void expect_kit_on_vlan(KitUnderTest& kit, uint16_t const vlan_id, uint8_t const priority)
{
    EXPECT_EQ(kit->host().components().msrp_handler.domain().sr_class_vid, vlan_id);
    EXPECT_EQ(kit->host().components().msrp_handler.domain().sr_class_priority, priority);
    EXPECT_EQ(kit->talker().vlan_id(), vlan_id);
    for (auto const& spec : kit.talker_specs()) {
        auto const* stream = kit->host().components().acmp_talker.get_stream(spec.index);
        EXPECT_NE(stream, nullptr);
        if (stream != nullptr) {
            EXPECT_EQ(stream->stream_vlan_id, vlan_id);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, vlan_id);
        EXPECT_EQ(info.priority, priority);
    }
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

TEST(avb_entity_kit_domain, a_started_kit_follows_the_domain_it_adopts)
{
    // The adoption hook is installed at construction; bringing the control
    // plane up installs the senders. A Domain adopted after start must still
    // move the frames, ACMP and the Talker Advertise, or the SRP handler
    // declares VLAN 5 for frames that go out on VLAN 7.
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_domain, after_a_restart_mvrp_registers_the_vlan_the_domain_declares)
{
    // stop() withdraws the VLAN and the Domain; start() registers the VLAN
    // again. It must be the one the Domain in force names, whatever that
    // is after an adoption, or MVRP and the declared Domain disagree until
    // the next adoption.
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{3}}));

    auto const& domain = kit->host().components().msrp_handler.domain();
    auto const& mvrp = kit->host().components().mvrp_handler;
    EXPECT_EQ(kit->talker().vlan_id(), domain.sr_class_vid);
    for (uint16_t const vlan_id : {uint16_t{5}, uint16_t{7}}) {
        auto const want = vlan_id == domain.sr_class_vid ? nanoavb::VlanState::Pending : nanoavb::VlanState::Unregistered;
        EXPECT_EQ(mvrp.get_vlan_state(vlan_id), want);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{4}}));
}

/// MVRP's registrations, from the kit's point of view: @p registered Pending
/// (or Registered), every other VLAN in @p others Unregistered.
void expect_mvrp_on(KitUnderTest& kit, uint16_t const registered, std::initializer_list<uint16_t> const others)
{
    auto const& mvrp = kit->host().components().mvrp_handler;
    EXPECT_NE(mvrp.get_vlan_state(registered), nanoavb::VlanState::Unregistered);
    for (uint16_t const other : others) {
        EXPECT_EQ(mvrp.get_vlan_state(other), nanoavb::VlanState::Unregistered);
    }
}

TEST(avb_entity_kit_domain, start_returns_to_the_configured_domain)
{
    // Milan v1.3 4.2.7.2.1: "After startup ... a PAAD shall use SR Class
    // Priority 3 and Default VLAN ID 2 for Class A" (the configured Domain).
    // Whatever was adopted before, start() brings the frames, ACMP, the
    // Talker Advertise and MVRP back to it.
    KitUnderTest kit{make_config(7, "milan13")};
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);

    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    expect_kit_on_vlan(kit, 7, 3);
    expect_mvrp_on(kit, 7, {5});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_domain, a_link_up_returns_to_the_configured_domain)
{
    // Milan v1.3 4.2.7.2.1: "... or a Link Up event". A link that bounces
    // onto another bridge starts from the configured Domain and adopts what
    // that bridge declares, not what the last one did.
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);
    expect_mvrp_on(kit, 5, {7});

    kit->on_link_up(TimePoint{std::chrono::seconds{3}});
    expect_kit_on_vlan(kit, 7, 3);
    expect_mvrp_on(kit, 7, {5});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{4}}));
}

TEST(avb_entity_kit_maap, a_maap_kit_is_stopped_started_and_bounced_without_a_port_left_behind)
{
    // The interface does not exist, so the MAAP socket never opens and the
    // kit keeps its static addresses — the fallback. The stop/start and Link
    // Up bookkeeping around the acquirer must hold all the same: no deadline
    // from a port that is not there, TX never gated, nothing dangling across
    // a restart (Table B.7 Release! on stop, PortOperational! on Link Up).
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    for (int round = 0; round < 2; ++round) {
        auto const base = std::chrono::seconds{1 + (round * 10)};
        EXPECT_TRUE(kit.start(TimePoint{base}));
        kit->on_link_up(TimePoint{base + std::chrono::seconds{1}});
        EXPECT_EQ(kit->poll_timeout_ms(100, 0), 100);                // no MAAP deadline bounds the loop
        kit->on_link_up(TimePoint{base + std::chrono::seconds{2}});  // a bounce
        EXPECT_TRUE(kit.stop(TimePoint{base + std::chrono::seconds{3}}));
    }
}

TEST(avb_entity_kit_maap, an_adopted_vlan_survives_the_acquisition_of_a_block)
{
    // The block is acquired on every Link Up (B.3.5.9); publishing it writes
    // each stream's dest MAC into ACMP's picture of the stream, and that
    // picture also carries the VLAN. The VLAN is the Domain's - here the one
    // adopted from the bridge - and must come through the publication as it
    // was, or every Link Up would move the Talker Advertise, the
    // CONNECT_TX_RESPONSE and GET_STREAM_INFO back to the default VLAN while
    // the frames stayed on the adopted one.
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));  // the talker slots exist from here
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);

    std::vector<uint16_t> indices;
    for (auto const& spec : kit.talker_specs()) {
        indices.push_back(spec.index);
    }
    Eui48 const block{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
    MaapAddressAcquirer::publish_block(kit->host().components(), kit->talker(), indices, block);

    for (size_t pos = 0; pos < indices.size(); ++pos) {
        auto const* stream = kit->host().components().acmp_talker.get_stream(indices[pos]);
        EXPECT_TRUE(stream != nullptr);
        if (stream != nullptr) {
            EXPECT_EQ(stream->stream_vlan_id, 5U);
            EXPECT_TRUE(stream->stream_dest_mac == avtp::maap_block_address(block, static_cast<uint16_t>(pos)));
        }
        auto const* slot = kit->talker().slot_for(indices[pos]);
        EXPECT_TRUE(slot != nullptr && slot->dest_mac == avtp::maap_block_address(block, static_cast<uint16_t>(pos)));
    }
    expect_kit_on_vlan(kit, 5, 5);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_maap, a_link_up_while_the_link_is_up_is_a_repeat_and_a_restart_starts_afresh)
{
    // The runner raises the start-up Link Up itself and then installs a link
    // monitor that reports the first state it sees, so a kit sees two Link
    // Ups within milliseconds. The second must not reach MAAP, or it restarts
    // the probe sequence the first began (B.3.5.9 PortOperational! from
    // PROBE: five PROBEs for four). What on_link_up() tells the two apart by
    // is the link-up state, which a Link Down ends, and which stop() ends so
    // that the Link Up after a restart begins the acquisition again.
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_FALSE(kit->link_up());

    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    EXPECT_FALSE(kit->link_up());
    auto const down = std::string{kit->host().state_string()};
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(kit->link_up());
    auto const after_first = std::string{kit->host().state_string()};
    EXPECT_NE(after_first, down);                                                        // the supervisor left Down on the first
    kit->on_link_up(TimePoint{std::chrono::seconds{2} + std::chrono::milliseconds{3}});  // the monitor's first report
    EXPECT_TRUE(kit->link_up());
    EXPECT_EQ(std::string{kit->host().state_string()}, after_first);  // and the repeat changed nothing

    kit->on_link_down(TimePoint{std::chrono::seconds{3}});
    EXPECT_FALSE(kit->link_up());
    kit->on_link_up(TimePoint{std::chrono::seconds{4}});  // a genuine bounce
    EXPECT_TRUE(kit->link_up());

    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{5}}));
    EXPECT_FALSE(kit->link_up());  // the next start's Link Up is a first one
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{6}}));
    EXPECT_FALSE(kit->link_up());
    kit->on_link_up(TimePoint{std::chrono::seconds{7}});
    EXPECT_TRUE(kit->link_up());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{8}}));
}

TEST(avb_entity_kit_maap, the_first_entity_available_waits_for_the_talker_addresses)
{
    // A Milan listener that hears ENTITY_AVAILABLE while the block is still
    // being probed probes at once, is told TALKER_DEST_MAC_FAIL and sits out
    // TMR_RETRY. Under the hold the host's Link Up starts everything but ADP
    // and owes it; the acquirer's ready callback releases it. A Link Down
    // cancels the debt and the next Link Up incurs it again. Driven at the
    // host, since the fixture's MAAP socket never opens (see below).
    using nanoavb::AdpAdvertiserState;
    KitUnderTest kit{make_config(2, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    auto& host = kit->host();
    auto& adp = host.components().adp_advertiser;
    host.hold_adp(true);
    host.on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(adp.state() == AdpAdvertiserState::Stopped);  // held
    EXPECT_TRUE(host.adp_owed());
    host.release_adp(TimePoint{std::chrono::seconds{3}});  // the block is defended
    EXPECT_TRUE(adp.state() == AdpAdvertiserState::Advertising);
    EXPECT_FALSE(host.adp_owed());
    host.release_adp(TimePoint{std::chrono::seconds{3}});  // a re-defend owes nothing
    EXPECT_TRUE(adp.state() == AdpAdvertiserState::Advertising);

    host.on_link_down(TimePoint{std::chrono::seconds{4}});
    EXPECT_FALSE(host.adp_owed());
    EXPECT_TRUE(adp.state() != AdpAdvertiserState::Advertising);
    host.on_link_up(TimePoint{std::chrono::seconds{5}});
    EXPECT_TRUE(host.adp_owed());  // the bounce asks again
    host.on_link_down(TimePoint{std::chrono::seconds{6}});
    EXPECT_FALSE(host.adp_owed());  // and a Link Down before the defend cancels it
    host.release_adp(TimePoint{std::chrono::seconds{7}});
    EXPECT_TRUE(adp.state() != AdpAdvertiserState::Advertising);

    host.hold_adp(false);
    host.on_link_up(TimePoint{std::chrono::seconds{8}});
    EXPECT_TRUE(adp.state() == AdpAdvertiserState::Advertising);  // no hold: at once, as a static kit
    EXPECT_FALSE(host.adp_owed());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{9}}));
}

TEST(avb_entity_kit_maap, without_a_maap_socket_adp_starts_at_link_up_as_before)
{
    // The fixture's interface does not exist, so the acquirer falls back to
    // the static addresses and is ready from the start: the kit's Link Up
    // holds nothing and ADP starts at once, as it does in static mode.
    using nanoavb::AdpAdvertiserState;
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(kit->host().components().adp_advertiser.state() == AdpAdvertiserState::Advertising);
    EXPECT_FALSE(kit->host().adp_owed());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{3}}));
}

namespace {

/// A PROBE_TX_COMMAND for talker stream @p index of @p kit, handed to its ACMP talker at @p now.
void probe_talker_stream(KitUnderTest& kit, uint16_t const index, TimePoint const now)
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.talker_entity_id = kit->host().components().acmp_talker.entity_id();
    cmd.talker_unique_id = index;
    cmd.listener_entity_id = Eui64{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    cmd.listener_unique_id = 0;
    (void)kit->host().components().acmp_talker.receive_command(cmd, now);
}

/// The MSRP state of talker stream @p spec of @p kit, Idle when the handler does not know the stream.
auto talker_state(KitUnderTest& kit, StreamSpec const& spec) -> nanoavb::TalkerReservationState
{
    auto const* stream = kit->host().components().msrp_handler.get_talker_stream(kit->make_talker_srp_info(spec).stream_id);
    return stream != nullptr ? stream->state : nanoavb::TalkerReservationState::Idle;
}

/// Start @p kit and take it to Ready: Link Up, then a grandmaster.
void bring_up(KitUnderTest& kit)
{
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    kit->on_gptp_announce(TimePoint{std::chrono::seconds{3}}, true);
    EXPECT_TRUE(kit->host().is_ready());
}

}  // namespace

TEST(avb_entity_kit_talker, a_milan_kit_declares_a_stream_when_it_is_probed_and_a_classic_kit_at_once)
{
    // Milan v1.3 4.3.3.1: no Talker attribute for a stream no listener has
    // asked for. The probe reaches the SRP handler through ACMP, and only
    // the stream it names is declared. IEEE 1722.1 as written declares
    // every stream once the entity is ready.
    using nanoavb::TalkerReservationState;
    KitUnderTest milan{make_config(2, "milan13")};
    bring_up(milan);
    EXPECT_TRUE(milan.talker_specs().size() >= 2);
    for (auto const& spec : milan.talker_specs()) {
        EXPECT_EQ(talker_state(milan, spec), TalkerReservationState::Waiting);
    }
    auto const& probed = milan.talker_specs().front();
    probe_talker_stream(milan, probed.index, TimePoint{std::chrono::seconds{4}});
    for (auto const& spec : milan.talker_specs()) {
        EXPECT_EQ(
            talker_state(milan, spec),
            spec.index == probed.index ? TalkerReservationState::Advertising : TalkerReservationState::Waiting);
    }
    EXPECT_TRUE(milan.stop(TimePoint{std::chrono::seconds{5}}));

    KitUnderTest classic{make_config(2, "classic")};
    bring_up(classic);
    for (auto const& spec : classic.talker_specs()) {
        EXPECT_EQ(talker_state(classic, spec), TalkerReservationState::Advertising);
    }
    EXPECT_TRUE(classic.stop(TimePoint{std::chrono::seconds{5}}));
}

TEST(avb_entity_kit_talker, a_restarted_milan_kit_holds_its_addresses_valid_again)
{
    // A stop gives the stream addresses up; the start after it must not
    // leave the SRP handler believing they are still gone, or a kit on
    // static addresses would never declare a talker again.
    KitUnderTest kit{make_config(2, "milan13")};
    bring_up(kit);
    kit->host().components().msrp_handler.set_talker_addresses_valid(false);  // what a MAAP stop leaves behind
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{5}}));
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{6}}));
    EXPECT_TRUE(kit->host().components().msrp_handler.talker_addresses_valid());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{7}}));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_kit_functional_test)
