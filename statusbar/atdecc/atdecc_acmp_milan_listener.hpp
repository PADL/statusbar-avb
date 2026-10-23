#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The Milan listener's connection management (Milan v1.2 Section 5.5.3) and per-sink talker
/// discovery (Section 5.6.4). No I/O and no clock: each event carries its time; effects go out
/// through the set_* hooks, called after the sink's state is updated. See docs/MILAN_LISTENER.md.

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string_view>

namespace statusbar::atdecc {

/// Milan v1.2 Section 5.5.2.2 renames CONNECT_RX and DISCONNECT_RX (same values).
constexpr uint8_t ACMP_MESSAGE_TYPE_BIND_RX_COMMAND = ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND;
constexpr uint8_t ACMP_MESSAGE_TYPE_BIND_RX_RESPONSE = ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE;
constexpr uint8_t ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND = ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND;
constexpr uint8_t ACMP_MESSAGE_TYPE_UNBIND_RX_RESPONSE = ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE;

/// The states of a sink (Table 5.28).
enum class MilanSinkState : uint8_t
{
    unbound,
    prb_w_avail,
    prb_w_delay,
    prb_w_resp,
    prb_w_resp2,
    prb_w_retry,
    settled_no_rsv,
    settled_rsv_ok,
};

[[nodiscard]] auto milan_sink_state_name(MilanSinkState state) noexcept -> std::string_view;

/// A bound sink's binding parameters (Section 5.3.8.3), kept in non-volatile
/// memory: the talker and its source, the controller that bound it, and the
/// started/stopped state (Section 5.3.8.7; STREAMING_WAIT = stopped).
struct MilanSinkBinding
{
    ieee::Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
    ieee::Eui64 controller_entity_id{};
    bool started{true};

    auto operator==(MilanSinkBinding const&) const -> bool = default;
};

/// A settled sink's SRP parameters (Section 5.3.8.9): exactly as the last
/// PROBE_TX_RESPONSE had them; all zero unless settled.
struct MilanSinkStream
{
    ieee::Eui64 stream_id{};
    ieee::Eui48 dest_mac{};
    uint16_t vlan_id{0};

    auto operator==(MilanSinkStream const&) const -> bool = default;
};

/// What a settled sink registers for its stream (Section 5.3.8.8): a matching
/// Talker Advertise, a matching Talker Failed, or neither.
enum class MilanTalkerRegistration : uint8_t
{
    none,
    advertise,
    failed,
};

/// One STREAM_INPUT's connection state.
struct MilanSink
{
    MilanSinkState state{MilanSinkState::unbound};
    /// Meaningful while bound (any state but unbound).
    MilanSinkBinding binding{};
    /// Settled only (zero otherwise).
    MilanSinkStream stream{};
    MilanTalkerRegistration registration{MilanTalkerRegistration::none};
    /// Probing status (Table 5.5, aem::probing_status) and ACMP status
    /// (Section 5.3.8.6): GET_STREAM_INFO's pbsta and acmpsta.
    uint8_t probing_status{0};
    uint8_t acmp_status{0};

    /// The last PROBE_TX_COMMAND sent, resent unchanged on TMR_NO_RESP
    /// (Section 5.5.3.5.16: the same sequence_id).
    AcmpCommandResponse probe{};
    /// The running TMR_DELAY / TMR_NO_RESP / TMR_RETRY / TMR_NO_TK: at most
    /// one per state.
    bool timer_running{false};
    sm::TimePoint deadline{};

    /// The ADP discovery state machine (Section 5.6.4), running while bound:
    /// TK_DISCOVERED, and what it saved from the talker's ENTITY_AVAILABLE.
    bool discovered{false};
    uint16_t interface_index{0};
    uint32_t available_index{0};
    sm::TimePoint adp_deadline{};  ///< TMR_NO_ADP: valid_time x 2 s

    [[nodiscard]] auto bound() const noexcept -> bool { return state != MilanSinkState::unbound; }
    [[nodiscard]] auto settled() const noexcept -> bool
    {
        return state == MilanSinkState::settled_no_rsv || state == MilanSinkState::settled_rsv_ok;
    }
    /// Registering a matching Talker Advertise or Talker Failed (settled only).
    [[nodiscard]] auto registering() const noexcept -> bool { return registration != MilanTalkerRegistration::none; }
};

/// The sinks of one Milan listener entity, each with its §5.5.3 and §5.6.4
/// state machines. Reactor thread only.
class MilanListener
{
  public:
    using TimePoint = sm::TimePoint;
    using Duration = std::chrono::nanoseconds;

    /// Table 5.29.
    static constexpr Duration TMR_NO_RESP = std::chrono::milliseconds{200};
    static constexpr Duration TMR_RETRY = std::chrono::seconds{4};
    static constexpr Duration TMR_NO_TK = std::chrono::seconds{10};
    /// TMR_DELAY is "between 0 and 1 second"; like GenAVB, never below
    /// 200 ms (time for a controller's asynchronous notifications).
    static constexpr Duration TMR_DELAY_MIN = std::chrono::milliseconds{200};
    static constexpr Duration TMR_DELAY_MAX = std::chrono::seconds{1};
    /// One sink per STREAM_INPUT, as many as the ACMP listener holds.
    static constexpr size_t MAX_SINKS = 32;

    /// Sends one ACMPDU (a PROBE_TX_COMMAND or a response; all go to the ACMP
    /// multicast address).
    using SendFn = sg14::inplace_function<bool(AcmpCommandResponse const&), 64>;
    /// The sink settled: start the SRP reservation and listening for the
    /// stream (Section 5.5.3.5.18 step 4).
    using SettledFn = sg14::inplace_function<void(uint16_t sink, MilanSinkStream const& stream), 64>;
    /// The sink left the settled states: "clear the SRP parameters and stop
    /// SRP" (and stop listening). The sink's stream is already zero; the one
    /// it settled on is passed.
    using UnsettledFn = sg14::inplace_function<void(uint16_t sink, MilanSinkStream const& was), 64>;
    /// The sink's binding parameters changed (bound, rebound, unbound, a new
    /// controller or started state): save them.
    using SinkFn = sg14::inplace_function<void(uint16_t sink), 64>;
    /// Whether @p controller may change bindings: false while the entity is
    /// locked by another controller (CONTROLLER_NOT_AUTHORIZED).
    using AuthorizedFn = sg14::inplace_function<bool(ieee::Eui64 const& controller), 32>;
    /// The next TMR_DELAY (default: uniform in [TMR_DELAY_MIN, TMR_DELAY_MAX]).
    using DelayFn = sg14::inplace_function<Duration(), 32>;

    explicit MilanListener(ieee::Eui64 entity_id = {}, size_t sinks = 0);

    void set_entity_id(ieee::Eui64 const entity_id) noexcept
    {
        entity_id_ = entity_id;
        rng_.seed(rng_seed(entity_id));
    }
    [[nodiscard]] auto entity_id() const noexcept -> ieee::Eui64 { return entity_id_; }

    void set_send(SendFn fn) { send_ = std::move(fn); }
    void set_on_settled(SettledFn fn) { on_settled_ = std::move(fn); }
    void set_on_unsettled(UnsettledFn fn) { on_unsettled_ = std::move(fn); }
    void set_on_binding_changed(SinkFn fn) { on_binding_changed_ = std::move(fn); }
    /// Something GET_STREAM_INFO reports of the sink changed (bound, started,
    /// probing or ACMP status, the stream, the registration): notify.
    void set_on_status_changed(SinkFn fn) { on_status_changed_ = std::move(fn); }
    void set_authorized(AuthorizedFn fn) { authorized_ = std::move(fn); }
    void set_delay(DelayFn fn) { delay_ = std::move(fn); }

    [[nodiscard]] auto sink_count() const noexcept -> size_t { return sinks_.size(); }
    /// The sink with listener_unique_id @p sink, or nullptr.
    [[nodiscard]] auto sink(uint16_t const sink) const noexcept -> MilanSink const*
    {
        return sink < sinks_.size() ? &sinks_[sink] : nullptr;
    }

    /// Section 5.5.3.5.2, startup with a saved binding: start discovering the
    /// talker and wait for it in PRB_W_AVAIL. False for an unknown or already
    /// bound sink.
    auto restore(uint16_t index, MilanSinkBinding const& binding) -> bool;

    /// A BIND_RX / UNBIND_RX / GET_RX_STATE command or PROBE_TX_RESPONSE addressed to this listener
    /// (Milan v1.2 Section 5.5.3.1; an unknown sink is answered LISTENER_UNKNOWN_ID, Table 5.27).
    /// False for any other ACMPDU.
    auto receive(AcmpCommandResponse const& pdu, TimePoint now) -> bool;

    /// Section 5.6.4.1: an ENTITY_AVAILABLE / ENTITY_DEPARTING, for every
    /// bound sink whose talker sent it. @p grandmaster and @p domain are this
    /// entity's current gPTP grandmaster and domain (Section 5.6.4.5.1).
    void receive_adp(AdpDu const& adp, tsn::ClockIdentity const& grandmaster, uint8_t domain, TimePoint now);

    /// The registration of settled sink @p index's stream changed (the caller matches stream ID,
    /// destination address and VLAN ID; Milan v1.2 Section 5.5.3.3). Raises EVT_TK_REGISTERED /
    /// EVT_TK_UNREGISTERED on a change of registered state; ignored unless the sink is settled.
    void set_registration(uint16_t index, MilanTalkerRegistration registration, TimePoint now);

    /// START_STREAMING / STOP_STREAMING (Sections 5.4.2.19-20): a bound sink
    /// becomes started / stopped. True when its state changed (no effect on
    /// an unbound sink, or one already so).
    auto set_started(uint16_t index, bool started) -> bool;

    /// Run the timers that expired by @p now (TMR_DELAY, TMR_NO_RESP,
    /// TMR_RETRY, TMR_NO_TK, TMR_NO_ADP).
    void tick(TimePoint now);

  private:
    /// TMR_DELAY's random sequence differs per entity.
    [[nodiscard]] static auto rng_seed(ieee::Eui64 const& entity_id) noexcept -> std::minstd_rand::result_type
    {
        auto const id = entity_id.to_uint64();
        return static_cast<std::minstd_rand::result_type>(id ^ (id >> 32U)) | 1U;
    }

    struct Status
    {
        bool bound{false};
        ieee::Eui64 talker_entity_id{};
        uint16_t talker_unique_id{0};
        bool started{true};
        uint8_t probing_status{0};
        uint8_t acmp_status{0};
        MilanSinkStream stream{};
        MilanTalkerRegistration registration{MilanTalkerRegistration::none};
        auto operator==(Status const&) const -> bool = default;
    };

    void bind(uint16_t index, AcmpCommandResponse const& cmd, TimePoint now);
    void unbind(uint16_t index, AcmpCommandResponse const& cmd);
    void get_rx_state(uint16_t index, AcmpCommandResponse const& cmd);
    void probe_response(uint16_t index, AcmpCommandResponse const& resp, TimePoint now);
    void send_probe(uint16_t index, TimePoint now);
    void respond(AcmpCommandResponse const& cmd, uint8_t status);
    /// Leave the settled states: clear the SRP parameters, stop SRP.
    void unsettle(uint16_t index);
    /// TMR_RETRY, TMR_NO_TK, EVT_TK_UNREGISTERED: wait for the talker, or
    /// probe again after TMR_DELAY when it is discovered.
    void reprobe(uint16_t index, TimePoint now);
    void start_delay(MilanSink& s, TimePoint now);
    void start_timer(MilanSink& s, TimePoint now, Duration duration) noexcept;
    void discovered(uint16_t index, TimePoint now);
    void departed(uint16_t index);
    void notify(uint16_t index);
    [[nodiscard]] auto authorized(ieee::Eui64 const& controller) const -> bool
    {
        return !authorized_ || authorized_(controller);
    }

    ieee::Eui64 entity_id_{};
    sg14::inplace_vector<MilanSink, MAX_SINKS> sinks_{};
    sg14::inplace_vector<Status, MAX_SINKS> notified_{};
    uint16_t sequence_id_{0};
    std::minstd_rand rng_;
    SendFn send_{};
    SettledFn on_settled_{};
    UnsettledFn on_unsettled_{};
    SinkFn on_binding_changed_{};
    SinkFn on_status_changed_{};
    AuthorizedFn authorized_{};
    DelayFn delay_{};
};

}  // namespace statusbar::atdecc
