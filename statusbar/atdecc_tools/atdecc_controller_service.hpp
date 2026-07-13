#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ControllerService — the backend contract for an ATDECC controller.
///
/// This is the seam between the portable controller business logic
/// (ControllerSimple: entity metadata, descriptor crawl, connection
/// tracking, events for UIs and scripts) and a platform's ADP/ACMP/AECP
/// implementation. The boundary carries DECODED PDUs and per-command
/// completions, never wire frames, because not every backend can expose
/// raw 0x22F0 bytes:
///
///  * RawnetControllerService (make_rawnet_controller_service) — our own
///    ADP/ACMP/AECP state machines over a raw L2 socket. Linux and any
///    platform with AF_PACKET-style access. This backend also sees
///    same-host entities (the raw socket taps locally-transmitted
///    frames).
///  * A macOS backend can implement this interface over the system AVB
///    framework, which owns the 1722.1 protocol on that platform — the
///    only way to reach an entity hosted by the same machine, whose
///    traffic never appears on the wire.
///
/// Contract notes:
///  - All sink callbacks and command completions are delivered on the
///    single thread that drives the service (the reactor thread). A
///    backend whose native delivery is elsewhere marshals first.
///  - The backend owns protocol timing: inflight tracking, retry, and
///    timeouts. Command outcomes surface through per-command
///    AemCommandCompletion closures (see atdecc_aecp_aem_controller.hpp),
///    which fire exactly once — response, timeout, or send failure — with
///    one exception: completions still in flight when the service is
///    destroyed are dropped. The send methods' bool means "request
///    accepted for outcome delivery", not "succeeded": failures arrive
///    through the completion/sink, so a send that immediately reports
///    SendFailed may still return true.
///  - on_acmp_observed is backend-dependent: the raw-socket backend
///    reports every ACMP PDU seen on the bus (passive connection
///    tracking, handshake tracing); a framework backend may not be able
///    to observe third-party traffic and may never call it.

#include "statusbar/atdecc/atdecc_acmp_pdu.hpp"
#include "statusbar/atdecc/atdecc_adp_discovery.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem_controller.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace statusbar::atdecc_tools {

/// Asynchronous events a ControllerService backend delivers to its consumer.
/// All callbacks are optional (unset = ignored) and are invoked on the
/// service's driving thread.
struct ControllerServiceSink
{
    template <typename Fn>
    using Cb = statusbar::sg14::inplace_function<Fn, 64>;

    // Entity directory (ADP on the raw backend; synthesized from the
    // platform's discovery service elsewhere).
    Cb<void(atdecc::DiscoveredEntity const&)> on_entity_added;
    Cb<void(atdecc::DiscoveredEntity const&)> on_entity_updated;
    Cb<void(ieee::Eui64)> on_entity_departed;

    // AEM broadcast surface: every final response, regardless of which
    // caller sent the command. The descriptor crawl consumes this; direct
    // request/response callers should prefer per-command completions.
    // `sent_payload` is the original request payload (response echoes are
    // unreliable on errors).
    Cb<void(
        ieee::Eui64 target,
        uint16_t command_type,
        uint8_t status,
        std::span<uint8_t const> sent_payload,
        std::span<uint8_t const> response)>
        on_aem_response;
    Cb<void(ieee::Eui64 target, uint16_t command_type)> on_aem_timeout;

    // ACMP responses to OUR commands (and their timeouts).
    Cb<void(atdecc::AcmpCommandResponse const&)> on_acmp_response;
    Cb<void(atdecc::AcmpCommandResponse const&)> on_acmp_timeout;

    // Every ACMP PDU observed on the bus, commands included — the passive
    // feed for connection tracking and handshake tracing. Backend-dependent
    // (see file header).
    Cb<void(atdecc::AcmpDu const&)> on_acmp_observed;
};

/// Abstract ATDECC controller backend: entity directory + AEM commands +
/// ACMP operations. Consumers depend only on this interface; backends own
/// the protocol state machines (or wrap a platform framework that does).
class ControllerService
{
  public:
    ControllerService() = default;
    virtual ~ControllerService() = default;
    ControllerService(ControllerService const&) = delete;
    auto operator=(ControllerService const&) -> ControllerService& = delete;
    ControllerService(ControllerService&&) = delete;
    auto operator=(ControllerService&&) -> ControllerService& = delete;

    // ---- Lifecycle --------------------------------------------------------

    /// Install the event sink. Call before start().
    virtual void set_sink(ControllerServiceSink sink) = 0;

    /// Start the backend (state machines / framework session).
    virtual void start() = 0;

    /// Drive backend timers (retries, timeouts). Call from the reactor loop.
    virtual void tick(int64_t now_ns) = 0;

    /// The OS event source driving this backend, when it has one (the raw
    /// socket backend returns itself). Backends whose framework delivers on
    /// its own thread return nullptr and marshal into the reactor instead.
    [[nodiscard]] virtual auto pollable() noexcept -> net::Pollable* = 0;

    // ---- Discovery / entity directory -------------------------------------

    /// Send a broadcast ENTITY_DISCOVER (or the backend's equivalent).
    virtual void discover_all() = 0;

    /// Look up a discovered entity by id (nullptr when unknown).
    [[nodiscard]] virtual auto find_entity(ieee::Eui64 const& id) const -> atdecc::DiscoveredEntity const* = 0;

    // ---- AEM commands ------------------------------------------------------
    //
    // Every command variant accepts an optional per-command completion that
    // fires exactly once with the outcome. Addressing is by entity id; MAC
    // resolution (where it exists at all) is a backend detail.

    virtual auto send_aem_command(
        ieee::Eui64 const& target,
        uint16_t command_code,
        std::span<uint8_t const> payload = {},
        atdecc::AemCommandCompletion completion = {}) -> bool = 0;

    /// Commands currently tracked awaiting a response.
    [[nodiscard]] virtual auto aem_inflight_count() const -> size_t = 0;

    virtual auto read_descriptor(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool = 0;
    virtual auto set_identify(ieee::Eui64 const& target, bool on, atdecc::AemCommandCompletion completion = {}) -> bool = 0;
    virtual auto get_counters(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool = 0;
    virtual auto set_stream_format(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index, uint64_t stream_format)
        -> bool = 0;
    virtual auto start_streaming(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool = 0;
    virtual auto stop_streaming(ieee::Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool = 0;
    virtual auto set_clock_source(
        ieee::Eui64 const& target, uint16_t desc_index, uint16_t clock_source_index, atdecc::AemCommandCompletion completion = {})
        -> bool = 0;
    virtual auto get_clock_source(ieee::Eui64 const& target, uint16_t desc_index, atdecc::AemCommandCompletion completion = {})
        -> bool = 0;
    virtual auto set_signal_selector(
        ieee::Eui64 const& target,
        uint16_t desc_index,
        uint16_t signal_type,
        uint16_t signal_index,
        uint16_t signal_output,
        atdecc::AemCommandCompletion completion = {}) -> bool = 0;
    virtual auto get_signal_selector(ieee::Eui64 const& target, uint16_t desc_index, atdecc::AemCommandCompletion completion = {})
        -> bool = 0;

    // ---- ACMP operations ---------------------------------------------------

    virtual auto connect_stream(ieee::Eui64 const& talker, uint16_t talker_uid, ieee::Eui64 const& listener, uint16_t listener_uid)
        -> bool = 0;
    virtual auto disconnect_stream(
        ieee::Eui64 const& talker, uint16_t talker_uid, ieee::Eui64 const& listener, uint16_t listener_uid) -> bool = 0;
    virtual auto connect_tx_stream(
        ieee::Eui64 const& talker, uint16_t talker_uid, ieee::Eui64 const& listener, uint16_t listener_uid) -> bool = 0;
    virtual auto disconnect_tx_stream(
        ieee::Eui64 const& talker, uint16_t talker_uid, ieee::Eui64 const& listener, uint16_t listener_uid) -> bool = 0;
    virtual auto get_rx_state(ieee::Eui64 const& listener, uint16_t listener_uid) -> bool = 0;
    virtual auto get_tx_state(ieee::Eui64 const& talker, uint16_t talker_uid) -> bool = 0;
};

/// The raw-L2-socket backend: our own ADP/ACMP/AECP state machines over
/// EtherType 0x22F0 (NanoAvbAemController + RawnetContext).
[[nodiscard]] auto make_rawnet_controller_service(net::RawnetContext context, ieee::Eui64 controller_id)
    -> std::unique_ptr<ControllerService>;

#if defined(__APPLE__)
/// The macOS backend over the system AVB framework (AudioVideoBridging):
/// the framework owns discovery and AECP/ACMP timing, and this is the only
/// path that reaches an entity hosted by the Mac itself. Returns nullptr
/// when the interface has no AVB framework support. Controller-only; the
/// passive on_acmp_observed feed carries only our own command responses.
[[nodiscard]] auto make_macos_avb_controller_service(std::string const& interface_name, ieee::Eui64 controller_id)
    -> std::unique_ptr<ControllerService>;
#endif

/// The platform's default controller backend name: the system AVB framework
/// on macOS (the only path to the Mac's own virtual entity), our raw-socket
/// stack everywhere else.
[[nodiscard]] constexpr auto default_controller_backend() noexcept -> char const*
{
#if defined(__APPLE__)
    return "avb";
#else
    return "raw";
#endif
}

/// Create a ControllerService by backend name: "raw" opens a raw L2 socket
/// on @p interface_name (root / cap_net_raw); "avb" uses the macOS AVB
/// framework. Returns nullptr with the reason in @p error on failure.
[[nodiscard]] auto make_controller_service(
    std::string const& backend, std::string const& interface_name, ieee::Eui64 controller_id, std::string& error)
    -> std::unique_ptr<ControllerService>;

}  // namespace statusbar::atdecc_tools
