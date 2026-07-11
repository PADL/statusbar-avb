// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// statusbar-atdecc-ctl — scriptable (non-interactive) ATDECC controller.
//
// A thin command front-end over ControllerSimple: discovers entities via ADP,
// resolves NAMES to entity IDs (or accepts raw EUI-64s), and drives ACMP
// connect/disconnect and AEM set/get-clock-source. Supports a TOML batch file
// so many operations run in one process (one discovery pass), instead of
// spawning one controller per connection.
//
// Commands:
//   --command=list            [--entity NAME]            list entities (or one's clocks)
//   --command=connect         --talker NAME:UID --listener NAME:UID
//   --command=disconnect      --talker NAME:UID --listener NAME:UID
//   --command=set-clock-source --entity NAME --clock-source S [--clock-domain D]
//   --command=get-clock-source --entity NAME             [--clock-domain D]
//   --command=identify        --entity NAME [--state on|off]
//   --command=set-signal-selector --entity NAME [--descriptor N] --signal-type TYPE [--signal-index I] [--signal-output O]
//   --command=get-signal-selector --entity NAME [--descriptor N]
//   --command=batch           --file ops.toml
//   --command=supervise       --file ops.toml  [--watch-ms N]
//                             ensure each declared connection exists (idempotent:
//                             connect only legs that aren't connected; never tear
//                             down a live one). --watch-ms>0 repeats forever.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_simple.hpp"
#include "statusbar/atdecc_tools/atdecc_ctl_ops.hpp"
#include "statusbar/atdecc_tools/atdecc_tools_common.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/toml/toml.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::ieee;
using namespace statusbar::net;

using atdecc_tools::ControllerSimple;
using atdecc_tools::EntityDisplayInfo;
using atdecc_tools::Op;
using atdecc_tools::OpKind;

auto const g_start_time = std::chrono::steady_clock::now();
auto elapsed_ns() -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start_time).count();
}

struct Config
{
    std::string command;  // list|connect|disconnect|set-clock-source|get-clock-source|batch
    std::string interface_name;
    std::string talker;       // NAME:UID
    std::string listener;     // NAME:UID
    std::string entity;       // NAME (clock-source target / list detail)
    std::string file;         // batch TOML path
    std::string state{"on"};  // identify: desired state (on|off)
    std::string signal_type;  // set-signal-selector: source descriptor type (name or integer)
    int64_t discover_ms{1500};
    int64_t clock_domain{0};
    int64_t clock_source{0};
    int64_t descriptor{0};     // signal-selector ops: SIGNAL_SELECTOR descriptor index
    int64_t signal_index{0};   // set-signal-selector: source signal_index
    int64_t signal_output{0};  // set-signal-selector: source signal_output
    int64_t interval_ms{700};  // deprecated/ignored (was the supervise counter-sample gap; supervise is now connection-state based)
    int64_t watch_ms{0};       // supervise: >0 => loop forever, sleeping this long between passes
    bool once{true};           // supervise: single pass (default); watch-ms>0 overrides
    bool trace{false};         // connect/disconnect: print a per-leg ACMP handshake verdict
};

auto build_arg_specs(Config& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    specs.add_choice(
        "command",
        "Action: list, validate, connect, disconnect, get-rx-state, get-tx-state, set-clock-source, get-clock-source, "
        "set-signal-selector, get-signal-selector, identify, batch, supervise",
        {"list",
         "validate",
         "connect",
         "disconnect",
         "get-rx-state",
         "get-tx-state",
         "set-clock-source",
         "get-clock-source",
         "set-signal-selector",
         "get-signal-selector",
         "identify",
         "batch",
         "supervise"},
        "list",
        [&](auto v) { config.command = std::string{v}; });
    specs.add_device("interface", "Network interface (e.g., eth0)", "", [&](auto v) { config.interface_name = std::string{v}; });
    specs.add<std::string>(
        "talker", "Talker endpoint NAME:UID (connect/disconnect)", "", [&](auto v) { config.talker = std::string{v}; });
    specs.add<std::string>(
        "listener", "Listener endpoint NAME:UID (connect/disconnect)", "", [&](auto v) { config.listener = std::string{v}; });
    specs.add<std::string>("entity", "Target entity NAME (clock-source / identify / list detail)", "", [&](auto v) {
        config.entity = std::string{v};
    });
    specs.add_choice("state", "identify: desired state", {"on", "off"}, "on", [&](auto v) { config.state = std::string{v}; });
    specs.add<std::string>(
        "file", "Batch/supervise TOML file (command=batch|supervise)", "", [&](auto v) { config.file = std::string{v}; });
    specs.add<int64_t>("discover-ms", "Discovery window in ms", 1500, [&](auto v) { config.discover_ms = v; });
    specs.add<int64_t>("clock-domain", "CLOCK_DOMAIN index (clock-source ops)", 0, [&](auto v) { config.clock_domain = v; });
    specs.add<int64_t>("clock-source", "CLOCK_SOURCE index (set-clock-source)", 0, [&](auto v) { config.clock_source = v; });
    specs.add<int64_t>(
        "descriptor", "SIGNAL_SELECTOR descriptor index (signal-selector ops)", 0, [&](auto v) { config.descriptor = v; });
    specs.add<std::string>(
        "signal-type", "set-signal-selector: source descriptor type (name like AUDIO_CLUSTER, or integer)", "", [&](auto v) {
            config.signal_type = std::string{v};
        });
    specs.add<int64_t>("signal-index", "set-signal-selector: source signal_index", 0, [&](auto v) { config.signal_index = v; });
    specs.add<int64_t>("signal-output", "set-signal-selector: source signal_output", 0, [&](auto v) { config.signal_output = v; });
    specs.add<int64_t>("interval-ms", "deprecated/ignored (supervise is now connection-state based)", 700, [&](auto v) {
        config.interval_ms = v;
    });
    specs.add<int64_t>("watch-ms", "supervise: if >0, loop forever sleeping this long between passes (ms)", 0, [&](auto v) {
        config.watch_ms = v;
    });
    specs.add_flag("once", "supervise: run a single pass (default; ignored when --watch-ms>0)", [&](auto v) { config.once = v; });
    specs.add_flag(
        "trace", "connect/disconnect: print a per-leg ACMP handshake verdict (RX -> relay -> talker -> listener)", [&](auto v) {
            config.trace = v;
        });
    return specs;
}

void print_usage(char const* prog, args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 7> examples{
        "--interface=eth0 --command=list",
        "--interface=eth0 --command=connect --talker=node-a:0 --listener=node-d:0",
        "--interface=eth0 --command=set-clock-source --entity=node-a --clock-source=1",
        "--interface=eth0 --command=set-signal-selector --entity=node-a --signal-type=AUDIO_CLUSTER --signal-index=1",
        "--interface=eth0 --command=identify --entity=node-a --state=on",
        "--interface=eth0 --command=batch --file=ops.toml",
        "--interface=eth0 --command=supervise --file=ops.toml",
    };
    config::default_print_usage(
        prog, specs, "Scriptable ATDECC controller (connect / clock-source / discovery / batch).", examples);
}

// Run the reactor for up to `budget_ms`, invoking `on_event` for every drained
// event, until `done()` returns true or the budget elapses. Returns done().
template <typename DoneFn, typename EventFn>
auto pump(MessageReactor& reactor, ControllerSimple& ctrl, int64_t budget_ms, DoneFn done, EventFn on_event) -> bool
{
    int64_t const deadline = elapsed_ns() + (budget_ms * 1'000'000);
    while (elapsed_ns() < deadline) {
        reactor.poll_once(20);
        for (auto const& ev : ctrl.drain_events()) {
            on_event(ev);
        }
        if (done()) {
            return true;
        }
    }
    return false;
}

// Resolve a NAME (or EUI-64) against the current discovery snapshot, printing an
// error on failure.
auto resolve_or_report(std::string_view name, std::vector<EntityDisplayInfo> const& snap) -> std::optional<Eui64>
{
    std::string err;
    auto id = atdecc_tools::resolve_entity(name, snap, err);
    if (!id) {
        std::println(stderr, "Error: {}", err);
    }
    return id;
}

// Print the leg-by-leg verdict for a (dis)connect handshake. `done` is whether
// the high-level success criterion was met; the legs explain WHY when it wasn't.
void print_trace_verdict(Op const& op, bool is_connect, atdecc_tools::HandshakeLegs const& legs, bool done)
{
    char const* const relay_name = is_connect ? "CONNECT_TX_COMMAND" : "DISCONNECT_TX_COMMAND";
    char const* const tresp_name = is_connect ? "CONNECT_TX_RESPONSE" : "DISCONNECT_TX_RESPONSE";
    char const* const lresp_name = is_connect ? "CONNECT_RX_RESPONSE" : "DISCONNECT_RX_RESPONSE";
    char const* const rx_name = is_connect ? "CONNECT_RX_COMMAND" : "DISCONNECT_RX_COMMAND";

    std::println(
        "  ACMP handshake  talker {}:{}  listener {}:{}",
        op.talker.name,
        op.talker.unique_id,
        op.listener.name,
        op.listener.unique_id);
    std::println("    [1] controller -> listener   {:<22} sent", rx_name);
    std::println("    [2] listener   -> talker     {:<22} {}", relay_name, legs.relay_seen ? "relayed" : "NOT SEEN");
    std::println(
        "    [3] talker     -> listener   {:<22} {}",
        tresp_name,
        legs.talker_status ? acmp_status_name(*legs.talker_status) : "NO REPLY");
    std::println(
        "    [4] listener   -> controller {:<22} {}",
        lresp_name,
        legs.listener_status ? acmp_status_name(*legs.listener_status) : "NO REPLY");

    // Pinpoint the first leg that broke -- this is the actionable verdict.
    std::string verdict;
    switch (atdecc_tools::classify_handshake(legs)) {
        case atdecc_tools::HandshakeFault::ListenerNoRelay:
            verdict = std::format(
                "listener '{}' never relayed {} -- it did not accept/forward our command (wrong "
                "listener id/uid, or the LISTENER entity is the fault)",
                op.listener.name,
                relay_name);
            break;
        case atdecc_tools::HandshakeFault::TalkerNoReply:
            verdict = std::format(
                "listener relayed to the talker but talker '{}' never answered -- TALKER-side (wrong "
                "talker uid :{}, or the talker is silent/declining, e.g. third-party devices)",
                op.talker.name,
                op.talker.unique_id);
            break;
        case atdecc_tools::HandshakeFault::TalkerRejected:
            verdict = std::format("talker '{}' REJECTED the stream: {}", op.talker.name, acmp_status_name(*legs.talker_status));
            break;
        case atdecc_tools::HandshakeFault::ListenerNoReply:
            verdict = "talker answered SUCCESS but the listener's response to the controller was not seen "
                      "(likely just missed on the bus -- the connection is probably up)";
            break;
        case atdecc_tools::HandshakeFault::ListenerRejected:
            verdict = std::format(
                "listener '{}' returned {} to the controller", op.listener.name, acmp_status_name(*legs.listener_status));
            break;
        case atdecc_tools::HandshakeFault::None:
            verdict = "all four legs completed with SUCCESS";
            break;
    }
    std::println("    verdict: {}{}", verdict, (done ? "" : "  [unconfirmed]"));
}

// Execute a single op. Returns true on success. When `trace` is set (connect/
// disconnect only), prints a per-leg ACMP handshake verdict for diagnosis.
auto execute_op(MessageReactor& reactor, ControllerSimple& ctrl, Op const& op, bool trace = false) -> bool
{
    auto const snap = ctrl.get_display_entities();
    using atdecc_tools::AcmpTraceEvent;
    using atdecc_tools::ConnectionAddedEvent;
    using atdecc_tools::ConnectionRemovedEvent;
    using atdecc_tools::ControllerAction;
    using atdecc_tools::ControllerActionKind;
    using atdecc_tools::HandshakeLegs;
    using atdecc_tools::StatusChangedEvent;

    switch (op.kind) {
        case OpKind::Connect:
        case OpKind::Disconnect: {
            auto const tid = resolve_or_report(op.talker.name, snap);
            auto const lid = resolve_or_report(op.listener.name, snap);
            if (!tid || !lid) {
                return false;
            }
            bool const is_connect = (op.kind == OpKind::Connect);
            ControllerAction action{};
            action.kind = is_connect ? ControllerActionKind::ConnectStream : ControllerActionKind::DisconnectStream;
            action.request.talker_entity_id = *tid;
            action.request.talker_unique_id = op.talker.unique_id;
            action.request.listener_entity_id = *lid;
            action.request.listener_unique_id = op.listener.unique_id;
            std::println(
                "{} {}:{} -> {}:{} ...",
                is_connect ? "connect" : "disconnect",
                op.talker.name,
                op.talker.unique_id,
                op.listener.name,
                op.listener.unique_id);

            // Leg matching: the relayed command, talker response and listener
            // response all carry OUR listener id+uid (the talker fields aren't
            // faithfully echoed by every device, so we key on the listener sink).
            uint8_t const relay_mt = is_connect ? ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND : ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND;
            uint8_t const tresp_mt = is_connect ? ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE : ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE;
            uint8_t const lresp_mt = is_connect ? ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE : ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE;

            HandshakeLegs legs{};
            ctrl.set_acmp_trace(trace);

            bool done = false;
            // Match on the LISTENER sink we asked to (dis)connect — the response's
            // talker fields aren't faithfully echoed by every device (e.g. the DSP processor),
            // so requiring talker_entity_id equality dropped real successes.
            auto const on_event = [&](auto const& ev) {
                if (is_connect) {
                    if (auto const* c = std::get_if<ConnectionAddedEvent>(&ev)) {
                        if (c->connection.listener_entity_id == *lid && c->connection.listener_unique_id == op.listener.unique_id) {
                            done = true;
                        }
                    }
                } else if (auto const* r = std::get_if<ConnectionRemovedEvent>(&ev)) {
                    if (r->listener_entity_id == *lid && r->listener_unique_id == op.listener.unique_id) {
                        done = true;
                    }
                }
                if (trace) {
                    if (auto const* t = std::get_if<AcmpTraceEvent>(&ev)) {
                        if (t->listener_entity_id == *lid && t->listener_unique_id == op.listener.unique_id) {
                            if (t->message_type == relay_mt) {
                                legs.relay_seen = true;
                            } else if (t->message_type == tresp_mt) {
                                legs.talker_status = t->status;
                            } else if (t->message_type == lresp_mt) {
                                legs.listener_status = t->status;
                            }
                        }
                    }
                }
            };
            // Some listeners (notably the DSP processor) only respond on a later attempt, so
            // re-issue the command up to a few times across the budget. Re-sending
            // a CONNECT/DISCONNECT for the same stream is idempotent.
            constexpr int kAttempts = 4;
            for (int attempt = 0; attempt < kAttempts && !done; ++attempt) {
                ctrl.dispatch(action, elapsed_ns());
                (void)pump(reactor, ctrl, 2500, [&] { return done; }, on_event);
            }
            ctrl.set_acmp_trace(false);
            std::println("  {}", done ? "OK" : "FAILED (no response after retries)");
            if (trace) {
                print_trace_verdict(op, is_connect, legs, done);
            }
            return done;
        }
        case OpKind::SetClockSource:
        case OpKind::GetClockSource: {
            auto const eid = resolve_or_report(op.entity, snap);
            if (!eid) {
                return false;
            }
            bool const is_set = (op.kind == OpKind::SetClockSource);
            ControllerAction action{};
            action.kind = is_set ? ControllerActionKind::SetClockSource : ControllerActionKind::GetClockSource;
            action.request.talker_entity_id = *eid;
            action.request.desc_index = static_cast<uint16_t>(op.clock_domain);
            action.request.clock_source_index = op.clock_source;
            std::println(
                "{} {} clock_domain={}{} ...",
                is_set ? "set-clock-source" : "get-clock-source",
                op.entity,
                op.clock_domain,
                is_set ? std::format(" clock_source={}", op.clock_source) : std::string{});
            ctrl.dispatch(action, elapsed_ns());
            bool done = false;
            bool ok = false;
            std::string text;
            (void)pump(
                reactor,
                ctrl,
                5000,
                [&] { return done; },
                [&](auto const& ev) {
                    if (auto const* s = std::get_if<StatusChangedEvent>(&ev)) {
                        text = s->status;
                        // Success if the AEM status name is SUCCESS (case-insensitive).
                        auto const lower = atdecc_tools::detail::to_lower(text);
                        ok = lower.find("success") != std::string::npos;
                        done = true;
                    }
                });
            std::println("  {}", done ? text : std::string{"FAILED (no response within 5s)"});
            return done && ok;
        }
        case OpKind::SetSignalSelector:
        case OpKind::GetSignalSelector: {
            auto const eid = resolve_or_report(op.entity, snap);
            if (!eid) {
                return false;
            }
            bool const is_set = (op.kind == OpKind::SetSignalSelector);
            ControllerAction action{};
            action.kind = is_set ? ControllerActionKind::SetSignalSelector : ControllerActionKind::GetSignalSelector;
            action.request.talker_entity_id = *eid;
            action.request.desc_index = op.descriptor;
            action.request.signal_type = op.signal_type;
            action.request.signal_index = op.signal_index;
            action.request.signal_output = op.signal_output;
            std::println(
                "{} {} descriptor={}{} ...",
                is_set ? "set-signal-selector" : "get-signal-selector",
                op.entity,
                op.descriptor,
                is_set ? std::format(
                             " signal_type={} signal_index={} signal_output={}",
                             atdecc::aem::descriptor_type_name(op.signal_type),
                             op.signal_index,
                             op.signal_output)
                       : std::string{});
            ctrl.dispatch(action, elapsed_ns());
            bool done = false;
            bool ok = false;
            std::string text;
            (void)pump(
                reactor,
                ctrl,
                5000,
                [&] { return done; },
                [&](auto const& ev) {
                    if (auto const* s = std::get_if<StatusChangedEvent>(&ev)) {
                        auto const lower = atdecc_tools::detail::to_lower(s->status);
                        if (lower.find("failed") != std::string::npos) {
                            text = s->status;
                            done = true;
                        } else if (lower.find("signal_selector") != std::string::npos) {
                            text = s->status;
                            ok = lower.find("success") != std::string::npos;
                            done = true;
                        }
                    }
                });
            std::println("  {}", done ? text : std::string{"FAILED (no response within 5s)"});
            return done && ok;
        }
        case OpKind::Identify: {
            auto const eid = resolve_or_report(op.entity, snap);
            if (!eid) {
                return false;
            }
            ControllerAction action{};
            action.kind = ControllerActionKind::IdentifyEntity;
            action.request.talker_entity_id = *eid;
            action.request.identify_state = op.identify_on ? int8_t{1} : int8_t{0};
            std::println("identify {} {} ...", op.entity, op.identify_on ? "on" : "off");
            ctrl.dispatch(action, elapsed_ns());
            bool done = false;
            bool ok = false;
            std::string text;
            (void)pump(
                reactor,
                ctrl,
                5000,
                [&] { return done; },
                [&](auto const& ev) {
                    if (auto const* s = std::get_if<StatusChangedEvent>(&ev)) {
                        auto const lower = atdecc_tools::detail::to_lower(s->status);
                        // Dispatch acks immediately ("Identify on/off"); the
                        // SET_CONTROL response is what proves the entity heard us.
                        if (lower.find("failed") != std::string::npos) {
                            text = s->status;
                            done = true;
                        } else if (lower.find("set_control") != std::string::npos) {
                            text = s->status;
                            ok = lower.find("success") != std::string::npos;
                            done = true;
                        }
                    }
                });
            std::println("  {}", done ? text : std::string{"FAILED (no response within 5s)"});
            return done && ok;
        }
    }
    return false;
}

// supervise: is the listener leg connected to THIS talker? Queries ACMP
// GET_RX_STATE (retried — a single datagram can drop) and returns true only on a
// definitive "connected to this talker" reply. Returns false on "not connected"
// OR no reply at all; the caller then (re)asserts the connection, which is
// harmless because the listener treats a CONNECT_RX for an already-connected
// stream as a no-op (idempotent). So a listener that doesn't answer GET_RX_STATE
// still gets connected without churn.
auto leg_connected(
    MessageReactor& reactor,
    ControllerSimple& ctrl,
    Eui64 const& talker_eid,
    uint16_t talker_uid,
    Eui64 const& listener_eid,
    uint16_t listener_uid) -> bool
{
    bool got = false;
    bool connected_here = false;
    constexpr int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts && !got; ++attempt) {
        ctrl.query_rx_state(listener_eid, listener_uid, elapsed_ns());
        (void)pump(
            reactor,
            ctrl,
            600,
            [&] { return got; },
            [&](auto const& ev) {
                if (auto const* r = std::get_if<atdecc_tools::RxStateEvent>(&ev)) {
                    if (r->listener_entity_id == listener_eid && r->listener_unique_id == listener_uid) {
                        got = true;
                        connected_here = r->connected && r->talker_entity_id == talker_eid && r->talker_unique_id == talker_uid;
                    }
                }
            });
    }
    return connected_here;
}

// supervise: (re)assert one leg's connection with a single CONNECT_RX. Idempotent
// at the listener, so calling it on a leg that is already up is a no-op there (no
// teardown, no MSRP churn).
void connect_leg(
    MessageReactor& reactor,
    ControllerSimple& ctrl,
    Eui64 const& talker_eid,
    uint16_t talker_uid,
    Eui64 const& listener_eid,
    uint16_t listener_uid)
{
    using atdecc_tools::ControllerAction;
    using atdecc_tools::ControllerActionKind;

    ControllerAction a{};
    a.kind = ControllerActionKind::ConnectStream;
    a.request.talker_entity_id = talker_eid;
    a.request.talker_unique_id = talker_uid;
    a.request.listener_entity_id = listener_eid;
    a.request.listener_unique_id = listener_uid;
    ctrl.dispatch(a, elapsed_ns());
    (void)pump(reactor, ctrl, 500, [] { return false; }, [](auto const&) {});
}

}  // namespace

int main(int argc, char* argv[])
{
    using namespace statusbar;

    Config config;
    auto specs = build_arg_specs(config);
    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-atdecc-ctl");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (config.interface_name.empty()) {
        std::println(stderr, "Error: --interface is required");
        return 1;
    }

    // Controller entity ID derived from our own NIC MAC (tool-id discriminated).
    atdecc_tools::CommonConfig cc;
    cc.interface_name = config.interface_name;
    auto id_result = atdecc_tools::resolve_controller_id(cc, atdecc_tools::TOOL_ID_CONTROLLER);
    if (!id_result) {
        std::println(stderr, "Error: cannot open interface '{}': {}", config.interface_name, id_result.error().message());
        return 1;
    }

    RawnetContext rawnet;
    if (!rawnet.open(config.interface_name, avtp::AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC)) {
        std::println(stderr, "Error: failed to open raw socket on '{}' (root/cap_net_raw required)", config.interface_name);
        return 1;
    }

    auto pollable = std::make_unique<ControllerSimple>(std::move(rawnet), *id_result);
    auto* ctrl = pollable.get();

    auto& stop = statusbar::itc::install_stop_signal();
    MessageReactor reactor{stop, elapsed_ns, 10};
    reactor.add(std::move(pollable));

    // DISCOVER phase: announce + read every entity's ENTITY descriptor (the
    // controller auto-enqueues that on ENTITY_AVAILABLE, populating names).
    atdecc_tools::ControllerAction discover{};
    discover.kind = atdecc_tools::ControllerActionKind::DiscoverAll;
    ctrl->dispatch(discover, elapsed_ns());
    (void)pump(reactor, *ctrl, config.discover_ms, [] { return false; }, [](auto const&) {});

    auto const snap = ctrl->get_display_entities();

    // ----- list -----
    if (config.command == "list") {
        if (!config.entity.empty()) {
            auto const eid = resolve_or_report(config.entity, snap);
            if (!eid) {
                return 1;
            }
            atdecc_tools::ControllerAction read{};
            read.kind = atdecc_tools::ControllerActionKind::ReadEntityDescriptors;
            read.request.talker_entity_id = *eid;
            ctrl->dispatch(read, elapsed_ns());
            bool got = false;
            (void)pump(
                reactor,
                *ctrl,
                5000,
                [&] { return got; },
                [&](auto const& ev) {
                    if (auto const* d = std::get_if<atdecc_tools::EntityDetailReadyEvent>(&ev)) {
                        if (d->detail.entity_id == *eid) {
                            std::println("== {} ({}) ==", d->detail.name, ieee::to_string(*eid).view());
                            for (auto const& line : d->detail.lines) {
                                std::println("{}", line.text);
                            }
                            got = true;
                        }
                    }
                });
            if (!got) {
                std::println(stderr, "Error: no descriptor detail for '{}' within 5s", config.entity);
                return 1;
            }
            return 0;
        }
        std::println("{} entit{} discovered:", snap.size(), snap.size() == 1 ? "y" : "ies");
        std::println("{:<28} {:<26} talker  listener", "NAME", "ENTITY_ID");
        for (auto const& e : snap) {
            std::println(
                "{:<28} {:<26} {:>6}  {:>8}",
                e.name.empty() ? "(unnamed)" : e.name,
                ieee::to_string(e.entity_id).view(),
                e.talker_stream_sources,
                e.listener_stream_sinks);
        }
        return 0;
    }

    // ----- validate (AEM model compliance) -----
    if (config.command == "validate") {
        if (config.entity.empty()) {
            std::println(stderr, "Error: --entity=<name> is required for validate");
            return 1;
        }
        auto const eid = resolve_or_report(config.entity, snap);
        if (!eid) {
            return 1;
        }
        // Crawl the full descriptor tree, then validate the cached raw descriptors.
        atdecc_tools::ControllerAction read{};
        read.kind = atdecc_tools::ControllerActionKind::ReadEntityDescriptors;
        read.request.talker_entity_id = *eid;
        ctrl->dispatch(read, elapsed_ns());
        bool crawled = false;
        (void)pump(
            reactor,
            *ctrl,
            8000,
            [&] { return crawled; },
            [&](auto const& ev) {
                if (auto const* d = std::get_if<atdecc_tools::EntityDetailReadyEvent>(&ev)) {
                    if (d->detail.entity_id == *eid) {
                        crawled = true;
                    }
                }
            });
        auto const descs = ctrl->cached_descriptors(*eid);
        if (descs.empty()) {
            std::println(stderr, "Error: no descriptors read from '{}' (entity unreachable?)", config.entity);
            return 1;
        }
        std::println("== AEM model validation: {} ({}) ==", config.entity, ieee::to_string(*eid).view());
        std::println("crawled {} descriptors{}", descs.size(), crawled ? "" : " (crawl did not signal complete; partial)");
        auto const findings = atdecc_tools::validate_aem_model(descs);
        int errors = 0;
        int warns = 0;
        for (auto const& f : findings) {
            if (f.severity == atdecc_tools::Severity::Error) {
                ++errors;
            } else if (f.severity == atdecc_tools::Severity::Warn) {
                ++warns;
            }
            std::println("  [{}] {}: {}", atdecc_tools::severity_name(f.severity), f.where, f.message);
        }
        std::println(
            "result: {} error(s), {} warning(s), {} info", errors, warns, static_cast<int>(findings.size()) - errors - warns);
        return errors == 0 ? 0 : 1;
    }

    // ----- get-rx-state (ACMP listener-sink connection state) -----
    // Diagnostic for the macOS acquire path: Audio MIDI Setup / avbdiagnose send
    // ACMP GET_RX_STATE to each of the entity's listener sinks and abort the
    // acquire ("Input stream N ... timed out") if any sink does not reply. This
    // probes exactly that: query one sink (--listener NAME:UID) or every sink of
    // an entity (--entity NAME), and report reply-vs-timeout per sink.
    if (config.command == "get-rx-state") {
        struct Sink
        {
            Eui64 id;
            uint16_t uid;
            std::string label;
        };
        std::vector<Sink> sinks;
        if (auto const l = atdecc_tools::parse_endpoint(config.listener)) {
            auto const lid = resolve_or_report(l->name, snap);
            if (!lid) {
                return 1;
            }
            sinks.push_back({*lid, l->unique_id, std::format("{}:{}", l->name, l->unique_id)});
        } else if (!config.entity.empty()) {
            auto const eid = resolve_or_report(config.entity, snap);
            if (!eid) {
                return 1;
            }
            uint16_t count = 0;
            for (auto const& e : snap) {
                if (e.entity_id == *eid) {
                    count = e.listener_stream_sinks;
                }
            }
            if (count == 0) {
                std::println(stderr, "Error: '{}' advertises no listener sinks", config.entity);
                return 1;
            }
            for (uint16_t i = 0; i < count; ++i) {
                sinks.push_back({*eid, i, std::format("{}:{}", config.entity, i)});
            }
        } else {
            std::println(stderr, "Error: get-rx-state needs --listener=NAME:UID or --entity=NAME");
            return 1;
        }

        int failures = 0;
        for (auto const& s : sinks) {
            bool got = false;
            std::string text;
            // Re-issue across the budget; a single ACMP datagram can be lost.
            constexpr int kAttempts = 3;
            for (int attempt = 0; attempt < kAttempts && !got; ++attempt) {
                ctrl->query_rx_state(s.id, s.uid, elapsed_ns());
                (void)pump(
                    reactor,
                    *ctrl,
                    600,
                    [&] { return got; },
                    [&](auto const& ev) {
                        if (auto const* r = std::get_if<atdecc_tools::RxStateEvent>(&ev)) {
                            if (r->listener_entity_id == s.id && r->listener_unique_id == s.uid) {
                                got = true;
                                if (r->connected) {
                                    text = std::format(
                                        "CONNECTED to talker {}:{} (status={})",
                                        ieee::to_string(r->talker_entity_id).view(),
                                        r->talker_unique_id,
                                        static_cast<int>(r->status));
                                } else {
                                    text = std::format("not connected (status={})", static_cast<int>(r->status));
                                }
                            }
                        }
                    });
            }
            if (got) {
                std::println("  {:<22} REPLY: {}", s.label, text);
            } else {
                std::println("  {:<22} TIMEOUT (no GET_RX_STATE_RESPONSE)", s.label);
                ++failures;
            }
        }
        std::println("get-rx-state: {} replied, {} timed out", sinks.size() - static_cast<size_t>(failures), failures);
        return failures == 0 ? 0 : 1;
    }

    // ----- get-tx-state (ACMP talker-source connection state) -----
    // Companion to get-rx-state: probes the TALKER side, which macOS reads
    // successfully. Run both to isolate whether silence is listener-specific.
    if (config.command == "get-tx-state") {
        auto const t = atdecc_tools::parse_endpoint(config.talker);
        if (!t) {
            std::println(stderr, "Error: get-tx-state needs --talker=NAME:UID");
            return 1;
        }
        auto const tid = resolve_or_report(t->name, snap);
        if (!tid) {
            return 1;
        }
        bool got = false;
        std::string text;
        constexpr int kAttempts = 3;
        for (int attempt = 0; attempt < kAttempts && !got; ++attempt) {
            ctrl->query_tx_state(*tid, t->unique_id, elapsed_ns());
            (void)pump(
                reactor,
                *ctrl,
                600,
                [&] { return got; },
                [&](auto const& ev) {
                    if (auto const* c = std::get_if<atdecc_tools::ConnectionAddedEvent>(&ev)) {
                        if (c->connection.talker_entity_id == *tid && c->connection.talker_unique_id == t->unique_id) {
                            got = true;
                            text = std::format(
                                "connection present (listener {})", ieee::to_string(c->connection.listener_entity_id).view());
                        }
                    }
                });
        }
        // The ACMP_TRACE env var prints every received ACMP frame, so a
        // GET_TX_STATE_RESPONSE shows there even when there is no connection
        // (which would not raise a ConnectionAddedEvent).
        std::println(
            "  {}:{:<20} {}", t->name, t->unique_id, got ? text : "no ConnectionAddedEvent (check ACMP_TRACE for mt=5 reply)");
        return 0;
    }

    // ----- single-op commands -----
    if (config.command == "connect" || config.command == "disconnect") {
        auto const t = atdecc_tools::parse_endpoint(config.talker);
        auto const l = atdecc_tools::parse_endpoint(config.listener);
        if (!t || !l) {
            std::println(stderr, "Error: --talker and --listener (NAME:UID) are required");
            return 1;
        }
        Op op{
            .kind = (config.command == "connect") ? OpKind::Connect : OpKind::Disconnect,
            .talker = *t,
            .listener = *l,
            .entity = {},
            .clock_domain = 0,
            .clock_source = 0};
        return execute_op(reactor, *ctrl, op, config.trace) ? 0 : 1;
    }
    if (config.command == "set-clock-source" || config.command == "get-clock-source") {
        if (config.entity.empty()) {
            std::println(stderr, "Error: --entity is required");
            return 1;
        }
        // These are descriptor indices (uint16 on the wire). The TOML path range-
        // checks them; the single-command CLI path must too, or e.g. --clock-domain
        // 70000 would silently wrap to 4464.
        if (config.clock_domain < 0 || config.clock_domain > 0xFFFF || config.clock_source < 0 || config.clock_source > 0xFFFF) {
            std::println(stderr, "Error: --clock-domain and --clock-source must be 0..65535");
            return 1;
        }
        Op op{
            .kind = (config.command == "set-clock-source") ? OpKind::SetClockSource : OpKind::GetClockSource,
            .talker = {},
            .listener = {},
            .entity = config.entity,
            .clock_domain = static_cast<uint16_t>(config.clock_domain),
            .clock_source = static_cast<uint16_t>(config.clock_source)};
        return execute_op(reactor, *ctrl, op) ? 0 : 1;
    }

    if (config.command == "set-signal-selector" || config.command == "get-signal-selector") {
        if (config.entity.empty()) {
            std::println(stderr, "Error: --entity is required");
            return 1;
        }
        if (config.descriptor < 0 || config.descriptor > 0xFFFF || config.signal_index < 0 || config.signal_index > 0xFFFF ||
            config.signal_output < 0 || config.signal_output > 0xFFFF) {
            std::println(stderr, "Error: --descriptor, --signal-index, and --signal-output must be 0..65535");
            return 1;
        }
        Op op{};
        op.entity = config.entity;
        op.descriptor = static_cast<uint16_t>(config.descriptor);
        if (config.command == "set-signal-selector") {
            auto const sig_type = atdecc_tools::parse_descriptor_type(config.signal_type);
            if (!sig_type) {
                std::println(stderr, "Error: --signal-type must be a descriptor type name (e.g. AUDIO_CLUSTER) or integer");
                return 1;
            }
            op.kind = OpKind::SetSignalSelector;
            op.signal_type = *sig_type;
            op.signal_index = static_cast<uint16_t>(config.signal_index);
            op.signal_output = static_cast<uint16_t>(config.signal_output);
        } else {
            op.kind = OpKind::GetSignalSelector;
        }
        return execute_op(reactor, *ctrl, op) ? 0 : 1;
    }

    if (config.command == "identify") {
        if (config.entity.empty()) {
            std::println(stderr, "Error: --entity is required");
            return 1;
        }
        Op op{};
        op.kind = OpKind::Identify;
        op.entity = config.entity;
        op.identify_on = (config.state != "off");
        return execute_op(reactor, *ctrl, op) ? 0 : 1;
    }

    // ----- batch -----
    if (config.command == "batch") {
        if (config.file.empty()) {
            std::println(stderr, "Error: --file is required for batch");
            return 1;
        }
        auto parsed = toml::parse_file(config.file);
        if (!parsed) {
            std::println(stderr, "Error: cannot parse '{}': {}", config.file, parsed.error().message());
            return 1;
        }
        std::string err;
        auto ops = atdecc_tools::parse_batch_ops(*parsed, err);
        if (!ops) {
            std::println(stderr, "Error: {}", err);
            return 1;
        }
        std::println("batch: {} operation(s)", ops->size());
        int failures = 0;
        for (auto const& op : *ops) {
            if (!execute_op(reactor, *ctrl, op)) {
                ++failures;
            }
        }
        std::println("batch done: {} ok, {} failed", ops->size() - static_cast<size_t>(failures), failures);
        return failures == 0 ? 0 : 1;
    }

    // ----- supervise (ensure each declared connection exists) -----
    // For each Connect op in the TOML, query the listener's ACMP GET_RX_STATE:
    // if it is already connected to the named talker, leave it completely alone
    // (no ACMP traffic at all); otherwise assert a single CONNECT_RX. This is
    // purely idempotent — it NEVER tears down a live connection — so a periodic
    // re-assert can't churn an established MSRP reservation. A listener that does
    // not answer GET_RX_STATE is (re)asserted anyway, harmlessly, since CONNECT_RX
    // is a no-op at an already-connected listener. With --watch-ms>0 the pass
    // repeats forever.
    if (config.command == "supervise") {
        if (config.file.empty()) {
            std::println(stderr, "Error: --file is required for supervise");
            return 1;
        }
        auto parsed = toml::parse_file(config.file);
        if (!parsed) {
            std::println(stderr, "Error: cannot parse '{}': {}", config.file, parsed.error().message());
            return 1;
        }
        std::string err;
        auto ops = atdecc_tools::parse_batch_ops(*parsed, err);
        if (!ops) {
            std::println(stderr, "Error: {}", err);
            return 1;
        }

        auto run_pass = [&]() {
            auto const pass_snap = ctrl->get_display_entities();
            int legs = 0;
            int already = 0;
            int asserted = 0;
            for (auto const& op : *ops) {
                if (op.kind != OpKind::Connect) {
                    continue;  // only stream connections are ensured
                }
                auto const tid = resolve_or_report(op.talker.name, pass_snap);
                auto const lid = resolve_or_report(op.listener.name, pass_snap);
                if (!tid || !lid) {
                    continue;  // unresolved name -> can't act
                }
                ++legs;
                auto const label =
                    std::format("{}:{} -> {}:{}", op.talker.name, op.talker.unique_id, op.listener.name, op.listener.unique_id);

                if (leg_connected(reactor, *ctrl, *tid, op.talker.unique_id, *lid, op.listener.unique_id)) {
                    std::println("supervise: {} CONNECTED -> leave as-is", label);
                    ++already;
                    continue;
                }
                std::println("supervise: {} not connected -> assert CONNECT_RX", label);
                connect_leg(reactor, *ctrl, *tid, op.talker.unique_id, *lid, op.listener.unique_id);
                ++asserted;
            }
            std::println("supervise: pass done -- {} leg(s): {} already connected, {} asserted", legs, already, asserted);
        };

        if (config.watch_ms > 0) {
            while (!stop.stop_requested()) {
                run_pass();
                // Re-discover between passes so newly-(re)appeared entities resolve.
                (void)pump(reactor, *ctrl, config.watch_ms, [&] { return stop.stop_requested(); }, [](auto const&) {});
                ctrl->dispatch(discover, elapsed_ns());
                (void)pump(reactor, *ctrl, config.discover_ms, [] { return false; }, [](auto const&) {});
            }
            return 0;
        }
        run_pass();
        return 0;
    }

    std::println(stderr, "Error: unknown command '{}'", config.command);
    return 1;
}
