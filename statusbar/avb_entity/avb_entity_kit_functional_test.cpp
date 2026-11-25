// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit's talker streams and the SR class domain: ACMP's picture of each
// stream and the MSRP Talker Advertise carry the VLAN and priority the
// frames are tagged with, configured or adopted from the wire (Milan v1.3
// 4.2.7.2.1), never the headers' defaults. Connection management: which
// ACMP a kit runs, and a listener kit and a talker kit taken through a
// connection under each, their PDUs carried between them by hand.

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/avb_entity/avb_entity_listener_bindings.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/test/test_data.hpp"

#include <array>
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
        auto const stream = kit->host().components().talker_stream(spec.index);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
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
        auto const stream = kit->host().components().talker_stream(indices[pos]);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
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

/// Hand ACMP PDU @p pdu to @p kit as a frame off the wire at @p now: the
/// kit's ATDECC handler gives it to whichever ACMP the kit runs.
void receive_acmp(KitUnderTest& kit, atdecc::AcmpCommandResponse const& pdu, TimePoint const now)
{
    std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
    auto const frame = atdecc::acmp_serialize_2016(pdu, buf);
    auto const now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    kit->host().net_handlers()->atdecc_handler().dispatch_frame(now_ns, Eui48{0x02, 0, 0, 0, 0, 0x09}, frame);
}

/// A PROBE_TX_COMMAND for talker stream @p index of @p kit, received at @p now.
void probe_talker_stream(KitUnderTest& kit, uint16_t const index, TimePoint const now)
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.init_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    cmd.talker_entity_id = kit->host().components().acmp_talker.entity_id();
    cmd.talker_unique_id = index;
    cmd.listener_entity_id = Eui64{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    cmd.listener_unique_id = 0;
    receive_acmp(kit, cmd, now);
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

//
// Connection management: IEEE 1722.1-2021 ACMP or Milan v1.3's, by the profile
//

namespace {

// Descriptor blob with stream inputs and outputs (committed fixture; source: examples/dual.json).
auto entity_audio_bin_path() -> std::filesystem::path
{
    return statusbar::test::functional_test_data(__FILE__, "testdata/entity_audio.bin");
}

constexpr Eui64 LISTENER_ID{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0A};
constexpr Eui64 TALKER_ID{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0B};
constexpr Eui64 CONTROLLER_ID{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
constexpr Eui64 OTHER_CONTROLLER_ID{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x99};

/// A configuration over the blob with both stream inputs and outputs. The
/// interface address is told and differs per entity, so each kit's stream
/// ids are its own.
auto make_io_config(Eui64 const& entity_id, std::string_view const profile, std::string bindings_path = {})
    -> AvbEntityAudioIOConfig
{
    AvbEntityAudioIOConfig config = make_config(2, profile);
    config.entity_id = entity_id;
    config.descriptor_storage_blob = load_file(entity_audio_bin_path());
    config.interface_mac = Eui48{0x02, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(entity_id.to_uint64() & 0xFFU)};
    config.listener_bindings_path = std::move(bindings_path);
    return config;
}

auto ns_of(TimePoint const now) -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

/// A controller's ACMP command, listener side or talker side by @p type.
auto controller_command(uint8_t const type, Eui64 const& talker, uint16_t const source, Eui64 const& listener, uint16_t const sink)
    -> atdecc::AcmpCommandResponse
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.init_command(type);
    cmd.controller_entity_id = CONTROLLER_ID;
    cmd.talker_entity_id = talker;
    cmd.talker_unique_id = source;
    cmd.listener_entity_id = listener;
    cmd.listener_unique_id = sink;
    cmd.sequence_id = 1;
    return cmd;
}

/// A listener kit and a talker kit on one wire with a controller: every
/// ACMP PDU either sends reaches the other and is kept for the controller
/// to read, and MSRP runs between the two as between two end stations.
struct KitPair
{
    KitUnderTest listener;
    KitUnderTest talker;
    TimePoint now{std::chrono::seconds{10}};
    std::vector<atdecc::AcmpCommandResponse> wire{};  ///< every ACMP PDU the kits sent, in order
    std::vector<atdecc::AcmpCommandResponse> acmp_to_listener{};
    std::vector<atdecc::AcmpCommandResponse> acmp_to_talker{};
    std::vector<std::vector<uint8_t>> msrp_to_listener{};
    std::vector<std::vector<uint8_t>> msrp_to_talker{};
    uint16_t source{0};
    uint16_t sink{0};

    explicit KitPair(std::string_view const profile, std::string bindings_path = {})
        : listener{make_io_config(LISTENER_ID, profile, std::move(bindings_path))}
        , talker{make_io_config(TALKER_ID, profile)}
    {
        bring_up(listener);
        bring_up(talker);
        hook(listener, acmp_to_talker, msrp_to_talker);
        hook(talker, acmp_to_listener, msrp_to_listener);
        source = talker.talker_specs().front().index;
        sink = listener->listener_specs().front().index;
    }
    KitPair(KitPair const&) = delete;
    auto operator=(KitPair const&) -> KitPair& = delete;

    /// Send what @p kit transmits to the other kit's queues and the controller's record.
    void hook(KitUnderTest& kit, std::vector<atdecc::AcmpCommandResponse>& acmp_out, std::vector<std::vector<uint8_t>>& msrp_out)
    {
        auto send = [this, &acmp_out](atdecc::AcmpCommandResponse const& pdu) {
            wire.push_back(pdu);
            acmp_out.push_back(pdu);
            return true;
        };
        auto& components = kit->host().components();
        if (components.acmp_is_milan()) {
            components.milan_acmp_talker->set_tx_response(send);
            components.milan_acmp_listener->set_senders(send, send);
        } else {
            components.acmp_talker.set_tx_response(send);
            components.acmp_listener.set_senders(send, send);
        }
        components.msrp_handler.set_send_packet([&msrp_out](std::span<uint8_t const> const packet) {
            msrp_out.emplace_back(packet.begin(), packet.end());
            return true;
        });
    }

    /// Carry everything in flight to where it is going, until nothing is.
    void deliver()
    {
        while (!acmp_to_listener.empty() || !acmp_to_talker.empty() || !msrp_to_listener.empty() || !msrp_to_talker.empty()) {
            for (auto const& pdu : std::exchange(acmp_to_listener, {})) {
                receive_acmp(listener, pdu, now);
            }
            for (auto const& pdu : std::exchange(acmp_to_talker, {})) {
                receive_acmp(talker, pdu, now);
            }
            for (auto const& packet : std::exchange(msrp_to_listener, {})) {
                listener->host().components().msrp_handler.receive_packet(packet, now);
            }
            for (auto const& packet : std::exchange(msrp_to_talker, {})) {
                talker->host().components().msrp_handler.receive_packet(packet, now);
            }
        }
    }

    /// @p rounds of 125 ms: both kits' protocol timers, then the wire.
    void run(int const rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{125};
            for (auto* kit : {&listener, &talker}) {
                (*kit)->host().components().msrp_handler.tick(now);
                (*kit)->host().net_handlers()->atdecc_handler().tick(ns_of(now));
            }
            deliver();
        }
    }

    /// The controller sends @p cmd; both kits hear it, as on the ACMP multicast.
    void controller_sends(atdecc::AcmpCommandResponse const& cmd)
    {
        receive_acmp(listener, cmd, now);
        receive_acmp(talker, cmd, now);
        deliver();
    }

    void bind()
    {
        controller_sends(controller_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER_ID, source, LISTENER_ID, sink));
    }

    /// The last PDU of @p type on the wire (a copy: the record grows), nullopt with none.
    [[nodiscard]] auto last(uint8_t const type) const -> std::optional<atdecc::AcmpCommandResponse>
    {
        for (auto it = wire.rbegin(); it != wire.rend(); ++it) {
            if (it->message_type() == type) {
                return *it;
            }
        }
        return std::nullopt;
    }

    /// The talker's stream as SRP knows it.
    [[nodiscard]] auto stream_id() -> nanoavb::StreamId
    {
        return talker->make_talker_srp_info(talker.talker_specs().front()).stream_id;
    }

    [[nodiscard]] auto listener_srp_state() -> nanoavb::ListenerReservationState
    {
        auto const* stream = listener->host().components().msrp_handler.get_listener_stream(stream_id());
        return stream != nullptr ? stream->state : nanoavb::ListenerReservationState::Idle;
    }

    [[nodiscard]] auto talker_transmits() -> bool
    {
        // The gate's grace runs on the media timer's clock: here the pair's own time.
        return talker->talker_should_transmit(source, realtime::MonotonicRawClock::time_point{now.time_since_epoch()});
    }
};

constexpr int ROUNDS_PER_SECOND = 8;

/// The talker kit's ENTITY_AVAILABLE as it stands, handed to the listener kit.
void talker_advertises(KitPair& pair, uint32_t const available_index)
{
    atdecc::AdpDu adp = pair.talker->host().components().adp_advertiser.adpdu();
    adp.set_message_type(atdecc::ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
    adp.set_valid_time(5);
    adp.available_index = available_index;
    std::array<uint8_t, atdecc::AdpDu::LENGTH> buf{};
    span_store(std::span<uint8_t>{buf}, adp);
    pair.listener->host().net_handlers()->atdecc_handler().dispatch_frame(ns_of(pair.now), Eui48{0x02, 0, 0, 0, 0, 0x0B}, buf);
}

/// Lock @p kit to @p controller with an AEM LOCK_ENTITY command.
void lock_entity(KitUnderTest& kit, Eui64 const& controller)
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_LOCK_ENTITY, 12);
    header.controller_entity_id = controller;
    std::array<uint8_t, 16> const lock_entity_command{};  // flags 0 (lock), locked id, ENTITY 0
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const result = kit->host().components().aem_handler.handle_command(header, lock_entity_command, out);
    EXPECT_EQ(result.status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(kit->host().components().aem_handler.is_locked());
}

}  // namespace

TEST(avb_entity_kit_acmp, the_profile_chooses_which_acmp_runs)
{
    // The milan13 profile, or a Milan model with no profile named, runs
    // Milan v1.3 connection management; anything else IEEE 1722.1-2021's.
    KitUnderTest milan{make_config(2, "milan13")};
    EXPECT_TRUE(milan->host().components().acmp_is_milan());
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_FALSE(classic->host().components().acmp_is_milan());
    EXPECT_TRUE(classic->host().components().milan_acmp_talker == nullptr);
    KitUnderTest milan_model{make_config(2, "", true)};
    EXPECT_TRUE(milan_model->host().components().acmp_is_milan());
    KitUnderTest milan_model_held_to_1722_1{make_config(2, "classic", true)};
    EXPECT_FALSE(milan_model_held_to_1722_1->host().components().acmp_is_milan());
    KitUnderTest plain_model{make_config(2, "")};
    EXPECT_FALSE(plain_model->host().components().acmp_is_milan());

    // ACMP's picture of the stream outputs is in the pair that runs.
    for (auto const& spec : milan.talker_specs()) {
        EXPECT_TRUE(milan->host().components().milan_acmp_talker->get_stream(spec.index)->stream_id != Eui64{});
        EXPECT_TRUE(classic->host().components().acmp_talker.get_stream(spec.index)->stream_id != Eui64{});
    }
}

TEST(avb_entity_kit_acmp, a_milan_listener_bound_to_a_milan_talker_settles_reserves_and_is_streamed_to)
{
    using atdecc::MilanSinkState;
    KitPair pair{"milan13"};
    auto const& sink = *pair.listener->host().components().milan_acmp_listener->sink(pair.sink);
    EXPECT_EQ(
        pair.talker->host().components().msrp_handler.get_talker_stream(pair.stream_id())->state,
        nanoavb::TalkerReservationState::Waiting);  // 4.3.3.1: nothing declared unasked
    EXPECT_FALSE(pair.talker_transmits());

    // 5.5.2.4: the bind is answered at once, and does not involve the talker.
    pair.bind();
    auto const bound = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_TRUE(bound.has_value());
    EXPECT_EQ(bound->status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(bound->connection_count.get(), 1);
    EXPECT_EQ(bound->stream_id, Eui64{});

    // 5.5.2.6: the listener probed by itself, the talker answered with its
    // parameters and no connection (5.5.4.1), and the sink settled on them.
    auto const probe = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    auto const answer = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_TRUE(probe.has_value() && answer.has_value());
    EXPECT_EQ(probe->controller_entity_id, CONTROLLER_ID);
    EXPECT_EQ(probe->flags.get(), atdecc::acmp_flags::FAST_CONNECT);
    EXPECT_EQ(answer->status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(answer->connection_count.get(), 0);
    EXPECT_EQ(answer->stream_id, pair.talker->host().components().talker_stream(pair.source)->stream_id);
    EXPECT_TRUE(sink.is_settled());
    EXPECT_EQ(sink.context().srp.stream_dest_mac, pair.talker->host().components().talker_stream(pair.source)->stream_dest_mac);
    EXPECT_EQ(sink.context().srp.stream_vlan_id, 2);
    EXPECT_EQ(pair.talker->host().components().acmp_talker.connection_count(pair.source), 0U);

    // SRP does the rest: the probe declared the Talker attribute (4.3.3.1),
    // the listener registers it and declares Ready, the talker streams.
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);
    EXPECT_EQ(sink.context().probing_status, atdecc::MilanProbingStatus::Completed);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Ready);
    EXPECT_TRUE(pair.talker_transmits());  // on the reservation alone: no ACMP connection exists

    // It holds with no further ACMP: past the probe's 15 seconds and several LeaveAlls.
    size_t const acmp_so_far = pair.wire.size();
    pair.run(40 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);
    EXPECT_TRUE(pair.talker_transmits());
    EXPECT_EQ(pair.wire.size(), acmp_so_far);

    // GET_RX_STATE and GET_TX_STATE as Milan has them (Tables 5.44 and 5.52).
    pair.controller_sends(controller_command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, Eui64{}, 0, LISTENER_ID, pair.sink));
    auto const rx_state = pair.last(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE);
    EXPECT_EQ(rx_state->talker_entity_id, TALKER_ID);
    EXPECT_EQ(rx_state->connection_count.get(), 1);
    EXPECT_EQ(rx_state->flags.get(), atdecc::acmp_flags::FAST_CONNECT);
    EXPECT_EQ(rx_state->stream_id, answer->stream_id);
    pair.controller_sends(controller_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, TALKER_ID, pair.source, Eui64{}, 0));
    auto const tx_state = pair.last(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE);
    EXPECT_EQ(tx_state->connection_count.get(), 0);
    EXPECT_EQ(tx_state->flags.get(), 0);
    EXPECT_EQ(tx_state->stream_id, answer->stream_id);
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, TALKER_ID, pair.source, Eui64{}, 0));
    EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE)->status(), atdecc::ACMP_STATUS_NOT_SUPPORTED);

    // 5.5.2.5: the unbind is answered at once, the Listener declaration goes,
    // and the talker, told nothing, stops when SRP shows nobody listening.
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE)->status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_FALSE(pair.last(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND).has_value());
    EXPECT_EQ(sink.state(), MilanSinkState::Unbound);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Idle);
    pair.run(8 * ROUNDS_PER_SECOND);
    EXPECT_FALSE(pair.talker_transmits());
    EXPECT_EQ(
        pair.talker->host().components().msrp_handler.get_talker_stream(pair.stream_id())->state,
        nanoavb::TalkerReservationState::Waiting);

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_acmp, a_milan_sink_whose_talker_withdraws_its_attribute_probes_again)
{
    // 5.5.2.6: an interrupted stream. The talker's stop takes its Talker
    // attribute away; the sink drops its parameters and its declaration and
    // waits for the talker's ADP before it asks again.
    using atdecc::MilanSinkState;
    KitPair pair{"milan13"};
    auto const& sink = *pair.listener->host().components().milan_acmp_listener->sink(pair.sink);
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);

    (void)pair.talker->host().components().msrp_handler.talker_withdraw(pair.stream_id(), pair.now);
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::PrbWAvail);  // the talker was never discovered over ADP
    EXPECT_TRUE(sink.is_bound());
    EXPECT_EQ(sink.context().srp.stream_id, Eui64{});
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Idle);

    // The talker back on the network: its ADP, a random delay of at most a second, a probe, and the stream again.
    (void)pair.talker->host().components().msrp_handler.talker_advertise(
        pair.talker->make_talker_srp_info(pair.talker.talker_specs().front()), pair.now);
    size_t const before = pair.wire.size();
    talker_advertises(pair, 1);
    EXPECT_EQ(sink.state(), MilanSinkState::PrbWDelay);
    pair.run(3 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(pair.wire.size() > before);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);
    EXPECT_TRUE(pair.talker_transmits());

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_acmp, a_classic_pair_connects_as_ieee_1722_1_has_it)
{
    // The same two kits held to IEEE 1722.1-2021: the listener relays the
    // connect to the talker, the talker counts the listener, and the
    // controller is answered once the talker has answered.
    KitPair pair{"classic"};
    EXPECT_EQ(
        pair.talker->host().components().msrp_handler.get_talker_stream(pair.stream_id())->state,
        nanoavb::TalkerReservationState::Advertising);  // declared from the start
    pair.bind();
    auto const relayed = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    auto const answer = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    auto const connected = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_TRUE(relayed.has_value() && answer.has_value() && connected.has_value());
    EXPECT_EQ(answer->connection_count.get(), 1);
    EXPECT_EQ(connected->status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(connected->stream_id, pair.talker->host().components().acmp_talker.get_stream(pair.source)->stream_id);
    EXPECT_TRUE(pair.listener->host().components().acmp_listener.is_connected(pair.sink));
    EXPECT_EQ(pair.talker->host().components().acmp_talker.connection_count(pair.source), 1U);
    EXPECT_TRUE(pair.listener->host().components().listener_sink(pair.sink)->connected);

    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Ready);
    EXPECT_TRUE(pair.talker_transmits());

    // The disconnect goes through the talker, which forgets the listener and stops.
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    EXPECT_TRUE(pair.last(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND).has_value());
    EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE)->status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(pair.talker->host().components().acmp_talker.connection_count(pair.source), 0U);
    EXPECT_FALSE(pair.talker_transmits());

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_acmp, a_milan_talker_without_a_valid_address_says_so_to_a_probe)
{
    // 5.5.4.1 step 3: after MAAP reports the range lost, a listener that
    // probes is told, and retries in four seconds (5.5.3.6.18 step 3).
    KitUnderTest kit{make_config(2, "milan13")};
    bring_up(kit);
    std::vector<atdecc::AcmpCommandResponse> sent{};
    kit->host().components().milan_acmp_talker->set_tx_response([&sent](atdecc::AcmpCommandResponse const& pdu) {
        sent.push_back(pdu);
        return true;
    });
    uint16_t const index = kit.talker_specs().front().index;
    probe_talker_stream(kit, index, TimePoint{std::chrono::seconds{4}});
    EXPECT_EQ(sent.back().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(sent.back().stream_dest_mac, kit->host().components().talker_stream(index)->stream_dest_mac);

    kit->host().components().msrp_handler.set_talker_addresses_valid(false);  // what the MAAP helper does on a conflict
    probe_talker_stream(kit, index, TimePoint{std::chrono::seconds{5}});
    EXPECT_EQ(sent.back().status(), atdecc::ACMP_STATUS_TALKER_DEST_MAC_FAIL);

    kit->host().components().msrp_handler.set_talker_addresses_valid(true);
    probe_talker_stream(kit, index, TimePoint{std::chrono::seconds{6}});
    EXPECT_EQ(sent.back().status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{7}}));
}

TEST(avb_entity_kit_acmp, a_locked_milan_entity_refuses_another_controllers_bind)
{
    // 5.5.2.4: the lock is the AEM handler's; ACMP asks it.
    KitPair pair{"milan13"};
    lock_entity(pair.listener, OTHER_CONTROLLER_ID);
    pair.bind();
    EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE)->status(), atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_FALSE(pair.listener->host().components().milan_acmp_listener->is_bound(pair.sink));
    EXPECT_FALSE(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND).has_value());

    pair.listener->host().components().aem_handler.release_lock();
    lock_entity(pair.listener, CONTROLLER_ID);  // the controller that binds holds the lock
    pair.bind();
    EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE)->status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->is_bound(pair.sink));

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_acmp, a_milan_binding_is_in_the_file_before_the_answer_and_starts_the_next_entity_bound)
{
    // 5.5.2.4 and 5.5.3.6.2
    using atdecc::MilanSinkState;
    auto const path = (std::filesystem::temp_directory_path() / "statusbar_milan_bindings_functional_test.txt").string();
    (void)std::filesystem::remove(path);
    uint16_t sink_index = 0;
    {
        KitPair pair{"milan13", path};
        sink_index = pair.sink;
        auto bind =
            controller_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink);
        bind.flags = atdecc::acmp_flags::STREAMING_WAIT;
        pair.controller_sends(bind);
        EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE)->status(), atdecc::ACMP_STATUS_SUCCESS);
        auto const saved = load_saved_listener_bindings(path);
        EXPECT_TRUE(saved.has_value());
        EXPECT_EQ(saved->size(), 1U);
        EXPECT_EQ(saved->front().listener_unique_id, pair.sink);
        EXPECT_EQ(saved->front().talker_entity_id, TALKER_ID);
        EXPECT_EQ(saved->front().talker_unique_id, pair.source);
        EXPECT_EQ(saved->front().controller_entity_id, CONTROLLER_ID);
        EXPECT_TRUE(saved->front().streaming_wait);
        EXPECT_TRUE(pair.listener.stop(pair.now));
        EXPECT_TRUE(pair.talker.stop(pair.now));
    }

    // The entity started again, as after a power cycle: bound, and waiting for its talker's ADP.
    KitPair again{"milan13", path};
    auto const& sink = *again.listener->host().components().milan_acmp_listener->sink(sink_index);
    EXPECT_EQ(sink.state(), MilanSinkState::PrbWAvail);
    EXPECT_EQ(sink.context().binding->talker_entity_id, TALKER_ID);
    EXPECT_EQ(sink.context().binding->controller_entity_id, CONTROLLER_ID);
    EXPECT_TRUE(sink.context().binding->streaming_wait);
    again.run(8 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(again.wire.empty());  // nothing is asked of a talker not yet heard from

    talker_advertises(again, 1);
    again.run(3 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);
    EXPECT_TRUE(again.talker_transmits());

    // An unbind takes it out of the file too.
    again.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, again.source, LISTENER_ID, sink_index));
    EXPECT_TRUE(load_saved_listener_bindings(path)->empty());
    EXPECT_TRUE(again.listener.stop(again.now));
    EXPECT_TRUE(again.talker.stop(again.now));
    (void)std::filesystem::remove(path);
}

TEST(avb_entity_kit_acmp, without_a_bindings_file_a_milan_binding_lasts_a_stop_and_start_of_the_kit)
{
    using atdecc::MilanSinkState;
    KitPair pair{"milan13"};
    auto const& sink = *pair.listener->host().components().milan_acmp_listener->sink(pair.sink);
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.listener.start(pair.now));
    EXPECT_EQ(sink.state(), MilanSinkState::PrbWAvail);  // as after a power cycle (5.5.1.4)
    EXPECT_TRUE(sink.is_bound());
    EXPECT_EQ(sink.context().srp.stream_id, Eui64{});
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Idle);

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_kit_functional_test)
