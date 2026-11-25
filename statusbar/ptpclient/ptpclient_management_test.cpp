// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The ptp4l management messages, built and parsed, and the judgment of a
// state: no socket is opened here. The client against a stand-in ptp4l is in
// ptpclient_management_functional_test.cpp.

#include "statusbar/ptpclient/ptpclient_management.hpp"

#include "statusbar/ptpclient/ptpclient_management_test_support.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace statusbar;
using namespace statusbar::ptpclient;
using namespace statusbar::ptpclient::management_test_support;

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

TEST_MAIN(statusbar_ptpclient, ptpclient_management_test)
