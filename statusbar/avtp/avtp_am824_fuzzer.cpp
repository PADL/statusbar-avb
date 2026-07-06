// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for the avtp AM824 receive path. Consumes raw AVTP wire bytes
/// from an untrusted source; must not crash or read out of bounds on any input.
/// Exercises: am824_parse_header, am824_get_audio_payload (the length-bounded
/// payload accessor), and am824_deserialize_interleaved (the sample decode).

#include "statusbar/avtp/avtp_am824.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {
uint8_t volatile g_sink = 0;
void consume(std::span<uint8_t const> const s) noexcept
{
    uint8_t x = 0;
    for (auto const b : s) {
        x = static_cast<uint8_t>(x ^ b);
    }
    g_sink = static_cast<uint8_t>(g_sink ^ x);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    using namespace statusbar::avtp;
    auto const packet = std::span<uint8_t const>(data, size);

    auto const pdu = am824_parse_header(packet);

    // Fuzz the payload accessor directly on the raw packet: it bounds the returned
    // span to the declared audio length (stream_data_length - CIP header), clamped
    // to the buffer. Consume the span so ASan validates the extent.
    auto const payload = am824_get_audio_payload(packet);
    consume(payload);

    if (pdu.has_value()) {
        auto const sink = *pdu;
        (void)sink;
        // Drive the sample decode with the header-declared geometry; the decoder
        // guards on payload/output size, so an implausible count safely returns 0.
        std::array<float, 4096> out{};
        (void)am824_deserialize_interleaved(payload, pdu->channel_count(), pdu->sample_count(), std::span<float>(out));
    }
    return 0;
}
