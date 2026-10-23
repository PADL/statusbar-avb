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
#include "statusbar/avb_entity/avb_entity_listener_vlan.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_persistent_state.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_declarations.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
        std::atomic<uint64_t>& last_gptp_ns,
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
    /// (menu/selection: never an error).
    [[nodiscard]] auto start(net::MessageReactor& reactor) -> Status;
    /// Stop the control plane (fails with not_connected when not running).
    /// Deletes the listener's VLAN link first if the kit created it (even
    /// when not running).
    [[nodiscard]] auto stop() -> Status;

#if defined(__linux__)
    // --- The listener's VID on the NIC's VLAN filter (Linux) -------------------

    /// Keep the SR-class VID in use on @p ifindex's receive VLAN filter through
    /// @p control until stop(); a no-op without stream inputs. start() calls it
    /// unless called first; @p control must outlive the kit.
    void bind_listener_vlan(net::vlan::VlanControl& control, int ifindex);
    [[nodiscard]] auto listener_vlan() const noexcept -> ListenerVlan const& { return listener_vlan_; }
#endif

    // --- Event handlers (drive the shared SM stack) ---------------------------
    void on_link_up(TimePoint time) { host_.on_link_up(time); }
    /// gPTP grandmaster found or changed / lost (EntityUpkeep wires these to
    /// the Announce handler): drives the talker streams' AVTP tu bit (Milan
    /// v1.2 Section 4.3.5.2, Annex B.1.1). Reactor thread.
    void note_grandmaster_changed()
    {
        host_.note_grandmaster_changed();
        talker_->note_grandmaster_changed(steady_now_ns());
    }
    void note_grandmaster_lost() noexcept { talker_->note_grandmaster_lost(); }
    void on_link_down(TimePoint time) { host_.on_link_down(time); }
    void on_gptp_announce(TimePoint time, bool has_grandmaster) { host_.on_gptp_announce(time, has_grandmaster); }
    void on_timeout(TimePoint time) { host_.on_timeout(time); }

    // --- Media-thread (RT) calls ----------------------------------------------

    /// Whether talker stream @p idx transmits this tick: admitted (or gating off),
    /// its MAAP address ready in "maap" mode, and not STOP_STREAMING'd.
    [[nodiscard]] auto talker_should_transmit(uint16_t const idx, int64_t const now_ns) const noexcept -> bool
    {
        if (!maap_.ready() || talker_->stream_stopped(idx)) {
            return false;
        }
        return gate_.should_transmit(idx, now_ns);
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

    /// Batch-drain the stream RX socket from the tool's dedicated SCHED_FIFO RX
    /// timer (config.stream_rx_rt_timer). No-op when RX is on the reactor.
    auto drain_stream_rx(int64_t const wake_gptp_ns) -> size_t
    {
        return (rt_rx_handler_ != nullptr && listener_ != nullptr) ? listener_->drain_rx(wake_gptp_ns) : 0;
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
    [[nodiscard]] auto make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo;
    void advertise_talker_streams(TimePoint time);
    void withdraw_talker_streams(TimePoint time);
    /// One pass of update_talker_declarations over the talker streams.
    void run_talker_declarations(int64_t now_ns, bool refresh);
    /// The ACMP talker's Milan hooks, the AEM presentation offset store and
    /// the GET_STREAM_INFO fills.
    void wire_milan_talker();
    /// The Milan listener's sink hooks (persistence, stopped inputs,
    /// GET_STREAM_INFO notifications) and its MSRP talker registration feed.
    void wire_milan_listener();
    /// Report settled Milan sink @p index's Talker registration (Section
    /// 5.5.3.3: matching stream ID, destination address and VLAN ID).
    void refresh_sink_registration(uint16_t index);
    /// A settled Milan input's VID joins MVRP (refcounted; the Class
    /// A VID is the host's), and leaves when the last such input goes.
    void join_sink_vlan(uint16_t vid);
    void leave_sink_vlan(uint16_t vid);
    /// Re-read talker stream @p idx's registered Listener at steady-clock
    /// @p now_ns; true when it changed (the declaration predicate and
    /// GET_STREAM_INFO follow it).
    auto refresh_talker_listener(uint16_t idx, int64_t now_ns) -> bool;
    /// The talker spec index for @p stream_id, or nullopt.
    [[nodiscard]] auto talker_index_of(nanoavb::StreamId const& stream_id) const noexcept -> std::optional<uint16_t>;
    [[nodiscard]] auto is_talker_index(uint16_t idx) const noexcept -> bool;
    /// Fires the unsolicited GET_STREAM_INFO for talker stream @p idx.
    void notify_talker_stream_info(uint16_t idx);
    [[nodiscard]] auto acquire_maap_addresses(net::MessageReactor& reactor) -> Status;
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
    /// The stream kind of talker stream @p index ("other" when not in the table).
    [[nodiscard]] auto talker_kind_of(uint16_t index) const noexcept -> StreamKind;

    AvbEntityAudioIOConfig const& config_;
    StreamSpecs talker_specs_;
    StreamSpecs listener_specs_;
    uint32_t sample_rate_;
    /// The owning entity's blob-backed descriptor handler; carries the
    /// built-ins the kit's lifecycle/CONTROL hooks dispatch from.
    nanoavb::DescriptorStorageHandler& storage_handler_;

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
    /// The VIDs connected or settled inputs use, each with its count.
    sg14::inplace_vector<std::pair<uint16_t, uint16_t>, MAX_ENTITY_STREAMS> sink_vlans_{};
    /// The admitted set last reported, and its listener.
    AdmittedStreams admitted_{};
    AdmissionChangedFn on_admission_changed_{};

    /// Stream TX path: qdisc-bypass socket + spec-shaped serializer slots + TX
    /// counters + capture recorder. Binds the ENTITY's media clock + gPTP atomic.
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
