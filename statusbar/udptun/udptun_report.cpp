// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/udptun/udptun_report.hpp"

#include "statusbar/ieee/ieee_ethernet_format.hpp"
#include "statusbar/udptun/udptun_stats.hpp"

#include <algorithm>
#include <format>
#include <iterator>

namespace statusbar::udptun {

namespace {

[[nodiscard]] auto compute_loss_pct(SourceState const& s) noexcept -> double
{
    auto const received = s.received_count.load();
    if (received == 0) {
        return 0.0;
    }
    auto const span = static_cast<uint32_t>(s.last_seq.load() - s.first_seq.load()) + 1U;
    if (span == 0) {
        return 0.0;
    }
    auto const expected = static_cast<double>(span);
    return 100.0 * (1.0 - (static_cast<double>(received) / expected));
}

/// Convert nanoseconds to milliseconds, rounded to 0.1 ms.
[[nodiscard]] constexpr auto ns_to_ms_tenth(int64_t ns) noexcept -> double
{
    return static_cast<double>(ns) / 1'000'000.0;
}

struct LossSummary
{
    uint64_t missing{0};
    double loss_pct{0.0};
};

/// For "missing" we want primary-only loss. With redundancy enabled the
/// histogram count includes rescued-redundant samples too, so the snap
/// count would understate primary loss; prefer the tracker's
/// primary_received count when it's available.
[[nodiscard]] auto compute_loss_summary(TxLossStats const& tx, RedundancyDisplayStats const* red, int64_t snap_count) noexcept
    -> LossSummary
{
    auto const received_for_loss =
        (red != nullptr) ? red->primary_received : static_cast<uint64_t>(std::max<int64_t>(0, snap_count));
    uint64_t const missing = (tx.sent_at_cutoff > received_for_loss) ? (tx.sent_at_cutoff - received_for_loss) : 0U;
    double const loss_pct =
        (tx.sent_at_cutoff > 0) ? 100.0 * static_cast<double>(missing) / static_cast<double>(tx.sent_at_cutoff) : 0.0;
    return {.missing = missing, .loss_pct = loss_pct};
}

void format_source_line(std::ostream& out, SourceState const& s, int64_t gptp_ns)
{
    auto hist_snap = s.stats->histogram.snapshot();
    auto ts_snap = s.stats->time_stats.snapshot();

    std::format_to(std::ostreambuf_iterator<char>(out), "  eui64=");
    ieee::Eui64 eui{};
    (void)eui.from_uint64(s.sender_eui64.load());
    ieee::format_to(std::ostreambuf_iterator<char>(out), eui);
    std::format_to(
        std::ostreambuf_iterator<char>(out),
        " count={} loss={:.2f}% min={:.1f}ms mean={:.1f}ms p50={:.1f}ms p95={:.1f}ms p99={:.1f}ms max={:.1f}ms ooo={} dup={}",
        s.received_count.load(),
        compute_loss_pct(s),
        ns_to_ms_tenth(ts_snap.min_ns),
        ns_to_ms_tenth(static_cast<int64_t>(ts_snap.average_ns())),
        ns_to_ms_tenth(percentile_from_histogram(hist_snap, 0.50)),
        ns_to_ms_tenth(percentile_from_histogram(hist_snap, 0.95)),
        ns_to_ms_tenth(percentile_from_histogram(hist_snap, 0.99)),
        ns_to_ms_tenth(ts_snap.max_ns),
        s.out_of_order_count.load(),
        s.duplicate_count.load());
    if (gptp_ns != 0) {
        std::format_to(std::ostreambuf_iterator<char>(out), " gptp_ns={}", gptp_ns);
    }
    std::format_to(std::ostreambuf_iterator<char>(out), "\n");
}

void format_rtt_line(std::ostream& out, LatencyStats const& rtt, TxLossStats const* tx, RedundancyDisplayStats const* red)
{
    auto hist_snap = rtt.histogram.snapshot();
    auto ts_snap = rtt.time_stats.snapshot();
    std::format_to(
        std::ostreambuf_iterator<char>(out),
        "  RTT count={} min={:.1f}ms mean={:.1f}ms p50={:.1f}ms p95={:.1f}ms p99={:.1f}ms max={:.1f}ms",
        ts_snap.count,
        ns_to_ms_tenth(ts_snap.min_ns),
        ns_to_ms_tenth(static_cast<int64_t>(ts_snap.average_ns())),
        ns_to_ms_tenth(percentile_from_histogram(hist_snap, 0.50)),
        ns_to_ms_tenth(percentile_from_histogram(hist_snap, 0.95)),
        ns_to_ms_tenth(percentile_from_histogram(hist_snap, 0.99)),
        ns_to_ms_tenth(ts_snap.max_ns));
    if (tx != nullptr && tx->sent_total > 0) {
        auto const summary = compute_loss_summary(*tx, red, ts_snap.count);
        std::format_to(
            std::ostreambuf_iterator<char>(out),
            " sent={} loss={:.2f}% ({} missing sequence ids)",
            tx->sent_total,
            summary.loss_pct,
            summary.missing);
        if (red != nullptr) {
            double const true_pct = (tx->sent_at_cutoff > 0)
                ? 100.0 * static_cast<double>(red->true_loss) / static_cast<double>(tx->sent_at_cutoff)
                : 0.0;
            std::format_to(
                std::ostreambuf_iterator<char>(out),
                " recovered={} true_loss={:.2f}% ({})",
                red->recovered,
                true_pct,
                red->true_loss);
        }
        if (tx->tx_failures > 0) {
            std::format_to(std::ostreambuf_iterator<char>(out), " tx_failures={}", tx->tx_failures);
        }
    }
    std::format_to(std::ostreambuf_iterator<char>(out), "\n");
}

}  // namespace

void print_live_report(
    std::ostream& out,
    double wall_seconds,
    PerSourceTracker const& tracker,
    LatencyStats const* rtt,
    TxLossStats const* tx,
    RedundancyDisplayStats const* red,
    int64_t gptp_ns)
{
    std::format_to(std::ostreambuf_iterator<char>(out), "[{:9.3f}]\n", wall_seconds);
    for (auto const& s : tracker.sources()) {
        if (!s.in_use.load()) {
            continue;
        }
        format_source_line(out, s, gptp_ns);
    }
    if (rtt != nullptr && rtt->time_stats.snapshot().count > 0) {
        format_rtt_line(out, *rtt, tx, red);
    }
}

void print_final_summary(
    std::ostream& out,
    std::string_view label,
    PerSourceTracker const& tracker,
    LatencyStats const* rtt,
    TxLossStats const* tx,
    RedundancyDisplayStats const* red,
    int64_t gptp_ns)
{
    std::format_to(std::ostreambuf_iterator<char>(out), "\n=== {} final summary ===\n", label);
    for (auto const& s : tracker.sources()) {
        if (!s.in_use.load()) {
            continue;
        }
        format_source_line(out, s, gptp_ns);
        auto snap = s.stats->histogram.snapshot();
        std::format_to(std::ostreambuf_iterator<char>(out), "  histogram (bin_width={} ns):\n", snap.config.bin_width_ns);
        snap.format_to(std::ostreambuf_iterator<char>(out), 40);
    }
    if (rtt != nullptr && rtt->time_stats.snapshot().count > 0) {
        format_rtt_line(out, *rtt, tx, red);
        auto snap = rtt->histogram.snapshot();
        std::format_to(std::ostreambuf_iterator<char>(out), "  RTT histogram (bin_width={} ns):\n", snap.config.bin_width_ns);
        snap.format_to(std::ostreambuf_iterator<char>(out), 40);
    }
    std::format_to(
        std::ostreambuf_iterator<char>(out),
        "dropped_invalid: {}  rx_truncated: {}\n",
        tracker.dropped_invalid(),
        tracker.truncated());
}

}  // namespace statusbar::udptun
