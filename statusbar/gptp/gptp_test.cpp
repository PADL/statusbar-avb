// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for gPTP module
// Tests IEEE 802.1AS gPTP message structures

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/gptp/gptp_format.hpp"
#include "statusbar/gptp/gptp_time_bridge.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <print>
#include <span>

using namespace statusbar::gptp;

// gPTP header length constants
static_assert(HEADER_LENGTH == 34);
static_assert(SYNC_MESSAGE_LENGTH == 44);
static_assert(FOLLOW_UP_MESSAGE_LENGTH == 44);
static_assert(PDELAY_REQ_MESSAGE_LENGTH == 54);
static_assert(PDELAY_RESP_MESSAGE_LENGTH == 54);

// ClockIdentity tests

TEST(clock_identity, default_constructor)
{
    ClockIdentity id;
    EXPECT_EQ(id.to_uint64(), 0U);
}

TEST(clock_identity, value_constructor)
{
    ClockIdentity id(0x0011223344556677ULL);
    EXPECT_EQ(id.to_uint64(), 0x0011223344556677ULL);
}

TEST(clock_identity, comparison)
{
    ClockIdentity a(0x1234567890ABCDEFULL);
    ClockIdentity b(0x1234567890ABCDEFULL);
    ClockIdentity c(0x0000000000000001ULL);

    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
    EXPECT_TRUE(c < a);
}

TEST(clock_identity, serialization)
{
    ClockIdentity id(0x0011223344556677ULL);

    std::array<uint8_t, 8> buf{};
    auto stored = store_unchecked(buf, id);
    EXPECT_EQ(stored, 8U);

    ClockIdentity loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 8U);

    EXPECT_TRUE(loaded == id);
}

// SourcePortIdentity tests

TEST(source_port_identity, default_constructor)
{
    SourcePortIdentity spi;
    EXPECT_EQ(spi.clock_identity.to_uint64(), 0U);
    EXPECT_EQ(spi.port_number.get(), 0U);
}

TEST(source_port_identity, value_constructor)
{
    ClockIdentity clock_id(0x1234567890ABCDEFULL);
    SourcePortIdentity spi(clock_id, 1);

    EXPECT_EQ(spi.clock_identity.to_uint64(), 0x1234567890ABCDEFULL);
    EXPECT_EQ(spi.port_number.get(), 1U);
}

TEST(source_port_identity, serialization)
{
    ClockIdentity clock_id(0x1234567890ABCDEFULL);
    SourcePortIdentity spi(clock_id, 42);

    std::array<uint8_t, 10> buf{};
    auto stored = store_unchecked(buf, spi);
    EXPECT_EQ(stored, 10U);

    SourcePortIdentity loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 10U);

    EXPECT_TRUE(loaded == spi);
}

// Timestamp tests

TEST(timestamp, default_constructor)
{
    Timestamp ts;
    EXPECT_EQ(ts.seconds(), 0U);
    EXPECT_EQ(ts.nanos(), 0U);
}

TEST(timestamp, value_constructor)
{
    Timestamp ts(12345678901234ULL, 500000000);
    EXPECT_EQ(ts.seconds(), 12345678901234ULL);
    EXPECT_EQ(ts.nanos(), 500000000U);
}

TEST(timestamp, setters)
{
    Timestamp ts;
    ts.set_seconds(999888777666ULL);
    ts.set_nanos(123456789);

    EXPECT_EQ(ts.seconds(), 999888777666ULL);
    EXPECT_EQ(ts.nanos(), 123456789U);
}

TEST(timestamp, serialization)
{
    Timestamp ts(12345678901234ULL, 500000000);

    std::array<uint8_t, 10> buf{};
    auto stored = store_unchecked(buf, ts);
    EXPECT_EQ(stored, 10U);

    Timestamp loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 10U);

    EXPECT_EQ(loaded.seconds(), ts.seconds());
    EXPECT_EQ(loaded.nanos(), ts.nanos());
}

// ClockQuality tests

TEST(clock_quality, default_constructor)
{
    ClockQuality cq;
    EXPECT_EQ(cq.clock_class.get(), 0);
    EXPECT_EQ(cq.clock_accuracy.get(), 0);
    EXPECT_EQ(cq.offset_scaled_log_variance.get(), 0);
}

TEST(clock_quality, value_constructor)
{
    ClockQuality cq(248, 0x21, 0x4100);
    EXPECT_EQ(cq.clock_class.get(), 248);
    EXPECT_EQ(cq.clock_accuracy.get(), 0x21);
    EXPECT_EQ(cq.offset_scaled_log_variance.get(), 0x4100);
}

TEST(clock_quality, serialization)
{
    ClockQuality cq(248, 0x21, 0x4100);

    std::array<uint8_t, 4> buf{};
    auto stored = store_unchecked(buf, cq);
    EXPECT_EQ(stored, 4U);

    ClockQuality loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 4U);

    EXPECT_TRUE(loaded == cq);
}

// MessageHeader tests

TEST(message_header, default_constructor)
{
    MessageHeader hdr;
    EXPECT_EQ(sizeof(hdr), 34U);
}

TEST(message_header, init_sync)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);

    EXPECT_EQ(hdr.major_sdo_id(), SDO_ID);
    EXPECT_EQ(hdr.message_type(), MESSAGE_TYPE_SYNC);
    EXPECT_EQ(hdr.version_ptp(), VERSION_PTP);
    EXPECT_EQ(hdr.message_length, 44U);
    EXPECT_EQ(hdr.sequence_id, 100U);
    EXPECT_TRUE(hdr.is_sync());
}

TEST(message_header, init_follow_up)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_FOLLOW_UP, 44, 200);

    EXPECT_EQ(hdr.message_type(), MESSAGE_TYPE_FOLLOW_UP);
    EXPECT_TRUE(hdr.is_follow_up());
    EXPECT_FALSE(hdr.is_sync());
}

TEST(message_header, field_setters)
{
    MessageHeader hdr;

    hdr.set_major_sdo_id(1);
    hdr.set_message_type(MESSAGE_TYPE_PDELAY_REQ);
    hdr.set_minor_version_ptp(1);
    hdr.set_version_ptp(2);

    EXPECT_EQ(hdr.major_sdo_id(), 1);
    EXPECT_EQ(hdr.message_type(), MESSAGE_TYPE_PDELAY_REQ);
    EXPECT_EQ(hdr.minor_version_ptp(), 1);
    EXPECT_EQ(hdr.version_ptp(), 2);
    EXPECT_TRUE(hdr.is_pdelay_req());
}

TEST(message_header, serialization)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 12345);
    hdr.source_port_identity = SourcePortIdentity(ClockIdentity(0xAABBCCDDEEFF0011ULL), 1);

    std::array<uint8_t, 34> buf{};
    auto stored = store_unchecked(buf, hdr);
    EXPECT_EQ(stored, 34U);

    MessageHeader loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 34U);

    EXPECT_EQ(loaded.message_type(), MESSAGE_TYPE_SYNC);
    EXPECT_EQ(loaded.sequence_id, 12345U);
    EXPECT_TRUE(loaded.source_port_identity == hdr.source_port_identity);
}

TEST(message_header, can_load)
{
    std::array<uint8_t, 34> buf{};
    MessageHeader hdr;

    auto result = statusbar::protocol::can_load(buf, &hdr);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 34U);

    std::array<uint8_t, 33> small_buf{};
    auto fail_result = statusbar::protocol::can_load(small_buf, &hdr);
    EXPECT_FALSE(fail_result.has_value());
}

// SyncMessage tests

TEST(sync_message, default_constructor)
{
    SyncMessage msg;
    EXPECT_EQ(sizeof(msg), 44U);
}

TEST(sync_message, init)
{
    SyncMessage msg;
    msg.init(1000);

    EXPECT_TRUE(msg.header.is_sync());
    EXPECT_EQ(msg.header.message_length, 44U);
    EXPECT_EQ(msg.header.sequence_id, 1000U);
}

TEST(sync_message, serialization)
{
    SyncMessage msg;
    msg.init(500);
    msg.origin_timestamp = Timestamp(1234567890, 123456789);

    std::array<uint8_t, 44> buf{};
    auto stored = store_unchecked(buf, msg);
    EXPECT_EQ(stored, 44U);

    SyncMessage loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 44U);

    EXPECT_TRUE(loaded.header.is_sync());
    EXPECT_EQ(loaded.origin_timestamp.seconds(), 1234567890U);
    EXPECT_EQ(loaded.origin_timestamp.nanos(), 123456789U);
}

// FollowUpMessage tests

TEST(follow_up_message, default_constructor)
{
    FollowUpMessage msg;
    EXPECT_EQ(sizeof(msg), 44U);
}

TEST(follow_up_message, init)
{
    FollowUpMessage msg;
    msg.init(2000);

    EXPECT_TRUE(msg.header.is_follow_up());
    EXPECT_EQ(msg.header.message_length, 44U);
    EXPECT_EQ(msg.header.sequence_id, 2000U);
}

TEST(follow_up_message, serialization)
{
    FollowUpMessage msg;
    msg.init(600);
    msg.precise_origin_timestamp = Timestamp(9876543210, 987654321);

    std::array<uint8_t, 44> buf{};
    auto stored = store_unchecked(buf, msg);
    EXPECT_EQ(stored, 44U);

    FollowUpMessage loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 44U);

    EXPECT_TRUE(loaded.header.is_follow_up());
    EXPECT_EQ(loaded.precise_origin_timestamp.seconds(), 9876543210U);
    EXPECT_EQ(loaded.precise_origin_timestamp.nanos(), 987654321U);
}

// PdelayReqMessage tests

TEST(pdelay_req_message, default_constructor)
{
    PdelayReqMessage msg;
    EXPECT_EQ(sizeof(msg), 54U);
}

TEST(pdelay_req_message, init)
{
    PdelayReqMessage msg;
    msg.init(3000);

    EXPECT_TRUE(msg.header.is_pdelay_req());
    EXPECT_EQ(msg.header.message_length, 54U);
    EXPECT_EQ(msg.header.sequence_id, 3000U);
}

TEST(pdelay_req_message, serialization)
{
    PdelayReqMessage msg;
    msg.init(700);

    std::array<uint8_t, 54> buf{};
    auto stored = store_unchecked(buf, msg);
    EXPECT_EQ(stored, 54U);

    PdelayReqMessage loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 54U);

    EXPECT_TRUE(loaded.header.is_pdelay_req());
}

// PdelayRespMessage tests

TEST(pdelay_resp_message, default_constructor)
{
    PdelayRespMessage msg;
    EXPECT_EQ(sizeof(msg), 54U);
}

TEST(pdelay_resp_message, init)
{
    PdelayRespMessage msg;
    msg.init(4000);

    EXPECT_TRUE(msg.header.is_pdelay_resp());
    EXPECT_EQ(msg.header.message_length, 54U);
}

TEST(pdelay_resp_message, serialization)
{
    PdelayRespMessage msg;
    msg.init(800);
    msg.request_receipt_timestamp = Timestamp(5555555555, 444444444);
    msg.requesting_port_identity = SourcePortIdentity(ClockIdentity(0x1122334455667788ULL), 2);

    std::array<uint8_t, 54> buf{};
    auto stored = store_unchecked(buf, msg);
    EXPECT_EQ(stored, 54U);

    PdelayRespMessage loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 54U);

    EXPECT_TRUE(loaded.header.is_pdelay_resp());
    EXPECT_EQ(loaded.request_receipt_timestamp.seconds(), 5555555555U);
    EXPECT_TRUE(loaded.requesting_port_identity == msg.requesting_port_identity);
}

// PdelayRespFollowUpMessage tests

TEST(pdelay_resp_follow_up_message, default_constructor)
{
    PdelayRespFollowUpMessage msg;
    EXPECT_EQ(sizeof(msg), 54U);
}

TEST(pdelay_resp_follow_up_message, init)
{
    PdelayRespFollowUpMessage msg;
    msg.init(5000);

    EXPECT_TRUE(msg.header.is_pdelay_resp_follow_up());
    EXPECT_EQ(msg.header.message_length, 54U);
}

TEST(pdelay_resp_follow_up_message, serialization)
{
    PdelayRespFollowUpMessage msg;
    msg.init(900);
    msg.response_origin_timestamp = Timestamp(6666666666, 333333333);

    std::array<uint8_t, 54> buf{};
    auto stored = store_unchecked(buf, msg);
    EXPECT_EQ(stored, 54U);

    PdelayRespFollowUpMessage loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 54U);

    EXPECT_TRUE(loaded.header.is_pdelay_resp_follow_up());
    EXPECT_EQ(loaded.response_origin_timestamp.seconds(), 6666666666U);
}

// Constants tests

TEST(constants, ethertype)
{
    EXPECT_EQ(GPTP_ETHERTYPE, 0x88F7);
}

TEST(constants, message_types)
{
    EXPECT_EQ(MESSAGE_TYPE_SYNC, 0);
    EXPECT_EQ(MESSAGE_TYPE_FOLLOW_UP, 8);
    EXPECT_EQ(MESSAGE_TYPE_PDELAY_REQ, 2);
    EXPECT_EQ(MESSAGE_TYPE_PDELAY_RESP, 3);
    EXPECT_EQ(MESSAGE_TYPE_PDELAY_RESP_FOLLOW_UP, 10);
}

TEST(constants, sizes)
{
    EXPECT_EQ(HEADER_LENGTH, 34U);
    EXPECT_EQ(SYNC_MESSAGE_LENGTH, 44U);
    EXPECT_EQ(FOLLOW_UP_MESSAGE_LENGTH, 44U);
    EXPECT_EQ(PDELAY_REQ_MESSAGE_LENGTH, 54U);
    EXPECT_EQ(PDELAY_RESP_MESSAGE_LENGTH, 54U);
}

// MessageHeaderis_valid tests =====

TEST(message_header_valid, valid_sync)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);

    EXPECT_TRUE(hdr.is_valid());
}

TEST(message_header_valid, valid_follow_up)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_FOLLOW_UP, 44, 200);

    EXPECT_TRUE(hdr.is_valid());
}

TEST(message_header_valid, valid_pdelay_req)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_PDELAY_REQ, 54, 300);

    EXPECT_TRUE(hdr.is_valid());
}

TEST(message_header_valid, valid_pdelay_resp)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_PDELAY_RESP, 54, 400);

    EXPECT_TRUE(hdr.is_valid());
}

TEST(message_header_valid, valid_pdelay_resp_follow_up)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_PDELAY_RESP_FOLLOW_UP, 54, 500);

    EXPECT_TRUE(hdr.is_valid());
}

TEST(message_header_valid, valid_announce)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_ANNOUNCE, 64, 600);

    EXPECT_TRUE(hdr.is_valid());
}

TEST(message_header_valid, invalid_wrong_sdo_id)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);
    hdr.set_major_sdo_id(0);  // Should be SDO_ID (1)

    EXPECT_FALSE(hdr.is_valid());
}

TEST(message_header_valid, invalid_wrong_version)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);
    hdr.set_version_ptp(1);  // Should be VERSION_PTP (2)

    EXPECT_FALSE(hdr.is_valid());
}

TEST(message_header_valid, invalid_message_type_4)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);
    hdr.set_message_type(4);  // Type 4-7 are invalid for gPTP

    EXPECT_FALSE(hdr.is_valid());
}

TEST(message_header_valid, invalid_message_type_7)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);
    hdr.set_message_type(7);  // Type 4-7 are invalid for gPTP

    EXPECT_FALSE(hdr.is_valid());
}

TEST(message_header_valid, invalid_message_type_14)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);
    hdr.set_message_type(14);  // Type 14+ are invalid

    EXPECT_FALSE(hdr.is_valid());
}

TEST(message_header_valid, invalid_short_message_length)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 100);
    hdr.message_length = 33;  // Less than header size (34)

    EXPECT_FALSE(hdr.is_valid());
}

TEST(message_header_valid, valid_minimum_message_length)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 34, 100);  // Exactly header size

    EXPECT_TRUE(hdr.is_valid());
}

// MessageHeaderis_announce test =====

TEST(message_header_types, is_announce)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_ANNOUNCE, 64, 100);

    EXPECT_TRUE(hdr.is_announce());
    EXPECT_FALSE(hdr.is_sync());
    EXPECT_FALSE(hdr.is_follow_up());
}

// MessageHeaderoperator== tests =====

TEST(message_header_ops, equality_same_headers)
{
    MessageHeader a;
    a.init(MESSAGE_TYPE_SYNC, 44, 100);
    MessageHeader b;
    b.init(MESSAGE_TYPE_SYNC, 44, 100);

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(message_header_ops, equality_different_seq_id)
{
    MessageHeader a;
    a.init(MESSAGE_TYPE_SYNC, 44, 100);
    MessageHeader b;
    b.init(MESSAGE_TYPE_SYNC, 44, 200);

    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(message_header_ops, equality_different_type)
{
    MessageHeader a;
    a.init(MESSAGE_TYPE_SYNC, 44, 100);
    MessageHeader b;
    b.init(MESSAGE_TYPE_FOLLOW_UP, 44, 100);

    EXPECT_FALSE(a == b);
}

// MessageHeaderformat_to tests =====

TEST(message_header_format, format_sync)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_SYNC, 44, 1234);
    hdr.domain_number = 5;

    std::string result;
    format_to(std::back_inserter(result), hdr);

    EXPECT_TRUE(result.find("Sync") != std::string::npos);
    EXPECT_TRUE(result.find("seq=1234") != std::string::npos);
    EXPECT_TRUE(result.find("domain=5") != std::string::npos);
}

TEST(message_header_format, format_follow_up)
{
    MessageHeader hdr;
    hdr.init(MESSAGE_TYPE_FOLLOW_UP, 44, 5678);

    std::string result;
    format_to(std::back_inserter(result), hdr);

    EXPECT_TRUE(result.find("Follow_Up") != std::string::npos);
    EXPECT_TRUE(result.find("seq=5678") != std::string::npos);
}

// ClockIdentity span tests

TEST(clock_identity_span, const_span)
{
    ClockIdentity id(0x0011223344556677ULL);
    auto const s = id.span();

    EXPECT_EQ(s.size(), 8U);
    EXPECT_EQ(s[0], 0x00);
    EXPECT_EQ(s[7], 0x77);
}

TEST(clock_identity_span, mutable_span)
{
    ClockIdentity id;
    auto s = id.span();
    s[0] = 0xAA;
    s[7] = 0xBB;

    auto value = id.to_uint64();
    EXPECT_EQ((value >> 56) & 0xFF, 0xAA);
    EXPECT_EQ(value & 0xFF, 0xBB);
}

// SourcePortIdentityformat_to tests =====

TEST(source_port_identity_format, format_to)
{
    ClockIdentity clock_id(0x0011223344556677ULL);
    SourcePortIdentity spi(clock_id, 42);

    std::string result;
    format_to(std::back_inserter(result), spi);

    EXPECT_TRUE(result.find("00:11:22:33:44:55:66:77") != std::string::npos);
    EXPECT_TRUE(result.find("42") != std::string::npos);
}

// ===== message_type_name tests =====

TEST(message_type_names, sync)
{
    std::string_view name = message_type_name(MESSAGE_TYPE_SYNC);
    EXPECT_TRUE(name.find("Sync") != std::string_view::npos);
}

TEST(message_type_names, follow_up)
{
    std::string_view name = message_type_name(MESSAGE_TYPE_FOLLOW_UP);
    EXPECT_TRUE(name.find("Follow_Up") != std::string_view::npos);
}

TEST(message_type_names, pdelay_req)
{
    std::string_view name = message_type_name(MESSAGE_TYPE_PDELAY_REQ);
    EXPECT_TRUE(name.find("Pdelay_Req") != std::string_view::npos);
}

TEST(message_type_names, pdelay_resp)
{
    std::string_view name = message_type_name(MESSAGE_TYPE_PDELAY_RESP);
    EXPECT_TRUE(name.find("Pdelay_Resp") != std::string_view::npos);
}

TEST(message_type_names, announce)
{
    std::string_view name = message_type_name(MESSAGE_TYPE_ANNOUNCE);
    EXPECT_TRUE(name.find("Announce") != std::string_view::npos);
}

TEST(message_type_names, unknown)
{
    std::string_view name = message_type_name(0xFF);
    EXPECT_TRUE(name.find("Unknown") != std::string_view::npos);
}

//
// Tests: MessageHeader accessor coverage
//

TEST(gptp_header_accessors, flags)
{
    MessageHeader hdr{};
    hdr.init(MESSAGE_TYPE_SYNC, SYNC_MESSAGE_LENGTH, 1);
    // Set known flag bytes
    hdr.flags.span()[0] = 0x12;
    hdr.flags.span()[1] = 0x34;
    EXPECT_EQ(hdr.flags, 0x1234U);
}

TEST(gptp_header_accessors, correction_field)
{
    MessageHeader hdr{};
    hdr.init(MESSAGE_TYPE_SYNC, SYNC_MESSAGE_LENGTH, 1);
    // Set known correction field bytes
    hdr.correction_field_raw.span()[0] = 0x01;
    hdr.correction_field_raw.span()[1] = 0x23;
    hdr.correction_field_raw.span()[2] = 0x45;
    hdr.correction_field_raw.span()[3] = 0x67;
    hdr.correction_field_raw.span()[4] = 0x89;
    hdr.correction_field_raw.span()[5] = 0xAB;
    hdr.correction_field_raw.span()[6] = 0xCD;
    hdr.correction_field_raw.span()[7] = 0xEF;
    EXPECT_EQ(hdr.correction_field(), 0x0123456789ABCDEFLL);
}

TEST(gptp_header_accessors, message_type_specific)
{
    MessageHeader hdr{};
    hdr.init(MESSAGE_TYPE_SYNC, SYNC_MESSAGE_LENGTH, 1);
    hdr.message_type_specific.span()[0] = 0xDE;
    hdr.message_type_specific.span()[1] = 0xAD;
    hdr.message_type_specific.span()[2] = 0xBE;
    hdr.message_type_specific.span()[3] = 0xEF;
    EXPECT_EQ(hdr.message_type_specific, 0xDEADBEEFU);
}

//
// Tests: AnnounceMessage init
//

TEST(gptp_announce, init_defaults)
{
    AnnounceMessage msg;
    msg.init(42);

    EXPECT_EQ(msg.header.message_type(), MESSAGE_TYPE_ANNOUNCE);
    EXPECT_EQ(msg.header.sequence_id, 42U);
    EXPECT_EQ(msg.grandmaster_priority1, 255U);
    EXPECT_EQ(msg.grandmaster_priority2, 255U);
    EXPECT_EQ(msg.steps_removed.get(), 0U);
    EXPECT_EQ(msg.time_source, 0U);
    EXPECT_EQ(msg.current_utc_offset.get(), 0U);
}

//
// Tests: parse_gptp
//

TEST(gptp_parse, sync_message)
{
    // Build a valid Sync message
    SyncMessage sync;
    sync.header.init(MESSAGE_TYPE_SYNC, SYNC_MESSAGE_LENGTH, 100);

    std::array<uint8_t, 128> buf{};
    auto stored = store_unchecked(std::span{buf}.first(SYNC_MESSAGE_LENGTH), sync);
    EXPECT_EQ(stored, static_cast<size_t>(SYNC_MESSAGE_LENGTH));

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(SYNC_MESSAGE_LENGTH));
    EXPECT_TRUE(result.has_value());
}

TEST(gptp_parse, follow_up_message)
{
    FollowUpMessage fup;
    fup.header.init(MESSAGE_TYPE_FOLLOW_UP, FOLLOW_UP_MESSAGE_LENGTH, 200);

    std::array<uint8_t, 128> buf{};
    auto stored = store_unchecked(std::span{buf}.first(FOLLOW_UP_MESSAGE_LENGTH), fup);
    EXPECT_EQ(stored, static_cast<size_t>(FOLLOW_UP_MESSAGE_LENGTH));

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(FOLLOW_UP_MESSAGE_LENGTH));
    EXPECT_TRUE(result.has_value());
}

TEST(gptp_parse, pdelay_req_message)
{
    PdelayReqMessage req;
    req.header.init(MESSAGE_TYPE_PDELAY_REQ, PDELAY_REQ_MESSAGE_LENGTH, 300);

    std::array<uint8_t, 128> buf{};
    auto stored = store_unchecked(std::span{buf}.first(PDELAY_REQ_MESSAGE_LENGTH), req);
    EXPECT_EQ(stored, static_cast<size_t>(PDELAY_REQ_MESSAGE_LENGTH));

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(PDELAY_REQ_MESSAGE_LENGTH));
    EXPECT_TRUE(result.has_value());
}

TEST(gptp_parse, announce_message)
{
    AnnounceMessage ann;
    ann.init(400);

    std::array<uint8_t, 128> buf{};
    auto stored = store_unchecked(std::span{buf}.first(AnnounceMessage::LENGTH), ann);
    EXPECT_EQ(stored, static_cast<size_t>(AnnounceMessage::LENGTH));

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(AnnounceMessage::LENGTH));
    EXPECT_TRUE(result.has_value());
}

TEST(gptp_parse, buffer_too_short)
{
    std::array<uint8_t, 10> buf{};
    auto result = parse_gptp(std::span<uint8_t const>{buf});
    EXPECT_FALSE(result.has_value());
}

// ===========================================================================
// Static size() method tests
// ===========================================================================

TEST(gptp_static_size, source_port_identity_size)
{
    EXPECT_EQ(SourcePortIdentity::size(), 10U);
}

TEST(gptp_static_size, timestamp_size)
{
    EXPECT_EQ(Timestamp::size(), 10U);
}

TEST(gptp_static_size, clock_quality_size)
{
    EXPECT_EQ(ClockQuality::size(), 4U);
}

// ===========================================================================
// ScaledNs tests
// ===========================================================================

TEST(gptp_scaled_ns, default_is_zero)
{
    ScaledNs s;
    EXPECT_EQ(s.integer_ns(), 0);
    EXPECT_EQ(s.fractional_ns(), 0);
}

TEST(gptp_scaled_ns, positive_integer_ns_roundtrip)
{
    ScaledNs s{1234567890};
    EXPECT_EQ(s.integer_ns(), 1234567890);
    EXPECT_EQ(s.fractional_ns(), 0);
}

TEST(gptp_scaled_ns, negative_integer_ns_roundtrip)
{
    ScaledNs s{-9876543210};
    EXPECT_EQ(s.integer_ns(), -9876543210);
    EXPECT_EQ(s.fractional_ns(), 0);
}

TEST(gptp_scaled_ns, size_is_12_bytes)
{
    EXPECT_EQ(ScaledNs::size(), 12U);
}

// ===========================================================================
// FollowUpInformationTLV tests
// ===========================================================================

TEST(gptp_tlv, follow_up_info_init_is_valid)
{
    FollowUpInformationTLV tlv;
    tlv.init();
    EXPECT_TRUE(tlv.is_valid());
    EXPECT_EQ(tlv.header.tlv_type.get(), TLV_TYPE_ORGANIZATION_EXTENSION);
    EXPECT_EQ(tlv.header.length_field.get(), FollowUpInformationTLV::EXPECTED_LENGTH_FIELD);
    EXPECT_EQ(tlv.get_cumulative_scaled_rate_offset(), 0);
    EXPECT_EQ(tlv.gm_time_base_indicator.get(), 0);
}

TEST(gptp_tlv, follow_up_info_roundtrip)
{
    FollowUpInformationTLV tlv;
    tlv.init();
    tlv.set_cumulative_scaled_rate_offset(-1234567);  // negative to test sign handling
    tlv.gm_time_base_indicator = 0xBEEF;
    tlv.last_gm_phase_change = ScaledNs{42'000'000'000LL};
    tlv.set_scaled_last_gm_freq_change(987654);

    std::array<uint8_t, 64> buf{};
    auto stored = store_unchecked(std::span<uint8_t>{buf}.first(FollowUpInformationTLV::LENGTH), tlv);
    EXPECT_EQ(stored, FollowUpInformationTLV::LENGTH);

    FollowUpInformationTLV decoded;
    auto loaded = load_unchecked(std::span<uint8_t const>{buf}.first(FollowUpInformationTLV::LENGTH), &decoded);
    EXPECT_EQ(loaded, FollowUpInformationTLV::LENGTH);
    EXPECT_TRUE(decoded.is_valid());
    EXPECT_EQ(decoded.get_cumulative_scaled_rate_offset(), -1234567);
    EXPECT_EQ(decoded.gm_time_base_indicator.get(), 0xBEEF);
    EXPECT_EQ(decoded.last_gm_phase_change.integer_ns(), 42'000'000'000LL);
    EXPECT_EQ(decoded.get_scaled_last_gm_freq_change(), 987654);
}

TEST(gptp_tlv, follow_up_info_length_is_32)
{
    EXPECT_EQ(FollowUpInformationTLV::size(), 32U);
    EXPECT_EQ(FollowUpInformationTLV::EXPECTED_LENGTH_FIELD, 28);
}

// ===========================================================================
// MessageIntervalRequestTLV tests
// ===========================================================================

TEST(gptp_tlv, message_interval_request_init_is_valid)
{
    MessageIntervalRequestTLV tlv;
    tlv.init();
    EXPECT_TRUE(tlv.is_valid());
    EXPECT_EQ(tlv.header.tlv_type.get(), TLV_TYPE_ORGANIZATION_EXTENSION);
    EXPECT_EQ(tlv.header.length_field.get(), MessageIntervalRequestTLV::EXPECTED_LENGTH_FIELD);
}

TEST(gptp_tlv, message_interval_request_roundtrip)
{
    MessageIntervalRequestTLV tlv;
    tlv.init();
    tlv.link_delay_interval = -3;  // 125 ms
    tlv.time_sync_interval = -5;   // 31.25 ms (AP default)
    tlv.announce_interval = 0;     // 1 s

    std::array<uint8_t, 32> buf{};
    auto stored = store_unchecked(std::span<uint8_t>{buf}.first(MessageIntervalRequestTLV::LENGTH), tlv);
    EXPECT_EQ(stored, MessageIntervalRequestTLV::LENGTH);

    MessageIntervalRequestTLV decoded;
    auto loaded = load_unchecked(std::span<uint8_t const>{buf}.first(MessageIntervalRequestTLV::LENGTH), &decoded);
    EXPECT_EQ(loaded, MessageIntervalRequestTLV::LENGTH);
    EXPECT_TRUE(decoded.is_valid());
    EXPECT_EQ(decoded.link_delay_interval, -3);
    EXPECT_EQ(decoded.time_sync_interval, -5);
    EXPECT_EQ(decoded.announce_interval, 0);
}

// ===========================================================================
// SignalingMessage tests
// ===========================================================================

TEST(gptp_messages, signaling_init)
{
    SignalingMessage msg;
    msg.init(500);
    EXPECT_TRUE(msg.header.is_signaling());
    EXPECT_EQ(msg.header.sequence_id, 500);
    EXPECT_EQ(msg.header.message_length, SignalingMessage::FIXED_LENGTH);
}

TEST(gptp_messages, signaling_roundtrip_fixed_portion)
{
    SignalingMessage msg;
    msg.init(600);
    ClockIdentity const target_id{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    msg.target_port_identity = SourcePortIdentity{target_id, 2};

    std::array<uint8_t, 64> buf{};
    auto stored = store_unchecked(std::span<uint8_t>{buf}.first(SignalingMessage::FIXED_LENGTH), msg);
    EXPECT_EQ(stored, SignalingMessage::FIXED_LENGTH);

    SignalingMessage decoded;
    auto loaded = load_unchecked(std::span<uint8_t const>{buf}.first(SignalingMessage::FIXED_LENGTH), &decoded);
    EXPECT_EQ(loaded, SignalingMessage::FIXED_LENGTH);
    EXPECT_EQ(decoded.header.sequence_id, 600);
    EXPECT_EQ(decoded.target_port_identity.port_number.get(), 2);
}

TEST(gptp_parse, signaling_message_fixed_portion)
{
    SignalingMessage msg;
    msg.init(700);

    std::array<uint8_t, 64> buf{};
    auto stored = store_unchecked(std::span<uint8_t>{buf}.first(SignalingMessage::FIXED_LENGTH), msg);
    EXPECT_EQ(stored, SignalingMessage::FIXED_LENGTH);

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(SignalingMessage::FIXED_LENGTH));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<SignalingMessage>(*result));
}

// ===========================================================================
// GptpConfig tests
// ===========================================================================

TEST(gptp_config, standard_defaults_validate)
{
    auto cfg = GptpConfig::standard_defaults();
    EXPECT_EQ(static_cast<int>(cfg.profile), static_cast<int>(Profile::Standard));
    EXPECT_EQ(cfg.initial_log_sync_interval, -3);
    EXPECT_EQ(cfg.oper_log_sync_interval, -3);
    EXPECT_TRUE(cfg.bmca_enabled);
    EXPECT_FALSE(cfg.as_capable_initial);
    EXPECT_EQ(static_cast<int>(cfg.pdelay_mode), static_cast<int>(PdelayMode::Active));
    EXPECT_TRUE(static_cast<bool>(cfg.validate()));
}

TEST(gptp_config, avnu_automotive_slave_defaults_validate)
{
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    EXPECT_EQ(static_cast<int>(cfg.profile), static_cast<int>(Profile::AvnuAutomotive));
    EXPECT_EQ(cfg.initial_log_sync_interval, -5);  // 31.25 ms
    EXPECT_EQ(cfg.oper_log_sync_interval, -5);
    EXPECT_FALSE(cfg.bmca_enabled);
    EXPECT_TRUE(cfg.as_capable_initial);
    EXPECT_FALSE(cfg.verify_source_port_identity);
    EXPECT_TRUE(cfg.allow_negative_correction_field);
    EXPECT_TRUE(static_cast<bool>(cfg.validate()));
}

TEST(gptp_config, validate_rejects_bad_sync_interval)
{
    auto cfg = GptpConfig::standard_defaults();
    cfg.initial_log_sync_interval = -8;  // out of range
    auto result = cfg.validate();
    EXPECT_FALSE(static_cast<bool>(result));
}

TEST(gptp_config, validate_rejects_zero_timeout_multiplier)
{
    auto cfg = GptpConfig::standard_defaults();
    cfg.sync_receipt_timeout_multiplier = 0;
    auto result = cfg.validate();
    EXPECT_FALSE(static_cast<bool>(result));
}

TEST(gptp_config, validate_rejects_negative_rate_ratio)
{
    auto cfg = GptpConfig::standard_defaults();
    cfg.manual_neighbor_rate_ratio = -1.0;
    auto result = cfg.validate();
    EXPECT_FALSE(static_cast<bool>(result));
}

TEST(gptp_config, validate_rejects_zero_capacity)
{
    auto cfg = GptpConfig::standard_defaults();
    cfg.max_pending_sync = 0;
    auto result = cfg.validate();
    EXPECT_FALSE(static_cast<bool>(result));
}

TEST(gptp_config, validate_accepts_manual_peer_delay)
{
    auto cfg = GptpConfig::avnu_automotive_slave_defaults();
    cfg.pdelay_mode = PdelayMode::Disabled;
    cfg.manual_peer_delay_ns = 500;
    cfg.manual_neighbor_rate_ratio = 1.0;
    EXPECT_TRUE(static_cast<bool>(cfg.validate()));
}

// ===========================================================================
// GptpError tests
// ===========================================================================

TEST(gptp_error, error_code_roundtrip)
{
    auto ec = make_error_code(GptpError::SyncReceiptTimeout);
    EXPECT_EQ(ec.value(), static_cast<int>(GptpError::SyncReceiptTimeout));
    EXPECT_EQ(std::string(ec.category().name()), "statusbar.gptp");
}

TEST(gptp_error, name_strings)
{
    EXPECT_EQ(std::string{gptp_error_name(GptpError::Success)}, "success");
    EXPECT_EQ(std::string{gptp_error_name(GptpError::PendingSyncTableFull)}, "pending Sync table full");
    EXPECT_EQ(std::string{gptp_error_name(GptpError::PeerMisbehaving)}, "peer misbehaving");
}

//
// format_to overloads — pretty-print helpers for every gPTP message type.
// Smoke-test each overload by writing into a std::string and asserting
// the output is non-empty. Exercises the per-type format_to and the
// variant-visiting format_to branch.
//

TEST(gptp_format_to, header_is_non_empty)
{
    MessageHeader h{};
    h.init(MESSAGE_TYPE_SYNC, SyncMessage::LENGTH, 7);
    std::string out;
    (void)format_to(std::back_inserter(out), h);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, sync_message)
{
    SyncMessage msg{};
    msg.init(1);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, follow_up_message)
{
    FollowUpMessage msg{};
    msg.init(1);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, pdelay_req_message)
{
    PdelayReqMessage msg{};
    msg.init(1);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, pdelay_resp_message)
{
    PdelayRespMessage msg{};
    msg.init(1);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, pdelay_resp_follow_up_message)
{
    PdelayRespFollowUpMessage msg{};
    msg.init(1);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, announce_message)
{
    AnnounceMessage msg{};
    msg.init(1);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, signaling_message)
{
    SignalingMessage msg{};
    msg.init(0);
    std::string out;
    (void)format_to(std::back_inserter(out), msg);
    EXPECT_TRUE(!out.empty());
}

TEST(gptp_format_to, variant_dispatch_covers_each_arm)
{
    using V = std::variant<
        GptpTruncated,
        SyncMessage,
        FollowUpMessage,
        PdelayReqMessage,
        PdelayRespMessage,
        PdelayRespFollowUpMessage,
        AnnounceMessage,
        SignalingMessage,
        MessageHeader>;

    auto run = [](V const& v) {
        std::string out;
        (void)format_to(std::back_inserter(out), v);
        EXPECT_TRUE(!out.empty());
    };

    run(V{GptpTruncated{}});
    {
        SyncMessage m{};
        m.init(1);
        run(V{m});
    }
    {
        FollowUpMessage m{};
        m.init(1);
        run(V{m});
    }
    {
        PdelayReqMessage m{};
        m.init(1);
        run(V{m});
    }
    {
        PdelayRespMessage m{};
        m.init(1);
        run(V{m});
    }
    {
        PdelayRespFollowUpMessage m{};
        m.init(1);
        run(V{m});
    }
    {
        AnnounceMessage m{};
        m.init(1);
        run(V{m});
    }
    {
        SignalingMessage m{};
        m.init(0);
        run(V{m});
    }
    {
        MessageHeader h{};
        h.init(MESSAGE_TYPE_SYNC, SyncMessage::LENGTH, 1);
        run(V{h});
    }
}

// ===========================================================================
// Decoder safety: malformed / truncated PTP PDUs must be recognized rather
// than silently dispatched to a partially-initialized message struct.
// parse_gptp is intentionally lenient — it returns a GptpTruncated{header}
// variant when the buffer is long enough for the common header but short of
// the per-type fixed length. These tests pin down that contract.
// ===========================================================================

TEST(gptp_parse_safety, below_common_header_returns_nullopt)
{
    std::array<uint8_t, 33> buf{};  // one short of MessageHeader::LENGTH
    auto result = parse_gptp(std::span<uint8_t const>{buf});
    EXPECT_FALSE(result.has_value());
}

TEST(gptp_parse_safety, sync_between_header_and_body_returns_truncated)
{
    // Build a Sync whose buffer is the 34-byte common header only —
    // message_type field indicates Sync so dispatcher looks for body,
    // finds short buffer, returns GptpTruncated.
    SyncMessage msg{};
    msg.header.init(MESSAGE_TYPE_SYNC, SYNC_MESSAGE_LENGTH, 42);
    std::array<uint8_t, 128> buf{};
    (void)store_unchecked(std::span{buf}.first(SYNC_MESSAGE_LENGTH), msg);

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(34));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<GptpTruncated>(*result));
}

TEST(gptp_parse_safety, sync_with_one_byte_short_body_returns_truncated)
{
    SyncMessage msg{};
    msg.header.init(MESSAGE_TYPE_SYNC, SYNC_MESSAGE_LENGTH, 42);
    std::array<uint8_t, 128> buf{};
    (void)store_unchecked(std::span{buf}.first(SYNC_MESSAGE_LENGTH), msg);

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(43));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<GptpTruncated>(*result));
}

TEST(gptp_parse_safety, pdelay_req_short_of_54_returns_truncated)
{
    PdelayReqMessage msg{};
    msg.header.init(MESSAGE_TYPE_PDELAY_REQ, PDELAY_REQ_MESSAGE_LENGTH, 17);
    std::array<uint8_t, 128> buf{};
    (void)store_unchecked(std::span{buf}.first(PDELAY_REQ_MESSAGE_LENGTH), msg);

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(53));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<GptpTruncated>(*result));
}

TEST(gptp_parse_safety, unknown_message_type_returns_header_variant)
{
    // message_type 6 is reserved (not in the 0-3 / 8-13 valid set).
    MessageHeader hdr{};
    hdr.init(6, MessageHeader::LENGTH, 1);
    std::array<uint8_t, 64> buf{};
    (void)store_unchecked(std::span{buf}.first(MessageHeader::LENGTH), hdr);

    auto result = parse_gptp(std::span<uint8_t const>{buf}.first(MessageHeader::LENGTH));
    EXPECT_TRUE(result.has_value());
    // Unknown message_type falls through the switch and returns just the header.
    EXPECT_TRUE(std::holds_alternative<MessageHeader>(*result));
}

TEST(gptp_header_validity, rejects_wrong_sdo_id)
{
    MessageHeader hdr{};
    hdr.init(MESSAGE_TYPE_SYNC, SyncMessage::LENGTH, 1);
    // Corrupt the upper 4 bits of byte 0 to a non-gPTP SDO.
    hdr.sdo_id_message_type = static_cast<uint8_t>((0xA << 4) | MESSAGE_TYPE_SYNC);
    EXPECT_FALSE(hdr.is_valid());
}

TEST(gptp_header_validity, rejects_wrong_ptp_version)
{
    MessageHeader hdr{};
    hdr.init(MESSAGE_TYPE_SYNC, SyncMessage::LENGTH, 1);
    hdr.version = 1;  // only 2 is valid for gPTP
    EXPECT_FALSE(hdr.is_valid());
}

TEST(gptp_header_validity, rejects_message_length_smaller_than_header)
{
    MessageHeader hdr{};
    hdr.init(MESSAGE_TYPE_SYNC, SyncMessage::LENGTH, 1);
    hdr.message_length = 20;  // < HEADER_LENGTH (34)
    EXPECT_FALSE(hdr.is_valid());
}

TEST(gptp_header_validity, rejects_reserved_message_types)
{
    for (uint8_t mt : {uint8_t{4}, uint8_t{5}, uint8_t{6}, uint8_t{7}, uint8_t{14}, uint8_t{15}}) {
        MessageHeader hdr{};
        hdr.init(MESSAGE_TYPE_SYNC, SyncMessage::LENGTH, 1);
        hdr.sdo_id_message_type = static_cast<uint8_t>((SDO_ID << 4) | mt);
        EXPECT_FALSE(hdr.is_valid());
    }
}

TEST(gptp_header_validity, accepts_all_spec_message_types)
{
    for (uint8_t mt :
         {uint8_t{0},
          uint8_t{1},
          uint8_t{2},
          uint8_t{3},
          uint8_t{8},
          uint8_t{9},
          uint8_t{10},
          uint8_t{11},
          uint8_t{12},
          uint8_t{13}}) {
        MessageHeader hdr{};
        hdr.init(mt, MessageHeader::LENGTH, 1);
        EXPECT_TRUE(hdr.is_valid());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GptpTimeBridge tests — verify the int64 ppt math, atomicity, and the
// rated conversion APIs. The bridge wires two clock-reader closures so
// these tests construct controllable mock clocks.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Mock clock holder. Synced reads via shared int64. Use volatile-by-
// way-of-pointer-deref to defeat optimizer caching across closure calls.
struct MockClocks
{
    int64_t app_ns{0};
    int64_t gptp_ns{0};
};

}  // namespace

TEST(gptp_time_bridge, initial_state_offset_zero_rate_zero)
{
    GptpTimeBridge b{};
    EXPECT_EQ(b.offset(), int64_t{0});
    EXPECT_EQ(b.rate_offset_ppt(), int64_t{0});
}

TEST(gptp_time_bridge, first_update_sets_offset_but_not_rate)
{
    MockClocks c{};
    c.app_ns = 1'000'000'000;
    c.gptp_ns = 1'000'000'500;  // gPTP is 500 ns ahead
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();
    EXPECT_EQ(b.offset(), int64_t{500});
    EXPECT_EQ(b.rate_offset_ppt(), int64_t{0});  // need 2 updates for rate
}

TEST(gptp_time_bridge, second_update_computes_ppt_exactly)
{
    // Set up a synthetic 100 ppm rate offset: gPTP advances 1 ms but
    // app advances 1 ms - 100 ns = 999 900 ns over the interval. That
    // makes the gptp:app ratio 1000000 / 999900 ≈ 1.0001 = 100 ppm
    // = 1e8 ppt.
    MockClocks c{};
    c.app_ns = 1'000'000'000;
    c.gptp_ns = 2'000'000'000;
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();  // anchor

    c.app_ns += 999'900;
    c.gptp_ns += 1'000'000;
    b.update();

    // skew = 100 ns over 999_900 ns app delta
    // ppt = (100 * 1e12) / 999900 ≈ 100_010_001 ppt ≈ 100.01 ppm
    int64_t const ppt = b.rate_offset_ppt();
    EXPECT_TRUE(ppt >= 100'010'000);
    EXPECT_TRUE(ppt <= 100'010'010);
}

TEST(gptp_time_bridge, to_gptp_unrated_uses_offset_only)
{
    MockClocks c{.app_ns = 1'000'000, .gptp_ns = 1'001'500};
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();

    // Without rate (only one update), to_gptp(X) = X + 1500
    EXPECT_EQ(b.to_gptp(2'000'000), int64_t{2'001'500});
    EXPECT_EQ(b.from_gptp(2'001'500), int64_t{2'000'000});
}

TEST(gptp_time_bridge, to_gptp_rated_extrapolates_using_rate_offset)
{
    // Set up a +1000 ppm rate offset — gPTP runs 1000 ppm faster
    // than app. After two updates the bridge should extrapolate
    // future app→gptp conversions with the rate correction baked in.
    MockClocks c{.app_ns = 0, .gptp_ns = 0};
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();  // anchor at (0, 0); offset = 0

    c.app_ns = 1'000'000'000;   // +1 s app
    c.gptp_ns = 1'000'001'000;  // +1.000001 s gPTP (1000 ppt = 1 ppm? wait)
    // Let me redo: 1 ppm = 1000 ns drift per 1 s = 1000 ppt is wrong.
    // 1 ppm = 1e-6 = 1e6 ppt. Drift over 1 s = 1e-6 * 1e9 ns = 1000 ns.
    // So +1000 ns drift over 1 s app = 1 ppm = 1e6 ppt.
    b.update();
    // anchor now at (app=1e9, gptp=1.000001e9), offset=1000, ppt~1e6

    int64_t const ppt = b.rate_offset_ppt();
    // ppt should be about 1e6 (1 ppm). Allow ±100 for round-off.
    EXPECT_TRUE(ppt > 1'000'000 - 100);
    EXPECT_TRUE(ppt < 1'000'000 + 100);

    // Now query: what does app=2'000'000'000 (1 s after anchor) project to?
    // Without rate compensation: 2e9 + 1000 = 2'000'001'000
    // With rate compensation: 2e9 + 1000 + (2e9 - 1e9) * 1e6 / 1e12
    //                       = 2e9 + 1000 + 1000 = 2'000'002'000
    int64_t const unrated = b.to_gptp(2'000'000'000);
    int64_t const rated = b.to_gptp_rated(2'000'000'000);
    EXPECT_EQ(unrated, int64_t{2'000'001'000});
    // Allow a few ns for round-off in the integer math.
    EXPECT_TRUE(rated >= 2'000'001'995);
    EXPECT_TRUE(rated <= 2'000'002'005);
}

TEST(gptp_time_bridge, from_gptp_rated_inverts_to_gptp_rated_within_tolerance)
{
    MockClocks c{.app_ns = 0, .gptp_ns = 0};
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();
    c.app_ns = 1'000'000'000;
    c.gptp_ns = 1'000'001'000;  // 1 ppm
    b.update();

    // Round-trip a future app timestamp: app → gptp_rated → app should
    // be near identity. Linear-approx introduces ≤ 1 ns error for
    // 1 ppm × 1 s deltas.
    int64_t const start_app = 2'000'000'000;
    int64_t const round_trip = b.from_gptp_rated(b.to_gptp_rated(start_app));
    int64_t const err = round_trip - start_app;
    EXPECT_TRUE(err >= -2 && err <= 2);
}

TEST(gptp_time_bridge, phase_step_does_not_spike_rate)
{
    // Simulate the situation that produced a -1.05e14 ppt spike on
    // the user's RPi5: PHC steps backward by ~13 seconds (servo
    // phase jump) between two bridge.update() calls. Real natural
    // drift over a single update interval is bounded to ms; anything
    // larger must be a clock step. The bridge should detect this and
    // leave the previous rate estimate intact.
    MockClocks c{.app_ns = 0, .gptp_ns = 0};
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();

    // Establish a known good rate first (~1 ppm).
    c.app_ns += 1'000'000'000;
    c.gptp_ns += 1'000'001'000;
    b.update();
    int64_t const known_good_ppt = b.rate_offset_ppt();
    EXPECT_TRUE(known_good_ppt > 500'000);
    EXPECT_TRUE(known_good_ppt < 1'500'000);

    // Now simulate a 13-second backward phase step on the gPTP side
    // between this update and the next.
    c.app_ns += 125'000'000;                    // normal 125 ms tick
    c.gptp_ns += 125'000'000 - 13'000'000'000;  // PHC steps back 13 s
    b.update();

    // Bridge should leave rate alone, not store the absurd value.
    EXPECT_EQ(b.rate_offset_ppt(), known_good_ppt);

    // Subsequent normal updates resume rate tracking.
    c.app_ns += 125'000'000;
    c.gptp_ns += 125'000'125;  // 1 ppm drift
    b.update();
    int64_t const post_step_ppt = b.rate_offset_ppt();
    // Still in the ~1 ppm neighborhood (servo dynamics, but the
    // bridge math is sane again).
    EXPECT_TRUE(post_step_ppt > 0);
    EXPECT_TRUE(post_step_ppt < 10'000'000);  // < 10 ppm
}

TEST(gptp_time_bridge, anchor_accessors_return_last_update_values)
{
    MockClocks c{.app_ns = 12345, .gptp_ns = 67890};
    GptpTimeBridge b{};
    b.get_app_time_ns = [&c]() { return c.app_ns; };
    b.get_gptp_time_ns = [&c]() { return c.gptp_ns; };
    b.update();
    EXPECT_EQ(b.last_anchor_app_ns(), int64_t{12345});
    EXPECT_EQ(b.last_anchor_gptp_ns(), int64_t{67890});
}

// Test runner

TEST_MAIN(statusbar_gptp, gptp_test)