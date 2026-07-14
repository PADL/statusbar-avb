// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"

#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_crf.hpp"

#include <system_error>

namespace statusbar::avb_entity {

namespace {

using atdecc::aem::DescriptorStream;

auto stream_specs(atdecc::aem::DescriptorStorage const& storage, uint16_t const config_idx, uint16_t const descriptor_type)
    -> StatusValue<StreamSpecs>
{
    StreamSpecs specs{};
    for (uint16_t index = 0;; ++index) {
        auto const desc = load_descriptor<DescriptorStream>(storage, config_idx, descriptor_type, index);
        if (!desc) {
            break;  // first missing index terminates the table (blob indices are dense)
        }
        if (specs.size() == MAX_ENTITY_STREAMS) {
            // More streams than the data plane composes: refuse loudly rather
            // than silently shaping the entity smaller than its model claims.
            return failure(std::errc::result_out_of_range);
        }
        StreamSpec spec{};
        spec.index = index;
        spec.format_word = desc->current_format.to_uint64();
        spec.format = decode_stream_format(spec.format_word);
        spec.clock_domain_index = desc->clock_domain_index;
        spec.avb_interface_index = desc->avb_interface_index;
        specs.push_back(spec);
    }
    return success(specs);
}

}  // namespace

auto talker_stream_specs(atdecc::aem::DescriptorStorage const& storage, uint16_t const config_idx) -> StatusValue<StreamSpecs>
{
    return stream_specs(storage, config_idx, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT);
}

auto listener_stream_specs(atdecc::aem::DescriptorStorage const& storage, uint16_t const config_idx) -> StatusValue<StreamSpecs>
{
    return stream_specs(storage, config_idx, atdecc::aem::DESCRIPTOR_STREAM_INPUT);
}

auto srp_max_frame_size(StreamFormatFields const& format) noexcept -> uint16_t
{
    switch (format.kind) {
        case StreamKind::aaf: {
            if (format.sample_rate_hz == 0) {
                return 0;
            }
            auto const samples = class_a_samples_per_packet(format.sample_rate_hz) + 1;
            return static_cast<uint16_t>(avtp::aaf_packet_size(format.aaf_format, format.channels, static_cast<uint16_t>(samples)));
        }
        case StreamKind::am824: {
            if (format.sample_rate_hz == 0) {
                return 0;
            }
            auto const samples = class_a_samples_per_packet(format.sample_rate_hz) + 1;
            return static_cast<uint16_t>(
                avtp::am824_packet_size(static_cast<uint8_t>(format.channels), static_cast<uint8_t>(samples)));
        }
        case StreamKind::crf:
            return static_cast<uint16_t>(
                avtp::CrfPdu::HEADER_LENGTH + (static_cast<size_t>(format.crf_timestamps_per_pdu) * avtp::CrfPdu::TIMESTAMP_SIZE));
        case StreamKind::other:
        default:
            return 0;
    }
}

auto common_audio_sample_rate(StreamSpecs const& specs, uint32_t const fallback_hz) -> StatusValue<uint32_t>
{
    uint32_t rate = 0;
    for (auto const& spec : specs) {
        if (spec.format.kind != StreamKind::am824 && spec.format.kind != StreamKind::aaf) {
            continue;
        }
        if (spec.format.sample_rate_hz == 0) {
            return failure(std::errc::invalid_argument);  // unknown rate code in an audio format
        }
        if (rate == 0) {
            rate = spec.format.sample_rate_hz;
        } else if (rate != spec.format.sample_rate_hz) {
            // The data plane runs ONE media clock; mixed audio rates need
            // per-clock-domain timers (kit phase 3) before they can compose.
            return failure(std::errc::invalid_argument);
        }
    }
    if (rate == 0) {
        rate = fallback_hz;
    }
    if (rate == 0) {
        return failure(std::errc::no_message);  // no audio stream and no fallback
    }
    return success(rate);
}

auto find_stream(StreamSpecs const& specs, StreamKind const kind) noexcept -> std::optional<uint16_t>
{
    for (auto const& spec : specs) {
        if (spec.format.kind == kind) {
            return spec.index;
        }
    }
    return std::nullopt;
}

}  // namespace statusbar::avb_entity
