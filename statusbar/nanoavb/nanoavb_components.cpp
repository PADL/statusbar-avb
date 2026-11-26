// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_components.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/gptp/gptp_base_format.hpp"
#include "statusbar/gptp/gptp_messages.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/ieee/ieee_protocols.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/tsn/tsn_clock_identity.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace statusbar::nanoavb {

//
// GptpAnnounceHandler implementation
//

void GptpAnnounceHandler::on_ready(int64_t now_ns)
{
    using namespace statusbar::gptp;

    ieee::Eui48 src_mac{};
    ieee::Eui48 dest_mac{};

    while (true) {  // NOLINT(statusbar-block-statements) one Announce read; pre-dates the gate
        auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
        if (!result || *result <= 0) {
            break;
        }
        auto const payload = std::span<uint8_t const>{payload_buf_.data(), static_cast<size_t>(*result)};
        auto const msg = parse_gptp(payload);
        if (!msg) {
            continue;
        }

        if (auto const* announce = std::get_if<AnnounceMessage>(&*msg)) {
            observe_announce(now_ns, *announce, parse_announce_path_trace(payload));
        }
    }
}

namespace {

/// The TLVs after the Announce body of @p announce, as far as its messageLength (IEEE 802.1AS-2020
/// Clause 10.6.3.1); empty when there are none.
auto announce_tlvs(std::span<uint8_t const> const announce) noexcept -> std::span<uint8_t const>
{
    if (announce.size() < gptp::AnnounceMessage::LENGTH) {
        return {};
    }
    gptp::MessageHeader header{};
    span_load(header, announce);
    size_t const end = std::min<size_t>(announce.size(), header.message_length.get());
    return end > gptp::AnnounceMessage::LENGTH
        ? announce.subspan(gptp::AnnounceMessage::LENGTH, end - gptp::AnnounceMessage::LENGTH)
        : std::span<uint8_t const>{};
}

/// The value of the PATH_TRACE TLV among @p tlvs; empty when it is absent or a TLV runs past them.
auto path_trace_value(std::span<uint8_t const> tlvs) noexcept -> std::span<uint8_t const>
{
    while (tlvs.size() >= gptp::TlvHeader::LENGTH) {
        gptp::TlvHeader tlv{};
        span_load(tlv, tlvs);
        auto const value = tlvs.subspan(gptp::TlvHeader::LENGTH);
        if (value.size() < tlv.length_field.get()) {
            return {};
        }
        if (tlv.tlv_type.get() == gptp::TLV_TYPE_PATH_TRACE) {
            return value.first(tlv.length_field.get());
        }
        tlvs = value.subspan(tlv.length_field.get());
    }
    return {};
}

/// The path @p announce makes known: @p path_trace, or its grandmaster alone without one.
auto announced_path(gptp::AnnounceMessage const& announce, AsPath const& path_trace) -> AsPath
{
    if (!path_trace.empty()) {
        return path_trace;
    }
    AsPath path{};
    path.push_back(announce.grandmaster_identity.to_eui64());
    return path;
}

}  // namespace

auto parse_announce_path_trace(std::span<uint8_t const> const announce) noexcept -> AsPath
{
    AsPath path{};
    auto const value = path_trace_value(announce_tlvs(announce));
    for (size_t at = 0; at + Eui64::LENGTH <= value.size() && path.size() < AsPath::capacity(); at += Eui64::LENGTH) {
        span_load(path.emplace_back(), value.subspan(at));
    }
    return path;
}

void GptpAnnounceHandler::observe_announce(int64_t const now_ns, gptp::AnnounceMessage const& announce, AsPath const& path_trace)
{
    // Record first: a callback that asks last_announce() must be told the
    // Announce that named the grandmaster it is being told about.
    last_announce_time_ns_ = now_ns;
    last_announce_ = announce;
    has_announce_ = true;
    set_path_trace(now_ns, announced_path(announce, path_trace));
    observe_grandmaster(now_ns, announce);
}

void GptpAnnounceHandler::observe_grandmaster(int64_t const now_ns, gptp::AnnounceMessage const& announce)
{
    if (announce.grandmaster_identity == grandmaster_identity_) {
        return;
    }
    grandmaster_identity_ = announce.grandmaster_identity;
    if (callbacks_.grandmaster_id_changed) {
        callbacks_.grandmaster_id_changed(now_ns, grandmaster_identity_, announce);
    }
    if (on_grandmaster_changed_) {
        on_grandmaster_changed_(now_ns);
    }
}

auto GptpAnnounceHandler::announce_receipt_timeout_ns() const noexcept -> int64_t
{
    // 802.1AS-2020 10.7.3.2: announceReceiptTimeout x announceInterval, the
    // interval 2^logMessageInterval s; the default timeout is 3. 10.7.2.3
    // gives logAnnounceInterval the range -3 (125 ms) to 4 (16 s).
    constexpr int64_t ONE_SECOND_NS = 1'000'000'000;
    constexpr int64_t ANNOUNCE_RECEIPT_TIMEOUT = 3;
    int const log_interval =
        has_announce_ ? std::clamp<int>(static_cast<int8_t>(last_announce_.header.log_message_interval), -3, 4) : 0;
    int64_t const interval_ns = log_interval >= 0 ? ONE_SECOND_NS << log_interval : ONE_SECOND_NS >> -log_interval;
    return ANNOUNCE_RECEIPT_TIMEOUT * interval_ns;
}

void GptpAnnounceHandler::tick(int64_t const now_ns)
{
    if (!has_grandmaster() || now_ns - last_announce_time_ns_ < announce_receipt_timeout_ns()) {
        return;
    }
    // The Announces have stopped: nothing is in force to follow. The last
    // Announce stays readable for the domain it was sent in.
    grandmaster_identity_ = {};
    if (callbacks_.grandmaster_lost) {
        callbacks_.grandmaster_lost(now_ns);
    }
    set_path_trace(now_ns, AsPath{});
}

void GptpAnnounceHandler::set_path_trace(int64_t const now_ns, AsPath const& path)
{
    if (std::ranges::equal(path, path_trace_)) {
        return;
    }
    path_trace_ = path;
    if (on_path_trace_changed_) {
        on_path_trace_changed_(now_ns);
    }
}

//
// NanoAvbComponents implementation
//

auto NanoAvbComponents::talker_connection_count() const -> size_t
{
    size_t count = 0;
    for (size_t i = 0; i < acmp_talker.max_streams(); ++i) {
        count += acmp_talker.connection_count(i);
    }
    return count;
}

void NanoAvbComponents::use_milan_acmp()
{
    if (acmp_is_milan()) {
        return;
    }
    milan_acmp_talker =
        std::make_unique<NanoAvbMilanAcmpTalker>(acmp_talker.entity_id(), AcmpTalkerCallbacks{}, acmp_talker.max_streams());
    milan_acmp_listener =
        std::make_unique<NanoAvbMilanAcmpListener>(acmp_listener.entity_id(), AcmpListenerCallbacks{}, acmp_listener.max_streams());
    // Milan v1.3 4.4.1: the VLANs of the settled sinks are declared by MVRP.
    milan_acmp_listener->set_on_sink_vlans_changed(
        [this](std::span<uint16_t const> const vlans, sm::TimePoint const now) { mvrp_handler.set_sink_vlans(vlans, now); });
    // Milan v1.3 5.6: such an entity advertises with valid_time 10 and by
    // Milan's advertise state machine.
    adp_advertiser.use_milan_advertise();
}

auto NanoAvbComponents::configure_talker_stream(
    uint16_t const stream_index, Eui64 const stream_id, Eui48 const dest_mac, uint16_t const vlan_id) -> Status
{
    if (milan_acmp_talker != nullptr) {
        return milan_acmp_talker->configure_stream(stream_index, stream_id, dest_mac, vlan_id);
    }
    return acmp_talker.configure_stream(stream_index, stream_id, dest_mac, vlan_id);
}

namespace {

/// The three fields both talkers keep per stream output, under the same names.
template <typename Stream>
[[nodiscard]] auto talker_stream_of(Stream const* const stream) noexcept -> std::optional<NanoAvbComponents::TalkerStream>
{
    if (stream == nullptr) {
        return std::nullopt;
    }
    return NanoAvbComponents::TalkerStream{
        .stream_id = stream->stream_id, .stream_dest_mac = stream->stream_dest_mac, .stream_vlan_id = stream->stream_vlan_id};
}

[[nodiscard]] auto sink_of(atdecc::ListenerStreamInfo const& sink) noexcept -> NanoAvbComponents::ListenerSink
{
    return NanoAvbComponents::ListenerSink{
        .connected = sink.connected,
        .talker_entity_id = sink.talker_entity_id,
        .talker_unique_id = sink.talker_unique_id,
        .stream_id = sink.stream_id,
        .stream_dest_mac = sink.stream_dest_mac,
        .stream_vlan_id = sink.stream_vlan_id};
}

[[nodiscard]] auto sink_of(atdecc::MilanSink const& sink) noexcept -> NanoAvbComponents::ListenerSink
{
    auto const& ctx = sink.context();
    return NanoAvbComponents::ListenerSink{
        .connected = sink.is_settled(),
        .talker_entity_id = ctx.binding.has_value() ? ctx.binding->talker_entity_id : Eui64{},
        .talker_unique_id = ctx.binding.has_value() ? ctx.binding->talker_unique_id : uint16_t{0},
        .stream_id = ctx.srp.stream_id,
        .stream_dest_mac = ctx.srp.stream_dest_mac,
        .stream_vlan_id = ctx.srp.stream_vlan_id};
}

template <typename Sink>
[[nodiscard]] auto optional_sink_of(Sink const* const sink) noexcept -> std::optional<NanoAvbComponents::ListenerSink>
{
    return sink != nullptr ? std::optional{sink_of(*sink)} : std::nullopt;
}

}  // namespace

auto NanoAvbComponents::talker_stream(uint16_t const stream_index) const noexcept -> std::optional<TalkerStream>
{
    if (milan_acmp_talker != nullptr) {
        return talker_stream_of(milan_acmp_talker->get_stream(stream_index));
    }
    return talker_stream_of(acmp_talker.get_stream(stream_index));
}

auto NanoAvbComponents::listener_sink(uint16_t const stream_index) const noexcept -> std::optional<ListenerSink>
{
    if (milan_acmp_listener != nullptr) {
        return optional_sink_of(milan_acmp_listener->sink(stream_index));
    }
    return optional_sink_of(acmp_listener.get_stream(stream_index));
}

void NanoAvbComponents::print_info() const
{  // NOLINT(statusbar-block-statements) the info print; pre-dates the gate
    std::print("Components initialized:\n");
    std::print("  - EntityModel with {} configuration(s)\n", entity_model.configuration_count());
    std::print("  - AemCommandHandler\n");
    std::print("  - NanoAvbAdpAdvertiser\n");
    std::print("  - NanoAvbAcmpTalker with {} streams\n", acmp_talker.max_streams());
    std::print("  - NanoAvbAcmpListener with {} streams\n", acmp_listener.max_streams());
    std::print("  - MvrpHandler\n");
    std::print("  - MsrpHandler\n");
}

//
// AtdeccNetHandler implementation
//

void AtdeccNetHandler::on_ready(int64_t now_ns)
{
    ieee::Eui48 src_mac{};
    ieee::Eui48 dest_mac{};

    while (true) {
        auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
        if (!result || *result <= 0) {
            break;
        }
        auto const len = static_cast<size_t>(*result);
        dispatch_frame(now_ns, src_mac, {payload_buf_.data(), len});
    }
}

auto AtdeccNetHandler::send(ieee::Eui48 const& dest_mac, std::span<uint8_t const> payload) -> StatusValue<ssize_t>
{
    auto const sent = static_cast<ssize_t>(payload.size());
    if (AtdeccLocalPort::is_local_mac(dest_mac)) {
        // A local controller's address (known now, forgotten, or of a port
        // since closed): never the network's business.
        if (local_port_ != nullptr && local_port_->send(dest_mac, payload)) {
            return success(sent);
        }
        return failure(std::errc::host_unreachable);
    }
    if (local_port_ != nullptr && dest_mac.is_multicast()) {
        bool const local = local_port_->send(dest_mac, payload);
        if (!context_.valid()) {
            return local ? StatusValue<ssize_t>{sent} : failure(std::errc::network_unreachable);
        }
    }
    return context_.send(&dest_mac, payload);
}

void AtdeccNetHandler::attach_local_port(AtdeccLocalPort* const port)
{
    if (local_port_ != nullptr) {
        local_port_->set_receiver({});
        local_port_->set_on_client_gone({});
    }
    local_port_ = port;
    if (port == nullptr) {
        return;
    }
    port->set_receiver([this](int64_t const now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> frame) {
        receive_local_frame(now_ns, src_mac, frame);
    });
    // A controller that has gone cannot deregister or release: drop its
    // registration (it holds a slot) and any acquisition (its MAC never comes
    // back to release it). Its entity ID may not be known to discovery.
    port->set_on_client_gone([this](ieee::Eui48 const& mac) { aem_handler_.forget_controller_at(mac); });
}

void AtdeccNetHandler::tick(int64_t now_ns)
{  // NOLINT(statusbar-block-statements) the ATDECC timers, one per protocol
    // Convert nanoseconds to sm::TimePoint for protocol handlers
    auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};

    // Tick ATDECC protocol handlers
    adp_advertiser_.tick(sm_now);
    // Note: NanoAvbAcmpTalker is event-driven, no tick needed
    acmp_listener_.tick(sm_now);  // Listener has timeout checking
    aem_handler_.tick(sm_now);    // AEM has LOCK / pending-acquire timeout checking
    if (adp_discovery_ != nullptr) {
        adp_discovery_->tick(sm_now);
    }

    ++tick_count_;

    // Periodic telemetry (wall-clock gated; emitted only when a logger is set,
    // at Debug: state changes and warnings are logged where they happen).
    if (logger_.has_value() && now_ns - last_status_ns_ >= STATUS_INTERVAL_NS) {
        last_status_ns_ = now_ns;
        log_status();
    }
}

namespace {

/// The advertise state for the status line.
[[nodiscard]] auto adp_state_label(AdpAdvertiserState const state) noexcept -> logging::StrLit
{
    switch (state) {
        case AdpAdvertiserState::Down:
            return logging::lit("Down");
        case AdpAdvertiserState::Waiting:
            return logging::lit("Waiting");
        case AdpAdvertiserState::Delay:
            return logging::lit("Delay");
        default:
            return logging::lit("Start");
    }
}

}  // namespace

void AtdeccNetHandler::log_status()
{
    int64_t const wakes = ptp_wake_count_ != nullptr ? ptp_wake_count_->load() : 0;

    // Count talker connections
    size_t talker_connections = 0;
    for (size_t i = 0; i < acmp_talker_.max_streams(); ++i) {
        talker_connections += acmp_talker_.connection_count(i);
    }

    // Count listener connections (streams whose ACMP CONNECT_RX completed)
    size_t listener_connections = 0;
    for (size_t i = 0; i < acmp_listener_.max_streams(); ++i) {
        if (acmp_listener_.is_connected(static_cast<uint16_t>(i))) {
            ++listener_connections;
        }
    }

    // NOLINTNEXTLINE(bugprone-unchecked-optional-access) - tick() calls this only with a logger set
    logger_->debug(
        "ticks={} ptp_wakes={} adp={} acmp_t={} acmp_l={}",
        tick_count_,
        wakes,
        adp_state_label(adp_advertiser_.state()),
        talker_connections,
        listener_connections());
}

auto AtdeccNetHandler::listener_connections() const -> size_t
{
    // Stream inputs whose CONNECT_RX completed, or under Milan those settled on their talker's stream.
    size_t count = 0;
    for (size_t i = 0; i < acmp_listener_.max_streams(); ++i) {
        auto const index = static_cast<uint16_t>(i);
        bool const milan_settled = milan_listener_ != nullptr && milan_listener_->is_settled(index);
        count += (acmp_listener_.is_connected(index) || milan_settled) ? 1U : 0U;
    }
    return count;
}

void AtdeccNetHandler::advertiser_receive(
    atdecc::AdpDu const& adp, ieee::Eui48 const& src_mac, sm::TimePoint const now, bool const local)
{
    // The local socket is not an AVB interface (Milan v1.2 Section 5.6.3): a local discover
    // is answered at once, to that controller alone, outside the advertise state machine.
    if (!local) {
        adp_advertiser_.receive_adpdu(adp, now);
    } else if (local_port_ != nullptr && adp_advertiser_.is_discover_for_us(adp)) {
        (void)adp_advertiser_.reply_available([&](AdpDu const& du) { return local_port_->send(src_mac, as_bytes(du)); });
    }
}

void AtdeccNetHandler::dispatch_adp(atdecc::AdpDu const& adp, ieee::Eui48 const& src_mac, sm::TimePoint const now, bool const local)
{
    advertiser_receive(adp, src_mac, now, local);
    if (adp_discovery_ != nullptr) {
        adp_discovery_->receive_adpdu(adp, src_mac, now);
    }
    // The Milan listener's per-sink talker discovery (Section 5.6.4)
    // compares the talker's gPTP grandmaster and domain with ours.
    acmp_listener_.receive_adpdu(
        adp, adp_advertiser_.adpdu().gptp_grandmaster_id, adp_advertiser_.adpdu().gptp_domain_number.get(), now);
    // A departing talker tears down the sinks still connected to it: ACMP clears a sink only
    // on a controller's DISCONNECT_RX, so it would otherwise stay connected (and hold its SRP
    // reservation) forever.
    if (adp.is_entity_departing()) {
        (void)acmp_listener_.on_talker_departed(adp.entity_id, now);
        // A departing controller's unsolicited registration is now stale.
        (void)aem_handler_.remove_unsolicited_registrations_for(adp.entity_id);
    }
}

void AtdeccNetHandler::dispatch_frame(
    int64_t now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> payload, bool const local)
{
    if (payload.empty()) {
        return;
    }
    // Convert nanoseconds to sm::TimePoint for protocol handlers
    auto const sm_now = sm::TimePoint{std::chrono::nanoseconds{now_ns}};

    switch (subtype) {
        case AvtpSubtype::adp: {  // NOLINT(statusbar-block-statements) one ADP frame's dispatch; pre-dates the gate
            // ADP message
            if (payload.size() < AdpDu::LENGTH) {
                return;
            }
            AdpDu adp{};
            span_load(adp, payload);
            dispatch_adp(adp, src_mac, sm_now, local);
            break;
        case avtp::AvtpSubtype::acmp:
            dispatch_acmp(payload, sm_now);
            break;
        }

        case AvtpSubtype::aecp: {
            // AECP message - an MVU command to the MVU handler, anything else
            // to the AEM handler; each sends its unicast response
            auto const our_entity_id = adp_advertiser_.adpdu().entity_id;
            (void)dispatch_aecp(aem_handler_, mvu_handler_, src_mac, payload, our_entity_id, sm_now);
            break;
        default:
            // Unknown ATDECC subtype
            break;
    }
}

void AtdeccNetHandler::dispatch_adp(ieee::Eui48 const& src_mac, std::span<uint8_t const> const payload, sm::TimePoint const now)
{
    if (payload.size() < atdecc::AdpDu::LENGTH) {
        return;
    }
    atdecc::AdpDu adp{};
    span_load(adp, payload);
    adp_advertiser_.receive_adpdu(adp, now);
    if (adp_discovery_ != nullptr) {
        adp_discovery_->receive_adpdu(adp, src_mac, now);
    }
    note_adp(adp, now);
}

void AtdeccNetHandler::note_adp(atdecc::AdpDu const& adp, sm::TimePoint const now)
{
    // Milan v1.3 5.6.4: each bound sink follows its own talker's ADP, and a
    // settled sink keeps its stream when the talker departs (5.5.3.6.41).
    if (milan_listener_ != nullptr) {
        milan_listener_->receive_adpdu(adp, now);
    }
    if (!adp.is_entity_departing()) {
        return;
    }
    // IEEE 1722.1: a talker announcing ENTITY_DEPARTING must tear down any
    // listener sink still connected to it. ACMP alone only clears a sink on
    // an explicit controller DISCONNECT_RX, so otherwise the sink would
    // report connected (and hold its SRP reservation) forever after the
    // talker left. Under Milan this listener has no sink connected.
    (void)acmp_listener_.on_talker_departed(adp.entity_id, now);
    // A departing controller's unsolicited registration is now stale.
    (void)aem_handler_.remove_unsolicited_registrations_for(adp.entity_id);
}

void AtdeccNetHandler::dispatch_acmp(std::span<uint8_t const> const payload, sm::TimePoint const now)
{
    if (payload.size() < atdecc::AcmpDu::LENGTH) {
        return;
    }
    atdecc::AcmpDu acmp{};
    span_load(acmp, payload);
    auto const pdu = atdecc::acmp_command_response_from_pdu(acmp);
    // One ACMP implementation runs: the choice was made at start-up (NanoAvbComponents::use_milan_acmp).
    if (milan_talker_ != nullptr && milan_listener_ != nullptr) {
        dispatch_milan_acmp(pdu, now);
        return;
    }
    dispatch_ieee_acmp(pdu, now);
}

void AtdeccNetHandler::dispatch_milan_acmp(atdecc::AcmpCommandResponse const& pdu, sm::TimePoint const now)
{
    (void)milan_talker_->receive_command(pdu, now);
    (void)milan_listener_->receive_controller_command(pdu, now, heard_advertisement_of(pdu.talker_entity_id));
    (void)milan_listener_->receive_talker_response(pdu, now);
}

auto AtdeccNetHandler::heard_advertisement_of(Eui64 const& talker) const -> std::optional<atdecc::MilanHeardAdvertisement>
{
    auto const* const entity = adp_discovery_ != nullptr ? adp_discovery_->find_entity(talker) : nullptr;
    if (entity == nullptr) {
        return std::nullopt;
    }
    return atdecc::MilanHeardAdvertisement{.adp = entity->adpdu, .valid_until = entity->expiry_time};
}

void AtdeccNetHandler::dispatch_ieee_acmp(atdecc::AcmpCommandResponse const& pdu, sm::TimePoint const now)
{
    // Dispatch to talker (for commands addressed to us)
    (void)acmp_talker_.receive_command(pdu, now);

    // Dispatch to listener for controller commands or talker responses
    bool const listener_handled = acmp_listener_.receive_controller_command(pdu, now);
    (void)acmp_listener_.receive_talker_response(pdu, now);
    trace_listener_command(pdu, listener_handled);
}

void AtdeccNetHandler::trace_listener_command(atdecc::AcmpCommandResponse const& pdu, bool const handled) const
{
    // Diagnostic: trace listener-directed commands (CONNECT_RX=6,
    // DISCONNECT_RX=8, GET_RX_STATE=10) so a silent listener can be
    // distinguished from an unhandled one. Only for commands addressed
    // to OUR listener — the ACMP multicast carries every entity's
    // traffic, and a controller polling some OTHER listener's RX state
    // (seen ~1/s in the LA install) would otherwise flood the journal.
    auto const mt = pdu.message_type();
    if ((mt != 6 && mt != 8 && mt != 10) || pdu.listener_entity_id != acmp_listener_.entity_id()) {
        return;
    }
    std::print(
        stderr,
        "[acmp-rx] mt={} target_listener={} my_listener={} handled={} state={}\n",
        mt,
        ieee::to_string(pdu.listener_entity_id).view(),
        ieee::to_string(acmp_listener_.entity_id()).view(),
        handled ? 1 : 0,
        static_cast<int>(acmp_listener_.current_state()));
}

void AtdeccNetHandler::dispatch_aecp(ieee::Eui48 const& src_mac, std::span<uint8_t const> const payload, sm::TimePoint const now)
{
    // AECP/AEM message - process and send unicast response
    auto const our_entity_id = adp_advertiser_.adpdu().entity_id;
    // Advance the handler's clock so a LOCK/ACQUIRE deadline set while
    // processing this command is relative to now, not epoch 0 (tick()
    // above then checks those deadlines each reactor tick).
    aem_handler_.set_event_time(now);
    (void)aem_handler_.process_packet(src_mac, payload, our_entity_id);
}

//
// NanoAvbComponents constructor
//

namespace {

/// The STREAM_INPUT descriptors in configuration 0 of @p storage, at most the Milan listener's sinks;
/// nullopt without a blob (the sinks are the inputs).
auto stream_input_count(atdecc::aem::DescriptorStorage const* const storage) noexcept -> std::optional<size_t>
{
    if (storage == nullptr) {
        return std::nullopt;
    }
    size_t count = 0;
    while (count < atdecc::MilanListener::MAX_SINKS &&
           storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_STREAM_INPUT, static_cast<uint16_t>(count)).has_value()) {
        ++count;
    }
    return count;
}

/// The CLOCK_DOMAIN descriptors in configuration 0 of @p storage (one, the
/// MVU handler's default, without a blob).
auto clock_domain_count(atdecc::aem::DescriptorStorage const* const storage) noexcept -> size_t
{
    if (storage == nullptr) {
        return 1;
    }
    size_t count = 0;
    while (count < MvuCommandHandler::MAX_CLOCK_DOMAINS &&
           storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, static_cast<uint16_t>(count)).has_value()) {
        ++count;
    }
    return count;
}

/// The entity's MSRP configuration: Milan's timers for a Milan entity (the immediate leave is set_instant_leave),
/// IEEE 802.1Q's for a classic one.
[[nodiscard]] auto profile_msrp_config(EntityProfile const profile, statusbar::srp::msrp::MsrpConfig const& config) noexcept
    -> statusbar::srp::msrp::MsrpConfig
{
    return profile.milan ? milan_msrp_config(config) : config;
}

/// The entity's MVRP configuration: Milan's timers for a Milan entity, IEEE 802.1Q's otherwise.
[[nodiscard]] auto profile_mvrp_config(EntityProfile const profile) noexcept -> statusbar::srp::mvrp::MvrpConfig
{
    return profile.milan ? milan_mvrp_config({}) : statusbar::srp::mvrp::MvrpConfig{};
}

}  // namespace

// Wire each handler against the *member* `entity_model`. Member init runs in
// declaration order (entity_model first), so the references the handlers take
// point at the final, stable member — valid for the object's whole lifetime
// because NanoAvbComponents is non-movable. ACMP gets the entity id by value.
NanoAvbComponents::NanoAvbComponents(
    EntityModel model,
    AdpAdvertiserConfig const adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams)
    : entity_model{std::move(model)}
    , profile{entity_profile_from_model(entity_model.descriptor_storage())}
    , aem_handler{entity_model}
    , adp_advertiser{entity_model.get_entity(), AdpAdvertiserCallbacks{}, adp_config}
    , acmp_talker{entity_model.get_entity().entity_id, AcmpTalkerCallbacks{}, talker_max_streams, talker_max_listeners}
    , acmp_listener{entity_model.get_entity().entity_id, AcmpListenerCallbacks{}, listener_max_streams}
    , mvrp_handler{profile_mvrp_config(profile), MvrpCallbacks{}}
    , msrp_handler{profile_msrp_config(profile, entity_msrp_config(talker_max_streams, listener_max_streams)), MsrpCallbacks{}}
{
    mvu_handler.set_aem_handler(&aem_handler);  // the AEM lock gates MVU SETs too
    mvu_handler.set_clock_domain_count(clock_domain_count(entity_model.descriptor_storage()));
    mvu_handler.set_stream_input_count(stream_input_count(entity_model.descriptor_storage()));
    wire_profile();
}

namespace {

/// Resolve the ENTITY descriptor (config 0, index 0) from a symbol-aware handler so
/// ADP + ACMP can read entity_id/capabilities. Goes through on_get_entity so the
/// handler's runtime patches (e.g. entity_id derived from the NIC MAC) are applied.
auto resolve_entity_descriptor(AemEntityHandler& handler) -> DescriptorEntity
{
    DescriptorEntity desc{};
    (void)handler.on_get_entity(
        DescriptorId{
            .ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0},
            .symbol = 0},
        desc);
    return desc;
}

}  // namespace

NanoAvbComponents::NanoAvbComponents(
    AemEntityHandler& handler,
    AdpAdvertiserConfig const adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams)
    : NanoAvbComponents{
          handler, resolve_entity_descriptor(handler), adp_config, talker_max_streams, talker_max_listeners, listener_max_streams}
{}

NanoAvbComponents::NanoAvbComponents(
    AemEntityHandler& handler,
    DescriptorEntity const& entity,
    AdpAdvertiserConfig const adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams)
    : entity_model{}  // empty: descriptors are served by the handler, not this member
    , profile{entity_profile_from_model(handler.descriptor_storage())}
    , aem_handler{handler}
    , adp_advertiser{entity, AdpAdvertiserCallbacks{}, adp_config}
    , acmp_talker{entity.entity_id, AcmpTalkerCallbacks{}, talker_max_streams, talker_max_listeners}
    , acmp_listener{entity.entity_id, AcmpListenerCallbacks{}, listener_max_streams}
    , mvrp_handler{profile_mvrp_config(profile), MvrpCallbacks{}}
    , msrp_handler{profile_msrp_config(profile, entity_msrp_config(talker_max_streams, listener_max_streams)), MsrpCallbacks{}}
{
    mvu_handler.set_aem_handler(&aem_handler);  // the AEM lock gates MVU SETs too
    mvu_handler.set_clock_domain_count(clock_domain_count(handler.descriptor_storage()));
    mvu_handler.set_stream_input_count(stream_input_count(handler.descriptor_storage()));
    wire_profile();
}

void NanoAvbComponents::wire_mvu_binding()
{
    aem_handler.set_entity_profile(profile);
    mvu_handler.set_profile(profile);
    // Milan v1.3 Sections 5.4.4.6-8: MVU binding addresses the Milan sinks, and their
    // GET_STREAM_INPUT_INFO_EX goes to the registered controllers as it changes (Table 5.21).
    mvu_handler.set_milan_listener(&acmp_listener.milan_sinks());
    acmp_listener.milan_sinks().set_on_info_ex_changed(
        [this](uint16_t const index) { mvu_handler.notify_stream_input_info_ex(index); });
    acmp_listener.milan_sinks().set_configuration_departs(profile.milan);
}

void NanoAvbComponents::wire_profile()
{
    wire_mvu_binding();
    acmp_talker.set_milan(profile.milan);
    acmp_listener.set_milan(profile.milan);
    // Milan v1.3 Section 5.5.3.6.3 step 1: a locked entity accepts bindings
    // only from the locking controller.
    acmp_listener.milan_sinks().set_authorized(
        [this](Eui64 const& controller) { return !aem_handler.exclusive_access_status(controller).has_value(); });
}

//
// NanoAvbComponentsBuilder implementation
//

auto NanoAvbComponentsBuilder::build(sm::TimePoint const now) -> StatusValue<std::unique_ptr<NanoAvbComponents>>
{
    // Validate required components
    if (!has_entity_model_) {
        return failure(make_error_code(NanoAvbError::InvalidConfiguration));
    }

    // Validate VLAN ID
    if (default_vlan_id_ == 0 || default_vlan_id_ >= 4095) {
        return failure(make_error_code(NanoAvbError::InvalidVlanId));
    }

    // Construct in place on the heap (non-movable type); the constructor wires
    // all internal cross-references against the owned entity model.
    auto components = std::make_unique<NanoAvbComponents>(
        std::move(entity_model_), adp_config_, talker_max_streams_, talker_max_listeners_, listener_max_streams_);

    // Post-construction setup that needs builder-supplied values.
    (void)components->mvrp_handler.register_vlan(default_vlan_id_, now);
    components->msrp_handler.set_domain(sr_domain_);

    return success(std::move(components));
}

//
// NanoAvbNetHandlers implementation
//

namespace {

/// Hand @p handler to the reactor when its socket opened and keep it in
/// @p unopened when it did not, so the pointer returned is good either way.
template <typename Handler>
auto place_handler(net::MessageReactor& reactor, std::unique_ptr<Handler> handler, std::unique_ptr<Handler>& unopened, bool& valid)
    -> Handler*
{
    Handler* const raw = handler.get();
    valid = raw->valid();
    if (valid) {
        reactor.add(std::move(handler));
    } else {
        unopened = std::move(handler);
    }
    return raw;
}

}  // namespace

void NanoAvbNetHandlers::wire_discovery_ageout()
{
    // Wire discovery ageout -> listener teardown. A talker that is powered off never
    // sends ENTITY_DEPARTING, so the explicit-departing path in dispatch_frame can't
    // catch it; the discovery database expires the stale entity and fires this, which
    // tears down any sink still connected to it. The AtdeccNetHandler feeds this
    // discovery received ADPDUs and ticks it. (A Milan sink times its own talker out,
    // 5.6.4.5.4; the IEEE 1722.1 listener then has no sink to tear down.)
    AdpDiscoveryCallbacks discovery_callbacks{};
    discovery_callbacks.on_entity_departing = [this](Eui64 id, sm::TimePoint now) {
        (void)components_->acmp_listener.on_talker_departed(id, now);
        // Aged-out controller: drop its now-stale unsolicited registration too.
        (void)components_->aem_handler.remove_unsolicited_registrations_for(id);
    };
    discovery_.set_callbacks(std::move(discovery_callbacks));
}

auto NanoAvbNetHandlers::make_atdecc_handler() -> std::unique_ptr<AtdeccNetHandler>
{
    auto atdecc = std::make_unique<AtdeccNetHandler>(
        interface_name_,
        components_->adp_advertiser,
        components_->acmp_talker,
        components_->acmp_listener,
        components_->aem_handler);
    wire_discovery_ageout();
    atdecc->set_adp_discovery(&discovery_);
    // Null for an entity that runs IEEE 1722.1 ACMP: nothing changes for it.
    atdecc->set_milan_acmp(components_->milan_acmp_talker.get(), components_->milan_acmp_listener.get());
    return atdecc;
}

    auto gptp = std::make_unique<GptpAnnounceHandler>(interface_name_);

    mvrp_valid_ = mvrp->valid();
    msrp_valid_ = msrp->valid();
    atdecc_valid_ = atdecc->valid();
    gptp_valid_ = gptp->valid();

    // Capture raw pointers before ownership transfer
    mvrp_ptr_ = mvrp.get();
    msrp_ptr_ = msrp.get();
    atdecc_ptr_ = atdecc.get();
    gptp_ptr_ = gptp.get();

    if (!local_port_config_.path.empty()) {
        if (auto port = AtdeccLocalPort::open(local_port_config_)) {
            local_ptr_ = port->get();
            atdecc->attach_local_port(local_ptr_);
            reactor.add(std::move(*port));
        } else {
            local_port_error_ = port.error();
        }
    }

    // Transfer ownership to reactor (only valid handlers; the ATDECC handler
    // always: tick-only without its socket, it still serves the local port
    // and runs the ADP/ACMP/AEM timers). The rest stay owned here.
    auto const place = [this, &reactor](bool const valid, std::unique_ptr<net::Pollable> handler) {
        if (valid) {
            reactor.add(std::move(handler));
        } else {
            unopened_.push_back(std::move(handler));
        }
    };
    place(mvrp_valid_, std::move(mvrp));
    place(msrp_valid_, std::move(msrp));
    place(true, std::move(atdecc));
    place(gptp_valid_, std::move(gptp));
}

void NanoAvbNetHandlers::close_local_port()
{
    if (local_ptr_ == nullptr) {
        return;
    }
    // Closing first forgets its controllers through the entity (their
    // registrations and acquisition go); then nothing more reaches it.
    local_ptr_->close();  // finished(): the reactor drops it
    atdecc_ptr_->attach_local_port(nullptr);
    local_ptr_ = nullptr;
}

void NanoAvbNetHandlers::print_status() const
{
    std::print(
        "Network handlers: MVRP={} MSRP={} ATDECC={} gPTP={}\n",
        mvrp_valid_ ? "OK" : "FAIL",
        msrp_valid_ ? "OK" : "FAIL",
        atdecc_valid_ ? "OK" : "FAIL",
        gptp_valid_ ? "OK" : "FAIL");
}

void NanoAvbNetHandlers::print_warnings(std::string_view interface_name) const
{
    if (!mvrp_valid_) {
        std::print(stderr, "Warning: Could not open MVRP socket on {}\n", interface_name);
    }
    if (!msrp_valid_) {
        std::print(stderr, "Warning: Could not open MSRP socket on {}\n", interface_name);
    }
    if (!atdecc_valid_) {
        std::print(stderr, "Warning: Could not open ATDECC socket on {}\n", interface_name);
    }
    if (local_port_error_) {
        std::print(
            stderr, "Warning: Could not open local ATDECC socket {}: {}\n", local_port_config_.path, local_port_error_.message());
    }
    if (!gptp_valid_) {
        std::print(stderr, "Warning: Could not open gPTP socket on {}\n", interface_name);
    }
}

void setup_nanoavb_callbacks(NanoAvbComponents& components, NanoAvbNetHandlers& handlers)
{  // NOLINT(statusbar-block-statements) one wiring statement per component; pre-dates the gate
    // ADP: send Entity Available/Departing to multicast
    components.adp_advertiser.set_callbacks(AdpAdvertiserCallbacks{.send_adpdu = [&handlers](atdecc::AdpDu const& adpdu) -> bool {
        auto& atdecc = handlers.atdecc_handler();
        return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, as_bytes(adpdu)).has_value();
    }});

    // ACMP Talker: send responses to multicast. On L2 these must be the 56-byte
    // (control_data_length=44) short form -- real talkers/listeners (third-party devices)
    // silently drop the oversized 96-byte extended PDU. acmp_serialize_2016
    // emits the extended form only when the UDP flag is set.
    components.acmp_talker.set_callbacks(
        AcmpTalkerCallbacks{.tx_response = [&handlers](atdecc::AcmpCommandResponse const& resp) -> bool {
            auto& atdecc = handlers.atdecc_handler();
            std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
            return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, atdecc::acmp_serialize_2016(resp, buf)).has_value();
        }});

    // ACMP Listener: send commands + responses to multicast, L2 short form (see above).
    components.acmp_listener.set_callbacks(AcmpListenerCallbacks{
        .tx_command = [&handlers](atdecc::AcmpCommandResponse const& cmd) -> bool {
            auto& atdecc = handlers.atdecc_handler();
            std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
            return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, atdecc::acmp_serialize_2016(cmd, buf)).has_value();
        },
        .tx_response = [&handlers](atdecc::AcmpCommandResponse const& resp) -> bool {
            auto& atdecc = handlers.atdecc_handler();
            std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
            return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, atdecc::acmp_serialize_2016(resp, buf)).has_value();
        }});

    // AECP AEM: send responses unicast to controller (unlike ADP/ACMP which use
    // multicast). Field setter, NOT set_callbacks: replacing the whole struct
    // would wipe callbacks installed before start (identify_changed,
    // get_counters, get_stream_info).
    components.aem_handler.set_send_response([&handlers](ieee::Eui48 const& dest_mac, std::span<uint8_t const> response) -> bool {
        auto& atdecc = handlers.atdecc_handler();
        return atdecc.send(dest_mac, response).has_value();
    });

    // AECP MVU: unicast to the controller, like AEM.
    components.mvu_handler.set_send_response([&handlers](ieee::Eui48 const& dest_mac, std::span<uint8_t const> response) -> bool {
        auto& atdecc = handlers.atdecc_handler();
        return atdecc.send(dest_mac, response).has_value();
    });

    // Unsolicited notifications are addressed FROM us: seed the handler's entity_id
    // from the advertiser so they carry the right target even before the first
    // command (e.g. an IDENTIFY button pressed at startup). process_packet keeps it
    // current thereafter.
    components.aem_handler.set_entity_id(components.adp_advertiser.adpdu().entity_id);

    // MVRP and MSRP: send packets to multicast. Field setters, NOT
    // set_callbacks: replacing the whole struct would wipe the hooks the
    // entity installed before start (on_domain_adopted, on_talker_listener),
    // and the entity would stop following the Domain it adopts.
    components.mvrp_handler.set_send_packet([&handlers](std::span<uint8_t const> packet) -> bool {
        auto& mvrp = handlers.mvrp_handler();
        if (!mvrp.valid()) {
            return false;
        }
        return mvrp.send(MVRP_MULTICAST_MAC, packet).has_value();
    });

    components.msrp_handler.set_send_packet([&handlers](std::span<uint8_t const> packet) -> bool {
        auto& msrp = handlers.msrp_handler();
        if (!msrp.valid()) {
            return false;
        }
        return msrp.send(MSRP_MULTICAST_MAC, packet).has_value();
    });

    // gPTP Announce: the grandmaster in force, gained, changed or lost, is
    // reported the same way to ADP and to the registered controllers.
    handlers.gptp_handler().set_callbacks(GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&components](
                int64_t now_ns, gptp::ClockIdentity const& grandmaster_id, gptp::AnnounceMessage const& announce) -> void {
            std::string gm_str;
            gptp::format_to(std::back_inserter(gm_str), grandmaster_id);
            std::print(
                "gPTP: Grandmaster changed to {} (priority1={}, priority2={}, steps={})\n",
                gm_str,
                announce.grandmaster_priority1.get(),
                announce.grandmaster_priority2.get(),
                announce.steps_removed.get());
            report_grandmaster(
                components, grandmaster_id, announce.header.domain_number.get(), sm::TimePoint{std::chrono::nanoseconds{now_ns}});
        },
        .grandmaster_lost = [&components, &handlers](int64_t const now_ns) -> void {
            std::print("gPTP: Grandmaster lost (no Announce for the receipt timeout)\n");
            auto const* last = handlers.gptp_handler().last_announce();
            report_grandmaster(
                components,
                grandmaster_after_loss(components),
                last != nullptr ? last->header.domain_number.get() : 0,
                sm::TimePoint{std::chrono::nanoseconds{now_ns}});
        }});

    // A new path trace, or none, is an unsolicited GET_AS_PATH (IEEE 1722.1-2021 Clause 7.4.41).
    // Apart from the callbacks above, which integrators replace whole.
    handlers.gptp_handler().set_on_path_trace_changed(
        [&components](int64_t /*now_ns*/) -> void { (void)components.aem_handler.notify_as_path_changed(0); });
}

auto grandmaster_after_loss(NanoAvbComponents const& components) -> gptp::ClockIdentity
{
    // ADP names what GET_AVB_INFO answers: the entity's own clock while it is grandmaster (Milan v1.3
    // Section 4.2.6.2.1), else none.
    auto const info = components.aem_handler.avb_info(0);
    return info.has_value() ? gptp::ClockIdentity{info->gptp_grandmaster_id.to_uint64()} : gptp::ClockIdentity{};
}

void report_grandmaster(
    NanoAvbComponents& components, gptp::ClockIdentity const& grandmaster_id, uint8_t const domain, sm::TimePoint const now)
{
    // ADP carries it after TMR_DELAY (Milan v1.2 Section 5.6.3.5.7).
    components.adp_advertiser.set_gptp_info(grandmaster_id, domain, now);
    // IEEE 1722.1 7.5.2: a grandmaster change is also an unsolicited
    // GET_AVB_INFO to the registered controllers (no-op until an entity wires
    // get_avb_info).
    (void)components.aem_handler.notify_avb_info_changed(0);
}

}  // namespace statusbar::nanoavb
