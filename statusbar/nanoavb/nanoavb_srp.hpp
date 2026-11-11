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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
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

/// Listener stream info for MSRP: one record per talker stream this port
/// listens to, however many of its inputs are bound to that stream.
struct ListenerStreamSrpInfo
{
    StreamId stream_id{};
    ListenerReservationState state = ListenerReservationState::Idle;
    /// Inputs attached to the stream. The Listener declaration stands on the
    /// wire while this is above zero and is withdrawn when the last goes.
    uint16_t attached{0};
};

/// Domain info for MSRP
struct DomainInfo
{
    uint8_t sr_class_id = 6;  // Class A
    uint8_t sr_class_priority = 3;
    uint16_t sr_class_vid = 2;
};

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
/// @tparam MaxStreams Compile-time capacity for talker/listener stream tracking
template <size_t MaxStreams = 32>
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

    [[nodiscard]] auto talker_advertise(TalkerStreamSrpInfo const& info, TimePoint now) -> StatusValue<size_t>
    {
        size_t idx = 0;
        bool found = false;
        for (size_t i = 0; i < talker_streams_.size(); ++i) {
            if (talker_streams_[i].stream_id == info.stream_id) {
                talker_streams_[i] = info;
                talker_streams_[i].state = TalkerReservationState::Advertising;
                idx = i;
                found = true;
                break;
            }
        }
        if (!found) {
            auto copy = info;
            copy.state = TalkerReservationState::Advertising;
            // inplace_vector::push_back throws when full; on the MSRP reactor path a
            // peer advertising more than MaxStreams must be rejected, not terminate.
            if (talker_streams_.try_push_back(copy) == nullptr) {
                return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
            }
            idx = talker_streams_.size() - 1;
        }

        statusbar::srp::msrp::TalkerAdvertiseFirstValue fv{};
        fv.stream_id = info.stream_id;
        fv.destination_address = info.dest_address;
        fv.vlan_identifier = info.vlan_id;
        fv.max_frame_size = info.max_frame_size;
        fv.max_interval_frames = info.max_interval_frames;
        fv.set_priority(info.priority);
        fv.set_rank(statusbar::srp::msrp::RANK_NON_EMERGENCY);
        fv.accumulated_latency = info.accumulated_latency;

        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        (void)participant_.declare_talker_advertise(fv, now);
        return idx;
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

    /// Declare this port's MSRP SR class domain(s) so the bridge can locate the
    /// SRP domain boundary (IEEE 802.1Q 35.2.1.4 / 35.2.2.9). Without an explicit
    /// declaration the bridge treats the port as a boundary for every SR class it
    /// supports, and converts inbound Talker Advertise declarations into Talker
    /// Failed with failure_code 8 ("Egress port is not AVB-capable") -- so a remote
    /// talker's stream can never reserve a path to us. We declare BOTH Class A
    /// (SRclassID 6, priority 3) and Class B (SRclassID 5, priority 2) on the
    /// configured VID, matching the default-case domain a bridge advertises
    /// (FirstValue {5,2,VID} with NumberOfValues=2). Idempotent; call on link-up.
    [[nodiscard]] auto declare_domain(TimePoint now) -> Status
    {
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        // Class B at its default priority 2; Class A at the domain's priority,
        // 3 unless set_domain() or an adoption said otherwise; both on its VLAN.
        declare_sr_class(statusbar::srp::msrp::SR_CLASS_B, 2, now);
        declare_sr_class(statusbar::srp::msrp::SR_CLASS_A, domain_.sr_class_priority, now);
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
        auto const idx = attach_listener_record(stream_id, ListenerReservationState::Ready);
        if (!idx) {
            return failure(idx.error());
        }
        ensure_started(now);
        (void)participant_.declare_listener(stream_id, statusbar::srp::msrp::ListenerDeclaration::Ready, now);
        return *idx;
    }

    /// One more input attached to @p stream_id, declared Asking Failed.
    [[nodiscard]] auto listener_asking_failed(StreamId const& stream_id, TimePoint now) -> Status
    {
        auto const idx = attach_listener_record(stream_id, ListenerReservationState::AskingFailed);
        if (!idx) {
            return failure(idx.error());
        }
        ensure_started(now);
        (void)participant_.declare_listener(stream_id, statusbar::srp::msrp::ListenerDeclaration::AskingFailed, now);
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
        listener_streams_[idx].state = ListenerReservationState::Idle;
        (void)participant_.withdraw_listener(stream_id, now);
        return success();
    }

    /// The end of a run: withdraw every talker, every listener and the
    /// domain this port declared, and send their Leaves now. A withdrawal
    /// alone only queues its Leave for the next join timer, which a
    /// stopping reactor never reaches, and the bridge would then keep this
    /// port's reservations until its own LeaveAll lapsed. Nothing declared:
    /// nothing sent.
    void withdraw_all(TimePoint now)
    {
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

    /// True when the talker's Advertise is registered from the wire and no
    /// Talker Failed has replaced it — the 802.1Q condition for a listener
    /// to declare Ready rather than Asking Failed.
    [[nodiscard]] auto talker_advertise_registered(StreamId const& stream_id) const noexcept -> bool
    {
        return participant_.find_talker_advertise(stream_id) != nullptr && participant_.find_talker_failed(stream_id) == nullptr;
    }

    /// Attach this listener to @p stream_id (kit phase 5d, IEEE 802.1Q
    /// 35.2.4.4): declare Listener Ready when the talker's Advertise is
    /// registered, Listener Asking Failed otherwise (the talker is not
    /// advertising yet, or registered Talker Failed). While attached, the
    /// declaration is upgraded/downgraded automatically as the talker's
    /// registration changes (see on_remote_talker_changed). Returns the
    /// state actually declared. Use instead of listener_ready() when the
    /// talker may not be up yet — e.g. an ACMP fast-connect at boot.
    [[nodiscard]] auto listener_attach(StreamId const& stream_id, TimePoint now) -> StatusValue<ListenerReservationState>
    {
        if (talker_advertise_registered(stream_id)) {
            if (auto const r = listener_ready(stream_id, now); !r) {
                return failure(r.error());
            }
            return success(ListenerReservationState::Ready);
        }
        if (auto const r = listener_asking_failed(stream_id, now); !r) {
            return failure(r.error());
        }
        return success(ListenerReservationState::AskingFailed);
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

    /// Every listener that is attached, withdrawn (withdraw_all), whatever
    /// its inputs: the run is over for all of them.
    void withdraw_listeners(TimePoint now)
    {
        for (auto& stream : listener_streams_) {
            if (stream.state != ListenerReservationState::Idle) {
                stream.state = ListenerReservationState::Idle;
                stream.attached = 0;
                (void)participant_.withdraw_listener(stream.stream_id, now);
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

    /// The record for @p stream_id, made when there is none, put in @p state
    /// with one more input attached. All inputs of one stream share its
    /// state: Ready or Asking Failed is a fact about the talker's
    /// registration, not about the input. Fails when the table is full.
    [[nodiscard]] auto attach_listener_record(StreamId const& stream_id, ListenerReservationState state) -> StatusValue<size_t>
    {
        size_t const idx = listener_index(stream_id);
        if (idx == listener_streams_.size() &&
            listener_streams_.try_push_back({.stream_id = stream_id, .state = state}) == nullptr) {
            // Reject rather than throw when the reservation table is full (reactor path).
            return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
        }
        auto& record = listener_streams_[idx];
        record.state = state;
        ++record.attached;
        return idx;
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

    /// The remote talker's registration for @p sid changed (Advertise or
    /// Failed registered, or the talker left): re-derive the declaration of
    /// an attached listener stream — AskingFailed -> Ready when the
    /// Advertise appears, Ready -> AskingFailed when it goes away — and
    /// surface the change through on_listener_state_change, told the time
    /// of the receive_packet()/tick() this fires from (last_event_time_).
    /// A Domain registered from the wire. Under adoption, a Class A Domain
    /// whose priority or VLAN differs from ours becomes ours (see
    /// set_adopt_domain); anything else is noted and left alone.
    void on_remote_domain(statusbar::srp::msrp::DomainFirstValue const& fv, statusbar::srp::msrp::Operation const op)
    {
        bool const class_a = fv.sr_class_id.get() == statusbar::srp::msrp::SR_CLASS_A;
        bool const same = fv.sr_class_priority.get() == domain_.sr_class_priority && fv.sr_class_vid.get() == domain_.sr_class_vid;
        if (!adopt_domain_ || op != statusbar::srp::msrp::Operation::Register || !class_a || same) {
            return;
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

    void on_remote_talker_changed(tsn::StreamId const& sid)
    {
        for (auto& stream : listener_streams_) {
            if (stream.stream_id == sid) {
                redeclare_attached_listener(stream);
                return;
            }
        }
    }

    /// Re-derive one attached listener stream's declaration from the remote
    /// talker's registration and declare it when it changed, telling the
    /// on_listener_state_change hook the event's time. A stream that is not
    /// attached (Idle/withdrawn) has nothing to re-declare.
    [[nodiscard]] static constexpr auto listener_is_attached(ListenerReservationState const state) noexcept -> bool
    {
        return state == ListenerReservationState::Ready || state == ListenerReservationState::AskingFailed;
    }

    void redeclare_attached_listener(ListenerStreamSrpInfo& stream)
    {
        bool const ready = talker_advertise_registered(stream.stream_id);
        ListenerReservationState const want = ready ? ListenerReservationState::Ready : ListenerReservationState::AskingFailed;
        if (!listener_is_attached(stream.state) || stream.state == want) {
            return;
        }
        stream.state = want;
        (void)participant_.declare_listener(
            stream.stream_id,
            ready ? statusbar::srp::msrp::ListenerDeclaration::Ready : statusbar::srp::msrp::ListenerDeclaration::AskingFailed,
            last_event_time_);
        if (callbacks_.on_listener_state_change) {
            callbacks_.on_listener_state_change(stream.stream_id, stream.state, last_event_time_);
        }
    }

    MsrpCallbacks callbacks_;
    DomainInfo domain_;
    bool adopt_domain_{false};  ///< Milan v1.3 4.2.7.2.1 Domain adoption (set_adopt_domain)
    statusbar::sg14::inplace_vector<TalkerStreamSrpInfo, MaxStreams> talker_streams_;
    statusbar::sg14::inplace_vector<ListenerStreamSrpInfo, MaxStreams> listener_streams_;
    statusbar::srp::msrp::MsrpParticipant participant_;
    TimePoint last_event_time_{};  ///< time of the current receive_packet()/tick() (for observer re-declares)
    bool started_{false};
};

}  // namespace statusbar::nanoavb
