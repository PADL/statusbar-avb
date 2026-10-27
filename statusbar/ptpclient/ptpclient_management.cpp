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
#include <cerrno>
#include <cstring>
#include <format>
#include <utility>

namespace statusbar::ptpclient {

namespace {

constexpr size_t HEADER_SIZE = gptp::MessageHeader::LENGTH;  ///< common header (13.3)
constexpr size_t MANAGEMENT_HEADER_SIZE = 14;                ///< targetPortIdentity .. reserved (15.4.1)
constexpr size_t TLV_HEADER_SIZE = 4;                        ///< tlvType, lengthField
constexpr size_t TLV_OFFSET = HEADER_SIZE + MANAGEMENT_HEADER_SIZE;
constexpr uint8_t MESSAGE_TYPE_MANAGEMENT = 0x0D;
constexpr uint8_t CONTROL_MANAGEMENT = 0x04;
constexpr uint8_t LOG_INTERVAL_NONE = 0x7F;
constexpr uint16_t TLV_MANAGEMENT = 0x0001;
constexpr uint16_t TLV_MANAGEMENT_ERROR_STATUS = 0x0002;

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

void put16(uint8_t* p, uint16_t v) noexcept
{
    p[0] = static_cast<uint8_t>(v >> 8U);
    p[1] = static_cast<uint8_t>(v);
}

[[nodiscard]] auto get16(uint8_t const* p) noexcept -> uint16_t
{
    return static_cast<uint16_t>((uint16_t{p[0]} << 8U) | p[1]);
}

[[nodiscard]] auto get32(uint8_t const* p) noexcept -> uint32_t
{
    return (uint32_t{p[0]} << 24U) | (uint32_t{p[1]} << 16U) | (uint32_t{p[2]} << 8U) | p[3];
}

[[nodiscard]] auto get64(uint8_t const* p) noexcept -> uint64_t
{
    return (uint64_t{get32(p)} << 32U) | get32(p + 4);
}

template <typename T>
[[nodiscard]] auto load(uint8_t const* p) noexcept -> T
{
    T value{};
    span_load(value, std::span<uint8_t const>{p, sizeof(T)});
    return value;
}

[[nodiscard]] auto bad_message() noexcept -> std::unexpected<std::error_code>
{
    return failure(std::errc::bad_message);
}

/// The common header of a management message (Clause 13.3) for one of @p length octets.
void store_ptp_header(
    std::span<uint8_t> const out,
    size_t const length,
    uint8_t const domain,
    uint16_t const sequence_id,
    gptp::SourcePortIdentity const& source) noexcept
{  // NOLINT(statusbar-block-statements): one header field per statement
    gptp::MessageHeader header{};
    header.set_major_sdo_id(0);  // transportSpecific 0, as pmc
    header.set_message_type(MESSAGE_TYPE_MANAGEMENT);
    header.set_version_ptp(gptp::VERSION_PTP);
    header.message_length = static_cast<uint16_t>(length);
    header.domain_number = domain;
    header.source_port_identity = source;
    header.sequence_id = sequence_id;
    header.control_field = CONTROL_MANAGEMENT;
    header.log_message_interval = LOG_INTERVAL_NONE;
    span_store(out, header);
}

/// The management header (Clause 15.4.1) and the MANAGEMENT TLV's header and managementId
/// of a message of @p length octets.
void store_management_header(
    std::span<uint8_t> const out, ManagementAction const action, uint16_t const management_id, size_t const length) noexcept
{
    // targetPortIdentity: every clock, every port; startingBoundaryHops (pmc -b 0), boundaryHops,
    // actionField, reserved
    std::array<uint8_t, MANAGEMENT_HEADER_SIZE> const fields{
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, static_cast<uint8_t>(action), 0};
    std::ranges::copy(fields, out.begin() + HEADER_SIZE);
    uint8_t* const tlv = out.data() + TLV_OFFSET;
    put16(tlv, TLV_MANAGEMENT);
    put16(tlv + 2, static_cast<uint16_t>(2 + length - MANAGEMENT_GET_SIZE));
    put16(tlv + 4, management_id);
}

/// @p data at the front of @p field, the rest of it zero (the pad octet).
void store_padded(std::span<uint8_t> const field, std::span<uint8_t const> const data) noexcept
{
    std::ranges::copy(data, field.begin());
    std::ranges::fill(field.subspan(data.size()), uint8_t{0});
}

/// One TLV: its type and its value (lengthField octets).
struct Tlv
{
    uint16_t type;
    std::span<uint8_t const> value;
};

/// The common header of the management message @p bytes, checked: a PTPv2 management
/// message whose messageLength holds the TLV header and fits @p bytes.
[[nodiscard]] auto parse_header(std::span<uint8_t const> const bytes) noexcept -> StatusValue<gptp::MessageHeader>
{
    if (bytes.size() < TLV_OFFSET + TLV_HEADER_SIZE) {
        return bad_message();
    }
    auto const header = load<gptp::MessageHeader>(bytes.data());
    if (header.message_type() != MESSAGE_TYPE_MANAGEMENT || header.version_ptp() != gptp::VERSION_PTP) {
        return bad_message();
    }
    size_t const length = header.message_length;
    if (length < TLV_OFFSET + TLV_HEADER_SIZE || length > bytes.size()) {
        return bad_message();
    }
    return header;
}

/// The TLV of the management message @p message (its messageLength octets), with room for a managementId.
[[nodiscard]] auto parse_tlv(std::span<uint8_t const> const message) noexcept -> StatusValue<Tlv>
{
    uint8_t const* const tlv = message.data() + TLV_OFFSET;
    size_t const tlv_length = get16(tlv + 2);
    if (tlv_length < 2 || TLV_OFFSET + TLV_HEADER_SIZE + tlv_length > message.size()) {
        return bad_message();
    }
    return Tlv{.type = get16(tlv), .value = message.subspan(TLV_OFFSET + TLV_HEADER_SIZE, tlv_length)};
}

/// PORT_PROPERTIES_NP's @p data (@p data_length octets, at least PortPropertiesNp::SIZE).
[[nodiscard]] auto parse_port_properties(uint8_t const* const data, size_t const data_length) noexcept
    -> StatusValue<PortPropertiesNp>
{
    size_t const text_length = data[12];
    if (PortPropertiesNp::SIZE + text_length > data_length) {
        return bad_message();
    }
    PortPropertiesNp properties{
        .port_identity = load<gptp::SourcePortIdentity>(data),
        .port_state = static_cast<PortState>(data[10]),
        .timestamping = data[11],
    };
    properties.interface_length = static_cast<uint8_t>(std::min(text_length, properties.interface.size()));
    std::ranges::transform(
        std::span<uint8_t const>{data + PortPropertiesNp::SIZE, properties.interface_length},
        properties.interface.begin(),
        [](uint8_t const c) { return static_cast<char>(c); });
    return properties;
}

/// @p message with the data set its managementId names decoded from @p data (@p data_length
/// octets, at least the data set's size).
[[nodiscard]] auto decode_data_set(ManagementMessage message, uint8_t const* const data, size_t const data_length) noexcept
    -> StatusValue<ManagementMessage>
{
    switch (message.management_id) {
        case management_id::parent_data_set:
            message.data = ParentDataSet{
                .parent_port_identity = load<gptp::SourcePortIdentity>(data),
                .parent_stats = (data[10] & 0x01U) != 0,
                .observed_parent_offset_scaled_log_variance = get16(data + 12),
                .observed_parent_clock_phase_change_rate = static_cast<int32_t>(get32(data + 14)),
                .grandmaster_priority1 = data[18],
                .grandmaster_clock_class = data[19],
                .grandmaster_clock_accuracy = data[20],
                .grandmaster_offset_scaled_log_variance = get16(data + 21),
                .grandmaster_priority2 = data[23],
                .grandmaster_identity = load<ieee::Eui64>(data + 24),
            };
            break;
        case management_id::current_data_set:
            message.data = CurrentDataSet{
                .steps_removed = get16(data),
                .offset_from_master_scaled = static_cast<int64_t>(get64(data + 2)),
                .mean_path_delay_scaled = static_cast<int64_t>(get64(data + 10)),
            };
            break;
        case management_id::time_properties_data_set:
            message.data = TimePropertiesDataSet{
                .current_utc_offset = static_cast<int16_t>(get16(data)),
                .flags = data[2],
                .time_source = data[3],
            };
            break;
        case management_id::port_data_set:
            message.data = PortDataSet{
                .port_identity = load<gptp::SourcePortIdentity>(data),
                .port_state = static_cast<PortState>(data[10]),
                .log_min_delay_req_interval = static_cast<int8_t>(data[11]),
                .peer_mean_path_delay_scaled = static_cast<int64_t>(get64(data + 12)),
                .log_announce_interval = static_cast<int8_t>(data[20]),
                .announce_receipt_timeout = data[21],
                .log_sync_interval = static_cast<int8_t>(data[22]),
                .delay_mechanism = static_cast<DelayMechanism>(data[23]),
                .log_min_pdelay_req_interval = static_cast<int8_t>(data[24]),
                .version_number = data[25],
            };
            break;
        case management_id::port_properties_np: {
            auto const properties = parse_port_properties(data, data_length);
            if (!properties) {
                return forward_failure(properties);
            }
            message.data = *properties;
            break;
        }
        case management_id::time_status_np:
            message.data = TimeStatusNp{
                .master_offset_ns = static_cast<int64_t>(get64(data)),
                .ingress_time_ns = static_cast<int64_t>(get64(data + 8)),
                .cumulative_scaled_rate_offset = static_cast<int32_t>(get32(data + 16)),
                .scaled_last_gm_phase_change = static_cast<int32_t>(get32(data + 20)),
                .gm_time_base_indicator = get16(data + 24),
                .gm_present = get32(data + 38) != 0,  // after lastGmPhaseChange (ScaledNs, 12 octets)
                .gm_identity = load<ieee::Eui64>(data + 42),
            };
            break;
        case management_id::grandmaster_settings_np:
            message.data = GrandmasterSettingsNp{
                .clock_class = data[0],
                .clock_accuracy = data[1],
                .offset_scaled_log_variance = get16(data + 2),
                .current_utc_offset = static_cast<int16_t>(get16(data + 4)),
                .time_flags = data[6],
                .time_source = data[7],
            };
            break;
        case management_id::subscribe_events_np: {
            SubscribeEventsNp subscription{.duration_s = get16(data)};
            std::ranges::copy(std::span<uint8_t const>{data + 2, subscription.bitmask.size()}, subscription.bitmask.begin());
            message.data = subscription;
            break;
        }
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
    message.management_id = get16(value.data());
    auto const* const known = std::ranges::find(DATA_SET_SIZES, message.management_id, &DataSetSize::id);
    if (known == DATA_SET_SIZES.end()) {
        return message;  // a data set not parsed here
    }
    if (value.size() - 2 < known->size) {
        if (message.action == ManagementAction::response) {
            return bad_message();
        }
        return message;  // a GET carries no data
    }
    return decode_data_set(message, value.data() + 2, value.size() - 2);
}

/// @p message with @p tlv: a MANAGEMENT_ERROR_STATUS's error, or a MANAGEMENT TLV's
/// managementId and data.
[[nodiscard]] auto parse_tlv_value(ManagementMessage message, Tlv const& tlv) noexcept -> StatusValue<ManagementMessage>
{
    if (tlv.type == TLV_MANAGEMENT_ERROR_STATUS) {
        // managementErrorId, managementId, reserved, displayData
        if (tlv.value.size() < 4) {
            return bad_message();
        }
        message.error = get16(tlv.value.data());
        message.management_id = get16(tlv.value.data() + 2);
        return message;
    }
    if (tlv.type != TLV_MANAGEMENT) {
        return bad_message();
    }
    return parse_management_tlv(message, tlv.value);
}

}  // namespace

auto build_management_get(
    std::span<uint8_t> const out,
    uint16_t const management_id,
    uint8_t const domain,
    uint16_t const sequence_id,
    gptp::SourcePortIdentity const& source) noexcept -> size_t
{
    return build_management_message(out, ManagementAction::get, management_id, {}, domain, sequence_id, source);
}

auto build_management_message(
    std::span<uint8_t> const out,
    ManagementAction const action,
    uint16_t const management_id,
    std::span<uint8_t const> const data,
    uint8_t const domain,
    uint16_t const sequence_id,
    gptp::SourcePortIdentity const& source) noexcept -> size_t
{
    size_t const length = MANAGEMENT_GET_SIZE + data.size() + (data.size() % 2);
    if (out.size() < length || length > UINT16_MAX) {
        return 0;
    }
    store_ptp_header(out, length, domain, sequence_id, source);
    store_management_header(out, action, management_id, length);
    store_padded(out.subspan(MANAGEMENT_GET_SIZE, length - MANAGEMENT_GET_SIZE), data);
    return length;
}

auto encode(GrandmasterSettingsNp const& settings) noexcept -> std::array<uint8_t, GrandmasterSettingsNp::SIZE>
{  // NOLINT(statusbar-block-statements): one field per statement, in wire order
    std::array<uint8_t, GrandmasterSettingsNp::SIZE> out{};
    out[0] = settings.clock_class;
    out[1] = settings.clock_accuracy;
    put16(&out[2], settings.offset_scaled_log_variance);
    put16(&out[4], static_cast<uint16_t>(settings.current_utc_offset));
    out[6] = settings.time_flags;
    out[7] = settings.time_source;
    return out;
}

auto encode(SubscribeEventsNp const& subscription) noexcept -> std::array<uint8_t, SubscribeEventsNp::SIZE>
{
    std::array<uint8_t, SubscribeEventsNp::SIZE> out{};
    put16(out.data(), subscription.duration_s);
    std::ranges::copy(subscription.bitmask, out.begin() + 2);
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
    ManagementMessage const message{
        .domain = header->domain_number,
        .sequence_id = header->sequence_id,
        .source = header->source_port_identity,
        .action = static_cast<ManagementAction>(bytes[HEADER_SIZE + 12] & 0x0FU),
    };
    return parse_tlv_value(message, *tlv);
}

// --- PtpManagementClient ---

PtpManagementClient::PtpManagementClient(
    PtpManagementConfig config,
    Listener listener,
    FileDescriptor fd,
    net::SocketAddress server,
    std::string local_path,
    gptp::SourcePortIdentity const& source) noexcept
    : config_{std::move(config)}
    , listener_{std::move(listener)}
    , fd_{std::move(fd)}
    , server_{server}
    , local_path_{std::move(local_path)}
    , source_{source}
{}

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
    note_answer(now_ns);
    apply(*message, now_ns);
    publish_if_changed();
}

void PtpManagementClient::note_answer(int64_t const now_ns)
{
    // Any answer: ptp4l is there, in our domain.
    last_answer_ns_ = now_ns;
    state_.error = {};
    if (state_.status == PtpManagementStatus::unreachable) {
        state_.status = PtpManagementStatus::no_grandmaster;
        send_settings();  // a restarted ptp4l has forgotten both
        send_subscription(now_ns);
    }
}

void PtpManagementClient::apply(ManagementMessage const& message, int64_t const now_ns)
{
    bool const ours = message.management_id == management_id::grandmaster_settings_np ||
        message.management_id == management_id::subscribe_events_np;
    if (message.error) {
        if (ours) {
            state_.refused = message.error;
        }
    } else if (auto const* parent = std::get_if<ParentDataSet>(&message.data)) {
        state_.parent = *parent;
        state_.grandmaster = parent->grandmaster_identity;
        bool const self = parent->grandmaster_identity.get() == message.source.clock_identity.get();
        state_.status = self ? PtpManagementStatus::no_grandmaster : PtpManagementStatus::locked;
    } else if (auto const* current = std::get_if<CurrentDataSet>(&message.data)) {
        state_.current = *current;
    } else if (auto const* properties = std::get_if<TimePropertiesDataSet>(&message.data)) {
        state_.time_properties = *properties;
    } else if (auto const* port = std::get_if<PortPropertiesNp>(&message.data)) {
        if (!config_.port_interface.empty() && port->interface_name() == config_.port_interface) {
            state_.port = port->port_identity;
        }
    } else if (auto const* port_data = std::get_if<PortDataSet>(&message.data)) {
        if (state_.port == port_data->port_identity) {
            state_.peer_mean_path_delay_ns = time_interval_ns(port_data->peer_mean_path_delay_scaled);
            state_.port_state = port_data->port_state;
            state_.delay_mechanism = port_data->delay_mechanism;
        }
    } else if (auto const* time_status = std::get_if<TimeStatusNp>(&message.data)) {
        state_.time_status = *time_status;
    } else if (auto const* settings = std::get_if<GrandmasterSettingsNp>(&message.data)) {
        state_.grandmaster_settings = *settings;
        if (settings_ && *settings != *settings_) {
            send_settings();  // ptp4l restarted between two queries
        } else {
            state_.refused.reset();
        }
    } else if (auto const* subscription = std::get_if<SubscribeEventsNp>(&message.data)) {
        state_.subscribed = subscription->bitmask == config_.events.bitmask;
        if (state_.subscribed) {
            state_.refused.reset();
        } else {
            send_subscription(now_ns);
        }
    }
}

void PtpManagementClient::publish_if_changed()
{  // NOLINT(statusbar-block-statements): one reported field per statement
    if (state_.status == reported_status_ && state_.grandmaster == reported_grandmaster_ && state_.error == reported_error_ &&
        state_.peer_mean_path_delay_ns == reported_peer_delay_ns_ && state_.port_state == reported_port_state_ &&
        state_.delay_mechanism == reported_delay_mechanism_ && state_.grandmaster_settings == reported_settings_ &&
        state_.refused == reported_refused_ && state_.subscribed == reported_subscribed_) {
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
    if (!queried_ || now_ns >= next_query_ns_) {
        query(now_ns);
    }
    // Silent too long: forget everything learnt, so state() never mixes
    // "unreachable" with stale data sets.
    if (state_.status != PtpManagementStatus::unreachable &&
        now_ns - last_answer_ns_ > config_.period_ns * config_.missed_periods) {
        std::error_code const error = state_.error;
        state_ = PtpManagementState{.error = error};
        next_subscribe_ns_ = now_ns;  // resubscribe as soon as ptp4l is back
        publish_if_changed();
    }
}

#if defined(__linux__)

PtpManagementClient::~PtpManagementClient()
{
    if (fd_.valid() && !local_path_.empty()) {
        ::unlink(local_path_.c_str());
    }
}

namespace {

/// The address of ptp4l's socket in @p config, checking the rest of @p config.
[[nodiscard]] auto server_address(PtpManagementConfig const& config) -> StatusValue<net::SocketAddress>
{
    if (config.period_ns <= 0 || config.missed_periods < 1 || config.subscription_duration_s < 2) {
        return failure(std::errc::invalid_argument);
    }
    auto server = net::SocketAddress::local(config.uds_address);  // absolute, no NUL, fits
    if (!server) {
        return failure(std::errc::invalid_argument);
    }
    return *server;
}

/// Our socket's path beside ptp4l's at @p uds_address, as pmc's: ptp4l answers to the sender's path.
[[nodiscard]] auto local_path_beside(std::string_view const uds_address) -> std::string
{
    auto const slash = uds_address.rfind('/');
    return std::format("{}/statusbar-ptpmgmt.{}", uds_address.substr(0, slash), ::getpid());
}

/// A non-blocking, close-on-exec datagram socket.
[[nodiscard]] auto local_socket() -> StatusValue<FileDescriptor>
{
    FileDescriptor fd{::socket(AF_UNIX, SOCK_DGRAM, 0)};
    if (!fd.valid()) {
        return failure(std::error_code{errno, std::generic_category()});
    }
    if (auto status = net::set_nonblocking(fd.get()); !status) {
        return std::unexpected(status.error());
    }
    (void)::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    return fd;
}

/// A local socket bound at @p path, replacing a stale socket from an earlier run of this pid.
[[nodiscard]] auto bind_local(std::string const& path) -> StatusValue<FileDescriptor>
{
    auto local = net::SocketAddress::local(path);
    if (!local) {
        return failure(std::errc::invalid_argument);
    }
    return local_socket().and_then([&](FileDescriptor fd) -> StatusValue<FileDescriptor> {
        ::unlink(path.c_str());
        if (::bind(fd.get(), local->sockaddr(), local->length()) != 0) {
            return failure(std::error_code{errno, std::generic_category()});
        }
        return fd;
    });
}

/// Our port identity: unique to this process; ptp4l echoes it as the target.
[[nodiscard]] auto process_port_identity() -> gptp::SourcePortIdentity
{
    auto const pid = static_cast<uint32_t>(::getpid());
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
        1};
}

}  // namespace

auto PtpManagementClient::create(PtpManagementConfig config, Listener listener) -> StatusValue<std::unique_ptr<PtpManagementClient>>
{
    auto const server = server_address(config);
    if (!server) {
        return forward_failure(server);
    }
    auto const local_path = local_path_beside(config.uds_address);
    auto fd = bind_local(local_path);
    if (!fd) {
        return forward_failure(fd);
    }
    return std::unique_ptr<PtpManagementClient>{new PtpManagementClient{
        std::move(config), std::move(listener), std::move(*fd), *server, local_path, process_port_identity()}};
}

void PtpManagementClient::query(int64_t const now_ns)
{
    // PORT_PROPERTIES_NP before PORT_DATA_SET: ptp4l answers in order, so the port is known first.
    std::array<uint16_t, 5> const ids{
        management_id::parent_data_set,
        management_id::current_data_set,
        management_id::time_properties_data_set,
        management_id::port_properties_np,
        management_id::port_data_set};
    for (uint16_t const id : std::span{ids}.first(config_.port_interface.empty() ? 3 : 5)) {
        if (!send(ManagementAction::get, id, {})) {
            break;  // ENOENT: no ptp4l; EACCES: its socket's mode
        }
    }
    // ptp4l answers these GETs with what it holds: a restart shows as a difference.
    if (settings_) {
        (void)send(ManagementAction::get, management_id::grandmaster_settings_np, {});
    }
    if (subscribing()) {
        if (now_ns >= next_subscribe_ns_) {
            send_subscription(now_ns);
        }
        (void)send(ManagementAction::get, management_id::subscribe_events_np, {});
    }
    if (!queried_) {
        last_answer_ns_ = now_ns;  // the silence timeout runs from the first query
        queried_ = true;
    }
    next_query_ns_ = now_ns + config_.period_ns;
}

auto PtpManagementClient::send(ManagementAction const action, uint16_t const management_id, std::span<uint8_t const> const data)
    -> Status
{
    std::array<uint8_t, MANAGEMENT_SET_MAX_SIZE> message{};
    size_t const length = build_management_message(message, action, management_id, data, config_.domain, sequence_++, source_);
    if (length == 0) {
        return failure(std::errc::message_size);
    }
    if (::sendto(fd_.get(), message.data(), length, 0, server_.sockaddr(), server_.length()) < 0) {
        std::error_code const error{errno, std::generic_category()};
        note_error(error);
        return failure(error);
    }
    return success();
}

auto PtpManagementClient::set_grandmaster_settings(GrandmasterSettingsNp const& settings) -> Status
{
    settings_ = settings;
    return send(ManagementAction::set, management_id::grandmaster_settings_np, encode(settings));
}

void PtpManagementClient::send_settings()
{
    if (settings_) {
        (void)send(ManagementAction::set, management_id::grandmaster_settings_np, encode(*settings_));
    }
}

auto PtpManagementClient::subscribing() const noexcept -> bool
{
    return config_.events.bitmask != SubscribeEventsNp{}.bitmask;
}

void PtpManagementClient::send_subscription(int64_t const now_ns)
{
    if (!subscribing()) {
        return;
    }
    SubscribeEventsNp subscription = config_.events;
    subscription.duration_s = config_.subscription_duration_s;
    if (send(ManagementAction::set, management_id::subscribe_events_np, encode(subscription))) {
        next_subscribe_ns_ = now_ns + (int64_t{config_.subscription_duration_s} * 500'000'000);
    }
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
            state_.error = std::error_code{error, std::generic_category()};
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

void PtpManagementClient::query(int64_t /*now_ns*/)
{}

auto PtpManagementClient::send(ManagementAction /*action*/, uint16_t /*management_id*/, std::span<uint8_t const> /*data*/) -> Status
{
    return failure(std::errc::not_supported);
}

auto PtpManagementClient::set_grandmaster_settings(GrandmasterSettingsNp const& /*settings*/) -> Status
{
    return failure(std::errc::not_supported);
}

void PtpManagementClient::send_settings()
{}

auto PtpManagementClient::subscribing() const noexcept -> bool
{
    return false;
}

void PtpManagementClient::send_subscription(int64_t /*now_ns*/)
{}

void PtpManagementClient::on_ready(int64_t /*now_ns*/)
{}

#endif  // __linux__

}  // namespace statusbar::ptpclient
