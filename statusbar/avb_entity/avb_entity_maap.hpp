#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// MAAP dynamic stream-address acquisition for kit entities (refactor
/// phase A — extracted from AvbEntityAudioIO / AvbEntityToneGenerator,
/// which carried near-identical copies).
///
/// Owns the TX-address-readiness gate the talkers consult per tick and the
/// publication of each acquired per-stream dest MAC into BOTH the ACMP
/// stream model (CONNECT_TX / GET_STREAM_INFO / MSRP) and the live
/// TalkerStreams slot. setup() opens the MAAP socket and puts the protocol
/// handler on the reactor, which owns it from then on; port_operational()
/// — the kit's Link Up, IEEE 1722-2025 B.3.5.9 — begins the acquisition and,
/// after a later Link Up, re-probes the range held; stop() releases the
/// range (Table B.7 Release!) and retires the reactor port. Acquisition
/// runs asynchronously on the reactor; the talker stays gated until the
/// block is defended. A socket failure falls back to the configured static
/// MACs (gate released, nothing on the reactor) so a MAAP-less network
/// still streams.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

/// B.3.6.1: the address draw is seeded from the least-significant octets of
/// the sum of the station's MAC address and its local real-time clock, so a
/// station picks a different sequence of ranges on every boot and two
/// stations never share one. Never 0, which MaapHandler reads as "the MAC
/// alone".
[[nodiscard]] inline auto maap_seed(ieee::Eui48 const& our_mac, int64_t const realtime_ns) noexcept -> uint64_t
{
    uint64_t const sum = our_mac.to_uint64() + static_cast<uint64_t>(realtime_ns);
    return sum == 0 ? 1U : sum;
}

class MaapAddressAcquirer
{
  public:
    using ReadyFn = statusbar::sg14::inplace_function<void(sm::TimePoint), 32>;

    /// Set up the acquisition of one MAAP block covering the given talker
    /// stream indices (block offset = position in the list). Nothing goes on
    /// the wire until port_operational(). @p our_mac drives the conflict
    /// tie-break and, with the real-time clock read here, seeds the address
    /// draw (B.3.6.1); @p on_addresses_ready fires on the reactor thread each
    /// time the block is (re)defended and every dest MAC has been published —
    /// entities re-declare their MSRP Talker Advertise there so listeners
    /// reserve against the MAAP address, told the reactor time it fires at.
    /// Call once from start() in "maap" address mode; in static mode simply
    /// never call it (ready() defaults true). A set-up still standing from
    /// an earlier call is retired first, at @p now.
    [[nodiscard]] auto setup(
        net::MessageReactor& reactor,
        std::string_view const interface_name,
        ieee::Eui48 const& our_mac,
        nanoavb::NanoAvbComponents& components,
        TalkerStreams& talker,
        std::span<uint16_t const> const stream_indices,
        logging::Logger const log,
        ReadyFn on_addresses_ready,
        sm::TimePoint const now = sm::Clock::now()) -> Status
    {
        begin_setup(log, stream_indices, std::move(on_addresses_ready), now);
        // One allocation covering all talker streams; the seed spreads our
        // initial draw per station and per boot.
        auto handler = std::make_unique<avtp::MaapHandler>(our_mac, maap_seed(our_mac, realtime_now_ns()));
        handler->prepare(static_cast<uint16_t>(indices_.size()));
        wire_handler(*handler, components, talker);
        return install(reactor, interface_name, std::move(handler));
    }

    /// B.3.5.9 PortOperational!: the port can transmit and receive AVTPDUs
    /// (again), told the control-plane time. The first one begins acquiring;
    /// a later one re-probes the range held, the TX gate closed until it is
    /// defended again. Nothing without a set-up (static mode, or the socket
    /// fallback).
    void port_operational(sm::TimePoint const now)
    {
        if (net_ != nullptr) {
            net_->handler().port_operational(to_ns(now));
        }
    }

    /// Table B.7 Release!: the range is no longer in use nor defended
    /// (B.3.5.2); the reactor port retires, and the reactor drops it on its
    /// next pass. The TX gate closes: there is no address to send to until
    /// the next setup() and Link Up. Nothing without a set-up.
    void stop(sm::TimePoint const now)
    {
        if (net_ == nullptr) {
            return;
        }
        net_->retire(to_ns(now));
        net_ = nullptr;
        ready_.store(false, std::memory_order_release);
    }

    /// TX-address readiness the talker gate consults per tick: true in
    /// static mode / after socket-failure fallback / once the block is
    /// defended; false while (re)acquiring and after stop().
    [[nodiscard]] auto ready() const noexcept -> bool { return ready_.load(std::memory_order_acquire); }

    /// The MAAP handler's next probe or announce deadline (reactor ns
    /// timebase), or nullopt without a set-up or with its timers idle. For
    /// the loop that drives the reactor: it bounds its poll timeout by this so
    /// the PDU goes out inside its interval. Reactor thread only.
    [[nodiscard]] auto next_deadline_ns() const noexcept -> std::optional<int64_t>
    {
        return net_ != nullptr ? net_->next_deadline_ns() : std::nullopt;
    }

    /// Whether setup() could not open the MAAP socket and the talkers kept
    /// the static per-kind destination MACs (so several talkers of one kind
    /// share a MAC). Reactor thread, after setup().
    [[nodiscard]] auto fell_back_to_static() const noexcept -> bool { return fell_back_to_static_; }

  private:
    [[nodiscard]] static auto to_ns(sm::TimePoint const tp) noexcept -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }

    /// The local real-time clock, for the seed (B.3.6.1): read once per
    /// set-up, a control-plane edge.
    [[nodiscard]] static auto realtime_now_ns() noexcept -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void begin_setup(
        logging::Logger const log, std::span<uint16_t const> const stream_indices, ReadyFn on_ready, sm::TimePoint const now)
    {
        log_ = log;
        stop(now);  // a set-up still standing is retired first
        indices_.clear();
        for (auto const idx : stream_indices) {
            (void)indices_.try_push_back(idx);
        }
        on_addresses_ready_ = std::move(on_ready);
    }

    /// Hand the handler to the reactor, which owns it from here: acquisition
    /// then runs ASYNCHRONOUSLY, from the Link Up on, and the handler keeps
    /// the block defended. A socket that will not open means no MAAP: the
    /// static dest MACs stand and the TX gate stays open.
    [[nodiscard]] auto install(
        net::MessageReactor& reactor, std::string_view const interface_name, std::unique_ptr<avtp::MaapHandler> handler) -> Status
    {
        auto net_handler = std::make_unique<nanoavb::MaapNetHandler>(interface_name, std::move(handler));
        if (!net_handler->valid()) {
            log_->warning("maap: socket open failed; using static stream dest MACs");
            fell_back_to_static_ = true;
            ready_.store(true, std::memory_order_release);  // fall back: do not gate
            return {};
        }
        // Gate stream TX until the block is defended. ADP/gPTP/SRP/ACMP are
        // unaffected — they keep running in the reactor; only the talker waits.
        ready_.store(false, std::memory_order_release);
        net_ = net_handler.get();
        reactor.add(std::move(net_handler));
        return {};
    }

    void wire_handler(avtp::MaapHandler& handler, nanoavb::NanoAvbComponents& components, TalkerStreams& talker)
    {
        handler.set_on_acquired(
            [this, &components, &talker](ieee::Eui48 const& block_start, uint16_t /*count*/, int64_t const now_ns) {
                // Reactor thread, once the block is defended. Publish each
                // per-stream dest MAC, then release the gate so the media
                // thread's acquire-load sees the new MACs before transmitting.
                publish_addresses(components, talker, block_start);
                ready_.store(true, std::memory_order_release);
                log_->status("maap: acquired {} stream address(es) from {:012x}", indices_.size(), block_start.to_uint64());
                if (on_addresses_ready_) {
                    // The MAAP handler's timeline is the reactor's (net::monotonic_ns,
                    // std::chrono::steady_clock): the same clock as sm::Clock.
                    on_addresses_ready_(sm::TimePoint{std::chrono::nanoseconds{now_ns}});
                }
            });
        handler.set_on_lost([this](ieee::Eui48 const& start, uint16_t /*count*/) {
            // A conflict, a Link Up re-probing the range, or the stop: the TX
            // gate closes until a block is defended (on_acquired re-opens it).
            ready_.store(false, std::memory_order_release);
            log_->warning("maap: no longer defending {:012x}; stream TX gated", start.to_uint64());
        });
    }

    void publish_addresses(nanoavb::NanoAvbComponents& components, TalkerStreams& talker, ieee::Eui48 const& block_start)
    {
        for (size_t pos = 0; pos < indices_.size(); ++pos) {
            uint16_t const idx = indices_[pos];
            ieee::Eui48 const dest = avtp::maap_block_address(block_start, static_cast<uint16_t>(pos));
            if (auto const* s = components.acmp_talker.get_stream(idx); s != nullptr) {
                (void)components.acmp_talker.configure_stream(idx, s->stream_id, dest);
            }
            if (auto* slot = talker.slot_for(idx); slot != nullptr) {
                slot->dest_mac = dest;
            }
        }
    }

    /// The port on the reactor, which owns it; null when nothing is set up.
    /// Valid until stop() retires it: the reactor drops only finished ports.
    nanoavb::MaapNetHandler* net_{nullptr};
    std::atomic<bool> ready_{true};
    bool fell_back_to_static_{false};
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices_{};
    ReadyFn on_addresses_ready_{};
    std::optional<logging::Logger> log_{};  ///< set by setup (Logger is not default-constructible)
};

}  // namespace statusbar::avb_entity
