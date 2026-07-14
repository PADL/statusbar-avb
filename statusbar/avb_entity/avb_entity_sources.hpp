#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_sources.hpp
/// @brief Ready-made per-stream TX sources for the Entity Construction Kit.
///
/// Small factories returning StreamRenderFn callbacks (see
/// avb_entity_stream_spec.hpp) a developer can bind to a stream by blob
/// symbol or index: `entity.set_render("aaf_out", make_white_noise_render(0.1F))`.
/// All are RT-safe: state lives inside the inplace_function capture, no
/// allocation, no blocking. The built-in default source (when nothing is
/// bound) is the entity's own generator — e.g. the tone generator's
/// white-key sine bank.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"

#include <cstdint>
#include <span>

namespace statusbar::avb_entity {

/// Digital silence.
[[nodiscard]] inline auto make_silence_render() -> StreamRenderFn
{
    return [](std::span<float> audio, uint32_t /*frames*/, uint64_t /*first_index*/, uint64_t /*pts_ns*/) {
        for (auto& s : audio) {
            s = 0.0F;
        }
    };
}

/// White noise at @p amplitude (peak, 0..1), all channels uncorrelated.
/// xorshift32 PRNG: cheap enough for the media thread, spectrally flat for
/// test purposes (not dithered/gaussian — a measurement source, not mastering).
[[nodiscard]] inline auto make_white_noise_render(float const amplitude, uint32_t const seed = 0x6A09E667U) -> StreamRenderFn
{
    return [state = (seed != 0 ? seed : 1U),
            amplitude](std::span<float> audio, uint32_t /*frames*/, uint64_t /*first_index*/, uint64_t /*pts_ns*/) mutable {
        for (auto& s : audio) {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            // Map to [-1, 1): 2^-31 scale of the signed reinterpretation.
            s = amplitude * (static_cast<float>(static_cast<int32_t>(state)) * 0x1.0p-31F);
        }
    };
}

}  // namespace statusbar::avb_entity
