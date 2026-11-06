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

/// The TLVs after an Announce's 64-octet body, up to messageLength (empty when there are none).
auto announce_tlvs(std::span<uint8_t const> const announce) noexcept -> std::span<uint8_t const>
{
    if (announce.size() < gptp::AnnounceMessage::LENGTH) {
        return {};
    }
    size_t const message_length = (size_t{announce[2]} << 8U) | announce[3];
    auto const tlvs = announce.first(std::min(announce.size(), message_length));
    return tlvs.size() > gptp::AnnounceMessage::LENGTH ? tlvs.subspan(gptp::AnnounceMessage::LENGTH) : std::span<uint8_t const>{};
}

/// The PATH_TRACE TLV's value among @p tlvs; empty when absent or cut short.
auto path_trace_value(std::span<uint8_t const> tlvs) noexcept -> std::span<uint8_t const>
{
    using gptp::TlvHeader;
    while (tlvs.size() >= TlvHeader::LENGTH) {
        uint16_t const type = static_cast<uint16_t>((uint16_t{tlvs[0]} << 8U) | tlvs[1]);
        size_t const length = (size_t{tlvs[2]} << 8U) | tlvs[3];
        if (tlvs.size() < TlvHeader::LENGTH + length) {
            return {};
        }
        if (type == gptp::TLV_TYPE_PATH_TRACE) {
            return tlvs.subspan(TlvHeader::LENGTH, length);
        }
        tlvs = tlvs.subspan(TlvHeader::LENGTH + length);
    }
    return {};
}

}  // namespace

auto parse_announce_path_trace(std::span<uint8_t const> const announce) noexcept -> AsPath
{
    AsPath path{};
    auto const value = path_trace_value(announce_tlvs(announce));
    for (size_t at = 0; at + 8 <= value.size() && path.size() < AsPath::capacity(); at += 8) {
        Eui64 id{};
        span_copy(id.span(), value.subspan(at, 8));
        path.push_back(id);
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
    observe_path_trace(now_ns, announce, path_trace);
    if (announce.grandmaster_identity == grandmaster_identity_) {
        return;
    }
    grandmaster_identity_ = announce.grandmaster_identity;
    if (callbacks_.grandmaster_id_changed) {
        callbacks_.grandmaster_id_changed(now_ns, grandmaster_identity_, announce);
    }
}

void GptpAnnounceHandler::observe_path_trace(int64_t const now_ns, gptp::AnnounceMessage const& announce, AsPath const& path_trace)
{
    // An Announce without a path trace names its grandmaster alone.
    AsPath path = path_trace;
    if (path.empty()) {
        path.push_back(announce.grandmaster_identity.to_eui64());
    }
    if (std::ranges::equal(path, path_trace_)) {
        return;
    }
    path_trace_ = path;
    if (callbacks_.path_trace_changed) {
        callbacks_.path_trace_changed(now_ns);
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
    if (!path_trace_.empty()) {
        path_trace_.clear();
        if (callbacks_.path_trace_changed) {
            callbacks_.path_trace_changed(now_ns);
        }
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

void NanoAvbComponents::print_info() const
{  // NOLINT(statusbar-block-statements) the info print; pre-dates the gate
    std::print("Components initialized:\n");
    std::print("  - EntityModel with {} configuration(s)\n", entity_model.configuration_count());
    std::print("  - AemCommandHandler\n");
    std::print("  - MvuCommandHandler\n");
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
        listener_connections);
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
    using namespace statusbar::atdecc;
    namespace AvtpSubtype = avtp::AvtpSubtype;

    if (payload.empty()) {
        return;
    }

    // First byte is the AVTP subtype
    uint8_t const subtype = payload[0];

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
        }

        case AvtpSubtype::acmp: {
            // ACMP message
            if (payload.size() < AcmpDu::LENGTH) {
                return;
            }
            AcmpDu acmp{};
            span_load(acmp, payload);
            auto const cmd_resp = acmp_command_response_from_pdu(acmp);

            // Dispatch to talker (for commands addressed to us)
            (void)acmp_talker_.receive_command(cmd_resp, sm_now);

            // Dispatch to listener for controller commands or talker responses
            bool const listener_handled = acmp_listener_.receive_controller_command(cmd_resp, sm_now);
            (void)acmp_listener_.receive_talker_response(cmd_resp, sm_now);

            // Diagnostic: trace listener-directed commands (CONNECT_RX=6,
            // DISCONNECT_RX=8, GET_RX_STATE=10) so a silent listener can be
            // distinguished from an unhandled one. Only for commands addressed
            // to OUR listener — the ACMP multicast carries every entity's
            // traffic, and a controller polling some OTHER listener's RX state
            // (seen ~1/s in the LA install) would otherwise flood the journal.
            if (auto const mt = cmd_resp.message_type();
                (mt == 6 || mt == 8 || mt == 10) && cmd_resp.listener_entity_id == acmp_listener_.entity_id()) {
                std::print(
                    stderr,
                    "[acmp-rx] mt={} target_listener={} my_listener={} handled={} state={}\n",
                    mt,
                    ieee::to_string(cmd_resp.listener_entity_id).view(),
                    ieee::to_string(acmp_listener_.entity_id()).view(),
                    listener_handled ? 1 : 0,
                    static_cast<int>(acmp_listener_.current_state()));
            }
            break;
        }

        case AvtpSubtype::aecp: {
            // AECP message - an MVU command to the MVU handler, anything else
            // to the AEM handler; each sends its unicast response
            auto const our_entity_id = adp_advertiser_.adpdu().entity_id;
            (void)dispatch_aecp(aem_handler_, mvu_handler_, src_mac, payload, our_entity_id, sm_now);
            break;
        }

        default:
            // Unknown ATDECC subtype
            break;
    }
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
    size_t const listener_max_streams,
    std::optional<bool> const milan)
    : entity_model{std::move(model)}
    , profile{entity_profile_from_model(entity_model.descriptor_storage(), milan)}
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
    size_t const listener_max_streams,
    std::optional<bool> const milan)
    : NanoAvbComponents{
          handler,
          resolve_entity_descriptor(handler),
          adp_config,
          talker_max_streams,
          talker_max_listeners,
          listener_max_streams,
          milan}
{}

NanoAvbComponents::NanoAvbComponents(
    AemEntityHandler& handler,
    DescriptorEntity const& entity,
    AdpAdvertiserConfig const adp_config,
    size_t const talker_max_streams,
    size_t const talker_max_listeners,
    size_t const listener_max_streams,
    std::optional<bool> const milan)
    : entity_model{}  // empty: descriptors are served by the handler, not this member
    , profile{entity_profile_from_model(handler.descriptor_storage(), milan)}
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

void NanoAvbComponents::set_milan_version(MilanVersion const version) noexcept
{
    profile.milan_version = version;
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
    acmp_listener.milan_sinks().set_configuration_departs(profile.milan_v1_3());
}

void NanoAvbComponents::wire_profile()
{
    wire_mvu_binding();
    acmp_talker.set_milan(profile.milan);
    acmp_listener.set_milan(profile.milan);
    msrp_handler.set_listener_waits_for_talker(profile.milan);
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

void NanoAvbNetHandlers::add_to_reactor(net::MessageReactor& reactor)
{  // NOLINT(statusbar-block-statements) the reactor wiring, one handler after another; pre-dates the gate
    auto mvrp = std::make_unique<MvrpNetHandler>(interface_name_, components_->mvrp_handler);
    auto msrp = std::make_unique<MsrpNetHandler>(interface_name_, components_->msrp_handler);
    auto atdecc = std::make_unique<AtdeccNetHandler>(
        interface_name_,
        components_->adp_advertiser,
        components_->acmp_talker,
        components_->acmp_listener,
        components_->aem_handler,
        components_->mvu_handler);

    // Wire discovery ageout -> listener teardown. A talker that is powered off never
    // sends ENTITY_DEPARTING, so the explicit-departing path in dispatch_frame can't
    // catch it; the discovery database expires the stale entity and fires this, which
    // tears down any sink still connected to it. The AtdeccNetHandler feeds this
    // discovery received ADPDUs and ticks it once set below.
    AdpDiscoveryCallbacks discovery_callbacks{};
    discovery_callbacks.on_entity_departing = [this](Eui64 id, sm::TimePoint now) {
        (void)components_->acmp_listener.on_talker_departed(id, now);
        // Aged-out controller: drop its now-stale unsolicited registration too.
        (void)components_->aem_handler.remove_unsolicited_registrations_for(id);
    };
    discovery_.set_callbacks(std::move(discovery_callbacks));
    atdecc->set_adp_discovery(&discovery_);

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

    // MVRP: send packets to multicast
    components.mvrp_handler.set_callbacks(MvrpCallbacks{.send_packet = [&handlers](std::span<uint8_t const> packet) -> bool {
        auto& mvrp = handlers.mvrp_handler();
        if (!mvrp.valid()) {
            return false;
        }
        return mvrp.send(MVRP_MULTICAST_MAC, packet).has_value();
    }});

    // MSRP: send packets to multicast
    components.msrp_handler.set_callbacks(MsrpCallbacks{.send_packet = [&handlers](std::span<uint8_t const> packet) -> bool {
        auto& msrp = handlers.msrp_handler();
        if (!msrp.valid()) {
            return false;
        }
        return msrp.send(MSRP_MULTICAST_MAC, packet).has_value();
    }});

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
                gptp::ClockIdentity{},
                last != nullptr ? last->header.domain_number.get() : 0,
                sm::TimePoint{std::chrono::nanoseconds{now_ns}});
        }});
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
