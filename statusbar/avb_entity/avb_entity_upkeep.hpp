#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// EntityUpkeep: a kit entity's reactor-side upkeep (link, gPTP, log drain,
/// timeouts, admission) as a Pollable, for a host whose main thread is a
/// net::MessageReactor. Reactor thread; see docs/AVB_ENTITY_KIT.md.

#include "statusbar/avb_entity/avb_entity_kit.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/logging/logging_collector.hpp"
#include "statusbar/logging/logging_sink.hpp"
#include "statusbar/net/net_message_reactor.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace statusbar::avb_entity {

class EntityUpkeep final : public net::Pollable
{
  public:
    /// How long the supervisor may sit in Init (no gPTP lock) before on_timeout.
    static constexpr auto GPTP_LOCK_TIMEOUT = std::chrono::seconds{10};
    static constexpr size_t MAX_LOG_CHANNELS = 4;

    /// Upkeep for @p kit, draining its ctl and media log channels to @p sink
    /// (both must outlive this).
    EntityUpkeep(AvbEntityKit& kit, logging::LogSink& sink);

    /// Drain another log channel too (false when full).
    [[nodiscard]] auto add_log_channel(logging::LogChannelBase& channel) noexcept -> bool { return collector_.add(channel); }

    /// The first link-up, a LinkMonitor on @p interface_name and the
    /// grandmaster-changed callback, for @p kit on @p reactor. Once, after the
    /// kit has started (the gPTP handler exists from then on).
    static void attach(net::MessageReactor& reactor, AvbEntityKit& kit, std::string_view interface_name);

    [[nodiscard]] auto fd() const noexcept -> int override { return -1; }
    void on_ready(int64_t /*now_ns*/) override {}
    void tick(int64_t now_ns) override;
    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

    /// Write out what has been logged (e.g. once more at shutdown).
    void drain() { (void)collector_.poll(sink_); }

  private:
    AvbEntityKit& kit_;
    logging::LogSink& sink_;
    logging::LogCollector<MAX_LOG_CHANNELS> collector_;
    std::string_view last_state_;
    std::chrono::steady_clock::time_point state_since_;
    bool lock_timeout_reported_{false};  ///< the gPTP watchdog fired in this stay in Init
    bool had_grandmaster_{false};
};

}  // namespace statusbar::avb_entity
