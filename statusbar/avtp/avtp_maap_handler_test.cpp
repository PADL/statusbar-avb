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

using statusbar::avtp::MAAP_DYNAMIC_POOL_END;
using statusbar::avtp::MAAP_DYNAMIC_POOL_START;
using statusbar::avtp::MaapDu;
using statusbar::avtp::MaapHandler;
using statusbar::ieee::Eui48;
using statusbar::tsn::StreamId;

namespace {

constexpr int64_t SEC = 1'000'000'000;  // 1s in ns; > any probe/announce interval, so each tick fires its timer

/// Captures sends + acquired/lost callbacks from one MaapHandler.
struct Rig
{
    MaapHandler handler;
    std::vector<MaapDu> sent;
    std::optional<Eui48> acquired_start;
    uint16_t acquired_count{0};
    std::optional<Eui48> lost_start;
    int acquired_calls{0};
    int lost_calls{0};

    explicit Rig(Eui48 mac, uint64_t seed = 1)
        : handler{mac, StreamId{}, seed}
    {
        handler.set_send([this](std::span<uint8_t const> bytes) {
            MaapDu du{};
            if (statusbar::protocol::load(bytes, &du) != 0) {
                sent.push_back(du);
            }
            return true;
        });
        handler.set_on_acquired([this](Eui48 const& s, uint16_t c) {
            acquired_start = s;
            acquired_count = c;
            ++acquired_calls;
        });
        handler.set_on_lost([this](Eui48 const& s, uint16_t /*c*/) {
            lost_start = s;
            ++lost_calls;
        });
    }

    /// Tick repeatedly (advancing well past each timer) until acquired or a cap.
    void run_until_acquired()
    {
        for (int k = 1; k <= 12 && !handler.is_acquired(); ++k) {
            handler.tick(static_cast<int64_t>(k + 1) * SEC);
        }
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

TEST(maap_handler, announce_against_us_while_defending_fires_lost_and_repicks)
{
    Rig rig{Eui48{0x02, 0, 0, 0, 0, 0x01}};
    rig.handler.acquire(2, SEC);
    rig.run_until_acquired();
    Eui48 const old_addr = rig.handler.address();

    // A conflicting ANNOUNCE means another station owns the range — we yield.
    auto frame = make_frame(statusbar::avtp::MAAP_MESSAGE_TYPE_ANNOUNCE, old_addr, 2);
    rig.handler.receive(Eui48{0x00, 0, 0, 0, 0, 0x09}, frame, 100 * SEC);

    EXPECT_EQ(rig.handler.state(), statusbar::avtp::maap_sm::Def::State::Probe);
    EXPECT_EQ(rig.lost_calls, 1);
    EXPECT_TRUE(rig.lost_start.has_value());
    EXPECT_EQ(*rig.lost_start, old_addr);        // reported the address we left
    EXPECT_NE(rig.handler.address(), old_addr);  // re-picked a new one
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
// Test Runner
//

TEST_MAIN(statusbar_avtp, avtp_maap_handler_test)
