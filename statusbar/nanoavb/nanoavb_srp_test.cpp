// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/srp/srp_msrp_carried_test_support.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

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
// MSRP Domain (Milan v1.2 Section 4.2.7.2.1)
//
namespace {

/// A bare MSRP participant standing in for the bridge port, wired to an
/// MsrpHandler (the end station) through two packet queues.
struct DomainBench
{
    TimePoint now{std::chrono::seconds{1}};
    std::vector<std::vector<uint8_t>> station_out{};
    std::vector<std::vector<uint8_t>> bridge_out{};
    MsrpHandler<> station{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) {
                              station_out.emplace_back(p.begin(), p.end());
                              return true;
                          }}};
    statusbar::srp::msrp::MsrpParticipant bridge{statusbar::srp::msrp::MsrpConfig{}, 0x5eed};
    std::vector<statusbar::srp::msrp::DomainFirstValue> seen_by_bridge{};

    DomainBench()
    {
        bridge.set_send_pdu([this](std::span<uint8_t const> p) {
            bridge_out.emplace_back(p.begin(), p.end());
            return true;
        });
        statusbar::srp::msrp::Observer obs{};
        obs.on_domain = [this](statusbar::srp::msrp::DomainFirstValue const& fv, statusbar::srp::msrp::Operation) {
            seen_by_bridge.push_back(fv);
        };
        (void)bridge.subscribe(obs);
        bridge.start(now);
    }

    void run(int rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{250};
            station.tick(now);
            bridge.tick(now);
            for (auto const& p : station_out) {
                bridge.receive_pdu(p, now);
            }
            station_out.clear();
            for (auto const& p : bridge_out) {
                station.receive_packet(p, now);
            }
            bridge_out.clear();
        }
    }

    static auto domain(uint8_t id, uint8_t prio, uint16_t vid) -> statusbar::srp::msrp::DomainFirstValue
    {
        statusbar::srp::msrp::DomainFirstValue fv{};
        fv.sr_class_id = id;
        fv.sr_class_priority = prio;
        fv.sr_class_vid = vid;
        return fv;
    }
};

}  // namespace

TEST(nanoavb_srp_msrp, declares_class_a_domain_only)
{
    DomainBench b{};
    (void)b.station.declare_domain(b.now);
    b.run(4);
    EXPECT_FALSE(b.seen_by_bridge.empty());
    for (auto const& fv : b.seen_by_bridge) {
        EXPECT_EQ(fv.sr_class_id.get(), 6);
        EXPECT_EQ(fv.sr_class_priority.get(), 3);
        EXPECT_EQ(fv.sr_class_vid.get(), 2);
    }
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
    TimePoint fired_at{};
    talker.set_on_talker_listener([&](StreamId const& s, bool ready, TimePoint const at) {
        if (s == sid) {
            fired_for_sid = true;
            ready_for_sid = ready;
            fired_at = at;
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
    // The hook is told the time of the receive_packet()/tick() it fired from:
    // one of the round times, never a clock read of its own.
    EXPECT_NE(fired_at, TimePoint{});
    auto const since_start = std::chrono::duration_cast<std::chrono::milliseconds>(fired_at - TimePoint{std::chrono::seconds{1}});
    EXPECT_EQ(since_start.count() % 120, 0);
    EXPECT_TRUE(fired_at <= now);
    // 'other' is not one of the talker's advertised streams -> never forwarded.
    EXPECT_FALSE(fired_for_other);
}

// on_talker_listener permits transmission for Listener Ready or Ready Failed, not Asking Failed,
// and stops once the registrar leaves (Milan v1.2 Section 5.3.7.3).
TEST(nanoavb_srp_msrp, on_talker_listener_permits_ready_and_ready_failed_only)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    using statusbar::srp::msrp::MsrpConfig;

    TimePoint now{std::chrono::seconds{1}};
    std::vector<std::vector<uint8_t>> talker_out;
    std::vector<std::vector<uint8_t>> listener_out;
    MsrpHandler<> talker{MsrpConfig{}, MsrpCallbacks{.send_packet = [&talker_out](std::span<uint8_t const> p) {
                             talker_out.emplace_back(p.begin(), p.end());
                             return true;
                         }}};
    statusbar::srp::msrp::MsrpParticipant listener{MsrpConfig{}};
    listener.set_send_pdu([&listener_out](std::span<uint8_t const> p) {
        listener_out.emplace_back(p.begin(), p.end());
        return true;
    });
    listener.start(now);

    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(7);
    std::optional<bool> permits;
    talker.set_on_talker_listener([&](StreamId const& s, bool ready, TimePoint) {
        if (s == sid) {
            permits = ready;
        }
    });
    TalkerStreamSrpInfo info;
    info.stream_id = sid;
    info.max_frame_size = 416;
    info.max_interval_frames = 1;
    (void)talker.talker_advertise(info, now);

    auto const run = [&](int rounds) {
        for (int round = 0; round < rounds; ++round) {
            now += std::chrono::milliseconds{120};
            talker.tick(now);
            listener.tick(now);
            for (auto const& p : talker_out) {
                listener.receive_pdu(p, now);
            }
            talker_out.clear();
            for (auto const& p : listener_out) {
                talker.receive_packet(p, now);
            }
            listener_out.clear();
        }
    };

    (void)listener.declare_listener(sid, ListenerDeclaration::AskingFailed, now);
    run(12);
    EXPECT_TRUE(permits.has_value());
    EXPECT_FALSE(permits.value_or(true));

    (void)listener.declare_listener(sid, ListenerDeclaration::ReadyFailed, now);
    run(12);
    EXPECT_TRUE(permits.value_or(false));

    (void)listener.declare_listener(sid, ListenerDeclaration::Ready, now);
    run(12);
    EXPECT_TRUE(permits.value_or(false));

    (void)listener.withdraw_listener(sid, now);
    run(40);
    EXPECT_FALSE(permits.value_or(true));
}

// The Milan listener (v1.2 Section 5.3.8.5) declares only for a registered talker, never Ready
// Failed, and withdraws when it leaves; peer_talker_* see only the live registration.
TEST(nanoavb_srp_msrp, milan_listener_declares_only_for_a_registered_talker)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    using statusbar::srp::msrp::MsrpConfig;
    using statusbar::srp::msrp::MsrpParticipant;

    TimePoint now{std::chrono::seconds{1}};
    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(9);

    std::vector<std::vector<uint8_t>> talker_out;
    std::vector<std::vector<uint8_t>> listener_out;
    MsrpParticipant talker{milan_msrp_config(MsrpConfig{}), 0x7a1};
    talker.set_instant_leave(true);
    talker.set_send_pdu([&talker_out](std::span<uint8_t const> p) {
        talker_out.emplace_back(p.begin(), p.end());
        return true;
    });
    talker.start(now);
    MsrpHandler<> listener{
        milan_msrp_config(MsrpConfig{}), MsrpCallbacks{.send_packet = [&listener_out](std::span<uint8_t const> p) {
            listener_out.emplace_back(p.begin(), p.end());
            return true;
        }}};
    listener.set_listener_waits_for_talker(true);
    listener.set_instant_leave(true);  // Milan v1.3 Section 4.2.7.2.2, as atdecc.profile = "milan13"
    int hooks = 0;
    listener.set_on_remote_talker([&](StreamId const& s, TimePoint) {
        if (s == sid) {
            ++hooks;
        }
    });
    auto run = [&](int const rounds) {
        for (int round = 0; round < rounds; ++round) {
            now += std::chrono::milliseconds{120};
            talker.tick(now);
            listener.tick(now);
            for (auto const& p : talker_out) {
                listener.receive_packet(p, now);
            }
            talker_out.clear();
            for (auto const& p : listener_out) {
                talker.receive_pdu(p, now);
            }
            listener_out.clear();
        }
    };
    auto const heard = [&] {
        auto const* rec = talker.find_listener(sid);
        return rec != nullptr && rec->registrar_is_registered() ? rec->substate : ListenerDeclaration::Ignore;
    };

    auto const attached = listener.listener_attach(sid, now);
    EXPECT_TRUE(attached.has_value());
    EXPECT_EQ(*attached, ListenerReservationState::Waiting);
    run(12);
    EXPECT_TRUE(heard() == ListenerDeclaration::Ignore);  // nothing declared

    statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
    fv.stream_id = sid;
    fv.vlan_identifier = 2;
    fv.max_frame_size = 416;
    fv.max_interval_frames = 1;
    (void)talker.declare_talker_advertise(fv, now);
    run(12);
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::Ready);
    EXPECT_TRUE(heard() == ListenerDeclaration::Ready);
    EXPECT_TRUE(listener.peer_talker_advertise(sid) != nullptr);
    EXPECT_TRUE(hooks > 0);

    // The talker leaves: the declaration goes; the lingering record is no
    // registration.
    int const before = hooks;
    (void)talker.withdraw_talker(sid, now);
    run(12);
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::Waiting);
    EXPECT_TRUE(listener.peer_talker_advertise(sid) == nullptr);
    EXPECT_TRUE(heard() == ListenerDeclaration::Ignore);
    EXPECT_TRUE(hooks > before);

    // A Talker Failed: Asking Failed.
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise = fv;
    failed.failure_code = 8;
    (void)talker.declare_talker_failed(failed, now);
    run(12);
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::AskingFailed);
    EXPECT_TRUE(heard() == ListenerDeclaration::AskingFailed);
    EXPECT_TRUE(listener.peer_talker_failed(sid) != nullptr);

    // Withdrawn while Waiting or declared: Idle, nothing left declared.
    EXPECT_TRUE(listener.listener_withdraw(sid, now).has_value());
    run(12);
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::Idle);
    EXPECT_TRUE(heard() == ListenerDeclaration::Ignore);
}

// Kit phase 5d: listener_attach holds the listener Waiting, with nothing
// declared, until the talker's Advertise is registered from the wire, then
// declares Ready by itself: the MSRP half of a fast-connect racing a talker's
// boot.
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
    std::vector<TimePoint> change_times;  // the time each change was told
    std::vector<TimePoint> event_times;   // the receive_packet()/tick() time it fired from
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
                [&](StreamId const& s, ListenerReservationState state, TimePoint told) {
                    if (s == sid) {
                        state_changes.push_back(state);
                        change_times.push_back(told);
                        event_times.push_back(now);
                    }
                }}};

    // Attach before the talker exists on the wire: Waiting.
    auto const attached = listener.listener_attach(sid, now);
    EXPECT_TRUE(attached.has_value());
    EXPECT_EQ(*attached, ListenerReservationState::Waiting);
    EXPECT_TRUE(!listener.talker_advertise_registered(sid));
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::Waiting);

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

    // The Advertise registered and the attached listener went to Ready,
    // surfacing the change through on_listener_state_change.
    EXPECT_TRUE(listener.talker_advertise_registered(sid));
    EXPECT_EQ(listener.get_listener_stream(sid)->state, ListenerReservationState::Ready);
    EXPECT_TRUE(!state_changes.empty());
    EXPECT_EQ(state_changes.back(), ListenerReservationState::Ready);
    // The hook is told the time of the receive_packet()/tick() it fired from.
    EXPECT_TRUE(change_times == event_times);

    // A fresh attach while the talker is advertising is immediately Ready.
    statusbar::tsn::StreamId sid2 = sid;
    (void)sid2;  // same stream: re-attach is idempotent Ready
    auto const reattached = listener.listener_attach(sid, now);
    EXPECT_TRUE(reattached.has_value());
    EXPECT_EQ(*reattached, ListenerReservationState::Ready);
}

namespace {

/// A talker's MSRP participant and a listener's MsrpHandler on one link: what
/// each sends reaches the other on the next run() round, and every PDU the
/// listener sent is kept for the test to read.
struct ListenerLink
{
    TimePoint now{std::chrono::seconds{1}};
    std::vector<std::vector<uint8_t>> talker_out{};
    std::vector<std::vector<uint8_t>> listener_out{};
    std::vector<std::vector<uint8_t>> listener_sent{};
    std::vector<ListenerReservationState> states{};
    statusbar::tsn::StreamId sid{Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 9};
    statusbar::srp::msrp::MsrpParticipant talker{statusbar::srp::msrp::MsrpConfig{}, 0x7a11};
    MsrpHandler<> listener{
        statusbar::srp::msrp::MsrpConfig{},
        MsrpCallbacks{
            .send_packet =
                [this](std::span<uint8_t const> p) {
                    listener_out.emplace_back(p.begin(), p.end());
                    return true;
                },
            .on_listener_state_change =
                [this](StreamId const&, ListenerReservationState const state, TimePoint) { states.push_back(state); }}};

    ListenerLink()
    {
        talker.set_send_pdu([this](std::span<uint8_t const> p) {
            talker_out.emplace_back(p.begin(), p.end());
            return true;
        });
        talker.start(now);
    }

    void deliver()
    {
        for (auto const& p : std::exchange(talker_out, {})) {
            listener.receive_packet(p, now);
        }
        for (auto const& p : std::exchange(listener_out, {})) {
            talker.receive_pdu(p, now);
            listener_sent.push_back(p);
        }
    }

    void run(int const rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{120};
            talker.tick(now);
            listener.tick(now);
            deliver();
        }
    }

    /// The Listener values the listener has sent since the last call.
    [[nodiscard]] auto take_sent() -> std::vector<statusbar::srp::msrp::test_support::CarriedListener>
    {
        return statusbar::srp::msrp::test_support::listener_carried(std::exchange(listener_sent, {}));
    }

    [[nodiscard]] auto state() const -> ListenerReservationState { return listener.get_listener_stream(sid)->state; }

    [[nodiscard]] auto advertise() const -> statusbar::srp::msrp::TalkerAdvertiseFirstValue
    {
        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fv.stream_id = sid;
        fv.destination_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34};
        fv.vlan_identifier = statusbar::srp::msrp::default_sr_class_vid;
        fv.max_frame_size = 416;
        return fv;
    }

    [[nodiscard]] auto failed() const -> statusbar::srp::msrp::TalkerFailedFirstValue
    {
        statusbar::srp::msrp::TalkerFailedFirstValue fv{};
        fv.advertise = advertise();
        fv.set_failure_code(statusbar::srp::msrp::FailureCode::InsufficientBandwidth);
        return fv;
    }
};

}  // namespace

// A listener attached before its talker advertises declares nothing (Milan
// v1.3 5.3.8.5 allows either that or Asking Failed). When the Talker
// Advertise is registered its first declaration is Ready, as New: a bridge
// first learns of this listener as a new registration of the type that
// holds, with no change of Declaration Type for it to act on.
TEST(nanoavb_srp_msrp, a_listener_attached_before_its_talker_declares_nothing_until_the_advertise_arrives)
{
    using statusbar::srp::mrp::AttributeEvent;
    using statusbar::srp::msrp::ListenerDeclaration;
    ListenerLink link{};

    auto const attached = link.listener.listener_attach(link.sid, link.now);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::Waiting);
    link.run(30);
    EXPECT_TRUE(link.take_sent().empty());
    EXPECT_EQ(link.listener.get_listener_stream(link.sid)->attached, 1);
    EXPECT_TRUE(link.states.empty());

    EXPECT_TRUE(link.talker.declare_talker_advertise(link.advertise(), link.now).has_value());
    link.run(12);
    EXPECT_EQ(link.state(), ListenerReservationState::Ready);
    EXPECT_TRUE(link.states == std::vector<ListenerReservationState>{ListenerReservationState::Ready});
    auto const sent = link.take_sent();
    EXPECT_TRUE(!sent.empty() && sent.front().event == AttributeEvent::New);
    EXPECT_TRUE(std::ranges::all_of(sent, [](auto const& value) {
        return value.declaration == ListenerDeclaration::Ready && value.event != AttributeEvent::Lv;
    }));
}

// The listener's declaration follows the talker's: when the Talker Advertise
// has gone the Listener is withdrawn with a Leave and the stream is Waiting
// again, and when the talker returns the Listener is declared anew.
TEST(nanoavb_srp_msrp, a_listener_withdraws_when_its_talker_has_gone_and_declares_again_when_it_returns)
{
    using statusbar::srp::mrp::AttributeEvent;
    using statusbar::srp::msrp::ListenerDeclaration;
    ListenerLink link{};
    EXPECT_TRUE(link.talker.declare_talker_advertise(link.advertise(), link.now).has_value());
    link.run(8);
    auto const attached = link.listener.listener_attach(link.sid, link.now);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::Ready);
    link.run(30);
    (void)link.take_sent();

    EXPECT_TRUE(link.talker.withdraw_talker(link.sid, link.now).has_value());
    link.run(30);
    EXPECT_EQ(link.state(), ListenerReservationState::Waiting);
    EXPECT_EQ(link.listener.get_listener_stream(link.sid)->attached, 1);
    auto const gone = link.take_sent();
    EXPECT_TRUE(!gone.empty() && gone.back().event == AttributeEvent::Lv);

    EXPECT_TRUE(link.talker.declare_talker_advertise(link.advertise(), link.now).has_value());
    link.run(12);
    EXPECT_EQ(link.state(), ListenerReservationState::Ready);
    auto const back = link.take_sent();
    EXPECT_TRUE(!back.empty() && back.front().event == AttributeEvent::New);
    EXPECT_TRUE(!back.empty() && back.front().declaration == ListenerDeclaration::Ready);
    std::vector<ListenerReservationState> const expected{ListenerReservationState::Waiting, ListenerReservationState::Ready};
    EXPECT_TRUE(link.states == expected);
}

// IEEE 802.1Q 35.1.2.2: a registered Talker Failed gets Listener Asking
// Failed. When the Talker Advertise replaces it the Declaration Type changes
// under the StreamID already declared, and that goes out as a Join carrying
// Ready (35.2.6), with no Leave and no New.
TEST(nanoavb_srp_msrp, a_listener_of_a_failed_talker_asks_and_goes_ready_with_a_join)
{
    using statusbar::srp::mrp::AttributeEvent;
    using statusbar::srp::msrp::ListenerDeclaration;
    ListenerLink link{};
    EXPECT_TRUE(link.talker.declare_talker_failed(link.failed(), link.now).has_value());
    link.run(8);
    auto const attached = link.listener.listener_attach(link.sid, link.now);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::AskingFailed);
    link.run(30);
    auto const asking = link.take_sent();
    EXPECT_TRUE(!asking.empty() && asking.front().event == AttributeEvent::New);
    EXPECT_TRUE(!asking.empty() && asking.front().declaration == ListenerDeclaration::AskingFailed);

    EXPECT_TRUE(link.talker.withdraw_talker(link.sid, link.now).has_value());
    EXPECT_TRUE(link.talker.declare_talker_advertise(link.advertise(), link.now).has_value());
    link.run(30);
    EXPECT_EQ(link.state(), ListenerReservationState::Ready);
    EXPECT_TRUE(link.states == std::vector<ListenerReservationState>{ListenerReservationState::Ready});
    auto const sent = link.take_sent();
    auto const first_ready =
        std::ranges::find_if(sent, [](auto const& value) { return value.declaration == ListenerDeclaration::Ready; });
    EXPECT_TRUE(first_ready != sent.end());
    EXPECT_TRUE(std::ranges::none_of(
        sent, [](auto const& value) { return value.event == AttributeEvent::Lv || value.event == AttributeEvent::New; }));
}

// set_listener_waits_for_talker(false): the listener declares Asking Failed
// before its talker advertises, the other choice the standards leave open.
// The Advertise then changes the Declaration Type, which goes out as a Join
// carrying Ready (IEEE 802.1Q 35.2.6), and the talker leaving changes it back.
TEST(nanoavb_srp_msrp, a_listener_told_not_to_wait_declares_asking_failed_before_its_talker)
{
    using statusbar::srp::mrp::AttributeEvent;
    using statusbar::srp::msrp::ListenerDeclaration;
    ListenerLink link{};
    link.listener.set_listener_waits_for_talker(false);

    auto const attached = link.listener.listener_attach(link.sid, link.now);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::AskingFailed);
    link.run(30);
    auto const asking = link.take_sent();
    EXPECT_TRUE(!asking.empty() && asking.front().event == AttributeEvent::New);
    EXPECT_TRUE(!asking.empty() && asking.front().declaration == ListenerDeclaration::AskingFailed);

    EXPECT_TRUE(link.talker.declare_talker_advertise(link.advertise(), link.now).has_value());
    link.run(30);
    EXPECT_EQ(link.state(), ListenerReservationState::Ready);
    EXPECT_TRUE(link.talker.withdraw_talker(link.sid, link.now).has_value());
    link.run(30);
    EXPECT_EQ(link.state(), ListenerReservationState::AskingFailed);
    std::vector<ListenerReservationState> const expected{ListenerReservationState::Ready, ListenerReservationState::AskingFailed};
    EXPECT_TRUE(link.states == expected);
    auto const sent = link.take_sent();
    EXPECT_TRUE(std::ranges::any_of(sent, [](auto const& value) { return value.declaration == ListenerDeclaration::Ready; }));
    EXPECT_TRUE(std::ranges::none_of(
        sent, [](auto const& value) { return value.event == AttributeEvent::Lv || value.event == AttributeEvent::New; }));
}

// A listener told which Talker attribute to follow (the destination address
// and VLAN a Milan sink probed, Milan v1.3 5.3.8.5 and 5.3.8.8) declares
// nothing for a Talker Advertise of its StreamID on another VLAN: it would
// be Ready for a stream it does not receive. When the talker declares the
// stream on the VLAN it named, the listener is Ready.
TEST(nanoavb_srp_msrp, a_listener_that_follows_one_talker_attribute_is_not_ready_for_another_of_its_stream)
{
    using statusbar::srp::mrp::AttributeEvent;
    using statusbar::srp::msrp::ListenerDeclaration;
    ListenerLink link{};
    TalkerMatch const probed{.dest_address = link.advertise().destination_address, .vlan_id = 2};
    auto const attached = link.listener.listener_attach(link.sid, link.now, probed);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::Waiting);
    EXPECT_TRUE(link.listener.get_listener_stream(link.sid)->follows.has_value());

    // The attribute arrives on VLAN 3: registered, and not the one this listener follows.
    auto elsewhere = link.advertise();
    elsewhere.vlan_identifier = 3;
    EXPECT_TRUE(link.talker.declare_talker_advertise(elsewhere, link.now).has_value());
    link.run(30);
    EXPECT_EQ(link.state(), ListenerReservationState::Waiting);
    EXPECT_TRUE(link.take_sent().empty());
    EXPECT_TRUE(link.listener.registered_talker(link.sid).has_value());           // it is registered,
    EXPECT_FALSE(link.listener.registered_talker(link.sid, probed).has_value());  // and it is not the one

    // The talker's new value waits out two LeaveAll periods after the old one left (IEEE 802.1Q 35.2.2.8).
    EXPECT_TRUE(link.talker.declare_talker_advertise(link.advertise(), link.now).has_value());
    link.run(300);
    EXPECT_EQ(link.state(), ListenerReservationState::Ready);
    EXPECT_TRUE(link.listener.registered_talker(link.sid, probed).has_value());
    auto const sent = link.take_sent();
    EXPECT_TRUE(!sent.empty() && sent.front().event == AttributeEvent::New);
    EXPECT_TRUE(!sent.empty() && sent.front().declaration == ListenerDeclaration::Ready);

    // Another destination address is another stream's too.
    TalkerMatch const other_address{.dest_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x35}, .vlan_id = 2};
    EXPECT_FALSE(link.listener.registered_talker(link.sid, other_address).has_value());
    EXPECT_EQ(link.listener.listener_state_wanted(link.sid, other_address), ListenerReservationState::Waiting);
}

// A probe answer that names no VLAN (0) follows the attribute on whichever
// VLAN it is declared, and a listener that names no attribute at all follows
// any of its StreamID, as IEEE 802.1Q has it: the last attach decides.
TEST(nanoavb_srp_msrp, a_listener_follows_any_vlan_when_its_match_names_none_and_any_attribute_without_a_match)
{
    ListenerLink link{};
    auto on_vlan_3 = link.advertise();
    on_vlan_3.vlan_identifier = 3;
    EXPECT_TRUE(link.talker.declare_talker_advertise(on_vlan_3, link.now).has_value());
    link.run(8);

    TalkerMatch const no_vlan{.dest_address = on_vlan_3.destination_address, .vlan_id = 0};
    auto const attached = link.listener.listener_attach(link.sid, link.now, no_vlan);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::Ready);
    EXPECT_TRUE(link.listener.listener_withdraw(link.sid, link.now).has_value());
    EXPECT_FALSE(link.listener.get_listener_stream(link.sid)->follows.has_value());  // gone with the last input

    TalkerMatch const vlan_2{.dest_address = on_vlan_3.destination_address, .vlan_id = 2};
    auto const strict = link.listener.listener_attach(link.sid, link.now, vlan_2);
    EXPECT_TRUE(strict.has_value() && *strict == ListenerReservationState::Waiting);
    auto const plain = link.listener.listener_attach(link.sid, link.now);
    EXPECT_TRUE(plain.has_value() && *plain == ListenerReservationState::Ready);
    EXPECT_FALSE(link.listener.get_listener_stream(link.sid)->follows.has_value());
    EXPECT_EQ(link.listener.get_listener_stream(link.sid)->attached, 2);
}

// An input already on a stream is given another Talker attribute to follow,
// as a Milan sink that settles again on another VLAN or destination address
// is (Milan v1.3 5.3.8.5: Ready goes with a Talker Advertise "with matching
// SRP parameters"). The Ready it held for the attribute left behind is
// withdrawn, the input stays attached once, and the declaration comes back
// when it is told to follow what is registered.
TEST(nanoavb_srp_msrp, an_attached_listener_told_to_follow_another_talker_attribute_leaves_the_one_it_was_ready_for)
{
    using statusbar::srp::mrp::AttributeEvent;
    using statusbar::srp::msrp::ListenerDeclaration;
    ListenerLink link{};
    auto on_vlan_3 = link.advertise();
    on_vlan_3.vlan_identifier = 3;
    EXPECT_TRUE(link.talker.declare_talker_advertise(on_vlan_3, link.now).has_value());
    link.run(8);
    TalkerMatch const vlan_3{.dest_address = on_vlan_3.destination_address, .vlan_id = 3};
    TalkerMatch const vlan_2{.dest_address = on_vlan_3.destination_address, .vlan_id = 2};
    auto const attached = link.listener.listener_attach(link.sid, link.now, vlan_3);
    EXPECT_TRUE(attached.has_value() && *attached == ListenerReservationState::Ready);
    link.run(30);
    (void)link.take_sent();

    // The same attribute again: nothing changes and nothing is sent for it.
    EXPECT_EQ(link.listener.listener_follow(link.sid, vlan_3, link.now), ListenerReservationState::Ready);
    link.run(4);
    auto const same = link.take_sent();
    EXPECT_TRUE(std::ranges::none_of(
        same, [](auto const& value) { return value.event == AttributeEvent::Lv || value.event == AttributeEvent::New; }));

    // Another one, which the talker does not declare: the Ready goes.
    EXPECT_EQ(link.listener.listener_follow(link.sid, vlan_2, link.now), ListenerReservationState::Waiting);
    EXPECT_EQ(link.state(), ListenerReservationState::Waiting);
    EXPECT_EQ(link.listener.get_listener_stream(link.sid)->attached, 1);
    auto const follows = link.listener.get_listener_stream(link.sid)->follows;
    EXPECT_TRUE(follows.has_value() && follows->vlan_id == 2);
    link.run(30);
    auto const left = link.take_sent();
    EXPECT_TRUE(std::ranges::any_of(left, [](auto const& value) { return value.event == AttributeEvent::Lv; }));

    // The registered one again: declared anew.
    EXPECT_EQ(link.listener.listener_follow(link.sid, vlan_3, link.now), ListenerReservationState::Ready);
    link.run(30);
    auto const again = link.take_sent();
    EXPECT_TRUE(!again.empty() && again.front().event == AttributeEvent::New);
    EXPECT_TRUE(!again.empty() && again.front().declaration == ListenerDeclaration::Ready);

    // No attribute named: any of the StreamID, as an IEEE 1722.1 connection follows.
    EXPECT_EQ(link.listener.listener_follow(link.sid, vlan_2, link.now), ListenerReservationState::Waiting);
    EXPECT_EQ(link.listener.listener_follow(link.sid, std::nullopt, link.now), ListenerReservationState::Ready);
    EXPECT_FALSE(link.listener.get_listener_stream(link.sid)->follows.has_value());

    // The caller is handed the state; the hook is for what the network changes.
    EXPECT_TRUE(link.states.empty());

    // One input was attached all along: one withdrawal lets the stream go.
    EXPECT_TRUE(link.listener.listener_withdraw(link.sid, link.now).has_value());
    EXPECT_EQ(link.state(), ListenerReservationState::Idle);
    // A stream no input is attached to follows nothing and is not attached by the call.
    EXPECT_EQ(link.listener.listener_follow(link.sid, vlan_3, link.now), ListenerReservationState::Idle);
    EXPECT_EQ(link.listener.get_listener_stream(link.sid)->attached, 0);
    EXPECT_FALSE(link.listener.get_listener_stream(link.sid)->follows.has_value());
    statusbar::tsn::StreamId const unknown{Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 77};
    EXPECT_EQ(link.listener.listener_follow(unknown, vlan_3, link.now), ListenerReservationState::Idle);
}

// A Talker Failed is followed by the same match as a Talker Advertise.
TEST(nanoavb_srp_msrp, a_listener_that_follows_one_talker_attribute_asks_failed_only_for_a_talker_failed_that_is_it)
{
    ListenerLink link{};
    auto failed_on_vlan_3 = link.failed();
    failed_on_vlan_3.advertise.vlan_identifier = 3;
    EXPECT_TRUE(link.talker.declare_talker_failed(failed_on_vlan_3, link.now).has_value());
    link.run(8);
    TalkerMatch const vlan_2{.dest_address = link.advertise().destination_address, .vlan_id = 2};
    TalkerMatch const vlan_3{.dest_address = link.advertise().destination_address, .vlan_id = 3};
    EXPECT_EQ(link.listener.listener_state_wanted(link.sid, vlan_2), ListenerReservationState::Waiting);
    EXPECT_EQ(link.listener.listener_state_wanted(link.sid, vlan_3), ListenerReservationState::AskingFailed);
    EXPECT_EQ(link.listener.listener_state_wanted(link.sid), ListenerReservationState::AskingFailed);
    auto const registered = link.listener.registered_talker(link.sid, vlan_3);
    EXPECT_TRUE(registered.has_value() && registered->failed);
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

// A full Talker Advertise table: the declaration fails, and the local
// reservation entry is rolled back -- a new entry dropped (so listener events
// for it are not forwarded as ours), an existing one restored.
TEST(nanoavb_srp_msrp, talker_advertise_failure_leaves_no_reservation)
{
    statusbar::srp::msrp::MsrpConfig config{};
    config.max_talker_advertise = 1;
    config.max_interesting_stream_ids = 1;  // validate(): no more than the Advertise table
    MsrpHandler<> handler{config};
    auto const info = [](uint16_t unique) {
        TalkerStreamSrpInfo i;
        i.stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
        i.stream_id.set_unique_id(unique);
        i.max_frame_size = 416;
        i.max_interval_frames = 1;
        return i;
    };
    TimePoint const now{std::chrono::seconds{1}};

    EXPECT_TRUE(handler.talker_advertise(info(1), now).has_value());
    EXPECT_FALSE(handler.talker_advertise(info(2), now).has_value());
    EXPECT_TRUE(handler.get_talker_stream(info(2).stream_id) == nullptr);
    EXPECT_EQ(handler.talker_streams().size(), size_t{1});
    EXPECT_EQ(handler.get_talker_stream(info(1).stream_id)->state, TalkerReservationState::Advertising);
    // Re-advertising the declared stream still works.
    EXPECT_TRUE(handler.talker_advertise(info(1), now).has_value());
}

// A full Listener attribute table: the declaration fails, and the local
// reservation entry must not stay marked as if it had been declared -- a new
// entry is dropped, an existing one keeps its previous state.
TEST(nanoavb_srp_msrp, listener_declare_failure_leaves_no_reservation)
{
    statusbar::srp::msrp::MsrpConfig config{};
    config.max_listeners = 1;
    MsrpHandler<> handler{config};
    auto const sid = [](uint16_t unique) {
        statusbar::tsn::StreamId stream_id;
        stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
        stream_id.set_unique_id(unique);
        return stream_id;
    };
    TimePoint const now{std::chrono::seconds{1}};

    EXPECT_TRUE(handler.listener_ready(sid(1), now).has_value());
    EXPECT_FALSE(handler.listener_ready(sid(2), now).has_value());
    EXPECT_TRUE(handler.get_listener_stream(sid(2)) == nullptr);
    EXPECT_FALSE(handler.listener_asking_failed(sid(3), now).has_value());
    EXPECT_TRUE(handler.get_listener_stream(sid(3)) == nullptr);
    EXPECT_EQ(handler.listener_streams().size(), size_t{1});

    // The declared stream is untouched, and re-declaring it still works.
    EXPECT_EQ(handler.get_listener_stream(sid(1))->state, ListenerReservationState::Ready);
    EXPECT_TRUE(handler.listener_asking_failed(sid(1), now).has_value());
    EXPECT_EQ(handler.get_listener_stream(sid(1))->state, ListenerReservationState::AskingFailed);
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

// Two inputs bound to one talker stream share one Listener declaration: the
// first to unbind must not take it from the other.
TEST(nanoavb_srp_msrp, two_inputs_on_one_stream_share_one_declaration)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};
    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(7);
    TimePoint const now{std::chrono::seconds{1}};

    EXPECT_TRUE(handler.listener_ready(stream_id, now).has_value());
    EXPECT_TRUE(handler.listener_ready(stream_id, now).has_value());
    EXPECT_EQ(handler.listener_streams().size(), 1);  // one record, two inputs
    auto const* stored = handler.get_listener_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->attached, 2);
    EXPECT_EQ(stored->state, ListenerReservationState::Ready);

    // The first input goes: the declaration stays for the second.
    EXPECT_TRUE(handler.listener_withdraw(stream_id, now).has_value());
    EXPECT_EQ(stored->attached, 1);
    EXPECT_EQ(stored->state, ListenerReservationState::Ready);

    // The second goes: withdrawn.
    EXPECT_TRUE(handler.listener_withdraw(stream_id, now).has_value());
    EXPECT_EQ(stored->attached, 0);
    EXPECT_EQ(stored->state, ListenerReservationState::Idle);

    // One withdraw too many changes nothing and does not count below zero.
    EXPECT_TRUE(handler.listener_withdraw(stream_id, now).has_value());
    EXPECT_EQ(stored->attached, 0);
    EXPECT_EQ(stored->state, ListenerReservationState::Idle);

    // A new input attaches to the same stream and the declaration is back.
    EXPECT_TRUE(handler.listener_asking_failed(stream_id, now).has_value());
    EXPECT_EQ(stored->attached, 1);
    EXPECT_EQ(stored->state, ListenerReservationState::AskingFailed);
    EXPECT_EQ(handler.listener_streams().size(), 1);
}

// The end of a run withdraws every listener whatever its inputs.
TEST(nanoavb_srp_msrp, withdraw_all_lets_every_input_go)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};
    statusbar::tsn::StreamId stream_id;
    stream_id.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    stream_id.set_unique_id(8);
    TimePoint const now{std::chrono::seconds{1}};

    EXPECT_TRUE(handler.listener_ready(stream_id, now).has_value());
    EXPECT_TRUE(handler.listener_ready(stream_id, now).has_value());
    handler.withdraw_all(now);
    auto const* stored = handler.get_listener_stream(stream_id);
    EXPECT_TRUE(stored != nullptr);
    EXPECT_EQ(stored->attached, 0);
    EXPECT_EQ(stored->state, ListenerReservationState::Idle);
}

// A withdrawn stream's reservation entry is reused: an end station that attaches and withdraws
// more distinct StreamIDs than the table holds keeps declaring.
TEST(nanoavb_srp_msrp, withdrawn_streams_free_their_entries)
{
    MsrpHandler<> handler{entity_msrp_config(4, 4), MsrpCallbacks{.send_packet = [](std::span<uint8_t const>) { return true; }}};
    TimePoint now{std::chrono::seconds{1}};
    for (uint16_t uid = 0; uid < 48; ++uid) {
        statusbar::tsn::StreamId sid;
        sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
        sid.set_unique_id(uid);
        TalkerStreamSrpInfo info{};
        info.stream_id = sid;
        info.max_frame_size = 185;
        info.max_interval_frames = 1;
        EXPECT_TRUE(handler.talker_advertise(info, now).has_value());
        EXPECT_TRUE(handler.listener_attach(sid, now).has_value());
        EXPECT_TRUE(handler.talker_withdraw(sid, now).has_value());
        EXPECT_TRUE(handler.listener_withdraw(sid, now).has_value());
        for (int i = 0; i < 8; ++i) {  // the participant reclaims what was withdrawn
            now += std::chrono::milliseconds{500};
            handler.tick(now);
        }
    }
    EXPECT_TRUE(handler.talker_streams().size() <= 1);
    EXPECT_TRUE(handler.listener_streams().size() <= 1);
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

namespace {

/// Two MSRP handlers on one wire, each told the other's PDUs.
struct DomainWire
{
    TimePoint now{std::chrono::seconds{1}};
    std::vector<std::vector<uint8_t>> bridge_out;
    std::vector<std::vector<uint8_t>> device_out;
    MsrpHandler<> bridge{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) {
                             bridge_out.emplace_back(p.begin(), p.end());
                             return true;
                         }}};
    MsrpHandler<> device{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) {
                             device_out.emplace_back(p.begin(), p.end());
                             return true;
                         }}};

    void run(int const rounds)
    {
        for (int round = 0; round < rounds; ++round) {
            now += std::chrono::milliseconds{120};
            bridge.tick(now);
            device.tick(now);
            for (auto const& p : bridge_out) {
                device.receive_packet(p, now);
            }
            for (auto const& p : device_out) {
                bridge.receive_packet(p, now);
            }
            bridge_out.clear();
            device_out.clear();
        }
    }
};

}  // namespace

TEST(nanoavb_srp_msrp, a_milan_handler_adopts_the_class_a_domain_registered_from_the_wire)
{
    // Milan v1.3 4.2.7.2.1: a Class A Domain received with other parameters
    // replaces ours, is re-declared, and is reported with the receive time.
    DomainWire wire;
    wire.device.set_adopt_domain(true);
    std::optional<DomainInfo> adopted{};
    TimePoint adopted_at{};
    wire.device.set_on_domain_adopted([&](DomainInfo const& d, TimePoint const at) {
        adopted = d;
        adopted_at = at;
    });
    wire.bridge.set_domain(DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    (void)wire.bridge.declare_domain(wire.now);
    (void)wire.device.declare_domain(wire.now);  // ours: priority 3, VLAN 2
    EXPECT_EQ(wire.device.domain().sr_class_priority, 3);
    EXPECT_EQ(wire.device.domain().sr_class_vid, 2);

    wire.run(6);
    EXPECT_TRUE(adopted.has_value());
    EXPECT_EQ(wire.device.domain().sr_class_priority, 5);
    EXPECT_EQ(wire.device.domain().sr_class_vid, 5);
    EXPECT_TRUE(adopted.has_value() && adopted->sr_class_priority == 5 && adopted->sr_class_vid == 5);
    // Told the time of the receive that brought the peer's Domain, one of the rounds.
    auto const since_start = std::chrono::duration_cast<std::chrono::milliseconds>(adopted_at - TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(since_start.count() > 0 && since_start.count() % 120 == 0);

    // The same Domain again changes nothing more.
    adopted.reset();
    wire.run(6);
    EXPECT_FALSE(adopted.has_value());
}

TEST(nanoavb_srp_msrp, a_domain_with_a_reserved_vid_or_an_impossible_priority_is_not_adopted)
{
    // IEEE 802.1Q-2022 Table 9-2: VID 0 means priority-tagged, no VLAN; 4095
    // is reserved. A priority is three bits. A bridge declaring any of them
    // offers nothing a stream could move to, so the Domain in force stays.
    for (auto const bad : {
             DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 0},
             DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 4095},
             DomainInfo{.sr_class_id = 6, .sr_class_priority = 8, .sr_class_vid = 5},
         }) {
        DomainWire wire;
        wire.device.set_adopt_domain(true);
        std::optional<DomainInfo> adopted{};
        wire.device.set_on_domain_adopted([&](DomainInfo const& d, TimePoint) { adopted = d; });
        wire.bridge.set_domain(bad);
        (void)wire.bridge.declare_domain(wire.now);
        (void)wire.device.declare_domain(wire.now);
        wire.run(6);
        EXPECT_FALSE(adopted.has_value());
        EXPECT_EQ(wire.device.domain().sr_class_priority, 3);
        EXPECT_EQ(wire.device.domain().sr_class_vid, 2);
    }

    // The edges of what is allowed are adopted as before.
    DomainWire wire;
    wire.device.set_adopt_domain(true);
    std::optional<DomainInfo> adopted{};
    wire.device.set_on_domain_adopted([&](DomainInfo const& d, TimePoint) { adopted = d; });
    wire.bridge.set_domain(DomainInfo{.sr_class_id = 6, .sr_class_priority = 7, .sr_class_vid = 4094});
    (void)wire.bridge.declare_domain(wire.now);
    (void)wire.device.declare_domain(wire.now);
    wire.run(6);
    EXPECT_TRUE(adopted.has_value() && adopted->sr_class_priority == 7 && adopted->sr_class_vid == 4094);
}

TEST(nanoavb_srp_msrp, installing_the_sender_alone_keeps_the_hooks_installed_before_it)
{
    // The entity installs its hooks once, at construction; the network path
    // comes with the run and installs the sender then. The sender's
    // installation must leave the hooks alone, or the Domain adopted after
    // start is never followed.
    DomainWire wire;
    MsrpHandler<> device{statusbar::srp::msrp::MsrpConfig{}};  // no callbacks yet
    device.set_adopt_domain(true);
    std::optional<DomainInfo> adopted{};
    device.set_on_domain_adopted([&](DomainInfo const& d, TimePoint) { adopted = d; });
    size_t sent = 0;
    device.set_send_packet([&](std::span<uint8_t const> p) {
        ++sent;
        wire.device_out.emplace_back(p.begin(), p.end());
        return true;
    });

    wire.bridge.set_domain(DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    (void)wire.bridge.declare_domain(wire.now);
    (void)device.declare_domain(wire.now);
    for (int round = 0; round < 6; ++round) {
        wire.now += std::chrono::milliseconds{120};
        wire.bridge.tick(wire.now);
        device.tick(wire.now);
        for (auto const& p : wire.bridge_out) {
            device.receive_packet(p, wire.now);
        }
        for (auto const& p : wire.device_out) {
            wire.bridge.receive_packet(p, wire.now);
        }
        wire.bridge_out.clear();
        wire.device_out.clear();
    }
    EXPECT_TRUE(adopted.has_value());
    EXPECT_TRUE(adopted.has_value() && adopted->sr_class_vid == 5 && adopted->sr_class_priority == 5);
    EXPECT_TRUE(sent > 0);  // the PDUs left through the sender installed on its own
}

TEST(nanoavb_srp_msrp, a_link_up_returns_to_the_configured_domain_and_adopts_afresh)
{
    // Milan v1.3 4.2.7.2.1: after startup or a Link Up the PAAD declares its
    // configured Class A Domain again. The peer's Domain is forgotten with
    // it, so the one the wire then carries, the same or another bridge's,
    // is adopted as on a first contact rather than taken for one already
    // registered.
    DomainWire wire;
    wire.device.set_adopt_domain(true);
    std::vector<DomainInfo> changes;
    wire.device.set_on_domain_adopted([&](DomainInfo const& d, TimePoint) { changes.push_back(d); });
    wire.bridge.set_domain(DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    (void)wire.bridge.declare_domain(wire.now);
    (void)wire.device.declare_domain(wire.now);
    wire.run(6);
    EXPECT_EQ(changes.size(), 1U);
    EXPECT_EQ(wire.device.domain().sr_class_vid, 5);

    // Link Up: the configured Domain is in force and declared again, and the
    // entity hears it at that instant.
    EXPECT_TRUE(wire.device.return_to_configured_domain(wire.now));
    EXPECT_EQ(wire.device.domain().sr_class_priority, 3);
    EXPECT_EQ(wire.device.domain().sr_class_vid, 2);
    EXPECT_EQ(changes.size(), 2U);
    DomainInfo const configured{.sr_class_id = 6, .sr_class_priority = 3, .sr_class_vid = 2};
    EXPECT_TRUE(changes.size() == 2 && changes.back() == configured);

    // The bridge is still there, still declaring: adopted afresh.
    wire.run(12);
    EXPECT_EQ(changes.size(), 3U);
    EXPECT_EQ(wire.device.domain().sr_class_vid, 5);
    EXPECT_EQ(wire.device.domain().sr_class_priority, 5);

    // Nothing adopted: nothing to return from, nothing reported.
    MsrpHandler<> fresh{statusbar::srp::msrp::MsrpConfig{}};
    bool reported = false;
    fresh.set_on_domain_adopted([&](DomainInfo const&, TimePoint) { reported = true; });
    EXPECT_FALSE(fresh.return_to_configured_domain(wire.now));
    EXPECT_FALSE(reported);
}

TEST(nanoavb_srp_msrp, a_classic_handler_keeps_its_own_domain)
{
    DomainWire wire;
    bool adopted = false;
    wire.device.set_on_domain_adopted([&](DomainInfo const&, TimePoint) { adopted = true; });
    wire.bridge.set_domain(DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    (void)wire.bridge.declare_domain(wire.now);
    (void)wire.device.declare_domain(wire.now);
    wire.run(6);
    EXPECT_FALSE(adopted);
    EXPECT_EQ(wire.device.domain().sr_class_priority, 3);
    EXPECT_EQ(wire.device.domain().sr_class_vid, 2);
}

TEST(nanoavb_srp_msrp, receive_short_packet)
{
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};

    // Too short packet should be ignored
    std::array<uint8_t, 2> short_packet = {0, 0};
    handler.receive_packet(short_packet, TimePoint{std::chrono::seconds{1}});  // Should not crash
}

TEST(nanoavb_srp_msrp, withdraw_all_ends_the_run_and_the_next_use_begins_again)
{
    // The first use of a run starts the participant (BEGIN!); withdraw_all
    // ends the run and stops it; the next use starts it again rather than
    // carrying on with the timers of a run that is over.
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}};
    EXPECT_FALSE(handler.started());
    handler.tick(TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(handler.started());
    handler.withdraw_all(TimePoint{std::chrono::seconds{2}});
    EXPECT_FALSE(handler.started());
    handler.tick(TimePoint{std::chrono::seconds{3}});
    EXPECT_TRUE(handler.started());
}

TEST(nanoavb_srp_mvrp, withdraw_all_ends_the_run_and_the_next_use_begins_again)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    EXPECT_FALSE(handler.started());
    (void)handler.register_vlan(2, TimePoint{std::chrono::seconds{1}});
    EXPECT_TRUE(handler.started());
    handler.withdraw_all(TimePoint{std::chrono::seconds{2}});
    EXPECT_FALSE(handler.started());
    handler.tick(TimePoint{std::chrono::seconds{3}});
    EXPECT_TRUE(handler.started());
}

TEST(nanoavb_srp_msrp, withdraw_all_sends_the_leaves_at_once)
{
    size_t sent = 0;
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = [&sent](std::span<uint8_t const>) {
                              ++sent;
                              return true;
                          }}};
    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(7);
    TalkerStreamSrpInfo info;
    info.stream_id = sid;

    // A run: the domain and a talker declared, and sent at the join timer.
    auto now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.declare_domain(now).has_value());
    EXPECT_TRUE(handler.talker_advertise(info, now).has_value());
    now += std::chrono::milliseconds{250};  // past JoinTime (IEEE 802.1Q-2018 Table 10-7)
    handler.tick(now);
    EXPECT_TRUE(sent > 0);

    // Its end: the withdrawals go out in the same call, no tick needed.
    // (A second call would send again - the applicants, observers now, take
    // any transmit opportunity to encode their optional In - which the peer
    // reads as no change.)
    auto const sent_before = sent;
    handler.withdraw_all(now);
    EXPECT_TRUE(sent > sent_before);
    EXPECT_EQ(handler.get_talker_stream(sid)->state, TalkerReservationState::Idle);

    // A handler that never declared anything sends nothing.
    size_t idle_sent = 0;
    MsrpHandler<> idle{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = [&idle_sent](std::span<uint8_t const>) {
                           ++idle_sent;
                           return true;
                       }}};
    idle.withdraw_all(now);
    EXPECT_EQ(idle_sent, 0U);
}

namespace {

/// A Milan listener handler (listener_waits_for_talker) on a direct link with a talker participant.
struct WaitingBench
{
    using ListenerDeclaration = statusbar::srp::msrp::ListenerDeclaration;
    TimePoint now{std::chrono::seconds{1}};
    std::vector<std::vector<uint8_t>> talker_out;
    std::vector<std::vector<uint8_t>> listener_out;
    statusbar::srp::msrp::MsrpParticipant talker{milan_msrp_config(statusbar::srp::msrp::MsrpConfig{}), 0x7a3};
    MsrpHandler<> listener{
        milan_msrp_config(statusbar::srp::msrp::MsrpConfig{}), MsrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) {
            listener_out.emplace_back(p.begin(), p.end());
            return true;
        }}};
    bool talker_alive{true};

    WaitingBench()
    {
        talker.set_send_pdu([this](std::span<uint8_t const> p) {
            talker_out.emplace_back(p.begin(), p.end());
            return true;
        });
        talker.start(now);
        listener.set_listener_waits_for_talker(true);
    }

    static auto stream(uint16_t const unique) -> statusbar::tsn::StreamId
    {
        statusbar::tsn::StreamId sid;
        sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
        sid.set_unique_id(unique);
        return sid;
    }

    /// The talker advertises `sid` (heard by the listener on the next run()).
    void advertise(statusbar::tsn::StreamId const& sid)
    {
        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fv.stream_id = sid;
        fv.vlan_identifier = 2;
        fv.max_frame_size = 416;
        fv.max_interval_frames = 1;
        (void)talker.declare_talker_advertise(fv, now);
    }

    /// `rounds` ticks of 120 ms each, PDUs exchanged; a talker not alive (powered off) sends nothing.
    void run(int const rounds)
    {
        for (int round = 0; round < rounds; ++round) {
            now += std::chrono::milliseconds{120};
            if (talker_alive) {
                talker.tick(now);
            }
            listener.tick(now);
            for (auto const& p : talker_out) {
                listener.receive_packet(p, now);
            }
            talker_out.clear();
            for (auto const& p : listener_out) {
                if (talker_alive) {
                    talker.receive_pdu(p, now);
                }
            }
            listener_out.clear();
        }
    }

    /// What the talker has registered of the listener's declaration for `sid`.
    auto heard(statusbar::tsn::StreamId const& sid) const -> ListenerDeclaration
    {
        auto const* rec = talker.find_listener(sid);
        return rec != nullptr && rec->registrar_is_registered() ? rec->substate : ListenerDeclaration::Ignore;
    }

    auto state(statusbar::tsn::StreamId const& sid) const -> ListenerReservationState
    {
        return listener.get_listener_stream(sid)->state;
    }

    /// A run: domain declared, `sid` attached (Waiting: no talker yet), then stopped with withdraw_all().
    void attach_and_stop(statusbar::tsn::StreamId const& sid)
    {
        EXPECT_TRUE(listener.declare_domain(now).has_value());
        EXPECT_EQ(*listener.listener_attach(sid, now), ListenerReservationState::Waiting);
        run(4);
        listener.withdraw_all(now);
        EXPECT_EQ(state(sid), ListenerReservationState::Waiting);
    }
};

}  // namespace

// A listener attached Waiting (listener_waits_for_talker) stays attached across withdraw_all(): a
// stopped handler declares nothing for a talker heard meanwhile, and the next run (declare_domain)
// forgets the last run's registrations, hears the talker again and declares the Listener.
TEST(nanoavb_srp_msrp, a_waiting_listener_survives_withdraw_all_and_declares_in_the_next_run)
{
    using ListenerDeclaration = WaitingBench::ListenerDeclaration;
    WaitingBench b;
    auto const sid = WaitingBench::stream(11);
    b.attach_and_stop(sid);

    // Stopped: the talker's Advertise heard now declares nothing.
    b.advertise(sid);
    b.run(12);
    EXPECT_TRUE(b.listener.peer_talker_advertise(sid) != nullptr);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Waiting);
    EXPECT_TRUE(b.heard(sid) == ListenerDeclaration::Ignore);

    // The next run: the last run's registrations are forgotten, the talker heard again, the Listener declared.
    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    EXPECT_TRUE(b.listener.peer_talker_advertise(sid) == nullptr);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Waiting);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);
    EXPECT_TRUE(b.heard(sid) == ListenerDeclaration::Ready);
}

// A talker heard while stopped, then powered off before the next run, is not declared for: the run's
// start forgets what the last run registered, and the talker never advertises again.
TEST(nanoavb_srp_msrp, a_talker_gone_while_stopped_is_not_declared_for_at_resume)
{
    WaitingBench b;
    auto const sid = WaitingBench::stream(12);
    b.attach_and_stop(sid);
    b.advertise(sid);
    b.run(12);
    EXPECT_TRUE(b.listener.peer_talker_advertise(sid) != nullptr);
    b.talker_alive = false;

    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    b.run(24);  // past the talker's leave timer and a LeaveAll
    EXPECT_TRUE(b.listener.peer_talker_advertise(sid) == nullptr);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Waiting);
}

// Only declare_domain() begins a run: a listener attached before it is declared, but the last run's
// Waiting listener is followed at declare_domain(), not before.
TEST(nanoavb_srp_msrp, a_run_begun_with_listener_attach_follows_the_waiting_listener_at_declare_domain)
{
    using ListenerDeclaration = WaitingBench::ListenerDeclaration;
    WaitingBench b;
    auto const sid = WaitingBench::stream(13);
    auto const other = WaitingBench::stream(14);
    b.attach_and_stop(sid);
    b.advertise(sid);
    b.run(12);
    EXPECT_TRUE(b.listener.peer_talker_advertise(sid) != nullptr);

    EXPECT_EQ(*b.listener.listener_attach(other, b.now), ListenerReservationState::Waiting);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Waiting);  // not resumed by the attach
    EXPECT_TRUE(b.heard(sid) == ListenerDeclaration::Ignore);
    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);
    EXPECT_TRUE(b.heard(sid) == ListenerDeclaration::Ready);
    EXPECT_EQ(b.state(other), ListenerReservationState::Waiting);
}

// A peer's Domain heard while withdrawn resumes nothing: no VID is adopted and no Listener declared
// until the next declare_domain(), which hears the Domain again and adopts it then.
TEST(nanoavb_srp_msrp, a_peer_domain_while_withdrawn_declares_nothing)
{
    using ListenerDeclaration = WaitingBench::ListenerDeclaration;
    WaitingBench b;
    b.listener.set_adopt_domain(true);  // Milan v1.3 Section 4.2.7.2.1, as atdecc.profile = "milan13"
    auto const sid = WaitingBench::stream(15);
    b.attach_and_stop(sid);
    b.advertise(sid);
    statusbar::srp::msrp::DomainFirstValue domain{};
    domain.sr_class_id = 6;
    domain.sr_class_priority = 3;
    domain.sr_class_vid = 7;
    (void)b.talker.declare_domain(domain, b.now);
    b.run(12);
    EXPECT_EQ(b.listener.domain().sr_class_vid, 2);  // not adopted while withdrawn
    EXPECT_EQ(b.state(sid), ListenerReservationState::Waiting);
    EXPECT_TRUE(b.heard(sid) == ListenerDeclaration::Ignore);

    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    b.run(12);
    EXPECT_EQ(b.listener.domain().sr_class_vid, 7);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);
    EXPECT_TRUE(b.heard(sid) == ListenerDeclaration::Ready);
}

// A talker that turns its Talker Advertise into a Talker Failed sends the Advertise's Lv and the Failed in
// one MRPDU: the listener follows that MRPDU once it is read whole, so its talker is never seen unregistered
// and its Listener goes from Ready to Asking Failed without a withdrawal between.
TEST(nanoavb_srp_msrp, a_talker_advertise_replaced_by_a_talker_failed_in_one_mrpdu_is_one_change)
{
    WaitingBench b;
    auto const sid = WaitingBench::stream(16);
    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    EXPECT_EQ(*b.listener.listener_attach(sid, b.now), ListenerReservationState::Waiting);
    b.advertise(sid);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);

    int unregistered = 0;
    b.listener.set_on_remote_talker([&b, &unregistered](statusbar::tsn::StreamId const& changed, TimePoint) {
        unregistered += b.listener.peer_talker_advertise(changed) == nullptr && b.listener.peer_talker_failed(changed) == nullptr;
    });
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise = *b.listener.peer_talker_advertise(sid);
    failed.failure_code = 1;
    (void)b.talker.withdraw_talker(sid, b.now);
    (void)b.talker.declare_talker_failed(failed, b.now);
    std::vector<ListenerReservationState> states{};
    for (int round = 0; round < 12; ++round) {
        b.run(1);
        states.push_back(b.state(sid));
    }
    EXPECT_EQ(unregistered, 0);
    EXPECT_TRUE(std::ranges::find(states, ListenerReservationState::Waiting) == states.end());
    EXPECT_EQ(b.state(sid), ListenerReservationState::AskingFailed);
}

// The reverse: a Talker Failed replaced by the Advertise in one MRPDU (the Advertise's New first, then the
// Failed's Lv) leaves the listener Ready, once the MRPDU is read, not Asking Failed.
TEST(nanoavb_srp_msrp, a_talker_failed_replaced_by_an_advertise_in_one_mrpdu_is_one_change)
{
    WaitingBench b;
    b.listener.set_instant_leave(true);  // the Failed's Lv ends it at once (Milan v1.3 Section 4.2.7.2.2)
    auto const sid = WaitingBench::stream(26);
    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    EXPECT_EQ(*b.listener.listener_attach(sid, b.now), ListenerReservationState::Waiting);
    b.advertise(sid);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);
    auto const adv = *b.listener.peer_talker_advertise(sid);
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise = adv;
    failed.failure_code = 1;
    (void)b.talker.withdraw_talker(sid, b.now);
    (void)b.talker.declare_talker_failed(failed, b.now);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::AskingFailed);
    (void)b.talker.withdraw_talker(sid, b.now);
    (void)b.talker.declare_talker_advertise(adv, b.now);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);
}

// A listener attached to the destination and VLAN its connection names declares nothing for a Talker
// Advertise of its StreamID on another VLAN, and Ready once the talker declares it on the named one.
TEST(nanoavb_srp_msrp, a_talker_advertise_on_another_vlan_declares_no_listener)
{
    WaitingBench b;
    auto const sid = WaitingBench::stream(17);
    Eui48 const group{0x91, 0xE0, 0xF0, 0x00, 0x01, 0x00};
    statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
    fv.stream_id = sid;
    fv.destination_address = group;
    fv.vlan_identifier = 3;
    fv.max_frame_size = 416;
    fv.max_interval_frames = 1;
    (void)b.talker.declare_talker_advertise(fv, b.now);
    EXPECT_TRUE(b.listener.declare_domain(b.now).has_value());
    b.run(12);
    EXPECT_EQ(
        *b.listener.listener_attach(sid, b.now, TalkerMatch{.destination = group, .vid = 2}), ListenerReservationState::Waiting);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Waiting);
    auto const* const declared = b.talker.find_listener(sid);
    EXPECT_FALSE(declared != nullptr && declared->registrar_is_registered());

    EXPECT_TRUE(b.listener.listener_asking_failed(sid, b.now).has_value());  // declared again: the match stays
    EXPECT_TRUE(b.listener.get_listener_stream(sid)->talker.has_value());

    (void)b.talker.withdraw_talker(sid, b.now);
    fv.vlan_identifier = 2;
    (void)b.talker.declare_talker_advertise(fv, b.now);
    b.run(12);
    EXPECT_EQ(b.state(sid), ListenerReservationState::Ready);
}

TEST(nanoavb_srp_mvrp, withdraw_all_sends_the_leave_at_once)
{
    size_t sent = 0;
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{.send_packet = [&sent](std::span<uint8_t const>) {
                            ++sent;
                            return true;
                        }}};
    auto now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.register_vlan(2, now).has_value());
    now += std::chrono::milliseconds{250};  // past JoinTime (IEEE 802.1Q-2018 Table 10-7)
    handler.tick(now);
    EXPECT_TRUE(sent > 0);

    auto const sent_before = sent;
    handler.withdraw_all(now);
    EXPECT_TRUE(sent > sent_before);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);

    // A handler that never registered anything sends nothing.
    size_t idle_sent = 0;
    MvrpHandler idle{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{.send_packet = [&idle_sent](std::span<uint8_t const>) {
                         ++idle_sent;
                         return true;
                     }}};
    idle.withdraw_all(now);
    EXPECT_EQ(idle_sent, 0U);
}

//
// Milan v1.3 4.4.1: the VLANs of the settled sinks (set_sink_vlans)
//

TEST(nanoavb_srp_mvrp, a_sink_vlan_is_declared_while_it_is_in_the_list)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};
    handler.set_sink_vlans(std::array<uint16_t, 2>{7, 9}, now);
    EXPECT_TRUE(handler.started());
    EXPECT_EQ(handler.get_vlan_state(7), VlanState::Pending);
    EXPECT_EQ(handler.get_vlan_state(9), VlanState::Pending);
    EXPECT_EQ(handler.sink_vlans().size(), 2U);

    handler.set_sink_vlans(std::array<uint16_t, 1>{9}, now);
    EXPECT_EQ(handler.get_vlan_state(7), VlanState::Unregistered);
    EXPECT_EQ(handler.get_vlan_state(9), VlanState::Pending);
    EXPECT_EQ(handler.sink_vlans().size(), 1U);
    EXPECT_EQ(handler.sink_vlans().front(), 9);

    handler.set_sink_vlans({}, now);
    EXPECT_EQ(handler.get_vlan_state(9), VlanState::Unregistered);
    EXPECT_TRUE(handler.sink_vlans().empty());
}

TEST(nanoavb_srp_mvrp, a_sink_vlan_is_joined_and_left_on_the_wire)
{
    // VID 0x123 as the FirstValue of a VID vector: the two octets, in order.
    std::vector<std::vector<uint8_t>> sent{};
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{.send_packet = [&sent](std::span<uint8_t const> pdu) {
                            sent.emplace_back(pdu.begin(), pdu.end());
                            return true;
                        }}};
    auto const names_the_vlan = [](std::vector<uint8_t> const& pdu) {
        std::array<uint8_t, 2> const vid{0x01, 0x23};
        return !std::ranges::search(pdu, vid).empty();
    };
    auto now = TimePoint{std::chrono::seconds{1}};
    handler.set_sink_vlans(std::array<uint16_t, 1>{0x123}, now);
    now += std::chrono::milliseconds{250};
    handler.tick(now);
    EXPECT_FALSE(sent.empty());
    EXPECT_TRUE(std::ranges::any_of(sent, names_the_vlan));

    // Out of the list: its Leave goes at the next opportunity.
    sent.clear();
    handler.set_sink_vlans({}, now);
    now += std::chrono::milliseconds{250};
    handler.tick(now);
    EXPECT_TRUE(std::ranges::any_of(sent, names_the_vlan));
}

TEST(nanoavb_srp_mvrp, the_entitys_vlan_outlasts_a_sink_that_leaves_it)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.register_vlan(2, now).has_value());
    handler.set_sink_vlans(std::array<uint16_t, 2>{2, 7}, now);
    EXPECT_EQ(handler.vlans().size(), 2U);
    handler.set_sink_vlans({}, now);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Pending);
    EXPECT_EQ(handler.get_vlan_state(7), VlanState::Unregistered);

    // Withdrawn by the entity with no sink on it, it goes.
    EXPECT_TRUE(handler.withdraw_vlan(2, now).has_value());
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);
}

TEST(nanoavb_srp_mvrp, a_sinks_vlan_outlasts_the_entitys_withdrawal_of_it)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.register_vlan(2, now).has_value());
    handler.set_sink_vlans(std::array<uint16_t, 1>{2}, now);
    EXPECT_TRUE(handler.withdraw_vlan(2, now).has_value());
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Pending);

    // The entity held it once and let go: the sink's leaving takes it away.
    handler.set_sink_vlans({}, now);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);

    // Registered twice and withdrawn once, the entity holds it no more.
    EXPECT_TRUE(handler.register_vlan(5, now).has_value());
    EXPECT_TRUE(handler.register_vlan(5, now).has_value());
    EXPECT_TRUE(handler.withdraw_vlan(5, now).has_value());
    EXPECT_EQ(handler.get_vlan_state(5), VlanState::Unregistered);
    handler.set_sink_vlans(std::array<uint16_t, 1>{5}, now);
    EXPECT_EQ(handler.get_vlan_state(5), VlanState::Pending);
    handler.set_sink_vlans({}, now);
    EXPECT_EQ(handler.get_vlan_state(5), VlanState::Unregistered);
}

TEST(nanoavb_srp_mvrp, a_repeat_and_an_id_that_is_no_vlan_are_left_out_of_the_sinks_list)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};
    handler.set_sink_vlans(std::array<uint16_t, 5>{7, 0, 7, 4095, 4094}, now);
    EXPECT_EQ(handler.sink_vlans().size(), 2U);
    EXPECT_EQ(handler.sink_vlans()[0], 7);
    EXPECT_EQ(handler.sink_vlans()[1], 4094);
    EXPECT_EQ(handler.vlans().size(), 2U);
}

TEST(nanoavb_srp_mvrp, a_sink_vlan_the_table_has_no_room_for_is_taken_once_there_is_room)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};
    for (uint16_t vid = 1; vid <= 16; ++vid) {
        EXPECT_TRUE(handler.register_vlan(vid, now).has_value());
    }
    handler.set_sink_vlans(std::array<uint16_t, 2>{17, 3}, now);
    EXPECT_EQ(handler.sink_vlans().size(), 1U);  // 3 is declared already; 17 has no slot
    EXPECT_EQ(handler.get_vlan_state(17), VlanState::Unregistered);

    EXPECT_TRUE(handler.withdraw_vlan(8, now).has_value());
    handler.set_sink_vlans(std::array<uint16_t, 2>{17, 3}, now);
    EXPECT_EQ(handler.sink_vlans().size(), 2U);
    EXPECT_EQ(handler.get_vlan_state(17), VlanState::Pending);
    EXPECT_EQ(handler.get_vlan_state(3), VlanState::Pending);
}

TEST(nanoavb_srp_mvrp, withdraw_all_leaves_nobody_holding_a_vlan)
{
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}};
    auto const now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.register_vlan(2, now).has_value());
    handler.set_sink_vlans(std::array<uint16_t, 1>{7}, now);
    handler.withdraw_all(now);
    EXPECT_TRUE(handler.sink_vlans().empty());
    EXPECT_EQ(handler.get_vlan_state(7), VlanState::Unregistered);

    // The next run: the sinks' list is declared afresh, and the entity's
    // hold of VLAN 2 is gone with the run it was made in.
    handler.set_sink_vlans(std::array<uint16_t, 2>{7, 2}, now);
    EXPECT_EQ(handler.get_vlan_state(7), VlanState::Pending);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Pending);
    handler.set_sink_vlans({}, now);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);
}

//
// Sender lifetime: one trampoline to the callbacks in force
//

//
// Milan v1.3 4.3.3.1: a talker that declares its attribute only while a
// listener wants the stream (set_talker_waits_for_listener)
//
namespace {

/// A talker's MsrpHandler that waits for a listener, and the bridge port it
/// is plugged into as an MSRP participant: what each sends reaches the other
/// on the next run() round. The bridge can declare the stream's Listener
/// toward the talker, as it does once a listener behind it has declared one.
struct TalkerLink
{
    TimePoint now{std::chrono::seconds{1}};
    std::vector<std::vector<uint8_t>> talker_out{};
    std::vector<std::vector<uint8_t>> bridge_out{};
    statusbar::tsn::StreamId sid{Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 3};
    statusbar::srp::msrp::MsrpParticipant bridge{statusbar::srp::msrp::MsrpConfig{}, 0x7a12};
    MsrpHandler<> talker{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) {
                             talker_out.emplace_back(p.begin(), p.end());
                             return true;
                         }}};

    TalkerLink()
    {
        bridge.set_send_pdu([this](std::span<uint8_t const> p) {
            bridge_out.emplace_back(p.begin(), p.end());
            return true;
        });
        bridge.start(now);
        talker.set_talker_waits_for_listener(true);
    }

    void run(int const rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            now += std::chrono::milliseconds{125};
            bridge.tick(now);
            talker.tick(now);
            deliver();
        }
    }

    void deliver()
    {
        for (auto const& p : std::exchange(bridge_out, {})) {
            talker.receive_packet(p, now);
        }
        for (auto const& p : std::exchange(talker_out, {})) {
            bridge.receive_pdu(p, now);
        }
    }

    /// The stream's parameters, with @p dest_last as the last octet of the destination address.
    [[nodiscard]] auto info(uint8_t const dest_last = 0x34) const -> TalkerStreamSrpInfo
    {
        TalkerStreamSrpInfo out{};
        out.stream_id = sid;
        out.dest_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, dest_last};
        out.max_frame_size = 416;
        out.max_interval_frames = 1;
        return out;
    }

    [[nodiscard]] auto state() const -> TalkerReservationState { return talker.get_talker_stream(sid)->state; }
    /// Whether the bridge holds the talker's attribute registered.
    [[nodiscard]] auto registered() const -> bool { return bridge.find_registered_talker_advertise(sid) != nullptr; }
    [[nodiscard]] auto seconds() const -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    }
};

constexpr int ROUNDS_PER_SECOND = 8;

}  // namespace

// Handed a stream's parameters, the talker declares nothing: no listener has
// asked. The bridge never learns of the stream, however long it waits.
TEST(nanoavb_srp_msrp, a_talker_that_waits_for_a_listener_declares_nothing_unasked)
{
    TalkerLink link{};
    auto const result = link.talker.talker_advertise(link.info(), link.now);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(link.state(), TalkerReservationState::Waiting);
    link.run(40 * ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Waiting);
    EXPECT_FALSE(link.registered());
}

// A PROBE_TX_COMMAND makes the stream's destination address valid for 15
// seconds: the attribute is declared at once, and withdrawn when the 15
// seconds have passed with no Listener registered.
TEST(nanoavb_srp_msrp, a_probe_declares_the_talker_for_fifteen_seconds)
{
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(), link.now);
    link.run(2 * ROUNDS_PER_SECOND);
    auto const probed_at = link.now;

    link.talker.note_talker_probe(link.sid, link.now);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    link.run(ROUNDS_PER_SECOND);
    EXPECT_TRUE(link.registered());

    link.run(14 * ROUNDS_PER_SECOND - 1);  // an eighth of a second short of the fifteen
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    EXPECT_TRUE(link.registered());
    link.run(1);
    EXPECT_EQ(link.now - probed_at, std::chrono::seconds{15});
    EXPECT_EQ(link.state(), TalkerReservationState::Waiting);
    link.run(2 * ROUNDS_PER_SECOND);  // the Lv, and the bridge's LeaveTime
    EXPECT_FALSE(link.registered());
}

// A second probe inside the 15 seconds starts them again.
TEST(nanoavb_srp_msrp, each_probe_gives_the_talker_fifteen_seconds_more)
{
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(), link.now);
    link.talker.note_talker_probe(link.sid, link.now);
    link.run(10 * ROUNDS_PER_SECOND);
    link.talker.note_talker_probe(link.sid, link.now);
    link.run(14 * ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    link.run(ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Waiting);
}

// Once the port registers a Listener attribute for the stream the probe no
// longer matters: the attribute stays declared for as long as the Listener
// does, and is withdrawn when the Listener has gone.
TEST(nanoavb_srp_msrp, a_registered_listener_keeps_the_talker_declared_and_its_leaving_withdraws_it)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(), link.now);
    link.talker.note_talker_probe(link.sid, link.now);
    link.run(ROUNDS_PER_SECOND);
    EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::Ready, link.now).has_value());

    link.run(60 * ROUNDS_PER_SECOND);  // four times the probe's life, and LeaveAlls from both ends
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    EXPECT_TRUE(link.registered());

    EXPECT_TRUE(link.bridge.withdraw_listener(link.sid, link.now).has_value());
    link.run(3 * ROUNDS_PER_SECOND);  // the Lv, the talker's LeaveTime, then its own Lv and the bridge's LeaveTime
    EXPECT_EQ(link.state(), TalkerReservationState::Waiting);
    EXPECT_FALSE(link.registered());
}

// A Listener attribute of any Declaration Type counts, and no probe need
// have come first: a talker back from a restart finds the bridge still
// declaring the Listener of a stream it was sending, and declares it again.
TEST(nanoavb_srp_msrp, a_listener_already_registered_declares_the_talker_without_a_probe)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    TalkerLink link{};
    EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::AskingFailed, link.now).has_value());
    link.run(ROUNDS_PER_SECOND);

    (void)link.talker.talker_advertise(link.info(), link.now);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    link.run(ROUNDS_PER_SECOND);
    EXPECT_TRUE(link.registered());
}

// A probe that arrives before the entity has handed the stream over (gPTP
// not yet locked, say) is remembered: the stream is declared when it is
// handed over inside the 15 seconds, and not when it comes later.
TEST(nanoavb_srp_msrp, a_probe_ahead_of_the_streams_parameters_counts_for_its_fifteen_seconds)
{
    TalkerLink early{};
    early.talker.note_talker_probe(early.sid, early.now);
    early.run(5 * ROUNDS_PER_SECOND);
    (void)early.talker.talker_advertise(early.info(), early.now);
    EXPECT_EQ(early.state(), TalkerReservationState::Advertising);
    early.run(10 * ROUNDS_PER_SECOND);
    EXPECT_EQ(early.state(), TalkerReservationState::Waiting);

    TalkerLink late{};
    late.talker.note_talker_probe(late.sid, late.now);
    late.run(16 * ROUNDS_PER_SECOND);
    (void)late.talker.talker_advertise(late.info(), late.now);
    EXPECT_EQ(late.state(), TalkerReservationState::Waiting);
}

// Condition 1 of 4.3.3.1: an address MAAP reports in conflict is not valid.
// The attribute is withdrawn at the next tick though a Listener is
// registered, and the address that replaces it is declared two LeaveAll
// periods after the withdrawal (IEEE 802.1Q 35.2.2.8), as Milan's Table 5.3
// describes for a talker whose MAAP address is lost.
TEST(nanoavb_srp_msrp, an_address_in_conflict_withdraws_the_talker_and_its_replacement_waits_two_leaveall_periods)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(0x34), link.now);
    EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::Ready, link.now).has_value());
    link.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);

    link.talker.set_talker_addresses_valid(false);
    link.run(1);
    auto const withdrawn_at = link.now;
    EXPECT_EQ(link.state(), TalkerReservationState::Waiting);

    link.run(2 * ROUNDS_PER_SECOND);  // MAAP has another address
    link.talker.set_talker_addresses_valid(true);
    (void)link.talker.talker_advertise(link.info(0x35), link.now);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);  // wanted, and held back by the participant
    EXPECT_FALSE(link.registered());

    while (link.now - withdrawn_at < std::chrono::milliseconds{29'875}) {
        link.run(1);
        EXPECT_FALSE(link.registered());
    }
    link.run(16 * ROUNDS_PER_SECOND);  // the first PDU after the thirty seconds carries it
    EXPECT_TRUE(link.registered());
    EXPECT_EQ(link.bridge.find_registered_talker_advertise(link.sid)->destination_address.to_uint64() & 0xFFU, 0x35U);
}

// A talker that comes back from a restart with another destination address
// under its StreamID, as one does whose MAAP block changed: told what the old
// run declared, the handler holds the Talker Advertise back for two LeaveAll
// periods from the start (IEEE 802.1Q 35.2.2.8), talker_held() says so, and
// the attribute then reaches the bridge with the new address.
TEST(nanoavb_srp_msrp, a_talker_restarted_with_another_address_holds_its_advertise_two_leaveall_periods)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    TalkerLink link{};
    auto const started_at = link.now;
    EXPECT_TRUE(link.talker.talker_hold_after_restart(link.info(0x34), link.now).has_value());
    EXPECT_FALSE(link.talker.talker_held(link.sid));  // nothing asked for yet
    (void)link.talker.talker_advertise(link.info(0x35), link.now);
    EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::Ready, link.now).has_value());
    link.run(2 * ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);  // wanted,
    EXPECT_TRUE(link.talker.talker_held(link.sid));                // and held back
    EXPECT_FALSE(link.registered());

    while (link.now - started_at < std::chrono::milliseconds{29'875}) {
        link.run(1);
        EXPECT_FALSE(link.registered());
    }
    link.run(16 * ROUNDS_PER_SECOND);  // the first PDU after the thirty seconds carries it
    EXPECT_TRUE(link.registered());
    EXPECT_FALSE(link.talker.talker_held(link.sid));
    EXPECT_EQ(link.bridge.find_registered_talker_advertise(link.sid)->destination_address.to_uint64() & 0xFFU, 0x35U);
}

// With the parameters the old run declared nothing waits: the bridge has the
// attribute within the second. A changed AccumulatedLatency is no change of
// the attribute (35.2.2.8.6) and waits no more.
TEST(nanoavb_srp_msrp, a_talker_restarted_with_the_same_parameters_is_declared_at_once)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    for (uint32_t const latency : {0U, 2'000'000U}) {
        TalkerLink link{};
        auto previous = link.info(0x34);
        previous.accumulated_latency = latency;
        EXPECT_TRUE(link.talker.talker_hold_after_restart(previous, link.now).has_value());
        (void)link.talker.talker_advertise(link.info(0x34), link.now);
        EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::Ready, link.now).has_value());
        link.run(ROUNDS_PER_SECOND);
        EXPECT_FALSE(link.talker.talker_held(link.sid));
        EXPECT_TRUE(link.registered());
    }
}

// A stream the entity has withdrawn (a Link Down) is not the handler's to
// declare: a probe leaves it Idle until the entity advertises it again.
TEST(nanoavb_srp_msrp, a_probe_does_not_declare_a_stream_the_entity_withdrew)
{
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(), link.now);
    EXPECT_TRUE(link.talker.talker_withdraw(link.sid, link.now).has_value());
    link.talker.note_talker_probe(link.sid, link.now);
    link.run(ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Idle);
    EXPECT_FALSE(link.registered());

    (void)link.talker.talker_advertise(link.info(), link.now);  // the probe is a second old: still good
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
}

// The loop that drives the handler must wake when a probe runs out, or the
// attribute it alone held up stays declared until something else wakes it.
TEST(nanoavb_srp_msrp, the_handlers_deadline_is_no_later_than_a_probes_end)
{
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(), link.now);
    link.talker.note_talker_probe(link.sid, link.now);
    auto const probe_end =
        std::chrono::duration_cast<std::chrono::nanoseconds>((link.now + std::chrono::seconds{15}).time_since_epoch());
    auto const deadline = link.talker.next_deadline_ns();
    EXPECT_TRUE(deadline.has_value() && *deadline <= probe_end.count());

    // With every probe run out, a deadline is never left standing in the past.
    link.run(20 * ROUNDS_PER_SECOND);
    auto const after = link.talker.next_deadline_ns();
    auto const now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(link.now.time_since_epoch()).count();
    EXPECT_TRUE(!after.has_value() || *after > now_ns);
}

// A handler that does not wait (IEEE 1722.1's way, the default) is not
// moved by a probe or by the address flag: it declares what it is handed.
TEST(nanoavb_srp_msrp, a_talker_that_does_not_wait_declares_at_once_and_ignores_probes)
{
    TalkerLink link{};
    link.talker.set_talker_waits_for_listener(false);
    link.talker.set_talker_addresses_valid(false);
    (void)link.talker.talker_advertise(link.info(), link.now);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    link.talker.note_talker_probe(link.sid, link.now);
    link.run(40 * ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    EXPECT_TRUE(link.registered());
}

namespace {

/// A MSRPDU carrying one Domain vector with the LeaveAll flag and no
/// values: what a bridge sends to make everyone re-declare. The handler
/// answers one synchronously (see MsrpParticipant::receive_pdu).
constexpr std::array<uint8_t, 15> MSRP_LEAVEALL_PDU{
    0x00,  // ProtocolVersion
    0x04,
    0x04,  // Domain, AttributeLength 4
    0x00,
    0x08,  // AttributeListLength: vector header + FirstValue + EndMark
    0x20,
    0x00,  // VectorHeader: LeaveAll, NumberOfValues 0
    0x06,
    0x03,
    0x00,
    0x02,  // FirstValue: class A, priority 3, VID 2
    0x00,
    0x00,  // EndMark (list)
    0x00,
    0x00,  // EndMark (PDU)
};

/// A counting sender whose count lives behind a shared_ptr, so a test can
/// tell from a weak_ptr whether the handler still holds it.
auto counting_sender(std::shared_ptr<size_t> const& count)
{
    return [count](std::span<uint8_t const>) {
        ++*count;
        return true;
    };
}

}  // namespace

TEST(nanoavb_srp_msrp, a_pdu_goes_to_the_sender_in_force_even_before_a_tick)
{
    auto const first = std::make_shared<size_t>(0);
    auto const second = std::make_shared<size_t>(0);
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = counting_sender(first)}};
    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(7);
    TalkerStreamSrpInfo info;
    info.stream_id = sid;

    // A run on the first sender: declared, and sent at the join timer.
    auto now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.declare_domain(now).has_value());
    EXPECT_TRUE(handler.talker_advertise(info, now).has_value());
    now += std::chrono::milliseconds{250};
    handler.tick(now);
    EXPECT_TRUE(*first > 0);

    // The network path is replaced (a restart) and, before any tick, the
    // bridge's LeaveAll arrives: the re-declaration it asks for goes to the
    // sender now in force, never to the one the last run had.
    size_t const first_before = *first;
    handler.set_callbacks(MsrpCallbacks{.send_packet = counting_sender(second)});
    now += std::chrono::milliseconds{10};
    handler.receive_packet(MSRP_LEAVEALL_PDU, now);
    EXPECT_EQ(*first, first_before);
    EXPECT_TRUE(*second > 0);
}

TEST(nanoavb_srp_msrp, a_dropped_sender_is_let_go_and_nothing_is_sent_after_it)
{
    auto count = std::make_shared<size_t>(0);
    std::weak_ptr<size_t> const alive = count;
    MsrpHandler<> handler{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{.send_packet = counting_sender(count)}};
    statusbar::tsn::StreamId sid;
    sid.set_system_address(Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05});
    sid.set_unique_id(8);
    TalkerStreamSrpInfo info;
    info.stream_id = sid;
    auto now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.declare_domain(now).has_value());
    EXPECT_TRUE(handler.talker_advertise(info, now).has_value());
    now += std::chrono::milliseconds{250};
    handler.tick(now);
    EXPECT_TRUE(*count > 0);

    // Dropped: the handler holds no copy anywhere, so the test's own
    // reference is the last and the count dies with it.
    handler.drop_sender();
    count.reset();
    EXPECT_TRUE(alive.expired());

    // What would have sent -- a LeaveAll's re-declaration, a Leave at the
    // end of the run -- now has nowhere to go and goes nowhere.
    now += std::chrono::milliseconds{10};
    handler.receive_packet(MSRP_LEAVEALL_PDU, now);
    handler.withdraw_all(now);
    EXPECT_EQ(handler.get_talker_stream(sid)->state, TalkerReservationState::Idle);
}

TEST(nanoavb_srp_mvrp, a_pdu_goes_to_the_sender_in_force_even_before_a_tick)
{
    auto const first = std::make_shared<size_t>(0);
    auto const second = std::make_shared<size_t>(0);
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{.send_packet = counting_sender(first)}};
    auto now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.register_vlan(2, now).has_value());
    now += std::chrono::milliseconds{250};
    handler.tick(now);
    EXPECT_TRUE(*first > 0);

    // The path replaced and, with no tick in between, the run ends: the
    // Leave goes to the sender in force.
    size_t const first_before = *first;
    handler.set_callbacks(MvrpCallbacks{.send_packet = counting_sender(second)});
    handler.withdraw_all(now);
    EXPECT_EQ(*first, first_before);
    EXPECT_TRUE(*second > 0);
}

TEST(nanoavb_srp_mvrp, a_dropped_sender_is_let_go_and_nothing_is_sent_after_it)
{
    auto count = std::make_shared<size_t>(0);
    std::weak_ptr<size_t> const alive = count;
    MvrpHandler handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{.send_packet = counting_sender(count)}};
    auto now = TimePoint{std::chrono::seconds{1}};
    EXPECT_TRUE(handler.register_vlan(2, now).has_value());
    now += std::chrono::milliseconds{250};
    handler.tick(now);
    EXPECT_TRUE(*count > 0);

    handler.drop_sender();
    count.reset();
    EXPECT_TRUE(alive.expired());
    handler.withdraw_all(now);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);
}

namespace {

/// A Talker Advertise a bridge passes on for a stream of somebody else's.
auto remote_advertise(statusbar::tsn::StreamId const& sid, uint8_t const dest_last, uint16_t const vlan)
    -> statusbar::srp::msrp::TalkerAdvertiseFirstValue
{
    statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
    fv.stream_id = sid;
    fv.destination_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, dest_last};
    fv.vlan_identifier = vlan;
    fv.max_frame_size = 416;
    fv.max_interval_frames = 1;
    return fv;
}

}  // namespace

// A Milan sink compares the Talker attribute this port registers with the
// parameters it probed (Milan v1.3 5.3.8.8): the handler says which
// attribute stands, with its address and VLAN, and tells of every change,
// attached listener or not.
TEST(nanoavb_srp_msrp, the_remote_talker_registered_for_a_stream_is_reported_with_its_address_and_vlan)
{
    TalkerLink link{};
    statusbar::tsn::StreamId const remote{Eui48{0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e}, 1};
    std::vector<statusbar::tsn::StreamId> told{};
    std::optional<RegisteredTalker> seen_in_callback{};
    link.talker.set_on_remote_talker([&](StreamId const& sid, TimePoint) {
        told.push_back(sid);
        seen_in_callback = link.talker.registered_talker(sid);
    });
    EXPECT_FALSE(link.talker.registered_talker(remote).has_value());

    EXPECT_TRUE(link.bridge.declare_talker_advertise(remote_advertise(remote, 0x77, 5), link.now).has_value());
    link.run(ROUNDS_PER_SECOND);
    EXPECT_FALSE(told.empty());
    EXPECT_TRUE(told.front() == remote);
    EXPECT_TRUE(seen_in_callback.has_value());  // the callback reads what stands now
    auto const registered = link.talker.registered_talker(remote);
    EXPECT_TRUE(registered.has_value());
    EXPECT_EQ(registered->dest_address, (Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x77}));
    EXPECT_EQ(registered->vlan_id, 5);
    EXPECT_FALSE(registered->failed);
    // remote_advertise() gives no latency: one that does is reported with it, and with no failure.
    auto later = remote_advertise(remote, 0x77, 5);
    later.accumulated_latency = 123'456;
    EXPECT_TRUE(link.bridge.withdraw_talker(remote, link.now).has_value());
    link.run(2 * ROUNDS_PER_SECOND);
    EXPECT_TRUE(link.bridge.declare_talker_advertise(later, link.now).has_value());
    link.run(ROUNDS_PER_SECOND);
    auto const with_latency = link.talker.registered_talker(remote);
    EXPECT_TRUE(with_latency.has_value());
    EXPECT_EQ(with_latency->accumulated_latency, 123'456U);
    EXPECT_EQ(with_latency->failure_code, 0);
    EXPECT_EQ(with_latency->failure_bridge_id, Eui64{});

    told.clear();
    EXPECT_TRUE(link.bridge.withdraw_talker(remote, link.now).has_value());
    link.run(2 * ROUNDS_PER_SECOND);
    EXPECT_FALSE(told.empty());
    EXPECT_FALSE(seen_in_callback.has_value());
    EXPECT_FALSE(link.talker.registered_talker(remote).has_value());
}

TEST(nanoavb_srp_msrp, a_registered_talker_failed_is_reported_as_failed)
{
    TalkerLink link{};
    statusbar::tsn::StreamId const remote{Eui48{0x00, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e}, 1};
    statusbar::srp::msrp::TalkerFailedFirstValue failed{};
    failed.advertise = remote_advertise(remote, 0x78, 2);
    failed.advertise.accumulated_latency = 2'000'125;
    failed.failure_bridge_id = Eui64{0x80, 0x00, 0x00, 0x1c, 0xab, 0x00, 0x00, 0x09};
    failed.set_failure_code(statusbar::srp::msrp::FailureCode::InsufficientBandwidth);
    EXPECT_TRUE(link.bridge.declare_talker_failed(failed, link.now).has_value());
    link.run(ROUNDS_PER_SECOND);
    auto const registered = link.talker.registered_talker(remote);
    EXPECT_TRUE(registered.has_value());
    EXPECT_TRUE(registered->failed);
    EXPECT_EQ(registered->dest_address, (Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x78}));
    EXPECT_EQ(registered->vlan_id, 2);
    // The attribute's latency, and its FailureInformation (IEEE 802.1Q-2022 35.2.2.8.7).
    EXPECT_EQ(registered->accumulated_latency, 2'000'125U);
    EXPECT_EQ(registered->failure_code, 1);
    EXPECT_EQ(registered->failure_bridge_id, (Eui64{0x80, 0x00, 0x00, 0x1c, 0xab, 0x00, 0x00, 0x09}));
}

// REGISTERING_FAILED in a Milan GET_TX_STATE_RESPONSE (v1.3 Table 5.23):
// the source declares its Talker attribute and registers a Listener Asking
// Failed for it.
TEST(nanoavb_srp_msrp, a_declared_talker_with_a_listener_asking_failed_registered_says_so)
{
    using statusbar::srp::msrp::ListenerDeclaration;
    TalkerLink link{};
    (void)link.talker.talker_advertise(link.info(), link.now);
    EXPECT_FALSE(link.talker.talker_listener_asking_failed(link.sid));  // nothing declared, nothing registered

    link.talker.note_talker_probe(link.sid, link.now);
    link.run(ROUNDS_PER_SECOND);
    EXPECT_EQ(link.state(), TalkerReservationState::Advertising);
    EXPECT_FALSE(link.talker.talker_listener_asking_failed(link.sid));  // declared, no listener yet

    EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::AskingFailed, link.now).has_value());
    link.run(ROUNDS_PER_SECOND);
    EXPECT_TRUE(link.talker.talker_listener_asking_failed(link.sid));

    EXPECT_TRUE(link.bridge.declare_listener(link.sid, ListenerDeclaration::Ready, link.now).has_value());
    link.run(ROUNDS_PER_SECOND);
    EXPECT_FALSE(link.talker.talker_listener_asking_failed(link.sid));
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