// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/srp/srp_mvrp_format.hpp"
#include "statusbar/srp/srp_mvrp_participant.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::srp::mvrp;
using namespace statusbar::ieee;
using statusbar::srp::mrp::Operation;
using TimePoint = statusbar::sm::TimePoint;

// Bring protocol functions into scope for ADL
using statusbar::protocol::load_unchecked;
using statusbar::protocol::store_unchecked;

//
// Tests: MVRP Constants
//

TEST(mvrp_constants, protocol_version_is_zero)
{
    EXPECT_EQ(PROTOCOL_VERSION, 0);
}

TEST(mvrp_constants, ethertype)
{
    EXPECT_EQ(ETHERTYPE, 0x88F5);
}

TEST(mvrp_constants, application_address)
{
    EXPECT_EQ(APPLICATION_ADDRESS, 0x0180C2000021ULL);
}

//
// Tests: AttributeType enum
//

TEST(mvrp_attribute_type, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(AttributeType::VlanIdentifier), 1);
}

TEST(mvrp_attribute_type, name_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile vlan_id_v = 1, unknown_v = 99;
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(vlan_id_v)), "VlanIdentifier");
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(unknown_v)), "Unknown");
}

//
// Tests: AttributeLength enum
//

TEST(mvrp_attribute_length, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(AttributeLength::VlanIdentifier), 2);
}

//
// Tests: VlanIdentifierFirstValue
//

TEST(mvrp_vlan_identifier, sizeof_is_2)
{
    EXPECT_EQ(sizeof(VlanIdentifierFirstValue), 2);
    EXPECT_EQ(VlanIdentifierFirstValue::LENGTH, 2);
}

TEST(mvrp_vlan_identifier, default_construction)
{
    VlanIdentifierFirstValue vlan{};
    EXPECT_EQ(vlan.get_vid(), 0);
}

TEST(mvrp_vlan_identifier, set_and_get_vid)
{
    VlanIdentifierFirstValue vlan{};
    vlan.set_vid(100);
    EXPECT_EQ(vlan.get_vid(), 100);
}

TEST(mvrp_vlan_identifier, set_vid_masks_to_12_bits)
{
    VlanIdentifierFirstValue vlan{};
    vlan.set_vid(0xFFFF);  // Only lower 12 bits should be set
    EXPECT_EQ(vlan.get_vid(), 0x0FFF);
}

TEST(mvrp_vlan_identifier, get_vid_masks_to_12_bits)
{
    VlanIdentifierFirstValue vlan{};
    vlan.vlan_identifier = 0xFFFF;      // Set all 16 bits directly
    EXPECT_EQ(vlan.get_vid(), 0x0FFF);  // Should only return 12 bits
}

TEST(mvrp_vlan_identifier, max_valid_vid)
{
    VlanIdentifierFirstValue vlan{};
    vlan.set_vid(4094);  // Max valid VLAN ID (0xFFE)
    EXPECT_EQ(vlan.get_vid(), 4094);
}

TEST(mvrp_vlan_identifier, designated_init)
{
    VlanIdentifierFirstValue vlan{.vlan_identifier = 2048};
    EXPECT_EQ(vlan.get_vid(), 2048);
}

TEST(mvrp_vlan_identifier, comparison)
{
    VlanIdentifierFirstValue vlan1{.vlan_identifier = 100};
    VlanIdentifierFirstValue vlan2{.vlan_identifier = 100};
    VlanIdentifierFirstValue vlan3{.vlan_identifier = 200};

    EXPECT_TRUE(vlan1 == vlan2);
    EXPECT_TRUE(vlan1 != vlan3);
    EXPECT_TRUE(vlan1 < vlan3);
}

TEST(mvrp_vlan_identifier, serialization_round_trip)
{
    VlanIdentifierFirstValue original{};
    original.set_vid(2048);

    std::array<uint8_t, 4> buffer{};
    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 2);

    VlanIdentifierFirstValue restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 2);

    EXPECT_EQ(restored.get_vid(), 2048);
}

TEST(mvrp_vlan_identifier, wire_format)
{
    VlanIdentifierFirstValue vlan{};
    vlan.set_vid(0x0102);  // 258 in decimal

    std::array<uint8_t, 2> buffer{};
    (void)store_unchecked(std::span<uint8_t>{buffer}, vlan);

    // Expected: 0x0102 in network byte order (big endian)
    EXPECT_EQ(buffer[0], 0x01);  // High byte
    EXPECT_EQ(buffer[1], 0x02);  // Low byte
}

//
// MVRP Participant Test Helpers
//

namespace {

struct MvrpTestClock
{
    TimePoint now{TimePoint{} + std::chrono::seconds(1)};
    auto advance(std::chrono::milliseconds d) -> TimePoint
    {
        now += d;
        return now;
    }
};

struct MvrpFabric
{
    std::vector<std::vector<uint8_t>> a_to_b{};
    std::vector<std::vector<uint8_t>> b_to_a{};

    [[nodiscard]] auto make_a_sender() -> std::function<bool(std::span<uint8_t const>)>
    {
        return [this](std::span<uint8_t const> pdu) {
            a_to_b.emplace_back(pdu.begin(), pdu.end());
            return true;
        };
    }
    [[nodiscard]] auto make_b_sender() -> std::function<bool(std::span<uint8_t const>)>
    {
        return [this](std::span<uint8_t const> pdu) {
            b_to_a.emplace_back(pdu.begin(), pdu.end());
            return true;
        };
    }
    void deliver_a_to_b(MvrpParticipant& b, TimePoint t)
    {
        for (auto const& p : a_to_b) {
            b.receive_pdu(p, t);
        }
        a_to_b.clear();
    }
    void deliver_b_to_a(MvrpParticipant& a, TimePoint t)
    {
        for (auto const& p : b_to_a) {
            a.receive_pdu(p, t);
        }
        b_to_a.clear();
    }
};

auto test_mvrp_config() -> MvrpConfig
{
    return MvrpConfig{.max_vlans = 8, .max_observers = 4};
}

}  // namespace

//
// Tests: MvrpParticipant
//

TEST(mvrp_participant, construct_default)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    EXPECT_EQ(p.vlan_count(), 0u);
}

// Security regression: a crafted MVRP PDU whose VlanIdentifier attribute_length
// is smaller than VlanIdentifierFirstValue::LENGTH (here 0) and whose buffer ends
// right after the vector header must NOT make decode_vector_attribute read the
// FirstValue past the end of the packet. Without the fixed-length guard in
// decode_vector_attribute this overreads 2 bytes (ASAN abort in CI; silent OOB in
// release). Layout: [version][type=VlanIdentifier][attr_length=0][vector_header].
TEST(mvrp_participant, receive_pdu_rejects_undersized_attr_length)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    std::array<uint8_t, 5> pdu{
        PROTOCOL_VERSION,
        static_cast<uint8_t>(AttributeType::VlanIdentifier),
        0x00,  // attribute_length = 0  (< VlanIdentifierFirstValue::LENGTH == 2)
        0x00,
        0x01,  // VectorAttributeHeader: number_of_values = 1, not END_MARK
    };
    p.receive_pdu(std::span<uint8_t const>(pdu), c.now);
    // The malformed attribute must be ignored, not registered, and must not crash.
    EXPECT_EQ(p.vlan_count(), 0u);
}

TEST(mvrp_participant, start_arms_timers)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    EXPECT_NE(p.next_deadline(), TimePoint::max());
}

TEST(mvrp_participant, stop_clears_timers)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    p.stop();
    EXPECT_EQ(p.next_deadline(), TimePoint::max());
}

TEST(mvrp_participant, declare_vlan_basic)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    EXPECT_TRUE(is_success(p.declare_vlan(100, c.now)));
    EXPECT_EQ(p.vlan_count(), 1u);
    EXPECT_TRUE(p.has_vlan(100));
}

TEST(mvrp_participant, declare_vlan_invalid_zero)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    EXPECT_TRUE(is_failure(p.declare_vlan(0, c.now)));
}

TEST(mvrp_participant, declare_vlan_invalid_4095)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    EXPECT_TRUE(is_failure(p.declare_vlan(4095, c.now)));
}

TEST(mvrp_participant, declare_vlan_capacity)
{
    MvrpConfig cfg{.max_vlans = 2, .max_observers = 2};
    MvrpParticipant p{cfg, 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    EXPECT_TRUE(is_success(p.declare_vlan(100, c.now)));
    EXPECT_TRUE(is_success(p.declare_vlan(200, c.now)));
    EXPECT_TRUE(is_failure(p.declare_vlan(300, c.now)));
}

TEST(mvrp_participant, withdraw_vlan_basic)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    (void)p.declare_vlan(100, c.now);
    EXPECT_TRUE(is_success(p.withdraw_vlan(100, c.now)));
}

TEST(mvrp_participant, withdraw_unknown_ok)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    EXPECT_TRUE(is_success(p.withdraw_vlan(999, c.now)));
}

TEST(mvrp_participant, subscribe_nonzero)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    Observer obs{};
    EXPECT_NE(p.subscribe(obs), 0u);
}

TEST(mvrp_participant, unsubscribe_safe)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    p.unsubscribe(999);
    p.unsubscribe(0);
}

TEST(mvrp_participant, has_vlan_false)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    EXPECT_FALSE(p.has_vlan(100));
}

TEST(mvrp_participant, is_vlan_registered_false)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    EXPECT_FALSE(p.is_vlan_registered(100));
}

TEST(mvrp_participant, declare_emits_pdu)
{
    MvrpParticipant a{test_mvrp_config(), 0xaa11};
    MvrpParticipant b{test_mvrp_config(), 0xbb22};
    MvrpFabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    MvrpTestClock c{};
    a.start(c.now);
    b.start(c.now);
    std::optional<uint16_t> b_vid{};
    Observer obs{};
    obs.on_vlan_registered = [&](uint16_t vid, Operation) { b_vid = vid; };
    b.subscribe(obs);
    (void)a.declare_vlan(100, c.now);
    auto t1 = c.advance(std::chrono::milliseconds(150));
    a.tick(t1);
    EXPECT_TRUE(!fab.a_to_b.empty());
    fab.deliver_a_to_b(b, t1);
    EXPECT_TRUE(b_vid.has_value());
    if (b_vid.has_value()) {
        EXPECT_EQ(*b_vid, 100);
    }
}

TEST(mvrp_participant, withdraw_notifies_leave)
{
    MvrpParticipant a{test_mvrp_config(), 0xcc33};
    MvrpParticipant b{test_mvrp_config(), 0xdd44};
    MvrpFabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    MvrpTestClock c{};
    a.start(c.now);
    b.start(c.now);
    std::optional<uint16_t> leave_vid{};
    Observer obs{};
    obs.on_vlan_leave = [&](uint16_t vid) { leave_vid = vid; };
    b.subscribe(obs);
    (void)a.declare_vlan(200, c.now);
    auto t1 = c.advance(std::chrono::milliseconds(150));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);
    (void)a.withdraw_vlan(200, c.now);
    auto t2 = c.advance(std::chrono::milliseconds(150));
    a.tick(t2);
    fab.deliver_a_to_b(b, t2);
    auto t3 = c.advance(std::chrono::milliseconds(1100));
    b.tick(t3);
    EXPECT_TRUE(leave_vid.has_value());
    if (leave_vid.has_value()) {
        EXPECT_EQ(*leave_vid, 200);
    }
}

TEST(mvrp_participant, receive_too_short)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 2> buf{0x00, 0x00};
    p.receive_pdu(buf, c.now);
    EXPECT_EQ(p.vlan_count(), 0u);
}

TEST(mvrp_participant, receive_wrong_version)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 3> buf{0x01, 0x00, 0x00};
    p.receive_pdu(buf, c.now);
    EXPECT_EQ(p.vlan_count(), 0u);
}

TEST(mvrp_participant, tick_noop_before_deadline)
{
    MvrpParticipant p{test_mvrp_config(), 0x1234};
    MvrpTestClock c{};
    p.start(c.now);
    p.tick(c.now);
    EXPECT_EQ(p.vlan_count(), 0u);
}

//
// Pretty-print helpers (tsn_mvrp_format.hpp, tsn_mrp_format.hpp).
// Capture a real PDU emitted by a declaring participant, then feed it
// through the format helpers and verify non-empty output. Exercises
// format_mvrp, format_mvrp_vector, and format_mrp_events.
//

TEST(mvrp_print, format_mvrp_of_declare_pdu)
{
    MvrpParticipant a{test_mvrp_config(), 0xaa11};
    MvrpFabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    MvrpTestClock c{};
    a.start(c.now);
    (void)a.declare_vlan(100, c.now);
    auto t1 = c.advance(std::chrono::milliseconds(150));
    a.tick(t1);
    EXPECT_TRUE(!fab.a_to_b.empty());

    std::string out;
    (void)statusbar::srp::mvrp::format_mvrp(std::back_inserter(out), std::span<uint8_t const>(fab.a_to_b.front()));
    EXPECT_TRUE(!out.empty());
    // Expect something that identifies this as an MVRP PDU and names the VID.
    EXPECT_TRUE(out.find("MVRP") != std::string::npos);
    EXPECT_TRUE(out.find("VlanIdentifier") != std::string::npos || out.find("vid=") != std::string::npos);
}

TEST(mvrp_print, format_mvrp_empty_payload)
{
    std::string out;
    std::array<uint8_t, 0> empty{};
    (void)statusbar::srp::mvrp::format_mvrp(std::back_inserter(out), std::span<uint8_t const>(empty));
    // Empty PDU can produce either an empty string or a "(empty)" marker
    // depending on the formatter; just assert no crash and that the
    // function returned a writable iterator.
    (void)out;
}

//
// Malformed-PDU decoder safety. Each "return" path in MvrpParticipant::
// receive_pdu and decode_vector_attribute should be reachable without
// crashing or polluting state.
//

namespace {

void expect_mvrp_pdu_ignored(MvrpParticipant& p, std::span<uint8_t const> pdu, MvrpTestClock& c)
{
    auto const before = p.vlan_count();
    p.receive_pdu(pdu, c.now);
    EXPECT_EQ(before, p.vlan_count());
}

}  // namespace

TEST(mvrp_participant_decode, empty_pdu)
{
    MvrpParticipant p{test_mvrp_config(), 0xa101};
    MvrpTestClock c{};
    p.start(c.now);
    expect_mvrp_pdu_ignored(p, std::span<uint8_t const>{}, c);
}

TEST(mvrp_participant_decode, attr_header_truncated)
{
    // Version + AttributeType only (no AttributeLength)
    MvrpParticipant p{test_mvrp_config(), 0xa102};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 2> pdu{0x00, 0x01};
    expect_mvrp_pdu_ignored(p, pdu, c);
}

TEST(mvrp_participant_decode, vector_num_values_overruns_payload)
{
    // VectorHeader claims 100 values; payload only has room for one.
    MvrpParticipant p{test_mvrp_config(), 0xa103};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 11> pdu{
        0x00,  // version
        0x01,
        0x02,  // VlanIdentifier, attr_length=2
        0x00,
        0x64,  // VectorHeader: LA=0, N=100
        0x00,
        0x64,  // FirstValue (vid=100)
        0x24,  // ThreePackedEvents (1 byte; should be ceil(100/3)=34)
        0x00,
        0x00,  // EndMark
        0x00,
    };
    expect_mvrp_pdu_ignored(p, pdu, c);
}

TEST(mvrp_participant_decode, missing_endmark_truncated)
{
    // Vector header + 2 bytes, then truncated. Decoder must not run past end.
    MvrpParticipant p{test_mvrp_config(), 0xa104};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 5> pdu{0x00, 0x01, 0x02, 0x00, 0x01};
    expect_mvrp_pdu_ignored(p, pdu, c);
}

TEST(mvrp_participant_decode, unknown_attr_type_skipped)
{
    // AttributeType = 9 (unknown). Decoder should skip past the EndMark
    // and not register anything.
    MvrpParticipant p{test_mvrp_config(), 0xa105};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 7> pdu{
        0x00,  // version
        0x09,
        0x02,  // unknown attr_type=9, attr_length=2
        0x00,
        0x00,  // attr-list EndMark
        0x00,
        0x00,  // final EndMark
    };
    expect_mvrp_pdu_ignored(p, pdu, c);
}

TEST(mvrp_participant_decode, threepacked_event_out_of_range_swallowed)
{
    // ThreePackedEvents = 250 -> first event = 6 (out of AttributeEvent range).
    // No registration should result.
    MvrpParticipant p{test_mvrp_config(), 0xa106};
    MvrpTestClock c{};
    p.start(c.now);
    std::array<uint8_t, 10> pdu{
        0x00,  // version
        0x01,
        0x02,  // VlanIdentifier, attr_length=2
        0x00,
        0x01,  // VectorHeader: 1 value
        0x00,
        0x64,  // FirstValue (vid=100)
        0xfa,  // ThreePackedEvents = 250 (invalid)
        0x00,
        0x00,  // EndMark
    };
    expect_mvrp_pdu_ignored(p, pdu, c);
}

//
// Main test runner
//

TEST_MAIN(statusbar_srp, srp_mvrp_test)
