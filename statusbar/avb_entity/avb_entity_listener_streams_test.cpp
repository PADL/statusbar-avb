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

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <utility>
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

}  // namespace

TEST(listener_consume, pending_binds_on_open_unknown_stays_inert)
{
    auto donor = make_donor();
    std::atomic<uint64_t> gptp{0};
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
    std::atomic<uint64_t> gptp{0};
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
    std::atomic<uint64_t> gptp{0};
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

// aaf_rx_packet() takes a packet's timing from its header alone and agrees with
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

// The acceptance rule is the deserializer's (avtp::aaf_payload_frames): no
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
    EXPECT_EQ(avtp::aaf_payload_frames(rx, audio.first(5)), uint16_t{0});

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
    EXPECT_EQ(avtp::aaf_payload_frames(wide, big), uint16_t{0});
    auto const refused = aaf_rx_packet(0, wide, pdu, big, 0);
    EXPECT_EQ(refused.frames, uint32_t{0});
    EXPECT_EQ(wide.packets_received, uint32_t{0});
    bool called = false;
    avtp::aaf_stream_deserialize(wide, pdu, big, 0, [&](uint8_t, std::span<float>, uint64_t, uint64_t) { called = true; });
    EXPECT_FALSE(called);
    EXPECT_EQ(wide.packets_received, uint32_t{0});
}

// Through on_stream_rx_frame on a connected listener: the timed sink gets each
// AAF packet; with no consume fn the float decode is skipped, with one it runs,
// and either way the sink sees the same packet and the slot the same counters.
// Detaching the sink stops delivery.
TEST(listener_timed, delivery_through_on_stream_rx_frame)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& aaf_spec = specs[1];
    EXPECT_TRUE(aaf_spec.format.kind == StreamKind::aaf);
    std::atomic<uint64_t> gptp{0};
    RecordingSink raw_sink;
    RecordingSink decoded_sink;
    ListenerStreams raw{1'000'000, aaf_spec.format.sample_rate_hz, donor->components(), gptp, nullptr};
    ListenerStreams decoded{1'000'000, aaf_spec.format.sample_rate_hz, donor->components(), gptp, nullptr};
    EXPECT_TRUE(raw.open_stream(aaf_spec).has_value());
    EXPECT_TRUE(decoded.open_stream(aaf_spec).has_value());
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
    std::atomic<uint64_t> gptp{0};
    RecordingSink sink;
    ListenerStreams ls{1'000'000, spec.format.sample_rate_hz, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(spec).has_value());
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

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_streams_test)
