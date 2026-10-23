// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_format.hpp"

namespace statusbar::atdecc::aem {

namespace {

/// Minimum desc_data size required to SAFELY format a descriptor whose wire form
/// has a count-driven variable trailer (CONFIGURATION descriptor_counts,
/// AUDIO_UNIT sampling_rates, STREAM stream_formats, AUDIO/VIDEO/SENSOR_MAP
/// mappings, CONTROL value_details, CLOCK_DOMAIN clock_sources).
///
/// The descriptor is reinterpret_cast directly onto the received bytes, so the
/// per-descriptor formatters read trailer entries whose COUNT is an
/// ATTACKER-CONTROLLED field. Validating only the fixed header
/// (descriptor_min_lengths) lets a crafted READ_DESCRIPTOR response with a large
/// count make those formatters read past the buffer (info leak on the live path,
/// heap-overflow read on the pcap path). We therefore require the trailer the
/// formatter will actually read to be fully present.
///
/// The trailer begins at the struct's fixed `LENGTH` (the inline array offset)
/// and the formatter reads exactly `used_*().size_bytes()` (the count CLAMPED to
/// the inline capacity). When that is 0 there is no trailer access at all, so the
/// fixed-header (min_length) check alone suffices — this is what allows a valid
/// zero-trailer STREAM sent at MINIMUM_LENGTH (< LENGTH). The count field itself
/// lives within min_length, so reading it here (via used_*()) is in-bounds.
/// Returns 0 when no trailer bytes are read (fixed descriptors, or empty trailer).
[[nodiscard]] auto descriptor_trailer_end(ParsedDescriptor const* d, uint16_t desc_type) -> size_t
{
    auto const trailer_end = [](size_t length, size_t used_bytes) -> size_t {
        return used_bytes == 0 ? size_t{0} : length + used_bytes;
    };
    switch (desc_type) {
        case DESCRIPTOR_CONFIGURATION:
            return trailer_end(DescriptorConfiguration::LENGTH, d->data.configuration.used_descriptor_counts().size_bytes());
        case DESCRIPTOR_AUDIO_UNIT:
            return trailer_end(DescriptorAudioUnit::LENGTH, d->data.audio_unit.used_sampling_rates().size_bytes());
        // STREAM_INPUT/OUTPUT: the formats sit at formats_offset (132 in 2013,
        // 138 in 2021), not at a fixed LENGTH; validated by
        // stream_descriptor_trailers() in parse_descriptor instead.
        case DESCRIPTOR_AUDIO_MAP:
            return trailer_end(DescriptorAudioMap::LENGTH, d->data.audio_map.used_mappings().size_bytes());
        case DESCRIPTOR_VIDEO_MAP:
            return trailer_end(DescriptorVideoMap::LENGTH, d->data.video_map.used_mappings().size_bytes());
        case DESCRIPTOR_SENSOR_MAP:
            return trailer_end(DescriptorSensorMap::LENGTH, d->data.sensor_map.used_mappings().size_bytes());
        case DESCRIPTOR_CONTROL:
            return trailer_end(DescriptorControl::LENGTH, d->data.control.value_details_bytes().size_bytes());
        case DESCRIPTOR_CLOCK_DOMAIN:
            return trailer_end(DescriptorClockDomain::LENGTH, d->data.clock_domain.used_clock_sources().size_bytes());
        default:
            return 0;  // fixed-size descriptor: min_length check is sufficient
    }
}

}  // namespace

auto parse_descriptor(std::span<uint8_t const> desc_data) -> StatusValue<ParsedDescriptor const*>
{
    // Need at least 4 bytes for descriptor_type and descriptor_index
    if (desc_data.size() < 4) {
        return failure(BufferError::insufficient_data);
    }

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const* parsed_descriptor = reinterpret_cast<ParsedDescriptor const*>(desc_data.data());
    uint16_t const desc_type = static_cast<uint16_t>(parsed_descriptor->data.common.descriptor_type);

    // Look up minimum length from table
    size_t min_length = 4;  // Default: just need type and index
    if (desc_type < NUM_DESCRIPTOR_TYPES && descriptor_min_lengths[desc_type] > 0) {
        min_length = descriptor_min_lengths[desc_type];
    }

    if (desc_data.size() < min_length) {
        return failure(BufferError::insufficient_data);
    }

    // For descriptors with a count-driven variable trailer, require the trailer
    // the formatter will actually read to be fully present, so the
    // attacker-controlled count cannot drive a read past desc_data. Safe to
    // evaluate now: the count field is within min_length.
    if (size_t const trailer_end = descriptor_trailer_end(parsed_descriptor, desc_type); desc_data.size() < trailer_end) {
        return failure(BufferError::insufficient_data);
    }
    if ((desc_type == DESCRIPTOR_STREAM_INPUT || desc_type == DESCRIPTOR_STREAM_OUTPUT) &&
        !stream_descriptor_trailers(desc_data).has_value()) {
        return failure(BufferError::insufficient_data);
    }

    return success(parsed_descriptor);
}

}  // namespace statusbar::atdecc::aem

namespace statusbar::atdecc {

auto parse_aem(uint16_t const cmd, bool const is_response, std::span<uint8_t const> payload)
    -> StatusValue<aem::ParsedAemPayload const*>
{
    // Unknown command codes have no min-length table entry, so the payload can't
    // be validated -- refuse rather than hand out a ParsedAemPayload pointer over
    // an unchecked (possibly zero-length) buffer.
    if (cmd >= aem::NUM_AEM_COMMANDS) {
        return failure(BufferError::insufficient_data);
    }

    // Look up minimum length from the table for this known command.
    auto const& lengths = aem_command_min_lengths[cmd];
    size_t const min_length = is_response && lengths.response > 0 ? lengths.response : lengths.command;
    if (min_length > 0 && payload.size() < min_length) {
        return failure(BufferError::insufficient_data);
    }

    // Return pointer to payload data as ParsedAemPayload
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return success(reinterpret_cast<aem::ParsedAemPayload const*>(payload.data()));
}

}  // namespace statusbar::atdecc
