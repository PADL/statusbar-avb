#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB Entity - AEM command/response handler
/// Handles incoming AECP AEM commands using the EntityModel

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/container/container_slot_table.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model_adapter.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

using ieee::doublet_t;
using ieee::Eui48;
using ieee::Eui64;
using ieee::quadlet_t;
using namespace atdecc;
using namespace atdecc::aem;
using statusbar::container::SlotTable;

//
// AEM Command Handler Response
//
/// Maximum size for AEM response data (8 byte header + largest descriptor).
/// Used by process_packet() to allocate one stack-local output buffer that
/// is passed into the per-command handlers.
constexpr size_t MAX_AEM_RESPONSE_SIZE = 1024;

/// Small value returned by each AEM command handler.
///
/// Handlers write their response bytes into a caller-supplied
/// `std::span<uint8_t>` and return just the status code plus the number
/// of bytes they wrote. Previously each handler returned a 1040-byte
/// struct containing an inline std::array<uint8_t, MAX_AEM_RESPONSE_SIZE>,
/// which the stub handlers (NOT_IMPLEMENTED) paid for even though they
/// produced zero bytes. This shape limits the per-dispatch stack cost
/// to one MAX_AEM_RESPONSE_SIZE buffer in process_packet(), regardless
/// of how many handlers are traversed.
struct AemCommandResponse
{
    uint8_t status{AEM_STATUS_SUCCESS};
    size_t size{0};  ///< Bytes written to the caller's output buffer
};

//
// AEM Command Handler Callbacks
//
using SendAemResponseFn = statusbar::sg14::inplace_function<bool(Eui48 const& dest_mac, std::span<uint8_t const> response), 64>;

/// Fill the IEEE 1722.1 counters block for a descriptor (Clause 7.4.42).
/// Counters are dynamic runtime state (stream reception health, etc.) so they
/// come from the application, not the static AEM descriptor model. On a descriptor
/// that has counters, set @p counters_valid (bit i set => counter i present) and
/// @p counters[i], and return true; return false for a descriptor with no
/// counters (the handler then replies NO_SUCH_DESCRIPTOR).
using GetCountersFn = statusbar::sg14::inplace_function<
    bool(uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& counters_valid, std::array<uint32_t, 32>& counters),
    64>;

/// Fill the IEEE 1722.1 GET_STREAM_INFO response (Clause 7.4.16) for a stream
/// descriptor. The handler pre-fills @p out.descriptor_type / descriptor_index;
/// the application fills the dynamic stream parameters (stream_id, stream_format,
/// stream_dest_mac, stream_vlan_id, msrp_accumulated_latency) and sets the
/// matching `*_VALID` / CONNECTED flag bits, then returns true. Return false for
/// a descriptor that is not one of this entity's streams (the handler then replies
/// NO_SUCH_DESCRIPTOR). Optional; if unset, GET_STREAM_INFO replies NOT_IMPLEMENTED.
///
/// Milan listeners (e.g. the DSP processor) query GET_STREAM_INFO on a talker's
/// STREAM_OUTPUT after connecting to verify the stream's format/identity, and
/// tear the connection down if the talker answers NOT_IMPLEMENTED.
using GetStreamInfoFn =
    statusbar::sg14::inplace_function<bool(uint16_t descriptor_type, uint16_t descriptor_index, AemStreamInfoPayload& out), 64>;

/// Notified when a SET_CONTROL targeting the entity's IDENTIFY control is
/// applied (see set_identify_control_index). @p active is true when the new
/// value is non-zero (a controller is identifying this entity), false when
/// cleared.
using IdentifyChangedFn = statusbar::sg14::inplace_function<void(bool active), 64>;

/// Callbacks for AECP AEM command handling
struct AemCommandHandlerCallbacks
{
    /// Send AECP AEM response to the requesting controller (unicast)
    /// @param dest_mac Destination MAC address (controller that sent the command)
    /// @param response Complete response packet (AemDu header + command-specific data)
    /// @return true if response was sent successfully
    SendAemResponseFn send_response;

    /// Provide counters for GET_COUNTERS. Optional; if unset, GET_COUNTERS replies
    /// NOT_IMPLEMENTED.
    GetCountersFn get_counters;

    /// Provide stream parameters for GET_STREAM_INFO. Optional; if unset,
    /// GET_STREAM_INFO replies NOT_IMPLEMENTED.
    GetStreamInfoFn get_stream_info;

    /// Observe IDENTIFY-control changes (e.g. blink an LED, or log when there
    /// is none). Optional.
    IdentifyChangedFn identify_changed;
};

/// Parameters for building acquire/lock response packets
/// Uses designated initializers for self-documenting call sites
struct AcquireLockResponseParams
{
    uint8_t status;                         ///< AEM status code
    uint32_t flags;                         ///< Response flags (network byte order conversion handled internally)
    Eui64 const& entity_id;                 ///< Owner (acquire) or locker (lock) entity ID
    std::span<uint8_t const> command_data;  ///< Original command data to echo descriptor info
};

//
// AEM Command Handler
//
/// AemCommandHandler — processes incoming AECP AEM commands using the
/// EntityModel. Does not do packet I/O itself; the caller injects packets
/// via `process_packet()` and supplies a `send_response` callback.
///
/// Thread model: all methods are single-threaded. The caller must serialize
/// access to a single AemCommandHandler instance — `process_packet()`,
/// `tick()`, `controller_available_response_received()`,
/// `controller_available_timed_out()`, and the state-query accessors
/// (`is_acquired()`, `is_locked()`, etc.) must not be invoked concurrently
/// from multiple threads. The typical deployment has one thread driving
/// a reactor that calls `process_packet` on inbound frames and `tick` on
/// a timer, both on the same thread.
class AemCommandHandler
{
  public:
    /// Construct from a legacy vector-backed `EntityModel`.
    ///
    /// Internally wraps the `EntityModel` in an `EntityModelAdapter`
    /// (stored in `adapter_storage_`) that implements `AemEntityHandler`,
    /// so READ_DESCRIPTOR responses still flow through the new
    /// handler-based dispatch path. This constructor exists for backward
    /// compatibility with the many call sites that still build an
    /// EntityModel — tests, nanoavb_components, nanoavb_example — and
    /// will be deleted in Phase 5 once everything migrates to
    /// `AemEntityHandler` directly.
    /// @param model Reference to the entity model for descriptor lookups.
    explicit AemCommandHandler(EntityModel& model)
        : adapter_storage_{std::in_place, model}
        , handler_{&*adapter_storage_}
    {}

    /// Construct from a user-supplied `AemEntityHandler`. This is the
    /// new-world constructor: the caller owns the handler (typically a
    /// `DescriptorStorageHandler` subclass that serves a compiled-in
    /// .aem blob plus runtime-patched fields) and AemCommandHandler
    /// stores just a pointer. No legacy EntityModel involved.
    /// @param handler Reference to the application's AemEntityHandler
    ///                implementation. Must outlive this object.
    explicit AemCommandHandler(AemEntityHandler& handler)
        : handler_{&handler}
    {}

    // Self-referential: `handler_` may point into `adapter_storage_`, so a
    // default move/copy would leave `handler_` dangling at the source object
    // (and the adapter still referencing the source's EntityModel). The type
    // is constructed in place — as a member of the non-movable
    // NanoAvbComponents — and never relocated.
    AemCommandHandler(AemCommandHandler const&) = delete;
    auto operator=(AemCommandHandler const&) -> AemCommandHandler& = delete;
    AemCommandHandler(AemCommandHandler&&) = delete;
    auto operator=(AemCommandHandler&&) -> AemCommandHandler& = delete;

    /// Set the callbacks for sending responses
    /// @param callbacks The callback interface for sending AEM responses
    void set_callbacks(AemCommandHandlerCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Set only the GET_COUNTERS provider, without disturbing the others
    /// (set_callbacks replaces the whole struct, which would clear send_response
    /// wired by the net layer). The application owns the dynamic counters.
    void set_send_response(SendAemResponseFn fn) { callbacks_.send_response = std::move(fn); }
    void set_get_counters(GetCountersFn fn) { callbacks_.get_counters = std::move(fn); }

    /// Emit descriptors at their IEEE 1722.1-2013/2016 wire sizes in
    /// READ_DESCRIPTOR responses (truncate the 2021-only tail fields). Set
    /// for controllers that reject 2021-length descriptors. Default off
    /// (2021 lengths).
    void set_legacy_2016(bool enable) noexcept { legacy_2016_ = enable; }

    /// Set only the GET_STREAM_INFO provider, without disturbing the others.
    /// The application owns the dynamic per-stream parameters.
    void set_get_stream_info(GetStreamInfoFn fn) { callbacks_.get_stream_info = std::move(fn); }
    void set_identify_changed(IdentifyChangedFn fn) { callbacks_.identify_changed = std::move(fn); }

    /// Called when SET_CONFIGURATION asks to switch to a different (valid)
    /// configuration — this is where the application re-shapes its data
    /// plane. Return AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_*
    /// to reject. Without a registered callback a configuration SWITCH is
    /// refused (NOT_SUPPORTED) — the entity must not claim a configuration
    /// its streams are not actually built for; a SET to the current
    /// configuration is always an idempotent SUCCESS.
    void set_on_configuration_changed(statusbar::sg14::inplace_function<uint8_t(uint16_t /*configuration*/), 64> fn)
    {
        on_configuration_changed_ = std::move(fn);
    }

    /// Callback to send CONTROLLER_AVAILABLE command to the current owner.
    /// Set this to enable the CONTROLLER_AVAILABLE handshake on acquire contention.
    /// Signature: send_controller_available(owner_entity_id) -> bool.
    /// See SendAemResponseFn for the inline-storage capacity rationale;
    /// this callback's captures are similarly small in practice.
    statusbar::sg14::inplace_function<bool(Eui64 const& owner_entity_id), 64> send_controller_available;

    /// Process an incoming AECP AEM packet and send response
    /// @param src_mac Source MAC address (for unicast response)
    /// @param payload The AECP packet payload (starting with AemDu header)
    /// @param our_entity_id Our entity ID (for target validation)
    /// @return true if packet was processed and response sent, false if ignored
    [[nodiscard]] auto process_packet(Eui48 const& src_mac, std::span<uint8_t const> payload, Eui64 const& our_entity_id) -> bool;

    /// Handle an incoming AEM command. Writes response bytes into
    /// `out_buffer` and returns status + bytes-written.
    /// @param header The AEM header
    /// @param command_data Additional command-specific data after the header
    /// @param out_buffer Caller-owned buffer for the response body (at least MAX_AEM_RESPONSE_SIZE bytes)
    [[nodiscard]] auto handle_command(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Check if the entity is currently acquired
    [[nodiscard]] auto is_acquired() const noexcept -> bool { return acquired_; }

    /// Check if the entity is currently locked
    [[nodiscard]] auto is_locked() const noexcept -> bool { return locked_; }

    /// Get the controller that acquired this entity
    [[nodiscard]] auto acquiring_controller() const noexcept -> Eui64 { return acquiring_controller_; }

    /// Get the controller that locked this entity
    [[nodiscard]] auto locking_controller() const noexcept -> Eui64 { return locking_controller_; }

    /// Release acquisition (e.g., on timeout)
    void release_acquisition() noexcept
    {
        acquired_ = false;
        acquiring_controller_ = {};
    }

    /// Release lock (e.g., on timeout)
    void release_lock() noexcept
    {
        locked_ = false;
        locking_controller_ = {};
    }

    /// Set the current event time (used by lock timeout tracking)
    void set_event_time(sm::TimePoint now) noexcept { event_time_ = now; }

    /// Notify that the current owner responded to CONTROLLER_AVAILABLE (is alive)
    /// Denies the pending acquire request with ENTITY_ACQUIRED.
    void controller_available_response_received();

    /// Notify that CONTROLLER_AVAILABLE timed out (owner is gone)
    /// Grants acquisition to the pending requester.
    void controller_available_timed_out();

    /// Check if there is a pending acquire waiting for CONTROLLER_AVAILABLE
    [[nodiscard]] auto has_pending_acquire() const noexcept -> bool { return pending_acquire_.active; }

    /// Maximum number of unsolicited notification registrations
    static constexpr size_t MAX_UNSOLICITED_REGISTRATIONS = 8;

    /// Get the number of active unsolicited registrations
    [[nodiscard]] auto unsolicited_registration_count() const noexcept -> size_t;

    /// Drop any unsolicited-notification registration held by @p controller_entity_id.
    /// Call this when that controller departs (ENTITY_DEPARTING or discovery age-out):
    /// otherwise its registration lingers forever, holding a slot and making us frame
    /// and send notifications to an entity that is gone. Returns the number removed.
    auto remove_unsolicited_registrations_for(Eui64 controller_entity_id) noexcept -> size_t;

    /// Set our own entity_id, used as the target_entity_id of unsolicited
    /// notifications. process_packet() also keeps this current; call this so
    /// notifications emitted before any command is received carry the right id.
    void set_entity_id(Eui64 id) noexcept { our_entity_id_ = id; }

    /// Tell the handler which CONTROL descriptor index is the entity's IDENTIFY
    /// control (IEEE 1722.1 7.3.5.2). When a SET_CONTROL changes that control, the
    /// unsolicited notification is ALSO sent to ATDECC_IDENTIFY_MULTICAST_MAC so any
    /// controller — even one not registered for unsolicited notifications — sees the
    /// identify. Mirror this index into the ADPDU (identify_control_index +
    /// AEM_IDENTIFY_CONTROL_INDEX_VALID). Unset => no identify multicast.
    void set_identify_control_index(uint16_t index) noexcept
    {
        identify_control_index_ = index;
        identify_control_index_valid_ = true;
    }

    /// The CONTROL descriptor index of the entity's IDENTIFY control, when one
    /// has been wired (AvbEntityHost does this automatically for blob-backed
    /// entities whose model carries an IDENTIFY-typed control).
    [[nodiscard]] auto identify_control_index() const noexcept -> std::optional<uint16_t>
    {
        if (!identify_control_index_valid_) {
            return std::nullopt;
        }
        return identify_control_index_;
    }

    /// Drive the entity's OWN identify control (e.g. from a GPIO button, a
    /// front-panel event, or software). Applies the value exactly as a
    /// controller's SET_CONTROL would, then fans out the unsolicited
    /// notifications — every registered controller plus the IDENTIFY multicast —
    /// and fires the identify_changed callback. Call from the entity's event
    /// loop (this class is single-threaded). Returns the AEM status;
    /// AEM_STATUS_NOT_IMPLEMENTED when no identify control has been wired.
    [[nodiscard]] auto set_local_identify(bool on) -> uint8_t;

    /// Apply a descriptor-value change originated by the entity ITSELF (the
    /// developer's code), as if a controller had sent the SET command, then
    /// notify every registered controller via an unsolicited response. This is
    /// how an entity with no physical controller (e.g. a software IDENTIFY
    /// button) drives its own controls. @p command_type is the AEM_COMMAND_*
    /// SET code (SET_CONTROL, SET_NAME, ...); @p command_body is the SET
    /// command body (descriptor_type + descriptor_index + value). Returns the
    /// AEM status from the handler's symbol-keyed set hook; notifications are
    /// emitted only on AEM_STATUS_SUCCESS.
    [[nodiscard]] auto apply_local_descriptor_value(uint16_t command_type, std::span<uint8_t const> command_body) -> uint8_t;

    /// Emit an unsolicited AEM response (U-bit set) carrying @p command_type and
    /// @p body to every registered controller (plus the IDENTIFY multicast when the
    /// change targets the IDENTIFY control). Exposed so an entity can push a
    /// notification it framed itself; apply_local_descriptor_value() is the usual
    /// entry point. No-op if no send_response callback is wired.
    void emit_unsolicited(uint16_t command_type, std::span<uint8_t const> body);

    /// Periodic tick to check lock timeout and pending acquire timeout
    void tick(sm::TimePoint now) noexcept
    {
        if (locked_ && now >= lock_expiry_time_) {
            release_lock();
        }
        if (pending_acquire_.active && now >= pending_acquire_.timeout) {
            controller_available_timed_out();
        }
    }

    /// Get the current configuration index
    [[nodiscard]] auto current_configuration() const noexcept -> uint16_t { return current_configuration_; }

    /// Set the current configuration index
    /// @param config The configuration index to set as current
    void set_current_configuration(uint16_t config) noexcept { current_configuration_ = config; }

  private:
    // Command Handlers — each writes its response body into `out_buffer` and
    // returns status + bytes-written. `out_buffer` is guaranteed to be at
    // least MAX_AEM_RESPONSE_SIZE bytes by process_packet().

    /// Handle READ_DESCRIPTOR command.
    [[nodiscard]] auto handle_read_descriptor(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_NAME command (IEEE 1722.1 Clause 7.4.18). Parses the
    /// descriptor_type/index/name_index/config header and routes through
    /// AemEntityModel::get_name_for_wire -> the handler's symbol-keyed
    /// on_get_name. NOT_IMPLEMENTED if the handler serves no name for the
    /// requested (descriptor, name_index).
    [[nodiscard]] auto handle_get_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle SET_NAME command (IEEE 1722.1 Clause 7.4.17). Routes through
    /// AemEntityModel::apply_set_name -> the handler's symbol-keyed
    /// on_set_name, echoing the command as the response.
    [[nodiscard]] auto handle_set_name(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// AVDECC exclusive-access gate (IEEE 1722.1 7.4). Returns the AEM status a
    /// mutating command must be rejected with when the entity is acquired or locked
    /// by a DIFFERENT controller (ENTITY_ACQUIRED / ENTITY_LOCKED), or nullopt when
    /// the sender owns the entity (or it is free) and the command may proceed.
    [[nodiscard]] auto check_exclusive_access(AemDu const& header) const noexcept -> std::optional<uint8_t>;

    /// Build a rejection response: the given status, echoing the command payload so
    /// the controller can match the response. Applies no mutation.
    [[nodiscard]] static auto reject_command(uint8_t status, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle GET_STREAM_FORMAT — returns the STREAM descriptor's current_format.
    [[nodiscard]] auto handle_get_stream_format(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_SAMPLING_RATE — returns the AUDIO_UNIT descriptor's current_sampling_rate.
    [[nodiscard]] auto handle_get_sampling_rate(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle ENTITY_AVAILABLE command (no body).
    [[nodiscard]] static auto handle_entity_available(AemDu const& /*header*/) -> AemCommandResponse
    {
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }

    [[nodiscard]] auto handle_acquire_entity(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    [[nodiscard]] static auto build_acquire_response(AcquireLockResponseParams const& params, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    [[nodiscard]] auto handle_lock_entity(AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    [[nodiscard]] static auto build_lock_response(AcquireLockResponseParams const& params, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle GET_CONFIGURATION command.
    [[nodiscard]] auto handle_get_configuration(AemDu const& header, std::span<uint8_t> out_buffer) const -> AemCommandResponse;

    /// Handle SET_CONFIGURATION command (kit phase 5c): validate against
    /// the ENTITY descriptor's configurations_count; an actual switch runs
    /// the on_configuration_changed veto/apply hook (refused NOT_SUPPORTED
    /// when no hook is registered); a SET to the current configuration is
    /// an idempotent SUCCESS.
    [[nodiscard]] auto handle_set_configuration(std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer)
        -> AemCommandResponse;

    /// Handle CONTROLLER_AVAILABLE command (always SUCCESS, no body).
    [[nodiscard]] static auto handle_controller_available(AemDu const& /*header*/) -> AemCommandResponse
    {
        return {.status = AEM_STATUS_SUCCESS, .size = 0};
    }

    /// Handle a SET descriptor-value command (SET_CONTROL, and the wider family).
    /// Routes through AemEntityModel::apply_set_descriptor_value -> the handler's
    /// symbol-keyed on_set_descriptor_value, echoing the command as the response.
    [[nodiscard]] auto handle_set_descriptor_value(
        uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle a GET descriptor-value command (GET_CONTROL, and the wider family).
    /// Routes through AemEntityModel::get_descriptor_value_for_wire -> the handler's
    /// symbol-keyed on_get_descriptor_value.
    [[nodiscard]] auto handle_get_descriptor_value(
        uint16_t command_type, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_COUNTERS command (IEEE 1722.1 Clause 7.4.42): read the target
    /// descriptor's counters from the get_counters callback and emit an
    /// AemCountersPayload response (descriptor_type/index + counters_valid + 32
    /// counter values). NOT_IMPLEMENTED if no callback; NO_SUCH_DESCRIPTOR if the
    /// descriptor has no counters.
    [[nodiscard]] auto handle_get_counters(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle GET_STREAM_INFO command (IEEE 1722.1 Clause 7.4.16): read the target
    /// stream's parameters from the get_stream_info callback and emit an
    /// AemStreamInfoPayload response. NOT_IMPLEMENTED if no callback;
    /// NO_SUCH_DESCRIPTOR if the descriptor is not one of this entity's streams.
    [[nodiscard]] auto handle_get_stream_info(
        AemDu const& header, std::span<uint8_t const> command_data, std::span<uint8_t> out_buffer) -> AemCommandResponse;

    /// Handle REGISTER_UNSOLICITED_NOTIFICATION command (no body).
    [[nodiscard]] auto handle_register_unsolicited(AemDu const& header) -> AemCommandResponse;

    /// Handle DEREGISTER_UNSOLICITED_NOTIFICATION command (no body).
    [[nodiscard]] auto handle_deregister_unsolicited(AemDu const& header) -> AemCommandResponse;

    /// Frame one unsolicited AEM response (U-bit set) for @p command_type / @p body,
    /// addressed to @p controller_id, and send it to @p dest_mac via the callback.
    void send_unsolicited_to(
        Eui48 const& dest_mac, Eui64 const& controller_id, uint16_t command_type, std::span<uint8_t const> body);

    /// True if (@p command_type, @p body) is a SET_CONTROL targeting the configured
    /// IDENTIFY control — i.e. the change must also go to the IDENTIFY multicast.
    [[nodiscard]] auto targets_identify_control(uint16_t command_type, std::span<uint8_t const> body) const noexcept -> bool;

    /// Build and send AECP AEM response.
    /// @param dest_mac Destination MAC address for the response
    /// @param cmd_header The original command header to echo in the response
    /// @param status The AEM status code for the response
    /// @param response_body Bytes to place after the AemDu header (may be empty)
    [[nodiscard]] auto send_response(
        Eui48 const& dest_mac, AemDu const& cmd_header, uint8_t status, std::span<uint8_t const> response_body) const -> bool;

    AemCommandHandlerCallbacks callbacks_;

    // One of two code paths:
    //  (1) Legacy `EntityModel&` constructor: adapter_storage_ holds an
    //      EntityModelAdapter wrapping the supplied EntityModel, and
    //      handler_ points at it.
    //  (2) New `AemEntityHandler&` constructor: adapter_storage_ is
    //      empty, and handler_ points at the caller-supplied handler.
    //
    // `handle_read_descriptor` materializes a local `AemEntityModel{*handler_}`
    // on each call to do the dispatch — constructing the model per call
    // is free (two pointers plus an optional) and avoids a dangling
    // self-pointer if this object is moved. Phase 5 will delete the
    // EntityModelAdapter path entirely.
    std::optional<EntityModelAdapter> adapter_storage_;
    AemEntityHandler* handler_{nullptr};

    // Emit 2013/2016-length descriptors in READ_DESCRIPTOR responses.
    bool legacy_2016_ = false;

    // Acquisition state
    bool acquired_ = false;
    bool acquired_persistent_ = false;
    Eui64 acquiring_controller_;

    // Lock state
    bool locked_ = false;
    Eui64 locking_controller_;
    sm::TimePoint lock_expiry_time_{};

    // Current event time (set by process_packet or set_event_time)
    sm::TimePoint event_time_{};

    // Source MAC of the request currently being dispatched. Stashed by
    // process_packet() before calling handle_command() so per-command
    // handlers (specifically handle_acquire_entity) can retain it in
    // pending_acquire_ for later out-of-band response sending.
    Eui48 current_src_mac_{};

    // Pending acquire state for CONTROLLER_AVAILABLE handshake.
    // When active, the requester is waiting on a final response that will
    // be sent from controller_available_response_received() (DENY) or
    // controller_available_timed_out() (GRANT).
    struct PendingAcquire
    {
        AemDu requester_header{};
        Eui48 requester_mac{};
        uint32_t requester_flags{0};
        std::array<uint8_t, 16> command_data_copy{};
        sm::TimePoint timeout{};
        bool active{false};
    };
    PendingAcquire pending_acquire_{};

    // Current configuration
    uint16_t current_configuration_ = 0;
    statusbar::sg14::inplace_function<uint8_t(uint16_t), 64> on_configuration_changed_{};

    // Unsolicited notification registrations.
    // Slot occupancy is tracked by the enclosing SlotTable (its size() is
    // the source of truth), so there is no longer a `bool active` field.
    struct UnsolicitedRegistration
    {
        Eui64 controller_entity_id{};
        Eui48 controller_mac{};  ///< Source MAC of the REGISTER command, for unicast notifications
    };
    SlotTable<UnsolicitedRegistration, MAX_UNSOLICITED_REGISTRATIONS> unsolicited_registrations_{};

    // Our own entity_id, needed as the target_entity_id of unsolicited
    // notifications. Stashed by process_packet (always current) and settable
    // directly via set_entity_id() for notifications emitted before any command
    // has been received (e.g. an IDENTIFY button press at startup).
    Eui64 our_entity_id_{};

    // Monotonic sequence_id for unsolicited notifications (IEEE 1722.1 9.2.1.2.4);
    // increments once per emitted notification.
    uint16_t unsolicited_sequence_id_{0};

    // The CONTROL descriptor index of the entity's IDENTIFY control, if it has one.
    // When valid, a SET_CONTROL on this index also notifies the IDENTIFY multicast.
    uint16_t identify_control_index_{0};
    bool identify_control_index_valid_{false};
};

}  // namespace statusbar::nanoavb
