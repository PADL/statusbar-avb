#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file udptun_record_sink.hpp
/// @brief RecordSink — the per-packet recording pipeline, extracted from Session.
///
/// Owns the consumer side of the CSV/colbin recorder: the SPSC ring (drained by a
/// dedicated writer thread), the CSV and/or .colbin writers, and the thread + its
/// stop/error flags. The RX hot path borrows an append handle via sink(); everything
/// else (opening the writers, running the drain loop, flushing every ~1 s, joining on
/// shutdown, surfacing the first write error) lives here as one testable unit instead
/// of being threaded through Session's ~1700-line body.

#include "statusbar/colbin/colbin_writer.hpp"
#include "statusbar/csv/csv.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/status/throw_or_abort.hpp"
#include "statusbar/udptun/udptun_csv_record.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace statusbar::udptun {

class RecordSink
{
  public:
    /// Opens the CSV and/or .colbin writers named by the (possibly empty) paths and,
    /// if either is configured, starts the drain thread. The thread is the last
    /// resource acquired so a throw while opening a writer leaves nothing to join.
    /// CsvWriter throws std::system_error on fopen / header-write failure; colbin
    /// returns a Status, surfaced here as the same failure mode via throw_or_abort.
    RecordSink(std::string_view csv_path, std::string_view bin_path, uint64_t bin_capacity_bytes)
        : enabled_{!csv_path.empty() || !bin_path.empty()}
    {
        if (!csv_path.empty()) {
            csv_writer_.emplace(std::string{csv_path}, kUdpTunCsvHeader);
        }
        if (!bin_path.empty()) {
            statusbar::colbin::WriterConfig const bin_cfg{.max_capacity_bytes = bin_capacity_bytes, .preallocate = true};
            auto w = statusbar::colbin::Writer::create(std::string{bin_path}, udptun_colbin_schema(), bin_cfg);
            if (!w) {
                statusbar::throw_or_abort(w.error(), "colbin::Writer::create failed");
            }
            bin_writer_.emplace(std::move(*w));
        }
        if (enabled_) {
            writer_thread_ = std::thread{[this]() { writer_loop(); }};
        }
    }

    ~RecordSink() { stop(); }

    RecordSink(RecordSink const&) = delete;
    auto operator=(RecordSink const&) -> RecordSink& = delete;
    RecordSink(RecordSink&&) = delete;
    auto operator=(RecordSink&&) -> RecordSink& = delete;

    /// True when at least one per-packet sink (CSV or colbin) is configured; the ring
    /// + writer thread only exist in that case.
    [[nodiscard]] auto enabled() const noexcept -> bool { return enabled_; }

    /// Append handle for RxContext. When disabled the ring pointer is null, so the
    /// hot-path append is a single null check and no work.
    [[nodiscard]] auto sink() noexcept -> CsvSink
    {
        return CsvSink{
            .ring = enabled_ ? &ring_ : nullptr,
            .records_dropped = enabled_ ? &records_dropped_ : nullptr,
        };
    }

    /// Number of records dropped because the SPSC ring was full (writer thread fell
    /// behind disk for longer than the ring depth).
    [[nodiscard]] auto records_dropped() const noexcept -> uint64_t { return records_dropped_.load(); }

    /// Stop the writer thread (draining what remains) and return the first write error
    /// it observed, if any. Idempotent.
    auto flush() -> Status
    {
        stop();
        int const err = writer_error_.load(std::memory_order_relaxed);
        if (err != 0) {
            return failure(std::error_code{err, std::generic_category()});
        }
        return success();
    }

  private:
    /// Drain everything currently in the ring; format and write each via CsvWriter
    /// and/or colbin. Errors set writer_error_ but otherwise let the remaining ring
    /// contents drain (so the producer doesn't start counting drops just because disk
    /// is broken). Returns true if any records were consumed.
    auto drain_ring(CsvScratch& scratch, std::array<std::string_view, 7>& fields) noexcept -> bool
    {
        bool drained = false;
        while (auto const rec = ring_.try_consume()) {
            if (writer_error_.load(std::memory_order_relaxed) == 0) {
                if (csv_writer_) {
                    format_csv_record(*rec, scratch, fields);
                    if (auto const st = csv_writer_->write_row(fields); !st) {
                        writer_error_.store(st.error().value(), std::memory_order_relaxed);
                    }
                }
                if (bin_writer_) {
                    // UdpTunCsvRecord is 48 bytes and matches the colbin schema's row
                    // layout exactly — memcpy directly.
                    std::span<uint8_t const> const row{reinterpret_cast<uint8_t const*>(&*rec), sizeof(UdpTunCsvRecord)};
                    if (auto const st = bin_writer_->write_row(row); !st) {
                        // Mirror CSV error path: surface the errno (system_category) so
                        // flush() reports it.
                        writer_error_.store(st.error().value() != 0 ? st.error().value() : EIO, std::memory_order_relaxed);
                    }
                }
            }
            drained = true;
        }
        return drained;
    }

    /// Flush both sinks if no prior write error. CSV does an fflush; colbin publishes
    /// committed_rows into the header so readers see the latest count.
    void maybe_flush() noexcept
    {
        if (writer_error_.load(std::memory_order_relaxed) != 0) {
            return;
        }
        if (csv_writer_) {
            if (auto const st = csv_writer_->flush(); !st) {
                writer_error_.store(st.error().value(), std::memory_order_relaxed);
                return;
            }
        }
        if (bin_writer_) {
            if (auto const st = bin_writer_->commit(); !st) {
                writer_error_.store(st.error().value() ? st.error().value() : EIO, std::memory_order_relaxed);
            }
        }
    }

    /// One iteration of the writer loop: drain, check stop, maybe flush, sleep if idle.
    /// Returns true when the caller should exit (stop signaled and ring drained).
    auto writer_pass(
        CsvScratch& scratch, std::array<std::string_view, 7>& fields, std::chrono::steady_clock::time_point& last_flush) noexcept
        -> bool
    {
        bool const drained = drain_ring(scratch, fields);
        if (writer_stop_.load(std::memory_order_acquire) && ring_.empty()) {
            return true;
        }
        auto const now = std::chrono::steady_clock::now();
        if (now - last_flush >= std::chrono::seconds{1}) {
            maybe_flush();
            last_flush = now;
        }
        if (!drained) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    /// Writer-thread body. Repeatedly drains the ring, fflushes every ~1 s (so recent
    /// rows survive an unclean process exit), and sleeps 20 ms when idle. Exits once
    /// the producer has signalled stop and the ring is empty. On a write error keeps
    /// draining (without writing) so the producer doesn't start counting drops;
    /// flush() surfaces the captured errno.
    void writer_loop() noexcept
    {
        CsvScratch scratch{};
        std::array<std::string_view, 7> fields{};
        auto last_flush = std::chrono::steady_clock::now();
        while (true) {
            if (writer_pass(scratch, fields, last_flush)) {
                break;
            }
        }
        maybe_flush();  // final fflush before ~CsvWriter fcloses
    }

    /// Idempotent shutdown of the writer thread.
    void stop() noexcept
    {
        if (!writer_thread_.joinable()) {
            return;
        }
        writer_stop_.store(true, std::memory_order_release);
        writer_thread_.join();
        csv_writer_.reset();
        bin_writer_.reset();
    }

    bool enabled_;
    // SPSC ring shared by the RX hot path (producer, via sink()) and writer_thread_
    // (consumer). writer_thread_ owns the open files via csv_writer_ / bin_writer_.
    CsvRing ring_{};
    statusbar::itc::TelemetryCounter<uint64_t> records_dropped_{};
    std::optional<statusbar::csv::CsvWriter> csv_writer_{};
    std::optional<statusbar::colbin::Writer> bin_writer_{};
    std::thread writer_thread_{};
    std::atomic<bool> writer_stop_{false};  // producer -> writer: drain and exit
    std::atomic<int> writer_error_{0};      // first write errno, surfaced by flush()
};

}  // namespace statusbar::udptun
