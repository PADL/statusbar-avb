// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_am824.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace statusbar::avtp {

auto am824_deserialize_interleaved(
    std::span<uint8_t const> payload, uint8_t channel_count, uint8_t sample_count, std::span<float> output) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * Am824Pdu::BYTES_PER_SAMPLE;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t const total_samples = static_cast<size_t>(channel_count) * sample_count;
    if (output.size() < total_samples) {
        return 0;
    }

    size_t payload_offset = 0;
    size_t output_idx = 0;

    // Data blocks are organized as: [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ch1_s1, ...]
    for (uint8_t s = 0; s < sample_count; ++s) {
        for (uint8_t ch = 0; ch < channel_count; ++ch) {
            // Read 4-byte AM824 quadlet in network byte order
            uint32_t const quadlet = (static_cast<uint32_t>(payload[payload_offset]) << 24) |
                (static_cast<uint32_t>(payload[payload_offset + 1]) << 16) |
                (static_cast<uint32_t>(payload[payload_offset + 2]) << 8) | static_cast<uint32_t>(payload[payload_offset + 3]);

            int32_t const sample = parse_am824_quadlet(quadlet);
            output[output_idx] = am824_sample_to_float(sample);

            payload_offset += 4;
            ++output_idx;
        }
    }

    return sample_count;
}

auto am824_deserialize_planar(
    std::span<uint8_t const> payload,
    uint8_t channel_count,
    uint8_t sample_count,
    std::span<float* const> channel_buffers,
    size_t buffer_capacity) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    if (channel_buffers.size() < channel_count) {
        return 0;
    }

    if (buffer_capacity < sample_count) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * Am824Pdu::BYTES_PER_SAMPLE;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t payload_offset = 0;

    // Data blocks are organized as: [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ch1_s1, ...]
    for (uint8_t s = 0; s < sample_count; ++s) {
        for (uint8_t ch = 0; ch < channel_count; ++ch) {
            // Read 4-byte AM824 quadlet in network byte order
            uint32_t const quadlet = (static_cast<uint32_t>(payload[payload_offset]) << 24) |
                (static_cast<uint32_t>(payload[payload_offset + 1]) << 16) |
                (static_cast<uint32_t>(payload[payload_offset + 2]) << 8) | static_cast<uint32_t>(payload[payload_offset + 3]);

            int32_t const sample = parse_am824_quadlet(quadlet);
            channel_buffers[ch][s] = am824_sample_to_float(sample);

            payload_offset += 4;
        }
    }

    return sample_count;
}

auto am824_deserialize_channel(
    std::span<uint8_t const> payload,
    uint8_t channel_count,
    uint8_t sample_count,
    uint8_t channel_index,
    std::span<float> output) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    if (channel_index >= channel_count) {
        return 0;
    }

    if (output.size() < sample_count) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * Am824Pdu::BYTES_PER_SAMPLE;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t const block_size = static_cast<size_t>(channel_count) * Am824Pdu::BYTES_PER_SAMPLE;

    for (uint8_t s = 0; s < sample_count; ++s) {
        size_t const payload_offset = (s * block_size) + (channel_index * Am824Pdu::BYTES_PER_SAMPLE);

        // Read 4-byte AM824 quadlet in network byte order
        uint32_t const quadlet = (static_cast<uint32_t>(payload[payload_offset]) << 24) |
            (static_cast<uint32_t>(payload[payload_offset + 1]) << 16) | (static_cast<uint32_t>(payload[payload_offset + 2]) << 8) |
            static_cast<uint32_t>(payload[payload_offset + 3]);

        int32_t const sample = parse_am824_quadlet(quadlet);
        output[s] = am824_sample_to_float(sample);
    }

    return sample_count;
}

auto am824_serialize_interleaved(
    std::span<float const> input, uint8_t channel_count, uint8_t sample_count, std::span<uint8_t> payload) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    size_t const total_samples = static_cast<size_t>(channel_count) * sample_count;
    if (input.size() < total_samples) {
        return 0;
    }

    size_t const required_payload = total_samples * Am824Pdu::BYTES_PER_SAMPLE;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t payload_offset = 0;
    size_t input_idx = 0;

    for (uint8_t s = 0; s < sample_count; ++s) {
        for (uint8_t ch = 0; ch < channel_count; ++ch) {
            int32_t const sample = float_to_am824_sample(input[input_idx]);
            uint32_t const quadlet = create_am824_quadlet(sample);

            // Write in network byte order
            payload[payload_offset] = static_cast<uint8_t>((quadlet >> 24) & 0xFFU);
            payload[payload_offset + 1] = static_cast<uint8_t>((quadlet >> 16) & 0xFFU);
            payload[payload_offset + 2] = static_cast<uint8_t>((quadlet >> 8) & 0xFFU);
            payload[payload_offset + 3] = static_cast<uint8_t>(quadlet & 0xFFU);

            payload_offset += 4;
            ++input_idx;
        }
    }

    return payload_offset;
}

auto am824_serialize_planar(
    std::span<float const* const> channel_buffers, uint8_t channel_count, uint8_t sample_count, std::span<uint8_t> payload) noexcept
    -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    if (channel_buffers.size() < channel_count) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * Am824Pdu::BYTES_PER_SAMPLE;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t payload_offset = 0;

    for (uint8_t s = 0; s < sample_count; ++s) {
        for (uint8_t ch = 0; ch < channel_count; ++ch) {
            int32_t const sample = float_to_am824_sample(channel_buffers[ch][s]);
            uint32_t const quadlet = create_am824_quadlet(sample);

            // Write in network byte order
            payload[payload_offset] = static_cast<uint8_t>((quadlet >> 24) & 0xFFU);
            payload[payload_offset + 1] = static_cast<uint8_t>((quadlet >> 16) & 0xFFU);
            payload[payload_offset + 2] = static_cast<uint8_t>((quadlet >> 8) & 0xFFU);
            payload[payload_offset + 3] = static_cast<uint8_t>(quadlet & 0xFFU);

            payload_offset += 4;
        }
    }

    return payload_offset;
}

auto am824_create_packet(
    StreamId const& stream_id,
    uint8_t sequence_num,
    uint32_t avtp_timestamp,
    Am824SampleRate sample_rate,
    uint8_t data_block_count,
    std::span<float const> interleaved_audio,
    uint8_t channel_count,
    uint8_t sample_count,
    std::span<uint8_t> packet_buffer) noexcept -> size_t
{
    size_t const required_size = am824_packet_size(channel_count, sample_count);
    if (packet_buffer.size() < required_size) {
        return 0;
    }

    // Create and initialize header
    Am824Pdu pdu;
    pdu.init(stream_id, channel_count, sample_rate);
    pdu.set_sequence_num(sequence_num);
    pdu.set_avtp_timestamp(avtp_timestamp);
    pdu.set_tv(true);
    pdu.set_data_block_count(data_block_count);
    pdu.set_dimensions(sample_count, channel_count);

    // Copy header to buffer
    span_store(packet_buffer, pdu);

    // Serialize audio payload
    auto const payload_span = packet_buffer.subspan(Am824Pdu::HEADER_LENGTH);
    size_t const audio_bytes = am824_serialize_interleaved(interleaved_audio, channel_count, sample_count, payload_span);

    if (audio_bytes == 0) {
        return 0;
    }

    return Am824Pdu::HEADER_LENGTH + audio_bytes;
}

auto am824_parse_header(std::span<uint8_t const> packet) noexcept -> std::optional<Am824Pdu>
{
    if (packet.size() < Am824Pdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    Am824Pdu pdu;
    span_load(pdu, packet);
    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto am824_get_audio_payload(std::span<uint8_t const> packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= Am824Pdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - Am824Pdu::HEADER_LENGTH;
    // Bound the payload to the declared audio length (stream_data_length minus the
    // CIP header), not the raw buffer extent: a wire packet is padded to the 60-byte
    // Ethernet minimum, and returning that padding would decode as bogus samples.
    // Still clamp to what the buffer actually holds so we never over-read.
    auto const pdu = am824_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->audio_payload_length() : available;
    return packet.subspan(Am824Pdu::HEADER_LENGTH, std::min(declared, available));
}

auto am824_sample_rate_name(Am824SampleRate const rate) noexcept -> char const*
{
    switch (rate) {
        case Am824SampleRate::rate_32_khz:
            return "32 kHz";
        case Am824SampleRate::rate_44_1_khz:
            return "44.1 kHz";
        case Am824SampleRate::rate_48_khz:
            return "48 kHz";
        case Am824SampleRate::rate_88_2_khz:
            return "88.2 kHz";
        case Am824SampleRate::rate_96_khz:
            return "96 kHz";
        case Am824SampleRate::rate_176_4_khz:
            return "176.4 kHz";
        case Am824SampleRate::rate_192_khz:
            return "192 kHz";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::avtp
