// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_srp_port.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/srp/srp_msrp_participant.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace statusbar::nanoavb;
using statusbar::ieee::Eui48;
using statusbar::srp::msrp::ListenerDeclaration;
using TimePoint = statusbar::sm::TimePoint;

namespace {

using Pdus = std::vector<std::vector<uint8_t>>;

auto collect(Pdus& out) -> SrpPort::SendFn
{
    return [&out](std::span<uint8_t const> p) {
        out.emplace_back(p.begin(), p.end());
        return true;
    };
}

auto discard() -> SrpPort::SendFn
{
    return [](std::span<uint8_t const>) { return true; };
}

auto stream_id(uint16_t const uid) -> StreamId
{
    StreamId sid{};
    sid.set_system_address(Eui48{0x00, 0x1b, 0x21, 0x01, 0x02, 0x03});
    sid.set_unique_id(uid);
    return sid;
}

Eui48 const GROUP{0x01, 0x00, 0x5e, 0x0a, 0x0b, 0x0c};

auto talker(uint16_t const uid) -> SrpTalker
{
    return SrpTalker{.stream_id = stream_id(uid), .destination = GROUP, .max_frame_size = 185, .max_interval_frames = 1};
}

/// Two SrpPorts on one link: the talker's MSRPDUs reach the listener and back.
struct Link
{
    TimePoint now{std::chrono::seconds{1}};
    Pdus t_out{};
    Pdus l_out{};
    SrpPort t{SrpPortConfig{}};
    SrpPort l{SrpPortConfig{}};

    Link()
    {
        t.set_senders(collect(t_out), discard());
        l.set_senders(collect(l_out), discard());
        (void)t.declare_domain(now);
        (void)l.declare_domain(now);
    }

    void run(int const rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{250};
            t.tick(now);
            l.tick(now);
            for (auto const& p : std::exchange(t_out, {})) {
                l.receive_msrp(p, now);
            }
            for (auto const& p : std::exchange(l_out, {})) {
                t.receive_msrp(p, now);
            }
        }
    }
};

/// A bare MSRP participant standing in for a bridge port, wired to one SrpPort.
struct Bridge
{
    TimePoint now{std::chrono::seconds{1}};
    Pdus port_out{};
    Pdus bridge_out{};
    SrpPort port{SrpPortConfig{}};
    statusbar::srp::msrp::MsrpParticipant bridge{
        statusbar::srp::msrp::MsrpConfig{.max_talker_advertise = 64, .max_talker_failed = 32, .max_listeners = 64}, 0x5eed};

    Bridge()
    {
        port.set_senders(collect(port_out), discard());
        bridge.set_send_pdu([this](std::span<uint8_t const> p) {
            bridge_out.emplace_back(p.begin(), p.end());
            return true;
        });
        bridge.start(now);
    }

    void run(int const rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{250};
            port.tick(now);
            bridge.tick(now);
            for (auto const& p : std::exchange(port_out, {})) {
                bridge.receive_pdu(p, now);
            }
            for (auto const& p : std::exchange(bridge_out, {})) {
                port.receive_msrp(p, now);
            }
        }
    }

    static auto domain(uint16_t const vid, uint8_t const priority = 3) -> statusbar::srp::msrp::DomainFirstValue
    {
        statusbar::srp::msrp::DomainFirstValue fv{};
        fv.sr_class_id = statusbar::srp::msrp::SR_CLASS_A;
        fv.sr_class_priority = priority;
        fv.sr_class_vid = vid;
        return fv;
    }
};

}  // namespace

// IEEE 802.1Q-2022 Clause 35.2.2.9: each port declares the Class A Domain, so each sees the
// other's registered and reports it once.
TEST(nanoavb_srp_port, the_peers_domain_registers)
{
    Link link{};
    int t_reports = 0;
    bool t_registered = false;
    link.t.set_observer(SrpPortObserver{.on_domain = [&](DomainInfo const& d, bool const registered) {
        ++t_reports;
        t_registered = registered;
        EXPECT_EQ(d.sr_class_vid, 2);
    }});
    EXPECT_FALSE(link.t.peer_domain_registered());
    link.run(8);
    EXPECT_TRUE(link.t.peer_domain_registered());
    EXPECT_TRUE(link.l.peer_domain_registered());
    EXPECT_EQ(t_reports, 1);
    EXPECT_TRUE(t_registered);
}

// The listener matches the talker's advertise by destination and VID (Clause 35.2.2.8.3), declares
// Ready for its StreamID, and the talker sees the declaration; the talker leaving withdraws it.
TEST(nanoavb_srp_port, a_listener_found_by_destination_reaches_the_talker)
{
    Link link{};
    std::vector<StreamId> advertised{};
    std::vector<ListenerReservationState> states{};
    int leaves = 0;
    link.l.set_observer(SrpPortObserver{
        .on_talker_advertise = [&](auto const& fv) { advertised.push_back(fv.stream_id); },
        .on_talker_leave = [&](StreamId const&) { ++leaves; },
        .on_listener_state = [&](StreamId const&, ListenerReservationState const s) { states.push_back(s); },
    });
    std::vector<ListenerDeclaration> heard{};
    link.t.set_observer(SrpPortObserver{.on_listener = [&](StreamId const&, ListenerDeclaration const d) { heard.push_back(d); }});

    EXPECT_TRUE(link.t.declare_talker(talker(7), link.now).has_value());
    link.run(4);
    EXPECT_EQ(advertised.size(), size_t{1});
    auto const* fv = link.l.find_registered_talker_advertise(GROUP, link.l.domain().sr_class_vid);
    EXPECT_NE(fv, nullptr);
    if (fv == nullptr) {
        return;
    }
    EXPECT_TRUE(fv->stream_id == stream_id(7));
    EXPECT_EQ(fv->max_frame_size.get(), 185);
    EXPECT_EQ(fv->get_priority(), 3);

    auto const attached = link.l.attach_listener(fv->stream_id, link.now);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::Ready);
    link.run(4);
    EXPECT_TRUE(link.t.registered_listener(stream_id(7)) == ListenerDeclaration::Ready);
    EXPECT_FALSE(heard.empty());
    EXPECT_TRUE(!heard.empty() && heard.back() == ListenerDeclaration::Ready);

    EXPECT_TRUE(link.t.withdraw_talker(stream_id(7), link.now).has_value());
    link.run(12);
    EXPECT_EQ(leaves, 1);
    EXPECT_TRUE(!states.empty() && states.back() == ListenerReservationState::Waiting);
    EXPECT_EQ(link.l.find_registered_talker_advertise(GROUP, 2), nullptr);
    EXPECT_TRUE(link.t.registered_listener(stream_id(7)) == ListenerDeclaration::Ignore);
    EXPECT_TRUE(!heard.empty() && heard.back() == ListenerDeclaration::Ignore);
}

// A Talker Failed is reported with its failure code and found by destination; a listener attached
// to it declares Asking Failed (Clause 35.2.4.4).
TEST(nanoavb_srp_port, a_talker_failed_is_reported_and_asked_for)
{
    Bridge b{};
    std::vector<statusbar::srp::msrp::FailureCode> codes{};
    b.port.set_observer(SrpPortObserver{.on_talker_failed = [&](auto const& fv) { codes.push_back(fv.get_failure_code()); }});
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise.stream_id = stream_id(9);
    failed.advertise.destination_address = GROUP;
    failed.advertise.vlan_identifier = 2;
    failed.set_failure_code(statusbar::srp::msrp::FailureCode::InsufficientBandwidth);
    (void)b.bridge.declare_talker_failed(failed, b.now);
    b.run(4);
    EXPECT_EQ(codes.size(), size_t{1});
    EXPECT_EQ(b.port.find_registered_talker_advertise(GROUP, 2), nullptr);
    EXPECT_NE(b.port.find_registered_talker_failed(GROUP, 2), nullptr);
    auto const attached = b.port.attach_listener(stream_id(9), b.now);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::AskingFailed);
}

// Milan v1.2 Section 4.2.7.2.1: the bridge's Class A VID is adopted; the port moves its MVRP join
// and re-advertises its talker on it, and declare_domain() (link up) returns to the configured VID.
TEST(nanoavb_srp_port, an_adopted_vid_moves_the_vlan_and_the_talkers)
{
    Bridge b{};
    std::vector<std::pair<uint16_t, bool>> reports{};
    b.port.set_observer(
        SrpPortObserver{.on_domain = [&](DomainInfo const& d, bool const r) { reports.emplace_back(d.sr_class_vid, r); }});
    EXPECT_TRUE(b.port.declare_domain(b.now).has_value());
    EXPECT_TRUE(b.port.declare_talker(talker(3), b.now).has_value());
    EXPECT_TRUE(b.port.mvrp().get_vlan_state(2) != VlanState::Unregistered);
    (void)b.bridge.declare_domain(Bridge::domain(7), b.now);
    b.run(8);
    EXPECT_EQ(b.port.domain().sr_class_vid, 7);
    EXPECT_TRUE(b.port.peer_domain_registered());
    EXPECT_TRUE(b.port.mvrp().get_vlan_state(7) != VlanState::Unregistered);
    EXPECT_TRUE(b.port.mvrp().get_vlan_state(2) == VlanState::Unregistered);
    auto const* fv = b.bridge.find_registered_talker_advertise(stream_id(3));
    EXPECT_TRUE(fv != nullptr && fv->vlan_identifier.get() == 7);
    using Reports = std::vector<std::pair<uint16_t, bool>>;
    EXPECT_TRUE(reports == Reports({{7, true}}));  // one report, the final state

    EXPECT_TRUE(b.port.declare_domain(b.now).has_value());
    EXPECT_EQ(b.port.domain().sr_class_vid, 2);
    EXPECT_TRUE(reports == Reports({{7, true}, {2, true}}));
    EXPECT_TRUE(b.port.mvrp().get_vlan_state(2) != VlanState::Unregistered);
    EXPECT_TRUE(b.port.msrp().get_talker_stream(stream_id(3))->vlan_id == 2);
}

// IEEE 802.1Q-2022 Clause 35.2.1.4: a peer's Class A Domain at another SRclassPriority leaves the
// port an SRP domain boundary; it is neither adopted nor reported as registered.
TEST(nanoavb_srp_port, a_peer_domain_at_another_priority_is_a_boundary)
{
    Bridge b{};
    std::vector<std::pair<uint16_t, bool>> reports{};
    b.port.set_observer(
        SrpPortObserver{.on_domain = [&](DomainInfo const& d, bool const r) { reports.emplace_back(d.sr_class_vid, r); }});
    EXPECT_TRUE(b.port.declare_domain(b.now).has_value());
    (void)b.bridge.declare_domain(Bridge::domain(7, 2), b.now);
    b.run(8);
    EXPECT_FALSE(b.port.peer_domain_registered());
    EXPECT_EQ(b.port.domain().sr_class_vid, 2);
    EXPECT_TRUE(reports.empty());
}

// A talker whose Advertise turns into a Talker Failed has not left: on_talker_leave comes once,
// when neither attribute is registered.
TEST(nanoavb_srp_port, a_talker_leaves_once_when_neither_attribute_is_registered)
{
    Bridge b{};
    int leaves = 0;
    b.port.set_observer(SrpPortObserver{.on_talker_leave = [&](StreamId const&) { ++leaves; }});
    statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
    fv.stream_id = stream_id(5);
    fv.destination_address = GROUP;
    fv.vlan_identifier = 2;
    fv.max_frame_size = 185;
    fv.max_interval_frames = 1;
    (void)b.bridge.declare_talker_advertise(fv, b.now);
    b.run(4);
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise = fv;
    failed.set_failure_code(statusbar::srp::msrp::FailureCode::InsufficientBandwidth);
    (void)b.bridge.withdraw_talker(fv.stream_id, b.now);
    (void)b.bridge.declare_talker_failed(failed, b.now);
    b.run(12);
    EXPECT_NE(b.port.find_registered_talker_failed(GROUP, 2), nullptr);
    EXPECT_EQ(b.port.find_registered_talker_advertise(GROUP, 2), nullptr);
    EXPECT_EQ(leaves, 0);
    (void)b.bridge.withdraw_talker(fv.stream_id, b.now);
    b.run(12);
    EXPECT_EQ(leaves, 1);
}

// An Advertise and a Talker Failed for one StreamID leaving in one pass are one talker leaving.
TEST(nanoavb_srp_port, a_talker_with_both_attributes_leaves_once)
{
    Bridge b{};
    int leaves = 0;
    b.port.set_observer(SrpPortObserver{.on_talker_leave = [&](StreamId const&) { ++leaves; }});
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise.stream_id = stream_id(6);
    failed.advertise.destination_address = GROUP;
    failed.advertise.vlan_identifier = 2;
    failed.advertise.max_frame_size = 185;
    failed.advertise.max_interval_frames = 1;
    failed.set_failure_code(statusbar::srp::msrp::FailureCode::InsufficientBandwidth);
    (void)b.bridge.declare_talker_advertise(failed.advertise, b.now);
    (void)b.bridge.declare_talker_failed(failed, b.now);
    b.run(4);
    EXPECT_NE(b.port.find_registered_talker_advertise(GROUP, 2), nullptr);
    EXPECT_NE(b.port.find_registered_talker_failed(GROUP, 2), nullptr);
    (void)b.bridge.withdraw_talker(failed.advertise.stream_id, b.now);
    b.run(12);
    EXPECT_EQ(b.port.find_registered_talker_failed(GROUP, 2), nullptr);
    EXPECT_EQ(leaves, 1);
}

// The port's participant holds every advertise a bridge propagates (Clause 35.2.2.8.3 discovery):
// the stream wanted is found after 39 others.
TEST(nanoavb_srp_port, a_stream_is_found_among_forty_foreign_advertises)
{
    Bridge b{};
    for (uint16_t uid = 0; uid < 40; ++uid) {
        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fv.stream_id = stream_id(static_cast<uint16_t>(0x100 + uid));
        fv.destination_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x10, static_cast<uint8_t>(uid)};
        fv.vlan_identifier = 2;
        fv.max_frame_size = 185;
        fv.max_interval_frames = 1;
        if (uid == 39) {
            fv.destination_address = GROUP;
        }
        EXPECT_TRUE(b.bridge.declare_talker_advertise(fv, b.now).has_value());
    }
    b.run(12);
    auto const* found = b.port.find_registered_talker_advertise(GROUP, 2);
    EXPECT_NE(found, nullptr);
    EXPECT_TRUE(found != nullptr && found->stream_id == stream_id(0x100 + 39));
}

// withdraw_all() sends the Leaves at once: with the port silent after it, the bridge's registrations
// of its talker and listener age out by the bridge's leave timer, not at its next LeaveAll.
TEST(nanoavb_srp_port, withdraw_all_sends_the_leaves_at_once)
{
    Bridge b{};
    statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
    fv.stream_id = stream_id(8);
    fv.destination_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x20, 0x01};
    fv.vlan_identifier = 2;
    fv.max_frame_size = 185;
    fv.max_interval_frames = 1;
    (void)b.bridge.declare_talker_advertise(fv, b.now);
    EXPECT_TRUE(b.port.declare_domain(b.now).has_value());
    EXPECT_TRUE(b.port.declare_talker(talker(4), b.now).has_value());
    b.run(4);
    EXPECT_TRUE(b.port.attach_listener(fv.stream_id, b.now).has_value());
    b.run(4);
    EXPECT_NE(b.bridge.find_registered_talker_advertise(stream_id(4)), nullptr);
    EXPECT_TRUE(b.bridge.listener_permits_transmit(fv.stream_id));

    b.port.withdraw_all(b.now);
    EXPECT_FALSE(b.port_out.empty());  // sent now, not at the next join timer
    for (auto const& p : std::exchange(b.port_out, {})) {
        b.bridge.receive_pdu(p, b.now);
    }
    b.now += std::chrono::seconds{2};
    b.bridge.tick(b.now);
    EXPECT_EQ(b.bridge.find_registered_talker_advertise(stream_id(4)), nullptr);
    EXPECT_FALSE(b.bridge.listener_permits_transmit(fv.stream_id));
}

// Without the interface (or the privilege to open raw sockets) nothing joins the reactor.
TEST(nanoavb_srp_port, a_missing_interface_is_refused)
{
    SrpPort port{SrpPortConfig{.interface_name = "srp-port-test-none"}};
    statusbar::itc::StopToken stop{};
    statusbar::net::MessageReactor reactor{stop, [] { return int64_t{0}; }, 0};
    auto const status = port.add_to_reactor(reactor);
    EXPECT_EQ(reactor.active_count(), size_t{0});
    EXPECT_FALSE(status.has_value());
    EXPECT_TRUE(!status.has_value() && status.error() == make_error_code(NanoAvbError::InvalidConfiguration));
}

TEST_MAIN(statusbar_nanoavb, nanoavb_srp_port_test)
