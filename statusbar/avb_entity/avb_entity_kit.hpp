#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_kit.hpp
/// @brief AvbEntityKit — the Entity Construction Kit facade (refactor phase D).
///
/// One collaborator that owns everything a blob-driven AVB entity needs
/// besides its audio: the control-plane host, the spec-shaped talker and
/// listener stream paths, the per-stream transmit gate, MAAP address
/// acquisition, CRF-input media-clock slaving, and the symbol-bound CONTROL
/// registry — plus all the wiring between them (ACMP connection callbacks,
/// MSRP advertise/withdraw, AEM GET_COUNTERS / GET_STREAM_INFO fills, the
/// stream-lifecycle storage-handler hooks, persisted listener bindings).
///
/// AvbEntityToneGenerator and AvbEntityAudioIO each carried a line-for-line
/// copy of this wiring; an entity built on the kit keeps only what is
/// genuinely its own: config parsing/validation, the DSP source, the media
/// clock, and process_audio(). The entity supplies the media clock and the
/// media-thread gPTP atomic (the kit's talker binds both) and forwards its
/// public control-plane surface here.
///
/// Threading: construction + start()/stop() + on_*() run on the main/reactor
/// side; talker_should_transmit() / media_rate() / drain_stream_rx() are the
/// media-thread (RT) calls; the CONTROL/lifecycle callbacks fire on the
/// reactor thread (see each hook).

#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_crf_clock_recovery.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

namespace statusbar::avb_entity {

class AvbEntityKit
{
  public:
    using TimePoint = sm::TimePoint;

    /// A bound control's value-changed handler (kit phase 4): receives the
    /// SET_CONTROL current-values payload (size-validated for linear types).
    /// Return AEM_STATUS_SUCCESS to accept (the built-in stores + notifies),
    /// any other AEM_STATUS_* to reject. Reactor thread.
    using ControlChangedFn = sg14::inplace_function<uint8_t(std::span<uint8_t const> value), 64>;

    /// @p handler serves the blob's descriptors and carries the built-ins the
    /// kit's lifecycle/CONTROL hooks dispatch from. It, @p media_clock and
    /// @p last_gptp_ns are the OWNING ENTITY's — the kit binds all three, so
    /// they must outlive the kit (declare them before it).
    /// @p audio_sink receives accepted listener stream audio (nullptr = none).
    AvbEntityKit(
        AvbEntityAudioIOConfig const& config,
        nanoavb::DescriptorStorageHandler& handler,
        StreamSpecs talker_specs,
        StreamSpecs listener_specs,
        uint32_t sample_rate,
        uint16_t initial_clock_source,
        std::optional<uint16_t> crf_clock_source_index,
        ptpclient::MediaClockGenerator& media_clock,
        std::atomic<int64_t>& last_gptp_ns,
        StreamRxAudioSink* audio_sink,
        std::pmr::memory_resource* memory_resource);

    AvbEntityKit(AvbEntityKit const&) = delete;
    auto operator=(AvbEntityKit const&) -> AvbEntityKit& = delete;
    AvbEntityKit(AvbEntityKit&&) = delete;
    auto operator=(AvbEntityKit&&) -> AvbEntityKit& = delete;
    ~AvbEntityKit() = default;

    /// Bring up the control plane and wire the whole kit: stream callbacks,
    /// MAAP (in "maap" address mode), the talker TX slots + socket (+ optional
    /// pcap capture), the listener RX slots + socket + CRF recovery feed, and
    /// persisted listener bindings. Logs any inert CONTROL registrations
    /// (menu/selection: never an error). @p now is the control-plane time the
    /// VLAN registration and the MAAP acquisition start at: a setup edge, read
    /// once here unless the caller holds it.
    [[nodiscard]] auto start(net::MessageReactor& reactor, TimePoint now = sm::Clock::now()) -> Status;
    /// Stop the control plane (fails with not_connected when not running),
    /// withdrawing its declarations at @p now.
    [[nodiscard]] auto stop(TimePoint now = sm::Clock::now()) -> Status;

    // --- Event handlers (drive the shared SM stack) ---------------------------
    void on_link_up(TimePoint time) { host_.on_link_up(time); }
    void on_link_down(TimePoint time) { host_.on_link_down(time); }
    void on_gptp_announce(TimePoint time, bool has_grandmaster) { host_.on_gptp_announce(time, has_grandmaster); }
    void on_timeout(TimePoint time) { host_.on_timeout(time); }

    /// How long the loop driving the reactor may wait this cycle: @p max_ms,
    /// or less when a MAAP probe or announce falls due sooner, told the
    /// reactor-clock @p now_ns the loop read. Reactor thread only.
    [[nodiscard]] auto poll_timeout_ms(int const max_ms, int64_t const now_ns) const noexcept -> int
    {
        return avtp::MaapHandler::poll_timeout_ms(maap_.next_deadline_ns(), now_ns, max_ms);
    }

    // --- Media-thread (RT) calls ----------------------------------------------
    /// Whether talker stream @p idx should put its AVTP stream on the wire this
    /// tick (gating disabled, or a downstream listener is ready/connected), told
    /// the wake's CLOCK_MONOTONIC_RAW instant @p now (the gate's grace clock).
    /// Also gated on MAAP-address readiness in "maap" mode, and closed while a
    /// controller has the stream STOP_STREAMING'd.
    [[nodiscard]] auto talker_should_transmit(uint16_t const idx, realtime::MonotonicRawClock::time_point const now) noexcept
        -> bool
    {
        if (!maap_.ready() || talker_->stream_stopped(idx)) {
            return false;
        }
        return gate_.should_transmit(idx, now);
    }

    /// The media-clock rate for this tick: @p base_r (the entity's own pacing —
    /// 1.0 for gPTP-locked, or its GPS tracker ratio) unless the CRF-input
    /// clock source is active AND the recovery is locked, in which case the
    /// recovered remote rate slaves this entity's media clock to the far
    /// talker's (nominal base_r until lock).
    [[nodiscard]] auto media_rate(double const base_r) noexcept -> double
    {
        if (crf_clock_source_index_ && active_clock_source_.load(std::memory_order_acquire) == *crf_clock_source_index_) {
            auto const est = crf_recovery_.estimate();
            return est.locked ? est.r : base_r;
        }
        return base_r;
    }

    /// The TX kick from the tool's SCHED_FIFO kicker timer (config.stream_tx_kicker)
    /// or from its RX timer's tick (config.stream_tx_kick_in_rx_timer).
    void kick_stream_tx() { talker_->kick_tx(); }

    /// The tool could not run the timer that was to kick: the media tick kicks
    /// again, as it does when neither option is set.
    void kick_from_media_tick() { (void)talker_->set_external_kick(false); }

    /// The tool could not run the RX timer start() kept the handler for: hand
    /// the handler to @p reactor so the reactor drains RX as it does by default,
    /// and, when the kick was to come from that timer, kick from the media tick.
    void rx_to_reactor(net::MessageReactor& reactor);

    /// Batch-drain the stream RX socket from the tool's dedicated SCHED_FIFO RX
    /// timer (config.stream_rx_rt_timer), told its wake on both clocks. No-op
    /// when RX is on the reactor.
    auto drain_stream_rx(realtime::GptpClock0::time_point const wake_gptp, net::RxTimePoint const wake_raw) -> size_t
    {
        return (rt_rx_handler_ != nullptr && listener_ != nullptr) ? listener_->drain_rx(wake_gptp, wake_raw) : 0;
    }

    // --- Per-control value handlers (kit phase 4) -----------------------------
    /// Register a handler for the CONTROL whose blob symbol code is
    /// @p symbol_code. The code is a menu, the model is the selection: an
    /// unknown symbol is recorded but inert (listed at start()); controls with
    /// no handler still get the generic store/serve built-in.
    void on_control_symbol(uint32_t symbol_code, ControlChangedFn fn);

    // --- CRF-input media-clock slaving (kit phase 3c) -------------------------
    /// The CLOCK_DOMAIN's active clock-source index (runtime SET_CLOCK_SOURCE
    /// state; the blob's authored default until a controller changes it).
    [[nodiscard]] auto active_clock_source() const noexcept -> uint16_t
    {
        return active_clock_source_.load(std::memory_order_acquire);
    }
    /// The clock-source index backed by the blob's CRF stream input, or
    /// nullopt when the model declares none (base-rate pacing only).
    [[nodiscard]] auto crf_clock_source_index() const noexcept -> std::optional<uint16_t> { return crf_clock_source_index_; }
    [[nodiscard]] auto crf_recovery() noexcept -> CrfClockRecovery& { return crf_recovery_; }

    // --- The owned collaborators (entities + tools reach through) -------------
    [[nodiscard]] auto host() noexcept -> AvbEntityHost& { return host_; }
    [[nodiscard]] auto host() const noexcept -> AvbEntityHost const& { return host_; }
    [[nodiscard]] auto talker() noexcept -> TalkerStreams& { return *talker_; }
    [[nodiscard]] auto talker() const noexcept -> TalkerStreams const& { return *talker_; }
    /// nullptr when the blob declares no stream inputs.
    [[nodiscard]] auto listener() noexcept -> ListenerStreams* { return listener_.get(); }
    [[nodiscard]] auto listener() const noexcept -> ListenerStreams const* { return listener_.get(); }
    [[nodiscard]] auto talker_specs() const noexcept -> StreamSpecs const& { return talker_specs_; }
    [[nodiscard]] auto listener_specs() const noexcept -> StreamSpecs const& { return listener_specs_; }
    [[nodiscard]] auto sample_rate() const noexcept -> uint32_t { return sample_rate_; }

    // --- Stream socket health (after start()) ---------------------------------
    /// Why start() could not open the stream TX socket (empty: it opened). The
    /// kit carries on without it (a control-plane-only entity); a host that
    /// needs the data plane checks here.
    [[nodiscard]] auto stream_tx_error() const noexcept -> std::error_code { return stream_tx_error_; }
    /// Why start() could not open the stream RX socket (empty: it opened, or
    /// the blob declares no stream inputs).
    [[nodiscard]] auto stream_rx_error() const noexcept -> std::error_code { return stream_rx_error_; }
    /// Whether "maap" address mode fell back to the static per-kind
    /// destination MACs (the MAAP socket would not open).
    [[nodiscard]] auto maap_fell_back_to_static() const noexcept -> bool { return maap_.fell_back_to_static(); }

  private:
    /// Attach the stream-specific control-plane behavior to the host: MSRP
    /// advertise/withdraw, the ACMP talker log + transmit-gate publication,
    /// the ACMP listener connect/disconnect drive, and the AEM
    /// GET_COUNTERS / GET_STREAM_INFO fills.
    void wire_stream_callbacks();

    /// The SRP handler adopted a Class A Domain from the wire (Milan v1.3
    /// 4.2.7.2.1): move the stream frames, MVRP and GET_AVB_INFO to it.
    void adopt_domain(nanoavb::DomainInfo const& domain, TimePoint now);

    /// The VLAN the streams use now: the adopted one, else the configured one.
    [[nodiscard]] auto current_vlan_id() const noexcept -> uint16_t;
    /// Let go of the data plane start() built: the RX handler kept for an RX
    /// timer, the talker's and the listener's slots, then the port. What the
    /// next start() opens is then the whole of it, as on a first start.
    void close_streams() noexcept;
    [[nodiscard]] auto make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo;
    void advertise_talker_streams(TimePoint time);
    [[nodiscard]] auto acquire_maap_addresses(net::MessageReactor& reactor, TimePoint now) -> Status;
    /// The stream kind of talker stream @p index ("other" when not in the table).
    [[nodiscard]] auto talker_kind_of(uint16_t index) const noexcept -> StreamKind;

    AvbEntityAudioIOConfig const& config_;
    StreamSpecs talker_specs_;
    /// The VLAN adopted from the wire (adopt_domain), 0 while it is the configured one.
    uint16_t active_vlan_id_{0};
    StreamSpecs listener_specs_;
    uint32_t sample_rate_;
    /// The owning entity's blob-backed descriptor handler; carries the
    /// built-ins the kit's lifecycle/CONTROL hooks dispatch from.
    nanoavb::DescriptorStorageHandler& storage_handler_;

    /// Reusable AVB control plane: talker/listener stream counts from the
    /// blob, 4 max listeners per talker stream.
    AvbEntityHost host_;

    /// Per-stream transmit gate (ACMP-AND-MSRP + grace). Binds config_ + components.
    TalkerGate gate_;

    /// The one port every stream frame goes through, backend from
    /// config_.net_backend. Declared before the talker, the listener and the
    /// RX handler, which hold it by reference.
    StreamPort stream_port_{};

    /// Stream TX path: spec-shaped serializer slots + TX counters + capture
    /// recorder on stream_port_. Binds the ENTITY's media clock + gPTP atomic.
    std::unique_ptr<TalkerStreams> talker_;

    /// Stream RX path (only allocated when the blob declares stream inputs).
    std::unique_ptr<ListenerStreams> listener_{};

    /// The stream RX socket handler when config.stream_rx_rt_timer keeps it
    /// out of the reactor (drained via drain_stream_rx()).
    std::unique_ptr<net::Pollable> rt_rx_handler_{};

    /// What opening the stream TX / RX sockets failed with (see stream_tx_error()).
    std::error_code stream_tx_error_{};
    std::error_code stream_rx_error_{};

    /// MAAP dynamic-address acquisition (active only in "maap"
    /// stream_address_mode; ready() defaults true for static MACs).
    MaapAddressAcquirer maap_{};

    /// CRF-input media-clock recovery: fed by the CRF stream input's
    /// timestamps on the RX/reactor thread; consulted by media_rate() on the
    /// media thread when the CRF clock source is active.
    CrfClockRecovery crf_recovery_{};
    /// The CLOCK_DOMAIN's active clock-source index. Written by the
    /// SET_CLOCK_SOURCE apply callback (reactor thread), read per tick by
    /// the media thread.
    std::atomic<uint16_t> active_clock_source_{0};
    std::optional<uint16_t> crf_clock_source_index_{};

    /// Symbol-bound control handlers (kit phase 4): resolved bindings carry
    /// the CONTROL descriptor index; unresolved ones stay inert (diag at start).
    struct ControlBinding
    {
        uint32_t symbol{0};
        uint16_t control_index{0};
        bool resolved{false};
        ControlChangedFn fn{};
    };
    static constexpr size_t MAX_CONTROL_BINDINGS = 8;
    sg14::inplace_vector<ControlBinding, MAX_CONTROL_BINDINGS> control_bindings_{};
};

}  // namespace statusbar::avb_entity
