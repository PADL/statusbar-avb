// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_controller.hpp"

#include <array>
#include <cstring>

namespace statusbar::nanoavb {

inline auto ns_to_timepoint(int64_t ns) -> sm::TimePoint
{
    return sm::TimePoint{std::chrono::nanoseconds{ns}};
}

using namespace atdecc;

NanoAvbAemController::NanoAvbAemController(Eui64 controller_entity_id, AemControllerEntityCallbacks callbacks)
    : controller_entity_id_{controller_entity_id}
    , callbacks_{std::move(callbacks)}
    , acmp_controller_{controller_entity_id}
{
    aem_ctx_.my_id = controller_entity_id;
    wire_all_callbacks();
}

void NanoAvbAemController::set_callbacks(AemControllerEntityCallbacks callbacks)
{
    callbacks_ = std::move(callbacks);
    wire_all_callbacks();
}

void NanoAvbAemController::start()
{
    acmp_controller_.start();
}

void NanoAvbAemController::wire_all_callbacks()
{
    // ADP discovery callbacks
    discovery_.set_callbacks({
        .send_adpdu = [this](AdpDu const& adp) -> bool {
            if (callbacks_.send_atdecc_multicast) {
                return callbacks_.send_atdecc_multicast(make_const_span(adp));
            }
            return false;
        },
        .on_entity_available =
            [this](DiscoveredEntity const& e) {
                if (callbacks_.on_entity_available) {
                    callbacks_.on_entity_available(e);
                }
            },
        .on_entity_updated =
            [this](DiscoveredEntity const& e) {
                if (callbacks_.on_entity_updated) {
                    callbacks_.on_entity_updated(e);
                }
            },
        .on_entity_departing =
            [this](Eui64 id) {
                if (callbacks_.on_entity_departing) {
                    callbacks_.on_entity_departing(id);
                }
            },
    });

    // ACMP controller callbacks
    acmp_controller_.set_callbacks({
        .tx_command = [this](AcmpCommandResponse const& cmd) -> bool {
            if (callbacks_.send_atdecc_multicast) {
                // L2 ACMP: emit the pre-2021 56-byte (control_data_length=44) short
                // form, matching the entity talker/listener TX. Per IEEE 1722.1 a
                // receiver MUST accept the longer 2021 PDU and ignore the extra
                // bytes, but several shipping devices (e.g. the DSP processor) silently drop the
                // 96-byte extended form -- so default to the interoperable short
                // form. acmp_serialize_2016 still emits the full PDU for UDP.
                std::array<uint8_t, AcmpDu2021::LENGTH> buf{};
                return callbacks_.send_atdecc_multicast(acmp_serialize_2016(cmd, buf));
            }
            return false;
        },
        .on_response =
            [this](AcmpCommandResponse const& resp) {
                if (callbacks_.on_acmp_response) {
                    callbacks_.on_acmp_response(resp);
                }
            },
        .on_timeout =
            [this](AcmpCommandResponse const& cmd) {
                if (callbacks_.on_acmp_timeout) {
                    callbacks_.on_acmp_timeout(cmd);
                }
            },
    });

    // AEM controller callbacks
    aem_ctx_.tx_command = [this](std::span<uint8_t const> packet) -> bool {
        if (packet.size() < AemDu::LENGTH) {
            return false;
        }
        // Parse target entity ID to look up destination MAC
        AemDu header{};
        span_load(header, packet);
        auto mac = discovery_.mac_for_entity(header.target_entity_id);
        if (!mac.has_value()) {
            return false;
        }
        if (callbacks_.send_atdecc_unicast) {
            return callbacks_.send_atdecc_unicast(*mac, packet);
        }
        return false;
    };

    aem_ctx_.on_response =
        [this](AemDu const& header, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data, uint8_t status) {
            if (callbacks_.on_aem_response) {
                callbacks_.on_aem_response(header.target_entity_id, header.command_code(), status, sent_payload, data);
            }
        };

    aem_ctx_.on_timeout = [this](AemDu const& header) {
        if (callbacks_.on_aem_timeout) {
            callbacks_.on_aem_timeout(header.target_entity_id, header.command_code());
        }
    };
}

// ADP Discovery

void NanoAvbAemController::discover_all()
{
    discovery_.discover_all();
}

void NanoAvbAemController::discover(Eui64 const& target)
{
    discovery_.discover(target);
}

auto NanoAvbAemController::find_entity(Eui64 const& id) const -> DiscoveredEntity const*
{
    return discovery_.find_entity(id);
}

auto NanoAvbAemController::mac_for_entity(Eui64 const& id) const -> std::optional<Eui48>
{
    return discovery_.mac_for_entity(id);
}

auto NanoAvbAemController::entity_count() const -> size_t
{
    return discovery_.entity_count();
}

// AEM Commands

auto NanoAvbAemController::send_aem_command(Eui64 target, uint16_t command_code, std::span<uint8_t const> payload) -> bool
{
    aem_ctx_.command_params = {
        .target_entity_id = target,
        .command_code = command_code,
        .command_data = payload,
    };
    aem_sm_.handle_event(aem_ctx_, AemControllerEvent::DoCommand, TimePoint::clock::now());
    return true;
}

auto NanoAvbAemController::acquire_entity(Eui64 target, bool persistent) -> bool
{
    std::array<uint8_t, 16> payload{};
    if (persistent) {
        payload[3] = 0x01;  // PERSISTENT flag (network byte order)
    }
    return send_aem_command(target, AEM_COMMAND_ACQUIRE_ENTITY, payload);
}

auto NanoAvbAemController::release_entity(Eui64 target) -> bool
{
    std::array<uint8_t, 16> payload{};
    payload[0] = 0x80;  // RELEASE flag (network byte order, bit 31)
    return send_aem_command(target, AEM_COMMAND_ACQUIRE_ENTITY, payload);
}

auto NanoAvbAemController::lock_entity(Eui64 target) -> bool
{
    std::array<uint8_t, 16> payload{};
    return send_aem_command(target, AEM_COMMAND_LOCK_ENTITY, payload);
}

auto NanoAvbAemController::unlock_entity(Eui64 target) -> bool
{
    std::array<uint8_t, 16> payload{};
    payload[3] = 0x01;  // UNLOCK flag
    return send_aem_command(target, AEM_COMMAND_LOCK_ENTITY, payload);
}

auto NanoAvbAemController::read_descriptor(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 8> payload{};
    // Configuration index (2 bytes) + reserved (2 bytes) + descriptor_type (2 bytes) + descriptor_index (2 bytes)
    payload[4] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[5] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[6] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[7] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_READ_DESCRIPTOR, payload);
}

auto NanoAvbAemController::register_unsolicited(Eui64 target) -> bool
{
    return send_aem_command(target, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
}

auto NanoAvbAemController::set_identify(Eui64 target, bool on) -> bool
{
    // Per IEEE 1722.1, an entity exposes "identify" as a CONTROL descriptor
    // with control_type = IDENTIFY_CONTROL_TYPE. The controller toggles the
    // entity's LED by writing 0xFF (on) or 0x00 (off) via SET_CONTROL.
    //
    // The ADPDU advertises which CONTROL descriptor index holds the identify
    // control via the `identify_control_index` field. The entity must be
    // discovered first; otherwise we have no way to know the control index.
    auto const* entity = find_entity(target);
    if (entity == nullptr) {
        return false;
    }
    // Only trust identify_control_index when the entity advertises it as valid
    // (IEEE 1722.1 6.2.1.10) — otherwise the field defaults to 0 and we would
    // blindly write CONTROL index 0, which may be some other control.
    if (!entity->adpdu.has_entity_capability(atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID)) {
        return false;
    }
    uint16_t const control_index = entity->adpdu.identify_control_index.get();
    std::array<uint8_t, 5> payload{};
    payload[0] = static_cast<uint8_t>((DESCRIPTOR_CONTROL >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(DESCRIPTOR_CONTROL & 0xFF);
    payload[2] = static_cast<uint8_t>((control_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(control_index & 0xFF);
    payload[4] = on ? 0xFF : 0x00;  // LINEAR_UINT8 value
    return send_aem_command(target, AEM_COMMAND_SET_CONTROL, payload);
}

auto NanoAvbAemController::get_stream_info(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 4> payload{};
    payload[0] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_GET_STREAM_INFO, payload);
}

auto NanoAvbAemController::get_avb_info(Eui64 target, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 4> payload{};
    payload[0] = static_cast<uint8_t>((DESCRIPTOR_AVB_INTERFACE >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(DESCRIPTOR_AVB_INTERFACE & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_GET_AVB_INFO, payload);
}

auto NanoAvbAemController::get_clock_source(Eui64 target, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 4> payload{};
    payload[0] = static_cast<uint8_t>((DESCRIPTOR_CLOCK_DOMAIN >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(DESCRIPTOR_CLOCK_DOMAIN & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_GET_CLOCK_SOURCE, payload);
}

auto NanoAvbAemController::get_counters(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 4> payload{};
    payload[0] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_GET_COUNTERS, payload);
}

auto NanoAvbAemController::set_stream_format(Eui64 target, uint16_t desc_type, uint16_t desc_index, uint64_t stream_format) -> bool
{
    std::array<uint8_t, 12> payload{};
    payload[0] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    for (int i = 0; i < 8; ++i) {
        payload[4 + i] = static_cast<uint8_t>((stream_format >> (56 - (i * 8))) & 0xFF);
    }
    return send_aem_command(target, AEM_COMMAND_SET_STREAM_FORMAT, payload);
}

auto NanoAvbAemController::start_streaming(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 4> payload{};
    payload[0] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_START_STREAMING, payload);
}

auto NanoAvbAemController::stop_streaming(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    std::array<uint8_t, 4> payload{};
    payload[0] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_STOP_STREAMING, payload);
}

auto NanoAvbAemController::set_clock_source(Eui64 target, uint16_t desc_index, uint16_t clock_source_index) -> bool
{
    std::array<uint8_t, 8> payload{};
    payload[0] = static_cast<uint8_t>((DESCRIPTOR_CLOCK_DOMAIN >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(DESCRIPTOR_CLOCK_DOMAIN & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    payload[4] = static_cast<uint8_t>((clock_source_index >> 8) & 0xFF);
    payload[5] = static_cast<uint8_t>(clock_source_index & 0xFF);
    return send_aem_command(target, AEM_COMMAND_SET_CLOCK_SOURCE, payload);
}

auto NanoAvbAemController::set_sampling_rate(Eui64 target, uint16_t desc_type, uint16_t desc_index, uint32_t sampling_rate) -> bool
{
    std::array<uint8_t, 8> payload{};
    payload[0] = static_cast<uint8_t>((desc_type >> 8) & 0xFF);
    payload[1] = static_cast<uint8_t>(desc_type & 0xFF);
    payload[2] = static_cast<uint8_t>((desc_index >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(desc_index & 0xFF);
    payload[4] = static_cast<uint8_t>((sampling_rate >> 24) & 0xFF);
    payload[5] = static_cast<uint8_t>((sampling_rate >> 16) & 0xFF);
    payload[6] = static_cast<uint8_t>((sampling_rate >> 8) & 0xFF);
    payload[7] = static_cast<uint8_t>(sampling_rate & 0xFF);
    return send_aem_command(target, AEM_COMMAND_SET_SAMPLING_RATE, payload);
}

// ACMP Commands

auto NanoAvbAemController::connect_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool
{
    return acmp_controller_.connect(talker, talker_uid, listener, listener_uid);
}

auto NanoAvbAemController::disconnect_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool
{
    return acmp_controller_.disconnect(talker, talker_uid, listener, listener_uid);
}

auto NanoAvbAemController::connect_tx_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool
{
    return acmp_controller_.connect_tx(talker, talker_uid, listener, listener_uid);
}

auto NanoAvbAemController::disconnect_tx_stream(Eui64 talker, uint16_t talker_uid, Eui64 listener, uint16_t listener_uid) -> bool
{
    return acmp_controller_.disconnect_tx(talker, talker_uid, listener, listener_uid);
}

auto NanoAvbAemController::get_rx_state(Eui64 listener, uint16_t listener_uid) -> bool
{
    return acmp_controller_.get_rx_state(listener, listener_uid);
}

auto NanoAvbAemController::get_tx_state(Eui64 talker, uint16_t talker_uid) -> bool
{
    return acmp_controller_.get_tx_state(talker, talker_uid);
}

// Packet Dispatch

void NanoAvbAemController::receive_adp(AdpDu const& adp, Eui48 const& src_mac, int64_t now_ns)
{
    discovery_.receive_adpdu(adp, src_mac, ns_to_timepoint(now_ns));
}

void NanoAvbAemController::receive_acmp(AcmpCommandResponse const& resp, int64_t now_ns)
{
    acmp_controller_.receive_response(resp, ns_to_timepoint(now_ns));
}

void NanoAvbAemController::receive_aecp(std::span<uint8_t const> payload, int64_t now_ns)
{
    if (payload.size() < AemDu::LENGTH) {
        return;
    }

    AemDu header{};
    span_load(header, payload);

    // Only process responses targeted at us
    if (!header.is_response() || header.controller_entity_id != controller_entity_id_) {
        return;
    }

    auto idx = aem_ctx_.find_inflight(header.sequence_id.get());
    if (idx >= AemControllerContext::MAX_INFLIGHT) {
        return;
    }

    aem_ctx_.rcvd_header = header;
    aem_ctx_.rcvd_response_data = payload.subspan(AemDu::LENGTH);
    aem_ctx_.current_inflight_index = idx;
    aem_sm_.handle_event(aem_ctx_, AemControllerEvent::RcvdResponse, ns_to_timepoint(now_ns));
}

void NanoAvbAemController::tick(int64_t now_ns)
{
    auto const now = ns_to_timepoint(now_ns);
    discovery_.tick(now);
    acmp_controller_.tick(now);

    // Check AEM timeouts
    auto idx = aem_ctx_.find_timed_out(now);
    while (idx < AemControllerContext::MAX_INFLIGHT) {
        aem_ctx_.current_inflight_index = idx;
        aem_sm_.handle_event(aem_ctx_, AemControllerEvent::Timeout, now);
        idx = aem_ctx_.find_timed_out(now);
    }
}

}  // namespace statusbar::nanoavb
