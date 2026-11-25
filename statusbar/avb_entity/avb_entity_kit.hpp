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
#include <string>
#include <string_view>
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
    void on_gptp_announce(TimePoint time, bool has_grandmaster) { host_.on_gptp_announce(time, has_grandmaster); }
    void on_timeout(TimePoint time) { host_.on_timeout(time); }

    /// How long the loop driving the reactor may wait this cycle: @p max_ms,
    /// or less when a MAAP probe or announce or an MRP timer falls due
    /// sooner, told the reactor-clock @p now_ns the loop read. Reactor
    /// thread only.
    [[nodiscard]] auto poll_timeout_ms(int const max_ms, int64_t const now_ns) const noexcept -> int
    {
        return avtp::MaapHandler::poll_timeout_ms(next_control_deadline_ns(), now_ns, max_ms);
    }

    /// The earliest of the MAAP deadline and the MSRP and MVRP timers. An MRP
    /// jointimer is 200 ms, so a loop that only woke every 100 ms sent a
    /// declaration's second New, and the Join after a LeaveAll, up to 100 ms
    /// past the JoinTime.
    [[nodiscard]] auto next_control_deadline_ns() const noexcept -> std::optional<int64_t>;

    // --- Media-thread (RT) calls ----------------------------------------------
    /// Whether talker stream @p idx should put its AVTP stream on the wire this
    /// tick (gating disabled, or a downstream listener is ready/connected), told
    /// the wake's CLOCK_MONOTONIC_RAW instant @p now (the gate's grace clock).
    /// Also gated on MAAP-address readiness in "maap" mode.
    [[nodiscard]] auto talker_should_transmit(uint16_t const idx, realtime::MonotonicRawClock::time_point const now) noexcept
        -> bool
    {
        if (!maap_.ready()) {
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
    /// The MSRP Talker Advertise talker stream @p spec is declared with:
    /// identity from ACMP's picture of the stream, VLAN and priority from the
    /// SR class domain in force (configured, or adopted from the wire).
    [[nodiscard]] auto make_talker_srp_info(StreamSpec const& spec) const -> nanoavb::TalkerStreamSrpInfo;
    /// Whether the entity answers to Milan v1.3 where it and IEEE 1722.1-2021
    /// differ: the configured atdecc_profile when there is one, else the
    /// model's (a Milan model does, any other does not).
    [[nodiscard]] auto milan_profile() const noexcept -> bool;
    [[nodiscard]] auto listener_specs() const noexcept -> StreamSpecs const& { return listener_specs_; }
    [[nodiscard]] auto sample_rate() const noexcept -> uint32_t { return sample_rate_; }

  private:
    /// Attach the stream-specific control-plane behavior to the host: MSRP
    /// advertise/withdraw, the ACMP talker log + transmit-gate publication,
    /// the ACMP listener connect/disconnect drive, and the AEM
    /// GET_COUNTERS / GET_STREAM_INFO fills.
    void wire_stream_callbacks();
    /// The MSRP third of wire_stream_callbacks(): advertise, withdraw, and the transmit gate's Listener Ready.
    void wire_srp_callbacks();
    /// The ACMP third, for whichever ACMP the components run.
    void wire_acmp_callbacks();
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
    /// The AEM third: the GET_COUNTERS and GET_STREAM_INFO fills.
    void wire_aem_stream_callbacks();
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
    /// START_STREAMING or STOP_STREAMING was accepted for a stream.
    [[nodiscard]] auto on_streaming_changed(uint16_t type, uint16_t index, bool streaming) -> uint8_t;
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

    // start()'s steps, in order.
    [[nodiscard]] auto bring_up_control_plane(net::MessageReactor& reactor, TimePoint now) -> Status;
    /// What every start puts back: the configured Domain, valid stream
    /// addresses, the stream callbacks, the VID, and Milan sinks at startup.
    void restore_control_state(TimePoint now);
    void restart_milan_sinks(TimePoint now);
    /// Not fatal when it does not open: the entity keeps its control plane.
    void open_stream_port();
    [[nodiscard]] auto claim_stream_addresses(net::MessageReactor& reactor, TimePoint now) -> Status;
    [[nodiscard]] auto open_streams(net::MessageReactor& reactor) -> Status;
    [[nodiscard]] auto open_talker_streams() -> Status;
    [[nodiscard]] auto open_listener_streams(net::MessageReactor& reactor) -> Status;
    /// Open stream input @p spec; the group it joins from the start (zero: none).
    [[nodiscard]] auto open_listener_stream(StreamSpec const& spec) -> StatusValue<ieee::Eui48>;
    void attach_listener_rx(std::span<ieee::Eui48 const> rx_groups, net::MessageReactor& reactor);
    /// The TX path, then the diagnostics.
    void finish_start();
    void report_inert_controls();

    /// The MRP timers of both SRP handlers: Milan v1.3 Table 4.3's under the
    /// Milan profile, IEEE 802.1Q Table 10-7's otherwise.
    void apply_mrp_timers(bool milan);
    /// Give the AEM handler the GET_MILAN_INFO answer a Milan model makes, or
    /// none for any other model.
    void install_milan_info();
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
    [[nodiscard]] auto setup_maap_addresses(net::MessageReactor& reactor, TimePoint now) -> Status;
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
