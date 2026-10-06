// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#include "statusbar/avb_entity/avb_entity_avb_info_mirror.hpp"

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>

namespace statusbar::avb_entity {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;

AvbInfoMirror::AvbInfoMirror(Config config, SendMulticastFn send_multicast, SendUnicastFn send_unicast)
    : config_{config}
    , controller_{config.controller_entity_id}
{
    nanoavb::AemControllerEntityCallbacks callbacks;
    callbacks.send_atdecc_multicast = std::move(send_multicast);
    callbacks.send_atdecc_unicast = std::move(send_unicast);
    callbacks.on_entity_available = [this](DiscoveredEntity const& e) {
        if (e.adpdu.entity_id == config_.target_entity_id) {
            on_target_available();
        }
    };
    callbacks.on_entity_departing = [this](ieee::Eui64 const id) {
        if (id == config_.target_entity_id) {
            // Start over when it comes back: re-read, re-register.
            target_seen_ = false;
            registered_ = false;
            reading_ = false;
            next_read_index_ = 0;
            descriptors_done_ = false;
        }
    };
    callbacks.on_aem_response =
        [this](
            ieee::Eui64 const target, uint16_t cmd, uint8_t status, std::span<uint8_t const> sent, std::span<uint8_t const> data) {
            if (target == config_.target_entity_id) {
                handle_response(cmd, status, sent, data);
            }
        };
    callbacks.on_aem_timeout = [this](ieee::Eui64 const target, uint16_t cmd) {
        if (target == config_.target_entity_id && cmd == AEM_COMMAND_READ_DESCRIPTOR) {
            reading_ = false;  // retried on the next tick
        }
    };
    callbacks.on_unsolicited = [this](ieee::Eui64 const target, uint16_t cmd, uint8_t status, std::span<uint8_t const> data) {
        if (target == config_.target_entity_id && cmd == AEM_COMMAND_GET_AVB_INFO && status == AEM_STATUS_SUCCESS) {
            handle_avb_info(data);
        }
    };
    controller_.set_callbacks(std::move(callbacks));
    controller_.start();
}

void AvbInfoMirror::receive_frame(ieee::Eui48 const& src_mac, std::span<uint8_t const> payload, int64_t const now_ns)
{
    if (payload.empty()) {
        return;
    }
    switch (payload[0]) {
        case avtp::AvtpSubtype::adp: {
            if (payload.size() < AdpDu::LENGTH) {
                return;
            }
            AdpDu adp{};
            span_load(adp, payload);
            controller_.receive_adp(adp, src_mac, now_ns);
            break;
        }
        case avtp::AvtpSubtype::aecp:
            controller_.receive_aecp(payload, now_ns);
            break;
        default:
            break;
    }
}

void AvbInfoMirror::on_target_available()
{
    target_seen_ = true;
}

void AvbInfoMirror::tick(int64_t const now_ns)
{
    controller_.tick(now_ns);
    if (!target_seen_) {
        if (last_discover_ns_ == 0 || now_ns - last_discover_ns_ >= config_.discover_interval_ns) {
            controller_.discover(config_.target_entity_id);
            last_discover_ns_ = now_ns;
        }
        return;
    }
    // One descriptor read in flight at a time, index 0 upward until the
    // target says NO_SUCH_DESCRIPTOR (or the table is full).
    if (!descriptors_done_ && !reading_) {
        if (next_read_index_ >= MAX_INTERFACES) {
            descriptors_done_ = true;
        } else if (controller_.read_descriptor(config_.target_entity_id, DESCRIPTOR_AVB_INTERFACE, next_read_index_)) {
            reading_ = true;
        }
    }
    if (descriptors_done_ && !registered_) {
        if (controller_.register_unsolicited(config_.target_entity_id)) {
            registered_ = true;
        }
    }
    if (!descriptors_done_) {
        return;
    }
    for (size_t i = 0; i < interface_count_; ++i) {
        auto& iface = interfaces_[i];
        if (iface.last_poll_ns == 0 || now_ns - iface.last_poll_ns >= config_.poll_interval_ns) {
            if (controller_.get_avb_info(config_.target_entity_id, iface.index)) {
                iface.last_poll_ns = now_ns;
            }
        }
    }
}

auto AvbInfoMirror::find(uint16_t const index) const noexcept -> Interface const*
{
    for (size_t i = 0; i < interface_count_; ++i) {
        if (interfaces_[i].index == index) {
            return &interfaces_[i];
        }
    }
    return nullptr;
}

auto AvbInfoMirror::interface_at(uint16_t const index) -> Interface&
{
    for (size_t i = 0; i < interface_count_; ++i) {
        if (interfaces_[i].index == index) {
            return interfaces_[i];
        }
    }
    auto& slot = interfaces_[std::min(interface_count_, MAX_INTERFACES - 1)];
    if (interface_count_ < MAX_INTERFACES) {
        ++interface_count_;
    }
    slot = Interface{};
    slot.index = index;
    return slot;
}

void AvbInfoMirror::handle_response(
    uint16_t const cmd, uint8_t const status, std::span<uint8_t const> sent, std::span<uint8_t const> data)
{
    if (cmd == AEM_COMMAND_READ_DESCRIPTOR) {
        handle_read_descriptor(sent, status, data);
    } else if (cmd == AEM_COMMAND_GET_AVB_INFO && status == AEM_STATUS_SUCCESS) {
        handle_avb_info(data);
    }
}

void AvbInfoMirror::handle_read_descriptor(std::span<uint8_t const> sent, uint8_t const status, std::span<uint8_t const> data)
{
    if (sent.size() < AemReadDescriptorCommandPayload::LENGTH) {
        return;
    }
    AemReadDescriptorCommandPayload request{};
    span_load(request, sent.first(AemReadDescriptorCommandPayload::LENGTH));
    if (request.descriptor_type.get() != DESCRIPTOR_AVB_INTERFACE || request.descriptor_index.get() != next_read_index_) {
        return;
    }
    reading_ = false;
    if (status != AEM_STATUS_SUCCESS) {
        descriptors_done_ = true;  // NO_SUCH_DESCRIPTOR (or refused): the table ends here
        return;
    }
    auto const payload = data.size() > AemReadDescriptorResponsePayload::LENGTH
        ? data.subspan(AemReadDescriptorResponsePayload::LENGTH)
        : std::span<uint8_t const>{};
    if (payload.size() < DescriptorAvbInterface::MINIMUM_LENGTH) {
        descriptors_done_ = true;
        return;
    }
    DescriptorAvbInterface desc{};
    span_load_padded(desc, payload);
    auto& iface = interface_at(next_read_index_);
    iface.descriptor = desc;
    ++next_read_index_;
    if (on_update_) {
        on_update_(iface.index, &*iface.descriptor, nullptr);
    }
}

void AvbInfoMirror::handle_avb_info(std::span<uint8_t const> data)
{
    if (data.size() < AemAvbInfoPayload::LENGTH) {
        return;
    }
    AemAvbInfoPayload p{};
    span_load(p, data.first(AemAvbInfoPayload::LENGTH));
    if (p.descriptor_type.get() != DESCRIPTOR_AVB_INTERFACE) {
        return;
    }
    nanoavb::AvbInfo info{};
    info.gptp_grandmaster_id = p.gptp_grandmaster_id.to_eui64();
    info.propagation_delay = p.propagation_delay.get();
    info.gptp_domain_number = static_cast<uint8_t>(p.gptp_domain_number);
    info.flags = static_cast<uint8_t>(p.flags);
    size_t const count = std::min<size_t>(p.msrp_mappings_count.get(), nanoavb::AvbInfo::MAX_MSRP_MAPPINGS);
    for (size_t i = 0; i < count; ++i) {
        auto const at = AemAvbInfoPayload::LENGTH + (i * 4);
        if (at + 4 > data.size()) {
            break;
        }
        info.msrp_mappings.push_back(
            {.traffic_class = data[at],
             .priority = data[at + 1],
             .vlan_id = static_cast<uint16_t>((data[at + 2] << 8U) | data[at + 3])});
    }
    auto& iface = interface_at(p.descriptor_index.get());
    bool changed = !iface.info.has_value();
    if (!changed) {
        auto const& old = *iface.info;
        changed = old.gptp_grandmaster_id != info.gptp_grandmaster_id || old.propagation_delay != info.propagation_delay ||
            old.gptp_domain_number != info.gptp_domain_number || old.flags != info.flags ||
            old.msrp_mappings.size() != info.msrp_mappings.size();
        for (size_t i = 0; !changed && i < info.msrp_mappings.size(); ++i) {
            changed = old.msrp_mappings[i].traffic_class != info.msrp_mappings[i].traffic_class ||
                old.msrp_mappings[i].priority != info.msrp_mappings[i].priority ||
                old.msrp_mappings[i].vlan_id != info.msrp_mappings[i].vlan_id;
        }
    }
    iface.info = info;
    if (changed && on_update_) {
        on_update_(iface.index, nullptr, &*iface.info);
    }
}

//
// AvbInfoMirrorPort
//

AvbInfoMirrorPort::AvbInfoMirrorPort(std::string_view const interface_name, AvbInfoMirror::Config config)
    : mirror_{
          config,
          [this](std::span<uint8_t const> packet) -> bool { return context_.send(&ATDECC_MULTICAST_MAC, packet).has_value(); },
          [this](ieee::Eui48 const& dest, std::span<uint8_t const> packet) -> bool {
              return context_.send(&dest, packet).has_value();
          }}
{
    (void)context_.open(interface_name, avtp::AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC);
}

void AvbInfoMirrorPort::on_ready(int64_t const now_ns)
{
    ieee::Eui48 src_mac{};
    ieee::Eui48 dest_mac{};
    while (true) {
        auto const result = context_.recv(&src_mac, &dest_mac, payload_buf_);
        if (!result || *result <= 0) {
            break;
        }
        mirror_.receive_frame(src_mac, {payload_buf_.data(), static_cast<size_t>(*result)}, now_ns);
    }
}

}  // namespace statusbar::avb_entity
