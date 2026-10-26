// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the Milan listener's sink state machine (Milan v1.2 Section 5.5.3, Table 5.30:
// one test per implemented cell, grouped by state), its per-sink ADP discovery (Section 5.6.4)
// and the response tables 5.27 and 5.31-5.39 field by field.

#include "statusbar/atdecc/atdecc_acmp_milan_listener.hpp"

#include "statusbar/atdecc/atdecc_acmp_milan_talker.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using ieee::Eui48;
using ieee::Eui64;
using std::chrono::milliseconds;
using std::chrono::seconds;
using State = MilanSinkState;
namespace pbsta = aem::probing_status;

namespace {

Eui64 const CONTROLLER{0x00, 0x1B, 0x92, 0xFF, 0xFE, 0x00, 0x00, 0x01};
Eui64 const OTHER_CONTROLLER{0x00, 0x1B, 0x92, 0xFF, 0xFE, 0x00, 0x00, 0x09};
Eui64 const TALKER{0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x02};
Eui64 const OTHER_TALKER{0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x05};
Eui64 const LISTENER{0x02, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x00, 0x03};
Eui64 const STREAM{0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01};
Eui48 const DEST{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
tsn::ClockIdentity const GM{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x07};
tsn::ClockIdentity const OTHER_GM{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x08};

constexpr uint16_t SINK = 1;

/// A listener with two sinks, its hooks recorded.
struct Fixture
{
    Fixture()
    {
        l.set_send([this](AcmpCommandResponse const& pdu) {
            sent.push_back(pdu);
            return true;
        });
        l.set_on_settled([this](uint16_t const sink, MilanSinkStream const& stream) {
            settled.push_back(sink);
            last_settled = stream;
        });
        l.set_on_unsettled([this](uint16_t const sink, MilanSinkStream const& was) {
            unsettled.push_back(sink);
            last_unsettled = was;
        });
        l.set_on_binding_changed([this](uint16_t const sink) { binding_changes.push_back(sink); });
        l.set_on_status_changed([this](uint16_t const sink) { status_changes.push_back(sink); });
        l.set_authorized([this](Eui64 const& controller) { return locked_by == Eui64{} || controller == locked_by; });
        l.set_delay([this] { return delay; });
    }

    [[nodiscard]] auto sink() const -> MilanSink const& { return *l.sink(SINK); }
    [[nodiscard]] auto state() const -> State { return sink().state; }

    static auto command(uint8_t const type, uint16_t const flags = 0, Eui64 const& talker = TALKER, uint16_t const tuid = 3)
        -> AcmpCommandResponse
    {
        AcmpCommandResponse cmd{};
        cmd.init_command(type);
        cmd.controller_entity_id = CONTROLLER;
        cmd.talker_entity_id = talker;
        cmd.listener_entity_id = LISTENER;
        cmd.talker_unique_id = tuid;
        cmd.listener_unique_id = SINK;
        cmd.sequence_id = uint16_t{0x4242};
        cmd.connection_count = uint16_t{9};
        cmd.flags = flags;
        cmd.stream_id = STREAM;  // undefined in commands: never echoed
        return cmd;
    }

    void bind(uint16_t const flags = 0, Eui64 const& talker = TALKER, uint16_t const tuid = 3)
    {
        sent.clear();
        EXPECT_TRUE(l.receive(command(ACMP_MESSAGE_TYPE_BIND_RX_COMMAND, flags, talker, tuid), now));
    }

    auto unbind() -> AcmpCommandResponse
    {
        sent.clear();
        EXPECT_TRUE(l.receive(command(ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND), now));
        return sent.back();
    }

    auto rx_state() -> AcmpCommandResponse
    {
        sent.clear();
        EXPECT_TRUE(l.receive(command(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND), now));
        EXPECT_EQ(sent.size(), size_t{1});
        return sent.back();
    }

    /// The talker's answer to the last probe.
    auto probe_response(uint8_t const status = ACMP_STATUS_SUCCESS) -> AcmpCommandResponse
    {
        auto resp = sink().probe;
        resp.set_message_type(ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
        resp.set_status(status);
        resp.stream_id = STREAM;
        resp.stream_dest_mac = DEST;
        resp.stream_vlan_id = uint16_t{2};
        return resp;
    }

    void respond(uint8_t const status = ACMP_STATUS_SUCCESS)
    {
        sent.clear();
        EXPECT_TRUE(l.receive(probe_response(status), now));
    }

    void advance(std::chrono::nanoseconds const by)
    {
        now += std::chrono::duration_cast<sm::TimePoint::duration>(by);
        l.tick(now);
    }

    auto adp(
        uint32_t const available_index, bool const available = true, tsn::ClockIdentity const& gm = GM, uint16_t const iface = 0)
        -> AdpDu
    {
        AdpDu du{};
        du.set_message_type(available ? ADP_MESSAGE_TYPE_ENTITY_AVAILABLE : ADP_MESSAGE_TYPE_ENTITY_DEPARTING);
        du.set_valid_time(10);
        du.entity_id = TALKER;
        du.available_index = available_index;
        du.gptp_grandmaster_id = gm;
        du.gptp_domain_number = uint8_t{0};
        du.interface_index = iface;
        return du;
    }

    void discover(uint32_t const available_index = 1, tsn::ClockIdentity const& gm = GM)
    {
        l.receive_adp(adp(available_index, true, gm), GM, 0, now);
    }

    void depart() { l.receive_adp(adp(0, false), GM, 0, now); }

    /// Bind and settle (SETTLED_NO_RSV).
    void settle()
    {
        bind();
        respond();
        EXPECT_TRUE(state() == State::settled_no_rsv);
    }

    /// Bind, settle and register (SETTLED_RSV_OK).
    void reserve(MilanTalkerRegistration const r = MilanTalkerRegistration::advertise)
    {
        settle();
        l.set_registration(SINK, r, now);
        EXPECT_TRUE(state() == State::settled_rsv_ok);
    }

    /// PRB_W_RETRY by a refused probe.
    void retry()
    {
        bind();
        respond(ACMP_STATUS_TALKER_DEST_MAC_FAIL);
        EXPECT_TRUE(state() == State::prb_w_retry);
    }

    /// PRB_W_AVAIL by a saved binding.
    void restore(bool const started = true)
    {
        EXPECT_TRUE(l.restore(
            SINK,
            MilanSinkBinding{
                .talker_entity_id = TALKER, .talker_unique_id = 3, .controller_entity_id = CONTROLLER, .started = started}));
        EXPECT_TRUE(state() == State::prb_w_avail);
    }

    /// PRB_W_DELAY: restored, then the talker discovered.
    void delay_state()
    {
        restore();
        discover();
        EXPECT_TRUE(state() == State::prb_w_delay);
    }

    MilanListener l{LISTENER, 2};
    sm::TimePoint now{std::chrono::seconds{100}};
    std::chrono::nanoseconds delay{milliseconds{500}};
    Eui64 locked_by{};
    std::vector<AcmpCommandResponse> sent;
    std::vector<uint16_t> settled;
    std::vector<uint16_t> unsettled;
    std::vector<uint16_t> binding_changes;
    std::vector<uint16_t> status_changes;
    MilanSinkStream last_settled{};
    MilanSinkStream last_unsettled{};
};

void expect_echoed(AcmpCommandResponse const& resp, uint8_t const type, uint8_t const status)
{
    EXPECT_EQ(resp.message_type(), type);
    EXPECT_EQ(resp.status(), status);
    EXPECT_EQ(resp.controller_entity_id, CONTROLLER);
    EXPECT_EQ(resp.listener_entity_id, LISTENER);
    EXPECT_EQ(resp.listener_unique_id.get(), SINK);
    EXPECT_EQ(resp.sequence_id.get(), uint16_t{0x4242});
}

/// Table 5.32.
void expect_bind_success(AcmpCommandResponse const& resp, bool const streaming_wait = false)
{
    expect_echoed(resp, ACMP_MESSAGE_TYPE_BIND_RX_RESPONSE, ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, TALKER);
    EXPECT_EQ(resp.talker_unique_id.get(), uint16_t{3});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{1});
    EXPECT_EQ(resp.flags.get(), streaming_wait ? acmp_flags::STREAMING_WAIT : uint16_t{0});
    EXPECT_EQ(resp.stream_id, Eui64{});
    EXPECT_EQ(resp.stream_dest_mac, Eui48{});
    EXPECT_EQ(resp.stream_vlan_id.get(), uint16_t{0});
}

/// Table 5.33.
void expect_probe(AcmpCommandResponse const& cmd, Eui64 const& talker = TALKER, uint16_t const tuid = 3)
{
    EXPECT_EQ(cmd.message_type(), ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND);
    EXPECT_EQ(cmd.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(cmd.controller_entity_id, CONTROLLER);
    EXPECT_EQ(cmd.talker_entity_id, talker);
    EXPECT_EQ(cmd.listener_entity_id, LISTENER);
    EXPECT_EQ(cmd.talker_unique_id.get(), tuid);
    EXPECT_EQ(cmd.listener_unique_id.get(), SINK);
    EXPECT_EQ(cmd.connection_count.get(), uint16_t{0});
    EXPECT_EQ(cmd.flags.get(), acmp_flags::FAST_CONNECT);
    EXPECT_EQ(cmd.stream_id, Eui64{});
    EXPECT_EQ(cmd.stream_dest_mac, Eui48{});
    EXPECT_EQ(cmd.stream_vlan_id.get(), uint16_t{0});
}

/// A new bind from a bound state (rebinding to another talker): the
/// response, a probe at once, PRB_W_RESP, discovery restarted.
void expect_rebind(Fixture& f)
{
    f.bind(0, OTHER_TALKER, 4);
    EXPECT_EQ(f.sent.size(), size_t{2});
    EXPECT_EQ(f.sent[0].message_type(), ACMP_MESSAGE_TYPE_BIND_RX_RESPONSE);
    EXPECT_EQ(f.sent[0].status(), ACMP_STATUS_SUCCESS);
    expect_probe(f.sent[1], OTHER_TALKER, 4);
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    EXPECT_FALSE(f.sink().discovered);
    EXPECT_EQ(f.sink().binding.talker_entity_id, OTHER_TALKER);
    EXPECT_EQ(f.sink().probing_status, pbsta::ACTIVE);
    EXPECT_EQ(f.binding_changes.back(), SINK);
}

/// The same talker bound again: only the controller and the started state.
void expect_same_bind_keeps_state(Fixture& f)
{
    auto const before = f.state();
    f.binding_changes.clear();
    auto cmd = Fixture::command(ACMP_MESSAGE_TYPE_BIND_RX_COMMAND, acmp_flags::STREAMING_WAIT);
    cmd.controller_entity_id = OTHER_CONTROLLER;
    f.sent.clear();
    EXPECT_TRUE(f.l.receive(cmd, f.now));
    EXPECT_EQ(f.sent.size(), size_t{1});  // no probe
    EXPECT_EQ(f.sent[0].status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(f.sent[0].flags.get(), acmp_flags::STREAMING_WAIT);
    EXPECT_TRUE(f.state() == before);
    EXPECT_EQ(f.sink().binding.controller_entity_id, OTHER_CONTROLLER);
    EXPECT_FALSE(f.sink().binding.started);
    EXPECT_EQ(f.binding_changes.size(), size_t{1});
}

/// A bind or unbind refused while locked by another controller (Tables
/// 5.31, 5.35): nothing changes.
void expect_locked_refusals(Fixture& f)
{
    auto const before = f.sink();
    f.locked_by = OTHER_CONTROLLER;
    f.bind(0, OTHER_TALKER, 4);
    EXPECT_EQ(f.sent.size(), size_t{1});
    EXPECT_EQ(f.sent[0].message_type(), ACMP_MESSAGE_TYPE_BIND_RX_RESPONSE);
    EXPECT_EQ(f.sent[0].status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_EQ(f.sent[0].talker_entity_id, OTHER_TALKER);
    auto const resp = f.unbind();
    EXPECT_EQ(resp.message_type(), ACMP_MESSAGE_TYPE_UNBIND_RX_RESPONSE);
    EXPECT_EQ(resp.status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_EQ(resp.talker_entity_id, TALKER);  // echoed
    EXPECT_TRUE(f.state() == before.state);
    EXPECT_TRUE(f.sink().binding == before.binding);
    f.locked_by = Eui64{};
}

/// Table 5.36, and UNBOUND after.
void expect_unbind(Fixture& f)
{
    f.binding_changes.clear();
    auto const resp = f.unbind();
    expect_echoed(resp, ACMP_MESSAGE_TYPE_UNBIND_RX_RESPONSE, ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, Eui64{});
    EXPECT_EQ(resp.talker_unique_id.get(), uint16_t{0});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{0});
    EXPECT_EQ(resp.flags.get(), uint16_t{0});
    EXPECT_EQ(resp.stream_id, Eui64{});
    EXPECT_EQ(f.sent.size(), size_t{1});  // never a DISCONNECT_TX
    EXPECT_TRUE(f.state() == State::unbound);
    EXPECT_EQ(f.sink().probing_status, pbsta::DISABLED);
    EXPECT_FALSE(f.sink().timer_running);
    EXPECT_EQ(f.binding_changes.size(), size_t{1});
}

/// Table 5.37 (a probing sink).
void expect_probing_rx_state(Fixture& f, bool const started = true)
{
    auto const resp = f.rx_state();
    expect_echoed(resp, ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, TALKER);
    EXPECT_EQ(resp.talker_unique_id.get(), uint16_t{3});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{1});
    EXPECT_EQ(resp.flags.get(), static_cast<uint16_t>(acmp_flags::FAST_CONNECT | (started ? 0 : acmp_flags::STREAMING_WAIT)));
}

}  // namespace

// --- Section 5.5.3.1: the incoming messages ---------------------------------

TEST(acmp_milan_listener, unknown_sink_table_5_27)
{
    Fixture f;
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_BIND_RX_COMMAND, ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND, ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND}) {
        auto cmd = Fixture::command(type);
        cmd.listener_unique_id = uint16_t{7};
        f.sent.clear();
        EXPECT_TRUE(f.l.receive(cmd, f.now));
        EXPECT_EQ(f.sent.size(), size_t{1});
        EXPECT_EQ(f.sent[0].message_type(), static_cast<uint8_t>(type + 1));
        EXPECT_EQ(f.sent[0].status(), ACMP_STATUS_LISTENER_UNKNOWN_ID);
        EXPECT_EQ(f.sent[0].listener_unique_id.get(), uint16_t{7});
        EXPECT_EQ(f.sent[0].sequence_id.get(), uint16_t{0x4242});
    }
    // A PROBE_TX_RESPONSE for no sink is ignored.
    auto resp = Fixture::command(ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
    resp.listener_unique_id = uint16_t{7};
    f.sent.clear();
    EXPECT_FALSE(f.l.receive(resp, f.now));
    EXPECT_TRUE(f.sent.empty());
}

TEST(acmp_milan_listener, ignores_other_listeners_and_messages)
{
    Fixture f;
    auto cmd = Fixture::command(ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
    cmd.listener_entity_id = OTHER_TALKER;
    EXPECT_FALSE(f.l.receive(cmd, f.now));
    for (uint8_t const type :
         {ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND,
          ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND,
          ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND,
          ACMP_MESSAGE_TYPE_BIND_RX_RESPONSE,
          ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND}) {
        EXPECT_FALSE(f.l.receive(Fixture::command(type), f.now));
    }
    EXPECT_TRUE(f.sent.empty());
    EXPECT_TRUE(f.state() == State::unbound);
}

// --- Startup (Sections 5.5.3.5.1-2) -----------------------------------------

TEST(acmp_milan_listener, startup_without_and_with_a_saved_binding)
{
    Fixture f;
    EXPECT_TRUE(f.state() == State::unbound);
    EXPECT_EQ(f.sink().probing_status, pbsta::DISABLED);
    f.restore(false);
    EXPECT_EQ(f.sink().probing_status, pbsta::PASSIVE);
    EXPECT_EQ(f.sink().acmp_status, uint8_t{0});
    EXPECT_FALSE(f.sink().discovered);
    EXPECT_FALSE(f.sink().binding.started);
    EXPECT_TRUE(f.sent.empty());                          // passive: waits for the talker's ADP
    EXPECT_TRUE(f.binding_changes.empty());               // already saved
    EXPECT_FALSE(f.l.restore(SINK, MilanSinkBinding{}));  // already bound
    EXPECT_FALSE(f.l.restore(9, MilanSinkBinding{}));
}

// --- UNBOUND ----------------------------------------------------------------

// The binding hook sees the sink already bound, with its started state
TEST(acmp_milan_listener, binding_hook_sees_the_bound_sink)
{
    Fixture f;
    std::vector<std::pair<bool, bool>> seen;  // bound, started
    f.l.set_on_binding_changed(
        [&f, &seen](uint16_t const sink) { seen.emplace_back(f.l.sink(sink)->bound(), f.l.sink(sink)->binding.started); });
    f.bind(acmp_flags::STREAMING_WAIT);
    EXPECT_TRUE((seen == std::vector<std::pair<bool, bool>>{{true, false}}));
    f.bind(0, OTHER_TALKER, 4);  // a rebinding
    EXPECT_TRUE((seen.back() == std::pair<bool, bool>(true, true)));
    (void)f.unbind();
    EXPECT_TRUE((seen.back() == std::pair<bool, bool>(false, true)));
}

// TMR_NO_ADP for a valid_time of 0 is one 2 s unit
TEST(acmp_milan_listener, discovery_timeout_floor)
{
    Fixture f;
    f.restore();
    auto du = f.adp(1);
    du.set_valid_time(0);
    f.l.receive_adp(du, GM, 0, f.now);
    EXPECT_TRUE(f.sink().discovered);
    EXPECT_TRUE(f.sink().adp_deadline == f.now + seconds{2});
}

// 5.5.3.5.3: saved, answered at once (Table 5.32), probed (Table 5.33)
TEST(acmp_milan_listener, unbound_bind)
{
    Fixture f;
    f.bind(acmp_flags::STREAMING_WAIT | acmp_flags::CLASS_B);
    EXPECT_EQ(f.sent.size(), size_t{2});
    expect_bind_success(f.sent[0], true);
    expect_probe(f.sent[1]);
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    EXPECT_EQ(f.sink().probing_status, pbsta::ACTIVE);
    EXPECT_EQ(f.sink().acmp_status, uint8_t{0});
    EXPECT_TRUE(f.sink().timer_running);
    EXPECT_TRUE(f.sink().deadline == f.now + milliseconds{200});
    EXPECT_FALSE(f.sink().binding.started);
    EXPECT_EQ(f.sink().binding.controller_entity_id, CONTROLLER);
    EXPECT_EQ(f.binding_changes.size(), size_t{1});
    EXPECT_FALSE(f.status_changes.empty());
}

TEST(acmp_milan_listener, unbound_bind_locked)
{
    Fixture f;
    f.locked_by = OTHER_CONTROLLER;
    f.bind();
    EXPECT_EQ(f.sent.size(), size_t{1});
    expect_echoed(f.sent[0], ACMP_MESSAGE_TYPE_BIND_RX_RESPONSE, ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_TRUE(f.state() == State::unbound);
    // The locking controller itself may bind.
    f.locked_by = CONTROLLER;
    f.bind();
    EXPECT_TRUE(f.state() == State::prb_w_resp);
}

// 5.5.3.5.4: Table 5.34
TEST(acmp_milan_listener, unbound_get_rx_state)
{
    Fixture f;
    auto const resp = f.rx_state();
    expect_echoed(resp, ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, Eui64{});
    EXPECT_EQ(resp.talker_unique_id.get(), uint16_t{0});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{0});
    EXPECT_EQ(resp.flags.get(), uint16_t{0});
    EXPECT_EQ(resp.stream_id, Eui64{});
    EXPECT_EQ(resp.stream_dest_mac, Eui48{});
    EXPECT_EQ(resp.stream_vlan_id.get(), uint16_t{0});
}

// 5.5.3.5.5: answered (Tables 5.35-5.36), nothing to change
TEST(acmp_milan_listener, unbound_unbind)
{
    Fixture f;
    f.locked_by = OTHER_CONTROLLER;
    EXPECT_EQ(f.unbind().status(), ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    f.locked_by = Eui64{};
    auto const resp = f.unbind();
    expect_echoed(resp, ACMP_MESSAGE_TYPE_UNBIND_RX_RESPONSE, ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, Eui64{});
    EXPECT_TRUE(f.binding_changes.empty());
}

TEST(acmp_milan_listener, unbound_ignores_probe_responses_and_adp)
{
    Fixture f;
    auto resp = Fixture::command(ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
    EXPECT_TRUE(f.l.receive(resp, f.now));
    f.discover();
    f.advance(seconds{30});
    EXPECT_TRUE(f.state() == State::unbound);
    EXPECT_FALSE(f.sink().discovered);
    EXPECT_TRUE(f.sent.empty());
}

// --- PRB_W_AVAIL ------------------------------------------------------------

// 5.5.3.5.6: the same talker keeps waiting; another one is probed at once
TEST(acmp_milan_listener, avail_bind)
{
    Fixture f;
    f.restore();
    expect_same_bind_keeps_state(f);
    expect_rebind(f);
}

TEST(acmp_milan_listener, avail_bind_and_unbind_locked)
{
    Fixture f;
    f.restore();
    expect_locked_refusals(f);
}

// 5.5.3.5.7: Table 5.37
TEST(acmp_milan_listener, avail_get_rx_state)
{
    Fixture f;
    f.restore(false);
    expect_probing_rx_state(f, false);
}

// 5.5.3.5.8
TEST(acmp_milan_listener, avail_unbind)
{
    Fixture f;
    f.restore();
    expect_unbind(f);
}

// 5.5.3.5.9: discovered: a random delay, then the probe (5.5.3.5.10)
TEST(acmp_milan_listener, avail_discovered)
{
    Fixture f;
    f.restore();
    f.delay = milliseconds{300};
    f.discover();
    EXPECT_TRUE(f.state() == State::prb_w_delay);
    EXPECT_TRUE(f.sink().discovered);
    EXPECT_EQ(f.sink().probing_status, pbsta::ACTIVE);
    EXPECT_TRUE(f.sink().deadline == f.now + milliseconds{300});
    EXPECT_TRUE(f.sent.empty());
}

TEST(acmp_milan_listener, avail_ignores_probe_responses)
{
    Fixture f;
    f.restore();
    EXPECT_TRUE(f.l.receive(Fixture::command(ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE), f.now));
    EXPECT_TRUE(f.state() == State::prb_w_avail);
}

// --- PRB_W_DELAY ------------------------------------------------------------

// 5.5.3.5.10: TMR_DELAY sends the probe
TEST(acmp_milan_listener, delay_timer)
{
    Fixture f;
    f.delay_state();
    f.advance(milliseconds{499});
    EXPECT_TRUE(f.state() == State::prb_w_delay);
    f.advance(milliseconds{1});
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    EXPECT_EQ(f.sent.size(), size_t{1});
    expect_probe(f.sent[0]);
    EXPECT_TRUE(f.sink().deadline == f.now + milliseconds{200});
}

// 5.5.3.5.11
TEST(acmp_milan_listener, delay_bind)
{
    Fixture f;
    f.delay_state();
    expect_same_bind_keeps_state(f);
    expect_rebind(f);
}

TEST(acmp_milan_listener, delay_locked)
{
    Fixture f;
    f.delay_state();
    expect_locked_refusals(f);
}

// 5.5.3.5.12
TEST(acmp_milan_listener, delay_get_rx_state)
{
    Fixture f;
    f.delay_state();
    expect_probing_rx_state(f);
}

// 5.5.3.5.13: TMR_DELAY stopped
TEST(acmp_milan_listener, delay_unbind)
{
    Fixture f;
    f.delay_state();
    expect_unbind(f);
    f.advance(seconds{2});
    EXPECT_TRUE(f.sent.empty() || f.sent.back().message_type() != ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND);
}

// 5.5.3.5.14: noted; 5.5.3.5.15: back to PRB_W_AVAIL, TMR_DELAY stopped
TEST(acmp_milan_listener, delay_discovered_and_departed)
{
    Fixture f;
    f.delay_state();
    auto const deadline = f.sink().deadline;
    f.discover(2);
    EXPECT_TRUE(f.state() == State::prb_w_delay);
    EXPECT_TRUE(f.sink().deadline == deadline);
    f.depart();
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_FALSE(f.sink().discovered);
    EXPECT_FALSE(f.sink().timer_running);
    EXPECT_EQ(f.sink().probing_status, pbsta::PASSIVE);
    f.advance(seconds{2});
    EXPECT_TRUE(f.sent.empty());
}

// --- PRB_W_RESP -------------------------------------------------------------

// 5.5.3.5.16: the duplicate keeps the sequence_id
TEST(acmp_milan_listener, resp_timeout_resends_the_same_probe)
{
    Fixture f;
    f.bind();
    auto const first = f.sent[1];
    f.sent.clear();
    f.advance(milliseconds{200});
    EXPECT_TRUE(f.state() == State::prb_w_resp2);
    EXPECT_EQ(f.sent.size(), size_t{1});
    EXPECT_TRUE(f.sent[0] == first);
    EXPECT_TRUE(f.sink().deadline == f.now + milliseconds{200});
}

// 5.5.3.5.17
TEST(acmp_milan_listener, resp_bind)
{
    Fixture f;
    f.bind();
    expect_same_bind_keeps_state(f);
    // The timer still runs for the first probe.
    EXPECT_TRUE(f.sink().timer_running);
    expect_rebind(f);
}

TEST(acmp_milan_listener, resp_locked)
{
    Fixture f;
    f.bind();
    expect_locked_refusals(f);
}

// 5.5.3.5.18: settles on a matching success
TEST(acmp_milan_listener, resp_probe_success_settles)
{
    Fixture f;
    f.bind();
    f.respond();
    EXPECT_TRUE(f.state() == State::settled_no_rsv);
    EXPECT_EQ(f.sink().probing_status, pbsta::COMPLETED);
    EXPECT_EQ(f.sink().acmp_status, uint8_t{0});
    EXPECT_TRUE((f.sink().stream == MilanSinkStream{.stream_id = STREAM, .dest_mac = DEST, .vlan_id = 2}));
    EXPECT_TRUE(f.sink().deadline == f.now + seconds{10});
    EXPECT_EQ(f.settled.size(), size_t{1});
    EXPECT_TRUE(f.last_settled == f.sink().stream);
}

// 5.5.3.5.18 step 1: controller, talker, unique id and sequence_id matched
TEST(acmp_milan_listener, resp_ignores_a_response_to_another_command)
{
    Fixture f;
    f.bind();
    auto resp = f.probe_response();
    resp.sequence_id = static_cast<uint16_t>(resp.sequence_id.get() + 1);
    EXPECT_TRUE(f.l.receive(resp, f.now));
    resp = f.probe_response();
    resp.controller_entity_id = OTHER_CONTROLLER;
    EXPECT_TRUE(f.l.receive(resp, f.now));
    resp = f.probe_response();
    resp.talker_entity_id = OTHER_TALKER;
    EXPECT_TRUE(f.l.receive(resp, f.now));
    resp = f.probe_response();
    resp.talker_unique_id = uint16_t{9};
    EXPECT_TRUE(f.l.receive(resp, f.now));
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    EXPECT_TRUE(f.settled.empty());
}

// 5.5.3.5.18 step 3: a refusal never settles; TMR_RETRY, the status kept
TEST(acmp_milan_listener, resp_refusals_retry)
{
    for (uint8_t const status :
         {ACMP_STATUS_TALKER_DEST_MAC_FAIL, ACMP_STATUS_TALKER_UNKNOWN_ID, ACMP_STATUS_INCOMPATIBLE_REQUEST}) {
        Fixture f;
        f.bind();
        f.respond(status);
        EXPECT_TRUE(f.state() == State::prb_w_retry);
        EXPECT_EQ(f.sink().acmp_status, status);
        EXPECT_EQ(f.sink().probing_status, pbsta::ACTIVE);
        EXPECT_TRUE(f.sink().deadline == f.now + seconds{4});
        EXPECT_TRUE(f.settled.empty());
        EXPECT_TRUE(f.binding_changes.size() == 1);  // the binding stands
    }
}

// 5.5.3.5.19
TEST(acmp_milan_listener, resp_get_rx_state)
{
    Fixture f;
    f.bind();
    expect_probing_rx_state(f);
}

// 5.5.3.5.20: TMR_NO_RESP stopped
TEST(acmp_milan_listener, resp_unbind)
{
    Fixture f;
    f.bind();
    expect_unbind(f);
    f.advance(seconds{1});
    EXPECT_TRUE(f.sent.empty() || f.sent.back().message_type() != ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND);
}

// 5.5.3.5.21: noted; 5.5.3.5.22: PRB_W_AVAIL
TEST(acmp_milan_listener, resp_discovered_and_departed)
{
    Fixture f;
    f.bind();
    f.discover();
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    EXPECT_TRUE(f.sink().discovered);
    f.depart();
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_FALSE(f.sink().timer_running);
    EXPECT_EQ(f.sink().probing_status, pbsta::PASSIVE);
    // A late response is ignored there.
    f.respond();
    EXPECT_TRUE(f.state() == State::prb_w_avail);
}

// --- PRB_W_RESP2 ------------------------------------------------------------

auto resp2(Fixture& f) -> void
{
    f.bind();
    f.advance(milliseconds{200});
    EXPECT_TRUE(f.state() == State::prb_w_resp2);
}

// 5.5.3.5.23: LISTENER_TALKER_TIMEOUT
TEST(acmp_milan_listener, resp2_timeout)
{
    Fixture f;
    resp2(f);
    f.sent.clear();
    f.advance(milliseconds{200});
    EXPECT_TRUE(f.state() == State::prb_w_retry);
    EXPECT_EQ(f.sink().acmp_status, ACMP_STATUS_LISTENER_TALKER_TIMEOUT);
    EXPECT_TRUE(f.sink().deadline == f.now + seconds{4});
    EXPECT_TRUE(f.sent.empty());
}

// 5.5.3.5.24-29: as in PRB_W_RESP
TEST(acmp_milan_listener, resp2_bind)
{
    Fixture f;
    resp2(f);
    expect_same_bind_keeps_state(f);
    expect_rebind(f);
}

TEST(acmp_milan_listener, resp2_locked)
{
    Fixture f;
    resp2(f);
    expect_locked_refusals(f);
}

TEST(acmp_milan_listener, resp2_probe_response)
{
    Fixture f;
    resp2(f);
    f.respond(ACMP_STATUS_TALKER_DEST_MAC_FAIL);
    EXPECT_TRUE(f.state() == State::prb_w_retry);
    Fixture g;
    resp2(g);
    g.respond();  // the answer to the duplicate (same sequence_id)
    EXPECT_TRUE(g.state() == State::settled_no_rsv);
}

TEST(acmp_milan_listener, resp2_get_rx_state_and_unbind)
{
    Fixture f;
    resp2(f);
    expect_probing_rx_state(f);
    expect_unbind(f);
}

TEST(acmp_milan_listener, resp2_discovered_and_departed)
{
    Fixture f;
    resp2(f);
    f.discover();
    EXPECT_TRUE(f.state() == State::prb_w_resp2);
    f.depart();
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_FALSE(f.sink().timer_running);
}

// --- PRB_W_RETRY ------------------------------------------------------------

// 5.5.3.5.30: not discovered: PRB_W_AVAIL; discovered: TMR_DELAY then probe
// with a new sequence_id
TEST(acmp_milan_listener, retry_timer)
{
    Fixture f;
    f.retry();
    f.advance(seconds{4});
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_EQ(f.sink().probing_status, pbsta::PASSIVE);
    EXPECT_EQ(f.sink().acmp_status, uint8_t{0});

    Fixture g;
    g.retry();
    auto const first_seq = g.sink().probe.sequence_id.get();
    g.discover();
    EXPECT_TRUE(g.state() == State::prb_w_retry);  // 5.5.3.5.34: noted
    g.advance(seconds{4});
    EXPECT_TRUE(g.state() == State::prb_w_delay);
    EXPECT_EQ(g.sink().acmp_status, ACMP_STATUS_TALKER_DEST_MAC_FAIL);  // the last probe's
    g.sent.clear();
    g.advance(milliseconds{500});
    EXPECT_TRUE(g.state() == State::prb_w_resp);
    EXPECT_EQ(g.sent.size(), size_t{1});
    EXPECT_NE(g.sent[0].sequence_id.get(), first_seq);
}

// 5.5.3.5.31
TEST(acmp_milan_listener, retry_bind)
{
    Fixture f;
    f.retry();
    expect_same_bind_keeps_state(f);
    expect_rebind(f);
}

TEST(acmp_milan_listener, retry_locked)
{
    Fixture f;
    f.retry();
    expect_locked_refusals(f);
}

// 5.5.3.5.32-33
TEST(acmp_milan_listener, retry_get_rx_state_and_unbind)
{
    Fixture f;
    f.retry();
    expect_probing_rx_state(f);
    expect_unbind(f);
    EXPECT_EQ(f.sink().acmp_status, uint8_t{0});
}

// 5.5.3.5.35: PRB_W_AVAIL, TMR_RETRY stopped
TEST(acmp_milan_listener, retry_departed)
{
    Fixture f;
    f.retry();
    f.discover();
    f.depart();
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_FALSE(f.sink().timer_running);
    EXPECT_EQ(f.sink().acmp_status, uint8_t{0});
}

TEST(acmp_milan_listener, retry_ignores_probe_responses)
{
    Fixture f;
    f.retry();
    f.respond();
    EXPECT_TRUE(f.state() == State::prb_w_retry);
}

// --- SETTLED_NO_RSV ---------------------------------------------------------

// 5.5.3.5.36: TMR_NO_TK: SRP stopped, probe again (or wait for the talker)
TEST(acmp_milan_listener, settled_no_tk)
{
    Fixture f;
    f.settle();
    f.advance(milliseconds{9'999});
    EXPECT_TRUE(f.state() == State::settled_no_rsv);
    f.advance(milliseconds{1});
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_EQ(f.unsettled.size(), size_t{1});
    EXPECT_TRUE(f.last_unsettled.stream_id == STREAM);
    EXPECT_TRUE(f.sink().stream == MilanSinkStream{});
    EXPECT_EQ(f.sink().probing_status, pbsta::PASSIVE);

    Fixture g;
    g.settle();
    g.discover();
    g.advance(seconds{10});
    EXPECT_TRUE(g.state() == State::prb_w_delay);
    EXPECT_EQ(g.sink().probing_status, pbsta::ACTIVE);
    EXPECT_EQ(g.unsettled.size(), size_t{1});
}

// 5.5.3.5.37
TEST(acmp_milan_listener, settled_bind)
{
    Fixture f;
    f.settle();
    expect_same_bind_keeps_state(f);
    EXPECT_TRUE(f.unsettled.empty());
    expect_rebind(f);
    EXPECT_EQ(f.unsettled.size(), size_t{1});  // SRP stopped first
    EXPECT_TRUE(f.sink().stream == MilanSinkStream{});
}

TEST(acmp_milan_listener, settled_locked)
{
    Fixture f;
    f.settle();
    expect_locked_refusals(f);
}

// 5.5.3.5.38: Table 5.38
TEST(acmp_milan_listener, settled_get_rx_state)
{
    Fixture f;
    f.settle();
    auto const resp = f.rx_state();
    expect_echoed(resp, ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, ACMP_STATUS_SUCCESS);
    EXPECT_EQ(resp.talker_entity_id, TALKER);
    EXPECT_EQ(resp.talker_unique_id.get(), uint16_t{3});
    EXPECT_EQ(resp.connection_count.get(), uint16_t{1});
    EXPECT_EQ(resp.flags.get(), acmp_flags::FAST_CONNECT);
    EXPECT_EQ(resp.stream_id, STREAM);
    EXPECT_EQ(resp.stream_dest_mac, DEST);
    EXPECT_EQ(resp.stream_vlan_id.get(), uint16_t{2});
}

// 5.5.3.5.39
TEST(acmp_milan_listener, settled_unbind)
{
    Fixture f;
    f.settle();
    expect_unbind(f);
    EXPECT_EQ(f.unsettled.size(), size_t{1});
    f.advance(seconds{11});
    EXPECT_EQ(f.unsettled.size(), size_t{1});
}

// 5.5.3.5.40-41: noted only
TEST(acmp_milan_listener, settled_discovered_and_departed)
{
    Fixture f;
    f.settle();
    f.discover();
    EXPECT_TRUE(f.sink().discovered);
    f.depart();
    EXPECT_FALSE(f.sink().discovered);
    EXPECT_TRUE(f.state() == State::settled_no_rsv);
    EXPECT_TRUE(f.sink().timer_running);
}

// 5.5.3.5.42: EVT_TK_REGISTERED stops TMR_NO_TK
TEST(acmp_milan_listener, settled_registered)
{
    Fixture f;
    f.settle();
    f.l.set_registration(SINK, MilanTalkerRegistration::none, f.now);
    EXPECT_TRUE(f.state() == State::settled_no_rsv);
    f.l.set_registration(SINK, MilanTalkerRegistration::advertise, f.now);
    EXPECT_TRUE(f.state() == State::settled_rsv_ok);
    EXPECT_FALSE(f.sink().timer_running);
    f.advance(seconds{20});
    EXPECT_TRUE(f.state() == State::settled_rsv_ok);
}

// A hook reporting the registration from on_settled (already registered)
TEST(acmp_milan_listener, settled_registered_from_the_settle_hook)
{
    Fixture f;
    f.l.set_on_settled(
        [&f](uint16_t const sink, MilanSinkStream const&) { f.l.set_registration(sink, MilanTalkerRegistration::failed, f.now); });
    f.bind();
    f.respond();
    EXPECT_TRUE(f.state() == State::settled_rsv_ok);
    EXPECT_TRUE(f.sink().registration == MilanTalkerRegistration::failed);
}

// --- SETTLED_RSV_OK ---------------------------------------------------------

// 5.5.3.5.43
TEST(acmp_milan_listener, rsv_ok_bind)
{
    Fixture f;
    f.reserve();
    expect_same_bind_keeps_state(f);
    expect_rebind(f);
    EXPECT_EQ(f.unsettled.size(), size_t{1});
}

TEST(acmp_milan_listener, rsv_ok_locked)
{
    Fixture f;
    f.reserve();
    expect_locked_refusals(f);
}

// 5.5.3.5.44: Table 5.39, REGISTERING_FAILED for a Talker Failed
TEST(acmp_milan_listener, rsv_ok_get_rx_state)
{
    Fixture f;
    f.reserve();
    auto resp = f.rx_state();
    EXPECT_EQ(resp.flags.get(), acmp_flags::FAST_CONNECT);
    EXPECT_EQ(resp.stream_id, STREAM);
    f.l.set_registration(SINK, MilanTalkerRegistration::failed, f.now);
    EXPECT_TRUE(f.state() == State::settled_rsv_ok);  // no event: still registered
    resp = f.rx_state();
    EXPECT_EQ(resp.flags.get(), static_cast<uint16_t>(acmp_flags::FAST_CONNECT | acmp_flags::REGISTERING_FAILED));
    EXPECT_TRUE(f.l.set_started(SINK, false));
    resp = f.rx_state();
    EXPECT_EQ(
        resp.flags.get(),
        static_cast<uint16_t>(acmp_flags::FAST_CONNECT | acmp_flags::REGISTERING_FAILED | acmp_flags::STREAMING_WAIT));
}

// 5.5.3.5.45
TEST(acmp_milan_listener, rsv_ok_unbind)
{
    Fixture f;
    f.reserve();
    expect_unbind(f);
    EXPECT_EQ(f.unsettled.size(), size_t{1});
}

// 5.5.3.5.46-47: noted only
TEST(acmp_milan_listener, rsv_ok_discovered_and_departed)
{
    Fixture f;
    f.reserve();
    f.discover();
    f.depart();
    EXPECT_TRUE(f.state() == State::settled_rsv_ok);
}

// 5.5.3.5.48: EVT_TK_UNREGISTERED: SRP stopped, re-probe
TEST(acmp_milan_listener, rsv_ok_unregistered)
{
    Fixture f;
    f.reserve();
    f.l.set_registration(SINK, MilanTalkerRegistration::none, f.now);
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    EXPECT_EQ(f.unsettled.size(), size_t{1});
    EXPECT_EQ(f.sink().probing_status, pbsta::PASSIVE);

    Fixture g;
    g.reserve();
    g.discover();
    g.l.set_registration(SINK, MilanTalkerRegistration::none, g.now);
    EXPECT_TRUE(g.state() == State::prb_w_delay);
    EXPECT_EQ(g.sink().probing_status, pbsta::ACTIVE);
    g.sent.clear();
    g.advance(milliseconds{500});
    EXPECT_EQ(g.sent.size(), size_t{1});
    expect_probe(g.sent[0]);
}

// Registrations reported while not settled are not kept
TEST(acmp_milan_listener, registration_ignored_unless_settled)
{
    Fixture f;
    f.bind();
    f.l.set_registration(SINK, MilanTalkerRegistration::advertise, f.now);
    EXPECT_TRUE(f.sink().registration == MilanTalkerRegistration::none);
    f.respond();
    EXPECT_TRUE(f.state() == State::settled_no_rsv);
}

// --- START/STOP_STREAMING (Sections 5.4.2.19-20, 5.3.8.7) --------------------

TEST(acmp_milan_listener, started_state)
{
    Fixture f;
    EXPECT_FALSE(f.l.set_started(SINK, false));  // unbound: no effect
    f.settle();
    f.binding_changes.clear();
    EXPECT_FALSE(f.l.set_started(SINK, true));  // already started
    EXPECT_TRUE(f.l.set_started(SINK, false));
    EXPECT_FALSE(f.sink().binding.started);
    EXPECT_EQ(f.binding_changes.size(), size_t{1});
    EXPECT_TRUE(f.state() == State::settled_no_rsv);  // nothing else changes
    EXPECT_TRUE(f.l.set_started(SINK, true));
}

// --- The ADP discovery state machine (Section 5.6.4) -------------------------

// 5.6.4.5.1: the grandmaster and domain must be ours
TEST(acmp_milan_listener, discovery_needs_our_gptp)
{
    Fixture f;
    f.restore();
    f.discover(1, OTHER_GM);
    EXPECT_FALSE(f.sink().discovered);
    auto other_domain = f.adp(1);
    other_domain.gptp_domain_number = uint8_t{1};
    f.l.receive_adp(other_domain, GM, 0, f.now);
    EXPECT_FALSE(f.sink().discovered);
    f.depart();  // ignored when not discovered
    EXPECT_TRUE(f.state() == State::prb_w_avail);
    auto other_talker = f.adp(1);
    other_talker.entity_id = OTHER_TALKER;
    f.l.receive_adp(other_talker, GM, 0, f.now);
    EXPECT_FALSE(f.sink().discovered);
    f.discover(1);
    EXPECT_TRUE(f.sink().discovered);
}

// 5.6.4.5.2-3: another interface index is ignored
TEST(acmp_milan_listener, discovery_interface_index)
{
    Fixture f;
    f.delay_state();
    f.l.receive_adp(f.adp(0, false, GM, 1), GM, 0, f.now);
    EXPECT_TRUE(f.sink().discovered);
    f.l.receive_adp(f.adp(0, true, GM, 1), GM, 0, f.now);  // index back to 0, other interface
    EXPECT_TRUE(f.state() == State::prb_w_delay);
}

// 5.6.4.5.2: available_index going backwards (wrap-safe) is a restart:
// DEPARTED then DISCOVERED
TEST(acmp_milan_listener, discovery_talker_restart)
{
    Fixture f;
    f.bind();  // PRB_W_RESP
    f.discover(10);
    f.discover(11);  // forward: nothing
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    f.discover(3);  // backwards: departed (PRB_W_AVAIL), then discovered (PRB_W_DELAY)
    EXPECT_TRUE(f.state() == State::prb_w_delay);
    EXPECT_TRUE(f.sink().discovered);
    EXPECT_EQ(f.sink().available_index, uint32_t{3});

    // Across the wrap: 0xFFFFFFFF -> 2 is forward
    Fixture g;
    g.bind();
    g.discover(0xFFFFFFFFU);
    g.discover(2);
    EXPECT_TRUE(g.state() == State::prb_w_resp);
    g.discover(2);  // equal: a restart
    EXPECT_TRUE(g.state() == State::prb_w_delay);

    // A restarted talker on another grandmaster: departed, not rediscovered
    Fixture h;
    h.delay_state();
    h.l.receive_adp(h.adp(0, true, OTHER_GM), GM, 0, h.now);
    EXPECT_TRUE(h.state() == State::prb_w_avail);
    EXPECT_FALSE(h.sink().discovered);
}

// 5.6.4.5.4: TMR_NO_ADP is valid_time x 2 s, restarted by each advertisement
TEST(acmp_milan_listener, discovery_timeout)
{
    Fixture f;
    f.bind();
    f.discover(1);
    f.advance(seconds{15});
    f.discover(2);
    f.advance(seconds{19});
    EXPECT_TRUE(f.sink().discovered);
    EXPECT_TRUE(f.state() == State::prb_w_retry);  // the probe timed out meanwhile
    f.advance(seconds{1});
    EXPECT_FALSE(f.sink().discovered);
    EXPECT_TRUE(f.state() == State::prb_w_avail);
}

// The whole sequence a restart with a saved binding goes through
TEST(acmp_milan_listener, restart_reprobes_the_saved_talker)
{
    Fixture f;
    f.restore();
    f.discover();
    f.advance(milliseconds{500});
    EXPECT_TRUE(f.state() == State::prb_w_resp);
    expect_probe(f.sent.back());
    f.respond();
    EXPECT_TRUE(f.state() == State::settled_no_rsv);
    f.l.set_registration(SINK, MilanTalkerRegistration::advertise, f.now);
    EXPECT_TRUE(f.state() == State::settled_rsv_ok);
}

// The default TMR_DELAY is within [200 ms, 1 s]
TEST(acmp_milan_listener, default_delay_range)
{
    sm::TimePoint now{std::chrono::seconds{1}};
    for (int i = 0; i < 50; ++i) {
        MilanListener m{Eui64{}.from_uint64(0x0200000000000000ULL + static_cast<uint64_t>(i)), 1};
        (void)m.restore(0, MilanSinkBinding{.talker_entity_id = TALKER});
        AdpDu du{};
        du.set_message_type(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);
        du.set_valid_time(10);
        du.entity_id = TALKER;
        m.receive_adp(du, tsn::ClockIdentity{}, 0, now);
        EXPECT_TRUE(m.sink(0)->state == State::prb_w_delay);
        auto const delay = m.sink(0)->deadline - now;
        EXPECT_TRUE(delay >= milliseconds{200} && delay <= seconds{1});
    }
}

// Status notifications only for what GET_STREAM_INFO shows
TEST(acmp_milan_listener, status_notifications)
{
    Fixture f;
    f.bind();
    f.status_changes.clear();
    f.advance(milliseconds{200});  // PRB_W_RESP -> RESP2: nothing visible changes
    EXPECT_TRUE(f.status_changes.empty());
    f.advance(milliseconds{200});  // acmp status 7
    EXPECT_EQ(f.status_changes.size(), size_t{1});
    (void)f.rx_state();
    EXPECT_EQ(f.status_changes.size(), size_t{1});
    // A rebinding to another talker is a change even with the same probing status
    f.bind();  // back to PRB_W_RESP, acmp status 0
    f.status_changes.clear();
    f.bind(0, OTHER_TALKER, 4);
    EXPECT_EQ(f.status_changes.size(), size_t{1});
}

TEST_MAIN(statusbar_atdecc, atdecc_acmp_milan_listener_test)
