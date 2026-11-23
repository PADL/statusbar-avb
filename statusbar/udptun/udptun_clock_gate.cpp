// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_clock_gate.hpp"

#include <format>
#include <iterator>
#include <ostream>

namespace statusbar::udptun {

namespace {

using Out = std::ostreambuf_iterator<char>;

/// A worst-case error in the unit a reader takes in at a glance: integer
/// arithmetic, one decimal.
void print_accuracy(std::ostream& out, int64_t const accuracy_ns)
{
    if (accuracy_ns == ClockReport::UNBOUNDED) {
        std::format_to(Out{out}, "unbounded");
        return;
    }
    bool const in_ms = accuracy_ns >= 1'000'000;
    int64_t const unit_ns = in_ms ? 1'000'000 : 1'000;
    int64_t const tenths = ((accuracy_ns * 10) + (unit_ns - 1)) / unit_ns;  // rounded up: a bound never reads low
    std::format_to(Out{out}, "+/-{}.{} {}", tenths / 10, tenths % 10, in_ms ? "ms" : "us");
}

void print_tenths_of_seconds(std::ostream& out, int64_t const ns)
{
    int64_t const tenths = ns / 100'000'000;
    std::format_to(Out{out}, "{}.{} s", tenths / 10, tenths % 10);
}

void print_limit(std::ostream& out, int64_t const limit_ns)
{
    std::format_to(Out{out}, " (limit ");
    print_accuracy(out, limit_ns);
}

void print_link(std::ostream& out, ClockReport const& report)
{
    std::format_to(Out{out}, " link ");
    print_accuracy(out, report.link_accuracy_ns);
    print_limit(out, report.link_limit_ns);
    std::format_to(Out{out}, ", synced ");
    print_tenths_of_seconds(out, report.link_age_ns);
    std::format_to(Out{out}, " ago)");
}

void print_link_record(std::ostream& out, ClockReport const& report)
{
    std::format_to(Out{out}, " excursions={} outside=", report.link_excursions);
    print_tenths_of_seconds(out, report.link_outside_ns);
    std::format_to(Out{out}, " timeline_changes={}", report.link_timeline_changes);
}

void print_utc(std::ostream& out, ClockReport const& report)
{
    std::format_to(Out{out}, " utc ");
    print_accuracy(out, report.utc_accuracy_ns);
    if (report.utc_limit_ns > 0) {
        print_limit(out, report.utc_limit_ns);
        std::format_to(Out{out}, ") excursions={}", report.utc_excursions);
    }
}

}  // namespace

void print_clock_report(std::ostream& out, ClockReport const& report)
{
    if (!report.has_link && !report.tracks_utc) {
        return;
    }
    std::format_to(Out{out}, "  clock:");
    if (report.has_link) {
        print_link(out, report);
        print_link_record(out, report);
    }
    if (report.tracks_utc) {
        print_utc(out, report);
    }
    if (report.tai_self_corrections > 0) {
        std::format_to(Out{out}, "  tai filter self-corrections {}", report.tai_self_corrections);
    }
    std::format_to(Out{out}, "{}\n", report.usable ? "" : "  NOT USABLE: nothing measured or sent");
}

}  // namespace statusbar::udptun
