#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB SRP - MVRP/MSRP handler skeletons
/// Skeleton classes for MRP-based stream reservation protocols

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/srp/srp_msrp_participant.hpp"
#include "statusbar/srp/srp_mvrp_participant.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

using ieee::Eui48;
using ieee::Eui64;
using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;
using statusbar::srp::mrp::AttributeEvent;
using tsn::StreamId;
using TimePoint = sm::TimePoint;

//
// MVRP Types
//
/// VLAN registration state
enum class VlanState : uint8_t
{
    Unregistered,
    Registered,
    Pending,
    Failed,
};

/// VLAN registration info
struct VlanInfo
{
    uint16_t vlan_id = 0;
    VlanState state = VlanState::Unregistered;
};

//
// MSRP Types
//
/// Stream reservation state (for talker)
enum class TalkerReservationState : uint8_t
{
    Idle,
    Advertising,
    Ready,
    Failed,
};

/// Stream reservation state (for listener)
enum class ListenerReservationState : uint8_t
{
    Idle,
    AskingFailed,
    Ready,
    ReadyFailed,
    /// Attached, declaring nothing until the talker's attribute is
    /// registered (the Milan listener: set_listener_waits_for_talker).
    Waiting,
};

/// Talker stream info for MSRP
struct TalkerStreamSrpInfo
{
    StreamId stream_id{};
    Eui48 dest_address{};
    uint16_t vlan_id = srp::DEFAULT_SR_CLASS_A_VID;
    uint16_t max_frame_size = 0;
    uint16_t max_interval_frames = 0;
    uint8_t priority = srp::SR_CLASS_A_PRIORITY;
    uint32_t accumulated_latency = 0;
    TalkerReservationState state = TalkerReservationState::Idle;
    uint8_t failure_code = 0;
    Eui64 failure_bridge_id{};
};

/// The Talker attribute a listener stream follows: the destination address and VLAN its connection
/// names (IEEE 802.1Q-2022 Clause 35.2.2.8.3).
struct TalkerMatch
{
    Eui48 destination{};
    uint16_t vid{0};  ///< 0: any (a probe answer naming none)
};

/// Listener stream info for MSRP: one record per talker stream this port
/// listens to, however many of its inputs are bound to that stream.
struct ListenerStreamSrpInfo
{
    StreamId stream_id{};
    ListenerReservationState state = ListenerReservationState::Idle;
    /// Inputs attached to the stream. The Listener declaration stands on the
    /// wire while this is above zero and is withdrawn when the last goes.
    uint16_t attached{0};
    std::optional<TalkerMatch> talker{};  ///< none: any Talker attribute of the StreamID
};

/// Domain info for MSRP: one SR class's SRclassID, SRclassPriority and SRclassVID (IEEE 802.1Q-2022 Clause 35.2.2.9).
struct DomainInfo
{
    uint8_t sr_class_id = 6;  // Class A
    uint8_t sr_class_priority = 3;
    uint16_t sr_class_vid = 2;
};

/// The default Class B Domain (IEEE 802.1Q-2022 Clause 35.2.2.9): SRclassID 5, priority 2, VID 2.
inline constexpr DomainInfo CLASS_B_DOMAIN{
    .sr_class_id = statusbar::srp::msrp::SR_CLASS_B,
    .sr_class_priority = srp::SR_CLASS_B_PRIORITY,
    .sr_class_vid = srp::DEFAULT_SR_CLASS_B_VID};

/// The MSRP participant's tables for an entity's components, at full capacity, with part of
/// each reserved for this entity's own streams and their peers so other stations' attributes
/// never refuse ours with attribute_table_full (docs/NANOAVB_ARCHITECTURE.md).
template <class Limits = statusbar::srp::msrp::DefaultMsrpLimits>
[[nodiscard]] constexpr auto entity_msrp_config(size_t const talker_streams, size_t const listener_streams) noexcept
    -> statusbar::srp::msrp::MsrpConfig
{  // NOLINT(statusbar-block-statements): the MSRP table limits, field by field
    statusbar::srp::msrp::MsrpConfig config{};
    config.max_talker_advertise = Limits::max_talker_advertise;
    config.max_talker_failed = Limits::max_talker_failed;
    config.max_listeners = Limits::max_listeners;
    config.reserved_talker_advertise = std::min(talker_streams + listener_streams, config.max_talker_advertise);
    config.reserved_talker_failed = std::min(listener_streams, config.max_talker_failed);
    config.reserved_listeners = std::min(talker_streams + listener_streams, config.max_listeners);
    return config;
}

/// Milan's MRP timers (Milan v1.2 Section 4.2.7.1.1, Table 4.3): JoinTime 200 ms, LeaveTime
/// 5000 ms, leavealltimer 10-15 s, periodictimer 1000 ms.
inline constexpr statusbar::srp::mrp::TimerConfig MILAN_MRP_TIMERS{
    .join_time = std::chrono::milliseconds{200},
    .leave_time = std::chrono::milliseconds{5000},
    .leave_all_time_min = std::chrono::milliseconds{10000},
    .leave_all_time_max = std::chrono::milliseconds{15000},
    .periodic_time = std::chrono::milliseconds{1000},
};

/// @p config with Milan's MRP timers; the registrar's immediate leave on rLv! (Milan v1.2 Section
/// 4.2.7.2.2) is MsrpHandler::set_instant_leave.
[[nodiscard]] constexpr auto milan_msrp_config(statusbar::srp::msrp::MsrpConfig config) noexcept -> statusbar::srp::msrp::MsrpConfig
{
    config.timers = MILAN_MRP_TIMERS;
    return config;
}

/// @p config with Milan's MRP timers (Table 4.3 covers every MRP application); MVRP keeps
/// IEEE 802.1Q's registrar.
[[nodiscard]] constexpr auto milan_mvrp_config(statusbar::srp::mvrp::MvrpConfig config) noexcept -> statusbar::srp::mvrp::MvrpConfig
{
    config.timers = MILAN_MRP_TIMERS;
    return config;
}

//
// MVRP Handler Callback Interface
//
/// Callbacks for MVRP operations
/// The network layer implements these to send/receive MVRP packets
struct MvrpCallbacks
{
    /// Called when the handler needs to send an MVRP packet
    /// @param packet The raw MVRP packet data to send
    /// @return true if send was successful
    statusbar::sg14::inplace_function<bool(std::span<uint8_t const> packet), 64> send_packet;

    /// Called when VLAN registration state changes
    statusbar::sg14::inplace_function<void(uint16_t vlan_id, VlanState state), 64> on_vlan_state_change;
};

//
// MVRP Handler
//
/// MvrpHandler - skeleton class for MVRP VLAN registration
/// Uses statusbar::srp::mvrp structures
/// Does NOT directly send/receive packets - uses callback interface
class MvrpHandler
{
  public:
    /// Construct with an explicit MVRP config and callbacks. The
    /// config is forwarded to the underlying MvrpParticipant and
    /// determines the fixed capacity for all attribute and observer
    /// tables; sizing must be decided at program initialization time.
    explicit MvrpHandler(statusbar::srp::mvrp::MvrpConfig const& config, MvrpCallbacks callbacks = {})
        : callbacks_{std::move(callbacks)}
        , participant_{config}
    {
        // vlans_ is fixed-capacity (inplace_vector); nothing to reserve. The
        // participant_ ctor already validates config.max_vlans against the
        // compile-time limit (throws std::bad_alloc if it exceeds capacity).
        //
        // The participant sends through this handler, once and for good: every
        // PDU goes to whatever send_packet is in force at that moment, so a
        // callback replaced by set_callbacks() or cleared by drop_sender() is
        // never outlived by a copy inside the participant. `this` is stable:
        // the handler is neither copied nor moved (below).
        participant_.set_send_pdu([this](std::span<uint8_t const> const packet) { return send(packet); });
    }
    MvrpHandler(MvrpHandler const&) = delete;
    MvrpHandler(MvrpHandler&&) = delete;
    auto operator=(MvrpHandler const&) -> MvrpHandler& = delete;
    auto operator=(MvrpHandler&&) -> MvrpHandler& = delete;
    ~MvrpHandler() = default;

    /// Get the MVRP EtherType
    [[nodiscard]] static constexpr auto ethertype() noexcept -> uint16_t { return srp::MVRP_ETHERTYPE; }

    /// Get the MVRP multicast address
    [[nodiscard]] static constexpr auto multicast_address() noexcept -> uint64_t { return srp::MVRP_MULTICAST_ADDRESS; }

    /// Set or update callbacks after construction
    /// This allows wiring up network handlers after both components are created
    /// @param callbacks The new callback interface
    void set_callbacks(MvrpCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Forget the sender: nothing goes out until set_callbacks() installs
    /// another. For the owner of the network path to call before it takes
    /// that path away, so a Leave or re-declaration that falls due in
    /// between has nowhere stale to go.
    void drop_sender() noexcept { callbacks_.send_packet = {}; }

    /// Register a VLAN
    /// @param vlan_id The VLAN ID to register (1-4094)
    /// @return Error if VLAN ID is invalid
    [[nodiscard]] auto register_vlan(uint16_t vlan_id, TimePoint now) -> Status;

    /// Withdraw a VLAN registration
    /// @param vlan_id The VLAN ID to withdraw
    [[nodiscard]] auto withdraw_vlan(uint16_t vlan_id, TimePoint now) -> Status;

    /// The end of a run: withdraw every VLAN this port registered and send
    /// the Leaves now rather than at a join timer a stopping reactor never
    /// reaches. Nothing registered: nothing sent.
    void withdraw_all(TimePoint now);

    /// Get the registration state of a VLAN
    /// @param vlan_id The VLAN ID to query
    [[nodiscard]] auto get_vlan_state(uint16_t vlan_id) const noexcept -> VlanState;

    /// Process a received MVRP packet
    /// @param packet The raw MVRP packet data
    /// @param now Current time for timeout tracking
    auto receive_packet(std::span<uint8_t const> packet, TimePoint now) -> void;

    /// Periodic tick - call this regularly to process timers
    /// @param now Current time for timer processing
    auto tick(TimePoint now) -> void;

    /// A LeaveAll sent now (MvrpParticipant::leave_all()).
    auto leave_all(TimePoint now) -> void { participant_.leave_all(now); }

    /// The MVRP participant's configuration (its tables and MRP timers).
    [[nodiscard]] auto mvrp_config() const noexcept -> statusbar::srp::mvrp::MvrpConfig const& { return participant_.config(); }

    /// Get all registered VLANs
    [[nodiscard]] auto vlans() const noexcept -> std::span<VlanInfo const> { return vlans_; }

  private:
    /// The one way a PDU leaves: through the send_packet in force now.
    auto send(std::span<uint8_t const> const packet) -> bool
    {
        return callbacks_.send_packet ? callbacks_.send_packet(packet) : false;
    }

    MvrpCallbacks callbacks_;
    // Fixed-capacity (no heap): VLAN count per endpoint is tiny in practice.
    // Capacity matches the wrapped MvrpParticipant's compile-time limit.
    statusbar::sg14::inplace_vector<VlanInfo, statusbar::srp::mvrp::DefaultMvrpLimits::max_vlans> vlans_;
    statusbar::srp::mvrp::MvrpParticipant participant_;
    bool started_{false};
};

//
// MSRP Handler Callback Interface
//
/// Callbacks for MSRP operations
/// The network layer implements these to send/receive MSRP packets
struct MsrpCallbacks
{
    /// Called when the handler needs to send an MSRP packet
    /// @param packet The raw MSRP packet data to send
    /// @return true if send was successful
    statusbar::sg14::inplace_function<bool(std::span<uint8_t const> packet), 64> send_packet;

    /// Called when talker stream reservation state changes, told the time of
    /// the call that changed it. Reserved: the handler declares its own talker
    /// state from talker_advertise()/talker_withdraw() and fires nothing here
    /// yet; a consumer written against it gets the time once it does.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, TalkerReservationState state, TimePoint now), 64>
        on_talker_state_change;

    /// Called when an attached listener stream's declaration changes because
    /// the remote talker's registration did (AskingFailed <-> Ready, see
    /// on_remote_talker_changed). Fires from receive_packet()/tick(), told that
    /// call's time: a Milan sink's re-probe timer starts from the event's
    /// instant, not from whatever the consumer has lying around.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, ListenerReservationState state, TimePoint now), 64>
        on_listener_state_change;

    /// Called when the set of remote listeners for one of OUR advertised talker
    /// streams changes. `ready` is true when at least one registered listener
    /// permits transmit on `stream_id` (the talker may start sending), false
    /// otherwise. Fires from receive_packet()/tick() on listener register/leave,
    /// told that call's time: the talker drives its own SM from it.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, bool ready, TimePoint now), 64> on_talker_listener;

    /// Called when the handler adopted a Class A Domain registered from the
    /// wire (Milan v1.3 4.2.7.2.1, see MsrpHandler::set_adopt_domain): the
    /// Domain now declared, and the time of the receive_packet() that brought
    /// the peer's. The entity takes the new VLAN and priority to its streams.
    statusbar::sg14::inplace_function<void(DomainInfo const& domain, TimePoint now), 64> on_domain_adopted;
};

//
// MSRP Handler
//
/// MsrpHandler - skeleton class for MSRP stream reservation
/// @tparam MaxStreams Compile-time capacity for talker/listener stream tracking; a withdrawn
///         (Idle) entry is reused by the next new StreamID
/// @tparam Limits The MSRP participant's table capacities
template <size_t MaxStreams = 32, class Limits = statusbar::srp::msrp::DefaultMsrpLimits>
class MsrpHandler
{
  public:
    explicit MsrpHandler(statusbar::srp::msrp::MsrpConfig const& config, MsrpCallbacks callbacks = {})
        : callbacks_{std::move(callbacks)}
        , participant_{config}
    {
        // The participant sends through this handler, once and for good: every
        // PDU goes to whatever send_packet is in force at that moment, so a
        // callback replaced by set_callbacks() or cleared by drop_sender() is
        // never outlived by a copy inside the participant.
        participant_.set_send_pdu([this](std::span<uint8_t const> const packet) { return send(packet); });
        // Observe remote listener declarations so we can tell a talker when a
        // listener becomes ready (or stops being ready) for one of its streams.
        // Safe: the handler is neither copied nor moved (below), so `this` is
        // stable for the lifetime of the captured observer and sender.
        (void)participant_.subscribe(statusbar::srp::msrp::Observer{
            .on_talker_advertise =
                [this](statusbar::srp::msrp::TalkerAdvertiseFirstValue const& fv, statusbar::srp::msrp::Operation op) {
                    if (op == statusbar::srp::msrp::Operation::Register) {
                        on_remote_talker_changed(fv.stream_id);
                    }
                },
            .on_talker_failed =
                [this](statusbar::srp::msrp::TalkerFailedFirstValue const& fv, statusbar::srp::msrp::Operation op) {
                    if (op == statusbar::srp::msrp::Operation::Register) {
                        on_remote_talker_changed(fv.advertise.stream_id);
                    }
                },
            .on_talker_leave = [this](tsn::StreamId const& sid) { on_remote_talker_changed(sid); },
            .on_listener = [this](
                               tsn::StreamId const& sid,
                               statusbar::srp::msrp::ListenerDeclaration /*decl*/,
                               statusbar::srp::msrp::Operation /*op*/) { notify_talker_listener(sid); },
            .on_listener_leave = [this](tsn::StreamId const& sid) { notify_talker_listener(sid); },
            .on_domain =
                [this](statusbar::srp::msrp::DomainFirstValue const& fv, statusbar::srp::msrp::Operation op) {
                    on_remote_domain(fv, op);
                },
        });
    }

    MsrpHandler(MsrpHandler const&) = delete;
    MsrpHandler(MsrpHandler&&) = delete;
    auto operator=(MsrpHandler const&) -> MsrpHandler& = delete;
    auto operator=(MsrpHandler&&) -> MsrpHandler& = delete;
    ~MsrpHandler() = default;

    [[nodiscard]] static constexpr auto ethertype() noexcept -> uint16_t { return srp::MSRP_ETHERTYPE; }
    [[nodiscard]] static constexpr auto multicast_address() noexcept -> uint64_t { return srp::MSRP_MULTICAST_ADDRESS; }

    void set_callbacks(MsrpCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Forget the sender: nothing goes out until set_callbacks() installs
    /// another. For the owner of the network path to call before it takes
    /// that path away, so a Leave or re-declaration that falls due in
    /// between has nowhere stale to go. The other hooks stay.
    void drop_sender() noexcept { callbacks_.send_packet = {}; }

    /// Forward a reactor-thread logger to the MSRP participant (Debug-level
    /// MRP event diagnostics).
    void set_logger(logging::Logger const log) noexcept { participant_.set_logger(log); }
    /// The MSRP participant's configuration (its tables, MRP timers and leave rule).
    [[nodiscard]] auto msrp_config() const noexcept -> statusbar::srp::msrp::MsrpConfig const& { return participant_.config(); }

    /// Set just the on_talker_listener hook without disturbing the others
    /// (set_callbacks replaces the whole struct; this updates one field).
    void set_on_talker_listener(statusbar::sg14::inplace_function<void(StreamId const&, bool, TimePoint), 64> cb)
    {
        callbacks_.on_talker_listener = std::move(cb);
    }

    /// Set just the on_domain_adopted hook without disturbing the others.
    void set_on_domain_adopted(statusbar::sg14::inplace_function<void(DomainInfo const&, TimePoint), 64> cb)
    {
        callbacks_.on_domain_adopted = std::move(cb);
    }

    /// Milan v1.3 4.2.7.2.1: a PAAD declares Class A with priority 3 and VLAN 2
    /// and, on receiving a Class A Domain with other parameters, "shall update
    /// its parameters using the received FirstValue and start declaring a
    /// matching MSRP Domain attribute". On, a registered Class A Domain that
    /// differs from ours replaces our priority and VLAN (Class B follows the
    /// VLAN), is re-declared (the old value leaves first) and is reported to
    /// on_domain_adopted. Off by default: IEEE 802.1Q keeps our own.
    void set_adopt_domain(bool const on) noexcept { adopt_domain_ = on; }

    /// Milan v1.3 4.2.7.2.2 instant leave for every Registrar of this port
    /// (see MsrpParticipant::set_instant_leave).
    void set_instant_leave(bool const on) noexcept { participant_.set_instant_leave(on); }
    /// Set just the packet sender (set_callbacks replaces the whole struct).
    void set_send_packet(statusbar::sg14::inplace_function<bool(std::span<uint8_t const> packet), 64> send)
    {
        callbacks_.send_packet = std::move(send);
    }

    /// Called when a peer's Talker Advertise or Talker Failed for @p stream_id is registered or
    /// leaves, after an attached listener's declaration has followed it.
    void set_on_remote_talker(statusbar::sg14::inplace_function<void(StreamId const&, TimePoint), 64> cb)
    {
        on_remote_talker_ = std::move(cb);
    }

    /// The Milan listener (v1.2 Section 5.3.8.5): Listener Ready while the talker's Advertise is
    /// registered, Asking Failed while its Talker Failed is, otherwise nothing; never Ready Failed.
    void set_listener_waits_for_talker(bool const wait) noexcept { listener_waits_for_talker_ = wait; }

    /// Diagnostic breakdown of why the talker gate is open/closed for a stream
    /// (record present? operation? registrar In? Ready substate?), for logging
    /// the listener-ready flap. See MsrpParticipant::listener_permit_debug.
    [[nodiscard]] auto listener_permit_debug(StreamId const& stream_id) const noexcept
    {
        return participant_.listener_permit_debug(stream_id);
    }
    void set_domain(DomainInfo const& domain) noexcept { domain_ = domain; }
    [[nodiscard]] auto domain() const noexcept -> DomainInfo const& { return domain_; }

    /// Sticky-Listener workaround passthrough. When enabled, registered Listener
    /// attributes are re-declared on every periodic/LeaveAll pass so a bridge
    /// keeps the forwarding path to a downstream listener warm. Needed when
    /// feeding a downstream listener through an AVB switch (the strict
    /// end-station change silenced the echo).
    /// See MsrpParticipant::set_redeclare_registered_listeners.
    void set_redeclare_registered_listeners(bool enabled) noexcept { participant_.set_redeclare_registered_listeners(enabled); }

    /// Suppress-LeaveAll workaround passthrough. When enabled, this participant
    /// never originates a LeaveAll; it only re-asserts via the periodic timer.
    /// Needed when feeding a downstream listener through an AVB switch whose
    /// stream forwarding blinks when we send periodic LeaveAlls. See
    /// MsrpParticipant::set_suppress_leaveall.
    void set_suppress_leaveall(bool enabled) noexcept { participant_.set_suppress_leaveall(enabled); }

    /// Declare (or update) @p info's Talker Advertise and record it Advertising. Returns its entry's
    /// index. Errors: NanoAvbError::SrpRegistrationFailed (table full), the participant's.
    [[nodiscard]] auto talker_advertise(TalkerStreamSrpInfo const& info, TimePoint now) -> StatusValue<size_t>
    {
        auto const idx = stream_slot(talker_streams_, info.stream_id, TalkerReservationState::Idle);
        if (!idx) {
            return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
        }
        // A talker's priority is its SR class's (IEEE 802.1Q-2022 Clause 35.2.2.8.5).
        TalkerStreamSrpInfo declared = info;
        declared.priority = domain_.sr_class_priority;
        // Declared first: a full attribute table leaves the reservation entries as they were.
        if (auto status = declare_talker_attribute(declared, now); !status) {
            return failure(status.error());
        }
        store_stream(talker_streams_, *idx, advertising(declared));
        return *idx;
    }

    [[nodiscard]] auto talker_withdraw(StreamId const& stream_id, TimePoint now) -> Status
    {
        for (auto& stream : talker_streams_) {
            if (stream.stream_id == stream_id) {
                stream.state = TalkerReservationState::Idle;
                (void)participant_.withdraw_talker(stream_id, now);
                return success();
            }
        }
        return failure(make_error_code(NanoAvbError::InvalidStreamIndex));
    }

    /// Declare the configured SR class's MSRP Domain on the VID in use (IEEE 802.1Q Clauses 35.2.1.4,
    /// 35.2.2.9; Milan v1.2 Section 4.2.7.2.1, Class A), so the bridge does not treat the port as an SRP
    /// domain boundary. Idempotent. The one start of a run after withdraw_all() (resume()).
    [[nodiscard]] auto declare_domain(TimePoint now) -> Status
    {
        bool const resumed = std::exchange(withdrawn_, false);
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        declare_sr_class(domain_.sr_class_id, domain_.sr_class_priority, now);  // 6 for Class A, 5 for Class B
        if (resumed) {
            resume(now);
        }
        return success();
    }

    /// Declare one SR class Domain on the handler's VLAN.
    void declare_sr_class(uint8_t const sr_class_id, uint8_t const priority, TimePoint now)
    {
        statusbar::srp::msrp::DomainFirstValue fv{};
        fv.sr_class_id = sr_class_id;
        fv.sr_class_priority = priority;
        fv.sr_class_vid = domain_.sr_class_vid;
        (void)participant_.declare_domain(fv, now);
    }

    /// Whether withdraw_all() ended a run and no declare_domain() has begun one since: the next resumes.
    [[nodiscard]] auto withdrawn() const noexcept -> bool { return withdrawn_; }
    /// The link is back after a down (MsrpParticipant::rejoin()).
    void rejoin(TimePoint now) { participant_.rejoin(now); }
    /// A LeaveAll sent now (MsrpParticipant::leave_all()).
    void leave_all(TimePoint now) { participant_.leave_all(now); }

    [[nodiscard]] auto get_talker_stream(StreamId const& stream_id) const noexcept -> TalkerStreamSrpInfo const*
    {
        for (auto const& stream : talker_streams_) {
            if (stream.stream_id == stream_id) {
                return &stream;
            }
        }
        return nullptr;
    }

    /// One more input attached to @p stream_id, declared Ready. A Listener
    /// attribute is identified by its StreamID (IEEE 802.1Q 35.2.2.8), so a
    /// port has one declaration per stream however many inputs are bound to
    /// it: the record counts the inputs and the declaration follows the
    /// count (see listener_withdraw). Returns the record's index.
    [[nodiscard]] auto listener_ready(StreamId const& stream_id, TimePoint now) -> StatusValue<size_t>
    {
        return attach_declared(stream_id, ListenerReservationState::Ready, statusbar::srp::msrp::ListenerDeclaration::Ready, now);
    }

    /// One more input attached to @p stream_id, declared Asking Failed.
    [[nodiscard]] auto listener_asking_failed(StreamId const& stream_id, TimePoint now) -> Status
    {
        auto const idx = attach_declared(
            stream_id, ListenerReservationState::AskingFailed, statusbar::srp::msrp::ListenerDeclaration::AskingFailed, now);
        if (!idx) {
            return failure(idx.error());
        }
        return success();
    }

    /// One input lets go of @p stream_id. The declaration is withdrawn from
    /// the wire only when the last input attached to the stream has gone:
    /// two inputs bound to one talker stream share one Listener declaration,
    /// and the first to unbind must not take it from the other. A stream no
    /// input is attached to is left as it is, and the call succeeds.
    [[nodiscard]] auto listener_withdraw(StreamId const& stream_id, TimePoint now) -> Status
    {
        size_t const idx = listener_index(stream_id);
        if (idx == listener_streams_.size() || listener_streams_[idx].attached == 0) {
            return success();
        }
        if (--listener_streams_[idx].attached > 0) {
            return success();
        }
        bool const declared = listener_is_declared(listener_streams_[idx].state);
        listener_streams_[idx].state = ListenerReservationState::Idle;
        if (declared) {
            (void)participant_.withdraw_listener(stream_id, now);
        }
        return success();
    }

    /// The end of a run: withdraw every talker, every declared listener and the domain this port
    /// declared, and send their Leaves now (a Leave left for the join timer never goes from a stopping
    /// reactor). A listener attached Waiting stays attached, for the next run's declare_domain().
    void withdraw_all(TimePoint now)
    {
        withdrawn_ = true;
        if (!started_) {
            return;
        }
        withdraw_talkers(now);
        withdraw_listeners(now);
        (void)participant_.withdraw_domain(statusbar::srp::msrp::SR_CLASS_A, now);
        (void)participant_.withdraw_domain(statusbar::srp::msrp::SR_CLASS_B, now);
        transmit_now(now);
    }

    [[nodiscard]] auto get_listener_stream(StreamId const& stream_id) const noexcept -> ListenerStreamSrpInfo const*
    {
        for (auto const& stream : listener_streams_) {
            if (stream.stream_id == stream_id) {
                return &stream;
            }
        }
        return nullptr;
    }

    /// A peer's Talker Advertise / Talker Failed for @p stream_id while its registrar holds it,
    /// or nullptr; never our own (Milan v1.2 Section 5.3.8.8).
    [[nodiscard]] auto peer_talker_advertise(StreamId const& stream_id) const noexcept
        -> statusbar::srp::msrp::TalkerAdvertiseFirstValue const*
    {
        return participant_.find_registered_talker_advertise(stream_id);
    }
    [[nodiscard]] auto peer_talker_failed(StreamId const& stream_id) const noexcept
        -> statusbar::srp::msrp::TalkerFailedFirstValue const*
    {
        return participant_.find_registered_talker_failed(stream_id);
    }
    /// Whether a peer's Talker Advertise or Failed for @p stream_id is registered In, not Leaving.
    [[nodiscard]] auto peer_talker_in(StreamId const& stream_id) const noexcept -> bool
    {
        return participant_.talker_registered_in(stream_id);
    }

    /// A peer's registered Talker Advertise / Talker Failed for the stream sent to @p destination on
    /// @p vid, or nullptr (IEEE 802.1Q-2022 Clause 35.2.2.8.3: one stream per destination).
    [[nodiscard]] auto peer_talker_advertise(Eui48 const& destination, uint16_t const vid) const noexcept
        -> statusbar::srp::msrp::TalkerAdvertiseFirstValue const*
    {
        return participant_.find_registered_talker_advertise(destination, vid);
    }
    [[nodiscard]] auto peer_talker_failed(Eui48 const& destination, uint16_t const vid) const noexcept
        -> statusbar::srp::msrp::TalkerFailedFirstValue const*
    {
        return participant_.find_registered_talker_failed(destination, vid);
    }

    /// Subscribe @p observer to the participant's registrations beside this handler's own; it is called
    /// after the handler has followed each one, except a listener stream's talker within an MRPDU, which
    /// the handler follows once the MRPDU is read (on_remote_talker). See MsrpParticipant::subscribe.
    [[nodiscard]] auto subscribe(statusbar::srp::msrp::Observer observer) ->
        typename statusbar::srp::msrp::MsrpParticipantT<Limits>::SubscriptionId
    {
        return participant_.subscribe(std::move(observer));
    }

    /// The registered (peer-declared, registrar In or Leaving) Listener
    /// declaration for one of our talker streams, or Ignore when none:
    /// Ready, ReadyFailed or AskingFailed.
    [[nodiscard]] auto registered_listener(StreamId const& stream_id) const noexcept -> statusbar::srp::msrp::ListenerDeclaration
    {
        auto const d = participant_.listener_permit_debug(stream_id);
        if (!d.has_record || d.operation != statusbar::srp::msrp::Operation::Register || !d.registrar_in) {
            return statusbar::srp::msrp::ListenerDeclaration::Ignore;
        }
        return d.substate;
    }

    /// True when the talker's Advertise is registered from the wire and no Talker Failed has
    /// replaced it; with set_listener_waits_for_talker, only a peer's live registration counts.
    [[nodiscard]] auto talker_advertise_registered(StreamId const& stream_id) const noexcept -> bool
    {
        return talker_advertise_registered(stream_id, std::nullopt);
    }
    /// As above, counting only a Talker Advertise or Failed on @p talker's destination and VLAN (any without one).
    [[nodiscard]] auto talker_advertise_registered(
        StreamId const& stream_id, std::optional<TalkerMatch> const& talker) const noexcept -> bool
    {
        if (listener_waits_for_talker_) {
            return matching(talker, peer_talker_advertise(stream_id)) && !talker_failed_registered(stream_id, talker);
        }
        return matching(talker, participant_.find_talker_advertise(stream_id)) &&
            !matching(talker, advertise_of(participant_.find_talker_failed(stream_id)));
    }
    /// A peer's Talker Failed for @p stream_id is registered on @p talker's destination and VLAN (any without one).
    [[nodiscard]] auto talker_failed_registered(StreamId const& stream_id, std::optional<TalkerMatch> const& talker) const noexcept
        -> bool
    {
        return matching(talker, advertise_of(peer_talker_failed(stream_id)));
    }

    /// Attach this listener to @p stream_id (kit phase 5d, IEEE 802.1Q
    /// 35.2.4.4): declare Listener Ready when the talker's Advertise is
    /// registered, Listener Asking Failed otherwise (the talker is not
    /// advertising yet, or registered Talker Failed). While attached, the
    /// declaration is upgraded/downgraded automatically as the talker's
    /// registration changes (see on_remote_talker_changed). Returns the
    /// state actually declared. Use instead of listener_ready() when the
    /// talker may not be up yet — e.g. an ACMP fast-connect at boot.
    ///
    /// With set_listener_waits_for_talker (Milan), nothing is declared until the talker's
    /// Advertise (Ready) or Talker Failed (Asking Failed) is registered: attached in Waiting.
    /// With @p talker, only a Talker attribute on its destination and VLAN counts, now and later.
    [[nodiscard]] auto listener_attach(
        StreamId const& stream_id, TimePoint now, std::optional<TalkerMatch> const& talker = std::nullopt)
        -> StatusValue<ListenerReservationState>
    {
        auto const state = attach_state(stream_id, talker);
        auto const attached = attach_as(stream_id, state, now);
        // Recorded even when the attach failed: an earlier connection's match is not this one's.
        if (auto* const entry = std::ranges::find(listener_streams_, stream_id, &ListenerStreamSrpInfo::stream_id);
            entry != listener_streams_.end()) {
            entry->talker = talker;
        }
        if (!attached) {
            return failure(attached.error());
        }
        return success(state);
    }

    auto receive_packet(std::span<uint8_t const> packet, TimePoint now) -> void
    {
        if (packet.size() < 3) {
            return;
        }
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        last_event_time_ = now;
        receiving_ = true;
        participant_.receive_pdu(packet, now);
        receiving_ = false;
        follow_changed_talkers();
    }

    auto tick(TimePoint now) -> void
    {
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        last_event_time_ = now;
        participant_.tick(now);
    }

    [[nodiscard]] auto talker_streams() const noexcept -> std::span<TalkerStreamSrpInfo const> { return talker_streams_; }
    [[nodiscard]] auto listener_streams() const noexcept -> std::span<ListenerStreamSrpInfo const> { return listener_streams_; }

  private:
    /// Every talker that is advertising, withdrawn (withdraw_all).
    void withdraw_talkers(TimePoint now)
    {
        for (auto& stream : talker_streams_) {
            if (stream.state != TalkerReservationState::Idle) {
                stream.state = TalkerReservationState::Idle;
                (void)participant_.withdraw_talker(stream.stream_id, now);
            }
        }
    }

    /// Every listener that is declared, withdrawn (withdraw_all); one still Waiting stays attached:
    /// the next run declares it when its talker is registered (follow_waiting_listeners).
    void withdraw_listeners(TimePoint now)
    {
        for (auto& stream : listener_streams_) {
            if (stream.state == ListenerReservationState::Waiting) {
                continue;  // never declared; its inputs stay attached
            }
            if (listener_is_declared(stream.state)) {
                (void)participant_.withdraw_listener(stream.stream_id, now);
            }
            stream.state = ListenerReservationState::Idle;
            stream.attached = 0;
        }
    }

    /// A run's start after withdraw_all(): the peer's registrations of the last run forgotten and asked for again
    /// (MsrpParticipant::resume()), then every Waiting listener re-checked against what is registered, at @p now.
    void resume(TimePoint now)
    {
        last_event_time_ = now;
        participant_.resume(now);
        follow_waiting_listeners(now);
    }

    /// Every Waiting listener re-checked against the peer's registrations, at @p now.
    void follow_waiting_listeners(TimePoint now)
    {
        for (size_t i = 0; i < listener_streams_.size(); ++i) {
            if (listener_streams_[i].state == ListenerReservationState::Waiting) {
                (void)follow_remote_talker(listener_streams_[i].stream_id, now);
            }
        }
    }

    /// Whether @p fv is a Talker attribute on @p talker's destination and VLAN (any without one).
    [[nodiscard]] static auto matching(
        std::optional<TalkerMatch> const& talker, statusbar::srp::msrp::TalkerAdvertiseFirstValue const* fv) noexcept -> bool
    {
        return fv != nullptr &&
            (!talker ||
             (fv->destination_address == talker->destination && (talker->vid == 0 || fv->vlan_identifier.get() == talker->vid)));
    }
    [[nodiscard]] static auto advertise_of(statusbar::srp::msrp::TalkerFailedFirstValue const* fv) noexcept
        -> statusbar::srp::msrp::TalkerAdvertiseFirstValue const*
    {
        return fv != nullptr ? &fv->advertise : nullptr;
    }

    /// The state a listener of @p stream_id following @p talker wants: what listener_attach declares
    /// and what a re-declaration moves an attached stream to.
    [[nodiscard]] auto attach_state(StreamId const& stream_id, std::optional<TalkerMatch> const& talker) const noexcept
        -> ListenerReservationState
    {
        if (talker_advertise_registered(stream_id, talker)) {
            return ListenerReservationState::Ready;
        }
        if (listener_waits_for_talker_ && !talker_failed_registered(stream_id, talker)) {
            return ListenerReservationState::Waiting;
        }
        return ListenerReservationState::AskingFailed;
    }

    /// Attach @p stream_id in @p state (Ready, Waiting or Asking Failed) at @p now.
    [[nodiscard]] auto attach_as(StreamId const& stream_id, ListenerReservationState const state, TimePoint now) -> Status
    {
        if (state != ListenerReservationState::Ready) {
            return state == ListenerReservationState::Waiting ? attach_waiting(stream_id, now)
                                                              : listener_asking_failed(stream_id, now);
        }
        if (auto const r = listener_ready(stream_id, now); !r) {
            return failure(r.error());
        }
        return success();
    }

    /// The index of @p stream_id's record, or listener_streams_.size().
    [[nodiscard]] auto listener_index(StreamId const& stream_id) const noexcept -> size_t
    {
        for (size_t i = 0; i < listener_streams_.size(); ++i) {
            if (listener_streams_[i].stream_id == stream_id) {
                return i;
            }
        }
        return listener_streams_.size();
    }

    /// The index of @p stream_id's entry in @p streams, else of a withdrawn (@p idle) entry to reuse,
    /// else the end while there is room; nullopt when the table is full.
    template <class Streams, class State>
    [[nodiscard]] static auto stream_slot(Streams const& streams, StreamId const& stream_id, State const idle) noexcept
        -> std::optional<size_t>
    {
        auto found = std::ranges::find(streams, stream_id, &Streams::value_type::stream_id);
        if (found == streams.end()) {
            found = std::ranges::find(streams, idle, &Streams::value_type::state);
        }
        if (found != streams.end()) {
            return static_cast<size_t>(found - streams.begin());
        }
        return streams.size() < MaxStreams ? std::optional<size_t>{streams.size()} : std::nullopt;
    }

    /// Put @p entry at @p idx of @p streams (a stream_slot() index: an entry, or the end).
    template <class Streams, class Entry>
    static void store_stream(Streams& streams, size_t const idx, Entry const& entry) noexcept
    {
        if (idx < streams.size()) {
            streams[idx] = entry;
            return;
        }
        (void)streams.try_push_back(entry);
    }

    /// @p info, Advertising.
    [[nodiscard]] static auto advertising(TalkerStreamSrpInfo info) noexcept -> TalkerStreamSrpInfo
    {
        info.state = TalkerReservationState::Advertising;
        return info;
    }

    /// Declare @p info's Talker Advertise attribute (rank non-emergency), starting the participant first.
    [[nodiscard]] auto declare_talker_attribute(TalkerStreamSrpInfo const& info, TimePoint now) -> Status
    {
        ensure_started(now);
        return participant_.declare_talker_advertise(talker_first_value(info), now);
    }

    [[nodiscard]] static auto talker_first_value(TalkerStreamSrpInfo const& info) noexcept
        -> statusbar::srp::msrp::TalkerAdvertiseFirstValue
    {  // NOLINT(statusbar-block-statements): the Talker Advertise FirstValue, field by field
        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fv.stream_id = info.stream_id;
        fv.destination_address = info.dest_address;
        fv.vlan_identifier = info.vlan_id;
        fv.max_frame_size = info.max_frame_size;
        fv.max_interval_frames = info.max_interval_frames;
        fv.set_priority(info.priority);
        fv.set_rank(statusbar::srp::msrp::RANK_NON_EMERGENCY);
        fv.accumulated_latency = info.accumulated_latency;
        return fv;
    }

    /// Declare @p stream_id's Listener @p decl, then attach one more input in @p state. A declaration
    /// the participant's full table refuses leaves the record as it was; a full record table
    /// withdraws a declaration made for a stream no record holds.
    [[nodiscard]] auto attach_declared(
        StreamId const& stream_id, ListenerReservationState state, statusbar::srp::msrp::ListenerDeclaration decl, TimePoint now)
        -> StatusValue<size_t>
    {
        ensure_started(now);
        if (auto const r = participant_.declare_listener(stream_id, decl, now); !r) {
            return failure(r.error());
        }
        auto const idx = attach_listener_record(stream_id, state);
        if (!idx) {
            (void)participant_.withdraw_listener(stream_id, now);
        }
        return idx;
    }

    /// The record for @p stream_id, made when there is none, put in @p state
    /// with one more input attached. All inputs of one stream share its
    /// state: Ready or Asking Failed is a fact about the talker's
    /// registration, not about the input. Fails when the table is full.
    [[nodiscard]] auto attach_listener_record(StreamId const& stream_id, ListenerReservationState state) -> StatusValue<size_t>
    {
        auto const idx = stream_slot(listener_streams_, stream_id, ListenerReservationState::Idle);
        if (!idx) {
            // Reject rather than throw when the reservation table is full (reactor path).
            return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
        }
        if (*idx == listener_streams_.size() || listener_streams_[*idx].stream_id != stream_id) {
            store_stream(listener_streams_, *idx, ListenerStreamSrpInfo{.stream_id = stream_id});
        }
        auto& record = listener_streams_[*idx];
        record.state = state;
        ++record.attached;
        return *idx;
    }

    /// One more input attached to @p stream_id in Waiting: counted, nothing declared (a
    /// declaration the record still had is withdrawn).
    [[nodiscard]] auto attach_waiting(StreamId const& stream_id, TimePoint now) -> Status
    {
        size_t const idx = listener_index(stream_id);
        bool const declared = idx < listener_streams_.size() && listener_is_declared(listener_streams_[idx].state);
        if (auto const r = attach_listener_record(stream_id, ListenerReservationState::Waiting); !r) {
            return failure(r.error());
        }
        if (declared) {
            (void)participant_.withdraw_listener(stream_id, now);
        }
        return success();
    }

    /// Whether a record in @p state has a Listener declaration on the wire.
    [[nodiscard]] static constexpr auto listener_is_declared(ListenerReservationState const state) noexcept -> bool
    {
        return state != ListenerReservationState::Idle && state != ListenerReservationState::Waiting;
    }

    void ensure_started(TimePoint now)
    {
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
    }

    /// What the applicants have pending, sent now through the send_packet in
    /// force.
    void transmit_now(TimePoint now) { participant_.transmit_pending(now); }

    /// The one way a PDU leaves: through the send_packet in force now.
    auto send(std::span<uint8_t const> const packet) -> bool
    {
        return callbacks_.send_packet ? callbacks_.send_packet(packet) : false;
    }

    /// Forward a listener register/leave for one of OUR talker streams to the
    /// on_talker_listener hook, reporting the aggregate "may transmit" state
    /// and the time of the receive_packet()/tick() this fires from.
    void notify_talker_listener(tsn::StreamId const& sid)
    {
        if (get_talker_stream(sid) != nullptr && callbacks_.on_talker_listener) {
            callbacks_.on_talker_listener(sid, participant_.listener_permits_transmit(sid), last_event_time_);
        }
    }

    /// A Domain registered from the wire. Under adoption, a Domain of our SR
    /// class whose priority or VLAN differs from ours becomes ours (see
    /// set_adopt_domain); anything else is noted and left alone.
    void on_remote_domain(statusbar::srp::msrp::DomainFirstValue const& fv, statusbar::srp::msrp::Operation const op)
    {
        bool const our_class = fv.sr_class_id.get() == domain_.sr_class_id;
        bool const same = fv.sr_class_priority.get() == domain_.sr_class_priority && fv.sr_class_vid.get() == domain_.sr_class_vid;
        if (!adopt_domain_ || op != statusbar::srp::msrp::Operation::Register || !our_class || same) {
            return;
        }
        if (withdrawn_) {
            return;  // withdrawn: nothing declares until the next run's declare_domain(), which hears it again
        }
        domain_ = DomainInfo{
            .sr_class_id = domain_.sr_class_id,
            .sr_class_priority = fv.sr_class_priority.get(),
            .sr_class_vid = fv.sr_class_vid.get()};
        (void)declare_domain(last_event_time_);  // the old values leave first (10.7)
        if (callbacks_.on_domain_adopted) {
            callbacks_.on_domain_adopted(domain_, last_event_time_);
        }
    }

    /// The remote talker's registration for @p sid changed (Advertise or
    /// Failed registered, or the talker left): re-derive the declaration of
    /// an attached listener stream — AskingFailed -> Ready when the
    /// Advertise appears, Ready -> AskingFailed when it goes away — and
    /// surface the change through on_listener_state_change, told the time
    /// of the receive_packet()/tick() this fires from (last_event_time_); for a listener stream's talker
    /// within an MRPDU, once it is read whole, so a Talker Advertise's Lv and the Talker Failed replacing it are one change.
    void on_remote_talker_changed(tsn::StreamId const& sid)
    {
        // At most MaxStreams listener streams, so the list never fills; a full one follows at once.
        bool const held = receiving_ && get_listener_stream(sid) != nullptr &&
            (std::ranges::contains(changed_talkers_, sid) || changed_talkers_.try_push_back(sid) != nullptr);
        if (!held) {
            (void)follow_changed_talker(sid);
        }
    }

    /// Follow each talker whose registration the MRPDU just read changed; a Listener declaration that
    /// changed goes now, after any re-declaration the MRPDU's LeaveAll made with the one before.
    void follow_changed_talkers()
    {
        bool declared = false;
        for (auto const& sid : changed_talkers_) {
            declared = follow_changed_talker(sid) || declared;
        }
        changed_talkers_.clear();
        if (declared) {
            participant_.transmit_pending(last_event_time_);
        }
    }

    /// Re-derive @p sid's attached listener, then tell on_remote_talker; true when its declaration changed.
    auto follow_changed_talker(tsn::StreamId const& sid) -> bool
    {
        bool const declared = follow_remote_talker(sid, last_event_time_);
        if (on_remote_talker_) {
            on_remote_talker_(sid, last_event_time_);  // the receive or tick this fires from
        }
        return declared;
    }

    /// The attached listener of @p sid re-derived from the talker's registration, declared or withdrawn at @p now;
    /// true when that changed its declaration.
    auto follow_remote_talker(tsn::StreamId const& sid, TimePoint now) -> bool
    {
        if (withdrawn_) {
            return false;  // between withdraw_all() and the next declare_domain(): nothing declares
        }
        for (auto& stream : listener_streams_) {
            if (stream.stream_id == sid) {
                return redeclare_attached_listener(stream, now);
            }
        }
        return false;
    }

    /// Re-derive one attached listener stream's declaration from the remote
    /// talker's registration and declare it at @p now when it changed, telling the
    /// on_listener_state_change hook that time; true when the declaration changed.
    /// A stream that is not attached (Idle/withdrawn) has nothing to re-declare.
    [[nodiscard]] static constexpr auto listener_is_attached(ListenerReservationState const state) noexcept -> bool
    {
        return state == ListenerReservationState::Ready || state == ListenerReservationState::AskingFailed ||
            state == ListenerReservationState::Waiting;
    }

    auto redeclare_attached_listener(ListenerStreamSrpInfo& stream, TimePoint now) -> bool
    {
        ListenerReservationState const want = attach_state(stream.stream_id, stream.talker);
        if (!listener_is_attached(stream.state) || stream.state == want) {
            return false;
        }
        stream.state = want;
        if (want == ListenerReservationState::Waiting) {
            (void)participant_.withdraw_listener(stream.stream_id, now);
        } else {
            (void)participant_.declare_listener(
                stream.stream_id,
                want == ListenerReservationState::Ready ? statusbar::srp::msrp::ListenerDeclaration::Ready
                                                        : statusbar::srp::msrp::ListenerDeclaration::AskingFailed,
                now);
        }
        if (callbacks_.on_listener_state_change) {
            callbacks_.on_listener_state_change(stream.stream_id, stream.state, now);
        }
        return true;
    }

    MsrpCallbacks callbacks_;
    DomainInfo domain_;
    bool adopt_domain_{false};  ///< Milan v1.3 4.2.7.2.1 Domain adoption (set_adopt_domain)
    statusbar::sg14::inplace_vector<TalkerStreamSrpInfo, MaxStreams> talker_streams_;
    statusbar::sg14::inplace_vector<ListenerStreamSrpInfo, MaxStreams> listener_streams_;
    statusbar::srp::msrp::MsrpParticipantT<Limits> participant_;
    TimePoint last_event_time_{};  ///< time of the current receive_packet()/tick() (for observer re-declares)
    statusbar::sg14::inplace_function<void(StreamId const&, TimePoint), 64> on_remote_talker_{};
    bool listener_waits_for_talker_{false};
    /// Talkers whose registration changed in the MRPDU receive_packet() is reading (follow_changed_talkers).
    statusbar::sg14::inplace_vector<tsn::StreamId, MaxStreams> changed_talkers_;
    bool receiving_{false};
    bool started_{false};
    bool withdrawn_{false};  ///< withdraw_all() ran and no declare_domain() has begun a run since
};

}  // namespace statusbar::nanoavb
