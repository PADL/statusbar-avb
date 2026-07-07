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

TEST_MAIN(statusbar_atdecc_tools, atdecc_ctl_resolve_test)
