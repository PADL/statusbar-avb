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
constexpr std::array<DataSetSize, 3> DATA_SET_SIZES{{
    {.id = management_id::parent_data_set, .size = ParentDataSet::SIZE},
    {.id = management_id::current_data_set, .size = CurrentDataSet::SIZE},
    {.id = management_id::time_properties_data_set, .size = TimePropertiesDataSet::SIZE},
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

}  // namespace

auto build_management_get(
    std::span<uint8_t> const out,
    uint16_t const management_id,
    uint8_t const domain,
    uint16_t const sequence_id,
    gptp::SourcePortIdentity const& source) noexcept -> size_t
{
    if (out.size() < MANAGEMENT_GET_SIZE) {
        return 0;
    }
    gptp::MessageHeader header{};
    header.set_major_sdo_id(0);  // transportSpecific 0, as pmc
    header.set_message_type(MESSAGE_TYPE_MANAGEMENT);
    header.set_version_ptp(gptp::VERSION_PTP);
    header.message_length = static_cast<uint16_t>(MANAGEMENT_GET_SIZE);
    header.domain_number = domain;
    header.source_port_identity = source;
    header.sequence_id = sequence_id;
    header.control_field = CONTROL_MANAGEMENT;
    header.log_message_interval = LOG_INTERVAL_NONE;
    span_store(out, header);

    uint8_t* const management = out.data() + HEADER_SIZE;
    std::memset(management, 0xFF, 10);  // targetPortIdentity: every clock, every port
    management[10] = 0;                 // startingBoundaryHops (pmc -b 0)
    management[11] = 0;                 // boundaryHops
    management[12] = static_cast<uint8_t>(ManagementAction::get);
    management[13] = 0;
    uint8_t* const tlv = out.data() + TLV_OFFSET;  // MANAGEMENT TLV: the managementId alone
    put16(tlv, TLV_MANAGEMENT);
    put16(tlv + 2, 2);
    put16(tlv + 4, management_id);
    return MANAGEMENT_GET_SIZE;
}

auto parse_management_message(std::span<uint8_t const> const bytes) noexcept -> StatusValue<ManagementMessage>
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
    ManagementMessage message{
        .domain = header.domain_number,
        .sequence_id = header.sequence_id,
        .source = header.source_port_identity,
        .action = static_cast<ManagementAction>(bytes[HEADER_SIZE + 12] & 0x0FU),
    };
    uint8_t const* const tlv = bytes.data() + TLV_OFFSET;
    uint16_t const tlv_type = get16(tlv);
    size_t const tlv_length = get16(tlv + 2);
    if (tlv_length < 2 || TLV_OFFSET + TLV_HEADER_SIZE + tlv_length > length) {
        return bad_message();
    }
    uint8_t const* const value = tlv + TLV_HEADER_SIZE;
    if (tlv_type == TLV_MANAGEMENT_ERROR_STATUS) {
        // managementErrorId, managementId, reserved, displayData
        if (tlv_length < 4) {
            return bad_message();
        }
        message.error = get16(value);
        message.management_id = get16(value + 2);
        return message;
    }
    if (tlv_type != TLV_MANAGEMENT) {
        return bad_message();
    }
    message.management_id = get16(value);
    uint8_t const* const data = value + 2;
    size_t const data_length = tlv_length - 2;
    auto const* const known = std::ranges::find(DATA_SET_SIZES, message.management_id, &DataSetSize::id);
    if (known == DATA_SET_SIZES.end()) {
        return message;  // a data set not parsed here
    }
    if (data_length < known->size) {
        if (message.action == ManagementAction::response) {
            return bad_message();
        }
        return message;  // a GET carries no data
    }
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
        default:
            break;
    }
    return message;
}

// --- PtpManagementClient ---

PtpManagementClient::PtpManagementClient(
    PtpManagementConfig config, Listener listener, FileDescriptor fd, net::SocketAddress server, std::string local_path) noexcept
    : config_{std::move(config)}
    , listener_{std::move(listener)}
    , fd_{std::move(fd)}
    , server_{server}
    , local_path_{std::move(local_path)}
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
    // Any answer: ptp4l is there, in our domain.
    last_answer_ns_ = now_ns;
    state_.error = {};
    if (state_.status == PtpManagementStatus::unreachable) {
        state_.status = PtpManagementStatus::no_grandmaster;
    }
    if (auto const* parent = std::get_if<ParentDataSet>(&message->data)) {
        state_.parent = *parent;
        state_.grandmaster = parent->grandmaster_identity;
        bool const self = parent->grandmaster_identity.get() == message->source.clock_identity.get();
        state_.status = self ? PtpManagementStatus::no_grandmaster : PtpManagementStatus::locked;
    } else if (auto const* current = std::get_if<CurrentDataSet>(&message->data)) {
        state_.current = *current;
    } else if (auto const* properties = std::get_if<TimePropertiesDataSet>(&message->data)) {
        state_.time_properties = *properties;
    }
    publish_if_changed();
}

void PtpManagementClient::publish_if_changed()
{
    if (state_.status == reported_status_ && state_.grandmaster == reported_grandmaster_ && state_.error == reported_error_) {
        return;
    }
    reported_status_ = state_.status;
    reported_grandmaster_ = state_.grandmaster;
    reported_error_ = state_.error;
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

auto PtpManagementClient::create(PtpManagementConfig config, Listener listener) -> StatusValue<std::unique_ptr<PtpManagementClient>>
{
    if (config.period_ns <= 0 || config.missed_periods < 1) {
        return failure(std::errc::invalid_argument);
    }
    auto server = net::SocketAddress::local(config.uds_address);  // absolute, no NUL, fits
    if (!server) {
        return failure(std::errc::invalid_argument);
    }
    // Our socket beside ptp4l's, as pmc's: ptp4l answers to the sender's path.
    auto const slash = config.uds_address.rfind('/');
    auto const local_path =
        std::format("{}/statusbar-ptpmgmt.{}", std::string_view{config.uds_address}.substr(0, slash), ::getpid());
    auto local = net::SocketAddress::local(local_path);
    if (!local) {
        return failure(std::errc::invalid_argument);
    }
    FileDescriptor fd{::socket(AF_UNIX, SOCK_DGRAM, 0)};
    if (!fd.valid()) {
        return failure(std::error_code{errno, std::generic_category()});
    }
    if (auto status = net::set_nonblocking(fd.get()); !status) {
        return std::unexpected(status.error());
    }
    (void)::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    ::unlink(local_path.c_str());  // a stale socket from an earlier run of this pid
    if (::bind(fd.get(), local->sockaddr(), local->length()) != 0) {
        return failure(std::error_code{errno, std::generic_category()});
    }
    std::unique_ptr<PtpManagementClient> client{
        new PtpManagementClient{std::move(config), std::move(listener), std::move(fd), *server, local_path}};
    // Our port identity: unique to this process; ptp4l echoes it as the target.
    auto const pid = static_cast<uint32_t>(::getpid());
    client->source_ = gptp::SourcePortIdentity{
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
    return client;
}

void PtpManagementClient::query(int64_t const now_ns)
{
    for (uint16_t const id :
         {management_id::parent_data_set, management_id::current_data_set, management_id::time_properties_data_set}) {
        std::array<uint8_t, MANAGEMENT_GET_SIZE> message{};
        size_t const length = build_management_get(message, id, config_.domain, sequence_++, source_);
        if (::sendto(fd_.get(), message.data(), length, 0, server_.sockaddr(), server_.length()) < 0) {
            note_error(std::error_code{errno, std::generic_category()});  // ENOENT: no ptp4l; EACCES: its socket's mode
            break;
        }
    }
    if (!queried_) {
        last_answer_ns_ = now_ns;  // the silence timeout runs from the first query
        queried_ = true;
    }
    next_query_ns_ = now_ns + config_.period_ns;
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

void PtpManagementClient::on_ready(int64_t /*now_ns*/)
{}

#endif  // __linux__

}  // namespace statusbar::ptpclient
