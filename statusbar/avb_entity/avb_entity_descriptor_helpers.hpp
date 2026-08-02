#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_descriptor_helpers.hpp
/// @brief Shared descriptor helpers for the blob-loaded AVB entities.
///
/// Consolidates three copies that lived verbatim in avb_entity_audio_io.cpp,
/// avb_entity_am824_io.cpp and avb_entity_tone_generator.cpp:
///
///   - load_descriptor<T>()        — load one AEM descriptor of type T from a
///                                   DescriptorStorage blob (partial-load safe).
///   - channels_from_storage()     — channel count from the first AUDIO_CLUSTER
///                                   descriptor (per-entity default).
///   - EntityIdentityDescriptorHandler — serves descriptors from an .aem blob
///                                   (symbol-aware), patching the runtime-only
///                                   ENTITY identity and (optionally) the
///                                   AVB_INTERFACE network + gPTP fields.
///
/// The AVB_INTERFACE patch is opt-in via the ctor's patch_avb_interface flag:
/// audio + tone patch it (live NIC MAC + slave-only gPTP params); am824 serves
/// the AVB_INTERFACE verbatim from its blob (patch_avb_interface=false), matching
/// its prior handler which did not override on_get_avb_interface.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace statusbar::avb_entity {

using statusbar::atdecc::aem::AtdeccString;
using statusbar::atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER;
using statusbar::atdecc::aem::DescriptorAudioCluster;
using statusbar::atdecc::aem::DescriptorAvbInterface;
using statusbar::atdecc::aem::DescriptorEntity;
using statusbar::atdecc::aem::DescriptorRef;

/// Load a single AEM descriptor of type T from storage.
/// Accepts blobs smaller than sizeof(T) via partial-load fallback.
template <typename T>
[[nodiscard]] inline auto load_descriptor(
    nanoavb::DescriptorStorage const& storage, uint16_t config_idx, uint16_t type, uint16_t index) -> StatusValue<T>
{
    auto result = storage.get_descriptor(config_idx, type, index);
    if (!result) {
        return failure(result.error());
    }
    auto const blob = *result;
    T desc{};
    span_load_padded(desc, blob);
    return success(desc);
}

/// Channel count from the first AUDIO_CLUSTER descriptor (default_channels when
/// absent). Drives the data-plane buffer sizing; the rest of the model is served
/// straight from the blob.
[[nodiscard]] inline auto channels_from_storage(nanoavb::DescriptorStorage const& storage, size_t default_channels) -> size_t
{
    size_t channels = default_channels;
    if (auto r = load_descriptor<DescriptorAudioCluster>(storage, 0, DESCRIPTOR_AUDIO_CLUSTER, 0)) {
        auto const ch = static_cast<uint16_t>(r->channel_count);
        if (ch >= 1) {
            channels = static_cast<size_t>(ch);
        }
    }
    return channels;
}

/// Find the clock-source index whose CLOCK_SOURCE descriptor is the
/// INPUT_STREAM at STREAM_INPUT @p stream_input_index — i.e. the blob's CRF
/// clock input (kit phase 3c). nullopt when the model declares none.
[[nodiscard]] inline auto find_input_stream_clock_source(
    nanoavb::DescriptorStorage const& storage, uint16_t const stream_input_index) -> std::optional<uint16_t>
{
    using statusbar::atdecc::aem::CLOCK_SOURCE_TYPE_INPUT_STREAM;
    using statusbar::atdecc::aem::DESCRIPTOR_CLOCK_SOURCE;
    using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    using statusbar::atdecc::aem::DescriptorClockSource;

    for (uint16_t index = 0;; ++index) {
        auto const cs = load_descriptor<DescriptorClockSource>(storage, 0, DESCRIPTOR_CLOCK_SOURCE, index);
        if (!cs) {
            return std::nullopt;
        }
        if (cs->clock_source_type == CLOCK_SOURCE_TYPE_INPUT_STREAM && cs->clock_source_location_type == DESCRIPTOR_STREAM_INPUT &&
            cs->clock_source_location_index == stream_input_index) {
            return index;
        }
    }
}

/// The CLOCK_DOMAIN 0 authored default clock-source selection (0 when the
/// model declares no clock domain).
[[nodiscard]] inline auto authored_clock_source(nanoavb::DescriptorStorage const& storage) -> uint16_t
{
    using statusbar::atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN;
    using statusbar::atdecc::aem::DescriptorClockDomain;

    if (auto const cd = load_descriptor<DescriptorClockDomain>(storage, 0, DESCRIPTOR_CLOCK_DOMAIN, 0)) {
        return cd->clock_source_index;
    }
    return 0;
}

/// Serves an entity's descriptors from its .aem blob (symbol-aware), patching the
/// runtime-only seams that cannot live in a static blob: the ENTITY identity
/// (entity_id/model_id/name/firmware, from config) and — when patch_avb_interface
/// is set — the AVB_INTERFACE network + gPTP identity (live NIC MAC, its
/// modified-EUI-64 clock identity, and the slave-only gPTP params the blob leaves
/// zero). Every other descriptor is served verbatim; with patch_avb_interface
/// false the AVB_INTERFACE is served verbatim too.
class EntityIdentityDescriptorHandler : public nanoavb::DescriptorStorageHandler
{
  public:
    EntityIdentityDescriptorHandler(
        nanoavb::DescriptorStorage storage,
        ieee::Eui64 entity_id,
        ieee::Eui64 entity_model_id,
        std::string_view firmware_version,
        std::string_view entity_name,
        std::optional<ieee::Eui48> iface_mac,
        bool patch_avb_interface)
        : DescriptorStorageHandler{storage}
        , entity_id_{entity_id}
        , entity_model_id_{entity_model_id}
        , firmware_version_{firmware_version}
        , iface_mac_{iface_mac}
        , patch_avb_{patch_avb_interface}
    {
        // Built-in GET_NAME/SET_NAME of the ENTITY's entity_name (descriptor 0,
        // name 0): seed it from config; the base then serves get/set and reflects
        // the current value here in on_get_entity. In-memory only (resets on
        // restart) unless a caller wires set_on_entity_name_changed for NV storage.
        manage_entity_name(AtdeccString{entity_name});
    }

    auto on_get_entity(nanoavb::DescriptorId id, DescriptorEntity& desc) -> bool override
    {
        // Base fills the blob bytes and reflects the managed entity_name.
        if (!DescriptorStorageHandler::on_get_entity(id, desc)) {
            return false;
        }
        desc.entity_id = entity_id_;
        desc.entity_model_id = entity_model_id_;
        desc.firmware_version = AtdeccString{firmware_version_};
        return true;
    }

    auto on_get_avb_interface(nanoavb::DescriptorId id, DescriptorAvbInterface& desc) -> bool override
    {
        if (!DescriptorStorageHandler::on_get_avb_interface(id, desc)) {
            return false;
        }
        if (!patch_avb_) {
            return true;
        }
        if (iface_mac_) {
            desc.mac_address = *iface_mac_;
            desc.clock_identity = iface_mac_->to_modified_eui64();
        }
        desc.priority1 = 248;    // gPTP default priority1
        desc.clock_class = 248;  // not grandmaster-capable (slave-only)
        desc.offset_scaled_log_variance = 0x436A;
        desc.clock_accuracy = 0xFE;  // unknown
        desc.priority2 = 248;
        desc.domain_number = 0;
        desc.log_sync_interval = static_cast<uint8_t>(static_cast<int8_t>(-3));  // 125 ms (gPTP Class A)
        desc.log_announce_interval = 0;                                          // 1 s
        desc.log_pdelay_interval = 0;                                            // 1 s
        return true;
    }

  private:
    ieee::Eui64 entity_id_;
    ieee::Eui64 entity_model_id_;
    std::string firmware_version_;
    std::optional<ieee::Eui48> iface_mac_;
    bool patch_avb_;
};

}  // namespace statusbar::avb_entity
