// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for the avtp AEF receive path. Consumes raw AVTP/AEF wire
/// bytes from an untrusted source; must not crash or read out of bounds on any
/// input. Exercises aef_{continuous,discrete}_parse_header plus the length-bounded
/// encrypted-payload accessors.

#include "statusbar/avtp/avtp_aef.hpp"

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

    auto const cont = aef_continuous_parse_header(packet);
    if (cont.has_value()) {
        auto const sink = *cont;
        (void)sink;
    }
    // Payload accessor bounds the span to the declared stream_data_length.
    consume(aef_continuous_get_encrypted_payload(packet));

    auto const disc = aef_discrete_parse_header(packet);
    if (disc.has_value()) {
        auto const sink = *disc;
        (void)sink;
    }
    // Payload accessor bounds the span to the declared control_data_length.
    consume(aef_discrete_get_encrypted_payload(packet));
    return 0;
}
