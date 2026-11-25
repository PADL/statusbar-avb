// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_children.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>

namespace statusbar::atdecc::aem {

namespace {

/// What every descriptor starts with (IEEE 1722.1-2021 7.2).
struct DescriptorStart
{
    doublet_t descriptor_type{0};
    doublet_t descriptor_index{0};
};

/// @p descriptor read as a T, the octets it lacks zero.
template <class T>
[[nodiscard]] auto loaded(std::span<uint8_t const> const descriptor) -> T
{
    T value{};
    span_load_padded(value, descriptor);
    return value;
}

[[nodiscard]] auto run(uint16_t const type, doublet_t const& base, doublet_t const& count) -> DescriptorRange
{
    return DescriptorRange{.type = type, .base = base.get(), .count = count.get()};
}

/// @p candidates without the empty ones.
template <size_t N>
[[nodiscard]] auto non_empty(std::array<DescriptorRange, N> const& candidates) -> DescriptorRanges
{
    DescriptorRanges ranges;
    for (auto const& range : candidates) {
        if (range.count != 0) {
            (void)ranges.try_push_back(range);
        }
    }
    return ranges;
}

/// AUDIO_UNIT, VIDEO_UNIT and SENSOR_UNIT name the same sixteen runs
/// (Tables 7-5, 7-7 and 7-9).
template <class Unit>
[[nodiscard]] auto unit_children(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    auto const u = loaded<Unit>(descriptor);
    return non_empty(std::array{
        run(DESCRIPTOR_STREAM_PORT_INPUT, u.base_stream_input_port, u.number_of_stream_input_ports),
        run(DESCRIPTOR_STREAM_PORT_OUTPUT, u.base_stream_output_port, u.number_of_stream_output_ports),
        run(DESCRIPTOR_EXTERNAL_PORT_INPUT, u.base_external_input_port, u.number_of_external_input_ports),
        run(DESCRIPTOR_EXTERNAL_PORT_OUTPUT, u.base_external_output_port, u.number_of_external_output_ports),
        run(DESCRIPTOR_INTERNAL_PORT_INPUT, u.base_internal_input_port, u.number_of_internal_input_ports),
        run(DESCRIPTOR_INTERNAL_PORT_OUTPUT, u.base_internal_output_port, u.number_of_internal_output_ports),
        run(DESCRIPTOR_CONTROL, u.base_control, u.number_of_controls),
        run(DESCRIPTOR_SIGNAL_SELECTOR, u.base_signal_selector, u.number_of_signal_selectors),
        run(DESCRIPTOR_MIXER, u.base_mixer, u.number_of_mixers),
        run(DESCRIPTOR_MATRIX, u.base_matrix, u.number_of_matrices),
        run(DESCRIPTOR_SIGNAL_SPLITTER, u.base_splitter, u.number_of_splitters),
        run(DESCRIPTOR_SIGNAL_COMBINER, u.base_combiner, u.number_of_combiners),
        run(DESCRIPTOR_SIGNAL_DEMULTIPLEXER, u.base_demultiplexer, u.number_of_demultiplexers),
        run(DESCRIPTOR_SIGNAL_MULTIPLEXER, u.base_multiplexer, u.number_of_multiplexers),
        run(DESCRIPTOR_SIGNAL_TRANSCODER, u.base_transcoder, u.number_of_transcoders),
        run(DESCRIPTOR_CONTROL_BLOCK, u.base_control_block, u.number_of_control_blocks)});
}

/// STREAM_PORT_INPUT and STREAM_PORT_OUTPUT (Table 7-23). An entity whose
/// mappings are dynamic has number_of_maps 0, and names no map.
[[nodiscard]] auto stream_port_children(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    auto const port = loaded<DescriptorStreamPort>(descriptor);
    return non_empty(std::array{
        run(DESCRIPTOR_CONTROL, port.base_control, port.number_of_controls),
        run(DESCRIPTOR_AUDIO_CLUSTER, port.base_cluster, port.number_of_clusters),
        run(DESCRIPTOR_AUDIO_MAP, port.base_map, port.number_of_maps)});
}

/// A descriptor whose only run is its CONTROLs.
template <class T>
[[nodiscard]] auto control_children(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    auto const owner = loaded<T>(descriptor);
    return non_empty(std::array{run(DESCRIPTOR_CONTROL, owner.base_control, owner.number_of_controls)});
}

[[nodiscard]] auto locale_children(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    auto const locale = loaded<DescriptorLocale>(descriptor);
    return non_empty(std::array{run(DESCRIPTOR_STRINGS, locale.base_strings, locale.number_of_strings)});
}

/// The MATRIX_SIGNALs of a MATRIX's sources (Table 7-45).
[[nodiscard]] auto matrix_children(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    auto const matrix = loaded<DescriptorMatrix>(descriptor);
    return non_empty(std::array{run(DESCRIPTOR_MATRIX_SIGNAL, matrix.base_source, matrix.number_of_sources)});
}

[[nodiscard]] auto ptp_instance_children(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    auto const instance = loaded<DescriptorPtpInstance>(descriptor);
    return non_empty(std::array{
        run(DESCRIPTOR_CONTROL, instance.base_control, instance.number_of_controls),
        run(DESCRIPTOR_PTP_PORT, instance.base_ptp_port, instance.number_of_ptp_ports)});
}

/// A descriptor type that owns others, and how its runs are read.
struct Owner
{
    uint16_t type;
    DescriptorRanges (*children)(std::span<uint8_t const>);
};

constexpr std::array OWNERS{
    Owner{.type = DESCRIPTOR_AUDIO_UNIT, .children = unit_children<DescriptorAudioUnit>},
    Owner{.type = DESCRIPTOR_VIDEO_UNIT, .children = unit_children<DescriptorVideoUnit>},
    Owner{.type = DESCRIPTOR_SENSOR_UNIT, .children = unit_children<DescriptorSensorUnit>},
    Owner{.type = DESCRIPTOR_STREAM_PORT_INPUT, .children = stream_port_children},
    Owner{.type = DESCRIPTOR_STREAM_PORT_OUTPUT, .children = stream_port_children},
    Owner{.type = DESCRIPTOR_EXTERNAL_PORT_INPUT, .children = control_children<DescriptorExternalPort>},
    Owner{.type = DESCRIPTOR_EXTERNAL_PORT_OUTPUT, .children = control_children<DescriptorExternalPort>},
    Owner{.type = DESCRIPTOR_INTERNAL_PORT_INPUT, .children = control_children<DescriptorInternalPort>},
    Owner{.type = DESCRIPTOR_INTERNAL_PORT_OUTPUT, .children = control_children<DescriptorInternalPort>},
    Owner{.type = DESCRIPTOR_JACK_INPUT, .children = control_children<DescriptorJack>},
    Owner{.type = DESCRIPTOR_JACK_OUTPUT, .children = control_children<DescriptorJack>},
    Owner{.type = DESCRIPTOR_AVB_INTERFACE, .children = control_children<DescriptorAvbInterface>},
    Owner{.type = DESCRIPTOR_CONTROL_BLOCK, .children = control_children<DescriptorControlBlock>},
    Owner{.type = DESCRIPTOR_LOCALE, .children = locale_children},
    Owner{.type = DESCRIPTOR_MATRIX, .children = matrix_children},
    Owner{.type = DESCRIPTOR_PTP_INSTANCE, .children = ptp_instance_children},
};

/// How far a stream port's cluster and map types lie from the audio ones
/// under a unit of @p unit_type: the three of each are consecutive, in the
/// order of the three units.
[[nodiscard]] auto unit_type_offset(uint16_t const unit_type) noexcept -> uint16_t
{
    return is_unit_descriptor(unit_type) ? static_cast<uint16_t>(unit_type - DESCRIPTOR_AUDIO_UNIT) : uint16_t{0};
}

static_assert(DESCRIPTOR_VIDEO_UNIT - DESCRIPTOR_AUDIO_UNIT == DESCRIPTOR_VIDEO_CLUSTER - DESCRIPTOR_AUDIO_CLUSTER);
static_assert(DESCRIPTOR_SENSOR_UNIT - DESCRIPTOR_AUDIO_UNIT == DESCRIPTOR_SENSOR_CLUSTER - DESCRIPTOR_AUDIO_CLUSTER);
static_assert(DESCRIPTOR_VIDEO_UNIT - DESCRIPTOR_AUDIO_UNIT == DESCRIPTOR_VIDEO_MAP - DESCRIPTOR_AUDIO_MAP);
static_assert(DESCRIPTOR_SENSOR_UNIT - DESCRIPTOR_AUDIO_UNIT == DESCRIPTOR_SENSOR_MAP - DESCRIPTOR_AUDIO_MAP);

}  // namespace

auto child_descriptor_ranges(std::span<uint8_t const> const descriptor) -> DescriptorRanges
{
    uint16_t const type = loaded<DescriptorStart>(descriptor).descriptor_type.get();
    auto const* const owner = std::ranges::find(OWNERS, type, &Owner::type);
    return owner != OWNERS.end() ? owner->children(descriptor) : DescriptorRanges{};
}

auto range_under_unit(DescriptorRange range, uint16_t const unit_type) noexcept -> DescriptorRange
{
    if (range.type == DESCRIPTOR_AUDIO_CLUSTER || range.type == DESCRIPTOR_AUDIO_MAP) {
        range.type = static_cast<uint16_t>(range.type + unit_type_offset(unit_type));
    }
    return range;
}

void DescriptorChildWalk::descriptor(std::span<uint8_t const> const descriptor, Visit const& visit)
{
    auto const start = loaded<DescriptorStart>(descriptor);
    Key const self{start.descriptor_type.get(), start.descriptor_index.get()};
    auto const ranges = child_descriptor_ranges(descriptor);
    if (is_unit_descriptor(self.first)) {
        unit(self.first, ranges, visit);
    } else if (is_stream_port_descriptor(self.first)) {
        stream_port(self, ranges, visit);
    } else {
        tell_ranges(ranges, DESCRIPTOR_AUDIO_UNIT, visit);
    }
}

void DescriptorChildWalk::unit(uint16_t const unit_type, DescriptorRanges const& ranges, Visit const& visit)
{
    for (auto const& range : ranges) {
        if (is_stream_port_descriptor(range.type)) {
            adopt_ports(range, unit_type, visit);
        }
        tell_range(range, visit);
    }
}

void DescriptorChildWalk::stream_port(Key const& port, DescriptorRanges const& ranges, Visit const& visit)
{
    auto const unit_of_port = port_units_.find(port);
    if (unit_of_port == port_units_.end()) {
        waiting_ports_[port] = ranges;
        return;
    }
    tell_ranges(ranges, unit_of_port->second, visit);
}

void DescriptorChildWalk::adopt_ports(DescriptorRange const& ports, uint16_t const unit_type, Visit const& visit)
{
    each_index(ports, [&](uint16_t const index) {
        Key const port{ports.type, index};
        port_units_[port] = unit_type;
        tell_waiting_port(port, unit_type, visit);
    });
}

void DescriptorChildWalk::tell_waiting_port(Key const& port, uint16_t const unit_type, Visit const& visit)
{
    auto const waiting = waiting_ports_.find(port);
    if (waiting == waiting_ports_.end()) {
        return;
    }
    auto const ranges = waiting->second;
    waiting_ports_.erase(waiting);
    tell_ranges(ranges, unit_type, visit);
}

void DescriptorChildWalk::tell_ranges(DescriptorRanges const& ranges, uint16_t const unit_type, Visit const& visit)
{
    for (auto const& range : ranges) {
        tell_range(range_under_unit(range, unit_type), visit);
    }
}

void DescriptorChildWalk::tell_range(DescriptorRange const& range, Visit const& visit)
{
    each_index(range, [&](uint16_t const index) { visit(range.type, index); });
}

auto DescriptorChildWalk::take_step() noexcept -> bool
{
    if (steps_left_ == 0) {
        exhausted_ = true;
        return false;
    }
    --steps_left_;
    return true;
}

}  // namespace statusbar::atdecc::aem
