#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Compatibility adapter that makes the legacy vector-backed `EntityModel`
/// implement the new `AemEntityHandler` interface.
///
/// This bridge exists so AemCommandHandler can switch its internal
/// descriptor-lookup path to the new handler-based AemEntityModel
/// (with table-driven dispatch, typed inline trailers, wire_size, etc.)
/// without forcing every client — avb_entity_am824_io, avb_entity_stereo_io,
/// the nanoavb_entity tests, and so on — to migrate in the same commit.
///
/// Each `on_get_<type>` override calls the corresponding typed getter on
/// the wrapped `EntityModel` (e.g. `get_audio_unit(index)`) and copies
/// the returned struct into the handler's output reference. The adapter
/// never parses bytes back into structs — it uses the model's in-memory
/// typed storage directly, so there is no wire-format round-trip cost.
///
/// Descriptor pairs that share a C++ struct are handled by branching
/// on `id.ref.descriptor_type`:
///   * STREAM_INPUT / STREAM_OUTPUT -> EntityModel::get_stream_input /
///     get_stream_output
///   * JACK_INPUT / JACK_OUTPUT -> EntityModel::get_jack_input /
///     get_jack_output
///   * STREAM_PORT_INPUT / STREAM_PORT_OUTPUT -> get_stream_port_input /
///     get_stream_port_output
///
/// This adapter is a *transitional* type. Phase 5 of the AEM refactor
/// deletes the legacy `EntityModel` and this adapter along with it; by
/// then all clients should implement `AemEntityHandler` directly.

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"

namespace statusbar::nanoavb {

/// Adapter that implements AemEntityHandler by delegating to a legacy
/// vector-backed EntityModel.
class EntityModelAdapter : public AemEntityHandler
{
  public:
    explicit EntityModelAdapter(EntityModel const& model) noexcept
        : model_{&model}
    {}

    // ---- Top-level descriptors -------------------------------------------

    auto on_get_entity(DescriptorId /*id*/, DescriptorEntity& desc) -> bool override
    {
        desc = model_->get_entity();
        return true;
    }

    auto on_get_configuration(DescriptorId id, DescriptorConfiguration& desc) -> bool override
    {
        auto result = model_->get_configuration(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    // ---- Unit descriptors ------------------------------------------------

    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override
    {
        auto result = model_->get_audio_unit(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    // ---- Streams / Jacks / Interface / Clock -----------------------------

    auto on_get_stream(DescriptorId id, DescriptorStream& desc) -> bool override
    {
        using atdecc::aem::DESCRIPTOR_STREAM_INPUT;
        using atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;

        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_INPUT) {
            auto result = model_->get_stream_input(id.ref.descriptor_index);
            if (!result.has_value()) {
                return false;
            }
            desc = **result;
            return true;
        }
        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
            auto result = model_->get_stream_output(id.ref.descriptor_index);
            if (!result.has_value()) {
                return false;
            }
            desc = **result;
            return true;
        }
        return false;
    }

    auto on_get_jack(DescriptorId id, DescriptorJack& desc) -> bool override
    {
        using atdecc::aem::DESCRIPTOR_JACK_INPUT;
        using atdecc::aem::DESCRIPTOR_JACK_OUTPUT;

        if (id.ref.descriptor_type == DESCRIPTOR_JACK_INPUT) {
            auto result = model_->get_jack_input(id.ref.descriptor_index);
            if (!result.has_value()) {
                return false;
            }
            desc = **result;
            return true;
        }
        if (id.ref.descriptor_type == DESCRIPTOR_JACK_OUTPUT) {
            auto result = model_->get_jack_output(id.ref.descriptor_index);
            if (!result.has_value()) {
                return false;
            }
            desc = **result;
            return true;
        }
        return false;
    }

    auto on_get_avb_interface(DescriptorId id, DescriptorAvbInterface& desc) -> bool override
    {
        auto result = model_->get_avb_interface(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    auto on_get_clock_source(DescriptorId id, DescriptorClockSource& desc) -> bool override
    {
        auto result = model_->get_clock_source(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    auto on_get_clock_domain(DescriptorId id, DescriptorClockDomain& desc) -> bool override
    {
        auto result = model_->get_clock_domain(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    // ---- Locale / Strings ------------------------------------------------

    auto on_get_locale(DescriptorId id, DescriptorLocale& desc) -> bool override
    {
        auto result = model_->get_locale(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    auto on_get_strings(DescriptorId id, DescriptorStrings& desc) -> bool override
    {
        auto result = model_->get_strings(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    // ---- Ports -----------------------------------------------------------

    auto on_get_stream_port(DescriptorId id, DescriptorStreamPort& desc) -> bool override
    {
        using atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
        using atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;

        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_PORT_INPUT) {
            auto result = model_->get_stream_port_input(id.ref.descriptor_index);
            if (!result.has_value()) {
                return false;
            }
            desc = **result;
            return true;
        }
        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_PORT_OUTPUT) {
            auto result = model_->get_stream_port_output(id.ref.descriptor_index);
            if (!result.has_value()) {
                return false;
            }
            desc = **result;
            return true;
        }
        return false;
    }

    // ---- Clusters / Maps / Control ---------------------------------------

    auto on_get_audio_cluster(DescriptorId id, DescriptorAudioCluster& desc) -> bool override
    {
        auto result = model_->get_audio_cluster(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    auto on_get_audio_map(DescriptorId id, DescriptorAudioMap& desc) -> bool override
    {
        auto result = model_->get_audio_map(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

    auto on_get_control(DescriptorId id, DescriptorControl& desc) -> bool override
    {
        auto result = model_->get_control(id.ref.descriptor_index);
        if (!result.has_value()) {
            return false;
        }
        desc = **result;
        return true;
    }

  private:
    EntityModel const* model_{nullptr};
};

}  // namespace statusbar::nanoavb
