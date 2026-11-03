// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_management.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/gptp/gptp_header.hpp"

#if defined(__linux__)
#    include "statusbar/net/net_socket.hpp"

#    include <fcntl.h>
#    include <unistd.h>

#    include <sys/socket.h>
#    include <sys/un.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <utility>

namespace statusbar::ptpclient {

namespace {

constexpr size_t HEADER_SIZE = gptp::MessageHeader::LENGTH;  ///< common header (13.3)
constexpr size_t TLV_OFFSET = HEADER_SIZE + ManagementHeaderPayload::LENGTH;
constexpr uint8_t MESSAGE_TYPE_MANAGEMENT = 0x0D;
constexpr uint8_t CONTROL_MANAGEMENT = 0x04;
constexpr uint8_t LOG_INTERVAL_NONE = 0x7F;
constexpr uint16_t TLV_MANAGEMENT = 0x0001;
constexpr uint16_t TLV_MANAGEMENT_ERROR_STATUS = 0x0002;
constexpr gptp::SourcePortIdentity ALL_PORTS{gptp::ClockIdentity{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 0xFFFF};

/// What fetch_all() asks for, in order: the clock's data sets, then (with a port
/// interface) PORT_PROPERTIES_NP, then every port's PORT_DATA_SET.
constexpr std::array<uint16_t, 5> FETCHED_DATA_SETS{
    management_id::parent_data_set,
    management_id::current_data_set,
    management_id::time_properties_data_set,
    management_id::port_properties_np,
    management_id::port_data_set};

/// The smallest data field a RESPONSE for each data set parsed here carries.
struct DataSetSize
{
    uint16_t id;
    size_t size;
};
constexpr std::array<DataSetSize, 8> DATA_SET_SIZES{{
    {.id = management_id::parent_data_set, .size = ParentDataSet::SIZE},
    {.id = management_id::current_data_set, .size = CurrentDataSet::SIZE},
    {.id = management_id::time_properties_data_set, .size = TimePropertiesDataSet::SIZE},
    {.id = management_id::port_data_set, .size = PortDataSet::SIZE},
    {.id = management_id::port_properties_np, .size = PortPropertiesNp::SIZE},
    {.id = management_id::time_status_np, .size = TimeStatusNp::SIZE},
    {.id = management_id::grandmaster_settings_np, .size = GrandmasterSettingsNp::SIZE},
    {.id = management_id::subscribe_events_np, .size = SubscribeEventsNp::SIZE},
}};

/// The packed @p T at the head of @p bytes (at least T::LENGTH octets).
template <typename T>
[[nodiscard]] auto load(std::span<uint8_t const> const bytes) noexcept -> T
{
    T value{};
    span_load(value, bytes.first(sizeof(T)));
    return value;
}

[[nodiscard]] auto bad_message() noexcept -> std::unexpected<std::error_code>
{
    return failure(std::errc::bad_message);
}

/// The common header of a management message (Clause 13.3) for one of @p length octets.
void store_ptp_header(std::span<uint8_t> const out, size_t const length, ManagementAddressing const& addressing) noexcept
{  // NOLINT(statusbar-block-statements): one header field per statement
    gptp::MessageHeader header{};
    header.set_major_sdo_id(addressing.transport_specific);
    header.set_message_type(MESSAGE_TYPE_MANAGEMENT);
    header.set_version_ptp(gptp::VERSION_PTP);
    header.message_length = static_cast<uint16_t>(length);
    header.domain_number = addressing.domain;
    header.source_port_identity = addressing.source;
    header.sequence_id = addressing.sequence_id;
    header.control_field = CONTROL_MANAGEMENT;
    header.log_message_interval = LOG_INTERVAL_NONE;
    span_store(out, header);
}

/// The management header addressed to every clock and port, as pmc -b 0 does (Clause 15.4.1), and
/// the MANAGEMENT TLV's header and managementId of a message of @p length octets (Clause 15.5.2).
void store_management_header(
    std::span<uint8_t> const out, ManagementAction const action, uint16_t const management_id, size_t const length) noexcept
{
    span_store(
        out.subspan(HEADER_SIZE),
        ManagementHeaderPayload{.target_port_identity = ALL_PORTS, .action = static_cast<uint8_t>(action)});
    span_store(
        out.subspan(TLV_OFFSET),
        ManagementTlvPayload{
            .tlv_type = TLV_MANAGEMENT,
            .length_field = static_cast<uint16_t>(length - TLV_OFFSET - TlvHeaderPayload::LENGTH),
            .management_id = management_id});
}

/// @p data at the front of @p field, the rest of it zero (the pad octet).
void store_padded(std::span<uint8_t> const field, std::span<uint8_t const> const data) noexcept
{
    std::ranges::copy(data, field.begin());
    std::ranges::fill(field.subspan(data.size()), uint8_t{0});
}

/// What the common and management headers say of a message, before its TLV.
[[nodiscard]] auto message_of(gptp::MessageHeader const& header, ManagementHeaderPayload const& management) noexcept
    -> ManagementMessage
{
    return ManagementMessage{
        .transport_specific = header.major_sdo_id(),
        .domain = header.domain_number,
        .sequence_id = header.sequence_id,
        .source = header.source_port_identity,
        .action = static_cast<ManagementAction>(management.action.get() & 0x0FU),
    };
}

/// One TLV: its type and its value (lengthField octets).
struct Tlv
{
    uint16_t type;
    std::span<uint8_t const> value;
};

/// The common header of the management message @p bytes, checked: a PTPv2 management message
/// (any minorVersionPTP and transportSpecific) whose messageLength holds a TLV header and fits.
[[nodiscard]] auto parse_header(std::span<uint8_t const> const bytes) noexcept -> StatusValue<gptp::MessageHeader>
{
    if (bytes.size() < TLV_OFFSET + TlvHeaderPayload::LENGTH) {
        return bad_message();
    }
    auto const header = load<gptp::MessageHeader>(bytes);
    if (header.message_type() != MESSAGE_TYPE_MANAGEMENT || header.version_ptp() != gptp::VERSION_PTP) {
        return bad_message();
    }
    size_t const length = header.message_length;
    if (length < TLV_OFFSET + TlvHeaderPayload::LENGTH || length > bytes.size()) {
        return bad_message();
    }
    return header;
}

/// The TLV of the management message @p message (its messageLength octets), with room for a managementId.
[[nodiscard]] auto parse_tlv(std::span<uint8_t const> const message) noexcept -> StatusValue<Tlv>
{
    auto const tlv = load<TlvHeaderPayload>(message.subspan(TLV_OFFSET));
    size_t const length = tlv.length_field;
    if (length < 2 || TLV_OFFSET + TlvHeaderPayload::LENGTH + length > message.size()) {
        return bad_message();
    }
    return Tlv{.type = tlv.tlv_type, .value = message.subspan(TLV_OFFSET + TlvHeaderPayload::LENGTH, length)};
}

[[nodiscard]] auto decode_parent(std::span<uint8_t const> const data) noexcept -> ParentDataSet
{
    auto const p = load<ParentDataSetPayload>(data);
    return ParentDataSet{
        .parent_port_identity = p.parent_port_identity,
        .parent_stats = (p.flags.get() & 0x01U) != 0,
        .observed_parent_offset_scaled_log_variance = p.observed_parent_offset_scaled_log_variance,
        .observed_parent_clock_phase_change_rate = static_cast<int32_t>(p.observed_parent_clock_phase_change_rate.get()),
        .grandmaster_priority1 = p.grandmaster_priority1,
        .grandmaster_clock_class = p.grandmaster_clock_class,
        .grandmaster_clock_accuracy = p.grandmaster_clock_accuracy,
        .grandmaster_offset_scaled_log_variance = p.grandmaster_offset_scaled_log_variance,
        .grandmaster_priority2 = p.grandmaster_priority2,
        .grandmaster_identity = p.grandmaster_identity,
    };
}

[[nodiscard]] auto decode_current(std::span<uint8_t const> const data) noexcept -> CurrentDataSet
{
    auto const p = load<CurrentDataSetPayload>(data);
    return CurrentDataSet{
        .steps_removed = p.steps_removed,
        .offset_from_master_scaled = static_cast<int64_t>(p.offset_from_master.get()),
        .mean_path_delay_scaled = static_cast<int64_t>(p.mean_path_delay.get()),
    };
}

[[nodiscard]] auto decode_time_properties(std::span<uint8_t const> const data) noexcept -> TimePropertiesDataSet
{
    auto const p = load<TimePropertiesDataSetPayload>(data);
    return TimePropertiesDataSet{
        .current_utc_offset = static_cast<int16_t>(p.current_utc_offset.get()), .flags = p.flags, .time_source = p.time_source};
}

[[nodiscard]] auto decode_port_data_set(std::span<uint8_t const> const data) noexcept -> PortDataSet
{
    auto const p = load<PortDataSetPayload>(data);
    return PortDataSet{
        .port_identity = p.port_identity,
        .port_state = static_cast<PortState>(p.port_state.get()),
        .log_min_delay_req_interval = static_cast<int8_t>(p.log_min_delay_req_interval.get()),
        .peer_mean_path_delay_scaled = static_cast<int64_t>(p.peer_mean_path_delay.get()),
        .log_announce_interval = static_cast<int8_t>(p.log_announce_interval.get()),
        .announce_receipt_timeout = p.announce_receipt_timeout,
        .log_sync_interval = static_cast<int8_t>(p.log_sync_interval.get()),
        .delay_mechanism = static_cast<DelayMechanism>(p.delay_mechanism.get()),
        .log_min_pdelay_req_interval = static_cast<int8_t>(p.log_min_pdelay_req_interval.get()),
        .version_number = p.version_number,
    };
}

/// The interface name @p text into @p properties, truncated to what it holds.
void copy_interface_name(PortPropertiesNp& properties, std::span<uint8_t const> const text) noexcept
{
    properties.interface_length = static_cast<uint8_t>(std::min(text.size(), properties.interface.size()));
    std::ranges::transform(text.first(properties.interface_length), properties.interface.begin(), [](uint8_t const c) {
        return static_cast<char>(c);
    });
}

/// PORT_PROPERTIES_NP's @p data (at least PortPropertiesNp::SIZE octets, its name's text following).
[[nodiscard]] auto decode_port_properties(std::span<uint8_t const> const data) noexcept -> StatusValue<PortPropertiesNp>
{
    auto const p = load<PortPropertiesNpPayload>(data);
    size_t const text_length = p.interface_length;
    if (PortPropertiesNp::SIZE + text_length > data.size()) {
        return bad_message();
    }
    PortPropertiesNp properties{
        .port_identity = p.port_identity, .port_state = static_cast<PortState>(p.port_state.get()), .timestamping = p.timestamping};
    copy_interface_name(properties, data.subspan(PortPropertiesNp::SIZE, text_length));
    return properties;
}

[[nodiscard]] auto decode_time_status(std::span<uint8_t const> const data) noexcept -> TimeStatusNp
{
    auto const p = load<TimeStatusNpPayload>(data);
    return TimeStatusNp{
        .master_offset_ns = static_cast<int64_t>(p.master_offset.get()),
        .ingress_time_ns = static_cast<int64_t>(p.ingress_time.get()),
        .cumulative_scaled_rate_offset = static_cast<int32_t>(p.cumulative_scaled_rate_offset.get()),
        .scaled_last_gm_phase_change = static_cast<int32_t>(p.scaled_last_gm_phase_change.get()),
        .gm_time_base_indicator = p.gm_time_base_indicator,
        .gm_present = p.gm_present.get() != 0,
        .gm_identity = p.gm_identity,
    };
}

[[nodiscard]] auto decode_settings(std::span<uint8_t const> const data) noexcept -> GrandmasterSettingsNp
{
    auto const p = load<GrandmasterSettingsNpPayload>(data);
    return GrandmasterSettingsNp{
        .clock_class = p.clock_class,
        .clock_accuracy = p.clock_accuracy,
        .offset_scaled_log_variance = p.offset_scaled_log_variance,
        .current_utc_offset = static_cast<int16_t>(p.current_utc_offset.get()),
        .time_flags = p.time_flags,
        .time_source = p.time_source,
    };
}

[[nodiscard]] auto decode_subscription(std::span<uint8_t const> const data) noexcept -> SubscribeEventsNp
{
    auto const p = load<SubscribeEventsNpPayload>(data);
    return SubscribeEventsNp{.duration_s = p.duration, .bitmask = p.bitmask};
}

/// @p message with the data set its managementId names decoded from @p data (at least its size).
[[nodiscard]] auto decode_data_set(ManagementMessage message, std::span<uint8_t const> const data) noexcept
    -> StatusValue<ManagementMessage>
{  // NOLINT(statusbar-block-statements): one case per data set
    switch (message.management_id) {
        case management_id::parent_data_set:
            message.data = decode_parent(data);
            break;
        case management_id::current_data_set:
            message.data = decode_current(data);
            break;
        case management_id::time_properties_data_set:
            message.data = decode_time_properties(data);
            break;
        case management_id::port_data_set:
            message.data = decode_port_data_set(data);
            break;
        case management_id::port_properties_np: {
            auto const properties = decode_port_properties(data);
            if (!properties) {
                return forward_failure(properties);
            }
            message.data = *properties;
            break;
        }
        case management_id::time_status_np:
            message.data = decode_time_status(data);
            break;
        case management_id::grandmaster_settings_np:
            message.data = decode_settings(data);
            break;
        case management_id::subscribe_events_np:
            message.data = decode_subscription(data);
            break;
        default:
            break;
    }
    return message;
}

/// @p message with the MANAGEMENT TLV value @p value's managementId and, for a data set
/// parsed here, its data. A RESPONSE too short for its data set is malformed; a GET carries none.
[[nodiscard]] auto parse_management_tlv(ManagementMessage message, std::span<uint8_t const> const value) noexcept
    -> StatusValue<ManagementMessage>
{
    message.management_id = load<doublet_t>(value).get();
    auto const* const known = std::ranges::find(DATA_SET_SIZES, message.management_id, &DataSetSize::id);
    if (known == DATA_SET_SIZES.end()) {
        return message;  // a data set not parsed here
    }
    auto const data = value.subspan(2);
    if (data.size() < known->size) {
        return message.action == ManagementAction::response ? bad_message() : StatusValue<ManagementMessage>{message};
    }
    return decode_data_set(message, data);
}

/// @p message with @p tlv: a MANAGEMENT_ERROR_STATUS's error, or a MANAGEMENT TLV's
/// managementId and data.
[[nodiscard]] auto parse_tlv_value(ManagementMessage message, Tlv const& tlv) noexcept -> StatusValue<ManagementMessage>
{
    if (tlv.type == TLV_MANAGEMENT_ERROR_STATUS) {
        if (tlv.value.size() < ManagementErrorStatusPayload::LENGTH) {
            return bad_message();
        }
        auto const status = load<ManagementErrorStatusPayload>(tlv.value);
        message.error = status.management_error_id.get();
        message.management_id = status.management_id.get();
        return message;
    }
    if (tlv.type != TLV_MANAGEMENT) {
        return bad_message();
    }
    return parse_management_tlv(message, tlv.value);
}

}  // namespace

auto build_management_get(
    std::span<uint8_t> const out, uint16_t const management_id, ManagementAddressing const& addressing) noexcept -> size_t
{
    return build_management_message(out, ManagementAction::get, management_id, {}, addressing);
}

auto build_management_message(
    std::span<uint8_t> const out,
    ManagementAction const action,
    uint16_t const management_id,
    std::span<uint8_t const> const data,
    ManagementAddressing const& addressing) noexcept -> size_t
{
    size_t const length = MANAGEMENT_GET_SIZE + data.size() + (data.size() % 2);
    if (out.size() < length || length > UINT16_MAX) {
        return 0;
    }
    store_ptp_header(out, length, addressing);
    store_management_header(out, action, management_id, length);
    store_padded(out.subspan(MANAGEMENT_GET_SIZE, length - MANAGEMENT_GET_SIZE), data);
    return length;
}

auto encode(GrandmasterSettingsNp const& settings) noexcept -> std::array<uint8_t, GrandmasterSettingsNp::SIZE>
{
    std::array<uint8_t, GrandmasterSettingsNp::SIZE> out{};
    span_store(
        out,
        GrandmasterSettingsNpPayload{
            .clock_class = settings.clock_class,
            .clock_accuracy = settings.clock_accuracy,
            .offset_scaled_log_variance = settings.offset_scaled_log_variance,
            .current_utc_offset = static_cast<uint16_t>(settings.current_utc_offset),
            .time_flags = settings.time_flags,
            .time_source = settings.time_source});
    return out;
}

auto encode(SubscribeEventsNp const& subscription) noexcept -> std::array<uint8_t, SubscribeEventsNp::SIZE>
{
    std::array<uint8_t, SubscribeEventsNp::SIZE> out{};
    span_store(out, SubscribeEventsNpPayload{.duration = subscription.duration_s, .bitmask = subscription.bitmask});
    return out;
}

auto parse_management_message(std::span<uint8_t const> const bytes) noexcept -> StatusValue<ManagementMessage>
{
    auto const header = parse_header(bytes);
    if (!header) {
        return forward_failure(header);
    }
    auto const tlv = parse_tlv(bytes.first(header->message_length));
    if (!tlv) {
        return forward_failure(tlv);
    }
    return parse_tlv_value(message_of(*header, load<ManagementHeaderPayload>(bytes.subspan(HEADER_SIZE))), *tlv);
}

// --- PtpManagementClient ---

PtpManagementClient::PtpManagementClient(
    PtpManagementConfig config,
    Listener listener,
    FileDescriptor fd,
    net::SocketAddress server,
    gptp::SourcePortIdentity const& source) noexcept
    : config_{std::move(config)}
    , listener_{std::move(listener)}
    , fd_{std::move(fd)}
    , server_{server}
    , source_{source}
{}

auto clock_synchronized(PtpManagementState const& state) noexcept -> bool
{
    auto const in = [&state](PortState const wanted) {
        return std::ranges::any_of(state.port_states, [wanted](std::optional<PortState> const& s) { return s == wanted; });
    };
    if (state.status == PtpManagementStatus::locked) {
        return in(PortState::slave);
    }
    return state.status == PtpManagementStatus::no_grandmaster && in(PortState::master) && !in(PortState::uncalibrated);
}

void PtpManagementClient::note_error(std::error_code const error)
{
    if (!state_.error) {
        state_.error = error;
        publish_if_changed();
    }
}

void PtpManagementClient::handle(std::span<uint8_t const> const bytes, int64_t const now_ns)
{
    auto message = parse_management_message(bytes);
    if (!message || message->action != ManagementAction::response) {
        return;
    }
    note_answer(*message, now_ns);
    apply(*message, now_ns);
    publish_if_changed();
}

void PtpManagementClient::note_answer(ManagementMessage const& message, int64_t const now_ns)
{
    last_answer_ns_ = now_ns;
    server_gone_ = false;
    state_.error = {};
    // A port's PORT_DATA_SET notification may carry its own transportSpecific, not the UDS
    // port's: only a clock data set's answer makes ptp4l reachable and sets the one to use.
    if (!reachable_ && message.management_id != management_id::port_data_set) {
        reachable_ = true;
        transport_specific_ = message.transport_specific;
        fetch_all(now_ns);
    }
}

void PtpManagementClient::apply(ManagementMessage const& message, int64_t const now_ns)
{  // NOLINT(statusbar-block-statements): one branch per data set
    if (message.error) {
        // A refused subscription (the read-only socket) leaves `subscribed` false: then everything is polled.
        bool const ours = message.management_id == management_id::grandmaster_settings_np;
        state_.refused = ours ? message.error : state_.refused;
    } else if (auto const* parent = std::get_if<ParentDataSet>(&message.data)) {
        apply_parent(*parent, message.source.clock_identity);
    } else if (auto const* current = std::get_if<CurrentDataSet>(&message.data)) {
        state_.current = *current;
    } else if (auto const* properties = std::get_if<TimePropertiesDataSet>(&message.data)) {
        state_.time_properties = *properties;
    } else if (auto const* port = std::get_if<PortPropertiesNp>(&message.data)) {
        if (!config_.port_interface.empty() && port->interface_name() == config_.port_interface) {
            state_.port = port->port_identity;
        }
    } else if (auto const* port_data = std::get_if<PortDataSet>(&message.data)) {
        apply_port_data_set(*port_data);
    } else if (auto const* time_status = std::get_if<TimeStatusNp>(&message.data)) {
        state_.time_status = *time_status;
    } else if (auto const* settings = std::get_if<GrandmasterSettingsNp>(&message.data)) {
        apply_settings(*settings);
    } else if (auto const* subscription = std::get_if<SubscribeEventsNp>(&message.data)) {
        apply_subscription(*subscription, now_ns);
    }
}

void PtpManagementClient::apply_parent(ParentDataSet const& parent, gptp::ClockIdentity const& sender)
{
    // A new parent (a notification): what ptp4l pushes nothing for may have changed with it.
    if (state_.parent && state_.parent->grandmaster_identity.get() != parent.grandmaster_identity.get()) {
        std::array<uint16_t, 2> const ids{management_id::current_data_set, management_id::time_properties_data_set};
        send_gets(ids);
    }
    state_.parent = parent;
    state_.grandmaster = parent.grandmaster_identity;
    bool const self = parent.grandmaster_identity.get() == sender.get();
    state_.status = self ? PtpManagementStatus::no_grandmaster : PtpManagementStatus::locked;
}

void PtpManagementClient::apply_port_data_set(PortDataSet const& port_data)
{
    if (uint16_t const number = port_data.port_identity.port_number.get(); number >= 1 && number <= MAX_PTP4L_PORTS) {
        note_port_state(state_.port_states.at(number - 1U), port_data.port_state);
    }
    if (state_.port != port_data.port_identity) {
        return;
    }
    state_.peer_mean_path_delay_ns = time_interval_ns(port_data.peer_mean_path_delay_scaled);
    state_.port_state = port_data.port_state;
    state_.delay_mechanism = port_data.delay_mechanism;
}

void PtpManagementClient::note_port_state(std::optional<PortState>& held, PortState const now)
{
    // A linuxptp before 4.2 echoes the PARENT_DATA_SET notify bit without honouring it: a port's
    // state change asks for the parent, which a new grandmaster brings with it.
    if (state_.subscribed && held && *held != now) {
        std::array<uint16_t, 1> const parent{management_id::parent_data_set};
        send_gets(parent);
    }
    held = now;
}

void PtpManagementClient::apply_settings(GrandmasterSettingsNp const& settings)
{
    state_.grandmaster_settings = settings;
    if (settings_ && settings != *settings_) {
        send_settings();  // ptp4l holds others: it restarted, or another client set them
        return;
    }
    state_.refused.reset();
}

void PtpManagementClient::apply_subscription(SubscribeEventsNp const& held, int64_t const now_ns)
{
    if (held.bitmask == subscription().bitmask) {
        state_.subscribed = true;
        return;
    }
    // Not held: a restarted ptp4l has forgotten everything we told it and may hold new data sets.
    if (state_.subscribed) {
        state_.subscribed = false;
        fetch_all(now_ns);
    }
}

void PtpManagementClient::publish_if_changed()
{  // NOLINT(statusbar-block-statements): one reported field per statement
    if (state_.status == reported_status_ && state_.grandmaster == reported_grandmaster_ && state_.error == reported_error_ &&
        state_.peer_mean_path_delay_ns == reported_peer_delay_ns_ && state_.port_state == reported_port_state_ &&
        state_.port_states == reported_port_states_ && state_.delay_mechanism == reported_delay_mechanism_ &&
        state_.grandmaster_settings == reported_settings_ && state_.refused == reported_refused_ &&
        state_.subscribed == reported_subscribed_) {
        return;
    }
    reported_settings_ = state_.grandmaster_settings;
    reported_refused_ = state_.refused;
    reported_subscribed_ = state_.subscribed;
    reported_status_ = state_.status;
    reported_grandmaster_ = state_.grandmaster;
    reported_error_ = state_.error;
    reported_peer_delay_ns_ = state_.peer_mean_path_delay_ns;
    reported_port_state_ = state_.port_state;
    reported_port_states_ = state_.port_states;
    reported_delay_mechanism_ = state_.delay_mechanism;
    if (listener_) {
        listener_(state_);
    }
}

void PtpManagementClient::tick(int64_t const now_ns)
{
    if (finished_) {
        return;
    }
    if (!reachable_) {
        probe_if_due(now_ns);
        return;
    }
    if (now_ns - last_answer_ns_ > silence_ns()) {
        forget(now_ns);
        return;
    }
    poll_if_due(now_ns);
    renew_if_due(now_ns);
    if (server_gone_) {
        forget(now_ns);  // ptp4l's socket refused a send: it exited or is restarting
    }
}

void PtpManagementClient::forget(int64_t const now_ns)
{  // NOLINT(statusbar-block-statements): a member-wise reset
    std::error_code const error = state_.error;
    state_ = PtpManagementState{.error = error};
    reachable_ = false;
    transport_specific_.reset();
    server_gone_ = false;
    next_probe_ns_ = now_ns;
    publish_if_changed();
}

auto PtpManagementClient::silence_ns() const noexcept -> int64_t
{
    // Two renewals unanswered: ptp4l answers each, even with a refusal.
    return 2 * (int64_t{config_.subscription_duration_s} * 1'000'000'000 / 3);
}

void PtpManagementClient::probe_if_due(int64_t const now_ns)
{
    if (now_ns < next_probe_ns_) {
        return;
    }
    next_probe_ns_ = now_ns + config_.retry_period_ns;
    // ptp4l drops a request in another transportSpecific than its port's (port.c port_ignore).
    for (uint8_t const ts : {uint8_t{0}, uint8_t{1}}) {
        if (!send_as(ManagementAction::get, management_id::parent_data_set, {}, ts)) {
            return;  // ENOENT: no ptp4l; EACCES: its socket's mode
        }
    }
}

void PtpManagementClient::fetch_all(int64_t const now_ns)
{
    state_.port_states = {};  // a restarted ptp4l may have other ports
    // PORT_PROPERTIES_NP before PORT_DATA_SET: ptp4l answers in order, so the port is known first.
    // Every port's PORT_DATA_SET either way: clock_synchronized() reads their states.
    send_gets(std::span{FETCHED_DATA_SETS}.first(config_.port_interface.empty() ? 3 : 4));
    send_gets(std::span{FETCHED_DATA_SETS}.last(1));
    send_settings();
    send_subscription(now_ns);
    next_poll_ns_ = now_ns + config_.poll_period_ns;
}

void PtpManagementClient::poll_if_due(int64_t const now_ns)
{
    if (now_ns < next_poll_ns_) {
        return;
    }
    next_poll_ns_ = now_ns + config_.poll_period_ns;
    // The followed port's PORT_DATA_SET once PORT_PROPERTIES_NP named it; without notifications
    // (no subscription), every port's and PARENT_DATA_SET too.
    bool const follows = !config_.port_interface.empty();
    std::array<uint16_t, 4> const ids{
        management_id::time_properties_data_set,
        management_id::port_properties_np,
        management_id::port_data_set,
        management_id::parent_data_set};
    std::array<bool, 4> const wanted{
        true, follows && !state_.port, (follows && state_.port) || !state_.subscribed, !state_.subscribed};
    for (size_t i = 0; i < ids.size(); ++i) {
        if (wanted.at(i)) {
            send_gets(std::span{ids}.subspan(i, 1));
        }
    }
}

void PtpManagementClient::renew_if_due(int64_t const now_ns)
{
    if (now_ns < next_renewal_ns_) {
        return;
    }
    // What a restarted ptp4l has forgotten: the subscription, and the settings when we gave some.
    std::array<uint16_t, 2> const held{management_id::subscribe_events_np, management_id::grandmaster_settings_np};
    send_gets(std::span{held}.first(settings_ ? 2 : 1));
    send_subscription(now_ns);
}

auto PtpManagementClient::subscription() const noexcept -> SubscribeEventsNp
{
    SubscribeEventsNp subscription = config_.events;
    subscription.duration_s = config_.subscription_duration_s;
    subscription.add(ManagementEvent::port_state);
    subscription.add(ManagementEvent::parent_data_set);
    return subscription;
}

void PtpManagementClient::send_gets(std::span<uint16_t const> const ids)
{
    for (uint16_t const id : ids) {
        if (!send(ManagementAction::get, id, {})) {
            return;
        }
    }
}

void PtpManagementClient::send_settings()
{
    if (settings_) {
        (void)send(ManagementAction::set, management_id::grandmaster_settings_np, encode(*settings_));
    }
}

void PtpManagementClient::send_subscription(int64_t const now_ns)
{
    // Three renewals per duration, as phc2sys (pmc_agent.c), so one can be lost before it expires.
    next_renewal_ns_ = now_ns + (int64_t{config_.subscription_duration_s} * 1'000'000'000 / 3);
    (void)send(ManagementAction::set, management_id::subscribe_events_np, encode(subscription()));
}

auto PtpManagementClient::send(ManagementAction const action, uint16_t const management_id, std::span<uint8_t const> const data)
    -> Status
{
    return send_as(action, management_id, data, transport_specific_.value_or(0));
}

auto PtpManagementClient::set_grandmaster_settings(GrandmasterSettingsNp const& settings) -> Status
{
    settings_ = settings;
    return send(ManagementAction::set, management_id::grandmaster_settings_np, encode(settings));
}

#if defined(__linux__)

PtpManagementClient::~PtpManagementClient()
{
    listener_ = {};  // whatever it reaches may be gone already
    if (reachable_) {
        // An empty bitmask unsubscribes (clock.c clock_update_subscription): no events to a closed socket.
        (void)send(ManagementAction::set, management_id::subscribe_events_np, encode(SubscribeEventsNp{}));
    }
}

namespace {

/// The address of ptp4l's socket in @p config, checking the rest of @p config.
[[nodiscard]] auto server_address(PtpManagementConfig const& config) -> StatusValue<net::SocketAddress>
{
    if (config.poll_period_ns <= 0 || config.retry_period_ns <= 0 || config.subscription_duration_s < 3 ||
        config.subscription_duration_s == UINT16_MAX) {
        return failure(std::errc::invalid_argument);
    }
    auto server = net::SocketAddress::local(config.uds_address);  // absolute, no NUL, fits
    if (!server) {
        return failure(std::errc::invalid_argument);
    }
    return *server;
}

/// A non-blocking, close-on-exec datagram socket.
[[nodiscard]] auto local_socket() -> StatusValue<FileDescriptor>
{
    FileDescriptor fd{::socket(AF_UNIX, SOCK_DGRAM, 0)};
    if (!fd.valid()) {
        return failure(net::errno_code());
    }
    if (::fcntl(fd.get(), F_SETFD, FD_CLOEXEC) != 0 || !net::set_nonblocking(fd.get())) {
        return failure(net::errno_code());
    }
    return fd;
}

/// A local socket autobound to an abstract address of the kernel's choosing (Linux), unique to
/// this client: ptp4l answers each request to its sender's address (recvfrom, then sendto).
[[nodiscard]] auto bind_autobound() -> StatusValue<FileDescriptor>
{
    return local_socket().and_then([](FileDescriptor fd) -> StatusValue<FileDescriptor> {
        sockaddr_un own{};
        own.sun_family = AF_UNIX;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sockaddr_un passed as sockaddr
        if (::bind(fd.get(), reinterpret_cast<sockaddr const*>(&own), sizeof(sa_family_t)) != 0) {
            return failure(net::errno_code());
        }
        return fd;
    });
}

/// Our port identity: the pid in the clock identity and a port number per client (1-65534), so two
/// clients in one process differ (ptp4l keys a subscription by it); ptp4l echoes it as the target.
[[nodiscard]] auto client_port_identity() -> gptp::SourcePortIdentity
{
    static std::atomic<uint32_t> next_client{0};
    auto const pid = static_cast<uint32_t>(::getpid());
    auto const port = static_cast<uint16_t>(1 + (next_client.fetch_add(1, std::memory_order_relaxed) % 0xFFFEU));
    return gptp::SourcePortIdentity{
        gptp::ClockIdentity{
            0x00,
            0x00,
            0x00,
            0xFF,
            0xFE,
            static_cast<uint8_t>(pid >> 16U),
            static_cast<uint8_t>(pid >> 8U),
            static_cast<uint8_t>(pid)},
        port};
}

/// Whether @p error says ptp4l's socket is gone (it exited, or is restarting).
[[nodiscard]] auto server_gone(std::error_code const error) noexcept -> bool
{
    return error == std::errc::no_such_file_or_directory || error == std::errc::connection_refused;
}

}  // namespace

auto PtpManagementClient::create(PtpManagementConfig config, Listener listener) -> StatusValue<std::unique_ptr<PtpManagementClient>>
{
    auto const server = server_address(config);
    if (!server) {
        return forward_failure(server);
    }
    auto fd = bind_autobound();
    if (!fd) {
        return forward_failure(fd);
    }
    return std::unique_ptr<PtpManagementClient>{
        new PtpManagementClient{std::move(config), std::move(listener), std::move(*fd), *server, client_port_identity()}};
}

auto PtpManagementClient::send_as(
    ManagementAction const action,
    uint16_t const management_id,
    std::span<uint8_t const> const data,
    uint8_t const transport_specific) -> Status
{
    std::array<uint8_t, MANAGEMENT_SET_MAX_SIZE> message{};
    ManagementAddressing const addressing{
        .domain = config_.domain, .sequence_id = sequence_++, .source = source_, .transport_specific = transport_specific};
    size_t const length = build_management_message(message, action, management_id, data, addressing);
    if (length != 0 && ::sendto(fd_.get(), message.data(), length, 0, server_.sockaddr(), server_.length()) >= 0) {
        return success();
    }
    return failure(note_send_error(length == 0 ? std::make_error_code(std::errc::message_size) : net::errno_code()));
}

auto PtpManagementClient::note_send_error(std::error_code const error) -> std::error_code
{
    note_error(error);
    server_gone_ = server_gone_ || server_gone(error);
    return error;
}

void PtpManagementClient::on_ready(int64_t const now_ns)
{
    for (;;) {
        ssize_t const n = ::recv(fd_.get(), buffer_.data(), buffer_.size(), MSG_DONTWAIT);
        if (n >= 0) {
            handle(std::span<uint8_t const>{buffer_.data(), static_cast<size_t>(n)}, now_ns);
            continue;
        }
        int const error = errno;
        if (error == EINTR) {
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK) {
            // A dead socket would only spin the reactor: stop.
            finished_ = true;
            state_.error = net::errno_code(error);
            publish_if_changed();
        }
        return;
    }
}

#else

PtpManagementClient::~PtpManagementClient() = default;

auto PtpManagementClient::create(PtpManagementConfig /*config*/, Listener /*listener*/)
    -> StatusValue<std::unique_ptr<PtpManagementClient>>
{
    return failure(std::errc::not_supported);
}

auto PtpManagementClient::send_as(
    ManagementAction /*action*/, uint16_t /*management_id*/, std::span<uint8_t const> /*data*/, uint8_t /*transport_specific*/)
    -> Status
{
    return failure(std::errc::not_supported);
}

void PtpManagementClient::on_ready(int64_t /*now_ns*/)
{}

#endif  // __linux__

}  // namespace statusbar::ptpclient
