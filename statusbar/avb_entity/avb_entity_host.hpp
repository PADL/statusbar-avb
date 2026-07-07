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
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::avb_entity {

using statusbar::Status;

/// The default ADP advertiser identity/timing shared by every avb_entity: valid_time 31
/// (62 s) + a 5 s reannounce interval. All four entities used byte-identical local copies;
/// this is the single source of truth.
[[nodiscard]] inline auto default_adp_advertiser_config() noexcept -> nanoavb::AdpAdvertiserConfig
{
    nanoavb::AdpAdvertiserConfig cfg{};
    cfg.valid_time = 31;                                        // 62 seconds
    cfg.reannounce_interval = std::chrono::milliseconds{5000};  // 5 seconds
    return cfg;
}

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
    AvbEntityHost(
        nanoavb::EntityModel model,
        nanoavb::AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams);

    /// Symbol-aware construction. The host takes ownership of a caller-built
    /// @p handler (typically a DescriptorStorageHandler subclass that serves the
    /// entity's .aem blob and patches dynamic fields) and serves descriptors +
    /// resolves their well-known **symbols** through it instead of a parsed model.
    /// Enables symbol_of() / descriptor_for_symbol() / get_descriptor(). The handler
    /// is owned by the host so it outlives the components that reference it.
    /// @param handler  the entity's descriptor/command handler (must be non-null).
    AvbEntityHost(
        std::unique_ptr<nanoavb::AemEntityHandler> handler,
        nanoavb::AdpAdvertiserConfig adp_config,
        size_t talker_max_streams,
        size_t talker_max_listeners,
        size_t listener_max_streams);

    AvbEntityHost(AvbEntityHost const&) = delete;
    auto operator=(AvbEntityHost const&) -> AvbEntityHost& = delete;
    AvbEntityHost(AvbEntityHost&&) = delete;
    auto operator=(AvbEntityHost&&) -> AvbEntityHost& = delete;
    ~AvbEntityHost() = default;

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

    // --- Event handlers (forward the entity's events into the SM stack) --------
    void on_link_up(TimePoint time);
    void on_link_down(TimePoint time);
    void on_gptp_announce(TimePoint time, bool has_grandmaster);
    void on_timeout(TimePoint time);

    // --- State queries ---------------------------------------------------------
    [[nodiscard]] auto is_ready() const noexcept -> bool;
    [[nodiscard]] auto state_string() const -> std::string_view;

    // --- Control-plane access for the entity's stream wiring -------------------
    [[nodiscard]] auto components() noexcept -> nanoavb::NanoAvbComponents& { return components_; }
    [[nodiscard]] auto components() const noexcept -> nanoavb::NanoAvbComponents const& { return components_; }
    [[nodiscard]] auto net_handlers() noexcept -> nanoavb::NanoAvbNetHandlers* { return net_handlers_.get(); }

    // --- Symbol <-> descriptor lookups (symbol-aware construction only) ---------
    // These let entity command-handling code reference descriptors by the designer's
    // well-known symbol instead of the blob's (volatile) descriptor index. All return
    // empty/failure on the legacy parsed-model path (no backing storage).

    /// The backing descriptor blob, or nullptr on the legacy path.
    [[nodiscard]] auto descriptor_storage() const noexcept -> atdecc::aem::DescriptorStorage const*
    {
        return handler_ ? handler_->descriptor_storage() : nullptr;
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

  private:
    /// Scan configuration 0's CONTROL descriptors for the standard IDENTIFY
    /// control_type and, when found, mirror its index into the AEM handler
    /// (SET_CONTROL on it additionally notifies the IDENTIFY multicast) and the
    /// ADP advertiser (identify_control_index + the
    /// AEM_IDENTIFY_CONTROL_INDEX_VALID capability). Symbol-aware path only —
    /// no-op without a descriptor storage.
    void wire_identify_control();

    // Generic SM-callback wiring (no stream specifics): split by SM group.
    void wire_callbacks();
    void wire_supervisor_callbacks();
    void wire_gptp_callbacks();
    void wire_mvrp_callbacks();
    void wire_srp_callbacks();  ///< MSRP talker/listener SM drive + the talker-listener bridge (uses the hooks)
    void wire_engine_callbacks();

    nanoavb::gptp_sm::Context gptp_ctx_{};
    nanoavb::msrp_talker_sm::Context msrp_talker_ctx_{};
    nanoavb::msrp_listener_sm::Context msrp_listener_ctx_{};
    nanoavb::mvrp_sm::Context mvrp_ctx_{};
    nanoavb::supervisor_sm::Context supervisor_ctx_{};
    nanoavb::talker_engine_sm::Context talker_engine_ctx_{};
    nanoavb::listener_engine_sm::Context listener_engine_ctx_{};

    /// Symbol-aware path only: the owned descriptor/command handler. Declared BEFORE
    /// components_ so it is constructed first and outlives the components that
    /// reference it. Null on the legacy (parsed-EntityModel) construction path.
    std::unique_ptr<nanoavb::AemEntityHandler> handler_{};

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

    std::string interface_name_{};
    bool running_{false};
};

}  // namespace statusbar::avb_entity
