// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_srp_port.hpp"

#include "statusbar/nanoavb/nanoavb_base.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

namespace statusbar::nanoavb {

SrpPort::SrpPort(SrpPortConfig config)
    : config_{std::move(config)}
    , mvrp_{config_.mvrp}
    , msrp_{config_.msrp}
{
    msrp_.set_domain(config_.class_a);
    msrp_.set_listener_waits_for_talker(true);
    wire();
}

void SrpPort::wire()
{
    mvrp_.set_callbacks(MvrpCallbacks{.send_packet = [this](std::span<uint8_t const> p) { return send_mvrp_ && send_mvrp_(p); }});
    msrp_.set_callbacks(MsrpCallbacks{
        .send_packet = [this](std::span<uint8_t const> p) { return send_msrp_ && send_msrp_(p); },
        .on_listener_state_change =
            [this](StreamId const& sid, ListenerReservationState const state) {
                if (observer_.on_listener_state) {
                    observer_.on_listener_state(sid, state);
                }
            },
    });
    msrp_.set_on_domain_vid_changed([this](uint16_t const o, uint16_t const n, TimePoint const t) { on_vid_changed(o, n, t); });
    (void)msrp_.subscribe(statusbar::srp::msrp::Observer{
        .on_talker_advertise =
            [this](statusbar::srp::msrp::TalkerAdvertiseFirstValue const& fv, statusbar::srp::msrp::Operation) {
                if (observer_.on_talker_advertise) {
                    observer_.on_talker_advertise(fv);
                }
            },
        .on_talker_failed =
            [this](statusbar::srp::msrp::TalkerFailedFirstValue const& fv, statusbar::srp::msrp::Operation) {
                if (observer_.on_talker_failed) {
                    observer_.on_talker_failed(fv);
                }
            },
        .on_talker_leave = [this](StreamId const& sid) { on_peer_talker_leave(sid); },
        .on_listener =
            [this](StreamId const& sid, statusbar::srp::msrp::ListenerDeclaration, statusbar::srp::msrp::Operation) {
                on_peer_listener(sid);
            },
        .on_listener_leave = [this](StreamId const& sid) { on_peer_listener(sid); },
        .on_domain =
            [this](statusbar::srp::msrp::DomainFirstValue const& fv, statusbar::srp::msrp::Operation) { on_peer_domain(fv, true); },
        .on_domain_leave = [this](statusbar::srp::msrp::DomainFirstValue const& fv) { on_peer_domain(fv, false); },
        .on_domain_declared = [this](statusbar::srp::msrp::DomainFirstValue const& fv) { on_peer_domain_declared(fv); },
    });
}

auto SrpPort::add_to_reactor(net::MessageReactor& reactor) -> Status
{
    if (added_) {
        return failure(make_error_code(NanoAvbError::InvalidConfiguration));
    }
    auto mvrp = std::make_unique<MvrpNetHandler>(config_.interface_name, mvrp_);
    auto msrp = std::make_unique<MsrpNetHandlerT<MsrpIo>>(config_.interface_name, msrp_io_);
    if (!mvrp->valid() || !msrp->valid()) {
        return failure(make_error_code(NanoAvbError::InvalidConfiguration));
    }
    hand_over(reactor, std::move(msrp), std::move(mvrp));
    return success();
}

void SrpPort::hand_over(
    net::MessageReactor& reactor, std::unique_ptr<MsrpNetHandlerT<MsrpIo>> msrp, std::unique_ptr<MvrpNetHandler> mvrp)
{
    set_senders(
        [net = msrp.get()](std::span<uint8_t const> p) { return net->send(MSRP_MULTICAST_MAC, p).has_value(); },
        [net = mvrp.get()](std::span<uint8_t const> p) { return net->send(MVRP_MULTICAST_MAC, p).has_value(); });
    reactor.add(std::move(mvrp));
    reactor.add(std::move(msrp));
    added_ = true;
}

void SrpPort::set_senders(SendFn msrp, SendFn mvrp) noexcept
{
    send_msrp_ = std::move(msrp);
    send_mvrp_ = std::move(mvrp);
}

void SrpPort::receive_msrp(std::span<uint8_t const> const pdu, TimePoint const now)
{
    msrp_.receive_packet(pdu, now);
    flush();
}

void SrpPort::tick_msrp(TimePoint const now)
{
    msrp_.tick(now);
    flush();
}

void SrpPort::tick(TimePoint const now)
{
    tick_msrp(now);
    tick_mvrp(now);
}

auto SrpPort::declare_domain(TimePoint const now) -> Status
{
    auto status = msrp_.reset_domain(now);
    if (status) {
        status = mvrp_.register_vlan(domain().sr_class_vid, now);
    }
    flush_domain();
    return status;
}

auto SrpPort::declare_talker(SrpTalker const& talker, TimePoint const now) -> Status
{
    auto const& d = domain();
    auto const declared = msrp_.talker_advertise(
        TalkerStreamSrpInfo{
            .stream_id = talker.stream_id,
            .dest_address = talker.destination,
            .vlan_id = d.sr_class_vid,
            .max_frame_size = talker.max_frame_size,
            .max_interval_frames = talker.max_interval_frames,
            .priority = d.sr_class_priority,
            .accumulated_latency = talker.accumulated_latency,
        },
        now);
    return declared ? success() : failure(declared.error());
}

auto SrpPort::withdraw_talker(StreamId const& stream_id, TimePoint const now) -> Status
{
    return msrp_.talker_withdraw(stream_id, now);
}

auto SrpPort::attach_listener(StreamId const& stream_id, TimePoint const now) -> StatusValue<ListenerReservationState>
{
    return msrp_.listener_attach(stream_id, now);
}

auto SrpPort::withdraw_listener(StreamId const& stream_id, TimePoint const now) -> Status
{
    return msrp_.listener_withdraw(stream_id, now);
}

auto SrpPort::peer_domain_registered() const noexcept -> bool
{
    return peer_class_a_ && peer_priority_ == domain().sr_class_priority;
}

auto SrpPort::find_registered_talker_advertise(Eui48 const& destination, uint16_t const vid) const noexcept
    -> statusbar::srp::msrp::TalkerAdvertiseFirstValue const*
{
    return msrp_.peer_talker_advertise(destination, vid);
}

auto SrpPort::find_registered_talker_failed(Eui48 const& destination, uint16_t const vid) const noexcept
    -> statusbar::srp::msrp::TalkerFailedFirstValue const*
{
    return msrp_.peer_talker_failed(destination, vid);
}

void SrpPort::on_vid_changed(uint16_t const old_vid, uint16_t const new_vid, TimePoint const now)
{
    (void)mvrp_.withdraw_vlan(old_vid, now);
    (void)mvrp_.register_vlan(new_vid, now);
    for (size_t i = 0; i < msrp_.talker_streams().size(); ++i) {
        auto info = msrp_.talker_streams()[i];
        if (info.state != TalkerReservationState::Idle && info.vlan_id == old_vid) {
            info.vlan_id = new_vid;
            (void)msrp_.talker_advertise(info, now);  // updates the entry in place
        }
    }
    domain_changed_ = true;  // reported by flush_domain() once the PDU is done
}

void SrpPort::on_peer_domain_declared(statusbar::srp::msrp::DomainFirstValue const& fv) noexcept
{
    if (fv.sr_class_id.get() == statusbar::srp::msrp::SR_CLASS_A) {
        peer_priority_ = fv.sr_class_priority.get();
    }
}

void SrpPort::on_peer_domain(statusbar::srp::msrp::DomainFirstValue const& fv, bool const registered) noexcept
{
    if (fv.sr_class_id.get() != statusbar::srp::msrp::SR_CLASS_A) {
        return;
    }
    peer_class_a_ = registered;
    if (!registered) {
        peer_priority_.reset();
    }
}

void SrpPort::on_peer_talker_leave(StreamId const& stream_id)
{
    // Decided once the PDU is done: its Talker Failed may follow the Advertise's leave.
    if (std::ranges::find(pending_leaves_, stream_id) == pending_leaves_.end()) {
        (void)pending_leaves_.try_push_back(stream_id);  // sized for every talker attribute
    }
}

void SrpPort::report_leave(StreamId const& stream_id)
{
    if (msrp_.peer_talker_advertise(stream_id) != nullptr || msrp_.peer_talker_failed(stream_id) != nullptr) {
        return;  // the talker is still there under its other attribute
    }
    if (observer_.on_talker_leave) {
        observer_.on_talker_leave(stream_id);
    }
}

void SrpPort::on_peer_listener(StreamId const& stream_id)
{
    if (msrp_.get_talker_stream(stream_id) != nullptr && observer_.on_listener) {
        observer_.on_listener(stream_id, msrp_.registered_listener(stream_id));
    }
}

void SrpPort::flush()
{
    for (auto const& sid : pending_leaves_) {
        report_leave(sid);
    }
    pending_leaves_.clear();
    flush_domain();
}

void SrpPort::flush_domain()
{
    bool const registered = peer_domain_registered();
    if (!domain_changed_ && registered == reported_peer_) {
        return;
    }
    domain_changed_ = false;
    reported_peer_ = registered;
    if (observer_.on_domain) {
        observer_.on_domain(domain(), registered);
    }
}

}  // namespace statusbar::nanoavb
