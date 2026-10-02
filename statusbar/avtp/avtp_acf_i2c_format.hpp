#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// format_to() overload for the ACF_I2C message. Split from
/// avtp_acf_i2c.hpp so consumers that only need the data structures do not
/// pay the compile-time cost of <format>.

#include "statusbar/avtp/avtp_acf_i2c.hpp"

#include <format>

namespace statusbar::avtp {

/// Format an AcfI2cMessageView to an output iterator
template <typename OutputIt>
auto format_to(OutputIt out, AcfI2cMessageView const& view) -> OutputIt
{
    out = std::format_to(
        out,
        "ACF_I2C pad_bits={} mtv={:d} i2c_bus_id={} i2c_code={} trr={:d} transaction_num=0x{:x} evt={} exception_code={} "
        "i2c_data=0x{:x}",
        view.fixed.pad_bits(),
        view.fixed.mtv(),
        view.fixed.i2c_bus_id(),
        view.fixed.i2c_code(),
        view.fixed.trr(),
        view.fixed.transaction_num(),
        view.fixed.evt(),
        view.fixed.exception_code(),
        view.fixed.i2c_data());
    if (view.fixed.mtv()) {
        out = std::format_to(out, " message_timestamp={}", view.fixed.get_message_timestamp());
    }
    return std::format_to(out, " payload={} octets", view.payload.size());
}

}  // namespace statusbar::avtp
