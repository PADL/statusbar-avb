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

#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

constexpr uint64_t AAF_96K_INT32_8CH = 0x020702200200C000ULL;
constexpr uint64_t CRF_MILAN_48K = 0x041060010000BB80ULL;
constexpr uint64_t AM824_48K_8CH = 0x00A0020840000800ULL;

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
    // The interface's address is told, so creating the entity asks the system for nothing.
    config.interface_mac = ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
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

// A Milan sink's connection names the Talker attribute it probed, and the
// MSRP handler is told to follow that one; an IEEE 1722.1 connection names
// none. The slot remembers which it was for the disconnect's log line.
TEST(listener_connection, a_settled_sink_hands_what_it_probed_to_msrp_and_a_connection_hands_nothing)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    auto const followed = [&donor](ieee::Eui64 const& stream) {
        nanoavb::StreamId sid{};
        sid.from_uint64(stream.to_uint64());
        return donor->components().msrp_handler.get_listener_stream(sid)->follows;
    };

    nanoavb::TalkerMatch const probed{.dest_address = GROUP, .vlan_id = 2};
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{}, probed);
    EXPECT_TRUE(ls.slot_for(0)->connection.settled);
    EXPECT_TRUE(followed(STREAM_1).has_value());
    EXPECT_TRUE(followed(STREAM_1).has_value() && followed(STREAM_1)->vlan_id == 2 && followed(STREAM_1)->dest_address == GROUP);
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_FALSE(ls.slot_for(0)->connection.settled);

    ls.on_listener_connected(0, STREAM_2, GROUP, sm::TimePoint{});
    EXPECT_FALSE(ls.slot_for(0)->connection.settled);
    EXPECT_FALSE(followed(STREAM_2).has_value());
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
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

    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
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

    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    auto const gone = srp_state_of(donor->components(), STREAM_1);
    EXPECT_TRUE(gone.has_value() && *gone == nanoavb::ListenerReservationState::Idle);
    EXPECT_FALSE(ls.slot_for(0)->connection.stream_id.is_set());
}

// A stream index with no RX slot is not one of our inputs: nothing is
// declared for it and nothing joined, and no slot is invented.
// The RX thread matches a frame to its input by the connection the reactor
// published at CONNECT_RX and cleared at DISCONNECT_RX, not by reading the
// ACMP listener's record under the reactor: here that record never connects
// and the match follows the published connection alone.
TEST(listener_connection, the_drain_matches_frames_by_the_published_connection)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());

    std::array<uint8_t, 24> frame{};
    frame[0] = avtp::AvtpSubtype::aaf;
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));
    auto other = frame;
    span_copy(std::span<uint8_t>{other}.subspan(4, 8), make_const_span(STREAM_2));

    EXPECT_FALSE(ls.frame_is_for_listener(0, frame));  // nothing connected yet
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_FALSE(donor->components().acmp_listener.is_connected(0));
    EXPECT_TRUE(ls.frame_is_for_listener(0, frame));
    EXPECT_FALSE(ls.frame_is_for_listener(0, other));
    EXPECT_FALSE(ls.frame_is_for_listener(1, frame));  // no such input
    EXPECT_FALSE(ls.frame_is_for_listener(0, std::span<uint8_t const>{frame}.first(11)));

    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_FALSE(ls.frame_is_for_listener(0, frame));
}

// The RX thread matches a frame to its slot by testing each slot in hand, not
// by looking the slot up again by its index inside the walk: with several
// inputs the second lookup made every frame cost a walk per slot, and a
// frame for no slot - our own transmit, another stream on the segment - paid
// for all of it. Same frames, same slots: the frame for the last of three
// inputs reaches that input alone, a stranger's frame reaches none, and a
// frame too short to carry a stream id is refused on this path too.
TEST(listener_connection, a_frame_is_matched_against_the_slot_in_hand)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    for (uint16_t index = 0; index < 3; ++index) {
        EXPECT_TRUE(ls.open_stream(make_aaf_spec(index)).has_value());
    }
    constexpr ieee::Eui64 STREAM_3{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x03};
    ls.on_listener_connected(2, STREAM_3, GROUP, sm::TimePoint{});

    auto const seen = [&](uint16_t const index) {
        auto const* slot = ls.slot_for(index);
        return slot == nullptr ? uint64_t{0} : slot->rx_packets.load() + slot->rx_bad.load();
    };

    std::array<uint8_t, 24> frame{};
    frame[0] = avtp::AvtpSubtype::aaf;
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_3));
    ls.on_stream_rx_frame(frame, 0);
    EXPECT_EQ(seen(2), uint64_t{1});
    EXPECT_EQ(seen(0), uint64_t{0});
    EXPECT_EQ(seen(1), uint64_t{0});

    auto stranger = frame;
    span_copy(std::span<uint8_t>{stranger}.subspan(4, 8), make_const_span(STREAM_1));
    ls.on_stream_rx_frame(stranger, 0);
    ls.on_stream_rx_frame(std::span<uint8_t const>{frame}.first(11), 0);  // the id cut short
    EXPECT_EQ(seen(2), uint64_t{1});
    EXPECT_EQ(seen(0) + seen(1), uint64_t{0});
    EXPECT_TRUE(ls.frame_is_for_listener(2, frame));
    EXPECT_FALSE(ls.frame_is_for_listener(2, std::span<uint8_t const>{frame}.first(11)));
}

TEST(listener_connection, a_connect_for_an_index_without_a_slot_declares_nothing)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    ls.on_listener_connected(7, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_FALSE(srp_state_of(donor->components(), STREAM_1).has_value());
    EXPECT_EQ(ls.slot_for(7), nullptr);
    ls.on_listener_disconnected(7, sm::TimePoint{}, StreamInputLoss::released);  // and nothing to undo
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

// A consumer may take its times as avtp::Nanoseconds. It is kept beside the
// consumer in unsigned nanoseconds, and both are handed every channel of
// every packet, with the same counts.
TEST(listener_consume, a_timed_consumer_is_handed_the_same_times_as_nanoseconds)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});

    // One AAF packet of the slot's format, as a talker builds it: twelve
    // samples presented 2 ms after the time it is received at.
    constexpr avtp::Nanoseconds NOW{1'795'000'000'000'000'000};
    constexpr avtp::Nanoseconds AHEAD{2'000'000};
    auto const& format = ls.slot_for(0)->spec.format;
    avtp::AafStreamOutputContext talker{
        tsn::StreamId{},
        format.aaf_format,
        *avtp::aaf_sample_rate_from_hz(format.sample_rate_hz),
        format.channels,
        format.bit_depth,
        0};
    avtp::AafPdu pdu{};
    std::array<uint8_t, avtp::AafPdu::HEADER_LENGTH + (12 * 8 * 4)> frame{};
    auto const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    EXPECT_EQ(avtp::aaf_stream_serialize(talker, pdu, payload, 12, NOW + AHEAD, [](uint8_t, std::span<float>) {}), payload.size());
    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));

    int timed_calls = 0;
    int unsigned_calls = 0;
    avtp::Nanoseconds timed_pts{};
    avtp::Nanoseconds timed_period{};
    uint64_t unsigned_pts = 0;
    ls.set_consume_timed(0, [&](uint8_t, std::span<float const> s, avtp::Nanoseconds pts, avtp::Nanoseconds period) {
        ++timed_calls;
        timed_pts = pts;
        timed_period = period;
        EXPECT_EQ(s.size(), 12U);
    });
    ls.set_consume(0, [&](uint8_t, std::span<float const>, uint64_t pts, uint64_t) {
        ++unsigned_calls;
        unsigned_pts = pts;
    });

    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{1});
    EXPECT_EQ(timed_calls, 8);     // once a channel
    EXPECT_EQ(unsigned_calls, 8);  // the other consumer is still served
    EXPECT_TRUE(timed_pts == NOW + AHEAD);
    EXPECT_TRUE(timed_period == avtp::Nanoseconds{10416});
    EXPECT_EQ(unsigned_pts, static_cast<uint64_t>((NOW + AHEAD).count()));

    // The receive time as a bare count reaches the same code.
    ls.on_stream_rx_frame(frame, NOW.count());
    EXPECT_EQ(timed_calls, 16);
}

namespace {

using AafFrame = std::array<uint8_t, avtp::AafPdu::HEADER_LENGTH + (12 * 8 * 4)>;

/// One AAF packet of @p format for STREAM_1, twelve samples presented at @p presentation.
auto aaf_frame_of(StreamFormatFields const& format, avtp::Nanoseconds const presentation) -> AafFrame
{
    avtp::AafStreamOutputContext talker{
        tsn::StreamId{},
        format.aaf_format,
        *avtp::aaf_sample_rate_from_hz(format.sample_rate_hz),
        format.channels,
        format.bit_depth,
        0};
    avtp::AafPdu pdu{};
    AafFrame frame{};
    auto const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    EXPECT_EQ(avtp::aaf_stream_serialize(talker, pdu, payload, 12, presentation, [](uint8_t, std::span<float>) {}), payload.size());
    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));
    return frame;
}

}  // namespace

TEST(listener_stopped, a_stopped_input_discards_its_frames_and_a_started_one_takes_them_again)
{
    // Milan v1.3 5.3.8.7: "A PAAD-AE having a stopped Stream Input shall
    // discard the Stream AVTPDUs it receives."
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    constexpr avtp::Nanoseconds NOW{1'795'000'000'000'000'000};
    auto const frame = aaf_frame_of(ls.slot_for(0)->spec.format, NOW + avtp::Nanoseconds{2'000'000});
    int calls = 0;
    ls.set_consume(0, [&](uint8_t, std::span<float const>, uint64_t, uint64_t) { ++calls; });
    auto const& slot = *ls.slot_for(0);

    EXPECT_FALSE(ls.stream_stopped(0));
    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(calls, 8);
    EXPECT_EQ(slot.rx_packets.load(), uint64_t{1});
    EXPECT_EQ(slot.counters.frames_rx.load(), 1U);
    EXPECT_EQ(slot.counters.timestamp_valid.load(), 1U);

    // Stopped: nothing is decoded and no consumer is called. The frame is
    // counted as received and as nothing else.
    ls.set_stream_stopped(0, true);
    EXPECT_TRUE(ls.stream_stopped(0));
    for (int i = 0; i < 3; ++i) {
        ls.on_stream_rx_frame(frame, NOW);
    }
    EXPECT_EQ(calls, 8);
    EXPECT_EQ(slot.rx_packets.load(), uint64_t{1});
    EXPECT_EQ(slot.rx_samples.load(), uint64_t{12});
    EXPECT_EQ(slot.counters.frames_rx.load(), 4U);
    EXPECT_EQ(slot.counters.timestamp_valid.load(), 1U);
    // It is still connected: stopping is not unbinding.
    EXPECT_TRUE(ls.frame_is_for_listener(0, frame));

    ls.set_stream_stopped(0, false);
    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(calls, 16);
    EXPECT_EQ(slot.rx_packets.load(), uint64_t{2});
    EXPECT_EQ(slot.counters.frames_rx.load(), 5U);
    EXPECT_EQ(slot.counters.seq_num_mismatch.load(), 0U);  // the same frame again, and no error: the run began afresh
}

TEST(listener_stopped, the_stopped_state_is_kept_by_stream_index_and_outlives_the_slots)
{
    // The entity's stop and start close and open the slots; an input that
    // was stopped is stopped still. One input's state is not another's.
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.set_stream_stopped(0, true);
    EXPECT_TRUE(ls.stream_stopped(0));
    EXPECT_FALSE(ls.stream_stopped(1));
    ls.close_rx();
    EXPECT_TRUE(ls.stream_stopped(0));
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_TRUE(ls.stream_stopped(0));
    // An index past the entity's streams is never stopped, and setting it does nothing.
    ls.set_stream_stopped(60000, true);
    EXPECT_FALSE(ls.stream_stopped(60000));
}

TEST(listener_consume, a_timed_consumer_is_kept_pending_and_through_a_close_like_the_other)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    // Before the slot exists, and beside a consumer of the other form for the same stream.
    ls.set_consume_timed(0, [](uint8_t, std::span<float const>, avtp::Nanoseconds, avtp::Nanoseconds) {});
    ls.set_consume(0, [](uint8_t, std::span<float const>, uint64_t, uint64_t) {});
    ls.set_consume_timed(9, [](uint8_t, std::span<float const>, avtp::Nanoseconds, avtp::Nanoseconds) {});

    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_TRUE(static_cast<bool>(ls.slot_for(0)->consume_timed));
    EXPECT_TRUE(static_cast<bool>(ls.slot_for(0)->consume));
    EXPECT_EQ(ls.slot_for(9), nullptr);

    ls.close_rx();
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_TRUE(static_cast<bool>(ls.slot_for(0)->consume_timed));
    EXPECT_TRUE(static_cast<bool>(ls.slot_for(0)->consume));
}

// An AM824 frame through the listener, as a talker builds it: the path
// receive_am824_frame takes (header, declared audio, the decoder, both
// consumers, the slot's counters).
TEST(listener_consume, an_am824_frame_reaches_both_consumers_with_its_presentation_time)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 48'000, donor->components(), gptp, nullptr};
    StreamSpec spec{};
    spec.index = 0;
    spec.format_word = AM824_48K_8CH;
    spec.format = decode_stream_format(AM824_48K_8CH);
    EXPECT_TRUE(ls.open_stream(spec).has_value());
    EXPECT_TRUE(ls.slot_for(0)->am824.has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});

    // Six data blocks of eight channels, the first on a SYT boundary, so
    // the packet carries a timestamp: 2 ms after the time it is received at.
    constexpr avtp::Nanoseconds NOW{1'795'000'000'000'000'000};
    constexpr avtp::Nanoseconds AHEAD{2'000'000};
    avtp::Am824StreamOutputContext talker{tsn::StreamId{}, avtp::Am824SampleRate::rate_48_khz, 8, 0};
    avtp::Am824Pdu pdu{};
    pdu.init(tsn::StreamId{}, 8, avtp::Am824SampleRate::rate_48_khz);
    std::array<uint8_t, avtp::Am824Pdu::HEADER_LENGTH + (6 * 8 * 4)> frame{};
    auto const payload = std::span<uint8_t>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH);
    EXPECT_EQ(avtp::am824_serialize_mbla(talker, pdu, payload, 6, NOW + AHEAD, [](uint8_t, std::span<float>) {}), payload.size());
    EXPECT_TRUE(pdu.tv());
    span_store(std::span<uint8_t>{frame}.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));

    int timed_calls = 0;
    int unsigned_calls = 0;
    avtp::Nanoseconds timed_pts{};
    uint64_t unsigned_pts = 0;
    ls.set_consume_timed(0, [&](uint8_t, std::span<float const> s, avtp::Nanoseconds pts, avtp::Nanoseconds period) {
        ++timed_calls;
        timed_pts = pts;
        EXPECT_EQ(s.size(), 6U);
        EXPECT_TRUE(period == avtp::Nanoseconds{20833});
    });
    ls.set_consume(0, [&](uint8_t, std::span<float const>, uint64_t pts, uint64_t) {
        ++unsigned_calls;
        unsigned_pts = pts;
    });

    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{1});
    EXPECT_EQ(ls.slot_for(0)->rx_samples.load(), uint64_t{6});
    EXPECT_EQ(ls.slot_for(0)->rx_bad.load(), uint64_t{0});
    EXPECT_EQ(timed_calls, 8);  // once a channel
    EXPECT_EQ(unsigned_calls, 8);
    EXPECT_TRUE(timed_pts == NOW + AHEAD);
    EXPECT_EQ(unsigned_pts, static_cast<uint64_t>((NOW + AHEAD).count()));

    // A frame cut short of its header is not a packet, and one for this
    // stream whose header is not a valid AM824 header is counted as bad.
    ls.on_stream_rx_frame(std::span<uint8_t const>{frame}.first(12), NOW);
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{1});
    frame[1] &= 0x7F;  // the stream id is no longer marked valid
    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{1});
    EXPECT_EQ(ls.slot_for(0)->rx_bad.load(), uint64_t{1});
    EXPECT_EQ(timed_calls, 8);
}

// A CRF frame through the listener: each timestamp of the packet reaches
// the consumer with the time the frame was received at.
TEST(listener_crf, a_crf_frame_hands_each_timestamp_to_the_consumer_with_its_receive_time)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 48'000, donor->components(), gptp, nullptr};
    StreamSpec spec{};
    spec.index = 1;
    spec.format_word = CRF_MILAN_48K;
    spec.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(ls.open_stream(spec).has_value());
    ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});

    // Milan's CRF: one timestamp a packet, every 96 samples at 48 kHz.
    constexpr avtp::Nanoseconds NOW{1'795'000'000'000'000'000};
    constexpr avtp::Nanoseconds FIRST{NOW.count() + 2'000'000};
    avtp::CrfStreamOutputContext talker{tsn::StreamId{}, avtp::CrfType::audio_sample, 48000, avtp::CrfPull::multiply_1_0, 96, 1};
    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(tsn::StreamId{}, 48000, avtp::CrfPull::multiply_1_0, 96, 1);
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> frame{};
    auto const data = std::span<uint8_t>{frame}.subspan(avtp::CrfPdu::HEADER_LENGTH);
    EXPECT_EQ(talker.build_packet(pdu, data, FIRST), data.size());
    span_store(std::span<uint8_t>{frame}.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_2));

    int calls = 0;
    uint64_t told_ts = 0;
    int64_t told_rx = 0;
    ls.set_crf(1, [&](uint64_t ts, uint16_t index, int64_t rx) {
        ++calls;
        told_ts = ts;
        told_rx = rx;
        EXPECT_EQ(index, uint16_t{0});
    });

    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(told_ts, static_cast<uint64_t>(FIRST.count()));
    EXPECT_EQ(told_rx, NOW.count());
    EXPECT_EQ(ls.slot_for(1)->rx_packets.load(), uint64_t{1});
    EXPECT_EQ(ls.slot_for(1)->rx_samples.load(), uint64_t{1});

    // A frame for another stream is not this slot's.
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));
    ls.on_stream_rx_frame(frame, NOW);
    EXPECT_EQ(calls, 1);
}

namespace {

/// The Milan Base format at 96 kHz (AAF, 32 bit integer, 12 samples per
/// AVTPDU) with @p channels channels: AAF_96K_INT32_8CH with another count.
constexpr auto aaf_96k_of(uint64_t const channels) noexcept -> uint64_t
{
    return (AAF_96K_INT32_8CH & ~avtp::AAF_STREAM_FORMAT_CHANNELS_MASK) | (channels << 22);
}

constexpr avtp::Nanoseconds FORMAT_NOW{1'795'000'000'000'000'000};

/// One AAF AVTPDU of the format @p word for STREAM_1 with @p samples samples
/// of each channel, channel ch holding (ch + 1) / 16 throughout.
auto aaf_frame_of_word(uint64_t const word, uint16_t const samples = 12) -> std::vector<uint8_t>
{
    auto const format = decode_stream_format(word);
    avtp::AafStreamOutputContext talker{
        tsn::StreamId{},
        format.aaf_format,
        *avtp::aaf_sample_rate_from_hz(format.sample_rate_hz),
        format.channels,
        format.bit_depth,
        0};
    avtp::AafPdu pdu{};
    std::vector<uint8_t> frame(avtp::AafPdu::HEADER_LENGTH + (size_t{samples} * format.channels * 4), 0);
    auto const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    auto const fill = [](uint8_t ch, std::span<float> lane) { std::ranges::fill(lane, static_cast<float>(ch + 1) / 16.0F); };
    EXPECT_EQ(
        avtp::aaf_stream_serialize(talker, pdu, payload, samples, FORMAT_NOW + avtp::Nanoseconds{2'000'000}, fill), payload.size());
    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));
    return frame;
}

/// What the consumers of one input were handed: per channel, the calls and the last lane.
struct HeardChannels
{
    std::array<int, 8> calls{};
    std::array<std::vector<float>, 8> lanes{};

    auto consumer()
    {
        return [this](uint8_t ch, std::span<float const> samples, uint64_t, uint64_t) {
            ++calls.at(ch);
            lanes.at(ch).assign(samples.begin(), samples.end());
        };
    }
    [[nodiscard]] auto total() const -> int { return std::accumulate(calls.begin(), calls.end(), 0); }
};

/// A raw audio sink that keeps the last payload it was handed.
struct KeepingSink : StreamRxAudioSink
{
    int calls{0};
    std::vector<uint8_t> last{};
    void on_listener_audio(uint16_t, StreamAudioFormat, std::span<uint8_t const> payload) override
    {
        ++calls;
        last.assign(payload.begin(), payload.end());
    }
};

}  // namespace

TEST(listener_format, an_input_reports_the_format_it_was_opened_with_until_another_is_set)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_EQ(ls.stream_format(0), uint64_t{0});  // not open
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_EQ(ls.stream_format(0), AAF_96K_INT32_8CH);

    EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(2)));
    EXPECT_EQ(ls.stream_format(0), aaf_96k_of(2));
    EXPECT_TRUE(ls.set_stream_format(0, AAF_96K_INT32_8CH));
    EXPECT_EQ(ls.stream_format(0), AAF_96K_INT32_8CH);
}

TEST(listener_format, an_input_takes_every_base_channel_count_at_its_rate)
{
    // Milan v1.3 6.5: a Stream Input that supports one Base format at a rate
    // supports all of them, 1, 2, 4, 6 and 8 channels.
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    HeardChannels heard{};
    ls.set_consume(0, heard.consumer());

    int frames = 0;
    for (uint64_t const channels : {1U, 2U, 4U, 6U, 8U}) {
        EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(channels)));
        ls.on_stream_rx_frame(aaf_frame_of_word(aaf_96k_of(channels)), FORMAT_NOW);
        ++frames;
        // Every channel of the opened input is handed on once per AVTPDU:
        // what the stream carries decoded, the rest silent.
        EXPECT_EQ(heard.total(), frames * 8);
        for (size_t ch = 0; ch < 8; ++ch) {
            float const expected = ch < channels ? static_cast<float>(ch + 1) / 16.0F : 0.0F;
            EXPECT_EQ(heard.lanes.at(ch).size(), size_t{12});
            EXPECT_EQ(heard.lanes.at(ch).front(), expected);
            EXPECT_EQ(heard.lanes.at(ch).back(), expected);
        }
    }
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{5});
    EXPECT_EQ(ls.slot_for(0)->counters.unsupported_format.load(), 0U);
}

TEST(listener_format, an_avtpdu_that_is_not_of_the_inputs_format_is_discarded)
{
    // Milan v1.3 4.4.2.2: "A Listener PAAD shall discard the incoming AVTPDUs
    // that don't respect the format which is configured in the associated
    // STREAM_INPUT descriptor."
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    HeardChannels heard{};
    ls.set_consume(0, heard.consumer());
    auto const& slot = *ls.slot_for(0);

    // Opened for eight channels: two channels, and six samples, are refused.
    ls.on_stream_rx_frame(aaf_frame_of_word(aaf_96k_of(2)), FORMAT_NOW);
    ls.on_stream_rx_frame(aaf_frame_of_word(AAF_96K_INT32_8CH, 6), FORMAT_NOW);
    EXPECT_EQ(heard.total(), 0);
    EXPECT_EQ(slot.rx_packets.load(), uint64_t{0});
    EXPECT_EQ(slot.rx_bad.load(), uint64_t{2});
    EXPECT_EQ(slot.counters.frames_rx.load(), 2U);
    EXPECT_EQ(slot.counters.unsupported_format.load(), 2U);

    // Set to two channels: now the eight channel AVTPDU is the one refused.
    EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(2)));
    ls.on_stream_rx_frame(aaf_frame_of_word(AAF_96K_INT32_8CH), FORMAT_NOW);
    EXPECT_EQ(heard.total(), 0);
    EXPECT_EQ(slot.counters.unsupported_format.load(), 3U);
    ls.on_stream_rx_frame(aaf_frame_of_word(aaf_96k_of(2)), FORMAT_NOW);
    EXPECT_EQ(heard.total(), 8);
    EXPECT_EQ(slot.rx_packets.load(), uint64_t{1});
    EXPECT_EQ(slot.counters.unsupported_format.load(), 3U);
}

TEST(listener_format, an_input_refuses_a_format_it_cannot_be_brought_to)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    StreamSpec crf{};
    crf.index = 1;
    crf.format_word = CRF_MILAN_48K;
    crf.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(ls.open_stream(crf).has_value());

    uint64_t const at_48k = (aaf_96k_of(2) & ~0x000F000000000000ULL) | 0x0005000000000000ULL;
    uint64_t const of_24_bits = (aaf_96k_of(2) & ~0x000000FF00000000ULL) | 0x0000001800000000ULL;
    uint64_t const of_6_samples = (aaf_96k_of(2) & ~(0x3FFULL << 12)) | (6ULL << 12);
    for (uint64_t const refused :
         {at_48k,
          of_24_bits,
          of_6_samples,
          aaf_96k_of(0),
          aaf_96k_of(MAX_AAF_INPUT_CHANNELS + 1U),
          aaf_96k_of(2) | avtp::AAF_STREAM_FORMAT_UT_BIT,
          CRF_MILAN_48K,
          AM824_48K_8CH}) {
        EXPECT_FALSE(ls.can_take_stream_format(0, refused));
        EXPECT_FALSE(ls.set_stream_format(0, refused));
    }
    EXPECT_EQ(ls.stream_format(0), AAF_96K_INT32_8CH);
    EXPECT_TRUE(ls.can_take_stream_format(0, aaf_96k_of(MAX_AAF_INPUT_CHANNELS)));

    // An input of another kind takes the word it was opened with and no other.
    EXPECT_TRUE(ls.set_stream_format(1, CRF_MILAN_48K));
    EXPECT_FALSE(ls.set_stream_format(1, aaf_96k_of(2)));
    EXPECT_EQ(ls.stream_format(1), CRF_MILAN_48K);
    // An input that is not open takes nothing.
    EXPECT_FALSE(ls.set_stream_format(5, AAF_96K_INT32_8CH));
    EXPECT_FALSE(ls.set_stream_format(60000, AAF_96K_INT32_8CH));
}

TEST(listener_format, the_format_is_kept_by_stream_index_and_outlives_the_slots)
{
    // The entity's stop and start close and open the slots; an input set to
    // two channels receives two channels still.
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(2)));
    ls.close_rx();
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_EQ(ls.stream_format(0), aaf_96k_of(2));

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    ls.on_stream_rx_frame(aaf_frame_of_word(aaf_96k_of(2)), FORMAT_NOW);
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{1});
    EXPECT_EQ(ls.slot_for(0)->counters.unsupported_format.load(), 0U);
}

TEST(listener_format, the_raw_sink_is_handed_frames_of_the_opened_channel_count)
{
    // A sink reads the payload by the width the input was opened with. A
    // narrower stream reaches it widened, its missing channels zero.
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    KeepingSink sink{};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, &sink};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});

    auto const wide = aaf_frame_of_word(AAF_96K_INT32_8CH);
    ls.on_stream_rx_frame(wide, FORMAT_NOW);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_TRUE(std::ranges::equal(sink.last, std::span{wide}.subspan(avtp::AafPdu::HEADER_LENGTH)));

    EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(2)));
    auto const narrow = aaf_frame_of_word(aaf_96k_of(2));
    ls.on_stream_rx_frame(narrow, FORMAT_NOW);
    EXPECT_EQ(sink.calls, 2);
    EXPECT_EQ(sink.last.size(), size_t{12} * 8 * 4);
    auto const audio = std::span{narrow}.subspan(avtp::AafPdu::HEADER_LENGTH);
    for (size_t frame = 0; frame < 12; ++frame) {
        auto const handed = std::span{sink.last}.subspan(frame * 8 * 4, size_t{8} * 4);
        EXPECT_TRUE(std::ranges::equal(handed.first(8), audio.subspan(frame * 2 * 4, 8)));
        EXPECT_TRUE(std::ranges::all_of(handed.subspan(8), [](uint8_t octet) { return octet == 0; }));
    }
}

namespace {

/// @p count consecutive AAF AVTPDUs of the eight channel format for STREAM_1
/// from one talker: the sequence numbers run on, and the timestamps are a
/// packet (twelve samples at 96 kHz, 125 us) apart from @p first.
auto aaf_run(avtp::AafStreamOutputContext& talker, int const count, avtp::Nanoseconds const first)
    -> std::vector<std::vector<uint8_t>>
{
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < count; ++i) {
        avtp::AafPdu pdu{};
        std::vector<uint8_t> frame(avtp::AafPdu::HEADER_LENGTH + (size_t{12} * 8 * 4), 0);
        auto const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
        auto const at = first + avtp::Nanoseconds{int64_t{125'000} * i};
        EXPECT_EQ(avtp::aaf_stream_serialize(talker, pdu, payload, 12, at, [](uint8_t, std::span<float>) {}), payload.size());
        span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
        span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(STREAM_1));
        frames.push_back(std::move(frame));
    }
    return frames;
}

/// A talker of the eight channel format the test inputs are opened with.
auto aaf_talker() -> avtp::AafStreamOutputContext
{
    auto const format = decode_stream_format(AAF_96K_INT32_8CH);
    return avtp::AafStreamOutputContext{
        tsn::StreamId{},
        format.aaf_format,
        *avtp::aaf_sample_rate_from_hz(format.sample_rate_hz),
        format.channels,
        format.bit_depth,
        0};
}

}  // namespace

TEST(listener_lock, an_input_that_loses_its_stream_is_no_longer_locked)
{
    // Milan v1.3 5.3.8.10: MEDIA_LOCKED is MEDIA_UNLOCKED while "the input
    // stream is not synchronized on the media clock", and one more while it
    // is. An input with no stream is synchronized on nothing, and no AVTPDU
    // comes to show it, so the lock is dropped as the stream is.
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto const& counters = ls.slot_for(0)->counters;
    auto talker = aaf_talker();

    for (auto const& frame : aaf_run(talker, 14, FORMAT_NOW)) {
        ls.on_stream_rx_frame(frame, FORMAT_NOW);
    }
    EXPECT_EQ(counters.media_locked.load(), 1U);
    EXPECT_EQ(counters.media_unlocked.load(), 0U);

    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_EQ(counters.media_locked.load(), 1U);
    EXPECT_EQ(counters.media_unlocked.load(), 1U);
    EXPECT_FALSE(counters.is_locked.load());
    // A second disconnect, of an input that has none, counts nothing.
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_EQ(counters.media_unlocked.load(), 1U);

    // Connected again, the stream locks it again: one apart, as before.
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    for (auto const& frame : aaf_run(talker, 14, FORMAT_NOW + avtp::Nanoseconds{14 * 125'000})) {
        ls.on_stream_rx_frame(frame, FORMAT_NOW);
    }
    EXPECT_EQ(counters.media_locked.load(), 2U);
    EXPECT_EQ(counters.media_unlocked.load(), 1U);
}

TEST(listener_lock, an_avtpdu_taken_up_as_the_stream_is_lost_leaves_no_lock)
{
    // The receive thread may be handling an AVTPDU while the reactor thread
    // disconnects the input: the stream id is cleared, then the lock dropped.
    // That AVTPDU continues a good run and would lock the input again, with
    // no stream. The receive thread sees the id gone and drops the lock.
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto& slot = *ls.slot_for(0);
    auto talker = aaf_talker();
    auto const frames = aaf_run(talker, 15, FORMAT_NOW);
    for (size_t i = 0; i < 14; ++i) {
        ls.on_stream_rx_frame(frames[i], FORMAT_NOW);
    }
    EXPECT_TRUE(slot.counters.is_locked.load());

    // What on_listener_disconnected does to the lock, the last AVTPDU read before it and counted after.
    slot.rx_stream_id.publish(0);
    unlock_stream_input(slot.counters);
    ls.receive_aaf_frame(slot, frames[14], FORMAT_NOW);

    EXPECT_FALSE(slot.counters.is_locked.load());
    EXPECT_EQ(slot.counters.media_locked.load(), slot.counters.media_unlocked.load());
}

TEST(listener_lock, the_stream_after_a_disconnect_follows_nothing_of_the_last)
{
    // A disconnect ends the stream; the next one, from a talker that numbers
    // its AVTPDUs from zero, is no SEQ_NUM_MISMATCH of the first, and locks
    // the input on a run of its own.
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto const& counters = ls.slot_for(0)->counters;
    auto first = aaf_talker();
    for (auto const& frame : aaf_run(first, 14, FORMAT_NOW)) {
        ls.on_stream_rx_frame(frame, FORMAT_NOW);
    }
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto second = aaf_talker();
    for (auto const& frame : aaf_run(second, 14, FORMAT_NOW + avtp::Nanoseconds{40'000'000})) {
        ls.on_stream_rx_frame(frame, FORMAT_NOW);
    }
    EXPECT_EQ(counters.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(counters.media_locked.load(), 2U);
    EXPECT_EQ(counters.media_unlocked.load(), 1U);
    EXPECT_EQ(counters.frames_rx.load(), 28U);
}

TEST(listener_lock, an_avtpdu_in_flight_at_a_disconnect_is_not_compared_with_the_next_stream)
{
    // The receive thread counts an AVTPDU of the old stream after the reactor
    // thread disconnected the input: the next stream, numbered from zero,
    // still starts afresh, with no SEQ_NUM_MISMATCH against that AVTPDU.
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto& slot = *ls.slot_for(0);
    auto first = aaf_talker();
    auto const frames = aaf_run(first, 15, FORMAT_NOW);
    for (size_t i = 0; i < 14; ++i) {
        ls.on_stream_rx_frame(frames[i], FORMAT_NOW);
    }
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    ls.receive_aaf_frame(slot, frames[14], FORMAT_NOW);

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto second = aaf_talker();
    for (auto const& frame : aaf_run(second, 14, FORMAT_NOW + avtp::Nanoseconds{40'000'000})) {
        ls.on_stream_rx_frame(frame, FORMAT_NOW);
    }
    EXPECT_EQ(slot.counters.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(slot.counters.media_reset.load(), 0U);
    EXPECT_EQ(slot.counters.frames_rx.load(), 29U);
}

TEST(listener_counters, an_input_whose_talker_goes_is_interrupted_and_a_released_one_is_not)
{
    // Milan v1.3 Table 5.6: STREAM_INTERRUPTED for any reason other than a
    // controller's unbind, a Talker attribute withdrawn among them.
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    auto const& counters = ls.slot_for(0)->counters;
    auto talker = aaf_talker();
    avtp::Nanoseconds start = FORMAT_NOW;
    for (auto const why : {StreamInputLoss::released, StreamInputLoss::talker_lost}) {
        ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
        for (auto const& frame : aaf_run(talker, 14, start)) {
            ls.on_stream_rx_frame(frame, FORMAT_NOW);
        }
        EXPECT_TRUE(counters.is_locked.load());
        ls.on_listener_disconnected(0, sm::TimePoint{}, why);
        start += avtp::Nanoseconds{14 * 125'000};
    }
    EXPECT_EQ(counters.media_locked.load(), 2U);
    EXPECT_EQ(counters.media_unlocked.load(), 2U);
    EXPECT_EQ(counters.stream_interrupted.load(), 1U);
}

TEST(listener_counters, get_counters_reports_an_interrupted_stream)
{
    // An AVTPDU lost from a locked stream breaks the timestamp step: the lock
    // goes and STREAM_INTERRUPTED (bit 2, Milan v1.3 Table 5.13) counts it.
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{5'000, 96'000, donor->components(), gptp, nullptr};  // a 5 us step tolerance
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto talker = aaf_talker();
    auto const frames = aaf_run(talker, 40, FORMAT_NOW);
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i != 20) {
            ls.on_stream_rx_frame(frames[i], FORMAT_NOW);
        }
    }

    uint32_t valid = 0;
    std::array<uint32_t, 32> out{};
    EXPECT_TRUE(ls.fill_stream_input_counters(0, valid, out));
    EXPECT_EQ(valid & 0x0000'0FFFU, 0x0000'0FFFU);
    EXPECT_EQ(out[0], 2U);  // MEDIA_LOCKED: before the loss and again after
    EXPECT_EQ(out[1], 1U);  // MEDIA_UNLOCKED
    EXPECT_EQ(out[2], 1U);  // STREAM_INTERRUPTED
    EXPECT_EQ(out[3], 1U);  // SEQ_NUM_MISMATCH
    EXPECT_EQ(out[11], 39U);
}

TEST(listener_counters, get_counters_reports_each_running_counter_at_its_own_bit)
{
    // IEEE 1722.1-2021 Clause 7.4.42: twelve STREAM_INPUT counters, bits 0 to 11 of counters_valid.
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    auto& c = ls.slot_for(0)->counters;
    uint32_t value = 100;
    for (auto* counter :
         {&c.media_locked,
          &c.media_unlocked,
          &c.stream_interrupted,
          &c.seq_num_mismatch,
          &c.media_reset,
          &c.timestamp_uncertain,
          &c.timestamp_valid,
          &c.timestamp_not_valid,
          &c.unsupported_format,
          &c.late_timestamp,
          &c.early_timestamp,
          &c.frames_rx}) {
        counter->store(value++);
    }

    uint32_t valid = 0;
    std::array<uint32_t, 32> out{};
    EXPECT_TRUE(ls.fill_stream_input_counters(0, valid, out));
    EXPECT_EQ(valid, 0x0000'0FFFU);
    for (uint32_t bit = 0; bit < 12; ++bit) {
        EXPECT_EQ(out[bit], 100U + bit);
    }
    EXPECT_EQ(out[12], 0U);
}

// --- A listener stream received beside the stream port ---

namespace {

/// Each (input, state) the input hook is told.
struct InputRecorder
{
    std::vector<std::pair<uint16_t, ListenerStreams::InputState>> told{};
    bool agreed{true};  ///< input_state() in the hook said the stream told

    void install(ListenerStreams& ls)
    {
        ls.set_on_input_changed([this, &ls](uint16_t const index, ListenerStreams::InputState const& state) {
            told.emplace_back(index, state);
            auto const now = ls.input_state(index).value_or(ListenerStreams::InputState{.stream_id = 1});
            agreed = agreed && now.stream_id == state.stream_id;
        });
    }
    [[nodiscard]] auto last_is(uint16_t const index, uint64_t const stream_id, bool const stopped, uint64_t const format) const
        -> bool
    {
        if (told.empty()) {
            return false;
        }
        auto const& [i, state] = told.back();
        return i == index && state.stream_id == stream_id && state.stopped == stopped && state.format == format;
    }
};

}  // namespace

// What a data plane beside the port needs to receive an input: the stream it receives, whether it is
// stopped (Milan v1.3 5.3.8.7) and the format in force (5.3.8.1), at open, at each change and at close.
TEST(listener_offload, the_hook_is_told_what_each_input_receives)
{
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    InputRecorder recorder;
    recorder.install(ls);
    ls.set_stream_stopped(1, true);
    EXPECT_TRUE(recorder.told.empty());  // no slot: no notice
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(1)).has_value());
    EXPECT_TRUE(recorder.last_is(1, 0, true, AAF_96K_INT32_8CH));  // the state in force at open
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_TRUE(recorder.last_is(0, 0, false, AAF_96K_INT32_8CH));

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_TRUE(recorder.last_is(0, STREAM_1.to_uint64(), false, AAF_96K_INT32_8CH));
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});  // the same stream: no change
    EXPECT_EQ(recorder.told.size(), size_t{3});
    ls.set_stream_stopped(0, true);
    EXPECT_TRUE(recorder.last_is(0, STREAM_1.to_uint64(), true, AAF_96K_INT32_8CH));
    ls.set_stream_stopped(0, true);
    EXPECT_EQ(recorder.told.size(), size_t{4});
    EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(2)));
    EXPECT_TRUE(recorder.last_is(0, STREAM_1.to_uint64(), true, aaf_96k_of(2)));
    EXPECT_TRUE(ls.set_stream_format(0, aaf_96k_of(2)));
    EXPECT_FALSE(ls.set_stream_format(0, CRF_MILAN_48K));  // refused: nothing changes
    EXPECT_EQ(recorder.told.size(), size_t{5});
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_TRUE(recorder.last_is(0, 0, true, aaf_96k_of(2)));
    EXPECT_FALSE(ls.input_state(2).has_value());
    auto const state = ls.input_state(0);
    EXPECT_TRUE(state.has_value());
    EXPECT_EQ(state.value_or(ListenerStreams::InputState{}).format, aaf_96k_of(2));

    ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});
    recorder.told.clear();
    ls.close_rx();  // each input it releases, with stream ID 0
    EXPECT_EQ(recorder.told.size(), size_t{2});
    EXPECT_TRUE(recorder.last_is(0, 0, true, aaf_96k_of(2)));
    EXPECT_EQ(recorder.told.front().first, uint16_t{1});
    EXPECT_EQ(recorder.told.front().second.stream_id, uint64_t{0});
    EXPECT_TRUE(recorder.agreed);
    ls.set_stream_stopped(0, false);  // no slot any more
    EXPECT_EQ(recorder.told.size(), size_t{2});
    ls.set_on_input_changed({});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_streams_test)
