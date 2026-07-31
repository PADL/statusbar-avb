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

namespace {

/// Serialize a fixed AEM payload wire struct into a byte array for
/// send_aem_command (which copies the span synchronously).
template <typename P>
[[nodiscard]] auto pack(P const& payload) -> std::array<uint8_t, sizeof(P)>
{
    std::array<uint8_t, sizeof(P)> buf{};
    span_store(buf, payload);
    return buf;
}

}  // namespace

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

auto NanoAvbAemController::send_aem_command(
    Eui64 target, uint16_t command_code, std::span<uint8_t const> payload, atdecc::AemCommandCompletion completion) -> bool
{
    aem_ctx_.command_params = {
        .target_entity_id = target,
        .command_code = command_code,
        .command_data = payload,
        .completion = std::move(completion),
    };
    aem_sm_.handle_event(aem_ctx_, AemControllerEvent::DoCommand, TimePoint::clock::now());
    return true;
}

auto NanoAvbAemController::acquire_entity(Eui64 target, bool persistent) -> bool
{
    aem::AemAcquireEntityPayload payload{};
    payload.set_persistent(persistent);
    return send_aem_command(target, AEM_COMMAND_ACQUIRE_ENTITY, pack(payload));
}

auto NanoAvbAemController::release_entity(Eui64 target) -> bool
{
    aem::AemAcquireEntityPayload payload{};
    payload.set_release(true);
    return send_aem_command(target, AEM_COMMAND_ACQUIRE_ENTITY, pack(payload));
}

auto NanoAvbAemController::lock_entity(Eui64 target) -> bool
{
    return send_aem_command(target, AEM_COMMAND_LOCK_ENTITY, pack(aem::AemLockEntityPayload{}));
}

auto NanoAvbAemController::unlock_entity(Eui64 target) -> bool
{
    aem::AemLockEntityPayload payload{};
    payload.set_unlock(true);
    return send_aem_command(target, AEM_COMMAND_LOCK_ENTITY, pack(payload));
}

auto NanoAvbAemController::read_descriptor(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    aem::AemReadDescriptorCommandPayload const payload{
        .configuration_index = 0, .reserved = 0, .descriptor_type = desc_type, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_READ_DESCRIPTOR, pack(payload));
}

auto NanoAvbAemController::register_unsolicited(Eui64 target) -> bool
{
    return send_aem_command(target, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
}

auto NanoAvbAemController::set_identify(Eui64 target, bool on, atdecc::AemCommandCompletion completion) -> bool
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
    std::array<uint8_t, aem::AemControlPayloadHeader::LENGTH + 1> payload{};
    span_store(payload, aem::AemControlPayloadHeader{.descriptor_type = DESCRIPTOR_CONTROL, .descriptor_index = control_index});
    payload[aem::AemControlPayloadHeader::LENGTH] = on ? 0xFF : 0x00;  // LINEAR_UINT8 value
    return send_aem_command(target, AEM_COMMAND_SET_CONTROL, payload, std::move(completion));
}

auto NanoAvbAemController::get_stream_info(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    aem::AemGetStreamInfoCommandPayload const payload{.descriptor_type = desc_type, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_GET_STREAM_INFO, pack(payload));
}

auto NanoAvbAemController::get_avb_info(Eui64 target, uint16_t desc_index) -> bool
{
    aem::AemGetAvbInfoCommandPayload const payload{.descriptor_type = DESCRIPTOR_AVB_INTERFACE, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_GET_AVB_INFO, pack(payload));
}

auto NanoAvbAemController::get_clock_source(Eui64 target, uint16_t desc_index, atdecc::AemCommandCompletion completion) -> bool
{
    aem::AemGetClockSourceCommandPayload const payload{.descriptor_type = DESCRIPTOR_CLOCK_DOMAIN, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_GET_CLOCK_SOURCE, pack(payload), std::move(completion));
}

auto NanoAvbAemController::set_signal_selector(
    Eui64 target,
    uint16_t desc_index,
    uint16_t signal_type,
    uint16_t signal_index,
    uint16_t signal_output,
    atdecc::AemCommandCompletion completion) -> bool
{
    aem::AemSignalSelectorPayload const payload{
        .descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR,
        .descriptor_index = desc_index,
        .source = {.signal_type = signal_type, .signal_index = signal_index, .signal_output = signal_output},
        .reserved = 0};
    return send_aem_command(target, AEM_COMMAND_SET_SIGNAL_SELECTOR, pack(payload), std::move(completion));
}

auto NanoAvbAemController::get_signal_selector(Eui64 target, uint16_t desc_index, atdecc::AemCommandCompletion completion) -> bool
{
    aem::AemGetSignalSelectorCommandPayload const payload{
        .descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_GET_SIGNAL_SELECTOR, pack(payload), std::move(completion));
}

auto NanoAvbAemController::get_counters(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    aem::AemGetCountersCommandPayload const payload{.descriptor_type = desc_type, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_GET_COUNTERS, pack(payload));
}

auto NanoAvbAemController::set_stream_format(Eui64 target, uint16_t desc_type, uint16_t desc_index, uint64_t stream_format) -> bool
{
    aem::AemStreamFormatPayload const header{.descriptor_type = desc_type, .descriptor_index = desc_index};
    auto payload = pack(header);
    span_store(make_span(payload, {.start = offsetof(aem::AemStreamFormatPayload, stream_format)}), ieee::octlet_t{stream_format});
    return send_aem_command(target, AEM_COMMAND_SET_STREAM_FORMAT, payload);
}

auto NanoAvbAemController::start_streaming(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    aem::AemStreamingPayload const payload{.descriptor_type = desc_type, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_START_STREAMING, pack(payload));
}

auto NanoAvbAemController::stop_streaming(Eui64 target, uint16_t desc_type, uint16_t desc_index) -> bool
{
    aem::AemStreamingPayload const payload{.descriptor_type = desc_type, .descriptor_index = desc_index};
    return send_aem_command(target, AEM_COMMAND_STOP_STREAMING, pack(payload));
}

auto NanoAvbAemController::set_clock_source(
    Eui64 target, uint16_t desc_index, uint16_t clock_source_index, atdecc::AemCommandCompletion completion) -> bool
{
    aem::AemClockSourcePayload const payload{
        .descriptor_type = DESCRIPTOR_CLOCK_DOMAIN,
        .descriptor_index = desc_index,
        .clock_source_index = clock_source_index,
        .reserved = 0};
    return send_aem_command(target, AEM_COMMAND_SET_CLOCK_SOURCE, pack(payload), std::move(completion));
}

auto NanoAvbAemController::set_sampling_rate(Eui64 target, uint16_t desc_type, uint16_t desc_index, uint32_t sampling_rate) -> bool
{
    aem::AemSamplingRatePayload const payload{
        .descriptor_type = desc_type, .descriptor_index = desc_index, .sampling_rate = sampling_rate};
    return send_aem_command(target, AEM_COMMAND_SET_SAMPLING_RATE, pack(payload));
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
