#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB Network Components
/// Provides network handlers for MVRP, MSRP, ATDECC, and gPTP protocols
/// with reactor integration for non-blocking I/O

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace statusbar::nanoavb {

// Import protocol constants from ieee module
using ieee::protocols::ETHERTYPE_AVTP;
using ieee::protocols::ETHERTYPE_MSRP;
using ieee::protocols::ETHERTYPE_MVRP;
using ieee::protocols::MAAP_MULTICAST_MAC;
using ieee::protocols::MSRP_MULTICAST_MAC;
using ieee::protocols::MVRP_MULTICAST_MAC;

//
// MVRP Network Handler
//

/// MVRP Network Handler - receives/sends MVRP packets via Pollable interface.
/// Handles periodic ticks for MVRP state machine.
///
/// Threading: All methods (on_ready, tick) are called from
/// the reactor's poll thread. Must not block — long operations should
/// be deferred. Must not throw — errors should be logged and dropped.
class MvrpNetHandler : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;

    /// @param interface_name The network interface name
    /// @param handler Reference to the MVRP protocol handler
    MvrpNetHandler(std::string_view interface_name, MvrpHandler& handler)
        : handler_{handler}
    {
        (void)context_.open(interface_name, ETHERTYPE_MVRP, &MVRP_MULTICAST_MAC);
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Send a frame directly
    [[nodiscard]] auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
    {
        return context_.send(&dest_mac, payload);
    }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        ieee::Eui48 src_mac{};
        ieee::Eui48 dest_mac{};
        while (true) {
            auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            auto const len = static_cast<size_t>(*result);
            auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
            handler_.receive_packet({payload_buf_.data(), len}, sm_now);
        }
    }

    void tick(int64_t now_ns) override
    {
        auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
        handler_.tick(sm_now);
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    MvrpHandler& handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::array<uint8_t, 2048> payload_buf_{};
};

//
// MSRP Network Handler
//

/// MSRP Network Handler - receives/sends MSRP packets via Pollable interface.
/// Handles periodic ticks for MSRP state machine.
///
/// Threading: All methods (on_ready, tick) are called from
/// the reactor's poll thread. Must not block — long operations should
/// be deferred. Must not throw — errors should be logged and dropped.
class MsrpNetHandler : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;

    /// @param interface_name The network interface name
    /// @param handler Reference to the MSRP protocol handler
    MsrpNetHandler(std::string_view interface_name, MsrpHandler<>& handler)
        : handler_{handler}
    {
        (void)context_.open(interface_name, ETHERTYPE_MSRP, &MSRP_MULTICAST_MAC);
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Send a frame directly
    [[nodiscard]] auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
    {
        return context_.send(&dest_mac, payload);
    }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        ieee::Eui48 src_mac{};
        ieee::Eui48 dest_mac{};
        while (true) {
            auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            auto const len = static_cast<size_t>(*result);
            auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
            handler_.receive_packet({payload_buf_.data(), len}, sm_now);
        }
    }

    void tick(int64_t now_ns) override
    {
        auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
        handler_.tick(sm_now);
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    MsrpHandler<>& handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::array<uint8_t, 2048> payload_buf_{};
};

//
// MAAP Network Handler
//

/// MAAP Network Handler — hosts a MaapHandler (avtp::MaapHandler) on the reactor.
/// Opens an AVTP-ethertype socket joined to the MAAP multicast group, wires the
/// MaapHandler's send callback to that socket, delivers received MAAP frames to
/// it, and drives its probe/announce timers on tick(). This is the reactor
/// slot-in for MAAP — construct one with a MaapHandler owned by the entity and
/// add it to the reactor (opt-in; nothing adds it by default).
///
/// The MaapHandler must outlive this object: this handler wires the MaapHandler's
/// send callback to capture `this`, and references the MaapHandler for receive/tick.
///
/// Threading: on_ready/tick run on the reactor poll thread. Must not block/throw.
class MaapNetHandler : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;

    /// @param interface_name The network interface name
    /// @param handler Reference to the MAAP protocol handler (must outlive this)
    MaapNetHandler(std::string_view interface_name, avtp::MaapHandler& handler)
        : handler_{handler}
    {
        (void)context_.open(interface_name, ETHERTYPE_AVTP, &MAAP_MULTICAST_MAC);
        // All MAAP PDUs go to the MAAP multicast group; route the handler's frames
        // out through this socket.
        handler_.set_send(
            [this](std::span<uint8_t const> bytes) -> bool { return context_.send(&MAAP_MULTICAST_MAC, bytes).has_value(); });
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        ieee::Eui48 src_mac{};
        ieee::Eui48 dest_mac{};
        while (true) {
            auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            handler_.receive(src_mac, {payload_buf_.data(), static_cast<size_t>(*result)}, now_ns);
        }
    }

    void tick(int64_t now_ns) override { handler_.tick(now_ns); }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    avtp::MaapHandler& handler_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::array<uint8_t, 2048> payload_buf_{};
};

//
// ATDECC Network Handler
//

/// ATDECC Network Handler - receives/sends ADP, ACMP, and AEM packets via Pollable interface.
/// Handles periodic ticks for ADP and ACMP state machines.
///
/// Threading: All methods (on_ready, tick) are called from
/// the reactor's poll thread. Must not block — long operations should
/// be deferred. Must not throw — errors should be logged and dropped.
class AtdeccNetHandler : public net::Pollable
{
  public:
    static constexpr int PROTOCOL_TICK_PERIOD_MS = 10;
    /// Periodic-status interval, wall clock. (Was a tick-count modulus that
    /// assumed a 10 ms tick — reactors tick ~1 kHz, which made this line spam
    /// journald ~10x/second.)
    static constexpr int64_t STATUS_INTERVAL_NS = 1'000'000'000;

    /// @param interface_name The network interface name
    /// @param adp_advertiser Reference to the ADP advertiser
    /// @param acmp_talker Reference to the ACMP talker handler
    /// @param acmp_listener Reference to the ACMP listener handler
    /// @param aem_handler Reference to the AEM command handler
    AtdeccNetHandler(
        std::string_view interface_name,
        NanoAvbAdpAdvertiser& adp_advertiser,
        NanoAvbAcmpTalker& acmp_talker,
        NanoAvbAcmpListener& acmp_listener,
        AemCommandHandler& aem_handler)
        : adp_advertiser_{adp_advertiser}
        , acmp_talker_{acmp_talker}
        , acmp_listener_{acmp_listener}
        , aem_handler_{aem_handler}
    {
        (void)context_.open(interface_name, ETHERTYPE_AVTP, &atdecc::ATDECC_MULTICAST_MAC);
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Send a frame directly
    [[nodiscard]] auto send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
    {
        return context_.send(&dest_mac, payload);
    }

    /// Set optional ADP discovery component for controller-side entity tracking
    void set_adp_discovery(NanoAvbAdpDiscovery* discovery) noexcept { adp_discovery_ = discovery; }

    /// Get the current tick count
    [[nodiscard]] auto tick_count() const noexcept -> int64_t { return tick_count_; }

    /// Set reference to PTP wake counter for status display
    /// @param counter Pointer to the TelemetryCounter for PTP wake telemetry
    void set_ptp_wake_counter(statusbar::itc::TelemetryCounter<int64_t>* counter) noexcept { ptp_wake_count_ = counter; }

    /// Install the reactor-thread logger. The once-per-second status line is
    /// emitted (at Status level) only when a logger is set — the handler is a
    /// library and owns no output destination of its own.
    void set_logger(logging::Logger const log) noexcept { logger_ = log; }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override;

    void tick(int64_t now_ns) override;

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    void dispatch_frame(int64_t now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> payload);
    void log_status();

    net::RawnetContext context_{};
    NanoAvbAdpAdvertiser& adp_advertiser_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    NanoAvbAcmpTalker& acmp_talker_;        // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    NanoAvbAcmpListener& acmp_listener_;    // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    AemCommandHandler& aem_handler_;        // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    NanoAvbAdpDiscovery* adp_discovery_{nullptr};
    statusbar::itc::TelemetryCounter<int64_t>* ptp_wake_count_{nullptr};
    std::optional<logging::Logger> logger_{};
    int64_t tick_count_{0};
    int64_t last_status_ns_{0};
    std::array<uint8_t, 2048> payload_buf_{};
};

//
// gPTP Announce Handler
//

/// Callbacks for gPTP Announce message handling
struct GptpAnnounceCallbacks
{
    /// Called when the grandmaster identity changes
    /// @param now Timestamp when the Announce message was received
    /// @param grandmaster_id The new grandmaster clock identity
    /// @param announce Reference to the full Announce message
    statusbar::sg14::
        inplace_function<void(int64_t now_ns, gptp::ClockIdentity const& grandmaster_id, gptp::AnnounceMessage const& announce), 64>
            grandmaster_id_changed;
};

/// gPTP Announce Handler - receives gPTP Announce messages and notifies on grandmaster changes.
/// Implements Pollable for use with MessageReactor.
class GptpAnnounceHandler : public net::Pollable
{
  public:
    /// @param interface_name The network interface name
    /// @param callbacks Callback interface for grandmaster change notifications
    GptpAnnounceHandler(std::string_view interface_name, GptpAnnounceCallbacks callbacks = {})
        : callbacks_{std::move(callbacks)}
    {
        (void)context_.open(interface_name, gptp::GPTP_ETHERTYPE, &gptp::GPTP_MULTICAST_MAC);
    }

    /// Check if the handler is valid (socket opened successfully)
    [[nodiscard]] auto valid() const noexcept -> bool { return context_.valid(); }

    /// Set or replace the callbacks
    /// @param callbacks The new callback interface
    void set_callbacks(GptpAnnounceCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Get the current grandmaster identity (8-byte clock identity)
    [[nodiscard]] auto grandmaster_identity() const noexcept -> gptp::ClockIdentity { return grandmaster_identity_; }

    /// Check if grandmaster identity has been received
    [[nodiscard]] auto has_grandmaster() const noexcept -> bool { return grandmaster_identity_.is_set(); }

    /// Get the time of the last announce (nanoseconds since epoch)
    [[nodiscard]] auto last_announce_time_ns() const noexcept -> int64_t { return last_announce_time_ns_; }

    // -- Pollable interface --

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override;

    void tick(int64_t /*now_ns*/) override {}

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    net::RawnetContext context_{};
    GptpAnnounceCallbacks callbacks_;
    gptp::ClockIdentity grandmaster_identity_{};
    int64_t last_announce_time_ns_{0};
    std::array<uint8_t, 2048> payload_buf_{};
};

//
// NanoAVB Components Container
//

/// Holds all NanoAVB protocol handlers (without network handlers).
///
/// `aem_handler` and `adp_advertiser` hold references into `entity_model` (and
/// `aem_handler` is itself self-referential), so this type is **non-copyable
/// and non-movable**: construct it in place and never relocate it. The
/// constructor wires each handler against the *member* `entity_model`
/// (declaration order guarantees it is constructed first), keeping the
/// cross-references valid for the object's whole lifetime. Build it directly as
/// a value member (entities) or on the heap via the builder (returns a
/// unique_ptr) — never return it by value or std::move it.
struct NanoAvbComponents
{
    using TimePoint = sm::TimePoint;

    /// Construct all handlers, wiring AEM/ADP/ACMP against the owned
    /// `entity_model` member. Post-construction setup that needs site-specific
    /// values (talker stream config, VLAN registration, MSRP domain) is done by
    /// the caller on the constructed members.
    /// @param model                 Entity model to take ownership of.
    /// @param adp_config            ADP advertiser configuration.
    /// @param talker_max_streams    Max talker streams.
    /// @param talker_max_listeners  Max listeners per talker stream.
    /// @param listener_max_streams  Max listener streams.
    NanoAvbComponents(
        EntityModel model,
        AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams);

    /// Symbol-aware construction: serve descriptors (and resolve their well-known
    /// symbols) through a caller-owned @p handler — typically a DescriptorStorageHandler
    /// backed by an .aem blob — instead of a parsed EntityModel. The handler MUST
    /// outlive this object. The entity descriptor (entity_id + capabilities for ADP +
    /// ACMP) is resolved from the handler at construction; the legacy `entity_model`
    /// member stays empty/unused on this path.
    NanoAvbComponents(
        AemEntityHandler& handler,
        AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams);

    NanoAvbComponents(NanoAvbComponents const&) = delete;
    auto operator=(NanoAvbComponents const&) -> NanoAvbComponents& = delete;
    NanoAvbComponents(NanoAvbComponents&&) = delete;
    auto operator=(NanoAvbComponents&&) -> NanoAvbComponents& = delete;
    ~NanoAvbComponents() = default;

  private:
    /// Delegated-to target for the symbol-aware ctor, with the entity descriptor
    /// pre-resolved so ADP + ACMP read it without resolving from the handler thrice.
    NanoAvbComponents(
        AemEntityHandler& handler,
        DescriptorEntity const& entity,
        AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams);

  public:
    // Declaration order matters: `entity_model` must precede the handlers that
    // reference it so it is fully constructed when they are wired.
    EntityModel entity_model;
    AemCommandHandler aem_handler;
    NanoAvbAdpAdvertiser adp_advertiser;
    NanoAvbAcmpTalker acmp_talker;
    NanoAvbAcmpListener acmp_listener;
    MvrpHandler mvrp_handler;
    MsrpHandler<> msrp_handler;

    void print_info() const;

    /// Get total talker connection count
    [[nodiscard]] auto talker_connection_count() const -> size_t;
};

//
// NanoAvbComponentsBuilder - Fluent builder for NanoAvbComponents
//

/// Builder for NanoAvbComponents with validation
/// Ensures all required components are configured before building
///
/// Example usage:
/// @code
///   auto result = NanoAvbComponentsBuilder{}
///       .with_entity_model(std::move(model))
///       .with_adp_config(adp_config)
///       .with_acmp_talker_streams(2, 4)
///       .with_acmp_listener_streams(2)
///       .with_default_vlan(2)
///       .with_sr_class(DomainInfo{6, 3, 2})
///       .build();
///   if (!result) { /* handle error */ }
///   auto components = std::move(*result);
/// @endcode
class NanoAvbComponentsBuilder
{
  public:
    /// Set the entity model (required)
    /// @param model The entity model to use
    auto with_entity_model(EntityModel model) -> NanoAvbComponentsBuilder&
    {
        entity_model_ = std::move(model);
        has_entity_model_ = true;
        return *this;
    }

    /// Set ADP advertiser configuration
    /// @param config The ADP advertiser configuration
    auto with_adp_config(AdpAdvertiserConfig const config) -> NanoAvbComponentsBuilder&
    {
        adp_config_ = config;
        return *this;
    }

    /// Set the number of talker streams and max listeners per stream
    /// @param max_streams Maximum number of talker streams
    /// @param max_listeners_per_stream Maximum number of listeners per stream
    auto with_acmp_talker_streams(size_t const max_streams, size_t const max_listeners_per_stream = 16) -> NanoAvbComponentsBuilder&
    {
        talker_max_streams_ = max_streams;
        talker_max_listeners_ = max_listeners_per_stream;
        return *this;
    }

    /// Set the number of listener streams
    /// @param max_streams Maximum number of listener streams
    auto with_acmp_listener_streams(size_t const max_streams) -> NanoAvbComponentsBuilder&
    {
        listener_max_streams_ = max_streams;
        return *this;
    }

    /// Set the default VLAN ID for MVRP registration
    /// @param vlan_id The VLAN ID to use (1-4094)
    auto with_default_vlan(uint16_t const vlan_id) -> NanoAvbComponentsBuilder&
    {
        default_vlan_id_ = vlan_id;
        return *this;
    }

    /// Set the SR class domain info for MSRP
    /// @param domain The SR class domain information
    auto with_sr_class(DomainInfo const domain) -> NanoAvbComponentsBuilder&
    {
        sr_domain_ = domain;
        return *this;
    }

    /// Build the components, validating all required fields are set.
    /// Returns a unique_ptr because NanoAvbComponents is non-movable (it holds
    /// internal cross-references) and so cannot be returned by value.
    /// @return StatusValue containing a unique_ptr<NanoAvbComponents> or error
    [[nodiscard]] auto build() -> StatusValue<std::unique_ptr<NanoAvbComponents>>;

  private:
    EntityModel entity_model_{};
    bool has_entity_model_{false};

    AdpAdvertiserConfig adp_config_{};
    size_t talker_max_streams_{16};
    size_t talker_max_listeners_{16};
    size_t listener_max_streams_{16};
    uint16_t default_vlan_id_{srp::DEFAULT_SR_CLASS_A_VID};
    DomainInfo sr_domain_{
        .sr_class_id = 6,        // SR Class A
        .sr_class_priority = 3,  // Priority 3
        .sr_class_vid = srp::DEFAULT_SR_CLASS_A_VID};
};

//
// NanoAvbNetHandlers - Container for all network handlers
//

/// Holds raw pointers to NanoAVB network handlers registered with a MessageReactor.
/// Handlers are created and transferred to the reactor via add_to_reactor().
/// Raw pointers are captured before ownership transfer for continued access.
class NanoAvbNetHandlers
{
  public:
    /// @param interface_name The network interface name
    /// @param components Reference to the NanoAVB protocol components
    NanoAvbNetHandlers(std::string_view interface_name, NanoAvbComponents& components)
        : interface_name_{interface_name}
        , components_{&components}
    {}

    /// Create handlers, transfer ownership to reactor, and store raw pointers.
    /// @param reactor The message reactor to register handlers with
    void add_to_reactor(net::MessageReactor& reactor);

    /// Get the ATDECC handler (requires add_to_reactor called first)
    [[nodiscard]] auto atdecc_handler() -> AtdeccNetHandler& { return *atdecc_ptr_; }
    [[nodiscard]] auto atdecc_handler() const -> AtdeccNetHandler const& { return *atdecc_ptr_; }

    /// Get the gPTP announce handler
    [[nodiscard]] auto gptp_handler() -> GptpAnnounceHandler& { return *gptp_ptr_; }
    [[nodiscard]] auto gptp_handler() const -> GptpAnnounceHandler const& { return *gptp_ptr_; }

    /// Get the MVRP handler
    [[nodiscard]] auto mvrp_handler() -> MvrpNetHandler& { return *mvrp_ptr_; }

    /// Get the MSRP handler
    [[nodiscard]] auto msrp_handler() -> MsrpNetHandler& { return *msrp_ptr_; }

    /// Print status of all handlers
    void print_status() const;

    /// Print warnings for any handlers that failed to open
    /// @param interface_name The network interface name for warning messages
    void print_warnings(std::string_view interface_name) const;

  private:
    std::string interface_name_;
    NanoAvbComponents* components_;
    // Passive discovery database, owned here and fed/ticked by the AtdeccNetHandler.
    // Its ageout tears down listener sinks whose talker vanished without an explicit
    // ENTITY_DEPARTING (e.g. powered off). Must outlive the reactor's handlers.
    NanoAvbAdpDiscovery discovery_{};
    MvrpNetHandler* mvrp_ptr_{nullptr};
    MsrpNetHandler* msrp_ptr_{nullptr};
    AtdeccNetHandler* atdecc_ptr_{nullptr};
    GptpAnnounceHandler* gptp_ptr_{nullptr};
    bool mvrp_valid_{false};
    bool msrp_valid_{false};
    bool atdecc_valid_{false};
    bool gptp_valid_{false};
};

/// Helper to create a byte span from a packed struct.
/// Thin wrapper around ::statusbar::make_const_span that returns a
/// dynamic-extent span (rather than the fixed-extent span returned
/// by the underlying helper) for callers that want the dynamic form.
/// @param obj The object to view as bytes
template <typename T>
[[nodiscard]] auto as_bytes(T const& obj) noexcept -> std::span<uint8_t const>
{
    return ::statusbar::make_const_span(obj);
}

/// Wire up all callbacks between NanoAvbComponents and NanoAvbNetHandlers
/// @param components The NanoAVB protocol components to wire up
/// @param handlers The network handlers to connect to the components
void setup_nanoavb_callbacks(NanoAvbComponents& components, NanoAvbNetHandlers& handlers);

}  // namespace statusbar::nanoavb
