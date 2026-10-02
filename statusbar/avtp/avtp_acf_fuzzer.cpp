// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for avtp::AcfMessageWalker. Walks untrusted
/// acf_payload_data both plainly and with trailer verification; must not
/// crash or read out of bounds on any input, and must always terminate. We
/// look for OOB/UB via ASan/UBSan.

#include "statusbar/avtp/avtp_acf.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto const payload = std::span<uint8_t const>(data, size);
    statusbar::avtp::AcfMessageWalker plain{payload};
    while (auto const message = plain.next()) {
        auto const sink = message->after_header();
        (void)sink;
    }
    statusbar::avtp::AcfMessageWalker verifying{payload};
    while (auto const message = verifying.next_verified()) {
        auto const sink = message->ok();
        (void)sink;
    }
    return 0;
}
