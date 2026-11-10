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
    /// Its parameters are known and nothing is declared: no listener has
    /// asked for the stream (MsrpHandler::set_talker_waits_for_listener).
    Waiting,
};

/// Stream reservation state (for listener)
enum class ListenerReservationState : uint8_t
{
    Idle,
    AskingFailed,
    Ready,
    ReadyFailed,
    /// Attached with nothing declared: neither a Talker Advertise nor a
    /// Talker Failed is registered for the stream (MsrpHandler::listener_attach).
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

/// A remote Talker attribute this port registers for a stream: what a
/// Milan sink compares with the parameters it probed (Milan v1.3 5.3.8.8).
struct RegisteredTalker
{
    Eui48 dest_address{};
    uint16_t vlan_id{0};
    /// The attribute is a Talker Failed, not a Talker Advertise.
    bool failed{false};
    /// The attribute's AccumulatedLatency in nanoseconds (IEEE 802.1Q-2022 35.2.2.8.6).
    uint32_t accumulated_latency{0};
    /// A Talker Failed's FailureInformation (35.2.2.8.7); zero for a Talker Advertise.
    uint8_t failure_code{0};
    Eui64 failure_bridge_id{};
};

/// The one Talker attribute a listener stream follows: the destination
/// address and VLAN a Milan sink was given by its talker's PROBE_TX_RESPONSE
/// (Milan v1.3 5.3.8.8, 5.3.8.9). A listener stream without one follows any
/// Talker attribute of its StreamID, as IEEE 802.1Q has it.
struct TalkerMatch
{
    Eui48 dest_address{};
    /// 0: the answer named no VLAN. An IEEE 1722.1 talker sends 0 for "the
    /// VLAN of the SRP Domain attribute" (IEEE 1722.1-2021 8.2.1.17; a Milan
    /// talker names its VLAN, Table 5.48), and a Talker attribute on any
    /// VLAN is the one.
    uint16_t vlan_id{0};

    [[nodiscard]] constexpr auto admits(RegisteredTalker const& talker) const noexcept -> bool
    {
        return talker.dest_address == dest_address && (vlan_id == 0 || talker.vlan_id == vlan_id);
    }
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
    /// The only Talker attribute the stream follows, when its last attach
    /// named one (MsrpHandler::listener_attach); none: any of its StreamID.
    std::optional<TalkerMatch> follows{};
};

/// Domain info for MSRP: one SR class's SRclassID, SRclassPriority and SRclassVID (IEEE 802.1Q-2022 Clause 35.2.2.9).
struct DomainInfo
{
    uint8_t sr_class_id = 6;  // Class A
    uint8_t sr_class_priority = 3;
    uint16_t sr_class_vid = 2;

    [[nodiscard]] constexpr auto operator==(DomainInfo const&) const noexcept -> bool = default;
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
/// Milan's MRP timers (Milan v1.3 4.2.7.1.1, Table 4.3): joinTime 200 ms,
/// LeaveTime 5000 ms, a leavealltimer of 10 to 15 s and a 1000 ms
/// periodictimer. The long LeaveTime goes with the Registrar's instant leave
/// (4.2.7.2.2, set_instant_leave), which sees an explicit withdrawal at once.
inline constexpr statusbar::srp::mrp::TimerConfig MILAN_MRP_TIMERS{
    .join_time = std::chrono::milliseconds{200},
    .leave_time = std::chrono::milliseconds{5000},
    .leave_all_time_min = std::chrono::milliseconds{10000},
    .leave_all_time_max = std::chrono::milliseconds{15000},
    .periodic_time = std::chrono::milliseconds{1000},
};

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

    /// Install just the sender, leaving the other hooks as they are
    /// (set_callbacks replaces the whole struct; this updates one field).
    /// For the owner of the network path, which comes and goes with the
    /// run, while the hooks are the entity's and are installed once.
    void set_send_packet(decltype(MvrpCallbacks::send_packet) send) { callbacks_.send_packet = std::move(send); }

    /// The MRP timer durations of this port from here on (see
    /// MsrpHandler::set_mrp_timers). An invalid @p timers is ignored.
    void set_mrp_timers(statusbar::srp::mrp::TimerConfig const& timers) noexcept { (void)participant_.set_mrp_timers(timers); }

    /// The MRP timer durations in force.
    [[nodiscard]] auto mrp_timers() const noexcept -> statusbar::srp::mrp::TimerConfig const& { return participant_.mrp_timers(); }

    /// Forget the sender: nothing goes out until set_send_packet() or
    /// set_callbacks() installs another. For the owner of the network path
    /// to call before it takes that path away, so a Leave or re-declaration
    /// that falls due in between has nowhere stale to go. The other hooks stay.
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

    /// The VLANs this entity's settled Milan sinks use, all of them (Milan
    /// v1.3 4.4.1: a Listener declares a VID attribute for each). A VLAN
    /// that enters the list is declared and one that leaves it is withdrawn.
    /// register_vlan() and this hold a VLAN each on their own: it is declared
    /// while either holds it, so a sink that leaves the entity's own VLAN
    /// takes nothing away, and withdraw_vlan() leaves a sink's VLAN declared.
    /// An id outside 1 to 4094, a repeat and an id the table has no room for
    /// are left out of the list.
    void set_sink_vlans(std::span<uint16_t const> vlans, TimePoint now);

    /// The VLANs set_sink_vlans() last took.
    [[nodiscard]] auto sink_vlans() const noexcept -> std::span<uint16_t const> { return sink_vlans_; }

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

    /// Whether the participant is running: started (BEGIN!) by the first use
    /// of a run, stopped by withdraw_all() at its end.
    [[nodiscard]] auto started() const noexcept -> bool { return started_; }

    /// When this port's MRP timers next need tick(), on the clock tick() is
    /// told, as nanoseconds; nullopt while the participant is not running.
    /// The loop that drives the handler must not sleep past it: a jointimer
    /// served late sends its message late.
    [[nodiscard]] auto next_deadline_ns() const noexcept -> std::optional<int64_t>
    {
        auto const next = participant_.next_deadline();
        if (!started_ || next == TimePoint::max()) {
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count();
    }

  private:
    /// The one way a PDU leaves: through the send_packet in force now.
    auto send(std::span<uint8_t const> const packet) -> bool
    {
        return callbacks_.send_packet ? callbacks_.send_packet(packet) : false;
    }

    using VlanIds = statusbar::sg14::inplace_vector<uint16_t, statusbar::srp::mvrp::DefaultMvrpLimits::max_vlans>;

    /// Declare @p vlan_id, whoever holds it: its slot in vlans_ and the participant's Join.
    [[nodiscard]] auto declare(uint16_t vlan_id, TimePoint now) -> Status;
    /// Withdraw @p vlan_id, which nobody holds any more.
    auto undeclare(uint16_t vlan_id, TimePoint now) -> Status;
    /// @p vlan_id of a new sink list goes into @p next, declared when nobody held it.
    void take_sink_vlan(VlanIds& next, uint16_t vlan_id, TimePoint now);
    /// The end of a run: nobody holds a VLAN.
    void release_holds() noexcept
    {
        entity_vlans_.clear();
        sink_vlans_.clear();
    }

    MvrpCallbacks callbacks_;
    // Fixed-capacity (no heap): VLAN count per endpoint is tiny in practice.
    // Capacity matches the wrapped MvrpParticipant's compile-time limit.
    statusbar::sg14::inplace_vector<VlanInfo, statusbar::srp::mvrp::DefaultMvrpLimits::max_vlans> vlans_;
    /// The VLANs register_vlan() holds.
    VlanIds entity_vlans_{};
    /// The VLANs set_sink_vlans() holds.
    VlanIds sink_vlans_{};
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
    /// the remote talker's registration did (Waiting, Ready and AskingFailed,
    /// see on_remote_talker_changed). Fires from receive_packet()/tick(), told that
    /// call's time: a Milan sink's re-probe timer starts from the event's
    /// instant, not from whatever the consumer has lying around.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, ListenerReservationState state, TimePoint now), 64>
        on_listener_state_change;

    /// Called when a remote Talker attribute (Advertise or Failed) for
    /// @p stream_id was registered, refreshed, changed or withdrawn, whether
    /// or not a listener here is attached to the stream. Fires from
    /// receive_packet()/tick(), told that call's time, after the handler has
    /// brought its own Listener declaration up to date: registered_talker()
    /// inside the callback is what stands now.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, TimePoint now), 64> on_remote_talker;

    /// Called when the set of remote listeners for one of OUR advertised talker
    /// streams changes. `ready` is true when at least one registered listener
    /// permits transmit on `stream_id` (the talker may start sending), false
    /// otherwise. Fires from receive_packet()/tick() on listener register/leave,
    /// told that call's time: the talker drives its own SM from it.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, bool ready, TimePoint now), 64> on_talker_listener;

    /// Called when the Class A Domain in force changed: adopted from the
    /// wire (Milan v1.3 4.2.7.2.1, see MsrpHandler::set_adopt_domain), told
    /// the time of the receive_packet() that brought the peer's; or back to
    /// the configured one (MsrpHandler::return_to_configured_domain), told
    /// the time of the startup or Link Up. The Domain now declared; the
    /// entity takes its VLAN and priority to its streams.
    statusbar::sg14::inplace_function<void(DomainInfo const& domain, TimePoint now), 64> on_domain_adopted;
};

//
// MSRP Handler
//

/// MsrpHandler - MSRP stream reservation. @tparam MaxStreams the talker and listener streams it
/// tracks (a withdrawn, Idle, entry is reused by the next new StreamID); @tparam Limits the MSRP
/// participant's table capacities
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

    /// Install just the sender, leaving the other hooks as they are
    /// (set_callbacks replaces the whole struct; this updates one field).
    /// For the owner of the network path, which comes and goes with the
    /// run, while the hooks (on_domain_adopted, on_talker_listener, ...)
    /// are the entity's and are installed once, before the first start.
    void set_send_packet(decltype(MsrpCallbacks::send_packet) send) { callbacks_.send_packet = std::move(send); }

    /// Forget the sender: nothing goes out until set_send_packet() or
    /// set_callbacks() installs another. For the owner of the network path
    /// to call before it takes that path away, so a Leave or re-declaration
    /// that falls due in between has nowhere stale to go. The other hooks stay.
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

    /// Set just the on_remote_talker hook without disturbing the others.
    void set_on_remote_talker(statusbar::sg14::inplace_function<void(StreamId const&, TimePoint), 64> cb)
    {
        callbacks_.on_remote_talker = std::move(cb);
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

    /// Called when a peer's Talker Advertise or Talker Failed for @p stream_id is registered or
    /// leaves, after an attached listener's declaration has followed it.
    void set_on_remote_talker(statusbar::sg14::inplace_function<void(StreamId const&, TimePoint), 64> cb)
    {
        on_remote_talker_ = std::move(cb);
    }

    /// The MRP timer durations in force.
    [[nodiscard]] auto mrp_timers() const noexcept -> statusbar::srp::mrp::TimerConfig const& { return participant_.mrp_timers(); }

    /// Diagnostic breakdown of why the talker gate is open/closed for a stream
    /// (record present? operation? registrar In? Ready substate?), for logging
    /// the listener-ready flap. See MsrpParticipant::listener_permit_debug.
    [[nodiscard]] auto listener_permit_debug(StreamId const& stream_id) const noexcept
    {
        return participant_.listener_permit_debug(stream_id);
    }
    /// The configured Domain: declared until an adoption replaces it, and
    /// declared again after return_to_configured_domain().
    void set_domain(DomainInfo const& domain) noexcept
    {
        configured_domain_ = domain;
        domain_ = domain;
    }
    /// The Domain in force: the configured one, or the one adopted from the wire.
    [[nodiscard]] auto domain() const noexcept -> DomainInfo const& { return domain_; }

    /// Milan v1.3 4.2.7.2.1: "After startup or a Link Up event, a PAAD shall
    /// use SR Class Priority 3 and Default VLAN ID 2 for Class A" (here: the
    /// Domain set_domain() gave) "and send MSRPDUs declaring an MSRP Domain
    /// for Class A using these parameters". The peers' registered Domains are
    /// forgotten, so the next Class A Domain received is adopted afresh rather
    /// than taken for one already registered. When the adopted Domain differed,
    /// the configured one is declared again (the adopted value leaves first,
    /// 802.1Q 10.7) and on_domain_adopted hears it; returns whether it did.
    auto return_to_configured_domain(TimePoint const now) -> bool
    {
        participant_.forget_registered_domains();
        if (domain_ == configured_domain_) {
            return false;
        }
        take_domain(configured_domain_, now);
        return true;
    }

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

    /// The parameters this station declared for a talker stream before it
    /// restarted (an entity keeps them in its state directory), told before
    /// the stream is handed to talker_advertise(). When the stream now has
    /// other parameters under the same StreamID, its Talker Advertise is held
    /// back for two LeaveAll periods from @p now (IEEE 802.1Q 35.2.2.8), as it
    /// is after a change within one run; with the same parameters nothing
    /// waits. AccumulatedLatency is no part of the comparison. Nothing is
    /// declared here.
    [[nodiscard]] auto talker_hold_after_restart(TalkerStreamSrpInfo const& previous, TimePoint const now) -> Status
    {
        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fill_talker_frames_and_tspec(previous, fv);
        fill_talker_priority_and_latency(previous, fv);
        return participant_.hold_talker_value_after_restart(fv, now);
    }

    /// Whether @p stream_id's Talker Advertise is held back: handed to
    /// talker_advertise() with parameters that have to wait out the value
    /// they replace. A stream so held has no Talker attribute of ours on the
    /// wire and is not to be sent.
    [[nodiscard]] auto talker_held(StreamId const& stream_id) const noexcept -> bool
    {
        return participant_.talker_change_pending(stream_id);
    }

    /// Whether a wait is running for @p stream_id's Talker Advertise, a value
    /// waiting or not (MsrpParticipant::talker_hold_running()). While it is,
    /// what the bridges last had registered for the StreamID is the value
    /// from before: an entity that keeps its declared values for the next
    /// start keeps that one until the wait is over.
    [[nodiscard]] auto talker_hold_running(StreamId const& stream_id) const noexcept -> bool
    {
        return participant_.talker_hold_running(stream_id);
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
        // Idle, and following nothing: the next attach says what it follows.
        listener_streams_[idx] = ListenerStreamSrpInfo{.stream_id = stream_id};
        (void)participant_.withdraw_listener(stream_id, now);
        return success();
    }

    /// The end of a run: withdraw every talker, every listener and the
    /// domain this port declared, and send their Leaves now. A withdrawal
    /// alone only queues its Leave for the next join timer, which a
    /// stopping reactor never reaches, and the bridge would then keep this
    /// port's reservations until its own LeaveAll lapsed. Nothing declared:
    /// nothing sent. The participant then stops; the next use of this
    /// handler starts it again (BEGIN!, IEEE 802.1Q-2022 10.7.5.1), as the
    /// first did. A listener attached Waiting stays attached, for the next
    /// run's declare_domain().
    void withdraw_all(TimePoint now)
    {
        withdrawn_ = true;
        if (!started_) {
            return;
        }
        withdraw_talkers(now);
        withdraw_listeners(now);
        withdraw_domains_and_send(now);
        stop_participant();
    }

    /// Whether the participant is running: started (BEGIN!) by the first use
    /// of a run, stopped by withdraw_all() at its end.
    [[nodiscard]] auto started() const noexcept -> bool { return started_; }

    /// When this port's MRP timers next need tick(), on the clock tick() is
    /// told, as nanoseconds; nullopt while the participant is not running.
    /// The loop that drives the handler must not sleep past it: a jointimer
    /// served late sends its message late.
    [[nodiscard]] auto next_deadline_ns() const noexcept -> std::optional<int64_t>
    {
        // The MRP timers, or sooner the moment a probe stops holding a
        // stream's Talker attribute up (set_talker_waits_for_listener).
        auto const next = std::min(participant_.next_deadline(), next_talker_probe_expiry());
        if (!started_ || next == TimePoint::max()) {
            return std::nullopt;
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(next.time_since_epoch()).count();
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

    /// Subscribe @p observer to the participant's registrations beside this handler's own; it is
    /// called after the handler has followed each one. See MsrpParticipant::subscribe.
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

    /// True when the talker's Advertise is registered from the wire and no
    /// Talker Failed has replaced it — the 802.1Q condition for a listener
    /// to declare Ready.
    [[nodiscard]] auto talker_advertise_registered(StreamId const& stream_id) const noexcept -> bool
    {
        return listener_state_wanted(stream_id) == ListenerReservationState::Ready;
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
    [[nodiscard]] auto listener_attach(StreamId const& stream_id, TimePoint now) -> StatusValue<ListenerReservationState>
    {
        if (talker_advertise_registered(stream_id)) {
            if (auto const r = listener_ready(stream_id, now); !r) {
                return failure(r.error());
            }
            return success(ListenerReservationState::Ready);
        }
        if (listener_waits_for_talker_ && peer_talker_failed(stream_id) == nullptr) {
            if (auto const r = attach_waiting(stream_id, now); !r) {
                return failure(r.error());
            }
            return success(ListenerReservationState::Waiting);
        }
        if (auto const r = listener_asking_failed(stream_id, now); !r) {
            return failure(r.error());
        }
        return success(ListenerReservationState::AskingFailed);
    }

    /// An attached stream follows @p only from @p now on, with no input
    /// attached or let go: what an input already on the stream calls when it
    /// is given another Talker attribute to follow, as a Milan sink that
    /// settles again on another destination address or VLAN is (Milan v1.3
    /// 5.3.8.5: Listener Ready goes with a Talker Advertise "with matching
    /// SRP parameters"). The declaration is re-derived at once from what is
    /// registered: a Ready held for the attribute left behind is withdrawn
    /// unless the new one is registered too, and it is declared again when
    /// that one arrives. Returns the state held, as listener_attach() does,
    /// and calls no on_listener_state_change hook; Idle, and nothing done,
    /// for a stream no input is attached to.
    [[nodiscard]] auto listener_follow(StreamId const& stream_id, std::optional<TalkerMatch> const& only, TimePoint const now)
        -> ListenerReservationState
    {
        size_t const idx = listener_index(stream_id);
        if (idx == listener_streams_.size() || !listener_is_attached(listener_streams_[idx].state)) {
            return ListenerReservationState::Idle;
        }
        auto& stream = listener_streams_[idx];
        stream.follows = only;
        if (auto const want = listener_state_wanted(stream_id, only); want != stream.state) {
            last_event_time_ = now;
            move_attached_listener(stream, want, now);
        }
        return stream.state;
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
        participant_.receive_pdu(packet, now);
    }

    auto tick(TimePoint now) -> void
    {
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        last_event_time_ = now;
        participant_.tick(now);
        follow_talker_demand(now);
    }

    [[nodiscard]] auto talker_streams() const noexcept -> std::span<TalkerStreamSrpInfo const> { return talker_streams_; }
    [[nodiscard]] auto listener_streams() const noexcept -> std::span<ListenerStreamSrpInfo const> { return listener_streams_; }

  private:
    /// A PROBE_TX_COMMAND remembered for a talker stream (note_talker_probe).
    struct TalkerProbe
    {
        StreamId stream_id{};
        TimePoint at{};
    };

    /// Every talker that is advertising or waiting, withdrawn (withdraw_all).
    /// The probes go with them: the run they asked in is over.
    void withdraw_talkers(TimePoint now)
    {
        for (auto& stream : talker_streams_) {
            if (stream.state != TalkerReservationState::Idle) {
                stream.state = TalkerReservationState::Idle;
                (void)participant_.withdraw_talker(stream.stream_id, now);
            }
        }
        talker_probes_.clear();
    }

    /// @p info into the talker table, its state kept (Idle for a stream new
    /// to it). The table's index, or a failure when it is full: a model with
    /// more talker streams than MaxStreams is refused, not a crash.
    [[nodiscard]] auto remember_talker(TalkerStreamSrpInfo const& info) -> StatusValue<size_t>
    {
        for (size_t i = 0; i < talker_streams_.size(); ++i) {
            if (talker_streams_[i].stream_id == info.stream_id) {
                auto const state = talker_streams_[i].state;
                talker_streams_[i] = info;
                talker_streams_[i].state = state;
                return i;
            }
        }
        if (talker_streams_.try_push_back(info) == nullptr) {
            return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
        }
        talker_streams_.back().state = TalkerReservationState::Idle;
        return talker_streams_.size() - 1;
    }

    /// Declare @p stream's Talker Advertise with the parameters it holds.
    void declare_talker(TalkerStreamSrpInfo& stream, TimePoint const now)
    {
        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fill_talker_frames_and_tspec(stream, fv);
        fill_talker_priority_and_latency(stream, fv);
        stream.state = TalkerReservationState::Advertising;
        (void)participant_.declare_talker_advertise(fv, now);
    }

    static void fill_talker_frames_and_tspec(
        TalkerStreamSrpInfo const& stream, statusbar::srp::msrp::TalkerAdvertiseFirstValue& fv) noexcept
    {
        fv.stream_id = stream.stream_id;
        fv.destination_address = stream.dest_address;
        fv.vlan_identifier = stream.vlan_id;
        fv.max_frame_size = stream.max_frame_size;
        fv.max_interval_frames = stream.max_interval_frames;
    }

    static void fill_talker_priority_and_latency(
        TalkerStreamSrpInfo const& stream, statusbar::srp::msrp::TalkerAdvertiseFirstValue& fv) noexcept
    {
        fv.set_priority(stream.priority);
        fv.set_rank(statusbar::srp::msrp::RANK_NON_EMERGENCY);
        fv.accumulated_latency = stream.accumulated_latency;
    }

    /// @p stream waits for a listener: what it declared is withdrawn.
    void stand_talker_down(TalkerStreamSrpInfo& stream, TimePoint const now)
    {
        if (stream.state == TalkerReservationState::Advertising) {
            (void)participant_.withdraw_talker(stream.stream_id, now);
        }
        stream.state = TalkerReservationState::Waiting;
    }

    /// Whether @p stream_id's Talker attribute is to be declared now. Always,
    /// where the handler does not wait for a listener. Where it does, Milan
    /// v1.3 4.3.3.1: the addresses are valid (condition 1), and a probe came
    /// within TALKER_PROBE_VALIDITY or a Listener attribute for the stream
    /// is registered on this port (condition 2).
    [[nodiscard]] auto talker_wanted(StreamId const& stream_id, TimePoint const now) const noexcept -> bool
    {
        if (!talker_waits_for_listener_) {
            return true;
        }
        return talker_addresses_valid_ && (talker_probed_recently(stream_id, now) || participant_.listener_registered(stream_id));
    }

    [[nodiscard]] auto talker_probed_recently(StreamId const& stream_id, TimePoint const now) const noexcept -> bool
    {
        for (auto const& probe : talker_probes_) {
            if (probe.stream_id == stream_id && now - probe.at < TALKER_PROBE_VALIDITY) {
                return true;
            }
        }
        return false;
    }

    /// The probe for @p stream_id at @p now, over any earlier one for the stream.
    void remember_talker_probe(StreamId const& stream_id, TimePoint const now)
    {
        if (auto* slot = talker_probe_slot(stream_id); slot != nullptr) {
            *slot = TalkerProbe{.stream_id = stream_id, .at = now};
        }
    }

    /// The entry a probe for @p stream_id goes into: the stream's own, else
    /// a new one, else (the table full of other streams' probes) the oldest.
    [[nodiscard]] auto talker_probe_slot(StreamId const& stream_id) noexcept -> TalkerProbe*
    {
        TalkerProbe* oldest = nullptr;
        for (auto& probe : talker_probes_) {
            if (probe.stream_id == stream_id) {
                return &probe;
            }
            oldest = (oldest == nullptr || probe.at < oldest->at) ? &probe : oldest;
        }
        auto* fresh = talker_probes_.try_push_back(TalkerProbe{});
        return fresh != nullptr ? fresh : oldest;
    }

    /// Declare the waiting streams a listener now wants and withdraw the
    /// declared ones no listener wants any more, then forget the probes
    /// that have run out. Nothing unless set_talker_waits_for_listener().
    void follow_talker_demand(TimePoint const now)
    {
        if (!talker_waits_for_listener_) {
            return;
        }
        for (auto& stream : talker_streams_) {
            follow_talker_demand_for(stream, now);
        }
        auto const run_out = [now](TalkerProbe const& probe) { return now - probe.at >= TALKER_PROBE_VALIDITY; };
        talker_probes_.erase(std::remove_if(talker_probes_.begin(), talker_probes_.end(), run_out), talker_probes_.end());
    }

    void follow_talker_demand_for(TalkerStreamSrpInfo& stream, TimePoint const now)
    {
        bool const wanted = talker_wanted(stream.stream_id, now);
        if (stream.state == TalkerReservationState::Waiting && wanted) {
            declare_talker(stream, now);
        } else if (stream.state == TalkerReservationState::Advertising && !wanted) {
            stand_talker_down(stream, now);
        }
    }

    /// When the earliest remembered probe stops counting: follow_talker_demand()
    /// must run then, to withdraw what only that probe held up. Never in the
    /// past of the last tick(), which drops the probes that have run out.
    [[nodiscard]] auto next_talker_probe_expiry() const noexcept -> TimePoint
    {
        auto next = TimePoint::max();
        for (auto const& probe : talker_probes_) {
            next = std::min(next, probe.at + TALKER_PROBE_VALIDITY);
        }
        return next;
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
                follow_remote_talker(listener_streams_[i].stream_id, now);
            }
        }
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

    /// The Domains this port declared, withdrawn, and every Leave pending
    /// sent now (withdraw_all).
    void withdraw_domains_and_send(TimePoint now)
    {
        (void)participant_.withdraw_domain(statusbar::srp::msrp::SR_CLASS_A, now);
        (void)participant_.withdraw_domain(statusbar::srp::msrp::SR_CLASS_B, now);
        transmit_now(now);
    }

    /// The end of the run for the participant: its timers stopped, and the
    /// next receive_packet()/tick()/declaration starts it again from BEGIN!.
    void stop_participant() noexcept
    {
        participant_.stop();
        started_ = false;
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

    /// Whether a Domain from the wire names something a stream could move to.
    /// IEEE 802.1Q-2022 Table 9-2 reserves VID 0 (priority-tagged, no VLAN)
    /// and VID 4095, and a priority is three bits (Table 9-3 names 0 to 7):
    /// a Domain carrying either is a peer's mistake, not a VLAN to join.
    [[nodiscard]] static constexpr auto adoptable(statusbar::srp::msrp::DomainFirstValue const& fv) noexcept -> bool
    {
        uint16_t const vid = fv.sr_class_vid.get();
        return vid >= 1 && vid <= 4094 && fv.sr_class_priority.get() <= 7;
    }

    /// A Domain registered from the wire. Under adoption, a Domain of our SR
    /// class whose priority or VLAN differs from ours becomes ours (see
    /// set_adopt_domain); anything else - another class, the same values, a
    /// VID or priority outside what 802.1Q allows - is noted and left alone,
    /// the Domain in force kept.
    void on_remote_domain(statusbar::srp::msrp::DomainFirstValue const& fv, statusbar::srp::msrp::Operation const op)
    {
        bool const our_class = fv.sr_class_id.get() == domain_.sr_class_id;
        bool const same = fv.sr_class_priority.get() == domain_.sr_class_priority && fv.sr_class_vid.get() == domain_.sr_class_vid;
        if (!adopt_domain_ || op != statusbar::srp::msrp::Operation::Register || !our_class || same || !adoptable(fv)) {
            return;
        }
        if (withdrawn_) {
            return;  // withdrawn: nothing declares until the next run's declare_domain(), which hears it again
        }
        take_domain(
            DomainInfo{
                .sr_class_id = domain_.sr_class_id,
                .sr_class_priority = fv.sr_class_priority.get(),
                .sr_class_vid = fv.sr_class_vid.get()},
            last_event_time_);
    }

    /// @p domain is the Class A Domain in force from @p now: declared (the
    /// value it replaces leaves first, 802.1Q 10.7) and reported to
    /// on_domain_adopted, told @p now.
    void take_domain(DomainInfo const& domain, TimePoint const now)
    {
        domain_ = domain;
        last_event_time_ = now;
        (void)declare_domain(now);
        if (callbacks_.on_domain_adopted) {
            callbacks_.on_domain_adopted(domain_, now);
        }
    }

    void on_remote_talker_changed(tsn::StreamId const& sid)
    {
        follow_remote_talker(sid, last_event_time_);
        if (on_remote_talker_) {
            on_remote_talker_(sid, last_event_time_);  // the receive or tick this fires from
        }
    }

    /// The attached listener of @p sid re-derived from the talker's registration, declared or withdrawn at @p now.
    void follow_remote_talker(tsn::StreamId const& sid, TimePoint now)
    {
        if (withdrawn_) {
            return;  // between withdraw_all() and the next declare_domain(): nothing declares
        }
        for (auto& stream : listener_streams_) {
            if (stream.stream_id == sid) {
                redeclare_attached_listener(stream, now);
                return;
            }
        }
        if (callbacks_.on_remote_talker) {
            callbacks_.on_remote_talker(sid, last_event_time_);
        }
    }

    /// Whether a listener that follows @p only (any attribute, with none) counts @p talker.
    [[nodiscard]] static constexpr auto admitted(std::optional<TalkerMatch> const& only, RegisteredTalker const& talker) noexcept
        -> bool
    {
        return !only.has_value() || only->admits(talker);
    }

    [[nodiscard]] static auto registered_talker_from(
        statusbar::srp::msrp::TalkerAdvertiseFirstValue const& value, bool const failed) noexcept -> RegisteredTalker
    {
        return RegisteredTalker{
            .dest_address = value.destination_address,
            .vlan_id = value.vlan_identifier.get(),
            .failed = failed,
            .accumulated_latency = value.accumulated_latency.get()};
    }

    [[nodiscard]] static auto registered_failed_talker_from(statusbar::srp::msrp::TalkerFailedFirstValue const& value) noexcept
        -> RegisteredTalker
    {
        auto talker = registered_talker_from(value.advertise, true);
        talker.failure_code = value.failure_code.get();
        talker.failure_bridge_id = value.failure_bridge_id;
        return talker;
    }

    /// A talker stream whose attribute is on the wire.
    [[nodiscard]] static constexpr auto talker_is_declared(TalkerReservationState const state) noexcept -> bool
    {
        return state == TalkerReservationState::Advertising || state == TalkerReservationState::Ready ||
            state == TalkerReservationState::Failed;
    }

    /// Re-derive one attached listener stream's declaration from the remote
    /// talker's registration and declare it at @p now when it changed, telling the
    /// on_listener_state_change hook that time. A stream that is not
    /// attached (Idle/withdrawn) has nothing to re-declare.
    [[nodiscard]] static constexpr auto listener_is_attached(ListenerReservationState const state) noexcept -> bool
    {
        return state == ListenerReservationState::Ready || state == ListenerReservationState::AskingFailed ||
            state == ListenerReservationState::Waiting;
    }

    /// Attached stream @p stream to @p want at @p now: its Listener withdrawn while it waits, else
    /// declared Ready or Asking Failed.
    void move_attached_listener(ListenerStreamSrpInfo& stream, ListenerReservationState const want, TimePoint const now)
    {
        stream.state = want;
        if (!listener_is_declared(want)) {
            (void)participant_.withdraw_listener(stream.stream_id, now);
            return;
        }
        bool const ready = want == ListenerReservationState::Ready;
        (void)participant_.declare_listener(
            stream.stream_id,
            ready ? statusbar::srp::msrp::ListenerDeclaration::Ready : statusbar::srp::msrp::ListenerDeclaration::AskingFailed,
            now);
    }

    /// Re-derive one attached listener stream's declaration from the remote
    /// talker's registration and declare it at @p now when it changed, telling the
    /// on_listener_state_change hook that time. A stream that is not
    /// attached (Idle/withdrawn) has nothing to re-declare.
    void redeclare_attached_listener(ListenerStreamSrpInfo& stream, TimePoint now)
    {
        stream.state = want;
        if (!listener_is_declared(want)) {
            (void)participant_.withdraw_listener(stream.stream_id, last_event_time_);
            return;
        }
        bool const ready = want == ListenerReservationState::Ready;
        (void)participant_.declare_listener(
            stream.stream_id,
            ready ? statusbar::srp::msrp::ListenerDeclaration::Ready : statusbar::srp::msrp::ListenerDeclaration::AskingFailed,
            last_event_time_);
    }

    /// Re-derive one attached listener stream's state from the remote
    /// talker's registration and act on it when it changed, telling the
    /// on_listener_state_change hook the event's time. A stream that is not
    /// attached (Idle/withdrawn) has nothing to follow.
    void redeclare_attached_listener(ListenerStreamSrpInfo& stream)
    {
        ListenerReservationState const want = listener_state_wanted(stream.stream_id, stream.follows);
        if (!listener_is_attached(stream.state) || stream.state == want) {
            return;
        }
        move_attached_listener(stream, want, now);
        if (callbacks_.on_listener_state_change) {
            callbacks_.on_listener_state_change(stream.stream_id, stream.state, now);
        }
    }

    MsrpCallbacks callbacks_;
    DomainInfo domain_;             ///< the Domain in force (configured, or adopted from the wire)
    DomainInfo configured_domain_;  ///< what set_domain() gave; return_to_configured_domain() goes back to it
    bool adopt_domain_{false};      ///< Milan v1.3 4.2.7.2.1 Domain adoption (set_adopt_domain)
    statusbar::sg14::inplace_vector<TalkerStreamSrpInfo, MaxStreams> talker_streams_;
    statusbar::sg14::inplace_vector<TalkerProbe, MaxStreams> talker_probes_;
    statusbar::sg14::inplace_vector<ListenerStreamSrpInfo, MaxStreams> listener_streams_;
    statusbar::srp::msrp::MsrpParticipantT<Limits> participant_;
    TimePoint last_event_time_{};  ///< time of the current receive_packet()/tick() (for observer re-declares)
    statusbar::sg14::inplace_function<void(StreamId const&, TimePoint), 64> on_remote_talker_{};
    bool listener_waits_for_talker_{false};
    bool started_{false};
    bool withdrawn_{false};  ///< withdraw_all() ran and no declare_domain() has begun a run since
};

}  // namespace statusbar::nanoavb
