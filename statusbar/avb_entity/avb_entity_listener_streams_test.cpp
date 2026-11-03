// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ListenerStreams RX-slot tests (kit phases 1+2): spec-shaped slot creation,
// the per-stream consume registration mechanics (pending-before-open, direct
// rebind after open), and the menu/selection principle — a registration for a
// stream index the model never declares stays pending and inert, never an
// error. A CRF slot hands each timestamp, its PDU's mr bit and its source generation to its consumer.
//
// Positive frame->consume delivery requires a connected ACMP listener + live
// frames and is exercised on hardware; the decode paths themselves are
// covered by the avtp deserializer unit tests.

#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_crf_clock_recovery.hpp"
#include "statusbar/avb_entity/avb_entity_stream_counters.hpp"
#include "statusbar/avb_entity/avb_entity_stream_rx_sink.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/buffer/buffer.hpp"
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

// A slot already on a stream is connected to it again with another Talker
// attribute, as a sink that settled again on another destination address
// would be. The stream's one declaration follows the new attribute and the
// slot is still attached once: one disconnect withdraws it.
TEST(listener_connection, a_slot_connected_again_to_its_stream_follows_the_talker_attribute_of_the_last_connection)
{
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    auto const record = [&donor](ieee::Eui64 const& stream) {
        nanoavb::StreamId sid{};
        sid.from_uint64(stream.to_uint64());
        return *donor->components().msrp_handler.get_listener_stream(sid);
    };
    constexpr ieee::Eui48 OTHER_GROUP{0x91, 0xE0, 0xF0, 0x00, 0x56, 0x78};

    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{}, nanoavb::TalkerMatch{.dest_address = GROUP, .vlan_id = 2});
    EXPECT_TRUE(record(STREAM_1).follows.has_value() && record(STREAM_1).follows->dest_address == GROUP);

    ls.on_listener_connected(
        0, STREAM_1, OTHER_GROUP, sm::TimePoint{}, nanoavb::TalkerMatch{.dest_address = OTHER_GROUP, .vlan_id = 3});
    auto const moved = record(STREAM_1);
    EXPECT_TRUE(moved.follows.has_value() && moved.follows->dest_address == OTHER_GROUP && moved.follows->vlan_id == 3);
    EXPECT_EQ(moved.attached, 1);
    EXPECT_TRUE(ls.slot_for(0)->connection.settled);

    // A CONNECT repeated with no attribute named follows any, and attaches nothing more.
    ls.on_listener_connected(0, STREAM_1, OTHER_GROUP, sm::TimePoint{});
    EXPECT_FALSE(record(STREAM_1).follows.has_value());
    EXPECT_EQ(record(STREAM_1).attached, 1);

    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_EQ(record(STREAM_1).attached, 0);
    EXPECT_EQ(record(STREAM_1).state, nanoavb::ListenerReservationState::Idle);
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
    ls.set_crf(1, [](uint64_t ts, uint16_t /*idx*/, int64_t /*rx*/, bool /*mr*/) {
        last_ts = ts;
        ++calls;
    });
    ls.set_crf(9, [](uint64_t, uint16_t, int64_t, bool) { ++calls; });

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
    slot->on_crf(123456789ULL, 0, 1000, false, 1);
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

// --- The timed raw RX seam (StreamRxTimedAudioSink) ---

namespace {

using avtp::AafFormat;
using avtp::AafSampleRate;

/// One AAF frame from `tx`, its payload `frames` frames of a byte pattern plus
/// `extra` trailing bytes, presented at `pts`.
auto aaf_frame(
    avtp::AafStreamOutputContext& tx, uint16_t const frames, uint64_t const pts, size_t const extra = 0, bool const tv = true)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame(512);
    StreamRawRenderFn const render = [](std::span<uint8_t> payload, uint32_t, uint64_t first_index, uint64_t) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>(first_index + i);
        }
        return true;
    };
    size_t const len = serialize_aaf_raw(tx, frame, frames, pts / 1000, pts, render);
    EXPECT_NE(len, size_t{0});
    if (!tv) {
        auto pdu = avtp::aaf_parse_header(std::span<uint8_t const>{frame.data(), len});
        pdu->set_tv(false);
        span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), *pdu);
    }
    frame.resize(len + extra);
    return frame;
}

/// Records what a timed sink is handed (payload copied: it is not valid after the call).
struct RecordingSink final : StreamRxTimedAudioSink
{
    struct Seen
    {
        StreamRxPacket packet;
        std::vector<uint8_t> bytes;
    };
    std::vector<Seen> seen;
    void on_listener_packet(StreamRxPacket const& packet) override
    {
        seen.push_back(Seen{.packet = packet, .bytes = {packet.payload.begin(), packet.payload.end()}});
    }
};

auto audio_blob() -> std::vector<uint8_t>
{
    return load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_audio.bin");
}

/// The kit's ACMP hooks for STREAM_INPUTs 0 and 1 as @p donor's listener now stands: the RX thread
/// matches frames by the connection on_listener_connected publishes (on_listener_disconnected clears).
void publish_connections(ListenerStreams& ls, AvbEntityAudioIO& donor)
{
    for (uint16_t sink = 0; sink < 2; ++sink) {
        auto const* stream = donor.components().acmp_listener.get_stream(sink);
        if (stream != nullptr && stream->connected) {
            ls.on_listener_connected(sink, stream->stream_id, stream->stream_dest_mac, sm::TimePoint{});
        } else {
            ls.on_listener_disconnected(sink, sm::TimePoint{});
        }
    }
}

/// An audio entity (it has listener streams) whose ACMP listener reports
/// STREAM_INPUTs 0 (AM824) and 1 (AAF) connected to the given stream ids, by
/// the real CONNECT_RX / CONNECT_TX handshake.
auto make_connected_donor(tsn::StreamId const am824_id, tsn::StreamId const aaf_id) -> std::unique_ptr<AvbEntityAudioIO>
{
    AvbEntityAudioIOConfig config{};
    config.entity_id = ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.descriptor_storage_blob = audio_blob();
    auto const listener_id = config.entity_id;
    auto created = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(created.has_value());
    auto& listener = (*created)->components().acmp_listener;
    nanoavb::AcmpListenerCallbacks cb;
    cb.tx_command = [](atdecc::AcmpCommandResponse const&) { return true; };
    cb.tx_response = [](atdecc::AcmpCommandResponse const&) { return true; };
    listener.set_callbacks(cb);
    listener.start();
    ieee::Eui64 const talker_id{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    uint16_t seq = 1;
    for (auto const& [sink, id] : {std::pair{uint16_t{0}, am824_id}, std::pair{uint16_t{1}, aaf_id}}) {
        atdecc::AcmpCommandResponse cmd{};
        cmd.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
        cmd.talker_entity_id = talker_id;
        cmd.listener_entity_id = listener_id;
        cmd.listener_unique_id = sink;
        cmd.sequence_id = seq++;
        EXPECT_TRUE(listener.receive_controller_command(cmd, sm::TimePoint{}));
        atdecc::AcmpCommandResponse resp = cmd;
        resp.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
        resp.set_status(atdecc::ACMP_STATUS_SUCCESS);
        auto const mac = id.get_system_address().value;
        resp.stream_id = ieee::Eui64{
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5],
            static_cast<uint8_t>(id.get_unique_id() >> 8U),
            static_cast<uint8_t>(id.get_unique_id())};
        resp.stream_dest_mac = ieee::Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, static_cast<uint8_t>(sink)};
        EXPECT_TRUE(listener.receive_talker_response(resp, sm::TimePoint{}));
        EXPECT_TRUE(listener.is_connected(sink));
    }
    return std::move(*created);
}

auto audio_specs() -> StreamSpecs
{
    auto const blob = audio_blob();
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto specs = listener_stream_specs(*storage, 0);
    EXPECT_TRUE(specs.has_value());
    return *specs;
}

tsn::StreamId const kAm824Id{ieee::Eui48{0x00, 0x1B, 0x21, 0x01, 0x02, 0x03}, 0};
tsn::StreamId const kAafId{ieee::Eui48{0x00, 0x1B, 0x21, 0x01, 0x02, 0x03}, 1};

}  // namespace

// aaf_rx_packet() reads a packet's timing from its header alone and agrees with
// the float deserializer on the reconstructed 64-bit presentation time, packet
// after packet, across a 2^32 ns wrap of the wire timestamp.
TEST(listener_timed, aaf_rx_packet_matches_the_deserializer)
{
    avtp::AafStreamOutputContext tx{avtp::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 0};
    avtp::AafStreamInputContext raw{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    avtp::AafStreamInputContext decoded{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    uint64_t const base_pts = (uint64_t{7} << 32U) - 250'000;
    for (uint64_t k = 0; k < 4; ++k) {
        uint64_t const pts = base_pts + (k * 125'000);
        auto const frame = aaf_frame(tx, 6, pts);
        auto const pdu = avtp::aaf_parse_header(frame);
        EXPECT_TRUE(pdu.has_value());
        auto const audio = avtp::aaf_get_audio_payload(frame);
        uint64_t const rx_now = pts - 1'500'000;
        auto const packet = aaf_rx_packet(3, raw, *pdu, audio, rx_now);
        uint64_t decoded_pts = 0;
        avtp::aaf_stream_deserialize(
            decoded, *pdu, audio, rx_now, [&](uint8_t, std::span<float>, uint64_t p, uint64_t) { decoded_pts = p; });
        EXPECT_EQ(packet.stream_index, uint16_t{3});
        EXPECT_TRUE(packet.pts_valid);
        EXPECT_FALSE(packet.tu);
        EXPECT_EQ(packet.pts_ns, pts);
        EXPECT_EQ(packet.pts_ns, decoded_pts);
        EXPECT_EQ(packet.frames, uint32_t{6});
        EXPECT_EQ(packet.channels, uint16_t{2});
        EXPECT_TRUE(packet.aaf_format == AafFormat::int_24bit);
        EXPECT_EQ(packet.bytes_per_sample, uint8_t{3});
        EXPECT_EQ(packet.payload.size(), size_t{36});
        EXPECT_EQ(packet.payload.data(), audio.data());
        EXPECT_EQ(raw.packets_received, decoded.packets_received);
        EXPECT_EQ(raw.samples_since_ts, decoded.samples_since_ts);
    }
}

// tv=0 packets: the presentation time is extrapolated from the last valid one
// by samples x period, and pts_valid carries over; a stream that has never
// carried a timestamp reports pts_valid false.
TEST(listener_timed, tv_clear_packets_extrapolate)
{
    avtp::AafStreamOutputContext tx{avtp::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 0};
    avtp::AafStreamInputContext rx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    uint64_t const pts = 5'000'000'000ULL;

    auto const untimed = aaf_frame(tx, 6, pts - 125'000, 0, /*tv=*/false);
    auto const first =
        aaf_rx_packet(1, rx, *avtp::aaf_parse_header(untimed), avtp::aaf_get_audio_payload(untimed), pts - 1'000'000);
    EXPECT_FALSE(first.pts_valid);
    EXPECT_EQ(first.frames, uint32_t{6});

    auto const timed = aaf_frame(tx, 6, pts);
    auto const anchor = aaf_rx_packet(1, rx, *avtp::aaf_parse_header(timed), avtp::aaf_get_audio_payload(timed), pts - 1'000'000);
    EXPECT_TRUE(anchor.pts_valid);
    EXPECT_EQ(anchor.pts_ns, pts);
    for (uint64_t k = 1; k <= 2; ++k) {
        auto const next = aaf_frame(tx, 6, pts + (k * 125'000), 0, /*tv=*/false);
        auto const packet = aaf_rx_packet(1, rx, *avtp::aaf_parse_header(next), avtp::aaf_get_audio_payload(next), pts - 1'000'000);
        EXPECT_TRUE(packet.pts_valid);
        EXPECT_EQ(packet.pts_ns, pts + (k * 6 * rx.sample_period_ns));
    }
}

// The acceptance rule is the deserializer's (avtp::aaf_samples_in_packet): no
// whole frame, or more than 64 channels, is refused with the context
// untouched; a trailing partial frame is trimmed from the payload.
TEST(listener_timed, refusals_and_trailing_bytes)
{
    avtp::AafStreamOutputContext tx{avtp::StreamId{}, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24, 0};
    avtp::AafStreamInputContext rx{AafFormat::int_24bit, AafSampleRate::rate_48_khz, 2, 24};
    auto const frame = aaf_frame(tx, 6, 1'000'000'000ULL, /*extra=*/4);
    auto const pdu = *avtp::aaf_parse_header(frame);
    auto const audio = avtp::aaf_get_audio_payload(frame);

    // less than one frame: refused, nothing advanced
    auto const none = aaf_rx_packet(0, rx, pdu, audio.first(5), 0);
    EXPECT_EQ(none.frames, uint32_t{0});
    EXPECT_EQ(rx.packets_received, uint32_t{0});
    EXPECT_EQ(rx.samples_since_ts, uint32_t{0});
    EXPECT_FALSE(rx.has_valid_timestamp);
    EXPECT_EQ((avtp::aaf_samples_in_packet<64, 256>(rx, 5)), size_t{0});

    // the payload as received carries 4 bytes past the last whole frame
    // (aaf_get_audio_payload follows stream_data_length, so pass the frame's tail)
    std::span<uint8_t const> const padded{frame.data() + avtp::AafPdu::HEADER_LENGTH, frame.size() - avtp::AafPdu::HEADER_LENGTH};
    EXPECT_EQ(padded.size(), size_t{40});
    auto const trimmed = aaf_rx_packet(0, rx, pdu, padded, 999'000'000ULL);
    EXPECT_EQ(trimmed.frames, uint32_t{6});
    EXPECT_EQ(trimmed.payload.size(), size_t{36});

    // more than 64 channels: the deserializer refuses it, so does the raw path
    avtp::AafStreamInputContext wide{AafFormat::int_16bit, AafSampleRate::rate_48_khz, 65, 16};
    std::vector<uint8_t> const big(size_t{65} * 2 * 2);
    EXPECT_EQ((avtp::aaf_samples_in_packet<64, 256>(wide, big.size())), size_t{0});
    auto const refused = aaf_rx_packet(0, wide, pdu, big, 0);
    EXPECT_EQ(refused.frames, uint32_t{0});
    EXPECT_EQ(wide.packets_received, uint32_t{0});
    bool called = false;
    avtp::aaf_stream_deserialize(wide, pdu, big, 0, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(wide.packets_received, uint32_t{0});
}

// The timed sink gets each AAF packet through on_stream_rx_frame, with or without a consume fn
// (same packet, same counters); detaching stops delivery.
TEST(listener_timed, delivery_through_on_stream_rx_frame)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& aaf_spec = specs[1];
    EXPECT_TRUE(aaf_spec.format.kind == StreamKind::aaf);
    std::atomic<int64_t> gptp{0};
    RecordingSink raw_sink;
    RecordingSink decoded_sink;
    ListenerStreams raw{1'000'000, aaf_spec.format.sample_rate_hz, donor->components(), gptp, nullptr};
    ListenerStreams decoded{1'000'000, aaf_spec.format.sample_rate_hz, donor->components(), gptp, nullptr};
    EXPECT_TRUE(raw.open_stream(aaf_spec).has_value());
    publish_connections(raw, *donor);
    EXPECT_TRUE(decoded.open_stream(aaf_spec).has_value());
    publish_connections(decoded, *donor);
    raw.set_timed_audio_sink(&raw_sink);
    decoded.set_timed_audio_sink(&decoded_sink);
    int decoded_channels = 0;
    decoded.set_consume(1, [&](uint8_t, std::span<float const>, uint64_t, uint64_t) { ++decoded_channels; });

    auto const rate = avtp::aaf_sample_rate_from_hz(aaf_spec.format.sample_rate_hz);
    EXPECT_TRUE(rate.has_value());
    uint16_t const channels = aaf_spec.format.channels;
    uint16_t const spp = static_cast<uint16_t>(class_a_samples_per_packet(aaf_spec.format.sample_rate_hz));
    avtp::AafStreamOutputContext tx{kAafId, aaf_spec.format.aaf_format, *rate, channels, aaf_spec.format.bit_depth, 0};
    uint64_t const pts0 = 9'000'000'000ULL;
    std::vector<std::vector<uint8_t>> frames;
    for (uint64_t k = 0; k < 4; ++k) {
        frames.push_back(aaf_frame(tx, spp, pts0 + (k * 125'000), /*extra=*/k == 2 ? 3 : 0));
    }
    for (auto const& f : frames) {
        raw.on_stream_rx_frame(f, static_cast<int64_t>(pts0 - 1'000'000));
        decoded.on_stream_rx_frame(f, static_cast<int64_t>(pts0 - 1'000'000));
    }
    EXPECT_EQ(raw_sink.seen.size(), size_t{4});
    EXPECT_EQ(decoded_sink.seen.size(), size_t{4});
    EXPECT_EQ(decoded_channels, 4 * channels);  // the decode ran only where a consumer asked
    size_t const bps = avtp::aaf_bytes_per_sample(aaf_spec.format.aaf_format);
    for (size_t i = 0; i < 4; ++i) {
        auto const& a = raw_sink.seen[i];
        auto const& b = decoded_sink.seen[i];
        EXPECT_EQ(a.packet.stream_index, uint16_t{1});
        EXPECT_EQ(a.packet.pts_ns, pts0 + (i * 125'000));
        EXPECT_EQ(a.packet.pts_ns, b.packet.pts_ns);
        EXPECT_EQ(a.packet.frames, uint32_t{spp});
        EXPECT_EQ(a.packet.frames, b.packet.frames);
        EXPECT_EQ(a.bytes.size(), size_t{spp} * channels * bps);
        EXPECT_TRUE(a.bytes == b.bytes);
        EXPECT_EQ(a.packet.pts_valid, b.packet.pts_valid);
        EXPECT_TRUE(a.packet.aaf_format == b.packet.aaf_format);
    }
    auto const* rs = raw.slot_for(1);
    auto const* ds = decoded.slot_for(1);
    EXPECT_EQ(rs->rx_packets.load(), ds->rx_packets.load());
    EXPECT_EQ(rs->rx_samples.load(), ds->rx_samples.load());
    EXPECT_EQ(rs->aaf->packets_received, ds->aaf->packets_received);
    EXPECT_EQ(rs->aaf->samples_since_ts, ds->aaf->samples_since_ts);

    // an empty payload: no sink call on either path, counters still equal
    auto empty = aaf_frame(tx, spp, pts0 + 500'000);
    empty.resize(avtp::AafPdu::HEADER_LENGTH);
    raw.on_stream_rx_frame(empty, static_cast<int64_t>(pts0));
    decoded.on_stream_rx_frame(empty, static_cast<int64_t>(pts0));
    EXPECT_EQ(raw_sink.seen.size(), size_t{4});
    EXPECT_EQ(decoded_sink.seen.size(), size_t{4});
    EXPECT_EQ(rs->rx_packets.load(), ds->rx_packets.load());
    EXPECT_EQ(rs->aaf->packets_received, ds->aaf->packets_received);

    // a frame for another stream id is not ours
    avtp::AafStreamOutputContext other{kAm824Id, aaf_spec.format.aaf_format, *rate, channels, aaf_spec.format.bit_depth, 0};
    raw.on_stream_rx_frame(aaf_frame(other, spp, pts0), static_cast<int64_t>(pts0));
    EXPECT_EQ(raw_sink.seen.size(), size_t{4});

    // detached: nothing more delivered
    raw.set_timed_audio_sink(nullptr);
    raw.on_stream_rx_frame(aaf_frame(tx, spp, pts0 + 625'000), static_cast<int64_t>(pts0));
    EXPECT_EQ(raw_sink.seen.size(), size_t{4});
}

// The AM824 timed path: pts_valid follows the deserializer's anchor (false
// until a SYT-bearing packet), and the payload is exactly frames x channels
// quadlets even when the frame carries padding.
TEST(listener_timed, am824_timed_path)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& spec = specs[0];
    EXPECT_TRUE(spec.format.kind == StreamKind::am824);
    std::atomic<int64_t> gptp{0};
    RecordingSink sink;
    ListenerStreams ls{1'000'000, spec.format.sample_rate_hz, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(spec).has_value());
    publish_connections(ls, *donor);
    ls.set_timed_audio_sink(&sink);

    auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
    EXPECT_TRUE(rate.has_value());
    auto const channels = static_cast<uint8_t>(spec.format.channels);
    auto const spp = static_cast<uint8_t>(class_a_samples_per_packet(spec.format.sample_rate_hz));
    avtp::Am824StreamOutputContext tx{kAm824Id, *rate, channels, 0};
    uint64_t const t0 = 7'000'000'000ULL;
    bool saw_invalid_before_anchor = false;
    for (uint64_t k = 0; k < 8; ++k) {
        std::array<uint8_t, 1024> frame{};
        avtp::Am824Pdu pdu{};
        pdu.init(kAm824Id, channels, *rate);
        auto const payload = std::span<uint8_t>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH);
        size_t const bytes = avtp::am824_serialize_mbla(
            tx, pdu, payload, spp, t0 + (k * 125'000), [](uint8_t, std::span<float> d) { std::fill(d.begin(), d.end(), 0.25F); });
        EXPECT_NE(bytes, size_t{0});
        span_store(std::span<uint8_t>{frame}.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
        size_t const len = avtp::Am824Pdu::HEADER_LENGTH + bytes + 4;  // 4 bytes of padding
        size_t const before = sink.seen.size();
        ls.on_stream_rx_frame(std::span<uint8_t const>{frame.data(), len}, static_cast<int64_t>(t0 - 1'000'000));
        EXPECT_EQ(sink.seen.size(), before + 1);
        auto const& seen = sink.seen.back();
        EXPECT_TRUE(seen.packet.fmt == StreamAudioFormat::am824_mbla);
        EXPECT_EQ(seen.packet.bytes_per_sample, uint8_t{4});
        EXPECT_EQ(seen.packet.frames, uint32_t{spp});
        EXPECT_EQ(seen.bytes.size(), size_t{spp} * channels * 4);
        EXPECT_EQ(seen.packet.pts_valid, ls.slot_for(0)->am824->has_valid_anchor);
        saw_invalid_before_anchor = saw_invalid_before_anchor || (!seen.packet.pts_valid && k == 0);
    }
    EXPECT_TRUE(sink.seen.back().packet.pts_valid);  // anchored within 8 packets
    (void)saw_invalid_before_anchor;
}

namespace {

/// What a CRF talker sends: the clock a CrfInputUnderTest is fed.
struct CrfClock
{
    avtp::CrfType type{avtp::CrfType::audio_sample};
    uint32_t base_frequency{48000};
    avtp::CrfPull pull{avtp::CrfPull::multiply_1_0};
    uint16_t timestamp_interval{96};
    uint16_t timestamps_per_pdu{1};
};

/// A listener with one CRF input, STREAM_INPUT 1 in the Milan Clock Reference
/// Format (48 kHz, one timestamp every 96 samples), connected to STREAM_2,
/// and a CRF talker whose AVTPDUs it is handed.
struct CrfInputUnderTest
{
    static constexpr avtp::Nanoseconds START{1'795'000'000'000'000'000};
    static constexpr avtp::Nanoseconds PDU_PERIOD{2'000'000};

    std::unique_ptr<AvbEntityToneGenerator> donor{make_donor()};
    std::atomic<int64_t> gptp{0};
    ListenerStreams ls;
    avtp::CrfStreamOutputContext talker;
    avtp::Nanoseconds now{START};
    int fed{0};                    ///< timestamps the CRF consumer was handed
    bool tu{false};                ///< the tu bit of the AVTPDUs from here on
    bool mr{false};                ///< their mr bit
    size_t cut{0};                 ///< octets cut off the end of each frame
    ieee::Eui64 stream{STREAM_2};  ///< the stream_id the AVTPDUs carry

    explicit CrfInputUnderTest(CrfClock const clock = {}, uint32_t const entity_sample_rate = 48'000)
        : ls{1'000'000, entity_sample_rate, donor->components(), gptp, nullptr}
        , talker{tsn::StreamId{}, clock.type, clock.base_frequency, clock.pull, clock.timestamp_interval, clock.timestamps_per_pdu}
    {
        StreamSpec spec{};
        spec.index = 1;
        spec.format_word = CRF_MILAN_48K;
        spec.format = decode_stream_format(CRF_MILAN_48K);
        EXPECT_TRUE(ls.open_stream(spec).has_value());
        ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});
        ls.set_crf(1, [this](uint64_t, uint16_t, int64_t, bool) { ++fed; });
    }
    CrfInputUnderTest(CrfInputUnderTest const&) = delete;
    auto operator=(CrfInputUnderTest const&) -> CrfInputUnderTest& = delete;

    /// The talker's next AVTPDU as the frame the listener receives.
    [[nodiscard]] auto next_frame() -> std::vector<uint8_t>
    {
        std::vector<uint8_t> frame(avtp::CrfPdu::HEADER_LENGTH + (talker.timestamps_per_packet * avtp::CrfPdu::TIMESTAMP_SIZE));
        avtp::CrfPdu pdu{};
        auto const data = std::span<uint8_t>{frame}.subspan(avtp::CrfPdu::HEADER_LENGTH);
        EXPECT_EQ(talker.build_packet(pdu, data, START + PDU_PERIOD), data.size());  // 2 ms ahead of its arrival
        pdu.set_tu(tu);
        pdu.set_mr(mr);
        span_store(std::span<uint8_t>{frame}.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
        span_copy(std::span<uint8_t>{frame}.subspan(4, 8), make_const_span(stream));
        frame.resize(frame.size() - cut);
        return frame;
    }

    /// @p count AVTPDUs arrive, each 2 ms after the one before.
    void receive(int const count = 1)
    {
        for (int i = 0; i < count; ++i) {
            ls.on_stream_rx_frame(next_frame(), now);
            now += PDU_PERIOD;
        }
    }

    [[nodiscard]] auto counters() -> StreamInputCounters const& { return ls.slot_for(1)->counters; }
    [[nodiscard]] auto bad() -> uint64_t { return ls.slot_for(1)->rx_bad.load(); }
};

}  // namespace

// Milan v1.3 5.3.8.10 asks the counters of Table 5.6 "for each Stream Input": a CRF input counts
// its AVTPDUs as an AAF input does.
TEST(listener_crf, a_crf_input_counts_its_avtpdus_and_locks_on_their_steady_step)
{
    CrfInputUnderTest crf;
    crf.receive(20);
    auto const& c = crf.counters();
    EXPECT_EQ(crf.fed, 20);
    EXPECT_EQ(c.frames_rx.load(), 20U);
    EXPECT_EQ(c.timestamp_valid.load(), 20U);  // a CRF AVTPDU always carries its timestamps
    EXPECT_EQ(c.timestamp_not_valid.load(), 0U);
    EXPECT_EQ(c.media_locked.load(), 1U);
    EXPECT_TRUE(c.is_locked.load());
    EXPECT_EQ(c.media_unlocked.load(), 0U);
    EXPECT_EQ(c.seq_num_mismatch.load(), 0U);
    EXPECT_EQ(c.media_reset.load(), 0U);
    EXPECT_EQ(c.timestamp_uncertain.load(), 0U);
    EXPECT_EQ(c.unsupported_format.load(), 0U);
    EXPECT_EQ(c.late_timestamp.load(), 0U);
    EXPECT_EQ(c.early_timestamp.load(), 0U);
    EXPECT_EQ(crf.bad(), 0U);
}

TEST(listener_crf, the_tu_and_mr_bits_and_the_sequence_number_of_a_crf_avtpdu_are_counted)
{
    CrfInputUnderTest crf;
    crf.receive(12);
    crf.tu = true;
    crf.receive(3);
    crf.tu = false;
    EXPECT_EQ(crf.counters().timestamp_uncertain.load(), 3U);

    // mr toggles once and stays: one MEDIA_RESET.
    crf.mr = true;
    crf.receive(4);
    EXPECT_EQ(crf.counters().media_reset.load(), 1U);

    // An AVTPDU that never arrives: the next one's sequence number is not the one expected.
    (void)crf.next_frame();
    crf.now += CrfInputUnderTest::PDU_PERIOD;
    crf.receive(2);
    EXPECT_EQ(crf.counters().seq_num_mismatch.load(), 1U);
    EXPECT_EQ(crf.counters().frames_rx.load(), 21U);
    EXPECT_EQ(crf.fed, 21);
}

TEST(listener_crf, the_lock_step_of_a_crf_input_is_judged_at_the_streams_rate_and_not_the_entitys)
{
    // An entity at 192 kHz with Milan's 48 kHz CRF input: a PDU of 96 samples is 2 ms of the
    // stream's clock and would be 0.5 ms of the entity's, where 2 ms is no plausible step.
    CrfInputUnderTest crf{CrfClock{}, 192'000};
    crf.receive(20);
    EXPECT_EQ(crf.counters().media_locked.load(), 1U);
    EXPECT_TRUE(crf.counters().is_locked.load());
}

TEST(listener_crf, a_late_crf_avtpdu_is_counted_and_still_feeds_the_recovery)
{
    // IEEE 1722-2016 10.6: a timestamp already past is late; it is a clock sample all the same.
    CrfInputUnderTest crf;
    crf.receive(10);
    crf.now += std::chrono::milliseconds{5};  // the next one arrives 3 ms after its timestamp
    crf.receive(1);
    EXPECT_EQ(crf.counters().late_timestamp.load(), 1U);
    EXPECT_EQ(crf.fed, 11);
    EXPECT_EQ(crf.counters().unsupported_format.load(), 0U);
}

TEST(listener_crf, a_crf_avtpdu_that_is_not_of_the_inputs_format_is_unsupported_and_feeds_nothing)
{
    using avtp::CrfPull;
    using avtp::CrfType;
    // Milan v1.3 Table 7.1: each field of the format, wrong in turn.
    for (CrfClock const clock :
         {CrfClock{.base_frequency = 96000},
          CrfClock{.timestamp_interval = 160},
          CrfClock{.timestamps_per_pdu = 2},
          CrfClock{.pull = CrfPull::multiply_1_div_1001},
          CrfClock{.type = CrfType::video_frame}}) {
        CrfInputUnderTest crf{clock};
        crf.receive(12);
        EXPECT_EQ(crf.fed, 0);
        EXPECT_EQ(crf.counters().frames_rx.load(), 12U);
        EXPECT_EQ(crf.counters().unsupported_format.load(), 12U);
        EXPECT_EQ(crf.counters().timestamp_valid.load(), 0U);
        EXPECT_EQ(crf.counters().media_locked.load(), 0U);
        EXPECT_EQ(crf.bad(), 12U);
    }
}

// IEEE 1722-2016 Clause 10.4.3: each timestamp of a CRF AVTPDU reaches the consumer with the
// AVTPDU's mr bit, so the consumer sees a media clock restart as the bit toggles.
TEST(listener_crf, each_crf_timestamp_reaches_the_consumer_with_its_avtpdus_mr_bit)
{
    CrfInputUnderTest crf;
    std::vector<bool> seen;
    crf.ls.set_crf(1, [&seen](uint64_t, uint16_t, int64_t, bool const mr) { seen.push_back(mr); });
    crf.receive(2);
    crf.mr = true;
    crf.receive(2);
    crf.mr = false;
    crf.receive(1);
    EXPECT_TRUE(seen == (std::vector<bool>{false, false, true, true, false}));
    EXPECT_EQ(crf.counters().media_reset.load(), 2U);
}

// The source generation is the same from the first AVTPDU on and new when the input is given a
// stream: another talker stream, or its own again after losing it. A toggle of mr leaves it, and
// so does a CONNECT repeated for the stream the input is on: the same talker, the same media clock.
TEST(listener_crf, the_crf_source_is_new_when_the_input_is_given_a_stream_and_not_with_a_repeated_connect_or_an_mr_toggle)
{
    CrfInputUnderTest crf;
    std::vector<uint32_t> sources;
    crf.ls.set_crf(1, [&sources](uint64_t, uint16_t, int64_t, bool, uint32_t const source) { sources.push_back(source); });
    crf.receive(2);
    crf.mr = true;
    crf.receive(1);

    crf.ls.on_listener_disconnected(1, sm::TimePoint{}, StreamInputLoss::released);
    crf.ls.on_listener_connected(1, STREAM_1, GROUP, sm::TimePoint{});
    crf.receive(1);  // still STREAM_2's: not this input's any more
    crf.stream = STREAM_1;
    crf.receive(2);
    crf.ls.on_listener_connected(1, STREAM_1, GROUP, sm::TimePoint{});  // the CONNECT again: nothing moved
    crf.receive(1);
    crf.ls.on_listener_disconnected(1, sm::TimePoint{}, StreamInputLoss::released);
    crf.ls.on_listener_connected(1, STREAM_1, GROUP, sm::TimePoint{});  // lost and given again
    crf.receive(1);
    EXPECT_TRUE(sources == (std::vector<uint32_t>{1, 1, 1, 2, 2, 2, 3}));
}

// IEEE 1722-2025 4.7.4.4: the streams that take their timestamps from a CRF stream toggle mr "if a
// disruption of the CRF stream occurs or if the mr bit in the CRF stream has been toggled". The
// input counts both for whoever times streams by it: a toggle of mr, the input losing its stream
// or being given one, an interruption. A CONNECT repeated for its stream is none of them.
TEST(listener_crf, a_crf_input_counts_the_restarts_and_disruptions_of_the_media_clock_it_carries)
{
    CrfInputUnderTest crf;
    auto const events = [&crf] { return crf.ls.crf_media_clock_events(1).value_or(0xFFFF'FFFFU); };
    uint32_t const connected = events();
    crf.receive(10);
    EXPECT_EQ(events(), connected);  // a steady stream: nothing
    crf.mr = true;
    crf.receive(2);
    EXPECT_EQ(events(), connected + 1);  // mr toggled once and stays
    crf.ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});
    EXPECT_EQ(events(), connected + 1);  // the CONNECT again: the same stream
    crf.ls.on_listener_disconnected(1, sm::TimePoint{}, StreamInputLoss::released);
    EXPECT_EQ(events(), connected + 2);  // lost
    crf.ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});
    EXPECT_EQ(events(), connected + 3);  // given again
    crf.receive(20);                     // locked again
    EXPECT_TRUE(crf.counters().is_locked.load());
    crf.ls.on_listener_disconnected(1, sm::TimePoint{}, StreamInputLoss::talker_lost);
    EXPECT_EQ(events(), connected + 5);  // interrupted, and lost

    EXPECT_FALSE(crf.ls.crf_media_clock_events(7).has_value());  // no such input
    auto donor = make_donor();
    std::atomic<int64_t> gptp{0};
    ListenerStreams audio{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(audio.open_stream(make_aaf_spec(0)).has_value());
    EXPECT_FALSE(audio.crf_media_clock_events(0).has_value());  // an audio input carries no media clock reference
}

// An AVTPDU of the old stream matched before the new connection was published, and delivered after
// the connection was counted, opens a new source; the new stream's AVTPDUs then open another.
TEST(listener_crf, an_avtpdu_of_another_stream_id_than_the_last_is_a_new_source)
{
    CrfInputUnderTest crf;
    std::vector<uint32_t> sources;
    crf.ls.set_crf(1, [&sources](uint64_t, uint16_t, int64_t, bool, uint32_t const source) { sources.push_back(source); });
    auto& slot = *crf.ls.slot_for(1);
    std::array<uint8_t, avtp::CrfPdu::TIMESTAMP_SIZE> const data{};
    for (uint16_t const unique_id : {uint16_t{2}, uint16_t{2}, uint16_t{7}}) {
        avtp::CrfPdu pdu{};
        pdu.init_audio_sample(
            tsn::StreamId{ieee::Eui48{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x00}, unique_id}, 48000, avtp::CrfPull::multiply_1_0, 96, 1);
        crf.ls.deliver_crf_timestamps(slot, pdu, data, crf.now);
    }
    EXPECT_TRUE(sources == (std::vector<uint32_t>{1, 1, 2}));
}

// A consumer kept across close_rx() and open_stream() is handed a source it has not seen, so it
// can tell the reopened input's talker from the one before.
TEST(listener_crf, a_reopened_input_hands_its_kept_consumer_a_new_source)
{
    CrfInputUnderTest crf;
    std::vector<uint32_t> sources;
    crf.ls.set_crf(1, [&sources](uint64_t, uint16_t, int64_t, bool, uint32_t const source) { sources.push_back(source); });
    crf.receive(2);
    crf.ls.close_rx();
    StreamSpec spec{};
    spec.index = 1;
    spec.format_word = CRF_MILAN_48K;
    spec.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(crf.ls.open_stream(spec).has_value());
    crf.ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});
    crf.receive(1);
    EXPECT_TRUE(sources == (std::vector<uint32_t>{1, 1, 2}));
}

// The kit's recovery kept across close_rx() and open_stream() starts its estimate again with the
// reopened input's first timestamp, not on top of the run before.
TEST(listener_crf, a_recovery_kept_across_a_reopen_of_its_input_restarts)
{
    CrfInputUnderTest crf;
    CrfClockRecovery rec{};
    crf.ls.set_crf(1, rec.make_consumer());
    crf.receive(20);
    EXPECT_TRUE(rec.estimate().samples == 20U);
    crf.ls.close_rx();
    StreamSpec spec{};
    spec.index = 1;
    spec.format_word = CRF_MILAN_48K;
    spec.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(crf.ls.open_stream(spec).has_value());
    crf.ls.on_listener_connected(1, STREAM_2, GROUP, sm::TimePoint{});
    crf.receive(1);
    auto const est = rec.estimate();
    EXPECT_TRUE(est.samples == 1U);
    EXPECT_FALSE(est.locked);
}

TEST(listener_crf, a_crf_frame_cut_short_of_its_timestamp_or_of_its_header_is_unsupported_too)
{
    for (size_t const cut : {size_t{1}, avtp::CrfPdu::TIMESTAMP_SIZE, avtp::CrfPdu::TIMESTAMP_SIZE + 3}) {
        CrfInputUnderTest crf;
        crf.cut = cut;
        crf.receive(3);
        EXPECT_EQ(crf.fed, 0);
        EXPECT_EQ(crf.counters().frames_rx.load(), 3U);
        EXPECT_EQ(crf.counters().unsupported_format.load(), 3U);
        EXPECT_EQ(crf.bad(), 3U);
    }
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

// What a data plane beside the port received reaches the input's counters, so GET_COUNTERS (IEEE 1722.1-2021
// Clause 7.4.42) goes on, and its telemetry.
TEST(listener_offload, a_report_reaches_the_counters_and_the_telemetry)
{
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_FALSE(ls.notify_offload_received(5, StreamInputCounts{.frames_rx = 1}, 12));  // no such input
    EXPECT_TRUE(ls.notify_offload_received(
        0, StreamInputCounts{.frames_rx = 10, .timestamp_valid = 8, .unsupported_format = 2, .media_locked = true}, 96));
    auto const* const slot = ls.slot_for(0);
    EXPECT_EQ(slot->rx_packets.load(), uint64_t{8});
    EXPECT_EQ(slot->rx_bad.load(), uint64_t{2});
    EXPECT_EQ(slot->rx_samples.load(), uint64_t{96});
    uint32_t valid = 0;
    std::array<uint32_t, 32> out{};
    EXPECT_TRUE(ls.fill_stream_input_counters(0, valid, out));
    EXPECT_EQ(out[11], 10U);  // FRAMES_RX
    EXPECT_EQ(out[8], 2U);    // UNSUPPORTED_FORMAT
    EXPECT_EQ(out[6], 8U);    // TIMESTAMP_VALID
    EXPECT_EQ(out[0], 1U);    // MEDIA_LOCKED
    EXPECT_TRUE(ls.notify_offload_received(0, StreamInputCounts{.frames_rx = 8, .media_locked = true}, 96));
    EXPECT_TRUE(ls.fill_stream_input_counters(0, valid, out));
    EXPECT_EQ(out[0], 1U);  // still locked: no second count
    EXPECT_EQ(out[11], 18U);
}

// Milan v1.3 5.3.8.7 and 5.3.8.10: a stopped input is not synchronized, so a lock reported lost then is no
// interruption, and nothing reaches the telemetry; a started bound one's loss is one; with no stream, the
// release decides.
TEST(listener_offload, a_lock_lost_while_stopped_or_without_a_stream_is_no_interruption)
{
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    auto const& c = ls.slot_for(0)->counters;
    auto lock = [&ls](bool const locked) {
        EXPECT_TRUE(ls.notify_offload_received(0, StreamInputCounts{.frames_rx = 8, .media_locked = locked}, 48));
    };
    lock(true);
    lock(true);
    EXPECT_TRUE(c.is_locked.load());

    ls.set_stream_stopped(0, true);
    lock(false);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    EXPECT_EQ(c.stream_interrupted.load(), 0U);
    lock(true);  // discarded: not synchronized
    EXPECT_FALSE(c.is_locked.load());
    EXPECT_EQ(ls.slot_for(0)->rx_packets.load(), uint64_t{16});  // the two reports before the stop
    ls.set_stream_stopped(0, false);
    lock(true);
    EXPECT_TRUE(c.is_locked.load());
    lock(false);
    EXPECT_EQ(c.stream_interrupted.load(), 1U);

    lock(true);
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::released);
    lock(false);
    EXPECT_EQ(c.stream_interrupted.load(), 1U);
}

// Milan v1.3 Table 5.6: an input whose talker goes is interrupted, even when the data plane reports its
// last counts, the lock lost, from inside the hook that tells it the stream is gone.
TEST(listener_offload, a_talker_lost_is_an_interruption_with_a_report_from_the_hook)
{
    auto donor = make_donor();
    std::atomic<int64_t> const gptp{0};
    ListenerStreams ls{1'000'000, 96'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    ls.on_listener_connected(0, STREAM_1, GROUP, sm::TimePoint{});
    EXPECT_TRUE(ls.notify_offload_received(0, StreamInputCounts{.frames_rx = 8, .media_locked = true}, 48));
    bool flushed = false;
    ls.set_on_input_changed([&ls, &flushed](uint16_t const index, ListenerStreams::InputState const& state) {
        if (state.stream_id == 0) {
            flushed = ls.notify_offload_received(index, StreamInputCounts{.frames_rx = 2, .media_locked = false}, 12);
        }
    });
    ls.on_listener_disconnected(0, sm::TimePoint{}, StreamInputLoss::talker_lost);
    EXPECT_TRUE(flushed);
    auto const& c = ls.slot_for(0)->counters;
    EXPECT_EQ(c.stream_interrupted.load(), 1U);
    EXPECT_EQ(c.media_unlocked.load(), 1U);
    EXPECT_EQ(c.frames_rx.load(), 10U);
    ls.set_on_input_changed({});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_streams_test)
