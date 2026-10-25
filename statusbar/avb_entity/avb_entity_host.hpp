#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_host.hpp
/// @brief AvbEntityHost — the reusable AVB entity control plane.
///
/// Every AVB entity needs the same control-plane scaffolding: the nanoavb state
/// machines (gPTP, MVRP, MSRP talker/listener, supervisor, talker/listener
/// engines), a NanoAvbComponents (ADP advertiser + ACMP + MSRP + AEM handler),
/// the net handlers, and the lifecycle that drives them (start/stop, link up/down,
/// gPTP locked/unlocked, the SR-domain advertise cycle). That scaffolding is
/// identical across entities and is the bulk of the boilerplate; this host owns it
/// once so a concrete entity supplies only its STREAMS and its per-tick data plane.
///
/// **Building-block usage** (compose, don't inherit):
/// @code
/// class MyEntity {
///     AvbEntityHost host_;          // <- this building block
///     TalkerStreams  talker_;        // your TX
///     ListenerStreams listener_;     // your RX
///   public:
///     MyEntity(nanoavb::EntityModel model, Config cfg)
///         : host_{std::move(model), make_adp(cfg), /*talkers*/3, /*listeners_per*/4, /*listeners*/2} {}
///
///     auto start(net::MessageReactor& r) -> Status {
///         if (auto s = host_.start_control_plane(r, cfg_.interface_name); !s) return s;
///         // attach your stream behavior via the hybrid API:
///         host_.set_advertise_streams([this](TimePoint t) { advertise_my_streams(t); });
///         host_.set_withdraw_streams([this](TimePoint t) { withdraw_my_streams(t); });
///         host_.set_on_listener_ready([this](nanoavb::StreamId const& id, bool r) { gate_.note_listener_ready(id, r); });
///         host_.components().acmp_talker.set_connection_callbacks(/* yours */);
///         host_.components().acmp_listener.set_connection_callbacks(/* yours */);
///         setup_my_data_plane(r);
///         return success();
///     }
///     void on_link_up(TimePoint t) { host_.on_link_up(t); }   // forward lifecycle
///     auto is_ready() const -> bool { return host_.is_ready(); }
///     void process_audio(TimePoint t);  // YOURS
///     void print_state() const;         // YOURS
/// };
/// @endcode
///
/// The host owns ALL state-machine wiring internally — the nanoavb SMs never leak
/// into entity code. A concrete entity touches the control plane only through
/// components() (ACMP/MSRP/AEM) and the three typed stream hooks below.

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_avb_info_mirror.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace statusbar::avb_entity {

using statusbar::Status;

/// The default ADP advertiser config shared by every avb_entity: valid_time 10
/// (20 s) and a 5 s reannounce interval (Milan v1.2 Section 5.6.2).
[[nodiscard]] inline auto default_adp_advertiser_config() noexcept -> nanoavb::AdpAdvertiserConfig
{
    nanoavb::AdpAdvertiserConfig cfg{};
    cfg.valid_time = 10;                                        // 20 seconds
    cfg.reannounce_interval = std::chrono::milliseconds{5000};  // 5 seconds
    return cfg;
}

/// An AS path from an Announce's path trace (802.1AS-2020 10.3.9.23): the
/// trace, then @p own_clock_identity unless it ends the trace already (this
/// system is the grandmaster) or is unknown; empty when the trace is.
[[nodiscard]] auto compose_as_path(nanoavb::AsPath const& path_trace, std::optional<ieee::Eui64> const& own_clock_identity)
    -> nanoavb::AsPath;

/// The shared AVB entity control plane: state machines + NanoAvbComponents + net
/// handlers + lifecycle. Owned by a concrete entity (composition). Non-copyable and
/// non-movable (NanoAvbComponents pins itself), so build it in place in the entity's
/// constructor init list.
class AvbEntityHost
{
  public:
    using TimePoint = sm::TimePoint;

    /// @param model               the AEM entity model (from a blob or hand-built).
    /// @param adp_config          ADP advertiser identity/capabilities.
    /// @param talker_max_streams  number of STREAM_OUTPUT (talker) streams.
    /// @param talker_max_listeners max listeners per talker stream.
    /// @param listener_max_streams number of STREAM_INPUT (listener) streams.
    /// @param milan  entity profile override (nanoavb::EntityProfile); unset
    ///               takes the model's, else Milan.
    AvbEntityHost(
        nanoavb::EntityModel model,
        nanoavb::AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams,
        std::optional<bool> milan = std::nullopt);

    /// Symbol-aware construction. The host serves descriptors and resolves their
    /// well-known **symbols** through a caller-built @p handler (typically a
    /// DescriptorStorageHandler subclass that serves the entity's .aem blob and
    /// patches dynamic fields) instead of a parsed model. Enables symbol_of() /
    /// descriptor_for_symbol() / get_descriptor(). The host neither owns nor
    /// copies the handler: it must outlive the host (declare it before the host).
    /// @param handler  the entity's descriptor/command handler.
    AvbEntityHost(
        nanoavb::AemEntityHandler& handler,
        nanoavb::AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams,
        std::optional<bool> milan = std::nullopt);

    AvbEntityHost(AvbEntityHost const&) = delete;
    auto operator=(AvbEntityHost const&) -> AvbEntityHost& = delete;
    AvbEntityHost(AvbEntityHost&&) = delete;
    auto operator=(AvbEntityHost&&) -> AvbEntityHost& = delete;
    ~AvbEntityHost();

    // --- Lifecycle -------------------------------------------------------------

    /// Bring up the control plane on @p interface_name: create the net handlers, add
    /// them to @p reactor, wire the (generic) state-machine callbacks, and mark the
    /// host running. Call this first from the entity's start(), then attach stream
    /// behavior + the data plane. Idempotent guard: fails if already running.
    [[nodiscard]] auto start_control_plane(net::MessageReactor& reactor, std::string_view interface_name) -> Status;

    /// Stop ADP advertising and release the net handlers. The entity should tear down
    /// its own data plane (sockets, workers) around this call. Fails if not running.
    [[nodiscard]] auto stop_control_plane(TimePoint now) -> Status;

    [[nodiscard]] auto is_running() const noexcept -> bool { return running_; }

    /// Drive this entity's OWN identify control (front-panel button, GPIO,
    /// software trigger): applies the value as a controller's SET_CONTROL would
    /// and multicasts the unsolicited IDENTIFY notification. Call from the
    /// entity's event loop. Returns false when the model has no identify
    /// control (or the value was refused).
    auto set_local_identify(bool const on) -> bool
    {
        return components_.aem_handler.set_local_identify(on) == nanoavb::AEM_STATUS_SUCCESS;
    }

    // --- Event handlers (forward the entity's events into the SM stack) --------
    void on_link_up(TimePoint time);
    void on_link_down(TimePoint time);
    void on_gptp_announce(TimePoint time, bool has_grandmaster);
    void on_timeout(TimePoint time);

    /// A gPTP grandmaster change seen (the Announce handler's): counted for
    /// GET_COUNTERS (AVB_INTERFACE GPTP_GM_CHANGED). Reactor thread.
    void note_grandmaster_changed();
    /// The grandmaster was lost (no Announce within the receipt timeout).
    /// Reactor thread.
    void note_grandmaster_lost();
    /// The Announces' path trace changed: an unsolicited GET_AS_PATH (Milan
    /// v1.2 Table 5.22). Reactor thread.
    void note_as_path_changed();

    /// The most CLOCK_DOMAINs whose LOCKED/UNLOCKED counters the host keeps.
    static constexpr uint16_t MAX_CLOCK_DOMAINS = 4;
    /// CLOCK_DOMAIN @p index locked or unlocked: a transition counts LOCKED or UNLOCKED (IEEE 1722.1-2021
    /// Clause 7.4.42.2.3) and is notified. From its first call for domain 0, domain 0 no longer follows
    /// gPTP. False for a domain the model lacks or past MAX_CLOCK_DOMAINS. Reactor thread.
    [[nodiscard]] auto note_clock_domain_locked(uint16_t index, bool locked) -> bool;

    /// The gPTP path for AVB_INTERFACE @p index (IEEE 1722.1-2021 Clause 7.4.41): the
    /// last Announce's path trace, then this entity's clock identity unless already
    /// last; empty with no grandmaster. False for an unknown interface. Reactor thread.
    [[nodiscard]] auto as_path(uint16_t index, nanoavb::AsPath& path) const -> bool;

    /// The clock identity AVB_INTERFACE 0 reports, once known.
    [[nodiscard]] auto own_clock_identity() const noexcept -> std::optional<ieee::Eui64> const& { return own_clock_identity_; }

    /// GET_COUNTERS for ENTITY, AVB_INTERFACE and CLOCK_DOMAIN descriptors (IEEE
    /// 1722.1-2021 Clause 7.4.42); see docs/AVB_ENTITY_KIT.md. False for any other
    /// descriptor or one the model lacks. Reactor thread.
    [[nodiscard]] auto fill_counters(
        uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) const -> bool;

    // --- State queries ---------------------------------------------------------
    [[nodiscard]] auto is_ready() const noexcept -> bool;
    [[nodiscard]] auto state_string() const -> std::string_view;

    // --- Control-plane access for the entity's stream wiring -------------------
    [[nodiscard]] auto components() noexcept -> nanoavb::NanoAvbComponents& { return components_; }
    [[nodiscard]] auto components() const noexcept -> nanoavb::NanoAvbComponents const& { return components_; }
    [[nodiscard]] auto net_handlers() noexcept -> nanoavb::NanoAvbNetHandlers* { return net_handlers_.get(); }

    /// Kit phase 5b: persist listener fast-connect bindings at @p path.
    /// Loads any saved bindings as fast-connect goals (the ACMP listener's
    /// reactor tick then re-connects each remembered talker until it
    /// answers), turns on sticky bindings (every successful connect is
    /// remembered; a controller DISCONNECT forgets it) and mirrors every
    /// goal change back to the file. One call from any kit entity's start
    /// path; empty @p path disables. Reactor thread only.
    void enable_listener_binding_persistence(std::string path);

    /// Serve local ATDECC controllers on an AF_UNIX datagram socket at @p path with
    /// file mode @p mode. Call before start_control_plane(); empty disables. A socket
    /// that will not open is a warning: the entity still serves the network.
    void set_local_atdecc_socket(std::string path, int mode = 0660)
    {
        local_atdecc_socket_ = nanoavb::AtdeccLocalPort::Config{.path = std::move(path), .mode = mode};
    }
    /// The local ATDECC port while the control plane is up, or nullptr.
    [[nodiscard]] auto local_atdecc_port() noexcept -> nanoavb::AtdeccLocalPort*
    {
        return net_handlers_ != nullptr ? net_handlers_->local_port() : nullptr;
    }

    // --- Logging -----------------------------------------------------------------
    //
    // The host owns two SPSC log channels, one per producer thread context:
    //   ctl   — the reactor/main thread (control plane: ACMP/MSRP/MAAP/identify,
    //           supervisor events, periodic status).
    //   media — the real-time media-timer thread. Deferred formatting makes
    //           logging from process_audio() safe (see statusbar/logging).
    // The entity's tool registers both with its LogCollector and drains them
    // from its main loop. Verbosity per channel at runtime; Debug lines under
    // --verbose is the tools' convention.

    /// Producer facade for reactor/main-thread code (single thread only).
    [[nodiscard]] auto ctl_log() noexcept -> logging::Logger { return ctl_log_channel_.logger(); }
    /// Producer facade for the RT media-timer thread (single thread only).
    [[nodiscard]] auto media_log() noexcept -> logging::Logger { return media_log_channel_.logger(); }

    [[nodiscard]] auto ctl_log_channel() noexcept -> logging::LogChannelBase& { return ctl_log_channel_; }
    [[nodiscard]] auto media_log_channel() noexcept -> logging::LogChannelBase& { return media_log_channel_; }

    /// Set both channels' verbosity (any thread).
    void set_log_verbosity(logging::LogLevel const v) noexcept
    {
        ctl_log_channel_.set_verbosity(v);
        media_log_channel_.set_verbosity(v);
    }

    // --- Symbol <-> descriptor lookups (symbol-aware construction only) ---------
    // These let entity command-handling code reference descriptors by the designer's
    // well-known symbol instead of the blob's (volatile) descriptor index. All return
    // empty/failure on the legacy parsed-model path (no backing storage).

    /// The backing descriptor blob, or nullptr on the legacy path.
    [[nodiscard]] auto descriptor_storage() const noexcept -> atdecc::aem::DescriptorStorage const*
    {
        return handler_ != nullptr ? handler_->descriptor_storage() : nullptr;
    }

    /// Forward: the well-known symbol assigned to descriptor (@p type, @p index) in
    /// configuration @p config, or nullopt if none / no storage.
    [[nodiscard]] auto symbol_of(uint16_t type, uint16_t index, uint16_t config = 0) const noexcept -> std::optional<uint32_t>
    {
        auto const* store = descriptor_storage();
        if (store == nullptr) {
            return std::nullopt;
        }
        auto r = store->get_symbol(config, type, index);
        return r ? std::optional<uint32_t>{*r} : std::nullopt;
    }

    /// Reverse: the descriptor (config/type/index) carrying @p symbol, or failure.
    [[nodiscard]] auto descriptor_for_symbol(uint32_t symbol) const noexcept
        -> StatusValue<atdecc::aem::DescriptorStorageSymbolEntry>
    {
        auto const* store = descriptor_storage();
        if (store == nullptr) {
            return failure(BufferError::invalid_offset);
        }
        return store->find_by_symbol(symbol);
    }

    /// The raw on-wire bytes of descriptor (@p type, @p index) -- the GET_DESCRIPTOR
    /// equivalent, e.g. to decode a CONTROL descriptor's value ranges. Failure if none.
    [[nodiscard]] auto get_descriptor(uint16_t type, uint16_t index, uint16_t config = 0) const noexcept
        -> StatusValue<std::span<uint8_t const>>
    {
        auto const* store = descriptor_storage();
        if (store == nullptr) {
            return failure(BufferError::invalid_offset);
        }
        return store->get_descriptor(config, type, index);
    }

    /// gPTP / MVRP status for the entity's print_state (the SM contexts are private).
    [[nodiscard]] auto gptp_locked() const noexcept -> bool { return gptp_ctx_.time_locked; }

    // --- AVB_INTERFACE and GET_AVB_INFO: the entity's gPTP state over 1722.1 --
    //
    // The host answers GET_AVB_INFO (IEEE 1722.1 7.4.40) for every entity and
    // keeps the AVB_INTERFACE descriptor's identity fields truthful (through
    // the handler's set_avb_interface_runtime). By default it reports what it
    // observes on the AVB wire: the grandmaster and domain of the gPTP
    // Announces, its own gPTP state machine, its link and SRP state, and the
    // NIC's MAC-derived clock identity. A proxy entity instead mirrors the
    // device it stands in for over 1722.1 (mirror_avb_info_from): that
    // device's AVB_INTERFACE descriptors and GET_AVB_INFO answers become the
    // proxy's own. Changes go out as unsolicited GET_AVB_INFO notifications
    // (7.5.2) and update the ADP grandmaster.

    /// Mirror @p target_entity_id's AVB interface state over 1722.1, sending
    /// as @p controller_entity_id (the entity's own id is fine). Call before
    /// start_control_plane(); the mirror joins the reactor with the control
    /// plane. Interface index N of the target maps to AVB_INTERFACE N here.
    void mirror_avb_info_from(
        ieee::Eui64 target_entity_id, ieee::Eui64 controller_entity_id, int64_t poll_interval_ns = 5'000'000'000);

    /// The entity whose AVB interface state is mirrored, if any.
    [[nodiscard]] auto mirror_target() const noexcept -> std::optional<ieee::Eui64>;

    /// The GET_AVB_INFO answer for AVB_INTERFACE @p index: what controllers see.
    [[nodiscard]] auto avb_info(uint16_t index = 0) const -> nanoavb::AvbInfo;

    /// Propagation delay (ns) to report in the observed (non-mirrored) state
    /// (e.g. the gPTP port's neighborPropDelay); a change is notified (Clause 7.4.40).
    void set_propagation_delay_ns(uint32_t ns);
    [[nodiscard]] auto mvrp_joined() const noexcept -> bool { return mvrp_ctx_.joined; }

    // --- Typed stream hooks (the stream-specific bits of the SM wiring) ---------

    /// Called when MSRP enters the advertise state, AFTER the host has declared the
    /// SR class domain for you. Declare your talker reservations here (one
    /// talker_advertise per STREAM_OUTPUT). Stream-set-specific, so the entity supplies it.
    void set_advertise_streams(statusbar::sg14::inplace_function<void(TimePoint), 64> fn) { advertise_streams_ = std::move(fn); }

    /// Called when MSRP withdraws: withdraw the reservations declared above.
    void set_withdraw_streams(statusbar::sg14::inplace_function<void(TimePoint), 64> fn) { withdraw_streams_ = std::move(fn); }

    /// Called for every MSRP Listener Ready register/leave on one of our talker
    /// streams (after the host drives the talker SM + logs the gate debug). Wire this
    /// to your TalkerGate::note_listener_ready so the transmit gate tracks readiness.
    void set_on_listener_ready(statusbar::sg14::inplace_function<void(nanoavb::StreamId const&, bool), 64> fn)
    {
        on_listener_ready_ = std::move(fn);
    }

    /// Called when the Class A VID in use changes (Milan v1.2 Section 4.2.7.2.1),
    /// after the host has moved the MVRP join and ACMP stream_vlan_id to @p vlan_id;
    /// the entity re-tags its frames. The advertise hook is called again when Ready.
    void set_on_domain_vid_changed(statusbar::sg14::inplace_function<void(uint16_t, TimePoint), 64> fn)
    {
        on_domain_vid_changed_ = std::move(fn);
    }

  private:
    /// Scan configuration 0's CONTROL descriptors for the standard IDENTIFY
    /// control_type and, when found, mirror its index into the AEM handler
    /// (SET_CONTROL on it additionally notifies the IDENTIFY multicast) and the
    /// ADP advertiser (identify_control_index + the
    /// AEM_IDENTIFY_CONTROL_INDEX_VALID capability). Symbol-aware path only —
    /// no-op without a descriptor storage.
    ///
    /// Identify customization points for entities:
    ///  - INBOUND indicator (a controller identified us): replace the default
    ///    journal log via components().aem_handler.set_identify_changed(fn) —
    ///    e.g. blink a real LED. The callback also fires for local triggers.
    ///  - OUTBOUND trigger (front-panel button / GPIO): call
    ///    set_local_identify(on) from the entity's event loop; it applies the
    ///    control and multicasts the unsolicited IDENTIFY notification.
    void wire_identify_control();

    // AVB_INTERFACE / GET_AVB_INFO plumbing (see mirror_avb_info_from).
    void wire_avb_info();
    void attach_mirror(net::MessageReactor& reactor);
    void apply_interface_runtime();
    void on_mirror_update(uint16_t index, atdecc::aem::DescriptorAvbInterface const* desc, nanoavb::AvbInfo const* info);

    // Generic SM-callback wiring (no stream specifics): split by SM group.
    void wire_callbacks();
    void wire_supervisor_callbacks();
    void wire_gptp_callbacks();
    void wire_mvrp_callbacks();
    /// Class A VID changes (adopted MSRP Domain / link-up reset): MVRP, ACMP
    /// talker streams, the entity hook and re-advertising. Wired at
    /// construction: it needs no network.
    void wire_domain_callbacks();
    void wire_srp_callbacks();  ///< MSRP talker/listener SM drive + the talker-listener bridge (uses the hooks)
    void wire_engine_callbacks();

    logging::LogChannel<256> ctl_log_channel_{logging::lit("ctl")};
    logging::LogChannel<64> media_log_channel_{logging::lit("media")};

    nanoavb::gptp_sm::Context gptp_ctx_{};
    nanoavb::msrp_talker_sm::Context msrp_talker_ctx_{};
    nanoavb::msrp_listener_sm::Context msrp_listener_ctx_{};
    nanoavb::mvrp_sm::Context mvrp_ctx_{};
    nanoavb::supervisor_sm::Context supervisor_ctx_{};
    nanoavb::talker_engine_sm::Context talker_engine_ctx_{};
    nanoavb::listener_engine_sm::Context listener_engine_ctx_{};

    /// Symbol-aware path only: the caller's descriptor/command handler (not owned;
    /// it outlives the host by construction contract). Null on the legacy
    /// (parsed-EntityModel) construction path.
    nanoavb::AemEntityHandler* handler_{nullptr};

    nanoavb::NanoAvbComponents components_;
    std::unique_ptr<nanoavb::NanoAvbNetHandlers> net_handlers_;

    nanoavb::supervisor_sm::Machine supervisor_{};
    nanoavb::gptp_sm::Machine gptp_{};
    nanoavb::mvrp_sm::Machine mvrp_{};
    nanoavb::msrp_talker_sm::Machine msrp_talker_{};
    nanoavb::msrp_listener_sm::Machine msrp_listener_{};
    nanoavb::talker_engine_sm::Machine talker_engine_{};
    nanoavb::listener_engine_sm::Machine listener_engine_{};

    statusbar::sg14::inplace_function<void(TimePoint), 64> advertise_streams_{};
    statusbar::sg14::inplace_function<void(TimePoint), 64> withdraw_streams_{};
    statusbar::sg14::inplace_function<void(nanoavb::StreamId const&, bool), 64> on_listener_ready_{};
    statusbar::sg14::inplace_function<void(uint16_t, TimePoint), 64> on_domain_vid_changed_{};

    std::string interface_name_{};
    net::MessageReactor* reactor_{nullptr};  ///< set while the control plane is up
    std::optional<AvbInfoMirror::Config> mirror_config_{};
    AvbInfoMirrorPort* mirror_{nullptr};  ///< owned by the reactor once attached
    /// Mirrored state per AVB_INTERFACE index (see mirror_avb_info_from).
    struct MirroredInterface
    {
        uint16_t index{0};
        std::optional<atdecc::aem::DescriptorAvbInterface> descriptor{};
        std::optional<nanoavb::AvbInfo> info{};
    };
    std::vector<MirroredInterface> mirrored_{};
    nanoavb::AvbInfo last_avb_info_{};  ///< what the last notification carried
    /// Notify GET_AVB_INFO of interface 0 asynchronously (Milan v1.2 Table 5.22)
    /// when a listed field differs from the last one sent.
    void refresh_avb_info();
    /// The gPTP lock state changed: CLOCK_DOMAIN 0's lock unless the entity drives it, and asCapable.
    void note_media_clock_locked(bool locked);
    /// Count CLOCK_DOMAIN @p index's transition to @p locked, if one, and notify it.
    void count_clock_domain_lock(uint16_t index, bool locked);
    /// The clock identity AVB_INTERFACE 0 reports (apply_interface_runtime).
    std::optional<ieee::Eui64> own_clock_identity_{};

    // GET_COUNTERS state (fill_counters): 32-bit counters that wrap.
    uint32_t link_up_count_{0};
    uint32_t link_down_count_{0};
    uint32_t gm_changed_count_{0};
    /// One CLOCK_DOMAIN's lock and its LOCKED/UNLOCKED counts.
    struct ClockDomainLock
    {
        uint32_t locked_count{0};
        uint32_t unlocked_count{0};
        bool locked{false};
        bool entity_driven{false};  ///< note_clock_domain_locked was called: gPTP no longer drives it
    };
    std::array<ClockDomainLock, MAX_CLOCK_DOMAINS> clock_domain_locks_{};
    bool grandmaster_seen_{false};
    bool propagation_delay_override_{false};
    uint32_t propagation_delay_override_ns_{0};
    std::string listener_bindings_path_{};  ///< see enable_listener_binding_persistence
    nanoavb::AtdeccLocalPort::Config local_atdecc_socket_{};  ///< see set_local_atdecc_socket
    bool running_{false};
};

}  // namespace statusbar::avb_entity
