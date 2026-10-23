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
#include "statusbar/avb_entity/avb_entity_clock_sources.hpp"
#include "statusbar/avb_entity/avb_entity_crf_clock_recovery.hpp"
#include "statusbar/avb_entity/avb_entity_host.hpp"
#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"
#include "statusbar/avb_entity/avb_entity_listener_vlan.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_persistent_state.hpp"
#include "statusbar/avb_entity/avb_entity_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_declarations.hpp"
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

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace statusbar::avb_entity {

// The control-plane collaborators the kit hands its stream tables to are sized
// in nanoavb/atdecc/srp; each must hold MAX_ENTITY_STREAMS per direction.
static_assert(
    atdecc::TalkerContext<>::MAX_STREAMS >= MAX_ENTITY_STREAMS && atdecc::ListenerContext<>::MAX_STREAMS >= MAX_ENTITY_STREAMS,
    "ACMP talker/listener contexts: one entry per STREAM_OUTPUT/STREAM_INPUT");
static_assert(
    nanoavb::DescriptorStorageHandler::MAX_STREAM_STATES >= 2 * MAX_ENTITY_STREAMS,
    "AEM stream runtime state (SET_STREAM_FORMAT, START/STOP_STREAMING): inputs and outputs share the table");
static_assert(
    TalkerDeclarations::LISTENER_GRACE_NS == TalkerGate::GRACE_NS,
    "a departed Listener keeps its stream declared as long as it keeps the gate open");
static_assert(
    nanoavb::NanoAvbAcmpListener::MAX_FAST_CONNECT_GOALS >= MAX_ENTITY_STREAMS,
    "fast-connect goals and persisted listener bindings: one per STREAM_INPUT");
static_assert(
    srp::msrp::DefaultMsrpLimits::max_talker_advertise >= 2 * MAX_ENTITY_STREAMS &&
        srp::msrp::DefaultMsrpLimits::max_listeners >= 2 * MAX_ENTITY_STREAMS &&
        srp::msrp::DefaultMsrpLimits::max_talker_failed >= MAX_ENTITY_STREAMS,
    "MSRP tables (nanoavb::entity_msrp_config): room to reserve our own declarations plus the bound peers', per "
    "direction; other stations' propagated advertises only get the remainder");

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
    /// once here unless the caller holds it. A failure once the streams began
    /// to open leaves those opened, as their hooks were told, until stop().
    [[nodiscard]] auto start(net::MessageReactor& reactor, TimePoint now = sm::Clock::now()) -> Status;
    /// Stop the control plane (fails with not_connected when not running),
    /// withdrawing its declarations at @p now. Deletes the listener's VLAN link
    /// first if the kit created it (even when not running).
    [[nodiscard]] auto stop(TimePoint now = sm::Clock::now()) -> Status;

#if defined(__linux__)
    // --- The listener's VID on the NIC's VLAN filter (Linux) -------------------

    /// Keep the SR-class VID in use on @p ifindex's receive VLAN filter through
    /// @p control until stop(); a no-op without stream inputs. start() calls it
    /// unless called first; @p control must outlive the kit.
    void bind_listener_vlan(net::vlan::VlanControl& control, int ifindex);
    [[nodiscard]] auto listener_vlan() const noexcept -> ListenerVlan const& { return listener_vlan_; }
#endif

    // --- Event handlers (drive the shared SM stack) ---------------------------
    /// The link is up: the control plane's Link Up, and MAAP's
    /// PortOperational! (IEEE 1722-2025 B.3.5.9) — the first begins acquiring
    /// the stream addresses, a later one re-probes the range held. A Link Up
    /// while the link is already up is a repeat, not a bounce (a runner that
    /// drives the start-up Link Up itself and then installs a link monitor
    /// reporting the first state it sees raises two within milliseconds):
    /// the supervisor's table has no row for it, and MAAP is not told, or the
    /// second one would restart the probe sequence the first began (five
    /// PROBEs for four). A Link Down, or stop(), ends the link-up state, so a
    /// genuine bounce and the Link Up after a restart both go through.
    void on_link_up(TimePoint time)
    {
        // MAAP first, the host after: the host's Link Up starts ADP unless
        // the kit holds it, and it is held while the block is being probed
        // (a first acquisition, or a bounce re-probing the range held). The
        // first ENTITY_AVAILABLE then goes out once the addresses are
        // defended, from the acquirer's ready callback, so no listener probes
        // a talker whose destination address is not yet its own. A repeat
        // (the link already up) reaches neither MAAP nor the hold.
        if (!link_up_) {
            maap_.port_operational(time);
        }
        link_up_ = true;
        host_.hold_adp(!maap_.ready());
        host_.on_link_up(time);
    }
    void on_link_down(TimePoint time)
    {
        link_up_ = false;
        host_.on_link_down(time);
    }
    /// Whether the kit has seen a Link Up since start(), stop() or the last
    /// Link Down: the state on_link_up() tells a repeat from a bounce by.
    [[nodiscard]] auto link_up() const noexcept -> bool { return link_up_; }
    /// gPTP grandmaster found or changed / lost (EntityUpkeep wires these to
    /// the Announce handler): drives the talker streams' AVTP tu bit (Milan
    /// v1.2 Section 4.3.5.2, Annex B.1.1). Reactor thread.
    void note_grandmaster_changed()
    {
        host_.note_grandmaster_changed();
        talker_->note_grandmaster_changed(steady_now_ns());
    }
    void note_grandmaster_lost() noexcept { talker_->note_grandmaster_lost(); }
    void on_gptp_announce(TimePoint time, bool has_grandmaster) { host_.on_gptp_announce(time, has_grandmaster); }
    void on_timeout(TimePoint time) { host_.on_timeout(time); }

    /// How long the loop driving the reactor may wait this cycle: @p max_ms,
    /// or less when a MAAP probe or announce, an MRP timer or the admission
    /// deadline falls due sooner, told the reactor-clock @p now_ns the loop
    /// read and the gate's clock @p raw_now. From the admission deadline on it
    /// answers 0 until end_cycle() runs: an entity that forwards this must
    /// forward end_cycle() too. Reactor thread only.
    [[nodiscard]] auto poll_timeout_ms(int max_ms, int64_t now_ns, TalkerGate::RawTimePoint raw_now) const noexcept -> int;
    /// As above for a loop that holds the reactor clock alone: the gate's
    /// clock at @p now_ns is the pair last told (tell_cycle_time) carried
    /// forward by the reactor clock, raw_at().
    [[nodiscard]] auto poll_timeout_ms(int max_ms, int64_t now_ns) const noexcept -> int;

    /// The reactor cycle now being served began at reactor-clock @p now_ns,
    /// which was the instant @p raw_now on the gate's clock: the pair the loop
    /// read together when the reactor read its clock, before any handler of
    /// the cycle ran (entity_reactor_clock). The kit reads no clock. Each gate
    /// input of the cycle (a Listener Ready over MSRP, an ACMP connection) is
    /// known at @p raw_now, and so is the admitted set asked for with no
    /// instant. Reactor thread only.
    void tell_cycle_time(int64_t now_ns, TalkerGate::RawTimePoint raw_now) noexcept;
    /// The gate's clock at reactor-clock @p now_ns: the pair last told, carried
    /// forward by the reactor clock. The two clocks' rates differ by parts per
    /// million and a cycle lasts milliseconds, so within the cycle it is the
    /// instant to well under a microsecond. Told no pair, RawTimePoint{}.
    [[nodiscard]] auto raw_at(int64_t now_ns) const noexcept -> TalkerGate::RawTimePoint;

    /// The earliest of the MAAP deadline, the MSRP and MVRP timers and the end
    /// of the counters' observation interval. An MRP jointimer is 200 ms, so a
    /// loop that only woke every 100 ms sent a declaration's second New, and
    /// the Join after a LeaveAll, up to 100 ms past the JoinTime.
    [[nodiscard]] auto next_control_deadline_ns() const noexcept -> std::optional<int64_t>;

    /// The observation interval of the stream counters: half a second, where
    /// Milan v1.3 Tables 5.4 and 5.6 allow a second at most. An interval is
    /// timed from the fold that ended the one before, so it lasts this and the
    /// loop's wake-up, and stays under the second.
    static constexpr int64_t COUNTERS_INTERVAL_NS = 500'000'000;
    /// The observation intervals between two unsolicited GET_COUNTERS of one
    /// descriptor. Each interval lasts COUNTERS_INTERVAL_NS at least, so this
    /// many of them are never under the second Table 5.20 asks for.
    static constexpr unsigned COUNTERS_NOTIFY_FOLDS = 2;
    static_assert(COUNTERS_INTERVAL_NS * COUNTERS_NOTIFY_FOLDS >= 1'000'000'000);
    /// The one call the loop owes the kit each reactor cycle, after the reactor
    /// served it, at reactor-clock @p now_ns: ends the stream counters'
    /// observation interval when due (the first call starts the first; under the
    /// Milan profile every COUNTERS_NOTIFY_FOLDS-th end notifies the descriptors
    /// whose counters changed) and, from the admission deadline on, re-evaluates
    /// admission at raw_at(@p now_ns) on the gate's clock. The loop bounds its
    /// wait by poll_timeout_ms(), which counts both deadlines. Reactor thread only.
    void end_cycle(int64_t now_ns);

    // --- Media-thread (RT) calls ----------------------------------------------

    /// Whether talker stream @p idx transmits this tick: admitted (or gating off),
    /// its MAAP address ready in "maap" mode, and not STOP_STREAMING'd.
    /// The gate runs on the steady clock until it moves to the wake's RAW time.
    [[nodiscard]] auto talker_should_transmit(uint16_t const idx, realtime::MonotonicRawClock::time_point const /*now*/) const noexcept
        -> bool
    {
        if (!maap_.ready() || talker_->stream_stopped(idx)) {
            return false;
        }
        return gate_.should_transmit(idx, steady_now_ns());
    }

    // --- Talker admission (reactor thread) ------------------------------------
    /// The gate's clock, CLOCK_MONOTONIC_RAW (TalkerGate::RawTimePoint).
    using RawTimePoint = TalkerGate::RawTimePoint;
    /// Talker stream indices, in table order.
    using AdmittedStreams = sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS>;
    /// The talker streams the gate admits at @p now (TalkerGate::admitted), in table order: a Listener
    /// Ready registered or within the grace, plus the ACMP connection where the gate needs one; every
    /// stream when the gate is off. A stream STOP_STREAMING stopped, or waiting for MAAP, stays in it.
    [[nodiscard]] auto admitted_talker_streams(RawTimePoint now) const noexcept -> AdmittedStreams;
    /// As above, at the instant of the reactor cycle last told (tell_cycle_time).
    [[nodiscard]] auto admitted_talker_streams() const noexcept -> AdmittedStreams;
    /// The hook told the admitted set each time it changes: the reactor thread calls it, at a gate
    /// input or at the admission deadline. It is not called for the set in force at install
    /// (admitted_talker_streams() has it); a later install replaces an earlier one, an empty function
    /// removes it. The span is the kit's, valid only during the call. It must not throw, install a
    /// hook or feed a gate input.
    using AdmissionChangedFn = sg14::inplace_function<void(std::span<uint16_t const> admitted), 32>;
    void set_on_admission_changed(AdmissionChangedFn fn) noexcept { on_admission_changed_ = std::move(fn); }
    /// Told each change of whether talker stream @p idx streams (slot open, MAAP ready, admitted, not stopped), on the
    /// reactor thread, start() and stop() included; not the state at install (talker_gate_open()). A later install
    /// replaces an earlier one, an empty function removes it. It must not throw, install a hook or feed a gate input.
    using TalkerGateFn = sg14::inplace_function<void(uint16_t idx, bool open), 32>;
    void set_on_talker_gate_changed(TalkerGateFn fn) noexcept { on_talker_gate_changed_ = std::move(fn); }
    /// Whether talker stream @p idx streams, as the reactor last evaluated and told it; false past the table.
    /// Reactor thread.
    [[nodiscard]] auto talker_gate_open(uint16_t const idx) const noexcept -> bool
    {
        return idx < talker_gates_.size() && talker_gates_[idx];
    }
    /// Reactor thread: an MSRP Listener Ready for @p stream_id was registered (@p ready) or withdrawn,
    /// known at the RAW instant @p now (the host's hook passes the cycle's, tell_cycle_time). Stamps
    /// the gate, then re-evaluates admission.
    void notify_listener_ready(nanoavb::StreamId const& stream_id, bool ready, RawTimePoint now);
    /// The earliest instant the grace of a stream still admitted runs out (TalkerGate::grace_ends),
    /// armed at each evaluation of admission; nullopt while no such grace runs.
    [[nodiscard]] auto admission_deadline() const noexcept -> std::optional<RawTimePoint> { return admission_deadline_; }

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

    // --- Talker admission (the hardware shaper's input) ------------------------

    /// The talker stream indices admitted at steady-clock @p now_ns, in table
    /// order (TalkerGate::admitted). The no-argument form reads the steady clock.
    using AdmittedStreams = sg14::inplace_vector<uint16_t, MAX_ENTITY_STREAMS>;
    [[nodiscard]] auto admitted_talker_streams(int64_t now_ns) const noexcept -> AdmittedStreams;
    [[nodiscard]] auto admitted_talker_streams() const noexcept -> AdmittedStreams
    {
        return admitted_talker_streams(steady_now_ns());
    }
    /// Called on the reactor thread with the new admitted set whenever it changes;
    /// not for the set in force at install (read admitted_talker_streams()).
    /// Pass an empty function to remove it before whatever it captures goes.
    using AdmissionChangedFn = sg14::inplace_function<void(std::span<uint16_t const> admitted), 32>;
    void set_on_admission_changed(AdmissionChangedFn fn) noexcept { on_admission_changed_ = std::move(fn); }
    /// Reactor thread: an MSRP listener declaration for @p stream_id was registered
    /// (@p ready) or withdrawn at @p now_ns; feeds the gate, re-evaluates admission.
    void note_listener_ready(nanoavb::StreamId const& stream_id, bool ready, int64_t now_ns);
    /// Reactor thread, periodically (EntityUpkeep::tick): re-evaluate admission
    /// at steady-clock @p now_ns, so a grace window that runs out with no MSRP
    /// event still closes the stream's admission and fires the callback.
    void poll_admission(int64_t const now_ns) { note_admission(now_ns); }

    // --- Talker Advertise declarations (Milan v1.2 Section 4.3.3.1) -----------

    /// Whether talker stream @p idx declares its Talker Advertise now
    /// (TalkerDeclarations; the rules are in docs/AVB_ENTITY_KIT.md).
    [[nodiscard]] auto talker_declared(uint16_t const idx) const noexcept -> bool { return declarations_.declared(idx); }
    [[nodiscard]] auto talker_declarations() const noexcept -> TalkerDeclarations const& { return declarations_; }
    /// Reactor thread: a PROBE_TX_COMMAND for talker stream @p idx counted at
    /// steady-clock @p now_ns (the ACMP talker's on_probe lands here): its
    /// window opens and the declarations are re-evaluated.
    void note_talker_probe(uint16_t idx, int64_t now_ns);
    /// Reactor thread: declare and withdraw Talker Advertises as TalkerDeclarations
    /// decides at @p now_ns (@p refresh re-declares, e.g. with a new Class A VID).
    /// The kit's own timer calls it at each deadline; events call it directly.
    void update_talker_declarations(int64_t now_ns, bool refresh = false);
    /// Reactor thread: the talker streams' MAAP addresses became valid (@p valid)
    /// or were lost to a conflict. Until valid, PROBE_TX / CONNECT_TX are answered
    /// TALKER_DEST_MAC_FAIL and nothing is declared.
    void note_talker_addresses(bool valid, int64_t now_ns);
    /// Reactor thread, every pass while started: run the declarations once
    /// their next deadline (TalkerDeclarations::next_deadline) has come.
    void on_declaration_timer(int64_t now_ns);

    // --- Presentation time offset (Milan v1.2 Section 5.3.7.6) ------------------

    /// Talker stream @p idx's presentation time offset in ns (any thread); starts
    /// at config.presentation_offset_ns, settable by a controller, persisted.
    [[nodiscard]] auto presentation_offset_ns(uint16_t const idx) const noexcept -> uint32_t
    {
        return idx < presentation_offset_ns_.size() ? presentation_offset_ns_[idx].load(std::memory_order_relaxed) : 0;
    }
    /// The entity applies a talker stream's new presentation offset (reactor thread,
    /// before the kit stores it) and answers an AEM status. Without one the kit
    /// refuses (NOT_SUPPORTED). Pass an empty function to remove it.
    using PresentationOffsetFn = sg14::inplace_function<uint8_t(uint16_t idx, uint32_t offset_ns), 32>;
    void set_on_presentation_offset_changed(PresentationOffsetFn fn) noexcept { on_presentation_offset_changed_ = std::move(fn); }

    // --- Persistent state (Milan non-volatile settings) ------------------------
    /// With config.persistent_state_path set, start() loads and applies the file through the checks
    /// a controller's command gets, and every later setting change rewrites it when it changed.
    /// Reactor thread.
    void persist();
    /// The state persist() would write now.
    [[nodiscard]] auto collect_persistent_state() const -> PersistentState;

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

    // --- What each clock source is, and which the entity runs -------------
    /// What CLOCK_SOURCE @p index of the model is; nullopt for one it lacks.
    [[nodiscard]] auto clock_source_kind(uint16_t const index) const noexcept -> std::optional<ClockSourceKind>
    {
        return index < clock_source_kinds_.size() ? std::optional{clock_source_kinds_[index]} : std::nullopt;
    }
    /// What clock domain 0's source in force is, for the entity to run its
    /// media clock by; Internal when the model does not describe it. Any thread.
    [[nodiscard]] auto active_clock_source_kind() const noexcept -> ClockSourceKind
    {
        return clock_source_kind(active_clock_source()).value_or(ClockSourceKind::Internal);
    }
    /// The entity knows how to follow the clock source whose blob symbol is
    /// @p symbol: from here on a SET_CLOCK_SOURCE to it is accepted. It is
    /// for sources of the External kind; the others run without being named.
    /// A symbol the blob lacks, or one of another descriptor, binds nothing
    /// (the menu / selection pattern). Before start().
    void run_clock_source(std::string_view symbol);
    /// Whether SET_CLOCK_SOURCE to CLOCK_SOURCE @p index is accepted: every
    /// source but one of the External kind that no run_clock_source() named.
    /// The model lists such a source and nothing here can follow it.
    [[nodiscard]] auto clock_source_runs(uint16_t const index) const noexcept -> bool
    {
        return clock_source_kind(index) != ClockSourceKind::External || external_clock_sources_run_.test(index);
    }
    [[nodiscard]] auto crf_recovery() noexcept -> CrfClockRecovery& { return crf_recovery_; }

    // --- The owned collaborators (entities + tools reach through) -------------
    [[nodiscard]] auto host() noexcept -> AvbEntityHost& { return host_; }
    [[nodiscard]] auto host() const noexcept -> AvbEntityHost const& { return host_; }
    [[nodiscard]] auto talker() noexcept -> TalkerStreams& { return *talker_; }
    [[nodiscard]] auto talker() const noexcept -> TalkerStreams const& { return *talker_; }
    /// The port the streams go through. One the host opened before start() (a test port,
    /// StreamPort::open_test) is kept by it; stop() closes it either way.
    [[nodiscard]] auto stream_port() noexcept -> StreamPort& { return stream_port_; }
    /// The MAAP acquirer of the talker streams' addresses ("maap" address mode). Reactor thread. The kit
    /// installs its set_on_ready_changed() hook, which keeps the talker gates; a caller must not replace it.
    [[nodiscard]] auto maap() noexcept -> MaapAddressAcquirer& { return maap_; }
    /// nullptr when the blob declares no stream inputs.
    [[nodiscard]] auto listener() noexcept -> ListenerStreams* { return listener_.get(); }
    [[nodiscard]] auto listener() const noexcept -> ListenerStreams const* { return listener_.get(); }
    [[nodiscard]] auto talker_specs() const noexcept -> StreamSpecs const& { return talker_specs_; }
    /// The MSRP Talker Advertise talker stream @p spec is declared with:
    /// identity from ACMP's picture of the stream, VLAN and priority from the
    /// SR class domain in force (configured, or adopted from the wire).
    [[nodiscard]] auto make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo;
    /// Whether the entity answers to Milan v1.3 where it and IEEE 1722.1-2021
    /// differ: the configured atdecc_profile when there is one, else the
    /// model's (a Milan model does, any other does not).
    [[nodiscard]] auto milan_profile() const noexcept -> bool;
    [[nodiscard]] auto listener_specs() const noexcept -> StreamSpecs const& { return listener_specs_; }
    /// The rate the kit's engines were built with; a SET_SAMPLING_RATE does not change it
    /// (set_on_unit_rate_changed()), so any thread may read it.
    [[nodiscard]] auto sample_rate() const noexcept -> uint32_t { return sample_rate_; }
    /// The entity configuration the kit was built with (VLAN, stream PCP, ...).
    [[nodiscard]] auto config() const noexcept -> AvbEntityAudioIOConfig const& { return config_; }

    /// The MSRP TSpec advertised for talker stream @p spec: srp_max_frame_size at
    /// the class A cadence, one frame per interval (also the shaper's input).
    [[nodiscard]] static auto talker_tspec(StreamSpec const& spec) noexcept -> TSpec
    {
        return TSpec{.max_frame_size = srp_max_frame_size(spec.format), .max_interval_frames = 1};
    }

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
    /// advertise/withdraw (+ the transmit gate's MSRP listener feed), the
    /// ACMP talker connection log,
    /// the ACMP listener connect/disconnect drive, and the AEM
    /// GET_COUNTERS / GET_STREAM_INFO fills.
    void wire_stream_callbacks();
    /// The MSRP third of wire_stream_callbacks(): advertise, withdraw, and the transmit gate's Listener Ready.
    void wire_srp_callbacks();
    /// The ACMP third, for whichever ACMP the components run.
    void wire_acmp_callbacks();
    /// Talker stream @p idx's IEEE 1722.1 ACMP connection count to the gate, then admission re-evaluated.
    void publish_talker_connections(uint16_t idx);
    /// Recompute the admitted set at @p now after a gate input changed, or at the admission
    /// deadline; tell on_admission_changed_ when it differs from the one last told.
    void notify_admission(RawTimePoint now);
    /// Arm the admission deadline at the earliest grace end after @p now (none: disarmed).
    void arm_admission_deadline(RawTimePoint now) noexcept;
    /// At or past the admission deadline (@p now on the gate's clock), re-evaluate admission, so a
    /// grace that runs out with no MSRP or ACMP event still leaves the set and is told to the hook.
    /// Before it, nothing. end_cycle() calls it.
    void on_admission_timer(RawTimePoint now);
    /// Evaluate each talker stream's gate (evaluate_talker_gate()); tell on_talker_gate_changed_ each that moved.
    void notify_talker_gates() noexcept;
    /// Whether talker stream @p idx streams by the reactor's state: slot open, MAAP ready, admitted, not stopped.
    [[nodiscard]] auto evaluate_talker_gate(uint16_t idx) const noexcept -> bool;
    /// IEEE 1722.1 ACMP: probes, the talker connection log and gate, the listener's connect and disconnect.
    void wire_ieee_acmp_callbacks();
    /// Milan ACMP: what its talker and its listener ask of SRP, MAAP, gPTP and the AEM lock.
    void wire_milan_talker_callbacks();
    void wire_milan_listener_callbacks();
    /// The IEEE 1722.1 listener's connect and disconnect, onto the stream inputs.
    template <typename Listener>
    void wire_listener_connections(Listener& listener);
    /// The Milan listener's settle and its giving up of the SRP parameters,
    /// onto the stream inputs, with the Talker attribute each sink probed.
    void wire_milan_listener_connections(nanoavb::NanoAvbMilanAcmpListener& listener);
    /// Why Milan sink @p stream_index is stopping SRP, as its input's loss. Within stop_srp only.
    [[nodiscard]] auto milan_input_loss(uint16_t stream_index) const -> StreamInputLoss;
    /// What a sink with the SRP parameters @p srp follows in SRP.
    [[nodiscard]] static auto probed_talker(atdecc::MilanSrpParameters const& srp) noexcept -> nanoavb::TalkerMatch;
    /// The stream outputs have destination addresses that are theirs to use
    /// (Milan v1.3 4.3.3.1, 5.5.4.1 step 3).
    [[nodiscard]] auto talker_addresses_valid() const noexcept -> bool;
    /// REGISTERING_FAILED for stream output @p stream_index (Milan v1.3 Table 5.23).
    [[nodiscard]] auto talker_registering_failed(uint16_t stream_index) const -> bool;
    /// The entity is locked by a controller other than @p controller.
    [[nodiscard]] auto locked_out(ieee::Eui64 const& controller) const noexcept -> bool;
    /// This entity's gPTP grandmaster and domain, as its own ADP reports them.
    [[nodiscard]] auto own_gptp_state() const noexcept -> atdecc::MilanGptpState;
    /// The Talker attribute SRP registers that matches @p srp (Milan v1.3 5.3.8.8).
    [[nodiscard]] auto talker_registration_for(atdecc::MilanSrpParameters const& srp) const -> atdecc::MilanTalkerRegistration;
    /// The AEM third: the GET_COUNTERS and GET_STREAM_INFO fills, and the
    /// presentation offsets of SET and GET_MAX_TRANSIT_TIME.
    void wire_aem_stream_callbacks();
    /// SET_MAX_TRANSIT_TIME's @p offset_ns for talker stream @p idx, through
    /// on_presentation_offset_changed_ (NOT_SUPPORTED without it): its AEM status.
    [[nodiscard]] auto change_presentation_offset(uint16_t idx, uint32_t offset_ns) -> uint8_t;
    /// Whether @p idx is one of the kit's talker streams.
    [[nodiscard]] auto is_talker_index(uint16_t idx) const noexcept -> bool;
    /// The storage handler's SET_SAMPLING_RATE hook: SUCCESS for the rate in force; another
    /// rate re-times AUDIO_UNIT 0's streams through on_unit_rate_changed_, NOT_SUPPORTED without it.
    [[nodiscard]] auto change_sampling_rate(uint16_t audio_unit, uint32_t rate) -> uint8_t;
    /// AUDIO_UNIT 0's streams at @p from, each at @p rate; else NOT_SUPPORTED (an audio stream that
    /// lists no such format) or STREAM_IS_RUNNING (an input runs, Milan v1.3 Section 5.4.2.7).
    [[nodiscard]] auto retimed_formats(uint32_t from, uint32_t rate) -> std::expected<RetimedFormats, uint8_t>;
    /// Stream @p spec (an output when @p output) added to @p out when a re-time from @p from to
    /// @p rate moves it; else the status retimed_formats() refuses with, or SUCCESS.
    [[nodiscard]] auto add_retimed(bool output, StreamSpec const& spec, uint32_t from, uint32_t rate, RetimedFormats& out)
        -> uint8_t;
    /// That stream's format in force when a re-time from @p from moves it: an audio stream at
    /// that rate in AUDIO_UNIT 0's clock domain (IEEE 1722.1-2021 Clauses 7.2.3, 7.2.6); else nullopt.
    [[nodiscard]] auto retimed_from(bool output, StreamSpec const& spec, uint32_t from) -> std::optional<uint64_t>;
    /// Format @p word of stream @p index at @p rate; else NOT_SUPPORTED when the stream's descriptor
    /// does not list it, or STREAM_IS_RUNNING for a running input (Milan v1.3 Section 5.4.2.7).
    [[nodiscard]] auto retimed_to(bool output, uint16_t index, uint64_t word, uint32_t rate) const
        -> std::expected<uint64_t, uint8_t>;
    /// Whether STREAM_INPUT @p index runs: its Milan sink bound (Milan v1.3 Section 5.4.2.7), or
    /// connected under IEEE 1722.1 (Clause 7.4.9.2). Its format does not change while it does.
    [[nodiscard]] auto input_running(uint16_t index) const noexcept -> bool;
    /// @p retimed put in force at @p rate by the entity's data plane, then in the descriptors,
    /// its notifications left for after the response; the data plane's status.
    [[nodiscard]] auto retime_streams(uint32_t rate, RetimedFormats const& retimed) -> uint8_t;
    /// Notify @p retimed as SET_STREAM_FORMATs once the SET_SAMPLING_RATE is answered.
    void notify_after_response(RetimedFormats const& retimed);
    /// The SET_STREAM_FORMAT notification of each stream notify_after_response() was given.
    void notify_retimed();
    /// A PROBE_TX_COMMAND arrived for stream output @p stream_index: tell the
    /// SRP handler, which under the Milan profile declares the stream's
    /// Talker attribute on it (Milan v1.3 4.3.3.1).
    void note_talker_probe(uint16_t stream_index, TimePoint now);

    /// The SRP handler's Class A Domain in force changed: adopted from the
    /// wire, or back to the configured one after a startup or Link Up (Milan
    /// v1.3 4.2.7.2.1). Move the stream frames, MVRP and GET_AVB_INFO to it.
    void follow_domain(nanoavb::DomainInfo const& domain, TimePoint now);

    /// Move the data plane, ACMP's stream picture and the Talker Advertises
    /// to @p domain's VLAN and priority (the follow_domain half that is the
    /// streams'; MVRP and GET_AVB_INFO are the caller's).
    void move_talker_streams_to(nanoavb::DomainInfo const& domain, TimePoint now);

    /// The VLAN the streams use now: the adopted one, else the configured one.
    [[nodiscard]] auto current_vlan_id() const noexcept -> uint16_t;
    // The constructor's steps.
    void make_listener_streams(std::atomic<int64_t>& last_gptp_ns, StreamRxAudioSink* audio_sink);
    /// The AEM handler's options, the profile, and the SRP handler's defaults.
    void configure_control_plane();
    /// Everything the profile decides (milan_profile()).
    void apply_profile();
    /// Which ACMP runs: IEEE 1722.1-2021's, or under the Milan profile
    /// Milan v1.3's (NanoAvbComponents::use_milan_acmp), with the transmit
    /// gate to match.
    void apply_acmp_profile(bool milan);
    void apply_srp_profile(bool milan);
    /// Each stream output's stream id and static destination address into ACMP's picture.
    void seed_talker_streams();
    /// The file a kind of saved state goes in: @p named when the
    /// configuration names one; else, when @p wanted, the file @p name of
    /// the entity's state directory (avb_entity_state_dir.hpp); else empty.
    [[nodiscard]] auto state_file(std::string const& named, std::string_view name, bool wanted) -> std::string;
    /// The entity's own state directory, made at the first call that needs
    /// it; empty when the machine has no state directory or it may not be
    /// written, which one status line then says.
    [[nodiscard]] auto entity_state_dir() -> std::string const&;
    [[nodiscard]] auto static_dest_mac_for(StreamKind kind) const noexcept -> ieee::Eui48;
    /// The descriptor storage handler's CONTROL, streaming and clock source hooks.
    void install_storage_hooks();
    /// SET_CLOCK_SOURCE asked for @p source in clock domain @p domain. NOT_SUPPORTED, and nothing
    /// changed, for a source of domain 0 the entity cannot run (clock_source_runs()). Otherwise
    /// SUCCESS: domain 0's active source is @p source, its streams' media clock restarted when
    /// that is another, and the choice is saved.
    [[nodiscard]] auto change_clock_source(uint16_t domain, uint16_t source) -> uint8_t;
    /// The file the clock domains' chosen sources are saved in (Milan v1.3 5.3.11.1), empty when
    /// the machine has no state directory. To save, @p make the entity's own directory when it is
    /// not there; to load, only name the file.
    [[nodiscard]] auto clock_sources_file(bool make) -> std::string;
    /// Save every clock domain's source in force, clock domain @p domain's as @p source: the one
    /// a SET_CLOCK_SOURCE being answered is about to put in force.
    void save_clock_sources(uint16_t domain, uint16_t source);
    /// The clock sources saved before the last stop, back in force.
    void restore_clock_sources();
    /// One saved clock source to its domain, when the model still lists it and the entity can
    /// still run it; a status line says so when not.
    void restore_clock_source(SavedClockSource const& saved);
    /// The media clock of clock domain @p domain restarted: mr toggles on each of its open stream outputs.
    void restart_media_clock(uint16_t domain);
    /// START_STREAMING or STOP_STREAMING was accepted for a stream.
    [[nodiscard]] auto on_streaming_changed(uint16_t type, uint16_t index, bool streaming) -> uint8_t;
    /// START_STREAMING (@p stopped false) or STOP_STREAMING for STREAM_OUTPUT
    /// @p index: the talker's stream stopped or started under IEEE 1722.1,
    /// NOT_SUPPORTED and nothing changed under the Milan profile. The status
    /// of the command.
    [[nodiscard]] auto start_or_stop_output(uint16_t index, bool stopped) -> uint8_t;
    /// START_STREAMING (@p stopped false) or STOP_STREAMING for STREAM_INPUT
    /// @p index: to its Milan sink, or straight to the input under IEEE 1722.1.
    void start_or_stop_input(uint16_t index, bool stopped);
    /// Whether STREAM_INPUT @p index is to discard its frames: a Milan
    /// sink bound with the stopped state (5.3.8.7), or an IEEE 1722.1 input
    /// after STOP_STREAMING.
    [[nodiscard]] auto input_stopped(uint16_t index) const noexcept -> bool;
    /// The input's stopped state set, logged and notified (GET_STREAM_INFO), when it changes.
    void set_input_stopped(uint16_t index, bool stopped);
    /// Every stream input given the stopped state input_stopped() says.
    void apply_input_stopped_states();
    /// A Milan sink's binding changed: each input's stopped state, and its
    /// counters zeroed when it became bound.
    void follow_milan_bindings();
    void follow_inputs_bound();
    void follow_input_bound(uint16_t index);
    /// End the stream counters' observation interval once COUNTERS_INTERVAL_NS has passed since
    /// the last, at reactor-clock @p now_ns; under the Milan profile, at every COUNTERS_NOTIFY_FOLDS-th
    /// end, notify each stream descriptor whose counters changed since it was last told (GET_COUNTERS).
    /// The first call starts the first interval. end_cycle() calls it.
    void poll_counters(int64_t now_ns);
    /// End the observation interval of every stream's counters, and at every
    /// COUNTERS_NOTIFY_FOLDS-th one notify those that changed.
    void fold_stream_counters();
    /// The stream outputs' and the stream inputs' halves of fold_stream_counters():
    /// each notes which descriptors' counters changed.
    void fold_output_counters();
    void fold_input_counters();
    /// Notify each stream descriptor whose counters changed since it was last
    /// told, under the Milan profile, and forget the changes under either.
    void notify_changed_counters();
    /// Whether the counters of stream descriptor @p index, noted in @p changed, are to be told now:
    /// they changed and the profile is Milan's. The note is cleared.
    [[nodiscard]] auto take_counters_change(std::array<bool, MAX_ENTITY_STREAMS>& changed, uint16_t index) const noexcept -> bool;
    /// Keep the signal presence STREAM_OUTPUT @p spec reports now, within its channels, as this
    /// fold's; true when it changed.
    [[nodiscard]] auto fold_output_presence(StreamSpec const& spec) noexcept -> bool;
    /// The channels of STREAM_OUTPUT @p index's format, 0 for an index the entity has no output for.
    [[nodiscard]] auto output_channels(uint16_t index) const noexcept -> uint16_t;
    /// SET_STREAM_FORMAT named, for a stream, a format its descriptor
    /// describes: the status of the command. A stream input takes another
    /// channel count (change_input_format); a stream output keeps the one
    /// format its serializer was made for, and any other is NOT_SUPPORTED.
    [[nodiscard]] auto on_stream_format_changed(uint16_t type, uint16_t index, uint64_t format) -> uint8_t;
    /// STREAM_INPUT @p index to @p format: STREAM_IS_RUNNING while the input
    /// runs (input_running), NOT_SUPPORTED for a format the listener cannot
    /// be brought to; else the listener takes it and the inputs' formats
    /// are saved.
    [[nodiscard]] auto change_input_format(uint16_t index, uint64_t format) -> uint8_t;
    /// The GET_STREAM_INFO answer for a stream: the talker's or the
    /// listener's picture of it, with the format now in force. False for a
    /// stream the entity does not have.
    [[nodiscard]] auto stream_info(uint16_t type, uint16_t index, atdecc::aem::AemStreamInfoPayload& out) -> bool;
    /// The current format of a stream as its descriptor has it; 0 for a stream the model does not have.
    [[nodiscard]] auto current_stream_format(uint16_t type, uint16_t index) -> uint64_t;
    /// The file the streams' current formats are saved in (Milan v1.3
    /// 5.3.7.1, 5.3.8.1), empty when the machine has no state directory.
    /// To save, @p make the entity's own directory when it is not there;
    /// to load, only name the file.
    [[nodiscard]] auto stream_formats_file(bool make) -> std::string;
    /// Save every stream input's current format, STREAM_INPUT @p index
    /// having @p format: its descriptor takes that once the command's hook
    /// has returned.
    void save_input_formats(uint16_t index, uint64_t format);
    /// The formats saved before the last stop, back to the stream inputs.
    void restore_input_formats();
    /// One saved format to its input, when the model still describes it
    /// and the listener takes it; else the input stays as authored, and a
    /// status line says so.
    void restore_input_format(SavedStreamFormat const& saved);

    // start()'s steps, in order.
    [[nodiscard]] auto bring_up_control_plane(net::MessageReactor& reactor, TimePoint now) -> Status;
    /// What every start puts back: the configured Domain, valid stream
    /// addresses, the stream callbacks, the VID, and Milan sinks at startup.
    void restore_control_state(TimePoint now);
    void restart_milan_sinks(TimePoint now);
    /// The talker's tu follows the grandmaster of this start's Announce handler, none at first.
    void follow_grandmaster();
    /// The talker told whether a grandmaster is in force (@p in_force), as of now.
    void notify_talker_of_grandmaster(bool in_force) noexcept;
    /// On config_.net_backend unless the host opened it already. Not fatal when it does not
    /// open: the entity keeps its control plane.
    void open_stream_port();
    [[nodiscard]] auto claim_stream_addresses(net::MessageReactor& reactor, TimePoint now) -> Status;
    [[nodiscard]] auto open_streams(net::MessageReactor& reactor) -> Status;
    [[nodiscard]] auto open_talker_streams() -> Status;
    [[nodiscard]] auto open_listener_streams(net::MessageReactor& reactor) -> Status;
    /// Open stream input @p spec; the group it joins from the start (zero: none).
    [[nodiscard]] auto open_listener_stream(StreamSpec const& spec) -> StatusValue<ieee::Eui48>;
    void attach_listener_rx(std::span<ieee::Eui48 const> rx_groups, net::MessageReactor& reactor);
    /// The TX path, the diagnostics, then each talker gate open now told.
    void finish_start();
    void report_inert_controls();

    /// The MRP timers of both SRP handlers: Milan v1.3 Table 4.3's under the
    /// Milan profile, IEEE 802.1Q Table 10-7's otherwise.
    void apply_mrp_timers(bool milan);
    /// Give the AEM handler the GET_MILAN_INFO answer a Milan model makes, or
    /// none for any other model.
    void install_milan_info();
    /// Say in the ENTITY descriptor and in ADP what a Milan PAAD-AE is held
    /// to say of itself (Milan v1.3 5.3.3.1, 5.6.2).
    void advertise_milan_capabilities();
    /// The features_flags of GET_MILAN_INFO: the configuration's, with
    /// MVU_BINDING set under the Milan profile and clear under any other.
    [[nodiscard]] auto milan_features_flags() const noexcept -> uint32_t;
    /// @p text as a Milan version word, or @p fallback (logged against the
    /// configuration key @p key) when it is not dotted numbers.
    [[nodiscard]] auto milan_version_or(std::string const& text, uint32_t fallback, std::string_view key) -> uint32_t;
    /// Let go of the data plane start() built: the RX handler kept for an RX
    /// timer, the talker's and the listener's slots, then the port. What the
    /// next start() opens is then the whole of it, as on a first start.
    void close_streams() noexcept;
    /// Stream @p index's identity (ACMP), VLAN and priority (the domain in force) into @p info.
    void fill_talker_srp_identity(uint16_t index, nanoavb::TalkerStreamSrpInfo& info) const;
    void advertise_talker_streams(TimePoint time);
    void withdraw_talker_streams(TimePoint time);
    /// One pass of update_talker_declarations over the talker streams.
    void run_talker_declarations(int64_t now_ns, bool refresh);
    /// The ACMP talker's Milan hooks, the AEM presentation offset store and
    /// the GET_STREAM_INFO fills.
    void wire_milan_talker();
    /// Re-read talker stream @p idx's registered Listener at steady-clock
    /// @p now_ns; true when it changed (the declaration predicate and
    /// GET_STREAM_INFO follow it).
    auto refresh_talker_listener(uint16_t idx, int64_t now_ns) -> bool;
    /// The talker spec index for @p stream_id, or nullopt.
    [[nodiscard]] auto talker_index_of(nanoavb::StreamId const& stream_id) const noexcept -> std::optional<uint16_t>;
    [[nodiscard]] auto is_talker_index(uint16_t idx) const noexcept -> bool;
    /// Fires the unsolicited GET_STREAM_INFO for talker stream @p idx.
    void notify_talker_stream_info(uint16_t idx);
    [[nodiscard]] auto setup_maap_addresses(net::MessageReactor& reactor, TimePoint now) -> Status;
    /// start(): load the persistent state and apply all but the listener
    /// bindings (kept for apply_saved_listener_bindings).
    void load_persistent_state();
    /// Apply one saved setting, logging (and dropping) what the model refuses.
    void apply_saved_settings(PersistentState const& saved);
    /// start(), the listener up: arm the saved bindings as fast-connect goals
    /// and save every change of them.
    void apply_saved_listener_bindings();
    /// The entity model id controllers see (the ENTITY descriptor served).
    [[nodiscard]] auto served_entity_model_id() const -> uint64_t;
    /// The presentation offset a talker stream starts with.
    [[nodiscard]] auto default_presentation_offset_ns() const noexcept -> uint32_t;
    /// Recompute the admitted set at steady-clock @p now_ns after a gate input
    /// changed (or time passed); fire on_admission_changed_ when it differs
    /// from the last one.
    void note_admission(int64_t now_ns);
    /// The steady clock in ns: the gate's time base (grace window stamps).
    [[nodiscard]] static auto steady_now_ns() noexcept -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    /// Control-plane time @p time as the gate's steady-clock ns.
    [[nodiscard]] static auto steady_ns_of(TimePoint const time) noexcept -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    }
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
    /// AUDIO_UNIT 0's CLOCK_DOMAIN, whose streams a SET_SAMPLING_RATE re-times; nullopt without
    /// the unit, or when another unit shares the domain (its rate would change with it).
    std::optional<uint16_t> retimed_clock_domain_;

    /// Reusable AVB control plane: talker/listener stream counts from the
    /// blob, 4 max listeners per talker stream.
    AvbEntityHost host_;

    /// Per-stream transmit gate (MSRP listener declaration + grace). Binds config_ + components.
    TalkerGate gate_;
    /// When each talker stream declares its Talker Advertise, and the deadline
    /// its timer next wakes for (nullopt: none).
    TalkerDeclarations declarations_;
    std::optional<int64_t> declaration_deadline_ns_{};
    bool updating_declarations_{false};
    bool declarations_pending_{false};          ///< asked for while updating
    bool declarations_pending_refresh_{false};  ///< ... with refresh
    /// What the declarations' timer reaches the kit through (start(); stop()
    /// drops it and the timer finishes).
    std::shared_ptr<AvbEntityKit* const> declaration_timer_token_{};
    /// The registered Listener per talker stream (refresh_talker_listener).
    std::array<srp::msrp::ListenerDeclaration, MAX_ENTITY_STREAMS> talker_listener_{};
    /// Each STREAM_OUTPUT's avb_interface_index (Milan PROBE_TX check).
    std::array<uint16_t, MAX_ENTITY_STREAMS> talker_interface_{};
    /// Per talker stream presentation time offset (written on the reactor,
    /// read by the data plane and GET_STREAM_INFO).
    std::array<std::atomic<uint32_t>, MAX_ENTITY_STREAMS> presentation_offset_ns_{};
    PresentationOffsetFn on_presentation_offset_changed_{};
    /// The persistent state: the entity name the handler was configured
    /// with, the bindings start() arms once the listener is up, and the text
    /// last written (persist() skips an unchanged state).
    std::string configured_entity_name_;
    std::vector<PersistedListenerBinding> saved_bindings_{};
    std::string persisted_text_{};
    bool persistence_active_{false};   ///< a state path is configured
    bool persistence_blocked_{false};  ///< the file did not read at start: nothing written this run
    bool persist_pending_{false};      ///< a listener goal changed this reactor pass
    /// The admitted set last reported, and its listener.
    AdmittedStreams admitted_{};
    AdmissionChangedFn on_admission_changed_{};

    /// The one port every stream frame goes through, backend from
    /// config_.net_backend or opened by the host (stream_port()). Declared before the talker, the listener and the
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
    /// entity_state_dir()'s answer once asked for; unset before.
    std::optional<std::string> entity_state_dir_{};
    bool link_up_{false};

    /// CRF-input media-clock recovery: fed by the CRF stream input's
    /// timestamps on the RX/reactor thread; consulted by media_rate() on the
    /// media thread when the CRF clock source is active.
    CrfClockRecovery crf_recovery_{};
    /// The CLOCK_DOMAIN's active clock-source index. Written by the
    /// SET_CLOCK_SOURCE apply callback (reactor thread), read per tick by
    /// the media thread.
    std::atomic<uint16_t> active_clock_source_{0};
    std::optional<uint16_t> crf_clock_source_index_{};
    /// What each CLOCK_SOURCE of the model is, by descriptor index. Fixed at construction.
    ClockSourceKinds clock_source_kinds_;
    /// The External clock sources run_clock_source() named, by descriptor index.
    std::bitset<MAX_CLOCK_SOURCES> external_clock_sources_run_{};

#if defined(__linux__)
    /// The listener's VID on the NIC's VLAN filter: start()'s own client
    /// (when no other control was bound), declared before the ListenerVlan
    /// that deletes its link through it on destruction.
    std::unique_ptr<net::vlan::VlanLinkClient> vlan_client_{};
    ListenerVlan listener_vlan_{host_.ctl_log()};
#endif

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
