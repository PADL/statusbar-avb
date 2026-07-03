// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp_tools/avtp_audio_stream_decoder.hpp"

#include "statusbar/avtp_tools/avtp_channel_interleaver.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <format>

namespace statusbar::avtp_tools {

auto AvtpAudioStreamDecoder::feed(std::span<uint8_t const> payload, uint64_t gptp_now_ns) -> FeedResult
{
    if (!initialized()) {
        if (!initialize_from_payload(payload)) {
            return FeedResult{.status = FeedStatus::format_error};
        }
    }

    if (format_.kind == StreamKind::aaf) {
        if (!payload_is_aaf(payload)) {
            return FeedResult{.status = FeedStatus::wrong_kind};
        }
        return decode_aaf_payload(payload, gptp_now_ns);
    }
    if (!payload_is_am824(payload)) {
        return FeedResult{.status = FeedStatus::wrong_kind};
    }
    return decode_am824_payload(payload, gptp_now_ns);
}

auto AvtpAudioStreamDecoder::initialize_from_payload(std::span<uint8_t const> payload) -> bool
{
    if (payload_is_aaf(payload)) {
        avtp::AafPdu pdu;
        span_load(pdu, payload.subspan(0, avtp::AafPdu::HEADER_LENGTH));
        format_.kind = StreamKind::aaf;
        format_.stream_id = pdu.stream_id().to_uint64();
        return initialize_aaf(pdu);
    }
    if (payload_is_am824(payload)) {
        avtp::Am824Pdu pdu;
        span_load(pdu, payload.subspan(0, avtp::Am824Pdu::HEADER_LENGTH));
        format_.kind = StreamKind::am824_mbla;
        format_.stream_id = pdu.stream_id().to_uint64();
        return initialize_am824(pdu);
    }
    return false;
}

auto AvtpAudioStreamDecoder::initialize_aaf(avtp::AafPdu const& pdu) -> bool
{
    format_.aaf_format = pdu.get_format();
    format_.aaf_rate = pdu.nsr();
    format_.aaf_bit_depth = pdu.get_bit_depth();
    format_.channel_count = pdu.channels_per_frame();
    format_.sample_rate_hz = avtp::aaf_sample_rate_hz(format_.aaf_rate);

    if (format_.sample_rate_hz == 0 || format_.channel_count == 0 || avtp::aaf_bytes_per_sample(format_.aaf_format) == 0) {
        if (diagnostic_) {
            diagnostic_(std::format(
                "first AAF packet has unusable format (format={}, nsr={}, channels={})",
                avtp::aaf_format_name(format_.aaf_format),
                avtp::aaf_sample_rate_name(format_.aaf_rate),
                format_.channel_count));
        }
        format_.kind = StreamKind::unknown;  // un-initialize so caller can retry
        return false;
    }

    aaf_ctx_.emplace(format_.aaf_format, format_.aaf_rate, format_.channel_count, format_.aaf_bit_depth);
    return true;
}

auto AvtpAudioStreamDecoder::initialize_am824(avtp::Am824Pdu const& pdu) -> bool
{
    format_.am824_rate = pdu.sample_rate();
    format_.channel_count = pdu.channel_count();
    format_.sample_rate_hz = avtp::am824_sample_rate_hz(format_.am824_rate);

    if (format_.sample_rate_hz == 0 || format_.channel_count == 0) {
        if (diagnostic_) {
            diagnostic_(std::format(
                "first AM824 packet has unusable format (rate={}, channels={})",
                avtp::am824_sample_rate_name(format_.am824_rate),
                format_.channel_count));
        }
        format_.kind = StreamKind::unknown;
        return false;
    }

    am824_ctx_.emplace(format_.am824_rate, format_.channel_count);
    return true;
}

auto AvtpAudioStreamDecoder::decode_aaf_payload(std::span<uint8_t const> payload, uint64_t gptp_now_ns) -> FeedResult
{
    // Format validity is established by initialize_aaf() — it emplaces
    // aaf_ctx_ and rejects a zero channel_count / bytes-per-sample — and
    // feed() only dispatches here when format_.kind == aaf. Re-check
    // locally so the invariant is explicit and self-defending.
    size_t const bytes_per_frame = size_t{format_.channel_count} * avtp::aaf_bytes_per_sample(format_.aaf_format);
    if (!aaf_ctx_ || bytes_per_frame == 0) {
        return FeedResult{.status = FeedStatus::format_error};
    }

    avtp::AafPdu pdu;
    span_load(pdu, payload.subspan(0, avtp::AafPdu::HEADER_LENGTH));
    auto const raw_audio = payload.subspan(avtp::AafPdu::HEADER_LENGTH);

    // stream_data_length is authoritative (IEEE 1722-2016 7.3.9 / 5.4): any
    // Ethernet min-frame padding or extra bytes past it are NOT audio. Trim to it
    // so the scratch sizing (expected_samples) and aaf_stream_deserialize's own
    // sample count agree. Without this, a padded or crafted frame whose captured
    // payload exceeds stream_data_length makes the deserializer report more frames
    // than the scratch was sized for, and the output span below reads past the
    // heap buffer. min() also covers a truncated frame (declared > captured).
    size_t const valid_bytes = std::min<size_t>(raw_audio.size(), pdu.get_stream_data_length());
    auto const audio = raw_audio.first(valid_bytes);

    size_t const expected_samples = valid_bytes / bytes_per_frame;
    if (expected_samples == 0) {
        return FeedResult{};
    }

    ChannelInterleaver il{scratch_, format_.channel_count};
    il.prepare(expected_samples);

    bool fired = false;
    size_t got_frames = 0;
    uint64_t pts_ns = 0;
    uint64_t const gaps_before = aaf_ctx_->sequence_gaps;

    avtp::aaf_stream_deserialize(
        *aaf_ctx_,
        pdu,
        audio,
        gptp_now_ns,
        [&](uint8_t channel, std::span<float> samples, uint64_t sample_pts_ns, uint64_t /*period*/) {
            fired = true;
            got_frames = samples.size();
            il.deposit(channel, samples);
            if (channel == 0) {
                pts_ns = sample_pts_ns;
            }
        });

    bool const gap_seen = aaf_ctx_->sequence_gaps > gaps_before;
    if (gap_seen) {
        ++sequence_gaps_observed_;
        if (diagnostic_) {
            diagnostic_(std::format("gap at AAF packet #{}: +{}", packets_decoded_ + 1, aaf_ctx_->sequence_gaps - gaps_before));
        }
    }

    if (!fired) {
        return FeedResult{.sequence_gap_observed = gap_seen};
    }

    if (packets_decoded_ == 0) {
        first_gptp_pts_ns_ = pts_ns;
    }

    AvtpAudioSamples const out{
        .interleaved = std::span<float const>(scratch_.data(), got_frames * format_.channel_count),
        .frames = got_frames,
        .gptp_pts_ns = pts_ns,
    };
    if (sink_ && !sink_(out)) {
        return FeedResult{.status = FeedStatus::sink_error, .sequence_gap_observed = gap_seen};
    }

    ++packets_decoded_;
    sample_frames_delivered_ += got_frames;
    return FeedResult{.sequence_gap_observed = gap_seen, .samples_delivered = true};
}

auto AvtpAudioStreamDecoder::decode_am824_payload(std::span<uint8_t const> payload, uint64_t gptp_now_ns) -> FeedResult
{
    // Format validity is established by initialize_am824() — it emplaces
    // am824_ctx_ and rejects a zero channel_count — and feed() only
    // dispatches here when format_.kind == am824_mbla. Re-check locally
    // so the invariant is explicit and self-defending.
    size_t const bytes_per_frame = size_t{format_.channel_count} * avtp::Am824Pdu::BYTES_PER_SAMPLE;
    if (!am824_ctx_ || bytes_per_frame == 0) {
        return FeedResult{.status = FeedStatus::format_error};
    }

    avtp::Am824Pdu pdu;
    span_load(pdu, payload.subspan(0, avtp::Am824Pdu::HEADER_LENGTH));
    auto const audio = payload.subspan(avtp::Am824Pdu::HEADER_LENGTH);

    size_t const expected_samples = audio.size() / bytes_per_frame;
    if (expected_samples == 0) {
        return FeedResult{};
    }

    ChannelInterleaver il{scratch_, format_.channel_count};
    il.prepare(expected_samples);

    bool fired = false;
    size_t got_frames = 0;
    uint64_t pts_ns = 0;
    uint64_t const gaps_before = am824_ctx_->sequence_gaps;

    avtp::am824_deserialize_mbla(
        *am824_ctx_,
        pdu,
        audio,
        gptp_now_ns,
        [&](uint8_t channel, std::span<float> samples, uint64_t sample_pts_ns, uint64_t /*period*/) {
            fired = true;
            got_frames = samples.size();
            il.deposit(channel, samples);
            if (channel == 0) {
                pts_ns = sample_pts_ns;
            }
        });

    bool const gap_seen = am824_ctx_->sequence_gaps > gaps_before;
    if (gap_seen) {
        ++sequence_gaps_observed_;
        if (diagnostic_) {
            diagnostic_(std::format("gap at AM824 packet #{}: +{}", packets_decoded_ + 1, am824_ctx_->sequence_gaps - gaps_before));
        }
    }

    if (!fired) {
        return FeedResult{.sequence_gap_observed = gap_seen};
    }

    if (packets_decoded_ == 0) {
        first_gptp_pts_ns_ = pts_ns;
    }

    AvtpAudioSamples const out{
        .interleaved = std::span<float const>(scratch_.data(), got_frames * format_.channel_count),
        .frames = got_frames,
        .gptp_pts_ns = pts_ns,
    };
    if (sink_ && !sink_(out)) {
        return FeedResult{.status = FeedStatus::sink_error, .sequence_gap_observed = gap_seen};
    }

    ++packets_decoded_;
    sample_frames_delivered_ += got_frames;
    return FeedResult{.sequence_gap_observed = gap_seen, .samples_delivered = true};
}

}  // namespace statusbar::avtp_tools
