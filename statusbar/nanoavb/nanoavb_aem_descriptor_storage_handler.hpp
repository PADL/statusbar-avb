#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// DescriptorStorageHandler — a reusable AemEntityHandler backed by a
/// compiled-in (.aem blob) DescriptorStorage.
///
/// This is the canonical "serve everything from a blob" handler. It
/// overrides every on_get_* method to check whether the attached
/// DescriptorStorage has the requested (configuration, type, index)
/// triple and returns true if so. The actual descriptor bytes are
/// already populated into the struct by AemEntityModel's dispatch
/// wrapper (which calls span_load_padded from the same blob), so the
/// handler only needs to approve the response.
///
/// Intended use as a base class
/// ----------------------------
/// Applications that want to override a handful of fields at runtime
/// (e.g. inject the current entity_id, set the current_configuration,
/// patch a dynamic stream_format) should DERIVE from
/// DescriptorStorageHandler and override the handler methods they
/// care about. Each override should first call the base-class method
/// to let the storage preload happen, then patch the desired fields:
///
///     class MyHandler : public DescriptorStorageHandler {
///       public:
///         explicit MyHandler(DescriptorStorage s, Eui64 id)
///             : DescriptorStorageHandler{s}, entity_id_{id} {}
///
///         auto on_get_entity(DescriptorRef ref, uint32_t sym,
///                            DescriptorEntity& desc) -> bool override {
///             if (!DescriptorStorageHandler::on_get_entity(ref, sym, desc))
///                 return false;
///             desc.entity_id = entity_id_;
///             return true;
///         }
///     };
///
/// Standalone use
/// --------------
/// For read-only entities that just need to emit whatever the blob
/// contains (no runtime patching), an instance of DescriptorStorageHandler
/// itself can be passed directly to AemEntityModel / AemCommandHandler
/// without subclassing.

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/sg14/inplace_function.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace statusbar::nanoavb {

using atdecc::aem::DescriptorStorage;

/// Handler that serves descriptors out of an attached DescriptorStorage
/// blob. Each on_get_<type> override looks up the (configuration, type,
/// index) triple in the storage and, if found, loads the descriptor's
/// bytes into the caller-supplied struct via span_load_padded. Returns
/// false if the descriptor is not in the blob.
///
/// Self-preload rationale
/// ----------------------
/// AemEntityModel::dispatch_fixed can ALSO preload a descriptor from
/// a DescriptorStorage if one is attached to the model itself — but
/// callers often want to construct an AemEntityModel with just a
/// handler and no separate storage. The handler preloads from its own
/// storage so the behaviour is the same regardless of whether the
/// enclosing AemEntityModel was given a storage. Double-preloading
/// (when both the model and the handler point at the same storage) is
/// a no-op: the second memcpy overwrites the first with identical bytes.
///
/// Intended use as a base class
/// ----------------------------
/// Derive from DescriptorStorageHandler and override the handler
/// methods that need to patch dynamic fields after the storage
/// preload. Each override should first call the base-class method
/// (so the bytes are loaded and the storage presence check runs)
/// and then mutate the fields it owns:
///
///     class MyHandler : public DescriptorStorageHandler {
///       public:
///         explicit MyHandler(DescriptorStorage s, Eui64 id)
///             : DescriptorStorageHandler{s}, entity_id_{id} {}
///
///         auto on_get_entity(DescriptorRef ref, uint32_t sym,
///                            DescriptorEntity& desc) -> bool override {
///             if (!DescriptorStorageHandler::on_get_entity(ref, sym, desc))
///                 return false;
///             desc.entity_id = entity_id_;
///             return true;
///         }
///     };
class DescriptorStorageHandler : public AemEntityHandler
{
  public:
    explicit DescriptorStorageHandler(DescriptorStorage storage) noexcept
        : storage_{storage}
    {}

    /// Expose the backing blob so the dispatch can resolve descriptor symbols.
    [[nodiscard]] auto descriptor_storage() const noexcept -> DescriptorStorage const* override { return &storage_; }

    // ---- Top-level ------------------------------------------------------

    auto on_get_entity(DescriptorRef ref, uint32_t /*symbol*/, DescriptorEntity& desc) -> bool override
    {
        if (!load(ref, desc)) {
            return false;
        }
        // Reflect the (runtime-changeable) managed entity name, if enabled.
        if (manage_entity_name_) {
            desc.entity_name = entity_name_;
        }
        return true;
    }

    // ---- Built-in entity name (every entity has exactly one) -------------
    //
    // Opt-in: call manage_entity_name(initial), and this handler then serves GET_NAME
    // and SET_NAME for the ENTITY descriptor's entity_name (descriptor_index 0,
    // name_index 0) AND reflects the current value in on_get_entity (so READ_DESCRIPTOR
    // agrees). The name lives in memory -- a controller can change it and read it back,
    // and it resets to the seed on restart. To make it survive a restart, set the change
    // callback and persist there, then seed manage_entity_name() from that store at boot.

    /// Enable built-in entity-name get/set, seeded with @p initial.
    void manage_entity_name(AtdeccString initial) noexcept
    {
        entity_name_ = initial;
        manage_entity_name_ = true;
    }

    /// The current entity name (meaningful once manage_entity_name() was called).
    [[nodiscard]] auto entity_name() const noexcept -> AtdeccString const& { return entity_name_; }

    /// Called when a controller's SET_NAME requests a new entity name. Return
    /// AEM_STATUS_SUCCESS to accept (the in-memory name then updates; persist to
    /// non-volatile storage here if you want it to survive a restart), or any other
    /// AEM_STATUS_* to reject the change. Unset => accept (in-memory only).
    void set_on_entity_name_changed(statusbar::sg14::inplace_function<uint8_t(AtdeccString const&), 64> fn)
    {
        on_entity_name_changed_ = std::move(fn);
    }

    auto on_get_name(NameRef ref, uint32_t /*symbol*/) const -> std::optional<AtdeccString> override
    {
        if (manages_entity_name_field(ref)) {
            return entity_name_;
        }
        return std::nullopt;
    }

    auto on_set_name(NameRef ref, uint32_t /*symbol*/, AtdeccString const& name) -> uint8_t override
    {
        if (!manages_entity_name_field(ref)) {
            return atdecc::AEM_STATUS_NOT_IMPLEMENTED;
        }
        uint8_t const status = on_entity_name_changed_ ? on_entity_name_changed_(name) : atdecc::AEM_STATUS_SUCCESS;
        if (status == atdecc::AEM_STATUS_SUCCESS) {
            entity_name_ = name;
        }
        return status;
    }

    auto on_get_configuration(DescriptorRef ref, uint32_t /*symbol*/, DescriptorConfiguration& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Unit descriptors -----------------------------------------------

    auto on_get_audio_unit(DescriptorRef ref, uint32_t /*symbol*/, DescriptorAudioUnit& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_video_unit(DescriptorRef ref, uint32_t /*symbol*/, DescriptorVideoUnit& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_sensor_unit(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSensorUnit& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Streams / Jacks / Interface / Clock ----------------------------

    auto on_get_stream(DescriptorRef ref, uint32_t /*symbol*/, DescriptorStream& desc) -> bool override { return load(ref, desc); }

    auto on_get_jack(DescriptorRef ref, uint32_t /*symbol*/, DescriptorJack& desc) -> bool override { return load(ref, desc); }

    auto on_get_avb_interface(DescriptorRef ref, uint32_t /*symbol*/, DescriptorAvbInterface& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_clock_source(DescriptorRef ref, uint32_t /*symbol*/, DescriptorClockSource& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_clock_domain(DescriptorRef ref, uint32_t /*symbol*/, DescriptorClockDomain& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Memory / Locale / Strings --------------------------------------

    auto on_get_memory_object(DescriptorRef ref, uint32_t /*symbol*/, DescriptorMemoryObject& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_locale(DescriptorRef ref, uint32_t /*symbol*/, DescriptorLocale& desc) -> bool override { return load(ref, desc); }

    auto on_get_strings(DescriptorRef ref, uint32_t /*symbol*/, DescriptorStrings& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Ports ----------------------------------------------------------

    auto on_get_stream_port(DescriptorRef ref, uint32_t /*symbol*/, DescriptorStreamPort& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_external_port(DescriptorRef ref, uint32_t /*symbol*/, DescriptorExternalPort& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_internal_port(DescriptorRef ref, uint32_t /*symbol*/, DescriptorInternalPort& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Clusters / Maps / Controls -------------------------------------

    auto on_get_audio_cluster(DescriptorRef ref, uint32_t /*symbol*/, DescriptorAudioCluster& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_video_cluster(DescriptorRef ref, uint32_t /*symbol*/, DescriptorVideoCluster& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_sensor_cluster(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSensorCluster& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_audio_map(DescriptorRef ref, uint32_t /*symbol*/, DescriptorAudioMap& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_video_map(DescriptorRef ref, uint32_t /*symbol*/, DescriptorVideoMap& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_sensor_map(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSensorMap& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_control(DescriptorRef ref, uint32_t /*symbol*/, DescriptorControl& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_control_block(DescriptorRef ref, uint32_t /*symbol*/, DescriptorControlBlock& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Signal routing -------------------------------------------------

    auto on_get_signal_selector(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSignalSelector& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_mixer(DescriptorRef ref, uint32_t /*symbol*/, DescriptorMixer& desc) -> bool override { return load(ref, desc); }

    auto on_get_matrix(DescriptorRef ref, uint32_t /*symbol*/, DescriptorMatrix& desc) -> bool override { return load(ref, desc); }

    auto on_get_matrix_signal(DescriptorRef ref, uint32_t /*symbol*/, DescriptorMatrixSignal& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_signal_splitter(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSignalSplitter& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_signal_combiner(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSignalCombiner& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_signal_demultiplexer(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSignalDemultiplexer& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_signal_multiplexer(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSignalMultiplexer& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_signal_transcoder(DescriptorRef ref, uint32_t /*symbol*/, DescriptorSignalTranscoder& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Timing / PTP ---------------------------------------------------

    auto on_get_timing(DescriptorRef ref, uint32_t /*symbol*/, DescriptorTiming& desc) -> bool override { return load(ref, desc); }

    auto on_get_ptp_instance(DescriptorRef ref, uint32_t /*symbol*/, DescriptorPtpInstance& desc) -> bool override
    {
        return load(ref, desc);
    }

    auto on_get_ptp_port(DescriptorRef ref, uint32_t /*symbol*/, DescriptorPtpPort& desc) -> bool override
    {
        return load(ref, desc);
    }

    // ---- Built-in IDENTIFY control (automatic when the blob has one) ------
    //
    // When the blob contains a CONTROL descriptor with the standard IDENTIFY
    // control_type, this handler accepts SET_CONTROL (stores the value) and
    // serves GET_CONTROL for it, so a blob-backed entity honors identify with
    // no per-entity code. The externally visible indicator is the
    // AemCommandHandler's identify_changed callback (fired on the successful
    // SET); the stored value keeps GET_CONTROL and polling controllers honest.

    auto on_set_descriptor_value(uint16_t command_type, DescriptorRef ref, uint32_t symbol, std::span<uint8_t const> value)
        -> uint8_t override
    {
        if (command_type == atdecc::AEM_COMMAND_SET_CONTROL && is_identify_control(ref) && !value.empty()) {
            identify_value_ = value[0];
            return atdecc::AEM_STATUS_SUCCESS;
        }
        return AemEntityHandler::on_set_descriptor_value(command_type, ref, symbol, value);
    }

    auto on_get_descriptor_value(uint16_t command_type, DescriptorRef ref, uint32_t symbol, std::span<uint8_t> out)
        -> size_t override
    {
        if (command_type == atdecc::AEM_COMMAND_GET_CONTROL && is_identify_control(ref) && !out.empty()) {
            out[0] = identify_value_;
            return 1;
        }
        return AemEntityHandler::on_get_descriptor_value(command_type, ref, symbol, out);
    }

    /// The stored identify value (0 = off, non-zero = identifying).
    [[nodiscard]] auto identify_value() const noexcept -> uint8_t { return identify_value_; }

    /// Access the underlying storage (useful for derived handlers that
    /// want to look up additional blobs beyond descriptors).
    [[nodiscard]] auto storage() const noexcept -> DescriptorStorage const& { return storage_; }

  private:
    /// True if @p ref names a CONTROL descriptor whose control_type (the
    /// EUI-64 at offset 82) is the standard IDENTIFY type.
    [[nodiscard]] auto is_identify_control(DescriptorRef ref) const noexcept -> bool
    {
        if (ref.descriptor_type != atdecc::aem::DESCRIPTOR_CONTROL) {
            return false;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorControl::LENGTH) {
            return false;
        }
        ieee::Eui64 control_type{};
        span_load(control_type, blob->subspan(82));
        return control_type == atdecc::aem::CONTROL_TYPE_IDENTIFY;
    }

    uint8_t identify_value_{0};

    /// Load the descriptor bytes for `ref` into `desc` via
    /// span_load_padded. Returns false if the storage doesn't have a
    /// descriptor for `ref`. Used by every on_get_* override.
    template <typename T>
    auto load(DescriptorRef ref, T& desc) const noexcept -> bool
    {
        auto blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value()) {
            return false;
        }
        span_load_padded(desc, *blob);
        return true;
    }

    /// True if @p ref names the entity's single entity_name (ENTITY descriptor,
    /// descriptor_index 0, name_index 0) and built-in management is enabled.
    [[nodiscard]] auto manages_entity_name_field(NameRef ref) const noexcept -> bool
    {
        return manage_entity_name_ && ref.descriptor.descriptor_type == atdecc::aem::DESCRIPTOR_ENTITY &&
            ref.descriptor.descriptor_index == 0 && ref.name_index == 0;
    }

    DescriptorStorage storage_;
    AtdeccString entity_name_{};
    bool manage_entity_name_{false};
    statusbar::sg14::inplace_function<uint8_t(AtdeccString const&), 64> on_entity_name_changed_{};
};

}  // namespace statusbar::nanoavb
