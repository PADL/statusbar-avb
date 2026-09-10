#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

//
// MVRP Participant — IEEE 802.1Q-2014 Clause 11.2.6 / MRP (Clause 10).
//
// Scope: local endpoint only (not bridge capable). One MvrpParticipant
// instance per NIC (per 802.1Q "port"). In-process API; no daemon
// sockets. Observers subscribe via callbacks and receive notifications
// as the Registrar FSMs fire.
//
// All attribute and observer storage is fixed-capacity
// statusbar::sg14::inplace_vector: MVRP traffic is inherently bounded by
// PDU size, LeaveAll timing, and the small number of VLANs any
// endpoint ever declares. The compile-time capacity is supplied via
// the Limits template parameter; the default (DefaultMvrpLimits)
// covers every realistic endpoint use case.
//

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/buffer_mutable_buffer.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/srp/srp_mrp_attribute.hpp"
#include "statusbar/srp/srp_mrp_participant.hpp"
#include "statusbar/srp/srp_mrp_timers.hpp"
#include "statusbar/srp/srp_mvrp.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn_error.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <system_error>

namespace statusbar::srp::mvrp {

//
// Default compile-time capacity limits.
//
// Override by defining a struct with the same fields and instantiating
// MvrpParticipantT<MyLimits> directly.
//
struct DefaultMvrpLimits
{
    static constexpr size_t max_vlans = 16;
    static constexpr size_t max_observers = 4;
};

//
// MvrpConfig — runtime soft-cap + RNG seed settings.
//
// max_vlans / max_observers are runtime soft caps used to bound the
// table size during construction; they must be <= the corresponding
// template Limits fields or the underlying inplace_vector::reserve()
// call throws std::bad_alloc at construction.
//
struct MvrpConfig
{
    /// Runtime-requested maximum VlanIdentifier attribute records
    /// (local + peer). Must be <= Limits::max_vlans.
    size_t max_vlans = DefaultMvrpLimits::max_vlans;

    /// Runtime-requested Observer slot count. Must be <= Limits::max_observers.
    size_t max_observers = DefaultMvrpLimits::max_observers;
};

//
// Observer callback interface.
//
struct Observer
{
    /// A VLAN registration was observed (first time or refresh).
    statusbar::sg14::inplace_function<void(uint16_t vid, mrp::Operation op), 64> on_vlan_registered;

    /// A previously-registered VLAN was withdrawn (LvTimer expiry or received Lv).
    statusbar::sg14::inplace_function<void(uint16_t vid), 64> on_vlan_leave;
};

namespace detail_mvrp {

using mrp::AttributeEvent;
using mrp::AttributeListHeader;
using mrp::VectorAttributeHeader;

namespace applicant_sm = mrp::applicant_sm;
namespace registrar_sm = mrp::registrar_sm;

/// Translate internal sndmsg to wire AttributeEvent using paired Registrar state.
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
            return AttributeEvent::Mt;
    }
}

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
            return {.applicant = applicant_sm::Def::Event::RIn, .registrar = registrar_sm::Def::Event::Count};
        case AttributeEvent::JoinMt:
            return {.applicant = applicant_sm::Def::Event::RJoinMt, .registrar = registrar_sm::Def::Event::RJoinMt};
        case AttributeEvent::Mt:
            return {.applicant = applicant_sm::Def::Event::RMt, .registrar = registrar_sm::Def::Event::Count};
        case AttributeEvent::Lv:
            return {.applicant = applicant_sm::Def::Event::RLeave, .registrar = registrar_sm::Def::Event::RLeave};
        default:
            return {.applicant = applicant_sm::Def::Event::Count, .registrar = registrar_sm::Def::Event::Count};
    }
}

[[nodiscard]] inline auto append_u8(MutableBuffer& buf, uint8_t value) noexcept -> bool
{
    std::array<uint8_t, 1> const src{value};
    return static_cast<bool>(buf.append(std::span<uint8_t const>(src)));
}

[[nodiscard]] inline auto append_be16(MutableBuffer& buf, uint16_t value) noexcept -> bool
{
    std::array<uint8_t, 2> const src{
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value & 0xFFU),
    };
    return static_cast<bool>(buf.append(std::span<uint8_t const>(src)));
}

}  // namespace detail_mvrp

using mrp::AttributeRecord;
using mrp::Operation;
using mrp::PortState;
using sm::TimePoint;
using statusbar::MutableBuffer;
using statusbar::MutableBufferWithStorage;

namespace applicant_sm = mrp::applicant_sm;
namespace registrar_sm = mrp::registrar_sm;

//
// MvrpParticipantT — per-port MVRP protocol engine, parameterized on
// compile-time capacity limits.
//
template <class Limits = DefaultMvrpLimits>
class MvrpParticipantT
{
  public:
    using SubscriptionId = uint32_t;
    using SendPduFn = statusbar::sg14::inplace_function<bool(std::span<uint8_t const>), 64>;

    /// Conservative upper bound on the size of a single outgoing MVRPDU.
    static constexpr size_t MAX_PDU_BYTES = 1500;

    /// Construct with an explicit capacity configuration. `rng_seed`
    /// is forwarded to the TimerScheduler for LeaveAll randomization.
    ///
    /// Throws std::bad_alloc if config_.max_vlans > Limits::max_vlans
    /// or config_.max_observers > Limits::max_observers.
    explicit MvrpParticipantT(MvrpConfig const& config, uint64_t rng_seed = 0)
        : config_{config}
        , port_{rng_seed}
    {
        vlans_.reserve(config_.max_vlans);
        observers_.resize(config_.max_observers);
    }

    /// Set or replace the outbound PDU callback.
    void set_send_pdu(SendPduFn fn) { send_pdu_ = std::move(fn); }

    /// Run BEGIN! on the per-port FSMs and arm initial timers.
    void start(TimePoint now) { port_.start(now); }

    /// Stop all timers. FSMs retain their current state.
    void stop() noexcept { port_.stop(); }

    // -------------------- Observer subscription --------------------

    auto subscribe(Observer obs) -> SubscriptionId
    {
        for (auto& slot : observers_) {
            if (slot.id == 0) {
                auto const id = next_subscription_id_++;
                if (next_subscription_id_ == 0) {
                    next_subscription_id_ = 1;
                }
                slot.id = id;
                slot.obs = std::move(obs);
                return id;
            }
        }
        return 0;  // observer table full
    }

    void unsubscribe(SubscriptionId id)
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

    // -------------------- Local declaration API --------------------

    auto declare_vlan(uint16_t vid, TimePoint now) -> Status
    {
        if (vid == 0 || vid > 4094) {
            return failure(make_error_code(tsn::TsnError::invalid_vlan_id));
        }
        auto* rec_ptr = find_or_create_vlan(vid);
        if (rec_ptr == nullptr) {
            return failure(make_error_code(tsn::TsnError::attribute_table_full));
        }
        auto& rec = *rec_ptr;
        bool const is_new = (rec.operation == Operation::Register) &&
            (rec.applicant_sm.current_state() == applicant_sm::Def::State::Start ||
             rec.applicant_sm.current_state() == applicant_sm::Def::State::Vo);
        rec.operation = Operation::Declare;
        auto const event = is_new ? applicant_sm::Def::Event::New : applicant_sm::Def::Event::Join;
        mrp::dispatch_applicant(rec, event, now);
        port_.timers().start_join(now);
        return success();
    }

    auto withdraw_vlan(uint16_t vid, TimePoint now) -> Status
    {
        for (auto& rec : vlans_) {
            if (rec.first_value.get_vid() == vid) {
                mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Leave, now);
                port_.timers().start_join(now);
                return success();
            }
        }
        return success();
    }

    // -------------------- Query API --------------------

    [[nodiscard]] auto vlan_count() const noexcept -> size_t { return vlans_.size(); }

    [[nodiscard]] auto has_vlan(uint16_t vid) const noexcept -> bool
    {
        for (auto const& rec : vlans_) {
            if (rec.first_value.get_vid() == vid) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto is_vlan_registered(uint16_t vid) const noexcept -> bool
    {
        for (auto const& rec : vlans_) {
            if (rec.first_value.get_vid() == vid) {
                return rec.registrar_sm.current_state() == registrar_sm::Def::State::In;
            }
        }
        return false;
    }

    // -------------------- PDU I/O --------------------

    void receive_pdu(std::span<uint8_t const> pdu, TimePoint now)
    {
        using detail_mvrp::AttributeListHeader;
        if (pdu.size() < 3) {
            return;
        }
        if (pdu[0] != PROTOCOL_VERSION) {
            return;
        }
        size_t pos = 1;
        while (pos + 2 <= pdu.size()) {
            uint16_t const end_check = mrp::read_doublet_at(pdu, pos);
            if (end_check == mrp::END_MARK) {
                break;
            }
            if (pos + AttributeListHeader::LENGTH > pdu.size()) {
                return;
            }
            AttributeListHeader hdr;
            (void)load_unchecked(pdu.subspan(pos), &hdr);
            pos += AttributeListHeader::LENGTH;

            uint8_t const attr_length = hdr.attribute_length.get();
            if (static_cast<AttributeType>(hdr.attribute_type.get()) != AttributeType::VlanIdentifier) {
                // This decoder only understands VlanIdentifier. An unknown attribute
                // must be followed immediately by an EndMark; consume it and move on.
                // Any non-EndMark word there is malformed -> bail.
                if (pos + 2 <= pdu.size()) {
                    uint16_t const v = mrp::read_doublet_at(pdu, pos);
                    if (v != mrp::END_MARK) {
                        return;
                    }
                    pos += 2;
                }
                continue;
            }

            while (pos + 2 <= pdu.size()) {
                uint16_t const end_check2 = mrp::read_doublet_at(pdu, pos);
                if (end_check2 == mrp::END_MARK) {
                    pos += 2;
                    break;
                }
                decode_vector_attribute(attr_length, pdu, pos, now);
            }
        }
        port_.timers().start_join(now);
    }

    // -------------------- Event loop --------------------

    void tick(TimePoint now)
    {
        auto const expired = port_.timers().tick(now);
        if (!expired.any()) {
            return;
        }
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

    [[nodiscard]] auto next_deadline() const noexcept -> TimePoint { return port_.timers().next_deadline(); }

  private:
    struct ObserverSlot
    {
        SubscriptionId id{0};
        Observer obs{};
    };

    MvrpConfig config_;
    statusbar::sg14::inplace_vector<AttributeRecord<VlanIdentifierFirstValue>, Limits::max_vlans> vlans_;
    PortState port_;
    SendPduFn send_pdu_{};
    MutableBufferWithStorage<MAX_PDU_BYTES> pdu_buffer_{};
    statusbar::sg14::inplace_vector<ObserverSlot, Limits::max_observers> observers_;
    SubscriptionId next_subscription_id_{1};

    // --- Timer expiry handlers ---

    void on_join_timer(TimePoint now) { build_and_send_pdu(now); }

    void on_leave_timer(TimePoint now)
    {
        for (auto& rec : vlans_) {
            if (rec.registrar_sm.current_state() != registrar_sm::Def::State::Lv) {
                continue;
            }
            mrp::dispatch_registrar(rec, registrar_sm::Def::Event::LvTimer, now, port_.timers());
            if (rec.registrar_ctx.notify == registrar_sm::Notify::Leave) {
                notify_vlan_leave(rec.first_value.get_vid());
            }
        }
    }

    void on_leaveall_timer(TimePoint now)
    {
        port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::LvaTimer, now);
        for (auto& rec : vlans_) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::TxLeaveAll, now);
            mrp::dispatch_registrar(rec, registrar_sm::Def::Event::TxLeaveAll, now, port_.timers());
        }
        port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::Tx, now);
        build_and_send_pdu(now);
    }

    void on_periodic_timer(TimePoint now)
    {
        port_.dispatch_periodic(mrp::periodic_sm::Def::Event::Periodic, now);
        for (auto& rec : vlans_) {
            mrp::dispatch_applicant(rec, applicant_sm::Def::Event::Periodic, now);
        }
        port_.timers().start_join(now);
    }

    // --- PDU processing ---

    void decode_vector_attribute(uint8_t attr_length, std::span<uint8_t const> payload, size_t& pos, TimePoint now)
    {
        using detail_mvrp::VectorAttributeHeader;
        if (pos + VectorAttributeHeader::LENGTH > payload.size()) {
            return;
        }
        VectorAttributeHeader vec_hdr;
        (void)load_unchecked(payload.subspan(pos), &vec_hdr);
        pos += VectorAttributeHeader::LENGTH;

        bool const leave_all = vec_hdr.get_leave_all();
        uint16_t const num_values = vec_hdr.get_number_of_values();

        if (leave_all) {
            for (auto& rec : vlans_) {
                mrp::dispatch_applicant(rec, applicant_sm::Def::Event::RLeaveAll, now);
                mrp::dispatch_registrar(rec, registrar_sm::Def::Event::RLeaveAll, now, port_.timers());
            }
            port_.dispatch_leaveall(mrp::leaveall_sm::Def::Event::RLeaveAll, now);
        }

        // Validate against the FIXED FirstValue length, not the attacker-supplied
        // attr_length: a crafted PDU with attr_length < VlanIdentifierFirstValue::LENGTH
        // (e.g. 0 or 1) near the end of the buffer would otherwise pass a
        // `pos + attr_length` check yet make load_unchecked read LENGTH bytes past
        // the packet. Require the full fixed FirstValue to be present.
        if (attr_length < VlanIdentifierFirstValue::LENGTH || pos + attr_length > payload.size()) {
            return;
        }
        VlanIdentifierFirstValue fv;
        (void)load_unchecked(payload.subspan(pos), &fv);
        pos += attr_length;

        size_t const num_event_octets = mrp::threepacked_octet_count(num_values);
        if (pos + num_event_octets > payload.size()) {
            return;
        }
        auto const event_bytes = payload.subspan(pos, num_event_octets);
        pos += num_event_octets;

        uint16_t const base_vid = fv.get_vid();
        for (uint16_t i = 0; i < num_values; ++i) {
            mrp::AttributeEvent const event = mrp::nth_of_three(mrp::unpack3_events(event_bytes[i / 3]), i);
            uint16_t const this_vid = static_cast<uint16_t>(base_vid + i);
            handle_rx_event(this_vid, event, now);
        }
    }

    void handle_rx_event(uint16_t vid, mrp::AttributeEvent event, TimePoint now)
    {
        auto const rx = detail_mvrp::rx_events_for(event);
        if (rx.applicant == applicant_sm::Def::Event::Count) {
            return;
        }
        auto* rec_ptr = find_or_create_vlan(vid);
        if (rec_ptr == nullptr) {
            return;  // peer attribute, table full — silently drop
        }
        auto& rec = *rec_ptr;
        bool const was_registered = (rec.registrar_sm.current_state() == registrar_sm::Def::State::In);
        mrp::dispatch_applicant(rec, rx.applicant, now);
        if (rx.registrar != registrar_sm::Def::Event::Count) {
            mrp::dispatch_registrar(rec, rx.registrar, now, port_.timers());
            auto const n = rec.registrar_ctx.notify;
            if (n == registrar_sm::Notify::New || n == registrar_sm::Notify::Join) {
                notify_vlan_registered(vid, Operation::Register);
            } else if (n == registrar_sm::Notify::Leave && was_registered) {
                notify_vlan_leave(vid);
            }
        }
    }

    void build_and_send_pdu(TimePoint now)
    {
        for (auto& rec : vlans_) {
            mrp::dispatch_applicant_tx(rec, now);
        }

        size_t emit_count = 0;
        for (auto const& rec : vlans_) {
            if (rec.applicant_ctx.tx_pending) {
                ++emit_count;
            }
        }
        if (emit_count == 0) {
            return;
        }

        pdu_buffer_.rewind();
        if (!detail_mvrp::append_u8(pdu_buffer_, PROTOCOL_VERSION)) {
            return;
        }
        if (!detail_mvrp::append_u8(pdu_buffer_, static_cast<uint8_t>(AttributeType::VlanIdentifier))) {
            return;
        }
        if (!detail_mvrp::append_u8(pdu_buffer_, static_cast<uint8_t>(AttributeLength::VlanIdentifier))) {
            return;
        }

        for (auto& rec : vlans_) {
            if (!rec.applicant_ctx.tx_pending) {
                continue;
            }
            bool const reg_in = rec.registrar_is_in();
            auto const wire_event = detail_mvrp::translate_sndmsg(rec.applicant_ctx.send_msg, reg_in);

            mrp::VectorAttributeHeader vh{};
            vh.set(false, 1);
            if (!detail_mvrp::append_be16(pdu_buffer_, vh.vector_header.get())) {
                break;
            }

            std::array<uint8_t, 2> fv_buf{};
            (void)store_unchecked(std::span<uint8_t>(fv_buf), rec.first_value);
            if (!static_cast<bool>(pdu_buffer_.append(std::span<uint8_t const>(fv_buf)))) {
                break;
            }
            if (!detail_mvrp::append_u8(
                    pdu_buffer_, mrp::pack3_events(wire_event, mrp::AttributeEvent::Mt, mrp::AttributeEvent::Mt))) {
                break;
            }

            rec.applicant_ctx.tx_pending = false;
            rec.applicant_ctx.send_msg = applicant_sm::SendMessage::None;
            rec.applicant_ctx.encode = applicant_sm::Encoding::None;
        }

        (void)detail_mvrp::append_be16(pdu_buffer_, mrp::END_MARK);
        (void)detail_mvrp::append_be16(pdu_buffer_, mrp::END_MARK);

        if (send_pdu_) {
            (void)send_pdu_(pdu_buffer_.get_span());
        }
        (void)now;
    }

    auto find_or_create_vlan(uint16_t vid) -> AttributeRecord<VlanIdentifierFirstValue>*
    {
        for (auto& rec : vlans_) {
            if (rec.first_value.get_vid() == vid) {
                return &rec;
            }
        }
        if (vlans_.size() >= config_.max_vlans) {
            return nullptr;
        }
        AttributeRecord<VlanIdentifierFirstValue> rec{};
        rec.first_value.set_vid(vid);
        vlans_.push_back(rec);
        return &vlans_.back();
    }

    void reclaim()
    {
        auto const new_end = std::remove_if(vlans_.begin(), vlans_.end(), [](auto const& rec) { return rec.is_dead(); });
        vlans_.erase(new_end, vlans_.end());
    }

    void notify_vlan_registered(uint16_t vid, Operation op)
    {
        for (auto const& slot : observers_) {
            if (slot.id != 0 && slot.obs.on_vlan_registered) {
                slot.obs.on_vlan_registered(vid, op);
            }
        }
    }

    void notify_vlan_leave(uint16_t vid)
    {
        for (auto const& slot : observers_) {
            if (slot.id != 0 && slot.obs.on_vlan_leave) {
                slot.obs.on_vlan_leave(vid);
            }
        }
    }
};

/// Default instantiation — matches the historical capacity defaults.
using MvrpParticipant = MvrpParticipantT<>;

}  // namespace statusbar::srp::mvrp
