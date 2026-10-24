// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for ptpclient::parse_management_message: the bytes a
/// ptp4l management socket answers with (IEEE 1588-2019 Clause 15).

#include "statusbar/ptpclient/ptpclient_management.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    (void)statusbar::ptpclient::parse_management_message(std::span<uint8_t const>(data, size));
    return 0;
}
