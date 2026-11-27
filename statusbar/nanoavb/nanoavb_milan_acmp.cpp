// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_milan_acmp.hpp"

#include <algorithm>
#include <utility>

namespace statusbar::nanoavb {

namespace {

[[nodiscard]] constexpr auto is_listener_command(uint8_t const message_type) noexcept -> bool
{
    return message_type == ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND || message_type == ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND ||
        message_type == ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND;
}

/// The two commands BIND_STREAM and UNBIND_STREAM stand for (Milan v1.3 Table 5.22).
[[nodiscard]] constexpr auto is_binding_command(uint8_t const message_type) noexcept -> bool
{
    return message_type == ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND || message_type == ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND;
}

/// A seed no two entities share and that is never zero (xorshift's fixed point).
[[nodiscard]] auto seed_from(Eui64 const& entity_id) noexcept -> uint32_t
{
    uint64_t const id = entity_id.to_uint64();
    return static_cast<uint32_t>(id ^ (id >> 32U)) | 1U;
}

}  // namespace

//
// NanoAvbMilanAcmpTalker
//

NanoAvbMilanAcmpTalker::NanoAvbMilanAcmpTalker(Eui64 const entity_id, AcmpTalkerCallbacks callbacks, size_t const max_streams)
    : entity_id_{entity_id}
    , callbacks_{std::move(callbacks)}
{
    streams_.resize(std::min(max_streams, MAX_STREAMS));
}

auto NanoAvbMilanAcmpTalker::configure_stream(
    uint16_t const stream_index, Eui64 const stream_id, Eui48 const dest_mac, uint16_t const vlan_id) -> Status
{
    if (stream_index >= streams_.size()) {
        return failure(make_error_code(NanoAvbError::InvalidStreamIndex));
    }
    streams_[stream_index] =
        atdecc::MilanTalkerStream{.stream_id = stream_id, .stream_dest_mac = dest_mac, .stream_vlan_id = vlan_id};
    return success();
}

auto NanoAvbMilanAcmpTalker::source_for(uint16_t const stream_index) const -> atdecc::MilanTalkerSource
{
    auto const* stream = get_stream(stream_index);
    if (stream == nullptr) {
        return atdecc::MilanTalkerSource{};
    }
    return atdecc::MilanTalkerSource{
        .stream = stream,
        .dest_mac_valid = !dest_mac_valid_ || dest_mac_valid_(stream_index),
        .registering_failed = registering_failed_ && registering_failed_(stream_index)};
}

void NanoAvbMilanAcmpTalker::report_probe(AcmpCommandResponse const& cmd, TimePoint const event_time)
{
    bool const is_probe = cmd.message_type() == ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
    if (is_probe && callbacks_.on_probe && get_stream(cmd.talker_unique_id.get()) != nullptr) {
        callbacks_.on_probe(cmd.talker_unique_id.get(), event_time);
    }
}

auto NanoAvbMilanAcmpTalker::receive_command(AcmpCommandResponse const& cmd, TimePoint const event_time) -> bool
{
    if (cmd.talker_entity_id != entity_id_) {
        return false;
    }
    auto const response = atdecc::milan_talker_response(cmd, source_for(cmd.talker_unique_id.get()));
    if (!response.has_value()) {
        return false;
    }
    // The probe is told first: the stream's Talker attribute is on its way
    // to being declared when the response reaches the listener (4.3.3.1).
    report_probe(cmd, event_time);
    if (callbacks_.tx_response) {
        (void)callbacks_.tx_response(*response);
    }
    return true;
}

//
// NanoAvbMilanAcmpListener
//

NanoAvbMilanAcmpListener::NanoAvbMilanAcmpListener(Eui64 const entity_id, AcmpListenerCallbacks callbacks, size_t const max_streams)
    : callbacks_{std::move(callbacks)}
    , random_state_{seed_from(entity_id)}
{
    env_.my_id = entity_id;
    wire_env();
    sinks_.resize(std::min(max_streams, MAX_STREAMS));
    for (size_t i = 0; i < sinks_.size(); ++i) {
        sinks_[i].attach(env_, static_cast<uint16_t>(i));
        told_.push_back(atdecc::milan_stream_input_notified(sinks_[i].context()));
    }
}

void NanoAvbMilanAcmpListener::wire_env()
{
    // The machines' ways out read callbacks_ when they are used, so a
    // callback set or replaced later is the one that runs.
    env_.tx_command = [this](AcmpCommandResponse const& pdu) { return callbacks_.tx_command ? callbacks_.tx_command(pdu) : false; };
    env_.tx_response = [this](AcmpCommandResponse const& pdu) { return answer_controller(pdu); };
    env_.start_srp = [this](uint16_t const index, atdecc::MilanSrpParameters const& srp, TimePoint const now) {
        if (callbacks_.on_connect) {
            callbacks_.on_connect(index, srp.stream_id, srp.stream_dest_mac, now);
        }
    };
    env_.stop_srp = [this](uint16_t const index, TimePoint const now) {
        if (callbacks_.on_disconnect) {
            callbacks_.on_disconnect(index, now);
        }
    };
    env_.binding_changed = [this](uint16_t /*index*/) {
        if (on_bindings_changed_) {
            on_bindings_changed_();
        }
    };
    env_.random_delay = [this]() { return draw_delay(); };
}

auto NanoAvbMilanAcmpListener::answer_controller(AcmpCommandResponse const& pdu) -> bool
{
    // One answer per command: whatever a sink sends a controller after it
    // goes the ordinary way.
    if (auto const* const respond = std::exchange(mvu_respond_, nullptr); respond != nullptr) {
        (*respond)(pdu);
        return true;
    }
    return callbacks_.tx_response ? callbacks_.tx_response(pdu) : false;
}

auto NanoAvbMilanAcmpListener::draw_delay() noexcept -> std::chrono::milliseconds
{
    // xorshift32: spread is all TMR_DELAY asks of it.
    random_state_ ^= random_state_ << 13U;
    random_state_ ^= random_state_ >> 17U;
    random_state_ ^= random_state_ << 5U;
    auto const span = static_cast<uint32_t>(atdecc::MILAN_TMR_DELAY_MAX.count()) + 1U;
    return std::chrono::milliseconds{random_state_ % span};
}

auto NanoAvbMilanAcmpListener::is_locked_out(Eui64 const& controller) const -> bool
{
    return locked_out_ && locked_out_(controller);
}

auto NanoAvbMilanAcmpListener::mutable_sink(uint16_t const stream_index) noexcept -> atdecc::MilanSink*
{
    return stream_index < sinks_.size() ? &sinks_[stream_index] : nullptr;
}

auto NanoAvbMilanAcmpListener::bindings() const -> Bindings
{
    Bindings bound{};
    for (auto const& each : sinks_) {
        if (each.is_bound()) {
            bound.push_back(*each.context().binding);
        }
    }
    return bound;
}

auto NanoAvbMilanAcmpListener::restore_binding(atdecc::MilanBinding const& binding, TimePoint const now) -> bool
{
    auto* found = mutable_sink(binding.listener_unique_id);
    if (found == nullptr) {
        return false;
    }
    found->restore(binding, now);
    return true;
}

void NanoAvbMilanAcmpListener::restart(TimePoint const now)
{
    for (auto& each : sinks_) {
        each.restart(now);
    }
}

auto NanoAvbMilanAcmpListener::receive_controller_command(
    AcmpCommandResponse const& cmd, TimePoint const event_time, std::optional<atdecc::MilanHeardAdvertisement> const& heard) -> bool
{
    if (cmd.listener_entity_id != env_.my_id || !is_listener_command(cmd.message_type())) {
        return false;
    }
    treat_controller_command(cmd, event_time, heard);
    return true;
}

auto NanoAvbMilanAcmpListener::receive_mvu_binding_command(
    AcmpCommandResponse const& cmd,
    TimePoint const event_time,
    Respond const& respond,
    std::optional<atdecc::MilanHeardAdvertisement> const& heard) -> bool
{
    if (cmd.listener_entity_id != env_.my_id || !is_binding_command(cmd.message_type())) {
        return false;
    }
    mvu_respond_ = &respond;
    treat_controller_command(cmd, event_time, heard);
    mvu_respond_ = nullptr;
    return true;
}

void NanoAvbMilanAcmpListener::treat_controller_command(
    AcmpCommandResponse const& cmd, TimePoint const event_time, std::optional<atdecc::MilanHeardAdvertisement> const& heard)
{
    auto* found = mutable_sink(cmd.listener_unique_id.get());
    if (found == nullptr) {
        // 5.5.3.1 step 1 with Table 5.27, 5.5.3.2 step 1 with Table 5.28
        (void)env_.tx_response(atdecc::milan_unknown_sink_response(cmd));
        return;
    }
    found->on_command(cmd, is_locked_out(cmd.controller_entity_id), event_time);
    if (heard.has_value() && cmd.message_type() == ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND) {
        auto const own = gptp_state_ ? gptp_state_() : atdecc::MilanGptpState{};
        found->on_advertisement_heard_earlier(heard->adp, own, heard->valid_until, event_time);
    }
}

auto NanoAvbMilanAcmpListener::receive_talker_response(AcmpCommandResponse const& resp, TimePoint const event_time) -> bool
{
    if (resp.message_type() != ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE || resp.listener_entity_id != env_.my_id) {
        return false;
    }
    auto* found = mutable_sink(resp.listener_unique_id.get());
    if (found == nullptr) {
        return false;
    }
    found->on_probe_response(resp, event_time);
    return true;
}

void NanoAvbMilanAcmpListener::receive_adpdu(atdecc::AdpDu const& adp, TimePoint const event_time)
{
    auto const own = gptp_state_ ? gptp_state_() : atdecc::MilanGptpState{};
    for (auto& each : sinks_) {
        each.on_adp(adp, own, event_time);
    }
}

void NanoAvbMilanAcmpListener::on_srp_changed(TimePoint const now)
{
    for (auto& each : sinks_) {
        each.on_srp_changed(now);
    }
}

auto NanoAvbMilanAcmpListener::set_streaming_wait(uint16_t const stream_index, bool const wait) -> bool
{
    auto* const found = mutable_sink(stream_index);
    return found != nullptr && found->set_streaming_wait(wait);
}

void NanoAvbMilanAcmpListener::tick(TimePoint const now)
{
    for (auto& each : sinks_) {
        each.tick(now);
    }
}

void NanoAvbMilanAcmpListener::tell_stream_input_changes(TellStreamInput const& tell)
{
    for (size_t i = 0; i < sinks_.size(); ++i) {
        auto const found = atdecc::milan_stream_input_notified(sinks_[i].context());
        if (std::exchange(told_[i], found) != found) {
            tell(static_cast<uint16_t>(i));
        }
    }
}

auto NanoAvbMilanAcmpListener::next_deadline() const noexcept -> std::optional<TimePoint>
{
    std::optional<TimePoint> earliest{};
    for (auto const& each : sinks_) {
        auto const next = each.next_deadline();
        if (next.has_value() && (!earliest.has_value() || *next < *earliest)) {
            earliest = next;
        }
    }
    return earliest;
}

}  // namespace statusbar::nanoavb
