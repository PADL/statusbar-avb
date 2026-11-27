// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avb_entity/avb_entity_upkeep.hpp"

#include "statusbar/net/net_ethernet_port.hpp"
#include "statusbar/net/net_link_monitor.hpp"

#include <memory>

namespace statusbar::avb_entity {

namespace {

/// The reactor-clock instant @p now_ns as the kit's TimePoint.
auto time_point_at(int64_t const now_ns) -> sm::TimePoint
{
    return sm::TimePoint{std::chrono::nanoseconds{now_ns}};
}

}  // namespace

EntityUpkeep::EntityUpkeep(AvbEntityKit& kit, logging::LogSink& sink)
    : kit_{kit}
    , sink_{sink}
    , last_state_{kit.host().state_string()}
{
    (void)collector_.add(kit.host().ctl_log_channel());
    (void)collector_.add(kit.host().media_log_channel());
    if (auto* handlers = kit.host().net_handlers(); handlers != nullptr) {
        had_grandmaster_ = handlers->gptp_handler().has_grandmaster();
    }
}

void EntityUpkeep::attach(
    net::MessageReactor& reactor, AvbEntityKit& kit, std::string_view const interface_name, sm::TimePoint const now)
{
    // As run_entity_main_loop: the initial link-up synchronously, so the
    // supervisor leaves Down -> Init before any gPTP announce is processed;
    // then the interface link state continuously.
    kit.on_link_up(now);
    reactor.add(std::make_unique<net::LinkMonitor>(interface_name, [kit = &kit](bool const up, int64_t const now_ns) {
        auto const time = sm::TimePoint{std::chrono::nanoseconds{now_ns}};
        if (up) {
            kit->on_link_up(time);
        } else {
            kit->on_link_down(time);
        }
    }));
    if (auto* handlers = kit.host().net_handlers(); handlers != nullptr) {
        handlers->gptp_handler().set_callbacks(nanoavb::GptpAnnounceCallbacks{
            .grandmaster_id_changed =
                [kit = &kit](int64_t const now_ns, gptp::ClockIdentity const& grandmaster, gptp::AnnounceMessage const& announce) {
                    kit->host().ctl_log().status(
                        "gPTP: grandmaster {:016x} (priority1={}, priority2={})",
                        grandmaster.get(),
                        announce.grandmaster_priority1.get(),
                        announce.grandmaster_priority2.get());
                    kit->notify_grandmaster_changed(grandmaster);  // ADP through the host; AVTP tu (Milan v1.2 B.1.1)
                    kit->on_gptp_announce(time_point_at(now_ns), true);
                },
            .grandmaster_lost =
                [kit = &kit](int64_t /*now_ns*/) {
                    kit->host().ctl_log().warning("gPTP: no Announce within the receipt timeout: grandmaster lost");
                    kit->notify_grandmaster_lost();  // AVTP tu until a grandmaster returns
                }});
    }
}

void EntityUpkeep::tick(int64_t const now_ns)
{
    auto const now = time_point_at(now_ns);
    notify_state(now);
    watch_gptp(now);
    kit_.end_cycle(now_ns);  // the counters' interval and the admission deadline
    drain();
}

void EntityUpkeep::notify_state(sm::TimePoint const now)
{
    state_since_ = state_since_.value_or(now);  // the first tick starts the first stay
    if (auto const state = kit_.host().state_string(); state != last_state_) {
        kit_.host().ctl_log().status("entity: {} -> {}", logging::static_str(last_state_), logging::static_str(state));
        last_state_ = state;
        state_since_ = now;
        lock_timeout_reported_ = false;
    }
}

void EntityUpkeep::watch_gptp(sm::TimePoint const now)
{
    auto const sm_now = now;
    // Grandmaster regained under an unchanged identity; a changed identity is
    // handled by the announce callback. Here, so one thread drives the SM/MSRP.
    if (auto* handlers = kit_.host().net_handlers(); handlers != nullptr) {
        bool const has_grandmaster = handlers->gptp_handler().has_grandmaster();
        if (has_grandmaster && !had_grandmaster_) {
            kit_.on_gptp_announce(sm_now, true);
        }
        had_grandmaster_ = has_grandmaster;
    }
    // The gPTP watchdog reports once per stay in Init (the entity keeps
    // advertising and waiting for a lock either way).
    if (last_state_ == "Init" && !lock_timeout_reported_ && now - state_since_.value_or(now) > GPTP_LOCK_TIMEOUT) {
        kit_.on_timeout(sm_now);
        lock_timeout_reported_ = true;
    }
}

}  // namespace statusbar::avb_entity
