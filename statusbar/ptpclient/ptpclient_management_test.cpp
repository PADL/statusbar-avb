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

/// A RESPONSE carrying `data` for `id`, in `domain`, built as the bytes above are.
auto response(uint16_t const id, std::vector<uint8_t> const& data, uint8_t const domain = 0) -> std::vector<uint8_t>
{
    std::vector<uint8_t> out(PARENT_RESPONSE.begin(), PARENT_RESPONSE.begin() + 48);
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
    size_t const n = build_management_get(out, management_id::parent_data_set, 0, 7, me);
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
    EXPECT_EQ(build_management_get(std::span<uint8_t>{out}.first(53), management_id::parent_data_set, 0, 7, me), size_t{0});
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
    size_t const n =
        build_management_message(out, ManagementAction::set, management_id::grandmaster_settings_np, encode(settings), 127, 9, me);
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
            0,
            0,
            me),
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
    size_t const n = build_management_message(out, ManagementAction::set, management_id::subscribe_events_np, data, 0, 1, {});
    EXPECT_EQ(n, MANAGEMENT_SET_MAX_SIZE);
    auto const parsed = parse_management_message(std::span<uint8_t const>{out.data(), n});
    EXPECT_TRUE(parsed.has_value());
    auto const* back = std::get_if<SubscribeEventsNp>(&parsed->data);
    EXPECT_TRUE(back != nullptr);
    EXPECT_EQ(back->duration_s, uint16_t{180});
    EXPECT_TRUE(back->has(ManagementEvent::parent_data_set));
    // odd data is padded to an even TLV
    std::array<uint8_t, 3> const odd{1, 2, 3};
    EXPECT_EQ(build_management_message(out, ManagementAction::set, 0xC0FF, odd, 0, 1, {}), MANAGEMENT_GET_SIZE + 4);
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

}  // namespace

TEST(ptp_management, client_reports_status_grandmaster_and_silence)
{
    auto const dir = socket_dir("status");
    auto const uds = (dir / "ptp4l").string();  // nothing listens there
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = uds, .domain = 0, .period_ns = 1'000'000'000, .missed_periods = 3},
        [&](PtpManagementState const& state) {
            ++calls;
            last = state;
        });
    EXPECT_TRUE(client.has_value());
    auto& c = **client;
    EXPECT_TRUE(std::filesystem::exists(dir / std::format("statusbar-ptpmgmt.{}", ::getpid())));  // pmc-style path

    // No ptp4l: the send's error is recorded once, the status unreachable.
    c.tick(0);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(last.status == PtpManagementStatus::unreachable);
    EXPECT_TRUE(
        last.error == std::make_error_code(std::errc::no_such_file_or_directory) ||
        last.error == std::make_error_code(std::errc::connection_refused));
    c.tick(1'000'000'000);
    EXPECT_EQ(calls, 1);  // the same error is not reported again

    // ptp4l answers: locked to the grandmaster, the error cleared.
    c.handle(PARENT_RESPONSE, 1'000'000'010);
    EXPECT_EQ(calls, 2);
    EXPECT_TRUE(last.status == PtpManagementStatus::locked);
    EXPECT_TRUE(last.grandmaster == GM);
    EXPECT_FALSE(last.error);
    c.handle(PARENT_RESPONSE, 1'000'000'020);  // unchanged: no call
    EXPECT_EQ(calls, 2);
    c.handle(response(management_id::current_data_set, std::vector<uint8_t>(18)), 1'000'000'030);
    c.handle(response(management_id::time_properties_data_set, {0x00, 0x25, 0x0C, 0x20}), 1'000'000'040);
    EXPECT_EQ(calls, 2);  // kept, but not a change
    EXPECT_TRUE(c.state().current.has_value() && c.state().time_properties.has_value());

    // ptp4l's PARENT_DATA_SET names its own clock: it follows no grandmaster.
    ieee::Eui64 const self{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0xAA, 0xBB, 0xCC};
    c.handle(response(management_id::parent_data_set, parent_data(self)), 1'000'000'050);
    EXPECT_EQ(calls, 3);
    EXPECT_TRUE(last.status == PtpManagementStatus::no_grandmaster);
    EXPECT_TRUE(last.grandmaster == self);

    // Silent for more than three periods: unreachable, everything learnt forgotten.
    c.tick(4'500'000'000);
    EXPECT_TRUE(last.status == PtpManagementStatus::unreachable);
    EXPECT_FALSE(c.state().grandmaster.has_value());
    EXPECT_FALSE(c.state().parent.has_value());
    EXPECT_FALSE(c.state().current.has_value());
    EXPECT_FALSE(c.state().time_properties.has_value());
    EXPECT_FALSE(c.finished());

    client->reset();
    EXPECT_FALSE(std::filesystem::exists(dir / std::format("statusbar-ptpmgmt.{}", ::getpid())));  // unlinked
    std::filesystem::remove_all(dir);
}

TEST(ptp_management, client_follows_the_named_port_s_peer_delay)
{
    auto const dir = socket_dir("port");
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = (dir / "ptp4l").string(), .port_interface = "enp8s0"},
        [&](PtpManagementState const& state) {
            ++calls;
            last = state;
        });
    EXPECT_TRUE(client.has_value());
    auto& c = **client;
    c.handle(PARENT_RESPONSE, 10);
    EXPECT_EQ(calls, 1);

    // A PORT_DATA_SET before PORT_PROPERTIES_NP names the port: not ours yet.
    c.handle(PORT_RESPONSE, 20);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(c.state().peer_mean_path_delay_ns.has_value());

    // ptp4l's ports, as pmc -b 0 sees them: port 1 ens4, port 2 enp8s0.
    c.handle(response(management_id::port_properties_np, port_properties(1, "ens4")), 30);
    c.handle(response(management_id::port_properties_np, port_properties(2, "enp8s0")), 31);
    EXPECT_TRUE((c.state().port == gptp::SourcePortIdentity{BRIDGE, 2}));
    EXPECT_EQ(calls, 1);                                                                       // the port alone is not a change
    c.handle(response(management_id::port_data_set, port_data(1, int64_t{900} * 65536)), 40);  // ens4's: ignored
    EXPECT_EQ(calls, 1);
    c.handle(PORT_RESPONSE, 41);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(last.peer_mean_path_delay_ns.value_or(0), int64_t{74});
    EXPECT_TRUE(last.port_state == PortState::master);
    EXPECT_TRUE(last.delay_mechanism == DelayMechanism::p2p);
    EXPECT_TRUE(last.grandmaster == GM);

    // The same delay to the ns: no call; a new one: a call.
    c.handle(response(management_id::port_data_set, port_data(2, int64_t{74} * 65536 + 100)), 50);
    EXPECT_EQ(calls, 2);
    c.handle(response(management_id::port_data_set, port_data(2, int64_t{77} * 65536)), 60);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(last.peer_mean_path_delay_ns.value_or(0), int64_t{77});

    // Silence forgets the port with the rest.
    c.tick(0);
    c.tick(10'000'000'000);
    EXPECT_FALSE(c.state().port.has_value());
    EXPECT_FALSE(last.peer_mean_path_delay_ns.has_value());
    client->reset();
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
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.missed_periods = 0}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.period_ns = 0}, listener).has_value());
    EXPECT_FALSE(PtpManagementClient::create(PtpManagementConfig{.subscription_duration_s = 1}, listener).has_value());
}

TEST(ptp_management, client_talks_to_a_ptp4l_socket)
{
    // A stand-in for ptp4l: a datagram socket at a path, answering each GET
    // PARENT_DATA_SET with the response above.
    auto const dir = socket_dir("talk");
    auto const path = (dir / "ptp4l").string();
    int const server = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    EXPECT_TRUE(server >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    EXPECT_EQ(::bind(server, reinterpret_cast<sockaddr const*>(&address), sizeof(address)), 0);

    int calls = 0;
    PtpManagementState seen{};
    auto client = PtpManagementClient::create(PtpManagementConfig{.uds_address = path}, [&](PtpManagementState const& state) {
        ++calls;
        seen = state;
    });
    EXPECT_TRUE(client.has_value());
    (*client)->tick(0);  // three GETs
    EXPECT_FALSE((*client)->state().error);

    int gets = 0;
    for (int i = 0; i < 3; ++i) {
        std::array<uint8_t, 128> in{};
        sockaddr_un from{};
        socklen_t from_length = sizeof(from);
        ssize_t const n = ::recvfrom(server, in.data(), in.size(), 0, reinterpret_cast<sockaddr*>(&from), &from_length);
        EXPECT_TRUE(n > 0);
        auto const get = parse_management_message(std::span<uint8_t const>{in.data(), static_cast<size_t>(n)});
        EXPECT_TRUE(get.has_value());
        EXPECT_TRUE(get->action == ManagementAction::get);
        ++gets;
        if (get->management_id == management_id::parent_data_set) {
            EXPECT_EQ(
                ::sendto(
                    server,
                    PARENT_RESPONSE.data(),
                    PARENT_RESPONSE.size(),
                    0,
                    reinterpret_cast<sockaddr const*>(&from),
                    from_length),
                static_cast<ssize_t>(PARENT_RESPONSE.size()));
        }
    }
    EXPECT_EQ(gets, 3);
    (*client)->on_ready(1);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(seen.status == PtpManagementStatus::locked);
    EXPECT_TRUE(seen.grandmaster == GM);
    EXPECT_FALSE((*client)->finished());

    // Following a port adds PORT_PROPERTIES_NP, then PORT_DATA_SET.
    std::vector<uint16_t> ids;
    auto follower = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = path, .port_interface = "enp8s0"}, [](PtpManagementState const&) {});
    EXPECT_TRUE(follower.has_value());
    (*follower)->tick(0);
    for (int i = 0; i < 5; ++i) {
        std::array<uint8_t, 128> in{};
        ssize_t const n = ::recv(server, in.data(), in.size(), 0);
        EXPECT_TRUE(n > 0);
        auto const get =
            parse_management_message(std::span<uint8_t const>{in.data(), static_cast<size_t>(std::max<ssize_t>(n, 0))});
        ids.push_back(get ? get->management_id : 0);
    }
    EXPECT_TRUE(
        (ids ==
         std::vector<uint16_t>{
             management_id::parent_data_set,
             management_id::current_data_set,
             management_id::time_properties_data_set,
             management_id::port_properties_np,
             management_id::port_data_set}));
    follower->reset();
    ::close(server);
    std::filesystem::remove_all(dir);
}

TEST(ptp_management, client_reports_notifications_settings_and_refusals)
{
    auto const dir = socket_dir("notify");
    int calls = 0;
    PtpManagementState last{};
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = (dir / "ptp4l").string(), .port_interface = "enp8s0"},
        [&](PtpManagementState const& state) {
            ++calls;
            last = state;
        });
    EXPECT_TRUE(client.has_value());
    auto& c = **client;
    c.handle(response(management_id::port_properties_np, port_properties(2, "enp8s0")), 10);
    c.handle(PORT_RESPONSE, 11);
    EXPECT_TRUE(last.port_state == PortState::master);
    int const before = calls;

    // A pushed PORT_DATA_SET (NOTIFY_PORT_STATE): the port is SLAVE now.
    auto slave = port_data(2, int64_t{74} * 65536);
    slave[10] = 0x09;
    c.handle(response(management_id::port_data_set, slave), 20);
    EXPECT_EQ(calls, before + 1);
    EXPECT_TRUE(last.port_state == PortState::slave);

    // TIME_STATUS_NP is kept but reports nothing by itself.
    c.handle(response(management_id::time_status_np, std::vector<uint8_t>(TimeStatusNp::SIZE)), 30);
    EXPECT_EQ(calls, before + 1);
    EXPECT_TRUE(c.state().time_status.has_value());

    // A SET acknowledged, the SUBSCRIBE acknowledged, then a refusal.
    GrandmasterSettingsNp const settings{.clock_class = 13, .clock_accuracy = 0x21};
    auto const settings_data = encode(settings);
    c.handle(response(management_id::grandmaster_settings_np, {settings_data.begin(), settings_data.end()}), 40);
    EXPECT_EQ(calls, before + 2);
    EXPECT_TRUE(last.grandmaster_settings == settings);
    auto const subscribe_data = encode(SubscribeEventsNp{.duration_s = 180});
    c.handle(response(management_id::subscribe_events_np, {subscribe_data.begin(), subscribe_data.end()}), 50);
    EXPECT_EQ(calls, before + 3);
    EXPECT_TRUE(last.subscribed);
    auto error = response(0, {});
    error.resize(48);
    error.insert(error.end(), {0x00, 0x02, 0x00, 0x08, 0x00, 0x05, 0xC0, 0x01, 0x00, 0x00, 0x00, 0x00});  // NOT_SETABLE
    error[3] = static_cast<uint8_t>(error.size());
    c.handle(error, 60);
    EXPECT_EQ(calls, before + 4);
    EXPECT_EQ(last.refused.value_or(0), uint16_t{5});
    // a refused GET is not a refusal of ours
    error[54] = 0x20;
    error[55] = 0x02;
    c.handle(error, 70);
    EXPECT_EQ(calls, before + 4);
    client->reset();
    std::filesystem::remove_all(dir);
}

TEST(ptp_management, client_subscribes_and_resends_its_settings_to_a_ptp4l_socket)
{
    auto const dir = socket_dir("subscribe");
    auto const path = (dir / "ptp4l").string();
    int const server = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    EXPECT_TRUE(server >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    EXPECT_EQ(::bind(server, reinterpret_cast<sockaddr const*>(&address), sizeof(address)), 0);

    /// Every message the stand-in has received, by managementId and action.
    auto drain = [&] {
        std::vector<std::pair<uint16_t, ManagementAction>> seen;
        sockaddr_un from{};
        for (;;) {
            std::array<uint8_t, 256> in{};
            socklen_t from_length = sizeof(from);
            ssize_t const n = ::recvfrom(server, in.data(), in.size(), 0, reinterpret_cast<sockaddr*>(&from), &from_length);
            if (n <= 0) {
                return std::pair{seen, from};
            }
            auto const message = parse_management_message(std::span<uint8_t const>{in.data(), static_cast<size_t>(n)});
            EXPECT_TRUE(message.has_value());
            if (message) {
                seen.emplace_back(message->management_id, message->action);
                if (auto const* s = std::get_if<SubscribeEventsNp>(&message->data)) {
                    EXPECT_EQ(s->duration_s, uint16_t{60});
                    EXPECT_TRUE(s->has(ManagementEvent::port_state) && s->has(ManagementEvent::parent_data_set));
                }
            }
        }
    };
    auto const count = [](auto const& seen, uint16_t const id, ManagementAction const action) {
        return std::ranges::count(seen, std::pair{id, action});
    };

    SubscribeEventsNp events{};
    events.add(ManagementEvent::port_state);
    events.add(ManagementEvent::parent_data_set);
    auto client = PtpManagementClient::create(
        PtpManagementConfig{.uds_address = path, .period_ns = 1'000'000'000, .events = events, .subscription_duration_s = 60},
        [](PtpManagementState const&) {});
    EXPECT_TRUE(client.has_value());
    auto& c = **client;

    c.tick(0);  // GETs and the subscription
    auto [first, from] = drain();
    EXPECT_EQ(count(first, management_id::subscribe_events_np, ManagementAction::set), 1);
    EXPECT_EQ(count(first, management_id::parent_data_set, ManagementAction::get), 1);

    GrandmasterSettingsNp const settings{.clock_class = 13, .clock_accuracy = 0x22};
    EXPECT_TRUE(c.set_grandmaster_settings(settings).has_value());
    EXPECT_EQ(count(drain().first, management_id::grandmaster_settings_np, ManagementAction::set), 1);

    // The first answer after silence resends them: the SET may have preceded ptp4l.
    c.handle(PARENT_RESPONSE, 1);
    EXPECT_EQ(count(drain().first, management_id::grandmaster_settings_np, ManagementAction::set), 1);

    // ptp4l keeps answering: nothing resent, the subscription renewed at half its duration.
    int64_t subscriptions = 0;
    int64_t settings_sent = 0;
    for (int64_t second = 1; second <= 31; ++second) {
        c.handle(PARENT_RESPONSE, (second * 1'000'000'000) - 1);
        c.tick(second * 1'000'000'000);
        auto const seen = drain().first;
        subscriptions += count(seen, management_id::subscribe_events_np, ManagementAction::set);
        settings_sent += count(seen, management_id::grandmaster_settings_np, ManagementAction::set);
    }
    EXPECT_EQ(subscriptions, int64_t{1});
    EXPECT_EQ(settings_sent, int64_t{0});

    // Silent, then back (a restarted ptp4l): subscription and settings are sent again.
    c.tick(34'000'000'000);
    EXPECT_TRUE(c.state().status == PtpManagementStatus::unreachable);
    EXPECT_FALSE(c.state().subscribed);
    (void)drain();
    c.tick(35'000'000'000);
    EXPECT_EQ(count(drain().first, management_id::subscribe_events_np, ManagementAction::set), 1);
    c.handle(PARENT_RESPONSE, 35'000'000'001);
    auto const back = drain().first;
    EXPECT_EQ(count(back, management_id::grandmaster_settings_np, ManagementAction::set), 1);
    EXPECT_EQ(count(back, management_id::subscribe_events_np, ManagementAction::set), 1);

    // A restart between two queries: ptp4l's GETs show its defaults, and both are sent again.
    c.tick(36'000'000'000);
    auto const gets = drain().first;
    EXPECT_EQ(count(gets, management_id::grandmaster_settings_np, ManagementAction::get), 1);
    EXPECT_EQ(count(gets, management_id::subscribe_events_np, ManagementAction::get), 1);
    auto const defaults = encode(GrandmasterSettingsNp{});
    c.handle(response(management_id::grandmaster_settings_np, {defaults.begin(), defaults.end()}), 36'000'000'001);
    auto const nothing = encode(SubscribeEventsNp{.duration_s = 0});
    c.handle(response(management_id::subscribe_events_np, {nothing.begin(), nothing.end()}), 36'000'000'002);
    EXPECT_FALSE(c.state().subscribed);
    auto const again = drain().first;
    EXPECT_EQ(count(again, management_id::grandmaster_settings_np, ManagementAction::set), 1);
    EXPECT_EQ(count(again, management_id::subscribe_events_np, ManagementAction::set), 1);
    // ptp4l holding ours: nothing more
    auto const held = encode(settings);
    c.handle(response(management_id::grandmaster_settings_np, {held.begin(), held.end()}), 36'000'000'003);
    auto mine = events;
    mine.duration_s = 59;
    auto const mine_data = encode(mine);
    c.handle(response(management_id::subscribe_events_np, {mine_data.begin(), mine_data.end()}), 36'000'000'004);
    EXPECT_TRUE(c.state().subscribed);
    EXPECT_TRUE(c.state().grandmaster_settings == settings);
    EXPECT_TRUE(drain().first.empty());
    (void)from;
    client->reset();
    ::close(server);
    std::filesystem::remove_all(dir);
}

#endif  // __linux__

TEST_MAIN(statusbar_ptpclient, ptpclient_management_test)
