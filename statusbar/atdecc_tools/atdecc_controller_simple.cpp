// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ControllerSimple implementation

#include "statusbar/atdecc_tools/atdecc_controller_simple.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace statusbar::atdecc_tools {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::ieee;
using namespace statusbar::net;

// Defensive cap on how many descriptors of one type a (possibly hostile or corrupt)
// entity's CONFIGURATION can make us enumerate. A wire count near 65535 across ~108
// descriptor types would otherwise drive millions of set inserts + queue pushes
// (hundreds of MB) -- a DoS from a single discovered entity. Real AVDECC entities
// have at most tens of any descriptor type, so this is far above anything genuine.
constexpr uint16_t MAX_DESCRIPTORS_PER_TYPE = 512;

ControllerSimple::ControllerSimple(RawnetContext context, Eui64 controller_id)
    : context_{std::move(context)}
    , controller_{controller_id}
{
    wire_controller();
    controller_.start();
    controller_.discover_all();
}

auto ControllerSimple::fd() const noexcept -> int
{
    return context_.fd();
}

void ControllerSimple::on_ready(int64_t now_ns)
{
    Eui48 src_mac{};
    Eui48 dest_mac{};
    while (true) {
        auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
        if (!result || *result <= 0) {
            break;
        }
        auto const len = static_cast<size_t>(*result);
        if (len == 0) {
            break;
        }
        dispatch_frame(now_ns, src_mac, {payload_buf_.data(), len});
    }
}

void ControllerSimple::tick(int64_t now_ns)
{
    controller_.tick(now_ns);
    // Drain one pending READ_DESCRIPTOR per tick (unified queue for ENTITY,
    // CONFIGURATION, STREAM_INPUT/OUTPUT, STRINGS, and any other descriptor
    // type — populated by per-descriptor reference following).
    send_next_descriptor_read(now_ns);
    // Drain one pending GET_RX_STATE query per tick to avoid flooding
    if (!rx_state_query_queue_.empty()) {
        auto const& [listener_id, uid] = rx_state_query_queue_.back();
        controller_.get_rx_state(listener_id, uid);
        rx_state_query_queue_.pop_back();
    }
    // Send one pending GET_STREAM_FORMAT query per tick
    send_next_stream_format_query(now_ns);
}

auto ControllerSimple::finished() const noexcept -> bool
{
    return false;
}

auto ControllerSimple::controller() -> nanoavb::NanoAvbAemController&
{
    return controller_;
}

auto ControllerSimple::get_display_entities() -> std::vector<EntityDisplayInfo>
{
    std::vector<EntityDisplayInfo> result;
    result.reserve(known_entity_ids_.size());
    for (auto const& id : known_entity_ids_) {
        auto const* entity = controller_.find_entity(id);
        if (entity == nullptr) {
            continue;
        }
        auto const& adp = entity->adpdu;
        EntityDisplayInfo info{};
        info.entity_id = id;

        auto it = entity_names_.find(id);
        if (it != entity_names_.end()) {
            info.name = it->second;
        } else {
            info.name = std::string{ieee::to_string(id).view()};
        }

        info.has_talker = adp.has_talker_capability(talker_capabilities::IMPLEMENTED);
        info.has_listener = adp.has_listener_capability(listener_capabilities::IMPLEMENTED);
        // Prefer the actual counts from the entity's CONFIGURATION descriptor
        // (descriptor_counts table). Fall back to the ADPDU's upper bounds
        // until that has been read.
        if (auto cit = descriptor_counts_.find(id); cit != descriptor_counts_.end()) {
            info.talker_stream_sources = cit->second.stream_outputs;
            info.listener_stream_sinks = cit->second.stream_inputs;
        } else {
            info.talker_stream_sources = adp.talker_stream_sources.get();
            info.listener_stream_sinks = adp.listener_stream_sinks.get();
        }

        // Populate per-stream format strings from the cache
        auto t_it = talker_formats_.find(id);
        if (t_it != talker_formats_.end()) {
            info.talker_stream_formats = t_it->second;
        }
        auto l_it = listener_formats_.find(id);
        if (l_it != listener_formats_.end()) {
            info.listener_stream_formats = l_it->second;
        }
        auto tn_it = talker_stream_names_.find(id);
        if (tn_it != talker_stream_names_.end()) {
            info.talker_stream_names = tn_it->second;
        }
        auto ln_it = listener_stream_names_.find(id);
        if (ln_it != listener_stream_names_.end()) {
            info.listener_stream_names = ln_it->second;
        }

        result.push_back(std::move(info));
    }
    return result;
}

void ControllerSimple::emit_status(std::string_view msg)
{
    pending_events_.emplace_back(StatusChangedEvent{std::string{msg}});
}

auto ControllerSimple::drain_events() -> std::vector<ControllerEvent>
{
    std::vector<ControllerEvent> out;
    out.swap(pending_events_);
    return out;
}

void ControllerSimple::fetch_entity_descriptors(Eui64 entity_id, int64_t /*now_ns*/)
{
    auto const* entity = controller_.find_entity(entity_id);
    uint16_t n_talkers = 0;
    uint16_t n_listeners = 0;
    if (entity != nullptr) {
        n_talkers = entity->adpdu.talker_stream_sources.get();
        n_listeners = entity->adpdu.listener_stream_sinks.get();
    }

    // Build the expected (type, index) list. ENTITY + CONFIGURATION +
    // AVB_INTERFACE + CLOCK_SOURCE + CLOCK_DOMAIN + per-stream descriptors.
    std::vector<std::pair<uint16_t, uint16_t>> expected_descs;
    expected_descs.reserve(5 + n_talkers + n_listeners);
    expected_descs.emplace_back(DESCRIPTOR_ENTITY, 0);
    expected_descs.emplace_back(DESCRIPTOR_CONFIGURATION, 0);
    expected_descs.emplace_back(DESCRIPTOR_AVB_INTERFACE, 0);
    expected_descs.emplace_back(DESCRIPTOR_CLOCK_SOURCE, 0);
    expected_descs.emplace_back(DESCRIPTOR_CLOCK_DOMAIN, 0);
    for (uint16_t i = 0; i < n_talkers; ++i) {
        expected_descs.emplace_back(DESCRIPTOR_STREAM_OUTPUT, i);
    }
    for (uint16_t i = 0; i < n_listeners; ++i) {
        expected_descs.emplace_back(DESCRIPTOR_STREAM_INPUT, i);
    }

    EntityDetailBuilder builder{};
    builder.entity_id = entity_id;
    builder.expected = expected_descs.size();

    // Seed from cache for descriptors the background discovery has already
    // fetched; enqueue the rest through the unified queue, which respects
    // the AEM inflight cap and retries. Sending them all directly here used
    // to overflow the 8-slot inflight tracker and silently drop responses.
    auto cache_it = descriptor_data_cache_.find(entity_id);
    for (auto const& [type, index] : expected_descs) {
        bool fed_from_cache = false;
        if (cache_it != descriptor_data_cache_.end()) {
            auto data_it = cache_it->second.find({type, index});
            if (data_it != cache_it->second.end()) {
                builder.receive(type, index, data_it->second.success, make_const_span(data_it->second.data));
                fed_from_cache = true;
            }
        }
        if (!fed_from_cache) {
            enqueue_descriptor_read(entity_id, type, index);
        }
    }

    bool const already_complete = builder.is_complete();
    detail_builders_[entity_id] = std::move(builder);

    if (already_complete) {
        auto it = detail_builders_.find(entity_id);
        pending_events_.emplace_back(EntityDetailReadyEvent{it->second.build()});
        detail_builders_.erase(it);
    }
}

auto ControllerSimple::cached_descriptors(ieee::Eui64 const& id) const -> std::vector<RawDescriptor>
{
    std::vector<RawDescriptor> out;
    auto const it = descriptor_data_cache_.find(id);
    if (it == descriptor_data_cache_.end()) {
        return out;
    }
    for (auto const& [key, cached] : it->second) {
        if (!cached.success) {
            continue;
        }
        out.push_back({.type = key.first, .index = key.second, .data = cached.data});
    }
    return out;
}

void ControllerSimple::dispatch(ControllerAction const& action, int64_t now_ns)
{
    switch (action.kind) {
        case ControllerActionKind::DiscoverAll:
            controller_.discover_all();
            queue_rx_state_for_all();
            break;
        case ControllerActionKind::ConnectStream:
            controller_.connect_stream(
                action.request.talker_entity_id,
                action.request.talker_unique_id,
                action.request.listener_entity_id,
                action.request.listener_unique_id);
            break;
        case ControllerActionKind::DisconnectStream:
            controller_.disconnect_stream(
                action.request.talker_entity_id,
                action.request.talker_unique_id,
                action.request.listener_entity_id,
                action.request.listener_unique_id);
            break;
        case ControllerActionKind::ReadEntityDescriptors:
            fetch_entity_descriptors(action.request.talker_entity_id, now_ns);
            break;
        case ControllerActionKind::IdentifyEntity: {
            // Toggle the per-entity identify state.
            auto const target = action.request.talker_entity_id;
            bool const new_on = !identify_state_[target];
            if (controller_.set_identify(target, new_on)) {
                identify_state_[target] = new_on;
                emit_status(new_on ? "Identify on" : "Identify off");
            } else {
                emit_status("Identify failed: entity not found or queue full");
            }
            break;
        }
        case ControllerActionKind::StartStreaming:
            if (!controller_.start_streaming(
                    action.request.talker_entity_id, action.request.desc_type, action.request.desc_index)) {
                emit_status("Start streaming failed: entity not found or queue full");
            }
            break;
        case ControllerActionKind::StopStreaming:
            if (!controller_.stop_streaming(action.request.talker_entity_id, action.request.desc_type, action.request.desc_index)) {
                emit_status("Stop streaming failed: entity not found or queue full");
            }
            break;
        case ControllerActionKind::SetStreamFormat:
            if (!controller_.set_stream_format(
                    action.request.talker_entity_id,
                    action.request.desc_type,
                    action.request.desc_index,
                    action.request.stream_format)) {
                emit_status("Set stream format failed: entity not found or queue full");
            }
            break;
        case ControllerActionKind::SetClockSource:
            // desc_index = CLOCK_DOMAIN index; clock_source_index = which source.
            if (!controller_.set_clock_source(
                    action.request.talker_entity_id, action.request.desc_index, action.request.clock_source_index)) {
                emit_status("Set clock source failed: entity not found or queue full");
            }
            break;
        case ControllerActionKind::GetClockSource:
            if (!controller_.get_clock_source(action.request.talker_entity_id, action.request.desc_index)) {
                emit_status("Get clock source failed: entity not found or queue full");
            }
            break;
        case ControllerActionKind::ConnectTxStream:
            controller_.connect_tx_stream(
                action.request.talker_entity_id,
                action.request.talker_unique_id,
                action.request.listener_entity_id,
                action.request.listener_unique_id);
            break;
        case ControllerActionKind::DisconnectTxStream:
            controller_.disconnect_tx_stream(
                action.request.talker_entity_id,
                action.request.talker_unique_id,
                action.request.listener_entity_id,
                action.request.listener_unique_id);
            break;
        case ControllerActionKind::GetCounters:
            if (!controller_.get_counters(action.request.talker_entity_id, action.request.desc_type, action.request.desc_index)) {
                emit_status("Get counters failed: entity not found or queue full");
            }
            break;
    }
}

void ControllerSimple::EntityDetailBuilder::receive(
    uint16_t desc_type, uint16_t desc_index, bool success, std::span<uint8_t const> data)
{
    // Deduplicate: only count each (type, index) pair once. This protects
    // against responses arriving from both the detail-fetch path and the
    // background stream-query path at the same time.
    auto const key = std::make_pair(desc_type, desc_index);
    if (seen.contains(key)) {
        return;
    }
    seen.insert(key);
    if (!success) {
        return;  // counted (to unblock is_complete) but nothing to store
    }
    switch (desc_type) {
        case DESCRIPTOR_ENTITY:
            entity_desc_data.assign(data.begin(), data.end());
            break;
        case DESCRIPTOR_AVB_INTERFACE:
            avb_iface_data.assign(data.begin(), data.end());
            break;
        case DESCRIPTOR_CLOCK_DOMAIN:
            clock_domain_data.assign(data.begin(), data.end());
            break;
        case DESCRIPTOR_CLOCK_SOURCE:
            clock_source_descs.emplace_back(desc_index, std::vector<uint8_t>(data.begin(), data.end()));
            break;
        case DESCRIPTOR_STREAM_OUTPUT:
            stream_output_descs.emplace_back(desc_index, std::vector<uint8_t>(data.begin(), data.end()));
            break;
        case DESCRIPTOR_STREAM_INPUT:
            stream_input_descs.emplace_back(desc_index, std::vector<uint8_t>(data.begin(), data.end()));
            break;
        default:
            break;
    }
}

auto ControllerSimple::EntityDetailBuilder::build() const -> EntityDetail
{
    EntityDetail detail{};
    detail.entity_id = entity_id;

    // Entity section
    detail.lines.push_back({.text = "Entity", .bold = true});
    if (entity_desc_data.size() >= DescriptorEntity::LENGTH) {
        DescriptorEntity desc{};
        span_load(desc, make_const_span(entity_desc_data));
        detail.name = std::string{desc.entity_name.as_string_view()};
        detail.lines.push_back({.text = std::format("  Name: {}", desc.entity_name.as_string_view()), .bold = false});
        detail.lines.push_back({.text = std::format("  Entity ID: {}", ieee::to_string(desc.entity_id).view()), .bold = false});
        detail.lines.push_back(
            {.text = std::format("  Model ID: {}", ieee::to_string(desc.entity_model_id).view()), .bold = false});
        detail.lines.push_back({.text = std::format("  Firmware: {}", desc.firmware_version.as_string_view()), .bold = false});
        detail.lines.push_back({.text = std::format("  Serial: {}", desc.serial_number.as_string_view()), .bold = false});
        detail.lines.push_back(
            {.text = std::format("  Streams: {} out, {} in", desc.talker_stream_sources.get(), desc.listener_stream_sinks.get()),
             .bold = false});
    } else {
        detail.name = std::string{ieee::to_string(entity_id).view()};
        detail.lines.push_back({.text = std::format("  Entity ID: {}", ieee::to_string(entity_id).view()), .bold = false});
        detail.lines.push_back({.text = "  (descriptor not received)", .bold = false});
    }

    // AVB Interface section
    if (avb_iface_data.size() >= DescriptorAvbInterface::MINIMUM_LENGTH) {
        // span_load_padded is safe for both 2013 (98 byte) and 2016+
        // (102 byte) payloads; plain span_load would memcpy sizeof(T)
        // and read past a shorter wire payload.
        DescriptorAvbInterface avb{};
        span_load_padded(avb, make_const_span(avb_iface_data));
        detail.lines.push_back({.text = "AVB Interface", .bold = true});
        detail.lines.push_back({.text = std::format("  Name: {}", avb.object_name.as_string_view()), .bold = false});
        detail.lines.push_back({.text = std::format("  MAC: {}", ieee::to_string(avb.mac_address).view()), .bold = false});
        detail.lines.push_back(
            {.text = std::format("  Clock Identity: {}", ieee::to_string(avb.clock_identity).view()), .bold = false});
        detail.lines.push_back({.text = std::format("  gPTP Domain: {}", static_cast<int>(avb.domain_number)), .bold = false});
    }

    // Clock section
    if (!clock_domain_data.empty() || !clock_source_descs.empty()) {
        detail.lines.push_back({.text = "Clock", .bold = true});
    }
    if (clock_domain_data.size() >= DescriptorClockDomain::LENGTH) {
        // DescriptorClockDomain now has an inline clock_sources trailer,
        // so sizeof() is much larger than the on-wire payload. Use
        // span_load_padded to safely ingest the fixed header plus any
        // populated trailer entries without reading past the wire bytes.
        DescriptorClockDomain cd{};
        span_load_padded(cd, make_const_span(clock_domain_data));
        detail.lines.push_back({.text = std::format("  Domain: {}", cd.object_name.as_string_view()), .bold = false});
        detail.lines.push_back({.text = std::format("  Active Source Index: {}", cd.clock_source_index.get()), .bold = false});
    }
    auto sorted_cs = clock_source_descs;
    std::sort(sorted_cs.begin(), sorted_cs.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto const& [idx, cs_data] : sorted_cs) {
        if (cs_data.size() >= DescriptorClockSource::LENGTH) {
            DescriptorClockSource cs{};
            span_load(cs, make_const_span(cs_data));
            auto const cs_type = cs.clock_source_type.get();
            char const* type_name = (cs_type == 0) ? "Internal"
                : (cs_type == 1)                   ? "External"
                : (cs_type == 2)                   ? "Input Stream"
                                                   : "Unknown";
            detail.lines.push_back(
                {.text = std::format("  Source {}: {} ({})", idx, cs.object_name.as_string_view(), type_name), .bold = false});
        }
    }

    // Stream sections
    auto add_streams = [&](char const* title, auto const& streams) {
        if (streams.empty()) {
            return;
        }
        detail.lines.push_back({.text = title, .bold = true});
        auto sorted = streams;
        std::sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [idx, s_data] : sorted) {
            if (s_data.size() >= DescriptorStream::MINIMUM_LENGTH) {
                DescriptorStream sd{};
                // 2013-format frames are 132 bytes (vs. 138 for 2021), so
                // zero-pad the trailing redundant_offset / number_of_redundant_streams /
                // timing fields when loading.
                span_load_padded(sd, make_const_span(s_data));
                auto const name = sd.object_name.as_string_view();
                auto const fmt_raw = sd.current_format.get();
                auto const fmt_bytes = make_const_span(sd.current_format);
                std::string fmt_str = std::format("0x{:016x}", fmt_raw);
                // Simple format decode
                if (fmt_bytes[0] == 0x00) {
                    // AM824
                    auto const channels = fmt_bytes[3];
                    fmt_str = std::format("AM824 {}ch", channels);
                } else if (fmt_bytes[0] == 0x02) {
                    // AAF
                    auto const channels = (static_cast<uint16_t>(fmt_bytes[1] & 0x03) << 8) | fmt_bytes[2];
                    auto const depth = fmt_bytes[3];
                    fmt_str = std::format("AAF {}ch {}-bit", channels, depth);
                }
                if (name.empty()) {
                    detail.lines.push_back({.text = std::format("  [{}] {}", idx, fmt_str), .bold = false});
                } else {
                    detail.lines.push_back({.text = std::format("  [{}] {} — {}", idx, name, fmt_str), .bold = false});
                }
            }
        }
    };
    add_streams("Stream Outputs", stream_output_descs);
    add_streams("Stream Inputs", stream_input_descs);

    return detail;
}

void ControllerSimple::queue_rx_state_for_all()
{
    for (auto const& id : known_entity_ids_) {
        auto const* entity = controller_.find_entity(id);
        if (entity != nullptr) {
            queue_rx_state_for_entity(entity->adpdu);
        }
    }
}

//
// Private implementation
//

void ControllerSimple::wire_controller()
{
    controller_.set_callbacks({
        .send_atdecc_multicast = [this](std::span<uint8_t const> pkt) -> bool {
            return context_.send(&ATDECC_MULTICAST_MAC, pkt).has_value();
        },
        .send_atdecc_unicast = [this](Eui48 const& dst, std::span<uint8_t const> pkt) -> bool {
            return context_.send(&dst, pkt).has_value();
        },
        .on_entity_available =
            [this](DiscoveredEntity const& e) {
                auto const& id = e.adpdu.entity_id;
                if (std::find(known_entity_ids_.begin(), known_entity_ids_.end(), id) == known_entity_ids_.end()) {
                    known_entity_ids_.push_back(id);
                }
                last_available_index_[id] = e.adpdu.available_index.get();
                // Route the initial ENTITY descriptor read through the
                // unified queue. This is the seed of the descriptor crawl.
                enqueue_descriptor_read(id, DESCRIPTOR_ENTITY, 0);
                queue_rx_state_for_entity(e.adpdu);
            },
        .on_entity_updated =
            [this](DiscoveredEntity const& e) {
                auto const& id = e.adpdu.entity_id;
                auto const new_index = e.adpdu.available_index.get();

                // Reboot detection: available_index strictly decreased.
                // The entity restarted its sequence (typically resetting to 0).
                // Forget cached metadata and re-read from scratch.
                auto const cached_it = last_available_index_.find(id);
                bool const rebooted = (cached_it != last_available_index_.end()) && (new_index < cached_it->second);
                last_available_index_[id] = new_index;

                if (rebooted) {
                    auto const name_it = entity_names_.find(id);
                    std::string const name =
                        (name_it != entity_names_.end()) ? name_it->second : std::string{ieee::to_string(id).view()};
                    forget_entity_metadata(id);
                    enqueue_descriptor_read(id, DESCRIPTOR_ENTITY, 0);
                    queue_rx_state_for_entity(e.adpdu);
                    emit_status(std::format("{} rebooted, re-reading descriptors", name));
                    return;
                }

                // Normal heartbeat: skip re-read if we already have the entity's
                // name (and therefore already queued and processed its stream
                // metadata).
                if (entity_names_.contains(id)) {
                    return;
                }
                enqueue_descriptor_read(id, DESCRIPTOR_ENTITY, 0);
            },
        .on_entity_departing =
            [this](Eui64 id) {
                known_entity_ids_.erase(
                    std::remove(known_entity_ids_.begin(), known_entity_ids_.end(), id), known_entity_ids_.end());
                last_available_index_.erase(id);
                forget_entity_metadata(id);
            },
        .on_aem_response =
            [this](
                Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data) {
                handle_aem_response(target, cmd, status, sent_payload, data);
            },
        .on_aem_timeout = [](Eui64, uint16_t) {},
        .on_acmp_response =
            [this](AcmpCommandResponse const& resp) {
                auto const mt = resp.message_type();
                auto const st = resp.status();
                auto name_or_id = [this](Eui64 const& id) -> std::string {
                    auto it = entity_names_.find(id);
                    if (it != entity_names_.end() && !it->second.empty()) {
                        return it->second;
                    }
                    return std::string{ieee::to_string(id).view()};
                };
                if (st == ACMP_STATUS_SUCCESS) {
                    if (mt == ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE) {
                        emit_status(std::format(
                            "Connected: {}:{} -> {}:{}",
                            name_or_id(resp.talker_entity_id),
                            resp.talker_unique_id.get(),
                            name_or_id(resp.listener_entity_id),
                            resp.listener_unique_id.get()));
                    } else if (mt == ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE) {
                        emit_status(std::format(
                            "Disconnected: {}:{} -> {}:{}",
                            name_or_id(resp.talker_entity_id),
                            resp.talker_unique_id.get(),
                            name_or_id(resp.listener_entity_id),
                            resp.listener_unique_id.get()));
                    }
                } else {
                    emit_status(std::format("ACMP {}: {}", acmp_message_type_name(mt), acmp_status_name(st)));
                }
            },
        .on_acmp_timeout = [this](AcmpCommandResponse const&) { emit_status("ACMP timeout"); },
    });
}

void ControllerSimple::forget_entity_metadata(Eui64 const& id)
{
    entity_names_.erase(id);
    talker_formats_.erase(id);
    listener_formats_.erase(id);
    talker_stream_names_.erase(id);
    listener_stream_names_.erase(id);
    std::erase_if(strings_waiters_, [&](auto const& kv) { return kv.first.first == id; });
    std::erase_if(strings_cache_, [&](auto const& kv) { return kv.first.first == id; });
    std::erase_if(stream_format_queries_, [&](auto const& q) { return q.target == id; });
    std::erase_if(descriptor_read_queue_, [&](auto const& q) { return q.target == id; });
    requested_descriptors_.erase(id);
    detail_builders_.erase(id);
    identify_state_.erase(id);
    descriptor_counts_.erase(id);
    descriptor_data_cache_.erase(id);
}

void ControllerSimple::query_rx_state(Eui64 listener_id, uint16_t unique_id, int64_t now_ns)
{
    controller_.tick(now_ns);
    controller_.get_rx_state(listener_id, unique_id);
}

void ControllerSimple::query_tx_state(Eui64 talker_id, uint16_t unique_id, int64_t now_ns)
{
    controller_.tick(now_ns);
    controller_.get_tx_state(talker_id, unique_id);
}

void ControllerSimple::queue_rx_state_for_entity(AdpDu const& adp)
{
    // Auto-probing is opt-in (set_auto_probe_rx_state). When off, discovery and
    // DiscoverAll never fan GET_RX_STATE at an entity's sinks -- so a one-shot
    // connect/list can't have its ACMP in-flight window starved by an
    // unresponsive multi-sink entity. The explicit get-rx-state command uses
    // query_rx_state() and is unaffected.
    if (!auto_probe_rx_state_) {
        return;
    }
    auto const listener_sinks = adp.listener_stream_sinks.get();
    auto const has_listener = (adp.listener_capabilities.get() & listener_capabilities::IMPLEMENTED) != 0;
    if (!has_listener || listener_sinks == 0) {
        return;
    }
    for (uint16_t i = 0; i < listener_sinks; ++i) {
        rx_state_query_queue_.push_back({adp.entity_id, i});
    }
}

auto ControllerSimple::make_active_connection(AcmpDu const& acmp) -> ActiveConnection
{
    auto name_or_empty = [this](Eui64 const& id) -> std::string {
        auto it = entity_names_.find(id);
        return (it != entity_names_.end()) ? it->second : std::string{};
    };
    ActiveConnection conn{};
    conn.talker_entity_id = acmp.talker_entity_id;
    conn.talker_unique_id = acmp.talker_unique_id.get();
    conn.listener_entity_id = acmp.listener_entity_id;
    conn.listener_unique_id = acmp.listener_unique_id.get();
    conn.talker_name = name_or_empty(acmp.talker_entity_id);
    conn.listener_name = name_or_empty(acmp.listener_entity_id);
    return conn;
}

void ControllerSimple::dispatch_frame(int64_t now_ns, Eui48 const& src_mac, std::span<uint8_t const> payload)
{
    if (payload.empty()) {
        return;
    }
    switch (payload[0]) {
        case avtp::AvtpSubtype::adp:
            dispatch_adp(now_ns, src_mac, payload);
            break;
        case avtp::AvtpSubtype::aecp:
            controller_.receive_aecp(payload, now_ns);
            break;
        case avtp::AvtpSubtype::acmp:
            dispatch_acmp(payload, now_ns);
            break;
        default:
            break;
    }
}

void ControllerSimple::dispatch_adp(int64_t now_ns, Eui48 const& src_mac, std::span<uint8_t const> payload)
{
    if (payload.size() < AdpDu::LENGTH) {
        return;
    }
    AdpDu adp{};
    span_load(adp, payload);
    controller_.receive_adp(adp, src_mac, now_ns);
}

void ControllerSimple::dispatch_acmp(std::span<uint8_t const> payload, int64_t now_ns)
{
    if (payload.size() < AcmpDu::LENGTH) {
        return;
    }
    AcmpDu acmp{};
    span_load(acmp, payload);
    if (std::getenv("ACMP_TRACE") != nullptr) {
        std::print(
            stderr,
            "[acmp-trace] mt={} status={} L={}:{} T={}:{} len={}\n",
            acmp.message_type(),
            acmp.status(),
            ieee::to_string(acmp.listener_entity_id).view(),
            acmp.listener_unique_id.get(),
            ieee::to_string(acmp.talker_entity_id).view(),
            acmp.talker_unique_id.get(),
            payload.size());
    }
    // Raw trace: surface EVERY ACMP PDU (commands included) so a diagnostic
    // caller can reconstruct a handshake leg by leg -- in particular the
    // listener's relayed CONNECT_TX_COMMAND, which is a command and would
    // otherwise be dropped by the is_response() gate below.
    if (acmp_trace_) {
        pending_events_.emplace_back(AcmpTraceEvent{
            .message_type = acmp.message_type(),
            .status = acmp.status(),
            .talker_entity_id = acmp.talker_entity_id,
            .talker_unique_id = acmp.talker_unique_id.get(),
            .listener_entity_id = acmp.listener_entity_id,
            .listener_unique_id = acmp.listener_unique_id.get()});
    }

    if (!acmp.is_response()) {
        return;
    }

    // Passively monitor all ACMP responses for connection tracking
    auto const mt = acmp.message_type();
    auto const st = acmp.status();
    // Surface every GET_RX_STATE_RESPONSE (any status), so a diagnostic caller
    // can tell "entity answered" from "no reply" even for an unconnected sink.
    if (mt == ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE) {
        pending_events_.emplace_back(RxStateEvent{
            .listener_entity_id = acmp.listener_entity_id,
            .listener_unique_id = acmp.listener_unique_id.get(),
            .status = st,
            .connected = (st == ACMP_STATUS_SUCCESS) && (acmp.talker_entity_id != Eui64{}),
            .talker_entity_id = acmp.talker_entity_id,
            .talker_unique_id = acmp.talker_unique_id.get()});
    }
    if (st == ACMP_STATUS_SUCCESS) {
        if (mt == ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE || mt == ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE) {
            pending_events_.emplace_back(ConnectionAddedEvent{make_active_connection(acmp)});
        } else if (mt == ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE) {
            if (acmp.talker_entity_id != Eui64{}) {
                pending_events_.emplace_back(ConnectionAddedEvent{make_active_connection(acmp)});
            }
        } else if (mt == ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE || mt == ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE) {
            pending_events_.emplace_back(ConnectionRemovedEvent{
                .listener_entity_id = acmp.listener_entity_id, .listener_unique_id = acmp.listener_unique_id.get()});
        }
    }

    auto resp = acmp_command_response_from_pdu(acmp);
    controller_.receive_acmp(resp, now_ns);
}

void ControllerSimple::handle_aem_response(
    Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data)
{
    if (cmd == AEM_COMMAND_READ_DESCRIPTOR) {
        handle_read_descriptor_response(target, status, sent_payload, data);
        return;
    }
    if (cmd == AEM_COMMAND_GET_STREAM_FORMAT) {
        handle_get_stream_format_response(target, status, data);
        return;
    }
    // GET_COUNTERS: parse the 136-byte AemCountersPayload and surface a
    // CountersReadyEvent so a supervise/diagnostic caller can sample a
    // specific counter (FRAMES_RX / FRAMES_TX) across two passes.
    if (cmd == AEM_COMMAND_GET_COUNTERS && status == AEM_STATUS_SUCCESS && data.size() >= sizeof(AemCountersPayload)) {
        AemCountersPayload cp{};
        span_load(cp, data.subspan(0, sizeof(AemCountersPayload)));
        CountersReadyEvent ev{};
        ev.entity_id = target;
        ev.descriptor_type = cp.descriptor_type.get();
        ev.descriptor_index = cp.descriptor_index.get();
        ev.counters_valid = cp.counters_valid.get();
        for (size_t i = 0; i < ev.counters.size(); ++i) {
            ev.counters[i] = cp.counters[i].get();
        }
        pending_events_.emplace_back(ev);
        return;
    }
    auto name_it = entity_names_.find(target);
    std::string name = (name_it != entity_names_.end()) ? name_it->second : std::string{ieee::to_string(target).view()};
    // GET/SET_CLOCK_SOURCE: also report the (current) clock source index from the
    // 8-byte AemClockSourcePayload the entity echoes back, so a scriptable caller
    // can read back which source the device is now locked to.
    if ((cmd == AEM_COMMAND_GET_CLOCK_SOURCE || cmd == AEM_COMMAND_SET_CLOCK_SOURCE) && status == AEM_STATUS_SUCCESS &&
        data.size() >= AemClockSourcePayload::LENGTH) {
        AemClockSourcePayload csp{};
        span_load(csp, data.subspan(0, AemClockSourcePayload::LENGTH));
        emit_status(std::format(
            "{} {}: {} clock_domain={} clock_source={}",
            aem_command_name(cmd),
            name,
            aem_status_name(status),
            csp.descriptor_index.get(),
            csp.clock_source_index.get()));
        return;
    }
    // Show status for other commands (identify, start/stop streaming, etc.)
    emit_status(std::format("{} {}: {}", aem_command_name(cmd), name, aem_status_name(status)));
}

void ControllerSimple::handle_read_descriptor_response(
    Eui64 target, uint8_t status, std::span<uint8_t const> sent_payload, std::span<uint8_t const> data)
{
    // Parse descriptor_type/index from the ORIGINAL request payload, not from
    // the response. Some entities (e.g. the audio interface) zero out the
    // response's echoed descriptor_type/index when returning NO_SUCH_DESCRIPTOR,
    // which would otherwise prevent us from clearing the pending query and
    // cause an infinite retry loop.
    if (sent_payload.size() < AemReadDescriptorCommandPayload::LENGTH) {
        return;
    }
    AemReadDescriptorCommandPayload sent_cmd{};
    span_load(sent_cmd, sent_payload.subspan(0, AemReadDescriptorCommandPayload::LENGTH));
    uint16_t const desc_type = sent_cmd.descriptor_type.get();
    uint16_t const desc_index = sent_cmd.descriptor_index.get();

    auto const desc_payload = (data.size() >= AemReadDescriptorResponsePayload::LENGTH)
        ? data.subspan(AemReadDescriptorResponsePayload::LENGTH)
        : std::span<uint8_t const>{};

    // Every READ_DESCRIPTOR response clears its entry from the unified queue.
    // (The dedup set in requested_descriptors_ keeps the (type, index) pair
    // so we don't re-enqueue.)
    clear_descriptor_read(target, desc_type, desc_index);

    // Cache the response so a later detail fetch can seed its builder
    // without re-issuing reads that the dedup set would skip.
    {
        auto& entry = descriptor_data_cache_[target][{desc_type, desc_index}];
        entry.success = (status == AEM_STATUS_SUCCESS);
        entry.data.assign(desc_payload.begin(), desc_payload.end());
    }

    // Feed the detail builder if one exists for this entity.
    // When complete, emit an EntityDetailReadyEvent and remove the builder.
    if (auto it = detail_builders_.find(target); it != detail_builders_.end()) {
        it->second.receive(desc_type, desc_index, status == AEM_STATUS_SUCCESS, desc_payload);
        if (it->second.is_complete()) {
            pending_events_.emplace_back(EntityDetailReadyEvent{it->second.build()});
            detail_builders_.erase(it);
        }
    }

    if (desc_type == DESCRIPTOR_ENTITY && status == AEM_STATUS_SUCCESS && desc_payload.size() >= DescriptorEntity::LENGTH) {
        DescriptorEntity desc{};
        span_load(desc, desc_payload);
        entity_names_[target] = std::string{desc.entity_name.as_string_view()};
        // Enqueue the CONFIGURATION descriptor read at the entity's
        // current_configuration index. The unified queue defers it to the
        // next tick so we don't re-enter the AEM state machine.
        enqueue_descriptor_read(target, DESCRIPTOR_CONFIGURATION, desc.current_configuration.get());
        return;
    }

    if (desc_type == DESCRIPTOR_CONFIGURATION && status == AEM_STATUS_SUCCESS) {
        handle_configuration_descriptor_response(target, desc_payload);
        return;
    }

    if (desc_type == DESCRIPTOR_STREAM_INPUT || desc_type == DESCRIPTOR_STREAM_OUTPUT) {
        handle_stream_descriptor_response(target, status, desc_type, desc_index, desc_payload);
        return;
    }

    if (desc_type == DESCRIPTOR_STRINGS) {
        handle_strings_descriptor_response(target, status, desc_index, desc_payload);
        return;
    }
}

void ControllerSimple::handle_configuration_descriptor_response(Eui64 const& target, std::span<uint8_t const> desc_payload)
{
    if (desc_payload.size() < DescriptorConfiguration::LENGTH) {
        return;
    }
    // Use span_load_padded (not span_load) because DescriptorConfiguration
    // now contains a max-sized inline descriptor_counts array and sizeof
    // is larger than the wire payload. span_load_padded copies up to
    // min(src.size(), sizeof(T)) and zero-fills the rest, so both the
    // fixed header and the populated trailer entries land in the struct.
    DescriptorConfiguration cfg{};
    span_load_padded(cfg, desc_payload);

    EntityDescriptorCounts counts{};
    auto const n = static_cast<size_t>(cfg.descriptor_counts_count.get());
    auto const n_clamped = std::min<size_t>(n, DescriptorConfiguration::MAX_DESCRIPTOR_COUNTS);

    // Walk the descriptor_counts table in declaration order, enqueueing
    // a READ_DESCRIPTOR for every (type, index) the entity advertises.
    // The unified queue dedupes against requested_descriptors_.
    for (size_t i = 0; i < n_clamped; ++i) {
        auto const& entry = cfg.descriptor_counts[i];
        auto const entry_type = entry.descriptor_type.get();
        auto const entry_count = entry.count.get();
        if (entry_type == DESCRIPTOR_STREAM_INPUT) {
            counts.stream_inputs = entry_count;
        } else if (entry_type == DESCRIPTOR_STREAM_OUTPUT) {
            counts.stream_outputs = entry_count;
        }
        // Bound enumeration so a hostile/corrupt count can't drive millions of
        // queued reads + set inserts (M2 DoS). Real entities never exceed this.
        uint16_t const read_count = std::min<uint16_t>(entry_count, MAX_DESCRIPTORS_PER_TYPE);
        if (read_count < entry_count) {
            std::print(
                stderr,
                "[atdecc] entity advertises {} descriptors of type 0x{:04x} (> {}); enumerating only {}\n",
                entry_count,
                entry_type,
                MAX_DESCRIPTORS_PER_TYPE,
                read_count);
        }
        for (uint16_t idx = 0; idx < read_count; ++idx) {
            enqueue_descriptor_read(target, entry_type, idx);
            if (entry_type == DESCRIPTOR_STREAM_INPUT || entry_type == DESCRIPTOR_STREAM_OUTPUT) {
                enqueue_stream_format_query(target, entry_type, idx);
            }
        }
    }
    descriptor_counts_[target] = counts;
}

/// Resolve a stream name from cached STRINGS payload data.
static void resolve_stream_name_from_cache(
    std::vector<std::string>& vec, uint16_t desc_index, std::vector<uint8_t> const& cached_payload, uint8_t string_offset)
{
    if (cached_payload.size() < DescriptorStrings::LENGTH) {
        return;
    }
    DescriptorStrings sd{};
    span_load(sd, make_const_span(cached_payload).first<DescriptorStrings::LENGTH>());
    auto const sa = sd.as_string_array();
    if (string_offset >= sa.size()) {  // parity with handle_strings_descriptor_response
        return;
    }
    std::string resolved{sa[string_offset]->as_string_view()};
    if (!resolved.empty()) {
        if (vec.size() <= desc_index) {
            vec.resize(static_cast<size_t>(desc_index) + 1);
        }
        vec[desc_index] = std::move(resolved);
    }
}

void ControllerSimple::handle_stream_descriptor_response(
    Eui64 target, uint8_t status, uint16_t desc_type, uint16_t desc_index, std::span<uint8_t const> desc_payload)
{
    auto& map = (desc_type == DESCRIPTOR_STREAM_OUTPUT) ? talker_stream_names_ : listener_stream_names_;
    auto& vec = map[target];
    if (vec.size() <= desc_index) {
        vec.resize(static_cast<size_t>(desc_index) + 1);
    }

    if (status != AEM_STATUS_SUCCESS || desc_payload.size() < DescriptorStream::MINIMUM_LENGTH) {
        if (status != AEM_STATUS_SUCCESS) {
            vec[desc_index] = std::format("({})", aem_status_name(status));
        }
        return;
    }

    // DescriptorStream now has an inline stream_formats trailer, so
    // sizeof() is much larger than the on-wire payload. span_load_padded
    // copies up to sizeof(T) and zero-fills the rest — safe regardless
    // of whether the wire carries a 132 (2013) or 138 (2016+) header or
    // any number of trailing format entries.
    DescriptorStream desc{};
    span_load_padded(desc, desc_payload);
    std::string name{desc.object_name.as_string_view()};

    if (!name.empty()) {
        vec[desc_index] = std::move(name);
        return;
    }

    // object_name is empty — try the localized_description fallback.
    auto const loc_ref = parse_localized_description(desc.localized_description.get());
    if (!loc_ref) {
        return;
    }

    auto const key = std::make_pair(target, loc_ref->strings_descriptor_index);

    // Try the cache first — STRINGS may have been read already by the
    // CONFIGURATION-driven full crawl, in which case the response handler
    // would have nothing to fire and the data would be stranded.
    if (auto cit = strings_cache_.find(key); cit != strings_cache_.end()) {
        resolve_stream_name_from_cache(vec, desc_index, cit->second, loc_ref->string_offset);
        return;
    }

    auto& waiters = strings_waiters_[key];
    bool const already_waiting = std::any_of(waiters.begin(), waiters.end(), [&](auto const& w) {
        return w.stream_desc_type == desc_type && w.stream_index == desc_index;
    });
    if (!already_waiting) {
        waiters.push_back(
            StringsWaiter{.stream_desc_type = desc_type, .stream_index = desc_index, .string_offset = loc_ref->string_offset});
    }

    // Enqueue the STRINGS descriptor read on the unified queue.
    enqueue_descriptor_read(target, DESCRIPTOR_STRINGS, loc_ref->strings_descriptor_index);
}

void ControllerSimple::handle_strings_descriptor_response(
    Eui64 target, uint8_t status, uint16_t strings_desc_idx, std::span<uint8_t const> desc_payload)
{
    if (status != AEM_STATUS_SUCCESS || desc_payload.size() < DescriptorStrings::LENGTH) {
        return;
    }

    DescriptorStrings strings_desc{};
    span_load(strings_desc, desc_payload);

    // Cache the parsed payload first so any stream descriptor that arrives
    // LATER (the new normal under the unified crawl) can resolve from it.
    auto const key = std::make_pair(target, strings_desc_idx);
    strings_cache_[key].assign(desc_payload.begin(), desc_payload.begin() + DescriptorStrings::LENGTH);

    std::vector<StringsWaiter> waiters;
    if (auto it = strings_waiters_.find(key); it != strings_waiters_.end()) {
        waiters = std::move(it->second);
        strings_waiters_.erase(it);
    }
    auto const string_array = strings_desc.as_string_array();

    for (auto const& w : waiters) {
        if (w.string_offset >= string_array.size()) {
            continue;
        }
        std::string resolved{string_array[w.string_offset]->as_string_view()};
        if (resolved.empty()) {
            continue;
        }
        auto& map = (w.stream_desc_type == DESCRIPTOR_STREAM_OUTPUT) ? talker_stream_names_ : listener_stream_names_;
        auto& vec = map[target];
        if (vec.size() <= w.stream_index) {
            vec.resize(static_cast<size_t>(w.stream_index) + 1);
        }
        vec[w.stream_index] = std::move(resolved);
    }
}

void ControllerSimple::handle_get_stream_format_response(Eui64 target, uint8_t status, std::span<uint8_t const> data)
{
    if (data.size() < AemStreamFormatPayload::LENGTH) {
        return;
    }
    AemStreamFormatPayload resp{};
    span_load(resp, data.subspan(0, AemStreamFormatPayload::LENGTH));
    uint16_t const desc_type = resp.descriptor_type.get();
    uint16_t const idx = resp.descriptor_index.get();

    // A response-supplied descriptor_index can be up to 65535; we only ever request
    // indices below the cap, so ignore anything beyond it rather than resize a
    // vector to ~64K entries from a single crafted response (M2 DoS).
    if (idx >= MAX_DESCRIPTORS_PER_TYPE) {
        return;
    }
    auto& map = (desc_type == DESCRIPTOR_STREAM_OUTPUT) ? talker_formats_ : listener_formats_;
    auto& vec = map[target];
    if (vec.size() <= idx) {
        vec.resize(static_cast<size_t>(idx) + 1);
    }

    if (status == AEM_STATUS_SUCCESS) {
        uint64_t fmt = 0;
        for (auto b : resp.stream_format) {
            fmt = (fmt << 8) | b;
        }
        vec[idx] = avtp::stream_format_to_string(fmt);
    } else {
        vec[idx] = std::format("({})", aem_status_name(status));
    }

    clear_pending_format_query(target, desc_type, idx);
}

void ControllerSimple::clear_pending_format_query(Eui64 const& target, uint16_t desc_type, uint16_t idx)
{
    auto it = std::remove_if(stream_format_queries_.begin(), stream_format_queries_.end(), [&](auto const& q) {
        return q.target == target && q.desc_type == desc_type && q.desc_index == idx;
    });
    stream_format_queries_.erase(it, stream_format_queries_.end());
}

void ControllerSimple::enqueue_stream_format_query(Eui64 const& target, uint16_t desc_type, uint16_t desc_index)
{
    bool const already = std::any_of(stream_format_queries_.begin(), stream_format_queries_.end(), [&](auto const& r) {
        return r.target == target && r.desc_type == desc_type && r.desc_index == desc_index;
    });
    if (!already) {
        stream_format_queries_.push_back(
            PendingStreamFormatQuery{.target = target, .desc_type = desc_type, .desc_index = desc_index, .last_sent_ns = 0});
    }
}

void ControllerSimple::enqueue_descriptor_read(Eui64 const& target, uint16_t desc_type, uint16_t desc_index)
{
    // Dedupe against the per-entity already-requested set.
    auto& seen = requested_descriptors_[target];
    if (!seen.insert(std::make_pair(desc_type, desc_index)).second) {
        return;  // already requested (in queue, in flight, or already received)
    }
    descriptor_read_queue_.push_back(
        PendingDescriptorRead{.target = target, .descriptor_type = desc_type, .descriptor_index = desc_index, .last_sent_ns = 0});
}

void ControllerSimple::clear_descriptor_read(Eui64 const& target, uint16_t desc_type, uint16_t desc_index)
{
    auto it = std::remove_if(descriptor_read_queue_.begin(), descriptor_read_queue_.end(), [&](auto const& q) {
        return q.target == target && q.descriptor_type == desc_type && q.descriptor_index == desc_index;
    });
    descriptor_read_queue_.erase(it, descriptor_read_queue_.end());
}

void ControllerSimple::send_next_descriptor_read(int64_t now_ns)
{
    // Throttle against AEM inflight tracker. The AEM controller's inflight
    // table is bounded by AemControllerContext::MAX_INFLIGHT (8). If we issue
    // a command when no slot is free, the packet goes on the wire untracked
    // and its response is silently dropped — which would strand the queued
    // descriptor read forever.
    if (controller_.aem_inflight_count() >= atdecc::AemControllerContext::MAX_INFLIGHT) {
        return;
    }
    if (descriptor_read_queue_.empty()) {
        return;
    }
    size_t const n = descriptor_read_queue_.size();
    for (size_t step = 0; step < n; ++step) {
        size_t const i = (descriptor_read_cursor_ + step) % n;
        auto& req = descriptor_read_queue_[i];
        if (req.last_sent_ns != 0 && (now_ns - req.last_sent_ns) < STREAM_QUERY_RETRY_NS) {
            continue;
        }
        controller_.read_descriptor(req.target, req.descriptor_type, req.descriptor_index);
        req.last_sent_ns = now_ns;
        descriptor_read_cursor_ = i + 1;
        return;
    }
}

void ControllerSimple::send_next_stream_format_query(int64_t now_ns)
{
    // Throttle against AEM inflight tracker — see send_next_descriptor_read.
    if (controller_.aem_inflight_count() >= atdecc::AemControllerContext::MAX_INFLIGHT) {
        return;
    }
    if (stream_format_queries_.empty()) {
        return;
    }
    size_t const n = stream_format_queries_.size();
    for (size_t step = 0; step < n; ++step) {
        size_t const i = (stream_format_cursor_ + step) % n;
        auto& req = stream_format_queries_[i];
        if (req.last_sent_ns != 0 && (now_ns - req.last_sent_ns) < STREAM_QUERY_RETRY_NS) {
            continue;
        }
        AemGetStreamFormatCommandPayload cmd_payload{};
        cmd_payload.descriptor_type = ieee::doublet_t{req.desc_type};
        cmd_payload.descriptor_index = ieee::doublet_t{req.desc_index};
        std::array<uint8_t, AemGetStreamFormatCommandPayload::LENGTH> payload_bytes{};
        span_store(std::span{payload_bytes}, cmd_payload);
        controller_.send_aem_command(req.target, AEM_COMMAND_GET_STREAM_FORMAT, payload_bytes);
        req.last_sent_ns = now_ns;
        stream_format_cursor_ = i + 1;
        return;
    }
}

}  // namespace statusbar::atdecc_tools
