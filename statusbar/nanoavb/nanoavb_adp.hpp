#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB ADP - Entity Discovery Advertiser
/// Skeleton class for ADP entity advertising per IEEE 1722.1 Clause 6

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp_discovery.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <ratio>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

using ieee::Eui64;
using statusbar::failure;
using statusbar::Status;
using statusbar::success;
using tsn::ClockIdentity;
using namespace atdecc;
using namespace atdecc::aem;
using TimePoint = sm::TimePoint;

//
// ADP Advertiser Configuration
//
/// Configuration for ADP advertiser behavior
struct AdpAdvertiserConfig
{
    /// Valid time in 2-second units (0-31, default 31 = 62 seconds)
    uint8_t valid_time = 31;

    /// Reannounce interval (should be less than valid_time * 2 seconds)
    std::chrono::milliseconds reannounce_interval{10000};  // 10 seconds default
};

//
// ADP Advertiser Callback Interface
//
/// Callbacks for ADP advertiser operations
/// The network layer implements these to send/receive ADP packets
struct AdpAdvertiserCallbacks
{
    /// Called when the advertiser needs to send an ADP packet
    /// @param adpdu The ADPDU to send (already in wire format)
    /// @return true if send was successful
    statusbar::sg14::inplace_function<bool(AdpDu const& adpdu), 64> send_adpdu;

    /// Called when the entity available index changes
    /// This can be used to notify other components that entity state changed
    statusbar::sg14::inplace_function<void(uint32_t available_index), 64> on_available_index_change;
};

//
// ADP Advertiser State
//
/// State of the ADP advertiser
enum class AdpAdvertiserState : uint8_t
{
    Stopped,      /// Not advertising
    Advertising,  /// Sending periodic Entity Available messages
    Departing,    /// Sent Entity Departing, transitioning to Stopped
};

//
// NanoAVB ADP Advertiser
//
/// NanoAvbAdpAdvertiser - skeleton class for ADP entity advertising.
/// Owns a copy of the entity descriptor (needed for entity_id during
/// advertisement) and handles the advertising schedule. Does NOT
/// directly send/receive packets - uses callback interface.
///
/// Holds the entity descriptor by value (312 bytes) rather than
/// referencing an external EntityModel so that Phase 5 of the AEM
/// refactor can delete the legacy vector-backed EntityModel entirely.
/// If runtime updates to the entity descriptor are needed, call
/// `set_entity()` followed by `notify_entity_changed()`.
class NanoAvbAdpAdvertiser
{
  public:
    /// Construct from a copy of the entity descriptor.
    /// @param entity The entity descriptor — entity_id is the only
    ///               field currently consumed, but a full DescriptorEntity
    ///               is copied in so future fields (capabilities, etc.)
    ///               are readily available.
    /// @param callbacks Callback interface for sending ADPDUs
    /// @param config Optional configuration for advertiser behavior
    explicit NanoAvbAdpAdvertiser(DescriptorEntity entity, AdpAdvertiserCallbacks callbacks = {}, AdpAdvertiserConfig config = {})
        : entity_{entity}
        , callbacks_{std::move(callbacks)}
        , config_{config}
    {
        // Initialize the ADPDU from the owned entity snapshot.
        update_adpdu_from_model();
    }

    /// Get the current advertiser state
    [[nodiscard]] auto state() const noexcept -> AdpAdvertiserState { return state_; }

    /// Get the current available index
    [[nodiscard]] auto available_index() const noexcept -> uint32_t { return available_index_; }

    /// Get the configured valid time (in 2-second units)
    [[nodiscard]] auto valid_time() const noexcept -> uint8_t { return config_.valid_time; }

    /// Get a reference to the current ADPDU
    [[nodiscard]] auto adpdu() const noexcept -> AdpDu const& { return adpdu_; }

    /// Set or update callbacks after construction
    /// This allows wiring up network handlers after both components are created
    /// @param callbacks The new callback interface
    auto set_callbacks(AdpAdvertiserCallbacks callbacks) -> void { callbacks_ = std::move(callbacks); }

    // State Control

    /// Start advertising (begin sending Entity Available messages)
    /// @param now Current time for scheduling
    /// @return Success or error if already advertising
    [[nodiscard]] auto start(TimePoint now) -> Status;

    /// Stop advertising (send Entity Departing and stop)
    /// @param now Current time
    /// @return Success or error if not advertising
    [[nodiscard]] auto stop(TimePoint now) -> Status;

    // Entity State Changes

    /// Notify that entity state has changed
    /// This should be called whenever the entity model changes
    /// If currently advertising, sends an updated Entity Available immediately
    auto notify_entity_changed() -> void;

    /// Update the ADPDU from the current owned entity snapshot.
    /// Call this if `set_entity()` has been used to install an updated
    /// entity descriptor.
    auto update_adpdu_from_model() -> void;

    /// Replace the owned entity descriptor. Does not send a new
    /// advertisement on its own — call `notify_entity_changed()` after
    /// `set_entity()` to push the change onto the wire.
    /// @param entity The new entity descriptor.
    auto set_entity(DescriptorEntity const& entity) -> void { entity_ = entity; }

    /// Read-only access to the owned entity snapshot.
    [[nodiscard]] auto entity() const noexcept -> DescriptorEntity const& { return entity_; }

    /// Set the gPTP grandmaster information
    /// @param grandmaster_id The gPTP grandmaster ID (ClockIdentity)
    /// @param domain_number The gPTP domain number
    void set_gptp_info(ClockIdentity const& grandmaster_id, uint8_t domain_number)
    {
        adpdu_.gptp_grandmaster_id = grandmaster_id;
        adpdu_.gptp_domain_number = domain_number;
    }

    /// Set the interface index
    /// @param index The AVB interface index
    void set_interface_index(uint16_t index) { adpdu_.interface_index = index; }

    /// Set the identify control index and advertise it as valid.
    /// Sets the ADPDU identify_control_index and the AEM_IDENTIFY_CONTROL_INDEX_VALID
    /// entity-capability bit (IEEE 1722.1 6.2.1.10) so controllers know which CONTROL
    /// descriptor is the entity's IDENTIFY control. Remembered across
    /// update_adpdu_from_model() rebuilds — the index is discovered from the
    /// descriptor storage, not part of the ENTITY descriptor the rebuild copies.
    /// @param index The identify control descriptor index
    void set_identify_control_index(uint16_t index)
    {
        identify_control_index_ = index;
        identify_control_index_valid_ = true;
        adpdu_.identify_control_index = index;
        adpdu_.entity_capabilities.set_flag(atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID);
    }

    // Packet Processing

    /// Process a received ADP packet
    /// @param adpdu The received ADPDU
    /// @param now Current time for timeout tracking
    auto receive_adpdu(AdpDu const& adpdu, TimePoint now) -> void;

    /// Periodic tick - call this regularly to process timers
    /// @param now Current time for timer processing
    auto tick(TimePoint now) -> void;

    // Configuration

    /// Get the current configuration
    [[nodiscard]] auto config() const noexcept -> AdpAdvertiserConfig const& { return config_; }

    /// Update the configuration
    /// @param config The new advertiser configuration
    void set_config(AdpAdvertiserConfig const& config) noexcept
    {
        config_ = config;
        adpdu_.set_valid_time(config_.valid_time);
    }

    /// Get the number of consecutive send failures
    [[nodiscard]] auto consecutive_send_failures() const noexcept -> uint32_t { return consecutive_send_failures_; }

  private:
    /// Send Entity Available message
    /// @return true if send succeeded, false if send failed or no callback
    auto send_entity_available() -> bool;

    /// Send Entity Departing message
    /// @return true if send succeeded, false if send failed or no callback
    auto send_entity_departing() -> bool;

    DescriptorEntity entity_;  // Owned snapshot (~312 bytes) — see ctor for rationale
    AdpAdvertiserCallbacks callbacks_;
    AdpAdvertiserConfig config_;
    uint16_t identify_control_index_ = 0;
    bool identify_control_index_valid_ = false;

    AdpAdvertiserState state_ = AdpAdvertiserState::Stopped;
    AdpDu adpdu_;
    uint32_t available_index_ = 0;
    uint32_t consecutive_send_failures_ = 0;  // Track send failures for diagnostics
    TimePoint last_announce_time_;
};

//
// NanoAVB ADP Discovery - Controller-side entity discovery
//

/// Callbacks for ADP discovery operations
struct AdpDiscoveryCallbacks
{
    /// Called to send an ADP packet (ENTITY_DISCOVER)
    statusbar::sg14::inplace_function<bool(AdpDu const&), 64> send_adpdu;

    /// Called when a new entity is discovered
    statusbar::sg14::inplace_function<void(atdecc::DiscoveredEntity const&), 64> on_entity_available;

    /// Called when an entity's state changes (new available_index)
    statusbar::sg14::inplace_function<void(atdecc::DiscoveredEntity const&), 64> on_entity_updated;

    /// Called when an entity departs or times out
    statusbar::sg14::inplace_function<void(Eui64), 64> on_entity_departing;
};

/// NanoAvbAdpDiscovery - discovers and tracks remote ATDECC entities
class NanoAvbAdpDiscovery
{
  public:
    using TimePoint = sm::TimePoint;

    explicit NanoAvbAdpDiscovery(AdpDiscoveryCallbacks callbacks = {});

    /// Set or update callbacks
    void set_callbacks(AdpDiscoveryCallbacks callbacks);

    /// Receive an ADPDU from the network
    void receive_adpdu(AdpDu const& adp, Eui48 const& src_mac, TimePoint now);

    /// Send broadcast ENTITY_DISCOVER
    void discover_all();

    /// Send targeted ENTITY_DISCOVER for a specific entity
    void discover(Eui64 const& target);

    /// Periodic tick for entity expiry checking
    void tick(TimePoint now);

    /// Find entity by ID
    [[nodiscard]] auto find_entity(Eui64 const& id) const -> atdecc::DiscoveredEntity const* { return ctx_.entities.find(id); }

    /// Get MAC address for a known entity
    [[nodiscard]] auto mac_for_entity(Eui64 const& id) const -> std::optional<Eui48> { return ctx_.entities.mac_for_entity(id); }

    /// Number of known entities
    [[nodiscard]] auto entity_count() const -> size_t { return ctx_.entities.count(); }

    /// Iterate over all known entities
    template <typename F>
    void for_each_entity(F&& fn) const
    {
        ctx_.entities.for_each(std::forward<F>(fn));
    }

  private:
    void wire_callbacks();

    AdpDiscoveryCallbacks callbacks_;
    atdecc::DiscoveryContext ctx_;
    atdecc::AdpDiscoveryStateMachine<> sm_;
};

}  // namespace statusbar::nanoavb
