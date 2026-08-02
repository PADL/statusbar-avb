// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_aaf.hpp"

#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::avtp {

auto decode_aaf_sample(std::span<uint8_t const> payload, size_t offset, AafFormat format) noexcept -> float
{
    switch (format) {
        case AafFormat::int_16bit: {
            int16_t const sample =
                static_cast<int16_t>((static_cast<uint16_t>(payload[offset]) << 8) | static_cast<uint16_t>(payload[offset + 1]));
            return aaf_int16_to_float(sample);
        }
        case AafFormat::int_24bit: {
            uint32_t const raw = (static_cast<uint32_t>(payload[offset]) << 16) |
                (static_cast<uint32_t>(payload[offset + 1]) << 8) | static_cast<uint32_t>(payload[offset + 2]);
            int32_t const sample = (raw & 0x800000U) != 0 ? static_cast<int32_t>(raw | 0xFF000000U) : static_cast<int32_t>(raw);
            return aaf_int24_to_float(sample);
        }
        case AafFormat::int_32bit: {
            int32_t const sample = static_cast<int32_t>(
                (static_cast<uint32_t>(payload[offset]) << 24) | (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                (static_cast<uint32_t>(payload[offset + 2]) << 8) | static_cast<uint32_t>(payload[offset + 3]));
            return aaf_int32_to_float(sample);
        }
        case AafFormat::float_32bit: {
            uint32_t const bits = (static_cast<uint32_t>(payload[offset]) << 24) |
                (static_cast<uint32_t>(payload[offset + 1]) << 16) | (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                static_cast<uint32_t>(payload[offset + 3]);
            return bits_to_float(bits);
        }
        default:
            return 0.0F;
    }
}

auto encode_aaf_sample(float sample_float, std::span<uint8_t> payload, size_t offset, AafFormat format) noexcept -> size_t
{
    switch (format) {
        case AafFormat::int_16bit: {
            int16_t const sample = float_to_aaf_int16(sample_float);
            payload[offset] = static_cast<uint8_t>((static_cast<uint16_t>(sample) >> 8) & 0xFFU);
            payload[offset + 1] = static_cast<uint8_t>(static_cast<uint16_t>(sample) & 0xFFU);
            return 2;
        }
        case AafFormat::int_24bit: {
            int32_t const sample = float_to_aaf_int24(sample_float);
            uint32_t const raw = static_cast<uint32_t>(sample) & 0xFFFFFFU;
            payload[offset] = static_cast<uint8_t>((raw >> 16) & 0xFFU);
            payload[offset + 1] = static_cast<uint8_t>((raw >> 8) & 0xFFU);
            payload[offset + 2] = static_cast<uint8_t>(raw & 0xFFU);
            return 3;
        }
        case AafFormat::int_32bit: {
            int32_t const sample = float_to_aaf_int32(sample_float);
            uint32_t const raw = static_cast<uint32_t>(sample);
            payload[offset] = static_cast<uint8_t>((raw >> 24) & 0xFFU);
            payload[offset + 1] = static_cast<uint8_t>((raw >> 16) & 0xFFU);
            payload[offset + 2] = static_cast<uint8_t>((raw >> 8) & 0xFFU);
            payload[offset + 3] = static_cast<uint8_t>(raw & 0xFFU);
            return 4;
        }
        case AafFormat::float_32bit: {
            uint32_t const bits = float_to_bits(sample_float);
            payload[offset] = static_cast<uint8_t>((bits >> 24) & 0xFFU);
            payload[offset + 1] = static_cast<uint8_t>((bits >> 16) & 0xFFU);
            payload[offset + 2] = static_cast<uint8_t>((bits >> 8) & 0xFFU);
            payload[offset + 3] = static_cast<uint8_t>(bits & 0xFFU);
            return 4;
        }
        default:
            return 0;
    }
}

auto aaf_deserialize_interleaved(
    std::span<uint8_t const> const payload,
    AafFormat const format,
    uint16_t const channel_count,
    uint16_t const sample_count,
    std::span<float> const output) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    size_t const bytes_per_sample = aaf_bytes_per_sample(format);
    if (bytes_per_sample == 0) {
        return 0;  // Unknown format
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * bytes_per_sample;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t const total_samples = static_cast<size_t>(channel_count) * sample_count;
    if (output.size() < total_samples) {
        return 0;
    }

    size_t payload_offset = 0;
    size_t output_idx = 0;

    // Samples are interleaved: [ch0_s0, ch1_s0, ..., chN_s0, ch0_s1, ...]
    for (uint16_t s = 0; s < sample_count; ++s) {
        for (uint16_t ch = 0; ch < channel_count; ++ch) {
            output[output_idx] = decode_aaf_sample(payload, payload_offset, format);
            payload_offset += bytes_per_sample;
            ++output_idx;
        }
    }

    return sample_count;
}

auto aaf_deserialize_planar(
    std::span<uint8_t const> const payload,
    AafFormat const format,
    uint16_t const channel_count,
    uint16_t const sample_count,
    std::span<float* const> const channel_buffers,
    size_t const buffer_capacity) noexcept -> size_t
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

    size_t const bytes_per_sample = aaf_bytes_per_sample(format);
    if (bytes_per_sample == 0) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * bytes_per_sample;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t payload_offset = 0;

    for (uint16_t s = 0; s < sample_count; ++s) {
        for (uint16_t ch = 0; ch < channel_count; ++ch) {
            channel_buffers[ch][s] = decode_aaf_sample(payload, payload_offset, format);
            payload_offset += bytes_per_sample;
        }
    }

    return sample_count;
}

auto aaf_deserialize_channel(
    std::span<uint8_t const> const payload,
    AafFormat const format,
    uint16_t const channel_count,
    uint16_t const sample_count,
    uint16_t const channel_index,
    std::span<float> const output) noexcept -> size_t
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

    size_t const bytes_per_sample = aaf_bytes_per_sample(format);
    if (bytes_per_sample == 0) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * bytes_per_sample;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t const frame_size = static_cast<size_t>(channel_count) * bytes_per_sample;

    for (uint16_t s = 0; s < sample_count; ++s) {
        size_t const payload_offset = (s * frame_size) + (channel_index * bytes_per_sample);
        output[s] = decode_aaf_sample(payload, payload_offset, format);
    }

    return sample_count;
}

auto aaf_serialize_interleaved(
    std::span<float const> const input,
    AafFormat const format,
    uint16_t const channel_count,
    uint16_t const sample_count,
    std::span<uint8_t> const payload) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    size_t const bytes_per_sample = aaf_bytes_per_sample(format);
    if (bytes_per_sample == 0) {
        return 0;
    }

    size_t const total_samples = static_cast<size_t>(channel_count) * sample_count;
    if (input.size() < total_samples) {
        return 0;
    }

    size_t const required_payload = total_samples * bytes_per_sample;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t payload_offset = 0;
    size_t input_idx = 0;

    for (uint16_t s = 0; s < sample_count; ++s) {
        for (uint16_t ch = 0; ch < channel_count; ++ch) {
            if (encode_aaf_sample(input[input_idx], payload, payload_offset, format) == 0) {
                return 0;
            }
            payload_offset += bytes_per_sample;
            ++input_idx;
        }
    }

    return payload_offset;
}

auto aaf_serialize_planar(
    std::span<float const* const> const channel_buffers,
    AafFormat const format,
    uint16_t const channel_count,
    uint16_t const sample_count,
    std::span<uint8_t> const payload) noexcept -> size_t
{
    if (channel_count == 0 || sample_count == 0) {
        return 0;
    }

    if (channel_buffers.size() < channel_count) {
        return 0;
    }

    size_t const bytes_per_sample = aaf_bytes_per_sample(format);
    if (bytes_per_sample == 0) {
        return 0;
    }

    size_t const required_payload = static_cast<size_t>(channel_count) * sample_count * bytes_per_sample;
    if (payload.size() < required_payload) {
        return 0;
    }

    size_t payload_offset = 0;

    for (uint16_t s = 0; s < sample_count; ++s) {
        for (uint16_t ch = 0; ch < channel_count; ++ch) {
            if (encode_aaf_sample(channel_buffers[ch][s], payload, payload_offset, format) == 0) {
                return 0;
            }
            payload_offset += bytes_per_sample;
        }
    }

    return payload_offset;
}

auto aaf_create_packet(
    AafStreamContext& ctx,
    uint32_t const avtp_timestamp,
    std::span<float const> const interleaved_audio,
    std::span<uint8_t> const packet_buffer) noexcept -> size_t
{
    size_t const required_size = ctx.packet_size();
    if (packet_buffer.size() < required_size) {
        return 0;
    }

    // Create and initialize header
    AafPdu pdu;
    pdu.init(ctx.stream_id, ctx.format, ctx.sample_rate, ctx.channel_count, ctx.bit_depth);
    pdu.set_sequence_num(ctx.next_sequence_num());
    pdu.set_avtp_timestamp(avtp_timestamp);
    pdu.set_tv(true);
    pdu.set_sp(true);  // sparse_timestamp is always true
    pdu.set_dimensions(ctx.sample_count, ctx.channel_count);

    // Copy header to buffer
    span_store(packet_buffer, pdu);

    // Serialize audio payload
    auto const payload_span = packet_buffer.subspan(AafPdu::HEADER_LENGTH);
    size_t const audio_bytes =
        aaf_serialize_interleaved(interleaved_audio, ctx.format, ctx.channel_count, ctx.sample_count, payload_span);

    if (audio_bytes == 0) {
        return 0;
    }

    return AafPdu::HEADER_LENGTH + audio_bytes;
}

auto aaf_parse_header(std::span<uint8_t const> const packet) noexcept -> std::optional<AafPdu>
{
    if (packet.size() < AafPdu::HEADER_LENGTH) {
        return std::nullopt;
    }

    AafPdu pdu;
    span_load(pdu, packet);
    if (!pdu.is_valid()) {
        return std::nullopt;
    }

    return pdu;
}

auto aaf_get_audio_payload(std::span<uint8_t const> const packet) noexcept -> std::span<uint8_t const>
{
    if (packet.size() <= AafPdu::HEADER_LENGTH) {
        return {};
    }
    size_t const available = packet.size() - AafPdu::HEADER_LENGTH;
    // Bound to the declared stream_data_length, not the raw buffer extent: a wire
    // packet is padded to the 60-byte Ethernet minimum, and returning that padding
    // would decode as bogus samples. Clamp to the buffer so we never over-read.
    auto const pdu = aaf_parse_header(packet);
    size_t const declared = pdu.has_value() ? pdu->get_stream_data_length() : available;
    return packet.subspan(AafPdu::HEADER_LENGTH, std::min(declared, available));
}

auto aaf_format_name(AafFormat const format) noexcept -> std::string_view
{
    switch (format) {
        case AafFormat::user_specified:
            return "User specified";
        case AafFormat::float_32bit:
            return "32-bit float";
        case AafFormat::int_32bit:
            return "32-bit integer";
        case AafFormat::int_24bit:
            return "24-bit integer";
        case AafFormat::int_16bit:
            return "16-bit integer";
        case AafFormat::aes3_32bit:
            return "AES3 32-bit";
        default:
            return "Reserved";
    }
}

auto aaf_sample_rate_name(AafSampleRate const rate) noexcept -> std::string_view
{
    switch (rate) {
        case AafSampleRate::user_specified:
            return "User specified";
        case AafSampleRate::rate_8_khz:
            return "8 kHz";
        case AafSampleRate::rate_16_khz:
            return "16 kHz";
        case AafSampleRate::rate_32_khz:
            return "32 kHz";
        case AafSampleRate::rate_44_1_khz:
            return "44.1 kHz";
        case AafSampleRate::rate_48_khz:
            return "48 kHz";
        case AafSampleRate::rate_88_2_khz:
            return "88.2 kHz";
        case AafSampleRate::rate_96_khz:
            return "96 kHz";
        case AafSampleRate::rate_176_4_khz:
            return "176.4 kHz";
        case AafSampleRate::rate_192_khz:
            return "192 kHz";
        case AafSampleRate::rate_24_khz:
            return "24 kHz";
        default:
            return "Reserved";
    }
}

}  // namespace statusbar::avtp
