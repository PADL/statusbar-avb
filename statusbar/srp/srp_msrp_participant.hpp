#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// MSRP Participant — IEEE 802.1Q-2014 Clause 35.
//
// Scope: local endpoint only (not bridge capable). One MsrpParticipant
// instance per NIC (per 802.1Q "port"). In-process API: no daemon
// sockets, no notification wire protocol. Observers subscribe via
// std::function callbacks and receive per-attribute notifications as
// the Registrar FSMs fire.
//
// PDU I/O is via a user-supplied callback (`set_send_pdu`) for
// transmit and explicit `receive_pdu()` for ingress. Timers are
// driven via `tick()` and `next_deadline()` for integration with any
// event loop.
//
// Implements:
//   - TalkerAdvertise / TalkerFailed attribute types (Clause 35.2.2.4)
//   - Listener attribute type with 4-packed substates Ignore /
//     AskingFailed / Ready / ReadyFailed (Clause 35.2.2.7.2)
//   - Domain attribute type (Clause 35.2.2.9)
//   - Per-attribute Applicant + Registrar FSMs via
//     statusbar::srp::mrp::PortState and the dispatch helpers
//   - JoinTimer / LeaveTimer / LeaveAllTimer / PeriodicTimer
//   - LeaveAll broadcast emission on timer expiry
//   - Reclaim sweep for fully-decayed attributes
//   - Interesting Stream ID pruning (mrp.c MSRP optional feature)
//
// Does NOT implement:
//   - MMRP (out of scope for local endpoint)
//   - MSRP_AGGREGATE_DOMAINS_VECTORS coalescing (singleton vectors only)
//   - Multi-value vector encode coalescing (singleton vectors only on tx;
//     decode handles multi-value vectors correctly)
//

#include "statusbar/buffer/buffer_mutable_buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/srp/srp_mrp_attribute.hpp"
#include "statusbar/srp/srp_mrp_participant.hpp"
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn_error.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <cstdint>
#include <functional>
#include <span>

namespace statusbar::srp::msrp {

/// Default compile-time capacity limits for MSRP attribute tables.
/// Override by defining a struct with the same fields and passing
/// it as the Limits template parameter.
struct DefaultMsrpLimits
{
    static constexpr size_t max_talker_advertise = 32;
    static constexpr size_t max_talker_failed = 16;
    static constexpr size_t max_listeners = 32;
    static constexpr size_t max_domains = 8;
    static constexpr size_t max_observers = 16;
    static constexpr size_t max_interesting_stream_ids = 32;
};

/// Back-compat alias.
using MsrpCapacityLimits = DefaultMsrpLimits;

using mrp::AttributeRecord;
using mrp::Operation;
using mrp::PortState;
using sm::TimePoint;
using statusbar::MutableBuffer;
using statusbar::MutableBufferWithStorage;
namespace applicant_sm = mrp::applicant_sm;
namespace registrar_sm = mrp::registrar_sm;

//
// MSRP Listener record — extends AttributeRecord with a per-attribute
// ListenerDeclaration substate aggregated from peer PDUs.
//
struct ListenerRecord
{
    ListenerFirstValue first_value{};
    Operation operation{Operation::Register};

    /// Per-attribute listener declaration substate
    /// (Clause 35.2.2.7.2). Updated on receive from peer; encoded
    /// into the outgoing 4-packed declarations field on tx.
    ListenerDeclaration substate{ListenerDeclaration::Ignore};

    applicant_sm::Context applicant_ctx{};
    applicant_sm::Machine applicant_sm{};
    registrar_sm::Context registrar_ctx{};
    registrar_sm::Machine registrar_sm{};
    bool should_reclaim{false};

    [[nodiscard]] auto registrar_is_in() const noexcept -> bool
    {
        return registrar_sm.current_state() == registrar_sm::Def::State::In;
    }

    /// A registration is valid while the registrar is In OR Lv (Leaving): per
    /// IEEE 802.1Q 10.7.7 the attribute stays registered through the LeaveTimer
    /// window, so a routine periodic LeaveAll refresh (In -> Lv -> In, every
    /// ~10-15 s) must NOT be seen as a deregistration. Only Mt (LeaveTimer
    /// expired with no re-Join) means the peer truly left. Gating reservations
    /// (e.g. talker transmit) on In-only makes them flap on every LeaveAll.
    [[nodiscard]] auto registrar_is_registered() const noexcept -> bool
    {
        auto const s = registrar_sm.current_state();
        return s == registrar_sm::Def::State::In || s == registrar_sm::Def::State::Lv;
    }

    [[nodiscard]] auto is_dead() const noexcept -> bool
    {
        return applicant_sm.current_state() == applicant_sm::Def::State::Vo &&
            registrar_sm.current_state() == registrar_sm::Def::State::Mt;
    }
};

//
// MsrpConfig — capacity limits resolved at construction time.
//
// All attribute / observer / interesting-stream containers are reserved
// up front at the configured maximums. At run time, the protocol engine
// allocates nothing: local declarations that would exceed their limit
// return `tsn::TsnError::attribute_table_full`, and peer-observed attributes
// are silently dropped (the peer will retransmit on its next periodic
// cycle if we later free capacity).
//
struct MsrpConfig
{
    /// Maximum TalkerAdvertise records (both locally declared and
    /// peer-observed). Sizing guidance:
    ///   - Listener-role endpoint: (interesting streams you track)
    ///     — one peer record per stream you want to receive. When
    ///     pruning is enabled, this is bounded by
    ///     max_interesting_stream_ids.
    ///   - Talker-role endpoint: (streams you produce) — one local
    ///     record per stream you advertise.
    ///   - Mixed: sum of the above.
    ///
    /// When pruning is enabled, peer records cannot exceed
    /// max_interesting_stream_ids, so a safe lower bound is
    /// (max_local_talker_streams + max_interesting_stream_ids).
    size_t max_talker_advertise = 16;

    /// Maximum TalkerFailed records. Almost always peer-observed —
    /// a bridge or upstream endpoint reporting a reservation failure
    /// for a stream it could not forward.
    size_t max_talker_failed = 4;

    /// Maximum Listener attribute records (both locally declared
    /// and peer-observed). Sizing guidance:
    ///   - Listener-role: (streams you consume) — local declarations.
    ///   - Talker-role: (streams you produce) — on a point-to-point
    ///     link the peer aggregates all downstream listeners for one
    ///     of your streams into a single declaration per stream. It
    ///     is that peer-observed declaration (with substate Ready or
    ///     ReadyFailed) that authorizes the talker application to
    ///     begin transmitting media frames per IEEE 802.1Q-2014
    ///     Clause 35. See MsrpParticipant::listener_permits_transmit.
    ///   - Mixed: sum of the above.
    size_t max_listeners = 16;

    /// Maximum Domain records. Typically 2 (SR Class A + B).
    size_t max_domains = 2;

    /// Maximum simultaneous Observer subscriptions.
    size_t max_observers = 4;

    /// Maximum Interesting Stream IDs for the optional pruning filter.
    /// Must be <= max_talker_advertise so the pruning set cannot grow
    /// larger than the storage that would hold the matching peer
    /// records.
    size_t max_interesting_stream_ids = 16;

    /// Check invariants. Returns Success if the config is
    /// self-consistent, otherwise a tsn::TsnError.
    [[nodiscard]] auto validate() const noexcept -> Status
    {
        if (max_interesting_stream_ids > max_talker_advertise) {
            return failure(make_error_code(tsn::TsnError::invalid_configuration));
        }
        return success();
    }
};

//
// Observer callback interface.
//
// Each callback is optional (nullable std::function). Notifications
// are delivered synchronously from within tick(), receive_pdu(), or
// the declare_* / withdraw_* API entry points, on the calling thread.
//
struct Observer
{
    /// A TalkerAdvertise attribute became registered (new) or was
    /// refreshed (join). `op` indicates whether the declaration came
    /// from a local client or from a peer.
    statusbar::sg14::inplace_function<void(TalkerAdvertiseFirstValue const&, Operation op), 64> on_talker_advertise;

    /// A TalkerFailed attribute became registered.
    statusbar::sg14::inplace_function<void(TalkerFailedFirstValue const&, Operation op), 64> on_talker_failed;

    /// A previously-registered Talker (Advertise or Failed) was
    /// withdrawn. Provides the tsn::StreamId since both attribute types
    /// share the same identity.
    statusbar::sg14::inplace_function<void(tsn::StreamId const&), 64> on_talker_leave;

    /// A Listener attribute became registered or changed substate.
    statusbar::sg14::inplace_function<void(tsn::StreamId const&, ListenerDeclaration, Operation op), 64> on_listener;

    /// A previously-registered Listener was withdrawn.
    statusbar::sg14::inplace_function<void(tsn::StreamId const&), 64> on_listener_leave;

    /// A Domain attribute became registered.
    statusbar::sg14::inplace_function<void(DomainFirstValue const&, Operation op), 64> on_domain;

    /// A previously-registered Domain was withdrawn.
    statusbar::sg14::inplace_function<void(DomainFirstValue const&), 64> on_domain_leave;
};

//
// MsrpParticipantT — per-port MSRP protocol engine, parameterized
// on compile-time capacity limits.
//
template <class Limits = DefaultMsrpLimits>
class MsrpParticipantT
{
  public:
    using SubscriptionId = uint32_t;
    using SendPduFn = statusbar::sg14::inplace_function<bool(std::span<uint8_t const>), 64>;

    /// Conservative upper bound on the size of a single outgoing
    /// MSRPDU. The participant allocates one fixed-size buffer of
    /// this length at construction time and reuses it on every
    /// transmit, so this value is a hard cap — PDUs are never split
    /// across buffers, attributes that would overflow are dropped
    /// (MRP retransmission recovers them on the next cycle).
    static constexpr size_t MAX_PDU_BYTES = 1500;

    /// Construct with an explicit capacity configuration. The config
    /// is validated; an invalid config will cause the constructor to
    /// throw std::system_error with tsn::TsnError::invalid_configuration.
    /// The rng_seed is forwarded to the TimerScheduler for LeaveAll
    /// interval randomization (pass nonzero for deterministic tests,
    /// zero for std::random_device seeding).
    explicit MsrpParticipantT(MsrpConfig const& config, uint64_t rng_seed = 0);

    /// Set or replace the outbound PDU callback. Called by the
    /// participant whenever it needs to transmit an MSRPDU.
    void set_send_pdu(SendPduFn fn);

    /// Run BEGIN! on the LeaveAll and Periodic FSMs and arm the
    /// initial timers. Must be called once after construction.
    void start(TimePoint now);

    /// Stop all timers. FSMs retain their current state; attribute
    /// database is preserved. Safe to call multiple times.
    void stop() noexcept;

    // -------------------- Observer subscription --------------------

    /// Register an Observer. Returns a handle for unsubscribe().
    /// Observers are not ordered and are invoked in unspecified order.
    auto subscribe(Observer obs) -> SubscriptionId;

    /// Remove a previously registered Observer. Safe on unknown IDs.
    void unsubscribe(SubscriptionId id);

    // -------------------- Local declaration API --------------------

    /// Declare a Talker Advertise attribute. Creates a new attribute
    /// record (if absent) or updates the existing record's
    /// FirstValue. Triggers Applicant New! or Join! event as
    /// appropriate and arms the join timer.
    auto declare_talker_advertise(TalkerAdvertiseFirstValue const& fv, TimePoint now) -> Status;

    /// Declare a Talker Failed attribute (used when a previously
    /// advertised talker cannot meet reservation requirements).
    auto declare_talker_failed(TalkerFailedFirstValue const& fv, TimePoint now) -> Status;

    /// Withdraw any Talker attribute (Advertise or Failed) matching
    /// `stream_id`. Fires Applicant Lv! event which drives the state
    /// through LA -> VO and reclaim.
    auto withdraw_talker(tsn::StreamId const& stream_id, TimePoint now) -> Status;

    /// Declare local Listener interest in a stream.
    auto declare_listener(tsn::StreamId const& stream_id, ListenerDeclaration decl, TimePoint now) -> Status;

    /// Withdraw local Listener interest.
    auto withdraw_listener(tsn::StreamId const& stream_id, TimePoint now) -> Status;

    /// Declare an SR class Domain.
    auto declare_domain(DomainFirstValue const& fv, TimePoint now) -> Status;

    /// Withdraw an SR class Domain by its sr_class_id.
    auto withdraw_domain(uint8_t sr_class_id, TimePoint now) -> Status;

    // -------------------- Query API --------------------

    /// Look up a locally known Talker Advertise by tsn::StreamId.
    [[nodiscard]] auto find_talker_advertise(tsn::StreamId const& id) const noexcept -> TalkerAdvertiseFirstValue const*;

    /// Look up a locally known Talker Failed by tsn::StreamId.
    [[nodiscard]] auto find_talker_failed(tsn::StreamId const& id) const noexcept -> TalkerFailedFirstValue const*;

    /// Look up a locally known Listener by tsn::StreamId and return its
    /// current substate if found.
    [[nodiscard]] auto find_listener(tsn::StreamId const& id) const noexcept -> ListenerRecord const*;

    /// Returns true if the peer listener state for this stream
    /// authorizes the application to transmit media frames (Ready or
    /// ReadyFailed), false otherwise (Ignore, AskingFailed, or no
    /// listener declaration received yet).
    ///
    /// IEEE 802.1Q-2014 Clause 35 requires that a talker not transmit
    /// stream frames until at least one downstream listener has
    /// declared Ready. On a point-to-point link the peer bridge
    /// aggregates all downstream listeners into a single declaration
    /// for each stream; this helper checks the latest such peer
    /// declaration for the given stream id.
    ///
    /// Typical use:
    /// \code
    ///   msrp.declare_talker_advertise(my_fv, now);
    ///   // ... observer fires on_listener(sid, Ready, Register) ...
    ///   if (msrp.listener_permits_transmit(my_sid)) {
    ///       audio_pipeline.enable();
    ///   }
    /// \endcode
    [[nodiscard]] auto listener_permits_transmit(tsn::StreamId const& stream_id) const noexcept -> bool;

    /// Diagnostic breakdown of listener_permits_transmit for one stream id: the
    /// exact reason the talker gate is open or closed, for logging the
    /// listener-ready flap. `permits` equals listener_permits_transmit().
    struct ListenerPermitDebug
    {
        bool has_record{false};
        Operation operation{Operation::Register};
        bool registrar_in{false};
        ListenerDeclaration substate{ListenerDeclaration::Ignore};
        bool permits{false};
    };
    [[nodiscard]] auto listener_permit_debug(tsn::StreamId const& stream_id) const noexcept -> ListenerPermitDebug;

    /// Look up a locally known Domain by sr_class_id.
    [[nodiscard]] auto find_domain(uint8_t sr_class_id) const noexcept -> DomainFirstValue const*;

    /// Number of active attributes of each type.
    [[nodiscard]] auto talker_advertise_count() const noexcept -> size_t { return talker_adv_.size(); }
    [[nodiscard]] auto talker_failed_count() const noexcept -> size_t { return talker_failed_.size(); }
    [[nodiscard]] auto listener_count() const noexcept -> size_t { return listeners_.size(); }
    [[nodiscard]] auto domain_count() const noexcept -> size_t { return domains_.size(); }

    /// Count of attribute messages skipped because the whole message would not fit
    /// in the current PDU (their records stay tx_pending and retransmit on the next
    /// build). Non-zero means the declared attribute set exceeds one MSRPDU and is
    /// being spread across successive PDUs -- expected 0 at normal deployment scale.
    [[nodiscard]] auto pdu_message_skip_count() const noexcept -> size_t { return pdu_message_skip_count_; }

    /// Count of received attributes whose StreamID matches one WE are locally
    /// declaring but whose payload differs -- a StreamIdInUseByAnotherTalker
    /// conflict (802.1Q-2018 35.2.4). We keep our own value (never adopt the
    /// peer's); a non-zero count means another station is declaring our StreamID.
    [[nodiscard]] auto foreign_declaration_conflict_count() const noexcept -> size_t { return foreign_declaration_conflict_count_; }

    // -------------------- PDU I/O --------------------

    /// Parse an inbound MSRPDU. The supplied span should cover the
    /// MSRP payload only (version octet through final EndMark), not
    /// the Ethernet / VLAN framing. Decoded events are dispatched to
    /// the per-attribute FSMs and observer notifications fire as a
    /// side effect.
    void receive_pdu(std::span<uint8_t const> pdu, TimePoint now);

    // -------------------- Event loop --------------------

    /// Check timers against `now` and process any expired ones.
    /// May trigger PDU emission (via the send_pdu callback) and
    /// observer notifications.
    void tick(TimePoint now);

    /// Earliest upcoming timer deadline. Returns TimePoint::max() if
    /// no timers are running. Use to schedule a poll/epoll wait.
    [[nodiscard]] auto next_deadline() const noexcept -> TimePoint;

    // -------------------- Interesting Stream ID pruning --------------------

    /// Enable or disable the optional Interesting Stream ID pruning
    /// filter. When enabled, TalkerAdvertise / TalkerFailed attributes
    /// learned from peers are only retained if their tsn::StreamId matches
    /// an entry in the interesting set. See mrp.c:556, 3596-3625.
    void set_pruning_enabled(bool enabled) noexcept { pruning_enabled_ = enabled; }

    [[nodiscard]] auto pruning_enabled() const noexcept -> bool { return pruning_enabled_; }

    /// Sticky-Listener workaround. When enabled, REGISTERED (operation==Register)
    /// Listener attributes are re-declared on every periodic/LeaveAll pass, not
    /// just our own declarations. This re-introduces, for the Listener type ONLY,
    /// the "sticky, never released" echo that cfea332 removed: an end station that
    /// is a TALKER holds a registered Listener record for its own stream (the
    /// downstream listener's Listener-Ready, propagated back to us by the bridge),
    /// and echoing it keeps the bridge's forwarding path to that listener warm.
    /// We deliberately do NOT extend this to the Talker type: re-declaring a
    /// registered TalkerAdvertise makes the bridge see a SECOND source for the
    /// StreamID and reject it with TalkerFailed code 19. A Listener has no such
    /// two-source conflict, so this echo is safe.
    /// Observed need: a downstream listener reached through an AVB switch stops
    /// being forwarded after the strict end-station change silenced the Listener
    /// echo. Default OFF (strict end-station behaviour); enable per host where a
    /// bridge needs the refresh.
    void set_redeclare_registered_listeners(bool enabled) noexcept { redeclare_registered_listeners_ = enabled; }

    [[nodiscard]] auto redeclare_registered_listeners() const noexcept -> bool { return redeclare_registered_listeners_; }

    /// Suppress-LeaveAll workaround. When enabled, the periodic LeaveAll timer
    /// NEVER transmits a LeaveAll on the wire and never drives our applicants
    /// into the leave/re-declare path: we just keep re-asserting our attributes
    /// via the periodic (1 Hz JoinIn/JoinMt) timer and never RELEASE them. This
    /// reproduces an earlier era (when a hardcoded leave_all_flag=false meant
    /// we never sent a LeaveAll) that fed a downstream listener through an AVB
    /// switch reliably. Hypothesis: the switch installs stream forwarding on a
    /// listener join but drops it when our LeaveAll drives the registration
    /// through Leaving, and only re-installs on a fresh join -- so a periodic
    /// LeaveAll makes the downstream forwarding blink. We still RECEIVE and
    /// honour peer LeaveAlls; we just don't
    /// originate them. Default OFF (spec-compliant periodic LeaveAll); enable per
    /// host that talks to a bridge with this behaviour.
    void set_suppress_leaveall(bool enabled) noexcept { suppress_leaveall_ = enabled; }

    [[nodiscard]] auto suppress_leaveall() const noexcept -> bool { return suppress_leaveall_; }

    /// Add a tsn::StreamId to the interesting set. Has no effect if the
    /// pruning filter is disabled. Returns failure if table is full.
    [[nodiscard]] auto add_interesting_stream_id(tsn::StreamId const& id) -> Status;

    /// Remove a tsn::StreamId from the interesting set.
    void remove_interesting_stream_id(tsn::StreamId const& id);

    /// Clear all interesting stream IDs.
    void clear_interesting_stream_ids() noexcept;

    [[nodiscard]] auto interesting_stream_id_count() const noexcept -> size_t { return interesting_stream_ids_.size(); }

  private:
    /// Slot entry for Observer subscriptions. `id == 0` indicates a
    /// free slot; the subscription search starts from index 0.
    struct ObserverSlot
    {
        SubscriptionId id{0};
        Observer obs{};
    };

    //
    // --- Configuration (captured at construction) ---
    //
    MsrpConfig config_;

    //
    // --- Attribute database ---
    // std::vector reserved once at construction to the configured
    // maximum. Linear-searched for lookup; AVB endpoint scale
    // (typically < 32 attributes per type) makes this faster than
    // any hashing.
    //
    statusbar::sg14::inplace_vector<AttributeRecord<TalkerAdvertiseFirstValue>, Limits::max_talker_advertise> talker_adv_;
    statusbar::sg14::inplace_vector<AttributeRecord<TalkerFailedFirstValue>, Limits::max_talker_failed> talker_failed_;
    statusbar::sg14::inplace_vector<ListenerRecord, Limits::max_listeners> listeners_;
    statusbar::sg14::inplace_vector<AttributeRecord<DomainFirstValue>, Limits::max_domains> domains_;

    //
    // --- Shared per-port state ---
    //
    PortState port_;
    SendPduFn send_pdu_{};

    //
    // --- Outgoing PDU buffer ---
    // Single fixed-size buffer owned by the participant; reset and
    // reused on every build_and_send_pdu() call. No heap allocation
    // on the TX hot path.
    //
    MutableBufferWithStorage<MAX_PDU_BYTES> pdu_buffer_{};
    /// See pdu_message_skip_count(): messages skipped for not fitting the PDU.
    size_t pdu_message_skip_count_{0};
    /// See foreign_declaration_conflict_count(): peer declared our StreamID.
    size_t foreign_declaration_conflict_count_{0};

    //
    // --- Observer subscriptions (slot-based, fixed capacity) ---
    //
    statusbar::sg14::inplace_vector<ObserverSlot, Limits::max_observers> observers_;
    SubscriptionId next_subscription_id_{1};

    //
    // --- Interesting Stream ID pruning ---
    // Sorted inplace_vector<uint64_t> — binary search for contains(),
    // insert-sorted for add(). Capacity fixed at compile time.
    //
    bool pruning_enabled_{false};
    // Sticky-Listener workaround (see set_redeclare_registered_listeners). When
    // true, registered Listener attributes are re-declared like our own.
    bool redeclare_registered_listeners_{false};
    // Suppress-LeaveAll workaround (see set_suppress_leaveall). When true, we
    // never originate a LeaveAll; we only re-assert via the periodic timer.
    bool suppress_leaveall_{false};
    statusbar::sg14::inplace_vector<uint64_t, Limits::max_interesting_stream_ids> interesting_stream_ids_;

    // Set by decode_vector_attribute when a received PDU carries a LeaveAll;
    // receive_pdu uses it to re-declare our attributes IMMEDIATELY rather than
    // waiting the full JoinTime (~100 ms). A bridge that issued the LeaveAll
    // declares our just-leaving registration as Mt toward downstream listeners on
    // its own join timer (~100 ms); a re-declare that lands just after that
    // window lets the listener (a DSP processor reached through an AVB switch) see our
    // talker blink out and drop its reservation. Re-declaring synchronously beats
    // that window.
    bool rx_leaveall_seen_{false};

    // ============================================================
    // Internal helpers
    // ============================================================

    //
    // Timer expiry handlers
    //
    void on_join_timer(TimePoint now);
    void on_leave_timer(TimePoint now);
    void on_leaveall_timer(TimePoint now);
    void on_periodic_timer(TimePoint now);

    //
    // PDU decode
    //
    void decode_attribute_list(
        AttributeType attr_type, uint8_t attr_length, std::span<uint8_t const> payload, size_t& pos, TimePoint now);

    void decode_vector_attribute(
        AttributeType attr_type, uint8_t attr_length, std::span<uint8_t const> payload, size_t& pos, TimePoint now);

    void handle_rx_event(
        AttributeType attr_type,
        uint8_t attr_length,
        std::span<uint8_t const> first_value_bytes,
        uint16_t value_index,
        mrp::AttributeEvent event,
        ListenerDeclaration decl,  // meaningful only for Listener
        TimePoint now);

    // Per-AttributeType rx handlers. Each is invoked by handle_rx_event
    // after translating the wire AttributeEvent to Applicant/Registrar
    // events. Keeps handle_rx_event down to a dispatch switch.
    void handle_talker_advertise_rx(
        uint8_t attr_length,
        std::span<uint8_t const> first_value_bytes,
        uint16_t value_index,
        applicant_sm::Def::Event applicant_event,
        registrar_sm::Def::Event registrar_event,
        TimePoint now);

    void handle_talker_failed_rx(
        uint8_t attr_length,
        std::span<uint8_t const> first_value_bytes,
        uint16_t value_index,
        applicant_sm::Def::Event applicant_event,
        registrar_sm::Def::Event registrar_event,
        TimePoint now);

    void handle_listener_rx(
        uint8_t attr_length,
        std::span<uint8_t const> first_value_bytes,
        uint16_t value_index,
        applicant_sm::Def::Event applicant_event,
        registrar_sm::Def::Event registrar_event,
        ListenerDeclaration decl,
        AttributeEvent wire_event,
        TimePoint now);

    void handle_domain_rx(
        uint8_t attr_length,
        std::span<uint8_t const> first_value_bytes,
        uint16_t value_index,
        applicant_sm::Def::Event applicant_event,
        registrar_sm::Def::Event registrar_event,
        TimePoint now);

    //
    // PDU encode
    //
    void build_and_send_pdu(TimePoint now, bool leave_all = false);

    template <typename RecordVec>
    auto append_attribute_message(
        MutableBuffer& out, RecordVec& records, AttributeType attr_type, uint8_t attr_length, bool include_leave_all) -> bool;

    //
    // Lookup helpers (mutable). Return nullptr if the attribute does
    // not exist AND the table is at its configured max capacity.
    // Callers that are servicing a local declare_* request should
    // translate nullptr into tsn::TsnError::attribute_table_full; callers
    // servicing a peer-observed attribute should silently drop.
    //
    auto find_or_create_talker_advertise(tsn::StreamId const& id) -> AttributeRecord<TalkerAdvertiseFirstValue>*;
    auto find_or_create_talker_failed(tsn::StreamId const& id) -> AttributeRecord<TalkerFailedFirstValue>*;
    auto find_or_create_listener(tsn::StreamId const& id) -> ListenerRecord*;
    auto find_or_create_domain(uint8_t sr_class_id) -> AttributeRecord<DomainFirstValue>*;

    //
    // Reclaim sweep — remove fully-decayed attributes.
    //
    void reclaim();

    //
    // Observer notification helpers
    //

    /// Fan out to every subscribed observer's @p member callback (when set), passing
    /// @p args. Replaces seven byte-identical "for slot: if set, call" loops.
    template <class Member, class... Args>
    void notify_all(Member Observer::* member, Args const&... args)
    {
        for (auto const& slot : observers_) {
            if (slot.id != 0 && (slot.obs.*member)) {
                (slot.obs.*member)(args...);
            }
        }
    }

    void notify_talker_advertise(TalkerAdvertiseFirstValue const& fv, Operation op);
    void notify_talker_failed(TalkerFailedFirstValue const& fv, Operation op);
    void notify_talker_leave(tsn::StreamId const& id);
    void notify_listener(tsn::StreamId const& id, ListenerDeclaration decl, Operation op);
    void notify_listener_leave(tsn::StreamId const& id);
    void notify_domain(DomainFirstValue const& fv, Operation op);
    void notify_domain_leave(DomainFirstValue const& fv);

    //
    // Interesting Stream ID predicate.
    // Returns true if (pruning is disabled) OR (stream_id is in the
    // interesting set). Applied only to talker-type attributes
    // learned from peer PDUs.
    //
    [[nodiscard]] auto is_interesting(tsn::StreamId const& id) const noexcept -> bool;
};

// ============================================================
// Implementation in msrp_participant.cpp
// ============================================================
// All out-of-line definitions for MsrpParticipantT<Limits> live in
// msrp_participant.cpp, which also contains the detail_msrp helper
// namespace and the explicit instantiation:
//
//   template class MsrpParticipantT<DefaultMsrpLimits>;
//
// If you introduce a new Limits type, add a matching explicit
// instantiation line in that .cpp.

/// Back-compat alias — default capacity.
using MsrpParticipant = MsrpParticipantT<>;

}  // namespace statusbar::srp::msrp
