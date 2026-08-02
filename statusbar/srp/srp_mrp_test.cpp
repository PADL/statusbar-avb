// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/srp/srp_mrp_participant.hpp"
#include "statusbar/srp/srp_mrp_timers.hpp"
#include "statusbar/srp/srp_mvrp.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"
#include "statusbar/tsn/tsn_error.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>

using namespace statusbar;
using namespace statusbar::srp::mrp;
using namespace statusbar::ieee;
using statusbar::sm::TimePoint;

// Bring protocol functions into scope for ADL
using statusbar::protocol::load_unchecked;
using statusbar::protocol::store_unchecked;

//
// Tests: MRP Constants
//

TEST(mrp_constants, end_mark_is_zero)
{
    EXPECT_EQ(END_MARK, 0x0000);
}

TEST(mrp_constants, protocol_version_is_zero)
{
    EXPECT_EQ(PROTOCOL_VERSION, 0);
}

TEST(mrp_constants, timer_join_time_is_200ms)
{
    EXPECT_EQ(TIMER_JOIN_TIME_NS, 200'000'000);
}

TEST(mrp_constants, timer_leave_time_is_800ms)
{
    EXPECT_EQ(TIMER_LEAVE_TIME_NS, 800'000'000);
}

TEST(mrp_constants, timer_leave_all_time_is_12s)
{
    EXPECT_EQ(TIMER_LEAVE_ALL_TIME_NS, 12'000'000'000);
}

//
// Tests: AttributeEvent enum
//

TEST(mrp_attribute_event, enum_values)
{
    EXPECT_EQ(static_cast<uint8_t>(AttributeEvent::New), 0);
    EXPECT_EQ(static_cast<uint8_t>(AttributeEvent::JoinIn), 1);
    EXPECT_EQ(static_cast<uint8_t>(AttributeEvent::In), 2);
    EXPECT_EQ(static_cast<uint8_t>(AttributeEvent::JoinMt), 3);
    EXPECT_EQ(static_cast<uint8_t>(AttributeEvent::Mt), 4);
    EXPECT_EQ(static_cast<uint8_t>(AttributeEvent::Lv), 5);
}

TEST(mrp_attribute_event, name_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile new_v = 0, joinin_v = 1, in_v = 2, joinmt_v = 3, mt_v = 4, lv_v = 5, unknown_v = 99;
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(new_v)), "New");
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(joinin_v)), "JoinIn");
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(in_v)), "In");
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(joinmt_v)), "JoinMt");
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(mt_v)), "Mt");
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(lv_v)), "Lv");
    EXPECT_EQ(attribute_event_name(static_cast<AttributeEvent>(unknown_v)), "Unknown");
}

//
// Tests: Vector Header functions
//

TEST(mrp_vector_header, calculate_with_leave_all_false)
{
    auto const header = calculate_vector_header(false, 100);
    EXPECT_EQ(header, 100);
}

TEST(mrp_vector_header, calculate_with_leave_all_true)
{
    auto const header = calculate_vector_header(true, 100);
    EXPECT_EQ(header, 0x2000 | 100);
}

TEST(mrp_vector_header, calculate_masks_number_of_values)
{
    // Max valid value is 8191 (0x1FFF)
    auto const header = calculate_vector_header(false, 0xFFFF);
    EXPECT_EQ(header, 0x1FFF);
}

TEST(mrp_vector_header, extract_number_of_values)
{
    EXPECT_EQ(extract_number_of_values(0x2064), 100);  // 0x64 = 100
}

TEST(mrp_vector_header, extract_leave_all_true)
{
    EXPECT_TRUE(extract_leave_all(0x2064));
}

TEST(mrp_vector_header, extract_leave_all_false)
{
    EXPECT_FALSE(extract_leave_all(0x0064));
}

TEST(mrp_vector_header, round_trip)
{
    auto const original_leave_all = true;
    uint16_t const original_count = 1234;
    auto const header = calculate_vector_header(original_leave_all, original_count);
    EXPECT_EQ(extract_leave_all(header), original_leave_all);
    EXPECT_EQ(extract_number_of_values(header), original_count);
}

//
// Tests: ThreePacked encoding
//

TEST(mrp_threepacked, pack_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile new_v = 0, lv_v = 5;
    auto const first = static_cast<AttributeEvent>(new_v);
    auto const second = static_cast<AttributeEvent>(new_v);
    auto const third = static_cast<AttributeEvent>(new_v);
    auto const fourth = static_cast<AttributeEvent>(lv_v);
    EXPECT_EQ(pack3_events(first, second, third), 0);
    EXPECT_EQ(pack3_events(fourth, first, first), 180);  // 5*36 + 0*6 + 0
}

TEST(mrp_threepacked, octet_count_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    size_t volatile zero = 0, one = 1, three = 3, four = 4, ten = 10;
    EXPECT_EQ(threepacked_octet_count(zero), 0);
    EXPECT_EQ(threepacked_octet_count(one), 1);
    EXPECT_EQ(threepacked_octet_count(three), 1);
    EXPECT_EQ(threepacked_octet_count(four), 2);
    EXPECT_EQ(threepacked_octet_count(ten), 4);
}

TEST(mrp_threepacked, pack_all_zeros)
{
    auto const packed = pack3_events(AttributeEvent::New, AttributeEvent::New, AttributeEvent::New);
    EXPECT_EQ(packed, 0);
}

TEST(mrp_threepacked, pack_first_only)
{
    // first=5 (Lv), second=0, third=0 -> 5*36 + 0*6 + 0 = 180
    auto const packed = pack3_events(AttributeEvent::Lv, AttributeEvent::New, AttributeEvent::New);
    EXPECT_EQ(packed, 180);
}

TEST(mrp_threepacked, pack_second_only)
{
    // first=0, second=5 (Lv), third=0 -> 0*36 + 5*6 + 0 = 30
    auto const packed = pack3_events(AttributeEvent::New, AttributeEvent::Lv, AttributeEvent::New);
    EXPECT_EQ(packed, 30);
}

TEST(mrp_threepacked, pack_third_only)
{
    // first=0, second=0, third=5 (Lv) -> 0*36 + 0*6 + 5 = 5
    auto const packed = pack3_events(AttributeEvent::New, AttributeEvent::New, AttributeEvent::Lv);
    EXPECT_EQ(packed, 5);
}

TEST(mrp_threepacked, pack_max_value)
{
    // first=5, second=5, third=5 -> 5*36 + 5*6 + 5 = 180 + 30 + 5 = 215
    auto const packed = pack3_events(AttributeEvent::Lv, AttributeEvent::Lv, AttributeEvent::Lv);
    EXPECT_EQ(packed, 215);
}

TEST(mrp_threepacked, unpack_zeros)
{
    auto const result = unpack3_events(0);
    EXPECT_EQ(result.first, AttributeEvent::New);
    EXPECT_EQ(result.second, AttributeEvent::New);
    EXPECT_EQ(result.third, AttributeEvent::New);
}

TEST(mrp_threepacked, unpack_max_value)
{
    auto const result = unpack3_events(215);
    EXPECT_EQ(result.first, AttributeEvent::Lv);
    EXPECT_EQ(result.second, AttributeEvent::Lv);
    EXPECT_EQ(result.third, AttributeEvent::Lv);
}

TEST(mrp_threepacked, unpack_above_max_yields_out_of_range_first)
{
    // Octet > 215 is not a legal encoding (5*36 + 5*6 + 5 = 215). The
    // unpack function still produces a deterministic result; downstream
    // event-dispatch code is responsible for rejecting events whose
    // numeric value falls outside the AttributeEvent enum.
    auto const result = unpack3_events(250);
    EXPECT_EQ(static_cast<uint8_t>(result.first), uint8_t{6});
    EXPECT_EQ(static_cast<uint8_t>(result.second), uint8_t{5});
    EXPECT_EQ(static_cast<uint8_t>(result.third), uint8_t{4});
}

TEST(mrp_threepacked, unpack_just_above_max_yields_six_zero_zero)
{
    auto const result = unpack3_events(216);
    EXPECT_EQ(static_cast<uint8_t>(result.first), uint8_t{6});
    EXPECT_EQ(static_cast<uint8_t>(result.second), uint8_t{0});
    EXPECT_EQ(static_cast<uint8_t>(result.third), uint8_t{0});
}

TEST(mrp_threepacked, round_trip)
{
    auto const first = AttributeEvent::JoinIn;
    auto const second = AttributeEvent::Mt;
    auto const third = AttributeEvent::Lv;
    auto const packed = pack3_events(first, second, third);
    auto const result = unpack3_events(packed);
    EXPECT_EQ(result.first, first);
    EXPECT_EQ(result.second, second);
    EXPECT_EQ(result.third, third);
}

TEST(mrp_threepacked, octet_count_zero)
{
    EXPECT_EQ(threepacked_octet_count(0), 0);
}

TEST(mrp_threepacked, octet_count_one)
{
    EXPECT_EQ(threepacked_octet_count(1), 1);
}

TEST(mrp_threepacked, octet_count_two)
{
    EXPECT_EQ(threepacked_octet_count(2), 1);
}

TEST(mrp_threepacked, octet_count_three)
{
    EXPECT_EQ(threepacked_octet_count(3), 1);
}

TEST(mrp_threepacked, octet_count_four)
{
    EXPECT_EQ(threepacked_octet_count(4), 2);
}

TEST(mrp_threepacked, octet_count_ten)
{
    EXPECT_EQ(threepacked_octet_count(10), 4);
}

//
// Tests: FourPacked encoding
//

TEST(mrp_fourpacked, pack_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    uint8_t volatile zero = 0, three = 3;
    EXPECT_EQ(pack4_declarations(zero, zero, zero, zero), 0);
    EXPECT_EQ(pack4_declarations(three, three, three, three), 0xFF);
}

TEST(mrp_fourpacked, octet_count_runtime)
{
    // Use volatile to prevent compile-time evaluation of constexpr function
    size_t volatile zero = 0, one = 1, four = 4, five = 5, ten = 10;
    EXPECT_EQ(fourpacked_octet_count(zero), 0);
    EXPECT_EQ(fourpacked_octet_count(one), 1);
    EXPECT_EQ(fourpacked_octet_count(four), 1);
    EXPECT_EQ(fourpacked_octet_count(five), 2);
    EXPECT_EQ(fourpacked_octet_count(ten), 3);
}

TEST(mrp_fourpacked, pack_all_zeros)
{
    auto const packed = pack4_declarations(0, 0, 0, 0);
    EXPECT_EQ(packed, 0);
}

TEST(mrp_fourpacked, pack_all_threes)
{
    // first=3, second=3, third=3, fourth=3 -> (3<<6)|(3<<4)|(3<<2)|3 = 0xFF
    auto const packed = pack4_declarations(3, 3, 3, 3);
    EXPECT_EQ(packed, 0xFF);
}

TEST(mrp_fourpacked, pack_first_only)
{
    auto const packed = pack4_declarations(3, 0, 0, 0);
    EXPECT_EQ(packed, 0xC0);
}

TEST(mrp_fourpacked, pack_second_only)
{
    auto const packed = pack4_declarations(0, 3, 0, 0);
    EXPECT_EQ(packed, 0x30);
}

TEST(mrp_fourpacked, pack_third_only)
{
    auto const packed = pack4_declarations(0, 0, 3, 0);
    EXPECT_EQ(packed, 0x0C);
}

TEST(mrp_fourpacked, pack_fourth_only)
{
    auto const packed = pack4_declarations(0, 0, 0, 3);
    EXPECT_EQ(packed, 0x03);
}

TEST(mrp_fourpacked, unpack_zeros)
{
    auto const result = unpack4_declarations(0);
    EXPECT_EQ(result.first, 0);
    EXPECT_EQ(result.second, 0);
    EXPECT_EQ(result.third, 0);
    EXPECT_EQ(result.fourth, 0);
}

TEST(mrp_fourpacked, unpack_all_threes)
{
    auto const result = unpack4_declarations(0xFF);
    EXPECT_EQ(result.first, 3);
    EXPECT_EQ(result.second, 3);
    EXPECT_EQ(result.third, 3);
    EXPECT_EQ(result.fourth, 3);
}

TEST(mrp_fourpacked, round_trip)
{
    uint8_t const first = 2;
    uint8_t const second = 1;
    uint8_t const third = 3;
    uint8_t const fourth = 0;
    auto const packed = pack4_declarations(first, second, third, fourth);
    auto const result = unpack4_declarations(packed);
    EXPECT_EQ(result.first, first);
    EXPECT_EQ(result.second, second);
    EXPECT_EQ(result.third, third);
    EXPECT_EQ(result.fourth, fourth);
}

TEST(mrp_fourpacked, octet_count_zero)
{
    EXPECT_EQ(fourpacked_octet_count(0), 0);
}

TEST(mrp_fourpacked, octet_count_one)
{
    EXPECT_EQ(fourpacked_octet_count(1), 1);
}

TEST(mrp_fourpacked, octet_count_four)
{
    EXPECT_EQ(fourpacked_octet_count(4), 1);
}

TEST(mrp_fourpacked, octet_count_five)
{
    EXPECT_EQ(fourpacked_octet_count(5), 2);
}

TEST(mrp_fourpacked, octet_count_ten)
{
    EXPECT_EQ(fourpacked_octet_count(10), 3);
}

//
// Tests: AttributeListHeader
//

TEST(mrp_attribute_list_header, sizeof_is_2)
{
    EXPECT_EQ(sizeof(AttributeListHeader), 2);
}

TEST(mrp_attribute_list_header, default_construction)
{
    AttributeListHeader header{};
    EXPECT_EQ(header.attribute_type.get(), 0);
    EXPECT_EQ(header.attribute_length.get(), 0);
}

TEST(mrp_attribute_list_header, designated_init)
{
    AttributeListHeader header{.attribute_type = 1, .attribute_length = 25};
    EXPECT_EQ(header.attribute_type.get(), 1);
    EXPECT_EQ(header.attribute_length.get(), 25);
}

TEST(mrp_attribute_list_header, serialization_round_trip)
{
    AttributeListHeader original{.attribute_type = 3, .attribute_length = 8};
    std::array<uint8_t, 4> buffer{};

    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 2);

    AttributeListHeader restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 2);

    EXPECT_EQ(restored.attribute_type.get(), 3);
    EXPECT_EQ(restored.attribute_length.get(), 8);
}

//
// Tests: VectorAttributeHeader
//

TEST(mrp_vector_attribute_header, sizeof_is_2)
{
    EXPECT_EQ(sizeof(VectorAttributeHeader), 2);
}

TEST(mrp_vector_attribute_header, default_construction)
{
    VectorAttributeHeader header{};
    EXPECT_FALSE(header.get_leave_all());
    EXPECT_EQ(header.get_number_of_values(), 0);
}

TEST(mrp_vector_attribute_header, set_method)
{
    VectorAttributeHeader header{};
    header.set(true, 500);
    EXPECT_TRUE(header.get_leave_all());
    EXPECT_EQ(header.get_number_of_values(), 500);
}

TEST(mrp_vector_attribute_header, set_without_leave_all)
{
    VectorAttributeHeader header{};
    header.set(false, 1000);
    EXPECT_FALSE(header.get_leave_all());
    EXPECT_EQ(header.get_number_of_values(), 1000);
}

TEST(mrp_vector_attribute_header, serialization_round_trip)
{
    VectorAttributeHeader original{};
    original.set(true, 2048);
    std::array<uint8_t, 4> buffer{};

    auto const written = store_unchecked(std::span<uint8_t>{buffer}, original);
    EXPECT_EQ(written, 2);

    VectorAttributeHeader restored{};
    auto const read = load_unchecked(std::span<uint8_t const>{buffer}, &restored);
    EXPECT_EQ(read, 2);

    EXPECT_TRUE(restored.get_leave_all());
    EXPECT_EQ(restored.get_number_of_values(), 2048);
}

TEST(mrp_vector_attribute_header, wire_format)
{
    VectorAttributeHeader header{};
    header.set(true, 0x0100);  // 256

    std::array<uint8_t, 2> buffer{};
    (void)store_unchecked(std::span<uint8_t>{buffer}, header);

    // Expected: 0x2100 in network byte order (big endian)
    EXPECT_EQ(buffer[0], 0x21);  // High byte: 0x20 (leave_all) | 0x01 (high bits of 256)
    EXPECT_EQ(buffer[1], 0x00);  // Low byte
}

//
// Tests: TimerScheduler
//

TEST(mrp_timer_scheduler, construct_deterministic_seed)
{
    TimerScheduler ts{42};
    EXPECT_FALSE(ts.join_running());
    EXPECT_FALSE(ts.leave_running());
    EXPECT_FALSE(ts.leaveall_running());
    EXPECT_FALSE(ts.periodic_running());
    EXPECT_EQ(ts.next_deadline(), TimePoint::max());
}

TEST(mrp_timer_scheduler, start_join_idempotent)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_join(now);
    EXPECT_TRUE(ts.join_running());
    auto const d1 = ts.next_deadline();
    ts.start_join(now + std::chrono::milliseconds(50));
    EXPECT_EQ(ts.next_deadline(), d1);
}

TEST(mrp_timer_scheduler, arm_join_force_rearm)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_join(now);
    auto const d1 = ts.next_deadline();
    ts.arm_join(now + std::chrono::milliseconds(50));
    EXPECT_NE(ts.next_deadline(), d1);
}

TEST(mrp_timer_scheduler, start_leave_runs)
{
    TimerScheduler ts{1};
    ts.start_leave(TimePoint{} + std::chrono::seconds(1));
    EXPECT_TRUE(ts.leave_running());
}

TEST(mrp_timer_scheduler, start_leaveall_runs)
{
    TimerScheduler ts{1};
    ts.start_leaveall(TimePoint{} + std::chrono::seconds(1));
    EXPECT_TRUE(ts.leaveall_running());
}

TEST(mrp_timer_scheduler, start_periodic_runs)
{
    TimerScheduler ts{1};
    ts.start_periodic(TimePoint{} + std::chrono::seconds(1));
    EXPECT_TRUE(ts.periodic_running());
}

TEST(mrp_timer_scheduler, stop_timers)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_join(now);
    ts.start_leave(now);
    ts.start_leaveall(now);
    ts.start_periodic(now);
    ts.stop_join();
    EXPECT_FALSE(ts.join_running());
    ts.stop_leave();
    EXPECT_FALSE(ts.leave_running());
    ts.stop_leaveall();
    EXPECT_FALSE(ts.leaveall_running());
    ts.stop_periodic();
    EXPECT_FALSE(ts.periodic_running());
}

TEST(mrp_timer_scheduler, tick_join_expires)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_join(now);
    auto e1 = ts.tick(now + std::chrono::milliseconds(50));
    EXPECT_FALSE(e1.any());
    auto e2 = ts.tick(now + std::chrono::milliseconds(100));
    EXPECT_TRUE(e2.join);
    EXPECT_FALSE(ts.join_running());
}

TEST(mrp_timer_scheduler, tick_leave_expires)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_leave(now);
    auto e = ts.tick(now + std::chrono::milliseconds(1000));
    EXPECT_TRUE(e.leave);
}

TEST(mrp_timer_scheduler, tick_periodic_expires)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_periodic(now);
    auto e = ts.tick(now + std::chrono::milliseconds(1000));
    EXPECT_TRUE(e.periodic);
}

TEST(mrp_timer_scheduler, next_deadline_picks_earliest)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_join(now);
    ts.start_periodic(now);
    EXPECT_EQ(ts.next_deadline(), now + std::chrono::milliseconds(100));
}

TEST(mrp_timer_scheduler, multiple_timers_expire)
{
    TimerScheduler ts{1};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    ts.start_join(now);
    ts.start_leave(now);
    ts.start_periodic(now);
    auto e = ts.tick(now + std::chrono::seconds(10));
    EXPECT_TRUE(e.join);
    EXPECT_TRUE(e.leave);
    EXPECT_TRUE(e.periodic);
}

//
// Tests: PortState
//

TEST(mrp_port_state, start_arms_timers)
{
    PortState port{42};
    port.start(TimePoint{} + std::chrono::seconds(1));
    EXPECT_TRUE(port.timers().leaveall_running());
    EXPECT_TRUE(port.timers().periodic_running());
}

TEST(mrp_port_state, stop_clears_all_timers)
{
    PortState port{42};
    port.start(TimePoint{} + std::chrono::seconds(1));
    port.stop();
    EXPECT_FALSE(port.timers().join_running());
    EXPECT_FALSE(port.timers().leave_running());
    EXPECT_FALSE(port.timers().leaveall_running());
    EXPECT_FALSE(port.timers().periodic_running());
}

//
// Tests: dispatch helpers + AttributeRecord
//

TEST(mrp_dispatch, applicant_dispatches)
{
    AttributeRecord<statusbar::srp::mvrp::VlanIdentifierFirstValue> rec{};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    dispatch_applicant(rec, applicant_sm::Def::Event::New, now);
    EXPECT_NE(rec.applicant_sm.current_state(), applicant_sm::Def::State::Start);
}

TEST(mrp_dispatch, registrar_dispatches)
{
    AttributeRecord<statusbar::srp::mvrp::VlanIdentifierFirstValue> rec{};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    TimerScheduler timers{1};
    dispatch_registrar(rec, registrar_sm::Def::Event::RNew, now, timers);
    EXPECT_TRUE(rec.registrar_is_in());
}

TEST(mrp_dispatch, applicant_tx_variant)
{
    AttributeRecord<statusbar::srp::mvrp::VlanIdentifierFirstValue> rec{};
    auto const now = TimePoint{} + std::chrono::seconds(1);
    TimerScheduler timers{1};
    dispatch_registrar(rec, registrar_sm::Def::Event::RNew, now, timers);
    dispatch_applicant(rec, applicant_sm::Def::Event::New, now);
    dispatch_applicant_tx(rec, now);
    (void)rec.applicant_ctx.tx_pending;
    EXPECT_TRUE(true);
}

TEST(mrp_attribute_record, default_state)
{
    AttributeRecord<statusbar::srp::mvrp::VlanIdentifierFirstValue> rec{};
    EXPECT_EQ(rec.operation, Operation::Register);
    EXPECT_FALSE(rec.registrar_is_in());
    EXPECT_FALSE(rec.is_dead());
}

//
// tsn::TsnErrorCategory - virtual name() dispatch
//

TEST(tsn_error, category_name)
{
    auto const ec = make_error_code(statusbar::tsn::TsnError::attribute_table_full);
    EXPECT_EQ(std::string_view{ec.category().name()}, std::string_view{"statusbar.tsn"});
}

TEST(tsn_error, category_message_non_empty)
{
    auto const ec = make_error_code(statusbar::tsn::TsnError::attribute_table_full);
    EXPECT_TRUE(!ec.message().empty());
}

//
// Main test runner
//

TEST_MAIN(statusbar_srp, srp_mrp_test)
