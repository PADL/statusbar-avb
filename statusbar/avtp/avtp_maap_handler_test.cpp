// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for MaapHandler — the driver/facade over the MAAP state machine:
// acquire -> probe -> defend, the acquired/lost callbacks, and conflict handling.

#include "statusbar/avtp/avtp_maap_handler.hpp"

#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using statusbar::avtp::maap_block_address;
using statusbar::avtp::MAAP_DYNAMIC_POOL_END;
using statusbar::avtp::MAAP_DYNAMIC_POOL_START;
using statusbar::avtp::MaapDu;
using statusbar::avtp::MaapHandler;
using statusbar::ieee::Eui48;
using statusbar::ieee::protocols::MAAP_MULTICAST_MAC;
using statusbar::tsn::StreamId;

namespace {

constexpr int64_t SEC = 1'000'000'000;  // 1s in ns; > any probe/announce interval, so each tick fires its timer

/// Captures sends + acquired/lost callbacks from one MaapHandler.
struct Rig
{
    MaapHandler handler;
    std::vector<MaapDu> sent;
    std::vector<Eui48> sent_to;  ///< the destination of each entry of `sent`
    std::optional<Eui48> acquired_start;
    uint16_t acquired_count{0};
    std::optional<Eui48> lost_start;
    int acquired_calls{0};
    int lost_calls{0};

    explicit Rig(Eui48 mac, uint64_t seed = 1)
        : handler{mac, seed}
    {
        handler.set_send([this](Eui48 const& dest, std::span<uint8_t const> bytes) {
            MaapDu du{};
            if (statusbar::protocol::load(bytes, &du) != 0) {
                sent.push_back(du);
                sent_to.push_back(dest);
            }
            return true;
        });
        handler.set_on_acquired([this](Eui48 const& s, uint16_t c, int64_t /*now_ns*/) {
            acquired_start = s;
            acquired_count = c;
            ++acquired_calls;
        });
        handler.set_on_lost([this](Eui48 const& s, uint16_t /*c*/, int64_t /*now_ns*/) {
            lost_start = s;
            ++lost_calls;
        });
    }

    /// Tick repeatedly (advancing well past each timer) until acquired or a cap,
    /// the ticks a second apart from @p from_ns on.
    void run_until_acquired(int64_t const from_ns = SEC)
    {
        for (int k = 1; k <= 12 && !handler.is_acquired(); ++k) {
            handler.tick(from_ns + static_cast<int64_t>(k) * SEC);
        }
    }

    /// Tick exactly at the next armed deadline; the time ticked at.
    auto tick_at_deadline() -> int64_t
    {
        auto const deadline = handler.next_deadline_ns();
        EXPECT_TRUE(deadline.has_value());
        handler.tick(deadline.value_or(0));
        return deadline.value_or(0);
    }
};

/// Serialize a MAAP PDU of the given message type for an address range, as it
/// would arrive on the wire from another station.
auto make_frame(uint8_t message_type, Eui48 const& start, uint16_t count) -> std::array<uint8_t, MaapDu::LENGTH>
{
    MaapDu du{};
    switch (message_type) {
        case statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE:
            du.init_probe(StreamId{}, start, count);
            break;
        case statusbar::avtp::MAAP_MESSAGE_TYPE_DEFEND:
            du.init_defend(StreamId{}, start, count, Eui48{}, 0);
            break;
        default:
            du.init_announce(StreamId{}, start, count);
            break;
    }
    std::array<uint8_t, MaapDu::LENGTH> buf{};
    (void)statusbar::protocol::store_unchecked(buf, du);
    return buf;
}

[[nodiscard]] auto in_dynamic_pool(Eui48 const& a, uint16_t count) -> bool
{
    uint64_t const v = a.to_uint64();
    return v >= MAAP_DYNAMIC_POOL_START.to_uint64() && (v + count - 1) <= MAAP_DYNAMIC_POOL_END.to_uint64();
}

}  // namespace

TEST(maap_handler, acquire_probes_then_defends_and_fires_acquired)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);

    // First PROBE sent immediately; address is inside the dynamic pool.
    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_EQ(rig.sent.size(), size_t{1});
    EXPECT_TRUE(rig.sent[0].is_probe());
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 2));

    rig.run_until_acquired();

    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.acquired_calls, 1);
    EXPECT_TRUE(rig.acquired_start.has_value());
    EXPECT_EQ(*rig.acquired_start, rig.handler.address());
    EXPECT_EQ(rig.acquired_count, static_cast<uint16_t>(2));
    // At least one ANNOUNCE was sent on entering Defend.
    bool announced = false;
    for (auto const& du : rig.sent) {
        if (du.is_announce()) {
            announced = true;
        }
    }
    EXPECT_TRUE(announced);
}

TEST(maap_handler, release_returns_to_initial)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(1, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());

    rig.handler.release(100 * SEC);
    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Initial);
    EXPECT_FALSE(rig.handler.is_acquired());
}

TEST(maap_handler, conflicting_probe_while_probing_we_lose_repicks_address)
{
    // Our MAC is high so we LOSE the reverse-octet tie-break against a low sender.
    Rig rig{Eui48{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    rig.handler.acquire(2, SEC);
    Eui48 const first = rig.handler.address();

    // A conflicting PROBE for the exact same range, from a low-MAC station.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, first, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x01}, frame, 2 * SEC);

    // We yielded: still probing, but on a freshly-picked (different) address.
    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_NE(rig.handler.address(), first);
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 2));
}

TEST(maap_handler, conflicting_probe_while_probing_we_win_keeps_address)
{
    // Our MAC is low so we WIN the tie-break; the conflicting probe is ignored.
    Rig rig{Eui48{0x00, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    Eui48 const first = rig.handler.address();

    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, first, 2);
    rig.handler.receive(Eui48{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, frame, 2 * SEC);

    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_EQ(rig.handler.address(), first);  // unchanged
}

TEST(maap_handler, probe_against_us_while_defending_sends_defend)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    Eui48 const addr = rig.handler.address();
    auto const sent_before = rig.sent.size();

    // Someone probes our defended range — we answer with a DEFEND, stay defending.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, addr, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 100 * SEC);

    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.lost_calls, 0);
    EXPECT_TRUE(rig.sent.size() > sent_before);
    EXPECT_TRUE(rig.sent.back().is_defend());
}

// Interop: a conflicting PROBE that carries maap_version 0 (a peer, or one of our
// own not-yet-updated pre-fix nodes) must still be honored -- we defend our range.
// Rejecting on version would let such a station steal our multicast MAC.
TEST(maap_handler, version_zero_probe_against_us_still_defended)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    Eui48 const addr = rig.handler.address();
    auto const sent_before = rig.sent.size();

    // Build a PROBE for our range but stamp maap_version 0 on the wire.
    MaapDu du{};
    du.init_probe(StreamId{}, addr, 2);
    du.set_maap_version(0);
    std::array<uint8_t, MaapDu::LENGTH> frame{};
    (void)statusbar::protocol::store_unchecked(frame, du);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 100 * SEC);

    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_TRUE(rig.sent.size() > sent_before);
    EXPECT_TRUE(rig.sent.back().is_defend());
}

// Table B.7 DEFEND column, rAnnounce/rDefend: compare_MAC runs first (note d).
// Our MAC {02:..:01} loses the octet-wise reverse comparison (B.3.6.4) to a
// sender ending in 00, so the conflicting ANNOUNCE makes us yield.
TEST(maap_handler, announce_from_a_lower_mac_while_defending_fires_lost_and_repicks)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    Eui48 const old_addr = rig.handler.address();

    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_ANNOUNCE, old_addr, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x00}, frame, 100 * SEC);

    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_EQ(rig.lost_calls, 1);
    EXPECT_TRUE(rig.lost_start.has_value());
    EXPECT_EQ(*rig.lost_start, old_addr);        // reported the address we left
    EXPECT_NE(rig.handler.address(), old_addr);  // re-picked a new one
}

TEST(maap_handler, defend_from_a_lower_mac_while_defending_fires_lost_and_repicks)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    Eui48 const old_addr = rig.handler.address();

    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_DEFEND, old_addr, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x00}, frame, 100 * SEC);

    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_EQ(rig.lost_calls, 1);
    EXPECT_NE(rig.handler.address(), old_addr);
}

// The other direction of note d: when compare_MAC is TRUE (our MAC is the lower),
// "no further processing or protocol action is taken and the protocol state does
// not change". A higher MAC cannot evict a defender with an ANNOUNCE or a DEFEND.
TEST(maap_handler, announce_from_a_higher_mac_while_defending_is_ignored)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    Eui48 const addr = rig.handler.address();
    auto const sent_before = rig.sent.size();

    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_ANNOUNCE, addr, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 100 * SEC);

    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.handler.address(), addr);
    EXPECT_EQ(rig.lost_calls, 0);
    EXPECT_EQ(rig.sent.size(), sent_before);  // no protocol action at all
}

TEST(maap_handler, defend_from_a_higher_mac_while_defending_is_ignored)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    Eui48 const addr = rig.handler.address();
    auto const sent_before = rig.sent.size();

    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_DEFEND, addr, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 100 * SEC);

    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.handler.address(), addr);
    EXPECT_EQ(rig.lost_calls, 0);
    EXPECT_EQ(rig.sent.size(), sent_before);
}

// Table B.7 PROBE column: rDefend/rAnnounce have no compare_MAC. A prober yields
// to a defender or announcer whatever the MACs say.
TEST(maap_handler, announce_from_a_higher_mac_while_probing_still_repicks)
{
    Rig rig{Eui48{0x00, 0, 0, 0, 0, 0x01}};  // we would win a comparison
    rig.handler.acquire(2, SEC);
    Eui48 const first = rig.handler.address();

    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_ANNOUNCE, first, 2);
    rig.handler.receive(Eui48{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, frame, 2 * SEC);

    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_NE(rig.handler.address(), first);
}

TEST(maap_handler, non_overlapping_frame_is_ignored)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    Eui48 const addr = rig.handler.address();
    auto const sent_before = rig.sent.size();

    // A probe for a clearly different range (top of the local pool) — not our conflict.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, Eui48{0x91, 0xe0, 0xf0, 0x00, 0xfe, 0xff}, 1);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 2 * SEC);

    EXPECT_EQ(rig.handler.address(), addr);   // unchanged
    EXPECT_EQ(rig.sent.size(), sent_before);  // no defend/probe emitted
}

TEST(maap_handler, ignores_our_own_echoed_frame)
{
    Eui48 const me{0x02, 0, 0, 0, 0, 0x01};
    Rig rig{me};
    rig.handler.acquire(2, SEC);
    Eui48 const addr = rig.handler.address();

    // The egress tap can deliver our own PROBE back to us — it must not self-conflict.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, addr, 2);
    rig.handler.receive(me, frame, 2 * SEC);

    EXPECT_EQ(rig.handler.address(), addr);  // unchanged
}

// B.2.1: a PROBE and an ANNOUNCE go to the MAAP multicast address; B.2.4: the
// stream_id is not used in MAAP and is zero in every PDU sent.
TEST(maap_handler, probes_and_announces_go_to_the_group_with_a_zero_stream_id)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_TRUE(rig.sent.size() >= size_t{5});  // four PROBEs and the ANNOUNCE
    for (size_t i = 0; i < rig.sent.size(); ++i) {
        EXPECT_TRUE(rig.sent[i].is_probe() || rig.sent[i].is_announce());
        EXPECT_EQ(rig.sent_to[i], MAAP_MULTICAST_MAC);
        EXPECT_EQ(rig.sent[i].stream_id(), StreamId{});
    }
}

// B.2.1: the DEFEND goes to the source MAC of the PROBE it answers. B.2.5/B.2.6:
// its requested fields echo the PROBE's. B.2.7/B.2.8: its conflict fields name
// the first of our addresses the PROBE covers and how many of ours it covers.
TEST(maap_handler, defend_goes_to_the_prober_echoing_its_range_and_naming_the_overlap)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(4, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    Eui48 const ours = rig.handler.address();  // [ours, ours+4)
    Eui48 const prober{0x00, 0, 0, 0, 0, 0x09};

    // A PROBE for [ours+2, ours+7): it covers our last two addresses.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, maap_block_address(ours, 2), 5);
    rig.handler.receive(prober, frame, 100 * SEC);

    MaapDu const& defend = rig.sent.back();
    EXPECT_TRUE(defend.is_defend());
    EXPECT_EQ(rig.sent_to.back(), prober);
    EXPECT_EQ(defend.stream_id(), StreamId{});
    EXPECT_EQ(defend.requested_start_address, maap_block_address(ours, 2));
    EXPECT_EQ(defend.requested_count.get(), static_cast<uint16_t>(5));
    EXPECT_EQ(defend.conflict_start_address, maap_block_address(ours, 2));
    EXPECT_EQ(defend.conflict_count.get(), static_cast<uint16_t>(2));
}

// The PROBE that covers our whole range from its own start is told about all of
// ours, from our first address.
TEST(maap_handler, defend_names_our_whole_range_when_the_probe_contains_it)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(3, SEC);
    rig.run_until_acquired();
    Eui48 const ours = rig.handler.address();  // [ours, ours+3)

    // A PROBE for [ours, ours+8) from a higher MAC.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE, ours, 8);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 100 * SEC);

    MaapDu const& defend = rig.sent.back();
    EXPECT_TRUE(defend.is_defend());
    EXPECT_EQ(defend.requested_start_address, ours);
    EXPECT_EQ(defend.requested_count.get(), static_cast<uint16_t>(8));
    EXPECT_EQ(defend.conflict_start_address, ours);
    EXPECT_EQ(defend.conflict_count.get(), static_cast<uint16_t>(3));
}

// B.3.6.3: dec_maap_probe_count raises probeCount! the moment the count reaches
// zero, and Table B.7 answers it with sAnnounce. With MAAP_PROBE_RETRANSMITS
// retransmissions the ANNOUNCE follows the last PROBE in the same tick, not one
// probe interval later.
TEST(maap_handler, announce_follows_the_last_probe_in_the_same_tick)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(1, SEC);
    EXPECT_EQ(rig.sent.size(), size_t{1});  // the first PROBE, on acquire
    for (uint32_t k = 1; k < statusbar::avtp::MAAP_PROBE_RETRANSMITS; ++k) {
        rig.tick_at_deadline();
        EXPECT_EQ(rig.sent.size(), size_t{1} + k);  // one more PROBE, nothing else
        EXPECT_TRUE(rig.sent.back().is_probe());
        EXPECT_FALSE(rig.handler.is_acquired());
    }
    rig.tick_at_deadline();                                                           // the last PROBE
    EXPECT_EQ(rig.sent.size(), size_t{2} + statusbar::avtp::MAAP_PROBE_RETRANSMITS);  // ... and the ANNOUNCE with it
    EXPECT_TRUE(rig.sent[rig.sent.size() - 2].is_probe());
    EXPECT_TRUE(rig.sent.back().is_announce());
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.acquired_calls, 1);
}

namespace {

constexpr int64_t US = 1'000;
constexpr int64_t PROBE_BASE = static_cast<int64_t>(statusbar::avtp::MAAP_PROBE_INTERVAL_BASE_US) * US;
constexpr int64_t PROBE_TOP = PROBE_BASE + static_cast<int64_t>(statusbar::avtp::MAAP_PROBE_INTERVAL_VARIATION_US) * US;
constexpr int64_t ANNOUNCE_BASE = static_cast<int64_t>(statusbar::avtp::MAAP_ANNOUNCE_INTERVAL_BASE_US) * US;
constexpr int64_t ANNOUNCE_TOP = ANNOUNCE_BASE + static_cast<int64_t>(statusbar::avtp::MAAP_ANNOUNCE_INTERVAL_VARIATION_US) * US;
constexpr int64_t LATE_TICK = static_cast<int64_t>(statusbar::avtp::MAAP_LATE_TICK_ALLOWANCE_US) * US;
constexpr int DRAWS = 8000;

}  // namespace

// B.3.4.1: every probe interval is strictly inside (BASE, BASE + VARIATION),
// with room for a tick MAAP_LATE_TICK_ALLOWANCE_US late. acquire() arms the
// probe timer with a fresh draw each time; the deadline is the draw.
TEST(maap_handler, probe_intervals_are_drawn_strictly_inside_their_window)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x55}, 7};
    int inside = 0;
    for (int k = 0; k < DRAWS; ++k) {
        int64_t const now = static_cast<int64_t>(k + 1) * SEC;
        rig.handler.release(now);
        rig.handler.acquire(1, now);
        auto const deadline = rig.handler.next_deadline_ns();
        if (deadline && *deadline - now > PROBE_BASE && *deadline - now + LATE_TICK < PROBE_TOP) {
            ++inside;
        }
    }
    EXPECT_EQ(inside, DRAWS);
}

// B.3.4.2: the same for announce intervals. Once defending, each tick at the
// deadline sends an ANNOUNCE and re-arms with a fresh draw.
TEST(maap_handler, announce_intervals_are_drawn_strictly_inside_their_window)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x66}, 11};
    rig.handler.acquire(1, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    int64_t now = 100 * SEC;
    rig.handler.tick(now);  // past whatever was armed on acquisition: from here each deadline is one draw
    int inside = 0;
    for (int k = 0; k < DRAWS; ++k) {
        auto const deadline = rig.handler.next_deadline_ns();
        if (deadline && *deadline - now > ANNOUNCE_BASE && *deadline - now + LATE_TICK < ANNOUNCE_TOP) {
            ++inside;
        }
        now = deadline.value_or(now + ANNOUNCE_TOP);
        rig.handler.tick(now);
    }
    EXPECT_EQ(inside, DRAWS);
}

// The tick that serves the deadline, even MAAP_LATE_TICK_ALLOWANCE_US late,
// still puts the ANNOUNCE on the wire inside BASE + VARIATION of the one before.
TEST(maap_handler, a_tick_at_the_deadline_sends_inside_the_window)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x77}, 5};
    rig.handler.acquire(1, SEC);
    rig.run_until_acquired();
    int64_t const first = 100 * SEC;
    rig.handler.tick(first);  // sends an ANNOUNCE now and arms the next
    size_t const sent_before = rig.sent.size();
    auto const deadline = rig.handler.next_deadline_ns();
    EXPECT_TRUE(deadline.has_value());

    rig.handler.tick(*deadline - 1);  // one ns early: nothing yet
    EXPECT_EQ(rig.sent.size(), sent_before);
    int64_t const late_tick = *deadline + LATE_TICK;
    rig.handler.tick(late_tick);
    EXPECT_EQ(rig.sent.size(), sent_before + 1);
    EXPECT_TRUE(rig.sent.back().is_announce());
    EXPECT_TRUE(late_tick - first > ANNOUNCE_BASE);
    EXPECT_TRUE(late_tick - first < ANNOUNCE_TOP);
}

TEST(maap_handler, next_deadline_is_the_nearer_armed_timer_or_nothing)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x88}};
    EXPECT_FALSE(rig.handler.next_deadline_ns().has_value());  // idle: nothing armed
    rig.handler.acquire(1, SEC);
    auto const probing = rig.handler.next_deadline_ns();
    EXPECT_TRUE(probing.has_value());
    EXPECT_TRUE(*probing > SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.next_deadline_ns().has_value());  // defending: the announce timer
    rig.handler.release(200 * SEC);
    EXPECT_FALSE(rig.handler.next_deadline_ns().has_value());
}

TEST(maap_handler, poll_timeout_is_bounded_by_the_deadline)
{
    constexpr int MAX = 100;
    int64_t const now = 50 * SEC;
    EXPECT_EQ(MaapHandler::poll_timeout_ms(std::nullopt, now, MAX), MAX);           // nothing armed: the whole period
    EXPECT_EQ(MaapHandler::poll_timeout_ms(now + SEC, now, MAX), MAX);              // further off than the period
    EXPECT_EQ(MaapHandler::poll_timeout_ms(now + 7 * 1'000'000, now, MAX), 7);      // 7 ms away: 7
    EXPECT_EQ(MaapHandler::poll_timeout_ms(now + 7 * 1'000'000 + 1, now, MAX), 8);  // a hair more: rounded up
    EXPECT_EQ(MaapHandler::poll_timeout_ms(now, now, MAX), 0);                      // due now
    EXPECT_EQ(MaapHandler::poll_timeout_ms(now - SEC, now, MAX), 0);                // overdue
}

TEST(maap_handler, block_address_helper_offsets_within_block)
{
    // A talker assigns block+0, +1, +2 to its streams from one acquired block.
    Eui48 const start{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34};
    EXPECT_EQ(statusbar::avtp::maap_block_address(start, 0), start);
    EXPECT_EQ(statusbar::avtp::maap_block_address(start, 1), (Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x35}));
    EXPECT_EQ(statusbar::avtp::maap_block_address(start, 2), (Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x36}));
    // Carry across the low octet boundary.
    EXPECT_EQ(
        statusbar::avtp::maap_block_address(Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0xff}, 1),
        (Eui48{0x91, 0xe0, 0xf0, 0x00, 0x13, 0x00}));
}

//
// B.3.5.9 PortOperational!
//

TEST(maap_handler, port_operational_begins_acquiring_a_prepared_range)
{
    using S = statusbar::avtp::maap_sm::Def::State;
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x91}};
    rig.handler.prepare(2);
    EXPECT_EQ(rig.handler.state(), S::Initial);
    EXPECT_TRUE(rig.sent.empty());  // nothing on the wire before the port is operational

    rig.handler.port_operational(SEC);
    EXPECT_EQ(rig.handler.state(), S::Probe);
    EXPECT_EQ(rig.sent.size(), size_t{1});
    EXPECT_TRUE(rig.sent.back().is_probe());
    EXPECT_EQ(rig.sent_to.back(), MAAP_MULTICAST_MAC);
    EXPECT_EQ(rig.handler.count(), 2U);
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 2));

    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.acquired_calls, 1);
}

// A range saved from an earlier life is probed first and, with nobody
// objecting, defended again: the streams keep their destination addresses.
TEST(maap_handler, a_supplied_start_is_probed_first_and_kept_when_nobody_objects)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x93}};
    Eui48 const saved{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
    rig.handler.prepare(2, saved);
    rig.handler.port_operational(SEC);
    EXPECT_EQ(rig.sent.size(), size_t{1});
    EXPECT_TRUE(rig.sent.back().is_probe());
    EXPECT_EQ(rig.handler.address(), saved);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.acquired_start.value_or(Eui48{}), saved);
}

TEST(maap_handler, a_supplied_start_somebody_else_defends_gives_way_to_a_fresh_draw)
{
    // Our MAC is high so we lose the tie-break against a low sender.
    Rig rig{Eui48{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    Eui48 const saved{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
    rig.handler.prepare(2, saved);
    rig.handler.port_operational(SEC);
    EXPECT_EQ(rig.handler.address(), saved);

    auto const frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_DEFEND, saved, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x01}, frame, 2 * SEC);
    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_NE(rig.handler.address(), saved);  // drawn afresh, once: the saved start is spent
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 2));
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_NE(rig.acquired_start.value_or(saved), saved);
}

TEST(maap_handler, a_supplied_start_the_range_does_not_fit_the_pool_from_is_ignored)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x94}};
    Eui48 const last{0x91, 0xE0, 0xF0, 0x00, 0xFD, 0xFF};  // the pool's last address: no room for two
    EXPECT_TRUE(MaapHandler::range_fits_pool(last, 1));
    EXPECT_FALSE(MaapHandler::range_fits_pool(last, 2));
    EXPECT_FALSE(MaapHandler::range_fits_pool(Eui48{0x91, 0xE0, 0xEF, 0xFF, 0xFF, 0xFF}, 1));
    rig.handler.prepare(2, last);
    rig.handler.port_operational(SEC);
    EXPECT_NE(rig.handler.address(), last);
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 2));
}

TEST(maap_handler, a_link_up_reprobe_keeps_the_supplied_range_it_defends)
{
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x95}};
    Eui48 const saved{0x91, 0xE0, 0xF0, 0x00, 0x56, 0x78};
    rig.handler.prepare(3, saved);
    rig.handler.port_operational(SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    rig.handler.port_operational(20 * SEC);  // a Link Up: the range held is re-probed (Table B.7 note a)
    EXPECT_EQ(rig.handler.address(), saved);
    rig.run_until_acquired(20 * SEC);
    EXPECT_EQ(rig.acquired_start.value_or(Eui48{}), saved);
    EXPECT_EQ(rig.acquired_calls, 2);
}

TEST(maap_handler, port_operational_with_nothing_to_acquire_does_nothing)
{
    using S = statusbar::avtp::maap_sm::Def::State;
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x92}};
    rig.handler.port_operational(SEC);
    EXPECT_EQ(rig.handler.state(), S::Initial);
    EXPECT_TRUE(rig.sent.empty());
    EXPECT_FALSE(rig.handler.next_deadline_ns().has_value());
}

TEST(maap_handler, port_operational_while_defending_reprobes_the_held_range)
{
    using S = statusbar::avtp::maap_sm::Def::State;
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x93}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    EXPECT_TRUE(rig.handler.is_acquired());
    Eui48 const held = rig.handler.address();
    size_t const sent_before = rig.sent.size();

    // The link bounces: Table B.7 DEFEND + PortOperational! -> INITIAL/Restart!,
    // the range we hold supplied back (note a), so the first PROBE is for it.
    rig.handler.port_operational(100 * SEC);
    EXPECT_EQ(rig.handler.state(), S::Probe);
    EXPECT_EQ(rig.handler.address(), held);
    EXPECT_EQ(rig.lost_calls, 1);  // not usable while it is re-probed
    EXPECT_TRUE(rig.lost_start.has_value() && *rig.lost_start == held);
    EXPECT_EQ(rig.sent.size(), sent_before + 1);
    EXPECT_TRUE(rig.sent.back().is_probe());
    EXPECT_EQ(rig.sent.back().requested_start_address, held);
    EXPECT_EQ(rig.sent_to.back(), MAAP_MULTICAST_MAC);

    // Nobody took it while the link was down: defended again, the same range.
    rig.run_until_acquired(100 * SEC);
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.handler.address(), held);
    EXPECT_EQ(rig.acquired_calls, 2);
}

TEST(maap_handler, a_range_taken_while_the_link_was_down_is_found_out_by_the_reprobe)
{
    using S = statusbar::avtp::maap_sm::Def::State;
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x94}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    Eui48 const held = rig.handler.address();

    rig.handler.port_operational(100 * SEC);
    // Another station now announces our old range: Table B.7 rAnnounce in
    // PROBE restarts us on a fresh draw.
    auto const frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_ANNOUNCE, held, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x01}, frame, 101 * SEC);
    EXPECT_EQ(rig.handler.state(), S::Probe);
    EXPECT_NE(rig.handler.address(), held);
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 2));

    rig.run_until_acquired(101 * SEC);
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_NE(rig.handler.address(), held);
}

TEST(maap_handler, port_operational_while_probing_restarts_the_probe_sequence_on_the_held_range)
{
    // A fresh acquisition's wire cost: its PROBEs and the ANNOUNCE.
    Rig fresh{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x95}};
    fresh.handler.acquire(2, SEC);
    fresh.run_until_acquired();
    size_t const pdus_per_acquisition = fresh.sent.size();

    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x96}};
    rig.handler.acquire(2, SEC);
    Eui48 const held = rig.handler.address();
    (void)rig.tick_at_deadline();  // one PROBE into the sequence
    size_t const sent_before = rig.sent.size();

    // Table B.7 PROBE + PortOperational!: the sequence starts over on the
    // range we were probing — as many PDUs again as a fresh acquisition.
    rig.handler.port_operational(50 * SEC);
    EXPECT_EQ(rig.handler.address(), held);
    EXPECT_EQ(rig.lost_calls, 0);  // it was never defended
    rig.run_until_acquired(50 * SEC);
    EXPECT_TRUE(rig.handler.is_acquired());
    EXPECT_EQ(rig.handler.address(), held);
    EXPECT_EQ(rig.sent.size() - sent_before, pdus_per_acquisition);
}

TEST(maap_handler, released_then_port_operational_acquires_afresh)
{
    using S = statusbar::avtp::maap_sm::Def::State;
    Rig rig{Eui48{0x00, 0x11, 0x22, 0x33, 0x44, 0x97}};
    rig.handler.acquire(1, SEC);
    rig.run_until_acquired();
    rig.handler.release(50 * SEC);
    EXPECT_EQ(rig.handler.state(), S::Initial);
    size_t const sent_before = rig.sent.size();

    // Table B.7 INITIAL + PortOperational!: acquisition begins, for the count
    // last asked for, on a new draw.
    rig.handler.port_operational(60 * SEC);
    EXPECT_EQ(rig.handler.state(), S::Probe);
    EXPECT_EQ(rig.handler.count(), 1U);
    EXPECT_EQ(rig.sent.size(), sent_before + 1);
    EXPECT_TRUE(rig.sent.back().is_probe());
    EXPECT_TRUE(in_dynamic_pool(rig.handler.address(), 1));
}

//
// Test Runner
//

TEST_MAIN(statusbar_avtp, avtp_maap_handler_test)
