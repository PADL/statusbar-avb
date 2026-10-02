// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for avtp::acf_crc_parse / acf_crc_verify. Consumes
/// untrusted bytes as a CRC message and verifies it against the input
/// itself; must not crash or read out of bounds on any input.

#include "statusbar/avtp/avtp_acf_crc.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto const bytes = std::span<uint8_t const>(data, size);
    auto const view = statusbar::avtp::acf_crc_parse(bytes);
    if (view.has_value()) {
        auto const sink = statusbar::avtp::acf_crc_verify(*view, bytes);
        (void)sink;
    }
    return 0;
}
