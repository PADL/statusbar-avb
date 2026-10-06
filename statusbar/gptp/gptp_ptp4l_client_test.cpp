// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#include "statusbar/gptp/gptp_ptp4l_client.hpp"

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/gptp/gptp_header.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/test/test.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>

using namespace statusbar;
using namespace statusbar::gptp;

namespace {

/// Build a management RESPONSE frame for @p management_id carrying @p data.
auto make_response(uint16_t const management_id, std::span<uint8_t const> data, uint16_t const sequence = 1) -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame(MessageHeader::LENGTH + 14 + 6 + data.size(), 0);
    MessageHeader header{};
    header.set_message_type(MESSAGE_TYPE_MANAGEMENT);
    header.set_version_ptp(2);
    header.message_length = doublet_t{static_cast<uint16_t>(frame.size())};
    header.sequence_id = doublet_t{sequence};
    header.control_field = octet_t{4};
    header.log_message_interval = octet_t{0x7F};
    span_store(std::span<uint8_t>{frame}.first(MessageHeader::LENGTH), header);
    size_t at = MessageHeader::LENGTH;
    // targetPortIdentity (10) + hops (2)
    at += 12;
    frame[at++] = 0x02;  // actionField RESPONSE
    frame[at++] = 0;
    frame[at++] = 0x00;  // tlvType MANAGEMENT
    frame[at++] = 0x01;
    auto const length_field = static_cast<uint16_t>(2 + data.size());
    frame[at++] = static_cast<uint8_t>(length_field >> 8U);
    frame[at++] = static_cast<uint8_t>(length_field);
    frame[at++] = static_cast<uint8_t>(management_id >> 8U);
    frame[at++] = static_cast<uint8_t>(management_id);
    std::copy(data.begin(), data.end(), frame.begin() + static_cast<std::ptrdiff_t>(at));
    return frame;
}

auto be16(std::vector<uint8_t>& v, uint16_t const x) -> void
{
    v.push_back(static_cast<uint8_t>(x >> 8U));
    v.push_back(static_cast<uint8_t>(x));
}
auto be32(std::vector<uint8_t>& v, uint32_t const x) -> void
{
    be16(v, static_cast<uint16_t>(x >> 16U));
    be16(v, static_cast<uint16_t>(x));
}
auto be64(std::vector<uint8_t>& v, uint64_t const x) -> void
{
    be32(v, static_cast<uint32_t>(x >> 32U));
    be32(v, static_cast<uint32_t>(x));
}

std::array<uint8_t, 8> const kOwnClock{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0xC3, 0x01};
std::array<uint8_t, 8> const kGmClock{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04};

auto default_data_set() -> std::vector<uint8_t>
{
    std::vector<uint8_t> d;
    d.push_back(0x01);  // flags: TSC
    d.push_back(0);
    be16(d, 1);         // numberPorts
    d.push_back(128);   // priority1
    d.push_back(248);   // clockClass
    d.push_back(0xFE);  // clockAccuracy
    be16(d, 0xFFFF);    // offsetScaledLogVariance
    d.push_back(128);   // priority2
    d.insert(d.end(), kOwnClock.begin(), kOwnClock.end());
    d.push_back(0);  // domainNumber
    d.push_back(0);
    return d;
}

auto parent_data_set() -> std::vector<uint8_t>
{
    std::vector<uint8_t> d;
    d.insert(d.end(), kGmClock.begin(), kGmClock.end());  // parentPortIdentity.clockIdentity
    be16(d, 1);                                           // parent port number
    d.push_back(0);                                       // parentStats
    d.push_back(0);
    be16(d, 0xFFFF);  // observedParentOffsetScaledLogVariance
    be32(d, 0x7FFFFFFF);
    d.push_back(246);  // grandmasterPriority1
    d.push_back(6);    // gm clockClass
    d.push_back(0x21);
    be16(d, 0x4100);
    d.push_back(248);  // grandmasterPriority2
    d.insert(d.end(), kGmClock.begin(), kGmClock.end());
    return d;
}

auto port_data_set(uint16_t const port, int64_t const peer_delay_ns) -> std::vector<uint8_t>
{
    std::vector<uint8_t> d;
    d.insert(d.end(), kOwnClock.begin(), kOwnClock.end());
    be16(d, port);
    d.push_back(ptp4l_port_state::SLAVE);
    d.push_back(0);                                              // logMinDelayReqInterval
    be64(d, static_cast<uint64_t>(peer_delay_ns) << 16U);        // peerMeanPathDelay (TimeInterval)
    d.push_back(0);                                              // logAnnounceInterval
    d.push_back(3);                                              // announceReceiptTimeout
    d.push_back(static_cast<uint8_t>(static_cast<int8_t>(-3)));  // logSyncInterval
    d.push_back(2);                                              // delayMechanism P2P
    d.push_back(0);                                              // logMinPdelayReqInterval
    d.push_back(2);                                              // versionNumber
    return d;
}

auto port_data_set_np(bool const as_capable) -> std::vector<uint8_t>
{
    std::vector<uint8_t> d;
    be32(d, 800);
    be32(d, as_capable ? 1U : 0U);
    return d;
}

auto time_status_np(bool const gm_present, int64_t const master_offset) -> std::vector<uint8_t>
{
    std::vector<uint8_t> d;
    be64(d, static_cast<uint64_t>(master_offset));
    be64(d, 0);  // ingress_time
    be32(d, 0);  // cumulativeScaledRateOffset
    be32(d, 0);  // scaledLastGmPhaseChange
    be16(d, 7);  // gmTimeBaseIndicator
    be16(d, 0);  // lastGmPhaseChange msb
    be64(d, 0);  // lsb
    be16(d, 0);  // fractional
    be32(d, gm_present ? 1U : 0U);
    d.insert(d.end(), kGmClock.begin(), kGmClock.end());
    return d;
}

}  // namespace

TEST(gptp_ptp4l_client, encode_get_matches_clause_15_layout)
{
    std::array<uint8_t, 64> frame{};
    auto const n = Ptp4lClient::encode_get(ptp4l_management_id::PORT_DATA_SET, 0x1234, 5, 1, frame);
    EXPECT_EQ(n, size_t{54});
    EXPECT_EQ(frame[0], 0x1D);  // majorSdoId 1 | MANAGEMENT
    EXPECT_EQ(frame[1], 0x02);  // versionPTP 2
    EXPECT_EQ(frame[2], 0x00);
    EXPECT_EQ(frame[3], 54);
    EXPECT_EQ(frame[4], 5);      // domain
    EXPECT_EQ(frame[30], 0x12);  // sequenceId
    EXPECT_EQ(frame[31], 0x34);
    EXPECT_EQ(frame[32], 4);     // controlField MANAGEMENT
    EXPECT_EQ(frame[33], 0x7F);  // logMessageInterval
    for (size_t i = 34; i < 44; ++i) {
        EXPECT_EQ(frame[i], 0xFF);  // targetPortIdentity: all ports of all clocks
    }
    EXPECT_EQ(frame[46], 0x00);  // actionField GET
    EXPECT_EQ(frame[48], 0x00);  // tlvType MANAGEMENT
    EXPECT_EQ(frame[49], 0x01);
    EXPECT_EQ(frame[50], 0x00);  // lengthField 2
    EXPECT_EQ(frame[51], 0x02);
    EXPECT_EQ(frame[52], 0x20);  // managementId PORT_DATA_SET
    EXPECT_EQ(frame[53], 0x04);
}

TEST(gptp_ptp4l_client, decode_each_data_set)
{
    Ptp4lStatus st{};
    EXPECT_EQ(
        Ptp4lClient::decode_response(make_response(ptp4l_management_id::DEFAULT_DATA_SET, default_data_set()), st, 10),
        ptp4l_management_id::DEFAULT_DATA_SET);
    EXPECT_TRUE(st.valid());
    EXPECT_EQ(st.priority1, 128);
    EXPECT_EQ(st.priority2, 128);
    EXPECT_EQ(st.clock_class, 248);
    EXPECT_EQ(st.offset_scaled_log_variance, 0xFFFF);
    EXPECT_EQ(st.clock_identity.to_uint64(), 0x70B3D5FFFEEDC301ULL);
    EXPECT_EQ(st.sampled_ns, 10);

    EXPECT_EQ(
        Ptp4lClient::decode_response(make_response(ptp4l_management_id::PARENT_DATA_SET, parent_data_set()), st, 11),
        ptp4l_management_id::PARENT_DATA_SET);
    EXPECT_EQ(st.grandmaster_identity.to_uint64(), 0x001CABFFFE007604ULL);
    EXPECT_EQ(st.gm_priority1, 246);
    EXPECT_EQ(st.gm_priority2, 248);
    EXPECT_EQ(st.gm_clock_class, 6);

    EXPECT_EQ(
        Ptp4lClient::decode_response(make_response(ptp4l_management_id::PORT_DATA_SET, port_data_set(1, 412)), st, 12),
        ptp4l_management_id::PORT_DATA_SET);
    EXPECT_EQ(st.port_number, 1);
    EXPECT_EQ(st.port_state, ptp4l_port_state::SLAVE);
    EXPECT_EQ(st.peer_mean_path_delay_ns, 412);
    EXPECT_EQ(st.log_sync_interval, -3);
    EXPECT_EQ(st.delay_mechanism, 2);

    // A second port's answer does not overwrite the first.
    EXPECT_EQ(
        Ptp4lClient::decode_response(make_response(ptp4l_management_id::PORT_DATA_SET, port_data_set(2, 999)), st, 13),
        ptp4l_management_id::PORT_DATA_SET);
    EXPECT_EQ(st.port_number, 1);
    EXPECT_EQ(st.peer_mean_path_delay_ns, 412);

    EXPECT_EQ(
        Ptp4lClient::decode_response(make_response(ptp4l_management_id::PORT_DATA_SET_NP, port_data_set_np(true)), st, 14),
        ptp4l_management_id::PORT_DATA_SET_NP);
    EXPECT_TRUE(st.as_capable);
    EXPECT_EQ(st.neighbor_prop_delay_thresh, 800U);

    EXPECT_EQ(
        Ptp4lClient::decode_response(make_response(ptp4l_management_id::TIME_STATUS_NP, time_status_np(true, -42)), st, 15),
        ptp4l_management_id::TIME_STATUS_NP);
    EXPECT_TRUE(st.gm_present);
    EXPECT_EQ(st.master_offset_ns, -42);
    EXPECT_EQ(st.gm_time_base_indicator, 7);
}

TEST(gptp_ptp4l_client, decode_rejects_errors_and_short_frames)
{
    Ptp4lStatus st{};
    auto frame = make_response(ptp4l_management_id::DEFAULT_DATA_SET, default_data_set());
    // Error status TLV type
    auto bad = frame;
    bad[MessageHeader::LENGTH + 14 + 1] = 0x02;
    EXPECT_EQ(Ptp4lClient::decode_response(bad, st, 1), 0);
    // A GET echoed back (action 0) is not a response
    bad = frame;
    bad[MessageHeader::LENGTH + 12] = 0x00;
    EXPECT_EQ(Ptp4lClient::decode_response(bad, st, 1), 0);
    // Truncated data
    frame.resize(frame.size() - 4);
    EXPECT_EQ(Ptp4lClient::decode_response(frame, st, 1), 0);
    EXPECT_FALSE(st.valid());
}

TEST(gptp_ptp4l_client, end_to_end_over_a_fake_ptp4l_socket)
{
    // A fake ptp4l: a bound datagram socket that answers every GET.
    std::string const server_path = std::format("/tmp/statusbar-fake-ptp4l.{}", ::getpid());
    ::unlink(server_path.c_str());
    int const server = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    EXPECT_TRUE(server >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, server_path.c_str(), sizeof(addr.sun_path) - 1);
    EXPECT_TRUE(::bind(server, net::sockaddr_cast(addr), sizeof(addr)) == 0);
    EXPECT_TRUE(::fcntl(server, F_SETFL, ::fcntl(server, F_GETFL, 0) | O_NONBLOCK) == 0);

    Ptp4lClient::Config cfg{};
    cfg.uds_path = server_path;
    cfg.poll_interval_ns = 1'000'000'000;
    Ptp4lClient client{cfg};
    EXPECT_TRUE(client.open().has_value());
    int changes = 0;
    client.set_on_changed([&](Ptp4lStatus const& s) {
        ++changes;
        (void)s;
    });

    client.tick(1'000'000'000);  // first tick sends the batch
    // Answer every GET with the matching data set.
    std::array<uint8_t, 256> buf{};
    sockaddr_un from{};
    int answered = 0;
    for (int i = 0; i < 6; ++i) {
        socklen_t from_len = sizeof(from);
        auto const n = ::recvfrom(server, buf.data(), buf.size(), 0, net::sockaddr_cast(from), &from_len);
        if (n <= 0) {
            break;
        }
        auto const id = static_cast<uint16_t>((buf[52] << 8U) | buf[53]);
        std::vector<uint8_t> reply;
        switch (id) {
            case ptp4l_management_id::DEFAULT_DATA_SET:
                reply = make_response(id, default_data_set());
                break;
            case ptp4l_management_id::CURRENT_DATA_SET: {
                std::vector<uint8_t> d;
                be16(d, 1);
                be64(d, 0);
                be64(d, static_cast<uint64_t>(int64_t{500} << 16U));
                reply = make_response(id, d);
                break;
            }
            case ptp4l_management_id::PARENT_DATA_SET:
                reply = make_response(id, parent_data_set());
                break;
            case ptp4l_management_id::PORT_DATA_SET:
                reply = make_response(id, port_data_set(1, 412));
                break;
            case ptp4l_management_id::PORT_DATA_SET_NP:
                reply = make_response(id, port_data_set_np(true));
                break;
            case ptp4l_management_id::TIME_STATUS_NP:
                reply = make_response(id, time_status_np(true, 3));
                break;
            default:
                break;
        }
        if (!reply.empty()) {
            (void)::sendto(server, reply.data(), reply.size(), 0, net::sockaddr_cast(from), from_len);
            ++answered;
        }
    }
    EXPECT_EQ(answered, 6);

    client.on_ready(1'000'500'000);
    auto const& st = client.status();
    EXPECT_TRUE(st.valid());
    EXPECT_TRUE(st.have_parent);
    EXPECT_TRUE(st.have_port);
    EXPECT_TRUE(st.have_port_np);
    EXPECT_TRUE(st.have_time_status);
    EXPECT_TRUE(st.have_current);
    EXPECT_EQ(st.mean_path_delay_ns, 500);
    EXPECT_EQ(st.grandmaster_identity.to_uint64(), 0x001CABFFFE007604ULL);
    EXPECT_TRUE(st.as_capable);
    EXPECT_EQ(changes, 1);

    // Nothing new: no further change notification; after three silent
    // intervals the snapshot goes stale and one invalidation fires.
    client.on_ready(1'000'600'000);
    EXPECT_EQ(changes, 1);
    client.tick(5'000'000'000);
    EXPECT_FALSE(client.status().valid());
    EXPECT_EQ(changes, 2);

    ::close(server);
    ::unlink(server_path.c_str());
}

TEST_MAIN(statusbar_gptp, gptp_ptp4l_client_test)
