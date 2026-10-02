// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for avtp::acf_checksum_parse / acf_checksum_verify.
/// Consumes untrusted bytes as a Checksum message and verifies it against
/// the input itself; must not crash or read out of bounds on any input.

#include "statusbar/avtp/avtp_acf_checksum.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto const bytes = std::span<uint8_t const>(data, size);
    auto const message = statusbar::avtp::acf_checksum_parse(bytes);
    if (message.has_value()) {
        auto const sink = statusbar::avtp::acf_checksum_verify(*message, bytes);
        (void)sink;
    }
    return 0;
}
