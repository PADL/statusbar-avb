#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// A PHC's lock to another clock's 1 PPS, from the PHC's own external timestamps of it (the pulses ts2phc
/// steers a sink by): PulseLock checks each pulse against the whole second nearest it, and ExttsReader
/// reads one channel's events on a descriptor of its own. See docs/LINUXPTP.md ("Pulse lock").

#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_phc.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace statusbar::ptpclient {

/// What PulseLock counts as a lock.
struct PulseLockParams
{
    int64_t max_offset_ns{25'000};  ///< a pulse further than this from its whole second is off
    /// An event further than this from a whole second is the pulse's other edge (ts2phc stamps both), ignored;
    /// it must be under the pulse's width (ts2phc's default 500 ms).
    int64_t edge_window_ns{250'000'000};
    int64_t max_silence_ns{3'000'000'000};  ///< no pulse for this long (local time) is unlocked
    uint32_t settle_pulses{3};              ///< pulses within max_offset_ns in a row before locked
};

/// A PHC's lock to a 1 PPS sent at the reference clock's whole seconds, from the PHC's timestamps of it.
/// Locked after settle_pulses good pulses in a row; one off pulse, a pulse whose second the reference
/// clock disagrees with, or max_silence_ns without one unlocks. Event-driven: pulse() and expire() only.
class PulseLock
{
  public:
    explicit PulseLock(PulseLockParams const& params = {}) noexcept
        : params_{params}
    {}

    /// An event the PHC stamped at @p event_ns, read at local time @p local_ns, when the reference clock
    /// read @p reference_ns (within a second after the edge). Returns whether locked() changed.
    [[nodiscard]] auto pulse(int64_t event_ns, int64_t reference_ns, int64_t local_ns) noexcept -> bool;
    /// Unlocked, its good run forgotten, when no pulse has come for max_silence_ns by local time
    /// @p local_ns. Returns whether locked() changed.
    [[nodiscard]] auto expire(int64_t local_ns) noexcept -> bool;

    /// Whether the PHC is locked to the pulses: set and cleared only by pulse() and expire().
    [[nodiscard]] auto locked() const noexcept -> bool { return locked_; }
    /// The last pulse's PHC time less its whole second (ns); 0 before one.
    [[nodiscard]] auto offset_ns() const noexcept -> int64_t { return offset_ns_; }
    /// Local time of the last pulse, which expire() checks silence against (0: none yet).
    [[nodiscard]] auto last_pulse_ns() const noexcept -> int64_t { return last_pulse_ns_; }

  private:
    /// Notes the latest pulse checked @p good (false: off, or silence); whether locked() changed.
    [[nodiscard]] auto note_pulse(bool good) noexcept -> bool;

    PulseLockParams params_;
    int64_t offset_ns_{0};
    int64_t last_pulse_ns_{0};
    uint32_t good_in_a_row_{0};
    bool locked_{false};
};

/// One channel's external timestamps of a PHC, read on a descriptor of its own: since Linux 6.7 each
/// open file has its own event queue, masked here to @p channel (PTP_MASK_EN_SINGLE). The channel is
/// its owner's to enable (ts2phc's); this only reads. A reactor Pollable. Linux only.
class ExttsReader final : public net::Pollable
{
  public:
    /// An event: the PHC's time of the edge (ns) and the reactor's time it was read at.
    using OnEvent = sg14::inplace_function<void(int64_t event_ns, int64_t now_ns), 32>;

    /// Open @p phc_path read-only for @p channel's events. Errors: the open's errno; not_supported where
    /// the kernel has no per-reader mask (its one queue is the owner's) or off Linux.
    [[nodiscard]] static auto open(std::string_view phc_path, uint32_t channel, OnEvent on_event)
        -> StatusValue<std::unique_ptr<ExttsReader>>;

    ~ExttsReader() override = default;
    ExttsReader(ExttsReader const&) = delete;
    auto operator=(ExttsReader const&) -> ExttsReader& = delete;

    // -- Pollable --
    /// The PHC's descriptor, readable while its queue holds an event of the channel.
    [[nodiscard]] auto fd() const noexcept -> int override { return phc_.fd(); }
    /// Read the queued events once and hand the channel's to the callback with @p now_ns.
    void on_ready(int64_t now_ns) override;
    /// Nothing: the reader is event-driven.
    void tick(int64_t /*now_ns*/) override {}
    /// True after a read error other than EAGAIN/EINTR (ENODEV: the PHC went; the host may open it again).
    [[nodiscard]] auto finished() const noexcept -> bool override { return finished_; }

  private:
    ExttsReader(PhcDevice phc, uint32_t channel, OnEvent on_event) noexcept;

    PhcDevice phc_;
    uint32_t channel_{0};
    OnEvent on_event_;
    bool finished_{false};
};

}  // namespace statusbar::ptpclient
