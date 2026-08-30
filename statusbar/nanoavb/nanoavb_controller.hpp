#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB AEM Controller Entity
/// Combines ADP discovery, ACMP controller, and AEM controller into a
/// full ATDECC controller entity per IEEE 1722.1

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp_discovery.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem_controller.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace statusbar::nanoavb {

using ieee::Eui48;
using ieee::Eui64;

/// Callbacks for AEM controller entity events
struct AemControllerEntityCallbacks
{
    // Network I/O
    statusbar::sg14::inplace_function<bool(std::span<uint8_t const> packet), 64> send_atdecc_multicast;
    statusbar::sg14::inplace_function<bool(Eui48 const& dest_mac, std::span<uint8_t const> packet), 64> send_atdecc_unicast;

    // Entity discovery notifications
    statusbar::sg14::inplace_function<void(atdecc::DiscoveredEntity const&), 64> on_entity_available;
    statusbar::sg14::inplace_function<void(atdecc::DiscoveredEntity const&), 64> on_entity_updated;
    statusbar::sg14::inplace_function<void(Eui64), 64> on_entity_departing;

    // AEM response notifications
    /// `sent_payload` is the original request payload (after the AemDu header).
    /// Useful when response payload echoes are unreliable on errors.
    statusbar::sg14::inplace_function<
        void(Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data),
        64>
        on_aem_response;
    statusbar::sg14::inplace_function<void(Eui64 target, uint16_t cmd), 64> on_aem_timeout;

    /// Unsolicited AEM response (U bit set): a change made by another
    /// controller or by the entity itself, delivered to controllers that
    /// sent register_unsolicited(). `data` is the response payload after
    /// the AemDu header (e.g. a SET_CONTROL response body). Without this
    /// callback unsolicited responses are dropped — they never match an
    /// in-flight sequence ID.
    statusbar::sg14::inplace_function<void(Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> data), 64>
        on_unsolicited;

    // ACMP response notifications
    statusbar::sg14::inplace_function<void(atdecc::AcmpCommandResponse const&), 64> on_acmp_response;
    statusbar::sg14::inplace_function<void(atdecc::AcmpCommandResponse const&), 64> on_acmp_timeout;
};

/// NanoAvbAemController - full ATDECC controller entity
/// Integrates ADP discovery, ACMP connection management, and AEM command sending
class NanoAvbAemController
{
  public:
    using TimePoint = sm::TimePoint;

    explicit NanoAvbAemController(Eui64 controller_entity_id, AemControllerEntityCallbacks callbacks = {});

    void set_callbacks(AemControllerEntityCallbacks callbacks);

    /// Start the controller (initializes all sub-state-machines)
    void start();

    // --- ADP Discovery ---

    /// Send broadcast ENTITY_DISCOVER
    void discover_all();

    /// Send targeted ENTITY_DISCOVER for a specific entity
    void discover(Eui64 const& target);

    /// Find a discovered entity
    [[nodiscard]] auto find_entity(Eui64 const& id) const -> atdecc::DiscoveredEntity const*;

    /// Get MAC for entity (for AEM unicast)
    [[nodiscard]] auto mac_for_entity(Eui64 const& id) const -> std::optional<Eui48>;

    /// Number of discovered entities
    [[nodiscard]] auto entity_count() const -> size_t;

    // --- AEM Commands ---

    /// Send an AEM command to a target entity (looked up via ADP discovery for MAC)
    /// Send an arbitrary AEM command. The optional @p completion fires
    /// exactly once with the outcome (final response, timeout, or send
    /// failure) — the caller never matches sequence IDs itself.
    auto send_aem_command(
        Eui64 target, uint16_t command_code, std::span<uint8_t const> payload = {}, atdecc::AemCommandCompletion completion = {})
        -> bool;

    /// Acquire entity
    auto acquire_entity(Eui64 target, bool persistent = false) -> bool;

    /// Release entity
    auto release_entity(Eui64 target) -> bool;

    /// Lock entity
    auto lock_entity(Eui64 target) -> bool;

    /// Unlock entity
    auto unlock_entity(Eui64 target) -> bool;

    /// Read descriptor
    auto read_descriptor(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool;

    /// Register for unsolicited notifications. The optional @p completion
    /// reports whether the entity accepted the registration.
    auto register_unsolicited(Eui64 target, atdecc::AemCommandCompletion completion = {}) -> bool;

    /// Set the identify state on a target entity (flashes/stops the LED).
    /// Sends SET_CONTROL on the CONTROL descriptor whose index is advertised
    /// by the ADPDU's `identify_control_index` field. `on=true` writes 0xFF,
    /// `on=false` writes 0x00.
    auto set_identify(Eui64 target, bool on, atdecc::AemCommandCompletion completion = {}) -> bool;

    /// Send GET_STREAM_INFO command
    auto get_stream_info(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool;

    /// Send GET_AVB_INFO command (AVB_INTERFACE descriptor)
    auto get_avb_info(Eui64 target, uint16_t desc_index = 0) -> bool;

    /// Send GET_CLOCK_SOURCE command (CLOCK_DOMAIN descriptor)
    auto get_clock_source(Eui64 target, uint16_t desc_index = 0, atdecc::AemCommandCompletion completion = {}) -> bool;

    /// Send GET_COUNTERS command
    auto get_counters(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool;

    /// Send SET_STREAM_FORMAT command
    auto set_stream_format(Eui64 target, uint16_t desc_type, uint16_t desc_index, uint64_t stream_format) -> bool;

    /// Send START_STREAMING command
    auto start_streaming(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool;

    /// Send STOP_STREAMING command
    auto stop_streaming(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool;

    /// Send SET_CLOCK_SOURCE command
    auto set_clock_source(
        Eui64 target, uint16_t desc_index, uint16_t clock_source_index, atdecc::AemCommandCompletion completion = {}) -> bool;

    /// Send SET_SIGNAL_SELECTOR command (SIGNAL_SELECTOR descriptor):
    /// select the {signal_type, signal_index, signal_output} source.
    auto set_signal_selector(
        Eui64 target,
        uint16_t desc_index,
        uint16_t signal_type,
        uint16_t signal_index,
        uint16_t signal_output,
        atdecc::AemCommandCompletion completion = {}) -> bool;

    /// Send GET_SIGNAL_SELECTOR command (SIGNAL_SELECTOR descriptor)
    auto get_signal_selector(Eui64 target, uint16_t desc_index = 0, atdecc::AemCommandCompletion completion = {}) -> bool;

    /// Send SET_SAMPLING_RATE command
    auto set_sampling_rate(Eui64 target, uint16_t desc_type, uint16_t desc_index, uint32_t sampling_rate) -> bool;

    // --- ACMP Commands ---

    /// Connect a stream (talker to listener)
    auto connect_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool;

    /// Disconnect a stream
    auto disconnect_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool;

    /// Send CONNECT_TX_COMMAND directly to the talker (self-heal path)
    auto connect_tx_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool;

    /// Send DISCONNECT_TX_COMMAND directly to the talker (self-heal path)
    auto disconnect_tx_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool;

    /// Query listener stream connection state (GET_RX_STATE)
    auto get_rx_state(Eui64 listener, uint16_t listener_uid) -> bool;

    /// Query talker stream connection state (GET_TX_STATE)
    auto get_tx_state(Eui64 talker, uint16_t talker_uid) -> bool;

    // --- Packet Dispatch ---

    /// Process a received ADP packet (nanosecond timestamp)
    void receive_adp(atdecc::AdpDu const& adp, Eui48 const& src_mac, int64_t now_ns);

    /// Process a received ADP packet (TimePoint overload for backward compatibility)
    void receive_adp(atdecc::AdpDu const& adp, Eui48 const& src_mac, TimePoint now)
    {
        receive_adp(adp, src_mac, std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    }

    /// Process a received ACMP response (nanosecond timestamp)
    void receive_acmp(atdecc::AcmpCommandResponse const& resp, int64_t now_ns);

    /// Process a received ACMP response (TimePoint overload for backward compatibility)
    void receive_acmp(atdecc::AcmpCommandResponse const& resp, TimePoint now)
    {
        receive_acmp(resp, std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    }

    /// Process a received AECP/AEM response (nanosecond timestamp)
    void receive_aecp(std::span<uint8_t const> payload, int64_t now_ns);

    /// Process a received AECP/AEM response (TimePoint overload for backward compatibility)
    void receive_aecp(std::span<uint8_t const> payload, TimePoint now)
    {
        receive_aecp(payload, std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
    }

    /// Periodic tick for timeouts and expiry (nanosecond timestamp)
    void tick(int64_t now_ns);

    /// Periodic tick for timeouts and expiry (TimePoint overload for backward compatibility)
    void tick(TimePoint now) { tick(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()); }

    /// Get the controller entity ID
    [[nodiscard]] auto entity_id() const noexcept -> Eui64 { return controller_entity_id_; }

    /// Get ACMP inflight count
    [[nodiscard]] auto acmp_inflight_count() const noexcept -> size_t { return acmp_controller_.inflight_count(); }

    /// Get AEM inflight count
    [[nodiscard]] auto aem_inflight_count() const noexcept -> size_t { return aem_ctx_.inflight_count(); }

  private:
    void wire_all_callbacks();

    Eui64 controller_entity_id_;
    AemControllerEntityCallbacks callbacks_;

    NanoAvbAdpDiscovery discovery_;
    NanoAvbAcmpController acmp_controller_;
    atdecc::AemControllerContext aem_ctx_;
    atdecc::AemControllerStateMachine<> aem_sm_;
};

}  // namespace statusbar::nanoavb
