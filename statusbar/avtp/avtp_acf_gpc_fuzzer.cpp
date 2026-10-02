// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for avtp::acf_gpc_parse. Consumes untrusted bytes as an
/// ACF_GPC message; must not crash or read out of bounds on any input. May
/// legally return std::nullopt. We look for OOB/UB via ASan/UBSan.

#include "statusbar/avtp/avtp_acf_gpc.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto const bytes = std::span<uint8_t const>(data, size);
    auto const view = statusbar::avtp::acf_gpc_parse(bytes);
    if (view.has_value()) {
        auto const sink = view->payload.size() + view->padded_payload.size();
        (void)sink;
    }
    return 0;
}
