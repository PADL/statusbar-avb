#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// PTP management over ptp4l's UNIX-domain socket, pmc-style (IEEE 1588-2019 Clause 15): the
/// grandmaster followed, one port's peer delay and grandmaster settings, event-driven through
/// linuxptp's SUBSCRIBE_EVENTS_NP. The domain must be ptp4l's. Linux only (create() fails elsewhere).

#include "statusbar/buffer/file_descriptor.hpp"
#include "statusbar/gptp/gptp_base.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/net/net_address.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient_management_format.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/status/status.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace statusbar::ptpclient {

/// IEEE 1588-2019 Table 59 managementId values used here.
namespace management_id {
inline constexpr uint16_t default_data_set = 0x2000;
inline constexpr uint16_t current_data_set = 0x2001;
inline constexpr uint16_t parent_data_set = 0x2002;
inline constexpr uint16_t time_properties_data_set = 0x2003;
inline constexpr uint16_t port_data_set = 0x2004;
/// linuxptp's TIME_STATUS_NP (implementation-specific range): the offset from the grandmaster.
inline constexpr uint16_t time_status_np = 0xC000;
/// linuxptp's GRANDMASTER_SETTINGS_NP: the quality and time properties ptp4l announces as grandmaster.
inline constexpr uint16_t grandmaster_settings_np = 0xC001;
/// linuxptp's SUBSCRIBE_EVENTS_NP: push notifications to the subscribing socket.
inline constexpr uint16_t subscribe_events_np = 0xC003;
/// linuxptp's PORT_PROPERTIES_NP: a port's interface name.
inline constexpr uint16_t port_properties_np = 0xC004;
}  // namespace management_id

/// actionField (Table 57).
enum class ManagementAction : uint8_t
{
    get = 0,
    set = 1,
    response = 2,
    command = 3,
    acknowledge = 4,
};

/// PARENT_DATA_SET (15.5.3.3.1).
struct ParentDataSet
{
    gptp::SourcePortIdentity parent_port_identity{};
    bool parent_stats{false};
    uint16_t observed_parent_offset_scaled_log_variance{0};
    int32_t observed_parent_clock_phase_change_rate{0};
    uint8_t grandmaster_priority1{0};
    uint8_t grandmaster_clock_class{0};
    uint8_t grandmaster_clock_accuracy{0};
    uint16_t grandmaster_offset_scaled_log_variance{0};
    uint8_t grandmaster_priority2{0};
    ieee::Eui64 grandmaster_identity{};

    static constexpr size_t SIZE = ParentDataSetPayload::LENGTH;
};

/// CURRENT_DATA_SET (15.5.3.2.1). TimeIntervals are ns scaled by 2^16.
struct CurrentDataSet
{
    uint16_t steps_removed{0};
    int64_t offset_from_master_scaled{0};
    int64_t mean_path_delay_scaled{0};

    static constexpr size_t SIZE = CurrentDataSetPayload::LENGTH;
};

/// TIME_PROPERTIES_DATA_SET (15.5.3.4.1).
struct TimePropertiesDataSet
{
    int16_t current_utc_offset{0};
    uint8_t flags{0};  ///< leap61, leap59, utcOffsetValid, ptpTimescale, timeTraceable, frequencyTraceable (bits 0..5)
    uint8_t time_source{0};

    static constexpr size_t SIZE = TimePropertiesDataSetPayload::LENGTH;
};

/// portState (IEEE 1588-2019 Table 20).
enum class PortState : uint8_t
{
    initializing = 1,
    faulty = 2,
    disabled = 3,
    listening = 4,
    pre_master = 5,
    master = 6,
    passive = 7,
    uncalibrated = 8,
    slave = 9,
};

/// delayMechanism (IEEE 1588-2019 Table 21).
enum class DelayMechanism : uint8_t
{
    e2e = 0x01,
    p2p = 0x02,
    common_p2p = 0x03,
    special = 0x04,
    no_mechanism = 0xFE,
};

/// PORT_DATA_SET (15.5.3.6.1). peerMeanPathDelay is ns scaled by 2^16.
struct PortDataSet
{
    gptp::SourcePortIdentity port_identity{};
    PortState port_state{PortState::initializing};
    int8_t log_min_delay_req_interval{0};
    int64_t peer_mean_path_delay_scaled{0};
    int8_t log_announce_interval{0};
    uint8_t announce_receipt_timeout{0};
    int8_t log_sync_interval{0};
    DelayMechanism delay_mechanism{DelayMechanism::e2e};
    int8_t log_min_pdelay_req_interval{0};
    uint8_t version_number{0};

    static constexpr size_t SIZE = PortDataSetPayload::LENGTH;
};

/// linuxptp's PORT_PROPERTIES_NP: portIdentity, portState, timestamping, then the
/// interface's name as a PTPText (truncated to `interface` here).
struct PortPropertiesNp
{
    gptp::SourcePortIdentity port_identity{};
    PortState port_state{PortState::initializing};
    uint8_t timestamping{0};
    std::array<char, 32> interface{};
    uint8_t interface_length{0};

    [[nodiscard]] auto interface_name() const noexcept -> std::string_view { return {interface.data(), interface_length}; }

    static constexpr size_t SIZE = PortPropertiesNpPayload::LENGTH;  ///< up to the PTPText's lengthField
};

/// linuxptp's TIME_STATUS_NP, as ptp4l sends it on each clock update (lastGmPhaseChange skipped).
struct TimeStatusNp
{
    int64_t master_offset_ns{0};               ///< the local clock less the grandmaster's, ns
    int64_t ingress_time_ns{0};                ///< the last Sync's ingress, local clock, ns
    int32_t cumulative_scaled_rate_offset{0};  ///< (rate ratio - 1) * 2^41 (802.1AS)
    int32_t scaled_last_gm_phase_change{0};    ///< 802.1AS scaledLastGmFreqChange
    uint16_t gm_time_base_indicator{0};        ///< 802.1AS gmTimeBaseIndicator
    bool gm_present{false};                    ///< a grandmaster other than the local clock
    ieee::Eui64 gm_identity{};                 ///< its clockIdentity

    static constexpr size_t SIZE = TimeStatusNpPayload::LENGTH;
};

/// timeFlags of GRANDMASTER_SETTINGS_NP (the flags of TIME_PROPERTIES_DATA_SET).
namespace time_flag {
inline constexpr uint8_t leap_61 = 0x01;
inline constexpr uint8_t leap_59 = 0x02;
inline constexpr uint8_t utc_offset_valid = 0x04;
inline constexpr uint8_t ptp_timescale = 0x08;
inline constexpr uint8_t time_traceable = 0x10;
inline constexpr uint8_t frequency_traceable = 0x20;
}  // namespace time_flag

/// linuxptp's GRANDMASTER_SETTINGS_NP: clockQuality (IEEE 1588-2019 7.6.2.5), currentUtcOffset,
/// timeFlags and timeSource (Table 6).
struct GrandmasterSettingsNp
{
    uint8_t clock_class{248};
    uint8_t clock_accuracy{0xFE};
    uint16_t offset_scaled_log_variance{0xFFFF};
    int16_t current_utc_offset{37};
    uint8_t time_flags{time_flag::ptp_timescale};
    uint8_t time_source{0xA0};  ///< INTERNAL_OSCILLATOR

    friend auto operator==(GrandmasterSettingsNp const&, GrandmasterSettingsNp const&) -> bool = default;

    static constexpr size_t SIZE = GrandmasterSettingsNpPayload::LENGTH;
};

/// The events SUBSCRIBE_EVENTS_NP names, as linuxptp's enum notification: each is pushed as an
/// unsolicited RESPONSE carrying the whole data set.
enum class ManagementEvent : uint8_t
{
    port_state = 0,       ///< a port's PORT_DATA_SET on each state change
    time_sync = 1,        ///< TIME_STATUS_NP on each clock update
    parent_data_set = 2,  ///< PARENT_DATA_SET when it changes (linuxptp 4.2)
    cmlds = 3,            ///< CMLDS_INFO_NP (linuxptp 4.3); not parsed here
};

/// linuxptp's SUBSCRIBE_EVENTS_NP: a duration in seconds (65535: never expires; an empty
/// bitmask unsubscribes) and a 64-byte event bitmask.
struct SubscribeEventsNp
{
    uint16_t duration_s{0};
    std::array<uint8_t, SubscribeEventsNpPayload::BITMASK_LENGTH> bitmask{};

    [[nodiscard]] auto has(ManagementEvent const event) const noexcept -> bool
    {
        auto const bit = static_cast<uint8_t>(event);
        return (bitmask[bit / 8U] & (1U << (bit % 8U))) != 0;
    }
    void add(ManagementEvent const event) noexcept
    {
        auto const bit = static_cast<uint8_t>(event);
        bitmask[bit / 8U] = static_cast<uint8_t>(bitmask[bit / 8U] | (1U << (bit % 8U)));
    }

    static constexpr size_t SIZE = SubscribeEventsNpPayload::LENGTH;
};

/// `scaled` (a TimeInterval: ns scaled by 2^16) in ns, rounded to the nearest.
[[nodiscard]] constexpr auto time_interval_ns(int64_t const scaled) noexcept -> int64_t
{
    int64_t const ns = scaled / 65536;
    int64_t const rest = scaled % 65536;
    return ns + (rest >= 32768 ? 1 : 0) - (rest <= -32768 ? 1 : 0);
}

/// One parsed management message.
struct ManagementMessage
{
    uint8_t transport_specific{0};  ///< the header's majorSdoId (transportSpecific)
    uint8_t domain{0};
    uint16_t sequence_id{0};
    gptp::SourcePortIdentity source{};
    ManagementAction action{ManagementAction::get};
    uint16_t management_id{0};
    /// A MANAGEMENT_ERROR_STATUS TLV's managementErrorId, instead of data.
    std::optional<uint16_t> error;
    std::variant<
        std::monostate,
        ParentDataSet,
        CurrentDataSet,
        TimePropertiesDataSet,
        PortDataSet,
        PortPropertiesNp,
        TimeStatusNp,
        GrandmasterSettingsNp,
        SubscribeEventsNp>
        data;
};

/// Bytes of a GET with no data: header 34 + management header 14 + TLV header 6.
inline constexpr size_t MANAGEMENT_GET_SIZE = 54;

/// What a message's common header says besides its body: the domain, sequence, sender and
/// transportSpecific (ptp4l drops a request whose transportSpecific is not its port's).
struct ManagementAddressing
{
    uint8_t domain{0};
    uint16_t sequence_id{0};
    gptp::SourcePortIdentity source{};
    uint8_t transport_specific{0};
};

/// Write a GET for `management_id` into `out` (at least MANAGEMENT_GET_SIZE
/// bytes) addressed to every clock and port, as pmc does: returns the bytes
/// written, or 0 if `out` is too small.
[[nodiscard]] auto build_management_get(
    std::span<uint8_t> out, uint16_t management_id, ManagementAddressing const& addressing) noexcept -> size_t;

/// Bytes of a SET or a GET carrying the largest data sent here (SUBSCRIBE_EVENTS_NP).
inline constexpr size_t MANAGEMENT_SET_MAX_SIZE = MANAGEMENT_GET_SIZE + SubscribeEventsNp::SIZE;

/// Write a message with `action` for `management_id` carrying `data` (padded to an even length)
/// into `out`, addressed as build_management_get's: returns the bytes written, 0 if `out` is too small.
[[nodiscard]] auto build_management_message(
    std::span<uint8_t> out,
    ManagementAction action,
    uint16_t management_id,
    std::span<uint8_t const> data,
    ManagementAddressing const& addressing) noexcept -> size_t;

/// GRANDMASTER_SETTINGS_NP's data field as linuxptp's struct grandmaster_settings_np.
[[nodiscard]] auto encode(GrandmasterSettingsNp const& settings) noexcept -> std::array<uint8_t, GrandmasterSettingsNp::SIZE>;
/// SUBSCRIBE_EVENTS_NP's data field as linuxptp's struct subscribe_events_np.
[[nodiscard]] auto encode(SubscribeEventsNp const& subscription) noexcept -> std::array<uint8_t, SubscribeEventsNp::SIZE>;

/// Parse a management message. Fails with bad_message unless it is a well-formed PTPv2 management
/// message (any minorVersionPTP, any transportSpecific) with one MANAGEMENT or MANAGEMENT_ERROR_STATUS
/// TLV long enough for its data set; other data sets parse with `data` empty.
[[nodiscard]] auto parse_management_message(std::span<uint8_t const> bytes) noexcept -> StatusValue<ManagementMessage>;

/// What the client knows of ptp4l.
enum class PtpManagementStatus : uint8_t
{
    unreachable,     ///< no answer in the configured domain (ptp4l absent, refused, or in another domain)
    no_grandmaster,  ///< ptp4l answers, and its PARENT_DATA_SET names its own clock: it follows no other
    locked,          ///< ptp4l answers, following `grandmaster`
};

/// What the client has learnt.
struct PtpManagementState
{
    PtpManagementStatus status{PtpManagementStatus::unreachable};
    /// PARENT_DATA_SET's grandmasterIdentity while ptp4l answers (its own
    /// clock when no_grandmaster: then it is, or is becoming, the grandmaster).
    std::optional<ieee::Eui64> grandmaster;
    std::optional<ParentDataSet> parent;
    std::optional<CurrentDataSet> current;
    std::optional<TimePropertiesDataSet> time_properties;
    /// The port named PtpManagementConfig::port_interface, once PORT_PROPERTIES_NP names it.
    std::optional<gptp::SourcePortIdentity> port;
    /// That port's PORT_DATA_SET: peerMeanPathDelay in ns (802.1AS-2020 neighborPropDelay,
    /// measured whatever the port's role), portState and delayMechanism.
    std::optional<int64_t> peer_mean_path_delay_ns;
    std::optional<PortState> port_state;
    std::optional<DelayMechanism> delay_mechanism;
    /// The settings ptp4l acknowledged for a SET GRANDMASTER_SETTINGS_NP.
    std::optional<GrandmasterSettingsNp> grandmaster_settings;
    /// The managementErrorId with which ptp4l last refused our GRANDMASTER_SETTINGS_NP; cleared by
    /// an acknowledged one.
    std::optional<uint16_t> refused;
    /// True while ptp4l holds our SUBSCRIBE_EVENTS_NP (its answer to the last SET or GET of it).
    bool subscribed{false};
    /// The latest TIME_STATUS_NP (time_sync notifications); kept, never reported by itself.
    std::optional<TimeStatusNp> time_status;
    /// The first send or receive error since the last answer (EACCES on a
    /// root-only socket, ENOENT with no ptp4l, ...); clear while ptp4l answers.
    std::error_code error;
};

struct PtpManagementConfig
{
    std::string uds_address{"/var/run/ptp4l"};  ///< ptp4l's uds_address; absolute
    uint8_t domain{0};                          ///< ptp4l's domainNumber (pmc -d)
    /// Between polls of what ptp4l pushes no notification for: TIME_PROPERTIES_DATA_SET and the
    /// followed port's PORT_DATA_SET (its peer delay), and everything while it refuses a subscription.
    int64_t poll_period_ns{2'000'000'000};
    int64_t retry_period_ns{1'000'000'000};  ///< between probes while ptp4l is unreachable
    std::string port_interface;              ///< the port whose PORT_DATA_SET to follow (empty: none)
    /// Events besides port_state and parent_data_set, which the client always subscribes to.
    SubscribeEventsNp events{};
    /// Renewed three times per duration, as phc2sys does; each renewal checks ptp4l still holds it
    /// (a restarted ptp4l does not: then everything is fetched again). At least 3, below 65535.
    uint16_t subscription_duration_s{6};
};

/// One initial fetch of ptp4l's data sets, then its SUBSCRIBE_EVENTS_NP notifications, polling only
/// the rest. Silence for two renewals, or a refused send, is unreachable until an answer and a
/// PARENT_DATA_SET start over (docs/LINUXPTP.md).
class PtpManagementClient final : public net::Pollable
{
  public:
    /// Called on the reactor thread when the status, the grandmaster, the error, the followed
    /// port's peer delay, state or delay mechanism, the grandmaster settings, a refusal or the
    /// subscription changes.
    using Listener = sg14::inplace_function<void(PtpManagementState const&), 64>;

    /// Bind the client's socket. Fails with invalid_argument for a bad
    /// configuration, not_supported off Linux, or the socket's error.
    [[nodiscard]] static auto create(PtpManagementConfig config, Listener listener)
        -> StatusValue<std::unique_ptr<PtpManagementClient>>;

    ~PtpManagementClient() override;
    PtpManagementClient(PtpManagementClient const&) = delete;
    auto operator=(PtpManagementClient const&) -> PtpManagementClient& = delete;

    [[nodiscard]] auto state() const noexcept -> PtpManagementState const& { return state_; }

    /// Handle one received message (on_ready's body; public for tests).
    void handle(std::span<uint8_t const> bytes, int64_t now_ns);
    /// SET GRANDMASTER_SETTINGS_NP now, and again whenever ptp4l is found (each renewal asks) to hold
    /// others or to have restarted; needs ptp4l's read-write socket. Fails with the send's error.
    [[nodiscard]] auto set_grandmaster_settings(GrandmasterSettingsNp const& settings) -> Status;

    // -- Pollable --
    [[nodiscard]] auto fd() const noexcept -> int override { return fd_.get(); }
    void on_ready(int64_t now_ns) override;
    void tick(int64_t now_ns) override;
    /// True after a socket error other than EAGAIN/EINTR (see state().error).
    [[nodiscard]] auto finished() const noexcept -> bool override { return finished_; }

  private:
    PtpManagementClient(
        PtpManagementConfig config,
        Listener listener,
        FileDescriptor fd,
        net::SocketAddress server,
        gptp::SourcePortIdentity const& source) noexcept;
    void note_error(std::error_code error);
    /// An answer from ptp4l at @p now_ns: one that is not a port's notification makes an
    /// unreachable ptp4l reachable, in its transportSpecific, and fetched from scratch.
    void note_answer(ManagementMessage const& message, int64_t now_ns);
    /// Apply what the RESPONSE @p message carries to state_.
    void apply(ManagementMessage const& message, int64_t now_ns);
    void apply_parent(ParentDataSet const& parent, gptp::ClockIdentity const& sender);
    void apply_port_data_set(PortDataSet const& port_data);
    void apply_settings(GrandmasterSettingsNp const& settings);
    void apply_subscription(SubscribeEventsNp const& held, int64_t now_ns);
    void publish_if_changed();
    /// While unreachable: one GET PARENT_DATA_SET per transportSpecific not yet ruled out.
    void probe_if_due(int64_t now_ns);
    /// Every data set followed, the settings and the subscription: the start of every reachable spell.
    void fetch_all(int64_t now_ns);
    void poll_if_due(int64_t now_ns);
    /// GET then SET SUBSCRIBE_EVENTS_NP: the GET's answer says whether ptp4l still held it.
    void renew_if_due(int64_t now_ns);
    /// Forget everything learnt (unreachable), and probe from @p now_ns.
    void forget(int64_t now_ns);
    [[nodiscard]] auto silence_ns() const noexcept -> int64_t;
    [[nodiscard]] auto send(ManagementAction action, uint16_t management_id, std::span<uint8_t const> data) -> Status;
    [[nodiscard]] auto send_as(
        ManagementAction action, uint16_t management_id, std::span<uint8_t const> data, uint8_t transport_specific) -> Status;
    void send_gets(std::span<uint16_t const> ids);
    /// A send's @p error, noted (state().error, and whether ptp4l's socket is gone); returned.
    auto note_send_error(std::error_code error) -> std::error_code;
    void send_settings();
    void send_subscription(int64_t now_ns);
    [[nodiscard]] auto subscription() const noexcept -> SubscribeEventsNp;

    PtpManagementConfig config_;
    Listener listener_;
    FileDescriptor fd_;
    net::SocketAddress server_;
    gptp::SourcePortIdentity source_{};
    uint16_t sequence_{0};
    /// ptp4l's UDS transportSpecific, from the first answer after unreachable (a request in another
    /// one is dropped); a port's notifications carry that port's, which is not used.
    std::optional<uint8_t> transport_specific_;
    bool reachable_{false};  ///< ptp4l has answered since the client last forgot it
    int64_t next_probe_ns_{INT64_MIN};
    int64_t next_poll_ns_{0};
    int64_t next_renewal_ns_{0};
    std::optional<GrandmasterSettingsNp> settings_;
    bool finished_{false};
    bool server_gone_{false};  ///< a send found ptp4l's socket gone since the last answer
    int64_t last_answer_ns_{0};
    PtpManagementState state_;
    PtpManagementStatus reported_status_{PtpManagementStatus::unreachable};
    std::optional<ieee::Eui64> reported_grandmaster_;
    std::error_code reported_error_;
    std::optional<int64_t> reported_peer_delay_ns_;
    std::optional<PortState> reported_port_state_;
    std::optional<DelayMechanism> reported_delay_mechanism_;
    std::optional<GrandmasterSettingsNp> reported_settings_;
    std::optional<uint16_t> reported_refused_;
    bool reported_subscribed_{false};
    std::array<uint8_t, 1500> buffer_{};
};

}  // namespace statusbar::ptpclient
