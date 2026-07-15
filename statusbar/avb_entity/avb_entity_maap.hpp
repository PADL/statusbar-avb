#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// MAAP dynamic stream-address acquisition for kit entities (refactor
/// phase A — extracted from AvbEntityAudioIO / AvbEntityToneGenerator,
/// which carried near-identical copies).
///
/// Owns the MaapHandler, the TX-address-readiness gate the talkers consult
/// per tick, and the publication of each acquired per-stream dest MAC into
/// BOTH the ACMP stream model (CONNECT_TX / GET_STREAM_INFO / MSRP) and the
/// live TalkerStreams slot. Acquisition runs asynchronously on the reactor;
/// the talker stays gated until the block is defended. A socket failure
/// falls back to the configured static MACs (gate released, handler
/// dropped) so a MAAP-less network still streams.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

class MaapAddressAcquirer
{
  public:
    /// Start the asynchronous acquisition of one MAAP block covering the
    /// given talker stream indices (block offset = position in the list).
    /// @p our_mac drives the conflict tie-break; @p on_addresses_ready
    /// fires on the reactor thread each time the block is (re)defended and
    /// every dest MAC has been published — entities re-declare their MSRP
    /// Talker Advertise there so listeners reserve against the MAAP
    /// address. Call once from start() in "maap" address mode; in static
    /// mode simply never call it (ready() defaults true).
    [[nodiscard]] auto acquire(
        net::MessageReactor& reactor,
        std::string_view const interface_name,
        ieee::Eui48 const& our_mac,
        nanoavb::NanoAvbComponents& components,
        TalkerStreams& talker,
        std::span<uint16_t const> const stream_indices,
        logging::Logger const log,
        statusbar::sg14::inplace_function<void(), 32> on_addresses_ready) -> Status
    {
        log_ = log;
        indices_.clear();
        for (auto const idx : stream_indices) {
            (void)indices_.try_push_back(idx);
        }
        on_addresses_ready_ = std::move(on_addresses_ready);

        // One allocation covering all talker streams; the PDUs carry the
        // first stream's id, and the seed spreads our initial random pick
        // per station.
        statusbar::tsn::StreamId maap_sid{};
        if (!indices_.empty()) {
            if (auto const* s = components.acmp_talker.get_stream(indices_.front()); s != nullptr) {
                (void)statusbar::tsn::load_unchecked(s->stream_id.span(), &maap_sid);
            }
        }
        handler_ = std::make_unique<avtp::MaapHandler>(our_mac, maap_sid, our_mac.to_uint64());

        // Gate stream TX until the block is defended. ADP/gPTP/SRP/ACMP are
        // unaffected — they keep running in the reactor; only the talker waits.
        ready_.store(false, std::memory_order_release);

        handler_->set_on_acquired([this, &components, &talker](ieee::Eui48 const& block_start, uint16_t /*count*/) {
            // Reactor thread, once the block is defended. Publish each
            // per-stream dest MAC, then release the gate so the media
            // thread's acquire-load sees the new MACs before transmitting.
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
            ready_.store(true, std::memory_order_release);
            log_->status("maap: acquired {} stream address(es) from {:012x}", indices_.size(), block_start.to_uint64());
            if (on_addresses_ready_) {
                on_addresses_ready_();
            }
        });

        handler_->set_on_lost([this](ieee::Eui48 const& /*start*/, uint16_t /*count*/) {
            // Conflict: close the TX gate until a new block is defended (the
            // handler is already re-probing; on_acquired re-opens it).
            ready_.store(false, std::memory_order_release);
            log_->warning("maap: address lost to a conflict; re-acquiring");
        });

        auto net_handler = std::make_unique<nanoavb::MaapNetHandler>(interface_name, *handler_);
        if (!net_handler->valid()) {
            log_->warning("maap: socket open failed; using static stream dest MACs");
            handler_.reset();
            ready_.store(true, std::memory_order_release);  // fall back: do not gate
            return {};
        }

        // Hand the handler to the reactor: acquisition runs ASYNCHRONOUSLY —
        // start() never blocks. The handler then keeps the block defended.
        reactor.add(std::move(net_handler));
        handler_->acquire(static_cast<uint16_t>(indices_.size()), net::monotonic_ns());
        return {};
    }

    /// TX-address readiness the talker gate consults per tick: true in
    /// static mode / after socket-failure fallback / once the block is
    /// defended; false while (re)acquiring.
    [[nodiscard]] auto ready() const noexcept -> bool { return ready_.load(std::memory_order_acquire); }

  private:
    std::unique_ptr<avtp::MaapHandler> handler_;
    std::atomic<bool> ready_{true};
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices_{};
    statusbar::sg14::inplace_function<void(), 32> on_addresses_ready_{};
    std::optional<logging::Logger> log_{};  ///< set by acquire (Logger is not default-constructible)
};

}  // namespace statusbar::avb_entity
