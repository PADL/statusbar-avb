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

        // Check if it's an Announce message and extract grandmaster identity
        if (auto const* announce = std::get_if<AnnounceMessage>(&*msg)) {
            auto const new_gm = announce->grandmaster_identity;
            bool const gm_changed = (new_gm != grandmaster_identity_);

            if (gm_changed) {
                grandmaster_identity_ = new_gm;

                // Invoke callback if set — convert nanoseconds to net::TimePoint for compatibility
                if (callbacks_.grandmaster_id_changed) {
                    auto const tp = now_ns;
                    callbacks_.grandmaster_id_changed(tp, new_gm, *announce);
                }
            }
            last_announce_time_ns_ = now_ns;
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
{
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

void AtdeccNetHandler::dispatch_frame(int64_t now_ns, ieee::Eui48 const& src_mac, std::span<uint8_t const> payload)
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
            adp_advertiser_.receive_adpdu(adp, sm_now);
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
            // distinguished from a misaddressed/unhandled one. These are rare
            // (only on a controller connect/probe), so the log is not hot.
            if (auto const mt = cmd_resp.message_type(); mt == 6 || mt == 8 || mt == 10) {
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
            // AECP/AEM message - process and send unicast response
            auto const our_entity_id = adp_advertiser_.adpdu().entity_id;
            // Advance the handler's clock so a LOCK/ACQUIRE deadline set while
            // processing this command is relative to now, not epoch 0 (tick()
            // above then checks those deadlines each reactor tick).
            aem_handler_.set_event_time(sm_now);
            (void)aem_handler_.process_packet(src_mac, payload, our_entity_id);
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
    size_t const listener_max_streams)
    : entity_model{std::move(model)}
    , aem_handler{entity_model}
    , adp_advertiser{entity_model.get_entity(), AdpAdvertiserCallbacks{}, adp_config}
    , acmp_talker{entity_model.get_entity().entity_id, AcmpTalkerCallbacks{}, talker_max_streams, talker_max_listeners}
    , acmp_listener{entity_model.get_entity().entity_id, AcmpListenerCallbacks{}, listener_max_streams}
    , mvrp_handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{}}
    , msrp_handler{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{}}
{}

namespace {

/// Resolve the ENTITY descriptor (config 0, index 0) from a symbol-aware handler so
/// ADP + ACMP can read entity_id/capabilities. Goes through on_get_entity so the
/// handler's runtime patches (e.g. entity_id derived from the NIC MAC) are applied.
auto resolve_entity_descriptor(AemEntityHandler& handler) -> DescriptorEntity
{
    DescriptorEntity desc{};
    (void)handler.on_get_entity(
        DescriptorRef{.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0},
        /*symbol=*/0,
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
    , aem_handler{handler}
    , adp_advertiser{entity, AdpAdvertiserCallbacks{}, adp_config}
    , acmp_talker{entity.entity_id, AcmpTalkerCallbacks{}, talker_max_streams, talker_max_listeners}
    , acmp_listener{entity.entity_id, AcmpListenerCallbacks{}, listener_max_streams}
    , mvrp_handler{statusbar::srp::mvrp::MvrpConfig{}, MvrpCallbacks{}}
    , msrp_handler{statusbar::srp::msrp::MsrpConfig{}, MsrpCallbacks{}}
{}

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
        components_->aem_handler);

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

    // Transfer ownership to reactor (only valid handlers)
    if (mvrp_valid_) {
        reactor.add(std::move(mvrp));
    }
    if (msrp_valid_) {
        reactor.add(std::move(msrp));
    }
    if (atdecc_valid_) {
        reactor.add(std::move(atdecc));
    }
    if (gptp_valid_) {
        reactor.add(std::move(gptp));
    }
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
    if (!gptp_valid_) {
        std::print(stderr, "Warning: Could not open gPTP socket on {}\n", interface_name);
    }
}

void setup_nanoavb_callbacks(NanoAvbComponents& components, NanoAvbNetHandlers& handlers)
{
    // ADP: send Entity Available/Departing to multicast
    components.adp_advertiser.set_callbacks(AdpAdvertiserCallbacks{.send_adpdu = [&handlers](atdecc::AdpDu const& adpdu) -> bool {
        auto& atdecc = handlers.atdecc_handler();
        if (!atdecc.valid()) {
            return false;
        }
        return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, as_bytes(adpdu)).has_value();
    }});

    // ACMP Talker: send responses to multicast. On L2 these must be the 56-byte
    // (control_data_length=44) short form -- real talkers/listeners (third-party devices)
    // silently drop the oversized 96-byte extended PDU. acmp_serialize_2016
    // emits the extended form only when the UDP flag is set.
    components.acmp_talker.set_callbacks(
        AcmpTalkerCallbacks{.tx_response = [&handlers](atdecc::AcmpCommandResponse const& resp) -> bool {
            auto& atdecc = handlers.atdecc_handler();
            if (!atdecc.valid()) {
                return false;
            }
            std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
            return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, atdecc::acmp_serialize_2016(resp, buf)).has_value();
        }});

    // ACMP Listener: send commands + responses to multicast, L2 short form (see above).
    components.acmp_listener.set_callbacks(AcmpListenerCallbacks{
        .tx_command = [&handlers](atdecc::AcmpCommandResponse const& cmd) -> bool {
            auto& atdecc = handlers.atdecc_handler();
            if (!atdecc.valid()) {
                return false;
            }
            std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
            return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, atdecc::acmp_serialize_2016(cmd, buf)).has_value();
        },
        .tx_response = [&handlers](atdecc::AcmpCommandResponse const& resp) -> bool {
            auto& atdecc = handlers.atdecc_handler();
            if (!atdecc.valid()) {
                return false;
            }
            std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
            return atdecc.send(atdecc::ATDECC_MULTICAST_MAC, atdecc::acmp_serialize_2016(resp, buf)).has_value();
        }});

    // AECP AEM: send responses unicast to controller (unlike ADP/ACMP which use
    // multicast). Field setter, NOT set_callbacks: replacing the whole struct
    // would wipe callbacks installed before start (identify_changed,
    // get_counters, get_stream_info).
    components.aem_handler.set_send_response([&handlers](ieee::Eui48 const& dest_mac, std::span<uint8_t const> response) -> bool {
        auto& atdecc = handlers.atdecc_handler();
        if (!atdecc.valid()) {
            return false;
        }
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

            // Update ADP advertiser with new grandmaster info
            // Domain 0 is the default gPTP domain
            components.adp_advertiser.set_gptp_info(grandmaster_id, 0);

            // Notify that entity state has changed (triggers ADP announcement)
            components.adp_advertiser.notify_entity_changed();
        }});
}

}  // namespace statusbar::nanoavb
