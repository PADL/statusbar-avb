// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ListenerStreams RX-slot tests (kit phases 1+2): spec-shaped slot creation,
// the per-stream consume registration mechanics (pending-before-open, direct
// rebind after open), and the menu/selection principle — a registration for a
// stream index the model never declares stays pending and inert, never an
// error. CRF input slots are rejected until media-clock recovery (phase 3).
//
// Positive frame->consume delivery requires a connected ACMP listener + live
// frames and is exercised on hardware; the decode paths themselves are
// covered by the avtp deserializer unit tests.

#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

constexpr uint64_t AAF_96K_INT32_8CH = 0x020702200200C000ULL;
constexpr uint64_t CRF_MILAN_48K = 0x041060010000BB80ULL;

auto load_file(std::filesystem::path const& path) -> std::vector<uint8_t>
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return {};
    }
    auto const size = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    stream_read(file, data);
    return data;
}

// Components donor: ListenerStreams binds a NanoAvbComponents&; the cheapest
// real one comes from a created (not started) tone entity, as the gate tests do.
auto make_donor() -> std::unique_ptr<AvbEntityToneGenerator>
{
    auto blob = load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone.bin");
    AvbEntityAudioIOConfig config{};
    config.entity_id = ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.descriptor_storage_blob = std::move(blob);
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    return std::move(*result);
}

auto make_aaf_spec(uint16_t index) -> StreamSpec
{
    StreamSpec spec{};
    spec.index = index;
    spec.format_word = AAF_96K_INT32_8CH;
    spec.format = decode_stream_format(AAF_96K_INT32_8CH);
    return spec;
}

/// What MSRP holds for a stream this listener declared, nothing when it never did.
auto srp_state_of(nanoavb::NanoAvbComponents& components, ieee::Eui64 const& id) -> std::optional<nanoavb::ListenerReservationState>
{
    tsn::StreamId sid{};
    (void)tsn::load_unchecked(id.span(), &sid);
    for (auto const& stream : components.msrp_handler.listener_streams()) {
        if (stream.stream_id == sid) {
            return stream.state;
        }
    }
    return std::nullopt;
}

constexpr ieee::Eui64 STREAM_1{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x01};
constexpr ieee::Eui64 STREAM_2{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x02};
constexpr ieee::Eui48 GROUP{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};

}  // namespace

// IEEE 1722.1-2021 8.2.1.13: the group joined for a talker is its destination
// when that is a multicast address; a unicast or zero destination has none.
TEST(listener_connection, only_a_multicast_destination_is_a_group_to_join)
{
    EXPECT_FALSE(ListenerStreams::group_to_join(ieee::Eui48{}).has_value());
    EXPECT_FALSE(ListenerStreams::group_to_join(ieee::Eui48{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x01}).has_value());
    auto const group = ListenerStreams::group_to_join(GROUP);
    EXPECT_TRUE(group.has_value());
    EXPECT_EQ(group->to_uint64(), 0x91E0F0001234ULL);
}

// The disconnect works from what the slot recorded at connect time, not from
// the ACMP state machine's copy of the stream, which it has cleared by the
// time the hook runs: the declaration withdrawn is the one that was made.
TEST(listener_connection, a_disconnect_withdraws_what_the_connect_declared)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());

    // A SUCCESS whose destination is still zero: declared, nothing to join.
    ls.on_listener_connected(0, STREAM_1, ieee::Eui48{}, sm::TimePoint{});
    EXPECT_EQ(ls.slot_for(0)->connection.stream_id, STREAM_1);
    EXPECT_FALSE(ls.slot_for(0)->connection.group.has_value());
    auto const declared = srp_state_of(donor->components(), STREAM_1);
    EXPECT_TRUE(declared.has_value());
    EXPECT_TRUE(declared.has_value() && *declared != nanoavb::ListenerReservationState::Idle);

    ls.on_listener_disconnected(0, sm::TimePoint{});
    EXPECT_FALSE(ls.slot_for(0)->connection.stream_id.is_set());
    auto const withdrawn = srp_state_of(donor->components(), STREAM_1);
    EXPECT_TRUE(withdrawn.has_value() && *withdrawn == nanoavb::ListenerReservationState::Idle);
}

// One slot holds one connection: connecting it to another stream withdraws
// the first declaration rather than leaving two standing.
TEST(listener_connection, a_second_connect_replaces_the_first_declaration)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    ls.on_listener_connected(0, STREAM_2, GROUP, sm::TimePoint{});
    EXPECT_EQ(ls.slot_for(0)->connection.stream_id, STREAM_2);
    auto const first = srp_state_of(donor->components(), STREAM_1);
    auto const second = srp_state_of(donor->components(), STREAM_2);
    EXPECT_TRUE(first.has_value() && *first == nanoavb::ListenerReservationState::Idle);
    EXPECT_TRUE(second.has_value() && *second != nanoavb::ListenerReservationState::Idle);
    // No port in this test, so no group was joined -- the record says so
    // rather than claiming a membership nothing holds.
    EXPECT_FALSE(ls.slot_for(0)->connection.group.has_value());
}

// A slot contributes one attachment per stream it holds: the same CONNECT
// repeated declares nothing more, so the one disconnect withdraws it all.
TEST(listener_connection, the_same_stream_connected_twice_is_withdrawn_by_one_disconnect)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_EQ(ls.slot_for(0)->connection.stream_id, STREAM_1);
    auto const held = srp_state_of(donor->components(), STREAM_1);
    EXPECT_TRUE(held.has_value() && *held != nanoavb::ListenerReservationState::Idle);

    ls.on_listener_disconnected(0, sm::TimePoint{});
    auto const gone = srp_state_of(donor->components(), STREAM_1);
    EXPECT_TRUE(gone.has_value() && *gone == nanoavb::ListenerReservationState::Idle);
    EXPECT_FALSE(ls.slot_for(0)->connection.stream_id.is_set());
}

// A stream index with no RX slot is not one of our inputs: nothing is
// declared for it and nothing joined, and no slot is invented.
TEST(listener_connection, a_connect_for_an_index_without_a_slot_declares_nothing)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    ls.on_listener_connected(7, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_FALSE(srp_state_of(donor->components(), STREAM_1).has_value());
    EXPECT_EQ(ls.slot_for(7), nullptr);
    ls.on_listener_disconnected(7, sm::TimePoint{});  // and nothing to undo
}

TEST(listener_consume, pending_binds_on_open_unknown_stays_inert)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};

    // Register BEFORE the slot exists (the normal pre-start pattern) plus one
    // for an index no model will ever declare (the menu is bigger than the
    // selection): both are accepted, neither errors.
    ls.set_consume(0, [](uint8_t, std::span<float const>, uint64_t, uint64_t) {});
    ls.set_consume(9, [](uint8_t, std::span<float const>, uint64_t, uint64_t) {});

    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    auto* slot = ls.slot_for(0);
    EXPECT_NE(slot, nullptr);
    EXPECT_TRUE(static_cast<bool>(slot->consume));  // pending registration bound at open
    EXPECT_EQ(ls.slot_for(9), nullptr);             // unknown index: no slot, no error

    // Re-registration after open binds directly (replaces the callback).
    ls.set_consume(0, [](uint8_t, std::span<float const>, uint64_t, uint64_t) {});
    EXPECT_TRUE(static_cast<bool>(ls.slot_for(0)->consume));
}

TEST(listener_consume, slot_shapes_follow_specs)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};

    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_NE(ls.slot_of(StreamKind::aaf), nullptr);
    EXPECT_TRUE(ls.slot_of(StreamKind::aaf)->aaf.has_value());
    EXPECT_FALSE(ls.slot_of(StreamKind::aaf)->am824.has_value());

    // Unknown format words never build a slot.
    StreamSpec other{};
    other.index = 2;
    EXPECT_FALSE(ls.open_stream(other).has_value());
}

TEST(listener_crf, crf_input_slot_delivers_timestamps)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};

    // Register the CRF consumer BEFORE the slot exists (pending), plus one
    // for an index the model never declares (menu/selection: inert).
    static uint64_t last_ts = 0;
    static int calls = 0;
    last_ts = 0;
    calls = 0;
    ls.set_crf(1, [](uint64_t ts, uint16_t /*idx*/, int64_t /*rx*/) {
        last_ts = ts;
        ++calls;
    });
    ls.set_crf(9, [](uint64_t, uint16_t, int64_t) { ++calls; });

    StreamSpec crf{};
    crf.index = 1;
    crf.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(ls.open_stream(crf).has_value());  // CRF input supported (phase 3a)
    auto* slot = ls.slot_for(1);
    EXPECT_NE(slot, nullptr);
    EXPECT_TRUE(slot->crf.has_value());
    EXPECT_TRUE(static_cast<bool>(slot->on_crf));  // pending registration bound
    EXPECT_EQ(ls.slot_for(9), nullptr);            // inert registration: no slot, no error

    // The bound consumer is the slot's delivery path (frame->consume delivery
    // itself requires a connected ACMP listener; covered on hardware).
    slot->on_crf(123456789ULL, 0, 1000);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(last_ts, 123456789ULL);
}

TEST(listener_drain, both_drain_forms_are_a_no_op_without_a_port)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{4'000'000'000LL};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    static_assert(std::is_same_v<net::RxTimePoint, statusbar::realtime::MonotonicRawClock::time_point>);

    // The drain takes the wake on both clocks and touches nothing when no
    // port is attached.
    EXPECT_EQ(ls.drain_rx(statusbar::realtime::GptpClock0::from_ns(4'000'000'000LL), net::RxTimePoint{}), 0U);
    EXPECT_EQ(ls.current_gptp_ns(), 4'000'000'000LL);
}

TEST(listener_consume, close_rx_keeps_the_consumers_for_the_next_open)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    // Registered once, before the first start, as an entity does.
    int delivered = 0;
    ls.set_consume(0, [&delivered](uint8_t, std::span<float const>, uint64_t, uint64_t) { ++delivered; });

    // A run, then its end: no slots left.
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_TRUE(static_cast<bool>(ls.slot_for(0)->consume));
    ls.close_rx();
    EXPECT_EQ(ls.slot_for(0), nullptr);
    EXPECT_EQ(ls.slot_of(StreamKind::aaf), nullptr);

    // The next run opens the same stream: one slot, and the consumer it was
    // given before the first run is bound to it again.
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    auto* slot = ls.slot_for(0);
    EXPECT_NE(slot, nullptr);
    EXPECT_TRUE(static_cast<bool>(slot->consume));
    slot->consume(0, {}, 0, 0);
    EXPECT_EQ(delivered, 1);
    EXPECT_EQ(ls.slot_of(StreamKind::aaf), slot);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_streams_test)
