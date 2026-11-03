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
///
/// With a range file named, the block defended is saved there (durably, a
/// temporary renamed over it) each time it is acquired, and the next
/// setup() hands the saved start back to the handler to probe first, so a
/// restart keeps its destination addresses unless somebody took them while
/// it was down. Without one, every start draws afresh.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/buffer/file_descriptor.hpp"
#include "statusbar/buffer/file_write.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

/// A defended MAAP range as the range file holds it.
struct SavedMaapRange
{
    ieee::Eui48 start{};
    uint16_t count{0};

    [[nodiscard]] auto operator==(SavedMaapRange const&) const noexcept -> bool = default;
};

/// The range file's one line: the start as twelve hex digits, a space, the
/// count in decimal, a newline. "91e0f0001234 3\n".
[[nodiscard]] auto encode_maap_range(SavedMaapRange const& range) -> std::string;

/// The range a range file's text holds, or nullopt for text that is not one
/// (empty, damaged, a count of 0, a range outside the dynamic pool).
[[nodiscard]] auto decode_maap_range(std::string_view text) noexcept -> std::optional<SavedMaapRange>;

/// Write @p range to @p path durably (write_file_durably: a temporary beside
/// it, synced and renamed over it). Errors: the write's.
[[nodiscard]] auto save_maap_range(std::string_view path, SavedMaapRange const& range) -> Status;

/// The range saved at @p path, or nullopt when there is no file, it cannot be
/// read, or it does not hold one: a missing or damaged file is a fresh draw,
/// not a failure.
[[nodiscard]] auto load_maap_range(std::string_view path) -> std::optional<SavedMaapRange>;

inline auto encode_maap_range(SavedMaapRange const& range) -> std::string
{
    return std::format("{:012x} {}\n", range.start.to_uint64(), range.count);
}

namespace detail {

/// The twelve hex digits at the front of a range line, as a number.
[[nodiscard]] inline auto maap_range_start_of(std::string_view const text) noexcept -> std::optional<uint64_t>
{
    constexpr size_t HEX_DIGITS = 12;
    if (text.size() < HEX_DIGITS + 2 || text[HEX_DIGITS] != ' ') {
        return std::nullopt;
    }
    uint64_t start = 0;
    auto const hex = std::from_chars(text.data(), text.data() + HEX_DIGITS, start, 16);
    return hex.ec == std::errc{} && hex.ptr == text.data() + HEX_DIGITS ? std::optional{start} : std::nullopt;
}

/// The count after the space, 1 to 65535.
[[nodiscard]] inline auto maap_range_count_of(std::string_view const rest) noexcept -> std::optional<uint16_t>
{
    uint32_t count = 0;
    auto const dec = std::from_chars(rest.data(), rest.data() + rest.size(), count, 10);
    if (dec.ec != std::errc{} || dec.ptr == rest.data() || count == 0 || count > UINT16_MAX) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(count);
}

}  // namespace detail

inline auto decode_maap_range(std::string_view const text) noexcept -> std::optional<SavedMaapRange>
{
    auto const start = detail::maap_range_start_of(text);
    auto const count = start ? detail::maap_range_count_of(text.substr(13)) : std::nullopt;
    if (!start || !count) {
        return std::nullopt;
    }
    SavedMaapRange range{.count = *count};
    range.start.from_uint64(*start);
    return avtp::MaapHandler::range_fits_pool(range.start, range.count) ? std::optional{range} : std::nullopt;
}

inline auto save_maap_range(std::string_view const path, SavedMaapRange const& range) -> Status
{
    std::string const text = encode_maap_range(range);
    return write_file_durably(path, make_const_span(text));
}

inline auto load_maap_range(std::string_view const path) -> std::optional<SavedMaapRange>
{
    FileDescriptor const fd{::open(std::string{path}.c_str(), O_RDONLY | O_CLOEXEC)};
    std::array<char, 64> text{};  // the one line is 20 octets; anything near the buffer's size is not ours
    ssize_t const got = fd.valid() ? ::read(fd.get(), text.data(), text.size()) : -1;
    if (got <= 0 || static_cast<size_t>(got) >= text.size()) {
        return std::nullopt;
    }
    return decode_maap_range(std::string_view{text.data(), static_cast<size_t>(got)});
}

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
    /// Called on the reactor thread with ready() at each change: a set-up that gates, a block defended or lost, stop().
    /// Not for the state at install; a later install replaces an earlier one, an empty function removes it. It must
    /// not throw.
    using ReadyChangedFn = statusbar::sg14::inplace_function<void(bool ready), 32>;
    void set_on_ready_changed(ReadyChangedFn fn) noexcept { on_ready_changed_ = std::move(fn); }

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
    /// an earlier call is retired first, at @p now. @p range_file, when not
    /// empty, is where the block defended is saved and read back from: the
    /// saved start is probed first (MaapHandler::prepare(count, start)).
    [[nodiscard]] auto setup(
        net::MessageReactor& reactor,
        std::string_view const interface_name,
        ieee::Eui48 const& our_mac,
        nanoavb::NanoAvbComponents& components,
        TalkerStreams& talker,
        std::span<uint16_t const> const stream_indices,
        logging::Logger const log,
        ReadyFn on_addresses_ready,
        sm::TimePoint const now = sm::Clock::now(),
        std::string_view const range_file = {}) -> Status
    {
        begin_setup(log, stream_indices, std::move(on_addresses_ready), now);
        range_file_ = std::string{range_file};
        // One allocation covering all talker streams; the seed spreads our
        // initial draw per station and per boot.
        auto handler = std::make_unique<avtp::MaapHandler>(our_mac, maap_seed(our_mac, realtime_now_ns()));
        prepare_range(*handler);
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
        set_ready(false);
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

    /// What a defended block means for the streams: stream @p indices[pos]
    /// takes address @p block_start + pos in ACMP's picture of it (what a
    /// CONNECT_TX_RESPONSE and GET_STREAM_INFO carry) and in the frames. The
    /// stream's id and VLAN stay as they are: the VLAN is the Domain's,
    /// configured or adopted from the bridge, and a block acquired on every
    /// Link Up must not move the stream back to the default. Reactor thread.
    static void publish_block(
        nanoavb::NanoAvbComponents& components,
        TalkerStreams& talker,
        std::span<uint16_t const> const indices,
        ieee::Eui48 const& block_start)
    {
        for (size_t pos = 0; pos < indices.size(); ++pos) {
            uint16_t const idx = indices[pos];
            ieee::Eui48 const dest = avtp::maap_block_address(block_start, static_cast<uint16_t>(pos));
            if (auto const s = components.talker_stream(idx); s.has_value()) {
                (void)components.configure_talker_stream(idx, s->stream_id, dest, s->stream_vlan_id);
            }
            talker.set_destination(idx, dest);
        }
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
        set_ready(false);
        net_ = net_handler.get();
        reactor.add(std::move(net_handler));
        return {};
    }

  public:
    /// @p handler's acquired and lost callbacks onto the streams' addresses, the SRP handler and ready(). setup()
    /// wires its own handler; a test may wire one it drives. Reactor thread.
    void wire_handler(avtp::MaapHandler& handler, nanoavb::NanoAvbComponents& components, TalkerStreams& talker)
    {
        handler.set_on_acquired(
            [this, &components, &talker](ieee::Eui48 const& block_start, uint16_t const count, int64_t const now_ns) {
                // Reactor thread, once the block is defended. Publish each
                // per-stream dest MAC, then release the gate so the media
                // thread's acquire-load sees the new MACs before transmitting.
                publish_block(components, talker, indices_, block_start);
                components.msrp_handler.set_talker_addresses_valid(true);
                set_ready(true);
                log_->status("maap: acquired {} stream address(es) from {:012x}", indices_.size(), block_start.to_uint64());
                remember(block_start, count);
                if (on_addresses_ready_) {
                    // The MAAP handler's timeline is the reactor's (net::monotonic_ns,
                    // std::chrono::steady_clock): the same clock as sm::Clock.
                    on_addresses_ready_(sm::TimePoint{std::chrono::nanoseconds{now_ns}});
                }
            });
        handler.set_on_lost([this, &components](ieee::Eui48 const& start, uint16_t /*count*/) {
            // A conflict, a Link Up re-probing the range, or the stop: the TX
            // gate closes until a block is defended (on_acquired re-opens it),
            // and a talker that declares on demand withdraws its Talker
            // attributes at the SRP handler's next tick (Milan v1.3 4.3.3.1:
            // an address in conflict is not a valid one).
            set_ready(false);
            components.msrp_handler.set_talker_addresses_valid(false);
            log_->warning("maap: no longer defending {:012x}; stream TX gated", start.to_uint64());
        });
    }

  private:
    /// Name the block's size to @p handler, and the start to try first where
    /// the range file holds one: the range of an earlier life, probed first so
    /// the streams keep their destination addresses unless somebody took them.
    void prepare_range(avtp::MaapHandler& handler)
    {
        auto const count = static_cast<uint16_t>(indices_.size());
        auto const saved = range_file_.empty() ? std::nullopt : load_maap_range(range_file_);
        if (!saved) {
            handler.prepare(count);
            return;
        }
        handler.prepare(count, saved->start);
        log_->status("maap: probing the saved range {:012x} first", saved->start.to_uint64());
    }

    /// The block defended into the range file, for the next start to probe
    /// first. A write that fails is logged and that is all: the block is
    /// defended either way, and the next start draws afresh.
    void remember(ieee::Eui48 const& block_start, uint16_t const count)
    {
        if (range_file_.empty()) {
            return;
        }
        if (auto const saved = save_maap_range(range_file_, SavedMaapRange{.start = block_start, .count = count}); !saved) {
            log_->warning("maap: the range file was not written: {}", logging::embed<40>(saved.error().message()));
        }
    }

    /// ready() from now on, told to on_ready_changed_ when that changed it. Reactor thread.
    void set_ready(bool const ready) noexcept
    {
        if (ready_.exchange(ready, std::memory_order_acq_rel) != ready && on_ready_changed_) {
            on_ready_changed_(ready);
        }
    }

    /// The port on the reactor, which owns it; null when nothing is set up.
    /// Valid until stop() retires it: the reactor drops only finished ports.
    nanoavb::MaapNetHandler* net_{nullptr};
    std::atomic<bool> ready_{true};
    bool fell_back_to_static_{false};
    sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS> indices_{};
    ReadyFn on_addresses_ready_{};
    ReadyChangedFn on_ready_changed_{};
    std::optional<logging::Logger> log_{};  ///< set by setup (Logger is not default-constructible)
    std::string range_file_{};              ///< empty: nothing saved, every start draws afresh
};

}  // namespace statusbar::avb_entity
