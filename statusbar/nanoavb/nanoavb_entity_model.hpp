#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB Entity Model - Raw AEM descriptor storage
/// Stores ATDECC Entity Model descriptors initialized at startup

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace statusbar::nanoavb {

using ieee::Eui64;
using statusbar::failure;
using statusbar::Status;
using statusbar::StatusValue;
using statusbar::success;
using namespace atdecc::aem;

//
// Entity Model Configuration
//
/// Configuration for EntityModel storage capacities
struct EntityModelConfig
{
    size_t max_configurations = default_max_configurations;
    size_t max_audio_units = default_max_audio_units;
    size_t max_stream_inputs = default_max_stream_inputs;
    size_t max_stream_outputs = default_max_stream_outputs;
    size_t max_jack_inputs = default_max_jack_inputs;
    size_t max_jack_outputs = default_max_jack_outputs;
    size_t max_avb_interfaces = default_max_avb_interfaces;
    size_t max_clock_sources = default_max_clock_sources;
    size_t max_clock_domains = default_max_clock_domains;
    size_t max_locales = default_max_locales;
    size_t max_strings = default_max_strings;
    size_t max_stream_port_inputs = default_max_stream_port_inputs;
    size_t max_stream_port_outputs = default_max_stream_port_outputs;
    size_t max_audio_clusters = default_max_audio_clusters;
    size_t max_audio_maps = default_max_audio_maps;
    size_t max_controls = default_max_controls;
};

//
// Entity Model
//
/// EntityModel - stores raw AEM descriptors
/// Uses std::pmr::vector with reserved capacity for zero-allocation
/// runtime operation. Callers that need full allocation control
/// (e.g. a monotonic_buffer_resource pinned to a preallocated arena)
/// can pass their own std::pmr::memory_resource; the default is
/// std::pmr::get_default_resource() which delegates to the global
/// new/delete allocator.
class EntityModel
{
  public:
    /// Construct with configuration (reserves capacity).
    /// @param config Storage capacity configuration for each descriptor type.
    /// @param memory_resource Memory resource used for all descriptor
    ///        vectors. nullptr is treated as std::pmr::get_default_resource().
    explicit EntityModel(EntityModelConfig const& config = {}, std::pmr::memory_resource* memory_resource = nullptr)
        : config_{config}
        , mem_resource_{memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource()}
        , configurations_{mem_resource_}
        , audio_units_{mem_resource_}
        , stream_inputs_{mem_resource_}
        , stream_outputs_{mem_resource_}
        , jack_inputs_{mem_resource_}
        , jack_outputs_{mem_resource_}
        , avb_interfaces_{mem_resource_}
        , clock_sources_{mem_resource_}
        , clock_domains_{mem_resource_}
        , locales_{mem_resource_}
        , strings_{mem_resource_}
        , stream_port_inputs_{mem_resource_}
        , stream_port_outputs_{mem_resource_}
        , audio_clusters_{mem_resource_}
        , audio_maps_{mem_resource_}
        , controls_{mem_resource_}
    {
        reserve_storage();
    }

    // Entity Descriptor (always exactly one)

    /// Get the entity descriptor
    [[nodiscard]] auto get_entity() const noexcept -> DescriptorEntity const& { return entity_; }

    /// Get mutable entity descriptor for initialization
    [[nodiscard]] auto get_entity_mut() noexcept -> DescriptorEntity& { return entity_; }

    /// Set the entity descriptor
    /// @param desc The entity descriptor to set
    auto set_entity(DescriptorEntity const& desc) noexcept -> void { entity_ = desc; }

    // Configuration Descriptors

    /// Get number of configurations
    [[nodiscard]] auto configuration_count() const noexcept { return configurations_.size(); }

    /// Get a configuration descriptor by index
    /// @param index The configuration descriptor index
    [[nodiscard]] auto get_configuration(uint16_t index) const noexcept -> StatusValue<DescriptorConfiguration const*>;

    /// Add a configuration descriptor
    /// @param desc The configuration descriptor to add
    [[nodiscard]] auto add_configuration(DescriptorConfiguration const& desc) -> StatusValue<uint16_t>;

    // Audio Unit Descriptors

    [[nodiscard]] auto audio_unit_count() const noexcept { return audio_units_.size(); }

    /// @param index The audio unit descriptor index
    [[nodiscard]] auto get_audio_unit(uint16_t index) const noexcept -> StatusValue<DescriptorAudioUnit const*>;

    /// @param desc The audio unit descriptor to add
    [[nodiscard]] auto add_audio_unit(DescriptorAudioUnit const& desc) -> StatusValue<uint16_t>;

    // Stream Input Descriptors

    [[nodiscard]] auto stream_input_count() const noexcept { return stream_inputs_.size(); }

    /// @param index The stream input descriptor index
    [[nodiscard]] auto get_stream_input(uint16_t index) const noexcept -> StatusValue<DescriptorStream const*>;

    /// @param desc The stream input descriptor to add
    [[nodiscard]] auto add_stream_input(DescriptorStream const& desc) -> StatusValue<uint16_t>;

    // Stream Output Descriptors

    [[nodiscard]] auto stream_output_count() const noexcept { return stream_outputs_.size(); }

    /// @param index The stream output descriptor index
    [[nodiscard]] auto get_stream_output(uint16_t index) const noexcept -> StatusValue<DescriptorStream const*>;

    /// @param desc The stream output descriptor to add
    [[nodiscard]] auto add_stream_output(DescriptorStream const& desc) -> StatusValue<uint16_t>;

    // Jack Input Descriptors

    [[nodiscard]] auto jack_input_count() const noexcept { return jack_inputs_.size(); }

    /// @param index The jack input descriptor index
    [[nodiscard]] auto get_jack_input(uint16_t index) const noexcept -> StatusValue<DescriptorJack const*>;

    /// @param desc The jack input descriptor to add
    [[nodiscard]] auto add_jack_input(DescriptorJack const& desc) -> StatusValue<uint16_t>;

    // Jack Output Descriptors

    [[nodiscard]] auto jack_output_count() const noexcept { return jack_outputs_.size(); }

    /// @param index The jack output descriptor index
    [[nodiscard]] auto get_jack_output(uint16_t index) const noexcept -> StatusValue<DescriptorJack const*>;

    /// @param desc The jack output descriptor to add
    [[nodiscard]] auto add_jack_output(DescriptorJack const& desc) -> StatusValue<uint16_t>;

    // AVB Interface Descriptors

    [[nodiscard]] auto avb_interface_count() const noexcept { return avb_interfaces_.size(); }

    /// @param index The AVB interface descriptor index
    [[nodiscard]] auto get_avb_interface(uint16_t index) const noexcept -> StatusValue<DescriptorAvbInterface const*>;

    /// @param desc The AVB interface descriptor to add
    [[nodiscard]] auto add_avb_interface(DescriptorAvbInterface const& desc) -> StatusValue<uint16_t>;

    // Clock Source Descriptors

    [[nodiscard]] auto clock_source_count() const noexcept { return clock_sources_.size(); }

    /// @param index The clock source descriptor index
    [[nodiscard]] auto get_clock_source(uint16_t index) const noexcept -> StatusValue<DescriptorClockSource const*>;

    /// @param desc The clock source descriptor to add
    [[nodiscard]] auto add_clock_source(DescriptorClockSource const& desc) -> StatusValue<uint16_t>;

    // Clock Domain Descriptors

    [[nodiscard]] auto clock_domain_count() const noexcept { return clock_domains_.size(); }

    /// @param index The clock domain descriptor index
    [[nodiscard]] auto get_clock_domain(uint16_t index) const noexcept -> StatusValue<DescriptorClockDomain const*>;

    /// @param desc The clock domain descriptor to add
    [[nodiscard]] auto add_clock_domain(DescriptorClockDomain const& desc) -> StatusValue<uint16_t>;

    // Locale Descriptors

    [[nodiscard]] auto locale_count() const noexcept { return locales_.size(); }

    /// @param index The locale descriptor index
    [[nodiscard]] auto get_locale(uint16_t index) const noexcept -> StatusValue<DescriptorLocale const*>;

    /// @param desc The locale descriptor to add
    [[nodiscard]] auto add_locale(DescriptorLocale const& desc) -> StatusValue<uint16_t>;

    // Strings Descriptors

    [[nodiscard]] auto strings_count() const noexcept { return strings_.size(); }

    /// @param index The strings descriptor index
    [[nodiscard]] auto get_strings(uint16_t index) const noexcept -> StatusValue<DescriptorStrings const*>;

    /// @param desc The strings descriptor to add
    [[nodiscard]] auto add_strings(DescriptorStrings const& desc) -> StatusValue<uint16_t>;

    // Stream Port Input Descriptors

    [[nodiscard]] auto stream_port_input_count() const noexcept { return stream_port_inputs_.size(); }

    /// @param index The stream port input descriptor index
    [[nodiscard]] auto get_stream_port_input(uint16_t index) const noexcept -> StatusValue<DescriptorStreamPort const*>;

    /// @param desc The stream port input descriptor to add
    [[nodiscard]] auto add_stream_port_input(DescriptorStreamPort const& desc) -> StatusValue<uint16_t>;

    // Stream Port Output Descriptors

    [[nodiscard]] auto stream_port_output_count() const noexcept { return stream_port_outputs_.size(); }

    /// @param index The stream port output descriptor index
    [[nodiscard]] auto get_stream_port_output(uint16_t index) const noexcept -> StatusValue<DescriptorStreamPort const*>;

    /// @param desc The stream port output descriptor to add
    [[nodiscard]] auto add_stream_port_output(DescriptorStreamPort const& desc) -> StatusValue<uint16_t>;

    // Audio Cluster Descriptors

    [[nodiscard]] auto audio_cluster_count() const noexcept { return audio_clusters_.size(); }

    /// @param index The audio cluster descriptor index
    [[nodiscard]] auto get_audio_cluster(uint16_t index) const noexcept -> StatusValue<DescriptorAudioCluster const*>;

    /// @param desc The audio cluster descriptor to add
    [[nodiscard]] auto add_audio_cluster(DescriptorAudioCluster const& desc) -> StatusValue<uint16_t>;

    // Audio Map Descriptors

    [[nodiscard]] auto audio_map_count() const noexcept { return audio_maps_.size(); }

    /// @param index The audio map descriptor index
    [[nodiscard]] auto get_audio_map(uint16_t index) const noexcept -> StatusValue<DescriptorAudioMap const*>;

    /// @param desc The audio map descriptor to add
    [[nodiscard]] auto add_audio_map(DescriptorAudioMap const& desc) -> StatusValue<uint16_t>;

    // Control Descriptors

    [[nodiscard]] auto control_count() const noexcept { return controls_.size(); }

    /// @param index The control descriptor index
    [[nodiscard]] auto get_control(uint16_t index) const noexcept -> StatusValue<DescriptorControl const*>;

    /// @param desc The control descriptor to add
    [[nodiscard]] auto add_control(DescriptorControl const& desc) -> StatusValue<uint16_t>;

    // Generic Descriptor Access (by type and index)

    /// Get raw descriptor bytes by type and index
    /// Returns span to the descriptor data, or error if not found
    /// @param descriptor_type The AEM descriptor type constant
    /// @param descriptor_index The descriptor index within its type
    [[nodiscard]] auto get_descriptor_raw(uint16_t descriptor_type, uint16_t descriptor_index) const noexcept
        -> StatusValue<std::span<uint8_t const>>;

    /// Set an external DescriptorStorage blob for READ_DESCRIPTOR fallback.
    /// When set, get_descriptor_raw() checks the blob first, falling back
    /// to the internal vectors only for descriptor types not found in storage.
    /// The blob must outlive the EntityModel.
    void set_descriptor_storage(atdecc::aem::DescriptorStorage storage) { descriptor_storage_ = storage; }

    /// Check if a DescriptorStorage blob is attached.
    [[nodiscard]] auto has_descriptor_storage() const noexcept -> bool { return descriptor_storage_.has_value(); }
    /// The attached DescriptorStorage blob, or nullptr.
    [[nodiscard]] auto descriptor_storage() const noexcept -> atdecc::aem::DescriptorStorage const*
    {
        return descriptor_storage_.has_value() ? &*descriptor_storage_ : nullptr;
    }

    /// Get configuration for this model
    [[nodiscard]] auto config() const noexcept -> EntityModelConfig const& { return config_; }

    /// Get the memory resource used by this model's descriptor vectors.
    [[nodiscard]] auto memory_resource() const noexcept -> std::pmr::memory_resource* { return mem_resource_; }

  private:
    auto reserve_storage() -> void;

    EntityModelConfig config_;
    std::pmr::memory_resource* mem_resource_;

    // Entity (always exactly one)
    DescriptorEntity entity_{};

    // Descriptor storage
    std::pmr::vector<DescriptorConfiguration> configurations_;
    std::pmr::vector<DescriptorAudioUnit> audio_units_;
    std::pmr::vector<DescriptorStream> stream_inputs_;
    std::pmr::vector<DescriptorStream> stream_outputs_;
    std::pmr::vector<DescriptorJack> jack_inputs_;
    std::pmr::vector<DescriptorJack> jack_outputs_;
    std::pmr::vector<DescriptorAvbInterface> avb_interfaces_;
    std::pmr::vector<DescriptorClockSource> clock_sources_;
    std::pmr::vector<DescriptorClockDomain> clock_domains_;
    std::pmr::vector<DescriptorLocale> locales_;
    std::pmr::vector<DescriptorStrings> strings_;
    std::pmr::vector<DescriptorStreamPort> stream_port_inputs_;
    std::pmr::vector<DescriptorStreamPort> stream_port_outputs_;
    std::pmr::vector<DescriptorAudioCluster> audio_clusters_;
    std::pmr::vector<DescriptorAudioMap> audio_maps_;
    std::pmr::vector<DescriptorControl> controls_;

    // Optional external descriptor storage blob (for READ_DESCRIPTOR fallback)
    std::optional<atdecc::aem::DescriptorStorage> descriptor_storage_;
};

//
// Entity Model Builder
//
/// Fluent builder for constructing EntityModel at startup
class EntityModelBuilder
{
  public:
    /// @param config Storage capacity configuration for each descriptor type
    /// @param memory_resource Memory resource forwarded to the EntityModel.
    ///        nullptr is treated as std::pmr::get_default_resource().
    explicit EntityModelBuilder(EntityModelConfig const& config = {}, std::pmr::memory_resource* memory_resource = nullptr)
        : model_{config, memory_resource}
    {}

    /// Set the entity descriptor
    /// @param desc The entity descriptor
    auto entity(DescriptorEntity const& desc) -> EntityModelBuilder&
    {
        model_.set_entity(desc);
        return *this;
    }

    /// Add a configuration descriptor
    /// @param desc The configuration descriptor to add
    auto configuration(DescriptorConfiguration const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_configuration(desc);
        return *this;
    }

    /// Add an audio unit descriptor
    /// @param desc The audio unit descriptor to add
    auto audio_unit(DescriptorAudioUnit const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_audio_unit(desc);
        return *this;
    }

    /// Add a stream input descriptor
    /// @param desc The stream input descriptor to add
    auto stream_input(DescriptorStream const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_stream_input(desc);
        return *this;
    }

    /// Add a stream output descriptor
    /// @param desc The stream output descriptor to add
    auto stream_output(DescriptorStream const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_stream_output(desc);
        return *this;
    }

    /// Add a jack input descriptor
    /// @param desc The jack input descriptor to add
    auto jack_input(DescriptorJack const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_jack_input(desc);
        return *this;
    }

    /// Add a jack output descriptor
    /// @param desc The jack output descriptor to add
    auto jack_output(DescriptorJack const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_jack_output(desc);
        return *this;
    }

    /// Add an AVB interface descriptor
    /// @param desc The AVB interface descriptor to add
    auto avb_interface(DescriptorAvbInterface const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_avb_interface(desc);
        return *this;
    }

    /// Add a clock source descriptor
    /// @param desc The clock source descriptor to add
    auto clock_source(DescriptorClockSource const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_clock_source(desc);
        return *this;
    }

    /// Add a clock domain descriptor
    /// @param desc The clock domain descriptor to add
    auto clock_domain(DescriptorClockDomain const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_clock_domain(desc);
        return *this;
    }

    /// Add a locale descriptor
    /// @param desc The locale descriptor to add
    auto locale(DescriptorLocale const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_locale(desc);
        return *this;
    }

    /// Add a strings descriptor
    /// @param desc The strings descriptor to add
    auto strings(DescriptorStrings const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_strings(desc);
        return *this;
    }

    /// Add a stream port input descriptor
    /// @param desc The stream port input descriptor to add
    auto stream_port_input(DescriptorStreamPort const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_stream_port_input(desc);
        return *this;
    }

    /// Add a stream port output descriptor
    /// @param desc The stream port output descriptor to add
    auto stream_port_output(DescriptorStreamPort const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_stream_port_output(desc);
        return *this;
    }

    /// Add an audio cluster descriptor
    /// @param desc The audio cluster descriptor to add
    auto audio_cluster(DescriptorAudioCluster const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_audio_cluster(desc);
        return *this;
    }

    /// Add an audio map descriptor
    /// @param desc The audio map descriptor to add
    auto audio_map(DescriptorAudioMap const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_audio_map(desc);
        return *this;
    }

    /// Add a control descriptor
    /// @param desc The control descriptor to add
    auto control(DescriptorControl const& desc) -> EntityModelBuilder&
    {
        (void)model_.add_control(desc);
        return *this;
    }

    /// Build and return the EntityModel (moves ownership)
    [[nodiscard]] auto build() { return std::move(model_); }

  private:
    EntityModel model_;
};

}  // namespace statusbar::nanoavb
