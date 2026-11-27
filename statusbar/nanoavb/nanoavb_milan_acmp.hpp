#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// NanoAVB Milan ACMP - the entity's Milan v1.3 talker and listener.
///
/// The Milan counterparts of NanoAvbAcmpTalker and NanoAvbAcmpListener
/// (nanoavb_acmp.hpp), around the machines in statusbar/atdecc. An entity
/// runs one pair or the other: Milan's connection management needs to know
/// what SRP registers, what ADP hears, the gPTP grandmaster and the entity's
/// lock, none of which the IEEE 1722.1 machines ask about. Each of those is
/// a question the owner answers through a setter here; nothing in this file
/// reaches into another protocol by itself.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_base.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace statusbar::nanoavb {

//
// Milan talker - Milan v1.3 5.5.4
//

/// Answers PROBE_TX, DISCONNECT_TX, GET_TX_STATE and GET_TX_CONNECTION
/// commands from the parameters of its stream outputs, and keeps nothing
/// about who asked (5.5.2.7).
///
/// Uses AcmpTalkerCallbacks: tx_response sends, on_probe reports each
/// PROBE_TX_COMMAND that names a stream output (Milan v1.3 4.3.3.1). The
/// connection and SRP callbacks never fire: a Milan talker has no connections.
class NanoAvbMilanAcmpTalker
{
  public:
    using TimePoint = sm::TimePoint;
    static constexpr size_t MAX_STREAMS = 32;

    explicit NanoAvbMilanAcmpTalker(Eui64 entity_id, AcmpTalkerCallbacks callbacks = {}, size_t max_streams = 16);

    [[nodiscard]] auto entity_id() const noexcept -> Eui64 { return entity_id_; }
    void set_entity_id(Eui64 const entity_id) noexcept { entity_id_ = entity_id; }

    void set_callbacks(AcmpTalkerCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Set just the sender without disturbing the others.
    void set_tx_response(statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> send)
    {
        callbacks_.tx_response = std::move(send);
    }

    /// Set the sender on behalf of @p owner, an address that names whoever
    /// carries the PDUs (a net handler passes itself), and take it away
    /// again with detach_sender(). A sender a later owner attached is left
    /// alone when an earlier owner lets go.
    void attach_sender(void const* const owner, statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> send)
    {
        callbacks_.tx_response = std::move(send);
        sender_owner_ = owner;
    }
    void detach_sender(void const* const owner) noexcept
    {
        if (sender_owner_ == owner) {
            callbacks_.tx_response = {};
            sender_owner_ = nullptr;
        }
    }

    /// Set just the probe callback without disturbing the others.
    void set_on_probe(statusbar::sg14::inplace_function<void(uint16_t, TimePoint), 64> on_probe)
    {
        callbacks_.on_probe = std::move(on_probe);
    }

    /// Whether a valid destination MAC address is available for a stream
    /// output (5.5.4.1 step 3): false answers a probe TALKER_DEST_MAC_FAILED.
    /// Unset, every configured address is taken as valid.
    void set_dest_mac_valid(statusbar::sg14::inplace_function<bool(uint16_t stream_index), 64> ask)
    {
        dest_mac_valid_ = std::move(ask);
    }

    /// Whether a stream output declares a Talker attribute and registers a
    /// matching Listener Asking Failed: REGISTERING_FAILED in a
    /// GET_TX_STATE_RESPONSE (Table 5.23). Unset, the flag stays clear.
    void set_registering_failed(statusbar::sg14::inplace_function<bool(uint16_t stream_index), 64> ask)
    {
        registering_failed_ = std::move(ask);
    }

    [[nodiscard]] auto max_streams() const noexcept -> size_t { return streams_.size(); }

    /// Set the parameters of stream output @p stream_index.
    [[nodiscard]] auto configure_stream(
        uint16_t stream_index, Eui64 stream_id, Eui48 dest_mac, uint16_t vlan_id = srp::DEFAULT_SR_CLASS_A_VID) -> Status;

    /// The parameters of stream output @p stream_index, nullptr when there is none.
    [[nodiscard]] auto get_stream(uint16_t const stream_index) const noexcept -> atdecc::MilanTalkerStream const*
    {
        return stream_index < streams_.size() ? &streams_[stream_index] : nullptr;
    }

    /// Process a received ACMP PDU. True when it was a talker command
    /// addressed to this entity (a response has then been sent).
    auto receive_command(AcmpCommandResponse const& cmd, TimePoint event_time) -> bool;

  private:
    [[nodiscard]] auto source_for(uint16_t stream_index) const -> atdecc::MilanTalkerSource;
    void report_probe(AcmpCommandResponse const& cmd, TimePoint event_time);

    Eui64 entity_id_;
    AcmpTalkerCallbacks callbacks_;
    void const* sender_owner_{nullptr};  ///< see attach_sender
    statusbar::sg14::inplace_function<bool(uint16_t), 64> dest_mac_valid_{};
    statusbar::sg14::inplace_function<bool(uint16_t), 64> registering_failed_{};
    statusbar::sg14::inplace_vector<atdecc::MilanTalkerStream, MAX_STREAMS> streams_{};
};

//
// Milan listener - Milan v1.3 5.5.3 and 5.6.4
//

/// One MilanSink per stream input, fed from ACMP, ADP, SRP and the clock.
///
/// Uses AcmpListenerCallbacks: tx_command sends a PROBE_TX_COMMAND,
/// tx_response answers a controller, on_connect fires when a sink settles
/// ("initiate SRP reservation and start listening for the stream packets")
/// and on_disconnect when it gives its SRP parameters up. The on_connect
/// stream is the one the talker's PROBE_TX_RESPONSE named.
class NanoAvbMilanAcmpListener
{
  public:
    using TimePoint = sm::TimePoint;
    static constexpr size_t MAX_STREAMS = 32;
    using Bindings = statusbar::sg14::inplace_vector<atdecc::MilanBinding, MAX_STREAMS>;

    explicit NanoAvbMilanAcmpListener(Eui64 entity_id, AcmpListenerCallbacks callbacks = {}, size_t max_streams = 16);

    // The sinks point at this object.
    NanoAvbMilanAcmpListener(NanoAvbMilanAcmpListener const&) = delete;
    auto operator=(NanoAvbMilanAcmpListener const&) -> NanoAvbMilanAcmpListener& = delete;
    NanoAvbMilanAcmpListener(NanoAvbMilanAcmpListener&&) = delete;
    auto operator=(NanoAvbMilanAcmpListener&&) -> NanoAvbMilanAcmpListener& = delete;
    ~NanoAvbMilanAcmpListener() = default;

    [[nodiscard]] auto entity_id() const noexcept -> Eui64 { return env_.my_id; }
    void set_entity_id(Eui64 const entity_id) noexcept { env_.my_id = entity_id; }

    void set_callbacks(AcmpListenerCallbacks callbacks) { callbacks_ = std::move(callbacks); }

    /// Set just the two senders without disturbing the others.
    void set_senders(
        statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_command,
        statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response)
    {
        callbacks_.tx_command = std::move(tx_command);
        callbacks_.tx_response = std::move(tx_response);
    }

    /// Set the two senders on behalf of @p owner and take them away again
    /// with detach_senders(), as NanoAvbMilanAcmpTalker::attach_sender does.
    void attach_senders(
        void const* const owner,
        statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_command,
        statusbar::sg14::inplace_function<bool(AcmpCommandResponse const&), 64> tx_response)
    {
        set_senders(std::move(tx_command), std::move(tx_response));
        sender_owner_ = owner;
    }
    void detach_senders(void const* const owner) noexcept
    {
        if (sender_owner_ == owner) {
            set_senders({}, {});
            sender_owner_ = nullptr;
        }
    }

    /// Set just the connection callbacks without disturbing the others.
    void set_connection_callbacks(
        statusbar::sg14::inplace_function<void(uint16_t, Eui64 const&, Eui48, TimePoint), 64> on_connect,
        statusbar::sg14::inplace_function<void(uint16_t, TimePoint), 64> on_disconnect)
    {
        callbacks_.on_connect = std::move(on_connect);
        callbacks_.on_disconnect = std::move(on_disconnect);
    }

    // What a Milan listener asks of the rest of the entity

    /// Is the entity locked by a controller other than @p controller? A bind
    /// or unbind from one that is shut out is refused CONTROLLER_NOT_AUTHORIZED
    /// (5.5.2.4, 5.5.2.5). Unset, nobody is shut out.
    void set_locked_out(statusbar::sg14::inplace_function<bool(Eui64 const& controller), 64> ask) { locked_out_ = std::move(ask); }

    /// This listener's gPTP grandmaster and domain: a talker is discovered
    /// only when its ENTITY_AVAILABLE names the same (5.6.4.5.1). Unset, an
    /// all-zero state, which no talker with a grandmaster matches.
    void set_gptp_state(statusbar::sg14::inplace_function<atdecc::MilanGptpState(), 64> ask) { gptp_state_ = std::move(ask); }

    /// What SRP registers now that matches a settled sink's stream id,
    /// destination address and VLAN (5.3.8.8). Call on_srp_changed() when the
    /// answer may have changed.
    void set_talker_registration(
        statusbar::sg14::inplace_function<atdecc::MilanTalkerRegistration(atdecc::MilanSrpParameters const&), 64> ask)
    {
        env_.talker_registration = std::move(ask);
    }

    /// Replace the source of TMR_DELAY values (0 to 1 second). The default
    /// is a generator seeded from the entity id, so two listeners that
    /// discover a talker together do not probe it together.
    void set_random_delay(statusbar::sg14::inplace_function<std::chrono::milliseconds(), 64> draw)
    {
        env_.random_delay = std::move(draw);
    }

    /// Called when the binding parameters of any sink changed or were
    /// cleared, before the controller is answered: the place to save
    /// bindings() to non-volatile memory (5.5.2.4).
    void set_on_bindings_changed(statusbar::sg14::inplace_function<void(), 64> fn) { on_bindings_changed_ = std::move(fn); }

    [[nodiscard]] auto max_streams() const noexcept -> size_t { return sinks_.size(); }

    // Bindings

    /// The binding parameters of every bound sink (5.3.8.3).
    [[nodiscard]] auto bindings() const -> Bindings;

    /// Startup with a saved binding (5.5.3.6.2) for sink
    /// binding.listener_unique_id. False when there is no such sink.
    auto restore_binding(atdecc::MilanBinding const& binding, TimePoint now) -> bool;

    /// The entity starts again: every bound sink gives up its reservation
    /// and waits for its talker to be discovered, as after a power cycle.
    void restart(TimePoint now);

    // Sink state

    /// Stream input @p stream_index, nullptr when there is none.
    [[nodiscard]] auto sink(uint16_t const stream_index) const noexcept -> atdecc::MilanSink const*
    {
        return stream_index < sinks_.size() ? &sinks_[stream_index] : nullptr;
    }

    [[nodiscard]] auto is_bound(uint16_t const stream_index) const noexcept -> bool
    {
        auto const* found = sink(stream_index);
        return found != nullptr && found->is_bound();
    }

    [[nodiscard]] auto is_settled(uint16_t const stream_index) const noexcept -> bool
    {
        auto const* found = sink(stream_index);
        return found != nullptr && found->is_settled();
    }

    // Events

    /// Process a received ACMP PDU as a controller's command. True when it
    /// was a BIND_RX, UNBIND_RX or GET_RX_STATE command addressed to this
    /// entity (5.5.3.1); every other ACMP message is ignored. @p heard is the
    /// last ENTITY_AVAILABLE of the talker the command names, when the
    /// entity's ADP discovery holds one that is still good: after a bind the
    /// sink is given it (MilanSink::on_advertisement_heard_earlier).
    auto receive_controller_command(
        AcmpCommandResponse const& cmd,
        TimePoint event_time,
        std::optional<atdecc::MilanHeardAdvertisement> const& heard = std::nullopt) -> bool;

    /// What takes a sink's answer to a controller in place of tx_response.
    using Respond = statusbar::sg14::inplace_function<void(AcmpCommandResponse const&), 64>;

    /// A BIND_STREAM or UNBIND_STREAM command (AECP MVU, Milan v1.3 5.4.4.6
    /// and 5.4.4.7), given as the BIND_RX or UNBIND_RX command it equals
    /// (Table 5.22, atdecc::milan_bind_command_from_mvu). The sink treats it
    /// as it treats the ACMP command (5.5.3.2), and at the step where it
    /// would send its ACMP response it calls @p respond with that response:
    /// the caller answers in MVU form there, before the sink goes on to
    /// probe. Nothing is sent through tx_response. @p heard as for
    /// receive_controller_command(). False, and @p respond is not called,
    /// when @p cmd is not a bind or unbind for this entity.
    auto receive_mvu_binding_command(
        AcmpCommandResponse const& cmd,
        TimePoint event_time,
        Respond const& respond,
        std::optional<atdecc::MilanHeardAdvertisement> const& heard = std::nullopt) -> bool;

    /// Process a received ACMP PDU as a talker's PROBE_TX_RESPONSE. True
    /// when it named this entity and one of its sinks.
    auto receive_talker_response(AcmpCommandResponse const& resp, TimePoint event_time) -> bool;

    /// An ADP message from any entity: each sink bound to its sender runs
    /// its discovery machine (5.6.4.1).
    void receive_adpdu(atdecc::AdpDu const& adp, TimePoint event_time);

    /// What SRP registers may have changed: the settled sinks look again.
    void on_srp_changed(TimePoint now);

    /// START_STREAMING (@p wait false) or STOP_STREAMING (true) for stream
    /// input @p stream_index: MilanSink::set_streaming_wait(). True when the
    /// sink's state changed; on_bindings_changed has then been called.
    auto set_streaming_wait(uint16_t stream_index, bool wait) -> bool;

    /// Run the timers that have fallen due.
    void tick(TimePoint now);

    /// Takes the index of a stream input.
    using TellStreamInput = statusbar::sg14::inplace_function<void(uint16_t stream_index), 64>;

    /// Call @p tell for each stream input of which something Milan v1.3
    /// Table 5.21 names is not as the call before found it: the talker it is
    /// bound to, its Probing status, its ACMP status, the registration of
    /// its Talker attribute (atdecc::MilanStreamInputNotified). That is when
    /// a GET_STREAM_INPUT_INFO_EX notification is due. However many times an
    /// input changed between two calls it is told once, and not at all when
    /// it is back as it was. A listener starts with every input unbound, so
    /// the first call tells of each binding restored before it.
    void tell_stream_input_changes(TellStreamInput const& tell);

    /// The earliest timer of any sink, nullopt with none running.
    [[nodiscard]] auto next_deadline() const noexcept -> std::optional<TimePoint>;

  private:
    void wire_env();
    /// A sink's answer to a controller: to the MVU caller while one waits
    /// for it, through tx_response otherwise.
    auto answer_controller(AcmpCommandResponse const& pdu) -> bool;
    /// A controller's command for this entity goes to its sink, or is
    /// answered for a sink that does not exist (5.5.3.1 and 5.5.3.2, step 1).
    void treat_controller_command(
        AcmpCommandResponse const& cmd, TimePoint event_time, std::optional<atdecc::MilanHeardAdvertisement> const& heard);
    [[nodiscard]] auto draw_delay() noexcept -> std::chrono::milliseconds;
    [[nodiscard]] auto is_locked_out(Eui64 const& controller) const -> bool;
    [[nodiscard]] auto mutable_sink(uint16_t stream_index) noexcept -> atdecc::MilanSink*;

    AcmpListenerCallbacks callbacks_;
    void const* sender_owner_{nullptr};    ///< see attach_senders
    Respond const* mvu_respond_{nullptr};  ///< see receive_mvu_binding_command
    atdecc::MilanListenerEnv env_{};
    statusbar::sg14::inplace_function<bool(Eui64 const&), 64> locked_out_{};
    statusbar::sg14::inplace_function<atdecc::MilanGptpState(), 64> gptp_state_{};
    statusbar::sg14::inplace_function<void(), 64> on_bindings_changed_{};
    statusbar::sg14::inplace_vector<atdecc::MilanSink, MAX_STREAMS> sinks_{};
    /// Each sink as tell_stream_input_changes() last found it.
    statusbar::sg14::inplace_vector<atdecc::MilanStreamInputNotified, MAX_STREAMS> told_{};
    uint32_t random_state_{0};
};

}  // namespace statusbar::nanoavb
