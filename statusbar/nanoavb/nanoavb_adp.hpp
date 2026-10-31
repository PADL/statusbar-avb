#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB ADP - Entity Discovery Advertiser
/// Skeleton class for ADP entity advertising per IEEE 1722.1 Clause 6

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp_discovery.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_adp_adv_sm.hpp"
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
    /// Valid time in 2-second units (0-31). Default 10 (20 seconds): Milan
    /// v1.2 Section 5.6.2, "The valid_time field shall be set to 10 in
    /// ENTITY_AVAILABLE messages".
    uint8_t valid_time = 10;

    /// TMR_ADVERTISE, from an ENTITY_AVAILABLE to the next TMR_DELAY: 5 s (Milan v1.2
    /// Table 5.50; IEEE 1722.1-2021 Clause 6.2.4.3, valid_time / 2).
    std::chrono::milliseconds reannounce_interval{5000};

    /// TMR_DELAY's range when started with the link up (Milan v1.2 Section 5.6.3.5.2).
    /// Elsewhere it is valid_time / 5 (IEEE 1722.1-2021 Clause 6.2.4.2.2): 4 s at 10.
    std::chrono::milliseconds startup_delay_max{2000};

    /// randomDeviceDelay()'s range for @p valid_time in 2-second units (Clause 6.2.4.2.2).
    [[nodiscard]] constexpr auto delay_max() const noexcept -> std::chrono::milliseconds
    {
        return std::chrono::milliseconds{valid_time * 400};
    }
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

/// State of the ADP advertiser: Start (no link state yet), Down, Waiting or Delay.
using AdpAdvertiserState = adp_adv_sm::Def::State;

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
    explicit NanoAvbAdpAdvertiser(DescriptorEntity entity, AdpAdvertiserCallbacks callbacks = {}, AdpAdvertiserConfig config = {});

    /// Get the current advertiser state
    [[nodiscard]] auto state() const noexcept -> AdpAdvertiserState { return machine_.current_state(); }

    /// True while the link is up (Waiting or Delay).
    [[nodiscard]] auto advertising() const noexcept -> bool
    {
        return state() == AdpAdvertiserState::Waiting || state() == AdpAdvertiserState::Delay;
    }

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

    // State Control - Milan v1.2 Section 5.6.3.5

    /// LINK_UP: ENTITY_AVAILABLE after TMR_DELAY: 0-2 s while no link state has been reported since
    /// construction or a shutdown() with the link up (Section 5.6.3.5.2), else 0-4 s from DOWN
    /// (Section 5.6.3.5.3; a shutdown() while down is ignored). Ignored while up.
    auto link_up(TimePoint now) -> void;

    /// LINK_DOWN: stop the timers and send nothing (Sections 5.6.3.5.6 and 5.6.3.5.10).
    auto link_down(TimePoint now) -> void;

    /// SHUTDOWN: ENTITY_DEPARTING if the link is up, available_index reset to 0 (Sections
    /// 5.6.3.5.8 and 5.6.3.5.11; IEEE 1722.1-2021 Clause 6.2.2.15). True if one was sent.
    auto shutdown(TimePoint now) -> bool;

    /// Seed randomDeviceDelay() from @p mac and the real-time clock @p realtime_ns
    /// (IEEE 1722.1-2021 Clause 6.2.4.2.2). The constructor seeds from the entity ID.
    auto seed_random(ieee::Eui48 const& mac, uint64_t realtime_ns) noexcept -> void { ctx_.random.seed(mac, realtime_ns); }

    // Entity State Changes

    /// Rebuild the ADPDU from the entity and, while Waiting, advertise it after TMR_DELAY
    /// (IEEE 1722.1-2021 Clause 6.2.7, needsAdvertise).
    auto notify_entity_changed(TimePoint now) -> void;

    /// Update the ADPDU from the current owned entity snapshot.
    /// Call this if `set_entity()` has been used to install an updated
    /// entity descriptor.
    auto update_adpdu_from_model() -> void;

    /// Replace the owned entity descriptor. Does not send a new
    /// advertisement on its own — call `notify_entity_changed()` after
    /// `set_entity()` to advertise the change.
    /// @param entity The new entity descriptor.
    auto set_entity(DescriptorEntity const& entity) -> void { entity_ = entity; }

    /// Read-only access to the owned entity snapshot.
    [[nodiscard]] auto entity() const noexcept -> DescriptorEntity const& { return entity_; }

    /// Set the advertised gPTP grandmaster and domain (IEEE 1722.1-2021 Clauses 6.2.2.16-17);
    /// a new grandmaster ID is GM_CHANGE (Milan v1.2 Section 5.6.3.5.7).
    auto set_gptp_info(ClockIdentity const& grandmaster_id, uint8_t domain_number, TimePoint now) -> void;

    /// Set the AVB_INTERFACE index the ADPDU carries, always with AEM_INTERFACE_INDEX_VALID
    /// (IEEE 1722.1-2021 Clause 6.2.1.10, Milan v1.2 Section 5.6.2); default 0, kept across
    /// update_adpdu_from_model().
    void set_interface_index(uint16_t index)
    {
        interface_index_ = index;
        adpdu_.interface_index = index;
    }

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

    /// True for an ENTITY_DISCOVER for every entity or for this one (Milan v1.2 Section 5.6.3.1).
    [[nodiscard]] auto is_discover_for_us(AdpDu const& adpdu) const noexcept -> bool;

    /// Hand the current ENTITY_AVAILABLE to @p send (a local controller's DISCOVER) now, outside
    /// the state machine. A sent reply is a transmission: available_index advances (Clause 6.2.2.15);
    /// its result is not counted in consecutive_send_failures(), which is the network's.
    template <typename Send>
    auto reply_available(Send&& send) -> bool
    {
        return note_available(std::forward<Send>(send)(available_adpdu()));
    }

    /// Process an ADPDU received from the network: a discover for us is RCV_ADP_DISCOVER.
    auto receive_adpdu(AdpDu const& adpdu, TimePoint now) -> void;

    /// Fire TMR_DELAY and TMR_ADVERTISE once @p now reaches their deadlines.
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
        apply_timer_config();
    }

    /// Get the number of consecutive send failures
    [[nodiscard]] auto consecutive_send_failures() const noexcept -> uint32_t { return consecutive_send_failures_; }

  private:
    /// Copy the configured timer ranges into the machine's variables.
    auto apply_timer_config() noexcept -> void;

    /// Run @p event, then transmit what it asked for (IEEE 1722.1-2021 Clause 6.2.5).
    auto handle(adp_adv_sm::Def::Event event, TimePoint now) -> void;

    /// Send Entity Available message
    /// @return true if send succeeded, false if send failed or no callback
    auto send_entity_available() -> bool;

    /// The ADPDU, as an ENTITY_AVAILABLE.
    auto available_adpdu() noexcept -> AdpDu const&;

    /// Advance available_index after a sent ENTITY_AVAILABLE; returns @p sent. No failure counting.
    auto note_available(bool sent) -> bool;

    /// Count a send's failure (or clear the count on success); returns @p sent.
    auto note_send(bool sent) noexcept -> bool;

    /// Send Entity Departing message
    /// @return true if send succeeded, false if send failed or no callback
    auto send_entity_departing() -> bool;

    DescriptorEntity entity_;  // Owned snapshot (~312 bytes) — see ctor for rationale
    AdpAdvertiserCallbacks callbacks_;
    AdpAdvertiserConfig config_;
    uint16_t identify_control_index_ = 0;
    bool identify_control_index_valid_ = false;
    uint16_t interface_index_ = 0;

    adp_adv_sm::Machine machine_{};
    adp_adv_sm::Context ctx_{};
    AdpDu adpdu_;
    uint32_t available_index_ = 0;
    uint32_t consecutive_send_failures_ = 0;  // Track send failures for diagnostics
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
