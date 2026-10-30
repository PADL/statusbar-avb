#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// SrpPort: MSRP and MVRP on one interface with its Class A Domain, for an end station that
/// declares Talker Advertise and Listener attributes without an ATDECC entity (IEEE 802.1Q-2022
/// Clause 35). Reactor thread. See docs/NANOAVB_ARCHITECTURE.md, "SrpPort".

#include "statusbar/ieee/ieee.hpp"
#include "statusbar/logging/logging.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/nanoavb/nanoavb_srp_net.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/sg14/inplace_vector.h"
#include "statusbar/srp/srp_msrp.hpp"
#include "statusbar/srp/srp_mvrp_participant.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace statusbar::nanoavb {

/// SrpPort's MSRP handler: 32 streams of its own, the participant sized for discovery.
using SrpPortMsrpHandler = MsrpHandler<32, statusbar::srp::msrp::DiscoveryMsrpLimits>;

/// What an SrpPort runs on: the interface, the configured Class A Domain and the MRP tables.
struct SrpPortConfig
{
    /// The interface whose MSRP and MVRP sockets add_to_reactor() opens.
    std::string interface_name{};
    /// The configured Class A Domain (IEEE 802.1Q-2022 Clause 35.2.2.9); a peer's VID is adopted over it.
    DomainInfo class_a{};
    /// The MSRP tables: 256 Talker Advertises and 256 Talker Failed, less the room kept for the
    /// port's own 16 + 16 streams; a foreign attribute beyond that is not registered.
    statusbar::srp::msrp::MsrpConfig msrp{entity_msrp_config<statusbar::srp::msrp::DiscoveryMsrpLimits>(16, 16)};
    statusbar::srp::mvrp::MvrpConfig mvrp{};
};

/// One talker stream an SrpPort advertises; its VID and priority are the Class A Domain's.
struct SrpTalker
{
    StreamId stream_id{};
    Eui48 destination{};              ///< DataFrameParameters.DestinationAddress (Clause 35.2.2.8.3)
    uint16_t max_frame_size{0};       ///< TSpec.MaxFrameSize, octets (Clause 35.2.2.8.4)
    uint16_t max_interval_frames{1};  ///< TSpec.MaxIntervalFrames
    uint32_t accumulated_latency{0};  ///< ns (Clause 35.2.2.8.6)
};

/// The registrations an SrpPort reports, synchronously from tick() and the receive calls; each
/// callback is optional. Registrations are peers' only, never the port's own declarations.
struct SrpPortObserver
{
    /// The Class A Domain in use changed (a peer's VID adopted, or declare_domain()), or a peer's
    /// Class A Domain registered or left: SRP is in force while @p peer_registered (Clause 35.2.1.4).
    sg14::inplace_function<void(DomainInfo const&, bool peer_registered), 64> on_domain;
    /// A peer's Talker Advertise registered (Clause 35.2.4.3).
    sg14::inplace_function<void(statusbar::srp::msrp::TalkerAdvertiseFirstValue const&), 64> on_talker_advertise;
    /// A peer's Talker Failed registered; its FailureInformation says why (Clause 35.2.2.8.7).
    sg14::inplace_function<void(statusbar::srp::msrp::TalkerFailedFirstValue const&), 64> on_talker_failed;
    /// A peer's Talker left: neither its Advertise nor a Talker Failed is registered any more.
    sg14::inplace_function<void(StreamId const&), 64> on_talker_leave;
    /// The registered Listener declaration for one of our talker streams changed (Ignore: none);
    /// Ready and Ready Failed permit transmission (Clause 35.2.4.4).
    sg14::inplace_function<void(StreamId const&, statusbar::srp::msrp::ListenerDeclaration), 64> on_listener;
    /// An attached listener stream's declaration followed its talker's registration.
    sg14::inplace_function<void(StreamId const&, ListenerReservationState), 64> on_listener_state;
};

/// MSRP and MVRP on one interface, constructible without NanoAvbComponents. Not movable: the
/// handlers call back into it. Once the reactor holding its sockets is gone it must not be ticked.
class SrpPort
{
  public:
    using SendFn = sg14::inplace_function<bool(std::span<uint8_t const>), 64>;

    explicit SrpPort(SrpPortConfig config);
    SrpPort(SrpPort const&) = delete;
    auto operator=(SrpPort const&) -> SrpPort& = delete;
    SrpPort(SrpPort&&) = delete;
    auto operator=(SrpPort&&) -> SrpPort& = delete;
    ~SrpPort() = default;

    /// Report registrations to @p observer, replacing the previous one.
    void set_observer(SrpPortObserver observer) noexcept { observer_ = std::move(observer); }
    /// Debug-level MRP diagnostics on @p log (reactor thread).
    void set_logger(logging::Logger const log) noexcept { msrp_.set_logger(log); }

    /// Open the MSRP and MVRP sockets on the interface and add them to @p reactor, which this port
    /// must outlive. Errors: NanoAvbError::InvalidConfiguration when either socket does not open or
    /// the sockets were already added.
    [[nodiscard]] auto add_to_reactor(net::MessageReactor& reactor) -> Status;
    /// Send MSRPDUs through @p msrp and MVRPDUs through @p mvrp instead (tests, or other I/O).
    void set_senders(SendFn msrp, SendFn mvrp) noexcept;
    /// Feed a received MSRPDU / MVRPDU (payload only) at @p now; add_to_reactor's sockets do this.
    void receive_msrp(std::span<uint8_t const> pdu, TimePoint now);
    void receive_mvrp(std::span<uint8_t const> pdu, TimePoint now) { mvrp_.receive_packet(pdu, now); }
    /// Run the MSRP / MVRP timers at @p now; add_to_reactor's sockets do this every 10 ms.
    void tick_msrp(TimePoint now);
    void tick_mvrp(TimePoint now) { mvrp_.tick(now); }
    /// tick_msrp() and tick_mvrp().
    void tick(TimePoint now);

    /// Return to the configured Class A Domain, join its VID with MVRP and declare it (Clause
    /// 35.2.2.9; Milan v1.2 Section 4.2.7.2.1). Call before any stream, and again on each link up.
    [[nodiscard]] auto declare_domain(TimePoint now) -> Status;
    /// Declare (or update) @p talker's Talker Advertise on the Domain's VID and priority, rank 1.
    /// Errors: NanoAvbError::SrpRegistrationFailed, tsn::TsnError::attribute_table_full.
    [[nodiscard]] auto declare_talker(SrpTalker const& talker, TimePoint now) -> Status;
    /// Withdraw @p stream_id's Talker Advertise. Errors: NanoAvbError::InvalidStreamIndex if unknown.
    [[nodiscard]] auto withdraw_talker(StreamId const& stream_id, TimePoint now) -> Status;
    /// Listen to @p stream_id: Ready while its Advertise is registered, Asking Failed while its
    /// Talker Failed is, Waiting (nothing declared) otherwise; then following it. Returns the state.
    [[nodiscard]] auto attach_listener(StreamId const& stream_id, TimePoint now) -> StatusValue<ListenerReservationState>;
    /// Withdraw the Listener declaration for @p stream_id (idempotent).
    [[nodiscard]] auto withdraw_listener(StreamId const& stream_id, TimePoint now) -> Status;

    /// The Class A Domain in use: the configured one, or the VID adopted from the peer.
    [[nodiscard]] auto domain() const noexcept -> DomainInfo const& { return msrp_.domain(); }
    /// Whether a peer's Class A Domain at our SRclassPriority is registered; otherwise the port is
    /// an SRP domain boundary (IEEE 802.1Q-2022 Clause 35.2.1.4).
    [[nodiscard]] auto peer_domain_registered() const noexcept -> bool;
    /// A peer's registered Talker Advertise / Failed sent to @p destination on @p vid, or nullptr.
    [[nodiscard]] auto find_registered_talker_advertise(Eui48 const& destination, uint16_t vid) const noexcept
        -> statusbar::srp::msrp::TalkerAdvertiseFirstValue const*;
    [[nodiscard]] auto find_registered_talker_failed(Eui48 const& destination, uint16_t vid) const noexcept
        -> statusbar::srp::msrp::TalkerFailedFirstValue const*;
    /// The registered Listener declaration for one of our talker streams, or Ignore.
    [[nodiscard]] auto registered_listener(StreamId const& stream_id) const noexcept -> statusbar::srp::msrp::ListenerDeclaration
    {
        return msrp_.registered_listener(stream_id);
    }

    /// The handlers, for what the port does not wrap (another VLAN, the MSRP workarounds). The
    /// Domain's VID is the port's: it is withdrawn when the Domain moves, whoever joined it.
    /// Driving msrp() directly defers the Domain and leave reports to the next receive or tick;
    /// declare_domain() called from an observer reports at once.
    [[nodiscard]] auto msrp() noexcept -> SrpPortMsrpHandler& { return msrp_; }
    [[nodiscard]] auto mvrp() noexcept -> MvrpHandler& { return mvrp_; }

  private:
    /// The MSRP socket's handler: received PDUs and ticks go through the port.
    struct MsrpIo
    {
        SrpPort& port;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
        void receive_packet(std::span<uint8_t const> pdu, TimePoint now) { port.receive_msrp(pdu, now); }
        void tick(TimePoint now) { port.tick_msrp(now); }
    };

    /// Subscribe to the MSRP participant's registrations and the Domain's VID changes.
    void wire();
    /// Send through the opened @p msrp and @p mvrp sockets and give them to @p reactor.
    void hand_over(
        net::MessageReactor& reactor, std::unique_ptr<MsrpNetHandlerT<MsrpIo>> msrp, std::unique_ptr<MvrpNetHandler> mvrp);
    /// The Class A VID moved: rejoin with MVRP and re-advertise the talkers on it.
    void on_vid_changed(uint16_t old_vid, uint16_t new_vid, TimePoint now);
    /// A peer declared a Domain (@p fv as on the wire): keep its Class A SRclassPriority.
    void on_peer_domain_declared(statusbar::srp::msrp::DomainFirstValue const& fv) noexcept;
    /// A peer's Class A Domain registered (@p registered) or left.
    void on_peer_domain(statusbar::srp::msrp::DomainFirstValue const& fv, bool registered) noexcept;
    /// A peer's Talker for @p stream_id left: decided when the PDU or tick is done.
    void on_peer_talker_leave(StreamId const& stream_id);
    /// Report @p stream_id's talker gone if neither of its attributes is registered.
    void report_leave(StreamId const& stream_id);
    /// The talker leaves, then the Domain, as the received PDU or the tick left them.
    void flush();
    /// A peer's Listener declaration for @p stream_id registered, changed or left.
    void on_peer_listener(StreamId const& stream_id);
    /// Report the Domain once, with its final state, if it changed since the last report.
    void flush_domain();

    SrpPortConfig config_;
    MvrpHandler mvrp_;
    SrpPortMsrpHandler msrp_;
    MsrpIo msrp_io_{*this};
    SrpPortObserver observer_{};
    SendFn send_msrp_{};
    SendFn send_mvrp_{};
    std::optional<uint8_t> peer_priority_{};  ///< the peer's Class A SRclassPriority, as declared
    bool peer_class_a_{false};                ///< a peer's Class A Domain is registered
    bool domain_changed_{false};              ///< the Domain moved since the last report
    bool reported_peer_{false};               ///< peer_domain_registered() as last reported
    bool added_{false};                       ///< add_to_reactor() handed the sockets over
    /// Talkers that left during this PDU or tick, one entry per StreamID.
    sg14::inplace_vector<
        StreamId,
        statusbar::srp::msrp::DiscoveryMsrpLimits::max_talker_advertise +
            statusbar::srp::msrp::DiscoveryMsrpLimits::max_talker_failed>
        pending_leaves_{};
};

}  // namespace statusbar::nanoavb
