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
#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_state_dir.hpp"
#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_stream_formats_file.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp_common_header.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/avtp/avtp_stream_format.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_test_port.hpp"
#include "statusbar/net/net_vlan_link.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/srp/srp_msrp_participant.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/test/test_data.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <unistd.h>

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

/// Whether ADP is advertising: up, waiting out TMR_ADVERTISE or TMR_DELAY (Milan v1.2 Table 5.51).
auto adp_advertising(nanoavb::NanoAvbAdpAdvertiser const& adp) -> bool
{
    auto const state = adp.state();
    return state == nanoavb::AdpAdvertiserState::Waiting || state == nanoavb::AdpAdvertiserState::Delay;
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
    KitUnderTest kit{make_config(2, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    auto& host = kit->host();
    auto const advertising = [&host] { return adp_advertising(host.components().adp_advertiser); };
    host.hold_adp(true);
    host.on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_FALSE(advertising());  // held
    EXPECT_TRUE(host.adp_owed());
    host.release_adp(TimePoint{std::chrono::seconds{3}});  // the block is defended
    EXPECT_TRUE(advertising());
    EXPECT_FALSE(host.adp_owed());
    host.release_adp(TimePoint{std::chrono::seconds{3}});  // a re-defend owes nothing
    EXPECT_TRUE(advertising());

    host.on_link_down(TimePoint{std::chrono::seconds{4}});
    EXPECT_FALSE(host.adp_owed());
    EXPECT_FALSE(advertising());
    host.on_link_up(TimePoint{std::chrono::seconds{5}});
    EXPECT_TRUE(host.adp_owed());  // the bounce asks again
    host.on_link_down(TimePoint{std::chrono::seconds{6}});
    EXPECT_FALSE(host.adp_owed());  // and a Link Down before the defend cancels it
    host.release_adp(TimePoint{std::chrono::seconds{7}});
    EXPECT_FALSE(advertising());

    host.hold_adp(false);
    host.on_link_up(TimePoint{std::chrono::seconds{8}});
    EXPECT_TRUE(advertising());  // no hold: at once, as a static kit
    EXPECT_FALSE(host.adp_owed());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{9}}));
}

TEST(avb_entity_kit_maap, without_a_maap_socket_adp_starts_at_link_up_as_before)
{
    // The fixture's interface does not exist, so the acquirer falls back to
    // the static addresses and is ready from the start: the kit's Link Up
    // holds nothing and ADP starts at once, as it does in static mode.
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(adp_advertising(kit->host().components().adp_advertiser));
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
        : KitPair{profile, make_io_config(LISTENER_ID, profile, std::move(bindings_path))}
    {}
    /// The listener from @p listener_config, the talker a plain one of @p profile.
    KitPair(std::string_view const profile, AvbEntityAudioIOConfig listener_config)
        : listener{std::move(listener_config)}
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

    /// @p rounds of 125 ms: both kits told the cycle's time (the pair's own, on both clocks),
    /// their protocol timers, then the wire.
    void run(int const rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{125};
            for (auto* kit : {&listener, &talker}) {
                (*kit)->tell_cycle_time(ns_of(now), realtime::MonotonicRawClock::time_point{now.time_since_epoch()});
                (*kit)->host().components().msrp_handler.tick(now);
                (*kit)->host().net_handlers()->atdecc_handler().tick(ns_of(now));
            }
            deliver();
        }
    }

    /// Neither kit originates a LeaveAll from here on. A kit's leavealltimer
    /// is drawn at random, 10 to 15 s, and a LeaveAll that falls in the round
    /// of a withdrawal carries the departure in place of a Leave: the peer
    /// then lets go a LeaveTime later, five seconds under Milan's timers,
    /// and not at once. A test that times a departure takes that case out,
    /// so that what it sees does not depend on the draw.
    void no_leave_alls()
    {
        for (auto* kit : {&listener, &talker}) {
            (*kit)->host().components().msrp_handler.set_suppress_leaveall(true);
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

//
// GET_AS_PATH - IEEE 1722.1-2021 Clause 7.4.41
//

namespace {

/// The unsolicited GET_AS_PATH responses among @p sent.
auto as_path_notifications(std::vector<std::vector<uint8_t>> const& sent) -> std::vector<std::vector<uint8_t>>
{
    std::vector<std::vector<uint8_t>> found;
    for (auto const& frame : sent) {
        atdecc::AemDu header{};
        span_load(header, std::span<uint8_t const>{frame});
        if (header.is_unsolicited() && header.command_code() == atdecc::AEM_COMMAND_GET_AS_PATH) {
            found.emplace_back(frame.begin() + atdecc::AemDu::LENGTH, frame.end());
        }
    }
    return found;
}

/// The path a GET_AS_PATH response @p body carries.
auto path_of(std::span<uint8_t const> const body) -> std::vector<Eui64>
{
    atdecc::aem::AemAsPathResponseHeader header{};
    span_load(header, body);
    std::vector<Eui64> path(header.count.get());
    for (size_t i = 0; i < path.size(); ++i) {
        span_load(path[i], body.subspan(atdecc::aem::AemAsPathResponseHeader::LENGTH + (i * Eui64::LENGTH)));
    }
    return path;
}

}  // namespace

TEST(avb_entity_kit_as_path, a_new_path_trace_is_notified_after_the_gptp_callbacks_are_replaced)
{
    KitUnderTest kit{make_io_config(TALKER_ID, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    auto& aem = kit->host().components().aem_handler;
    std::vector<std::vector<uint8_t>> sent;
    aem.set_send_response([&sent](Eui48 const&, std::span<uint8_t const> const frame) {
        sent.emplace_back(frame.begin(), frame.end());
        return true;
    });
    atdecc::AemDu reg{};
    reg.init_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, atdecc::AemDu::AEM_DATA_LENGTH);
    reg.target_entity_id = TALKER_ID;
    reg.controller_entity_id = CONTROLLER_ID;
    std::array<uint8_t, atdecc::AemDu::LENGTH> reg_frame{};
    span_store(reg_frame, reg);
    (void)aem.process_packet(Eui48{0x02, 0, 0, 0, 0, 0x77}, reg_frame, TALKER_ID);

    // An integrator's own gPTP callbacks, installed whole after start, as the tools do.
    auto& announces = kit->host().net_handlers()->gptp_handler();
    announces.set_callbacks(nanoavb::GptpAnnounceCallbacks{});
    Eui64 const gm{0x00, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
    Eui64 const bridge{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x02};
    nanoavb::AsPath trace{};
    trace.push_back(gm);
    trace.push_back(bridge);
    gptp::AnnounceMessage announce{};
    announce.init(1);
    announce.grandmaster_identity = gptp::ClockIdentity{gm};
    announces.observe_announce(2'000'000'000, announce, trace);
    announces.observe_announce(3'000'000'000, announce, trace);  // the same path: nothing new

    auto const notified = as_path_notifications(sent);
    EXPECT_EQ(notified.size(), size_t{1});
    nanoavb::AsPath answered{};
    EXPECT_TRUE(kit->host().as_path(0, answered));
    std::array const announced{gm, bridge};
    EXPECT_TRUE(std::ranges::equal(std::span{answered}.first(std::min(answered.size(), announced.size())), announced));
    auto const notified_path = notified.empty() ? std::vector<Eui64>{} : path_of(notified.back());
    EXPECT_TRUE(std::ranges::equal(notified_path, answered));

    // Announces stop: the path is gone, and that is notified too.
    announces.tick(10'000'000'000);
    EXPECT_EQ(as_path_notifications(sent).size(), size_t{2});
    EXPECT_TRUE(path_of(as_path_notifications(sent).back()).empty());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

//
// SET_MAX_TRANSIT_TIME / GET_MAX_TRANSIT_TIME - IEEE 1722.1-2021 Clauses 7.4.76, 7.4.77
//

namespace {

/// @p kit's answer to MAX_TRANSIT_TIME @p command for talker stream @p index, @p time_ns asked
/// of a SET: the status and the time the response carries.
auto transit_time_command(KitUnderTest& kit, uint16_t const command, uint16_t const index, uint64_t const time_ns = 0)
    -> std::pair<uint8_t, uint64_t>
{
    std::array<uint8_t, atdecc::aem::AemSetMaxTransitTimePayload::LENGTH> body{};
    span_store(
        body,
        atdecc::aem::AemSetMaxTransitTimePayload{
            .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, .descriptor_index = index, .max_transit_time = time_ns});
    bool const set = command == atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME;
    auto const sent = set ? std::span<uint8_t const>{body} : std::span<uint8_t const>{body}.first(4);
    atdecc::AemDu header{};
    header.init_command(command, static_cast<uint16_t>(atdecc::AemDu::AEM_DATA_LENGTH + sent.size()));
    header.controller_entity_id = CONTROLLER_ID;
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const r = kit->host().components().aem_handler.handle_command(header, sent, out);
    atdecc::aem::AemSetMaxTransitTimePayload answer{};
    span_load(answer, std::span<uint8_t const>{out});
    return {r.status, r.size >= body.size() ? answer.max_transit_time.get() : 0};
}

}  // namespace

TEST(avb_entity_kit_max_transit_time, without_an_entity_hook_the_configured_offset_is_answered_and_kept)
{
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    uint16_t const index = kit.talker_specs().front().index;
    uint64_t const configured = kit->presentation_offset_ns(index);
    EXPECT_EQ(configured, uint64_t{1'000'000});  // the classic default
    auto const get = transit_time_command(kit, atdecc::AEM_COMMAND_GET_MAX_TRANSIT_TIME, index);
    EXPECT_EQ(get.first, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(get.second, configured);
    auto const set = transit_time_command(kit, atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, index, 1'500'000);
    EXPECT_EQ(set.first, atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(set.second, configured);
    EXPECT_EQ(uint64_t{kit->presentation_offset_ns(index)}, configured);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_max_transit_time, an_entity_hook_puts_a_new_offset_in_force_or_refuses_it)
{
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    uint16_t const index = kit.talker_specs().front().index;
    std::vector<std::pair<uint16_t, uint32_t>> applied;
    uint8_t answer = atdecc::AEM_STATUS_SUCCESS;
    kit->set_on_presentation_offset_changed([&](uint16_t const idx, uint32_t const offset_ns) -> uint8_t {
        applied.emplace_back(idx, offset_ns);
        return answer;
    });

    auto const set = transit_time_command(kit, atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, index, 1'500'000);
    EXPECT_EQ(set.first, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(applied == (std::vector<std::pair<uint16_t, uint32_t>>{{index, 1'500'000}}));
    EXPECT_EQ(transit_time_command(kit, atdecc::AEM_COMMAND_GET_MAX_TRANSIT_TIME, index).second, uint64_t{1'500'000});
    std::array<uint8_t, 4> const stream_output{0x00, 0x06, 0x00, static_cast<uint8_t>(index)};
    atdecc::AemDu get_info{};
    get_info.init_command(atdecc::AEM_COMMAND_GET_STREAM_INFO, atdecc::AemDu::AEM_DATA_LENGTH + 4);
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    EXPECT_EQ(kit->host().components().aem_handler.handle_command(get_info, stream_output, out).status, atdecc::AEM_STATUS_SUCCESS);
    atdecc::aem::AemStreamInfoPayload info{};
    span_load(info, std::span<uint8_t const>{out});
    EXPECT_EQ(info.msrp_accumulated_latency.get(), 1'500'000U);

    // The entity cannot while the stream runs: the time in force stays.
    answer = atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    auto const refused = transit_time_command(kit, atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, index, 3'000'000);
    EXPECT_EQ(refused.first, atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(refused.second, uint64_t{1'500'000});
    EXPECT_EQ(kit->presentation_offset_ns(index), 1'500'000U);

    // The kit holds no deferred offset: an IN_PROGRESS is refused, nothing deferred or kept.
    answer = atdecc::AEM_STATUS_IN_PROGRESS;
    auto const deferred = transit_time_command(kit, atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, index, 3'000'000);
    EXPECT_EQ(deferred.first, atdecc::AEM_STATUS_ENTITY_MISBEHAVING);
    EXPECT_EQ(deferred.second, uint64_t{1'500'000});
    EXPECT_EQ(kit->host().components().aem_handler.pending_command_count(), size_t{0});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

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
    pair.no_leave_alls();  // the unbind below is timed
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
    // 5.3.8.5: the Listener declaration follows the Talker attribute that was probed, and no other.
    auto const followed = pair.listener->host().components().msrp_handler.get_listener_stream(pair.stream_id())->follows;
    EXPECT_TRUE(followed.has_value());
    EXPECT_TRUE(followed.has_value() && followed->dest_address == sink.context().srp.stream_dest_mac && followed->vlan_id == 2);

    // It holds with no further ACMP: past the probe's 15 seconds and through
    // several LeaveAlls, at every round, with no grace to carry it (the
    // registrar keeps the Listener registered for LeaveTime).
    size_t const acmp_so_far = pair.wire.size();
    bool held = true;
    for (int round = 0; round < 40 * ROUNDS_PER_SECOND; ++round) {
        pair.run(1);
        held = held && pair.talker_transmits() && sink.state() == MilanSinkState::SettledRsvOk;
    }
    EXPECT_TRUE(held);
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
    // The gate has no grace under Milan: the stream stops with the Listener's
    // registration, a join time after the unbind, not five seconds later.
    pair.run(ROUNDS_PER_SECOND);
    EXPECT_FALSE(pair.talker_transmits());
    pair.run(7 * ROUNDS_PER_SECOND);
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
    pair.no_leave_alls();  // the withdrawal below is timed
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
    // An IEEE 1722.1 connection follows any Talker attribute of its StreamID.
    EXPECT_FALSE(pair.listener->host().components().msrp_handler.get_listener_stream(pair.stream_id())->follows.has_value());

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
    // 5.3.8.7: the stopped state is restored with the binding, before any frame can arrive.
    EXPECT_TRUE(again.listener->listener()->stream_stopped(sink_index));
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

TEST(avb_entity_kit_acmp, an_ieee_1722_1_listener_started_with_a_saved_connection_connects_to_its_talker_again)
{
    // IEEE 1722.1-2021 8.2.2.1.1: a listener restoring a saved connection is its
    // own controller. Its CONNECT_TX_COMMAND crosses the wire as octets here, so
    // the talker answers only a well-formed ACMPDU.
    auto const path = (std::filesystem::temp_directory_path() / "statusbar_ieee_bindings_functional_test.txt").string();
    (void)std::filesystem::remove(path);
    {
        KitPair pair{"classic", path};
        pair.bind();
        pair.run(2 * ROUNDS_PER_SECOND);
        EXPECT_TRUE(pair.listener->host().components().acmp_listener.is_connected(pair.sink));
        EXPECT_EQ(load_saved_listener_bindings(path)->size(), 1U);
        EXPECT_TRUE(pair.listener.stop(pair.now));
        EXPECT_TRUE(pair.talker.stop(pair.now));
    }

    // Both entities started again, as after a power cycle, and no controller anywhere.
    KitPair again{"classic", path};
    EXPECT_FALSE(again.listener->host().components().acmp_listener.is_connected(again.sink));
    again.run(4 * ROUNDS_PER_SECOND);
    auto const asked = again.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    auto const answer = again.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_TRUE(asked.has_value() && answer.has_value());
    EXPECT_EQ(asked.value_or(atdecc::AcmpCommandResponse{}).controller_entity_id, LISTENER_ID);
    EXPECT_EQ(answer.value_or(atdecc::AcmpCommandResponse{}).connection_count.get(), 1);
    EXPECT_TRUE(again.listener->host().components().acmp_listener.is_connected(again.sink));
    EXPECT_EQ(again.listener_srp_state(), nanoavb::ListenerReservationState::Ready);
    EXPECT_TRUE(again.talker_transmits());

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

//
// BIND_STREAM, UNBIND_STREAM and GET_STREAM_INPUT_INFO_EX - Milan v1.3 5.4.4.6 to 5.4.4.8
//

namespace {

/// A Milan pair whose listener is a Milan model, so that it answers MVU, and
/// a controller that speaks MVU to it: each response is kept.
struct MvuKitPair
{
    KitPair pair;
    std::vector<std::vector<uint8_t>> responses{};

    MvuKitPair()
        : pair{"milan13", milan_listener_config()}
    {
        pair.listener->host().components().aem_handler.set_send_response(
            [this](Eui48 const&, std::span<uint8_t const> const response) {
                responses.emplace_back(response.begin(), response.end());
                return true;
            });
    }

    static auto milan_listener_config() -> AvbEntityAudioIOConfig
    {
        auto config = make_io_config(LISTENER_ID, "milan13");
        config.milan = true;
        return config;
    }

    /// The controller sends @p data as the MVU command @p command to the listener.
    template <typename Data>
    void controller_sends(uint16_t const command, Data const& data, Eui64 const& controller = CONTROLLER_ID)
    {
        atdecc::AecpMvuDu du{};
        du.init_command(command, static_cast<uint16_t>(Data::LENGTH));
        du.common.target_entity_id = LISTENER_ID;
        du.common.controller_entity_id = controller;
        std::vector<uint8_t> frame(atdecc::AecpMvuDu::LENGTH + Data::LENGTH);
        span_store(std::span<uint8_t>{frame}.first(atdecc::AecpMvuDu::LENGTH), du);
        span_store(std::span<uint8_t>{frame}.subspan(atdecc::AecpMvuDu::LENGTH), data);
        pair.listener->host().net_handlers()->atdecc_handler().dispatch_frame(
            ns_of(pair.now), Eui48{0x02, 0, 0, 0, 0, 0x09}, frame);
        pair.deliver();
    }

    void bind_stream()
    {
        atdecc::MvuBindStream bind{};
        bind.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT;
        bind.descriptor_index = pair.sink;
        bind.talker_entity_id = TALKER_ID;
        bind.talker_stream_index = pair.source;
        controller_sends(atdecc::MVU_COMMAND_BIND_STREAM, bind);
    }

    [[nodiscard]] auto sink_named() const -> atdecc::MvuStreamInput
    {
        atdecc::MvuStreamInput input{};
        input.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT;
        input.descriptor_index = pair.sink;
        return input;
    }

    /// The status of the last response.
    [[nodiscard]] auto last_status() const -> uint8_t
    {
        atdecc::AecpMvuDu du{};
        span_load(du, std::span<uint8_t const>{responses.back()}.first(atdecc::AecpMvuDu::LENGTH));
        return du.common.status();
    }

    /// The sink's info, as GET_STREAM_INPUT_INFO_EX answers it now.
    auto info() -> atdecc::MvuGetStreamInputInfoExResponse
    {
        controller_sends(atdecc::MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, sink_named());
        atdecc::MvuGetStreamInputInfoExResponse answer{};
        span_load(answer, std::span<uint8_t const>{responses.back()}.subspan(atdecc::AecpMvuDu::LENGTH));
        return answer;
    }
};

}  // namespace

TEST(avb_entity_kit_mvu, a_milan_listener_bound_with_bind_stream_settles_and_says_how_far_it_is)
{
    using atdecc::MilanSinkState;
    MvuKitPair mvu;
    auto& pair = mvu.pair;
    auto const& sink = *pair.listener->host().components().milan_acmp_listener->sink(pair.sink);
    EXPECT_EQ(mvu.info().pbsta(), 0);  // PROBING_DISABLED

    // 5.5.3.2: the bind is the sink's, whichever protocol brought it. The
    // answer is the MVU one and no BIND_RX_RESPONSE goes out beside it.
    mvu.bind_stream();
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_SUCCESS);
    EXPECT_FALSE(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE).has_value());
    EXPECT_TRUE(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND).has_value());
    EXPECT_EQ(pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND)->controller_entity_id, CONTROLLER_ID);
    EXPECT_TRUE(sink.is_settled());

    auto const settled = mvu.info();
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_SUCCESS);
    EXPECT_EQ(settled.talker_entity_id, TALKER_ID);
    EXPECT_EQ(settled.talker_unique_id.get(), pair.source);
    EXPECT_EQ(settled.pbsta(), 3);  // PROBING_COMPLETED
    EXPECT_EQ(settled.acmpsta(), 0);

    // The reservation follows as after an ACMP bind.
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(sink.state(), MilanSinkState::SettledRsvOk);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Ready);

    mvu.controller_sends(atdecc::MVU_COMMAND_UNBIND_STREAM, mvu.sink_named());
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_SUCCESS);
    EXPECT_FALSE(sink.is_bound());
    EXPECT_FALSE(pair.last(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE).has_value());
    auto const unbound = mvu.info();
    EXPECT_EQ(unbound.talker_entity_id, Eui64{});
    EXPECT_EQ(unbound.pbsta(), 0);

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_mvu, a_locked_milan_entity_answers_another_controllers_bind_stream_entity_locked)
{
    // Tables 5.33 and 5.39: the same lock that refuses a BIND_RX_COMMAND
    // CONTROLLER_NOT_AUTHORIZED, under the name AECP has for it.
    MvuKitPair mvu;
    auto& pair = mvu.pair;
    lock_entity(pair.listener, OTHER_CONTROLLER_ID);
    mvu.bind_stream();
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_ENTITY_LOCKED);
    EXPECT_FALSE(pair.listener->host().components().milan_acmp_listener->is_bound(pair.sink));
    EXPECT_TRUE(pair.wire.empty());

    pair.listener->host().components().aem_handler.release_lock();
    lock_entity(pair.listener, CONTROLLER_ID);  // the controller that binds holds the lock
    mvu.bind_stream();
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_SUCCESS);
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->is_bound(pair.sink));

    // Locked out of the unbind too, and of nothing that only reads.
    pair.listener->host().components().aem_handler.release_lock();
    lock_entity(pair.listener, OTHER_CONTROLLER_ID);
    mvu.controller_sends(atdecc::MVU_COMMAND_UNBIND_STREAM, mvu.sink_named());
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_ENTITY_LOCKED);
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->is_bound(pair.sink));
    EXPECT_EQ(mvu.info().talker_entity_id, TALKER_ID);
    EXPECT_EQ(mvu.last_status(), atdecc::MVU_STATUS_SUCCESS);

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_mvu, a_milan_model_held_to_ieee_1722_1_says_it_does_not_implement_bind_stream)
{
    // It answers GET_MILAN_INFO, without MVU_BINDING, and has no sink to bind.
    auto config = make_io_config(LISTENER_ID, "classic");
    config.milan = true;
    KitUnderTest kit{config};
    bring_up(kit);
    std::vector<std::vector<uint8_t>> responses{};
    kit->host().components().aem_handler.set_send_response([&responses](Eui48 const&, std::span<uint8_t const> const response) {
        responses.emplace_back(response.begin(), response.end());
        return true;
    });
    atdecc::MvuBindStream bind{};
    bind.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    bind.talker_entity_id = TALKER_ID;
    atdecc::AecpMvuDu du{};
    du.init_command(atdecc::MVU_COMMAND_BIND_STREAM, atdecc::MvuBindStream::LENGTH);
    du.common.target_entity_id = LISTENER_ID;
    du.common.controller_entity_id = CONTROLLER_ID;
    std::vector<uint8_t> frame(atdecc::AecpMvuDu::LENGTH + atdecc::MvuBindStream::LENGTH);
    span_store(std::span<uint8_t>{frame}.first(atdecc::AecpMvuDu::LENGTH), du);
    span_store(std::span<uint8_t>{frame}.subspan(atdecc::AecpMvuDu::LENGTH), bind);
    kit->host().net_handlers()->atdecc_handler().dispatch_frame(0, Eui48{0x02, 0, 0, 0, 0, 0x09}, frame);
    EXPECT_EQ(responses.size(), 1U);
    if (responses.size() == 1) {
        atdecc::AecpMvuDu answer{};
        span_load(answer, std::span<uint8_t const>{responses.front()}.first(atdecc::AecpMvuDu::LENGTH));
        EXPECT_EQ(answer.common.status(), atdecc::MVU_STATUS_NOT_IMPLEMENTED);
    }
    EXPECT_TRUE(kit.stop(TimePoint{}));
}

//
// A stopped stream input - Milan v1.3 5.3.8.7, 5.4.2.19, 5.4.2.20
//

namespace {

/// One stream descriptor of a kit.
struct StreamOf
{
    KitUnderTest& kit;
    uint16_t index{0};
    uint16_t type{atdecc::aem::DESCRIPTOR_STREAM_INPUT};
};

/// START_STREAMING or STOP_STREAMING (@p command) for @p stream, from @p controller: the status.
auto streaming_command(StreamOf const stream, uint16_t const command, Eui64 const& controller = CONTROLLER_ID) -> uint8_t
{
    atdecc::AemDu header{};
    header.init_command(command, atdecc::aem::AemStreamingPayload::LENGTH);
    header.controller_entity_id = controller;
    atdecc::aem::AemStreamingPayload payload{};
    payload.descriptor_type = stream.type;
    payload.descriptor_index = stream.index;
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    return stream.kit->host().components().aem_handler.handle_command(header, make_const_span(payload), out).status;
}

/// START_STREAMING or STOP_STREAMING (@p command) for STREAM_INPUT @p index of @p kit.
auto streaming_command(KitUnderTest& kit, uint16_t const command, uint16_t const index, Eui64 const& controller = CONTROLLER_ID)
    -> uint8_t
{
    return streaming_command(StreamOf{.kit = kit, .index = index}, command, controller);
}

/// Whether GET_STREAM_INFO reports @p index of @p kit, a STREAM_INPUT unless @p type says otherwise, as STREAMING_WAIT.
auto reports_streaming_wait(KitUnderTest& kit, uint16_t const index, uint16_t const type = atdecc::aem::DESCRIPTOR_STREAM_INPUT)
    -> bool
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_GET_STREAM_INFO, atdecc::aem::AemGetStreamInfoCommandPayload::LENGTH);
    header.controller_entity_id = CONTROLLER_ID;
    atdecc::aem::AemGetStreamInfoCommandPayload asked{};
    asked.descriptor_type = type;
    asked.descriptor_index = index;
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const result = kit->host().components().aem_handler.handle_command(header, make_const_span(asked), out);
    EXPECT_EQ(result.status, atdecc::AEM_STATUS_SUCCESS);
    atdecc::aem::AemStreamInfoPayload info{};
    span_load(info, std::span<uint8_t const>{out}.first(atdecc::aem::AemStreamInfoPayload::LENGTH));
    return (info.flags.get() & atdecc::aem::stream_info_flags::STREAMING_WAIT) != 0;
}

}  // namespace

TEST(avb_entity_kit_stopped, a_milan_input_bound_stopped_discards_until_it_is_started)
{
    // A bind with STREAMING_WAIT binds the input in the stopped state
    // (Table 5.19 and its ACMP twin). The sink probes, settles and reserves
    // all the same, and the talker streams: only the input's own taking of
    // the frames is off (5.3.8.7, 5.5.4.1).
    KitPair pair{"milan13"};
    auto const& sink = *pair.listener->host().components().milan_acmp_listener->sink(pair.sink);
    auto const* const inputs = pair.listener->listener();
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));
    EXPECT_FALSE(reports_streaming_wait(pair.listener, pair.sink));

    auto bind = controller_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink);
    bind.flags = atdecc::acmp_flags::STREAMING_WAIT;
    pair.controller_sends(bind);
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(inputs->stream_stopped(pair.sink));
    EXPECT_TRUE(reports_streaming_wait(pair.listener, pair.sink));
    EXPECT_EQ(sink.state(), atdecc::MilanSinkState::SettledRsvOk);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Ready);
    EXPECT_TRUE(pair.talker_transmits());

    // 5.4.2.19: START_STREAMING on a bound and stopped input starts it, and the state is saved with the binding.
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_START_STREAMING, pair.sink), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));
    EXPECT_FALSE(reports_streaming_wait(pair.listener, pair.sink));
    EXPECT_FALSE(sink.context().binding->streaming_wait);
    EXPECT_EQ(sink.state(), atdecc::MilanSinkState::SettledRsvOk);

    // 5.4.2.20, and back.
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_STOP_STREAMING, pair.sink), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(inputs->stream_stopped(pair.sink));
    EXPECT_TRUE(sink.context().binding->streaming_wait);
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    EXPECT_TRUE(pair.last(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE)->is_streaming_wait());

    // A bind without the flag starts it: the state is the binding's.
    pair.bind();
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_stopped, an_unbound_milan_input_is_not_stopped_and_a_command_for_it_has_no_effect)
{
    // "The started/stopped state is undefined when the Stream Input is not
    // bound", and START and STOP_STREAMING have "no effect on a Stream Input
    // that is not already bound": they are answered, and change nothing.
    KitPair pair{"milan13"};
    auto const* const inputs = pair.listener->listener();
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_STOP_STREAMING, pair.sink), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));
    EXPECT_FALSE(reports_streaming_wait(pair.listener, pair.sink));

    // Bound and stopped, then unbound: the binding's state goes with the binding.
    pair.bind();
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_STOP_STREAMING, pair.sink), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(inputs->stream_stopped(pair.sink));
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));
    EXPECT_FALSE(reports_streaming_wait(pair.listener, pair.sink));
    // A new bind does not find the old state.
    pair.bind();
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_stopped, a_locked_milan_entity_refuses_another_controllers_stop_streaming)
{
    // 5.4.2.20: "it shall not accept a STOP_STREAMING command from a different controller".
    KitPair pair{"milan13"};
    pair.bind();
    lock_entity(pair.listener, OTHER_CONTROLLER_ID);
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_STOP_STREAMING, pair.sink), atdecc::AEM_STATUS_ENTITY_LOCKED);
    EXPECT_FALSE(pair.listener->listener()->stream_stopped(pair.sink));
    EXPECT_EQ(
        streaming_command(pair.listener, atdecc::AEM_COMMAND_STOP_STREAMING, pair.sink, OTHER_CONTROLLER_ID),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(pair.listener->listener()->stream_stopped(pair.sink));

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_stopped, a_milan_stream_output_cannot_be_stopped)
{
    // Milan v1.3 5.4.2.19, 5.4.2.20: "shall not support ... for a Stream
    // Output (NOT_SUPPORTED shall be returned)". 5.3.7.3: an output streams
    // while its Talker attribute is declared and a Listener registered.
    KitPair pair{"milan13"};
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(pair.talker_transmits());
    StreamOf const output{.kit = pair.talker, .index = pair.source, .type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT};

    EXPECT_EQ(streaming_command(output, atdecc::AEM_COMMAND_STOP_STREAMING), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_FALSE(pair.talker->talker().stream_stopped(pair.source));
    EXPECT_TRUE(pair.talker_transmits());
    EXPECT_FALSE(reports_streaming_wait(pair.talker, pair.source, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT));  // Table 5.9: always 0
    EXPECT_EQ(streaming_command(output, atdecc::AEM_COMMAND_START_STREAMING), atdecc::AEM_STATUS_NOT_SUPPORTED);
    pair.run(ROUNDS_PER_SECOND);
    EXPECT_TRUE(pair.talker_transmits());

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_stopped, an_ieee_1722_1_stream_output_is_stopped_and_started_as_before)
{
    // IEEE 1722.1-2021 7.4.35, 7.4.36 and Table 7-145: the classic profile is unchanged.
    KitPair pair{"classic"};
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(pair.talker_transmits());
    StreamOf const output{.kit = pair.talker, .index = pair.source, .type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT};

    // The latch is the talker streams' own, beside the gate: a stopped stream is not sent.
    EXPECT_EQ(streaming_command(output, atdecc::AEM_COMMAND_STOP_STREAMING), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(pair.talker->talker().stream_stopped(pair.source));
    EXPECT_TRUE(reports_streaming_wait(pair.talker, pair.source, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT));
    EXPECT_EQ(streaming_command(output, atdecc::AEM_COMMAND_START_STREAMING), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(pair.talker->talker().stream_stopped(pair.source));
    EXPECT_FALSE(reports_streaming_wait(pair.talker, pair.source, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT));

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_stopped, an_ieee_1722_1_input_is_stopped_and_started_by_the_two_commands)
{
    // Under IEEE 1722.1 connection management the state is the command's
    // alone (7.4.35, 7.4.36): nothing binds an input stopped, nothing saves it.
    KitPair pair{"classic"};
    pair.bind();
    auto const* const inputs = pair.listener->listener();
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_STOP_STREAMING, pair.sink), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(inputs->stream_stopped(pair.sink));
    EXPECT_TRUE(reports_streaming_wait(pair.listener, pair.sink));
    EXPECT_EQ(streaming_command(pair.listener, atdecc::AEM_COMMAND_START_STREAMING, pair.sink), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(inputs->stream_stopped(pair.sink));
    EXPECT_FALSE(reports_streaming_wait(pair.listener, pair.sink));

    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

namespace {

/// An empty state directory of the test's own, and the listener's directory inside it.
struct StateDir
{
    std::filesystem::path state;
    std::filesystem::path entity;

    explicit StateDir(char const* const name)
        : state{std::filesystem::temp_directory_path() / name}
        , entity{state / entity_state_dir_name(LISTENER_ID)}
    {
        std::filesystem::remove_all(state);
        std::filesystem::create_directories(state);
    }
    StateDir(StateDir const&) = delete;
    auto operator=(StateDir const&) -> StateDir& = delete;
    ~StateDir() { std::filesystem::remove_all(state); }

    [[nodiscard]] auto bindings() const -> std::string { return (entity / entity_state_file::LISTENER_BINDINGS).string(); }

    /// The listener's configuration under @p profile with this state directory.
    [[nodiscard]] auto listener_config(std::string_view const profile, std::string bindings_path = {}) const
        -> AvbEntityAudioIOConfig
    {
        auto config = make_io_config(LISTENER_ID, profile, std::move(bindings_path));
        config.state_dir = state.string();
        return config;
    }
};

}  // namespace

TEST(avb_entity_kit_state, a_milan_kit_saves_its_bindings_in_its_own_state_directory_and_starts_bound_from_them)
{
    // Milan v1.3 5.3.8.3 with no file named: <state>/<entity ID>/listener-bindings.
    using atdecc::MilanSinkState;
    StateDir const dir{"statusbar_state_dir_functional_test"};
    uint16_t sink_index = 0;
    {
        KitPair pair{"milan13", dir.listener_config("milan13")};
        sink_index = pair.sink;
        EXPECT_EQ(dir.entity.filename().string(), std::string{"000102030405060a"});
        EXPECT_TRUE(std::filesystem::is_directory(dir.entity));
        pair.bind();
        auto const saved = load_saved_listener_bindings(dir.bindings());
        EXPECT_TRUE(saved.has_value());
        EXPECT_EQ(saved->size(), 1U);
        EXPECT_EQ(saved->front().talker_entity_id, TALKER_ID);
        EXPECT_TRUE(pair.listener.stop(pair.now));
        EXPECT_TRUE(pair.talker.stop(pair.now));
    }
    KitPair again{"milan13", dir.listener_config("milan13")};
    auto const& sink = *again.listener->host().components().milan_acmp_listener->sink(sink_index);
    EXPECT_EQ(sink.state(), MilanSinkState::PrbWAvail);
    EXPECT_EQ(sink.context().binding->talker_entity_id, TALKER_ID);
    EXPECT_TRUE(again.listener.stop(again.now));
    EXPECT_TRUE(again.talker.stop(again.now));
}

TEST(avb_entity_kit_state, a_named_bindings_file_is_used_and_the_state_directory_left_alone)
{
    StateDir const dir{"statusbar_state_dir_named_functional_test"};
    auto const named = (dir.state / "named-bindings.txt").string();
    // Static addresses: a kit that asks MAAP for its own keeps the range it
    // defends in the state directory (the test after next).
    auto config = dir.listener_config("milan13", named);
    config.stream_address_mode = "static";
    KitPair pair{"milan13", std::move(config)};
    pair.bind();
    EXPECT_EQ(load_saved_listener_bindings(named)->size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(dir.entity));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_state, an_ieee_1722_1_kit_saves_no_connection_it_was_not_given_a_file_for)
{
    // Its fast-connect memory changes what a restart does, so it stays a thing asked for by name.
    StateDir const dir{"statusbar_state_dir_classic_functional_test"};
    KitPair pair{"classic", dir.listener_config("classic")};
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_FALSE(std::filesystem::exists(dir.bindings()));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_state, where_the_state_directory_is_not_there_a_milan_kit_makes_none_and_keeps_its_bindings)
{
    using atdecc::MilanSinkState;
    StateDir const dir{"statusbar_state_dir_absent_functional_test"};
    std::filesystem::remove_all(dir.state);
    KitPair pair{"milan13", dir.listener_config("milan13")};
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(pair.listener->host().components().milan_acmp_listener->sink(pair.sink)->state(), MilanSinkState::SettledRsvOk);
    EXPECT_FALSE(std::filesystem::exists(dir.state));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

namespace {

/// The kit's own talker-advertises file inside @p dir.
auto talker_advertises_path(StateDir const& dir, AvbEntityAudioIOConfig const& config) -> std::string
{
    return (dir.state / entity_state_dir_name(config.entity_id) / entity_state_file::TALKER_ADVERTISES).string();
}

/// A classic kit on static addresses with @p dir as its state directory and the SRP gate as @p gated.
auto talker_config(StateDir const& dir, bool const gated) -> AvbEntityAudioIOConfig
{
    auto config = make_config(2, "classic");
    config.state_dir = dir.state.string();
    config.gate_talker_on_listener = gated;
    return config;
}

/// One reactor cycle of @p kit ends at @p seconds.
void end_cycle_at(KitUnderTest& kit, int64_t const seconds)
{
    kit->end_cycle(seconds * 1'000'000'000LL);
}

}  // namespace

// IEEE 802.1Q 35.2.2.8 needs two LeaveAll periods between a stream's old value
// and another under its StreamID, and a restarted entity knows the old value
// only if it kept it. The kit writes what it hands to MSRP into its state
// directory, one line per stream output.
TEST(avb_entity_kit_talker_hold, a_kit_keeps_what_it_declares_for_its_next_start)
{
    StateDir const dir{"statusbar_state_dir_talker_keep_functional_test"};
    auto const config = talker_config(dir, true);
    KitUnderTest kit{config};
    // Nothing is kept of streams MSRP has not been handed yet: before the
    // link is up and a grandmaster is there, their parameters are not final.
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    end_cycle_at(kit, 1);
    EXPECT_FALSE(std::filesystem::exists(talker_advertises_path(dir, config)));
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    kit->on_gptp_announce(TimePoint{std::chrono::seconds{3}}, true);
    EXPECT_FALSE(std::filesystem::exists(talker_advertises_path(dir, config)));  // nor before a cycle has looked
    end_cycle_at(kit, 4);
    auto const kept = load_talker_advertises(talker_advertises_path(dir, config));
    EXPECT_TRUE(kept.has_value());
    EXPECT_EQ(kept->size(), kit.talker_specs().size());
    for (size_t i = 0; i < kept->size() && i < kit.talker_specs().size(); ++i) {
        EXPECT_TRUE((*kept)[i] == saved_talker_advertise(kit->make_talker_srp_info(kit.talker_specs()[i])));
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{5}}));
}

// Started again with the parameters it kept, the kit holds nothing back.
// Started with another destination address than the last run declared for
// one stream, it holds that stream's Advertise back: MSRP says so, the gate
// is shut for it though the SRP gate is off, the stream beside it runs, and
// the file keeps the old line until the wait is over, so that a restart
// inside the wait would hold the stream back again.
TEST(avb_entity_kit_talker_hold, a_kit_restarted_with_another_address_holds_that_stream_back_and_no_other)
{
    StateDir const dir{"statusbar_state_dir_talker_hold_functional_test"};
    auto const config = talker_config(dir, false);
    auto const raw = [](int64_t const seconds) { return realtime::MonotonicRawClock::time_point{std::chrono::seconds{seconds}}; };
    SavedTalkerAdvertises declared{};
    {
        KitUnderTest first{config};
        bring_up(first);
        end_cycle_at(first, 4);
        declared = *load_talker_advertises(talker_advertises_path(dir, config));
        EXPECT_TRUE(declared.size() >= 2);
        EXPECT_TRUE(first.stop(TimePoint{std::chrono::seconds{5}}));
    }
    {
        KitUnderTest same{config};
        bring_up(same);
        end_cycle_at(same, 4);
        for (auto const& spec : same.talker_specs()) {
            EXPECT_FALSE(same->host().components().msrp_handler.talker_held(same->make_talker_srp_info(spec).stream_id));
            EXPECT_TRUE(same->talker_should_transmit(spec.index, raw(4)));
        }
        EXPECT_TRUE(same.stop(TimePoint{std::chrono::seconds{5}}));
    }

    // What the last run is said to have declared for the first stream: another address.
    auto before = declared;
    before[0].dest_address ^= 0x40U;
    EXPECT_TRUE(save_talker_advertises(talker_advertises_path(dir, config), before).has_value());

    KitUnderTest changed{config};
    bring_up(changed);
    end_cycle_at(changed, 4);
    auto const& msrp = changed->host().components().msrp_handler;
    auto const& held = changed.talker_specs()[0];
    auto const& beside = changed.talker_specs()[1];
    EXPECT_TRUE(msrp.talker_held(changed->make_talker_srp_info(held).stream_id));
    EXPECT_FALSE(msrp.talker_held(changed->make_talker_srp_info(beside).stream_id));
    EXPECT_FALSE(changed->talker_should_transmit(held.index, raw(4)));
    EXPECT_TRUE(changed->talker_should_transmit(beside.index, raw(4)));
    auto const kept = load_talker_advertises(talker_advertises_path(dir, config));
    EXPECT_TRUE(kept.has_value() && kept->size() == before.size() && (*kept)[0] == before[0]);
    EXPECT_TRUE(changed.stop(TimePoint{std::chrono::seconds{5}}));
}

// A file that cannot be read holds nothing back, and is written afresh.
TEST(avb_entity_kit_talker_hold, a_corrupt_file_holds_nothing_back_and_is_replaced)
{
    StateDir const dir{"statusbar_state_dir_talker_corrupt_functional_test"};
    auto const config = talker_config(dir, false);
    std::filesystem::create_directories(std::filesystem::path{talker_advertises_path(dir, config)}.parent_path());
    std::ofstream{talker_advertises_path(dir, config)} << "not the file\n";

    KitUnderTest kit{config};
    bring_up(kit);
    end_cycle_at(kit, 4);
    for (auto const& spec : kit.talker_specs()) {
        EXPECT_FALSE(kit->host().components().msrp_handler.talker_held(kit->make_talker_srp_info(spec).stream_id));
    }
    auto const kept = load_talker_advertises(talker_advertises_path(dir, config));
    EXPECT_TRUE(kept.has_value() && kept->size() == kit.talker_specs().size());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{5}}));
}

// With no state directory nothing is kept and nothing is held back.
TEST(avb_entity_kit_talker_hold, with_no_state_directory_nothing_is_kept)
{
    KitUnderTest kit{make_config(2, "classic")};
    bring_up(kit);
    end_cycle_at(kit, 4);
    for (auto const& spec : kit.talker_specs()) {
        EXPECT_FALSE(kit->host().components().msrp_handler.talker_held(kit->make_talker_srp_info(spec).stream_id));
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{5}}));
}

TEST(avb_entity_kit_state, a_maap_kit_has_its_state_directory_for_the_range_it_defends)
{
    // The interface does not exist, so no block is acquired here; the place its range would be saved is the entity's.
    StateDir const dir{"statusbar_state_dir_maap_functional_test"};
    auto config = dir.listener_config("classic");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    EXPECT_TRUE(std::filesystem::is_directory(dir.entity));
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_state, a_milan_kit_given_no_address_mode_asks_maap_and_so_has_its_state_directory)
{
    // Milan v1.3 4.3.3.1: nothing said of the addresses, a Milan talker takes them from MAAP.
    StateDir const dir{"statusbar_state_dir_milan_maap_functional_test"};
    KitUnderTest kit{dir.listener_config("milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    EXPECT_TRUE(std::filesystem::is_directory(dir.entity));
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

//
// The format of a stream input - Milan v1.3 5.3.8.1, 5.4.2.7 and 6.5
//

namespace {

// examples/dual.json: STREAM_INPUT 1 and STREAM_OUTPUT 1 are the AAF pair,
// 96 kHz, 32 bit, eight channels of twelve samples. The input lists that
// format "up to" eight channels.
constexpr uint16_t AAF_STREAM = 1;
constexpr uint64_t AAF_96K_8CH = 0x020702200200C000ULL;

constexpr auto aaf_96k_of(uint64_t const channels) noexcept -> uint64_t
{
    return (AAF_96K_8CH & ~avtp::AAF_STREAM_FORMAT_CHANNELS_MASK) | (channels << 22);
}

/// SET_STREAM_FORMAT of @p stream to @p format, from CONTROLLER_ID: the status.
auto set_stream_format(StreamOf const stream, uint64_t const format) -> uint8_t
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, atdecc::aem::AemStreamFormatPayload::LENGTH);
    header.controller_entity_id = CONTROLLER_ID;
    std::array<uint8_t, atdecc::aem::AemStreamFormatPayload::LENGTH> body{};
    span_store(
        std::span<uint8_t>{body},
        atdecc::aem::AemStreamFormatPayload{.descriptor_type = stream.type, .descriptor_index = stream.index});
    span_store(
        std::span<uint8_t>{body}.subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format)), ieee::octlet_t{format});
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    return stream.kit->host().components().aem_handler.handle_command(header, body, out).status;
}

/// The format GET_STREAM_FORMAT reports for @p stream.
auto reported_stream_format(StreamOf const stream) -> uint64_t
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_GET_STREAM_FORMAT, atdecc::aem::AemGetStreamFormatCommandPayload::LENGTH);
    header.controller_entity_id = CONTROLLER_ID;
    atdecc::aem::AemGetStreamFormatCommandPayload const asked{.descriptor_type = stream.type, .descriptor_index = stream.index};
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const result = stream.kit->host().components().aem_handler.handle_command(header, make_const_span(asked), out);
    EXPECT_EQ(result.status, atdecc::AEM_STATUS_SUCCESS);
    ieee::octlet_t format{0};
    span_load(format, std::span<uint8_t const>{out}.subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format), 8));
    return format.get();
}

/// The format GET_STREAM_INFO reports for @p stream, which it marks valid.
auto stream_info_format(StreamOf const stream) -> uint64_t
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_GET_STREAM_INFO, atdecc::aem::AemGetStreamInfoCommandPayload::LENGTH);
    header.controller_entity_id = CONTROLLER_ID;
    atdecc::aem::AemGetStreamInfoCommandPayload const asked{.descriptor_type = stream.type, .descriptor_index = stream.index};
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    auto const result = stream.kit->host().components().aem_handler.handle_command(header, make_const_span(asked), out);
    EXPECT_EQ(result.status, atdecc::AEM_STATUS_SUCCESS);
    atdecc::aem::AemStreamInfoPayload info{};
    span_load(info, std::span<uint8_t const>{out}.first(atdecc::aem::AemStreamInfoPayload::LENGTH));
    EXPECT_TRUE((info.flags.get() & atdecc::aem::stream_info_flags::STREAM_FORMAT_VALID) != 0);
    ieee::octlet_t format{0};
    span_load(format, make_const_span(info.stream_format));
    return format.get();
}

/// The AAF input of the listener kit of a pair, with the pair's bind aimed at the AAF output.
auto aaf_input_of(KitPair& pair) -> StreamOf
{
    pair.sink = AAF_STREAM;
    pair.source = AAF_STREAM;
    return StreamOf{.kit = pair.listener, .index = AAF_STREAM};
}

}  // namespace

TEST(avb_entity_kit_format, a_stream_input_takes_each_base_channel_count_and_its_listener_follows)
{
    // Milan v1.3 6.5: an input that lists one Base format at a rate takes all
    // of them. The model says so with one "up to" entry (6.6).
    for (std::string_view const profile : {std::string_view{"milan13"}, std::string_view{"classic"}}) {
        KitPair pair{profile};
        auto const input = aaf_input_of(pair);
        EXPECT_EQ(reported_stream_format(input), AAF_96K_8CH);
        for (uint64_t const channels : {1U, 2U, 4U, 6U, 8U}) {
            EXPECT_EQ(set_stream_format(input, aaf_96k_of(channels)), atdecc::AEM_STATUS_SUCCESS);
            EXPECT_EQ(reported_stream_format(input), aaf_96k_of(channels));
            EXPECT_EQ(stream_info_format(input), aaf_96k_of(channels));  // GET_STREAM_INFO says the same
            EXPECT_EQ(pair.listener->listener()->stream_format(AAF_STREAM), aaf_96k_of(channels));
        }
        EXPECT_TRUE(pair.listener.stop(pair.now));
        EXPECT_TRUE(pair.talker.stop(pair.now));
    }
}

TEST(avb_entity_kit_format, a_format_the_input_does_not_list_is_refused_and_nothing_changes)
{
    KitPair pair{"milan13"};
    auto const input = aaf_input_of(pair);
    uint64_t const at_48k = (aaf_96k_of(2) & ~0x000F000000000000ULL) | 0x0005000000000000ULL;
    for (uint64_t const refused : {aaf_96k_of(9), at_48k, aaf_96k_of(8) | avtp::AAF_STREAM_FORMAT_UT_BIT}) {
        EXPECT_EQ(set_stream_format(input, refused), atdecc::AEM_STATUS_NOT_SUPPORTED);
    }
    EXPECT_EQ(reported_stream_format(input), AAF_96K_8CH);
    EXPECT_EQ(pair.listener->listener()->stream_format(AAF_STREAM), AAF_96K_8CH);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_format, a_bound_milan_input_answers_stream_is_running)
{
    // Milan v1.3 5.4.2.7: "The PAAD-AE shall not accept a SET_STREAM_FORMAT
    // command on a Stream Input if the Stream Input is bound ... In these
    // cases, the STREAM_IS_RUNNING error code shall be returned."
    KitPair pair{"milan13"};
    auto const input = aaf_input_of(pair);
    pair.bind();
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->sink(AAF_STREAM)->is_bound());
    EXPECT_EQ(set_stream_format(input, aaf_96k_of(2)), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(reported_stream_format(input), AAF_96K_8CH);
    EXPECT_EQ(pair.listener->listener()->stream_format(AAF_STREAM), AAF_96K_8CH);

    // Unbound, it takes the format.
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    EXPECT_FALSE(pair.listener->host().components().milan_acmp_listener->sink(AAF_STREAM)->is_bound());
    EXPECT_EQ(set_stream_format(input, aaf_96k_of(2)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(reported_stream_format(input), aaf_96k_of(2));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_format, a_connected_ieee_1722_1_input_answers_stream_is_running)
{
    // IEEE 1722.1-2021 7.4.9.2: "If the stream is currently streaming then
    // the ATDECC Entity responds with a STREAM_IS_RUNNING status."
    KitPair pair{"classic"};
    auto const input = aaf_input_of(pair);
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(pair.listener->host().components().acmp_listener.is_connected(AAF_STREAM));
    EXPECT_EQ(set_stream_format(input, aaf_96k_of(2)), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(reported_stream_format(input), AAF_96K_8CH);

    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    pair.run(ROUNDS_PER_SECOND);
    EXPECT_FALSE(pair.listener->host().components().acmp_listener.is_connected(AAF_STREAM));
    EXPECT_EQ(set_stream_format(input, aaf_96k_of(2)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_format, the_format_in_force_named_again_is_accepted_whatever_the_stream_is_doing)
{
    // Under IEEE 1722.1 the command reaches the kit; a stream output has the
    // one format, and naming it is no change.
    KitPair pair{"classic"};
    auto const input = aaf_input_of(pair);
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(set_stream_format(input, AAF_96K_8CH), atdecc::AEM_STATUS_SUCCESS);
    StreamOf const output{.kit = pair.talker, .index = AAF_STREAM, .type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT};
    EXPECT_EQ(set_stream_format(output, AAF_96K_8CH), atdecc::AEM_STATUS_SUCCESS);
    // The output lists no other format: its mappings hold in that one (5.3.3.4).
    EXPECT_EQ(set_stream_format(output, aaf_96k_of(2)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(reported_stream_format(output), AAF_96K_8CH);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_format, the_format_of_an_input_is_saved_and_restored_at_the_next_start)
{
    // Milan v1.3 5.3.8.1: "The current format shall be saved in a
    // non-volatile memory and restored after a power cycle."
    StateDir const dir{"statusbar_state_dir_format_functional_test"};
    auto const file = (dir.entity / entity_state_file::STREAM_FORMATS).string();
    {
        KitPair pair{"milan13", dir.listener_config("milan13")};
        auto const input = aaf_input_of(pair);
        EXPECT_FALSE(std::filesystem::exists(file));  // nothing set, nothing saved
        EXPECT_EQ(set_stream_format(input, aaf_96k_of(2)), atdecc::AEM_STATUS_SUCCESS);
        auto const saved = load_stream_formats(file);
        EXPECT_TRUE(saved.has_value());
        EXPECT_EQ(saved->size(), pair.listener->listener_specs().size());  // one line per input
        EXPECT_TRUE(
            saved->at(AAF_STREAM) ==
            (SavedStreamFormat{
                .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT, .descriptor_index = AAF_STREAM, .format = aaf_96k_of(2)}));
        EXPECT_TRUE(pair.listener.stop(pair.now));
        EXPECT_TRUE(pair.talker.stop(pair.now));
    }
    // Another process, the same entity: its input is on two channels from the start.
    KitPair again{"milan13", dir.listener_config("milan13")};
    auto const input = aaf_input_of(again);
    EXPECT_EQ(reported_stream_format(input), aaf_96k_of(2));
    EXPECT_EQ(stream_info_format(input), aaf_96k_of(2));
    EXPECT_EQ(again.listener->listener()->stream_format(AAF_STREAM), aaf_96k_of(2));
    EXPECT_TRUE(again.listener.stop(again.now));
    EXPECT_TRUE(again.talker.stop(again.now));
}

TEST(avb_entity_kit_format, a_saved_format_the_model_does_not_describe_is_left_and_the_input_keeps_its_own)
{
    // The model changed since the format was saved: 48 kHz is not this input's.
    StateDir const dir{"statusbar_state_dir_stale_format_functional_test"};
    std::filesystem::create_directories(dir.entity);
    auto const file = (dir.entity / entity_state_file::STREAM_FORMATS).string();
    uint64_t const at_48k = (aaf_96k_of(2) & ~0x000F000000000000ULL) | 0x0005000000000000ULL;
    std::array const stale{
        SavedStreamFormat{
            .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT, .descriptor_index = AAF_STREAM, .format = at_48k},
        SavedStreamFormat{
            .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, .descriptor_index = AAF_STREAM, .format = aaf_96k_of(2)}};
    EXPECT_TRUE(save_stream_formats(file, stale).has_value());

    KitPair pair{"milan13", dir.listener_config("milan13")};
    auto const input = aaf_input_of(pair);
    EXPECT_EQ(reported_stream_format(input), AAF_96K_8CH);
    EXPECT_EQ(pair.listener->listener()->stream_format(AAF_STREAM), AAF_96K_8CH);
    StreamOf const output{.kit = pair.listener, .index = AAF_STREAM, .type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT};
    EXPECT_EQ(reported_stream_format(output), AAF_96K_8CH);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_format, an_ieee_1722_1_kit_makes_its_state_directory_when_a_format_is_first_set)
{
    // Static addresses and no bindings file: a classic kit has nothing to
    // save until a controller sets a format.
    StateDir const dir{"statusbar_state_dir_classic_format_functional_test"};
    KitPair pair{"classic", dir.listener_config("classic")};
    auto const input = aaf_input_of(pair);
    EXPECT_FALSE(std::filesystem::exists(dir.entity));
    EXPECT_EQ(set_stream_format(input, aaf_96k_of(4)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(std::filesystem::exists(dir.entity / entity_state_file::STREAM_FORMATS));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_format, with_no_state_directory_the_format_is_set_and_nothing_is_saved)
{
    StateDir const dir{"statusbar_state_dir_absent_format_functional_test"};
    std::filesystem::remove_all(dir.state);
    KitPair pair{"classic", dir.listener_config("classic")};
    auto const input = aaf_input_of(pair);
    EXPECT_EQ(set_stream_format(input, aaf_96k_of(4)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(reported_stream_format(input), aaf_96k_of(4));
    EXPECT_FALSE(std::filesystem::exists(dir.state));
    // A stop and a start of the same kit keep it: the descriptor and the listener hold it.
    EXPECT_TRUE(pair.listener.stop(pair.now));
    bring_up(pair.listener);
    EXPECT_EQ(reported_stream_format(input), aaf_96k_of(4));
    EXPECT_EQ(pair.listener->listener()->stream_format(AAF_STREAM), aaf_96k_of(4));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

//
// The kit's Milan inputs on the NIC: VLANs and multicast groups - Milan v1.3 Section 4.4.1
//

namespace {

/// A talker the pair has no kit for: the test answers the sink's probe for it.
constexpr Eui64 ANSWERED_TALKER_ID{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0C};

/// Bind @p pair's input to stream 0 of @p talker and answer the sink's probe with @p stream_id
/// to @p group on VLAN @p vid: the input settles on those.
void bind_answered(
    KitPair& pair, uint64_t const stream_id, Eui48 const& group, uint16_t const vid, Eui64 const& talker = ANSWERED_TALKER_ID)
{
    pair.controller_sends(controller_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, talker, 0, LISTENER_ID, pair.sink));
    auto answer = pair.last(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(answer.has_value() && answer->talker_entity_id == talker);
    if (answer.has_value()) {
        answer->set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
        answer->stream_id = Eui64{}.from_uint64(stream_id);
        answer->stream_dest_mac = group;
        answer->stream_vlan_id = vid;
        receive_acmp(pair.listener, *answer, pair.now);
    }
}

/// Start @p pair's stopped listener kit again at the pair's time and take it to Ready, its sends hooked again.
void restart_listener(KitPair& pair)
{
    EXPECT_TRUE(pair.listener.start(pair.now));
    pair.listener->on_link_up(pair.now);
    pair.listener->on_gptp_announce(pair.now, true);
    EXPECT_TRUE(pair.listener->host().is_ready());
    pair.hook(pair.listener, pair.acmp_to_talker, pair.msrp_to_talker);
}

/// @p pair's listener kit stopped and started again on a test port, every join and leave in @p log; that port.
auto listener_on_test_port(KitPair& pair, std::vector<net::TestPortContext::MembershipChange>* const log = nullptr)
    -> net::TestPortContext&
{
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.listener->stream_port().open_test({.hardware_address = Eui48{0x02, 0, 0, 0, 0, 0x0A}}).has_value());
    auto& port = *pair.listener->stream_port().test_port();
    port.record_memberships(log);
    restart_listener(pair);
    return port;
}

/// A bridge port declaring @p declare to @p pair's listener kit over MSRP, until it is registered.
template <typename Declare>
void bridge_declares(KitPair& pair, Declare&& declare)
{
    std::vector<std::vector<uint8_t>> out;
    srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9e};
    bridge.set_send_pdu([&out](std::span<uint8_t const> const pdu) {
        out.emplace_back(pdu.begin(), pdu.end());
        return true;
    });
    bridge.start(pair.now);
    std::forward<Declare>(declare)(bridge);
    for (int i = 0; i < 4; ++i) {
        pair.now += std::chrono::milliseconds{250};
        bridge.tick(pair.now);
        for (auto const& pdu : std::exchange(out, {})) {
            pair.listener->host().components().msrp_handler.receive_packet(pdu, pair.now);
        }
    }
}

#if defined(__linux__)
/// The NIC's VLAN links as the kit asks for them: "add <link>" and "del <link>", in order.
class VlanRecorder final : public net::vlan::VlanControl
{
  public:
    std::vector<std::string> requests;

    auto add_vlan(int /*parent_ifindex*/, std::string_view const name, uint16_t /*vid*/)
        -> StatusValue<net::vlan::LinkOwnership> override
    {
        requests.push_back("add " + std::string{name});
        return net::vlan::LinkOwnership::created;
    }
    auto remove_link(std::string_view const name) -> Status override
    {
        requests.push_back("del " + std::string{name});
        return success();
    }
    auto bring_up(std::string_view /*name*/) -> Status override { return success(); }
    [[nodiscard]] auto last_message() const noexcept -> std::string const& override { return message_; }

  private:
    std::string message_{};
};

/// @p pair's listener kit stopped and started again with its VLAN filter on @p vlans.
void listener_on_vlans(KitPair& pair, VlanRecorder& vlans)
{
    EXPECT_TRUE(pair.listener.stop(pair.now));
    pair.listener->bind_listener_vlan(vlans, 9);
    restart_listener(pair);
}
#endif

}  // namespace

TEST(avb_entity_kit_input_vlan, an_input_settled_on_another_vid_joins_it_with_mvrp_and_leaves_it_at_its_unbind)
{
    KitPair pair{"milan13"};
    auto const& mvrp = pair.listener->host().components().mvrp_handler;
    bind_answered(pair, 0x0200000000000777ULL, Eui48{0x91, 0xE0, 0xF0, 0x00, 0x07, 0x77}, 5);
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->sink(pair.sink)->is_settled());
    EXPECT_TRUE(mvrp.get_vlan_state(5) != nanoavb::VlanState::Unregistered);
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, ANSWERED_TALKER_ID, 0, LISTENER_ID, pair.sink));
    EXPECT_TRUE(mvrp.get_vlan_state(5) == nanoavb::VlanState::Unregistered);
    EXPECT_TRUE(mvrp.get_vlan_state(2) != nanoavb::VlanState::Unregistered);  // the Class A VID stays
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_vlan, a_rebound_input_leaves_its_group_before_it_joins_the_next)
{
    KitPair pair{"milan13"};
    auto const& port = listener_on_test_port(pair);
    Eui48 const first{0x91, 0xE0, 0xF0, 0x00, 0x07, 0x01};
    Eui48 const second{0x91, 0xE0, 0xF0, 0x00, 0x07, 0x02};
    auto const joins = [&port](Eui48 const& group) { return std::ranges::count(port.joined_multicast_groups(), group); };
    bind_answered(pair, 0x0200000000000777ULL, first, 2);
    EXPECT_EQ(joins(first), 1);
    bind_answered(pair, 0x0200000000000042ULL, second, 2, Eui64{}.from_uint64(0x0200000000000042ULL));
    EXPECT_EQ(joins(first), 0);
    EXPECT_EQ(joins(second), 1);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_vlan, a_stop_leaves_the_connected_group_and_closes_the_port)
{
    std::vector<net::TestPortContext::MembershipChange> log;  // outlives the port
    KitPair pair{"milan13"};
    (void)listener_on_test_port(pair, &log);
    Eui48 const group{0x91, 0xE0, 0xF0, 0x00, 0x07, 0x03};
    auto const held = [&log](Eui48 const& g) {
        return std::ranges::count(log, net::TestPortContext::MembershipChange{.group = g, .joined = true}) -
            std::ranges::count(log, net::TestPortContext::MembershipChange{.group = g, .joined = false});
    };
    bind_answered(pair, 0x0200000000000777ULL, group, 2);
    EXPECT_EQ(held(group), 1);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_FALSE(pair.listener->stream_port().valid());
    EXPECT_EQ(held(group), 0);
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_vlan, a_restarted_input_joins_its_group_again_once_its_saved_binding_settles)
{
    // stop() leaves the group with the connection, not the binding: after a restart the input is
    // unconnected until the binding settles again, then reserves and joins anew (Milan v1.3 Section 5.3.8.3).
    KitPair pair{"milan13"};
    pair.bind();
    pair.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Ready);
    auto const group = pair.listener->host().components().milan_acmp_listener->sink(pair.sink)->context().srp.stream_dest_mac;

    auto const& port = listener_on_test_port(pair);
    auto const joins = [&port, &group] { return std::ranges::count(port.joined_multicast_groups(), group); };
    auto const static_joins = joins();  // the model's pre-joined group of the input may be the same
    auto const& sink = *pair.listener->host().components().milan_acmp_listener->sink(pair.sink);
    EXPECT_TRUE(sink.is_bound());
    EXPECT_FALSE(sink.is_settled());
    talker_advertises(pair, 1);
    pair.run(3 * ROUNDS_PER_SECOND);  // TMR_DELAY, the probe, the reservation
    EXPECT_EQ(sink.state(), atdecc::MilanSinkState::SettledRsvOk);
    EXPECT_EQ(pair.listener_srp_state(), nanoavb::ListenerReservationState::Ready);
    EXPECT_EQ(joins(), static_joins + 1);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

#if defined(__linux__)
TEST(avb_entity_kit_input_vlan, an_input_settled_on_another_vlan_holds_its_vid_on_the_filter)
{
    // VID 3 beside the Class A VID 2. When an adopted Domain moves Class A onto 3, the link both
    // use stays and 2's goes; the input's unbind leaves 3 to the Class A VID.
    VlanRecorder vlans;  // first: it outlives the kit, which deletes its links through it
    KitPair pair{"milan13"};
    listener_on_vlans(pair, vlans);
    auto const link = [](uint16_t const vid) { return net::vlan::vlan_link_name("no-such-interface-0", vid); };
    bind_answered(pair, 0x0200000000000777ULL, Eui48{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x10}, 3);
    EXPECT_TRUE(vlans.requests == (std::vector<std::string>{"add " + link(2), "add " + link(3)}));

    bridge_declares(pair, [&pair](srp::msrp::MsrpParticipant& bridge) {
        srp::msrp::DomainFirstValue class_a{};
        class_a.sr_class_vid = 3;
        (void)bridge.declare_domain(class_a, pair.now);
    });
    EXPECT_EQ(pair.listener->host().components().msrp_handler.domain().sr_class_vid, 3);
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, ANSWERED_TALKER_ID, 0, LISTENER_ID, pair.sink));
    EXPECT_TRUE(vlans.requests == (std::vector<std::string>{"add " + link(2), "add " + link(3), "del " + link(2)}));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_vlan, an_input_probed_without_a_vlan_joins_its_talker_advertises)
{
    // A probe answer naming VLAN 0 matches the Talker Advertise on any VLAN; the input joins the
    // VLAN that Advertise is on (3) once it is registered, and leaves it at its unbind.
    VlanRecorder vlans;
    KitPair pair{"milan13"};
    listener_on_vlans(pair, vlans);
    auto const link = [](uint16_t const vid) { return net::vlan::vlan_link_name("no-such-interface-0", vid); };
    Eui48 const group{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x11};
    uint64_t const stream = 0x0200000000000888ULL;
    bind_answered(pair, stream, group, 0);
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->sink(pair.sink)->is_settled());
    EXPECT_TRUE(vlans.requests == (std::vector<std::string>{"add " + link(2)}));  // no VID yet

    bridge_declares(pair, [&](srp::msrp::MsrpParticipant& bridge) {
        srp::msrp::TalkerAdvertiseFirstValue fv{};
        (void)tsn::load_unchecked(Eui64{}.from_uint64(stream).span(), &fv.stream_id);
        fv.destination_address = group;
        fv.vlan_identifier = 3;
        fv.max_frame_size = 416;
        fv.max_interval_frames = 1;
        (void)bridge.declare_talker_advertise(fv, pair.now);
    });
    EXPECT_TRUE(vlans.requests == (std::vector<std::string>{"add " + link(2), "add " + link(3)}));
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, ANSWERED_TALKER_ID, 0, LISTENER_ID, pair.sink));
    EXPECT_TRUE(vlans.requests == (std::vector<std::string>{"add " + link(2), "add " + link(3), "del " + link(3)}));
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}
#endif

//
// A Milan input's counters - Milan v1.3 Section 5.3.8.10, Table 5.16
//

namespace {

// examples/dual.json: STREAM_INPUT 2 and STREAM_OUTPUT 2 are the CRF pair, 48 kHz, 96 events a timestamp.
constexpr uint16_t CRF_STREAM = 2;

/// GET_COUNTERS of STREAM_INPUT @p index of @p kit.
auto input_counters(KitUnderTest& kit, uint16_t const index) -> atdecc::aem::AemCountersPayload
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_GET_COUNTERS, atdecc::AemDu::AEM_DATA_LENGTH + 4);
    header.controller_entity_id = CONTROLLER_ID;
    std::array<uint8_t, 4> const body{0x00, 0x05, 0x00, static_cast<uint8_t>(index)};
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    EXPECT_EQ(kit->host().components().aem_handler.handle_command(header, body, out).status, atdecc::AEM_STATUS_SUCCESS);
    atdecc::aem::AemCountersPayload counters{};
    span_load(counters, std::span<uint8_t const>{out}.first(sizeof(atdecc::aem::AemCountersPayload)));
    return counters;
}

// Table 5.16's positions in the counters block.
constexpr size_t MEDIA_LOCKED = 0;
constexpr size_t MEDIA_UNLOCKED = 1;
constexpr size_t STREAM_INTERRUPTED = 2;
constexpr size_t SEQ_NUM_MISMATCH = 3;
constexpr size_t TIMESTAMP_UNCERTAIN = 5;
constexpr size_t UNSUPPORTED_FORMAT = 8;
constexpr size_t FRAMES_RX = 11;

/// A KitPair with the listener's CRF input bound to the talker's CRF output, settled and reserved.
struct CrfPair
{
    KitPair pair{"milan13"};
    ListenerStreams* inputs{nullptr};
    avtp::CrfStreamOutputContext tx;
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> frame{};
    uint64_t ts{9'000'000'000ULL};
    int64_t poll_ns{1'000'000'000};

    CrfPair()
        : tx{crf_context(96)}
    {
        pair.source = CRF_STREAM;
        pair.sink = CRF_STREAM;
        pair.bind();
        pair.run(2 * ROUNDS_PER_SECOND);
        inputs = pair.listener->listener();
        EXPECT_TRUE(inputs != nullptr && inputs->slot_for(CRF_STREAM) != nullptr);
        pair.listener->poll_counters(poll_ns);  // the first interval starts
    }
    CrfPair(CrfPair const&) = delete;
    auto operator=(CrfPair const&) -> CrfPair& = delete;
    ~CrfPair() = default;

    /// Both kits stopped.
    void stop()
    {
        EXPECT_TRUE(pair.listener.stop(pair.now));
        EXPECT_TRUE(pair.talker.stop(pair.now));
    }

    /// The talker's CRF stream as a sender at @p interval events a timestamp.
    [[nodiscard]] auto crf_context(uint16_t const interval) -> avtp::CrfStreamOutputContext
    {
        StreamSpec const* spec = nullptr;
        for (auto const& s : pair.talker.talker_specs()) {
            spec = s.index == CRF_STREAM ? &s : spec;
        }
        auto const sid = spec != nullptr ? pair.talker->make_talker_srp_info(*spec).stream_id : nanoavb::StreamId{};
        return {sid, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, interval, 1};
    }

    /// The next PDU of @p ctx, its header changed by @p edit, @p length octets of it received.
    template <typename Edit>
    void send(avtp::CrfStreamOutputContext& ctx, Edit&& edit, size_t const length)
    {
        std::array<uint64_t, 1> const one{ts};
        EXPECT_EQ(serialize_crf_raw(ctx, frame, one), frame.size());
        avtp::CrfPdu pdu{};
        span_load(pdu, std::span<uint8_t const>{frame}.first(avtp::CrfPdu::HEADER_LENGTH));
        std::forward<Edit>(edit)(pdu);
        span_store(std::span{frame}.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
        inputs->on_stream_rx_frame(std::span{frame}.first(length), static_cast<int64_t>(ts) - 2'000'000);
        ts += 2'000'000;  // 96 events at 48 kHz
    }
    void send()
    {
        send(tx, [](avtp::CrfPdu&) {}, frame.size());
    }

    /// The counters a second on, as GET_COUNTERS answers them.
    auto counters_after_a_second() -> atdecc::aem::AemCountersPayload
    {
        pair.listener->poll_counters(poll_ns += AvbEntityKit::COUNTERS_INTERVAL_NS);
        return input_counters(pair.listener, CRF_STREAM);
    }
};

}  // namespace

TEST(avb_entity_kit_input_counters, a_bind_zeroes_an_inputs_counters_and_an_unbind_keeps_them)
{
    // Milan v1.3 Section 5.3.8.10: zeroed when the input becomes bound, not when it is unbound.
    KitPair pair{"milan13"};
    auto* const slot = pair.listener->listener()->slot_for(pair.sink);
    EXPECT_TRUE(slot != nullptr);
    int64_t t = 1'000'000'000;
    pair.listener->poll_counters(t);
    slot->counters.frames_rx.store(10);
    slot->counters.seq_num_mismatch.store(3);
    pair.listener->poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    auto counters = input_counters(pair.listener, pair.sink);
    EXPECT_EQ(counters.counters_valid.get(), StreamInputIntervals::VALID);
    EXPECT_EQ(counters.counters[FRAMES_RX].get(), 1U);  // one interval
    EXPECT_EQ(counters.counters[SEQ_NUM_MISMATCH].get(), 1U);

    pair.bind();
    counters = input_counters(pair.listener, pair.sink);
    EXPECT_EQ(counters.counters[FRAMES_RX].get(), 0U);
    EXPECT_EQ(counters.counters[SEQ_NUM_MISMATCH].get(), 0U);
    slot->counters.frames_rx.store(20);
    pair.listener->poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS - 1);  // not yet a second
    EXPECT_EQ(input_counters(pair.listener, pair.sink).counters[FRAMES_RX].get(), 0U);
    pair.listener->poll_counters(t += 1);
    EXPECT_EQ(input_counters(pair.listener, pair.sink).counters[FRAMES_RX].get(), 1U);

    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    EXPECT_EQ(input_counters(pair.listener, pair.sink).counters[FRAMES_RX].get(), 1U);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_counters, an_unbind_of_a_restored_binding_is_no_interruption)
{
    // A binding restored at start is bound: its unbind counts MEDIA_UNLOCKED, not
    // STREAM_INTERRUPTED (Milan v1.2 Table 5.6), and a stopped input's discarded stream neither.
    StateDir const dir{"statusbar_state_dir_restored_counters_functional_test"};
    {
        KitPair first{"milan13", dir.listener_config("milan13")};
        first.bind();
        EXPECT_TRUE(first.listener.stop(first.now));
        EXPECT_TRUE(first.talker.stop(first.now));
    }
    KitPair pair{"milan13", dir.listener_config("milan13")};
    EXPECT_TRUE(pair.listener->host().components().milan_acmp_listener->sink(pair.sink)->is_bound());
    auto& c = pair.listener->listener()->slot_for(pair.sink)->counters;
    int64_t t = 1'000'000'000;
    pair.listener->poll_counters(t);
    c.media_locked.store(1);  // as if its stream had locked
    c.is_locked.store(true);
    c.frames_rx.store(100);
    pair.listener->poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    pair.controller_sends(
        controller_command(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink));
    pair.listener->poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    auto counters = input_counters(pair.listener, pair.sink);
    EXPECT_EQ(counters.counters[MEDIA_LOCKED].get(), 1U);
    EXPECT_EQ(counters.counters[MEDIA_UNLOCKED].get(), 1U);
    EXPECT_EQ(counters.counters[STREAM_INTERRUPTED].get(), 0U);

    auto bind = controller_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, TALKER_ID, pair.source, LISTENER_ID, pair.sink);
    bind.flags = atdecc::acmp_flags::STREAMING_WAIT;
    pair.controller_sends(bind);
    c.media_locked.fetch_add(1);
    c.is_locked.store(true);
    pair.listener->poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    counters = input_counters(pair.listener, pair.sink);
    EXPECT_EQ(counters.counters[STREAM_INTERRUPTED].get(), 0U);
    EXPECT_EQ(counters.counters[MEDIA_LOCKED].get(), counters.counters[MEDIA_UNLOCKED].get());
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_counters, what_an_offload_received_reaches_get_counters)
{
    // An offload data plane's reception of an input counts as the kit's own: Table 5.16's
    // interval counters, MEDIA_LOCKED from its check of the lock.
    KitPair pair{"milan13"};
    int64_t t = 1'000'000'000;
    pair.listener->poll_counters(t);
    EXPECT_TRUE(pair.listener->listener()->notify_offload_received(
        pair.sink,
        {.frames_rx = 8, .seq_num_mismatch = 1, .timestamp_uncertain = 2, .timestamp_valid = 8, .media_locked = true},
        48));
    pair.listener->poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    auto const counters = input_counters(pair.listener, pair.sink);
    EXPECT_EQ(counters.counters[MEDIA_LOCKED].get(), 1U);
    EXPECT_EQ(counters.counters[SEQ_NUM_MISMATCH].get(), 1U);  // one interval
    EXPECT_EQ(counters.counters[TIMESTAMP_UNCERTAIN].get(), 1U);
    EXPECT_EQ(counters.counters[FRAMES_RX].get(), 1U);
    EXPECT_TRUE(pair.listener.stop(pair.now));
    EXPECT_TRUE(pair.talker.stop(pair.now));
}

TEST(avb_entity_kit_input_counters, a_crf_input_counts_media_lock_from_its_timestamps)
{
    // A CRF input locks on a steady timestamp step (IEEE 1722-2016 Clause 10.4) and unlocks on a
    // lost PDU; a PDU of another format is discarded and counted in UNSUPPORTED_FORMAT.
    CrfPair crf;
    for (int k = 0; k < 20; ++k) {
        crf.send();
    }
    auto counters = crf.counters_after_a_second();
    EXPECT_EQ(counters.counters[MEDIA_LOCKED].get(), 1U);
    EXPECT_EQ(counters.counters[MEDIA_UNLOCKED].get(), 0U);
    EXPECT_EQ(counters.counters[FRAMES_RX].get(), 1U);  // one interval, however many PDUs
    EXPECT_EQ(counters.counters[UNSUPPORTED_FORMAT].get(), 0U);

    crf.ts += 2'000'000;  // one PDU lost
    crf.send();
    auto other = crf.crf_context(160);
    crf.send(other, [](avtp::CrfPdu&) {}, crf.frame.size());
    counters = crf.counters_after_a_second();
    EXPECT_EQ(counters.counters[MEDIA_UNLOCKED].get(), 1U);
    EXPECT_EQ(counters.counters[STREAM_INTERRUPTED].get(), 1U);
    EXPECT_EQ(counters.counters[UNSUPPORTED_FORMAT].get(), 1U);
    crf.stop();
}

TEST(avb_entity_kit_input_counters, a_rejected_crf_pdu_is_unsupported_and_keeps_its_place_in_the_sequence)
{
    // Milan v1.3 Section 5.3.8.10: a CRF PDU the input rejects is still a Stream Data AVTPDU of the
    // stream; one with sv=0 is none, and is not counted.
    CrfPair crf;
    auto const typed = [](uint8_t const type, bool const sv) {
        return [type, sv](avtp::CrfPdu& pdu) {
            pdu.set_type(type);
            pdu.set_sv(sv);
        };
    };
    crf.send(crf.tx, typed(1, true), crf.frame.size());
    crf.send(crf.tx, typed(7, true), crf.frame.size());  // a CRF type IEEE 1722-2016 does not define
    crf.send(crf.tx, typed(1, true), crf.frame.size());
    auto counters = crf.counters_after_a_second();
    EXPECT_EQ(counters.counters[UNSUPPORTED_FORMAT].get(), 1U);
    EXPECT_EQ(counters.counters[SEQ_NUM_MISMATCH].get(), 0U);
    crf.send(crf.tx, typed(1, false), crf.frame.size());
    EXPECT_EQ(crf.counters_after_a_second().counters[UNSUPPORTED_FORMAT].get(), 1U);  // the interval before alone
    crf.stop();
}

TEST(avb_entity_kit_input_counters, a_short_crf_pdu_is_unsupported_and_keeps_its_place_in_the_sequence)
{
    CrfPair crf;
    crf.send();
    crf.send(crf.tx, [](avtp::CrfPdu&) {}, avtp::AVTP_COMMON_HEADER_LENGTH + 4);  // the common header, then cut short
    crf.send();
    auto const counters = crf.counters_after_a_second();
    EXPECT_EQ(counters.counters[UNSUPPORTED_FORMAT].get(), 1U);
    EXPECT_EQ(counters.counters[SEQ_NUM_MISMATCH].get(), 0U);
    crf.stop();
}

TEST(avb_entity_kit_input_counters, a_frame_of_another_subtype_on_an_inputs_stream_is_an_unsupported_format)
{
    // A frame of another AVTP subtype carrying the input's stream_id is a Stream Data AVTPDU of that
    // stream in a format it does not accept (Milan v1.3 Section 5.3.8.10).
    CrfPair crf;
    auto const subtype = [](uint8_t const value) { return [value](avtp::CrfPdu& pdu) { pdu.subtype = value; }; };
    crf.send(crf.tx, subtype(avtp::AvtpSubtype::crf), crf.frame.size());
    crf.send(crf.tx, subtype(avtp::AvtpSubtype::aaf), crf.frame.size());
    crf.send(crf.tx, subtype(avtp::AvtpSubtype::crf), crf.frame.size());
    auto const counters = crf.counters_after_a_second();
    EXPECT_EQ(counters.counters[UNSUPPORTED_FORMAT].get(), 1U);
    EXPECT_EQ(counters.counters[SEQ_NUM_MISMATCH].get(), 0U);
    crf.stop();
}

TEST_MAIN(statusbar_avb_entity, avb_entity_kit_functional_test)
