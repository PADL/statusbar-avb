// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ptpclient/ptpclient_management.hpp"

#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>

using namespace statusbar;
using namespace statusbar::ptpclient;

namespace {

// ptp4l's RESPONSE to `pmc -u -b 0 'GET PARENT_DATA_SET'` (IEEE 1588-2019 15.5.3.3.1): clock 00:1b:21:ff:fe:aa:bb:cc
// port 1, domain 0; grandmaster 00:1d:c1:ff:fe:12:34:56 (priority1 128, class 6, accuracy 0x21, variance 0x4e5d).
// clang-format off
constexpr std::array<uint8_t, 86> PARENT_RESPONSE{
    // common header
    0x0D, 0x02, 0x00, 0x56,                          // management, PTPv2, messageLength 86
    0x00, 0x00, 0x00, 0x00,                          // domain 0, minorSdoId, flags
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // correctionField
    0x00, 0x00, 0x00, 0x00,                          // messageTypeSpecific
    0x00, 0x1B, 0x21, 0xFF, 0xFE, 0xAA, 0xBB, 0xCC,  // sourcePortIdentity.clockIdentity
    0x00, 0x01,                                      // sourcePortIdentity.portNumber
    0x00, 0x07,                                      // sequenceId 7
    0x04, 0x7F,                                      // controlField, logMessageInterval
    // management header
    0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x30, 0x39, 0x00, 0x01,  // targetPortIdentity (pmc's)
    0x00, 0x00,                                                  // startingBoundaryHops, boundaryHops
    0x02, 0x00,                                                  // actionField RESPONSE, reserved
    // MANAGEMENT TLV
    0x00, 0x01, 0x00, 0x22,                                      // tlvType MANAGEMENT, lengthField 34
    0x20, 0x02,                                                  // managementId PARENT_DATA_SET
    0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56, 0x00, 0x01,  // parentPortIdentity
    0x00, 0x00,                                                  // parentStats, reserved
    0xFF, 0xFF,                                                  // observedParentOffsetScaledLogVariance
    0x7F, 0xFF, 0xFF, 0xFF,                                      // observedParentClockPhaseChangeRate
    0x80,                                                        // grandmasterPriority1
    0x06, 0x21, 0x4E, 0x5D,                                      // grandmasterClockQuality
    0x80,                                                        // grandmasterPriority2
    0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56,              // grandmasterIdentity
};
// clang-format on

ieee::Eui64 const GM{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56};

// ptp4l's RESPONSE to `pmc -u -b 0 'GET PORT_DATA_SET'` (IEEE 1588-2019 15.5.3.6.1, linuxptp's struct portDS) from
// a gPTP master port: 8c1f64.fffe.e89000-2, MASTER, peerMeanPathDelay 74.25 ns, P2P, version 2.
// clang-format off
constexpr std::array<uint8_t, 80> PORT_RESPONSE{
    // common header
    0x0D, 0x02, 0x00, 0x50,                          // management, PTPv2, messageLength 80
    0x00, 0x00, 0x00, 0x00,                          // domain 0, minorSdoId, flags
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // correctionField
    0x00, 0x00, 0x00, 0x00,                          // messageTypeSpecific
    0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00,  // sourcePortIdentity.clockIdentity
    0x00, 0x02,                                      // sourcePortIdentity.portNumber
    0x00, 0x09,                                      // sequenceId 9
    0x04, 0x7F,                                      // controlField, logMessageInterval
    // management header
    0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x30, 0x39, 0x00, 0x01,  // targetPortIdentity (pmc's)
    0x00, 0x00,                                                  // startingBoundaryHops, boundaryHops
    0x02, 0x00,                                                  // actionField RESPONSE, reserved
    // MANAGEMENT TLV
    0x00, 0x01, 0x00, 0x1C,                                      // tlvType MANAGEMENT, lengthField 28
    0x20, 0x04,                                                  // managementId PORT_DATA_SET
    0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00, 0x00, 0x02,  // portIdentity
    0x06,                                                        // portState MASTER
    0x00,                                                        // logMinDelayReqInterval
    0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x40, 0x00,              // peerMeanPathDelay 74.25 ns
    0x00, 0x03,                                                  // logAnnounceInterval, announceReceiptTimeout
    0xFD,                                                        // logSyncInterval -3
    0x02,                                                        // delayMechanism P2P
    0x00, 0x02,                                                  // logMinPdelayReqInterval, versionNumber
};
// clang-format on

gptp::ClockIdentity const BRIDGE{0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00};

/// PORT_DATA_SET's data for `port` of BRIDGE with `delay_scaled`.
auto port_data(uint16_t const port, int64_t const delay_scaled) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data(PORT_RESPONSE.begin() + 54, PORT_RESPONSE.end());
    data[9] = static_cast<uint8_t>(port);
    for (int i = 0; i < 8; ++i) {
        data[12 + static_cast<size_t>(i)] = static_cast<uint8_t>(static_cast<uint64_t>(delay_scaled) >> (56 - (8 * i)));
    }
    return data;
}

// Synchronised: a followed grandmaster with a port in SLAVE, or the grandmaster itself with a port in
// MASTER and none UNCALIBRATED; LISTENING, UNCALIBRATED (a servo jump) and unreachable are not.
TEST(ptp_management, the_clock_is_synchronized_by_a_slave_port_or_as_grandmaster)
{
    PtpManagementState state{.status = PtpManagementStatus::locked};
    state.port_states[0] = PortState::uncalibrated;
    state.port_states[1] = PortState::master;
    EXPECT_FALSE(clock_synchronized(state));
    state.port_states[0] = PortState::slave;
    EXPECT_TRUE(clock_synchronized(state));
    state.status = PtpManagementStatus::unreachable;
    EXPECT_FALSE(clock_synchronized(state));

    PtpManagementState own{.status = PtpManagementStatus::no_grandmaster};
    own.port_states[0] = PortState::listening;
    EXPECT_FALSE(clock_synchronized(own));
    own.port_states[1] = PortState::master;
    EXPECT_TRUE(clock_synchronized(own));
    own.port_states[0] = PortState::uncalibrated;
    EXPECT_FALSE(clock_synchronized(own));
}

/// linuxptp's PORT_PROPERTIES_NP data for `port` of BRIDGE on `name` (MASTER, hardware timestamping).
auto port_properties(uint16_t const port, std::string_view const name) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data{0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00, 0x00, static_cast<uint8_t>(port), 0x06, 0x02};
    data.push_back(static_cast<uint8_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
    if (data.size() % 2 != 0) {
        data.push_back(0);  // ptp4l pads the TLV to an even length
    }
    return data;
}

/// A RESPONSE carrying `data` for `id`, in `domain` and transportSpecific `ts`, built as the bytes above are.
auto response(uint16_t const id, std::vector<uint8_t> const& data, uint8_t const domain = 0, uint8_t const ts = 0)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> out(PARENT_RESPONSE.begin(), PARENT_RESPONSE.begin() + 48);
    out[0] = static_cast<uint8_t>((ts << 4U) | 0x0DU);
    out[4] = domain;
    size_t const length = 48 + 6 + data.size();
    out[2] = static_cast<uint8_t>(length >> 8U);
    out[3] = static_cast<uint8_t>(length);
    uint16_t const tlv_length = static_cast<uint16_t>(2 + data.size());
    out.insert(out.end(), {0x00, 0x01, static_cast<uint8_t>(tlv_length >> 8U), static_cast<uint8_t>(tlv_length)});
    out.insert(out.end(), {static_cast<uint8_t>(id >> 8U), static_cast<uint8_t>(id)});
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

auto parent_data(ieee::Eui64 const& gm) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data(PARENT_RESPONSE.begin() + 54, PARENT_RESPONSE.end());
    uint64_t const v = gm.get();
    for (int i = 0; i < 8; ++i) {
        data[24 + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (56 - (8 * i)));
    }
    return data;
}

}  // namespace

TEST(ptp_management, builds_a_pmc_get)
{
    std::array<uint8_t, 64> out{};
    gptp::SourcePortIdentity const me{gptp::ClockIdentity{0, 0, 0, 0xFF, 0xFE, 0, 0x30, 0x39}, 1};
    size_t const n = build_management_get(out, management_id::parent_data_set, {.sequence_id = 7, .source = me});
    EXPECT_EQ(n, MANAGEMENT_GET_SIZE);
    // clang-format off
    std::array<uint8_t, 54> const expect{
        0x0D, 0x02, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00,                          // management, v2, length 54, domain 0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // correction, typeSpecific
        0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x30, 0x39, 0x00, 0x01,              // sourcePortIdentity
        0x00, 0x07, 0x04, 0x7F,                                                  // sequenceId, control, interval
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,              // targetPortIdentity: all
        0x00, 0x00, 0x00, 0x00,                                                  // hops 0, GET, reserved
        0x00, 0x01, 0x00, 0x02, 0x20, 0x02,                                      // MANAGEMENT TLV, PARENT_DATA_SET
    };
    // clang-format on
    EXPECT_TRUE(std::equal(expect.begin(), expect.end(), out.begin()));
    EXPECT_EQ(
        build_management_get(std::span<uint8_t>{out}.first(53), management_id::parent_data_set, {.sequence_id = 7, .source = me}),
        size_t{0});
    // and it parses back as a GET with no data
    auto const parsed = parse_management_message(std::span<uint8_t const>{out.data(), n});
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->action == ManagementAction::get);
    EXPECT_EQ(parsed->management_id, management_id::parent_data_set);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(parsed->data));
}

TEST(ptp_management, parses_parent_data_set)
{
    auto const parsed = parse_management_message(PARENT_RESPONSE);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->action == ManagementAction::response);
    EXPECT_EQ(parsed->domain, uint8_t{0});
    EXPECT_EQ(parsed->sequence_id, uint16_t{7});
    EXPECT_TRUE((parsed->source.clock_identity == gptp::ClockIdentity{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0xAA, 0xBB, 0xCC}));
    EXPECT_EQ(static_cast<uint16_t>(parsed->source.port_number), uint16_t{1});
    EXPECT_EQ(parsed->management_id, management_id::parent_data_set);
    auto const* parent = std::get_if<ParentDataSet>(&parsed->data);
    EXPECT_TRUE(parent != nullptr);
    EXPECT_TRUE(parent->grandmaster_identity == GM);
    EXPECT_EQ(parent->parent_port_identity.clock_identity.get(), GM.get());
    EXPECT_EQ(static_cast<uint16_t>(parent->parent_port_identity.port_number), uint16_t{1});
    EXPECT_EQ(parent->grandmaster_priority1, uint8_t{128});
    EXPECT_EQ(parent->grandmaster_clock_class, uint8_t{6});
    EXPECT_EQ(parent->grandmaster_clock_accuracy, uint8_t{0x21});
    EXPECT_EQ(parent->grandmaster_offset_scaled_log_variance, uint16_t{0x4E5D});
    EXPECT_EQ(parent->grandmaster_priority2, uint8_t{128});
    EXPECT_EQ(parent->observed_parent_clock_phase_change_rate, int32_t{0x7FFFFFFF});
}

TEST(ptp_management, parses_current_and_time_properties)
{
    // stepsRemoved 1, offsetFromMaster -12.5 ns (scaled 2^16), meanPathDelay 500 ns
    std::vector<uint8_t> current{
        0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF3, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xF4, 0x00, 0x00};
    auto const c = parse_management_message(response(management_id::current_data_set, current, 3));
    EXPECT_TRUE(c.has_value());
    EXPECT_EQ(c->domain, uint8_t{3});
    auto const* cds = std::get_if<CurrentDataSet>(&c->data);
    EXPECT_TRUE(cds != nullptr);
    EXPECT_EQ(cds->steps_removed, uint16_t{1});
    EXPECT_EQ(cds->offset_from_master_scaled, int64_t{-12 * 65536 - 32768});
    EXPECT_EQ(cds->mean_path_delay_scaled, int64_t{500} * 65536);

    // UTC offset 37, ptpTimescale | utcOffsetValid, GPS
    auto const t = parse_management_message(response(management_id::time_properties_data_set, {0x00, 0x25, 0x0C, 0x20}));
    EXPECT_TRUE(t.has_value());
    auto const* tp = std::get_if<TimePropertiesDataSet>(&t->data);
    EXPECT_TRUE(tp != nullptr);
    EXPECT_EQ(tp->current_utc_offset, int16_t{37});
    EXPECT_EQ(tp->flags, uint8_t{0x0C});
    EXPECT_EQ(tp->time_source, uint8_t{0x20});
}

TEST(ptp_management, parses_port_data_set)
{
    auto const parsed = parse_management_message(PORT_RESPONSE);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->management_id, management_id::port_data_set);
    auto const* port = std::get_if<PortDataSet>(&parsed->data);
    EXPECT_TRUE(port != nullptr);
    EXPECT_TRUE((port->port_identity == gptp::SourcePortIdentity{BRIDGE, 2}));
    EXPECT_TRUE(port->port_state == PortState::master);
    EXPECT_EQ(port->peer_mean_path_delay_scaled, int64_t{74} * 65536 + 16384);
    EXPECT_EQ(time_interval_ns(port->peer_mean_path_delay_scaled), int64_t{74});
    EXPECT_EQ(port->announce_receipt_timeout, uint8_t{3});
    EXPECT_EQ(port->log_sync_interval, int8_t{-3});
    EXPECT_TRUE(port->delay_mechanism == DelayMechanism::p2p);
    EXPECT_EQ(port->version_number, uint8_t{2});
    // too short for the data set
    EXPECT_FALSE(parse_management_message(response(management_id::port_data_set, std::vector<uint8_t>(25))).has_value());
}

// linuxptp's CMLDS_INFO_NP (struct cmlds_info_np): meanLinkDelay 74.25 ns, a neighbor rate ratio, asCapable 1
TEST(ptp_management, parses_cmlds_info_np)
{
    std::vector<uint8_t> const data{0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x40, 0x00, 0xFF, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x01};
    auto const parsed = parse_management_message(response(management_id::cmlds_info_np, data));
    EXPECT_TRUE(parsed.has_value());
    auto const* cmlds = parsed ? std::get_if<CmldsInfoNp>(&parsed->data) : nullptr;
    EXPECT_TRUE(cmlds != nullptr);
    if (cmlds != nullptr) {
        EXPECT_EQ(time_interval_ns(cmlds->mean_link_delay_scaled), int64_t{74});
        EXPECT_EQ(cmlds->scaled_neighbor_rate_ratio, int32_t{-1024});
        EXPECT_TRUE(cmlds->as_capable == AsCapable::capable);
    }
    EXPECT_FALSE(parse_management_message(response(management_id::cmlds_info_np, std::vector<uint8_t>(15))).has_value());
}

TEST(ptp_management, parses_port_properties_np)
{
    auto const parsed = parse_management_message(response(management_id::port_properties_np, port_properties(2, "enp8s0")));
    EXPECT_TRUE(parsed.has_value());
    auto const* properties = std::get_if<PortPropertiesNp>(&parsed->data);
    EXPECT_TRUE(properties != nullptr);
    EXPECT_TRUE((properties->port_identity == gptp::SourcePortIdentity{BRIDGE, 2}));
    EXPECT_TRUE(properties->port_state == PortState::master);
    EXPECT_EQ(properties->timestamping, uint8_t{2});
    EXPECT_TRUE(properties->interface_name() == "enp8s0");
    // a PTPText longer than the TLV
    auto bad = port_properties(2, "enp8s0");
    bad[12] = 40;
    EXPECT_FALSE(parse_management_message(response(management_id::port_properties_np, bad)).has_value());
}

TEST(ptp_management, time_intervals_round_to_the_nearest_ns)
{
    EXPECT_EQ(time_interval_ns(0), int64_t{0});
    EXPECT_EQ(time_interval_ns(int64_t{74} * 65536), int64_t{74});
    EXPECT_EQ(time_interval_ns(int64_t{74} * 65536 + 32767), int64_t{74});
    EXPECT_EQ(time_interval_ns(int64_t{74} * 65536 + 32768), int64_t{75});
    EXPECT_EQ(time_interval_ns(int64_t{-12} * 65536 - 32768), int64_t{-13});
    EXPECT_EQ(time_interval_ns(int64_t{-12} * 65536 - 32767), int64_t{-12});
    EXPECT_EQ(time_interval_ns(INT64_MAX), INT64_MAX / 65536 + 1);
}

TEST(ptp_management, refuses_malformed_messages)
{
    std::vector<uint8_t> bytes(PARENT_RESPONSE.begin(), PARENT_RESPONSE.end());
    // truncated below the declared length
    EXPECT_FALSE(parse_management_message(std::span<uint8_t const>{bytes}.first(60)).has_value());
    // not management
    auto other = bytes;
    other[0] = 0x00;
    EXPECT_FALSE(parse_management_message(other).has_value());
    // not PTPv2
    other = bytes;
    other[1] = 0x01;
    EXPECT_FALSE(parse_management_message(other).has_value());
    // a TLV longer than the message
    other = bytes;
    other[51] = 0x40;
    EXPECT_FALSE(parse_management_message(other).has_value());
    // a PARENT_DATA_SET response too short for the data set
    EXPECT_FALSE(parse_management_message(response(management_id::parent_data_set, {0x00, 0x01, 0x02})).has_value());
    // an unknown data set parses, without data
    auto const unknown = parse_management_message(response(0x2005, {0x01, 0x02}));
    EXPECT_TRUE(unknown.has_value());
    EXPECT_TRUE(std::holds_alternative<std::monostate>(unknown->data));
    // MANAGEMENT_ERROR_STATUS: NO_SUCH_ID (0x0002) for PARENT_DATA_SET
    auto error = response(0, {});
    error.resize(48);
    error.insert(error.end(), {0x00, 0x02, 0x00, 0x08, 0x00, 0x02, 0x20, 0x02, 0x00, 0x00, 0x00, 0x00});
    error[3] = static_cast<uint8_t>(error.size());
    auto const e = parse_management_message(error);
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e->error.value_or(0), uint16_t{2});
    EXPECT_EQ(e->management_id, management_id::parent_data_set);
}

TEST(ptp_management, grandmaster_settings_encode_as_linuxptp_reads_them)
{
    // linuxptp's struct grandmaster_settings_np: clockQuality, utc_offset, time_flags, time_source.
    GrandmasterSettingsNp const settings{
        .clock_class = 13,
        .clock_accuracy = 0x21,
        .offset_scaled_log_variance = 0x4E5D,
        .current_utc_offset = 37,
        .time_flags = time_flag::ptp_timescale | time_flag::utc_offset_valid,
        .time_source = 0x90,
    };
    std::array<uint8_t, MANAGEMENT_SET_MAX_SIZE> out{};
    gptp::SourcePortIdentity const me{gptp::ClockIdentity{0, 0, 0, 0xFF, 0xFE, 0, 0x30, 0x39}, 1};
    size_t const n = build_management_message(
        out,
        ManagementAction::set,
        management_id::grandmaster_settings_np,
        encode(settings),
        {.domain = 127, .sequence_id = 9, .source = me});
    EXPECT_EQ(n, size_t{62});
    // clang-format off
    std::array<uint8_t, 62> const expect{
        0x0D, 0x02, 0x00, 0x3E, 0x7F, 0x00, 0x00, 0x00,                          // management, v2, length 62, domain 127
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // correction, typeSpecific
        0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x30, 0x39, 0x00, 0x01,              // sourcePortIdentity
        0x00, 0x09, 0x04, 0x7F,                                                  // sequenceId, control, interval
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,              // targetPortIdentity: all
        0x00, 0x00, 0x01, 0x00,                                                  // hops 0, SET, reserved
        0x00, 0x01, 0x00, 0x0A, 0xC0, 0x01,                                      // MANAGEMENT TLV, GRANDMASTER_SETTINGS_NP
        0x0D, 0x21, 0x4E, 0x5D,                                                  // clockClass, clockAccuracy, variance
        0x00, 0x25, 0x0C, 0x90,                                                  // utc_offset 37, flags, GPS
    };
    // clang-format on
    EXPECT_TRUE(std::equal(expect.begin(), expect.end(), out.begin()));
    auto const parsed = parse_management_message(std::span<uint8_t const>{out.data(), n});
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->action == ManagementAction::set);
    EXPECT_EQ(parsed->domain, uint8_t{127});
    auto const* back = std::get_if<GrandmasterSettingsNp>(&parsed->data);
    EXPECT_TRUE(back != nullptr && *back == settings);
    EXPECT_EQ(
        build_management_message(
            std::span<uint8_t>{out}.first(61),
            ManagementAction::set,
            management_id::grandmaster_settings_np,
            encode(settings),
            {.source = me}),
        size_t{0});
}

TEST(ptp_management, a_subscription_encodes_its_duration_and_events)
{
    SubscribeEventsNp subscription{.duration_s = 180};
    subscription.add(ManagementEvent::port_state);
    subscription.add(ManagementEvent::parent_data_set);
    EXPECT_TRUE(subscription.has(ManagementEvent::port_state));
    EXPECT_FALSE(subscription.has(ManagementEvent::time_sync));
    auto const data = encode(subscription);
    EXPECT_EQ(data[0], uint8_t{0x00});
    EXPECT_EQ(data[1], uint8_t{0xB4});
    EXPECT_EQ(data[2], uint8_t{0x05});  // bits 0 and 2 of the first octet
    EXPECT_TRUE(std::all_of(data.begin() + 3, data.end(), [](uint8_t const b) { return b == 0; }));
    std::array<uint8_t, MANAGEMENT_SET_MAX_SIZE> out{};
    size_t const n =
        build_management_message(out, ManagementAction::set, management_id::subscribe_events_np, data, {.sequence_id = 1});
    EXPECT_EQ(n, MANAGEMENT_SET_MAX_SIZE);
    auto const parsed = parse_management_message(std::span<uint8_t const>{out.data(), n});
    EXPECT_TRUE(parsed.has_value());
    auto const* back = std::get_if<SubscribeEventsNp>(&parsed->data);
    EXPECT_TRUE(back != nullptr);
    EXPECT_EQ(back->duration_s, uint16_t{180});
    EXPECT_TRUE(back->has(ManagementEvent::parent_data_set));
    // odd data is padded to an even TLV
    std::array<uint8_t, 3> const odd{1, 2, 3};
    EXPECT_EQ(build_management_message(out, ManagementAction::set, 0xC0FF, odd, {.sequence_id = 1}), MANAGEMENT_GET_SIZE + 4);
    EXPECT_EQ(out[51], uint8_t{6});
    EXPECT_EQ(out[57], uint8_t{0});
}

TEST(ptp_management, a_port_state_notification_is_parsed)
{
    // ptp4l's NOTIFY_PORT_STATE: a RESPONSE carrying the port's PORT_DATA_SET, now SLAVE.
    std::vector<uint8_t> bytes(PORT_RESPONSE.begin(), PORT_RESPONSE.end());
    bytes[54 + 10] = 0x09;
    auto const parsed = parse_management_message(bytes);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->action == ManagementAction::response);
    auto const* port = std::get_if<PortDataSet>(&parsed->data);
    EXPECT_TRUE(port != nullptr);
    EXPECT_TRUE(port->port_state == PortState::slave);
}

TEST(ptp_management, a_time_sync_notification_is_parsed)
{
    // linuxptp's TIME_STATUS_NP: master_offset -42 ns, ingress 1 s, gmPresent, grandmaster GM.
    std::vector<uint8_t> data(TimeStatusNp::SIZE);
    for (size_t i = 0; i < 8; ++i) {
        data[i] = static_cast<uint8_t>(static_cast<uint64_t>(int64_t{-42}) >> (56 - (8 * i)));
        data[8 + i] = static_cast<uint8_t>(uint64_t{1'000'000'000} >> (56 - (8 * i)));
        data[42 + i] = static_cast<uint8_t>(GM.get() >> (56 - (8 * i)));
    }
    data[19] = 0x10;  // cumulativeScaledRateOffset
    data[25] = 0x02;  // gmTimeBaseIndicator
    data[41] = 0x01;  // gmPresent
    auto const parsed = parse_management_message(response(management_id::time_status_np, data, 127));
    EXPECT_TRUE(parsed.has_value());
    auto const* status = std::get_if<TimeStatusNp>(&parsed->data);
    EXPECT_TRUE(status != nullptr);
    EXPECT_EQ(status->master_offset_ns, int64_t{-42});
    EXPECT_EQ(status->ingress_time_ns, int64_t{1'000'000'000});
    EXPECT_EQ(status->cumulative_scaled_rate_offset, int32_t{0x10});
    EXPECT_EQ(status->gm_time_base_indicator, uint16_t{2});
    EXPECT_TRUE(status->gm_present);
    EXPECT_TRUE(status->gm_identity == GM);
    EXPECT_FALSE(parse_management_message(response(management_id::time_status_np, std::vector<uint8_t>(48))).has_value());
}

TEST(ptp_management, accepts_any_minor_version_and_transport_specific)
{
    // linuxptp's default ptp_minor_version 1 sends 0x12, and a gPTP port's transportSpecific is 1.
    auto bytes = response(management_id::parent_data_set, parent_data(GM), 0, 1);
    bytes[1] = 0x12;
    auto const parsed = parse_management_message(bytes);
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->transport_specific, uint8_t{1});
    EXPECT_TRUE(std::holds_alternative<ParentDataSet>(parsed->data));
    bytes[1] = 0x13;  // PTPv3: not ours
    EXPECT_FALSE(parse_management_message(bytes).has_value());
}

#if defined(__linux__)

namespace {

/// A fresh directory for a client's socket (and a stand-in ptp4l's).
auto socket_dir(char const* name) -> std::filesystem::path
{
    auto const dir = std::filesystem::temp_directory_path() / std::format("ptpmgmt-{}-{}", name, ::getpid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/// The address @p fd is bound to.
auto bound_address(int const fd) -> std::pair<sockaddr_un, socklen_t>
{
    sockaddr_un address{};
    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    return {address, length};
}

/// A message the stand-in received: its managementId, action and transportSpecific.
struct Received
{
    uint16_t id;
    ManagementAction action;
    uint8_t transport_specific;
};

/// A stand-in for ptp4l: a datagram socket at `dir`/ptp4l that records requests and answers the
/// last sender, as ptp4l's uds transport does.
struct Ptp4l
{
    explicit Ptp4l(std::filesystem::path const& dir)
        : path{(dir / "ptp4l").string()}
        , fd{::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0)}
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.data(), path.size());
        EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)), 0);
    }
    Ptp4l(Ptp4l const&) = delete;
    auto operator=(Ptp4l const&) -> Ptp4l& = delete;
    ~Ptp4l() { ::close(fd); }

    /// Every request since the last drain.
    auto drain() -> std::vector<Received>
    {
        std::vector<Received> seen;
        for (;;) {
            std::array<uint8_t, 256> in{};
            sockaddr_un sender{};
            socklen_t sender_length = sizeof(sender);
            ssize_t const n = ::recvfrom(fd, in.data(), in.size(), 0, reinterpret_cast<sockaddr*>(&sender), &sender_length);
            if (n <= 0) {
                return seen;
            }
            from = sender;
            from_length = sender_length;
            auto const message = parse_management_message(std::span<uint8_t const>{in.data(), static_cast<size_t>(n)});
            EXPECT_TRUE(message.has_value());
            if (message) {
                seen.push_back(Received{message->management_id, message->action, message->transport_specific});
            }
        }
    }

    /// Answer the last sender with @p bytes.
    void answer(std::span<uint8_t const> const bytes) const
    {
        EXPECT_EQ(
            ::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr const*>(&from), from_length),
            static_cast<ssize_t>(bytes.size()));
    }

    std::string path;
    int fd;
    sockaddr_un from{};
    socklen_t from_length{0};
};

auto count(std::vector<Received> const& seen, uint16_t const id, ManagementAction const action) -> int64_t
{
    return std::ranges::count_if(seen, [&](Received const& r) { return r.id == id && r.action == action; });
}

/// The subscription a client holds once ptp4l answers its SET or GET.
auto held_subscription(uint16_t const duration_s, bool const cmlds = false) -> std::vector<uint8_t>
{
    SubscribeEventsNp held{.duration_s = duration_s};
    held.add(ManagementEvent::port_state);
    held.add(ManagementEvent::parent_data_set);
    if (cmlds) {
        held.add(ManagementEvent::cmlds);
    }
    auto const data = encode(held);
    return response(management_id::subscribe_events_np, {data.begin(), data.end()});
}

constexpr int64_t SECOND = 1'000'000'000;

}  // namespace

// Each client autobinds an abstract address of its own (Linux), so two in one process never
// share a reply address
TEST(ptp_management, two_clients_in_a_process_have_their_own_addresses)
{
    auto const dir = socket_dir("autobind");
    PtpManagementConfig const config{.uds_address = (dir / "ptp4l").string()};
    auto a = PtpManagementClient::create(config, [](PtpManagementState const&) {});
    auto b = PtpManagementClient::create(config, [](PtpManagementState const&) {});
    EXPECT_TRUE(a.has_value() && b.has_value());
    if (!a || !b) {
        return;
    }
    auto const [address_a, length_a] = bound_address((*a)->fd());
    auto const [address_b, length_b] = bound_address((*b)->fd());
    EXPECT_TRUE(length_a > offsetof(sockaddr_un, sun_path) && address_a.sun_path[0] == '\0');  // abstract
    EXPECT_TRUE(length_b > offsetof(sockaddr_un, sun_path) && address_b.sun_path[0] == '\0');
    EXPECT_FALSE(length_a == length_b && std::memcmp(&address_a, &address_b, length_a) == 0);
    EXPECT_TRUE(std::filesystem::is_empty(dir));
    std::filesystem::remove_all(dir);
}

TEST(ptp_management, create_validates_its_configuration)
{
    auto const listener = [](PtpManagementState const&) {};
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.uds_address = "relative/ptp4l"}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.uds_address = ""}, listener).has_value());
    EXPECT_FALSE(
        PtpManagementClient::create(PtpManagementConfig{.uds_address = std::string("/tmp/a\0b", 8)}, listener).has_value());
    EXPECT_FALSE(
        PtpManagementClient::create(PtpManagementConfig{.uds_address = "/" + std::string(200, 'x')}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.poll_period_ns = 0}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.retry_period_ns = 0}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.subscription_duration_s = 2}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.subscription_duration_s = 65535}, listener).has_value());
}

// Unreachable, the client probes in both transportSpecific values; the first answer fetches every
// data set once and subscribes, after which only what ptp4l never notifies is polled
TEST(ptp_management, client_probes_then_fetches_once_and_polls_only_the_unnotified)
{
    auto const dir = socket_dir("fetch");
    Ptp4l ptp4l{dir};
    int calls = 0;
    PtpManagementState seen{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .subscription_duration_s = 60}, [&](PtpManagementState const& state) {
            ++calls;
            seen = state;
        });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.tick(0);
    auto probes = ptp4l.drain();
    EXPECT_EQ(probes.size(), size_t{2});
    EXPECT_TRUE(probes.size() == 2 && probes[0].transport_specific == 0 && probes[1].transport_specific == 1);
    c.tick(SECOND / 2);
    EXPECT_TRUE(ptp4l.drain().empty());  // the next probe waits for retry_period_ns

    // ptp4l (a gPTP port, transportSpecific 1, minor version 1) answers the second probe.
    auto parent = response(management_id::parent_data_set, parent_data(GM), 0, 1);
    parent[1] = 0x12;
    ptp4l.answer(parent);
    c.on_ready(SECOND / 2 + 1);
    EXPECT_TRUE(seen.status == PtpManagementStatus::locked && seen.grandmaster == GM);
    auto const fetch = ptp4l.drain();
    EXPECT_EQ(count(fetch, management_id::parent_data_set, ManagementAction::get), 1);
    EXPECT_EQ(count(fetch, management_id::current_data_set, ManagementAction::get), 1);
    EXPECT_EQ(count(fetch, management_id::time_properties_data_set, ManagementAction::get), 1);
    EXPECT_EQ(count(fetch, management_id::subscribe_events_np, ManagementAction::set), 1);
    EXPECT_TRUE(std::ranges::all_of(fetch, [](Received const& r) { return r.transport_specific == 1; }));

    // Subscribed: a poll asks only for TIME_PROPERTIES_DATA_SET.
    c.handle(held_subscription(6), SECOND);
    EXPECT_TRUE(c.state().subscribed);
    c.tick(SECOND * 5 / 2);
    EXPECT_TRUE(ptp4l.drain().empty());  // one poll period after the fetch is not yet over
    c.tick(3 * SECOND);
    auto const poll = ptp4l.drain();
    EXPECT_EQ(poll.size(), size_t{1});
    EXPECT_EQ(count(poll, management_id::time_properties_data_set, ManagementAction::get), 1);
    EXPECT_EQ(calls, 2);  // locked, then subscribed
    client->reset();
    std::filesystem::remove_all(dir);
}

// Notifications: a port state change and a new parent arrive as RESPONSEs; a new parent fetches
// what ptp4l notifies nothing for, and TIME_STATUS_NP is kept without a report
TEST(ptp_management, client_receives_notifications)
{
    auto const dir = socket_dir("notify");
    Ptp4l ptp4l{dir};
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .port_interface = "enp8s0"}, [&](PtpManagementState const& state) {
            ++calls;
            last = state;
        });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 10);
    c.handle(response(management_id::port_properties_np, port_properties(2, "enp8s0")), 11);
    c.handle(PORT_RESPONSE, 12);
    EXPECT_TRUE(last.port_state == PortState::master);
    (void)ptp4l.drain();
    int const before = calls;

    auto slave = port_data(2, int64_t{74} * 65536);
    slave[10] = 0x09;
    c.handle(response(management_id::port_data_set, slave), 20);
    EXPECT_EQ(calls, before + 1);
    EXPECT_TRUE(last.port_state == PortState::slave);

    ieee::Eui64 const other{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x65, 0x43, 0x21};
    c.handle(response(management_id::parent_data_set, parent_data(other)), 30);
    EXPECT_TRUE(last.grandmaster == other);
    auto const refetch = ptp4l.drain();
    EXPECT_EQ(count(refetch, management_id::current_data_set, ManagementAction::get), 1);
    EXPECT_EQ(count(refetch, management_id::time_properties_data_set, ManagementAction::get), 1);
    int const after = calls;
    c.handle(response(management_id::time_status_np, std::vector<uint8_t>(TimeStatusNp::SIZE)), 40);
    EXPECT_EQ(calls, after);
    EXPECT_TRUE(c.state().time_status.has_value());
    client->reset();
    std::filesystem::remove_all(dir);
}

// The followed port: PORT_PROPERTIES_NP names it, its PORT_DATA_SET gives the peer delay to the ns
TEST(ptp_management, client_follows_the_named_port_s_peer_delay)
{
    auto const dir = socket_dir("port");
    Ptp4l ptp4l{dir};
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .port_interface = "enp8s0"}, [&](PtpManagementState const& state) {
            ++calls;
            last = state;
        });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 10);
    EXPECT_EQ(calls, 1);
    c.handle(PORT_RESPONSE, 20);  // before PORT_PROPERTIES_NP names the port: its state only
    EXPECT_FALSE(c.state().peer_mean_path_delay_ns.has_value());
    EXPECT_EQ(calls, 2);
    c.handle(response(management_id::port_properties_np, port_properties(1, "ens4")), 30);
    c.handle(response(management_id::port_properties_np, port_properties(2, "enp8s0")), 31);
    EXPECT_TRUE((c.state().port == gptp::SourcePortIdentity{BRIDGE, 2}));
    c.handle(response(management_id::port_data_set, port_data(1, int64_t{900} * 65536)), 40);  // ens4's: its state only
    EXPECT_EQ(calls, 3);
    EXPECT_FALSE(c.state().peer_mean_path_delay_ns.has_value());
    c.handle(PORT_RESPONSE, 41);
    EXPECT_EQ(calls, 4);
    EXPECT_EQ(last.peer_mean_path_delay_ns.value_or(0), int64_t{74});
    EXPECT_TRUE(last.delay_mechanism == DelayMechanism::p2p);
    c.handle(response(management_id::port_data_set, port_data(2, int64_t{74} * 65536 + 100)), 50);
    EXPECT_EQ(calls, 4);  // the same delay to the ns
    c.handle(response(management_id::port_data_set, port_data(2, int64_t{77} * 65536)), 60);
    EXPECT_EQ(last.peer_mean_path_delay_ns.value_or(0), int64_t{77});
    client->reset();
    std::filesystem::remove_all(dir);
}

// Every port's state is kept, followed port or not, and reported: a port moving SLAVE to UNCALIBRATED
// (linuxptp's servo jump) is a report, and clock_synchronized() follows it
TEST(ptp_management, client_keeps_every_port_s_state)
{
    auto const dir = socket_dir("states");
    Ptp4l ptp4l{dir};
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(PtpManagementConfig{.uds_address = ptp4l.path}, [&](PtpManagementState const& state) {
        ++calls;
        last = state;
    });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 10);
    EXPECT_EQ(count(ptp4l.drain(), management_id::port_data_set, ManagementAction::get), 1);
    auto slave = port_data(1, 0);
    slave[10] = static_cast<uint8_t>(PortState::slave);
    c.handle(response(management_id::port_data_set, slave), 20);
    c.handle(response(management_id::port_data_set, port_data(2, 0)), 21);
    EXPECT_TRUE(last.port_states[0] == PortState::slave && last.port_states[1] == PortState::master);
    EXPECT_FALSE(last.port_state.has_value());  // no port followed
    EXPECT_TRUE(clock_synchronized(last));
    int const before = calls;
    slave[10] = static_cast<uint8_t>(PortState::uncalibrated);
    c.handle(response(management_id::port_data_set, slave), 30);
    EXPECT_EQ(calls, before + 1);
    EXPECT_FALSE(clock_synchronized(last));
    client->reset();
    std::filesystem::remove_all(dir);
}

// Subscribed, a port's state change asks for PARENT_DATA_SET again (a linuxptp before 4.2 holds
// the bit without sending the notification); the same state again asks nothing
TEST(ptp_management, a_port_state_change_asks_for_the_parent)
{
    auto const dir = socket_dir("parent");
    Ptp4l ptp4l{dir};
    auto client = PtpManagementClient::create(PtpManagementConfig{.uds_address = ptp4l.path}, [](PtpManagementState const&) {});
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 0);
    c.handle(held_subscription(180), 1);
    EXPECT_TRUE(c.state().subscribed);
    auto slave = port_data(1, 0);
    slave[10] = static_cast<uint8_t>(PortState::slave);
    c.handle(response(management_id::port_data_set, slave), 2);
    (void)ptp4l.drain();
    c.handle(response(management_id::port_data_set, slave), 3);
    EXPECT_EQ(count(ptp4l.drain(), management_id::parent_data_set, ManagementAction::get), 0);
    slave[10] = static_cast<uint8_t>(PortState::uncalibrated);
    c.handle(response(management_id::port_data_set, slave), 4);
    EXPECT_EQ(count(ptp4l.drain(), management_id::parent_data_set, ManagementAction::get), 1);
    client->reset();
    std::filesystem::remove_all(dir);
}

// The renewal: GET then SET SUBSCRIBE_EVENTS_NP three times per duration; a GET answered with no
// subscription is a restarted ptp4l, so everything is fetched and the settings sent again
TEST(ptp_management, a_renewal_finding_no_subscription_fetches_again)
{
    auto const dir = socket_dir("renew");
    Ptp4l ptp4l{dir};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .poll_period_ns = 10 * SECOND, .subscription_duration_s = 6},
        [](PtpManagementState const&) {});
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    GrandmasterSettingsNp const settings{.clock_class = 13, .clock_accuracy = 0x22};
    EXPECT_TRUE(c.set_grandmaster_settings(settings).has_value());
    EXPECT_EQ(count(ptp4l.drain(), management_id::grandmaster_settings_np, ManagementAction::set), 1);
    c.handle(PARENT_RESPONSE, 0);  // reachable: fetch, the settings again (they may have preceded ptp4l), subscription
    EXPECT_EQ(count(ptp4l.drain(), management_id::grandmaster_settings_np, ManagementAction::set), 1);
    c.handle(held_subscription(6), 1);
    EXPECT_TRUE(c.state().subscribed);

    c.handle(PARENT_RESPONSE, SECOND);
    c.tick(SECOND);
    EXPECT_TRUE(ptp4l.drain().empty());  // nothing due before 2 s
    c.handle(PARENT_RESPONSE, 2 * SECOND);
    c.tick(2 * SECOND);
    auto const renewal = ptp4l.drain();
    EXPECT_EQ(count(renewal, management_id::grandmaster_settings_np, ManagementAction::get), 1);  // ours given
    EXPECT_EQ(count(renewal, management_id::subscribe_events_np, ManagementAction::get), 1);
    EXPECT_EQ(count(renewal, management_id::subscribe_events_np, ManagementAction::set), 1);
    c.handle(held_subscription(4), 2 * SECOND + 1);  // still held: nothing more
    EXPECT_TRUE(ptp4l.drain().empty());

    auto const nothing = encode(SubscribeEventsNp{.duration_s = 0});
    c.handle(response(management_id::subscribe_events_np, {nothing.begin(), nothing.end()}), 2 * SECOND + 2);
    EXPECT_FALSE(c.state().subscribed);
    auto const again = ptp4l.drain();
    EXPECT_EQ(count(again, management_id::parent_data_set, ManagementAction::get), 1);
    EXPECT_EQ(count(again, management_id::grandmaster_settings_np, ManagementAction::set), 1);
    EXPECT_EQ(count(again, management_id::subscribe_events_np, ManagementAction::set), 1);
    client->reset();
    std::filesystem::remove_all(dir);
}

// Silence for two renewals is unreachable, everything learnt forgotten; with no ptp4l socket a
// send fails at once, and the probes go on
TEST(ptp_management, silence_or_a_vanished_socket_is_unreachable)
{
    auto const dir = socket_dir("silence");
    int calls = 0;
    PtpManagementState last{};
    auto ptp4l = std::make_unique<Ptp4l>(dir);
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l->path, .subscription_duration_s = 6}, [&](PtpManagementState const& state) {
            ++calls;
            last = state;
        });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 0);
    c.handle(response(management_id::time_properties_data_set, {0x00, 0x25, 0x0C, 0x20}), 1);
    EXPECT_TRUE(last.status == PtpManagementStatus::locked);
    EXPECT_TRUE(c.state().time_properties.has_value());
    c.tick(4 * SECOND);
    EXPECT_TRUE(last.status == PtpManagementStatus::locked);
    c.tick(5 * SECOND);  // no answer for more than two 2 s renewals and two polls
    EXPECT_TRUE(last.status == PtpManagementStatus::unreachable);
    EXPECT_FALSE(c.state().grandmaster.has_value() || c.state().time_properties.has_value());

    // ptp4l back, then gone: its socket refuses the next send, and the client is unreachable at once.
    c.handle(PARENT_RESPONSE, 6 * SECOND);
    EXPECT_TRUE(last.status == PtpManagementStatus::locked);
    ptp4l.reset();
    std::filesystem::remove(dir / "ptp4l");
    c.tick(9 * SECOND);  // a poll
    EXPECT_TRUE(last.status == PtpManagementStatus::unreachable);
    EXPECT_TRUE(last.error == std::errc::no_such_file_or_directory || last.error == std::errc::connection_refused);
    EXPECT_FALSE(c.finished());
    client->reset();
    std::filesystem::remove_all(dir);
}

// The grandmaster settings: acknowledged, refused, and sent again when ptp4l holds others
TEST(ptp_management, client_reports_settings_and_refusals)
{
    auto const dir = socket_dir("settings");
    Ptp4l ptp4l{dir};
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(PtpManagementConfig{.uds_address = ptp4l.path}, [&](PtpManagementState const& state) {
        ++calls;
        last = state;
    });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    GrandmasterSettingsNp const settings{.clock_class = 13, .clock_accuracy = 0x21};
    c.handle(PARENT_RESPONSE, 0);
    EXPECT_TRUE(c.set_grandmaster_settings(settings).has_value());
    (void)ptp4l.drain();
    int const before = calls;
    auto const settings_data = encode(settings);
    c.handle(response(management_id::grandmaster_settings_np, {settings_data.begin(), settings_data.end()}), 10);
    EXPECT_EQ(calls, before + 1);
    EXPECT_TRUE(last.grandmaster_settings == settings);
    EXPECT_TRUE(ptp4l.drain().empty());
    auto const defaults = encode(GrandmasterSettingsNp{});
    c.handle(response(management_id::grandmaster_settings_np, {defaults.begin(), defaults.end()}), 20);
    EXPECT_EQ(count(ptp4l.drain(), management_id::grandmaster_settings_np, ManagementAction::set), 1);

    auto error = response(0, {});
    error.resize(48);
    error.insert(error.end(), {0x00, 0x02, 0x00, 0x08, 0x00, 0x05, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00});  // NOT_SETABLE
    error[3] = static_cast<uint8_t>(error.size());
    c.handle(error, 30);
    EXPECT_EQ(last.refused.value_or(0), uint16_t{5});
    int const refused = calls;
    error[54] = 0x20;  // a refused GET is not a refusal of ours
    error[55] = 0x02;
    c.handle(error, 40);
    EXPECT_EQ(calls, refused);
    client->reset();
    std::filesystem::remove_all(dir);
}

// A port's notification arriving first, after a silence, makes ptp4l neither reachable nor gives
// the client its transportSpecific: the next clock data set's answer does
TEST(ptp_management, a_notification_first_does_not_set_the_transport_specific)
{
    auto const dir = socket_dir("tsfirst");
    Ptp4l ptp4l{dir};
    auto client = PtpManagementClient::create(PtpManagementConfig{.uds_address = ptp4l.path}, [](PtpManagementState const&) {});
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(response(management_id::port_data_set, port_data(2, 0), 0, 1), 0);  // the gPTP port: 1
    EXPECT_TRUE(c.state().status == PtpManagementStatus::unreachable);
    c.handle(PARENT_RESPONSE, 1);  // the UDS port: 0
    (void)ptp4l.drain();
    c.tick(3 * SECOND);
    auto const poll = ptp4l.drain();
    EXPECT_FALSE(poll.empty());
    EXPECT_TRUE(std::ranges::all_of(poll, [](Received const& r) { return r.transport_specific == 0; }));
    client->reset();
    std::filesystem::remove_all(dir);
}

// TIME_PROPERTIES_DATA_SET answers after PARENT_DATA_SET: its change (timeTraceable, say) is reported too
TEST(ptp_management, a_change_of_time_properties_is_reported)
{
    auto const dir = socket_dir("properties");
    Ptp4l ptp4l{dir};
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(PtpManagementConfig{.uds_address = ptp4l.path}, [&](PtpManagementState const& state) {
        ++calls;
        last = state;
    });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 0);
    int const before = calls;
    std::vector<uint8_t> const traceable{0x00, 0x25, 0x1C, 0x20};  // ptpTimescale | utcOffsetValid | timeTraceable
    c.handle(response(management_id::time_properties_data_set, traceable), 10);
    EXPECT_EQ(calls, before + 1);
    EXPECT_TRUE(last.time_properties && (last.time_properties->flags & time_flag::time_traceable) != 0);
    c.handle(response(management_id::time_properties_data_set, traceable), 20);
    EXPECT_EQ(calls, before + 1);  // the same again: no report
    client->reset();
    std::filesystem::remove_all(dir);
}

// A port's notification carries that port's transportSpecific (port.c port_management_construct),
// which the client does not adopt: it keeps the UDS port's, from its first answer
TEST(ptp_management, a_port_notification_leaves_the_transport_specific)
{
    auto const dir = socket_dir("ts");
    Ptp4l ptp4l{dir};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .port_interface = "enp8s0"}, [](PtpManagementState const&) {});
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 0);  // the UDS port: transportSpecific 0
    c.handle(response(management_id::port_properties_np, port_properties(2, "enp8s0")), 1);
    auto slave = port_data(2, int64_t{74} * 65536);
    slave[10] = 0x09;
    c.handle(response(management_id::port_data_set, slave, 0, 1), 2);  // the gPTP port: 1
    EXPECT_TRUE(c.state().port_state == PortState::slave);
    (void)ptp4l.drain();
    c.tick(3 * SECOND);  // a poll
    auto const poll = ptp4l.drain();
    EXPECT_FALSE(poll.empty());
    EXPECT_TRUE(std::ranges::all_of(poll, [](Received const& r) { return r.transport_specific == 0; }));
    client->reset();
    std::filesystem::remove_all(dir);
}

// On the read-only socket ptp4l refuses SUBSCRIBE_EVENTS_NP: no refusal of ours is reported, the
// status waits for PARENT_DATA_SET, and with no notifications the poll asks for it too
TEST(ptp_management, without_a_subscription_the_parent_is_polled)
{
    auto const dir = socket_dir("ro");
    Ptp4l ptp4l{dir};
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .subscription_duration_s = 60},
        [&](PtpManagementState const& state) { last = state; });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(response(management_id::time_properties_data_set, {0x00, 0x25, 0x0C, 0x20}), 0);
    EXPECT_TRUE(c.state().status == PtpManagementStatus::unreachable);  // no grandmaster known yet
    auto error = response(0, {});
    error.resize(48);
    error.insert(error.end(), {0x00, 0x02, 0x00, 0x08, 0x00, 0x03, 0xC0, 0x03, 0x00, 0x00, 0x00, 0x00});  // NOT_SUPPORTED
    error[3] = static_cast<uint8_t>(error.size());
    c.handle(error, 1);
    EXPECT_FALSE(last.refused.has_value());
    EXPECT_FALSE(c.state().subscribed);
    c.handle(PARENT_RESPONSE, 2);
    EXPECT_TRUE(last.status == PtpManagementStatus::locked);
    (void)ptp4l.drain();
    c.tick(2 * SECOND);
    auto const poll = ptp4l.drain();
    EXPECT_EQ(count(poll, management_id::parent_data_set, ManagementAction::get), 1);
    EXPECT_EQ(count(poll, management_id::time_properties_data_set, ManagementAction::get), 1);
    client->reset();
    std::filesystem::remove_all(dir);
}

// The followed port's asCapable: CMLDS_INFO_NP is fetched once and then notified (NOTIFY_CMLDS, named by
// the header's sourcePortIdentity); another port's is not the followed one's; a state change of the
// followed port fetches it once, and DISABLED or FAULTY drops it (linuxptp keeps it then); nothing polls it
TEST(ptp_management, client_follows_the_named_port_s_as_capable_from_notifications)
{
    auto const dir = socket_dir("ascapable");
    Ptp4l ptp4l{dir};
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = ptp4l.path, .port_interface = "enp8s0"},
        [&](PtpManagementState const& state) { last = state; });
    EXPECT_TRUE(client.has_value());
    if (!client) {
        return;
    }
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 10);
    auto const fetch = ptp4l.drain();
    EXPECT_EQ(count(fetch, management_id::cmlds_info_np, ManagementAction::get), 1);
    c.handle(held_subscription(6, true), 11);
    EXPECT_TRUE(c.state().subscribed);  // NOTIFY_CMLDS held with the others
    c.handle(response(management_id::port_properties_np, port_properties(2, "enp8s0")), 12);
    c.handle(PORT_RESPONSE, 13);
    auto const from_port = [](uint16_t const port, uint8_t const as_capable) {
        std::vector<uint8_t> data(CmldsInfoNp::SIZE);  // meanLinkDelay 0, rate ratio 0
        data.back() = as_capable;
        auto out = response(management_id::cmlds_info_np, data);
        std::array<uint8_t, 8> const bridge{0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00};  // BRIDGE
        std::copy(bridge.begin(), bridge.end(), out.begin() + 20);
        out[29] = static_cast<uint8_t>(port);
        return out;
    };
    c.handle(from_port(1, 1), 14);
    EXPECT_FALSE(last.as_capable.has_value());
    c.handle(from_port(2, 1), 15);
    EXPECT_TRUE(last.as_capable == std::optional{AsCapable::capable});
    c.handle(from_port(2, 0), 16);
    EXPECT_TRUE(last.as_capable == std::optional{AsCapable::not_capable});
    c.handle(from_port(2, 2), 17);  // asCapable true: always
    EXPECT_TRUE(last.as_capable == std::optional{AsCapable::always_capable});
    c.handle(from_port(2, 1), 18);
    (void)ptp4l.drain();

    // DISABLED: linuxptp keeps asCapable as it was, so the client drops it without asking.
    auto const port_in = [](PortState const state) {
        auto data = port_data(2, int64_t{74} * 65536);
        data[10] = static_cast<uint8_t>(state);
        return response(management_id::port_data_set, data);
    };
    c.handle(port_in(PortState::disabled), 20);
    EXPECT_TRUE(last.as_capable == std::optional{AsCapable::not_capable});
    EXPECT_EQ(count(ptp4l.drain(), management_id::cmlds_info_np, ManagementAction::get), 0);
    c.handle(port_in(PortState::disabled), 21);   // the same state: nothing
    c.handle(port_in(PortState::listening), 22);  // another change: fetched once
    EXPECT_EQ(count(ptp4l.drain(), management_id::cmlds_info_np, ManagementAction::get), 1);
    c.tick(3 * SECOND);
    EXPECT_EQ(count(ptp4l.drain(), management_id::cmlds_info_np, ManagementAction::get), 0);
    client->reset();
    std::filesystem::remove_all(dir);
}

#endif  // __linux__

TEST_MAIN(statusbar_ptpclient, ptpclient_management_test)
