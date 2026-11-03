#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_stream_rx_sink.hpp
/// @brief StreamRxAudioSink — where a listener delivers received stream audio.
///
/// The AVB listener's job is to receive a connected talker's stream, validate
/// it, and hand the accepted audio payload to *something*. Today that something
/// is the inter-site WAN tunnel (EntityUdptunBridge), but it could equally be a
/// local audio device, a file recorder, or a DSP graph. This interface is the
/// seam that keeps the listener from depending on any one of those concretely:
/// the listener calls on_listener_audio() for every accepted packet and the sink
/// decides what to do with it (including dropping it -- e.g. the tunnel ignores
/// audio for a stream it is not configured to source).

#include "statusbar/avtp/avtp_aaf.hpp"

#include <cstdint>
#include <span>

namespace statusbar::avb_entity {

/// Wire format of the payload bytes handed to the sink. The sink may need to
/// transcode: AM824 carries 61883-6 MBLA quadlets ([0x40 label][24-bit]); AAF
/// carries interleaved int32 PCM directly.
enum class StreamAudioFormat : uint8_t
{
    am824_mbla,  ///< IEC 61883-6 MBLA quadlets (needs MBLA->int32 transcode for int32 sinks)
    aaf_int32,   ///< AAF interleaved int32 PCM (already linear)
};

/// Consumes the audio payload of one accepted listener stream packet. Called on
/// the reactor/RX thread (the same thread the listener runs on). Implementations
/// must be cheap and non-blocking on that path.
struct StreamRxAudioSink
{
    /// @param stream_index  STREAM_INPUT index the packet arrived on (0=AM824, 1=AAF).
    /// @param fmt           wire format of @p payload.
    /// @param payload       the AVTP audio payload (header already stripped).
    virtual void on_listener_audio(uint16_t stream_index, StreamAudioFormat fmt, std::span<uint8_t const> payload) = 0;

    StreamRxAudioSink() = default;
    StreamRxAudioSink(StreamRxAudioSink const&) = default;
    StreamRxAudioSink(StreamRxAudioSink&&) = default;
    auto operator=(StreamRxAudioSink const&) -> StreamRxAudioSink& = default;
    auto operator=(StreamRxAudioSink&&) -> StreamRxAudioSink& = default;
    virtual ~StreamRxAudioSink() = default;
};

/// One accepted listener stream packet with its timing: what a sink needs to
/// place the frames on the media clock without decoding them.
struct StreamRxPacket
{
    uint16_t stream_index{0};                             ///< STREAM_INPUT index the packet arrived on
    StreamAudioFormat fmt{StreamAudioFormat::aaf_int32};  ///< wire family: AM824 or AAF (any AAF sample format)
    /// AAF: the stream's sample format (int16/24/32, float32, AES3); AM824: user_specified.
    avtp::AafFormat aaf_format{avtp::AafFormat::user_specified};
    uint8_t bytes_per_sample{0};       ///< AAF: per aaf_format; AM824: 4 (one quadlet)
    std::span<uint8_t const> payload;  ///< exactly frames x channels x bytes_per_sample bytes, as received
    uint32_t frames{0};                ///< samples per channel in @p payload
    uint16_t channels{0};              ///< channels per frame
    uint64_t pts_ns{0};                ///< reconstructed 64-bit presentation time of the first frame
    bool pts_valid{false};             ///< false until the stream has carried a valid timestamp
    bool tu{false};                    ///< AVTP tu: the talker marked its timestamp uncertain
    bool mr{false};                    ///< AVTP mr: the talker's media clock restart toggle, as received
};

/// The timestamped StreamRxAudioSink: each accepted packet's raw payload, 64-bit AVTP presentation
/// time and dimensions, on the RX thread with the same discipline. With only this sink attached,
/// AAF is not decoded to float.
struct StreamRxTimedAudioSink
{
    virtual void on_listener_packet(StreamRxPacket const& packet) = 0;

    StreamRxTimedAudioSink() = default;
    StreamRxTimedAudioSink(StreamRxTimedAudioSink const&) = default;
    StreamRxTimedAudioSink(StreamRxTimedAudioSink&&) = default;
    auto operator=(StreamRxTimedAudioSink const&) -> StreamRxTimedAudioSink& = default;
    auto operator=(StreamRxTimedAudioSink&&) -> StreamRxTimedAudioSink& = default;
    virtual ~StreamRxTimedAudioSink() = default;
};

}  // namespace statusbar::avb_entity
