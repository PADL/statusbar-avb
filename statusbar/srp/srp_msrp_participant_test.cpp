// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_msrp_participant.hpp"

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/srp/srp_msrp_format.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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
        auto const tp = clock.advance(std::chrono::milliseconds(150));
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
    a.tick(clock.advance(std::chrono::milliseconds(150)));
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
    a.tick(clock.advance(std::chrono::milliseconds(150)));
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
    a.tick(clock.advance(std::chrono::milliseconds(150)));
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
    a.tick(clock.advance(std::chrono::milliseconds(150)));

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
        a.tick(clock.advance(std::chrono::milliseconds(150)));

        // b is a downstream LISTENER for 0xA001; deliver its Listener-Ready to a so
        // a holds a REGISTERED (operation=Register) Listener record for its stream.
        (void)b.declare_listener(make_stream_id(0xA001), ListenerDeclaration::Ready, clock.now);
        bfab.a_to_b.clear();
        b.tick(clock.advance(std::chrono::milliseconds(150)));
        for (auto const& pdu : bfab.a_to_b) {
            a.receive_pdu(std::span<uint8_t const>(pdu.data(), pdu.size()), clock.now);
        }

        // Run a periodic + LeaveAll pass on a and see whether a Listener attribute
        // (the only one a could emit -- a never DECLARED a listener) goes out.
        fab.a_to_b.clear();
        a.tick(clock.advance(std::chrono::milliseconds(150)));
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
        a.tick(clock.advance(std::chrono::milliseconds(150)));
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
    a.tick(t1);
    fab.deliver_a_to_b(b, t1);
    auto const stream_id = make_stream_id(0x0042);
    (void)a.withdraw_talker(stream_id, clock.now);
    auto const t2 = clock.advance(std::chrono::milliseconds(150));
    a.tick(t2);
    fab.deliver_a_to_b(b, t2);
    auto const t3 = clock.advance(std::chrono::milliseconds(1100));
    b.tick(t3);
    EXPECT_TRUE(leave_id.has_value());
    if (leave_id.has_value()) {
        EXPECT_EQ(leave_id->get_unique_id(), 0x0042);
    }
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
    b.tick(t1);
    fab.deliver_b_to_a(a, t1);
    EXPECT_TRUE(listener_seen.has_value());
    if (listener_seen.has_value()) {
        EXPECT_EQ(std::get<0>(*listener_seen).get_unique_id(), 0x0099);
        EXPECT_EQ(static_cast<int>(std::get<1>(*listener_seen)), static_cast<int>(ListenerDeclaration::Ready));
    }
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
    auto t = clock.advance(std::chrono::milliseconds(150));
    b.tick(t);
    fab.deliver_b_to_a(a, t);
    EXPECT_TRUE(a.listener_permits_transmit(sid));  // registered (Registrar In)

    // Fire A's own LeaveAll many times (each ~20 s jump) with NO re-Join delivered
    // from b. Our own LeaveAll must never age our own Registrars (In -> Lv -> Mt):
    // it never touches them, so the transmit gate holds.
    for (int i = 0; i < 6; ++i) {
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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
    auto t1 = clock.advance(std::chrono::milliseconds(150));
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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

    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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
    a.tick(clock.advance(std::chrono::milliseconds(150)));
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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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
    a.tick(clock.advance(std::chrono::milliseconds(150)));

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
    auto const t1 = clock.advance(std::chrono::milliseconds(150));
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

TEST_MAIN(statusbar_srp, srp_msrp_participant_test)
