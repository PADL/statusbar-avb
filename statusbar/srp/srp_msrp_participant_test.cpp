// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_msrp_participant.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/srp/srp_msrp_carried_test_support.hpp"
#include "statusbar/srp/srp_msrp_format.hpp"
#include "statusbar/srp/srp_msrp_test_support.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::srp::msrp;
using statusbar::srp::mrp::Operation;
using statusbar::tsn::StreamId;
using TimePoint = statusbar::sm::TimePoint;

namespace {

struct Fabric
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
    void deliver_a_to_b(MsrpParticipant& b, TimePoint now)
    {
        for (auto const& pdu : a_to_b) {
            b.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), now);
        }
        a_to_b.clear();
    }
    void deliver_b_to_a(MsrpParticipant& a, TimePoint now)
    {
        for (auto const& pdu : b_to_a) {
            a.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), now);
        }
        b_to_a.clear();
    }
};

struct TestClock
{
    TimePoint now{TimePoint{} + std::chrono::seconds(1)};
    auto advance(std::chrono::milliseconds d) -> TimePoint
    {
        now += d;
        return now;
    }
};

auto make_stream_id(uint16_t uid) -> StreamId
{
    ieee::Eui48 const mac{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    return tsn::StreamId{mac, uid};
}

auto test_msrp_config() -> MsrpConfig
{
    return MsrpConfig{
        .max_talker_advertise = 8,
        .max_talker_failed = 4,
        .max_listeners = 8,
        .max_domains = 2,
        .max_observers = 4,
        .max_interesting_stream_ids = 4,
    };
}

auto make_talker_adv(uint16_t uid) -> TalkerAdvertiseFirstValue
{
    TalkerAdvertiseFirstValue fv{};
    fv.stream_id = make_stream_id(uid);
    fv.destination_address = ieee::Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34};
    fv.vlan_identifier = default_sr_class_vid;
    fv.max_frame_size = 150;
    fv.max_interval_frames = 1;
    fv.set_priority(default_sr_class_priority);
    fv.set_rank(RANK_NON_EMERGENCY);
    fv.accumulated_latency = 0;
    return fv;
}

// Walk a raw MSRPDU and collect the unique_id of every TalkerAdvertise the PDU
// actually carries on the wire -- regardless of the three-packed event
// (Join/In/Mt). Used to assert an end station never re-declares a registered
// peer's talker. The cursor resyncs at each message's AttributeListLength
// boundary, so non-TalkerAdvertise messages are skipped wholesale.
auto talker_advertise_unique_ids(std::span<uint8_t const> pdu) -> std::vector<uint16_t>
{
    std::vector<uint16_t> ids{};
    if (pdu.empty()) {
        return ids;
    }
    size_t p = 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size()) {
        uint8_t const attr_type = pdu[p];
        if (attr_type == 0) {
            break;  // end of PDU
        }
        uint8_t const attr_len = pdu[p + 1];
        size_t const list_len = (static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3];
        p += 4;
        size_t const list_end = p + list_len;
        if (list_end > pdu.size()) {
            break;
        }
        if (attr_type == static_cast<uint8_t>(AttributeType::TalkerAdvertise)) {
            size_t vp = p;
            while (vp + 2 <= list_end) {
                uint16_t const vh = (static_cast<uint16_t>(pdu[vp]) << 8) | pdu[vp + 1];
                vp += 2;
                if (vh == 0) {
                    break;  // EndMark
                }
                uint16_t const num_values = vh & 0x1FFF;
                bool const leave_all = ((vh >> 13) & 0x7) != 0;
                if (num_values == 0) {
                    if (leave_all) {
                        continue;  // empty LeaveAll vector: no FirstValue/events
                    }
                    break;
                }
                if (vp + attr_len > list_end || attr_len < 8) {
                    break;
                }
                // One FirstValue covers num_values consecutive stream_ids
                // (the receiver increments the unique_id per index).
                uint16_t const base_uid = (static_cast<uint16_t>(pdu[vp + 6]) << 8) | pdu[vp + 7];
                for (uint16_t i = 0; i < num_values; ++i) {
                    ids.push_back(static_cast<uint16_t>(base_uid + i));
                }
                vp += attr_len;
                vp += (static_cast<size_t>(num_values) + 2) / 3;  // ThreePackedEvents
            }
        }
        p = list_end;
    }
    return ids;
}

/// One Talker Advertise value as a PDU carried it: its unique_id, its
/// three-packed AttributeEvent and the TSpec MaxFrameSize of the FirstValue
/// it was encoded under (the field the change tests vary).
struct CarriedTalker
{
    uint16_t unique_id{0};
    statusbar::srp::mrp::AttributeEvent event{};
    uint16_t max_frame_size{0};
};

// Walk a raw MSRPDU and collect every Talker Advertise value it carries, in
// wire order, with the event the sender attached to it. Same message walk as
// talker_advertise_unique_ids(), reading the events as well.
auto talker_advertise_carried(std::span<uint8_t const> pdu) -> std::vector<CarriedTalker>
{
    std::vector<CarriedTalker> out{};
    size_t p = pdu.empty() ? pdu.size() : 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size() && pdu[p] != 0) {
        uint8_t const attr_type = pdu[p];
        uint8_t const attr_len = pdu[p + 1];
        size_t const list_end = p + 4 + ((static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3]);
        size_t vp = p + 4;
        if (list_end > pdu.size()) {
            break;
        }
        while (attr_type == static_cast<uint8_t>(AttributeType::TalkerAdvertise) && vp + 2 <= list_end) {
            uint16_t const vh = (static_cast<uint16_t>(pdu[vp]) << 8) | pdu[vp + 1];
            uint16_t const num_values = vh & 0x1FFF;
            vp += 2;
            if (vh == 0 || num_values == 0 || vp + attr_len > list_end) {
                break;  // EndMark, or an empty LeaveAll vector with no FirstValue
            }
            uint16_t const base_uid = (static_cast<uint16_t>(pdu[vp + 6]) << 8) | pdu[vp + 7];
            uint16_t const max_frame_size = (static_cast<uint16_t>(pdu[vp + 16]) << 8) | pdu[vp + 17];
            auto const events = pdu.subspan(vp + attr_len, (static_cast<size_t>(num_values) + 2) / 3);
            for (uint16_t i = 0; i < num_values; ++i) {
                auto const unpacked = statusbar::srp::mrp::unpack3_events(events[i / 3]);
                auto const event = (i % 3 == 0) ? unpacked.first : (i % 3 == 1) ? unpacked.second : unpacked.third;
                out.push_back({.unique_id = static_cast<uint16_t>(base_uid + i), .event = event, .max_frame_size = max_frame_size});
            }
            vp += attr_len + events.size();
        }
        p = list_end;
    }
    return out;
}

// True if any message in the PDU has its first vector header carrying the
// LeaveAll event bits (top 3 bits of the 2-byte vector header).
auto pdu_has_leaveall(std::span<uint8_t const> pdu) -> bool
{
    if (pdu.empty()) {
        return false;
    }
    size_t p = 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size()) {
        uint8_t const attr_type = pdu[p];
        if (attr_type == 0) {
            break;  // end of PDU
        }
        size_t const list_len = (static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3];
        p += 4;
        size_t const list_end = p + list_len;
        if (list_end > pdu.size() || p + 2 > pdu.size()) {
            break;
        }
        uint16_t const vh = (static_cast<uint16_t>(pdu[p]) << 8) | pdu[p + 1];
        if (((vh >> 13) & 0x7) != 0) {
            return true;  // LeaveAll event set on this message's first vector
        }
        p = list_end;
    }
    return false;
}

namespace mrp = statusbar::srp::mrp;
using mrp::AttributeEvent;

// Build a single-message MSRPDU carrying ONE VectorAttribute with the given base
// FirstValue bytes, one AttributeEvent per value (num_values == events.size()),
// and optional 4-packed Listener declarations. This is what a peer that
// coalesces consecutive attributes into a multi-value vector puts on the wire.
auto build_vector_pdu(
    AttributeType type,
    uint8_t attr_len,
    std::span<uint8_t const> first_value,
    std::vector<AttributeEvent> const& events,
    std::vector<uint8_t> const& decls = {}) -> std::vector<uint8_t>
{
    auto const num_values = static_cast<uint16_t>(events.size());
    size_t const event_octets = (static_cast<size_t>(num_values) + 2) / 3;
    size_t const decl_octets = decls.empty() ? 0 : (static_cast<size_t>(num_values) + 3) / 4;
    std::vector<uint8_t> pdu;
    pdu.push_back(PROTOCOL_VERSION);
    pdu.push_back(static_cast<uint8_t>(type));
    pdu.push_back(attr_len);
    auto const list_len = static_cast<uint16_t>(2 + attr_len + event_octets + decl_octets + 2);
    pdu.push_back(static_cast<uint8_t>(list_len >> 8));
    pdu.push_back(static_cast<uint8_t>(list_len & 0xFFU));
    uint16_t const vh = mrp::calculate_vector_header(false, num_values);
    pdu.push_back(static_cast<uint8_t>(vh >> 8));
    pdu.push_back(static_cast<uint8_t>(vh & 0xFFU));
    pdu.insert(pdu.end(), first_value.begin(), first_value.end());
    auto event_at = [&](size_t k) { return k < events.size() ? events[k] : AttributeEvent::Mt; };
    for (size_t o = 0; o < event_octets; ++o) {
        pdu.push_back(mrp::pack3_events(event_at(o * 3), event_at((o * 3) + 1), event_at((o * 3) + 2)));
    }
    auto decl_at = [&](size_t k) -> uint8_t { return k < decls.size() ? decls[k] : 0; };
    for (size_t o = 0; o < decl_octets; ++o) {
        pdu.push_back(mrp::pack4_declarations(decl_at(o * 4), decl_at((o * 4) + 1), decl_at((o * 4) + 2), decl_at((o * 4) + 3)));
    }
    pdu.push_back(0x00);
    pdu.push_back(0x00);  // attr-list EndMark
    pdu.push_back(0x00);
    pdu.push_back(0x00);  // message EndMark
    return pdu;
}

// Collect the NumberOfValues of every VectorAttribute of the wanted AttributeType
// the PDU carries (skipping empty LeaveAll vectors), so a test can assert how a
// run of declared attributes was packed on the wire. Listener vectors carry an
// extra 4-packed declaration block per value, accounted for when advancing.
auto vector_num_values(std::span<uint8_t const> pdu, AttributeType want) -> std::vector<uint16_t>
{
    std::vector<uint16_t> out{};
    if (pdu.empty()) {
        return out;
    }
    bool const has_decls = (want == AttributeType::Listener);
    size_t p = 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size()) {
        uint8_t const attr_type = pdu[p];
        if (attr_type == 0) {
            break;
        }
        uint8_t const attr_len = pdu[p + 1];
        size_t const list_len = (static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3];
        p += 4;
        size_t const list_end = p + list_len;
        if (list_end > pdu.size()) {
            break;
        }
        if (attr_type == static_cast<uint8_t>(want)) {
            size_t vp = p;
            while (vp + 2 <= list_end) {
                uint16_t const vh = (static_cast<uint16_t>(pdu[vp]) << 8) | pdu[vp + 1];
                vp += 2;
                if (vh == 0) {
                    break;  // EndMark
                }
                uint16_t const num_values = vh & 0x1FFF;
                bool const leave_all = ((vh >> 13) & 0x7) != 0;
                if (num_values == 0) {
                    if (leave_all) {
                        continue;  // empty LeaveAll vector
                    }
                    break;
                }
                out.push_back(num_values);
                vp += attr_len + (static_cast<size_t>(num_values) + 2) / 3 +
                    (has_decls ? (static_cast<size_t>(num_values) + 3) / 4 : 0);
            }
        }
        p = list_end;
    }
    return out;
}

// Collect the AttributeType of every message in the PDU (walking by the message
// AttributeListLength), so a corrupt/record-less message is visible.
auto pdu_attribute_types(std::span<uint8_t const> pdu) -> std::vector<uint8_t>
{
    std::vector<uint8_t> types{};
    if (pdu.empty()) {
        return types;
    }
    size_t p = 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size()) {
        uint8_t const attr_type = pdu[p];
        if (attr_type == 0) {
            break;  // end of PDU
        }
        size_t const list_len = (static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3];
        types.push_back(attr_type);
        p += 4 + list_len;
    }
    return types;
}

}  // namespace

TEST(msrp_participant, start_arms_timers)
{
    MsrpParticipant p{test_msrp_config(), 0x12345};
    TestClock clock{};
    p.start(clock.now);
    EXPECT_NE(p.next_deadline(), TimePoint::max());
}

TEST(msrp_participant, declare_talker_emits_pdu_peer_notifies)
{
    MsrpParticipant a{test_msrp_config(), 0xaa11};
    MsrpParticipant b{test_msrp_config(), 0xbb22};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    std::optional<TalkerAdvertiseFirstValue> b_received_talker{};
    Observer obs{};
    obs.on_talker_advertise = [&](TalkerAdvertiseFirstValue const& fv, Operation) { b_received_talker = fv; };
    b.subscribe(obs);
    auto const ta = make_talker_adv(0x0001);
    auto status = a.declare_talker_advertise(ta, clock.now);
    EXPECT_TRUE(bool(status));
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    EXPECT_TRUE(!fab.a_to_b.empty());
    fab.deliver_a_to_b(b, t1);
    EXPECT_TRUE(b_received_talker.has_value());
    if (b_received_talker.has_value()) {
        EXPECT_EQ(b_received_talker->stream_id.get_unique_id(), 0x0001);
    }
    auto const* looked_up = b.find_talker_advertise(make_stream_id(0x0001));
    EXPECT_NE(looked_up, nullptr);
}

// End station, not a bridge: a registered (peer) TalkerAdvertise is tracked by
// the Registrar -- so we learn the remote stream and can send Listener Ready --
// but it must NEVER be re-declared on the wire. A single-port end station has no
// MAP (propagation) component; the observer applicant's optional sIn
// (a_tx_in_optional) would otherwise put the peer's StreamID on our egress,
// making the bridge see a second source for it and reject the reservation with
// TalkerFailed. Verify our PDU re-advertises only what WE declared, even across a
// LeaveAll re-assertion (where the observer fires its optional In).
TEST(msrp_participant, registered_peer_talker_is_not_readvertised)
{
    MsrpParticipant a{test_msrp_config(), 0xa001};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    // a HEARS a peer's talker (uid 0xB001) and registers it (operation=Register).
    {
        MsrpParticipant peer{test_msrp_config(), 0xb002};
        Fabric peer_fab{};
        peer.set_send_pdu(peer_fab.make_a_sender());
        peer.start(clock.now);
        (void)peer.declare_talker_advertise(make_talker_adv(0xB001), clock.now);
        auto const tp = clock.advance(std::chrono::milliseconds(250));
        peer.tick(tp);
        for (auto const& pdu : peer_fab.a_to_b) {
            a.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), clock.now);
        }
    }
    // a learned the peer stream (so its observer applicant is live and would
    // otherwise re-declare it).
    EXPECT_NE(a.find_talker_advertise(make_stream_id(0xB001)), nullptr);

    // a also declares ITS OWN talker (uid 0xA001) -- this one MUST go on the wire.
    (void)a.declare_talker_advertise(make_talker_adv(0xA001), clock.now);

    // Collect everything a transmits across a join-timer pass and a LeaveAll
    // re-assertion (the 20s jump fires the LeaveAll, driving every applicant --
    // including the registered-peer observer -- to attempt a transmit).
    fab.a_to_b.clear();
    a.tick(clock.advance(std::chrono::milliseconds(250)));
    a.tick(clock.advance(std::chrono::seconds(20)));
    EXPECT_TRUE(!fab.a_to_b.empty());

    bool saw_own = false;
    bool saw_peer = false;
    for (auto const& pdu : fab.a_to_b) {
        for (uint16_t const uid : talker_advertise_unique_ids(std::span<uint8_t const>(pdu.data(), pdu.size()))) {
            saw_own = saw_own || (uid == 0xA001);
            saw_peer = saw_peer || (uid == 0xB001);
        }
    }
    EXPECT_TRUE(saw_own);    // our own declaration is advertised
    EXPECT_FALSE(saw_peer);  // the registered peer's talker is NEVER re-advertised
}

TEST(msrp_participant, periodic_leaveall_is_transmitted)
{
    // Regression: build_and_send_pdu hardcoded leave_all_flag=false, so when the
    // LeaveAll timer fired it drove our OWN registrars to Lv (TxLeaveAll) but
    // never put the LeaveAll on the wire. The peer therefore never re-declared,
    // our registrations aged out Lv->Mt, and any reservation we had registered
    // (e.g. a downstream listener for our talker stream) was dropped every
    // LeaveAll period -- the listener-ready flap. The LeaveAll MUST be sent.
    MsrpParticipant a{test_msrp_config(), 0xa001};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    (void)a.declare_talker_advertise(make_talker_adv(0xA001), clock.now);
    fab.a_to_b.clear();
    a.tick(clock.advance(std::chrono::milliseconds(250)));
    a.tick(clock.advance(std::chrono::seconds(20)));  // fire the LeaveAll timer
    EXPECT_TRUE(!fab.a_to_b.empty());
    bool saw_leaveall = false;
    for (auto const& pdu : fab.a_to_b) {
        saw_leaveall = saw_leaveall || pdu_has_leaveall(std::span<uint8_t const>(pdu.data(), pdu.size()));
    }
    EXPECT_TRUE(saw_leaveall);  // our periodic LeaveAll is actually on the wire
}

TEST(msrp_participant, leaveall_pdu_redeclares_listener_and_omits_recordless_talkerfailed)
{
    // Regression: build_and_send_pdu attached the LeaveAll flag to EVERY attribute
    // type, so a type with no records (TalkerFailed on a pure talker) emitted a
    // standalone empty LeaveAll vector. Mis-parsing peers (some AVB switches,
    // tshark) read a FirstValue for that 0-value vector and consumed the bytes of
    // the following Listener/Domain messages -- DESTROYING the re-declarations in
    // the same PDU. Losing the Domain re-declaration makes a bridge fail the
    // talker with code 8. The fired LeaveAll PDU must re-declare our Listener and
    // must NOT carry a record-less TalkerFailed message.
    MsrpParticipant a{test_msrp_config(), 0xa001};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    (void)a.declare_talker_advertise(make_talker_adv(0xA001), clock.now);
    (void)a.declare_listener(make_stream_id(0xB001), ListenerDeclaration::Ready, clock.now);
    fab.a_to_b.clear();
    a.tick(clock.advance(std::chrono::milliseconds(250)));
    a.tick(clock.advance(std::chrono::seconds(20)));  // fire the LeaveAll timer
    bool found_la = false;
    for (auto const& pdu : fab.a_to_b) {
        auto const span = std::span<uint8_t const>(pdu.data(), pdu.size());
        if (!pdu_has_leaveall(span)) {
            continue;
        }
        found_la = true;
        bool has_listener = false;
        bool has_talker_failed = false;
        for (uint8_t const t : pdu_attribute_types(span)) {
            has_listener = has_listener || (t == static_cast<uint8_t>(AttributeType::Listener));
            has_talker_failed = has_talker_failed || (t == static_cast<uint8_t>(AttributeType::TalkerFailed));
        }
        EXPECT_TRUE(has_listener);        // Listener still re-declared in the LeaveAll PDU
        EXPECT_FALSE(has_talker_failed);  // no record-less TalkerFailed message corrupting the PDU
    }
    EXPECT_TRUE(found_la);
}

TEST(msrp_participant, received_leaveall_triggers_immediate_redeclare)
{
    // A received LeaveAll must make us re-declare SYNCHRONOUSLY in receive_pdu, not
    // wait the ~100 ms JoinTime. A bridge that issued the LeaveAll declares our
    // (leaving) attributes as Mt toward downstream listeners on its own ~100 ms
    // join timer; a re-declare landing just after that lets the listener (a DSP
    // processor reached through an AVB switch) see our talker blink out and drop the reservation,
    // stopping E->the DSP processor forwarding. So delivering a peer LeaveAll must immediately
    // produce a re-declaration on our TX with NO intervening tick.
    MsrpParticipant a{test_msrp_config(), 0xa001};
    MsrpParticipant b{test_msrp_config(), 0xb002};
    Fabric fab{};
    Fabric bfab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(bfab.make_a_sender());  // b's egress captured in bfab.a_to_b
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    (void)a.declare_talker_advertise(make_talker_adv(0xA001), clock.now);
    a.tick(clock.advance(std::chrono::milliseconds(250)));

    // Fire b's periodic LeaveAll and grab the PDU it emits.
    bfab.a_to_b.clear();
    b.tick(clock.advance(std::chrono::seconds(20)));
    std::vector<uint8_t> leaveall_pdu;
    for (auto const& pdu : bfab.a_to_b) {
        if (pdu_has_leaveall(std::span<uint8_t const>(pdu.data(), pdu.size()))) {
            leaveall_pdu = pdu;
            break;
        }
    }
    EXPECT_TRUE(!leaveall_pdu.empty());

    // Deliver b's LeaveAll to a with NO a.tick(); a must re-declare immediately.
    fab.a_to_b.clear();
    auto const t = clock.advance(std::chrono::milliseconds(1));
    a.receive_pdu(std::span<uint8_t const>(leaveall_pdu.data(), leaveall_pdu.size()), t);
    bool redeclared = false;
    for (auto const& pdu : fab.a_to_b) {
        for (uint16_t const uid : talker_advertise_unique_ids(std::span<uint8_t const>(pdu.data(), pdu.size()))) {
            redeclared = redeclared || (uid == 0xA001);
        }
    }
    EXPECT_TRUE(redeclared);  // re-declared synchronously in receive_pdu, no tick
}

TEST(msrp_participant, registered_listener_redeclared_only_when_sticky_enabled)
{
    // Sticky-Listener workaround (set_redeclare_registered_listeners). An
    // end-station TALKER registers a Listener record for its own stream when a
    // downstream listener's Listener-Ready propagates back to it. By default
    // (cfea332) that registered Listener is NEVER re-emitted -- strict end-station
    // behaviour. With the workaround enabled it IS re-emitted on every
    // periodic/LeaveAll pass (the echo that keeps a bridge forwarding our stream
    // to a downstream listener). The Talker type is unaffected either way (no two-source
    // conflict allowed). This test exercises both flag states.
    auto listener_echoed = [](bool sticky) -> bool {
        MsrpParticipant a{test_msrp_config(), 0xa001};
        a.set_redeclare_registered_listeners(sticky);
        MsrpParticipant b{test_msrp_config(), 0xb002};
        Fabric fab{};
        Fabric bfab{};
        a.set_send_pdu(fab.make_a_sender());
        b.set_send_pdu(bfab.make_a_sender());
        TestClock clock{};
        a.start(clock.now);
        b.start(clock.now);

        // a is the TALKER for 0xA001.
        (void)a.declare_talker_advertise(make_talker_adv(0xA001), clock.now);
        a.tick(clock.advance(std::chrono::milliseconds(250)));

        // b is a downstream LISTENER for 0xA001; deliver its Listener-Ready to a so
        // a holds a REGISTERED (operation=Register) Listener record for its stream.
        (void)b.declare_listener(make_stream_id(0xA001), ListenerDeclaration::Ready, clock.now);
        bfab.a_to_b.clear();
        b.tick(clock.advance(std::chrono::milliseconds(250)));
        for (auto const& pdu : bfab.a_to_b) {
            a.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), clock.now);
        }

        // Run a periodic + LeaveAll pass on a and see whether a Listener attribute
        // (the only one a could emit -- a never DECLARED a listener) goes out.
        fab.a_to_b.clear();
        a.tick(clock.advance(std::chrono::milliseconds(250)));
        a.tick(clock.advance(std::chrono::seconds(20)));
        for (auto const& pdu : fab.a_to_b) {
            for (uint8_t const t : pdu_attribute_types(std::span<uint8_t const>(pdu.data(), pdu.size()))) {
                if (t == static_cast<uint8_t>(AttributeType::Listener)) {
                    return true;
                }
            }
        }
        return false;
    };

    EXPECT_FALSE(listener_echoed(false));  // default: registered listener never re-emitted
    EXPECT_TRUE(listener_echoed(true));    // sticky: registered listener echoed back to the bridge
}

TEST(msrp_participant, suppress_leaveall_never_originates_leaveall)
{
    // Suppress-LeaveAll workaround (set_suppress_leaveall). By default the periodic
    // LeaveAll timer puts a LeaveAll on the wire (regression-tested elsewhere).
    // With the workaround enabled we must NEVER originate a LeaveAll -- we just
    // keep re-asserting via the periodic timer (the earlier sticky behaviour an
    // AVB bridge needs). We still re-declare our own talker continuously.
    auto run = [](bool suppress) -> std::pair<bool, bool> {  // {saw_leaveall, saw_own_talker}
        MsrpParticipant a{test_msrp_config(), 0xa001};
        a.set_suppress_leaveall(suppress);
        Fabric fab{};
        a.set_send_pdu(fab.make_a_sender());
        TestClock clock{};
        a.start(clock.now);
        (void)a.declare_talker_advertise(make_talker_adv(0xA001), clock.now);
        fab.a_to_b.clear();
        a.tick(clock.advance(std::chrono::milliseconds(250)));
        a.tick(clock.advance(std::chrono::seconds(20)));  // fire the LeaveAll timer
        a.tick(clock.advance(std::chrono::seconds(1)));   // periodic re-assert
        bool saw_leaveall = false;
        bool saw_own = false;
        for (auto const& pdu : fab.a_to_b) {
            auto const span = std::span<uint8_t const>(pdu.data(), pdu.size());
            saw_leaveall = saw_leaveall || pdu_has_leaveall(span);
            for (uint16_t const uid : talker_advertise_unique_ids(span)) {
                saw_own = saw_own || (uid == 0xA001);
            }
        }
        return {saw_leaveall, saw_own};
    };

    auto const [la_default, own_default] = run(false);
    EXPECT_TRUE(la_default);  // default: periodic LeaveAll is transmitted
    EXPECT_TRUE(own_default);

    auto const [la_suppressed, own_suppressed] = run(true);
    EXPECT_FALSE(la_suppressed);  // suppressed: we NEVER originate a LeaveAll
    EXPECT_TRUE(own_suppressed);  // but we still re-assert our own talker
}

TEST(msrp_participant, withdraw_talker_notifies_peer_leave)
{
    MsrpParticipant a{test_msrp_config(), 0xcc33};
    MsrpParticipant b{test_msrp_config(), 0xdd44};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    std::optional<tsn::StreamId> leave_id{};
    Observer obs{};
    obs.on_talker_leave = [&](tsn::StreamId const& id) { leave_id = id; };
    b.subscribe(obs);
    (void)a.declare_talker_advertise(make_talker_adv(0x0042), clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);
    auto const stream_id = make_stream_id(0x0042);
    (void)a.withdraw_talker(stream_id, clock.now);
    auto const t2 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t2);
    fab.deliver_a_to_b(b, t2);
    auto const t3 = clock.advance(std::chrono::milliseconds(1100));
    b.tick(t3);
    EXPECT_TRUE(leave_id.has_value());
    if (leave_id.has_value()) {
        EXPECT_EQ(leave_id->get_unique_id(), 0x0042);
    }
}

// IEEE 802.1Q-2022 10.7.4.2: the leavetimer is per Registrar. Two talkers
// withdrawn 650 ms apart are dropped 650 ms apart, each a full LeaveTime
// after its own Leave, not both when the first one's timer runs out. The
// record goes when its Registrar reaches Mt (reclaim), so the count says
// which timers have fired; the Leave notification itself is also given
// on the peer's Lv message, so it is not what is counted here.
TEST(msrp_participant, each_registrar_leaves_a_full_leave_time_after_its_own_leave)
{
    MsrpParticipant a{test_msrp_config(), 0xcc33};
    MsrpParticipant b{test_msrp_config(), 0xdd44};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    std::vector<uint16_t> timer_left{};
    Observer obs{};
    obs.on_talker_leave = [&](tsn::StreamId const& id) { timer_left.push_back(id.get_unique_id()); };
    (void)a.declare_talker_advertise(make_talker_adv(0x0001), clock.now);
    (void)a.declare_talker_advertise(make_talker_adv(0x0002), clock.now);
    auto const t0 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t0);
    fab.deliver_a_to_b(b, t0);
    EXPECT_EQ(b.talker_advertise_count(), 2U);

    (void)a.withdraw_talker(make_stream_id(0x0001), clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);  // b's Registrar for 0x0001 enters Lv at t1
    EXPECT_TRUE(b.next_deadline() <= t1 + std::chrono::milliseconds(1000));

    (void)a.withdraw_talker(make_stream_id(0x0002), clock.advance(std::chrono::milliseconds(500)));
    auto const t2 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t2);
    fab.deliver_a_to_b(b, t2);  // b's Registrar for 0x0002 enters Lv at t2 = t1 + 650 ms
    EXPECT_EQ(b.talker_advertise_count(), 2U);
    b.subscribe(obs);  // from here only the timers can notify

    b.tick(clock.advance(std::chrono::milliseconds(350)));  // t1 + 1000: the first timer, not the second
    EXPECT_EQ(b.talker_advertise_count(), 1U);
    EXPECT_EQ(timer_left.size(), 1U);
    EXPECT_TRUE(!timer_left.empty() && timer_left.front() == 0x0001);
    EXPECT_TRUE(b.next_deadline() <= t2 + std::chrono::milliseconds(1000));

    b.tick(clock.advance(std::chrono::milliseconds(300)));  // t1 + 1300: still only the first
    EXPECT_EQ(b.talker_advertise_count(), 1U);
    b.tick(clock.advance(std::chrono::milliseconds(350)));  // t2 + 1000: now the second
    EXPECT_EQ(b.talker_advertise_count(), 0U);
    EXPECT_EQ(timer_left.size(), 2U);
    EXPECT_TRUE(timer_left.size() == 2 && timer_left.back() == 0x0002);
}

TEST(msrp_participant, listener_declare_reaches_peer)
{
    MsrpParticipant a{test_msrp_config(), 0xee55};
    MsrpParticipant b{test_msrp_config(), 0xff66};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    std::optional<std::tuple<tsn::StreamId, ListenerDeclaration>> listener_seen{};
    Observer obs{};
    obs.on_listener = [&](tsn::StreamId const& id, ListenerDeclaration decl, Operation) {
        listener_seen = std::make_tuple(id, decl);
    };
    a.subscribe(obs);
    auto const sid = make_stream_id(0x0099);
    (void)b.declare_listener(sid, ListenerDeclaration::Ready, clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    b.tick(t1);
    fab.deliver_b_to_a(a, t1);
    EXPECT_TRUE(listener_seen.has_value());
    if (listener_seen.has_value()) {
        EXPECT_EQ(std::get<0>(*listener_seen).get_unique_id(), 0x0099);
        EXPECT_EQ(static_cast<int>(std::get<1>(*listener_seen)), static_cast<int>(ListenerDeclaration::Ready));
    }
}

/// Two participants, @p b's Ready listener registered on @p a (a's config @p config_a), then
/// withdrawn and its Lv delivered to @p a.
struct ListenerLeaveRig
{
    explicit ListenerLeaveRig(MsrpConfig const& config_a)
        : a{config_a, 0xee55}
        , b{test_msrp_config(), 0xff66}
    {
        a.set_send_pdu(fab.make_a_sender());
        b.set_send_pdu(fab.make_b_sender());
        a.start(clock.now);
        b.start(clock.now);
        Observer obs{};
        obs.on_listener_leave = [this](tsn::StreamId const& id) { left = id; };
        a.subscribe(obs);
    }
    void exchange()
    {
        auto const t = clock.advance(std::chrono::milliseconds(250));
        b.tick(t);
        fab.deliver_b_to_a(a, t);
    }
    void declare_then_withdraw()
    {
        (void)b.declare_listener(sid, ListenerDeclaration::Ready, clock.now);
        exchange();
        EXPECT_TRUE(a.listener_permits_transmit(sid));
        (void)b.withdraw_listener(sid, clock.now);
        exchange();
    }

    MsrpParticipant a;
    MsrpParticipant b;
    Fabric fab{};
    TestClock clock{};
    std::optional<tsn::StreamId> left{};
    tsn::StreamId sid{make_stream_id(0x0077)};
};

// IEEE 802.1Q-2018 Clause 10.7.8 (the default): an Lv moves the registrar to LV, still
// registered, and the leavetimer (1 s) ends the registration.
TEST(msrp_participant, listener_leave_runs_the_leavetimer_by_default)
{
    ListenerLeaveRig rig{test_msrp_config()};
    rig.declare_then_withdraw();
    EXPECT_TRUE(rig.a.listener_permits_transmit(rig.sid));
    rig.a.tick(rig.clock.advance(std::chrono::milliseconds(500)));
    EXPECT_TRUE(rig.a.listener_permits_transmit(rig.sid));
    rig.a.tick(rig.clock.advance(std::chrono::milliseconds(600)));
    EXPECT_FALSE(rig.a.listener_permits_transmit(rig.sid));
}

// The crafted Listener LeaveAll is the MSRPDU the receive path parses: a Listener message with an
// empty LeaveAll vector, then the EndMarks.
TEST(msrp_test_support, the_listener_leave_all_pdu_has_the_wire_layout)
{
    std::array<uint8_t, 11> const wire{0x00, 0x03, 0x08, 0x00, 0x04, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_TRUE(listener_leave_all_pdu() == wire);
}

TEST(msrp_participant, own_leaveall_does_not_drop_listener_reservation)
{
    // A's OWN periodic LeaveAll must NOT garbage-collect its registrar for a
    // peer-declared Listener it is actively serving. Driving the registrar to Lv
    // on our own TxLeaveAll (the spec's In + TxLeaveAll -> Lv) assumes the peer
    // re-declares within LeaveTime; a non-compliant bridge (an AVB switch that
    // re-declares only on ITS own LeaveAll, not in response to ours) does not, so
    // we would age the registration In -> Lv -> Mt every LeaveAll period and close
    // the talker gate -- freezing the stream. Only a RECEIVED LeaveAll/Leave
    // retires a registration. The reservation must survive many of our own
    // LeaveAlls with no re-Join delivered.
    MsrpParticipant a{test_msrp_config(), 0xee55};
    MsrpParticipant b{test_msrp_config(), 0xff66};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    auto const sid = make_stream_id(0x0099);
    (void)b.declare_listener(sid, ListenerDeclaration::Ready, clock.now);
    auto t = clock.advance(std::chrono::milliseconds(250));
    b.tick(t);
    fab.deliver_b_to_a(a, t);
    EXPECT_TRUE(a.listener_permits_transmit(sid));  // registered (Registrar In)

    // Fire A's own LeaveAll many times (each ~20 s jump) with NO re-Join delivered
    // from b. Our own LeaveAll must never age our own Registrars (In -> Lv -> Mt):
    // it never touches them, so the transmit gate holds.
    for (int i = 0; i < 12; ++i) {
        t = clock.advance(std::chrono::seconds(20));
        a.tick(t);
        EXPECT_TRUE(a.listener_permits_transmit(sid));  // not dropped by our own LeaveAll
    }
}

TEST(msrp_participant, interesting_stream_id_pruning)
{
    MsrpParticipant a{test_msrp_config(), 0x1111};
    MsrpParticipant b{test_msrp_config(), 0x2222};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    b.set_pruning_enabled(true);
    EXPECT_TRUE(is_success(b.add_interesting_stream_id(make_stream_id(0x0001))));
    EXPECT_EQ(b.interesting_stream_id_count(), 1u);
    int talker_advertise_count = 0;
    Observer obs{};
    obs.on_talker_advertise = [&](TalkerAdvertiseFirstValue const&, Operation) { ++talker_advertise_count; };
    b.subscribe(obs);
    (void)a.declare_talker_advertise(make_talker_adv(0x0001), clock.now);
    (void)a.declare_talker_advertise(make_talker_adv(0x0002), clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);
    EXPECT_EQ(talker_advertise_count, 1);
    EXPECT_EQ(b.talker_advertise_count(), 1u);
}

TEST(msrp_participant, unsubscribe_stops_notifications)
{
    MsrpParticipant p{test_msrp_config(), 0x3333};
    TestClock clock{};
    p.start(clock.now);
    int count = 0;
    Observer obs{};
    obs.on_talker_advertise = [&](auto const&, auto) { ++count; };
    auto const sub = p.subscribe(obs);
    p.unsubscribe(sub);
    EXPECT_EQ(count, 0);
    p.unsubscribe(sub);
}

TEST(msrp_participant, add_interesting_stream_id_capacity)
{
    auto const cfg = test_msrp_config();
    MsrpParticipant p{cfg};
    p.set_pruning_enabled(true);
    for (uint16_t i = 1; i <= 4; ++i) {
        EXPECT_TRUE(is_success(p.add_interesting_stream_id(make_stream_id(i))));
    }
    EXPECT_EQ(p.interesting_stream_id_count(), 4u);
    EXPECT_TRUE(is_failure(p.add_interesting_stream_id(make_stream_id(0x0005))));
    EXPECT_EQ(p.interesting_stream_id_count(), 4u);
    EXPECT_TRUE(is_success(p.add_interesting_stream_id(make_stream_id(0x0001))));
}

//
// Domain declaration + exchange
//

TEST(msrp_participant, declare_domain_basic)
{
    MsrpParticipant a{test_msrp_config(), 0xaa01};
    MsrpParticipant b{test_msrp_config(), 0xbb02};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    std::optional<DomainFirstValue> domain_seen{};
    Observer obs{};
    obs.on_domain = [&](DomainFirstValue const& fv, Operation) { domain_seen = fv; };
    b.subscribe(obs);
    DomainFirstValue dfv{};
    dfv.sr_class_id = SR_CLASS_A;
    dfv.sr_class_priority = default_sr_class_priority;
    dfv.sr_class_vid = default_sr_class_vid;
    EXPECT_TRUE(bool(a.declare_domain(dfv, clock.now)));
    EXPECT_EQ(a.domain_count(), 1u);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);
    EXPECT_TRUE(domain_seen.has_value());
}

TEST(msrp_participant, withdraw_domain_unknown_ok)
{
    MsrpParticipant p{test_msrp_config(), 0xdd04};
    TestClock clock{};
    p.start(clock.now);
    EXPECT_TRUE(bool(p.withdraw_domain(99, clock.now)));
}

TEST(msrp_participant, declare_talker_failed_basic)
{
    MsrpParticipant p{test_msrp_config(), 0xee05};
    TestClock clock{};
    p.start(clock.now);
    TalkerFailedFirstValue tfv{};
    tfv.advertise = make_talker_adv(0x0010);
    tfv.set_failure_code(FailureCode::InsufficientBandwidth);
    EXPECT_TRUE(bool(p.declare_talker_failed(tfv, clock.now)));
    EXPECT_EQ(p.talker_failed_count(), 1u);
}

TEST(msrp_participant, withdraw_talker_failed)
{
    MsrpParticipant p{test_msrp_config(), 0x1107};
    TestClock clock{};
    p.start(clock.now);
    TalkerFailedFirstValue tfv{};
    tfv.advertise = make_talker_adv(0x0020);
    (void)p.declare_talker_failed(tfv, clock.now);
    EXPECT_TRUE(bool(p.withdraw_talker(make_stream_id(0x0020), clock.now)));
}

TEST(msrp_participant, withdraw_listener_basic)
{
    MsrpParticipant p{test_msrp_config(), 0x2208};
    TestClock clock{};
    p.start(clock.now);
    (void)p.declare_listener(make_stream_id(0x0030), ListenerDeclaration::Ready, clock.now);
    EXPECT_EQ(p.listener_count(), 1u);
    EXPECT_TRUE(bool(p.withdraw_listener(make_stream_id(0x0030), clock.now)));
}

TEST(msrp_participant, withdraw_listener_unknown_ok)
{
    MsrpParticipant p{test_msrp_config(), 0x3309};
    TestClock clock{};
    p.start(clock.now);
    EXPECT_TRUE(bool(p.withdraw_listener(make_stream_id(0xFFFF), clock.now)));
}

TEST(msrp_participant, find_talker_advertise_not_found)
{
    MsrpParticipant p{test_msrp_config(), 0x440a};
    EXPECT_EQ(p.find_talker_advertise(make_stream_id(1)), nullptr);
}
TEST(msrp_participant, find_talker_failed_not_found)
{
    MsrpParticipant p{test_msrp_config(), 0x550b};
    EXPECT_EQ(p.find_talker_failed(make_stream_id(1)), nullptr);
}
TEST(msrp_participant, find_listener_not_found)
{
    MsrpParticipant p{test_msrp_config(), 0x660c};
    EXPECT_EQ(p.find_listener(make_stream_id(1)), nullptr);
}
TEST(msrp_participant, find_domain_not_found)
{
    MsrpParticipant p{test_msrp_config(), 0x770d};
    EXPECT_EQ(p.find_domain(99), nullptr);
}

TEST(msrp_participant, find_talker_advertise_found)
{
    MsrpParticipant p{test_msrp_config(), 0x880e};
    TestClock clock{};
    p.start(clock.now);
    (void)p.declare_talker_advertise(make_talker_adv(0x0042), clock.now);
    EXPECT_NE(p.find_talker_advertise(make_stream_id(0x0042)), nullptr);
}

TEST(msrp_participant, find_domain_found)
{
    MsrpParticipant p{test_msrp_config(), 0x990f};
    TestClock clock{};
    p.start(clock.now);
    DomainFirstValue dfv{};
    dfv.sr_class_id = SR_CLASS_B;
    (void)p.declare_domain(dfv, clock.now);
    EXPECT_NE(p.find_domain(SR_CLASS_B), nullptr);
}

TEST(msrp_participant, listener_permits_no_listener)
{
    MsrpParticipant p{test_msrp_config(), 0xaa10};
    EXPECT_FALSE(p.listener_permits_transmit(make_stream_id(1)));
}

TEST(msrp_participant, listener_permits_local_false)
{
    MsrpParticipant p{test_msrp_config(), 0xbb11};
    TestClock clock{};
    p.start(clock.now);
    (void)p.declare_listener(make_stream_id(1), ListenerDeclaration::Ready, clock.now);
    EXPECT_FALSE(p.listener_permits_transmit(make_stream_id(1)));
}

TEST(msrp_participant, remove_interesting_stream_id)
{
    MsrpParticipant p{test_msrp_config(), 0xcc12};
    p.set_pruning_enabled(true);
    (void)p.add_interesting_stream_id(make_stream_id(1));
    (void)p.add_interesting_stream_id(make_stream_id(2));
    EXPECT_EQ(p.interesting_stream_id_count(), 2u);
    p.remove_interesting_stream_id(make_stream_id(1));
    EXPECT_EQ(p.interesting_stream_id_count(), 1u);
    p.remove_interesting_stream_id(make_stream_id(99));
    EXPECT_EQ(p.interesting_stream_id_count(), 1u);
}

TEST(msrp_participant, clear_interesting_stream_ids)
{
    MsrpParticipant p{test_msrp_config(), 0xdd13};
    p.set_pruning_enabled(true);
    (void)p.add_interesting_stream_id(make_stream_id(1));
    (void)p.add_interesting_stream_id(make_stream_id(2));
    p.clear_interesting_stream_ids();
    EXPECT_EQ(p.interesting_stream_id_count(), 0u);
}

TEST(msrp_participant, pruning_default_disabled)
{
    MsrpParticipant p{test_msrp_config(), 0xee14};
    EXPECT_FALSE(p.pruning_enabled());
}

TEST(msrp_participant, set_pruning_toggle)
{
    MsrpParticipant p{test_msrp_config(), 0xff15};
    p.set_pruning_enabled(true);
    EXPECT_TRUE(p.pruning_enabled());
    p.set_pruning_enabled(false);
    EXPECT_FALSE(p.pruning_enabled());
}

TEST(msrp_participant, invalid_config_throws)
{
    MsrpConfig bad{};
    bad.max_interesting_stream_ids = 100;
    bool threw = false;
    try {
        MsrpParticipant p{bad};
    } catch (std::system_error const&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(msrp_participant, initial_counts_zero)
{
    MsrpParticipant p{test_msrp_config(), 0x1116};
    EXPECT_EQ(p.talker_advertise_count(), 0u);
    EXPECT_EQ(p.talker_failed_count(), 0u);
    EXPECT_EQ(p.listener_count(), 0u);
    EXPECT_EQ(p.domain_count(), 0u);
}

TEST(msrp_participant, talker_adv_capacity_limit)
{
    MsrpConfig cfg{};
    cfg.max_talker_advertise = 2;
    cfg.max_interesting_stream_ids = 2;
    MsrpParticipant p{cfg, 0x2217};
    TestClock clock{};
    p.start(clock.now);
    EXPECT_TRUE(is_success(p.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_TRUE(is_success(p.declare_talker_advertise(make_talker_adv(2), clock.now)));
    EXPECT_TRUE(is_failure(p.declare_talker_advertise(make_talker_adv(3), clock.now)));
}

TEST(msrp_participant, stop_preserves_attrs)
{
    MsrpParticipant p{test_msrp_config(), 0x3318};
    TestClock clock{};
    p.start(clock.now);
    (void)p.declare_talker_advertise(make_talker_adv(1), clock.now);
    p.stop();
    EXPECT_EQ(p.talker_advertise_count(), 1u);
}

TEST(msrp_participant, receive_pdu_too_short)
{
    MsrpParticipant p{test_msrp_config(), 0x4419};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 2> buf{0x00, 0x00};
    p.receive_pdu(buf, clock.now);
    EXPECT_EQ(p.talker_advertise_count(), 0u);
}

TEST(msrp_participant, receive_pdu_wrong_version)
{
    MsrpParticipant p{test_msrp_config(), 0x551a};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 3> buf{0x01, 0x00, 0x00};
    p.receive_pdu(buf, clock.now);
    EXPECT_EQ(p.talker_advertise_count(), 0u);
}

TEST(msrp_config_validate, valid_default)
{
    MsrpConfig cfg{};
    EXPECT_TRUE(is_success(cfg.validate()));
}

TEST(msrp_config_validate, bad_timers_are_refused)
{
    MsrpConfig empty_range{};
    empty_range.timers.leave_all_time_max = empty_range.timers.leave_all_time_min;
    EXPECT_TRUE(is_failure(empty_range.validate()));
    MsrpConfig no_join{};
    no_join.timers.join_time = std::chrono::milliseconds{0};
    EXPECT_TRUE(is_failure(no_join.validate()));
}

TEST(msrp_config_validate, reserved_exceeds_table)
{
    MsrpConfig cfg = test_msrp_config();
    cfg.reserved_talker_advertise = cfg.max_talker_advertise + 1;
    EXPECT_TRUE(is_failure(cfg.validate()));
}

TEST(msrp_config_validate, invalid_exceeds)
{
    MsrpConfig cfg{};
    cfg.max_interesting_stream_ids = cfg.max_talker_advertise + 1;
    EXPECT_TRUE(is_failure(cfg.validate()));
}

TEST(msrp_participant, leaveall_emits_pdu)
{
    MsrpParticipant a{test_msrp_config(), 0x661b};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    (void)a.declare_talker_advertise(make_talker_adv(1), clock.now);
    auto t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.a_to_b.clear();
    auto t2 = clock.advance(std::chrono::seconds(20));
    a.tick(t2);
    EXPECT_TRUE(!fab.a_to_b.empty());
}

//
// Pretty-print helpers (tsn_msrp_format.hpp, tsn_mrp_format.hpp).
// Capture a real MSRPDU, feed it through format_msrp, and verify
// non-empty output. Exercises format_msrp, format_msrp_vector,
// format_first_value, format_stream_id, and format_mrp_events.
//

TEST(msrp_print, format_msrp_of_declare_talker_advertise_pdu)
{
    MsrpParticipant a{test_msrp_config(), 0x1122};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    auto const ta = make_talker_adv(0x0001);
    auto status = a.declare_talker_advertise(ta, clock.now);
    EXPECT_TRUE(bool(status));
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    EXPECT_TRUE(!fab.a_to_b.empty());

    std::string out;
    (void)statusbar::srp::msrp::format_msrp(std::back_inserter(out), std::span<uint8_t const>(fab.a_to_b.front()));
    EXPECT_TRUE(!out.empty());
}

TEST(msrp_print, format_msrp_of_listener_and_domain_pdus)
{
    MsrpParticipant a{test_msrp_config(), 0x9988};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    auto const listener_status = a.declare_listener(make_stream_id(0x0042), ListenerDeclaration::Ready, clock.now);
    EXPECT_TRUE(bool(listener_status));

    DomainFirstValue domain{};
    domain.sr_class_id = 6;
    domain.sr_class_priority = 3;
    domain.sr_class_vid.set(2);
    auto const domain_status = a.declare_domain(domain, clock.now);
    EXPECT_TRUE(bool(domain_status));

    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    EXPECT_TRUE(!fab.a_to_b.empty());

    std::string out;
    for (auto const& pdu : fab.a_to_b) {
        (void)statusbar::srp::msrp::format_msrp(std::back_inserter(out), std::span<uint8_t const>(pdu));
    }
    EXPECT_TRUE(!out.empty());
}

//
// Malformed-PDU decoder safety: every "return" path in receive_pdu /
// decode_attribute_list / decode_vector_attribute should be reachable
// without crashing or polluting state. The participant must not declare
// any new records when fed garbage.
//

namespace {

void expect_pdu_ignored(MsrpParticipant& p, std::span<uint8_t const> pdu, TestClock& clock)
{
    auto const before = p.talker_advertise_count() + p.listener_count() + p.domain_count() + p.talker_failed_count();
    p.receive_pdu(pdu, clock.now);
    auto const after = p.talker_advertise_count() + p.listener_count() + p.domain_count() + p.talker_failed_count();
    EXPECT_EQ(before, after);
}

}  // namespace

TEST(msrp_participant_decode, empty_pdu)
{
    MsrpParticipant p{test_msrp_config(), 0xa001};
    TestClock clock{};
    p.start(clock.now);
    expect_pdu_ignored(p, std::span<uint8_t const>{}, clock);
}

TEST(msrp_participant_decode, version_only_no_endmark)
{
    MsrpParticipant p{test_msrp_config(), 0xa002};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 1> pdu{0x00};
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, attr_header_truncated_no_listlen)
{
    // Version + AttributeType + AttributeLength but no AttributeListLength
    MsrpParticipant p{test_msrp_config(), 0xa003};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 3> pdu{0x00, 0x01, 0x19};
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, attr_list_length_extends_past_pdu)
{
    // Talker attr, AttrListLength = 200 (way beyond pdu size)
    MsrpParticipant p{test_msrp_config(), 0xa004};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 5> pdu{0x00, 0x01, 0x19, 0x00, 0xC8};
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, attr_list_length_zero_then_endmark)
{
    // Empty attribute list immediately followed by message endmark + final endmark.
    // Should be a no-op, not a crash.
    MsrpParticipant p{test_msrp_config(), 0xa005};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 7> pdu{0x00, 0x01, 0x19, 0x00, 0x00, 0x00, 0x00};
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, vector_num_values_overruns_payload)
{
    // Domain attr, AttrListLength = 9 (room for one vector + endmark),
    // but VectorHeader claims 50 values which is larger than payload.
    // Decoder must walk away cleanly.
    MsrpParticipant p{test_msrp_config(), 0xa006};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 14> pdu{
        0x00,  // version
        0x04,
        0x04,
        0x00,
        0x09,  // Domain, len=4, list_len=9
        0x00,
        0x32,  // VectorHeader: LA=0, N=50  (lies)
        0x06,
        0x03,
        0x00,
        0x02,  // FirstValue (4 bytes)
        0x00,  // ThreePackedEvents (1 byte; should be ceil(50/3)=17)
        0x00,
        0x00,  // EndMark
    };
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, missing_endmark_within_attr_list)
{
    // AttrListLength = 4, but the inner bytes don't form a valid vector
    // and don't reach EndMark. Decoder must bound the read by attr_list_end.
    MsrpParticipant p{test_msrp_config(), 0xa007};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 9> pdu{
        0x00,  // version
        0x04,
        0x04,
        0x00,
        0x04,  // Domain, len=4, list_len=4
        0x00,
        0x01,
        0xFF,
        0xFF,  // partial vector header + garbage
    };
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, wrong_protocol_version_skipped)
{
    // Version 1 (only 0 is valid); decoder must not parse anything.
    MsrpParticipant p{test_msrp_config(), 0xa008};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 5> pdu{0x01, 0x04, 0x04, 0x00, 0x09};
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, unknown_attr_type_skipped)
{
    // AttributeType = 7 (undefined). decode_attribute_list switch has no
    // case for it, so the inner loop should skip-past via attr_list_length.
    MsrpParticipant p{test_msrp_config(), 0xa009};
    TestClock clock{};
    p.start(clock.now);
    std::array<uint8_t, 11> pdu{
        0x00,  // version
        0x07,
        0x04,
        0x00,
        0x04,  // unknown attr_type=7, len=4, list_len=4
        0x00,
        0x00,
        0x00,
        0x00,  // 4 bytes of inner content
        0x00,
        0x00,  // final EndMark
    };
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, talker_advertise_attr_length_too_small)
{
    // Real attr type but attr_length=4 (< sizeof TalkerAdvertiseFirstValue=25).
    // handle_rx_event should bail without dispatching.
    MsrpParticipant p{test_msrp_config(), 0xa00a};
    TestClock clock{};
    p.start(clock.now);
    (void)p.add_interesting_stream_id(make_stream_id(1));
    std::array<uint8_t, 14> pdu{
        0x00,
        0x01,
        0x04,
        0x00,
        0x09,  // TalkerAdvertise, len=4 (lie), list_len=9
        0x00,
        0x01,  // VectorHeader: 1 value
        0xaa,
        0xbb,
        0xcc,
        0xdd,  // 4-byte "first value" (too short for talker)
        0x24,  // ThreePackedEvents (JoinIn)
        0x00,
        0x00,  // EndMark
    };
    expect_pdu_ignored(p, pdu, clock);
}

TEST(msrp_participant_decode, threepacked_event_out_of_range_swallowed)
{
    // ThreePackedEvents byte = 250 -> unpack3 returns first=6 which is
    // outside AttributeEvent enum. handle_rx_event must classify it as
    // unknown and do nothing.
    MsrpParticipant p{test_msrp_config(), 0xa00c};
    TestClock clock{};
    p.start(clock.now);
    (void)p.add_interesting_stream_id(make_stream_id(0xbeef));
    std::array<uint8_t, 37> pdu{
        0x00, 0x01, 0x19, 0x00, 0x1e,                    // TalkerAdvertise, len=25, list_len=30
        0x00, 0x01,                                      // VectorHeader: 1 value
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0xbe, 0xef,  // tsn::StreamId (8)
        0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34,              // dest MAC (6)
        0x00, 0x02,                                      // VLAN (2)
        0x00, 0x96,                                      // max_frame_size (2)
        0x00, 0x01,                                      // max_interval (2)
        0x60,                                            // priority_and_rank (1)
        0x00, 0x00, 0x00, 0x00,                          // accumulated_latency (4) -> total 25
        0xfa,                                            // ThreePackedEvents = 250 (out of range)
        0x00, 0x00,                                      // EndMark
    };
    expect_pdu_ignored(p, pdu, clock);
}

//
// Resource-exhaustion / boundary tests (Batch 2)
//

TEST(msrp_participant, listener_capacity_limit)
{
    MsrpConfig cfg{};
    cfg.max_listeners = 2;
    cfg.max_interesting_stream_ids = 4;
    MsrpParticipant p{cfg, 0xb001};
    TestClock clock{};
    p.start(clock.now);
    EXPECT_TRUE(is_success(p.declare_listener(make_stream_id(1), ListenerDeclaration::Ready, clock.now)));
    EXPECT_TRUE(is_success(p.declare_listener(make_stream_id(2), ListenerDeclaration::Ready, clock.now)));
    EXPECT_TRUE(is_failure(p.declare_listener(make_stream_id(3), ListenerDeclaration::Ready, clock.now)));
}

TEST(msrp_participant, domain_capacity_limit)
{
    MsrpConfig cfg{};
    cfg.max_domains = 1;
    cfg.max_interesting_stream_ids = 4;
    MsrpParticipant p{cfg, 0xb002};
    TestClock clock{};
    p.start(clock.now);
    DomainFirstValue d1{};
    d1.sr_class_id = 6;
    d1.sr_class_priority = 3;
    d1.sr_class_vid.set(2);
    DomainFirstValue d2{};
    d2.sr_class_id = 5;
    d2.sr_class_priority = 2;
    d2.sr_class_vid.set(2);
    EXPECT_TRUE(is_success(p.declare_domain(d1, clock.now)));
    EXPECT_TRUE(is_failure(p.declare_domain(d2, clock.now)));
}

TEST(msrp_participant, declare_redeclare_same_stream_id_idempotent)
{
    // Re-declaring the same stream should not consume a second slot.
    MsrpConfig cfg{};
    cfg.max_talker_advertise = 1;
    cfg.max_interesting_stream_ids = 1;
    MsrpParticipant p{cfg, 0xb004};
    TestClock clock{};
    p.start(clock.now);
    EXPECT_TRUE(is_success(p.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_TRUE(is_success(p.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_EQ(p.talker_advertise_count(), 1u);
}

// A re-declared Talker Advertise with a changed TSpec is a different attribute
// (IEEE 802.1Q-2022 Clause 10.7): the value on the wire is left first, and the
// new one is declared New two LeaveAll periods after that Lv has gone out
// (35.2.2.8). A bridge that saw a changed FirstValue under a registered
// StreamID would otherwise fail the reservation with Table 35-6 code 16.
TEST(msrp_participant, a_changed_talker_advertise_leaves_the_old_value_before_declaring_the_new)
{
    MsrpParticipant a{test_msrp_config(), 0xc001};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto const before = make_talker_adv(1);
    auto after = before;
    after.max_frame_size = 300;

    EXPECT_TRUE(is_success(a.declare_talker_advertise(before, clock.now)));
    a.transmit_pending(clock.now);
    EXPECT_EQ(fab.a_to_b.size(), 1u);
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(1))->max_frame_size.get(), 150U);  // the value still on the wire
    EXPECT_EQ(a.talker_advertise_count(), 1u);

    // The next PDU carries the Lv, under the OLD FirstValue.
    a.transmit_pending(clock.now);
    EXPECT_EQ(fab.a_to_b.size(), 1u);
    auto const lv = talker_advertise_carried(fab.a_to_b.front());
    EXPECT_EQ(lv.size(), 1u);
    EXPECT_EQ(lv.front().event, AttributeEvent::Lv);
    EXPECT_EQ(lv.front().max_frame_size, 150U);
    fab.a_to_b.clear();

    // The stream stays withdrawn for two LeaveAll periods (2 x 15 s): it is
    // not declared (after a LeaveAll of ours Table 10-3's LO sends one Mt for
    // it, which declares nothing), and the record is kept for the value that
    // waits.
    for (int i = 0; i < 119; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    EXPECT_EQ(a.talker_advertise_count(), 1u);
    for (auto const& pdu : fab.a_to_b) {
        for (auto const& carried : talker_advertise_carried(pdu)) {
            EXPECT_TRUE(carried.event == AttributeEvent::Mt || carried.event == AttributeEvent::In);
        }
    }
    fab.a_to_b.clear();

    // Then the new value is declared, as New.
    for (int i = 0; i < 8; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(1))->max_frame_size.get(), 300U);
    std::vector<CarriedTalker> anew{};
    for (auto const& pdu : fab.a_to_b) {
        auto const carried = talker_advertise_carried(pdu);
        anew.insert(anew.end(), carried.begin(), carried.end());
    }
    EXPECT_TRUE(!anew.empty() && anew.front().event == AttributeEvent::New);
    EXPECT_TRUE(!anew.empty() && anew.front().max_frame_size == 300U);
    EXPECT_EQ(a.talker_advertise_count(), 1u);
}

namespace {

/// Every Talker Advertise value the PDUs in @p pdus carried, in wire order.
auto all_talkers_carried(std::vector<std::vector<uint8_t>> const& pdus) -> std::vector<CarriedTalker>
{
    std::vector<CarriedTalker> out{};
    for (auto const& pdu : pdus) {
        auto const carried = talker_advertise_carried(pdu);
        out.insert(out.end(), carried.begin(), carried.end());
    }
    return out;
}

/// Whether @p event declares its value: New, JoinIn or JoinMt.
auto declares(AttributeEvent const event) -> bool
{
    return event == AttributeEvent::New || event == AttributeEvent::JoinIn || event == AttributeEvent::JoinMt;
}

/// Participant @p a declares stream 1's Talker Advertise, sends it, withdraws
/// it and sends the Lv; the PDUs are then dropped from @p fab.
void declare_then_withdraw_stream_1(MsrpParticipant& a, Fabric& fab, TestClock const& clock)
{
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    a.transmit_pending(clock.now);
    EXPECT_TRUE(is_success(a.withdraw_talker(make_stream_id(1), clock.now)));
    a.transmit_pending(clock.now);
    auto const sent = all_talkers_carried(fab.a_to_b);
    EXPECT_TRUE(sent.size() == 2 && sent.back().event == AttributeEvent::Lv);
    fab.a_to_b.clear();
}

/// Tick @p a every 250 ms for @p quarters quarter seconds.
void run_quarters(MsrpParticipant& a, TestClock& clock, int const quarters)
{
    for (int i = 0; i < quarters; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
}

}  // namespace

// IEEE 802.1Q 35.2.2.8 puts two LeaveAll periods "between when the Talker
// removes the existing Stream registration and declares the new". A
// withdrawal removes the registration as a change does: another value
// declared under the StreamID ten seconds later waits until thirty seconds
// after the withdrawal, and is then declared New.
TEST(msrp_participant, another_value_after_a_withdrawal_waits_two_leaveall_periods_from_the_withdrawal)
{
    MsrpParticipant a{test_msrp_config(), 0xc011};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    declare_then_withdraw_stream_1(a, fab, clock);

    run_quarters(a, clock, 40);  // ten seconds
    auto after = make_talker_adv(1);
    after.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));

    run_quarters(a, clock, 79);  // to a quarter second short of the thirty
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    for (auto const& carried : all_talkers_carried(fab.a_to_b)) {
        EXPECT_FALSE(declares(carried.event));
    }
    fab.a_to_b.clear();

    // The first PDU after the thirty seconds completes the change (a LeaveAll
    // of ours comes at least every fifteen), and the value follows as New.
    run_quarters(a, clock, 64);
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    auto const anew = all_talkers_carried(fab.a_to_b);
    bool told_new = false;
    for (auto const& carried : anew) {
        told_new = told_new || (carried.event == AttributeEvent::New && carried.max_frame_size == 300U);
        EXPECT_TRUE(carried.max_frame_size == 300U || !declares(carried.event));
    }
    EXPECT_TRUE(told_new);
}

// A station that restarts has removed its registration as surely as one that
// withdrew it, and the participant of the new run remembers nothing. Told the
// value the old run declared, it holds another value under the StreamID for
// two LeaveAll periods from the start (IEEE 802.1Q 35.2.2.8) and then
// declares it New; nothing of the stream is sent meanwhile.
TEST(msrp_participant, after_a_restart_another_value_than_the_old_runs_waits_two_leaveall_periods)
{
    MsrpParticipant a{test_msrp_config(), 0xc021};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    EXPECT_TRUE(is_success(a.hold_talker_value_after_restart(make_talker_adv(1), clock.now)));
    run_quarters(a, clock, 4);
    EXPECT_TRUE(all_talkers_carried(fab.a_to_b).empty());  // the call declares nothing
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));

    auto changed = make_talker_adv(1);
    changed.destination_address = statusbar::ieee::Eui48{0x91, 0xe0, 0xf0, 0x00, 0x77, 0x77};
    EXPECT_TRUE(is_success(a.declare_talker_advertise(changed, clock.now)));
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));

    run_quarters(a, clock, 115);  // to a quarter second short of thirty from the start
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    for (auto const& carried : all_talkers_carried(fab.a_to_b)) {
        EXPECT_FALSE(declares(carried.event));
    }
    fab.a_to_b.clear();

    run_quarters(a, clock, 64);  // the first PDU after the thirty seconds completes it
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    bool told_new = false;
    for (auto const& carried : all_talkers_carried(fab.a_to_b)) {
        told_new = told_new || carried.event == AttributeEvent::New;
    }
    EXPECT_TRUE(told_new);
    auto const* const declared = a.find_talker_advertise(make_stream_id(1));
    EXPECT_TRUE(declared != nullptr && declared->destination_address == changed.destination_address);
}

// The value the old run declared is the same registration in the new run: it
// is declared at once. So is one that differs in AccumulatedLatency alone.
TEST(msrp_participant, after_a_restart_the_old_runs_value_is_declared_at_once)
{
    for (uint32_t const latency : {0U, 1500U}) {
        MsrpParticipant a{test_msrp_config(), 0xc022};
        Fabric fab{};
        a.set_send_pdu(fab.make_a_sender());
        TestClock clock{};
        a.start(clock.now);
        EXPECT_TRUE(is_success(a.hold_talker_value_after_restart(make_talker_adv(1), clock.now)));
        auto same = make_talker_adv(1);
        same.accumulated_latency = same.accumulated_latency.get() + latency;
        EXPECT_TRUE(is_success(a.declare_talker_advertise(same, clock.now)));
        EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
        a.transmit_pending(clock.now);
        auto const sent = all_talkers_carried(fab.a_to_b);
        EXPECT_TRUE(!sent.empty() && sent.front().event == AttributeEvent::New);
    }
}

// A hold nobody needed is forgotten when its time is up, and the record with
// it: the StreamID is then free for any value, at once.
TEST(msrp_participant, a_hold_after_a_restart_ends_by_itself_and_then_any_value_is_declared_at_once)
{
    MsrpParticipant a{test_msrp_config(), 0xc023};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    EXPECT_TRUE(is_success(a.hold_talker_value_after_restart(make_talker_adv(1), clock.now)));
    run_quarters(a, clock, 200);  // fifty seconds: past the hold and a LeaveAll of ours
    fab.a_to_b.clear();

    auto changed = make_talker_adv(1);
    changed.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(changed, clock.now)));
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    a.transmit_pending(clock.now);
    auto const sent = all_talkers_carried(fab.a_to_b);
    EXPECT_TRUE(!sent.empty() && sent.front().event == AttributeEvent::New && sent.front().max_frame_size == 300U);
}

// A wait is running from the moment the old run's value is told, a value
// waiting or not, until the first PDU after its two LeaveAll periods; and
// after a withdrawal in the same way. An entity keeps the old value for its
// next start as long.
TEST(msrp_participant, a_wait_is_running_from_the_restart_or_a_withdrawal_until_its_two_leaveall_periods_are_over)
{
    MsrpParticipant a{test_msrp_config(), 0xc025};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    EXPECT_FALSE(a.talker_hold_running(make_stream_id(1)));
    EXPECT_TRUE(is_success(a.hold_talker_value_after_restart(make_talker_adv(1), clock.now)));
    EXPECT_TRUE(a.talker_hold_running(make_stream_id(1)));
    EXPECT_FALSE(a.talker_hold_running(make_stream_id(2)));
    run_quarters(a, clock, 110);
    EXPECT_TRUE(a.talker_hold_running(make_stream_id(1)));
    run_quarters(a, clock, 74);  // past the thirty seconds and a LeaveAll of ours
    EXPECT_FALSE(a.talker_hold_running(make_stream_id(1)));

    // The old run's value declared again ends the wait at once.
    EXPECT_TRUE(is_success(a.hold_talker_value_after_restart(make_talker_adv(2), clock.now)));
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(2), clock.now)));
    EXPECT_FALSE(a.talker_hold_running(make_stream_id(2)));
    a.transmit_pending(clock.now);
    EXPECT_TRUE(is_success(a.withdraw_talker(make_stream_id(2), clock.now)));
    EXPECT_TRUE(a.talker_hold_running(make_stream_id(2)));
}

// What this run has done with a StreamID decides: a value told of the old run
// after the stream is declared changes nothing.
TEST(msrp_participant, a_hold_told_for_a_stream_this_run_has_declared_is_ignored)
{
    MsrpParticipant a{test_msrp_config(), 0xc024};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    a.transmit_pending(clock.now);
    auto older = make_talker_adv(1);
    older.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.hold_talker_value_after_restart(older, clock.now)));
    auto const* const declared = a.find_talker_advertise(make_stream_id(1));
    EXPECT_TRUE(declared != nullptr && declared->max_frame_size.get() == make_talker_adv(1).max_frame_size.get());
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
}

// The value that was withdrawn is the same registration when it comes back:
// it is declared again at once, however short the time since the withdrawal.
TEST(msrp_participant, the_withdrawn_value_may_be_declared_again_at_once)
{
    MsrpParticipant a{test_msrp_config(), 0xc012};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    declare_then_withdraw_stream_1(a, fab, clock);

    run_quarters(a, clock, 4);
    fab.a_to_b.clear();
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    a.transmit_pending(clock.now);
    auto const back = all_talkers_carried(fab.a_to_b);
    EXPECT_TRUE(back.size() == 1 && declares(back.front().event));
    EXPECT_TRUE(back.size() == 1 && back.front().max_frame_size == 150U);

    // And with the old value declared again the wait is over: a change from
    // here is an ordinary change of a declared value.
    auto after = make_talker_adv(1);
    after.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
}

// Once the two LeaveAll periods have passed, another value is declared with
// no further wait, and a stream never declared again gives its record back.
TEST(msrp_participant, after_the_wait_another_value_is_declared_at_once_and_an_unused_record_is_reclaimed)
{
    MsrpParticipant a{test_msrp_config(), 0xc013};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    declare_then_withdraw_stream_1(a, fab, clock);
    EXPECT_EQ(a.talker_advertise_count(), 1u);  // kept for the wait

    run_quarters(a, clock, 184);  // forty-six seconds: the wait, and a LeaveAll of ours after it
    EXPECT_EQ(a.talker_advertise_count(), 0u);
    fab.a_to_b.clear();

    auto after = make_talker_adv(1);
    after.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    a.transmit_pending(clock.now);
    auto const sent = all_talkers_carried(fab.a_to_b);
    EXPECT_TRUE(sent.size() == 1 && sent.front().event == AttributeEvent::New);
    EXPECT_TRUE(sent.size() == 1 && sent.front().max_frame_size == 300U);
}

// A change whose old value has already left keeps its wait through a
// withdrawal: the value declared after it still waits out what remains.
TEST(msrp_participant, a_withdrawal_during_a_changes_wait_does_not_end_the_wait)
{
    MsrpParticipant a{test_msrp_config(), 0xc014};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto after = make_talker_adv(1);
    after.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    a.transmit_pending(clock.now);
    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    a.transmit_pending(clock.now);  // the Lv of the old value: the wait starts

    run_quarters(a, clock, 20);  // five seconds in, the stream is withdrawn
    EXPECT_TRUE(is_success(a.withdraw_talker(make_stream_id(1), clock.now)));
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    run_quarters(a, clock, 20);  // ten seconds in, the new value is asked for again
    fab.a_to_b.clear();
    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    run_quarters(a, clock, 76);  // to twenty-nine seconds
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    for (auto const& carried : all_talkers_carried(fab.a_to_b)) {
        EXPECT_FALSE(declares(carried.event));
    }
}

// Milan v1.3 4.3.3.1 asks whether "the PAAD is registering a Listener
// attribute that matches the Stream ID": any Declaration Type counts, Asking
// Failed included, which permits no transmission. A Listener this port
// declares itself is not a peer's.
TEST(msrp_participant, listener_registered_answers_for_a_peers_listener_of_any_declaration_type)
{
    MsrpParticipant talker{test_msrp_config(), 0xc015};
    MsrpParticipant listener{test_msrp_config(), 0xc016};
    Fabric fab{};
    talker.set_send_pdu(fab.make_a_sender());
    listener.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    talker.start(clock.now);
    listener.start(clock.now);
    auto const sid = make_stream_id(0x0061);
    EXPECT_FALSE(talker.listener_registered(sid));

    EXPECT_TRUE(is_success(listener.declare_listener(sid, ListenerDeclaration::AskingFailed, clock.now)));
    listener.transmit_pending(clock.now);
    fab.deliver_b_to_a(talker, clock.now);
    EXPECT_TRUE(talker.listener_registered(sid));
    EXPECT_FALSE(talker.listener_permits_transmit(sid));
    EXPECT_FALSE(listener.listener_registered(sid));  // its own declaration

    EXPECT_TRUE(is_success(listener.withdraw_listener(sid, clock.now)));
    listener.transmit_pending(clock.now);
    fab.deliver_b_to_a(talker, clock.now);
    run_quarters(talker, clock, 8);  // the LeaveTime after the Lv
    EXPECT_FALSE(talker.listener_registered(sid));
}

namespace {

/// One Domain value as a PDU carried it.
struct CarriedDomain
{
    uint8_t sr_class_id{0};
    uint8_t sr_class_priority{0};
    uint16_t sr_class_vid{0};
    statusbar::srp::mrp::AttributeEvent event{};
};

// Walk a raw MSRPDU and collect every Domain value it carries, in wire order,
// with the event the sender attached to it (the 4-octet FirstValue is
// SRclassID, SRclassPriority, SRclassVID).
auto domain_carried(std::span<uint8_t const> pdu) -> std::vector<CarriedDomain>
{
    std::vector<CarriedDomain> out{};
    size_t p = pdu.empty() ? pdu.size() : 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size() && pdu[p] != 0) {
        uint8_t const attr_type = pdu[p];
        uint8_t const attr_len = pdu[p + 1];
        size_t const list_end = p + 4 + ((static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3]);
        size_t vp = p + 4;
        if (list_end > pdu.size()) {
            break;
        }
        while (attr_type == static_cast<uint8_t>(AttributeType::Domain) && vp + 2 <= list_end) {
            uint16_t const vh = (static_cast<uint16_t>(pdu[vp]) << 8) | pdu[vp + 1];
            uint16_t const num_values = vh & 0x1FFF;
            vp += 2;
            if (vh == 0 || num_values == 0 || vp + attr_len > list_end) {
                break;
            }
            auto const events = pdu.subspan(vp + attr_len, (static_cast<size_t>(num_values) + 2) / 3);
            auto const unpacked = statusbar::srp::mrp::unpack3_events(events[0]);
            out.push_back(
                {.sr_class_id = pdu[vp],
                 .sr_class_priority = pdu[vp + 1],
                 .sr_class_vid = static_cast<uint16_t>((static_cast<uint16_t>(pdu[vp + 2]) << 8) | pdu[vp + 3]),
                 .event = unpacked.first});
            vp += attr_len + events.size();
        }
        p = list_end;
    }
    return out;
}

auto make_domain(uint8_t const priority, uint16_t const vid) -> DomainFirstValue
{
    DomainFirstValue fv{};
    fv.sr_class_id = SR_CLASS_A;
    fv.sr_class_priority = priority;
    fv.sr_class_vid = vid;
    return fv;
}

}  // namespace

TEST(msrp_participant, a_changed_domain_leaves_the_old_value_before_declaring_the_new)
{
    // IEEE 802.1Q-2022 Clause 10.7: a Domain with another priority or VID under
    // the same SRclassID is another attribute, so the old one leaves first and
    // a peer never holds both at once.
    MsrpParticipant a{test_msrp_config(), 0xd001};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    EXPECT_TRUE(is_success(a.declare_domain(make_domain(3, 2), clock.now)));
    a.transmit_pending(clock.now);
    EXPECT_EQ(fab.a_to_b.size(), 1u);
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.declare_domain(make_domain(5, 5), clock.now)));
    EXPECT_TRUE(a.domain_change_pending(SR_CLASS_A));
    EXPECT_EQ(a.domain_count(), 1u);

    // The next PDU carries the Lv, under the OLD value, and nothing else.
    a.transmit_pending(clock.now);
    EXPECT_EQ(fab.a_to_b.size(), 1u);
    auto const lv = domain_carried(fab.a_to_b.front());
    EXPECT_EQ(lv.size(), 1u);
    EXPECT_EQ(lv.front().event, AttributeEvent::Lv);
    EXPECT_EQ(lv.front().sr_class_priority, 3);
    EXPECT_EQ(lv.front().sr_class_vid, 2);
    fab.a_to_b.clear();
    EXPECT_FALSE(a.domain_change_pending(SR_CLASS_A));

    // Then the new value, New.
    a.transmit_pending(clock.now);
    EXPECT_EQ(fab.a_to_b.size(), 1u);
    auto const anew = domain_carried(fab.a_to_b.front());
    EXPECT_EQ(anew.size(), 1u);
    EXPECT_EQ(anew.front().event, AttributeEvent::New);
    EXPECT_EQ(anew.front().sr_class_priority, 5);
    EXPECT_EQ(anew.front().sr_class_vid, 5);
    EXPECT_EQ(a.domain_count(), 1u);
}

TEST(msrp_participant, a_same_value_domain_redeclaration_sends_no_leave)
{
    MsrpParticipant a{test_msrp_config(), 0xd002};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    EXPECT_TRUE(is_success(a.declare_domain(make_domain(3, 2), clock.now)));
    a.transmit_pending(clock.now);
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.declare_domain(make_domain(3, 2), clock.now)));
    EXPECT_FALSE(a.domain_change_pending(SR_CLASS_A));
    a.transmit_pending(clock.now);
    for (auto const& pdu : fab.a_to_b) {
        for (auto const& carried : domain_carried(pdu)) {
            EXPECT_TRUE(carried.event != AttributeEvent::Lv);
        }
    }
}

namespace {

/// One Talker Failed value as a PDU carried it: its unique_id, event,
/// AccumulatedLatency and FailureCode.
struct CarriedFailed
{
    uint16_t unique_id{0};
    statusbar::srp::mrp::AttributeEvent event{};
    uint32_t accumulated_latency{0};
    uint8_t failure_code{0};
};

auto talker_failed_carried(std::span<uint8_t const> pdu) -> std::vector<CarriedFailed>
{
    std::vector<CarriedFailed> out{};
    size_t p = pdu.empty() ? pdu.size() : 1;  // skip ProtocolVersion
    while (p + 4 <= pdu.size() && pdu[p] != 0) {
        uint8_t const attr_type = pdu[p];
        uint8_t const attr_len = pdu[p + 1];
        size_t const list_end = p + 4 + ((static_cast<size_t>(pdu[p + 2]) << 8) | pdu[p + 3]);
        size_t vp = p + 4;
        if (list_end > pdu.size()) {
            break;
        }
        while (attr_type == static_cast<uint8_t>(AttributeType::TalkerFailed) && vp + 2 <= list_end) {
            uint16_t const vh = (static_cast<uint16_t>(pdu[vp]) << 8) | pdu[vp + 1];
            uint16_t const num_values = vh & 0x1FFF;
            vp += 2;
            if (vh == 0 || num_values == 0 || vp + attr_len > list_end) {
                break;
            }
            auto const events = pdu.subspan(vp + attr_len, (static_cast<size_t>(num_values) + 2) / 3);
            uint32_t const latency = (static_cast<uint32_t>(pdu[vp + 21]) << 24) | (static_cast<uint32_t>(pdu[vp + 22]) << 16) |
                (static_cast<uint32_t>(pdu[vp + 23]) << 8) | pdu[vp + 24];
            out.push_back(
                {.unique_id = static_cast<uint16_t>((static_cast<uint16_t>(pdu[vp + 6]) << 8) | pdu[vp + 7]),
                 .event = statusbar::srp::mrp::unpack3_events(events[0]).first,
                 .accumulated_latency = latency,
                 .failure_code = pdu[vp + 33]});
            vp += attr_len + events.size();
        }
        p = list_end;
    }
    return out;
}

}  // namespace

TEST(msrp_participant, a_talker_advertise_whose_latency_would_increase_becomes_a_talker_failed)
{
    // IEEE 802.1Q 35.2.2.8.6: the reported latency is not to increase during
    // the life of the reservation; an increase turns the Advertise into a
    // Talker Failed with Failure Code 7, Reported latency has changed.
    MsrpParticipant a{test_msrp_config(), 0xf001};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto before = make_talker_adv(1);
    before.accumulated_latency = 1'000'000;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(before, clock.now)));
    a.transmit_pending(clock.now);
    fab.a_to_b.clear();

    // Less latency is a change in place: no Leave, no failure.
    auto less = before;
    less.accumulated_latency = 900'000;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(less, clock.now)));
    a.transmit_pending(clock.now);
    for (auto const& pdu : fab.a_to_b) {
        EXPECT_TRUE(talker_failed_carried(pdu).empty());
        for (auto const& carried : talker_advertise_carried(pdu)) {
            EXPECT_TRUE(carried.event != AttributeEvent::Lv);
        }
    }
    fab.a_to_b.clear();

    // More latency: the Advertise leaves and a Talker Failed with code 7 and the
    // new latency is declared.
    auto more = before;
    more.accumulated_latency = 2'000'000;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(more, clock.now)));
    a.transmit_pending(clock.now);
    bool advertise_left = false;
    std::vector<CarriedFailed> failed{};
    for (auto const& pdu : fab.a_to_b) {
        for (auto const& carried : talker_advertise_carried(pdu)) {
            advertise_left = advertise_left || carried.event == AttributeEvent::Lv;
        }
        auto const f = talker_failed_carried(pdu);
        failed.insert(failed.end(), f.begin(), f.end());
    }
    EXPECT_TRUE(advertise_left);
    EXPECT_EQ(failed.size(), 1u);
    EXPECT_TRUE(!failed.empty() && failed.front().event == AttributeEvent::New);
    EXPECT_TRUE(!failed.empty() && failed.front().failure_code == 7);
    EXPECT_TRUE(!failed.empty() && failed.front().accumulated_latency == 2'000'000U);
    EXPECT_TRUE(a.find_talker_failed(make_stream_id(1)) != nullptr);
}

TEST(msrp_participant, instant_leave_retires_a_withdrawn_registration_on_arrival)
{
    // Milan v1.3 4.2.7.2.2: IN / rLv! goes straight to MT, so a peer's explicit
    // withdrawal is seen when it arrives, not a LeaveTime later.
    MsrpParticipant a{test_msrp_config(), 0xf003};
    MsrpParticipant b{test_msrp_config(), 0xf004};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    b.set_instant_leave(true);
    std::vector<uint16_t> left{};
    Observer obs{};
    obs.on_talker_leave = [&](tsn::StreamId const& id) { left.push_back(id.get_unique_id()); };
    b.subscribe(obs);

    (void)a.declare_talker_advertise(make_talker_adv(0x0001), clock.now);
    auto const t0 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t0);
    fab.deliver_a_to_b(b, t0);
    EXPECT_EQ(b.talker_advertise_count(), 1U);
    EXPECT_TRUE(left.empty());

    (void)a.withdraw_talker(make_stream_id(0x0001), clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);
    // Told on arrival; the registration is gone without a leavetimer.
    EXPECT_EQ(left.size(), 1U);
    EXPECT_TRUE(!left.empty() && left.front() == 0x0001);
}

TEST(msrp_participant, a_same_value_redeclaration_sends_no_leave)
{
    MsrpParticipant a{test_msrp_config(), 0xc002};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    a.transmit_pending(clock.now);
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    a.transmit_pending(clock.now);
    for (auto const& pdu : fab.a_to_b) {
        for (auto const& carried : talker_advertise_carried(pdu)) {
            EXPECT_TRUE(carried.event != AttributeEvent::Lv);
        }
    }
    EXPECT_EQ(a.talker_advertise_count(), 1u);
}

// AccumulatedLatency is the one Talker Advertise field 35.2.2.8.6 lets move
// in place: no Lv, the record takes the new latency at once, and the next
// re-assertion simply carries it.
TEST(msrp_participant, an_accumulated_latency_decrease_stays_in_place)
{
    // 35.2.2.8.6 lets AccumulatedLatency move in place; only an increase is a
    // failure (see the latency test below), so a decrease is a plain re-join.
    MsrpParticipant a{test_msrp_config(), 0xc003};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto first = make_talker_adv(1);
    first.accumulated_latency = 250'000;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(first, clock.now)));
    a.transmit_pending(clock.now);
    fab.a_to_b.clear();

    auto later = make_talker_adv(1);
    later.accumulated_latency = 125'000;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(later, clock.now)));
    EXPECT_FALSE(a.talker_change_pending(make_stream_id(1)));
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(1))->accumulated_latency.get(), 125'000U);
    a.transmit_pending(clock.now);
    for (auto const& pdu : fab.a_to_b) {
        for (auto const& carried : talker_advertise_carried(pdu)) {
            EXPECT_TRUE(carried.event != AttributeEvent::Lv);
        }
    }
}

// A peer's LeaveAll arriving between the Lv and the New: the stream is
// withdrawn, so the re-declaration the LeaveAll provokes declares neither its
// old value nor its new one, and the two LeaveAll periods still run their
// course.
TEST(msrp_participant, a_leaveall_between_the_leave_and_the_new_declares_nothing_for_the_stream)
{
    MsrpParticipant a{test_msrp_config(), 0xc004};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto after = make_talker_adv(1);
    after.max_frame_size = 300;
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    a.transmit_pending(clock.now);
    EXPECT_TRUE(is_success(a.declare_talker_advertise(after, clock.now)));
    a.transmit_pending(clock.now);  // the Lv
    fab.a_to_b.clear();

    // A Talker Advertise message whose one vector is an empty LeaveAll.
    uint16_t const vh = mrp::calculate_vector_header(true, 0);
    std::vector<uint8_t> const leaveall{
        PROTOCOL_VERSION,
        static_cast<uint8_t>(AttributeType::TalkerAdvertise),
        25,
        0x00,
        0x04,
        static_cast<uint8_t>(vh >> 8),
        static_cast<uint8_t>(vh & 0xFFU),
        0x00,
        0x00,
        0x00,
        0x00};
    a.receive_pdu(leaveall, clock.now);
    for (int i = 0; i < 8; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }

    // Table 10-3's LO answers the LeaveAll with an Mt, which declares nothing.
    for (auto const& pdu : fab.a_to_b) {
        for (auto const& carried : talker_advertise_carried(pdu)) {
            EXPECT_TRUE(carried.event == AttributeEvent::Mt || carried.event == AttributeEvent::In);
        }
    }
    EXPECT_TRUE(a.talker_change_pending(make_stream_id(1)));
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(1))->max_frame_size.get(), 150U);
}

// The first declaration of an attribute the port was not declaring is told
// New! (IEEE 802.1Q 10.7.5.4), whichever type it is: the first PDU carries a
// Talker Advertise, a Talker Failed, a Listener and a Domain each as New.
TEST(msrp_participant, a_first_declaration_is_told_new)
{
    MsrpParticipant a{test_msrp_config(), 0xd101};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    TalkerFailedFirstValue failed{};
    failed.advertise = make_talker_adv(2);
    failed.set_failure_code(FailureCode::InsufficientBandwidth);
    DomainFirstValue domain{};
    domain.sr_class_id = 6;
    domain.sr_class_priority = 3;
    domain.sr_class_vid.set(2);
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_TRUE(is_success(a.declare_talker_failed(failed, clock.now)));
    EXPECT_TRUE(is_success(a.declare_listener(make_stream_id(3), ListenerDeclaration::Ready, clock.now)));
    EXPECT_TRUE(is_success(a.declare_domain(domain, clock.now)));

    a.transmit_pending(clock.now);
    EXPECT_EQ(fab.a_to_b.size(), 1u);
    auto const pdu = fab.a_to_b.empty() ? std::vector<uint8_t>{} : fab.a_to_b.front();
    auto const talkers = talker_advertise_carried(pdu);
    auto const faileds = talker_failed_carried(pdu);
    auto const listeners = test_support::listener_carried(std::span<uint8_t const>(pdu));
    auto const domains = domain_carried(pdu);
    EXPECT_TRUE(talkers.size() == 1 && talkers.front().event == AttributeEvent::New);
    EXPECT_TRUE(faileds.size() == 1 && faileds.front().event == AttributeEvent::New);
    EXPECT_TRUE(listeners.size() == 1 && listeners.front().event == AttributeEvent::New);
    EXPECT_TRUE(domains.size() == 1 && domains.front().event == AttributeEvent::New);
}

// A Listener's Declaration Type changes under the StreamID the port already
// declares: Asking Failed while the Talker Advertise is missing, Ready once
// it has arrived, Asking Failed again when the talker has gone. IEEE 802.1Q
// 35.2.6: the change is a JoinIn or JoinMt carrying the new FourPackedEvent,
// with no withdrawal of the old declaration and no New.
TEST(msrp_participant, a_listener_whose_declaration_type_changes_sends_it_as_a_join)
{
    MsrpParticipant a{test_msrp_config(), 0xd102};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto const sid = make_stream_id(0x0051);
    std::vector<test_support::CarriedListener> everything{};
    auto const settle = [&] {
        for (int i = 0; i < 12; ++i) {
            a.tick(clock.advance(std::chrono::milliseconds(250)));
        }
        auto const carried = test_support::listener_carried(fab.a_to_b);
        everything.insert(everything.end(), carried.begin(), carried.end());
        fab.a_to_b.clear();
        return carried;
    };
    auto const is_join = [](AttributeEvent const event) {
        return event == AttributeEvent::JoinMt || event == AttributeEvent::JoinIn;
    };

    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::AskingFailed, clock.now)));
    auto const first = settle();
    EXPECT_TRUE(!first.empty() && first.front().event == AttributeEvent::New);
    EXPECT_TRUE(!first.empty() && first.front().declaration == ListenerDeclaration::AskingFailed);
    // Settled: the declaration is re-asserted as a Join, no longer as New.
    EXPECT_TRUE(!first.empty() && is_join(first.back().event));

    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::Ready, clock.now)));
    a.transmit_pending(clock.now);
    auto const upgraded = settle();
    EXPECT_TRUE(!upgraded.empty() && is_join(upgraded.front().event));
    EXPECT_TRUE(!upgraded.empty() && upgraded.front().declaration == ListenerDeclaration::Ready);
    EXPECT_EQ(a.listener_count(), 1u);

    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::AskingFailed, clock.now)));
    a.transmit_pending(clock.now);
    auto const downgraded = settle();
    EXPECT_TRUE(!downgraded.empty() && is_join(downgraded.front().event));
    EXPECT_TRUE(!downgraded.empty() && downgraded.front().declaration == ListenerDeclaration::AskingFailed);

    // One attribute throughout, never left, and New only while it was new.
    for (auto const& carried : everything) {
        EXPECT_EQ(carried.unique_id, 0x0051);
        EXPECT_TRUE(carried.event != AttributeEvent::Lv);
        EXPECT_TRUE(carried.event != AttributeEvent::New || carried.declaration == ListenerDeclaration::AskingFailed);
    }
}

// The receiving half of 35.2.6: a peer whose Listener changes its Declaration
// Type with a Join is reported with each new type as it arrives, and is
// never reported as having left.
TEST(msrp_participant, a_peer_listener_whose_type_changes_with_a_join_is_reported_and_never_left)
{
    MsrpParticipant talker{test_msrp_config(), 0xd104};
    MsrpParticipant listener{test_msrp_config(), 0xd105};
    Fabric fab{};
    talker.set_send_pdu(fab.make_a_sender());
    listener.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    talker.start(clock.now);
    listener.start(clock.now);
    std::vector<ListenerDeclaration> reported{};
    int leaves = 0;
    Observer obs{};
    obs.on_listener = [&](tsn::StreamId const&, ListenerDeclaration const decl, Operation) {
        if (reported.empty() || reported.back() != decl) {
            reported.push_back(decl);
        }
    };
    obs.on_listener_leave = [&](tsn::StreamId const&) { ++leaves; };
    talker.subscribe(obs);
    auto const sid = make_stream_id(0x0053);
    auto const declare_and_settle = [&](ListenerDeclaration const decl) {
        EXPECT_TRUE(is_success(listener.declare_listener(sid, decl, clock.now)));
        for (int i = 0; i < 12; ++i) {
            auto const now = clock.advance(std::chrono::milliseconds(250));
            listener.tick(now);
            fab.deliver_b_to_a(talker, now);
        }
    };

    declare_and_settle(ListenerDeclaration::AskingFailed);
    declare_and_settle(ListenerDeclaration::Ready);
    declare_and_settle(ListenerDeclaration::ReadyFailed);
    declare_and_settle(ListenerDeclaration::AskingFailed);

    std::vector<ListenerDeclaration> const expected{
        ListenerDeclaration::AskingFailed,
        ListenerDeclaration::Ready,
        ListenerDeclaration::ReadyFailed,
        ListenerDeclaration::AskingFailed};
    EXPECT_TRUE(reported == expected);
    EXPECT_EQ(leaves, 0);
}

// A declaration made again after its withdrawal has gone out is a new
// declaration, whether or not the reclaim sweep has taken the record yet.
TEST(msrp_participant, a_declaration_made_again_after_its_leave_went_out_is_new)
{
    MsrpParticipant a{test_msrp_config(), 0xd106};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto const sid = make_stream_id(0x0054);
    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::Ready, clock.now)));
    for (int i = 0; i < 12; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.withdraw_listener(sid, clock.now)));
    a.transmit_pending(clock.now);  // the Lv goes out; no reclaim sweep runs here
    auto const left = test_support::listener_carried(fab.a_to_b);
    EXPECT_TRUE(left.size() == 1 && left.front().event == AttributeEvent::Lv);
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::Ready, clock.now)));
    a.transmit_pending(clock.now);
    auto const again = test_support::listener_carried(fab.a_to_b);
    EXPECT_TRUE(again.size() == 1 && again.front().event == AttributeEvent::New);
}

// A declaration no peer ever declared back has a Registrar that never saw an
// event. Once it is withdrawn and its Leave has gone out the record is
// reclaimed all the same, and nothing more is sent for it.
TEST(msrp_participant, a_withdrawn_declaration_is_reclaimed_once_its_leave_has_gone_out)
{
    MsrpParticipant a{test_msrp_config(), 0xd109};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto const sid = make_stream_id(0x0056);
    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::Ready, clock.now)));
    for (int i = 0; i < 12; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    EXPECT_EQ(a.listener_count(), 1u);
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.withdraw_listener(sid, clock.now)));
    for (int i = 0; i < 12; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    auto const sent = test_support::listener_carried(fab.a_to_b);
    EXPECT_TRUE(sent.size() == 1 && sent.front().event == AttributeEvent::Lv);
    EXPECT_EQ(a.listener_count(), 0u);
}

// find_registered_talker_advertise() answers only while the peer's declaration
// is registered. The Leave is told once, when the LeaveTime after the peer's
// Lv has run out. From then on the talker is not found registered, though the
// record itself stands until the reclaim sweep and find_talker_advertise()
// still answers it.
TEST(msrp_participant, a_talker_that_has_left_is_no_longer_found_registered)
{
    MsrpParticipant talker{test_msrp_config(), 0xd107};
    MsrpParticipant listener{test_msrp_config(), 0xd108};
    Fabric fab{};
    talker.set_send_pdu(fab.make_a_sender());
    listener.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    talker.start(clock.now);
    listener.start(clock.now);
    auto const sid = make_stream_id(0x0055);
    std::vector<bool> registered_at_leave{};
    bool known_at_last_leave = false;
    Observer obs{};
    obs.on_talker_leave = [&](tsn::StreamId const& id) {
        registered_at_leave.push_back(listener.find_registered_talker_advertise(id) != nullptr);
        known_at_last_leave = listener.find_talker_advertise(id) != nullptr;
    };
    listener.subscribe(obs);
    auto const run = [&](int const ticks) {
        for (int i = 0; i < ticks; ++i) {
            auto const now = clock.advance(std::chrono::milliseconds(250));
            talker.tick(now);
            listener.tick(now);
            fab.deliver_a_to_b(listener, now);
        }
    };

    EXPECT_EQ(listener.find_registered_talker_advertise(sid), nullptr);
    EXPECT_TRUE(is_success(talker.declare_talker_advertise(make_talker_adv(0x0055), clock.now)));
    run(4);
    EXPECT_NE(listener.find_registered_talker_advertise(sid), nullptr);
    EXPECT_EQ(listener.find_registered_talker_failed(sid), nullptr);

    EXPECT_TRUE(is_success(talker.withdraw_talker(sid, clock.now)));
    run(12);
    EXPECT_TRUE(registered_at_leave == std::vector<bool>{false});
    EXPECT_TRUE(known_at_last_leave);
    EXPECT_EQ(listener.find_registered_talker_advertise(sid), nullptr);
}

//
// The AVnu end station tests of MSRP (802.1Qat End Station Implementation
// Compliance Test Suite, groups e2 and e3), with the timers Milan v1.3 Table
// 4.3 shares with IEEE 802.1Q Table 10-7. Each test below names the AVnu test
// it follows and is told the time, 10 ms at a step.
//

namespace {

/// What one participant sent, each PDU with the time it went out.
struct TimedPdus
{
    struct Sent
    {
        TimePoint at{};
        std::vector<uint8_t> pdu{};
    };
    std::vector<Sent> sent{};

    [[nodiscard]] auto sender(TestClock const& clock) -> std::function<bool(std::span<uint8_t const>)>
    {
        return [this, &clock](std::span<uint8_t const> pdu) {
            sent.push_back({.at = clock.now, .pdu = std::vector<uint8_t>(pdu.begin(), pdu.end())});
            return true;
        };
    }
};

/// One value's event as a PDU carried it, with the PDU's time.
struct TimedEvent
{
    TimePoint at{};
    AttributeEvent event{};
};

/// The events sent for the value @p unique_id, read from each PDU by @p carried.
template <class Carried>
auto events_of(TimedPdus const& pdus, uint16_t const unique_id, Carried carried) -> std::vector<TimedEvent>
{
    std::vector<TimedEvent> out{};
    for (auto const& sent : pdus.sent) {
        for (auto const& value : carried(std::span<uint8_t const>(sent.pdu))) {
            if (value.unique_id == unique_id) {
                out.push_back({.at = sent.at, .event = value.event});
            }
        }
    }
    return out;
}

void run(MsrpParticipant& p, TestClock& clock, int const steps)
{
    for (int i = 0; i < steps; ++i) {
        p.tick(clock.advance(std::chrono::milliseconds(10)));
    }
}

/// AVnu 35.e2.1 to 35.e2.5, observable b: New in the first two messages, one
/// JoinTime (200 ms) apart, then JoinMt no later than the next periodictimer.
void expect_new_twice_then_joinmt(std::vector<TimedEvent> const& events)
{
    EXPECT_TRUE(events.size() >= 3);
    if (events.size() < 3) {
        return;
    }
    EXPECT_EQ(events[0].event, AttributeEvent::New);
    EXPECT_EQ(events[1].event, AttributeEvent::New);
    EXPECT_TRUE(events[1].at - events[0].at == std::chrono::milliseconds(200));
    EXPECT_EQ(events[2].event, AttributeEvent::JoinMt);
    EXPECT_TRUE(events[2].at - events[1].at <= std::chrono::milliseconds(1200));
}

}  // namespace

// AVnu 35.e2.1 to 35.e2.5: registering a Talker Advertise, a Talker Failed and
// a Listener of each Declaration Type sends New twice within the JoinTime and
// JoinMt after that; deregistering sends Lv.
TEST(msrp_end_station, e2_a_registration_is_new_twice_within_the_join_time_and_a_deregistration_is_lv)
{
    MsrpParticipant a{test_msrp_config(), 0xe201};
    TestClock clock{};
    TimedPdus pdus{};
    a.set_send_pdu(pdus.sender(clock));
    a.start(clock.now);
    TalkerFailedFirstValue failed{};
    failed.advertise = make_talker_adv(2);
    failed.set_failure_code(FailureCode::InsufficientBandwidth);
    EXPECT_TRUE(is_success(a.declare_talker_advertise(make_talker_adv(1), clock.now)));
    EXPECT_TRUE(is_success(a.declare_talker_failed(failed, clock.now)));
    EXPECT_TRUE(is_success(a.declare_listener(make_stream_id(3), ListenerDeclaration::Ready, clock.now)));
    EXPECT_TRUE(is_success(a.declare_listener(make_stream_id(5), ListenerDeclaration::AskingFailed, clock.now)));
    EXPECT_TRUE(is_success(a.declare_listener(make_stream_id(7), ListenerDeclaration::ReadyFailed, clock.now)));
    run(a, clock, 300);

    auto const listeners = [](std::span<uint8_t const> pdu) { return test_support::listener_carried(pdu); };
    expect_new_twice_then_joinmt(events_of(pdus, 1, talker_advertise_carried));
    expect_new_twice_then_joinmt(events_of(pdus, 2, talker_failed_carried));
    expect_new_twice_then_joinmt(events_of(pdus, 3, listeners));
    expect_new_twice_then_joinmt(events_of(pdus, 5, listeners));
    expect_new_twice_then_joinmt(events_of(pdus, 7, listeners));

    EXPECT_TRUE(is_success(a.withdraw_talker(make_stream_id(1), clock.now)));
    EXPECT_TRUE(is_success(a.withdraw_talker(make_stream_id(2), clock.now)));
    EXPECT_TRUE(is_success(a.withdraw_listener(make_stream_id(3), clock.now)));
    run(a, clock, 100);
    EXPECT_EQ(events_of(pdus, 1, talker_advertise_carried).back().event, AttributeEvent::Lv);
    EXPECT_EQ(events_of(pdus, 2, talker_failed_carried).back().event, AttributeEvent::Lv);
    EXPECT_EQ(events_of(pdus, 3, listeners).back().event, AttributeEvent::Lv);
    // A registration that was not deregistered goes on as JoinMt.
    EXPECT_EQ(events_of(pdus, 5, listeners).back().event, AttributeEvent::JoinMt);
}

// AVnu 35.e2.7, observable a: a LeaveAll every 10 to 15 s, each one
// re-declaring the Domain in the same PDU.
TEST(msrp_end_station, e2_7_a_leaveall_comes_every_ten_to_fifteen_seconds_with_the_domain)
{
    MsrpParticipant a{test_msrp_config(), 0xe207};
    TestClock clock{};
    TimedPdus pdus{};
    a.set_send_pdu(pdus.sender(clock));
    a.start(clock.now);
    auto const started = clock.now;
    DomainFirstValue domain{};
    domain.sr_class_id = 6;
    EXPECT_TRUE(is_success(a.declare_domain(domain, clock.now)));
    run(a, clock, 12000);  // two minutes

    std::vector<TimePoint> leavealls{};
    for (auto const& sent : pdus.sent) {
        if (pdu_has_leaveall(sent.pdu)) {
            leavealls.push_back(sent.at);
            EXPECT_FALSE(domain_carried(sent.pdu).empty());
        }
    }
    EXPECT_TRUE(leavealls.size() >= 7);
    auto previous = started;
    for (auto const at : leavealls) {
        EXPECT_TRUE(at - previous >= std::chrono::seconds(10) && at - previous <= std::chrono::seconds(15));
        previous = at;
    }
}

// AVnu 35.e2.7, the rest of observable a: after a peer's LeaveAll the Domain
// goes out twice within a JoinTime.
TEST(msrp_end_station, e2_7_after_a_received_leaveall_the_domain_is_sent_twice_within_the_join_time)
{
    MsrpParticipant a{test_msrp_config(), 0xe217};
    MsrpParticipant peer{test_msrp_config(), 0xe218};
    TestClock clock{};
    TimedPdus a_pdus{};
    TimedPdus peer_pdus{};
    a.set_send_pdu(a_pdus.sender(clock));
    peer.set_send_pdu(peer_pdus.sender(clock));
    a.start(clock.now);
    DomainFirstValue domain{};
    domain.sr_class_id = 6;
    EXPECT_TRUE(is_success(a.declare_domain(domain, clock.now)));
    run(a, clock, 300);  // settled, and three seconds ahead of the peer's LeaveAll
    peer.start(clock.now);
    EXPECT_TRUE(is_success(peer.declare_domain(domain, clock.now)));

    std::optional<TimePoint> peer_leaveall{};
    size_t delivered = 0;
    for (int i = 0; i < 1200 && !peer_leaveall.has_value(); ++i) {
        auto const now = clock.advance(std::chrono::milliseconds(10));
        peer.tick(now);
        a.tick(now);
        for (; delivered < peer_pdus.sent.size(); ++delivered) {
            auto const& pdu = peer_pdus.sent[delivered].pdu;
            a_pdus.sent.clear();
            a.receive_pdu(std::span<uint8_t const>(pdu), now);
            peer_leaveall = pdu_has_leaveall(pdu) ? std::optional<TimePoint>{now} : peer_leaveall;
        }
    }
    EXPECT_TRUE(peer_leaveall.has_value());
    run(a, clock, 50);
    std::vector<TimePoint> domains{};
    for (auto const& sent : a_pdus.sent) {
        if (!domain_carried(sent.pdu).empty()) {
            domains.push_back(sent.at);
        }
    }
    EXPECT_EQ(domains.size(), 2u);
    EXPECT_TRUE(domains.size() == 2 && domains[0] == *peer_leaveall);
    EXPECT_TRUE(domains.size() == 2 && domains[1] - domains[0] <= std::chrono::milliseconds(200));
}

// AVnu 35.e3.1 and 35.e3.2: a Talker Advertise is indicated once, however many
// LeaveAlls pass while its talker keeps declaring it, and its deregistration
// is indicated once.
TEST(msrp_end_station, e3_1_e3_2_a_talker_advertise_is_indicated_once_and_its_leave_once)
{
    MsrpParticipant listener{test_msrp_config(), 0xe301};
    MsrpParticipant talker{test_msrp_config(), 0xe302};
    Fabric fab{};
    listener.set_send_pdu(fab.make_a_sender());
    talker.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    listener.start(clock.now);
    talker.start(clock.now);
    int registered = 0;
    int left = 0;
    Observer obs{};
    obs.on_talker_advertise = [&](TalkerAdvertiseFirstValue const&, Operation) { ++registered; };
    obs.on_talker_leave = [&](tsn::StreamId const&) { ++left; };
    listener.subscribe(obs);
    auto const both = [&](int const steps) {
        for (int i = 0; i < steps; ++i) {
            auto const now = clock.advance(std::chrono::milliseconds(10));
            listener.tick(now);
            talker.tick(now);
            fab.deliver_a_to_b(talker, now);
            fab.deliver_b_to_a(listener, now);
        }
    };

    EXPECT_TRUE(is_success(talker.declare_talker_advertise(make_talker_adv(0x0031), clock.now)));
    both(4000);  // forty seconds: several LeaveAlls from each side
    EXPECT_EQ(registered, 1);
    EXPECT_EQ(left, 0);

    EXPECT_TRUE(is_success(talker.withdraw_talker(make_stream_id(0x0031), clock.now)));
    both(300);
    EXPECT_EQ(registered, 1);
    EXPECT_EQ(left, 1);
}

// AVnu 35.e3.3: a talker that switches a registered stream from Talker Failed
// to Talker Advertise and back, with JoinMt and no Leave (IEEE 802.1Q 35.2.6),
// is indicated under each type in turn and never as having left.
TEST(msrp_end_station, e3_3_a_talker_switching_between_failed_and_advertise_is_never_indicated_as_left)
{
    MsrpParticipant a{test_msrp_config(), 0xe303};
    TestClock clock{};
    a.start(clock.now);
    std::vector<AttributeType> indicated{};
    int left = 0;
    Observer obs{};
    obs.on_talker_advertise = [&](TalkerAdvertiseFirstValue const&, Operation) {
        indicated.push_back(AttributeType::TalkerAdvertise);
    };
    obs.on_talker_failed = [&](TalkerFailedFirstValue const&, Operation) { indicated.push_back(AttributeType::TalkerFailed); };
    obs.on_talker_leave = [&](tsn::StreamId const&) { ++left; };
    a.subscribe(obs);
    auto const sid = make_stream_id(0x0033);
    std::array<uint8_t, 25> advertise{};
    (void)store_unchecked(std::span<uint8_t>(advertise), make_talker_adv(0x0033));
    TalkerFailedFirstValue failed_value{};
    failed_value.advertise = make_talker_adv(0x0033);
    failed_value.set_failure_code(FailureCode::InsufficientBandwidth);
    std::array<uint8_t, 34> failed{};
    (void)store_unchecked(std::span<uint8_t>(failed), failed_value);
    // The test station declares one type for 20 s, a JoinMt a second.
    auto const declare_for_20s = [&](AttributeType const type, uint8_t const length, std::span<uint8_t const> value) {
        for (int second = 0; second < 20; ++second) {
            auto const pdu = build_vector_pdu(type, length, value, {AttributeEvent::JoinMt});
            a.receive_pdu(std::span<uint8_t const>(pdu), clock.now);
            run(a, clock, 100);
        }
    };

    declare_for_20s(AttributeType::TalkerFailed, 34, failed);
    EXPECT_NE(a.find_registered_talker_failed(sid), nullptr);
    declare_for_20s(AttributeType::TalkerAdvertise, 25, advertise);
    EXPECT_NE(a.find_registered_talker_advertise(sid), nullptr);
    EXPECT_EQ(a.find_registered_talker_failed(sid), nullptr);
    declare_for_20s(AttributeType::TalkerFailed, 34, failed);
    EXPECT_NE(a.find_registered_talker_failed(sid), nullptr);
    EXPECT_EQ(a.find_registered_talker_advertise(sid), nullptr);

    std::vector<AttributeType> const expected{
        AttributeType::TalkerFailed, AttributeType::TalkerAdvertise, AttributeType::TalkerFailed};
    EXPECT_TRUE(indicated == expected);
    EXPECT_EQ(left, 0);
}

// AVnu 35.e3.5: two Talker Advertises that share one vector are each indicated
// once, and each deregistration once.
TEST(msrp_end_station, e3_5_two_talkers_in_one_vector_are_each_indicated_once)
{
    MsrpParticipant a{test_msrp_config(), 0xe305};
    TestClock clock{};
    a.start(clock.now);
    std::vector<int> log{};  // +unique_id registered, -unique_id left
    Observer obs{};
    obs.on_talker_advertise = [&](TalkerAdvertiseFirstValue const& fv, Operation) { log.push_back(fv.stream_id.get_unique_id()); };
    obs.on_talker_leave = [&](tsn::StreamId const& id) { log.push_back(-static_cast<int>(id.get_unique_id())); };
    a.subscribe(obs);
    std::array<uint8_t, 25> stream_a{};
    (void)store_unchecked(std::span<uint8_t>(stream_a), make_talker_adv(0x0051));
    auto stream_b_value = make_talker_adv(0x0052);
    stream_b_value.destination_address = ieee::Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x35};
    std::array<uint8_t, 25> stream_b{};
    (void)store_unchecked(std::span<uint8_t>(stream_b), stream_b_value);
    auto const receive = [&](std::span<uint8_t const> value, std::vector<AttributeEvent> const& events) {
        auto const pdu = build_vector_pdu(AttributeType::TalkerAdvertise, 25, value, events);
        a.receive_pdu(std::span<uint8_t const>(pdu), clock.now);
        run(a, clock, 150);
    };

    receive(stream_a, {AttributeEvent::JoinMt});
    receive(stream_a, {AttributeEvent::JoinMt, AttributeEvent::JoinMt});
    receive(stream_a, {AttributeEvent::Lv, AttributeEvent::JoinMt});
    receive(stream_b, {AttributeEvent::Lv});

    std::vector<int> const expected{0x0051, 0x0052, -0x0051, -0x0052};
    EXPECT_TRUE(log == expected);
}

// Milan v1.3 Table 4.3 and 4.2.7.2.2: Milan's LeaveTime is 5 s, which a
// Registrar in LV waits out, and the instant leave that goes with it ends a
// registration on the Lv itself.
TEST(msrp_end_station, milan_the_leave_time_is_five_seconds_and_an_instant_leave_ends_it_at_once)
{
    mrp::TimerConfig milan{};
    milan.leave_time = std::chrono::milliseconds(5000);
    MsrpParticipant a{test_msrp_config(), 0xe401};
    EXPECT_TRUE(is_success(a.set_mrp_timers(milan)));
    EXPECT_TRUE(a.mrp_timers() == milan);
    TestClock clock{};
    a.start(clock.now);
    int left = 0;
    Observer obs{};
    obs.on_talker_leave = [&](tsn::StreamId const&) { ++left; };
    a.subscribe(obs);
    std::array<uint8_t, 25> value{};
    (void)store_unchecked(std::span<uint8_t>(value), make_talker_adv(0x0041));
    auto const sid = make_stream_id(0x0041);
    auto const join = build_vector_pdu(AttributeType::TalkerAdvertise, 25, value, {AttributeEvent::JoinMt});
    auto const lv = build_vector_pdu(AttributeType::TalkerAdvertise, 25, value, {AttributeEvent::Lv});

    // Table 10-4 as written: the Lv starts the leavetimer, 5 s under Milan.
    a.receive_pdu(std::span<uint8_t const>(join), clock.now);
    a.receive_pdu(std::span<uint8_t const>(lv), clock.now);
    run(a, clock, 499);
    EXPECT_NE(a.find_registered_talker_advertise(sid), nullptr);
    EXPECT_EQ(left, 0);
    run(a, clock, 2);
    EXPECT_EQ(a.find_registered_talker_advertise(sid), nullptr);
    EXPECT_EQ(left, 1);

    // With the instant leave: gone on arrival, told once.
    a.set_instant_leave(true);
    a.receive_pdu(std::span<uint8_t const>(join), clock.now);
    EXPECT_NE(a.find_registered_talker_advertise(sid), nullptr);
    a.receive_pdu(std::span<uint8_t const>(lv), clock.now);
    EXPECT_EQ(a.find_registered_talker_advertise(sid), nullptr);
    EXPECT_EQ(left, 2);

    mrp::TimerConfig bad{};
    bad.join_time = std::chrono::milliseconds(0);
    EXPECT_TRUE(is_failure(a.set_mrp_timers(bad)));
    EXPECT_TRUE(a.mrp_timers() == milan);
}

// Milan v1.3 4.2.7.1.2: what follows an invalid field is discarded, in the
// same AttributeList and in every later Message of the MRPDU; what came
// before it has been processed. Here three Messages: a good Talker Advertise,
// a Listener vector whose NumberOfValues overruns its list, and another good
// Talker Advertise.
TEST(msrp_end_station, milan_what_follows_an_invalid_field_is_discarded_with_every_later_message)
{
    MsrpParticipant a{test_msrp_config(), 0xe402};
    TestClock clock{};
    a.start(clock.now);
    auto const message_of = [](std::vector<uint8_t> const& pdu) { return std::vector<uint8_t>(pdu.begin() + 1, pdu.end() - 2); };
    std::array<uint8_t, 25> first{};
    (void)store_unchecked(std::span<uint8_t>(first), make_talker_adv(0x0061));
    std::array<uint8_t, 25> later{};
    (void)store_unchecked(std::span<uint8_t>(later), make_talker_adv(0x0069));
    std::array<uint8_t, 8> listener{};
    (void)store_unchecked(std::span<uint8_t>(listener), ListenerFirstValue{.stream_id = make_stream_id(0x0065)});
    auto good_first = message_of(build_vector_pdu(AttributeType::TalkerAdvertise, 25, first, {AttributeEvent::JoinMt}));
    auto bad = message_of(build_vector_pdu(AttributeType::Listener, 8, listener, {AttributeEvent::JoinMt}, {2}));
    auto good_later = message_of(build_vector_pdu(AttributeType::TalkerAdvertise, 25, later, {AttributeEvent::JoinMt}));
    bad[5] = 200;  // NumberOfValues: 200 values cannot be in this list

    std::vector<uint8_t> pdu{PROTOCOL_VERSION};
    pdu.insert(pdu.end(), good_first.begin(), good_first.end());
    pdu.insert(pdu.end(), bad.begin(), bad.end());
    pdu.insert(pdu.end(), good_later.begin(), good_later.end());
    pdu.insert(pdu.end(), {0x00, 0x00});
    a.receive_pdu(std::span<uint8_t const>(pdu), clock.now);

    EXPECT_NE(a.find_registered_talker_advertise(make_stream_id(0x0061)), nullptr);
    EXPECT_EQ(a.find_listener(make_stream_id(0x0065)), nullptr);
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(0x0069)), nullptr);
    EXPECT_EQ(a.talker_advertise_count(), 1u);
    EXPECT_EQ(a.listener_count(), 0u);
}

// The same within one vector: the values before an AttributeEvent outside the
// six are processed, the ones after it are not.
TEST(msrp_end_station, milan_the_values_after_an_invalid_event_are_discarded)
{
    MsrpParticipant a{test_msrp_config(), 0xe403};
    TestClock clock{};
    a.start(clock.now);
    std::array<uint8_t, 25> value{};
    (void)store_unchecked(std::span<uint8_t>(value), make_talker_adv(0x0071));
    auto pdu = build_vector_pdu(
        AttributeType::TalkerAdvertise,
        25,
        value,
        {AttributeEvent::JoinMt, AttributeEvent::JoinMt, AttributeEvent::JoinMt, AttributeEvent::JoinMt, AttributeEvent::JoinMt});
    // The second three-packed octet (after the 5 header octets, the vector
    // header and the 25-octet FirstValue, then the first packed octet) holds
    // the fourth and fifth events: 250 unpacks its first event as 6.
    pdu[33] = 250;
    a.receive_pdu(std::span<uint8_t const>(pdu), clock.now);

    EXPECT_NE(a.find_registered_talker_advertise(make_stream_id(0x0071)), nullptr);
    EXPECT_NE(a.find_registered_talker_advertise(make_stream_id(0x0073)), nullptr);
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(0x0074)), nullptr);
    EXPECT_EQ(a.find_talker_advertise(make_stream_id(0x0075)), nullptr);
    EXPECT_EQ(a.talker_advertise_count(), 3u);
}

// Declaring the type that already stands changes nothing a peer has to act
// on: it stays a Join (a second input attaching to the same stream does this).
TEST(msrp_participant, a_listener_redeclared_with_the_type_that_stands_is_not_new)
{
    MsrpParticipant a{test_msrp_config(), 0xd103};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    auto const sid = make_stream_id(0x0052);
    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::Ready, clock.now)));
    for (int i = 0; i < 12; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    fab.a_to_b.clear();

    EXPECT_TRUE(is_success(a.declare_listener(sid, ListenerDeclaration::Ready, clock.now)));
    a.transmit_pending(clock.now);
    for (int i = 0; i < 12; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    auto const carried = test_support::listener_carried(fab.a_to_b);
    EXPECT_FALSE(carried.empty());
    for (auto const& value : carried) {
        EXPECT_TRUE(value.event != AttributeEvent::New && value.event != AttributeEvent::Lv);
        EXPECT_TRUE(value.declaration == ListenerDeclaration::Ready);
    }
}

TEST(msrp_participant, declared_attribute_reasserts_each_periodic)
{
    // Regression (IEEE 802.1Q-2014 Clause 10.7.5.23): a declared attribute MUST
    // keep being re-transmitted on the ~1 s PeriodicTransmissionTime
    // (Applicant Qa -> Aa -> JoinIn) so a bridge/switch registrar stays fresh.
    // The failure mode this guards: a participant that emits the initial
    // declaration once and then goes silent lets the switch's registration age
    // out, dropping the reserved stream. Over 5 s we must see multiple periodic
    // re-assertions.
    MsrpParticipant a{test_msrp_config(), 0xaa11};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    EXPECT_TRUE(is_success(a.declare_listener(make_stream_id(0x0001), ListenerDeclaration::Ready, clock.now)));

    // Flush the initial New/JoinIn burst, then ignore it.
    for (int i = 0; i < 10; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(50)));
    }

    // Drive 20 s at 50 ms granularity — long enough to cross at least one
    // LeaveAll cycle (10-15 s). Count re-assertions in the FINAL 5 s: a
    // healthy participant keeps re-Joining (PeriodicTime = 1 s) and re-Joins
    // after each LeaveAll, so the tail window must still carry traffic. A
    // participant that goes silent after the first declaration (or drops the
    // attribute on LeaveAll) emits 0 here.
    for (int i = 0; i < 300; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(50)));
    }
    fab.a_to_b.clear();
    for (int i = 0; i < 100; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(50)));
    }
    EXPECT_TRUE(fab.a_to_b.size() >= 3);
}

//
// Multi-value VectorAttribute decode (AVnu MSRP.End.c.35.1.11): a vector with
// NumberOfValues > 1 carries ONE FirstValue; each subsequent value is the
// previous one incremented per the attribute-type-specific rule.
//

TEST(msrp_participant_decode, talker_multi_value_increments_unique_id_and_dest_mac)
{
    // 35.1.11: Talker Advertise increments BOTH the StreamID Unique ID and the
    // DataFrameParameters destination_address per value. Decoding a 2-value
    // vector must yield a second talker whose dest MAC is base + 1, not base.
    MsrpParticipant p{test_msrp_config(), 0xa00d};
    TestClock clock{};
    p.start(clock.now);
    auto const base = make_talker_adv(0x0001);
    std::array<uint8_t, 25> fv{};
    (void)store_unchecked(std::span<uint8_t>(fv), base);
    auto const pdu = build_vector_pdu(AttributeType::TalkerAdvertise, 25, fv, {AttributeEvent::New, AttributeEvent::New});
    p.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), clock.now);

    EXPECT_EQ(p.talker_advertise_count(), 2u);
    auto const* second = p.find_talker_advertise(make_stream_id(0x0002));
    EXPECT_NE(second, nullptr);
    if (second != nullptr) {
        EXPECT_EQ(second->destination_address.to_uint64(), base.destination_address.to_uint64() + 1);
    }
}

TEST(msrp_participant_decode, domain_multi_value_increments_class_id_and_priority)
{
    // 35.1.11: Domain increments BOTH SRclassID and SRclassPriority per value.
    // A 2-value vector based at class B (5, prio 2) must also register class A
    // (6, prio 3) -- every value past index 0 must register, not be dropped.
    MsrpParticipant p{test_msrp_config(), 0xa00e};
    TestClock clock{};
    p.start(clock.now);
    DomainFirstValue base{};
    base.sr_class_id = SR_CLASS_B;  // 5
    base.sr_class_priority = 2;
    base.sr_class_vid = default_sr_class_vid;
    std::array<uint8_t, 4> fv{};
    (void)store_unchecked(std::span<uint8_t>(fv), base);
    auto const pdu = build_vector_pdu(AttributeType::Domain, 4, fv, {AttributeEvent::New, AttributeEvent::New});
    p.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), clock.now);

    EXPECT_EQ(p.domain_count(), 2u);
    auto const* second = p.find_domain(SR_CLASS_A);  // 6
    EXPECT_NE(second, nullptr);
    if (second != nullptr) {
        EXPECT_EQ(second->sr_class_priority.get(), 3);
        EXPECT_EQ(second->sr_class_vid.get(), default_sr_class_vid);  // VID is NOT incremented
    }
}

//
// Multi-value VectorAttribute encode (coalescing): a run of declared attributes
// whose FirstValues form a 35.1.11 increment chain is emitted as ONE vector with
// NumberOfValues > 1, not a series of singleton vectors.
//

TEST(msrp_participant, consecutive_listeners_coalesce_into_one_vector)
{
    // Three listeners with consecutive Unique IDs (same system address) form a
    // Listener increment chain, so they must be packed into a single
    // VectorAttribute with NumberOfValues == 3.
    MsrpParticipant a{test_msrp_config(), 0xa00f};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    (void)a.declare_listener(make_stream_id(0x0001), ListenerDeclaration::Ready, clock.now);
    (void)a.declare_listener(make_stream_id(0x0002), ListenerDeclaration::Ready, clock.now);
    (void)a.declare_listener(make_stream_id(0x0003), ListenerDeclaration::Ready, clock.now);
    a.tick(clock.advance(std::chrono::milliseconds(250)));
    EXPECT_TRUE(!fab.a_to_b.empty());

    std::vector<uint16_t> nvals{};
    for (auto const& pdu : fab.a_to_b) {
        for (uint16_t const n : vector_num_values(std::span<uint8_t const>(pdu.data(), pdu.size()), AttributeType::Listener)) {
            nvals.push_back(n);
        }
    }
    EXPECT_EQ(nvals.size(), 1u);
    if (nvals.size() == 1) {
        EXPECT_EQ(nvals[0], 3u);
    }
}

TEST(msrp_participant, coalesced_listeners_round_trip_to_peer)
{
    // The coalesced multi-value vector must decode back to the exact same set of
    // attributes on the peer (encode/decode symmetry -- the heart of 35.1.11).
    MsrpParticipant a{test_msrp_config(), 0xa101};
    MsrpParticipant b{test_msrp_config(), 0xb102};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    (void)a.declare_listener(make_stream_id(0x0001), ListenerDeclaration::Ready, clock.now);
    (void)a.declare_listener(make_stream_id(0x0002), ListenerDeclaration::Ready, clock.now);
    (void)a.declare_listener(make_stream_id(0x0003), ListenerDeclaration::Ready, clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);

    EXPECT_EQ(b.listener_count(), 3u);
    for (uint16_t uid = 1; uid <= 3; ++uid) {
        auto const* rec = b.find_listener(make_stream_id(uid));
        EXPECT_NE(rec, nullptr);
        if (rec != nullptr) {
            EXPECT_EQ(static_cast<int>(rec->substate), static_cast<int>(ListenerDeclaration::Ready));
        }
    }
}

TEST(msrp_participant, non_consecutive_listeners_stay_separate_vectors)
{
    // Listeners whose Unique IDs do NOT form an increment chain must not be
    // coalesced -- each is its own singleton vector. Guards against the encoder
    // collapsing unrelated streams into one (mis-)derived vector.
    MsrpParticipant a{test_msrp_config(), 0xa103};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);
    (void)a.declare_listener(make_stream_id(0x0001), ListenerDeclaration::Ready, clock.now);
    (void)a.declare_listener(make_stream_id(0x0005), ListenerDeclaration::Ready, clock.now);
    a.tick(clock.advance(std::chrono::milliseconds(250)));

    std::vector<uint16_t> nvals{};
    for (auto const& pdu : fab.a_to_b) {
        for (uint16_t const n : vector_num_values(std::span<uint8_t const>(pdu.data(), pdu.size()), AttributeType::Listener)) {
            nvals.push_back(n);
        }
    }
    EXPECT_EQ(nvals.size(), 2u);
    for (uint16_t const n : nvals) {
        EXPECT_EQ(n, 1u);
    }
}

TEST(msrp_participant, domain_class_b_then_a_coalesce_and_round_trip)
{
    // SR class B (id 5, prio 2) and class A (id 6, prio 3) form a Domain
    // increment chain, so a station advertising both coalesces them into one
    // 2-value Domain vector (AVnu 35.1.11 Part B). It must round-trip to the peer
    // as both domains.
    MsrpParticipant a{test_msrp_config(), 0xa104};
    MsrpParticipant b{test_msrp_config(), 0xb105};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    DomainFirstValue dom_b{};
    dom_b.sr_class_id = SR_CLASS_B;  // 5
    dom_b.sr_class_priority = 2;
    dom_b.sr_class_vid = default_sr_class_vid;
    DomainFirstValue dom_a{};
    dom_a.sr_class_id = SR_CLASS_A;  // 6
    dom_a.sr_class_priority = 3;
    dom_a.sr_class_vid = default_sr_class_vid;
    (void)a.declare_domain(dom_b, clock.now);  // declared B first so container order chains B -> A
    (void)a.declare_domain(dom_a, clock.now);
    auto const t1 = clock.advance(std::chrono::milliseconds(250));
    a.tick(t1);

    std::vector<uint16_t> nvals{};
    for (auto const& pdu : fab.a_to_b) {
        for (uint16_t const n : vector_num_values(std::span<uint8_t const>(pdu.data(), pdu.size()), AttributeType::Domain)) {
            nvals.push_back(n);
        }
    }
    EXPECT_EQ(nvals.size(), 1u);
    if (nvals.size() == 1) {
        EXPECT_EQ(nvals[0], 2u);
    }

    fab.deliver_a_to_b(b, t1);
    EXPECT_EQ(b.domain_count(), 2u);
    EXPECT_NE(b.find_domain(SR_CLASS_B), nullptr);
    EXPECT_NE(b.find_domain(SR_CLASS_A), nullptr);
}

// ===========================================================================
// PDU buffer overflow (S1): a message that does not fit is skipped whole, never
// emitted as a truncated length-prefix that desyncs the receiver.
// ===========================================================================

namespace {

// Mirror of receive_pdu's bounds discipline: an MSRPDU is well-formed iff every
// message fits within the buffer (its attribute_list_length never claims bytes
// past the end -- the S1 truncation bug) and it terminates with an END_MARK.
auto msrpdu_well_formed(std::span<uint8_t const> pdu) -> bool
{
    if (pdu.empty()) {
        return false;
    }
    size_t pos = 1;  // skip PROTOCOL_VERSION
    while (pos + 2 <= pdu.size()) {
        uint16_t const end_check = (static_cast<uint16_t>(pdu[pos]) << 8) | pdu[pos + 1];
        if (end_check == 0) {
            return true;  // END_MARK terminator
        }
        if (pos + 4 > pdu.size()) {
            return false;  // header itself truncated
        }
        uint16_t const attr_list_length = (static_cast<uint16_t>(pdu[pos + 2]) << 8) | pdu[pos + 3];
        size_t const msg_end = pos + 4 + attr_list_length;
        if (msg_end > pdu.size()) {
            return false;  // message claims more bytes than present -> truncated
        }
        pos = msg_end;
    }
    return false;  // ran off the end with no END_MARK
}

}  // namespace

TEST(msrp_participant, oversized_declaration_set_never_emits_truncated_pdu)
{
    // Full storage tables (default 32/16/32) all near-max: TalkerAdvertise +
    // TalkerFailed + Listener together pack to ~1.9 KB, past one 1500-byte MSRPDU.
    MsrpConfig const cfg{
        .max_talker_advertise = 32,
        .max_talker_failed = 16,
        .max_listeners = 32,
        .max_domains = 8,
        .max_observers = 16,
        .max_interesting_stream_ids = 32,
    };
    MsrpParticipant p{cfg, 0xabc};
    std::vector<std::vector<uint8_t>> pdus;
    p.set_send_pdu([&](std::span<uint8_t const> pdu) -> bool {
        pdus.emplace_back(pdu.begin(), pdu.end());
        return true;
    });
    TestClock clock{};
    p.start(clock.now);

    // Declare NON-consecutive stream IDs so each is its own singleton vector run (no
    // coalescing) -> large packed encoding. All pending together, flushed by the
    // single join-timer tick below.
    for (uint16_t i = 0; i < 32; ++i) {
        (void)p.declare_talker_advertise(make_talker_adv(static_cast<uint16_t>(1 + (i * 3))), clock.now);
    }
    for (uint16_t i = 0; i < 16; ++i) {
        TalkerFailedFirstValue tfv{};
        tfv.advertise = make_talker_adv(static_cast<uint16_t>(1000 + (i * 3)));
        tfv.set_failure_code(FailureCode::InsufficientBandwidth);
        (void)p.declare_talker_failed(tfv, clock.now);
    }
    for (uint16_t i = 0; i < 32; ++i) {
        (void)p.declare_listener(make_stream_id(static_cast<uint16_t>(2000 + (i * 3))), ListenerDeclaration::Ready, clock.now);
    }

    // Fire the join timer -> one build with everything pending -> the overflow path.
    p.tick(clock.advance(std::chrono::milliseconds(200)));

    // Every emitted PDU is well-formed: no message ever claims more bytes than are
    // present (the S1 truncation bug would fail this).
    EXPECT_FALSE(pdus.empty());
    for (auto const& pdu : pdus) {
        EXPECT_TRUE(msrpdu_well_formed(std::span<uint8_t const>(pdu.data(), pdu.size())));
    }
    // The declared set exceeds one PDU, so at least one whole message must have been
    // skipped (its records stay pending and retransmit) -- the observability counter.
    EXPECT_TRUE(p.pdu_message_skip_count() > 0);
}

// ===========================================================================
// StreamIdInUseByAnotherTalker (S2): a peer declaring a StreamID WE declare must
// not overwrite our record's payload (which we would then re-advertise as ours).
// ===========================================================================

TEST(msrp_participant, peer_declaration_does_not_overwrite_our_talker)
{
    MsrpParticipant a{test_msrp_config(), 0xa100};
    MsrpParticipant b{test_msrp_config(), 0xb200};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);

    ieee::Eui48 const our_mac{0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    ieee::Eui48 const foreign_mac{0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB};

    // A declares stream S with OUR dest MAC.
    auto ta_ours = make_talker_adv(0x0001);
    ta_ours.destination_address = our_mac;
    EXPECT_TRUE(bool(a.declare_talker_advertise(ta_ours, clock.now)));
    a.tick(clock.advance(std::chrono::milliseconds(250)));

    // B declares the SAME stream S with a DIFFERENT dest MAC; A receives it.
    auto ta_foreign = make_talker_adv(0x0001);
    ta_foreign.destination_address = foreign_mac;
    EXPECT_TRUE(bool(b.declare_talker_advertise(ta_foreign, clock.now)));
    b.tick(clock.advance(std::chrono::milliseconds(250)));
    fab.deliver_b_to_a(a, clock.now);

    // A must NOT have adopted B's foreign dest MAC into our own declaration -- our
    // record still carries our value (had it adopted B's, this would be foreign_mac).
    auto const* fv = a.find_talker_advertise(make_stream_id(0x0001));
    EXPECT_NE(fv, nullptr);
    if (fv != nullptr) {
        EXPECT_TRUE(fv->destination_address == our_mac);
    }
    // The collision is observable.
    EXPECT_TRUE(a.foreign_declaration_conflict_count() > 0);
}

TEST(msrp_participant, transmit_pending_sends_a_withdrawal_without_waiting_for_the_join_timer)
{
    MsrpParticipant a{test_msrp_config(), 0xcc33};
    MsrpParticipant b{test_msrp_config(), 0xdd44};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    std::optional<tsn::StreamId> leave_id{};
    Observer obs{};
    obs.on_talker_leave = [&](tsn::StreamId const& id) { leave_id = id; };
    b.subscribe(obs);

    (void)a.declare_talker_advertise(make_talker_adv(0x0042), clock.now);
    a.tick(clock.advance(std::chrono::milliseconds(250)));  // past JoinTime (IEEE 802.1Q-2018 Table 10-7)
    fab.deliver_a_to_b(b, clock.now);
    EXPECT_NE(b.find_talker_advertise(make_stream_id(0x0042)), nullptr);

    // A stopping station: the withdrawal and its transmit opportunity at the
    // same instant, no join timer in between. The Leave is on the fabric at
    // once...
    (void)a.withdraw_talker(make_stream_id(0x0042), clock.now);
    EXPECT_TRUE(fab.a_to_b.empty());
    a.transmit_pending(clock.now);
    EXPECT_FALSE(fab.a_to_b.empty());

    // ...and the peer, hearing nothing more from a, drops the registration at
    // its leave timer.
    fab.deliver_a_to_b(b, clock.now);
    b.tick(clock.advance(std::chrono::milliseconds(1100)));
    EXPECT_TRUE(leave_id.has_value());
    if (leave_id.has_value()) {
        EXPECT_EQ(leave_id->get_unique_id(), 0x0042);
    }
}

namespace {
/// Fill @p a with 32 Talker Advertises and 15 Talker Faileds beside the one under
/// test, each on its own stream id two apart so no two coalesce into one vector.
/// On the wire that is 903 bytes of Talker Advertise and 600 of Talker Failed: the
/// second message does not fit beside the first in one 1500-byte PDU and waits
/// for a later one.
auto overflow_msrp_config() -> MsrpConfig
{
    return MsrpConfig{
        .max_talker_advertise = 32,
        .max_talker_failed = 16,
        .max_listeners = 8,
        .max_domains = 2,
        .max_observers = 4,
        .max_interesting_stream_ids = 4,
    };
}

void declare_a_full_pdu_of_talkers(MsrpParticipant& a, TimePoint now)
{
    for (uint16_t i = 0; i < 32; ++i) {
        EXPECT_TRUE(static_cast<bool>(a.declare_talker_advertise(make_talker_adv(static_cast<uint16_t>(0x1000 + 2 * i)), now)));
    }
    for (uint16_t i = 0; i < 15; ++i) {
        TalkerFailedFirstValue fv{};
        fv.advertise = make_talker_adv(static_cast<uint16_t>(0x2000 + 2 * i));
        fv.failure_code = 1;
        EXPECT_TRUE(static_cast<bool>(a.declare_talker_failed(fv, now)));
    }
}

/// The event the PDUs in @p pdus carry for Talker Failed @p uid, if any.
auto talker_failed_event(std::vector<std::vector<uint8_t>> const& pdus, uint16_t uid) -> std::optional<AttributeEvent>
{
    for (auto const& pdu : pdus) {
        for (auto const& carried : talker_failed_carried(pdu)) {
            if (carried.unique_id == uid) {
                return carried.event;
            }
        }
    }
    return std::nullopt;
}
}  // namespace

// IEEE 802.1Q 10.7.6.3: an attribute type that does not fit in this PDU waits
// for the next. The Applicants of its records had already taken their tx!
// transition when the message was built, so on the next pass they used to be
// dispatched tx! again from the new state: a Talker Failed whose Lv was skipped
// for space (La + tx! had gone to Vo) came out as an In, or not at all, and the
// peer kept the registration. A skipped message now puts its Applicants back
// where they stood, and the settled talkers' optional JoinIn refreshes yield
// the room: the next PDU carries the Lv.
TEST(msrp_participant, a_leave_skipped_for_space_rides_the_next_pdu)
{
    MsrpParticipant a{overflow_msrp_config(), 0xcc33};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    TestClock clock{};
    a.start(clock.now);

    // The Talker Failed under test is declared alone first, so it is the one
    // record whose Applicant is settled (Qa) before the withdrawal.
    TalkerFailedFirstValue const under_test = [] {
        TalkerFailedFirstValue fv{};
        fv.advertise = make_talker_adv(0x0777);
        fv.failure_code = 1;
        return fv;
    }();
    EXPECT_TRUE(static_cast<bool>(a.declare_talker_failed(under_test, clock.now)));
    for (int i = 0; i < 3; ++i) {
        a.tick(clock.advance(std::chrono::milliseconds(250)));
    }
    EXPECT_TRUE(talker_failed_event(fab.a_to_b, 0x0777).has_value());
    fab.a_to_b.clear();

    // Withdraw it, and in the same join period fill the PDU with talkers so
    // the Talker Failed message (its Lv among them) does not fit this time.
    EXPECT_TRUE(static_cast<bool>(a.withdraw_talker(make_stream_id(0x0777), clock.now)));
    declare_a_full_pdu_of_talkers(a, clock.now);
    a.tick(clock.advance(std::chrono::milliseconds(250)));
    EXPECT_FALSE(fab.a_to_b.empty());
    EXPECT_TRUE(a.pdu_message_skip_count() >= 1U);
    EXPECT_FALSE(talker_failed_event(fab.a_to_b, 0x0777).has_value());

    // The Talker Advertises need three passes to settle; the Talker Failed
    // message rides the first PDU with room for it, Lv intact.
    std::optional<AttributeEvent> lv_seen{};
    for (int i = 0; i < 4 && !lv_seen.has_value(); ++i) {
        fab.a_to_b.clear();
        a.tick(clock.advance(std::chrono::milliseconds(250)));
        lv_seen = talker_failed_event(fab.a_to_b, 0x0777);
    }
    EXPECT_TRUE(lv_seen.has_value() && *lv_seen == AttributeEvent::Lv);
}

// Foreign Talker Advertises a bridge propagates get only max - reserved slots: our own Talker
// Advertises, declared later, still declare, and the one our listener stream is bound to registers.
TEST(msrp_participant, foreign_talker_advertises_cannot_starve_own_declarations)
{
    auto const run = [](size_t reserved, bool expect_own_ok) {
        MsrpConfig cfg = test_msrp_config();  // 8 Talker Advertises, 8 Listeners
        cfg.reserved_talker_advertise = reserved;
        cfg.reserved_listeners = reserved;
        MsrpParticipant a{cfg, 0xa5a5};
        MsrpConfig bridge_cfg = test_msrp_config();
        bridge_cfg.max_talker_advertise = 16;
        bridge_cfg.max_listeners = 16;
        MsrpParticipant b{bridge_cfg, 0xb6b6};
        Fabric fab{};
        a.set_send_pdu(fab.make_a_sender());
        b.set_send_pdu(fab.make_b_sender());
        TestClock clock{};
        a.start(clock.now);
        b.start(clock.now);

        // The "bridge" propagates 8 foreign advertises and 8 foreign Listeners.
        for (uint16_t uid = 0x0100; uid < 0x0108; ++uid) {
            EXPECT_TRUE(is_success(b.declare_talker_advertise(make_talker_adv(uid), clock.now)));
            EXPECT_TRUE(is_success(b.declare_listener(make_stream_id(uid + 0x0100), ListenerDeclaration::Ready, clock.now)));
        }
        b.tick(clock.advance(std::chrono::milliseconds(250)));
        fab.deliver_b_to_a(a, clock.now);
        EXPECT_EQ(a.talker_advertise_count(), 8u - reserved);
        EXPECT_EQ(a.listener_count(), 8u - reserved);

        // Our two talker streams and our listener stream, declared later.
        bool const t1 = is_success(a.declare_talker_advertise(make_talker_adv(0x0001), clock.now));
        bool const t2 = is_success(a.declare_talker_advertise(make_talker_adv(0x0002), clock.now));
        bool const l1 = is_success(a.declare_listener(make_stream_id(0x0300), ListenerDeclaration::AskingFailed, clock.now));
        EXPECT_EQ(t1 && t2 && l1, expect_own_ok);
        if (!expect_own_ok) {
            return;
        }

        // The talker our listener stream is bound to comes up: its Advertise
        // registers, as do the peer Listeners for our talker streams.
        EXPECT_TRUE(is_success(b.declare_talker_advertise(make_talker_adv(0x0300), clock.now)));
        EXPECT_TRUE(is_success(b.declare_listener(make_stream_id(0x0001), ListenerDeclaration::Ready, clock.now)));
        // Yet more foreign traffic is still held to the remainder.
        EXPECT_TRUE(is_success(b.declare_talker_advertise(make_talker_adv(0x0108), clock.now)));
        b.tick(clock.advance(std::chrono::milliseconds(250)));
        fab.deliver_b_to_a(a, clock.now);
        EXPECT_NE(a.find_talker_advertise(make_stream_id(0x0300)), nullptr);
        EXPECT_EQ(a.find_talker_advertise(make_stream_id(0x0108)), nullptr);
        EXPECT_NE(a.find_listener(make_stream_id(0x0001)), nullptr);
        EXPECT_TRUE(a.listener_permits_transmit(make_stream_id(0x0001)));
        EXPECT_EQ(a.talker_advertise_count(), 8u - reserved + 3u);
    };
    run(0, false);  // unreserved: the foreign advertises refuse ours
    run(4, true);   // 2 talker + 2 listener streams' worth reserved
}

// IEEE 802.1Q-2022 Clause 35.2.2.8.3: a listener that knows only a stream's destination and VID
// finds the registered Talker Advertise (or Failed) carrying them; our own declarations and other
// VIDs do not match, and a talker that leaves is no longer found.
TEST(msrp_participant, a_registered_talker_is_found_by_destination_and_vid)
{
    MsrpParticipant a{test_msrp_config(), 0xaa31};
    MsrpParticipant b{test_msrp_config(), 0xbb32};
    Fabric fab{};
    a.set_send_pdu(fab.make_a_sender());
    b.set_send_pdu(fab.make_b_sender());
    TestClock clock{};
    a.start(clock.now);
    b.start(clock.now);
    ieee::Eui48 const group_a{0x01, 0x00, 0x5e, 0x01, 0x02, 0x03};
    ieee::Eui48 const group_b{0x01, 0x00, 0x5e, 0x01, 0x02, 0x04};
    ieee::Eui48 const group_c{0x01, 0x00, 0x5e, 0x01, 0x02, 0x05};
    auto first = make_talker_adv(0x0101);
    first.destination_address = group_a;
    auto second = make_talker_adv(0x0102);
    second.destination_address = group_b;
    TalkerFailedFirstValue failed{};
    failed.advertise = make_talker_adv(0x0103);
    failed.advertise.destination_address = group_c;
    failed.set_failure_code(FailureCode::InsufficientBandwidth);
    EXPECT_TRUE(bool(a.declare_talker_advertise(first, clock.now)));
    EXPECT_TRUE(bool(a.declare_talker_advertise(second, clock.now)));
    EXPECT_TRUE(bool(a.declare_talker_failed(failed, clock.now)));
    auto const run = [&](int const rounds) {
        for (int i = 0; i < rounds; ++i) {
            auto const t = clock.advance(std::chrono::milliseconds(250));
            a.tick(t);
            b.tick(t);
            fab.deliver_a_to_b(b, t);
            fab.deliver_b_to_a(a, t);
        }
    };
    run(4);

    auto const* found = b.find_registered_talker_advertise(group_b, default_sr_class_vid);
    EXPECT_NE(found, nullptr);
    if (found != nullptr) {
        EXPECT_EQ(found->stream_id.get_unique_id(), 0x0102);
    }
    EXPECT_NE(b.find_registered_talker_advertise(group_a, default_sr_class_vid), nullptr);
    EXPECT_EQ(b.find_registered_talker_advertise(group_a, 3), nullptr);
    EXPECT_EQ(b.find_registered_talker_advertise(group_c, default_sr_class_vid), nullptr);
    EXPECT_EQ(a.find_registered_talker_advertise(group_a, default_sr_class_vid), nullptr);  // ours, not registered
    auto const* refused = b.find_registered_talker_failed(group_c, default_sr_class_vid);
    EXPECT_NE(refused, nullptr);
    if (refused != nullptr) {
        EXPECT_TRUE(refused->get_failure_code() == FailureCode::InsufficientBandwidth);
    }
    EXPECT_EQ(b.find_registered_talker_failed(group_a, default_sr_class_vid), nullptr);

    EXPECT_TRUE(bool(a.withdraw_talker(first.stream_id, clock.now)));
    run(12);
    EXPECT_EQ(b.find_registered_talker_advertise(group_a, default_sr_class_vid), nullptr);
    EXPECT_NE(b.find_registered_talker_advertise(group_b, default_sr_class_vid), nullptr);
}

TEST_MAIN(statusbar_srp, srp_msrp_participant_test)
