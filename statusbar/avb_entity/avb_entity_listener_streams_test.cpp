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
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/avtp/avtp_crf_stream_output.hpp"
#include "statusbar/avtp/avtp_stream_common.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
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
    ls.on_listener_disconnected(0, sm::TimePoint{});
    EXPECT_FALSE(ls.slot_for(0)->connection.settled);

    ls.on_listener_connected(0, STREAM_2, GROUP, sm::TimePoint{});
    EXPECT_FALSE(ls.slot_for(0)->connection.settled);
    EXPECT_FALSE(followed(STREAM_2).has_value());
    ls.on_listener_disconnected(0, sm::TimePoint{});
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

    ls.on_listener_disconnected(0, sm::TimePoint{});
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

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_streams_test)
