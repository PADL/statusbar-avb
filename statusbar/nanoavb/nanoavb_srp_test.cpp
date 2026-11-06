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
#include <memory>
#include <optional>
#include <span>
#include <system_error>
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
    int hooks = 0;
    listener.set_on_remote_talker([&](StreamId const& s) {
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
    // The hook is told the time of the receive_packet()/tick() it fired from.
    EXPECT_TRUE(change_times == event_times);

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
// Sender lifetime: one trampoline to the callbacks in force
//

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
    now += std::chrono::milliseconds{150};
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
    now += std::chrono::milliseconds{150};
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
    now += std::chrono::milliseconds{150};
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
    now += std::chrono::milliseconds{150};
    handler.tick(now);
    EXPECT_TRUE(*count > 0);

    handler.drop_sender();
    count.reset();
    EXPECT_TRUE(alive.expired());
    handler.withdraw_all(now);
    EXPECT_EQ(handler.get_vlan_state(2), VlanState::Unregistered);
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