// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"

#include "statusbar/atdecc/atdecc_aem_control_values.hpp"

#include <string_view>

namespace statusbar::atdecc::aem {

auto DescriptorControl::value_details_length() const noexcept -> size_t
{
    size_t const len = control_value_details_length(control_value_type.get(), number_of_values.get());
    return len > MAX_VALUE_DETAILS ? MAX_VALUE_DETAILS : len;
}

auto descriptor_type_name(uint16_t const type) noexcept -> std::string_view
{
    switch (type) {
        case DESCRIPTOR_ENTITY:
            return "Entity";
        case DESCRIPTOR_CONFIGURATION:
            return "Configuration";
        case DESCRIPTOR_AUDIO_UNIT:
            return "Audio Unit";
        case DESCRIPTOR_VIDEO_UNIT:
            return "Video Unit";
        case DESCRIPTOR_SENSOR_UNIT:
            return "Sensor Unit";
        case DESCRIPTOR_STREAM_INPUT:
            return "Stream Input";
        case DESCRIPTOR_STREAM_OUTPUT:
            return "Stream Output";
        case DESCRIPTOR_JACK_INPUT:
            return "Jack Input";
        case DESCRIPTOR_JACK_OUTPUT:
            return "Jack Output";
        case DESCRIPTOR_AVB_INTERFACE:
            return "AVB Interface";
        case DESCRIPTOR_CLOCK_SOURCE:
            return "Clock Source";
        case DESCRIPTOR_MEMORY_OBJECT:
            return "Memory Object";
        case DESCRIPTOR_LOCALE:
            return "Locale";
        case DESCRIPTOR_STRINGS:
            return "Strings";
        case DESCRIPTOR_STREAM_PORT_INPUT:
            return "Stream Port Input";
        case DESCRIPTOR_STREAM_PORT_OUTPUT:
            return "Stream Port Output";
        case DESCRIPTOR_EXTERNAL_PORT_INPUT:
            return "External Port Input";
        case DESCRIPTOR_EXTERNAL_PORT_OUTPUT:
            return "External Port Output";
        case DESCRIPTOR_INTERNAL_PORT_INPUT:
            return "Internal Port Input";
        case DESCRIPTOR_INTERNAL_PORT_OUTPUT:
            return "Internal Port Output";
        case DESCRIPTOR_AUDIO_CLUSTER:
            return "Audio Cluster";
        case DESCRIPTOR_VIDEO_CLUSTER:
            return "Video Cluster";
        case DESCRIPTOR_SENSOR_CLUSTER:
            return "Sensor Cluster";
        case DESCRIPTOR_AUDIO_MAP:
            return "Audio Map";
        case DESCRIPTOR_VIDEO_MAP:
            return "Video Map";
        case DESCRIPTOR_SENSOR_MAP:
            return "Sensor Map";
        case DESCRIPTOR_CONTROL:
            return "Control";
        case DESCRIPTOR_SIGNAL_SELECTOR:
            return "Signal Selector";
        case DESCRIPTOR_MIXER:
            return "Mixer";
        case DESCRIPTOR_MATRIX:
            return "Matrix";
        case DESCRIPTOR_MATRIX_SIGNAL:
            return "Matrix Signal";
        case DESCRIPTOR_SIGNAL_SPLITTER:
            return "Signal Splitter";
        case DESCRIPTOR_SIGNAL_COMBINER:
            return "Signal Combiner";
        case DESCRIPTOR_SIGNAL_DEMULTIPLEXER:
            return "Signal Demultiplexer";
        case DESCRIPTOR_SIGNAL_MULTIPLEXER:
            return "Signal Multiplexer";
        case DESCRIPTOR_SIGNAL_TRANSCODER:
            return "Signal Transcoder";
        case DESCRIPTOR_CLOCK_DOMAIN:
            return "Clock Domain";
        case DESCRIPTOR_CONTROL_BLOCK:
            return "Control Block";
        case DESCRIPTOR_TIMING:
            return "Timing";
        case DESCRIPTOR_PTP_INSTANCE:
            return "PTP Instance";
        case DESCRIPTOR_PTP_PORT:
            return "PTP Port";
        case DESCRIPTOR_INVALID:
            return "Invalid";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::atdecc::aem
