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
#include "statusbar/sg14/inplace_vector.h"

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

    auto on_get_entity(DescriptorId id, DescriptorEntity& desc) -> bool override
    {
        if (!load(id.ref, desc)) {
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

    auto on_get_name(DescriptorId id, uint16_t const name_index) const -> std::optional<AtdeccString> override
    {
        if (manages_entity_name_field(id.ref, name_index)) {
            return entity_name_;
        }
        return std::nullopt;
    }

    auto on_set_name(DescriptorId id, uint16_t const name_index, AtdeccString const& name) -> uint8_t override
    {
        if (!manages_entity_name_field(id.ref, name_index)) {
            return atdecc::AEM_STATUS_NOT_IMPLEMENTED;
        }
        uint8_t const status = on_entity_name_changed_ ? on_entity_name_changed_(name) : atdecc::AEM_STATUS_SUCCESS;
        if (status == atdecc::AEM_STATUS_SUCCESS) {
            entity_name_ = name;
        }
        return status;
    }

    auto on_get_configuration(DescriptorId id, DescriptorConfiguration& desc) -> bool override { return load(id.ref, desc); }

    // ---- Unit descriptors -----------------------------------------------

    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_video_unit(DescriptorId id, DescriptorVideoUnit& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_sensor_unit(DescriptorId id, DescriptorSensorUnit& desc) -> bool override { return load(id.ref, desc); }

    // ---- Streams / Jacks / Interface / Clock ----------------------------

    auto on_get_stream(DescriptorId id, DescriptorStream& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_jack(DescriptorId id, DescriptorJack& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_avb_interface(DescriptorId id, DescriptorAvbInterface& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_clock_source(DescriptorId id, DescriptorClockSource& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_clock_domain(DescriptorId id, DescriptorClockDomain& desc) -> bool override { return load(id.ref, desc); }

    // ---- Memory / Locale / Strings --------------------------------------

    auto on_get_memory_object(DescriptorId id, DescriptorMemoryObject& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_locale(DescriptorId id, DescriptorLocale& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_strings(DescriptorId id, DescriptorStrings& desc) -> bool override { return load(id.ref, desc); }

    // ---- Ports ----------------------------------------------------------

    auto on_get_stream_port(DescriptorId id, DescriptorStreamPort& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_external_port(DescriptorId id, DescriptorExternalPort& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_internal_port(DescriptorId id, DescriptorInternalPort& desc) -> bool override { return load(id.ref, desc); }

    // ---- Clusters / Maps / Controls -------------------------------------

    auto on_get_audio_cluster(DescriptorId id, DescriptorAudioCluster& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_video_cluster(DescriptorId id, DescriptorVideoCluster& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_sensor_cluster(DescriptorId id, DescriptorSensorCluster& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_audio_map(DescriptorId id, DescriptorAudioMap& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_video_map(DescriptorId id, DescriptorVideoMap& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_sensor_map(DescriptorId id, DescriptorSensorMap& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_control(DescriptorId id, DescriptorControl& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_control_block(DescriptorId id, DescriptorControlBlock& desc) -> bool override { return load(id.ref, desc); }

    // ---- Signal routing -------------------------------------------------

    auto on_get_signal_selector(DescriptorId id, DescriptorSignalSelector& desc) -> bool override
    {
        if (!load(id.ref, desc)) {
            return false;
        }
        // READ_DESCRIPTOR agrees with GET_SIGNAL_SELECTOR: reflect a runtime
        // selection over the blob's authored current_signal_* fields.
        if (auto const* state = find_selector_state(id.ref.descriptor_index)) {
            desc.current_signal_type = state->source.signal_type;
            desc.current_signal_index = state->source.signal_index;
            desc.current_signal_output = state->source.signal_output;
        }
        return true;
    }

    auto on_get_mixer(DescriptorId id, DescriptorMixer& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_matrix(DescriptorId id, DescriptorMatrix& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_matrix_signal(DescriptorId id, DescriptorMatrixSignal& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_signal_splitter(DescriptorId id, DescriptorSignalSplitter& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_signal_combiner(DescriptorId id, DescriptorSignalCombiner& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_signal_demultiplexer(DescriptorId id, DescriptorSignalDemultiplexer& desc) -> bool override
    {
        return load(id.ref, desc);
    }

    auto on_get_signal_multiplexer(DescriptorId id, DescriptorSignalMultiplexer& desc) -> bool override
    {
        return load(id.ref, desc);
    }

    auto on_get_signal_transcoder(DescriptorId id, DescriptorSignalTranscoder& desc) -> bool override { return load(id.ref, desc); }

    // ---- Timing / PTP ---------------------------------------------------

    auto on_get_timing(DescriptorId id, DescriptorTiming& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_ptp_instance(DescriptorId id, DescriptorPtpInstance& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_ptp_port(DescriptorId id, DescriptorPtpPort& desc) -> bool override { return load(id.ref, desc); }

    // ---- Built-in IDENTIFY control (automatic when the blob has one) ------
    //
    // When the blob contains a CONTROL descriptor with the standard IDENTIFY
    // control_type, this handler accepts SET_CONTROL (stores the value) and
    // serves GET_CONTROL for it, so a blob-backed entity honors identify with
    // no per-entity code. The externally visible indicator is the
    // AemCommandHandler's identify_changed callback (fired on the successful
    // SET); the stored value keeps GET_CONTROL and polling controllers honest.

    auto on_set_descriptor_value(uint16_t command_type, DescriptorId id, std::span<uint8_t const> value) -> uint8_t override
    {
        if (command_type == atdecc::AEM_COMMAND_SET_CONTROL && is_identify_control(id.ref) && !value.empty()) {
            identify_value_ = value[0];
            return atdecc::AEM_STATUS_SUCCESS;
        }
        if (command_type == atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR &&
            id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR) {
            return set_signal_selector(id.ref, value);
        }
        return AemEntityHandler::on_set_descriptor_value(command_type, id, value);
    }

    auto on_get_descriptor_value(uint16_t command_type, DescriptorId id, std::span<uint8_t> out) -> size_t override
    {
        if (command_type == atdecc::AEM_COMMAND_GET_CONTROL && is_identify_control(id.ref) && !out.empty()) {
            out[0] = identify_value_;
            return 1;
        }
        if (command_type == atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR &&
            id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR && out.size() >= SELECTOR_VALUE_WIRE_SIZE) {
            if (auto const current = current_selector_source(id.ref)) {
                store_signal_source(*current, out);
                out[6] = 0;  // reserved doublet completing the
                out[7] = 0;  // AemSignalSelectorPayload quadlet row
                return SELECTOR_VALUE_WIRE_SIZE;
            }
            return 0;  // no such selector in the blob
        }
        return AemEntityHandler::on_get_descriptor_value(command_type, id, out);
    }

    /// The stored identify value (0 = off, non-zero = identifying).
    [[nodiscard]] auto identify_value() const noexcept -> uint8_t { return identify_value_; }

    // ---- Built-in SIGNAL_SELECTOR (automatic when the blob has one) --------
    //
    // SET_SIGNAL_SELECTOR is accepted when the requested source is one of the
    // descriptor's authored sources; the current selection lives in RAM
    // (keyed by descriptor index) and GET_SIGNAL_SELECTOR / READ_DESCRIPTOR
    // reflect it. The change callback is where the application actually
    // reroutes its signal path; returning any status other than SUCCESS
    // rejects the change and keeps the previous selection.

    /// One {signal_type, signal_index, signal_output} source triple.
    struct SignalSourceRef
    {
        uint16_t signal_type{0};
        uint16_t signal_index{0};
        uint16_t signal_output{0};

        auto operator==(SignalSourceRef const&) const noexcept -> bool = default;
    };

    /// Called when SET_SIGNAL_SELECTOR requests a new source (already
    /// validated against the descriptor's sources list). Return
    /// AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to reject.
    /// Unset => accept (in-memory only).
    void set_on_signal_selector_changed(
        statusbar::sg14::inplace_function<uint8_t(uint16_t /*descriptor_index*/, SignalSourceRef const&), 64> fn)
    {
        on_signal_selector_changed_ = std::move(fn);
    }

    /// The selector's current source: the runtime selection when one was
    /// made, otherwise the blob's authored current_signal_* fields.
    /// nullopt when the blob has no such SIGNAL_SELECTOR descriptor.
    [[nodiscard]] auto current_selector_source(DescriptorRef ref) const noexcept -> std::optional<SignalSourceRef>
    {
        if (auto const* state = find_selector_state(ref.descriptor_index)) {
            return state->source;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorSignalSelector::LENGTH) {
            return std::nullopt;
        }
        return load_signal_source(blob->subspan(CURRENT_SIGNAL_OFFSET));
    }

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

    // SIGNAL_SELECTOR wire geometry (DescriptorSignalSelector field offsets
    // and the 6-byte source triple).
    static constexpr size_t SIGNAL_SOURCE_WIRE_SIZE = 6;
    static constexpr size_t SELECTOR_VALUE_WIRE_SIZE = 8;  // triple + reserved doublet
    static constexpr size_t SOURCES_OFFSET_FIELD = 80;
    static constexpr size_t NUMBER_OF_SOURCES_FIELD = 82;
    static constexpr size_t CURRENT_SIGNAL_OFFSET = 84;

    /// A runtime signal-selector selection (descriptor index -> source).
    struct SelectorState
    {
        uint16_t descriptor_index{0};
        SignalSourceRef source{};
    };

    static constexpr size_t MAX_SELECTOR_STATES = 8;

    [[nodiscard]] auto find_selector_state(uint16_t const descriptor_index) const noexcept -> SelectorState const*
    {
        for (auto const& s : selector_states_) {
            if (s.descriptor_index == descriptor_index) {
                return &s;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static auto load_signal_source(std::span<uint8_t const> bytes) noexcept -> SignalSourceRef
    {
        atdecc::doublet_t t{};
        atdecc::doublet_t i{};
        atdecc::doublet_t o{};
        span_load(t, bytes.subspan(0, 2));
        span_load(i, bytes.subspan(2, 2));
        span_load(o, bytes.subspan(4, 2));
        return SignalSourceRef{.signal_type = t.get(), .signal_index = i.get(), .signal_output = o.get()};
    }

    static void store_signal_source(SignalSourceRef const& src, std::span<uint8_t> out) noexcept
    {
        atdecc::doublet_t const t{src.signal_type};
        atdecc::doublet_t const i{src.signal_index};
        atdecc::doublet_t const o{src.signal_output};
        span_store(out.subspan(0, 2), t);
        span_store(out.subspan(2, 2), i);
        span_store(out.subspan(4, 2), o);
    }

    /// Apply a SET_SIGNAL_SELECTOR value ({signal_type, signal_index,
    /// signal_output}) to the selector at @p ref.
    [[nodiscard]] auto set_signal_selector(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        if (value.size() < SIGNAL_SOURCE_WIRE_SIZE) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorSignalSelector::LENGTH) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        auto const requested = load_signal_source(value);

        // The requested source must be one of the descriptor's authored sources.
        atdecc::doublet_t sources_offset{};
        atdecc::doublet_t number_of_sources{};
        span_load(sources_offset, blob->subspan(SOURCES_OFFSET_FIELD, 2));
        span_load(number_of_sources, blob->subspan(NUMBER_OF_SOURCES_FIELD, 2));
        bool valid = false;
        for (uint16_t n = 0; n < number_of_sources.get(); ++n) {
            size_t const off = sources_offset.get() + (size_t{n} * SIGNAL_SOURCE_WIRE_SIZE);
            if (off + SIGNAL_SOURCE_WIRE_SIZE > blob->size()) {
                break;
            }
            if (load_signal_source(blob->subspan(off)) == requested) {
                valid = true;
                break;
            }
        }
        if (!valid) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }

        if (on_signal_selector_changed_) {
            if (auto const status = on_signal_selector_changed_(ref.descriptor_index, requested);
                status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }

        for (auto& s : selector_states_) {
            if (s.descriptor_index == ref.descriptor_index) {
                s.source = requested;
                return atdecc::AEM_STATUS_SUCCESS;
            }
        }
        if (selector_states_.try_push_back(SelectorState{.descriptor_index = ref.descriptor_index, .source = requested}) ==
            nullptr) {
            return atdecc::AEM_STATUS_NO_RESOURCES;
        }
        return atdecc::AEM_STATUS_SUCCESS;
    }

    statusbar::sg14::inplace_vector<SelectorState, MAX_SELECTOR_STATES> selector_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, SignalSourceRef const&), 64> on_signal_selector_changed_{};

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
    [[nodiscard]] auto manages_entity_name_field(DescriptorRef const ref, uint16_t const name_index) const noexcept -> bool
    {
        return manage_entity_name_ && ref.descriptor_type == atdecc::aem::DESCRIPTOR_ENTITY && ref.descriptor_index == 0 &&
            name_index == 0;
    }

    DescriptorStorage storage_;
    AtdeccString entity_name_{};
    bool manage_entity_name_{false};
    statusbar::sg14::inplace_function<uint8_t(AtdeccString const&), 64> on_entity_name_changed_{};
};

}  // namespace statusbar::nanoavb
