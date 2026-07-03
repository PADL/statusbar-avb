#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Pure (network-free) helpers for the scriptable ATDECC controller
/// (`statusbar-atdecc-ctl`): endpoint parsing, entity-name resolution, and
/// batch-TOML op parsing. Kept header-only and side-effect-free so they are
/// unit-testable without a socket or a live network.

#include "statusbar/atdecc/atdecc_acmp_types.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_model.hpp"
#include "statusbar/atdecc_tools/atdecc_tools_common.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/toml/toml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace statusbar::atdecc_tools {

/// A talker/listener endpoint parsed from a `NAME:UID` token (UID defaults to 0
/// if the `:UID` suffix is omitted). NAME may itself be an EUI-64 literal.
struct Endpoint
{
    std::string name;
    uint16_t unique_id{0};
};

/// Parse a `NAME:UID` (or bare `NAME`) token. Returns nullopt if NAME is empty
/// or UID is non-numeric / out of range. NAME may contain ':' itself only when
/// it is an EUI-64 (8 colon-separated bytes); to disambiguate, the unique_id is
/// taken from the LAST ':'-separated field only when that field is a pure
/// decimal number AND the remainder still looks like a name/EUI-64.
[[nodiscard]] inline auto parse_endpoint(std::string_view token) -> std::optional<Endpoint>
{
    if (token.empty()) {
        return std::nullopt;
    }
    auto const colon = token.rfind(':');
    // An EUI-64 literal has 7 colons; a NAME:UID has the uid after the last
    // colon. Treat the tail as a uid only if it is all digits AND the head is
    // non-empty AND the token isn't a bare EUI-64 (8 hex byte fields).
    // colon > 0 keeps the NAME head non-empty: a leading-colon token like ":5"
    // must NOT split to an empty name (which resolve_entity would match against
    // every entity via find("")), so it falls through to a literal-name endpoint.
    if (colon != std::string_view::npos && colon > 0 && colon + 1 < token.size()) {
        auto const tail = token.substr(colon + 1);
        bool const tail_is_num =
            !tail.empty() && std::all_of(tail.begin(), tail.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        // Count colons to detect a bare EUI-64 (xx:xx:xx:xx:xx:xx:xx:xx).
        auto const colon_count = static_cast<size_t>(std::count(token.begin(), token.end(), ':'));
        if (tail_is_num && colon_count != 7) {
            uint64_t uid = 0;
            for (char c : tail) {
                uid = (uid * 10) + static_cast<uint64_t>(c - '0');
                if (uid > 0xFFFF) {
                    return std::nullopt;
                }
            }
            return Endpoint{.name = std::string{token.substr(0, colon)}, .unique_id = static_cast<uint16_t>(uid)};
        }
    }
    return Endpoint{.name = std::string{token}, .unique_id = 0};
}

namespace detail {
[[nodiscard]] inline auto to_lower(std::string_view s) -> std::string
{
    std::string out{s};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
}  // namespace detail

/// Resolve a name to an entity_id against a discovery snapshot.
///  - If `name` parses as an EUI-64 literal, it is used directly.
///  - Otherwise a case-insensitive SUBSTRING match against each discovered
///    entity's display name is attempted.
///  - No match or an ambiguous (multiple distinct entities) match is an error:
///    `err` is filled (listing candidates for ambiguity) and nullopt returned.
[[nodiscard]] inline auto resolve_entity(std::string_view name, std::vector<EntityDisplayInfo> const& entities, std::string& err)
    -> std::optional<ieee::Eui64>
{
    if (ieee::Eui64 direct{}; parse_eui64(name, direct)) {
        return direct;
    }
    auto const needle = detail::to_lower(name);
    // Collect distinct matching entities (by entity_id).
    std::vector<EntityDisplayInfo const*> matches;
    for (auto const& e : entities) {
        if (detail::to_lower(e.name).find(needle) == std::string::npos) {
            continue;
        }
        bool const already =
            std::any_of(matches.begin(), matches.end(), [&](auto const* m) { return m->entity_id == e.entity_id; });
        if (!already) {
            matches.push_back(&e);
        }
    }
    if (matches.empty()) {
        err = std::format("no entity matches name '{}'", name);
        return std::nullopt;
    }
    if (matches.size() > 1) {
        std::string candidates;
        for (auto const* m : matches) {
            candidates += std::format("\n  - {} ({})", m->name, ieee::to_string(m->entity_id).view());
        }
        err = std::format("name '{}' is ambiguous; candidates:{}", name, candidates);
        return std::nullopt;
    }
    return matches.front()->entity_id;
}

/// A single batch operation parsed from the TOML file.
enum class OpKind : uint8_t
{
    Connect,
    Disconnect,
    SetClockSource,
    GetClockSource,
};

struct Op
{
    OpKind kind{OpKind::Connect};
    Endpoint talker{};     ///< connect/disconnect
    Endpoint listener{};   ///< connect/disconnect
    std::string entity{};  ///< clock-source ops: target entity name
    uint16_t clock_domain{0};
    uint16_t clock_source{0};
};

namespace detail {
/// Parse a `[[connect]]`/`[[disconnect]]` table into an Op. Requires
/// `talker` and `listener` string keys (each a NAME:UID token).
[[nodiscard]] inline auto parse_stream_op(toml::Table const& t, OpKind kind, std::string& err) -> std::optional<Op>
{
    auto const* tk = t.get("talker");
    auto const* ls = t.get("listener");
    auto const tks = (tk != nullptr) ? tk->as_string() : std::nullopt;
    auto const lss = (ls != nullptr) ? ls->as_string() : std::nullopt;
    if (!tks || !lss) {
        err = "connect/disconnect requires string 'talker' and 'listener' (NAME:UID)";
        return std::nullopt;
    }
    auto const te = parse_endpoint(*tks);
    auto const le = parse_endpoint(*lss);
    if (!te || !le) {
        err = std::format("invalid talker/listener token: '{}' / '{}'", *tks, *lss);
        return std::nullopt;
    }
    return Op{.kind = kind, .talker = *te, .listener = *le, .entity = {}, .clock_domain = 0, .clock_source = 0};
}

/// Parse a `[[set_clock_source]]` table into an Op. Requires `entity` (string)
/// and `clock_source` (int); `clock_domain` optional (default 0).
[[nodiscard]] inline auto parse_clock_op(toml::Table const& t, std::string& err) -> std::optional<Op>
{
    auto const* ent = t.get("entity");
    auto const* src = t.get("clock_source");
    auto const ents = (ent != nullptr) ? ent->as_string() : std::nullopt;
    auto const srci = (src != nullptr) ? src->as_integer() : std::nullopt;
    if (!ents || !srci || *srci < 0 || *srci > 0xFFFF) {
        err = "set_clock_source requires string 'entity' and integer 'clock_source' (0-65535)";
        return std::nullopt;
    }
    auto const* dom = t.get("clock_domain");
    int64_t const domain = (dom != nullptr) ? dom->integer_or(0) : 0;
    if (domain < 0 || domain > 0xFFFF) {
        err = "clock_domain out of range (0-65535)";
        return std::nullopt;
    }
    return Op{
        .kind = OpKind::SetClockSource,
        .talker = {},
        .listener = {},
        .entity = std::string{*ents},
        .clock_domain = static_cast<uint16_t>(domain),
        .clock_source = static_cast<uint16_t>(*srci)};
}

/// Append every table in `root[key]` (a TOML array-of-tables) parsed via `fn`.
template <typename Fn>
[[nodiscard]] inline auto parse_array_of_tables(toml::Table const& root, std::string_view key, std::vector<Op>& out, Fn fn)
    -> std::optional<std::string>
{
    auto const* v = root.get(key);
    if (v == nullptr) {
        return std::nullopt;  // key absent is fine
    }
    auto const* arr = v->as_array();
    if (arr == nullptr) {
        return std::format("'{}' must be an array of tables ([[{}]])", key, key);
    }
    for (size_t i = 0; i < arr->size(); ++i) {
        auto const* elem = (*arr)[i].as_table();
        if (elem == nullptr) {
            return std::format("'{}' entry {} is not a table", key, i);
        }
        std::string err;
        auto op = fn(*elem, err);
        if (!op) {
            return std::format("'{}' entry {}: {}", key, i, err);
        }
        out.push_back(*op);
    }
    return std::nullopt;
}
}  // namespace detail

/// Parse a batch-ops TOML document into a flat op list. Connections are applied
/// first, then clock-source sets, then disconnects (deterministic ordering
/// independent of key order in the file). On error, returns nullopt and fills
/// `err`.
[[nodiscard]] inline auto parse_batch_ops(toml::Table const& root, std::string& err) -> std::optional<std::vector<Op>>
{
    std::vector<Op> ops;
    if (auto e = detail::parse_array_of_tables(root, "connect", ops, [](toml::Table const& t, std::string& er) {
            return detail::parse_stream_op(t, OpKind::Connect, er);
        })) {
        err = *e;
        return std::nullopt;
    }
    if (auto e = detail::parse_array_of_tables(
            root, "set_clock_source", ops, [](toml::Table const& t, std::string& er) { return detail::parse_clock_op(t, er); })) {
        err = *e;
        return std::nullopt;
    }
    if (auto e = detail::parse_array_of_tables(root, "disconnect", ops, [](toml::Table const& t, std::string& er) {
            return detail::parse_stream_op(t, OpKind::Disconnect, er);
        })) {
        err = *e;
        return std::nullopt;
    }
    if (ops.empty()) {
        err = "batch file contains no [[connect]], [[set_clock_source]], or [[disconnect]] entries";
        return std::nullopt;
    }
    return ops;
}

//
// supervise: stale-stream self-heal helpers (pure, network-free)
//

/// Bit positions in the counters_valid bitmap for the frame-movement counters
/// we poll. FRAMES_RX lives in the STREAM_INPUT counter set; FRAMES_TX in the
/// STREAM_OUTPUT set. (Mirrors atdecc_aem_get_counters_tool.cpp.)
constexpr size_t STREAM_INPUT_FRAMES_RX_BIT = 11;
constexpr size_t STREAM_OUTPUT_FRAMES_TX_BIT = 6;

/// Identifies which counter, on OUR local end, measures frame movement for one
/// leg of a connection.
struct LocalCounter
{
    uint16_t descriptor_type{0};   ///< STREAM_INPUT (we are listener) or STREAM_OUTPUT (we are talker)
    uint16_t descriptor_index{0};  ///< the local stream's unique id
    size_t counter_bit{0};         ///< bit in counters_valid for the frame counter
};

/// Decide which end of a talker->listener leg is us, and therefore which
/// counter to poll. Returns nullopt when neither end is our entity (we can't
/// measure that leg). When we are BOTH ends (loopback) the listener side is
/// preferred, since FRAMES_RX is the receive-side proof of delivery.
[[nodiscard]] inline auto select_local_counter(
    ieee::Eui64 const& our_eid,
    ieee::Eui64 const& talker_eid,
    uint16_t talker_uid,
    ieee::Eui64 const& listener_eid,
    uint16_t listener_uid) -> std::optional<LocalCounter>
{
    if (our_eid == listener_eid) {
        return LocalCounter{
            .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_INPUT,
            .descriptor_index = listener_uid,
            .counter_bit = STREAM_INPUT_FRAMES_RX_BIT};
    }
    if (our_eid == talker_eid) {
        return LocalCounter{
            .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_OUTPUT,
            .descriptor_index = talker_uid,
            .counter_bit = STREAM_OUTPUT_FRAMES_TX_BIT};
    }
    return std::nullopt;
}

//
// connect --trace: per-leg ACMP handshake classification (pure, network-free)
//

/// Observations of one ACMP (dis)connect handshake, reconstructed from the raw
/// ACMP PDUs a passive controller sees on the bus. Each leg is multicast, so all
/// of these are observable, which is what lets us pinpoint where one stalled.
struct HandshakeLegs
{
    bool relay_seen{false};                    ///< listener forwarded the *_TX_COMMAND to the talker
    std::optional<uint8_t> talker_status{};    ///< talker's *_TX_RESPONSE status (nullopt = never answered)
    std::optional<uint8_t> listener_status{};  ///< listener's *_RX_RESPONSE status (nullopt = never answered)
};

/// Which leg of the handshake is at fault, in protocol order. The FIRST broken
/// leg is the actionable one — a later leg can't succeed if an earlier one didn't.
enum class HandshakeFault : uint8_t
{
    None,              ///< all four legs completed with SUCCESS
    ListenerNoRelay,   ///< listener never relayed the command to the talker (listener-side / bad ids)
    TalkerNoReply,     ///< listener relayed but the talker never answered (talker-side / bad talker uid)
    TalkerRejected,    ///< talker answered with a non-SUCCESS status
    ListenerNoReply,   ///< talker said SUCCESS but the listener's response was not seen on the bus
    ListenerRejected,  ///< listener returned a non-SUCCESS status to the controller
};

/// Classify a handshake by its first broken leg. Pure function over the observed
/// legs so the diagnosis is unit-testable without a socket.
[[nodiscard]] inline auto classify_handshake(HandshakeLegs const& legs) -> HandshakeFault
{
    if (!legs.relay_seen) {
        return HandshakeFault::ListenerNoRelay;
    }
    if (!legs.talker_status) {
        return HandshakeFault::TalkerNoReply;
    }
    if (*legs.talker_status != atdecc::ACMP_STATUS_SUCCESS) {
        return HandshakeFault::TalkerRejected;
    }
    if (!legs.listener_status) {
        return HandshakeFault::ListenerNoReply;
    }
    if (*legs.listener_status != atdecc::ACMP_STATUS_SUCCESS) {
        return HandshakeFault::ListenerRejected;
    }
    return HandshakeFault::None;
}

/// A stream is "stale" when the second counter sample did not increase over the
/// first. Equal (frozen) or decreasing (counter reset / wrap with no progress)
/// both count as stale; a strict increase means the stream is live.
[[nodiscard]] inline auto is_stale(uint64_t sample1, uint64_t sample2) -> bool
{
    return !(sample2 > sample1);
}

}  // namespace statusbar::atdecc_tools
