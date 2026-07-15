// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <system_error>

using namespace statusbar::nanoavb;
using statusbar::ieee::Eui48;
using TimePoint = statusbar::sm::TimePoint;

//
// MVRP Constants Tests
//
TEST(nanoavb_srp_mvrp, ethertype)
{
    EXPECT_EQ(MvrpHandler::ethertype(), 0x88F5);
}

TEST(nanoavb_srp_mvrp, multicast_address)
{
    EXPECT_EQ(MvrpHandler::multicast_address(), 0x0180C2000021ULL);
}

//
// MVRP Handler Tests
//
TEST(nanoavb_srp_mvrp, register_vlan)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};

    auto result = handler.register_vlan(2, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());

    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Pending);
}

TEST(nanoavb_srp_mvrp, register_invalid_vlan)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};

    auto result = handler.register_vlan(0, TimePoint{std::chrono::seconds{1}});
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error() == make_error_code(NanoAvbError::InvalidVlanId));

    result = handler.register_vlan(4095, TimePoint{std::chrono::seconds{1}});
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error() == make_error_code(NanoAvbError::InvalidVlanId));
}

TEST(nanoavb_srp_mvrp, withdraw_vlan)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};

    (void)handler.register_vlan(2, TimePoint{std::chrono::seconds{1}});
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Pending);

    auto result = handler.withdraw_vlan(2, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);
}

TEST(nanoavb_srp_mvrp, withdraw_nonexistent_vlan)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};

    // Withdrawing a non-existent VLAN should succeed (idempotent)
    auto result = handler.withdraw_vlan(99, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());
}

TEST(nanoavb_srp_mvrp, multiple_vlans)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};

    (void)handler.register_vlan(2, TimePoint{std::chrono::seconds{1}});
    (void)handler.register_vlan(100, TimePoint{std::chrono::seconds{1}});
    (void)handler.register_vlan(200, TimePoint{std::chrono::seconds{1}});

    EXPECT_EQ(handler.vlans().size(), 3);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Pending);
    EXPECT_EQ(handler.get_vlan_state(100), VlanState::Pending);
    EXPECT_EQ(handler.get_vlan_state(200), VlanState::Pending);
}

TEST(nanoavb_srp_mvrp, register_vlan_table_full)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};

    // Fill every slot with a live registration (default capacity = 16).
    for (uint16_t vid = 1; vid <= 16; ++vid) {
        EXPECT_TRUE(handler.register_vlan(vid, now).has_value());
    }
    EXPECT_EQ(handler.vlans().size(), 16);

    // A 17th distinct VLAN while all slots are live is rejected; no live
    // VLAN loses its slot to the newcomer and the table is unchanged.
    auto const result = handler.register_vlan(17, now);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error() == make_error_code(NanoAvbError::VlanTableFull));
    EXPECT_EQ(handler.vlans().size(), 16);
    EXPECT_EQ(handler.get_vlan_state(17), VlanState::Unregistered);
    EXPECT_EQ(handler.get_vlan_state(1), VlanState::Pending);
}

TEST(nanoavb_srp_mvrp, register_vlan_reclaims_withdrawn_slot)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};

    for (uint16_t vid = 1; vid <= 16; ++vid) {
        EXPECT_TRUE(handler.register_vlan(vid, now).has_value());
    }

    // Withdraw one VLAN: the slot becomes a reclaimable tombstone (withdraw
    // marks Unregistered, it does not erase), so size stays at capacity.
    EXPECT_TRUE(handler.withdraw_vlan(8, now).has_value());
    EXPECT_EQ(handler.get_vlan_state(8), VlanState::Unregistered);
    EXPECT_EQ(handler.vlans().size(), 16);

    // A new VLAN reclaims the withdrawn slot instead of being rejected.
    EXPECT_TRUE(handler.register_vlan(17, now).has_value());
    EXPECT_EQ(handler.get_vlan_state(17), VlanState::Pending);
    EXPECT_EQ(handler.vlans().size(), 16);
    // The withdrawn VLAN is gone; a live VLAN is untouched.
    EXPECT_EQ(handler.get_vlan_state(8), VlanState::Unregistered);
    EXPECT_EQ(handler.get_vlan_state(1), VlanState::Pending);
}

TEST(nanoavb_srp_mvrp, receive_short_packet)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};

    // Too short packet should be ignored
    std::array<uint8_t, 2> short_packet = {0, 0};
    handler.receive_packet(short_packet, TimePoint{std::chrono::seconds{1}});  // Should not crash
}

//
// MSRP Constants Tests
//
TEST(nanoavb_srp_msrp, ethertype)
{
    EXPECT_EQ(MsrpHandler<>::ethertype(), 0x22EA);
}

TEST(nanoavb_srp_msrp, multicast_address)
{
    EXPECT_EQ(MsrpHandler<>::multicast_address(), 0x0180C200000EULL);
}

//
// MSRP Handler Domain Tests
//
TEST(nanoavb_srp_msrp, default_domain)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    auto const& domain = handler.domain();
    EXPECT_EQ(domain.sr_class_id, 6);
    EXPECT_EQ(domain.sr_class_priority, 3);
    EXPECT_EQ(domain.sr_class_vid, 2);
}

TEST(nanoavb_srp_msrp, set_domain)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    DomainInfo new_domain{5, 2, 100};
    handler.set_domain(new_domain);

    auto const& domain = handler.domain();
    EXPECT_EQ(domain.sr_class_id, 5);
    EXPECT_EQ(domain.sr_class_priority, 2);
    EXPECT_EQ(domain.sr_class_vid, 100);
}

//
// MSRP Handler Talker Tests
//
TEST(nanoavb_srp_msrp, talker_advertise)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(1);

    TalkerStreamSrpInfo info;
    info.stream_id = stream_id;
    info.max_frame_size = 128;
    info.max_interval_frames = 1;

    auto result = handler.talker_advertise(info, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);

    auto const* stored = handler.get_talker_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->state, TalkerReservationState::Advertising);
    EXPECT_EQ(stored->max_frame_size, 128);
}

TEST(nanoavb_srp_msrp, talker_withdraw)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(1);

    TalkerStreamSrpInfo info;
    info.stream_id = stream_id;

    (void)handler.talker_advertise(info, TimePoint{std::chrono::seconds{1}});

    auto result = handler.talker_withdraw(stream_id, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());

    auto const* stored = handler.get_talker_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->state, TalkerReservationState::Idle);
}

TEST(nanoavb_srp_msrp, talker_withdraw_nonexistent)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_unique_id(99);

    auto result = handler.talker_withdraw(stream_id, TimePoint{std::chrono::seconds{1}});
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(result.error() == make_error_code(NanoAvbError::InvalidStreamIndex));
}

// End-to-end: a talker handler advertising a stream is told (via the
// on_talker_listener hook) when a remote listener becomes ready for it. Two
// handlers exchange MSRPDUs through their send_packet callbacks.
TEST(nanoavb_srp_msrp, on_talker_listener_fires_when_listener_ready)
{
    using statusbar::srp::msrp::MsrpConfig;

    TimePoint now{std::chrono::seconds{1}};

    std::vector<std::vector<uint8_t>> talker_out;
    std::vector<std::vector<uint8_t>> listener_out;

    MsrpHandler<> talker{MsrpConfig{}, MsrpCallbacks{.send_packet = [&talker_out](std::span<uint8_t const> p) {
                             talker_out.emplace_back(p.begin(), p.end());
                             return true;
                         }}};
    MsrpHandler<> listener{MsrpConfig{}, MsrpCallbacks{.send_packet = [&listener_out](std::span<uint8_t const> p) {
                               listener_out.emplace_back(p.begin(), p.end());
                               return true;
                           }}};

    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(7);

    statusbar::tsn::StreamId other;
    other.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    other.set_unique_id(99);

    bool fired_for_sid = false;
    bool ready_for_sid = false;
    bool fired_for_other = false;
    talker.set_on_talker_listener([&](StreamId const& s, bool ready) {
        if (s == sid) {
            fired_for_sid = true;
            ready_for_sid = ready;
        } else if (s == other) {
            fired_for_other = true;
        }
    });

    TalkerStreamSrpInfo info;
    info.stream_id = sid;
    info.max_frame_size = 416;
    info.max_interval_frames = 1;
    (void)talker.talker_advertise(info, now);
    (void)listener.listener_ready(sid, now);
    // A listener for a stream the talker does NOT advertise must be filtered out.
    (void)listener.listener_ready(other, now);

    auto pump = [&now](std::vector<std::vector<uint8_t>>& out, MsrpHandler<>& dst) {
        for (auto const& p : out) {
            dst.receive_packet(p, now);
        }
        out.clear();
    };

    for (int round = 0; round < 12; ++round) {
        now += std::chrono::milliseconds{120};
        talker.tick(now);
        listener.tick(now);
        pump(talker_out, listener);
        pump(listener_out, talker);
    }

    EXPECT_TRUE(fired_for_sid);
    EXPECT_TRUE(ready_for_sid);
    // 'other' is not one of the talker's advertised streams -> never forwarded.
    EXPECT_FALSE(fired_for_other);
}

// Kit phase 5d: listener_attach declares AskingFailed until the talker's
// Advertise is registered from the wire, then upgrades to Ready
// automatically (802.1Q 35.2.4.4) — the MSRP half of a fast-connect
// racing a talker's boot.
TEST(nanoavb_srp_msrp, listener_attach_upgrades_when_talker_advertises)
{
    using statusbar::srp::msrp::MsrpConfig;

    TimePoint now{std::chrono::seconds{1}};

    std::vector<std::vector<uint8_t>> talker_out;
    std::vector<std::vector<uint8_t>> listener_out;

    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(7);

    std::vector<ListenerReservationState> state_changes;
    MsrpHandler<> talker{MsrpConfig{}, MsrpCallbacks{.send_packet = [&talker_out](std::span<uint8_t const> p) {
                             talker_out.emplace_back(p.begin(), p.end());
                             return true;
                         }}};
    MsrpHandler<> listener{
        MsrpConfig{},
        MsrpCallbacks{
            .send_packet =
                [&listener_out](std::span<uint8_t const> p) {
                    listener_out.emplace_back(p.begin(), p.end());
                    return true;
                },
            .on_listener_state_change =
                [&](StreamId const& s, ListenerReservationState state) {
                    if (s == sid) {
                        state_changes.push_back(state);
                    }
                }}};

    // Attach before the talker exists on the wire: AskingFailed.
    auto const attached = listener.listener_attach(sid, now);
    EXPECT_TRUE(attached.has_value());
    EXPECT_EQ(*attached, ListenerReservationState::AskingFailed);
    EXPECT_TRUE(!listener.talker_advertise_registered(sid));
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::AskingFailed);

    // The talker boots and advertises; pump PDUs both ways.
    TalkerStreamSrpInfo info;
    info.stream_id = sid;
    info.max_frame_size = 416;
    info.max_interval_frames = 1;
    (void)talker.talker_advertise(info, now);

    auto pump = [&now](std::vector<std::vector<uint8_t>>& out, MsrpHandler<>& dst) {
        for (auto const& p : out) {
            dst.receive_packet(p, now);
        }
        out.clear();
    };
    for (int round = 0; round < 12; ++round) {
        now += std::chrono::milliseconds{120};
        talker.tick(now);
        listener.tick(now);
        pump(talker_out, listener);
        pump(listener_out, talker);
    }

    // The Advertise registered and the attached listener upgraded to Ready,
    // surfacing the change through on_listener_state_change.
    EXPECT_TRUE(listener.talker_advertise_registered(sid));
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::Ready);
    EXPECT_TRUE(!state_changes.empty());
    EXPECT_EQ(state_changes.back(), ListenerReservationState::Ready);

    // A fresh attach while the talker is advertising is immediately Ready.
    statusbar::tsn::StreamId sid2 = sid;
    (void)sid2;  // same stream: re-attach is idempotent Ready
    auto const reattached = listener.listener_attach(sid, now);
    EXPECT_TRUE(reattached.has_value());
    EXPECT_EQ(*reattached, ListenerReservationState::Ready);
}

//
// MSRP Handler Listener Tests
//
TEST(nanoavb_srp_msrp, listener_ready)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(1);

    auto result = handler.listener_ready(stream_id, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);

    auto const* stored = handler.get_listener_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->state, ListenerReservationState::Ready);
}

TEST(nanoavb_srp_msrp, listener_asking_failed)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(1);

    auto result = handler.listener_asking_failed(stream_id, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());

    auto const* stored = handler.get_listener_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->state, ListenerReservationState::AskingFailed);
}

TEST(nanoavb_srp_msrp, listener_withdraw)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(1);

    (void)handler.listener_ready(stream_id, TimePoint{std::chrono::seconds{1}});

    auto result = handler.listener_withdraw(stream_id, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());

    auto const* stored = handler.get_listener_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->state, ListenerReservationState::Idle);
}

TEST(nanoavb_srp_msrp, listener_withdraw_nonexistent)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    statusbar::tsn::StreamId stream_id;
    stream_id.set_unique_id(99);

    // Withdrawing a non-existent listener should succeed (idempotent)
    auto result = handler.listener_withdraw(stream_id, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(result.has_value());
}

TEST(nanoavb_srp_msrp, multiple_streams)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    for (int i = 0; i < 4; ++i) {
        statusbar::tsn::StreamId stream_id;
        stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
        stream_id.set_unique_id(i);

        TalkerStreamSrpInfo info;
        info.stream_id = stream_id;
        (void)handler.talker_advertise(info, TimePoint{std::chrono::seconds{1}});
    }

    EXPECT_EQ(handler.talker_streams().size(), 4);
}

TEST(nanoavb_srp_msrp, receive_short_packet)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    // Too short packet should be ignored
    std::array<uint8_t, 2> short_packet = {0, 0};
    handler.receive_packet(short_packet, TimePoint{std::chrono::seconds{1}});  // Should not crash
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_srp_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_srp_test");
    return result;
}