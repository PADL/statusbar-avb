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
#include <array>
#include <cstdio>  // stderr (diagnostic [srp-mrp] logging)
#include <optional>
#include <print>
#include <string_view>

namespace statusbar::srp::msrp {

// ============================================================
// Implementation — out-of-line template method definitions
// ============================================================

namespace detail_msrp {

/// A Registrar state for a log line.
inline auto registrar_state_name(registrar_sm::Def::State s) noexcept -> char const*
{
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
}

/// The @p value_index-th Listener FirstValue of a vector that starts with
/// the one encoded in @p first_value_bytes.
inline auto listener_first_value_at(std::span<uint8_t const> first_value_bytes, uint16_t value_index) -> ListenerFirstValue
{
    ListenerFirstValue fv;
    (void)protocol::load_unchecked(first_value_bytes, &fv);
    for (uint16_t k = 0; k < value_index; ++k) {
        increment_first_value(fv);
    }
    return fv;
}

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

/// The FirstValue length of an MSRP attribute type (IEEE 802.1Q Table 35-2),
/// or 0 for a type MSRP does not define.
constexpr auto first_value_length(AttributeType const type) noexcept -> size_t
{
    constexpr std::array<size_t, 5> lengths{
        0, TalkerAdvertiseFirstValue::LENGTH, TalkerFailedFirstValue::LENGTH, ListenerFirstValue::LENGTH, DomainFirstValue::LENGTH};
    auto const index = static_cast<size_t>(type);
    return index < lengths.size() ? lengths[index] : 0;
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
    , port_{rng_seed, config.timers}
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
    rearm_leave_timer();  // stop() dropped the slot; Registrars may still be in Lv
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
auto MsrpParticipantT<Limits>::find_registered_talker_advertise(tsn::StreamId const& id) const noexcept
    -> TalkerAdvertiseFirstValue const*
{
    for (auto const& rec : talker_adv_) {
        if (rec.first_value.stream_id == id && rec.registrar_is_registered()) {
            return &rec.first_value;
        }
    }
    return nullptr;
}

template <class Limits>
auto MsrpParticipantT<Limits>::talker_registered_in(tsn::StreamId const& id) const noexcept -> bool
{
    auto const advertise = std::ranges::any_of(
        talker_adv_, [&id](auto const& rec) { return rec.first_value.stream_id == id && rec.registrar_is_in(); });
    return advertise || std::ranges::any_of(talker_failed_, [&id](auto const& rec) {
               return rec.first_value.advertise.stream_id == id && rec.registrar_is_in();
           });
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_registered_talker_advertise(Eui48 const& destination, uint16_t const vid) const noexcept
    -> TalkerAdvertiseFirstValue const*
{
    for (auto const& rec : talker_adv_) {
        auto const& fv = rec.first_value;
        if (fv.destination_address == destination && fv.vlan_identifier.get() == vid && rec.registrar_is_registered()) {
            return &fv;
        }
    }
    return nullptr;
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_registered_talker_failed(Eui48 const& destination, uint16_t const vid) const noexcept
    -> TalkerFailedFirstValue const*
{
    for (auto const& rec : talker_failed_) {
        auto const& fv = rec.first_value.advertise;
        if (fv.destination_address == destination && fv.vlan_identifier.get() == vid && rec.registrar_is_registered()) {
            return &rec.first_value;
        }
    }
    return nullptr;
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_registered_talker_failed(tsn::StreamId const& id) const noexcept
    -> TalkerFailedFirstValue const*
{
    for (auto const& rec : talker_failed_) {
        if (rec.first_value.advertise.stream_id == id && rec.registrar_is_registered()) {
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
auto MsrpParticipantT<Limits>::listener_registered(tsn::StreamId const& stream_id) const noexcept -> bool
{
    // A record this port declares (its own Listener) is no peer's request.
    auto const* rec = find_listener(stream_id);
    return rec != nullptr && rec->operation == Operation::Register && rec->registrar_is_registered();
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
auto MsrpParticipantT<Limits>::talker_owned(Operation const op, tsn::StreamId const& id) const noexcept -> bool
{
    if (op == Operation::Declare) {
        return true;
    }
    return std::any_of(listeners_.begin(), listeners_.end(), [&id](ListenerRecord const& r) {
        return r.operation == Operation::Declare && r.first_value.stream_id == id;
    });
}

template <class Limits>
auto MsrpParticipantT<Limits>::listener_owned(Operation const op, tsn::StreamId const& id) const noexcept -> bool
{
    if (op == Operation::Declare) {
        return true;
    }
    return std::any_of(talker_adv_.begin(), talker_adv_.end(), [&id](auto const& r) {
        return r.operation == Operation::Declare && r.first_value.stream_id == id;
    });
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_talker_advertise(tsn::StreamId const& id, bool const local)
    -> AttributeRecord<TalkerAdvertiseFirstValue>*
{
    return find_or_create(
        talker_adv_,
        config_.max_talker_advertise,
        config_.max_talker_advertise - config_.reserved_talker_advertise,
        local,
        id,
        [](auto const& r) { return r.first_value.stream_id; },
        [](auto& r, auto const& k) { r.first_value.stream_id = k; },
        [this](Operation op, tsn::StreamId const& k) { return talker_owned(op, k); });
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_talker_failed(tsn::StreamId const& id, bool const local)
    -> AttributeRecord<TalkerFailedFirstValue>*
{
    return find_or_create(
        talker_failed_,
        config_.max_talker_failed,
        config_.max_talker_failed - config_.reserved_talker_failed,
        local,
        id,
        [](auto const& r) { return r.first_value.advertise.stream_id; },
        [](auto& r, auto const& k) { r.first_value.advertise.stream_id = k; },
        [this](Operation op, tsn::StreamId const& k) { return talker_owned(op, k); });
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_listener(tsn::StreamId const& id, bool const local) -> ListenerRecord*
{
    return find_or_create(
        listeners_,
        config_.max_listeners,
        config_.max_listeners - config_.reserved_listeners,
        local,
        id,
        [](auto const& r) { return r.first_value.stream_id; },
        [](auto& r, auto const& k) { r.first_value.stream_id = k; },
        [this](Operation op, tsn::StreamId const& k) { return listener_owned(op, k); });
}

template <class Limits>
auto MsrpParticipantT<Limits>::find_or_create_domain(uint8_t sr_class_id) -> AttributeRecord<DomainFirstValue>*
{
    return find_or_create(
        domains_,
        config_.max_domains,
        config_.max_domains,
        true,
        sr_class_id,
        [](auto const& r) { return r.first_value.sr_class_id.get(); },
        [](auto& r, auto const& k) { r.first_value.sr_class_id = k; },
        [](Operation, uint8_t) { return true; });
}

// ============================================================
// Local declaration API
// ============================================================

template <class Limits>
auto MsrpParticipantT<Limits>::declare_talker_advertise(TalkerAdvertiseFirstValue const& fv, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_talker_advertise(fv.stream_id, true);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    auto& rec = *rec_ptr;
    if (waits_out_the_old_value(rec, fv, now)) {
        rec.pending_first_value = fv;  // the old value has left: the newest one waits out the same two LeaveAll periods
    } else if (is_actively_declared(rec) && changes_the_attribute(rec.first_value, fv)) {
        change_declared_value(rec, fv, now);
    } else if (is_actively_declared(rec) && fv.accumulated_latency.get() > rec.first_value.accumulated_latency.get()) {
        return fail_talker_latency(rec, fv, now);
    } else {
        rec.pending_not_before.reset();  // the value that left is back, or its wait is over
        adopt_value(rec, fv, now);
    }
    port_.timers().start_join(now);
    return success();
}

/// IEEE 802.1Q 35.2.2.8: "there must be at least two LeaveAllTime ... time
/// periods between when the Talker removes the existing Stream registration
/// and declares the new". The wait is running (pending_not_before) and
/// @p fv is another attribute than the one that left: it waits, taking the
/// place of any value already waiting. The value that left may come back at
/// any time, a waiting change or not: it is the same registration.
template <class Limits>
auto MsrpParticipantT<Limits>::waits_out_the_old_value(
    AttributeRecord<TalkerAdvertiseFirstValue> const& rec, TalkerAdvertiseFirstValue const& fv, TimePoint now) noexcept -> bool
{
    if (!rec.pending_not_before.has_value() || !changes_the_attribute(rec.first_value, fv)) {
        return false;
    }
    return rec.pending_first_value.has_value() || now < *rec.pending_not_before;
}

template <class Limits>
auto MsrpParticipantT<Limits>::talker_value_hold() const noexcept -> std::chrono::milliseconds
{
    return 2 * port_.timers().config().leave_all_time_max;
}

/// The wait starts at the withdrawal, not at the Lv that follows it within a
/// JoinTime: the hold is two of the longest LeaveAll periods (30 s by Table
/// 10-7) where the standard asks for two LeaveAllTime (20 s), which covers
/// the difference many times over. A record with nothing of ours on the wire
/// (a change whose old value has already left, or nothing declared at all)
/// keeps the wait it has.
template <class Limits>
void MsrpParticipantT<Limits>::hold_other_values_after_withdrawal(
    AttributeRecord<TalkerAdvertiseFirstValue>& rec, TimePoint now) const noexcept
{
    if (is_actively_declared(rec)) {
        rec.pending_not_before = now + talker_value_hold();
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::forget_finished_holds(TimePoint now) noexcept
{
    for (auto& rec : talker_adv_) {
        if (rec.pending_not_before.has_value() && !rec.pending_first_value.has_value() && now >= *rec.pending_not_before) {
            rec.pending_not_before.reset();
        }
    }
}

/// IEEE 802.1Q 35.2.2.8.6: the latency a Talker Advertise reports "is not
/// intended to increase during the life of the reservation. If some event
/// occurs that would increase the latency beyond the original guarantee, MSRP
/// will change the Talker Advertise to a Talker Failed and report Failure
/// Code=7". The Advertise leaves and a Talker Failed carrying the new latency
/// is declared in its place; an end station names no bridge in the failure.
template <class Limits>
auto MsrpParticipantT<Limits>::fail_talker_latency(
    AttributeRecord<TalkerAdvertiseFirstValue>& rec, TalkerAdvertiseFirstValue const& fv, TimePoint now) -> Status
{
    rec.pending_first_value.reset();
    mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
    TalkerFailedFirstValue failed{};
    failed.advertise = fv;
    failed.failure_code = static_cast<uint8_t>(FailureCode::ReportedLatencyHasChanged);
    return declare_new_talker_failed(failed, now);
}

/// A Talker Failed the port has not declared before, told New!.
template <class Limits>
auto MsrpParticipantT<Limits>::declare_new_talker_failed(TalkerFailedFirstValue const& fv, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_talker_failed(fv.advertise.stream_id, true);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    declare_new_value(*rec_ptr, fv, now);
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::talker_change_pending(tsn::StreamId const& stream_id) const noexcept -> bool
{
    for (auto const& rec : talker_adv_) {
        if (rec.first_value.stream_id == stream_id) {
            return rec.pending_first_value.has_value();
        }
    }
    return false;
}

/// Whether @p next is another attribute than @p current under the same
/// StreamID: anything but AccumulatedLatency differs. AccumulatedLatency is
/// the one field 35.2.2.8.6 lets a Talker update in place.
template <class Limits>
auto MsrpParticipantT<Limits>::changes_the_attribute(
    TalkerAdvertiseFirstValue const& current, TalkerAdvertiseFirstValue const& next) noexcept -> bool
{
    auto latency_moved = current;
    latency_moved.accumulated_latency = next.accumulated_latency;
    return !(latency_moved == next);
}

/// Whether the record's value is ours on the wire, or about to be: a
/// declaration the applicant is still asserting (Vn/An/Aa/Qa/Vp/Ap/Qp) or
/// has yet to leave (La). An observer or idle applicant has nothing of ours
/// out there for a changed value to collide with.
template <class Limits>
template <class FirstValue>
auto MsrpParticipantT<Limits>::is_actively_declared(AttributeRecord<FirstValue> const& rec) noexcept -> bool
{
    using S = applicant_sm::Def::State;
    auto const s = rec.applicant_sm.current_state();
    return rec.operation == Operation::Declare &&
        (s == S::Vn || s == S::An || s == S::Aa || s == S::Qa || s == S::Vp || s == S::Ap || s == S::Qp || s == S::La);
}

/// The record takes @p fv as a value never declared before: New!.
template <class Limits>
template <class FirstValue>
void MsrpParticipantT<Limits>::declare_new_value(AttributeRecord<FirstValue>& rec, FirstValue const& fv, TimePoint now)
{
    rec.first_value = fv;
    rec.operation = Operation::Declare;
    rec.pending_first_value.reset();
    mrp::dispatch_applicant(rec, applicant_sm::Def::Event::New, now);
}

/// A first declaration, a same-value re-declaration or (for a Talker
/// Advertise) an AccumulatedLatency update: the value goes into the record in
/// place and the applicant is told New! or Join! as mark_declared decides.
template <class Limits>
template <class FirstValue>
void MsrpParticipantT<Limits>::adopt_value(AttributeRecord<FirstValue>& rec, FirstValue const& fv, TimePoint now)
{
    rec.first_value = fv;
    rec.pending_first_value.reset();
    mrp::dispatch_applicant(rec, mark_declared(rec), now);
}

/// A changed value under a declared key (a Talker Advertise's StreamID, a
/// Domain's SRclassID): leave the old one first. When the applicant now owes
/// an Lv (La), the new value waits on the record until that Lv has been
/// encoded (complete_pending_changes); when it owed nothing (Vp/Ap/Qp went to
/// Vo/Ao/Qo: our Join was never sent, or a LeaveAll already cleared it), the
/// new value is declared at once.
template <class Limits>
template <class FirstValue>
void MsrpParticipantT<Limits>::change_declared_value(AttributeRecord<FirstValue>& rec, FirstValue const& fv, TimePoint now)
{
    mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
    if (rec.applicant_sm.current_state() == applicant_sm::Def::State::La) {
        rec.pending_first_value = fv;
        return;
    }
    declare_new_value(rec, fv, now);
}

/// After a PDU has been built: a record whose Lv went out in it (La + tx! is
/// Vo; a message skipped for space put its Applicants back in La, see
/// transmit_attribute_type, and waits) takes its pending value and declares
/// it New, once @p hold has passed since that Lv. A LeaveAll of ours in
/// between (La + txLA! is Lo, with no Lv owed) completes the change the same
/// way. Returns whether any record did.
template <class Limits>
template <class Table>
auto MsrpParticipantT<Limits>::complete_value_changes(Table& table, std::chrono::milliseconds const hold, TimePoint now) -> bool
{
    bool any = false;
    for (auto& rec : table) {
        if (!change_may_complete(rec, hold, now)) {
            continue;
        }
        rec.pending_not_before.reset();
        declare_new_value(rec, *rec.pending_first_value, now);
        any = true;
    }
    return any;
}

/// Whether the record has a new value waiting whose old one has left the wire
/// at least @p hold ago. The wait starts at the first pass that finds the Lv
/// gone.
template <class Limits>
template <class Record>
auto MsrpParticipantT<Limits>::change_may_complete(Record& rec, std::chrono::milliseconds const hold, TimePoint now) -> bool
{
    bool const lv_still_owed = rec.applicant_ctx.tx_pending || rec.applicant_sm.current_state() == applicant_sm::Def::State::La;
    if (!rec.pending_first_value.has_value() || lv_still_owed) {
        return false;
    }
    if (!rec.pending_not_before.has_value()) {
        rec.pending_not_before = now + hold;
    }
    return now >= *rec.pending_not_before;
}

/// The changes a PDU's going out has settled. A Domain's new value follows
/// its Lv at once. A Talker Advertise's waits two LeaveAll periods: IEEE
/// 802.1Q 35.2.2.8, "there must be at least two LeaveAllTime ... time periods
/// between when the Talker removes the existing Stream registration and
/// declares the new", which Milan v1.3 5.3.7.4 repeats for its talkers.
template <class Limits>
void MsrpParticipantT<Limits>::complete_pending_changes(TimePoint now)
{
    bool const talkers = complete_value_changes(talker_adv_, talker_value_hold(), now);
    bool const domains = complete_value_changes(domains_, std::chrono::milliseconds{0}, now);
    forget_finished_holds(now);
    if (talkers || domains) {
        port_.timers().start_join(now);
    }
}

template <class Limits>
auto MsrpParticipantT<Limits>::declare_talker_failed(TalkerFailedFirstValue const& fv, TimePoint now) -> Status
{
    auto* rec_ptr = find_or_create_talker_failed(fv.advertise.stream_id, true);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    adopt_value(*rec_ptr, fv, now);
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::withdraw_talker(tsn::StreamId const& stream_id, TimePoint now) -> Status
{
    bool found = false;
    for (auto& rec : talker_adv_) {
        if (rec.first_value.stream_id == stream_id) {
            rec.pending_first_value.reset();  // a withdrawal cancels a change in flight
            hold_other_values_after_withdrawal(rec, now);
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
    auto* rec_ptr = find_or_create_listener(stream_id, true);
    if (rec_ptr == nullptr) {
        return failure(make_error_code(tsn::TsnError::attribute_table_full));
    }
    declare_listener_type(*rec_ptr, decl, now);
    port_.timers().start_join(now);
    return success();
}

/// A Listener's Declaration Type is not part of its FirstValue (the StreamID
/// alone): it rides the FourPackedEvents of the one attribute. A change of
/// type under a StreamID this port already declares is a Join carrying the
/// new FourPackedEvent, as IEEE 802.1Q 35.2.6 describes it: the receiver
/// behaves as though the old registration left and the new one joined, which
/// "allows an Applicant to indicate a change in a stream reservation ...
/// without having to issue both a withdrawal of the old attribute, and a
/// declaration of the new". Only a declaration the port was not making is New.
template <class Limits>
void MsrpParticipantT<Limits>::declare_listener_type(ListenerRecord& rec, ListenerDeclaration decl, TimePoint now)
{
    auto const event = mark_declared(rec);
    rec.substate = decl;
    rec.applicant_ctx.clear_outputs();
    rec.applicant_sm.handle_event(rec.applicant_ctx, event, now);
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
    // A Domain is identified by its SRclassID; another priority or VID under
    // the same id is another attribute (10.7), left before it is declared.
    auto& rec = *rec_ptr;
    if (is_actively_declared(rec) && !(rec.first_value == fv)) {
        change_declared_value(rec, fv, now);
    } else {
        adopt_value(rec, fv, now);
    }
    port_.timers().start_join(now);
    return success();
}

template <class Limits>
auto MsrpParticipantT<Limits>::domain_change_pending(uint8_t const sr_class_id) const noexcept -> bool
{
    for (auto const& rec : domains_) {
        if (rec.first_value.sr_class_id.get() == sr_class_id) {
            return rec.pending_first_value.has_value();
        }
    }
    return false;
}

template <class Limits>
auto MsrpParticipantT<Limits>::withdraw_domain(uint8_t sr_class_id, TimePoint now) -> Status
{
    for (auto& rec : domains_) {
        if (rec.first_value.sr_class_id.get() == sr_class_id) {
            rec.pending_first_value.reset();  // a withdrawal cancels a change in flight
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
            port_.timers().start_join(now);
            return success();
        }
    }
    return success();
}

template <class Limits>
void MsrpParticipantT<Limits>::forget_registered_domains()
{
    detail_msrp::erase_if(domains_, [](auto const& rec) { return rec.operation == Operation::Register; });
    for (auto& rec : domains_) {
        rec.registrar_sm.reset();  // BEGIN!: Mt once the next event runs the UCT chain
        rec.registrar_ctx.clear_outputs();
        rec.leave_deadline.reset();
    }
    rearm_leave_timer();
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
    run_expired_timers(expired, now);
    reclaim();
    request_transmit_if_owed(now);
}

template <class Limits>
void MsrpParticipantT<Limits>::run_expired_timers(mrp::TimerScheduler::Expired const& expired, TimePoint now)
{
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
}

template <class Limits>
void MsrpParticipantT<Limits>::request_transmit_if_owed(TimePoint now)
{
    auto const owes = [](auto const& table) {
        return std::ranges::any_of(table, [](auto const& rec) {
            return rec.operation == Operation::Declare && applicant_sm::Def::wants_transmit(rec.applicant_sm.current_state());
        });
    };
    if (owes(talker_adv_) || owes(talker_failed_) || owes(listeners_) || owes(domains_)) {
        port_.timers().start_join(now);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::on_join_timer(TimePoint now)
{
    build_and_send_pdu(now);
}

template <class Limits>
void MsrpParticipantT<Limits>::transmit_pending(TimePoint now)
{
    build_and_send_pdu(now);
}

template <class Limits>
void MsrpParticipantT<Limits>::on_leave_timer(TimePoint now)
{
    // The earliest Registrar leavetimer has expired. Fire LvTimer into each
    // Registrar whose OWN timer is due (10.7.4.2 runs one per Registrar): it
    // goes Lv -> Mt and notifies Leave. One that entered Lv later, in this
    // same tick's LeaveAll included, keeps its full LeaveTime.
    auto drive = [&](auto& records, auto&& notify_leave_id) {
        for (auto& rec : records) {
            if (!mrp::leave_due(rec, now)) {
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
    rearm_leave_timer();
}

template <class Limits>
void MsrpParticipantT<Limits>::rearm_leave_timer() noexcept
{
    std::optional<TimePoint> earliest{};
    mrp::fold_earliest_leave(talker_adv_, earliest);
    mrp::fold_earliest_leave(talker_failed_, earliest);
    mrp::fold_earliest_leave(listeners_, earliest);
    mrp::fold_earliest_leave(domains_, earliest);
    mrp::rearm_leave(port_.timers(), earliest);
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
    notify_all(&Observer::on_talker_advertise, fv, op);
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_talker_failed(TalkerFailedFirstValue const& fv, Operation op)
{
    notify_all(&Observer::on_talker_failed, fv, op);
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_talker_leave(tsn::StreamId const& id)
{
    notify_all(&Observer::on_talker_leave, id);
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_listener(tsn::StreamId const& id, ListenerDeclaration decl, Operation op)
{
    notify_all(&Observer::on_listener, id, decl, op);
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_listener_leave(tsn::StreamId const& id)
{
    notify_all(&Observer::on_listener_leave, id);
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_domain(DomainFirstValue const& fv, Operation op)
{
    notify_all(&Observer::on_domain, fv, op);
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_domain_leave(DomainFirstValue const& fv)
{
    notify_all(&Observer::on_domain_leave, fv);
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
    if (pdu.size() < 3 || pdu[0] != PROTOCOL_VERSION) {  // version + EndMark at least; another version is not ours
        return;
    }
    rx_leaveall_seen_ = false;
    decode_messages(pdu, now);
    finish_receive(now);
}

/// The Messages of one MSRPDU in order, up to the PDU's EndMark. Milan v1.3
/// 4.2.7.1.2: what precedes an invalid field has been processed; what follows
/// it in the same AttributeList is discarded, and so is every later Message.
template <class Limits>
void MsrpParticipantT<Limits>::decode_messages(std::span<uint8_t const> pdu, TimePoint now)
{
    size_t pos = 1;
    bool valid = true;
    while (valid && pos + 2 <= pdu.size() && mrp::read_doublet_at(pdu, pos) != mrp::END_MARK) {
        valid = decode_message(pdu, pos, now);
    }
}

/// The header of the Message at @p pos, or nullopt when the header or the
/// AttributeListLength it gives (IEEE 802.1Q 10.8.2.3: the vectors and their
/// EndMark) runs past the PDU.
template <class Limits>
auto MsrpParticipantT<Limits>::parse_message_header(std::span<uint8_t const> pdu, size_t const pos) -> std::optional<RxMessage>
{
    if (pos + AttributeListHeader::LENGTH + 2 > pdu.size()) {
        return std::nullopt;
    }
    AttributeListHeader hdr;
    (void)load_unchecked(pdu.subspan(pos), &hdr);
    size_t const list_begin = pos + AttributeListHeader::LENGTH + 2;
    RxMessage const message{
        .type = static_cast<AttributeType>(hdr.attribute_type.get()),
        .attr_length = hdr.attribute_length.get(),
        .list_begin = list_begin,
        .list_end = list_begin + mrp::read_doublet_at(pdu, pos + AttributeListHeader::LENGTH)};
    return message.list_end > pdu.size() ? std::nullopt : std::optional<RxMessage>{message};
}

/// One Message. @p pos moves to the next one by the AttributeListLength,
/// whatever the list decoder consumed. False when the Message is invalid.
template <class Limits>
auto MsrpParticipantT<Limits>::decode_message(std::span<uint8_t const> pdu, size_t& pos, TimePoint now) -> bool
{
    auto const message = parse_message_header(pdu, pos);
    if (!message) {
        return false;
    }
    pos = message->list_end;
    return decode_attribute_list(*message, pdu.first(message->list_end), now);
}

/// The VectorAttributes of one Message up to the list's EndMark. A type MSRP
/// does not define is passed over whole; a known type whose AttributeLength
/// is short of its FirstValue, a vector that overruns the list and a list
/// that ends without its EndMark are invalid.
template <class Limits>
auto MsrpParticipantT<Limits>::decode_attribute_list(RxMessage const& message, std::span<uint8_t const> payload, TimePoint now)
    -> bool
{
    size_t const first_value_length = detail_msrp::first_value_length(message.type);
    if (first_value_length == 0 || message.attr_length < first_value_length) {
        return first_value_length == 0;
    }
    size_t pos = message.list_begin;
    bool valid = true;
    while (valid && pos + 2 <= payload.size() && mrp::read_doublet_at(payload, pos) != mrp::END_MARK) {
        valid = decode_vector_attribute(message, payload, pos, now);
    }
    return valid && pos + 2 <= payload.size();
}

/// One VectorAttribute at @p pos: its header, the LeaveAll it may carry, then
/// its FirstValue and packed events. False when it overruns the list or holds
/// an event value outside the six.
template <class Limits>
auto MsrpParticipantT<Limits>::decode_vector_attribute(
    RxMessage const& message, std::span<uint8_t const> payload, size_t& pos, TimePoint now) -> bool
{
    VectorAttributeHeader vec_hdr;
    (void)load_unchecked(payload.subspan(pos), &vec_hdr);
    pos += VectorAttributeHeader::LENGTH;
    uint16_t const num_values = vec_hdr.get_number_of_values();
    if (vec_hdr.get_leave_all()) {
        receive_leaveall(message.type, num_values, now);
    }
    return decode_vector_values(message, num_values, payload, pos, now);
}

/// A LeaveAll in a vector's header applies to every attribute of the
/// Message's type on this port, and restarts our own leavealltimer.
template <class Limits>
void MsrpParticipantT<Limits>::receive_leaveall(AttributeType const type, uint16_t const num_values, TimePoint now)
{
    rx_leaveall_seen_ = true;  // finish_receive re-declares at once (see there)
    // Diagnostic: a LeaveAll on the Listener type drives EVERY listener
    // registrar to Leaving (registrar_in -> false) until the next Join
    // re-registers it -- a prime suspect for the listener-ready flap.
    if (logger_ && type == AttributeType::Listener) {
        logger_->debug("msrp: Listener LeaveAll (num_values={}) -> all listener registrars leave", num_values);
    }
    for_each_record_of(type, [&](auto& rec) {
        mrp::dispatch_applicant(rec, applicant_sm::Def::Event::RLeaveAll, now);
        mrp::dispatch_registrar(rec, registrar_sm::Def::Event::RLeaveAll, now, port_.timers());
    });
    // Also ack the LeaveAll into our LeaveAll FSM to restart its timer.
    port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::RLeaveAll, now);
}

/// The FirstValue, ThreePackedEvents and (for a Listener) FourPackedEvents of
/// one vector of @p num_values values; @p pos moves past them.
template <class Limits>
auto MsrpParticipantT<Limits>::decode_vector_values(
    RxMessage const& message, uint16_t const num_values, std::span<uint8_t const> payload, size_t& pos, TimePoint now) -> bool
{
    size_t const event_octets = mrp::threepacked_octet_count(num_values);
    size_t const decl_octets = (message.type == AttributeType::Listener) ? mrp::fourpacked_octet_count(num_values) : 0;
    if (pos + message.attr_length + event_octets + decl_octets > payload.size()) {
        return false;
    }
    RxVector const vector{
        .first_value = payload.subspan(pos, message.attr_length),
        .events = payload.subspan(pos + message.attr_length, event_octets),
        .declarations = payload.subspan(pos + message.attr_length + event_octets, decl_octets)};
    pos += message.attr_length + event_octets + decl_octets;
    return deliver_vector_values(message, num_values, vector, now);
}

/// Each value of one vector to the machines, in order. An AttributeEvent
/// outside the six the standard defines is an invalid field: the values
/// before it have been delivered, and nothing after it is.
template <class Limits>
auto MsrpParticipantT<Limits>::deliver_vector_values(
    RxMessage const& message, uint16_t const num_values, RxVector const& vector, TimePoint now) -> bool
{
    for (uint16_t i = 0; i < num_values; ++i) {
        AttributeEvent const event = mrp::nth_of_three(mrp::unpack3_events(vector.events[i / 3]), i);
        if (detail_msrp::rx_events_for(event).applicant == applicant_sm::Def::Event::Count) {
            return false;
        }
        uint8_t const decl =
            vector.declarations.empty() ? uint8_t{0} : mrp::nth_of_four(mrp::unpack4_declarations(vector.declarations[i / 4]), i);
        handle_rx_event(
            message.type, message.attr_length, vector.first_value, i, event, static_cast<ListenerDeclaration>(decl), now);
    }
    return true;
}

template <class Limits>
void MsrpParticipantT<Limits>::finish_receive(TimePoint now)
{
    // A received LeaveAll asked everyone to re-declare. Do it SYNCHRONOUSLY here
    // rather than only arming the join timer: the bridge that sent the
    // LeaveAll declares our (now-leaving) attributes as Mt toward downstream
    // listeners on its own join timer, and a re-declare that lands just
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
    handle_peer_attr_rx<TalkerAdvertiseFirstValue>(
        attr_length,
        first_value_bytes,
        value_index,
        applicant_event,
        registrar_event,
        now,
        [this](TalkerAdvertiseFirstValue const& fv) { return is_interesting(fv.stream_id); },
        [this](TalkerAdvertiseFirstValue const& fv) { return find_or_create_talker_advertise(fv.stream_id, false); },
        [this](TalkerAdvertiseFirstValue const& fv) { notify_talker_advertise(fv, Operation::Register); },
        [this](TalkerAdvertiseFirstValue const& fv) { notify_talker_leave(fv.stream_id); });
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
    handle_peer_attr_rx<TalkerFailedFirstValue>(
        attr_length,
        first_value_bytes,
        value_index,
        applicant_event,
        registrar_event,
        now,
        [this](TalkerFailedFirstValue const& fv) { return is_interesting(fv.advertise.stream_id); },
        [this](TalkerFailedFirstValue const& fv) { return find_or_create_talker_failed(fv.advertise.stream_id, false); },
        [this](TalkerFailedFirstValue const& fv) { notify_talker_failed(fv, Operation::Register); },
        [this](TalkerFailedFirstValue const& fv) { notify_talker_leave(fv.advertise.stream_id); });
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
    auto const fv = detail_msrp::listener_first_value_at(first_value_bytes, value_index);
    auto* rec_ptr = find_or_create_listener(fv.stream_id, false);
    if (rec_ptr == nullptr) {
        return;  // peer attribute, table full — silently drop
    }
    auto const before = adopt_listener_rx(*rec_ptr, fv, decl);
    drive_listener_rx(*rec_ptr, before, applicant_event, registrar_event, decl, wire_event, now);
}

template <class Limits>
auto MsrpParticipantT<Limits>::adopt_listener_rx(ListenerRecord& rec, ListenerFirstValue const& fv, ListenerDeclaration decl)
    -> ListenerRxBefore
{
    ListenerRxBefore const before{
        .state = rec.registrar_sm.current_state(), .was_registered = rec.registrar_is_registered(), .substate = rec.substate};
    // Adopt the received listener declaration ONLY for a pure peer registration. If
    // WE declare this StreamID, keep OUR substate (first_value is just the StreamID
    // here; substate is the payload) so our next JoinIn re-advertises our own.
    if (rec.operation == Operation::Register) {
        rec.first_value = fv;
        rec.substate = decl;
    } else if (rec.substate != decl) {
        ++foreign_declaration_conflict_count_;
    }
    return before;
}

template <class Limits>
void MsrpParticipantT<Limits>::drive_listener_rx(
    ListenerRecord& rec,
    ListenerRxBefore const& before,
    applicant_sm::Def::Event applicant_event,
    registrar_sm::Def::Event registrar_event,
    ListenerDeclaration decl,
    AttributeEvent wire_event,
    TimePoint now)
{
    mrp::dispatch_applicant(rec, applicant_event, now);
    if (registrar_event == registrar_sm::Def::Event::Count) {
        return;
    }
    drive_registrar(rec, registrar_event, now);
    log_listener_rx(rec, before.state, decl, wire_event);
    notify_listener_rx(rec, before, decl);
}

template <class Limits>
void MsrpParticipantT<Limits>::log_listener_rx(
    ListenerRecord const& rec, registrar_sm::Def::State state_before, ListenerDeclaration decl, AttributeEvent wire_event)
{
    // Diagnostic: a Listener attribute received for one of OUR talker streams --
    // print the wire MRP event and the registrar STATE transition, to trace what
    // drives the listener-ready flap ([srp-gate]). Seeing In<->Lv = a routine
    // LeaveAll refresh (benign once the gate accepts Lv); reaching Mt = the peer
    // truly de-registered (re-declaration / relay problem). `is_interesting`
    // keeps this to our streams.
    if (logger_ && logger_->enabled(logging::LogLevel::Debug) && is_interesting(rec.first_value.stream_id)) {
        logger_->debug(
            "msrp: listener rx sid={:016x} wire_event={} decl={} reg {}->{} lvtimer={}",
            rec.first_value.stream_id.to_uint64(),
            logging::static_str(attribute_event_name(wire_event)),
            logging::static_str(listener_declaration_name(decl)),
            logging::static_str(detail_msrp::registrar_state_name(state_before)),
            logging::static_str(detail_msrp::registrar_state_name(rec.registrar_sm.current_state())),
            rec.registrar_ctx.lvtimer_request);
    }
}

template <class Limits>
void MsrpParticipantT<Limits>::notify_listener_rx(
    ListenerRecord const& rec, ListenerRxBefore const& before, ListenerDeclaration decl)
{
    // One indication per change, as for the other attribute types (see
    // notify_peer_rx): registered, a registered Declaration Type that changed
    // (IEEE 802.1Q 35.2.6, with no Leave between), and the registration ended.
    bool const registered = rec.registrar_is_registered();
    if (registered && (!before.was_registered || before.substate != decl)) {
        notify_listener(rec.first_value.stream_id, decl, Operation::Register);
    } else if (!registered && before.was_registered) {
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
    // Domain has no interesting-stream filter, so it passes an always-true predicate.
    // Multi-value Domain vectors increment SRclassID and SRclassPriority per value
    // (IEEE 802.1Q 35.2.2.9; AVnu MSRP.End.c.35.1.11 Part B), handled by
    // increment_first_value inside the shared spine; the VID is carried unchanged.
    handle_peer_attr_rx<DomainFirstValue>(
        attr_length,
        first_value_bytes,
        value_index,
        applicant_event,
        registrar_event,
        now,
        [](DomainFirstValue const&) { return true; },
        [this](DomainFirstValue const& fv) { return find_or_create_domain(fv.sr_class_id.get()); },
        [this](DomainFirstValue const& fv) { notify_domain(fv, Operation::Register); },
        [this](DomainFirstValue const& fv) { notify_domain_leave(fv); });
}

// ============================================================
// PDU encode
// ============================================================

/// One attribute type's message into @p out. A message that would not fit whole
/// is skipped and counted in pdu_message_skip_count_; append_or_roll_back sees
/// the count move and puts the records' Applicants back as the pass found them,
/// so the next pass dispatches tx! to them again (see transmit_attribute_type).
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

    // The byte cost of one emitted run of length L, and the code that actually emits
    // it, are defined as a pair right here so the size pass and the emit pass below
    // cannot drift out of sync (the classic "two loops that must match" hazard). A run
    // costs: VectorHeader(2) + FirstValue(attr_length) + ThreePackedEvents(ceil(L/3))
    // [+ FourPackedDeclarations(ceil(L/4)) for Listener]. emit_run writes exactly that.
    auto const run_encoded_size = [&](size_t run_len) -> size_t {
        return size_t{2} + size_t{attr_length} + mrp::threepacked_octet_count(run_len) +
            (is_listener_attr ? mrp::fourpacked_octet_count(run_len) : size_t{0});
    };

    // Emit one VectorAttribute for the run [s, s+run_len): the VectorAttributeHeader
    // (leave_all_flag | num_values), the run's base FirstValue, the ThreePacked event
    // octets (one AttributeEvent per value, Mt fillers in the last octet), and -- for
    // Listener only -- the FourPacked declaration octets (Ignore fillers). Returns
    // false if any append hit the buffer cap. Must write exactly run_encoded_size(L).
    auto emit_run = [&](size_t s, size_t run_len, bool leave_all_flag) -> bool {
        VectorAttributeHeader vh{};
        vh.set(leave_all_flag, static_cast<uint16_t>(run_len));
        if (!append_be16(out, vh.vector_header.get())) {
            return false;
        }
        std::array<uint8_t, 64> fv_buf{};  // 34 bytes is the largest (TalkerFailed)
        auto const fv_span = std::span<uint8_t>(fv_buf.data(), attr_length);
        (void)store_unchecked(fv_span, records[s].first_value);
        if (!static_cast<bool>(out.append(std::span<uint8_t const>(fv_span.data(), attr_length)))) {
            return false;
        }
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
                return false;
            }
        }
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
                    return false;
                }
            }
        }
        return true;
    };

    // Pre-compute AttributeListLength (IEEE 802.1Q-2014 Clause 10.8.2.3): byte count of
    // the VectorAttributes plus the trailing EndMark (2 octets), summing run_encoded_size
    // over the same runs the emit pass will write.
    size_t attr_list_length = size_t{2};  // trailing EndMark
    size_t run_count = 0;
    for (size_t run_len = 0, s = next_run(records, 0, run_len); s < records.size(); s = next_run(records, s + run_len, run_len)) {
        ++run_count;
        attr_list_length += run_encoded_size(run_len);
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
        // The LeaveAll flag rides the first vector emitted.
        if (!emit_run(s, run_len, !leave_all_emitted)) {
            return any_vector_emitted;
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

/// One transmit pass: the PDU, then whatever the PDU's going out has
/// settled. A changed Talker Advertise whose Lv the pass encoded declares
/// its new value now, to ride the next PDU.
template <class Limits>
void MsrpParticipantT<Limits>::build_and_send_pdu(TimePoint now, bool leave_all)
{
    encode_and_send_pdu(now, leave_all);
    complete_pending_changes(now);
}

namespace {
/// Whether any record of @p recs is about to re-declare something. The
/// LeaveAll flag must ride a non-empty vector (with a FirstValue): a
/// standalone EMPTY LeaveAll vector (NumberOfValues=0) for a type we have no
/// records of -- e.g. TalkerFailed on a pure talker -- is mis-parsed by some
/// peers (some AVB switches) AND tshark: they read a FirstValue for the
/// 0-value vector and consume the following Listener/Domain messages' bytes,
/// DESTROYING the rest of the PDU. A LeaveAll PDU must re-declare every
/// active attribute in the same packet (IEEE 802.1Q 10.7.6.2); losing the
/// Domain re-declaration makes the bridge treat our port as non-AVB and fail
/// the talker with code 8.
template <class Table>
auto has_emittable(Table const& recs) -> bool
{
    for (auto const& r : recs) {
        if (r.applicant_ctx.tx_pending && r.operation == Operation::Declare) {
            return true;
        }
    }
    return false;
}

/// The Applicant state of every record of @p recs, in order, into @p out.
template <class Table, class Saved>
void save_applicants(Table const& recs, Saved& out)
{
    out.clear();
    for (auto const& rec : recs) {
        out.push_back(rec.applicant_sm);
    }
}

/// Every record of @p recs back to the Applicant state @p saved holds for it,
/// with no transmit outputs: as if this pass had never given it tx!.
template <class Table, class Saved>
void restore_applicants_of(Table& recs, Saved const& saved)
{
    for (size_t i = 0; i < recs.size() && i < saved.size(); ++i) {
        recs[i].applicant_sm = saved[i];
        recs[i].applicant_ctx.clear_outputs();
    }
}
}  // namespace

template <class Limits>
void MsrpParticipantT<Limits>::encode_and_send_pdu(TimePoint now, bool leave_all)
{
    if (!leave_all && !transmit_owed()) {
        return;
    }
    ApplicantSnapshot const before = snapshot_applicants();
    bool la_remaining = false;
    bool const any_message = transmit_pass_shedding_optional(before, leave_all, now, la_remaining);
    finish_pdu(any_message, la_remaining);
}

namespace {
/// Whether an Applicant of @p recs is in a state that sends a message on tx!.
template <class Table>
auto any_wants_transmit(Table const& recs) noexcept -> bool
{
    return std::ranges::any_of(
        recs, [](auto const& rec) { return applicant_sm::Def::wants_transmit(rec.applicant_sm.current_state()); });
}
}  // namespace

template <class Limits>
auto MsrpParticipantT<Limits>::transmit_owed() const noexcept -> bool
{
    return any_wants_transmit(talker_adv_) || any_wants_transmit(talker_failed_) || any_wants_transmit(listeners_) ||
        any_wants_transmit(domains_);
}

template <class Limits>
auto MsrpParticipantT<Limits>::snapshot_applicants() const -> ApplicantSnapshot
{
    ApplicantSnapshot snap{};
    save_applicants(talker_adv_, snap.talker_adv);
    save_applicants(talker_failed_, snap.talker_failed);
    save_applicants(listeners_, snap.listeners);
    save_applicants(domains_, snap.domains);
    return snap;
}

template <class Limits>
void MsrpParticipantT<Limits>::restore_applicants(ApplicantSnapshot const& before)
{
    restore_applicants_of(talker_adv_, before.talker_adv);
    restore_applicants_of(talker_failed_, before.talker_failed);
    restore_applicants_of(listeners_, before.listeners);
    restore_applicants_of(domains_, before.domains);
}

/// The transmit pass, twice at most. First with every encoding; when a message
/// did not fit, again from the same Applicant states with the optional
/// encodings left out (Table 10-3's [sJ] and [sIn]: the settled talkers'
/// JoinIn refreshes, which would otherwise take the whole PDU every pass and
/// starve a required Leave for ever), so the required ones have the room. A
/// message that does not fit even then has its Applicants rolled back and
/// waits for the next join period.
template <class Limits>
auto MsrpParticipantT<Limits>::transmit_pass_shedding_optional(
    ApplicantSnapshot const& before, bool leave_all, TimePoint now, bool& la_remaining) -> bool
{
    PassResult const full = transmit_pass(before, leave_all, now, /*shed_optional=*/false, la_remaining);
    if (!full.skipped) {
        return full.any_message;
    }
    restore_applicants(before);
    PassResult const required = transmit_pass(before, leave_all, now, /*shed_optional=*/true, la_remaining);
    if (required.skipped) {
        port_.timers().start_join(now);
    }
    return required.any_message;
}

/// One message per attribute type, each type's Applicants given tx! as its
/// message is built, in the PDU's order. The LeaveAll flag rides the first
/// message that re-declares a record (see has_emittable).
template <class Limits>
auto MsrpParticipantT<Limits>::transmit_pass(
    ApplicantSnapshot const& before, bool leave_all, TimePoint now, bool shed_optional, bool& la_remaining) -> PassResult
{
    la_remaining = leave_all;
    size_t const skips_before = pdu_message_skip_count_;
    bool any_message = false;
    if (begin_pdu()) {
        any_message |= transmit_attribute_type(
            talker_adv_,
            before.talker_adv,
            AttributeType::TalkerAdvertise,
            static_cast<uint8_t>(AttributeLength::TalkerAdvertise),
            la_remaining,
            now,
            shed_optional);
        any_message |= transmit_attribute_type(
            talker_failed_,
            before.talker_failed,
            AttributeType::TalkerFailed,
            static_cast<uint8_t>(AttributeLength::TalkerFailed),
            la_remaining,
            now,
            shed_optional);
        any_message |= transmit_attribute_type(
            listeners_,
            before.listeners,
            AttributeType::Listener,
            static_cast<uint8_t>(AttributeLength::Listener),
            la_remaining,
            now,
            shed_optional);
        any_message |= transmit_attribute_type(
            domains_,
            before.domains,
            AttributeType::Domain,
            static_cast<uint8_t>(AttributeLength::Domain),
            la_remaining,
            now,
            shed_optional);
    }
    return PassResult{.any_message = any_message, .skipped = pdu_message_skip_count_ != skips_before};
}

/// The PDU's head in the participant's pre-allocated buffer (no per-tx heap).
template <class Limits>
auto MsrpParticipantT<Limits>::begin_pdu() -> bool
{
    pdu_buffer_.rewind();
    return append_u8(pdu_buffer_, PROTOCOL_VERSION);
}

/// One attribute type's share of a transmit pass: tx! to its Applicants, then
/// their message into the PDU. The transition an Applicant takes on tx! IS its
/// message going out (10.7.7, Table 10-3), so a message skipped for space
/// (10.7.6.3: it waits for a later PDU) rolls its Applicants back to where they
/// stood, and the next pass dispatches tx! to them again from there: a Leave
/// stays a Leave. Dispatching from the state the transition had reached would
/// replace the owed Leave with that state's optional In, or with nothing, and
/// the peers would never hear it. With @p shed_optional the optional encodings
/// are left out: their tx! transitions are self-loops, so not sending them
/// changes no state, which is the permission the brackets in Table 10-3 give.
/// The optional In or Mt of an Applicant that declares nothing is always left
/// out: a value this port has withdrawn is not put on the wire again as Mt.
template <class Limits>
template <class Table, class Saved>
auto MsrpParticipantT<Limits>::transmit_attribute_type(
    Table& records,
    Saved const& before,
    AttributeType type,
    uint8_t attr_length,
    bool& la_remaining,
    TimePoint now,
    bool shed_optional) -> bool
{
    for (auto& rec : records) {
        mrp::dispatch_applicant_tx(rec, now);
        bool const shed = shed_optional || applicant_sm::Def::is_observer(rec.applicant_sm.current_state());
        if (shed && rec.applicant_ctx.encode == applicant_sm::Encoding::Optional) {
            rec.applicant_ctx.clear_outputs();
        }
    }
    bool const carry_la = la_remaining && has_emittable(records);
    bool const emitted = append_or_roll_back(records, before, type, attr_length, carry_la);
    if (carry_la && emitted) {
        la_remaining = false;  // LeaveAll consumed by this non-empty message
    }
    return emitted;
}

/// The message into the PDU, or, when it did not fit, the Applicants back to
/// @p before: they did not transmit, so they did not see tx!.
template <class Limits>
template <class Table, class Saved>
auto MsrpParticipantT<Limits>::append_or_roll_back(
    Table& records, Saved const& before, AttributeType type, uint8_t attr_length, bool carry_la) -> bool
{
    size_t const skips_before = pdu_message_skip_count_;
    bool const emitted = append_attribute_message(pdu_buffer_, records, type, attr_length, carry_la);
    if (pdu_message_skip_count_ != skips_before) {
        restore_applicants_of(records, before);
        return false;
    }
    return emitted;
}

/// The PDU's tail: a LeaveAll still owed with nothing re-declared, the final
/// EndMark, and the send.
template <class Limits>
void MsrpParticipantT<Limits>::finish_pdu(bool any_message, bool la_remaining)
{
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
}

// ============================================================
// Explicit instantiation
// ============================================================

template class MsrpParticipantT<DefaultMsrpLimits>;
template class MsrpParticipantT<DiscoveryMsrpLimits>;

}  // namespace statusbar::srp::msrp
