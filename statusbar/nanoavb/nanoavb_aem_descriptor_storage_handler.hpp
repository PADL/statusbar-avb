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
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace statusbar::nanoavb {

using atdecc::aem::DescriptorStorage;
using atdecc::aem::SignalSource;

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

    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override
    {
        if (!load(id.ref, desc)) {
            return false;
        }
        // READ_DESCRIPTOR agrees with GET_SAMPLING_RATE: reflect a runtime
        // sampling-rate selection over the blob's authored current rate.
        if (auto const* state = find_sampling_rate_state(id.ref.descriptor_index)) {
            desc.current_sampling_rate = state->rate;
        }
        return true;
    }

    auto on_get_video_unit(DescriptorId id, DescriptorVideoUnit& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_sensor_unit(DescriptorId id, DescriptorSensorUnit& desc) -> bool override { return load(id.ref, desc); }

    // ---- Streams / Jacks / Interface / Clock ----------------------------

    auto on_get_stream(DescriptorId id, DescriptorStream& desc) -> bool override
    {
        if (!load(id.ref, desc)) {
            return false;
        }
        // READ_DESCRIPTOR agrees with GET_STREAM_FORMAT: reflect a runtime
        // SET_STREAM_FORMAT over the blob's authored current_format.
        if (auto const* state = find_stream_state(id.ref.descriptor_type, id.ref.descriptor_index);
            state != nullptr && state->has_format) {
            desc.current_format = state->format;
        }
        return true;
    }

    auto on_get_jack(DescriptorId id, DescriptorJack& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_avb_interface(DescriptorId id, DescriptorAvbInterface& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_clock_source(DescriptorId id, DescriptorClockSource& desc) -> bool override { return load(id.ref, desc); }

    auto on_get_clock_domain(DescriptorId id, DescriptorClockDomain& desc) -> bool override
    {
        if (!load(id.ref, desc)) {
            return false;
        }
        // READ_DESCRIPTOR agrees with GET_CLOCK_SOURCE: reflect a runtime
        // clock-source selection over the blob's authored clock_source_index.
        if (auto const* state = find_clock_domain_state(id.ref.descriptor_index)) {
            desc.clock_source_index = state->clock_source_index;
        }
        return true;
    }

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
        // selection over the blob's authored current_signal triple.
        if (auto const* state = find_selector_state(id.ref.descriptor_index)) {
            desc.current_signal = state->source;
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
        if (command_type == atdecc::AEM_COMMAND_SET_CONTROL && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL) {
            // Generic CONTROL built-in (kit phase 4): validate, offer to the
            // change callback, store. IDENTIFY additionally mirrors into
            // identify_value_ (the identify_changed indicator + accessor).
            auto const status = set_control(id.ref, value);
            if (status == atdecc::AEM_STATUS_SUCCESS && is_identify_control(id.ref) && !value.empty()) {
                identify_value_ = value[0];
            }
            return status;
        }
        if (command_type == atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR &&
            id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR) {
            return set_signal_selector(id.ref, value);
        }
        if (command_type == atdecc::AEM_COMMAND_SET_MATRIX && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_MATRIX) {
            return set_matrix(id.ref, value);
        }
        if (command_type == atdecc::AEM_COMMAND_SET_MIXER && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_MIXER) {
            return set_mixer(id.ref, value);
        }
        if (command_type == atdecc::AEM_COMMAND_SET_CLOCK_SOURCE &&
            id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN) {
            return set_clock_source(id.ref, value);
        }
        if (command_type == atdecc::AEM_COMMAND_SET_STREAM_FORMAT && is_stream_descriptor(id.ref.descriptor_type)) {
            return set_stream_format(id.ref, value);
        }
        if (command_type == atdecc::AEM_COMMAND_SET_SAMPLING_RATE && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_AUDIO_UNIT) {
            return set_sampling_rate(id.ref, value);
        }
        if ((command_type == atdecc::AEM_COMMAND_START_STREAMING || command_type == atdecc::AEM_COMMAND_STOP_STREAMING) &&
            is_stream_descriptor(id.ref.descriptor_type)) {
            return set_streaming(id.ref, command_type == atdecc::AEM_COMMAND_START_STREAMING);
        }
        return AemEntityHandler::on_set_descriptor_value(command_type, id, value);
    }

    auto on_get_descriptor_value(uint16_t command_type, DescriptorId id, std::span<uint8_t const> request, std::span<uint8_t> out)
        -> size_t override
    {
        if (command_type == atdecc::AEM_COMMAND_GET_CONTROL && is_identify_control(id.ref) && !out.empty()) {
            out[0] = identify_value_;
            return 1;
        }
        if (command_type == atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR &&
            id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR && out.size() >= SELECTOR_VALUE_WIRE_SIZE) {
            if (auto const current = current_selector_source(id.ref)) {
                // The source triple + reserved tail of AemSignalSelectorPayload
                // (the generic framing supplies the descriptor type/index head).
                span_store(out.first(SignalSource::LENGTH), *current);
                span_store(out.subspan(SignalSource::LENGTH, 2), atdecc::doublet_t{0});
                return SELECTOR_VALUE_WIRE_SIZE;
            }
            return 0;  // no such selector in the blob
        }
        if (command_type == atdecc::AEM_COMMAND_GET_MATRIX && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_MATRIX) {
            return get_matrix(id.ref, request, out);
        }
        if (command_type == atdecc::AEM_COMMAND_GET_MIXER && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_MIXER) {
            return get_mixer(id.ref, out);
        }
        if (command_type == atdecc::AEM_COMMAND_GET_CLOCK_SOURCE &&
            id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN && out.size() >= CLOCK_SOURCE_VALUE_WIRE_SIZE) {
            if (auto const current = current_clock_source(id.ref)) {
                // The clock_source_index + reserved tail of AemClockSourcePayload
                // (the generic framing supplies the descriptor type/index head).
                atdecc::doublet_t const cs{*current};
                atdecc::doublet_t const reserved{0};
                span_store(out.subspan(0, 2), cs);
                span_store(out.subspan(2, 2), reserved);
                return CLOCK_SOURCE_VALUE_WIRE_SIZE;
            }
            return 0;  // no such CLOCK_DOMAIN in the blob
        }
        if (command_type == atdecc::AEM_COMMAND_GET_CONTROL && id.ref.descriptor_type == atdecc::aem::DESCRIPTOR_CONTROL) {
            return get_control(id.ref, out);  // generic CONTROL built-in (kit phase 4)
        }
        return AemEntityHandler::on_get_descriptor_value(command_type, id, request, out);
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

    /// Called when SET_SIGNAL_SELECTOR requests a new source (already
    /// validated against the descriptor's sources list). Return
    /// AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to reject.
    /// Unset => accept (in-memory only).
    void set_on_signal_selector_changed(
        statusbar::sg14::inplace_function<uint8_t(uint16_t /*descriptor_index*/, SignalSource const&), 64> fn)
    {
        on_signal_selector_changed_ = std::move(fn);
    }

    /// The selector's current source: the runtime selection when one was
    /// made, otherwise the blob's authored current_signal triple.
    /// nullopt when the blob has no such SIGNAL_SELECTOR descriptor.
    [[nodiscard]] auto current_selector_source(DescriptorRef ref) const noexcept -> std::optional<SignalSource>
    {
        if (auto const* state = find_selector_state(ref.descriptor_index)) {
            return state->source;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorSignalSelector::LENGTH) {
            return std::nullopt;
        }
        SignalSource current{};
        span_load(current, blob->subspan(CURRENT_SIGNAL_OFFSET, SignalSource::LENGTH));
        return current;
    }

    // ---- Built-in CLOCK_SOURCE selection (automatic; kit phase 3) ----------
    //
    // SET_CLOCK_SOURCE is accepted when the requested clock_source_index is
    // one of the CLOCK_DOMAIN's authored clock_sources; the current selection
    // lives in RAM (keyed by descriptor index) and GET_CLOCK_SOURCE /
    // READ_DESCRIPTOR reflect it. The change callback is where the
    // application actually re-clocks (e.g. swap the media clock's rate source
    // to a CRF recovery); returning any status other than SUCCESS rejects the
    // change and keeps the previous selection.

    /// Called when SET_CLOCK_SOURCE requests a new source (already validated
    /// against the CLOCK_DOMAIN's clock_sources list). Return
    /// AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to reject.
    /// Unset => accept (in-memory only).
    void set_on_clock_source_changed(
        statusbar::sg14::inplace_function<uint8_t(uint16_t /*clock_domain_index*/, uint16_t /*clock_source_index*/), 64> fn)
    {
        on_clock_source_changed_ = std::move(fn);
    }

    /// The CLOCK_DOMAIN's current clock source index: the runtime selection
    /// when one was made, otherwise the blob's authored clock_source_index.
    /// nullopt when the blob has no such CLOCK_DOMAIN descriptor.
    [[nodiscard]] auto current_clock_source(DescriptorRef ref) const noexcept -> std::optional<uint16_t>
    {
        if (auto const* state = find_clock_domain_state(ref.descriptor_index)) {
            return state->clock_source_index;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorClockDomain::LENGTH) {
            return std::nullopt;
        }
        atdecc::doublet_t cs{};
        span_load(cs, blob->subspan(CLOCK_DOMAIN_SOURCE_INDEX_FIELD, 2));
        return cs.get();
    }

    // ---- Built-in generic CONTROL values (automatic; kit phase 4) ----------
    //
    // ANY CONTROL descriptor in the blob gets SET_CONTROL / GET_CONTROL
    // handling out of the box (previously only IDENTIFY): the SET payload
    // (the control's CURRENT values) is size-validated against the
    // descriptor's value type and count, stored in RAM keyed by descriptor
    // index, and served by GET_CONTROL / a controller poll; before any SET,
    // GET falls back to the current values authored in the blob's
    // value_details. The change callback is the veto/apply hook where the
    // application consumes the value (e.g. a gain into its DSP); returning
    // any status other than SUCCESS rejects and keeps the previous value.

    /// Called when SET_CONTROL delivers a new value payload for a
    /// non-IDENTIFY CONTROL (size already validated for linear types).
    /// Return AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to
    /// reject. Unset => accept (in-memory only).
    void set_on_control_changed(
        statusbar::sg14::inplace_function<uint8_t(uint16_t /*control_index*/, std::span<uint8_t const> /*value*/), 64> fn)
    {
        on_control_changed_ = std::move(fn);
    }

    // ---- Built-in MATRIX (automatic when the blob has one) -----------------
    //
    // SET_MATRIX applies region writes (rep / direction / value_count /
    // item_offset per Clause 7.4.33) to an in-RAM width x height value grid
    // seeded from the descriptor's authored current value; GET_MATRIX reads
    // regions back. Only linear control_value_types are supported, and the
    // grid must fit MAX_MATRIX_VALUE_BYTES. The change callback receives the
    // full pending MatrixWrite (region + incoming values) before the grid is
    // touched; returning any status other than SUCCESS rejects the write.

    /// A rectangular subregion of a matrix (columns x rows).
    struct MatrixRegion
    {
        uint16_t column{0};
        uint16_t row{0};
        uint16_t width{0};
        uint16_t height{0};
    };

    /// One pending SET_MATRIX region write, handed to the change callback
    /// BEFORE the grid is modified so it can be vetoed. The incoming
    /// values ride along (the grid still holds the old values during the
    /// callback; matrix_cell() reflects the new state only after the SET
    /// completes).
    struct MatrixWrite
    {
        MatrixRegion region{};
        uint16_t direction{0};              ///< Table 7-146: 0 horizontal, 1 vertical
        uint16_t item_offset{0};            ///< cells skipped before applying, in `direction` order
        uint16_t value_count{0};            ///< elements in `values`
        bool rep{false};                    ///< values repeat to fill the region
        uint8_t elem_size{0};               ///< bytes per matrix point value
        std::span<uint8_t const> values{};  ///< value_count * elem_size bytes (valid only during the call)
    };

    /// Called when SET_MATRIX requests a region write (already validated
    /// against the descriptor's dimensions) — this is where the
    /// application applies the incoming crosspoint values to its DSP.
    /// Return AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to
    /// reject (the grid is then left untouched).
    void set_on_matrix_changed(statusbar::sg14::inplace_function<uint8_t(uint16_t /*descriptor_index*/, MatrixWrite const&), 64> fn)
    {
        on_matrix_changed_ = std::move(fn);
    }

    /// Read one matrix cell's current value bytes (element size = the
    /// matrix's control_value_type element). Returns the number of bytes
    /// written to @p out (0 if the matrix/cell is unknown or out is small).
    [[nodiscard]] auto matrix_cell(DescriptorRef ref, uint16_t const column, uint16_t const row, std::span<uint8_t> out) noexcept
        -> size_t
    {
        auto* const state = find_or_init_matrix_state(ref);
        if (state == nullptr || column >= state->width || row >= state->height) {
            return 0;
        }
        size_t const elem = state->elem_size;
        size_t const off = ((size_t{row} * state->width) + column) * elem;
        if (out.size() < elem) {
            return 0;
        }
        span_copy(out.first(elem), make_const_span(state->values, {.start = off, .length = elem}));
        return elem;
    }

    // ---- Built-in MIXER (automatic when the blob has one) ------------------
    //
    // A MIXER carries ONE control value (of its linear control_value_type)
    // governing the mix of its sources. SET_MIXER validates the incoming
    // value size, offers it to the change callback, and stores it in RAM
    // (seeded from the descriptor's authored `current` field); GET_MIXER
    // serves the stored value back.

    /// Called when SET_MIXER carries a size-valid value for a MIXER the
    /// blob authors — this is where the application applies the new mix
    /// value to its DSP. Return AEM_STATUS_SUCCESS to accept, any other
    /// AEM_STATUS_* to reject (the stored value is then left untouched).
    void set_on_mixer_changed(
        statusbar::sg14::inplace_function<uint8_t(uint16_t /*descriptor_index*/, std::span<uint8_t const> /*value*/), 64> fn)
    {
        on_mixer_changed_ = std::move(fn);
    }

    /// Read the mixer's current value bytes (element size = the mixer's
    /// control_value_type element). Returns the number of bytes written
    /// to @p out (0 if the mixer is unknown or out is small).
    [[nodiscard]] auto mixer_value(DescriptorRef ref, std::span<uint8_t> out) noexcept -> size_t
    {
        auto* const state = find_or_init_mixer_state(ref);
        if (state == nullptr || out.size() < state->elem_size) {
            return 0;
        }
        span_copy(out.first(state->elem_size), make_const_span(state->value, {.start = 0, .length = state->elem_size}));
        return state->elem_size;
    }

    // ---- Built-in STREAM lifecycle (kit phase 5) ---------------------------
    //
    // SET_STREAM_FORMAT validates the requested format against the STREAM
    // descriptor's authored formats (current_format + the stream_formats
    // trailer), offers it to a veto/apply callback, and stores it in RAM;
    // GET_STREAM_FORMAT and READ_DESCRIPTOR then serve the runtime value.
    // START/STOP_STREAMING latch a per-stream streaming/stopped state the
    // data plane can honor via the change callback. SET_SAMPLING_RATE does
    // the same for AUDIO_UNIT against its authored sampling_rates.

    /// Called when SET_STREAM_FORMAT carries a supported format — this is
    /// where the application re-configures its serializers. Return
    /// AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to reject.
    void set_on_stream_format_changed(statusbar::sg14::inplace_function<
                                      uint8_t(uint16_t /*descriptor_type*/, uint16_t /*descriptor_index*/, uint64_t /*format*/),
                                      64> fn)
    {
        on_stream_format_changed_ = std::move(fn);
    }

    /// The stream's runtime current_format (falls back to the blob's
    /// authored current_format; nullopt if the blob has no such stream).
    [[nodiscard]] auto current_stream_format(DescriptorRef ref) noexcept -> std::optional<ieee::Eui64>
    {
        if (auto const* state = find_stream_state(ref.descriptor_type, ref.descriptor_index);
            state != nullptr && state->has_format) {
            return state->format;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < STREAM_CURRENT_FORMAT_FIELD + 8) {
            return std::nullopt;
        }
        ieee::Eui64 authored{};
        span_load(authored, blob->subspan(STREAM_CURRENT_FORMAT_FIELD, 8));
        return authored;
    }

    /// Called when START/STOP_STREAMING flips a stream's streaming state —
    /// this is where the application gates its transmitter. Return
    /// AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to reject.
    void set_on_streaming_changed(statusbar::sg14::inplace_function<
                                  uint8_t(uint16_t /*descriptor_type*/, uint16_t /*descriptor_index*/, bool /*streaming*/),
                                  64> fn)
    {
        on_streaming_changed_ = std::move(fn);
    }

    /// False only after a STOP_STREAMING on this stream (streams default
    /// to streaming; unknown streams report streaming too).
    [[nodiscard]] auto is_streaming(uint16_t const descriptor_type, uint16_t const descriptor_index) const noexcept -> bool
    {
        auto const* state = find_stream_state(descriptor_type, descriptor_index);
        return state == nullptr || !state->stopped;
    }

    /// Called when SET_SAMPLING_RATE carries an authored rate for an
    /// AUDIO_UNIT — this is where the application re-clocks. Return
    /// AEM_STATUS_SUCCESS to accept, any other AEM_STATUS_* to reject.
    void set_on_sampling_rate_changed(
        statusbar::sg14::inplace_function<uint8_t(uint16_t /*audio_unit_index*/, uint32_t /*rate*/), 64> fn)
    {
        on_sampling_rate_changed_ = std::move(fn);
    }

    /// The audio unit's runtime sampling rate (falls back to the blob's
    /// authored current_sampling_rate; nullopt if no such AUDIO_UNIT).
    [[nodiscard]] auto current_sampling_rate(DescriptorRef ref) noexcept -> std::optional<uint32_t>
    {
        if (auto const* state = find_sampling_rate_state(ref.descriptor_index)) {
            return state->rate;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < AUDIO_UNIT_CURRENT_RATE_FIELD + 4) {
            return std::nullopt;
        }
        ieee::quadlet_t rate{0};
        span_load(rate, blob->subspan(AUDIO_UNIT_CURRENT_RATE_FIELD, 4));
        return rate.get();
    }

    /// Access the underlying storage (useful for derived handlers that
    /// want to look up additional blobs beyond descriptors).
    [[nodiscard]] auto storage() const noexcept -> DescriptorStorage const& { return storage_; }

  private:
    // ---- Shared wire / state-table helpers --------------------------------

    [[nodiscard]] static auto read_u16(std::span<uint8_t const> const bytes, size_t const off) noexcept -> uint16_t
    {
        atdecc::doublet_t v{};
        span_load(v, bytes.subspan(off, 2));
        return v.get();
    }

    static void write_u16(std::span<uint8_t> const bytes, size_t const off, uint16_t const value) noexcept
    {
        atdecc::doublet_t const v{value};
        span_store(bytes.subspan(off, 2), v);
    }

    /// True when @p wanted appears in the blob's authored list of T
    /// entries; the list's byte offset and entry count are read from the
    /// doublet fields at @p offset_field / @p count_field. Entries that
    /// would run past the blob's end are ignored.
    template <typename T>
    [[nodiscard]] static auto blob_list_contains(
        std::span<uint8_t const> const blob, size_t const offset_field, size_t const count_field, T const& wanted) noexcept -> bool
    {
        size_t const offset = read_u16(blob, offset_field);
        size_t const count = read_u16(blob, count_field);
        for (size_t i = 0; i < count; ++i) {
            size_t const off = offset + (i * sizeof(T));
            if (off + sizeof(T) > blob.size()) {
                break;
            }
            T candidate{};
            span_load(candidate, blob.subspan(off, sizeof(T)));
            if (candidate == wanted) {
                return true;
            }
        }
        return false;
    }

    /// The first entry in the runtime state table matching @p pred, or
    /// nullptr.
    template <typename Table, typename Pred>
    [[nodiscard]] static auto find_state(Table& states, Pred const pred) noexcept -> decltype(states.data())
    {
        for (auto& s : states) {
            if (pred(s)) {
                return &s;
            }
        }
        return nullptr;
    }

    /// The entry matching @p pred, appending @p fresh when absent.
    /// nullptr when the table is full.
    template <typename Table, typename Pred>
    [[nodiscard]] static auto find_or_push_state(Table& states, Pred const pred, typename Table::value_type const& fresh) noexcept
        -> decltype(states.data())
    {
        if (auto* const s = find_state(states, pred)) {
            return s;
        }
        return states.try_push_back(fresh);
    }

    /// Predicate matching a runtime state entry by its descriptor_index.
    [[nodiscard]] static constexpr auto by_index(uint16_t const descriptor_index) noexcept
    {
        return [descriptor_index](auto const& s) noexcept { return s.descriptor_index == descriptor_index; };
    }

    /// Predicate matching a runtime state entry by descriptor type + index
    /// (streams come in INPUT and OUTPUT flavours sharing one table).
    [[nodiscard]] static constexpr auto by_stream(uint16_t const descriptor_type, uint16_t const descriptor_index) noexcept
    {
        return [descriptor_type, descriptor_index](auto const& s) noexcept {
            return s.descriptor_type == descriptor_type && s.descriptor_index == descriptor_index;
        };
    }

    /// True if @p ref names a CONTROL descriptor whose control_type EUI-64
    /// is the standard IDENTIFY type.
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
        span_load(control_type, blob->subspan(CONTROL_TYPE_FIELD, sizeof(ieee::Eui64)));
        return control_type == atdecc::aem::CONTROL_TYPE_IDENTIFY;
    }

    uint8_t identify_value_{0};

    // SIGNAL_SELECTOR wire geometry, derived from the shared wire structs.
    static constexpr size_t SELECTOR_VALUE_WIRE_SIZE = atdecc::aem::AemSignalSelectorPayload::LENGTH -
        offsetof(atdecc::aem::AemSignalSelectorPayload, source);  // triple + reserved doublet
    static constexpr size_t SOURCES_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorSignalSelector, sources_offset);
    static constexpr size_t NUMBER_OF_SOURCES_FIELD = offsetof(atdecc::aem::DescriptorSignalSelector, number_of_sources);
    static constexpr size_t CURRENT_SIGNAL_OFFSET = offsetof(atdecc::aem::DescriptorSignalSelector, current_signal);

    /// A runtime signal-selector selection (descriptor index -> source).
    struct SelectorState
    {
        uint16_t descriptor_index{0};
        SignalSource source{};
    };

    static constexpr size_t MAX_SELECTOR_STATES = 8;

    [[nodiscard]] auto find_selector_state(uint16_t const descriptor_index) const noexcept -> SelectorState const*
    {
        return find_state(selector_states_, by_index(descriptor_index));
    }

    /// Apply a SET_SIGNAL_SELECTOR value ({signal_type, signal_index,
    /// signal_output}) to the selector at @p ref.
    [[nodiscard]] auto set_signal_selector(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        if (value.size() < SignalSource::LENGTH) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorSignalSelector::LENGTH) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        SignalSource requested{};
        span_load(requested, value.first(SignalSource::LENGTH));

        // The requested source must be one of the descriptor's authored sources.
        if (!blob_list_contains(*blob, SOURCES_OFFSET_FIELD, NUMBER_OF_SOURCES_FIELD, requested)) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }

        if (on_signal_selector_changed_) {
            if (auto const status = on_signal_selector_changed_(ref.descriptor_index, requested);
                status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }

        auto* const state = find_or_push_state(
            selector_states_, by_index(ref.descriptor_index), SelectorState{.descriptor_index = ref.descriptor_index});
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_RESOURCES;
        }
        state->source = requested;
        return atdecc::AEM_STATUS_SUCCESS;
    }

    statusbar::sg14::inplace_vector<SelectorState, MAX_SELECTOR_STATES> selector_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, SignalSource const&), 64> on_signal_selector_changed_{};

    // CLOCK_SOURCE built-in machinery. DescriptorClockDomain wire offsets and
    // the SET/GET_CLOCK_SOURCE value shape (clock_source_index + reserved,
    // after the 4-byte descriptor header).
    static constexpr size_t CLOCK_DOMAIN_SOURCE_INDEX_FIELD = offsetof(atdecc::aem::DescriptorClockDomain, clock_source_index);
    static constexpr size_t CLOCK_DOMAIN_SOURCES_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorClockDomain, clock_sources_offset);
    static constexpr size_t CLOCK_DOMAIN_SOURCES_COUNT_FIELD = offsetof(atdecc::aem::DescriptorClockDomain, clock_sources_count);
    static constexpr size_t CLOCK_SOURCE_VALUE_WIRE_SIZE =
        atdecc::aem::AemClockSourcePayload::LENGTH - offsetof(atdecc::aem::AemClockSourcePayload, clock_source_index);
    static constexpr size_t MAX_CLOCK_DOMAIN_STATES = 4;

    /// The runtime clock-source selection for one CLOCK_DOMAIN descriptor.
    struct ClockDomainState
    {
        uint16_t descriptor_index{0};
        uint16_t clock_source_index{0};
    };

    [[nodiscard]] auto find_clock_domain_state(uint16_t const descriptor_index) const noexcept -> ClockDomainState const*
    {
        return find_state(clock_domain_states_, by_index(descriptor_index));
    }

    /// Apply a SET_CLOCK_SOURCE value (clock_source_index doublet) to the
    /// CLOCK_DOMAIN at @p ref.
    [[nodiscard]] auto set_clock_source(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        if (value.size() < 2) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorClockDomain::LENGTH) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        atdecc::doublet_t requested{};
        span_load(requested, value.subspan(0, 2));

        // The requested index must be one of the domain's authored clock sources.
        if (!blob_list_contains(*blob, CLOCK_DOMAIN_SOURCES_OFFSET_FIELD, CLOCK_DOMAIN_SOURCES_COUNT_FIELD, requested)) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }

        if (on_clock_source_changed_) {
            if (auto const status = on_clock_source_changed_(ref.descriptor_index, requested.get());
                status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }

        auto* const state = find_or_push_state(
            clock_domain_states_, by_index(ref.descriptor_index), ClockDomainState{.descriptor_index = ref.descriptor_index});
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_RESOURCES;
        }
        state->clock_source_index = requested.get();
        return atdecc::AEM_STATUS_SUCCESS;
    }

    statusbar::sg14::inplace_vector<ClockDomainState, MAX_CLOCK_DOMAIN_STATES> clock_domain_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, uint16_t), 64> on_clock_source_changed_{};

    // Generic CONTROL built-in machinery. DescriptorControl wire offsets
    // (same layout is_identify_control() reads) and the RAM value store.
    static constexpr size_t CONTROL_VALUE_TYPE_FIELD = offsetof(atdecc::aem::DescriptorControl, control_value_type);
    static constexpr size_t CONTROL_TYPE_FIELD = offsetof(atdecc::aem::DescriptorControl, control_type);
    static constexpr size_t CONTROL_VALUES_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorControl, values_offset);
    static constexpr size_t CONTROL_NUMBER_OF_VALUES_FIELD = offsetof(atdecc::aem::DescriptorControl, number_of_values);
    static constexpr size_t MAX_CONTROL_VALUE_BYTES = 64;
    static constexpr size_t MAX_CONTROL_STATES = 8;

    /// The stored current-values payload for one CONTROL descriptor.
    struct ControlState
    {
        uint16_t descriptor_index{0};
        statusbar::sg14::inplace_vector<uint8_t, MAX_CONTROL_VALUE_BYTES> value{};
    };

    [[nodiscard]] auto find_control_state(uint16_t const descriptor_index) const noexcept -> ControlState const*
    {
        return find_state(control_states_, by_index(descriptor_index));
    }

    /// The expected SET/GET_CONTROL value payload size for the CONTROL at
    /// @p blob: number_of_values x element size for linear types; nullopt for
    /// non-linear types (accepted un-validated up to the store's capacity).
    [[nodiscard]] static auto expected_control_value_size(std::span<uint8_t const> blob) noexcept -> std::optional<size_t>
    {
        atdecc::doublet_t vt{};
        atdecc::doublet_t n{};
        span_load(vt, blob.subspan(CONTROL_VALUE_TYPE_FIELD, 2));
        span_load(n, blob.subspan(CONTROL_NUMBER_OF_VALUES_FIELD, 2));
        uint16_t const base = static_cast<uint16_t>(vt.get() & atdecc::aem::CONTROL_VALUE_TYPE_MASK);
        if (!atdecc::aem::is_linear_value_type(base)) {
            return std::nullopt;
        }
        return static_cast<size_t>(n.get()) * atdecc::aem::control_value_element_size(base);
    }

    /// Apply a SET_CONTROL current-values payload to the CONTROL at @p ref.
    [[nodiscard]] auto set_control(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorControl::LENGTH) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        if (auto const expected = expected_control_value_size(*blob); expected.has_value() && value.size() != *expected) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        if (value.size() > MAX_CONTROL_VALUE_BYTES) {
            return atdecc::AEM_STATUS_NO_RESOURCES;
        }

        if (on_control_changed_) {
            if (auto const status = on_control_changed_(ref.descriptor_index, value); status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }

        auto* const state = find_or_push_state(
            control_states_, by_index(ref.descriptor_index), ControlState{.descriptor_index = ref.descriptor_index, .value = {}});
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_RESOURCES;
        }
        state->value.clear();
        for (auto const b : value) {
            state->value.push_back(b);
        }
        return atdecc::AEM_STATUS_SUCCESS;
    }

    /// Serve GET_CONTROL for the CONTROL at @p ref: the stored value when a
    /// SET happened, otherwise the CURRENT fields extracted from the blob's
    /// authored value_details (linear types). Returns bytes written (0 =>
    /// not implemented for this control).
    [[nodiscard]] auto get_control(DescriptorRef ref, std::span<uint8_t> out) const -> size_t
    {
        if (auto const* state = find_control_state(ref.descriptor_index)) {
            if (out.size() < state->value.size()) {
                return 0;
            }
            for (size_t i = 0; i < state->value.size(); ++i) {
                out[i] = state->value[i];
            }
            return state->value.size();
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorControl::LENGTH) {
            return 0;
        }
        atdecc::doublet_t vt{};
        atdecc::doublet_t n{};
        atdecc::doublet_t values_offset{};
        span_load(vt, blob->subspan(CONTROL_VALUE_TYPE_FIELD, 2));
        span_load(n, blob->subspan(CONTROL_NUMBER_OF_VALUES_FIELD, 2));
        span_load(values_offset, blob->subspan(CONTROL_VALUES_OFFSET_FIELD, 2));
        uint16_t const base = static_cast<uint16_t>(vt.get() & atdecc::aem::CONTROL_VALUE_TYPE_MASK);
        if (!atdecc::aem::is_linear_value_type(base)) {
            return 0;  // non-linear defaults need type-specific extraction
        }
        size_t const elem = atdecc::aem::control_value_element_size(base);
        size_t const item = atdecc::aem::linear_entry_size(elem);
        size_t const total = static_cast<size_t>(n.get()) * elem;
        if (out.size() < total) {
            return 0;
        }
        for (uint16_t i = 0; i < n.get(); ++i) {
            size_t const current_off = values_offset.get() + (size_t{i} * item) + atdecc::aem::linear_entry_current_offset(elem);
            if (current_off + elem > blob->size()) {
                return 0;
            }
            for (size_t b = 0; b < elem; ++b) {
                out[(size_t{i} * elem) + b] = (*blob)[current_off + b];
            }
        }
        return total;
    }

    statusbar::sg14::inplace_vector<ControlState, MAX_CONTROL_STATES> control_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, std::span<uint8_t const>), 64> on_control_changed_{};

    // MATRIX built-in machinery. DescriptorMatrix wire offsets and the
    // SET/GET_MATRIX region header (after the 4-byte descriptor header).
    static constexpr size_t MATRIX_CONTROL_VALUE_TYPE_FIELD = offsetof(atdecc::aem::DescriptorMatrix, control_value_type);
    static constexpr size_t MATRIX_WIDTH_FIELD = offsetof(atdecc::aem::DescriptorMatrix, width);
    static constexpr size_t MATRIX_HEIGHT_FIELD = offsetof(atdecc::aem::DescriptorMatrix, height);
    static constexpr size_t MATRIX_VALUES_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorMatrix, values_offset);
    // Region-header field offsets within the value span (the
    // AemMatrixPayloadHeader fields shifted by the 4-byte descriptor head
    // the generic framing strips).
    static constexpr size_t MATRIX_REGION_BASE = offsetof(atdecc::aem::AemMatrixPayloadHeader, matrix_column);
    static constexpr size_t REGION_COLUMN_FIELD = offsetof(atdecc::aem::AemMatrixPayloadHeader, matrix_column) - MATRIX_REGION_BASE;
    static constexpr size_t REGION_ROW_FIELD = offsetof(atdecc::aem::AemMatrixPayloadHeader, matrix_row) - MATRIX_REGION_BASE;
    static constexpr size_t REGION_WIDTH_FIELD = offsetof(atdecc::aem::AemMatrixPayloadHeader, region_width) - MATRIX_REGION_BASE;
    static constexpr size_t REGION_HEIGHT_FIELD = offsetof(atdecc::aem::AemMatrixPayloadHeader, region_height) - MATRIX_REGION_BASE;
    static constexpr size_t REGION_REP_DIR_COUNT_FIELD =
        offsetof(atdecc::aem::AemMatrixPayloadHeader, rep_direction_value_count) - MATRIX_REGION_BASE;
    static constexpr size_t REGION_ITEM_OFFSET_FIELD =
        offsetof(atdecc::aem::AemMatrixPayloadHeader, item_offset) - MATRIX_REGION_BASE;
    static constexpr size_t MATRIX_REGION_HEADER_SIZE = atdecc::aem::AemMatrixPayloadHeader::LENGTH - MATRIX_REGION_BASE;

    static constexpr size_t MAX_MATRIX_VALUE_BYTES = 512;
    static constexpr size_t MAX_MATRIX_STATES = 4;

    /// The in-RAM value grid for one MATRIX descriptor (row-major).
    struct MatrixState
    {
        uint16_t descriptor_index{0};
        uint16_t width{0};
        uint16_t height{0};
        uint8_t elem_size{0};
        std::array<uint8_t, MAX_MATRIX_VALUE_BYTES> values{};
    };

    /// Find (or lazily create from the blob) the value grid for @p ref.
    /// Returns nullptr when the blob has no such MATRIX, its value type is
    /// not linear, or the grid exceeds MAX_MATRIX_VALUE_BYTES. New grids
    /// seed every cell from the authored linear entry's `current` field.
    [[nodiscard]] auto find_or_init_matrix_state(DescriptorRef ref) noexcept -> MatrixState*
    {
        if (auto* const s = find_state(matrix_states_, by_index(ref.descriptor_index))) {
            return s;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorMatrix::LENGTH) {
            return nullptr;
        }
        auto const value_type =
            static_cast<uint16_t>(read_u16(*blob, MATRIX_CONTROL_VALUE_TYPE_FIELD) & atdecc::aem::CONTROL_VALUE_TYPE_MASK);
        auto const elem = atdecc::aem::control_value_element_size(value_type);
        uint16_t const width = read_u16(*blob, MATRIX_WIDTH_FIELD);
        uint16_t const height = read_u16(*blob, MATRIX_HEIGHT_FIELD);
        size_t const cells = size_t{width} * height;
        if (!atdecc::aem::is_linear_value_type(value_type) || elem == 0 || cells == 0 || cells * elem > MAX_MATRIX_VALUE_BYTES) {
            return nullptr;
        }
        MatrixState state{
            .descriptor_index = ref.descriptor_index,
            .width = width,
            .height = height,
            .elem_size = static_cast<uint8_t>(elem),
            .values = {}};
        // Seed every cell from the authored linear entry's `current` field.
        size_t const values_offset = read_u16(*blob, MATRIX_VALUES_OFFSET_FIELD);
        size_t const current_offset = values_offset + atdecc::aem::linear_entry_current_offset(elem);
        if (current_offset + elem <= blob->size()) {
            for (size_t cell = 0; cell < cells; ++cell) {
                span_copy(make_span(state.values, {.start = cell * elem, .length = elem}), blob->subspan(current_offset, elem));
            }
        }
        return matrix_states_.try_push_back(state);
    }

    /// Enumerate the region's cell grid offsets in `direction` order,
    /// starting after `item_offset` skipped cells, calling @p apply with
    /// (enumeration_index, byte_offset_into_values) for each remaining cell.
    template <typename Fn>
    static void for_each_region_cell(
        MatrixState const& state, MatrixRegion const& region, uint16_t const direction, uint16_t const item_offset, Fn apply)
    {
        size_t enumerated = 0;
        size_t applied = 0;
        size_t const total = size_t{region.width} * region.height;
        for (size_t i = 0; i < total; ++i) {
            uint16_t col{};
            uint16_t row{};
            if (direction == atdecc::aem::AemMatrixPayloadHeader::DIRECTION_VERTICAL) {
                col = static_cast<uint16_t>(region.column + (i / region.height));
                row = static_cast<uint16_t>(region.row + (i % region.height));
            } else {
                col = static_cast<uint16_t>(region.column + (i % region.width));
                row = static_cast<uint16_t>(region.row + (i / region.width));
            }
            if (enumerated++ < item_offset) {
                continue;
            }
            size_t const cell_off = ((size_t{row} * state.width) + col) * state.elem_size;
            if (!apply(applied, cell_off)) {
                return;
            }
            ++applied;
        }
    }

    /// Apply a SET_MATRIX region write (Clause 7.4.33). @p value is the
    /// payload after the 4-byte descriptor header: the 12-byte region header
    /// followed by value_count matrix point values.
    [[nodiscard]] auto set_matrix(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        if (value.size() < MATRIX_REGION_HEADER_SIZE) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto* const state = find_or_init_matrix_state(ref);
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        MatrixRegion const region{
            .column = read_u16(value, REGION_COLUMN_FIELD),
            .row = read_u16(value, REGION_ROW_FIELD),
            .width = read_u16(value, REGION_WIDTH_FIELD),
            .height = read_u16(value, REGION_HEIGHT_FIELD)};
        uint16_t const rep_dir_count = read_u16(value, REGION_REP_DIR_COUNT_FIELD);
        uint16_t const item_offset = read_u16(value, REGION_ITEM_OFFSET_FIELD);
        bool const rep = (rep_dir_count & atdecc::aem::AemMatrixPayloadHeader::REP_FLAG) != 0;
        uint16_t const direction = static_cast<uint16_t>(
            (rep_dir_count >> atdecc::aem::AemMatrixPayloadHeader::DIRECTION_SHIFT) &
            atdecc::aem::AemMatrixPayloadHeader::DIRECTION_MASK);
        uint16_t const value_count = static_cast<uint16_t>(rep_dir_count & atdecc::aem::AemMatrixPayloadHeader::VALUE_COUNT_MASK);

        size_t const region_cells = size_t{region.width} * region.height;
        size_t const elem = state->elem_size;
        auto const values = value.subspan(MATRIX_REGION_HEADER_SIZE);
        if (region.width == 0 || region.height == 0 || region.column + region.width > state->width ||
            region.row + region.height > state->height || direction > atdecc::aem::AemMatrixPayloadHeader::DIRECTION_VERTICAL ||
            value_count == 0 || item_offset >= region_cells || values.size() < size_t{value_count} * elem) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }

        if (on_matrix_changed_) {
            MatrixWrite const write{
                .region = region,
                .direction = direction,
                .item_offset = item_offset,
                .value_count = value_count,
                .rep = rep,
                .elem_size = static_cast<uint8_t>(elem),
                .values = values.first(size_t{value_count} * elem)};
            if (auto const status = on_matrix_changed_(ref.descriptor_index, write); status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }

        size_t const writes = rep ? (region_cells - item_offset) : std::min<size_t>(value_count, region_cells - item_offset);
        for_each_region_cell(*state, region, direction, item_offset, [&](size_t const n, size_t const cell_off) {
            if (n >= writes) {
                return false;
            }
            size_t const src = (n % value_count) * elem;
            span_copy(make_span(state->values, {.start = cell_off, .length = elem}), values.subspan(src, elem));
            return true;
        });
        return atdecc::AEM_STATUS_SUCCESS;
    }

    /// Serve a GET_MATRIX region read (Clause 7.4.34). @p request is the
    /// command payload after the 4-byte descriptor header; the response
    /// value written to @p out echoes the 12-byte region header (with the
    /// actual returned count) followed by the region's current values.
    [[nodiscard]] auto get_matrix(DescriptorRef ref, std::span<uint8_t const> request, std::span<uint8_t> out) noexcept -> size_t
    {
        if (request.size() < MATRIX_REGION_HEADER_SIZE || out.size() < MATRIX_REGION_HEADER_SIZE) {
            return 0;
        }
        auto* const state = find_or_init_matrix_state(ref);
        if (state == nullptr) {
            return 0;
        }
        MatrixRegion const region{
            .column = read_u16(request, REGION_COLUMN_FIELD),
            .row = read_u16(request, REGION_ROW_FIELD),
            .width = read_u16(request, REGION_WIDTH_FIELD),
            .height = read_u16(request, REGION_HEIGHT_FIELD)};
        uint16_t const rep_dir_count = read_u16(request, REGION_REP_DIR_COUNT_FIELD);
        uint16_t const item_offset = read_u16(request, REGION_ITEM_OFFSET_FIELD);
        uint16_t const direction = static_cast<uint16_t>(
            (rep_dir_count >> atdecc::aem::AemMatrixPayloadHeader::DIRECTION_SHIFT) &
            atdecc::aem::AemMatrixPayloadHeader::DIRECTION_MASK);
        uint16_t const requested = static_cast<uint16_t>(rep_dir_count & atdecc::aem::AemMatrixPayloadHeader::VALUE_COUNT_MASK);

        size_t const region_cells = size_t{region.width} * region.height;
        size_t const elem = state->elem_size;
        if (region.width == 0 || region.height == 0 || region.column + region.width > state->width ||
            region.row + region.height > state->height || direction > atdecc::aem::AemMatrixPayloadHeader::DIRECTION_VERTICAL ||
            item_offset >= region_cells) {
            return 0;
        }
        // value_count 0 reads the whole (remaining) region.
        size_t const remaining = region_cells - item_offset;
        size_t const count = (requested == 0) ? remaining : std::min<size_t>(requested, remaining);
        if (out.size() < MATRIX_REGION_HEADER_SIZE + (count * elem)) {
            return 0;
        }

        span_copy(out.first(MATRIX_REGION_HEADER_SIZE), request.first(MATRIX_REGION_HEADER_SIZE));
        write_u16(
            out,
            REGION_REP_DIR_COUNT_FIELD,
            static_cast<uint16_t>((direction << atdecc::aem::AemMatrixPayloadHeader::DIRECTION_SHIFT) | count));
        auto const values_out = out.subspan(MATRIX_REGION_HEADER_SIZE);
        for_each_region_cell(*state, region, direction, item_offset, [&](size_t const n, size_t const cell_off) {
            if (n >= count) {
                return false;
            }
            span_copy(values_out.subspan(n * elem, elem), make_const_span(state->values, {.start = cell_off, .length = elem}));
            return true;
        });
        return MATRIX_REGION_HEADER_SIZE + (count * elem);
    }

    statusbar::sg14::inplace_vector<MatrixState, MAX_MATRIX_STATES> matrix_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, MatrixWrite const&), 64> on_matrix_changed_{};

    // MIXER built-in machinery. DescriptorMixer wire offsets (the value
    // trailer entry layout matches CONTROL/MATRIX: min, max, step,
    // default, current, unit, string).
    static constexpr size_t MIXER_CONTROL_VALUE_TYPE_FIELD = offsetof(atdecc::aem::DescriptorMixer, control_value_type);
    static constexpr size_t MIXER_VALUE_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorMixer, value_offset);

    static constexpr size_t MAX_MIXER_VALUE_BYTES = 8;
    static constexpr size_t MAX_MIXER_STATES = 8;

    /// The in-RAM value for one MIXER descriptor.
    struct MixerState
    {
        uint16_t descriptor_index{0};
        uint8_t elem_size{0};
        std::array<uint8_t, MAX_MIXER_VALUE_BYTES> value{};
    };

    /// Find (or lazily create from the blob) the value state for @p ref.
    /// Returns nullptr when the blob has no such MIXER or its value type
    /// is not linear. New states seed from the authored linear entry's
    /// `current` field.
    [[nodiscard]] auto find_or_init_mixer_state(DescriptorRef ref) noexcept -> MixerState*
    {
        if (auto* const s = find_state(mixer_states_, by_index(ref.descriptor_index))) {
            return s;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorMixer::LENGTH) {
            return nullptr;
        }
        auto const value_type =
            static_cast<uint16_t>(read_u16(*blob, MIXER_CONTROL_VALUE_TYPE_FIELD) & atdecc::aem::CONTROL_VALUE_TYPE_MASK);
        auto const elem = atdecc::aem::control_value_element_size(value_type);
        if (!atdecc::aem::is_linear_value_type(value_type) || elem == 0 || elem > MAX_MIXER_VALUE_BYTES) {
            return nullptr;
        }
        MixerState state{.descriptor_index = ref.descriptor_index, .elem_size = static_cast<uint8_t>(elem), .value = {}};
        // Seed from the authored linear entry's `current` field.
        size_t const value_offset = read_u16(*blob, MIXER_VALUE_OFFSET_FIELD);
        size_t const current_offset = value_offset + atdecc::aem::linear_entry_current_offset(elem);
        if (current_offset + elem <= blob->size()) {
            span_copy(make_span(state.value, {.start = 0, .length = elem}), blob->subspan(current_offset, elem));
        }
        return mixer_states_.try_push_back(state);
    }

    /// Apply a SET_MIXER value write (Clause 7.4.35). @p value is the
    /// payload after the 4-byte AemMixerPayloadHeader: the mixer's new
    /// value (one element of its control_value_type).
    [[nodiscard]] auto set_mixer(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        auto* const state = find_or_init_mixer_state(ref);
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        if (value.size() < state->elem_size) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto const incoming = value.first(state->elem_size);
        if (on_mixer_changed_) {
            if (auto const status = on_mixer_changed_(ref.descriptor_index, incoming); status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }
        span_copy(make_span(state->value, {.start = 0, .length = state->elem_size}), incoming);
        return atdecc::AEM_STATUS_SUCCESS;
    }

    /// Serve a GET_MIXER value read (Clause 7.4.36): the stored value.
    [[nodiscard]] auto get_mixer(DescriptorRef ref, std::span<uint8_t> out) noexcept -> size_t { return mixer_value(ref, out); }

    statusbar::sg14::inplace_vector<MixerState, MAX_MIXER_STATES> mixer_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, std::span<uint8_t const>), 64> on_mixer_changed_{};

    // STREAM lifecycle machinery (kit phase 5). DescriptorStream wire offsets.
    static constexpr size_t STREAM_CURRENT_FORMAT_FIELD = offsetof(atdecc::aem::DescriptorStream, current_format);
    static constexpr size_t STREAM_FORMATS_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorStream, formats_offset);
    static constexpr size_t STREAM_NUMBER_OF_FORMATS_FIELD = offsetof(atdecc::aem::DescriptorStream, number_of_formats);
    static constexpr size_t MAX_STREAM_STATES = 16;

    /// Runtime state for one STREAM_INPUT/OUTPUT descriptor.
    struct StreamRuntimeState
    {
        uint16_t descriptor_type{0};
        uint16_t descriptor_index{0};
        bool has_format{false};
        ieee::Eui64 format{};  ///< runtime current_format (valid when has_format)
        bool stopped{false};   ///< STOP_STREAMING latched
    };

    [[nodiscard]] static auto is_stream_descriptor(uint16_t const descriptor_type) noexcept -> bool
    {
        return descriptor_type == atdecc::aem::DESCRIPTOR_STREAM_INPUT || descriptor_type == atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
    }

    [[nodiscard]] auto find_stream_state(uint16_t const descriptor_type, uint16_t const descriptor_index) const noexcept
        -> StreamRuntimeState const*
    {
        return find_state(stream_states_, by_stream(descriptor_type, descriptor_index));
    }

    /// Find (or lazily create) the runtime state for @p ref. Returns
    /// nullptr when the blob has no such stream or the table is full.
    [[nodiscard]] auto find_or_init_stream_state(DescriptorRef ref) noexcept -> StreamRuntimeState*
    {
        if (auto* const s = find_state(stream_states_, by_stream(ref.descriptor_type, ref.descriptor_index))) {
            return s;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorStream::MINIMUM_LENGTH) {
            return nullptr;
        }
        return stream_states_.try_push_back(
            StreamRuntimeState{.descriptor_type = ref.descriptor_type, .descriptor_index = ref.descriptor_index});
    }

    /// True when @p format is the descriptor's authored current_format or
    /// appears in its stream_formats trailer.
    [[nodiscard]] static auto stream_format_supported(std::span<uint8_t const> const blob, ieee::Eui64 const& format) noexcept
        -> bool
    {
        ieee::Eui64 authored{};
        span_load(authored, blob.subspan(STREAM_CURRENT_FORMAT_FIELD, 8));
        return authored == format || blob_list_contains(blob, STREAM_FORMATS_OFFSET_FIELD, STREAM_NUMBER_OF_FORMATS_FIELD, format);
    }

    /// Apply a SET_STREAM_FORMAT (Clause 7.4.9). @p value is the payload
    /// after the 4-byte descriptor header: the requested 8-byte format.
    [[nodiscard]] auto set_stream_format(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        if (value.size() < sizeof(ieee::Eui64)) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorStream::MINIMUM_LENGTH) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        ieee::Eui64 format{};
        span_load(format, value.first(sizeof(ieee::Eui64)));
        if (!stream_format_supported(*blob, format)) {
            return atdecc::AEM_STATUS_NOT_SUPPORTED;
        }
        auto* const state = find_or_init_stream_state(ref);
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        if (on_stream_format_changed_) {
            if (auto const status = on_stream_format_changed_(ref.descriptor_type, ref.descriptor_index, format.to_uint64());
                status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }
        state->format = format;
        state->has_format = true;
        return atdecc::AEM_STATUS_SUCCESS;
    }

    /// Apply a START/STOP_STREAMING (Clause 7.4.35/7.4.36 of -2013
    /// numbering; streaming defaults to on, both directions idempotent).
    [[nodiscard]] auto set_streaming(DescriptorRef ref, bool const streaming) -> uint8_t
    {
        auto* const state = find_or_init_stream_state(ref);
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        if (on_streaming_changed_) {
            if (auto const status = on_streaming_changed_(ref.descriptor_type, ref.descriptor_index, streaming);
                status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }
        state->stopped = !streaming;
        return atdecc::AEM_STATUS_SUCCESS;
    }

    statusbar::sg14::inplace_vector<StreamRuntimeState, MAX_STREAM_STATES> stream_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, uint16_t, uint64_t), 64> on_stream_format_changed_{};
    statusbar::sg14::inplace_function<uint8_t(uint16_t, uint16_t, bool), 64> on_streaming_changed_{};

    // AUDIO_UNIT sampling-rate machinery. DescriptorAudioUnit wire offsets.
    static constexpr size_t AUDIO_UNIT_CURRENT_RATE_FIELD = offsetof(atdecc::aem::DescriptorAudioUnit, current_sampling_rate);
    static constexpr size_t AUDIO_UNIT_RATES_OFFSET_FIELD = offsetof(atdecc::aem::DescriptorAudioUnit, sampling_rates_offset);
    static constexpr size_t AUDIO_UNIT_RATES_COUNT_FIELD = offsetof(atdecc::aem::DescriptorAudioUnit, sampling_rates_count);
    static constexpr size_t MAX_AUDIO_UNIT_STATES = 4;

    /// The runtime sampling rate for one AUDIO_UNIT descriptor.
    struct SamplingRateState
    {
        uint16_t descriptor_index{0};
        uint32_t rate{0};
    };

    [[nodiscard]] auto find_sampling_rate_state(uint16_t const descriptor_index) const noexcept -> SamplingRateState const*
    {
        return find_state(sampling_rate_states_, by_index(descriptor_index));
    }

    /// Apply a SET_SAMPLING_RATE (Clause 7.4.21). @p value is the payload
    /// after the 4-byte descriptor header: the requested 4-byte rate,
    /// validated against the AUDIO_UNIT's authored sampling_rates.
    [[nodiscard]] auto set_sampling_rate(DescriptorRef ref, std::span<uint8_t const> value) -> uint8_t
    {
        if (value.size() < 4) {
            return atdecc::AEM_STATUS_BAD_ARGUMENTS;
        }
        auto const blob = storage_.get_descriptor(ref.configuration_index, ref.descriptor_type, ref.descriptor_index);
        if (!blob.has_value() || blob->size() < atdecc::aem::DescriptorAudioUnit::LENGTH) {
            return atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
        }
        ieee::quadlet_t requested{0};
        span_load(requested, value.first(4));

        ieee::quadlet_t authored{0};
        span_load(authored, blob->subspan(AUDIO_UNIT_CURRENT_RATE_FIELD, 4));
        if (authored != requested &&
            !blob_list_contains(*blob, AUDIO_UNIT_RATES_OFFSET_FIELD, AUDIO_UNIT_RATES_COUNT_FIELD, requested)) {
            return atdecc::AEM_STATUS_NOT_SUPPORTED;
        }

        if (on_sampling_rate_changed_) {
            if (auto const status = on_sampling_rate_changed_(ref.descriptor_index, requested.get());
                status != atdecc::AEM_STATUS_SUCCESS) {
                return status;
            }
        }
        auto* const state = find_or_push_state(
            sampling_rate_states_, by_index(ref.descriptor_index), SamplingRateState{.descriptor_index = ref.descriptor_index});
        if (state == nullptr) {
            return atdecc::AEM_STATUS_NO_RESOURCES;
        }
        state->rate = requested.get();
        return atdecc::AEM_STATUS_SUCCESS;
    }

    statusbar::sg14::inplace_vector<SamplingRateState, MAX_AUDIO_UNIT_STATES> sampling_rate_states_;
    statusbar::sg14::inplace_function<uint8_t(uint16_t, uint32_t), 64> on_sampling_rate_changed_{};

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
