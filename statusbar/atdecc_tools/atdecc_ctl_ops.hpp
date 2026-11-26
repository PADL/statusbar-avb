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
#include <iterator>
#include <optional>
#include <span>
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

/// Resolve a descriptor-type token: an unsigned integer (decimal or 0x hex)
/// or a standard descriptor type name (e.g. "AUDIO_CLUSTER",
/// case-insensitive). Returns nullopt for unknown names / bad numbers.
[[nodiscard]] inline auto parse_descriptor_type(std::string_view token) -> std::optional<uint16_t>
{
    if (token.empty()) {
        return std::nullopt;
    }
    if (std::isdigit(static_cast<unsigned char>(token.front())) != 0) {
        uint32_t v = 0;
        auto const base = token.starts_with("0x") || token.starts_with("0X") ? 16 : 10;
        auto const digits = (base == 16) ? token.substr(2) : token;
        for (char const c : digits) {
            int digit = -1;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (base == 16 && std::isxdigit(static_cast<unsigned char>(c)) != 0) {
                digit = 10 + (std::tolower(static_cast<unsigned char>(c)) - 'a');
            }
            if (digit < 0) {
                return std::nullopt;
            }
            v = (v * static_cast<uint32_t>(base)) + static_cast<uint32_t>(digit);
            if (v > 0xFFFF) {
                return std::nullopt;
            }
        }
        return static_cast<uint16_t>(v);
    }
    // Name lookup against the standard descriptor-type name table. The table
    // holds display names ("Audio Cluster"); normalize both sides to
    // UPPER_SNAKE so authors can write the standard's AUDIO_CLUSTER spelling.
    auto to_upper_snake = [](std::string_view s) {
        std::string out{s};
        for (char& c : out) {
            c = (c == ' ') ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return out;
    };
    auto const wanted = to_upper_snake(token);
    for (uint16_t t = 0; t <= atdecc::aem::DESCRIPTOR_CONTROL_BLOCK; ++t) {
        if (wanted == to_upper_snake(atdecc::aem::descriptor_type_name(t))) {
            return t;
        }
    }
    return std::nullopt;
}

/// A single batch operation parsed from the TOML file.
enum class OpKind : uint8_t
{
    Connect,
    Disconnect,
    SetClockSource,
    GetClockSource,
    Identify,
    SetSignalSelector,
    GetSignalSelector,
};

struct Op
{
    OpKind kind{OpKind::Connect};
    Endpoint talker{};     ///< connect/disconnect
    Endpoint listener{};   ///< connect/disconnect
    std::string entity{};  ///< clock-source / identify / signal-selector ops: target entity name
    uint16_t clock_domain{0};
    uint16_t clock_source{0};
    bool identify_on{true};     ///< identify: desired state
    uint16_t descriptor{0};     ///< signal-selector ops: SIGNAL_SELECTOR descriptor index
    uint16_t signal_type{0};    ///< set_signal_selector: source signal_type
    uint16_t signal_index{0};   ///< set_signal_selector: source signal_index
    uint16_t signal_output{0};  ///< set_signal_selector: source signal_output
};

//
// Discovery: the entities a command names (pure, network-free)
//

/// How long an entity a command names is looked for, counted from the
/// ENTITY_DISCOVER the tool starts with. IEEE 1722.1-2021 6.2.4.2.2 has an
/// entity wait a random time before it answers one, "across the range of zero
/// (0) to 1/5 of the valid time of the ATDECC Entity": up to 12.4 s for the
/// largest valid time, 62 s, and up to 4 s for the 20 s of a Milan device
/// (Milan v1.3 5.6.2: valid_time 10, in units of 2 s). The rest is for
/// reading the entity's name.
constexpr int64_t NAMED_ENTITY_WAIT_MS = 13'000;

/// The discovery window of a command that names its entities, or that was
/// given none of its own: what is seen in it is enough to go on, since a named
/// entity that is missing is waited for beyond it.
constexpr int64_t DISCOVERY_WINDOW_MS = 1'500;

/// How long a command collects ENTITY_AVAILABLE messages before it goes on.
/// @param asked_ms --discover-ms; 0 (its default) or less leaves the choice here.
/// @param lists_every_entity The command is a list of whatever is on the
///        network. It names nothing that could be waited for, so by default
///        it listens until no entity can still be on its way
///        (NAMED_ENTITY_WAIT_MS). Any other command takes DISCOVERY_WINDOW_MS.
[[nodiscard]] constexpr auto discovery_window_ms(int64_t const asked_ms, bool const lists_every_entity) noexcept -> int64_t
{
    if (asked_ms > 0) {
        return asked_ms;
    }
    return lists_every_entity ? NAMED_ENTITY_WAIT_MS : DISCOVERY_WINDOW_MS;
}

/// Whether @p name is an entity of @p entities, by resolve_entity()'s
/// matching: an EUI-64 literal is itself, any other name is a
/// case-insensitive part of a discovered entity's name. An entity is in the
/// snapshot from its ENTITY_AVAILABLE on, but nameless until its ENTITY
/// descriptor is read, so its name is not known before that. No name at all
/// is nothing to look for.
[[nodiscard]] inline auto entity_known(std::string_view name, std::vector<EntityDisplayInfo> const& entities) -> bool
{
    ieee::Eui64 direct{};
    if (name.empty() || parse_eui64(name, direct)) {
        return true;
    }
    auto const needle = detail::to_lower(name);
    return std::any_of(entities.begin(), entities.end(), [&](auto const& e) {
        return detail::to_lower(e.name).find(needle) != std::string::npos;
    });
}

/// Those of @p names that are no entity of @p entities yet.
[[nodiscard]] inline auto unknown_entities(std::span<std::string const> names, std::vector<EntityDisplayInfo> const& entities)
    -> std::vector<std::string>
{
    std::vector<std::string> unknown;
    std::copy_if(
        names.begin(), names.end(), std::back_inserter(unknown), [&](auto const& name) { return !entity_known(name, entities); });
    return unknown;
}

/// How much of NAMED_ENTITY_WAIT_MS is left, @p since_discover_ms after the
/// ENTITY_DISCOVER went out: an entity that has not answered by the end of it
/// is not going to.
[[nodiscard]] constexpr auto named_entity_wait_left_ms(int64_t const since_discover_ms) noexcept -> int64_t
{
    return std::clamp<int64_t>(NAMED_ENTITY_WAIT_MS - since_discover_ms, 0, NAMED_ENTITY_WAIT_MS);
}

/// The entities @p op names: the talker and the listener of a connect or a
/// disconnect, the target entity of anything else.
[[nodiscard]] inline auto op_entity_names(Op const& op) -> std::vector<std::string>
{
    bool const is_stream_op = op.kind == OpKind::Connect || op.kind == OpKind::Disconnect;
    return is_stream_op ? std::vector<std::string>{op.talker.name, op.listener.name} : std::vector<std::string>{op.entity};
}

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

/// Parse an `[[identify]]` table into an Op. Requires `entity` (string);
/// `state` optional ("on"/"off", default "on").
[[nodiscard]] inline auto parse_identify_op(toml::Table const& t, std::string& err) -> std::optional<Op>
{
    auto const* ent = t.get("entity");
    auto const ents = (ent != nullptr) ? ent->as_string() : std::nullopt;
    if (!ents) {
        err = "identify requires string 'entity'";
        return std::nullopt;
    }
    bool on = true;
    if (auto const* st = t.get("state"); st != nullptr) {
        auto const sts = st->as_string();
        if (!sts || (*sts != "on" && *sts != "off")) {
            err = "identify 'state' must be \"on\" or \"off\"";
            return std::nullopt;
        }
        on = (*sts == "on");
    }
    Op op{};
    op.kind = OpKind::Identify;
    op.entity = std::string{*ents};
    op.identify_on = on;
    return op;
}

/// Parse a `[[set_signal_selector]]` / `[[get_signal_selector]]` table into an
/// Op. Requires `entity` (string); `descriptor` optional (int, default 0).
/// For set: `signal_type` required (descriptor-type name or integer),
/// `signal_index` / `signal_output` optional ints (default 0).
[[nodiscard]] inline auto parse_signal_selector_op(toml::Table const& t, bool const is_set, std::string& err) -> std::optional<Op>
{
    auto const* ent = t.get("entity");
    auto const ents = (ent != nullptr) ? ent->as_string() : std::nullopt;
    if (!ents) {
        err = "set_signal_selector/get_signal_selector requires string 'entity'";
        return std::nullopt;
    }
    Op op{};
    op.kind = is_set ? OpKind::SetSignalSelector : OpKind::GetSignalSelector;
    op.entity = std::string{*ents};

    auto read_u16 = [&t, &err](std::string_view key, uint16_t& out) -> bool {
        auto const* v = t.get(key);
        if (v == nullptr) {
            return true;
        }
        auto const i = v->as_integer();
        if (!i || *i < 0 || *i > 0xFFFF) {
            err = std::format("'{}' must be an integer 0-65535", key);
            return false;
        }
        out = static_cast<uint16_t>(*i);
        return true;
    };
    if (!read_u16("descriptor", op.descriptor)) {
        return std::nullopt;
    }
    if (!is_set) {
        return op;
    }

    auto const* st = t.get("signal_type");
    std::optional<uint16_t> sig_type;
    if (st != nullptr) {
        if (auto const s = st->as_string()) {
            sig_type = parse_descriptor_type(*s);
        } else if (auto const i = st->as_integer(); i && *i >= 0 && *i <= 0xFFFF) {
            sig_type = static_cast<uint16_t>(*i);
        }
    }
    if (!sig_type) {
        err = "set_signal_selector requires 'signal_type' (descriptor type name or integer)";
        return std::nullopt;
    }
    op.signal_type = *sig_type;
    if (!read_u16("signal_index", op.signal_index) || !read_u16("signal_output", op.signal_output)) {
        return std::nullopt;
    }
    return op;
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
/// first, then clock-source sets, then signal-selector sets/gets, then
/// identifies, then disconnects (deterministic ordering independent of key
/// order in the file). On error, returns nullopt and fills `err`.
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
    if (auto e = detail::parse_array_of_tables(root, "set_signal_selector", ops, [](toml::Table const& t, std::string& er) {
            return detail::parse_signal_selector_op(t, /*is_set=*/true, er);
        })) {
        err = *e;
        return std::nullopt;
    }
    if (auto e = detail::parse_array_of_tables(root, "get_signal_selector", ops, [](toml::Table const& t, std::string& er) {
            return detail::parse_signal_selector_op(t, /*is_set=*/false, er);
        })) {
        err = *e;
        return std::nullopt;
    }
    if (auto e = detail::parse_array_of_tables(
            root, "identify", ops, [](toml::Table const& t, std::string& er) { return detail::parse_identify_op(t, er); })) {
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
        err = "batch file contains no [[connect]], [[set_clock_source]], [[set_signal_selector]], "
              "[[get_signal_selector]], [[identify]], or [[disconnect]] entries";
        return std::nullopt;
    }
    return ops;
}

//
// supervise: stale-stream self-heal helpers (pure, network-free)
//

/// Bit positions in the counters_valid bitmap for the frame-movement counters
/// we poll (IEEE 1722.1-2021 7.4.42): FRAMES_RX is bit 11 of the STREAM_INPUT
/// set (Table 7-156), FRAMES_TX bit 7 of the STREAM_OUTPUT set (Table 7-158).
/// (Mirrors atdecc_aem_get_counters_tool.cpp and avb_entity_stream_info.hpp.)
constexpr size_t STREAM_INPUT_FRAMES_RX_BIT = 11;
constexpr size_t STREAM_OUTPUT_FRAMES_TX_BIT = 7;

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
///
/// That order is IEEE 1722.1's, where the listener answers the controller
/// last, with what the talker told it. A Milan listener answers first and by
/// itself: "the Controller Bind process does not involve the Talker" (Milan
/// v1.3 5.5.2.4, and 5.5.2.5 for an unbind). It probes the talker after a
/// bind, with the same two messages under the names PROBE_TX_COMMAND and
/// PROBE_TX_RESPONSE, and sends the talker nothing at an unbind. So a
/// listener's SUCCESS is never a fault, whatever was or was not seen of the
/// talker: ListenerAlone and ProbeRejected say what was.
enum class HandshakeFault : uint8_t
{
    None,              ///< the talker and the listener both answered SUCCESS
    ListenerNoRelay,   ///< listener never relayed the command to the talker (listener-side / bad ids)
    TalkerNoReply,     ///< listener relayed but the talker never answered (talker-side / bad talker uid)
    TalkerRejected,    ///< talker answered with a non-SUCCESS status
    ListenerNoReply,   ///< talker said SUCCESS but the listener's response was not seen on the bus
    ListenerRejected,  ///< listener returned a non-SUCCESS status to the controller
    ListenerAlone,     ///< listener answered SUCCESS and no talker response was seen: a Milan listener
    ProbeRejected,     ///< listener answered SUCCESS and the talker answered its probe with a non-SUCCESS status
};

namespace detail {

/// classify_handshake() for a handshake the listener answered with SUCCESS.
[[nodiscard]] inline auto classify_acknowledged(HandshakeLegs const& legs) -> HandshakeFault
{
    if (!legs.talker_status) {
        return HandshakeFault::ListenerAlone;
    }
    return *legs.talker_status == atdecc::ACMP_STATUS_SUCCESS ? HandshakeFault::None : HandshakeFault::ProbeRejected;
}

}  // namespace detail

/// Classify a handshake by its first broken leg. Pure function over the observed
/// legs so the diagnosis is unit-testable without a socket.
[[nodiscard]] inline auto classify_handshake(HandshakeLegs const& legs) -> HandshakeFault
{
    if (legs.listener_status == atdecc::ACMP_STATUS_SUCCESS) {
        return detail::classify_acknowledged(legs);
    }
    if (!legs.relay_seen) {
        return HandshakeFault::ListenerNoRelay;
    }
    if (!legs.talker_status) {
        return HandshakeFault::TalkerNoReply;
    }
    if (*legs.talker_status != atdecc::ACMP_STATUS_SUCCESS) {
        return HandshakeFault::TalkerRejected;
    }
    return legs.listener_status ? HandshakeFault::ListenerRejected : HandshakeFault::ListenerNoReply;
}

/// How long a traced connect is watched after the listener's SUCCESS for the
/// talker's answer to a probe. Milan v1.3 5.5.3.6: the listener sends its
/// PROBE_TX_COMMAND right after the BIND_RX_RESPONSE, or after a random delay
/// of up to 1 s, and repeats it once when 200 ms pass with no response.
constexpr int64_t PROBE_WATCH_MS = 1'500;

/// Whether a traced handshake that our command has completed is worth
/// watching for PROBE_WATCH_MS more: a connect the listener answered with
/// SUCCESS before any talker response was seen. Under IEEE 1722.1 the talker
/// has answered by then; a Milan listener probes the talker afterwards.
[[nodiscard]] constexpr auto awaits_probe_response(HandshakeLegs const& legs, bool const is_connect) noexcept -> bool
{
    return is_connect && legs.listener_status == atdecc::ACMP_STATUS_SUCCESS && !legs.talker_status;
}

namespace detail {

/// handshake_verdict() for a listener that answered SUCCESS with no talker
/// response seen.
[[nodiscard]] inline auto listener_alone_verdict(Op const& op, bool const is_connect) -> std::string
{
    if (!is_connect) {
        return std::format(
            "listener '{}' answered SUCCESS without asking the talker -- a Milan listener unbinds by itself (Milan "
            "v1.3 5.5.2.5): unbound",
            op.listener.name);
    }
    return std::format(
        "listener '{}' answered SUCCESS and no response of talker '{}' was seen -- a Milan listener acknowledges a "
        "bind by itself and probes the talker afterwards (Milan v1.3 5.5.2.4): bound, the talker has not answered a "
        "probe yet",
        op.listener.name,
        op.talker.name);
}

/// handshake_verdict() for a handshake both ends answered with SUCCESS.
[[nodiscard]] inline auto success_verdict(HandshakeLegs const& legs) -> std::string
{
    return legs.relay_seen ? "all four legs completed with SUCCESS"
                           : "talker and listener answered SUCCESS (the listener's command to the talker was not seen on the bus)";
}

}  // namespace detail

/// The verdict of a traced (dis)connect handshake in one line: the first leg
/// that broke and whose side it is on, or what the handshake came to.
[[nodiscard]] inline auto handshake_verdict(Op const& op, bool const is_connect, HandshakeLegs const& legs) -> std::string
{
    switch (classify_handshake(legs)) {
        case HandshakeFault::ListenerNoRelay:
            return std::format(
                "listener '{}' never relayed {} -- it did not accept/forward our command (wrong "
                "listener id/uid, or the LISTENER entity is the fault)",
                op.listener.name,
                is_connect ? "CONNECT_TX_COMMAND" : "DISCONNECT_TX_COMMAND");
        case HandshakeFault::TalkerNoReply:
            return std::format(
                "listener relayed to the talker but talker '{}' never answered -- TALKER-side (wrong "
                "talker uid :{}, or the talker is silent/declining, e.g. third-party devices)",
                op.talker.name,
                op.talker.unique_id);
        case HandshakeFault::TalkerRejected:
            return std::format(
                "talker '{}' REJECTED the stream: {}", op.talker.name, atdecc::acmp_status_name(*legs.talker_status));
        case HandshakeFault::ListenerNoReply:
            return "talker answered SUCCESS but the listener's response to the controller was not seen "
                   "(likely just missed on the bus -- the connection is probably up)";
        case HandshakeFault::ListenerRejected:
            return std::format(
                "listener '{}' returned {} to the controller", op.listener.name, atdecc::acmp_status_name(*legs.listener_status));
        case HandshakeFault::ListenerAlone:
            return detail::listener_alone_verdict(op, is_connect);
        case HandshakeFault::ProbeRejected:
            return std::format(
                "listener '{}' answered SUCCESS and talker '{}' answered its probe with {} -- bound (Milan v1.3 "
                "5.5.2.4), and the talker refuses the stream",
                op.listener.name,
                op.talker.name,
                atdecc::acmp_status_name(*legs.talker_status));
        case HandshakeFault::None:
            break;
    }
    return detail::success_verdict(legs);
}

/// Whether the outcome of one of our ACMP commands confirms the (dis)connect of
/// one listener sink: it must be OUR *_RX_RESPONSE, with SUCCESS, for that sink.
/// The talker's *_TX_RESPONSE leg of the same handshake, and every other
/// controller's transaction, are seen on the bus too (as ConnectionAdded /
/// ConnectionRemoved events) and confirm nothing about our command.
[[nodiscard]] inline auto confirms_sink_command(
    AcmpCommandCompletedEvent const& ev, bool is_connect, ieee::Eui64 const& listener_id, uint16_t listener_unique_id) -> bool
{
    uint8_t const wanted =
        is_connect ? atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE : atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE;
    return ev.ok() && ev.message_type == wanted && ev.listener_entity_id == listener_id &&
        ev.listener_unique_id == listener_unique_id;
}

/// A stream is "stale" when the second counter sample did not increase over the
/// first. Equal (frozen) or decreasing (counter reset / wrap with no progress)
/// both count as stale; a strict increase means the stream is live.
[[nodiscard]] inline auto is_stale(uint64_t sample1, uint64_t sample2) -> bool
{
    return !(sample2 > sample1);
}

}  // namespace statusbar::atdecc_tools
