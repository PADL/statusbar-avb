// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for the avtp AAF receive path. Consumes raw AVTP/AAF wire
/// bytes from an untrusted source and must not crash or read out of bounds on any
/// input. Exercises: aaf_parse_header, aaf_get_audio_payload (the length-bounded
/// payload accessor), and aaf_deserialize_interleaved (the sample decode).

#include "statusbar/avtp/avtp_aaf.hpp"

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

    auto const pdu = aaf_parse_header(packet);

    // The payload accessor is a standalone entry point on the raw packet; fuzz it
    // directly (it does its own bounds check + header parse and clamps to the
    // declared stream_data_length). Consume the span so ASan validates the extent.
    auto const payload = aaf_get_audio_payload(packet);
    consume(payload);

    if (pdu.has_value()) {
        (void)pdu->stream_data_length;
        (void)pdu->sv_version_flags;
        // Drive the sample decode with the header-declared geometry. The decoder
        // guards on payload/output size, so an implausible count safely returns 0.
        std::array<float, 4096> out{};
        (void)aaf_deserialize_interleaved(
            payload, pdu->get_format(), pdu->channels_per_frame(), pdu->sample_count(), std::span<float>(out));
    }
    return 0;
}
