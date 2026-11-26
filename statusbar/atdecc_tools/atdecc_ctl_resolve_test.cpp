// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Unit tests for the network-free helpers behind `statusbar-atdecc-ctl`:
/// endpoint parsing, entity-name resolution, and batch-TOML op parsing.

#include "statusbar/atdecc_tools/atdecc_ctl_ops.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/toml/toml.hpp"

#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc_tools;
using statusbar::ieee::Eui64;

namespace {

auto make_entity(Eui64 id, std::string name) -> EntityDisplayInfo
{
    EntityDisplayInfo e{};
    e.entity_id = id;
    e.name = std::move(name);
    return e;
}

}  // namespace

//
// parse_endpoint
//

TEST(atdecc_ctl_endpoint, name_with_uid)
{
    auto e = parse_endpoint("node-a:3");
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e->name, std::string{"node-a"});
    EXPECT_EQ(e->unique_id, 3U);
}

TEST(atdecc_ctl_endpoint, bare_name_defaults_uid_zero)
{
    auto e = parse_endpoint("audio-iface");
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e->name, std::string{"audio-iface"});
    EXPECT_EQ(e->unique_id, 0U);
}

TEST(atdecc_ctl_endpoint, eui64_literal_is_not_split_on_colon)
{
    // 8 colon-separated bytes (7 colons) must stay whole as the name.
    auto e = parse_endpoint("70:b3:d5:ed:cf:00:00:02");
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e->name, std::string{"70:b3:d5:ed:cf:00:00:02"});
    EXPECT_EQ(e->unique_id, 0U);
}

TEST(atdecc_ctl_endpoint, empty_is_error)
{
    EXPECT_FALSE(parse_endpoint("").has_value());
}

// A leading-colon token must NOT split into an empty name (which would match every
// entity via find("")); it is kept whole as a literal name with uid 0.
TEST(atdecc_ctl_endpoint, leading_colon_does_not_yield_empty_name)
{
    auto e = parse_endpoint(":5");
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e->name, std::string{":5"});
    EXPECT_EQ(e->unique_id, 0U);
}

TEST(atdecc_ctl_endpoint, uid_out_of_range_is_error)
{
    EXPECT_FALSE(parse_endpoint("name:70000").has_value());
}

//
// resolve_entity
//

TEST(atdecc_ctl_resolve, eui64_passthrough)
{
    std::vector<EntityDisplayInfo> entities;  // empty: still resolves a literal
    std::string err;
    auto id = resolve_entity("70:b3:d5:ff:fe:ed:cf:00", entities, err);
    EXPECT_TRUE(id.has_value());
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(*id, (Eui64{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0xCF, 0x00}));
}

TEST(atdecc_ctl_resolve, case_insensitive_substring)
{
    std::vector<EntityDisplayInfo> entities{
        make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 1}, "node-a"),
        make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 2}, "audio-iface"),
    };
    std::string err;
    auto id = resolve_entity("AUDIO-IFACE", entities, err);  // differing case exercises the match
    EXPECT_TRUE(id.has_value());
    EXPECT_EQ(*id, (Eui64{0, 0, 0, 0, 0, 0, 0, 2}));
}

TEST(atdecc_ctl_resolve, no_match_is_error)
{
    std::vector<EntityDisplayInfo> entities{make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 1}, "node-a")};
    std::string err;
    auto id = resolve_entity("dsp-rack", entities, err);
    EXPECT_FALSE(id.has_value());
    EXPECT_FALSE(err.empty());
}

TEST(atdecc_ctl_resolve, ambiguous_is_error)
{
    std::vector<EntityDisplayInfo> entities{
        make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 1}, "node-a"),
        make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 2}, "node-d"),
    };
    std::string err;
    auto id = resolve_entity("node", entities, err);  // matches both
    EXPECT_FALSE(id.has_value());
    EXPECT_FALSE(err.empty());
}

TEST(atdecc_ctl_resolve, same_id_twice_is_not_ambiguous)
{
    // Two display rows for the SAME entity_id resolve unambiguously.
    std::vector<EntityDisplayInfo> entities{
        make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 7}, "node-a"),
        make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 7}, "node-a"),
    };
    std::string err;
    auto id = resolve_entity("node-a", entities, err);
    EXPECT_TRUE(id.has_value());
    EXPECT_EQ(*id, (Eui64{0, 0, 0, 0, 0, 0, 0, 7}));
}

//
// parse_batch_ops
//

//
// Discovery: the entities a command names
//

TEST(atdecc_ctl_discovery, a_name_is_known_once_an_entity_carries_it)
{
    std::vector<EntityDisplayInfo> entities;
    EXPECT_FALSE(entity_known("stage", entities));

    // Seen by its ENTITY_AVAILABLE, its ENTITY descriptor not read yet: no name.
    entities.push_back(make_entity(Eui64{0, 1, 2, 3, 4, 5, 6, 7}, ""));
    EXPECT_FALSE(entity_known("stage", entities));

    entities[0].name = "Stage Left";
    EXPECT_TRUE(entity_known("stage", entities));
    EXPECT_TRUE(entity_known("LEFT", entities));
    EXPECT_FALSE(entity_known("right", entities));
}

TEST(atdecc_ctl_discovery, an_eui64_literal_and_no_name_need_no_discovery)
{
    std::vector<EntityDisplayInfo> const none;
    EXPECT_TRUE(entity_known("00:01:02:03:04:05:06:07", none));
    EXPECT_TRUE(entity_known("", none));
}

TEST(atdecc_ctl_discovery, unknown_entities_lists_the_names_still_missing)
{
    std::vector<EntityDisplayInfo> entities;
    entities.push_back(make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 1}, "node-a"));
    std::vector<std::string> const names{"node-a", "node-d", "00:01:02:03:04:05:06:07"};
    auto const missing = unknown_entities(names, entities);
    EXPECT_EQ(missing.size(), size_t{1});
    EXPECT_TRUE(missing[0] == "node-d");

    entities.push_back(make_entity(Eui64{0, 0, 0, 0, 0, 0, 0, 2}, "node-d"));
    EXPECT_TRUE(unknown_entities(names, entities).empty());
    EXPECT_TRUE(unknown_entities({}, entities).empty());
}

TEST(atdecc_ctl_discovery, the_wait_runs_from_the_discover_and_ends)
{
    // IEEE 1722.1-2021 6.2.4.2.2: an entity answers within 1/5 of its valid
    // time, 12.4 s for the largest. The wait covers that.
    EXPECT_TRUE(NAMED_ENTITY_WAIT_MS >= 12'400);
    EXPECT_EQ(named_entity_wait_left_ms(0), NAMED_ENTITY_WAIT_MS);
    EXPECT_EQ(named_entity_wait_left_ms(1500), NAMED_ENTITY_WAIT_MS - 1500);
    EXPECT_EQ(named_entity_wait_left_ms(NAMED_ENTITY_WAIT_MS - 1), int64_t{1});
    EXPECT_EQ(named_entity_wait_left_ms(NAMED_ENTITY_WAIT_MS), int64_t{0});
    EXPECT_EQ(named_entity_wait_left_ms(NAMED_ENTITY_WAIT_MS + 60'000), int64_t{0});
    EXPECT_EQ(named_entity_wait_left_ms(-5), NAMED_ENTITY_WAIT_MS);
}

TEST(atdecc_ctl_discovery, the_window_is_what_was_asked_for_or_fits_the_command)
{
    // Asked for: that, whatever the command.
    EXPECT_EQ(discovery_window_ms(400, /*lists_every_entity=*/false), int64_t{400});
    EXPECT_EQ(discovery_window_ms(400, /*lists_every_entity=*/true), int64_t{400});
    EXPECT_EQ(discovery_window_ms(60'000, /*lists_every_entity=*/true), int64_t{60'000});
    // Left to us: a command that names its entities waits for those it misses,
    // so a short window does; a list of everything can only listen.
    EXPECT_EQ(discovery_window_ms(0, /*lists_every_entity=*/false), DISCOVERY_WINDOW_MS);
    EXPECT_EQ(discovery_window_ms(0, /*lists_every_entity=*/true), NAMED_ENTITY_WAIT_MS);
    EXPECT_EQ(discovery_window_ms(-1, /*lists_every_entity=*/true), NAMED_ENTITY_WAIT_MS);
    EXPECT_EQ(DISCOVERY_WINDOW_MS, int64_t{1'500});
    EXPECT_TRUE(NAMED_ENTITY_WAIT_MS > DISCOVERY_WINDOW_MS);
}

TEST(atdecc_ctl_discovery, an_op_names_its_endpoints_or_its_entity)
{
    Op connect{};
    connect.kind = OpKind::Connect;
    connect.talker = {.name = "node-a", .unique_id = 0};
    connect.listener = {.name = "node-d", .unique_id = 1};
    connect.entity = "ignored";
    EXPECT_TRUE(op_entity_names(connect) == (std::vector<std::string>{"node-a", "node-d"}));
    connect.kind = OpKind::Disconnect;
    EXPECT_TRUE(op_entity_names(connect) == (std::vector<std::string>{"node-a", "node-d"}));

    Op identify{};
    identify.kind = OpKind::Identify;
    identify.entity = "node-f";
    identify.talker = {.name = "ignored", .unique_id = 0};
    EXPECT_TRUE(op_entity_names(identify) == (std::vector<std::string>{"node-f"}));
}

TEST(atdecc_ctl_batch, parses_connect_clock_disconnect_in_order)
{
    auto doc = toml::parse("[[connect]]\n"
                           "talker = \"node-a:0\"\n"
                           "listener = \"node-d:1\"\n"
                           "[[disconnect]]\n"
                           "talker = \"node-a:1\"\n"
                           "listener = \"node-e:1\"\n"
                           "[[set_clock_source]]\n"
                           "entity = \"audio-iface\"\n"
                           "clock_domain = 0\n"
                           "clock_source = 2\n");
    EXPECT_TRUE(doc.has_value());

    std::string err;
    auto ops = parse_batch_ops(*doc, err);
    EXPECT_TRUE(ops.has_value());
    EXPECT_EQ(ops->size(), 3U);
    // Deterministic order: connect, then set_clock_source, then disconnect.
    EXPECT_TRUE((*ops)[0].kind == OpKind::Connect);
    EXPECT_EQ((*ops)[0].talker.name, std::string{"node-a"});
    EXPECT_EQ((*ops)[0].listener.unique_id, 1U);
    EXPECT_TRUE((*ops)[1].kind == OpKind::SetClockSource);
    EXPECT_EQ((*ops)[1].entity, std::string{"audio-iface"});
    EXPECT_EQ((*ops)[1].clock_source, 2U);
    EXPECT_TRUE((*ops)[2].kind == OpKind::Disconnect);
    EXPECT_EQ((*ops)[2].listener.name, std::string{"node-e"});
}

TEST(atdecc_ctl_batch, parses_signal_selector_ops)
{
    auto doc = toml::parse("[[set_signal_selector]]\n"
                           "entity = \"tone\"\n"
                           "descriptor = 1\n"
                           "signal_type = \"AUDIO_CLUSTER\"\n"
                           "signal_index = 2\n"
                           "[[get_signal_selector]]\n"
                           "entity = \"tone\"\n");
    EXPECT_TRUE(doc.has_value());
    std::string err;
    auto ops = parse_batch_ops(*doc, err);
    EXPECT_TRUE(ops.has_value());
    EXPECT_EQ(ops->size(), 2U);
    EXPECT_TRUE((*ops)[0].kind == OpKind::SetSignalSelector);
    EXPECT_EQ((*ops)[0].descriptor, 1U);
    EXPECT_EQ((*ops)[0].signal_type, uint16_t{0x0014});  // AUDIO_CLUSTER
    EXPECT_EQ((*ops)[0].signal_index, 2U);
    EXPECT_EQ((*ops)[0].signal_output, 0U);
    EXPECT_TRUE((*ops)[1].kind == OpKind::GetSignalSelector);
    EXPECT_EQ((*ops)[1].descriptor, 0U);

    // signal_type is required for set and must resolve.
    auto bad = toml::parse("[[set_signal_selector]]\n"
                           "entity = \"tone\"\n"
                           "signal_type = \"NOT_A_TYPE\"\n");
    EXPECT_TRUE(bad.has_value());
    auto bad_ops = parse_batch_ops(*bad, err);
    EXPECT_FALSE(bad_ops.has_value());
}

TEST(atdecc_ctl_batch, parse_descriptor_type_tokens)
{
    EXPECT_EQ(parse_descriptor_type("AUDIO_CLUSTER").value_or(0xDEAD), uint16_t{0x0014});
    EXPECT_EQ(parse_descriptor_type("audio_cluster").value_or(0xDEAD), uint16_t{0x0014});
    EXPECT_EQ(parse_descriptor_type("21").value_or(0xDEAD), uint16_t{21});
    EXPECT_EQ(parse_descriptor_type("0x0014").value_or(0xDEAD), uint16_t{0x0014});
    EXPECT_FALSE(parse_descriptor_type("NOT_A_TYPE").has_value());
    EXPECT_FALSE(parse_descriptor_type("70000").has_value());
    EXPECT_FALSE(parse_descriptor_type("").has_value());
}

TEST(atdecc_ctl_batch, missing_listener_is_error)
{
    auto doc = toml::parse("[[connect]]\n"
                           "talker = \"node-a:0\"\n");
    EXPECT_TRUE(doc.has_value());
    std::string err;
    auto ops = parse_batch_ops(*doc, err);
    EXPECT_FALSE(ops.has_value());
    EXPECT_FALSE(err.empty());
}

TEST(atdecc_ctl_batch, empty_document_is_error)
{
    auto doc = toml::parse("title = \"nothing actionable\"\n");
    EXPECT_TRUE(doc.has_value());
    std::string err;
    auto ops = parse_batch_ops(*doc, err);
    EXPECT_FALSE(ops.has_value());
}

//
// supervise: select_local_counter
//

TEST(atdecc_ctl_supervise, select_local_counter_we_are_talker)
{
    Eui64 const us{0, 0, 0, 0, 0, 0, 0, 1};
    Eui64 const them{0, 0, 0, 0, 0, 0, 0, 2};
    // We are the talker (uid 3); they are the listener.
    auto lc = select_local_counter(us, us, 3, them, 5);
    EXPECT_TRUE(lc.has_value());
    EXPECT_EQ(lc->descriptor_type, statusbar::atdecc::aem::DESCRIPTOR_STREAM_OUTPUT);
    EXPECT_EQ(lc->descriptor_index, 3U);
    EXPECT_EQ(lc->counter_bit, STREAM_OUTPUT_FRAMES_TX_BIT);
    // IEEE 1722.1-2021 Table 7-158: FRAMES_TX is bit 7 (bit 6 is TIMESTAMP_NOT_VALID).
    EXPECT_EQ(STREAM_OUTPUT_FRAMES_TX_BIT, 7U);
    EXPECT_EQ(STREAM_INPUT_FRAMES_RX_BIT, 11U);
}

TEST(atdecc_ctl_supervise, select_local_counter_we_are_listener)
{
    Eui64 const us{0, 0, 0, 0, 0, 0, 0, 1};
    Eui64 const them{0, 0, 0, 0, 0, 0, 0, 2};
    // We are the listener (uid 7); they are the talker.
    auto lc = select_local_counter(us, them, 4, us, 7);
    EXPECT_TRUE(lc.has_value());
    EXPECT_EQ(lc->descriptor_type, statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(lc->descriptor_index, 7U);
    EXPECT_EQ(lc->counter_bit, STREAM_INPUT_FRAMES_RX_BIT);
}

TEST(atdecc_ctl_supervise, select_local_counter_neither_end_is_us)
{
    Eui64 const us{0, 0, 0, 0, 0, 0, 0, 1};
    Eui64 const talker{0, 0, 0, 0, 0, 0, 0, 2};
    Eui64 const listener{0, 0, 0, 0, 0, 0, 0, 3};
    auto lc = select_local_counter(us, talker, 4, listener, 5);
    EXPECT_FALSE(lc.has_value());
}

//
// supervise: is_stale
//

TEST(atdecc_ctl_supervise, is_stale_increasing_is_live)
{
    EXPECT_FALSE(is_stale(100, 101));
}

TEST(atdecc_ctl_supervise, is_stale_equal_is_stale)
{
    EXPECT_TRUE(is_stale(222935, 222935));
}

TEST(atdecc_ctl_supervise, is_stale_decreasing_is_stale)
{
    EXPECT_TRUE(is_stale(500, 499));
}

//
// connect --trace: handshake classification (the diagnostic verdict)
//

TEST(atdecc_ctl_trace, all_legs_success_is_none)
{
    HandshakeLegs legs{
        .relay_seen = true, .talker_status = atdecc::ACMP_STATUS_SUCCESS, .listener_status = atdecc::ACMP_STATUS_SUCCESS};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::None);
}

TEST(atdecc_ctl_trace, no_relay_blames_listener)
{
    // Listener never forwarded the command -> listener-side fault (the exact
    // case the entity tests rule out: a healthy listener always relays).
    HandshakeLegs legs{};  // relay_seen=false
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::ListenerNoRelay);
}

TEST(atdecc_ctl_trace, relay_but_no_talker_reply_blames_talker)
{
    HandshakeLegs legs{.relay_seen = true};  // talker never answered
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::TalkerNoReply);
}

TEST(atdecc_ctl_trace, talker_nonsuccess_is_rejected)
{
    HandshakeLegs legs{.relay_seen = true, .talker_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::TalkerRejected);
}

TEST(atdecc_ctl_trace, talker_ok_but_no_listener_reply)
{
    HandshakeLegs legs{.relay_seen = true, .talker_status = atdecc::ACMP_STATUS_SUCCESS};  // listener_status nullopt
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::ListenerNoReply);
}

TEST(atdecc_ctl_trace, listener_nonsuccess_is_rejected)
{
    HandshakeLegs legs{
        .relay_seen = true,
        .talker_status = atdecc::ACMP_STATUS_SUCCESS,
        .listener_status = atdecc::ACMP_STATUS_LISTENER_EXCLUSIVE};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::ListenerRejected);
}

TEST(atdecc_ctl_trace, first_broken_leg_wins)
{
    // A talker rejection must be reported even though a later listener leg also
    // failed -- the earliest fault is the actionable one.
    HandshakeLegs legs{
        .relay_seen = true,
        .talker_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH,
        .listener_status = atdecc::ACMP_STATUS_LISTENER_EXCLUSIVE};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::TalkerRejected);
}

//
// connect --trace with a Milan listener: it answers the controller by itself
// (Milan v1.3 5.5.2.4, 5.5.2.5), and probes the talker after a bind
//

TEST(atdecc_ctl_trace, a_listener_that_answered_alone_is_no_fault)
{
    // A Milan bind seen before its probe, and every Milan unbind: the
    // listener's SUCCESS and nothing of the talker.
    HandshakeLegs legs{.listener_status = atdecc::ACMP_STATUS_SUCCESS};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::ListenerAlone);
    // The probe went out and the talker has not answered it.
    legs.relay_seen = true;
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::ListenerAlone);
}

TEST(atdecc_ctl_trace, an_answered_probe_completes_the_handshake)
{
    HandshakeLegs legs{
        .relay_seen = true, .talker_status = atdecc::ACMP_STATUS_SUCCESS, .listener_status = atdecc::ACMP_STATUS_SUCCESS};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::None);
    // Both answers seen and the command between them missed: nobody's fault.
    legs.relay_seen = false;
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::None);
}

TEST(atdecc_ctl_trace, a_refused_probe_is_told_apart_from_a_refused_connect)
{
    HandshakeLegs legs{
        .relay_seen = true,
        .talker_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH,
        .listener_status = atdecc::ACMP_STATUS_SUCCESS};
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::ProbeRejected);
    // Under IEEE 1722.1 the listener passes the talker's status on.
    legs.listener_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH;
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::TalkerRejected);
    legs.listener_status.reset();
    EXPECT_TRUE(classify_handshake(legs) == HandshakeFault::TalkerRejected);
}

TEST(atdecc_ctl_trace, a_connect_is_watched_for_the_probe_after_the_listeners_success_only)
{
    HandshakeLegs legs{};
    EXPECT_FALSE(awaits_probe_response(legs, /*is_connect=*/true));
    legs.listener_status = atdecc::ACMP_STATUS_LISTENER_EXCLUSIVE;
    EXPECT_FALSE(awaits_probe_response(legs, /*is_connect=*/true));
    legs.listener_status = atdecc::ACMP_STATUS_SUCCESS;
    EXPECT_TRUE(awaits_probe_response(legs, /*is_connect=*/true));
    legs.relay_seen = true;
    EXPECT_TRUE(awaits_probe_response(legs, /*is_connect=*/true));
    // An unbind sends the talker nothing: there is nothing to watch for.
    EXPECT_FALSE(awaits_probe_response(legs, /*is_connect=*/false));
    legs.talker_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH;
    EXPECT_FALSE(awaits_probe_response(legs, /*is_connect=*/true));
    // Milan v1.3 5.5.3.6: up to 1 s of delay and two tries 200 ms apart.
    EXPECT_TRUE(PROBE_WATCH_MS >= 1'400);
}

namespace {

auto traced_op() -> Op
{
    Op op{};
    op.talker = {.name = "node-a", .unique_id = 6};
    op.listener = {.name = "node-d", .unique_id = 2};
    return op;
}

auto has(std::string const& text, std::string_view part) -> bool
{
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST(atdecc_ctl_trace, the_verdict_blames_nobody_for_a_milan_bind_or_unbind)
{
    HandshakeLegs const alone{.listener_status = atdecc::ACMP_STATUS_SUCCESS};
    auto const bind = handshake_verdict(traced_op(), /*is_connect=*/true, alone);
    EXPECT_TRUE(has(bind, "listener 'node-d' answered SUCCESS"));
    EXPECT_TRUE(has(bind, "Milan v1.3 5.5.2.4"));
    EXPECT_TRUE(has(bind, "bound, the talker has not answered a probe yet"));
    EXPECT_FALSE(has(bind, "never"));
    EXPECT_FALSE(has(bind, "fault"));

    auto const unbind = handshake_verdict(traced_op(), /*is_connect=*/false, alone);
    EXPECT_TRUE(has(unbind, "listener 'node-d' answered SUCCESS"));
    EXPECT_TRUE(has(unbind, "Milan v1.3 5.5.2.5"));
    EXPECT_TRUE(has(unbind, ": unbound"));
    EXPECT_FALSE(has(unbind, "never"));
    EXPECT_FALSE(has(unbind, "fault"));
}

TEST(atdecc_ctl_trace, the_verdict_of_a_refused_probe_names_the_status)
{
    HandshakeLegs const legs{
        .relay_seen = true,
        .talker_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH,
        .listener_status = atdecc::ACMP_STATUS_SUCCESS};
    auto const text = handshake_verdict(traced_op(), /*is_connect=*/true, legs);
    EXPECT_TRUE(has(text, "talker 'node-a' answered its probe with "));
    EXPECT_TRUE(has(text, atdecc::acmp_status_name(atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH)));
    EXPECT_TRUE(has(text, "bound"));
}

TEST(atdecc_ctl_trace, the_verdicts_of_an_ieee_1722_1_handshake_are_as_they_were)
{
    auto const op = traced_op();
    EXPECT_TRUE(
        handshake_verdict(op, true, {}) ==
        "listener 'node-d' never relayed CONNECT_TX_COMMAND -- it did not accept/forward our command (wrong listener id/uid, "
        "or the LISTENER entity is the fault)");
    EXPECT_TRUE(has(handshake_verdict(op, false, {}), "never relayed DISCONNECT_TX_COMMAND"));
    EXPECT_TRUE(
        handshake_verdict(op, true, {.relay_seen = true}) ==
        "listener relayed to the talker but talker 'node-a' never answered -- TALKER-side (wrong talker uid :6, or the talker "
        "is silent/declining, e.g. third-party devices)");
    EXPECT_TRUE(
        has(handshake_verdict(op, true, {.relay_seen = true, .talker_status = atdecc::ACMP_STATUS_TALKER_NO_BANDWIDTH}),
            "talker 'node-a' REJECTED the stream: "));
    EXPECT_TRUE(
        has(handshake_verdict(op, true, {.relay_seen = true, .talker_status = atdecc::ACMP_STATUS_SUCCESS}),
            "talker answered SUCCESS but the listener's response to the controller was not seen"));
    EXPECT_TRUE(
        has(handshake_verdict(
                op,
                true,
                {.relay_seen = true,
                 .talker_status = atdecc::ACMP_STATUS_SUCCESS,
                 .listener_status = atdecc::ACMP_STATUS_LISTENER_EXCLUSIVE}),
            "listener 'node-d' returned "));
    HandshakeLegs done{
        .relay_seen = true, .talker_status = atdecc::ACMP_STATUS_SUCCESS, .listener_status = atdecc::ACMP_STATUS_SUCCESS};
    EXPECT_TRUE(handshake_verdict(op, true, done) == "all four legs completed with SUCCESS");
    done.relay_seen = false;
    EXPECT_TRUE(has(handshake_verdict(op, true, done), "talker and listener answered SUCCESS"));
}

TEST(atdecc_ctl_batch, parses_identify_ops)
{
    auto doc = toml::parse("[[identify]]\n"
                           "entity = \"tone\"\n"
                           "[[identify]]\n"
                           "entity = \"88:a2:9e:ff:fe:82:83:00\"\n"
                           "state = \"off\"\n");
    EXPECT_TRUE(doc.has_value());
    std::string err;
    auto ops = parse_batch_ops(*doc, err);
    EXPECT_TRUE(ops.has_value());
    EXPECT_EQ(ops->size(), 2u);
    EXPECT_TRUE((*ops)[0].kind == OpKind::Identify);
    EXPECT_EQ((*ops)[0].entity, "tone");
    EXPECT_TRUE((*ops)[0].identify_on);  // state defaults to "on"
    EXPECT_TRUE((*ops)[1].kind == OpKind::Identify);
    EXPECT_FALSE((*ops)[1].identify_on);
}

TEST(atdecc_ctl_batch, identify_rejects_bad_state_and_missing_entity)
{
    std::string err;
    auto bad_state = toml::parse("[[identify]]\n"
                                 "entity = \"tone\"\n"
                                 "state = \"blink\"\n");
    EXPECT_TRUE(bad_state.has_value());
    EXPECT_FALSE(parse_batch_ops(*bad_state, err).has_value());

    auto no_entity = toml::parse("[[identify]]\nstate = \"on\"\n");
    EXPECT_TRUE(no_entity.has_value());
    EXPECT_FALSE(parse_batch_ops(*no_entity, err).has_value());
}

//
// confirms_sink_command: connect/disconnect succeed on OUR command's RX response
//

namespace {

auto completed(uint8_t message_type, uint8_t status, Eui64 const& listener, uint16_t uid, bool timed_out = false)
    -> AcmpCommandCompletedEvent
{
    AcmpCommandCompletedEvent ev{};
    ev.message_type = message_type;
    ev.status = status;
    ev.timed_out = timed_out;
    ev.listener_entity_id = listener;
    ev.listener_unique_id = uid;
    return ev;
}

}  // namespace

TEST(atdecc_ctl_handshake, only_our_rx_response_for_the_sink_confirms)
{
    using namespace statusbar::atdecc;
    Eui64 const sink{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    Eui64 const other{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x78};

    // The talker's TX leg of our own handshake names the sink too: not a confirmation.
    EXPECT_FALSE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, ACMP_STATUS_SUCCESS, sink, 1), true, sink, 1));
    // Another sink, a failure, a timeout, the other command: no.
    EXPECT_FALSE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS, other, 1), true, sink, 1));
    EXPECT_FALSE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS, sink, 2), true, sink, 1));
    EXPECT_FALSE(confirms_sink_command(
        completed(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_LISTENER_EXCLUSIVE, sink, 1), true, sink, 1));
    EXPECT_FALSE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, ACMP_STATUS_SUCCESS, sink, 1, true), true, sink, 1));
    EXPECT_FALSE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS, sink, 1), true, sink, 1));
    // Our CONNECT_RX_RESPONSE / DISCONNECT_RX_RESPONSE with SUCCESS for the sink: yes.
    EXPECT_TRUE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS, sink, 1), true, sink, 1));
    EXPECT_TRUE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS, sink, 1), false, sink, 1));
    EXPECT_FALSE(
        confirms_sink_command(completed(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS, sink, 1), false, sink, 1));
}

TEST_MAIN(statusbar_atdecc_tools, atdecc_ctl_resolve_test)
