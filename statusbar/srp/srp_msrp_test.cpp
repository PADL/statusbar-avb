// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/srp/srp_msrp.hpp"

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

using namespace statusbar;
using namespace statusbar::tsn;
using namespace statusbar::srp::msrp;
using namespace statusbar::ieee;

// Bring protocol functions into scope for ADL
using statusbar::protocol::load_unchecked;
using statusbar::protocol::store_unchecked;

//
// Tests: MSRP Constants
//

TEST(msrp_constants, protocol_version_is_zero)
{
    EXPECT_EQ(PROTOCOL_VERSION, 0);
}

TEST(msrp_constants, ethertype)
{
    EXPECT_EQ(ETHERTYPE, 0x22EA);
}

TEST(msrp_constants, application_address)
{
    EXPECT_EQ(APPLICATION_ADDRESS, 0x0180C200000EULL);
}

TEST(msrp_constants, sr_class_a)
{
    EXPECT_EQ(SR_CLASS_A, 6);
}

TEST(msrp_constants, sr_class_b)
{
    EXPECT_EQ(SR_CLASS_B, 5);
}

TEST(msrp_constants, default_sr_class)
{
    EXPECT_EQ(default_sr_class, SR_CLASS_A);
}

TEST(msrp_constants, default_sr_class_priority)
{
    EXPECT_EQ(default_sr_class_priority, 3);
}

TEST(msrp_constants, default_sr_class_vid)
{
    EXPECT_EQ(default_sr_class_vid, 2);
}

TEST(msrp_constants, rank_emergency)
{
    EXPECT_EQ(RANK_EMERGENCY, 0);
}

TEST(msrp_constants, rank_non_emergency)
{
    EXPECT_EQ(RANK_NON_EMERGENCY, 1);
}

//
// Tests: AttributeType enum
//

TEST(msrp_attribute_type, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(AttributeType::TalkerAdvertise), 1);
    EXPECT_EQ(static_cast<uint8_t>(AttributeType::TalkerFailed), 2);
    EXPECT_EQ(static_cast<uint8_t>(AttributeType::Listener), 3);
    EXPECT_EQ(static_cast<uint8_t>(AttributeType::Domain), 4);
}

TEST(msrp_attribute_type, name_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile talker_advertise_v = 1, talker_failed_v = 2, listener_v = 3, domain_v = 4, unknown_v = 99;
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(talker_advertise_v)), "TalkerAdvertise");
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(talker_failed_v)), "TalkerFailed");
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(listener_v)), "Listener");
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(domain_v)), "Domain");
    EXPECT_EQ(attribute_type_name(static_cast<AttributeType>(unknown_v)), "Unknown");
}

//
// Tests: AttributeLength enum
//

TEST(msrp_attribute_length, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(AttributeLength::TalkerAdvertise), 25);
    EXPECT_EQ(static_cast<uint8_t>(AttributeLength::TalkerFailed), 34);
    EXPECT_EQ(static_cast<uint8_t>(AttributeLength::Listener), 8);
    EXPECT_EQ(static_cast<uint8_t>(AttributeLength::Domain), 4);
}

//
// Tests: ListenerDeclaration enum
//

TEST(msrp_listener_declaration, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(ListenerDeclaration::Ignore), 0);
    EXPECT_EQ(static_cast<uint8_t>(ListenerDeclaration::AskingFailed), 1);
    EXPECT_EQ(static_cast<uint8_t>(ListenerDeclaration::Ready), 2);
    EXPECT_EQ(static_cast<uint8_t>(ListenerDeclaration::ReadyFailed), 3);
}

TEST(msrp_listener_declaration, name_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile ignore_v = 0, asking_failed_v = 1, ready_v = 2, ready_failed_v = 3, unknown_v = 99;
    EXPECT_EQ(listener_declaration_name(static_cast<ListenerDeclaration>(ignore_v)), "Ignore");
    EXPECT_EQ(listener_declaration_name(static_cast<ListenerDeclaration>(asking_failed_v)), "AskingFailed");
    EXPECT_EQ(listener_declaration_name(static_cast<ListenerDeclaration>(ready_v)), "Ready");
    EXPECT_EQ(listener_declaration_name(static_cast<ListenerDeclaration>(ready_failed_v)), "ReadyFailed");
    EXPECT_EQ(listener_declaration_name(static_cast<ListenerDeclaration>(unknown_v)), "Unknown");
}

//
// Tests: FailureCode enum
//

TEST(msrp_failure_code, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(FailureCode::NoFailure), 0);
    EXPECT_EQ(static_cast<uint8_t>(FailureCode::InsufficientBandwidth), 1);
    EXPECT_EQ(static_cast<uint8_t>(FailureCode::SrClassPriorityMismatch), 19);
}

TEST(msrp_failure_code, name_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile no_failure_v = 0, insufficient_bandwidth_v = 1, sr_class_mismatch_v = 19, unknown_v = 99;
    EXPECT_EQ(failure_code_name(static_cast<FailureCode>(no_failure_v)), "NoFailure");
    EXPECT_EQ(failure_code_name(static_cast<FailureCode>(insufficient_bandwidth_v)), "InsufficientBandwidth");
    EXPECT_EQ(failure_code_name(static_cast<FailureCode>(sr_class_mismatch_v)), "SrClassPriorityMismatch");
    EXPECT_EQ(failure_code_name(static_cast<FailureCode>(unknown_v)), "Unknown");
}

//
// Tests: Priority and Rank encoding
//

TEST(msrp_priority_rank, encode_priority_only)
{
    auto const encoded = encode_priority_and_rank(7, 0);
    EXPECT_EQ(encoded, 0xE0);  // priority=7 in bits 5-7
}

TEST(msrp_priority_rank, encode_rank_only)
{
    auto const encoded = encode_priority_and_rank(0, 1);
    EXPECT_EQ(encoded, 0x10);  // rank=1 in bit 4
}

TEST(msrp_priority_rank, encode_both)
{
    auto const encoded = encode_priority_and_rank(3, 1);
    EXPECT_EQ(encoded, 0x70);  // priority=3 (0x60) | rank=1 (0x10)
}

TEST(msrp_priority_rank, decode_priority)
{
    EXPECT_EQ(decode_priority(0xE0), 7);
    EXPECT_EQ(decode_priority(0x60), 3);
    EXPECT_EQ(decode_priority(0x00), 0);
}

TEST(msrp_priority_rank, decode_rank)
{
    EXPECT_EQ(decode_rank(0x10), 1);
    EXPECT_EQ(decode_rank(0x00), 0);
    EXPECT_EQ(decode_rank(0xF0), 1);  // Other bits should be masked out
}

TEST(msrp_priority_rank, round_trip)
{
    uint8_t const priority = 5;
    uint8_t const rank = 1;
    auto const encoded = encode_priority_and_rank(priority, rank);
    EXPECT_EQ(decode_priority(encoded), priority);
    EXPECT_EQ(decode_rank(encoded), rank);
}

//
// Tests: DomainFirstValue
//

TEST(msrp_domain, sizeof_is_4)
{
    EXPECT_EQ(sizeof(DomainFirstValue), 4);
    EXPECT_EQ(DomainFirstValue::LENGTH, 4);
}

TEST(msrp_domain, default_construction)
{
    DomainFirstValue domain{};
    EXPECT_EQ(domain.sr_class_id.get(), default_sr_class);
    EXPECT_EQ(domain.sr_class_priority.get(), default_sr_class_priority);
    EXPECT_EQ(domain.sr_class_vid.get(), default_sr_class_vid);
}

TEST(msrp_domain, designated_init)
{
    DomainFirstValue domain{.sr_class_id = SR_CLASS_B, .sr_class_priority = 2, .sr_class_vid = 100};
    EXPECT_EQ(domain.sr_class_id.get(), SR_CLASS_B);
    EXPECT_EQ(domain.sr_class_priority.get(), 2);
    EXPECT_EQ(domain.sr_class_vid.get(), 100);
}

TEST(msrp_domain, serialization_round_trip)
{
    DomainFirstValue original{.sr_class_id = 5, .sr_class_priority = 4, .sr_class_vid = 2048};
    std::array<uint8_t, 8> buffer{};

    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 4);

    DomainFirstValue restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 4);

    EXPECT_EQ(restored.sr_class_id.get(), 5);
    EXPECT_EQ(restored.sr_class_priority.get(), 4);
    EXPECT_EQ(restored.sr_class_vid.get(), 2048);
}

//
// Tests: ListenerFirstValue
//

TEST(msrp_listener, sizeof_is_8)
{
    EXPECT_EQ(sizeof(ListenerFirstValue), 8);
    EXPECT_EQ(ListenerFirstValue::LENGTH, 8);
}

TEST(msrp_listener, default_construction)
{
    ListenerFirstValue listener{};
    EXPECT_FALSE(listener.stream_id.is_set());
}

TEST(msrp_listener, serialization_round_trip)
{
    Eui48 mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    ListenerFirstValue original{.stream_id = tsn::StreamId{mac, 0x1234}};
    std::array<uint8_t, 16> buffer{};

    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 8);

    ListenerFirstValue restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 8);

    EXPECT_TRUE(restored.stream_id.is_set());
    EXPECT_EQ(restored.stream_id.get_unique_id(), 0x1234);
}

//
// Tests: TalkerAdvertiseFirstValue
//

TEST(msrp_talker_advertise, sizeof_is_25)
{
    EXPECT_EQ(sizeof(TalkerAdvertiseFirstValue), 25);
    EXPECT_EQ(TalkerAdvertiseFirstValue::LENGTH, 25);
}

TEST(msrp_talker_advertise, default_construction)
{
    TalkerAdvertiseFirstValue talker{};
    EXPECT_FALSE(talker.stream_id.is_set());
    EXPECT_EQ(talker.vlan_identifier.get(), 0);
    EXPECT_EQ(talker.max_frame_size.get(), 0);
    EXPECT_EQ(talker.max_interval_frames.get(), 0);
    EXPECT_EQ(talker.get_priority(), 0);
    EXPECT_EQ(talker.get_rank(), 0);
    EXPECT_EQ(talker.accumulated_latency.get(), 0);
}

TEST(msrp_talker_advertise, set_and_get_priority)
{
    TalkerAdvertiseFirstValue talker{};
    talker.set_priority(5);
    EXPECT_EQ(talker.get_priority(), 5);
    EXPECT_EQ(talker.get_rank(), 0);  // Rank should remain unchanged
}

TEST(msrp_talker_advertise, set_and_get_rank)
{
    TalkerAdvertiseFirstValue talker{};
    talker.set_priority(3);
    talker.set_rank(1);
    EXPECT_EQ(talker.get_rank(), 1);
    EXPECT_EQ(talker.get_priority(), 3);  // Priority should remain unchanged
}

TEST(msrp_talker_advertise, serialization_round_trip)
{
    Eui48 mac{0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    Eui48 dest{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x00};

    TalkerAdvertiseFirstValue original{};
    original.stream_id = tsn::StreamId{mac, 0xABCD};
    original.destination_address = dest;
    original.vlan_identifier = 2;
    original.max_frame_size = 1500;
    original.max_interval_frames = 1;
    original.set_priority(3);
    original.set_rank(1);
    original.accumulated_latency = 2000;

    std::array<uint8_t, 32> buffer{};
    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 25);

    TalkerAdvertiseFirstValue restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 25);

    EXPECT_TRUE(restored.stream_id.is_set());
    EXPECT_EQ(restored.stream_id.get_unique_id(), 0xABCD);
    EXPECT_EQ(restored.vlan_identifier.get(), 2);
    EXPECT_EQ(restored.max_frame_size.get(), 1500);
    EXPECT_EQ(restored.max_interval_frames.get(), 1);
    EXPECT_EQ(restored.get_priority(), 3);
    EXPECT_EQ(restored.get_rank(), 1);
    EXPECT_EQ(restored.accumulated_latency.get(), 2000);
}

//
// Tests: TalkerFailedFirstValue
//

TEST(msrp_talker_failed, sizeof_is_34)
{
    EXPECT_EQ(sizeof(TalkerFailedFirstValue), 34);
    EXPECT_EQ(TalkerFailedFirstValue::LENGTH, 34);
}

TEST(msrp_talker_failed, default_construction)
{
    TalkerFailedFirstValue failed{};
    EXPECT_FALSE(failed.advertise.stream_id.is_set());
    EXPECT_EQ(failed.get_failure_code(), FailureCode::NoFailure);
}

TEST(msrp_talker_failed, set_and_get_failure_code)
{
    TalkerFailedFirstValue failed{};
    failed.set_failure_code(FailureCode::InsufficientBandwidth);
    EXPECT_EQ(failed.get_failure_code(), FailureCode::InsufficientBandwidth);
}

TEST(msrp_talker_failed, serialization_round_trip)
{
    Eui48 mac{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    Eui64 bridge_id{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

    TalkerFailedFirstValue original{};
    original.advertise.stream_id = tsn::StreamId{mac, 0x1234};
    original.advertise.max_frame_size = 1500;
    original.failure_bridge_id = bridge_id;
    original.set_failure_code(FailureCode::StreamPreemptedByHigherRank);

    std::array<uint8_t, 40> buffer{};
    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 34);

    TalkerFailedFirstValue restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 34);

    EXPECT_TRUE(restored.advertise.stream_id.is_set());
    EXPECT_EQ(restored.advertise.stream_id.get_unique_id(), 0x1234);
    EXPECT_EQ(restored.advertise.max_frame_size.get(), 1500);
    EXPECT_EQ(restored.get_failure_code(), FailureCode::StreamPreemptedByHigherRank);
}

//
// FirstValue incrementation rules - IEEE 802.1Q 35.2.2.{7,8,9}
// (AVnu End-Station test MSRP.End.c.35.1.11)
//

TEST(msrp_increment, listener_adds_one_to_unique_id_only)
{
    ListenerFirstValue fv{};
    fv.stream_id = StreamId{Eui48{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, 0x0007};
    auto const before = fv.stream_id.to_uint64();
    increment_first_value(fv);
    EXPECT_EQ(fv.stream_id.get_unique_id(), 0x0008);
    EXPECT_EQ(fv.stream_id.to_uint64(), before + 1);  // only the low 16 bits move
}

TEST(msrp_increment, talker_advertise_adds_one_to_unique_id_and_dest_mac)
{
    TalkerAdvertiseFirstValue fv{};
    fv.stream_id = StreamId{Eui48{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, 0x0001};
    fv.destination_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34};
    auto const dest_before = fv.destination_address.to_uint64();
    increment_first_value(fv);
    EXPECT_EQ(fv.stream_id.get_unique_id(), 0x0002);
    EXPECT_EQ(fv.destination_address.to_uint64(), dest_before + 1);
}

TEST(msrp_increment, talker_failed_increments_advertise_and_keeps_failure_info)
{
    TalkerFailedFirstValue fv{};
    fv.advertise.stream_id = StreamId{Eui48{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff}, 0x0010};
    fv.advertise.destination_address = Eui48{0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34};
    fv.set_failure_code(FailureCode::InsufficientBandwidth);
    increment_first_value(fv);
    EXPECT_EQ(fv.advertise.stream_id.get_unique_id(), 0x0011);
    EXPECT_EQ(fv.advertise.destination_address.to_uint64(), 0x91e0f0001235ULL);
    EXPECT_EQ(static_cast<int>(fv.get_failure_code()), static_cast<int>(FailureCode::InsufficientBandwidth));
}

TEST(msrp_increment, domain_adds_one_to_class_id_and_priority_not_vid)
{
    DomainFirstValue fv{};
    fv.sr_class_id = SR_CLASS_B;  // 5
    fv.sr_class_priority = 2;
    fv.sr_class_vid = default_sr_class_vid;
    increment_first_value(fv);
    EXPECT_EQ(fv.sr_class_id.get(), SR_CLASS_A);  // 6
    EXPECT_EQ(fv.sr_class_priority.get(), 3);
    EXPECT_EQ(fv.sr_class_vid.get(), default_sr_class_vid);  // VID is NOT incremented
}

//
// Main test runner
//

TEST_MAIN(statusbar_srp, srp_msrp_test)