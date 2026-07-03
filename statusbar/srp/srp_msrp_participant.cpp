// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// MsrpParticipant — out-of-line template definitions.
//
// Contains the implementation of every MsrpParticipantT<Limits> method
// along with the detail_msrp helper namespace. Explicit instantiation
// of MsrpParticipantT<DefaultMsrpLimits> (the only Limits type used in
// this tree) lives at the bottom; add another template class line here
// if you ever need a second variant.
//

#include "statusbar/srp/srp_msrp_participant.hpp"

#include "statusbar/status/throw_or_abort.hpp"

#include <algorithm>
#include <cstdio>  // stderr (diagnostic [srp-mrp] logging)
#include <print>

namespace statusbar::srp::msrp {

// ============================================================
// Implementation — out-of-line template method definitions
// ============================================================

namespace detail_msrp {

using mrp::AttributeEvent;
using mrp::AttributeListHeader;
using mrp::VectorAttributeHeader;

/// Translate an internal Applicant sndmsg (New/Join/In/Leave) into the
/// wire-level AttributeEvent. The Join->JoinIn/JoinMt and In->In/Mt
/// decision requires knowing the paired Registrar state — see
/// msrp.c:2002-2036.
constexpr auto translate_sndmsg(applicant_sm::SendMessage msg, bool registrar_is_in) noexcept -> AttributeEvent
{
    using applicant_sm::SendMessage;
    switch (msg) {
        case SendMessage::New:
            return AttributeEvent::New;
        case SendMessage::Join:
            return registrar_is_in ? AttributeEvent::JoinIn : AttributeEvent::JoinMt;
        case SendMessage::In:
            return registrar_is_in ? AttributeEvent::In : AttributeEvent::Mt;
        case SendMessage::Leave:
            return AttributeEvent::Lv;
        case SendMessage::None:
        default:
            return AttributeEvent::Mt;  // unreachable in practice; harmless default
    }
}

/// Translate a wire AttributeEvent into the Applicant/Registrar input
/// events that should be dispatched for this receive.
struct RxEvents
{
    applicant_sm::Def::Event applicant;
    registrar_sm::Def::Event registrar;
};

constexpr auto rx_events_for(AttributeEvent event) noexcept -> RxEvents
{
    switch (event) {
        case AttributeEvent::New:
            return {.applicant = applicant_sm::Def::Event::RNew, .registrar = registrar_sm::Def::Event::RNew};
        case AttributeEvent::JoinIn:
            return {.applicant = applicant_sm::Def::Event::RJoinIn, .registrar = registrar_sm::Def::Event::RJoinIn};
        case AttributeEvent::In:
            // Registrar ignores rIn! per Clause 10.7.8 Table 10-4
            return {.applicant = applicant_sm::Def::Event::RIn, .registrar = registrar_sm::Def::Event::Count};
        case AttributeEvent::JoinMt:
            return {.applicant = applicant_sm::Def::Event::RJoinMt, .registrar = registrar_sm::Def::Event::RJoinMt};
        case AttributeEvent::Mt:
            // Registrar ignores rMt! per Clause 10.7.8 Table 10-4
            return {.applicant = applicant_sm::Def::Event::RMt, .registrar = registrar_sm::Def::Event::Count};
        case AttributeEvent::Lv:
            return {.applicant = applicant_sm::Def::Event::RLeave, .registrar = registrar_sm::Def::Event::RLeave};
        default:
            return {.applicant = applicant_sm::Def::Event::Count, .registrar = registrar_sm::Def::Event::Count};
    }
}

/// Generic erase-remove for containers without std::erase_if support
/// (e.g. statusbar::sg14::inplace_vector).
template <typename Container, typename Pred>
void erase_if(Container& c, Pred pred)
{
    c.erase(std::remove_if(c.begin(), c.end(), pred), c.end());
}

/// Reset a MutableBuffer's used-data span to zero length, preserving
/// the underlying storage for reuse across TX passes.
inline void reset_buffer(MutableBuffer& buf) noexcept
{
    buf.set_span(std::span<uint8_t const>(buf.total_buffer_span().data(), 0));
}

/// Append a single byte to a MutableBuffer. Returns false if the
/// buffer has no free space (caller should stop appending and emit
/// whatever has been produced so far).
[[nodiscard]] inline auto append_u8(MutableBuffer& buf, uint8_t value) noexcept -> bool
{
    std::array<uint8_t, 1> const src{value};
    return static_cast<bool>(buf.append(std::span<uint8_t const>(src)));
}

/// Append a big-endian uint16 to a MutableBuffer.
[[nodiscard]] inline auto append_be16(MutableBuffer& buf, uint16_t value) noexcept -> bool
{
    std::array<uint8_t, 2> const src{
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFFU),
    };
    return static_cast<bool>(buf.append(std::span<uint8_t const>(src)));
}

}  // namespace detail_msrp

// Convenience using-declarations so method bodies can refer to the
// mrp helpers and detail_msrp helpers unqualified, matching the
// pre-templatization code.
using detail_msrp::append_be16;
using detail_msrp::append_u8;
using detail_msrp::erase_if;
using detail_msrp::reset_buffer;
using detail_msrp::rx_events_for;
using detail_msrp::translate_sndmsg;
using mrp::AttributeEvent;
using mrp::AttributeListHeader;
using mrp::VectorAttributeHeader;

// ============================================================
// Construction / lifecycle
// ============================================================

template <class Limits>
MsrpParticipantT<Limits>::MsrpParticipantT(MsrpConfig const& config, uint64_t rng_seed)
    : config_{config}
    , port_{rng_seed}
{
    if (auto const valid = config_.validate(); !valid) {
        statusbar::throw_or_abort(valid.error());
    }
    // Reserve all dynamic containers up front — no further allocations
    // occur during protocol processing. statusbar::sg14::inplace_vector::reserve()
    // is a static no-op that throws std::bad_alloc if the runtime size
    // exceeds the template capacity, so these calls serve as a runtime
    // capacity check against the MsrpCapacityLimits values.
    // NOLINTBEGIN(readability-static-accessed-through-instance)
    talker_adv_.reserve(config_.max_talker_advertise);
    talker_failed_.reserve(config_.max_talker_failed);
    listeners_.reserve(config_.max_listeners);
    domains_.reserve(config_.max_domains);
    // NOLINTEND(readability-static-accessed-through-instance)
    observers_.resize(config_.max_observers);                             // all slots initially free (id == 0)
    interesting_stream_ids_.reserve(config_.max_interesting_stream_ids);  // NOLINT(readability-static-accessed-through-instance)
}

template <class Limits>
void MsrpParticipantT<Limits>::set_send_pdu(SendPduFn fn)
{
    send_pdu_ = std::move(fn);
}

template <class Limits>
void MsrpParticipantT<Limits>::start(TimePoint now)
{
    port_.start(now);
}

template <class Limits>
void MsrpParticipantT<Limits>::stop() noexcept
{
    port_.stop();
}

// ============================================================
// Observer subscription (slot-based, fixed capacity)
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::subscribe(Observer obs) -> SubscriptionId
{
    for (auto& slot : observers_) {
        if (slot.id == 0) {
            auto const id = next_subscription_id_++;
            // Guard against id==0 wraparound (never hand out the sentinel).
            if (next_subscription_id_ == 0) {
                next_subscription_id_ = 1;
            }
            slot.id = id;
            slot.obs = std::move(obs);
            return id;
        }
    }
    // Observer table full. Return 0 — the caller should check.
    return 0;
}

template <class Limits>
void MsrpParticipantT<Limits>::unsubscribe(SubscriptionId id)
{
    if (id == 0) {
        return;
    }
    for (auto& slot : observers_) {
        if (slot.id == id) {
            slot.id = 0;
            slot.obs = Observer{};
            return;
        }
    }
}

// ============================================================
// Interesting Stream ID pruning (sorted vector, binary searched)
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::add_interesting_stream_id(tsn::StreamId const& id) -> Status
{
    auto const key = id.to_uint64();
    auto* const it = std::lower_bound(interesting_stream_ids_.begin(), interesting_stream_ids_.end(), key);
    if (it != interesting_stream_ids_.end() && *it == key) {
        return success();  // already present
    }
    if (interesting_stream_ids_.size() >= config_.max_interesting_stream_ids) {
        return failure(make_error_code(tsn::TsnError::interesting_table_full));
    }
    interesting_stream_ids_.insert(it, key);
    return success();
}

template <class Limits>
void MsrpParticipantT<Limits>::remove_interesting_stream_id(tsn::StreamId const& id)
{
    auto const key = id.to_uint64();
    auto* const it = std::lower_bound(interesting_stream_ids_.begin(), interesting_stream_ids_.end(), key);
    if (it != interesting_stream_ids_.end() && *it == key) {
        interesting_stream_ids_.erase(it);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::clear_interesting_stream_ids() noexcept
{
    interesting_stream_ids_.clear();
}

template <class Limits>
auto MsrpParticipantT<Limits>::is_interesting(tsn::StreamId const& id) const noexcept -> bool
{
    if (!pruning_enabled_) {
        return true;
    }
    auto const key = id.to_uint64();
    return std::binary_search(interesting_stream_ids_.begin(), interesting_stream_ids_.end(), key);
}

// ============================================================
// Query API
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::find_talker_advertise(tsn::StreamId const& id) const noexcept -> TalkerAdvertiseFirstValue const*
{
    for (auto const& rec : talker_adv_) {
        if (rec.first_value.stream_id == id) {
            return &rec.first_value;
        }
    }
    return nullptr;
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_talker_failed(tsn::StreamId const& id) const noexcept -> TalkerFailedFirstValue const*
{
    for (auto const& rec : talker_failed_) {
        if (rec.first_value.advertise.stream_id == id) {
            return &rec.first_value;
        }
    }
    return nullptr;
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_listener(tsn::StreamId const& id) const noexcept -> ListenerRecord const*
{
    for (auto const& rec : listeners_) {
        if (rec.first_value.stream_id == id) {
            return &rec;
        }
    }
    return nullptr;
}

template <class Limits>
auto MsrpParticipantT<Limits>::listener_permits_transmit(tsn::StreamId const& stream_id) const noexcept -> bool
{
    auto const* rec = find_listener(stream_id);
    if (rec == nullptr) {
        return false;  // no listener declaration received yet
    }
    // Only peer-observed listener declarations authorize transmit;
    // a locally-declared listener doesn't prove anyone downstream
    // actually wants the stream.
    if (rec->operation != Operation::Register) {
        return false;
    }
    // Must be registered: Registrar In OR Lv (Leaving). Lv is the transient
    // window of a routine periodic LeaveAll refresh (In -> Lv -> In); the
    // attribute is still registered until the LeaveTimer expires to Mt. Gating
    // on In-only made the talker flap off on every ~10-15 s LeaveAll. See
    // ListenerRecord::registrar_is_registered (IEEE 802.1Q 10.7.7).
    if (!rec->registrar_is_registered()) {
        return false;
    }
    return rec->substate == ListenerDeclaration::Ready || rec->substate == ListenerDeclaration::ReadyFailed;
}

template <class Limits>
auto MsrpParticipantT<Limits>::listener_permit_debug(tsn::StreamId const& stream_id) const noexcept -> ListenerPermitDebug
{
    ListenerPermitDebug d;
    auto const* rec = find_listener(stream_id);
    if (rec == nullptr) {
        return d;  // has_record=false, permits=false
    }
    d.has_record = true;
    d.operation = rec->operation;
    d.registrar_in = rec->registrar_is_registered();  // gate criterion: In or Lv
    d.substate = rec->substate;
    d.permits = (rec->operation == Operation::Register) && d.registrar_in &&
        (rec->substate == ListenerDeclaration::Ready || rec->substate == ListenerDeclaration::ReadyFailed);
    return d;
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_domain(uint8_t sr_class_id) const noexcept -> DomainFirstValue const*
{
    for (auto const& rec : domains_) {
        if (rec.first_value.sr_class_id.get() == sr_class_id) {
            return &rec.first_value;
        }
    }
    return nullptr;
}

// ============================================================
// Find-or-create helpers
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_talker_advertise(tsn::StreamId const& id)
    -> AttributeRecord<TalkerAdvertiseFirstValue>*
{
    for (auto& rec : talker_adv_) {
        if (rec.first_value.stream_id == id) {
            return &rec;
        }
    }
    if (talker_adv_.size() >= config_.max_talker_advertise) {
        return nullptr;
    }
    AttributeRecord<TalkerAdvertiseFirstValue> rec{};
    rec.first_value.stream_id = id;
    talker_adv_.push_back(rec);
    return &talker_adv_.back();
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_talker_failed(tsn::StreamId const& id) -> AttributeRecord<TalkerFailedFirstValue>*
{
    for (auto& rec : talker_failed_) {
        if (rec.first_value.advertise.stream_id == id) {
            return &rec;
        }
    }
    if (talker_failed_.size() >= config_.max_talker_failed) {
        return nullptr;
    }
    AttributeRecord<TalkerFailedFirstValue> rec{};
    rec.first_value.advertise.stream_id = id;
    talker_failed_.push_back(rec);
    return &talker_failed_.back();
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_listener(tsn::StreamId const& id) -> ListenerRecord*
{
    for (auto& rec : listeners_) {
        if (rec.first_value.stream_id == id) {
            return &rec;
        }
    }
    if (listeners_.size() >= config_.max_listeners) {
        return nullptr;
    }
    ListenerRecord rec{};
    rec.first_value.stream_id = id;
    listeners_.push_back(rec);
    return &listeners_.back();
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_domain(uint8_t sr_class_id) -> AttributeRecord<DomainFirstValue>*
{
    for (auto& rec : domains_) {
        if (rec.first_value.sr_class_id.get() == sr_class_id) {
            return &rec;
        }
    }
    if (domains_.size() >= config_.max_domains) {
        return nullptr;
    }
    AttributeRecord<DomainFirstValue> rec{};
    rec.first_value.sr_class_id = sr_class_id;
    domains_.push_back(rec);
    return &domains_.back();
}

// ============================================================
// Local declaration API
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::declare_talker_advertise(TalkerAdvertiseFirstValue const& fv, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_talker_advertise(fv.stream_id);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    auto& rec = *rec_ptr;
    bool const is_new = (rec.operation == Operation::Register) &&
        (rec.applicant_sm.current_state() == applicant_sm::Def::State::Start ||
         rec.applicant_sm.current_state() == applicant_sm::Def::State::Vo);
    rec.first_value = fv;
    rec.operation = Operation::Declare;

    auto const event = is_new ? applicant_sm::Def::Event::New : applicant_sm::Def::Event::Join;
    mrp::dispatch_applicant(rec, event, now);
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::declare_talker_failed(TalkerFailedFirstValue const& fv, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_talker_failed(fv.advertise.stream_id);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    auto& rec = *rec_ptr;
    bool const is_new = (rec.operation == Operation::Register) &&
        (rec.applicant_sm.current_state() == applicant_sm::Def::State::Start ||
         rec.applicant_sm.current_state() == applicant_sm::Def::State::Vo);
    rec.first_value = fv;
    rec.operation = Operation::Declare;

    auto const event = is_new ? applicant_sm::Def::Event::New : applicant_sm::Def::Event::Join;
    mrp::dispatch_applicant(rec, event, now);
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::withdraw_talker(tsn::StreamId const& stream_id, TimePoint now) -> Status
{
    bool found = false;
    for (auto& rec : talker_adv_) {
        if (rec.first_value.stream_id == stream_id) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
            found = true;
        }
    }
    for (auto& rec : talker_failed_) {
        if (rec.first_value.advertise.stream_id == stream_id) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
            found = true;
        }
    }
    if (found) {
        port_.timers().start_join(now);
    }
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::declare_listener(tsn::StreamId const& stream_id, ListenerDeclaration decl, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_listener(stream_id);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    auto& rec = *rec_ptr;
    bool const is_new = (rec.operation == Operation::Register) &&
        (rec.applicant_sm.current_state() == applicant_sm::Def::State::Start ||
         rec.applicant_sm.current_state() == applicant_sm::Def::State::Vo);
    rec.operation = Operation::Declare;
    rec.substate = decl;

    auto const event = is_new ? applicant_sm::Def::Event::New : applicant_sm::Def::Event::Join;
    rec.applicant_ctx.clear_outputs();
    rec.applicant_sm.handle_event(rec.applicant_ctx, event, now);
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::withdraw_listener(tsn::StreamId const& stream_id, TimePoint now) -> Status
{
    for (auto& rec : listeners_) {
        if (rec.first_value.stream_id == stream_id) {
            rec.applicant_ctx.clear_outputs();
            rec.applicant_sm.handle_event(rec.applicant_ctx, applicant_sm::Def::Event::Leave, now);
            port_.timers().start_join(now);
            return success();
        }
    }
    return success();  // withdraw of unknown listener is not an error
}

template <class Limits>
auto MsrpParticipantT<Limits>::declare_domain(DomainFirstValue const& fv, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_domain(fv.sr_class_id.get());
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    auto& rec = *rec_ptr;
    bool const is_new = (rec.operation == Operation::Register) &&
        (rec.applicant_sm.current_state() == applicant_sm::Def::State::Start ||
         rec.applicant_sm.current_state() == applicant_sm::Def::State::Vo);
    rec.first_value = fv;
    rec.operation = Operation::Declare;

    auto const event = is_new ? applicant_sm::Def::Event::New : applicant_sm::Def::Event::Join;
    mrp::dispatch_applicant(rec, event, now);
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::withdraw_domain(uint8_t sr_class_id, TimePoint now) -> Status
{
    for (auto& rec : domains_) {
        if (rec.first_value.sr_class_id.get() == sr_class_id) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
            port_.timers().start_join(now);
            return success();
        }
    }
    return success();
}

// ============================================================
// Event loop — tick / next_deadline
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::next_deadline() const noexcept -> TimePoint
{
    return port_.timers().next_deadline();
}

template <class Limits>
void MsrpParticipantT<Limits>::tick(TimePoint now)
{
    auto const expired = port_.timers().tick(now);
    if (!expired.any()) {
        return;
    }
    // Order matches mrp.c's dispatch loop ordering: LeaveAll first
    // (triggers TxLA events on all attributes), then Leave (Registrar
    // expiry), then Periodic, then Join (actual transmit).
    if (expired.leaveall) {
        on_leaveall_timer(now);
    }
    if (expired.leave) {
        on_leave_timer(now);
    }
    if (expired.periodic) {
        on_periodic_timer(now);
    }
    if (expired.join) {
        on_join_timer(now);
    }
    reclaim();
}

template <class Limits>
void MsrpParticipantT<Limits>::on_join_timer(TimePoint now)
{
    build_and_send_pdu(now);
}

template <class Limits>
void MsrpParticipantT<Limits>::on_leave_timer(TimePoint now)
{
    // Fire LvTimer event into every attribute's Registrar. Any that
    // were in Lv state transition to Mt and notify Leave.
    auto drive = [&](auto& records, auto&& notify_leave_id) {
        for (auto& rec : records) {
            if (rec.registrar_sm.current_state() != registrar_sm::Def::State::Lv) {
                continue;
            }
            mrp::dispatch_registrar(rec, registrar_sm::Def::Event::LvTimer, now, port_.timers());
            if (rec.registrar_ctx.notify == registrar_sm::Notify::Leave) {
                notify_leave_id(rec);
            }
        }
    };
    drive(talker_adv_, [&](auto const& rec) { notify_talker_leave(rec.first_value.stream_id); });
    drive(talker_failed_, [&](auto const& rec) { notify_talker_leave(rec.first_value.advertise.stream_id); });
    drive(listeners_, [&](auto const& rec) { notify_listener_leave(rec.first_value.stream_id); });
    drive(domains_, [&](auto const& rec) { notify_domain_leave(rec.first_value); });
}

template <class Limits>
void MsrpParticipantT<Limits>::on_leaveall_timer(TimePoint now)
{
    // LeaveAll timer expired: drive the LeaveAll FSM, which rearms its own timer,
    // then fire TxLeaveAll on every per-attribute Applicant (so our own
    // declarations are RE-DECLARED in the LeaveAll PDU). After this, the next TX
    // pass includes the LeaveAll flag.
    port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::LvaTimer, now);

    // Suppress-LeaveAll workaround: re-arm the FSM (done above) but never
    // originate a LeaveAll and never drive our applicants into the leave path.
    // We just keep re-asserting via the periodic timer -- the earlier "sticky,
    // never release" behaviour that an AVB bridge needs for stable downstream
    // forwarding. Consume the FSM's pending Tx so it does not get stuck Active,
    // but pass leave_all=false so the bit never reaches the wire.
    if (suppress_leaveall_) {
        port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::Tx, now);
        build_and_send_pdu(now, /*leave_all=*/false);
        return;
    }

    auto drive = [&](auto& records) {
        for (auto& rec : records) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::TxLeaveAll, now);
            // Deliberately DO NOT drive our own Registrars to Lv on our OWN
            // LeaveAll (the spec's In + TxLeaveAll -> Lv). That GC step assumes a
            // peer re-declares the attribute within LeaveTime; a non-compliant
            // bridge (an AVB switch that re-declares only on ITS own ~12 s
            // LeaveAll, not in response to ours) does not, so we would age our own
            // ACTIVELY-SERVED registration to Mt every LeaveAll period -- closing
            // the talker gate (registrar not In) and freezing the stream. As an
            // end-station MAD-only participant we let the peer's LeaveAll/Leave
            // (RLeaveAll/RLeave) be the only thing that retires a registration; a
            // received LeaveAll always arrives with the peer's re-declare in the
            // same PDU, so it never strands us.
        }
    };
    drive(talker_adv_);
    drive(talker_failed_);
    drive(listeners_);
    drive(domains_);

    // The LeaveAll FSM is now in Active; dispatch Tx so it transitions back to
    // Passive with tx_leaveall_pending=true, and emit the PDU WITH the LeaveAll
    // flag set. Forwarding tx_leaveall_pending is what actually puts the LeaveAll
    // (and the re-declarations) on the wire. See build_and_send_pdu.
    auto const& lva_ctx = port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::Tx, now);
    build_and_send_pdu(now, lva_ctx.tx_leaveall_pending);
}

template <class Limits>
void MsrpParticipantT<Limits>::on_periodic_timer(TimePoint now)
{
    // Rearm the Periodic FSM (it requests a timer restart while Active).
    port_.dispatch_periodic(mrp::periodic_sm::Def::Event::Periodic, now);

    // Fire Periodic on every attribute's Applicant.
    auto drive = [&](auto& records) {
        for (auto& rec : records) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Periodic, now);
        }
    };
    drive(talker_adv_);
    drive(talker_failed_);
    drive(listeners_);
    drive(domains_);

    // Periodic state transitions (Qa -> Aa, Qp -> Ap) may imply tx;
    // schedule the join timer to produce a PDU opportunity shortly.
    port_.timers().start_join(now);
}

// ============================================================
// Observer notifications
// ============================================================

template <class Limits>
void MsrpParticipantT<Limits>::notify_talker_advertise(TalkerAdvertiseFirstValue const& fv, Operation op)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_talker_advertise) {
            slot.obs.on_talker_advertise(fv, op);
        }
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_talker_failed(TalkerFailedFirstValue const& fv, Operation op)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_talker_failed) {
            slot.obs.on_talker_failed(fv, op);
        }
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_talker_leave(tsn::StreamId const& id)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_talker_leave) {
            slot.obs.on_talker_leave(id);
        }
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_listener(tsn::StreamId const& id, ListenerDeclaration decl, Operation op)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_listener) {
            slot.obs.on_listener(id, decl, op);
        }
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_listener_leave(tsn::StreamId const& id)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_listener_leave) {
            slot.obs.on_listener_leave(id);
        }
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_domain(DomainFirstValue const& fv, Operation op)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_domain) {
            slot.obs.on_domain(fv, op);
        }
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_domain_leave(DomainFirstValue const& fv)
{
    for (auto const& slot : observers_) {
        if (slot.id != 0 && slot.obs.on_domain_leave) {
            slot.obs.on_domain_leave(fv);
        }
    }
}

// ============================================================
// Reclaim
// ============================================================

template <class Limits>
void MsrpParticipantT<Limits>::reclaim()
{
    auto is_dead = [](auto const& rec) { return rec.is_dead(); };
    detail_msrp::erase_if(talker_adv_, is_dead);
    detail_msrp::erase_if(talker_failed_, is_dead);
    detail_msrp::erase_if(listeners_, is_dead);
    detail_msrp::erase_if(domains_, is_dead);
}

// ============================================================
// PDU decode
// ============================================================

template <class Limits>
void MsrpParticipantT<Limits>::receive_pdu(std::span<uint8_t const> pdu, TimePoint now)
{
    if (pdu.size() < 3) {  // minimum: version + EndMark
        return;
    }
    if (pdu[0] != PROTOCOL_VERSION) {
        return;  // unknown protocol version
    }

    rx_leaveall_seen_ = false;
    size_t pos = 1;
    while (pos + 2 <= pdu.size()) {
        uint16_t const end_check = (static_cast<uint16_t>(pdu[pos]) << 8) | pdu[pos + 1];
        if (end_check == mrp::END_MARK) {
            break;
        }

        if (pos + AttributeListHeader::LENGTH + 2 > pdu.size()) {
            return;
        }
        AttributeListHeader hdr;
        (void)load_unchecked(pdu.subspan(pos), &hdr);
        pos += AttributeListHeader::LENGTH;

        // AttributeListLength (IEEE 802.1Q-2014 Clause 10.8.2.3): length in
        // octets of the AttributeList (vectors + trailing EndMark). Consumed
        // here and used to bound the per-message decode scope so a malformed
        // length can't make us read past the message.
        uint16_t const attr_list_length = (static_cast<uint16_t>(pdu[pos]) << 8) | pdu[pos + 1];
        pos += 2;

        size_t const attr_list_end = pos + attr_list_length;
        if (attr_list_end > pdu.size()) {
            return;
        }

        auto const attr_type = static_cast<AttributeType>(hdr.attribute_type.get());
        uint8_t const attr_length = hdr.attribute_length.get();

        auto const message_payload = pdu.first(attr_list_end);
        decode_attribute_list(attr_type, attr_length, message_payload, pos, now);

        // Skip over any trailing bytes the inner decoder didn't consume,
        // keeping the outer message-walk cursor aligned with the sender.
        pos = attr_list_end;
    }

    // A received LeaveAll asked everyone to re-declare. Do it SYNCHRONOUSLY here
    // rather than only arming the join timer (~100 ms): the bridge that sent the
    // LeaveAll declares our (now-leaving) attributes as Mt toward downstream
    // listeners on its own ~100 ms join timer, and a re-declare that lands just
    // after that window makes the listener see our talker momentarily disappear
    // and tear down its reservation (observed feeding a downstream listener through an
    // AVB switch: the downstream stream stopped being forwarded). Emitting our re-declarations
    // immediately keeps our registration continuously visible to the bridge.
    if (rx_leaveall_seen_) {
        build_and_send_pdu(now);
    }

    // After processing received events, a join timer opportunity may
    // be needed so that state transitions triggered by rx events can
    // be transmitted.
    port_.timers().start_join(now);
}

template <class Limits>
void MsrpParticipantT<Limits>::decode_attribute_list(
    AttributeType attr_type, uint8_t attr_length, std::span<uint8_t const> payload, size_t& pos, TimePoint now)
{
    while (pos + 2 <= payload.size()) {
        uint16_t const end_check = (static_cast<uint16_t>(payload[pos]) << 8) | payload[pos + 1];
        if (end_check == mrp::END_MARK) {
            pos += 2;
            return;
        }
        decode_vector_attribute(attr_type, attr_length, payload, pos, now);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::decode_vector_attribute(
    AttributeType attr_type, uint8_t attr_length, std::span<uint8_t const> payload, size_t& pos, TimePoint now)
{
    if (pos + VectorAttributeHeader::LENGTH > payload.size()) {
        return;
    }
    VectorAttributeHeader vec_hdr;
    (void)load_unchecked(payload.subspan(pos), &vec_hdr);
    pos += VectorAttributeHeader::LENGTH;

    bool const leave_all = vec_hdr.get_leave_all();
    uint16_t const num_values = vec_hdr.get_number_of_values();

    // If the vector carries a LeaveAll flag, deliver it to every
    // attribute of this type.
    if (leave_all) {
        rx_leaveall_seen_ = true;  // receive_pdu re-declares immediately (see there)
        if (attr_type == AttributeType::Listener) {
            // Diagnostic: a LeaveAll on the Listener type drives EVERY listener
            // registrar to Leaving (registrar_in -> false) until the next Join
            // re-registers it -- a prime suspect for the listener-ready flap.
            std::print(stderr, "[srp-mrp] Listener LeaveAll (num_values={}) -> all listener registrars leave\n", num_values);
        }
        auto drive_leaveall = [&](auto& records) {
            for (auto& rec : records) {
                mrp::dispatch_applicant(rec, applicant_sm::Def::Event::RLeaveAll, now);
                mrp::dispatch_registrar(rec, registrar_sm::Def::Event::RLeaveAll, now, port_.timers());
            }
        };
        switch (attr_type) {
            case AttributeType::TalkerAdvertise:
                drive_leaveall(talker_adv_);
                break;
            case AttributeType::TalkerFailed:
                drive_leaveall(talker_failed_);
                break;
            case AttributeType::Listener:
                drive_leaveall(listeners_);
                break;
            case AttributeType::Domain:
                drive_leaveall(domains_);
                break;
        }
        // Also ack the LeaveAll into our LeaveAll FSM to restart its timer.
        port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::RLeaveAll, now);
    }

    if (pos + attr_length > payload.size()) {
        return;
    }
    auto const first_value_bytes = payload.subspan(pos, attr_length);
    pos += attr_length;

    size_t const num_event_octets = mrp::threepacked_octet_count(num_values);
    bool const has_declarations = (attr_type == AttributeType::Listener);
    size_t const num_decl_octets = has_declarations ? mrp::fourpacked_octet_count(num_values) : 0;

    if (pos + num_event_octets + num_decl_octets > payload.size()) {
        return;
    }
    auto const event_bytes = payload.subspan(pos, num_event_octets);
    auto const decl_bytes =
        has_declarations ? payload.subspan(pos + num_event_octets, num_decl_octets) : std::span<uint8_t const>{};
    pos += num_event_octets + num_decl_octets;

    // Walk each of the num_values attributes in this vector.
    for (uint16_t i = 0; i < num_values; ++i) {
        auto const unpacked = mrp::unpack3_events(event_bytes[i / 3]);
        AttributeEvent event{};
        switch (i % 3) {
            case 0:
                event = unpacked.first;
                break;
            case 1:
                event = unpacked.second;
                break;
            case 2:
                event = unpacked.third;
                break;
            default:
                break;  // unreachable (i % 3 is always 0..2)
        }

        ListenerDeclaration decl = ListenerDeclaration::Ignore;
        if (has_declarations) {
            auto const four = mrp::unpack4_declarations(decl_bytes[i / 4]);
            uint8_t raw = 0;
            switch (i % 4) {
                case 0:
                    raw = four.first;
                    break;
                case 1:
                    raw = four.second;
                    break;
                case 2:
                    raw = four.third;
                    break;
                case 3:
                    raw = four.fourth;
                    break;
                default:
                    break;  // unreachable (i % 4 is always 0..3)
            }
            decl = static_cast<ListenerDeclaration>(raw);
        }

        handle_rx_event(attr_type, attr_length, first_value_bytes, i, event, decl, now);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::handle_rx_event(
    AttributeType attr_type,
    uint8_t attr_length,
    std::span<uint8_t const> first_value_bytes,
    uint16_t value_index,
    AttributeEvent event,
    ListenerDeclaration decl,
    TimePoint now)
{
    auto const rx = detail_msrp::rx_events_for(event);
    if (rx.applicant == applicant_sm::Def::Event::Count) {
        return;  // unknown event
    }
    switch (attr_type) {
        case AttributeType::TalkerAdvertise:
            handle_talker_advertise_rx(attr_length, first_value_bytes, value_index, rx.applicant, rx.registrar, now);
            break;
        case AttributeType::TalkerFailed:
            handle_talker_failed_rx(attr_length, first_value_bytes, value_index, rx.applicant, rx.registrar, now);
            break;
        case AttributeType::Listener:
            handle_listener_rx(attr_length, first_value_bytes, value_index, rx.applicant, rx.registrar, decl, event, now);
            break;
        case AttributeType::Domain:
            handle_domain_rx(attr_length, first_value_bytes, value_index, rx.applicant, rx.registrar, now);
            break;
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::handle_talker_advertise_rx(
    uint8_t attr_length,
    std::span<uint8_t const> first_value_bytes,
    uint16_t value_index,
    applicant_sm::Def::Event applicant_event,
    registrar_sm::Def::Event registrar_event,
    TimePoint now)
{
    if (attr_length < TalkerAdvertiseFirstValue::LENGTH) {
        return;
    }
    TalkerAdvertiseFirstValue fv;
    (void)load_unchecked(first_value_bytes, &fv);
    for (uint16_t k = 0; k < value_index; ++k) {
        increment_first_value(fv);
    }
    if (!is_interesting(fv.stream_id)) {
        return;
    }
    auto* rec_ptr = find_or_create_talker_advertise(fv.stream_id);
    if (rec_ptr == nullptr) {
        return;  // peer attribute, table full — silently drop
    }
    auto& rec = *rec_ptr;
    bool const was_registered = (rec.registrar_sm.current_state() == registrar_sm::Def::State::In);
    // Adopt the received payload ONLY for a pure peer registration. If WE are
    // locally declaring this StreamID, a peer declaring the same one is a
    // StreamIdInUseByAnotherTalker conflict (802.1Q-2018 35.2.4): keep OUR value so
    // our next JoinIn re-advertises our own params, not the peer's.
    if (rec.operation == Operation::Register) {
        rec.first_value = fv;
    } else if (!(rec.first_value == fv)) {
        ++foreign_declaration_conflict_count_;
    }
    mrp::dispatch_applicant(rec, applicant_event, now);
    if (registrar_event == registrar_sm::Def::Event::Count) {
        return;
    }
    mrp::dispatch_registrar(rec, registrar_event, now, port_.timers());
    auto const n = rec.registrar_ctx.notify;
    if (n == registrar_sm::Notify::New || n == registrar_sm::Notify::Join) {
        notify_talker_advertise(rec.first_value, Operation::Register);
    } else if (n == registrar_sm::Notify::Leave && was_registered) {
        notify_talker_leave(rec.first_value.stream_id);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::handle_talker_failed_rx(
    uint8_t attr_length,
    std::span<uint8_t const> first_value_bytes,
    uint16_t value_index,
    applicant_sm::Def::Event applicant_event,
    registrar_sm::Def::Event registrar_event,
    TimePoint now)
{
    if (attr_length < TalkerFailedFirstValue::LENGTH) {
        return;
    }
    TalkerFailedFirstValue fv;
    (void)load_unchecked(first_value_bytes, &fv);
    for (uint16_t k = 0; k < value_index; ++k) {
        increment_first_value(fv);
    }
    if (!is_interesting(fv.advertise.stream_id)) {
        return;
    }
    auto* rec_ptr = find_or_create_talker_failed(fv.advertise.stream_id);
    if (rec_ptr == nullptr) {
        return;  // peer attribute, table full — silently drop
    }
    auto& rec = *rec_ptr;
    bool const was_registered = (rec.registrar_sm.current_state() == registrar_sm::Def::State::In);
    // Keep our own payload if WE declare this StreamID (see handle_talker_advertise_rx).
    if (rec.operation == Operation::Register) {
        rec.first_value = fv;
    } else if (!(rec.first_value == fv)) {
        ++foreign_declaration_conflict_count_;
    }
    mrp::dispatch_applicant(rec, applicant_event, now);
    if (registrar_event == registrar_sm::Def::Event::Count) {
        return;
    }
    mrp::dispatch_registrar(rec, registrar_event, now, port_.timers());
    auto const n = rec.registrar_ctx.notify;
    if (n == registrar_sm::Notify::New || n == registrar_sm::Notify::Join) {
        notify_talker_failed(rec.first_value, Operation::Register);
    } else if (n == registrar_sm::Notify::Leave && was_registered) {
        notify_talker_leave(rec.first_value.advertise.stream_id);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::handle_listener_rx(
    uint8_t attr_length,
    std::span<uint8_t const> first_value_bytes,
    uint16_t value_index,
    applicant_sm::Def::Event applicant_event,
    registrar_sm::Def::Event registrar_event,
    ListenerDeclaration decl,
    AttributeEvent wire_event,
    TimePoint now)
{
    if (attr_length < ListenerFirstValue::LENGTH) {
        return;
    }
    ListenerFirstValue fv;
    (void)load_unchecked(first_value_bytes, &fv);
    for (uint16_t k = 0; k < value_index; ++k) {
        increment_first_value(fv);
    }
    auto* rec_ptr = find_or_create_listener(fv.stream_id);
    if (rec_ptr == nullptr) {
        return;  // peer attribute, table full — silently drop
    }
    auto& rec = *rec_ptr;
    auto const state_before = rec.registrar_sm.current_state();
    bool const was_registered = (state_before == registrar_sm::Def::State::In);
    auto const prev_substate = rec.substate;
    // Adopt the received listener declaration ONLY for a pure peer registration. If
    // WE declare this StreamID, keep OUR substate (first_value is just the StreamID
    // here; substate is the payload) so our next JoinIn re-advertises our own.
    if (rec.operation == Operation::Register) {
        rec.first_value = fv;
        rec.substate = decl;
    } else if (rec.substate != decl) {
        ++foreign_declaration_conflict_count_;
    }

    rec.applicant_ctx.clear_outputs();
    rec.applicant_sm.handle_event(rec.applicant_ctx, applicant_event, now);
    if (registrar_event == registrar_sm::Def::Event::Count) {
        return;
    }
    rec.registrar_ctx.clear_outputs();
    rec.registrar_sm.handle_event(rec.registrar_ctx, registrar_event, now);
    // Diagnostic: a Listener attribute received for one of OUR talker streams --
    // print the wire MRP event and the registrar STATE transition, to trace what
    // drives the listener-ready flap ([srp-gate]). Seeing In<->Lv = a routine
    // LeaveAll refresh (benign once the gate accepts Lv); reaching Mt = the peer
    // truly de-registered (re-declaration / relay problem). `is_interesting`
    // keeps this to our streams.
    if (is_interesting(rec.first_value.stream_id)) {
        auto const state_name = [](registrar_sm::Def::State s) -> char const* {
            switch (s) {
                case registrar_sm::Def::State::In:
                    return "In";
                case registrar_sm::Def::State::Lv:
                    return "Lv";
                case registrar_sm::Def::State::Mt:
                    return "Mt";
                case registrar_sm::Def::State::Start:
                    return "Start";
                default:
                    return "?";
            }
        };
        std::print(
            stderr,
            "[srp-mrp] listener rx sid={:016x} wire_event={} decl={} reg {}->{} lvtimer={}\n",
            rec.first_value.stream_id.to_uint64(),
            attribute_event_name(wire_event),
            listener_declaration_name(decl),
            state_name(state_before),
            state_name(rec.registrar_sm.current_state()),
            rec.registrar_ctx.lvtimer_request);
    }
    if (rec.registrar_ctx.lvtimer_request) {
        port_.timers().start_leave(now);
    }
    auto const n = rec.registrar_ctx.notify;
    if (n == registrar_sm::Notify::New || n == registrar_sm::Notify::Join || (was_registered && prev_substate != decl)) {
        notify_listener(rec.first_value.stream_id, decl, Operation::Register);
    } else if (n == registrar_sm::Notify::Leave && was_registered) {
        notify_listener_leave(rec.first_value.stream_id);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::handle_domain_rx(
    uint8_t attr_length,
    std::span<uint8_t const> first_value_bytes,
    uint16_t value_index,
    applicant_sm::Def::Event applicant_event,
    registrar_sm::Def::Event registrar_event,
    TimePoint now)
{
    if (attr_length < DomainFirstValue::LENGTH) {
        return;
    }
    DomainFirstValue fv;
    (void)load_unchecked(first_value_bytes, &fv);
    // Multi-value Domain vectors increment SRclassID and SRclassPriority per
    // value (IEEE 802.1Q 35.2.2.9; AVnu MSRP.End.c.35.1.11 Part B). The VID is
    // carried unchanged.
    for (uint16_t k = 0; k < value_index; ++k) {
        increment_first_value(fv);
    }
    auto* rec_ptr = find_or_create_domain(fv.sr_class_id.get());
    if (rec_ptr == nullptr) {
        return;  // peer attribute, table full — silently drop
    }
    auto& rec = *rec_ptr;
    bool const was_registered = (rec.registrar_sm.current_state() == registrar_sm::Def::State::In);
    // Keep our own payload if WE declare this domain (see handle_talker_advertise_rx).
    if (rec.operation == Operation::Register) {
        rec.first_value = fv;
    } else if (!(rec.first_value == fv)) {
        ++foreign_declaration_conflict_count_;
    }
    mrp::dispatch_applicant(rec, applicant_event, now);
    if (registrar_event == registrar_sm::Def::Event::Count) {
        return;
    }
    mrp::dispatch_registrar(rec, registrar_event, now, port_.timers());
    auto const n = rec.registrar_ctx.notify;
    if (n == registrar_sm::Notify::New || n == registrar_sm::Notify::Join) {
        notify_domain(rec.first_value, Operation::Register);
    } else if (n == registrar_sm::Notify::Leave && was_registered) {
        notify_domain_leave(rec.first_value);
    }
}

// ============================================================
// PDU encode
// ============================================================

template <class Limits>
template <typename RecordVec>
auto MsrpParticipantT<Limits>::append_attribute_message(
    MutableBuffer& out, RecordVec& records, AttributeType attr_type, uint8_t attr_length, bool include_leave_all) -> bool
{
    // Count how many records actually want to tx in this pass. End-station
    // MAD-only participant: only attributes WE declared (operation==Declare) are
    // transmitted. Registered peer attributes (operation==Register) are tracked
    // by the Registrar -- so we learn remote streams and can send Listener Ready
    // -- but must never be re-declared on the wire. Re-declaring a registered
    // attribute is the MAP (propagation) behaviour of a bridge port, not an end
    // station: the observer applicant states (Ao/Qo) fire the *optional* sIn
    // (a_tx_in_optional), which on a shared medium refreshes a shared registrar
    // but on a point-to-point AVB link is pure noise -- and worse, makes the
    // bridge see a second source for that StreamID and reject the reservation
    // with TalkerFailed (SrClassPriorityMismatch). So we never emit Register
    // records; see the matching skip in the emit loop below.
    //
    // Sticky-Listener exception: when redeclare_registered_listeners_ is set,
    // registered Listener attributes ARE re-emitted (the cfea332 echo, for the
    // Listener type only -- safe because a Listener has no two-source conflict).
    // See set_redeclare_registered_listeners() for the rationale (node-e -> the DSP processor).
    bool const allow_register_redeclare = redeclare_registered_listeners_ && (attr_type == AttributeType::Listener);
    auto const may_emit = [&](auto const& rec) { return rec.operation == Operation::Declare || allow_register_redeclare; };
    // A run is a maximal sequence of records that (a) want to tx this pass, (b)
    // we may emit, and (c) whose FirstValues form a 35.1.11 increment chain --
    // each is the previous one passed through increment_first_value(). A run is
    // coalesced into ONE VectorAttribute with NumberOfValues == run length (one
    // ThreePackedEvent per value, plus one FourPackedDeclaration per value for
    // Listener). Records that cannot chain emit as singleton vectors.
    //
    // next_run(recs, from, out_len) returns the start index of the next run at or
    // after `from` (skipping not-pending / non-emittable records), or recs.size()
    // when none remain; out_len receives the run length.
    auto next_run = [&](auto const& recs, size_t from, size_t& out_len) -> size_t {
        size_t const n = recs.size();
        size_t i = from;
        while (i < n && !(recs[i].applicant_ctx.tx_pending && may_emit(recs[i]))) {
            ++i;
        }
        if (i >= n) {
            out_len = 0;
            return n;
        }
        size_t len = 1;
        auto expected = recs[i].first_value;
        increment_first_value(expected);
        size_t j = i + 1;
        while (j < n && recs[j].applicant_ctx.tx_pending && may_emit(recs[j]) && recs[j].first_value == expected) {
            ++len;
            increment_first_value(expected);
            ++j;
        }
        out_len = len;
        return i;
    };

    bool const is_listener_attr = (attr_type == AttributeType::Listener);

    // Pre-compute AttributeListLength (IEEE 802.1Q-2014 Clause 10.8.2.3): byte
    // count of the VectorAttributes plus the trailing EndMark (2 octets). A run
    // of length L costs: VectorHeader(2) + FirstValue(attr_length) +
    // ThreePackedEvents(ceil(L/3)) [+ FourPackedDeclarations(ceil(L/4)) for
    // Listener]. This run walk MUST match the emit loop below so the length
    // stays in sync with the bytes actually written.
    size_t attr_list_length = size_t{2};  // trailing EndMark
    size_t run_count = 0;
    for (size_t run_len = 0, s = next_run(records, 0, run_len); s < records.size(); s = next_run(records, s + run_len, run_len)) {
        ++run_count;
        attr_list_length += size_t{2} + size_t{attr_length} + mrp::threepacked_octet_count(run_len) +
            (is_listener_attr ? mrp::fourpacked_octet_count(run_len) : size_t{0});
    }

    // If no attributes want to tx and we don't need to broadcast a
    // LeaveAll, skip the whole message.
    if (run_count == 0 && !include_leave_all) {
        return false;
    }
    if (run_count == 0 && include_leave_all) {
        // Empty-vector LeaveAll: VectorHeader(2) only, no FirstValue / events.
        attr_list_length += size_t{2};
    }
    if (attr_list_length > 0xFFFFU) {
        return false;
    }

    // The WHOLE message must fit before we commit its header. The header writes
    // attribute_list_length up front; if a later vector append then hit the buffer
    // cap and we bailed mid-message, the header would promise more bytes than we
    // wrote and a receiver seeking by attribute_list_length would read the next
    // message as vector garbage. Message = attribute_type(1) + attribute_length(1) +
    // attribute_list_length(2) + attribute_list(attr_list_length); reserve 2 more
    // for the trailing PDU EndMark. If it won't fit, skip the whole message --
    // tx_pending stays set on its records, so MRP retransmits it in a later PDU.
    if ((size_t{4} + attr_list_length + size_t{2}) > out.available_space()) {
        ++pdu_message_skip_count_;
        return false;
    }

    // AttributeListHeader: attribute_type, attribute_length, attribute_list_length
    if (!append_u8(out, static_cast<uint8_t>(attr_type))) {
        return false;
    }
    if (!append_u8(out, attr_length)) {
        return false;
    }
    if (!append_be16(out, static_cast<uint16_t>(attr_list_length))) {
        return false;
    }

    // Clear stale tx flags on records we will NOT emit (registered peer
    // attributes whose observer applicant fired the optional sIn). next_run skips
    // them, so clear here to keep the flag from lingering into the next pass --
    // see the count-loop comment above for why an end station never re-declares a
    // registered attribute.
    for (auto& rec : records) {
        if (rec.applicant_ctx.tx_pending && !may_emit(rec)) {
            rec.applicant_ctx.tx_pending = false;
            rec.applicant_ctx.send_msg = applicant_sm::SendMessage::None;
            rec.applicant_ctx.encode = applicant_sm::Encoding::None;
        }
    }

    // Emit one VectorAttribute per run (coalescing increment chains), or a single
    // empty LeaveAll vector below if a LeaveAll is owed but nothing wants to tx.
    // The LeaveAll flag rides the first vector emitted.
    bool leave_all_emitted = !include_leave_all;
    bool any_vector_emitted = false;

    for (size_t run_len = 0, s = next_run(records, 0, run_len); s < records.size(); s = next_run(records, s + run_len, run_len)) {
        // VectorAttributeHeader (2 bytes): leave_all_flag | num_values=run_len
        VectorAttributeHeader vh{};
        vh.set(!leave_all_emitted, static_cast<uint16_t>(run_len));
        if (!append_be16(out, vh.vector_header.get())) {
            return any_vector_emitted;
        }
        // FirstValue: the run's base value. The receiver derives values 1..L-1 by
        // applying increment_first_value() per the matching ThreePackedEvent.
        std::array<uint8_t, 64> fv_buf{};  // 34 bytes is the largest (TalkerFailed)
        auto const fv_span = std::span<uint8_t>(fv_buf.data(), attr_length);
        (void)store_unchecked(fv_span, records[s].first_value);
        if (!static_cast<bool>(out.append(std::span<uint8_t const>(fv_span.data(), attr_length)))) {
            return any_vector_emitted;
        }
        // ThreePackedEvents: one AttributeEvent per value, packed 3 per octet
        // (unused trailing slots in the final octet are Mt fillers).
        auto event_at = [&](size_t k) -> AttributeEvent {
            if (k >= run_len) {
                return AttributeEvent::Mt;
            }
            auto const& rec = records[s + k];
            bool reg_in = false;
            if constexpr (requires { rec.registrar_is_in(); }) {
                reg_in = rec.registrar_is_in();
            }
            return detail_msrp::translate_sndmsg(rec.applicant_ctx.send_msg, reg_in);
        };
        for (size_t o = 0; o < run_len; o += 3) {
            if (!append_u8(out, mrp::pack3_events(event_at(o), event_at(o + 1), event_at(o + 2)))) {
                return any_vector_emitted;
            }
        }
        // FourPackedDeclarations (Listener only): one per value, packed 4 per
        // octet (unused trailing slots are 0 == Ignore fillers).
        if (is_listener_attr) {
            auto decl_at = [&](size_t k) -> uint8_t {
                if (k >= run_len) {
                    return 0;
                }
                auto const& rec = records[s + k];
                if constexpr (requires { rec.substate; }) {
                    return static_cast<uint8_t>(rec.substate);
                } else {
                    return 0;
                }
            };
            for (size_t o = 0; o < run_len; o += 4) {
                if (!append_u8(out, mrp::pack4_declarations(decl_at(o), decl_at(o + 1), decl_at(o + 2), decl_at(o + 3)))) {
                    return any_vector_emitted;
                }
            }
        }

        leave_all_emitted = true;
        any_vector_emitted = true;

        // Clear tx_pending across the whole run so the next build doesn't re-emit.
        for (size_t k = 0; k < run_len; ++k) {
            auto& rec = records[s + k];
            rec.applicant_ctx.tx_pending = false;
            rec.applicant_ctx.send_msg = applicant_sm::SendMessage::None;
            rec.applicant_ctx.encode = applicant_sm::Encoding::None;
        }
    }

    // If we needed to emit a LeaveAll but no attributes produced a tx,
    // synthesize an empty-vector LeaveAll. This is the "LeaveAll-only"
    // PDU case used on LvaTimer expiry when the database is empty.
    if (include_leave_all && !any_vector_emitted) {
        VectorAttributeHeader vh{};
        vh.set(true, 0);  // leave_all flag set, num_values = 0
        if (!append_be16(out, vh.vector_header.get())) {
            return false;
        }
    }

    // VectorAttribute list EndMark
    if (!append_be16(out, mrp::END_MARK)) {
        return any_vector_emitted || include_leave_all;
    }
    return true;
}

template <class Limits>
void MsrpParticipantT<Limits>::build_and_send_pdu(TimePoint now, bool leave_all)
{
    // First, dispatch the appropriate TX event to every attribute's
    // Applicant to generate tx outputs. We pick TxRegistrarIn vs
    // TxRegistrarMt per-attribute based on the paired Registrar's
    // state (Clause 10.7.7 Note 8).
    auto drive_tx = [&](auto& records) {
        for (auto& rec : records) {
            mrp::dispatch_applicant_tx(rec, now);
        }
    };
    drive_tx(talker_adv_);
    drive_tx(talker_failed_);
    drive_tx(listeners_);
    drive_tx(domains_);

    // Did the LeaveAll FSM ask us to tag this PDU? on_leaveall_timer passes the
    // leaveall_sm's tx_leaveall_pending here; the join/periodic TX paths pass
    // false. (Previously hardcoded false -> our LeaveAll was never transmitted.)
    //
    // The LeaveAll must ride the FIRST attribute type that actually re-declares a
    // record, so the flag is always carried by a non-empty vector (with a
    // FirstValue). Emitting a standalone EMPTY LeaveAll vector (NumberOfValues=0)
    // for a type we have no records of -- e.g. TalkerFailed on a pure talker --
    // is mis-parsed by some peers (some AVB switches) AND tshark: they read a
    // FirstValue for the 0-value vector and consume the following Listener/Domain
    // messages' bytes, DESTROYING the rest of the PDU. A LeaveAll PDU must
    // re-declare every active attribute in the same packet (IEEE 802.1Q
    // 10.7.6.2); losing the Domain re-declaration makes the bridge treat our port
    // as non-AVB and fail the talker with code 8. So: carry the LeaveAll on the
    // first re-declaring message, and never synthesize an empty LeaveAll vector
    // ahead of other messages.
    auto has_emittable = [](auto const& recs) {
        for (auto const& r : recs) {
            if (r.applicant_ctx.tx_pending && r.operation == Operation::Declare) {
                return true;
            }
        }
        return false;
    };
    bool la_remaining = leave_all;

    // Reuse the participant's pre-allocated PDU buffer; no per-tx heap.
    reset_buffer(pdu_buffer_);
    if (!append_u8(pdu_buffer_, PROTOCOL_VERSION)) {
        return;
    }

    bool any_message = false;
    auto emit_msg = [&](auto& recs, AttributeType type, uint8_t len) {
        bool const carry_la = la_remaining && has_emittable(recs);
        bool const emitted = append_attribute_message(pdu_buffer_, recs, type, len, carry_la);
        if (carry_la && emitted) {
            la_remaining = false;  // LeaveAll consumed by this non-empty message
        }
        any_message |= emitted;
    };
    emit_msg(talker_adv_, AttributeType::TalkerAdvertise, static_cast<uint8_t>(AttributeLength::TalkerAdvertise));
    emit_msg(talker_failed_, AttributeType::TalkerFailed, static_cast<uint8_t>(AttributeLength::TalkerFailed));
    emit_msg(listeners_, AttributeType::Listener, static_cast<uint8_t>(AttributeLength::Listener));
    emit_msg(domains_, AttributeType::Domain, static_cast<uint8_t>(AttributeLength::Domain));

    // Empty-database LeaveAll: we owe a LeaveAll but have nothing to re-declare.
    // A single standalone empty LeaveAll is safe here -- there are no later
    // messages in the PDU for a mis-parsing peer to run into.
    if (la_remaining && !any_message) {
        any_message |= append_attribute_message(
            pdu_buffer_, talker_adv_, AttributeType::TalkerAdvertise, static_cast<uint8_t>(AttributeLength::TalkerAdvertise), true);
    }

    if (!any_message) {
        return;
    }
    // Final message EndMark
    (void)append_be16(pdu_buffer_, mrp::END_MARK);

    if (send_pdu_) {
        (void)send_pdu_(pdu_buffer_.get_span());
    }

    (void)now;
}

// ============================================================
// Explicit instantiation
// ============================================================

template class MsrpParticipantT<DefaultMsrpLimits>;

}  // namespace statusbar::srp::msrp
