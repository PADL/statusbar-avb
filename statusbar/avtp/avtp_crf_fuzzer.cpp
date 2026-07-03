// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for the avtp CRF receive path. Consumes raw AVTP wire bytes
/// from an untrusted source; must not crash or read out of bounds on any input.
/// Exercises crf_parse_header, crf_get_timestamp_data (bounds to crf_data_length),
/// and crf_get_timestamp across every declared timestamp index.

#include "statusbar/avtp/avtp_crf.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace {
volatile uint64_t g_sink = 0;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    using namespace statusbar::avtp;
    auto const packet = std::span<uint8_t const>(data, size);

    auto const pdu = crf_parse_header(packet);

    // Payload accessor bounds the timestamp block to the declared crf_data_length.
    auto const ts_data = crf_get_timestamp_data(packet);

    if (pdu.has_value()) {
        auto const sink = *pdu;
        (void)sink;
        // Read every declared timestamp; the accessor bounds-checks each index.
        auto const n = pdu->timestamp_count();
        for (uint16_t i = 0; i < n; ++i) {
            if (auto const ts = crf_get_timestamp(ts_data, i)) {
                g_sink = g_sink ^ *ts;
            }
        }
    }
    return 0;
}
