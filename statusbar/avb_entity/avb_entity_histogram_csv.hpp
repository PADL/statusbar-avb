#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_histogram_csv.hpp
/// @brief One AtomicHistogram snapshot to a CSV file, for the exit-time
/// dumps the entity tools offer (media-timer wake error and duration, the
/// listener's presentation margin per input).

#include "statusbar/stats/stats_atomic_histogram.hpp"

#include <fstream>
#include <print>
#include <string>

namespace statusbar::avb_entity {

/// Dump an AtomicHistogram snapshot as bin_low_ns,bin_high_ns,count CSV.
[[nodiscard]] inline auto write_histogram_csv(std::string const& path, stats::AtomicHistogram<128>::Snapshot const& hist) -> bool
{
    std::ofstream out{path};
    if (!out) {
        std::println(stderr, "warning: cannot open {} for writing", path);
        return false;
    }
    out << "bin_low_ns,bin_high_ns,count\n" << ',' << hist.config.low_ns << ',' << hist.underflow << '\n';
    for (size_t i = 0; i < hist.num_bins; ++i) {
        out << hist.bin_low(i) << ',' << hist.bin_high(i) << ',' << hist.bins[i] << '\n';
    }
    out << hist.config.high_ns << ',' << ',' << hist.overflow << '\n';
    return static_cast<bool>(out);
}

}  // namespace statusbar::avb_entity
