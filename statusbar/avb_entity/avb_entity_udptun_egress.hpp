#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_egress.hpp
/// @brief Pure helpers for the inter-site tunnel egress -> local-talker path.
///
/// First seam of the EntityUdptunBridge extraction (god-object phase 2): the
/// egress fill used to write de-tunneled PCM straight into AvbEntityAudioIO's
/// shared audio_buffer_. This pulls the int32-network-byte-order -> float
/// conversion into a buffer-agnostic, unit-testable function that takes the
/// destination as an out-span, so the later egress move doesn't reach into
/// entity state directly.

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::avb_entity {

/// De-interleave `samples` frames of interleaved 32-bit big-endian (network
/// byte order) PCM from `pcm_be` into normalized float in `out`, channel-major
/// interleaved. Each int32 maps to [-1, 1) via /2^31.
///
/// `pcm_be` must hold at least samples*channels*4 bytes and `out` at least
/// samples*channels floats (the egress fill guarantees both); on a short span
/// it writes nothing rather than reading/writing out of bounds.
inline void udptun_egress_deinterleave_to_float(
    std::span<uint8_t const> pcm_be, std::span<float> out, size_t const channels, size_t const samples)
{
    size_t const count = samples * channels;
    if (pcm_be.size() < count * 4 || out.size() < count) {
        return;
    }
    for (size_t n = 0; n < count; ++n) {
        ieee::quadlet_t sample{0};
        span_load(sample, pcm_be.subspan(n * 4, 4));
        out[n] = static_cast<float>(static_cast<int32_t>(sample.get())) / 2147483648.0F;
    }
}

}  // namespace statusbar::avb_entity
