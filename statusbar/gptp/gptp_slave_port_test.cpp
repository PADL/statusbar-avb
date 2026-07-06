// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Integration tests for the gPTP slave-role follower.
// Exercises GptpSlavePort with synthetic Sync + FollowUp messages
// injected from a simulated grandmaster.

#include "statusbar/gptp/gptp_slave_port.hpp"

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/gptp/gptp_time_util.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

using namespace statusbar;
using namespace statusbar::gptp;
using TimePoint = statusbar::sm::TimePoint;

namespace {

/// Deterministic test clock.
struct TestClock
{
    TimePoint now{TimePoint{} + std::chrono::seconds(1)};

    auto advance(std::chrono::nanoseconds d) -> TimePoint
    {
        now += d;
        return now;
    }
};

/// Software-only GptpClockOps: all timestamps derived from the TestClock,
/// frequency adjustments just recorded for assertion.
struct SoftwareOps
{
    int64_t current_time_ns{1'000'000'000LL};
    double last_freq_ppb{0.0};
    int64_t last_phase_jump_ns{0};
    int phase_jump_count{0};
    int freq_adjust_count{0};

    auto make_ops() -> GptpClockOps
    {
        GptpClockOps ops{};
        ops.get_local_time_ns = [this]() -> int64_t { return current_time_ns; };
        ops.send_frame = [](std::span<uint8_t const>) -> TxResult { return TxResult{.ok = true, .tx_timestamp_ns = 0}; };
        ops.adjust_phase_ns = [this](int64_t ns) {
            last_phase_jump_ns = ns;
            ++phase_jump_count;
        };
        ops.adjust_frequency_ppb = [this](double ppb) {
            last_freq_ppb = ppb;
            ++freq_adjust_count;
        };
        ops.get_link_speed = []() { return LinkSpeedMbps::Mbps1000; };
        return ops;
    }
};

/// Serialize a FollowUp with a valid FollowUp Information TLV.
auto make_follow_up_with_tlv(
    uint16_t seq_id,
    Timestamp precise_origin,
    int64_t correction_raw,
    SourcePortIdentity const& source,
    int32_t scaled_rate_offset = 0) -> std::array<uint8_t, FollowUpMessage::LENGTH + FollowUpInformationTLV::LENGTH>
{
    FollowUpMessage fup{};
    fup.init(seq_id);
    fup.precise_origin_timestamp = precise_origin;
    fup.header.set_correction_field(correction_raw);
    fup.header.source_port_identity = source;

    FollowUpInformationTLV tlv{};
    tlv.init();
    tlv.set_cumulative_scaled_rate_offset(scaled_rate_offset);

    std::array<uint8_t, FollowUpMessage::LENGTH + FollowUpInformationTLV::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf).first(FollowUpMessage::LENGTH), fup);
    (void)store_unchecked(std::span<uint8_t>(buf).subspan(FollowUpMessage::LENGTH, FollowUpInformationTLV::LENGTH), tlv);
    return buf;
}

auto make_sync(uint16_t seq_id, SourcePortIdentity const& source, int8_t log_interval = -5)
    -> std::array<uint8_t, SyncMessage::LENGTH>
{
    SyncMessage sync{};
    sync.init(seq_id);
    sync.header.source_port_identity = source;
    sync.header.log_message_interval = static_cast<uint8_t>(log_interval);
    std::array<uint8_t, SyncMessage::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), sync);
    return buf;
}

auto gm_port_identity() -> SourcePortIdentity
{
    tsn::ClockIdentity const gm{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x01};
    return SourcePortIdentity{gm, 1};
}

}  // namespace

// ===========================================================================
// Construction and startup
// ===========================================================================

TEST(gptp_slave_port, construction_standard_profile)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::standard_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    EXPECT_FALSE(port.is_synchronized());
    EXPECT_EQ(static_cast<int>(port.current_port_state()), static_cast<int>(port_state_sm::Def::State::Start));
}

TEST(gptp_slave_port, construction_automotive_profile)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    EXPECT_FALSE(port.is_synchronized());
}

TEST(gptp_slave_port, start_link_up_standard_goes_to_listening)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::standard_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);
    // Standard profile: LinkUp -> Initializing, then needs AsCapableAcquired
    // to move to Uncalibrated. Without Pdelay success, stays in early state.
    EXPECT_FALSE(port.is_synchronized());
}

TEST(gptp_slave_port, start_link_up_automotive_goes_to_uncalibrated)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);
    // Automotive with as_capable_initial=true: should have jumped past
    // Listening to Uncalibrated (as_capable is pre-granted).
    EXPECT_TRUE(port.as_capable());
    EXPECT_FALSE(port.is_synchronized());
    EXPECT_EQ(static_cast<int>(port.current_port_state()), static_cast<int>(port_state_sm::Def::State::Uncalibrated));
}

// ===========================================================================
// Sync + FollowUp pairing → first servo pass → Slave state
// ===========================================================================

TEST(gptp_slave_port, sync_follow_up_pair_reaches_servo)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    // Disable pdelay — use manual delay so we don't need to drive
    // the 4-timestamp exchange in this test.
    cfg.pdelay_mode = PdelayMode::Disabled;
    cfg.manual_peer_delay_ns = 500;

    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);
    EXPECT_TRUE(port.as_capable());

    // Capture observer callbacks.
    bool sync_state_fired = false;
    bool sync_update_fired = false;
    Observer obs{};
    obs.on_sync_state_change = [&](bool synced) { sync_state_fired = synced; };
    obs.on_sync_update = [&](int64_t, double) { sync_update_fired = true; };
    (void)port.subscribe(obs);

    // Inject a Sync + FollowUp from a simulated grandmaster.
    auto const gm = gm_port_identity();
    auto const sync_buf = make_sync(1, gm);
    auto const t1 = clock.advance(std::chrono::milliseconds(125));

    // The Sync arrives with rx hw timestamp = current_time_ns.
    sw.current_time_ns = 2'000'000'000LL;
    port.receive_frame(std::span<uint8_t const>(sync_buf), sw.current_time_ns, t1);

    // Now the FollowUp arrives with precise origin timestamp at 1 second.
    auto const fup_buf = make_follow_up_with_tlv(1, Timestamp{1, 999'999'500}, 0, gm, 0);
    port.receive_frame(std::span<uint8_t const>(fup_buf), 0 /* no HW ts for general msgs */, t1);

    // The servo should have run: on_sync_update fires, and because
    // this is the first sync, PortStateSM transitions Uncalibrated -> Slave.
    EXPECT_TRUE(sync_update_fired);
    EXPECT_TRUE(sync_state_fired);
    EXPECT_TRUE(port.is_synchronized());
    EXPECT_EQ(static_cast<int>(port.current_port_state()), static_cast<int>(port_state_sm::Def::State::Slave));
}

// ===========================================================================
// Observer unsubscribe
// ===========================================================================

TEST(gptp_slave_port, unsubscribe_stops_callbacks)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;

    // Subscribe BEFORE start so the observer sees the as_capable
    // transition fired during start().
    int count = 0;
    Observer obs{};
    obs.on_as_capable_change = [&](bool) { ++count; };
    auto const sub = port.subscribe(obs);
    EXPECT_EQ(count, 0);  // not fired yet

    port.start(clock.now, true);
    EXPECT_EQ(count, 1);  // fired during start() -> as_capable_initial

    port.unsubscribe(sub);
    // Should not fire again.
    port.on_link_down(clock.now);
    port.on_link_up(clock.advance(std::chrono::milliseconds(100)));
    EXPECT_EQ(count, 1);
}

// ===========================================================================
// Sync receipt timeout → sync lost
// ===========================================================================

TEST(gptp_slave_port, sync_receipt_timeout_loses_sync)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    cfg.pdelay_mode = PdelayMode::Disabled;
    cfg.manual_peer_delay_ns = 500;
    cfg.sync_receipt_timeout_multiplier = 3;  // 3 × 31.25 ms = ~94 ms

    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    // Drive one sync to reach Slave state.
    auto const gm = gm_port_identity();
    auto const sync_buf = make_sync(1, gm);
    auto const t1 = clock.advance(std::chrono::milliseconds(32));
    sw.current_time_ns = 2'000'000'000LL;
    port.receive_frame(std::span<uint8_t const>(sync_buf), sw.current_time_ns, t1);
    auto const fup_buf = make_follow_up_with_tlv(1, Timestamp{1, 999'999'500}, 0, gm, 0);
    port.receive_frame(std::span<uint8_t const>(fup_buf), 0, t1);
    EXPECT_TRUE(port.is_synchronized());

    // Now wait beyond the sync receipt timeout without sending another sync.
    auto const t2 = clock.advance(std::chrono::milliseconds(200));
    port.tick(t2);

    // Sync should have been lost.
    EXPECT_FALSE(port.is_synchronized());
    EXPECT_EQ(static_cast<int>(port.current_port_state()), static_cast<int>(port_state_sm::Def::State::Uncalibrated));
}

// ===========================================================================
// Link delay math smoke test
// ===========================================================================

TEST(gptp_link_delay, known_symmetric_delay)
{
    PdelayExchange ex{};
    ex.t1_req_tx_local_ns = 1'000'000'000LL;
    ex.t2_req_rx_peer_ns = 1'000'000'500LL;
    ex.t3_resp_tx_peer_ns = 1'000'001'000LL;
    ex.t4_resp_rx_local_ns = 1'000'001'500LL;

    // Symmetric link: round_trip = 1500 ns, peer_delta = 500 ns,
    // ratio = 1.0 → link_delay = (1500 - 500) / 2 = 500 ns.
    auto const delay = compute_link_delay_ns(ex, 1.0);
    EXPECT_EQ(delay, 500);
}

TEST(gptp_link_delay, rate_ratio_from_consecutive_exchanges)
{
    PdelayExchange prev{};
    prev.t1_req_tx_local_ns = 1'000'000'000LL;
    prev.t2_req_rx_peer_ns = 1'000'000'000LL;

    PdelayExchange curr{};
    curr.t1_req_tx_local_ns = 2'000'000'000LL;
    curr.t2_req_rx_peer_ns = 2'000'000'000LL;

    // Both clocks tick at the same rate → ratio = 1.0.
    auto const ratio = compute_neighbor_rate_ratio(prev, curr);
    EXPECT_TRUE(ratio > 0.999999 && ratio < 1.000001);
}

TEST(gptp_link_delay, clamp_rate_ratio)
{
    // 500 ppm offset → clamp to 250 ppm.
    auto const clamped = clamp_rate_ratio_ppm(1.0005, 250.0);
    EXPECT_TRUE(clamped <= 1.00025 + 1e-9);
    EXPECT_TRUE(clamped >= 1.00025 - 1e-9);
}

// ===========================================================================
// Servo smoke test
// ===========================================================================

TEST(gptp_servo, converges_with_zero_offset)
{
    auto cfg = GptpConfig::standard_defaults();
    ServoLoop servo{cfg};

    MDSyncReceiveIndication ind{};
    ind.precise_origin_timestamp = Timestamp{1, 0};
    ind.sync_rx_local_ns = 1'000'000'000LL;  // exactly matches master
    ind.log_message_interval = -3;           // 125 ms
    ind.has_follow_up_tlv = true;
    ind.cumulative_scaled_rate_offset = 0;

    // First sample bootstraps; second produces a rate estimate.
    auto out1 = servo.process(ind, 0, 1.0);
    EXPECT_EQ(out1.master_offset_ns, 0);  // perfect alignment

    // Advance by 125 ms in both clocks.
    ind.precise_origin_timestamp = Timestamp{1, 125'000'000};
    ind.sync_rx_local_ns = 1'125'000'000LL;
    auto out2 = servo.process(ind, 0, 1.0);
    EXPECT_EQ(out2.master_offset_ns, 0);
    // Frequency should remain near zero.
    EXPECT_TRUE(out2.frequency_adjust_ppb.has_value());
    EXPECT_TRUE(std::abs(*out2.frequency_adjust_ppb) < 1.0);
}

TEST(gptp_servo, phase_jump_detected)
{
    auto cfg = GptpConfig::standard_defaults();
    cfg.servo_phase_jump_consecutive_samples = 2;   // jump after 2 samples
    cfg.servo_phase_jump_threshold_ns = 1'000'000;  // 1 ms
    ServoLoop servo{cfg};

    // Feed samples with 10 ms offset (>> 1 ms threshold).
    // With consecutive_samples=2, the jump fires on the SECOND sample
    // (i==1), after the counter reaches 2.
    bool jump_seen = false;
    for (int i = 0; i < 3; ++i) {
        MDSyncReceiveIndication ind{};
        ind.precise_origin_timestamp = Timestamp{1, static_cast<uint32_t>(i * 125'000'000)};
        ind.sync_rx_local_ns = 1'000'000'000LL + (i * 125'000'000LL) + 10'000'000LL;
        ind.log_message_interval = -3;
        ind.has_follow_up_tlv = true;
        ind.cumulative_scaled_rate_offset = 0;

        auto out = servo.process(ind, 0, 1.0);
        if (out.phase_jump_ns.has_value()) {
            jump_seen = true;
        }
    }
    EXPECT_TRUE(jump_seen);
}

// ===========================================================================
// Announce latching
// ===========================================================================

TEST(gptp_slave_port, announce_latches_grandmaster)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    tsn::ClockIdentity gm_seen{};
    Observer obs{};
    obs.on_grandmaster_change = [&](tsn::ClockIdentity const& id) { gm_seen = id; };
    (void)port.subscribe(obs);

    // Inject an Announce.
    AnnounceMessage ann{};
    ann.init(1);
    ann.grandmaster_identity = tsn::ClockIdentity{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    ann.grandmaster_priority1 = 128;
    ann.header.source_port_identity = gm_port_identity();

    std::array<uint8_t, AnnounceMessage::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), ann);
    port.receive_frame(std::span<uint8_t const>(buf), 0, clock.now);

    EXPECT_EQ(gm_seen, ann.grandmaster_identity);
    EXPECT_EQ(port.grandmaster_identity(), ann.grandmaster_identity);

    // grandmaster_info() query path.
    auto const& info = port.grandmaster_info();
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->identity, ann.grandmaster_identity);
    EXPECT_EQ(info->priority1, 128);
}

// ===========================================================================
// stop() / next_deadline() query paths
// ===========================================================================

TEST(gptp_slave_port, stop_leaves_port_unsynchronized)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);
    port.stop();
    EXPECT_FALSE(port.is_synchronized());
}

TEST(gptp_slave_port, next_deadline_after_start_is_finite)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);
    auto const deadline = port.next_deadline();
    EXPECT_TRUE(deadline != TimePoint::max());
}

// ===========================================================================
// PDelay request / response path
// ===========================================================================

TEST(gptp_slave_port, pdelay_req_triggers_response)
{
    SoftwareOps sw;
    int send_count = 0;
    // Override send_frame to count TX frames.
    auto ops = sw.make_ops();
    ops.send_frame = [&send_count](std::span<uint8_t const>) -> TxResult {
        ++send_count;
        return TxResult{.ok = true, .tx_timestamp_ns = 0};
    };
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, ops};
    TestClock clock;
    port.start(clock.now, true);
    send_count = 0;  // reset after start

    // Build a PdelayReq from the peer.
    PdelayReqMessage req{};
    req.init(7);
    auto const peer = gm_port_identity();
    req.header.source_port_identity = peer;
    std::array<uint8_t, PdelayReqMessage::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), req);
    port.receive_frame(std::span<uint8_t const>(buf), 1'234'000'000LL, clock.now);

    // The port should emit at least PdelayResp + PdelayRespFollowUp.
    EXPECT_TRUE(send_count >= 2);
}

// ===========================================================================
// Signaling / unhandled message tolerance
// ===========================================================================

TEST(gptp_slave_port, signaling_message_is_handled_without_error)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    SignalingMessage sig{};
    sig.init(0);
    sig.header.source_port_identity = gm_port_identity();
    std::array<uint8_t, SignalingMessage::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), sig);

    // Injecting a Signaling message should not crash or throw.
    port.receive_frame(std::span<uint8_t const>(buf), 0, clock.now);
    // Port remains operational.
    EXPECT_TRUE(port.as_capable());
}

// ===========================================================================
// Timer expiry paths via tick()
// ===========================================================================

TEST(gptp_slave_port, tick_past_pdelay_interval_drives_pdelay_send)
{
    SoftwareOps sw;
    int send_count = 0;
    auto ops = sw.make_ops();
    ops.send_frame = [&send_count](std::span<uint8_t const>) -> TxResult {
        ++send_count;
        return TxResult{.ok = true, .tx_timestamp_ns = 0};
    };
    auto cfg = GptpConfig::standard_defaults();  // pdelay active by default
    GptpSlavePort port{cfg, ops};
    TestClock clock;
    port.start(clock.now, true);
    int const send_count_after_start = send_count;

    // Advance well past the pdelay interval and tick. on_pdelay_interval_expired
    // should fire and emit at least one PdelayReq.
    auto const later = clock.advance(std::chrono::seconds(10));
    port.tick(later);
    EXPECT_TRUE(send_count > send_count_after_start);
}

// Regression (G1): a lost Pdelay exchange must not permanently halt the
// Pdelay engine. Before the fix, the receipt-timeout path reset both timers
// without re-arming the interval timer, so after the first unanswered
// Pdelay_Req no further requests were ever sent. Here the peer never
// responds; the port must still emit a second Pdelay_Req after the receipt
// timeout and the next interval elapse.
TEST(gptp_slave_port, pdelay_survives_lost_response)
{
    SoftwareOps sw;
    int send_count = 0;
    auto ops = sw.make_ops();
    ops.send_frame = [&send_count](std::span<uint8_t const>) -> TxResult {
        ++send_count;
        return TxResult{.ok = true, .tx_timestamp_ns = 0};
    };
    auto cfg = GptpConfig::standard_defaults();  // pdelay active by default
    GptpSlavePort port{cfg, ops};
    TestClock clock;
    port.start(clock.now, true);
    send_count = 0;  // ignore any startup frames

    // First interval fires -> Pdelay_Req #1 is sent (no response follows).
    port.tick(clock.advance(std::chrono::seconds(60)));
    int const after_first = send_count;
    EXPECT_TRUE(after_first >= 1);

    // Receipt timeout fires (peer silent). The fix re-arms the interval timer.
    port.tick(clock.advance(std::chrono::seconds(60)));

    // Next interval fires -> Pdelay_Req #2. Without the fix the engine is dead
    // and send_count would stay at after_first forever.
    port.tick(clock.advance(std::chrono::seconds(60)));
    EXPECT_TRUE(send_count > after_first);
}

TEST(gptp_slave_port, tick_past_announce_receipt_timeout_clears_grandmaster)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    // Latch a grandmaster via an Announce.
    AnnounceMessage ann{};
    ann.init(1);
    ann.grandmaster_identity = tsn::ClockIdentity{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ann.header.source_port_identity = gm_port_identity();
    std::array<uint8_t, AnnounceMessage::LENGTH> buf{};
    (void)store_unchecked(std::span<uint8_t>(buf), ann);
    port.receive_frame(std::span<uint8_t const>(buf), 0, clock.now);
    EXPECT_EQ(port.grandmaster_identity(), ann.grandmaster_identity);

    // Record grandmaster_change callbacks — we expect an "all-zero" identity
    // when the announce receipt timeout fires.
    tsn::ClockIdentity last_id{};
    bool fired = false;
    Observer obs{};
    obs.on_grandmaster_change = [&](tsn::ClockIdentity const& id) {
        last_id = id;
        fired = true;
    };
    (void)port.subscribe(obs);

    // Advance past any reasonable announce receipt timeout. Tick repeatedly
    // until next_deadline settles, giving on_announce_receipt_timeout_expired
    // a chance to run.
    auto const t = clock.advance(std::chrono::seconds(30));
    port.tick(t);
    EXPECT_TRUE(fired);
    EXPECT_EQ(last_id, tsn::ClockIdentity{});
}

// ===========================================================================
// Deferred TX timestamp feedback
// ===========================================================================

TEST(gptp_slave_port, report_tx_timestamp_does_not_crash_on_unknown_message)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    // Call report_tx_timestamp with a PDelay_Req that the port doesn't
    // recognize. Should be a silent no-op (unknown seq_id).
    port.report_tx_timestamp(MESSAGE_TYPE_PDELAY_REQ, 0xFFFF, 5'000'000'000LL, clock.now);
    EXPECT_TRUE(port.as_capable());
}

// ===========================================================================
// asCapable acquisition/loss (MDPdelayReq state that the port forwards to the SM)
// ===========================================================================

namespace {
// Drive one full successful Pdelay exchange through MDPdelayReq with local
// (t1,t4) and peer (t2,t3) timestamps in ns. link_delay = ((t4-t1)-(t3-t2))/2.
void run_pdelay_exchange(MDPdelayReq& mdp, std::int64_t t1, std::int64_t t2, std::int64_t t3, std::int64_t t4)
{
    auto const seq = mdp.pdelay_interval_timer_expired();
    if (!seq.has_value()) {
        return;
    }
    mdp.on_pdelay_req_tx_timestamp(*seq, t1);
    PdelayRespMessage resp{};
    resp.init(*seq);
    resp.request_receipt_timestamp =
        Timestamp(static_cast<uint64_t>(t2 / 1'000'000'000LL), static_cast<uint32_t>(t2 % 1'000'000'000LL));
    mdp.on_pdelay_resp(resp, t4);
    PdelayRespFollowUpMessage fup{};
    fup.init(*seq);
    fup.response_origin_timestamp =
        Timestamp(static_cast<uint64_t>(t3 / 1'000'000'000LL), static_cast<uint32_t>(t3 % 1'000'000'000LL));
    (void)mdp.on_pdelay_resp_follow_up(fup);
}
}  // namespace

// A link delay beyond the neighbor-prop-delay threshold drops asCapable even
// though the exchange itself succeeded (802.1AS 11.2.2).
TEST(md_pdelay_req, link_delay_over_threshold_drops_as_capable)
{
    MDPdelayReq mdp{MDPdelayReq::Config{.lost_response_threshold = 3, .neighbor_prop_delay_threshold_ns = 800}};
    mdp.enable();
    // Under threshold: round_trip 1500, peer_delta 500 -> 500 ns.
    run_pdelay_exchange(mdp, 1'000'000'000, 1'000'000'500, 1'000'001'000, 1'000'001'500);
    EXPECT_TRUE(mdp.is_as_capable());
    // Over threshold: round_trip 2500, peer_delta 500 -> 1000 ns > 800.
    run_pdelay_exchange(mdp, 2'000'000'000, 2'000'000'500, 2'000'001'000, 2'000'002'500);
    EXPECT_FALSE(mdp.is_as_capable());
}

// Consecutive lost responses past the threshold drop asCapable; a later success
// re-acquires it. (Before the G1/G2 fixes this state was frozen and never
// reached the port state machine.)
TEST(md_pdelay_req, lost_responses_drop_then_recover_as_capable)
{
    MDPdelayReq mdp{MDPdelayReq::Config{.lost_response_threshold = 3, .neighbor_prop_delay_threshold_ns = 0}};
    mdp.enable();
    run_pdelay_exchange(mdp, 1'000'000'000, 1'000'000'500, 1'000'001'000, 1'000'001'500);
    EXPECT_TRUE(mdp.is_as_capable());

    for (int i = 0; i < 3; ++i) {
        (void)mdp.pdelay_interval_timer_expired();  // send a Pdelay_Req
        mdp.on_pdelay_resp_receipt_timeout();       // ... no response arrives
    }
    EXPECT_FALSE(mdp.is_as_capable());  // 3 losses >= threshold

    run_pdelay_exchange(mdp, 3'000'000'000, 3'000'000'500, 3'000'001'000, 3'000'001'500);
    EXPECT_TRUE(mdp.is_as_capable());  // recovered
}

// ===========================================================================
// logMessageInterval clamping (G3): a wire value must never reach pow()/int64 raw
// ===========================================================================

TEST(gptp_log_interval, clamp_bounds_out_of_range_values)
{
    // In-range values pass through unchanged.
    EXPECT_EQ(clamp_log_interval(0), 0);
    EXPECT_EQ(clamp_log_interval(-3), -3);
    EXPECT_EQ(clamp_log_interval(7), 7);
    EXPECT_EQ(clamp_log_interval(-7), -7);
    // The dangerous wire values: 0x7F (127) would drive pow(2,127)*1e9 into an
    // int64 cast UB; 0x80 (-128) would arm a 0-ns timeout. Both clamp to the range.
    EXPECT_EQ(clamp_log_interval(static_cast<int8_t>(0x7F)), 7);
    EXPECT_EQ(clamp_log_interval(static_cast<int8_t>(0x80)), -7);
    EXPECT_EQ(clamp_log_interval(100), 7);
    EXPECT_EQ(clamp_log_interval(-100), -7);
}

// End-to-end: a Sync carrying a rogue logMessageInterval (0x7F) must arm a finite,
// bounded sync-receipt timeout rather than an absurd/UB one.
TEST(gptp_slave_port, rogue_sync_interval_arms_bounded_timeout)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);
    auto const t0 = clock.now;

    auto const sync_buf = make_sync(1, gm_port_identity(), static_cast<int8_t>(0x7F));
    port.receive_frame(std::span<uint8_t const>(sync_buf), 1'000'000'000LL, t0);

    auto const nd = port.next_deadline();
    EXPECT_TRUE(nd != TimePoint::max());           // a timer is armed
    EXPECT_TRUE(nd >= t0);                         // not in the past (a 0-ns / negative-clamp bug)
    EXPECT_TRUE(nd - t0 < std::chrono::hours(1));  // bounded (a raw 2^127 s would be astronomically larger)
}

// ===========================================================================
// Ingress validation (G4): reject non-gPTP frames and non-grandmaster sources
// ===========================================================================

// A plain IEEE-1588 frame (majorSdoId 0) or a wrong-domain frame on EtherType
// 0x88F7 must never reach the servo; a valid domain-0 gPTP frame still does.
TEST(gptp_slave_port, non_gptp_frames_do_not_reach_servo)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    cfg.pdelay_mode = PdelayMode::Disabled;
    cfg.manual_peer_delay_ns = 500;
    cfg.verify_source_port_identity = false;  // isolate the header/domain check
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    bool sync_update_fired = false;
    Observer obs{};
    obs.on_sync_update = [&](int64_t, double) { sync_update_fired = true; };
    (void)port.subscribe(obs);

    auto const gm = gm_port_identity();
    auto const t1 = clock.advance(std::chrono::milliseconds(125));
    sw.current_time_ns = 2'000'000'000LL;

    // Plain IEEE-1588: clear byte-0 upper nibble (majorSdoId -> 0).
    auto sync_1588 = make_sync(1, gm);
    sync_1588[0] = static_cast<uint8_t>(sync_1588[0] & 0x0F);
    port.receive_frame(std::span<uint8_t const>(sync_1588), sw.current_time_ns, t1);
    auto fup_1588 = make_follow_up_with_tlv(1, Timestamp{1, 999'999'500}, 0, gm, 0);
    fup_1588[0] = static_cast<uint8_t>(fup_1588[0] & 0x0F);
    port.receive_frame(std::span<uint8_t const>(fup_1588), 0, t1);
    EXPECT_FALSE(sync_update_fired);
    EXPECT_FALSE(port.is_synchronized());

    // Wrong PTP domain: domain_number (byte 4) != 0.
    auto sync_dom = make_sync(2, gm);
    sync_dom[4] = 5;
    port.receive_frame(std::span<uint8_t const>(sync_dom), sw.current_time_ns, t1);
    auto fup_dom = make_follow_up_with_tlv(2, Timestamp{1, 999'999'500}, 0, gm, 0);
    fup_dom[4] = 5;
    port.receive_frame(std::span<uint8_t const>(fup_dom), 0, t1);
    EXPECT_FALSE(sync_update_fired);
    EXPECT_FALSE(port.is_synchronized());

    // A valid domain-0 gPTP pair IS processed.
    auto const sync_ok = make_sync(3, gm);
    port.receive_frame(std::span<uint8_t const>(sync_ok), sw.current_time_ns, t1);
    auto const fup_ok = make_follow_up_with_tlv(3, Timestamp{1, 999'999'500}, 0, gm, 0);
    port.receive_frame(std::span<uint8_t const>(fup_ok), 0, t1);
    EXPECT_TRUE(sync_update_fired);
    EXPECT_TRUE(port.is_synchronized());
}

// With verify_source_port_identity (default on), Sync/FollowUp from a source other
// than the latched grandmaster are dropped, so a second/rogue master can't thrash
// the servo; the real grandmaster's Sync is still accepted.
TEST(gptp_slave_port, sync_from_non_grandmaster_source_is_dropped)
{
    SoftwareOps sw;
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    cfg.pdelay_mode = PdelayMode::Disabled;
    cfg.manual_peer_delay_ns = 500;
    cfg.verify_source_port_identity = true;  // automotive defaults this off; opt in for this test
    GptpSlavePort port{cfg, sw.make_ops()};
    TestClock clock;
    port.start(clock.now, true);

    bool sync_update_fired = false;
    Observer obs{};
    obs.on_sync_update = [&](int64_t, double) { sync_update_fired = true; };
    (void)port.subscribe(obs);

    // Latch the grandmaster via an Announce from `gm`.
    auto const gm = gm_port_identity();
    AnnounceMessage ann{};
    ann.init(1);
    ann.grandmaster_identity = tsn::ClockIdentity{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    ann.header.source_port_identity = gm;
    std::array<uint8_t, AnnounceMessage::LENGTH> ann_buf{};
    (void)store_unchecked(std::span<uint8_t>(ann_buf), ann);
    port.receive_frame(std::span<uint8_t const>(ann_buf), 0, clock.now);

    auto const t1 = clock.advance(std::chrono::milliseconds(125));
    sw.current_time_ns = 2'000'000'000LL;

    // Sync + FollowUp from a DIFFERENT source -> dropped.
    SourcePortIdentity const rogue{tsn::ClockIdentity{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11}, 1};
    auto const sync_rogue = make_sync(1, rogue);
    port.receive_frame(std::span<uint8_t const>(sync_rogue), sw.current_time_ns, t1);
    auto const fup_rogue = make_follow_up_with_tlv(1, Timestamp{1, 999'999'500}, 0, rogue, 0);
    port.receive_frame(std::span<uint8_t const>(fup_rogue), 0, t1);
    EXPECT_FALSE(sync_update_fired);
    EXPECT_FALSE(port.is_synchronized());

    // Sync + FollowUp from the latched grandmaster -> accepted.
    auto const sync_gm = make_sync(2, gm);
    port.receive_frame(std::span<uint8_t const>(sync_gm), sw.current_time_ns, t1);
    auto const fup_gm = make_follow_up_with_tlv(2, Timestamp{1, 999'999'500}, 0, gm, 0);
    port.receive_frame(std::span<uint8_t const>(fup_gm), 0, t1);
    EXPECT_TRUE(sync_update_fired);
    EXPECT_TRUE(port.is_synchronized());
}

TEST_MAIN(statusbar_gptp, gptp_slave_port_test)
