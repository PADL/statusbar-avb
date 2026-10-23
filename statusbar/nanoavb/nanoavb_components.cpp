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

    while (true) {
        auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
        if (!result || *result <= 0) {
            break;
        }
        auto const len = static_cast<size_t>(*result);
        auto const payload = std::span<uint8_t const>{payload_buf_.data(), len};

        auto const msg = parse_gptp(payload);
        if (!msg) {
            continue;
        }

        if (auto const* announce = std::get_if<AnnounceMessage>(&*msg)) {
            on_announce(*announce, now_ns);
        }
    }
}

void GptpAnnounceHandler::on_announce(gptp::AnnounceMessage const& announce, int64_t const now_ns)
{
    auto const new_gm = announce.grandmaster_identity;
    if (new_gm != grandmaster_identity_) {
        grandmaster_identity_ = new_gm;
        if (callbacks_.grandmaster_id_changed) {
            callbacks_.grandmaster_id_changed(now_ns, new_gm, announce);
        }
    }
    // announceReceiptTimeout x the sender's announce interval, at the slow end of Milan v1.2
    // Tables 4.1-4.2 (+50%: 4.5 s at a 1 s interval); the log interval is kept to [-3, 4].
    auto const log_interval = std::clamp<int>(static_cast<int8_t>(announce.header.log_message_interval.get()), -3, 4);
    int64_t const interval_ns =
        log_interval >= 0 ? (INT64_C(1'000'000'000) << log_interval) : (INT64_C(1'000'000'000) >> -log_interval);
    announce_timeout_ns_ =
        ANNOUNCE_RECEIPT_TIMEOUT * interval_ns * ANNOUNCE_INTERVAL_TOLERANCE_NUM / ANNOUNCE_INTERVAL_TOLERANCE_DEN;
    last_announce_time_ns_ = now_ns;
    last_announce_ = announce;
    has_announce_ = true;
}

void GptpAnnounceHandler::tick(int64_t const now_ns)
{
    if (!grandmaster_identity_.is_set() || !has_announce_ || now_ns - last_announce_time_ns_ <= announce_timeout_ns_) {
        return;
    }
    grandmaster_identity_ = gptp::ClockIdentity{};
    if (callbacks_.grandmaster_lost) {
        callbacks_.grandmaster_lost(now_ns);
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
{
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
    if (local_port_ != nullptr) {
        if (dest_mac.is_multicast()) {
            bool const local = local_port_->send(local_reply_to_.value_or(dest_mac), payload);
            if (local_reply_to_ || !context_.valid()) {
                return local ? StatusValue<ssize_t>{sent} : failure(std::errc::network_unreachable);
            }
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
{
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

    // Periodic status (wall-clock gated; emitted only when a logger is set).
    if (logger_.has_value() && now_ns - last_status_ns_ >= STATUS_INTERVAL_NS) {
        last_status_ns_ = now_ns;
        log_status();
    }
}

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

    logger_->status(
        "ticks={} ptp_wakes={} adp={} acmp_t={} acmp_l={}",
        tick_count_,
        wakes,
        adp_advertiser_.state() == AdpAdvertiserState::Advertising ? logging::lit("Advertising") : logging::lit("Stopped"),
        talker_connections,
        listener_connections);
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
        case AvtpSubtype::adp: {
            // ADP message
            if (payload.size() < AdpDu::LENGTH) {
                return;
            }
            AdpDu adp{};
            span_load(adp, payload);
            // A local controller's ENTITY_DISCOVER is answered to it alone:
            // its discovery is no reason to advertise on the network (or to
            // the other local controllers) every few seconds.
            if (local) {
                local_reply_to_ = src_mac;
            }
            adp_advertiser_.receive_adpdu(adp, sm_now);
            local_reply_to_.reset();
            // The local socket does not depend on the network: with the
            // advertiser stopped (link down), a local discover is still
            // answered with the entity's ADPDU.
            if (local && local_port_ != nullptr && adp.is_entity_discover() &&
                adp_advertiser_.state() != AdpAdvertiserState::Advertising &&
                (adp.entity_id == Eui64{} || adp.entity_id == adp_advertiser_.adpdu().entity_id)) {
                (void)local_port_->send(src_mac, as_bytes(adp_advertiser_.adpdu()));
            }
            if (adp_discovery_ != nullptr) {
                adp_discovery_->receive_adpdu(adp, src_mac, sm_now);
            }
            // A talker announcing ENTITY_DEPARTING must tear down any listener sink
            // still connected to it: ACMP alone only clears a sink on an explicit
            // controller DISCONNECT_RX, so otherwise the sink would report connected
            // (and hold its SRP reservation) forever after the talker left.
            if (adp.is_entity_departing()) {
                (void)acmp_listener_.on_talker_departed(adp.entity_id);
                // A departing controller's unsolicited registration is now stale.
                (void)aem_handler_.remove_unsolicited_registrations_for(adp.entity_id);
            }
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
            // Advance the handler's clock so a LOCK/ACQUIRE deadline set while
            // processing this command is relative to now, not epoch 0 (tick()
            // above then checks those deadlines each reactor tick).
            aem_handler_.set_event_time(sm_now);
            (void)dispatch_aecp(aem_handler_, mvu_handler_, src_mac, payload, our_entity_id);
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
    , mvrp_handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{}}
    , msrp_handler{entity_msrp_config(talker_max_streams, listener_max_streams), MsrpCallbacks{}}
{
    aem_handler.set_profile(profile);
    mvu_handler.set_profile(profile);
    acmp_talker.set_milan(profile.milan);
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
    , mvrp_handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{}}
    , msrp_handler{entity_msrp_config(talker_max_streams, listener_max_streams), MsrpCallbacks{}}
{
    aem_handler.set_profile(profile);
    mvu_handler.set_profile(profile);
    acmp_talker.set_milan(profile.milan);
}

//
// NanoAvbComponentsBuilder implementation
//

auto NanoAvbComponentsBuilder::build() -> StatusValue<std::unique_ptr<NanoAvbComponents>>
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
    (void)components->mvrp_handler.register_vlan(default_vlan_id_, sm::Clock::now());
    components->msrp_handler.set_domain(sr_domain_);

    return success(std::move(components));
}

//
// NanoAvbNetHandlers implementation
//

void NanoAvbNetHandlers::add_to_reactor(net::MessageReactor& reactor)
{
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
    discovery_callbacks.on_entity_departing = [this](Eui64 id) {
        (void)components_->acmp_listener.on_talker_departed(id);
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
{
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

    // gPTP Announce: notify when grandmaster changes
    handlers.gptp_handler().set_callbacks(GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&components](
                int64_t now_ns, gptp::ClockIdentity const& grandmaster_id, gptp::AnnounceMessage const& announce) -> void {
            (void)now_ns;

            // Print notification
            std::string gm_str;
            gptp::format_to(std::back_inserter(gm_str), grandmaster_id);
            std::print(
                "gPTP: Grandmaster changed to {} (priority1={}, priority2={}, steps={})\n",
                gm_str,
                announce.grandmaster_priority1.get(),
                announce.grandmaster_priority2.get(),
                announce.steps_removed.get());

            // Update ADP advertiser with the new grandmaster and the domain the
            // Announce was sent in.
            components.adp_advertiser.set_gptp_info(grandmaster_id, announce.header.domain_number.get());

            // Notify that entity state has changed (triggers ADP announcement)
            components.adp_advertiser.notify_entity_changed();

            // IEEE 1722.1 7.5.2: a grandmaster change is also an unsolicited
            // GET_AVB_INFO to the registered controllers (no-op until an
            // entity wires get_avb_info).
            (void)components.aem_handler.notify_avb_info_changed(0);
        }});
}

}  // namespace statusbar::nanoavb
