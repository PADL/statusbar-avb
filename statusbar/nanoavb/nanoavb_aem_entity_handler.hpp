#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB AEM Entity Handler — abstract interface implemented by the
/// application to provide descriptor content on demand.
///
/// This is the Phase-3 replacement for the vector-backed `EntityModel`.
/// The new model does not own descriptor storage; instead, it delegates
/// every READ_DESCRIPTOR, GET_NAME, and SET_NAME request to a user-
/// supplied handler. Handlers receive a typed descriptor struct reference
/// (not raw bytes) and fill in the dynamic fields — they never do byte-
/// offset math into a wire buffer.
///
/// Symbol identifiers
/// ==================
/// Each hook receives a `DescriptorId` by value: the wire address plus the
/// blob symbol. Symbols are pre-agreed application-layer identifiers that
/// survive descriptor_index reshuffling: if the AEM tooling is regenerated
/// and the index of a control changes, its symbol stays the same, so
/// application code keeps binding to the same abstract "volume control"
/// entry. The AemEntityModel looks up the symbol from the
/// `DescriptorStorage` symbol table (when one is attached) and passes it
/// through to the handler; without a storage, the symbol is 0 and the
/// handler is expected to dispatch on the wire address directly.
///
/// Default behaviour
/// =================
/// Every method has a no-op default implementation that returns "not
/// supported" — `false` for getters, `nullopt` for names, and
/// NOT_IMPLEMENTED for setters. Applications override only the methods
/// relevant to their entity.
///
/// Threading
/// =========
/// All methods are called from the reactor's single event-loop thread.
/// Handlers must not block or throw.

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::atdecc::aem {
class DescriptorStorage;  // forward decl: descriptor_storage() returns a pointer only
}

namespace statusbar::nanoavb {

using atdecc::AEM_STATUS_NOT_IMPLEMENTED;
using atdecc::aem::AtdeccString;
using atdecc::aem::DescriptorAudioCluster;
using atdecc::aem::DescriptorAudioMap;
using atdecc::aem::DescriptorAudioUnit;
using atdecc::aem::DescriptorAvbInterface;
using atdecc::aem::DescriptorClockDomain;
using atdecc::aem::DescriptorClockSource;
using atdecc::aem::DescriptorConfiguration;
using atdecc::aem::DescriptorControl;
using atdecc::aem::DescriptorControlBlock;
using atdecc::aem::DescriptorEntity;
using atdecc::aem::DescriptorExternalPort;
using atdecc::aem::DescriptorInternalPort;
using atdecc::aem::DescriptorJack;
using atdecc::aem::DescriptorLocale;
using atdecc::aem::DescriptorMatrix;
using atdecc::aem::DescriptorMatrixSignal;
using atdecc::aem::DescriptorMemoryObject;
using atdecc::aem::DescriptorMixer;
using atdecc::aem::DescriptorPtpInstance;
using atdecc::aem::DescriptorPtpPort;
using atdecc::aem::DescriptorRef;
using atdecc::aem::DescriptorSensorCluster;
using atdecc::aem::DescriptorSensorMap;
using atdecc::aem::DescriptorSensorUnit;
using atdecc::aem::DescriptorSignalCombiner;
using atdecc::aem::DescriptorSignalDemultiplexer;
using atdecc::aem::DescriptorSignalMultiplexer;
using atdecc::aem::DescriptorSignalSelector;
using atdecc::aem::DescriptorSignalSplitter;
using atdecc::aem::DescriptorSignalTranscoder;
using atdecc::aem::DescriptorStorage;
using atdecc::aem::DescriptorStream;
using atdecc::aem::DescriptorStreamPort;
using atdecc::aem::DescriptorStrings;
using atdecc::aem::DescriptorTiming;
using atdecc::aem::DescriptorVideoCluster;
using atdecc::aem::DescriptorVideoMap;
using atdecc::aem::DescriptorVideoUnit;
using atdecc::aem::NameRef;

/// Identity of the descriptor a handler hook is being asked about, passed
/// BY VALUE to every on_get_*/on_set_* hook: the wire address (`ref` —
/// configuration, descriptor type, descriptor index) plus the blob's
/// `symbol` for that descriptor.
///
/// Descriptor indices depend on the AEM hierarchy — regenerating the entity
/// model (adding a control, reordering a unit) renumbers everything after
/// the change. The symbol is the stable application-level identifier that
/// survives those updates, so handler code should key its dynamic state on
/// `id.symbol` and treat `id.ref` as the transient wire address. Without an
/// attached DescriptorStorage the symbol is 0 and handlers fall back to
/// dispatching on the wire address.
struct DescriptorId
{
    DescriptorRef ref{};  ///< wire address: configuration_index + descriptor_type + descriptor_index
    uint32_t symbol{0};   ///< blob symbol-table id (stable across model regeneration); 0 = none
};

/// Abstract base class for AEM descriptor providers.
///
/// Handlers implement only the descriptor types their entity uses.
/// Each on_get_* method receives a mutable reference to a default-
/// initialized (or DescriptorStorage-preloaded) descriptor struct;
/// the handler populates dynamic fields and returns true. Returning
/// false tells the AemEntityModel that the descriptor does not exist,
/// which maps to AEM_STATUS_NO_SUCH_DESCRIPTOR at the wire level.
///
/// One method per C++ struct type is provided. For pairs like
/// STREAM_INPUT/STREAM_OUTPUT that share a struct but carry different
/// descriptor_type codes, the handler can branch on
/// `id.ref.descriptor_type` inside a single `on_get_stream` override.
class AemEntityHandler
{
  public:
    AemEntityHandler() = default;
    virtual ~AemEntityHandler() = default;

    // Copy is disabled: concrete handlers often hold references or
    // application state that shouldn't be copied. Move is defaulted so
    // objects containing a handler-backed AemEntityModel (e.g. AemCommandHandler
    // inside NanoAvbComponents) can still be move-constructed. Derived
    // handler types that need to forbid movement should mark their own
    // members non-movable rather than deleting here.
    AemEntityHandler(AemEntityHandler const&) = delete;
    auto operator=(AemEntityHandler const&) -> AemEntityHandler& = delete;
    AemEntityHandler(AemEntityHandler&&) noexcept = default;
    auto operator=(AemEntityHandler&&) noexcept -> AemEntityHandler& = default;

    /// The DescriptorStorage backing this handler, if any. The command dispatch
    /// uses it to resolve a descriptor's well-known **symbol** (the blob symbol
    /// table) so every on_get_*/on_set_* handler receives the symbol -- the stable
    /// identifier that decouples handler code from the designer's descriptor-index
    /// choices. Default: none (symbols resolve to 0). A storage-backed handler
    /// (e.g. DescriptorStorageHandler) overrides this to return its blob.
    [[nodiscard]] virtual auto descriptor_storage() const noexcept -> DescriptorStorage const* { return nullptr; }

    // ---- Top-level descriptors -------------------------------------------

    virtual auto on_get_entity(DescriptorId /*id*/, DescriptorEntity& /*desc*/) -> bool { return false; }

    virtual auto on_get_configuration(DescriptorId /*id*/, DescriptorConfiguration& /*desc*/) -> bool { return false; }

    // ---- Unit descriptors ------------------------------------------------

    virtual auto on_get_audio_unit(DescriptorId /*id*/, DescriptorAudioUnit& /*desc*/) -> bool { return false; }

    virtual auto on_get_video_unit(DescriptorId /*id*/, DescriptorVideoUnit& /*desc*/) -> bool { return false; }

    virtual auto on_get_sensor_unit(DescriptorId /*id*/, DescriptorSensorUnit& /*desc*/) -> bool { return false; }

    // ---- Stream / Jack / Interface / Clock -------------------------------
    // Stream input and output share DescriptorStream. Branch on
    // ref.descriptor_type (DESCRIPTOR_STREAM_INPUT / DESCRIPTOR_STREAM_OUTPUT)
    // inside this method to distinguish direction.

    virtual auto on_get_stream(DescriptorId /*id*/, DescriptorStream& /*desc*/) -> bool { return false; }

    /// Jack input and output share DescriptorJack. Branch on
    /// ref.descriptor_type (DESCRIPTOR_JACK_INPUT / DESCRIPTOR_JACK_OUTPUT).
    virtual auto on_get_jack(DescriptorId /*id*/, DescriptorJack& /*desc*/) -> bool { return false; }

    virtual auto on_get_avb_interface(DescriptorId /*id*/, DescriptorAvbInterface& /*desc*/) -> bool { return false; }

    virtual auto on_get_clock_source(DescriptorId /*id*/, DescriptorClockSource& /*desc*/) -> bool { return false; }

    virtual auto on_get_clock_domain(DescriptorId /*id*/, DescriptorClockDomain& /*desc*/) -> bool { return false; }

    // ---- Memory / Locale / Strings ---------------------------------------

    virtual auto on_get_memory_object(DescriptorId /*id*/, DescriptorMemoryObject& /*desc*/) -> bool { return false; }

    virtual auto on_get_locale(DescriptorId /*id*/, DescriptorLocale& /*desc*/) -> bool { return false; }

    virtual auto on_get_strings(DescriptorId /*id*/, DescriptorStrings& /*desc*/) -> bool { return false; }

    // ---- Ports -----------------------------------------------------------
    // Each *_port descriptor has input and output variants sharing one
    // struct; branch on ref.descriptor_type inside the handler.

    virtual auto on_get_stream_port(DescriptorId /*id*/, DescriptorStreamPort& /*desc*/) -> bool { return false; }

    virtual auto on_get_external_port(DescriptorId /*id*/, DescriptorExternalPort& /*desc*/) -> bool { return false; }

    virtual auto on_get_internal_port(DescriptorId /*id*/, DescriptorInternalPort& /*desc*/) -> bool { return false; }

    // ---- Clusters --------------------------------------------------------

    virtual auto on_get_audio_cluster(DescriptorId /*id*/, DescriptorAudioCluster& /*desc*/) -> bool { return false; }

    virtual auto on_get_video_cluster(DescriptorId /*id*/, DescriptorVideoCluster& /*desc*/) -> bool { return false; }

    virtual auto on_get_sensor_cluster(DescriptorId /*id*/, DescriptorSensorCluster& /*desc*/) -> bool { return false; }

    // ---- Maps ------------------------------------------------------------

    virtual auto on_get_audio_map(DescriptorId /*id*/, DescriptorAudioMap& /*desc*/) -> bool { return false; }

    virtual auto on_get_video_map(DescriptorId /*id*/, DescriptorVideoMap& /*desc*/) -> bool { return false; }

    virtual auto on_get_sensor_map(DescriptorId /*id*/, DescriptorSensorMap& /*desc*/) -> bool { return false; }

    // ---- Controls --------------------------------------------------------

    virtual auto on_get_control(DescriptorId /*id*/, DescriptorControl& /*desc*/) -> bool { return false; }

    virtual auto on_get_control_block(DescriptorId /*id*/, DescriptorControlBlock& /*desc*/) -> bool { return false; }

    // ---- Signal routing --------------------------------------------------

    virtual auto on_get_signal_selector(DescriptorId /*id*/, DescriptorSignalSelector& /*desc*/) -> bool { return false; }

    virtual auto on_get_mixer(DescriptorId /*id*/, DescriptorMixer& /*desc*/) -> bool { return false; }

    virtual auto on_get_matrix(DescriptorId /*id*/, DescriptorMatrix& /*desc*/) -> bool { return false; }

    virtual auto on_get_matrix_signal(DescriptorId /*id*/, DescriptorMatrixSignal& /*desc*/) -> bool { return false; }

    virtual auto on_get_signal_splitter(DescriptorId /*id*/, DescriptorSignalSplitter& /*desc*/) -> bool { return false; }

    virtual auto on_get_signal_combiner(DescriptorId /*id*/, DescriptorSignalCombiner& /*desc*/) -> bool { return false; }

    virtual auto on_get_signal_demultiplexer(DescriptorId /*id*/, DescriptorSignalDemultiplexer& /*desc*/) -> bool { return false; }

    virtual auto on_get_signal_multiplexer(DescriptorId /*id*/, DescriptorSignalMultiplexer& /*desc*/) -> bool { return false; }

    virtual auto on_get_signal_transcoder(DescriptorId /*id*/, DescriptorSignalTranscoder& /*desc*/) -> bool { return false; }

    // ---- Timing / PTP ----------------------------------------------------

    virtual auto on_get_timing(DescriptorId /*id*/, DescriptorTiming& /*desc*/) -> bool { return false; }

    virtual auto on_get_ptp_instance(DescriptorId /*id*/, DescriptorPtpInstance& /*desc*/) -> bool { return false; }

    virtual auto on_get_ptp_port(DescriptorId /*id*/, DescriptorPtpPort& /*desc*/) -> bool { return false; }

    // ---- Name get/set (cross-cutting, IEEE 1722.1 Clause 7.4.17/7.4.18) --

    /// Return the current value of the named field, or nullopt if the
    /// (descriptor, name_index) pair does not exist. Multiple names per
    /// descriptor are common — e.g. ENTITY has entity_name (index 0)
    /// and group_name (index 1).
    virtual auto on_get_name(DescriptorId /*id*/, uint16_t /*name_index*/) const -> std::optional<AtdeccString>
    {
        return std::nullopt;
    }

    /// Apply a SET_NAME command. Return an AEM_STATUS_* code.
    /// AEM_STATUS_NOT_IMPLEMENTED is the appropriate default for read-only
    /// entities; return AEM_STATUS_SUCCESS after persisting the new name.
    virtual auto on_set_name(DescriptorId /*id*/, uint16_t /*name_index*/, AtdeccString const& /*name*/) -> uint8_t
    {
        return AEM_STATUS_NOT_IMPLEMENTED;
    }

    // ---- General descriptor-value SET/GET (the AEM command family) -------
    //
    // One symbol-keyed pair serves the whole family of AEM commands that read or
    // write a descriptor's mutable VALUE: SET/GET_CONTROL, SET/GET_STREAM_FORMAT,
    // SET/GET_SAMPLING_RATE, SET/GET_CLOCK_SOURCE, SET/GET_SIGNAL_SELECTOR, ... The
    // dispatch resolves the target descriptor (@p ref) and its well-known @p symbol;
    // the handler branches on @p command_type (and/or @p symbol) and interprets the
    // raw value bytes per that command's wire format. To learn valid ranges (e.g. a
    // CONTROL's min/max/step) the handler queries its own descriptor_storage().

    /// A controller wrote a descriptor value (@p command_type is the AEM_COMMAND_SET_*
    /// code). @p value is the command payload after the descriptor_type/index header.
    /// Apply it (e.g. map a CONTROL value to DSP coefficients), optionally clamped to
    /// the descriptor's range, and return an AEM_STATUS_* code. Default: NOT_IMPLEMENTED.
    virtual auto on_set_descriptor_value(uint16_t /*command_type*/, DescriptorId /*id*/, std::span<uint8_t const> /*value*/)
        -> uint8_t
    {
        return AEM_STATUS_NOT_IMPLEMENTED;
    }

    /// A controller read a descriptor value (@p command_type is the AEM_COMMAND_GET_*
    /// code). Write the current value payload (the bytes that follow the
    /// descriptor_type/index header on the wire) into @p out and return the number of
    /// bytes written, or 0 for NOT_IMPLEMENTED / no such descriptor. Default: 0.
    virtual auto on_get_descriptor_value(uint16_t /*command_type*/, DescriptorId /*id*/, std::span<uint8_t> /*out*/) -> size_t
    {
        return 0;
    }
};

}  // namespace statusbar::nanoavb
