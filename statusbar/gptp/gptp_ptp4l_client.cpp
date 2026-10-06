// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#include "statusbar/gptp/gptp_ptp4l_client.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/gptp/gptp_header.hpp"
#include "statusbar/net/net_posix_util.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <bit>
#include <cerrno>
#include <cstring>
#include <format>

#include <sys/socket.h>
#include <sys/un.h>

namespace statusbar::gptp {

namespace {

// IEEE 1588-2019 Clause 15.4: the management message after the 34-byte
// common header.
constexpr size_t MANAGEMENT_BODY_LENGTH = 14;  // targetPortIdentity(10) hops(2) action(1) reserved(1)
constexpr size_t TLV_HEADER_LENGTH = 6;        // tlvType(2) lengthField(2) managementId(2)
constexpr uint16_t TLV_TYPE_MANAGEMENT = 0x0001;
constexpr uint16_t TLV_TYPE_MANAGEMENT_ERROR_STATUS = 0x0002;
constexpr uint8_t ACTION_GET = 0;
constexpr uint8_t ACTION_RESPONSE = 2;
constexpr uint8_t CONTROL_FIELD_MANAGEMENT = 4;
constexpr uint8_t LOG_MESSAGE_INTERVAL_NONE = 0x7F;

/// Management message body (Figure 37): follows the common header.
struct ManagementBody
{
    SourcePortIdentity target_port_identity;
    octet_t starting_boundary_hops;
    octet_t boundary_hops;
    octet_t action;  ///< reserved(4) | actionField(4)
    octet_t reserved;
};
static_assert(sizeof(ManagementBody) == MANAGEMENT_BODY_LENGTH);

struct ManagementTlvHeader
{
    doublet_t tlv_type;
    doublet_t length_field;
    doublet_t management_id;
};
static_assert(sizeof(ManagementTlvHeader) == TLV_HEADER_LENGTH);

// Data set layouts (IEEE 1588-2019 15.5.3 and linuxptp's pmc_common.c).
struct DefaultDataSet
{
    octet_t flags;  ///< TSC bit 0, two-step bit 1
    octet_t reserved;
    doublet_t number_ports;
    octet_t priority1;
    ClockQuality clock_quality;
    octet_t priority2;
    ClockIdentity clock_identity;
    octet_t domain_number;
    octet_t reserved2;
};
static_assert(sizeof(DefaultDataSet) == 20);

struct CurrentDataSet
{
    doublet_t steps_removed;
    octlet_t offset_from_master;  ///< TimeInterval (ns << 16, signed)
    octlet_t mean_path_delay;     ///< TimeInterval
};
static_assert(sizeof(CurrentDataSet) == 18);

struct ParentDataSet
{
    SourcePortIdentity parent_port_identity;
    octet_t parent_stats;
    octet_t reserved;
    doublet_t observed_parent_offset_scaled_log_variance;
    quadlet_t observed_parent_clock_phase_change_rate;
    octet_t grandmaster_priority1;
    ClockQuality grandmaster_clock_quality;
    octet_t grandmaster_priority2;
    ClockIdentity grandmaster_identity;
};
static_assert(sizeof(ParentDataSet) == 32);

struct PortDataSet
{
    SourcePortIdentity port_identity;
    octet_t port_state;
    octet_t log_min_delay_req_interval;
    octlet_t peer_mean_path_delay;  ///< TimeInterval
    octet_t log_announce_interval;
    octet_t announce_receipt_timeout;
    octet_t log_sync_interval;
    octet_t delay_mechanism;
    octet_t log_min_pdelay_req_interval;
    octet_t version_number;
};
static_assert(sizeof(PortDataSet) == 26);

struct PortDataSetNp
{
    quadlet_t neighbor_prop_delay_thresh;
    quadlet_t as_capable;
};
static_assert(sizeof(PortDataSetNp) == 8);

struct TimeStatusNp
{
    octlet_t master_offset;  ///< Integer64 ns
    octlet_t ingress_time;   ///< Integer64 ns
    quadlet_t cumulative_scaled_rate_offset;
    quadlet_t scaled_last_gm_phase_change;
    doublet_t gm_time_base_indicator;
    doublet_t last_gm_phase_change_msb;  ///< scaled_ns.nanoseconds_msb
    octlet_t last_gm_phase_change_lsb;
    doublet_t last_gm_phase_change_fractional;
    quadlet_t gm_present;
    ClockIdentity gm_identity;
};
static_assert(sizeof(TimeStatusNp) == 50);

[[nodiscard]] auto time_interval_ns(uint64_t const raw) noexcept -> int64_t
{
    // TimeInterval: signed, nanoseconds * 2^16.
    return std::bit_cast<int64_t>(raw) >> 16;
}

[[nodiscard]] auto signed8(uint8_t const v) noexcept -> int8_t
{
    return std::bit_cast<int8_t>(v);
}

std::atomic<unsigned> g_client_counter{0};

}  // namespace

// Wire-struct serialization traits for the local layouts above.
}  // namespace statusbar::gptp

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::ManagementBody> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::ManagementTlvHeader> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::DefaultDataSet> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::CurrentDataSet> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::ParentDataSet> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::PortDataSet> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::PortDataSetNp> : std::true_type
{};
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::gptp::TimeStatusNp> : std::true_type
{};

namespace statusbar::gptp {

Ptp4lClient::Ptp4lClient(Config config)
    : config_{std::move(config)}
{}

Ptp4lClient::~Ptp4lClient()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (!local_path_.empty()) {
        ::unlink(local_path_.c_str());
    }
}

auto Ptp4lClient::open() -> Status
{
    if (fd_ >= 0) {
        return success();
    }
    local_path_ = std::format("{}.{}.{}", config_.local_path_prefix, ::getpid(), g_client_counter.fetch_add(1));
    sockaddr_un local{};
    if (local_path_.size() >= sizeof(local.sun_path)) {
        return failure(std::errc::filename_too_long);
    }
    int const fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return failure(std::error_code{errno, std::generic_category()});
    }
    ::unlink(local_path_.c_str());
    local.sun_family = AF_UNIX;
    std::strncpy(local.sun_path, local_path_.c_str(), sizeof(local.sun_path) - 1);
    if (::bind(fd, net::sockaddr_cast(local), sizeof(local)) < 0) {
        int const err = errno;
        ::close(fd);
        return failure(std::error_code{err, std::generic_category()});
    }
    if (::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        int const err = errno;
        ::close(fd);
        ::unlink(local_path_.c_str());
        return failure(std::error_code{err, std::generic_category()});
    }
    fd_ = fd;
    return success();
}

auto Ptp4lClient::encode_get(
    uint16_t const management_id,
    uint16_t const sequence_id,
    uint8_t const domain_number,
    uint8_t const transport_specific,
    std::span<uint8_t> out) -> size_t
{
    constexpr size_t LENGTH = MessageHeader::LENGTH + MANAGEMENT_BODY_LENGTH + TLV_HEADER_LENGTH;
    if (out.size() < LENGTH) {
        return 0;
    }
    MessageHeader header{};
    header.set_major_sdo_id(transport_specific);
    header.set_message_type(MESSAGE_TYPE_MANAGEMENT);
    header.set_version_ptp(2);
    header.message_length = doublet_t{static_cast<uint16_t>(LENGTH)};
    header.domain_number = octet_t{domain_number};
    header.sequence_id = doublet_t{sequence_id};
    header.control_field = octet_t{CONTROL_FIELD_MANAGEMENT};
    header.log_message_interval = octet_t{LOG_MESSAGE_INTERVAL_NONE};

    ManagementBody body{};
    body.target_port_identity = SourcePortIdentity{ClockIdentity{}.from_uint64(0xFFFFFFFFFFFFFFFFULL), 0xFFFF};
    body.starting_boundary_hops = octet_t{0};
    body.boundary_hops = octet_t{0};
    body.action = octet_t{ACTION_GET};

    ManagementTlvHeader tlv{};
    tlv.tlv_type = doublet_t{TLV_TYPE_MANAGEMENT};
    tlv.length_field = doublet_t{2};  // managementId only, no data on a GET
    tlv.management_id = doublet_t{management_id};

    span_store(out.first(MessageHeader::LENGTH), header);
    span_store(out.subspan(MessageHeader::LENGTH, MANAGEMENT_BODY_LENGTH), body);
    span_store(out.subspan(MessageHeader::LENGTH + MANAGEMENT_BODY_LENGTH, TLV_HEADER_LENGTH), tlv);
    return LENGTH;
}

auto Ptp4lClient::decode_response(std::span<uint8_t const> frame, Ptp4lStatus& status, int64_t const now_ns) -> uint16_t
{
    if (frame.size() < MessageHeader::LENGTH + MANAGEMENT_BODY_LENGTH + TLV_HEADER_LENGTH) {
        return 0;
    }
    MessageHeader header{};
    span_load(header, frame.first(MessageHeader::LENGTH));
    if (header.message_type() != MESSAGE_TYPE_MANAGEMENT) {
        return 0;
    }
    ManagementBody body{};
    span_load(body, frame.subspan(MessageHeader::LENGTH, MANAGEMENT_BODY_LENGTH));
    if ((body.action.get() & 0x0FU) != ACTION_RESPONSE) {
        return 0;
    }
    ManagementTlvHeader tlv{};
    span_load(tlv, frame.subspan(MessageHeader::LENGTH + MANAGEMENT_BODY_LENGTH, TLV_HEADER_LENGTH));
    if (tlv.tlv_type.get() == TLV_TYPE_MANAGEMENT_ERROR_STATUS || tlv.tlv_type.get() != TLV_TYPE_MANAGEMENT) {
        return 0;
    }
    size_t const data_offset = MessageHeader::LENGTH + MANAGEMENT_BODY_LENGTH + TLV_HEADER_LENGTH;
    size_t const length_field = tlv.length_field.get();
    if (length_field < 2 || data_offset + (length_field - 2) > frame.size()) {
        return 0;
    }
    auto const data = frame.subspan(data_offset, length_field - 2);
    auto const id = tlv.management_id.get();

    auto const fits = [&](size_t const n) { return data.size() >= n; };
    switch (id) {
        case ptp4l_management_id::DEFAULT_DATA_SET: {
            if (!fits(sizeof(DefaultDataSet))) {
                return 0;
            }
            DefaultDataSet ds{};
            span_load(ds, data.first(sizeof(DefaultDataSet)));
            status.clock_identity = ds.clock_identity;
            status.priority1 = ds.priority1.get();
            status.priority2 = ds.priority2.get();
            status.clock_class = ds.clock_quality.clock_class.get();
            status.clock_accuracy = ds.clock_quality.clock_accuracy.get();
            status.offset_scaled_log_variance = ds.clock_quality.offset_scaled_log_variance.get();
            status.domain_number = ds.domain_number.get();
            status.number_ports = ds.number_ports.get();
            status.have_default = true;
            break;
        }
        case ptp4l_management_id::CURRENT_DATA_SET: {
            if (!fits(sizeof(CurrentDataSet))) {
                return 0;
            }
            CurrentDataSet ds{};
            span_load(ds, data.first(sizeof(CurrentDataSet)));
            status.steps_removed = ds.steps_removed.get();
            status.offset_from_master_ns = time_interval_ns(ds.offset_from_master.get());
            status.mean_path_delay_ns = time_interval_ns(ds.mean_path_delay.get());
            status.have_current = true;
            break;
        }
        case ptp4l_management_id::PARENT_DATA_SET: {
            if (!fits(sizeof(ParentDataSet))) {
                return 0;
            }
            ParentDataSet ds{};
            span_load(ds, data.first(sizeof(ParentDataSet)));
            status.grandmaster_identity = ds.grandmaster_identity;
            status.gm_priority1 = ds.grandmaster_priority1.get();
            status.gm_priority2 = ds.grandmaster_priority2.get();
            status.gm_clock_class = ds.grandmaster_clock_quality.clock_class.get();
            status.gm_clock_accuracy = ds.grandmaster_clock_quality.clock_accuracy.get();
            status.gm_offset_scaled_log_variance = ds.grandmaster_clock_quality.offset_scaled_log_variance.get();
            status.have_parent = true;
            break;
        }
        case ptp4l_management_id::PORT_DATA_SET: {
            if (!fits(sizeof(PortDataSet))) {
                return 0;
            }
            PortDataSet ds{};
            span_load(ds, data.first(sizeof(PortDataSet)));
            // With several ports ptp4l answers once per port; keep the first.
            if (status.have_port && ds.port_identity.port_number.get() != status.port_number) {
                return id;
            }
            status.port_number = ds.port_identity.port_number.get();
            status.port_state = ds.port_state.get();
            status.peer_mean_path_delay_ns = time_interval_ns(ds.peer_mean_path_delay.get());
            status.log_announce_interval = signed8(ds.log_announce_interval.get());
            status.announce_receipt_timeout = ds.announce_receipt_timeout.get();
            status.log_sync_interval = signed8(ds.log_sync_interval.get());
            status.delay_mechanism = ds.delay_mechanism.get();
            status.log_min_pdelay_req_interval = signed8(ds.log_min_pdelay_req_interval.get());
            status.version_number = ds.version_number.get();
            status.have_port = true;
            break;
        }
        case ptp4l_management_id::PORT_DATA_SET_NP: {
            if (!fits(sizeof(PortDataSetNp))) {
                return 0;
            }
            PortDataSetNp ds{};
            span_load(ds, data.first(sizeof(PortDataSetNp)));
            status.neighbor_prop_delay_thresh = ds.neighbor_prop_delay_thresh.get();
            status.as_capable = ds.as_capable.get() != 0;
            status.have_port_np = true;
            break;
        }
        case ptp4l_management_id::TIME_STATUS_NP: {
            if (!fits(sizeof(TimeStatusNp))) {
                return 0;
            }
            TimeStatusNp ds{};
            span_load(ds, data.first(sizeof(TimeStatusNp)));
            status.master_offset_ns = std::bit_cast<int64_t>(ds.master_offset.get());
            status.gm_present = ds.gm_present.get() != 0;
            status.gm_time_base_indicator = ds.gm_time_base_indicator.get();
            if (!status.have_parent) {
                status.grandmaster_identity = ds.gm_identity;
            }
            status.have_time_status = true;
            break;
        }
        default:
            return 0;
    }
    status.sampled_ns = now_ns;
    return id;
}

void Ptp4lClient::request_now(int64_t const now_ns)
{
    if (fd_ < 0) {
        return;
    }
    sockaddr_un target{};
    target.sun_family = AF_UNIX;
    std::strncpy(target.sun_path, config_.uds_path.c_str(), sizeof(target.sun_path) - 1);
    std::array<uint8_t, 64> frame{};
    for (auto const id : REQUESTED_IDS) {
        auto const n = encode_get(id, sequence_id_++, config_.domain_number, config_.transport_specific, frame);
        if (n == 0) {
            continue;
        }
        // A missing ptp4l (ENOENT/ECONNREFUSED) just leaves the snapshot to go
        // stale; the next interval tries again.
        (void)::sendto(fd_, frame.data(), n, 0, net::sockaddr_cast(target), sizeof(target));
    }
    last_request_ns_ = now_ns;
}

void Ptp4lClient::on_ready(int64_t const now_ns)
{
    while (true) {
        auto const n = ::recv(fd_, rx_buf_.data(), rx_buf_.size(), 0);
        if (n <= 0) {
            break;
        }
        if (decode_response(std::span<uint8_t const>{rx_buf_.data(), static_cast<size_t>(n)}, status_, now_ns) != 0) {
            last_reply_ns_ = now_ns;
            batch_dirty_ = true;
        }
    }
    // Replies of one batch arrive back to back; announce once they have.
    if (batch_dirty_ && on_changed_ && exposed_fields_changed(status_, announced_)) {
        announced_ = status_;
        batch_dirty_ = false;
        on_changed_(status_);
    }
}

void Ptp4lClient::tick(int64_t const now_ns)
{
    if (fd_ < 0) {
        return;
    }
    if (last_request_ns_ == 0 || now_ns - last_request_ns_ >= config_.poll_interval_ns) {
        request_now(now_ns);
    }
    // Stale: ptp4l went away. Invalidate once so the entity falls back.
    if (status_.have_default && last_reply_ns_ != 0 &&
        now_ns - last_reply_ns_ > config_.poll_interval_ns * config_.stale_after_intervals) {
        status_ = Ptp4lStatus{};
        last_reply_ns_ = 0;
        if (on_changed_) {
            announced_ = status_;
            on_changed_(status_);
        }
    }
}

auto Ptp4lClient::exposed_fields_changed(Ptp4lStatus const& a, Ptp4lStatus const& b) noexcept -> bool
{
    return a.valid() != b.valid() || a.clock_identity != b.clock_identity || a.priority1 != b.priority1 ||
        a.priority2 != b.priority2 || a.domain_number != b.domain_number || a.clock_class != b.clock_class ||
        a.clock_accuracy != b.clock_accuracy || a.offset_scaled_log_variance != b.offset_scaled_log_variance ||
        a.grandmaster_identity != b.grandmaster_identity || a.gm_priority1 != b.gm_priority1 || a.gm_priority2 != b.gm_priority2 ||
        a.port_state != b.port_state || a.as_capable != b.as_capable || a.gm_present != b.gm_present ||
        a.log_announce_interval != b.log_announce_interval || a.log_sync_interval != b.log_sync_interval ||
        a.log_min_pdelay_req_interval != b.log_min_pdelay_req_interval || a.peer_mean_path_delay_ns != b.peer_mean_path_delay_ns ||
        a.steps_removed != b.steps_removed;
}

}  // namespace statusbar::gptp
