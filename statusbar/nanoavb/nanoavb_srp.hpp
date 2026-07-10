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

/// Listener stream info for MSRP
struct ListenerStreamSrpInfo
{
    StreamId stream_id{};
    ListenerReservationState state = ListenerReservationState::Idle;
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
    }

    /// Get the MVRP EtherType
    [[nodiscard]] static constexpr auto ethertype() noexcept -> uint16_t { return srp::MVRP_ETHERTYPE; }

    /// Get the MVRP multicast address
    [[nodiscard]] static constexpr auto multicast_address() noexcept -> uint64_t { return srp::MVRP_MULTICAST_ADDRESS; }

    /// Set or update callbacks after construction
    /// This allows wiring up network handlers after both components are created
    /// @param callbacks The new callback interface
    void set_callbacks(MvrpCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Register a VLAN
    /// @param vlan_id The VLAN ID to register (1-4094)
    /// @return Error if VLAN ID is invalid
    [[nodiscard]] auto register_vlan(uint16_t vlan_id, TimePoint now) -> Status;

    /// Withdraw a VLAN registration
    /// @param vlan_id The VLAN ID to withdraw
    [[nodiscard]] auto withdraw_vlan(uint16_t vlan_id, TimePoint now) -> Status;

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

    /// Called when talker stream reservation state changes
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, TalkerReservationState state), 64> on_talker_state_change;

    /// Called when listener stream reservation state changes
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, ListenerReservationState state), 64> on_listener_state_change;

    /// Called when the set of remote listeners for one of OUR advertised talker
    /// streams changes. `ready` is true when at least one registered listener
    /// permits transmit on `stream_id` (the talker may start sending), false
    /// otherwise. Fires from receive_packet()/tick() on listener register/leave.
    statusbar::sg14::inplace_function<void(StreamId const& stream_id, bool ready), 64> on_talker_listener;
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
        // Observe remote listener declarations so we can tell a talker when a
        // listener becomes ready (or stops being ready) for one of its streams.
        // Safe: MsrpHandler lives in the non-movable NanoAvbComponents, so
        // `this` is stable for the lifetime of the captured observer.
        (void)participant_.subscribe(statusbar::srp::msrp::Observer{
            .on_listener = [this](
                               tsn::StreamId const& sid,
                               statusbar::srp::msrp::ListenerDeclaration /*decl*/,
                               statusbar::srp::msrp::Operation /*op*/) { notify_talker_listener(sid); },
            .on_listener_leave = [this](tsn::StreamId const& sid) { notify_talker_listener(sid); },
        });
    }

    [[nodiscard]] static constexpr auto ethertype() noexcept -> uint16_t { return srp::MSRP_ETHERTYPE; }
    [[nodiscard]] static constexpr auto multicast_address() noexcept -> uint64_t { return srp::MSRP_MULTICAST_ADDRESS; }

    void set_callbacks(MsrpCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Forward a reactor-thread logger to the MSRP participant (Debug-level
    /// MRP event diagnostics).
    void set_logger(logging::Logger const log) noexcept { participant_.set_logger(log); }

    /// Set just the on_talker_listener hook without disturbing the others
    /// (set_callbacks replaces the whole struct; this updates one field).
    void set_on_talker_listener(statusbar::sg14::inplace_function<void(StreamId const&, bool), 64> cb)
    {
        callbacks_.on_talker_listener = std::move(cb);
    }

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
        statusbar::srp::msrp::DomainFirstValue class_b{};
        class_b.sr_class_id = statusbar::srp::msrp::SR_CLASS_B;  // 5
        class_b.sr_class_priority = 2;
        class_b.sr_class_vid = domain_.sr_class_vid;
        (void)participant_.declare_domain(class_b, now);

        statusbar::srp::msrp::DomainFirstValue class_a{};
        class_a.sr_class_id = statusbar::srp::msrp::SR_CLASS_A;                       // 6
        class_a.sr_class_priority = statusbar::srp::msrp::default_sr_class_priority;  // 3
        class_a.sr_class_vid = domain_.sr_class_vid;
        (void)participant_.declare_domain(class_a, now);
        return success();
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

    [[nodiscard]] auto listener_ready(StreamId const& stream_id, TimePoint now) -> StatusValue<size_t>
    {
        size_t idx = 0;
        bool found = false;
        for (size_t i = 0; i < listener_streams_.size(); ++i) {
            if (listener_streams_[i].stream_id == stream_id) {
                listener_streams_[i].state = ListenerReservationState::Ready;
                idx = i;
                found = true;
                break;
            }
        }
        if (!found) {
            // Reject rather than throw when the reservation table is full (reactor path).
            if (listener_streams_.try_push_back({.stream_id = stream_id, .state = ListenerReservationState::Ready}) == nullptr) {
                return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
            }
            idx = listener_streams_.size() - 1;
        }

        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        (void)participant_.declare_listener(stream_id, statusbar::srp::msrp::ListenerDeclaration::Ready, now);
        return idx;
    }

    [[nodiscard]] auto listener_asking_failed(StreamId const& stream_id, TimePoint now) -> Status
    {
        bool found = false;
        for (auto& stream : listener_streams_) {
            if (stream.stream_id == stream_id) {
                stream.state = ListenerReservationState::AskingFailed;
                found = true;
                break;
            }
        }
        if (!found) {
            // Reject rather than throw when the reservation table is full (reactor path).
            if (listener_streams_.try_push_back({.stream_id = stream_id, .state = ListenerReservationState::AskingFailed}) ==
                nullptr) {
                return failure(make_error_code(NanoAvbError::SrpRegistrationFailed));
            }
        }

        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        (void)participant_.declare_listener(stream_id, statusbar::srp::msrp::ListenerDeclaration::AskingFailed, now);
        return success();
    }

    [[nodiscard]] auto listener_withdraw(StreamId const& stream_id, TimePoint now) -> Status
    {
        for (auto& stream : listener_streams_) {
            if (stream.stream_id == stream_id) {
                stream.state = ListenerReservationState::Idle;
                (void)participant_.withdraw_listener(stream_id, now);
                return success();
            }
        }
        return success();
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

    auto receive_packet(std::span<uint8_t const> packet, TimePoint now) -> void
    {
        if (packet.size() < 3) {
            return;
        }
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        participant_.receive_pdu(packet, now);
    }

    auto tick(TimePoint now) -> void
    {
        if (!started_) {
            participant_.start(now);
            started_ = true;
        }
        participant_.tick(now);
        if (callbacks_.send_packet) {
            participant_.set_send_pdu(callbacks_.send_packet);
        }
    }

    [[nodiscard]] auto talker_streams() const noexcept -> std::span<TalkerStreamSrpInfo const> { return talker_streams_; }
    [[nodiscard]] auto listener_streams() const noexcept -> std::span<ListenerStreamSrpInfo const> { return listener_streams_; }

  private:
    /// Forward a listener register/leave for one of OUR talker streams to the
    /// on_talker_listener hook, reporting the aggregate "may transmit" state.
    void notify_talker_listener(tsn::StreamId const& sid)
    {
        if (get_talker_stream(sid) != nullptr && callbacks_.on_talker_listener) {
            callbacks_.on_talker_listener(sid, participant_.listener_permits_transmit(sid));
        }
    }

    MsrpCallbacks callbacks_;
    DomainInfo domain_;
    statusbar::sg14::inplace_vector<TalkerStreamSrpInfo, MaxStreams> talker_streams_;
    statusbar::sg14::inplace_vector<ListenerStreamSrpInfo, MaxStreams> listener_streams_;
    statusbar::srp::msrp::MsrpParticipant participant_;
    bool started_{false};
};

}  // namespace statusbar::nanoavb
