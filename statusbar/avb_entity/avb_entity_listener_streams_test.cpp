// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ListenerStreams RX-slot tests (kit phases 1+2): spec-shaped slot creation,
// the per-stream consume registration mechanics (pending-before-open, direct
// rebind after open), and the menu/selection principle — a registration for a
// stream index the model never declares stays pending and inert, never an
// error. A CRF slot hands each timestamp and its PDU's mr bit to its consumer.
//
// Positive frame->consume delivery requires a connected ACMP listener + live
// frames and is exercised on hardware; the decode paths themselves are
// covered by the avtp deserializer unit tests.

#include "statusbar/avb_entity/avb_entity_listener_streams.hpp"

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avb_entity/avb_entity_test_stream_port.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_am824_stream_output.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_message_reactor.hpp"
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
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};

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
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};

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
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};

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

/// Connect (or disconnect) @p donor's STREAM_INPUT @p sink and talker stream @p id: the classic
/// CONNECT_RX / CONNECT_TX (DISCONNECT_RX / DISCONNECT_TX) exchange.
void connect_sink(
    AvbEntityAudioIO& donor, uint16_t const sink, tsn::StreamId const id, uint16_t const sequence_id, bool const connect = true)
{
    auto& listener = donor.components().acmp_listener;
    ieee::Eui64 const talker_id{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    atdecc::AcmpCommandResponse cmd{};
    cmd.set_message_type(connect ? atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND : atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    cmd.talker_entity_id = talker_id;
    cmd.talker_unique_id = id.get_unique_id();  // one talker stream, one talker unique id
    cmd.listener_entity_id = ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    cmd.listener_unique_id = sink;
    cmd.sequence_id = sequence_id;
    EXPECT_TRUE(listener.receive_controller_command(cmd, sm::TimePoint{}));
    atdecc::AcmpCommandResponse resp = cmd;
    resp.set_message_type(
        connect ? atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE : atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE);
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
    EXPECT_EQ(listener.is_connected(sink), connect);
}

/// An audio entity (it has listener streams) whose ACMP listener reports
/// STREAM_INPUTs 0 (AM824) and 1 (AAF) connected to the given stream ids, by
/// the real CONNECT_RX / CONNECT_TX handshake.
auto make_connected_donor(tsn::StreamId const am824_id, tsn::StreamId const aaf_id) -> std::unique_ptr<AvbEntityAudioIO>
{
    AvbEntityAudioIOConfig config{};
    config.entity_id = ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.descriptor_storage_blob = audio_blob();
    config.milan = false;  // the classic CONNECT_RX / CONNECT_TX handshake
    auto created = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(created.has_value());
    auto& listener = (*created)->components().acmp_listener;
    nanoavb::AcmpListenerCallbacks cb;
    cb.tx_command = [](atdecc::AcmpCommandResponse const&) { return true; };
    cb.tx_response = [](atdecc::AcmpCommandResponse const&) { return true; };
    listener.set_callbacks(cb);
    listener.start();
    connect_sink(**created, 0, am824_id, 1);
    connect_sink(**created, 1, aaf_id, 2);
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

// The timed sink gets each AAF packet through on_stream_rx_frame, with or without a consume fn
// (same packet, same counters); detaching stops delivery.
TEST(listener_timed, delivery_through_on_stream_rx_frame)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& aaf_spec = specs[1];
    EXPECT_TRUE(aaf_spec.format.kind == StreamKind::aaf);
    std::atomic<uint64_t> gptp{0};
    RecordingSink raw_sink;
    RecordingSink decoded_sink;
    ListenerStreams raw{1'000'000, donor->components(), gptp, nullptr};
    ListenerStreams decoded{1'000'000, donor->components(), gptp, nullptr};
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

// Milan v1.2 Section 4.4.2.2: AVTPDUs that don't respect the STREAM_INPUT's
// configured format are discarded and counted in UNSUPPORTED_FORMAT.
TEST(listener_format, aaf_header_matches_format)
{
    StreamFormatFields f{};
    f.kind = StreamKind::aaf;
    f.sample_rate_hz = 48000;
    f.channels = 8;
    f.bit_depth = 32;
    f.aaf_format = AafFormat::int_32bit;
    f.aaf_samples_per_frame = 6;
    avtp::AafStreamOutputContext tx{kAafId, AafFormat::int_32bit, AafSampleRate::rate_48_khz, 8, 32, 0};
    auto header = [&](avtp::AafStreamOutputContext& ctx, uint16_t frames) {
        auto const bytes = aaf_frame(ctx, frames, 1'000'000);
        return *avtp::aaf_parse_header(std::span<uint8_t const>{bytes.data(), bytes.size()});
    };
    EXPECT_TRUE(aaf_header_matches_format(f, header(tx, 6)));
    EXPECT_FALSE(aaf_header_matches_format(f, header(tx, 7)));  // not the format's 6 samples
    auto pdu = header(tx, 6);
    pdu.set_bit_depth(24);  // 24 bits in an INT_32 container: same layout, accepted
    EXPECT_TRUE(aaf_header_matches_format(f, pdu));
    pdu.set_bit_depth(0);
    EXPECT_FALSE(aaf_header_matches_format(f, pdu));
    pdu.set_bit_depth(33);
    EXPECT_FALSE(aaf_header_matches_format(f, pdu));
    avtp::AafStreamOutputContext two{kAafId, AafFormat::int_32bit, AafSampleRate::rate_48_khz, 2, 32, 0};
    EXPECT_FALSE(aaf_header_matches_format(f, header(two, 6)));  // channels
    avtp::AafStreamOutputContext fast{kAafId, AafFormat::int_32bit, AafSampleRate::rate_96_khz, 8, 32, 0};
    EXPECT_FALSE(aaf_header_matches_format(f, header(fast, 6)));  // rate
    avtp::AafStreamOutputContext narrow{kAafId, AafFormat::int_24bit, AafSampleRate::rate_48_khz, 8, 24, 0};
    EXPECT_FALSE(aaf_header_matches_format(f, header(narrow, 6)));  // sample format
    f.aaf_samples_per_frame = 0;                                    // a format word naming no count
    EXPECT_TRUE(aaf_header_matches_format(f, header(tx, 7)));
}

TEST(listener_format, mismatched_aaf_is_discarded_and_counted)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& aaf_spec = specs[1];
    std::atomic<uint64_t> gptp{0};
    RecordingSink sink;
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(aaf_spec).has_value());
    ls.set_timed_audio_sink(&sink);
    auto const rate = *avtp::aaf_sample_rate_from_hz(aaf_spec.format.sample_rate_hz);
    uint16_t const spp = static_cast<uint16_t>(class_a_samples_per_packet(aaf_spec.format.sample_rate_hz));
    avtp::AafStreamOutputContext good{
        kAafId, aaf_spec.format.aaf_format, rate, aaf_spec.format.channels, aaf_spec.format.bit_depth, 0};
    avtp::AafStreamOutputContext wrong{
        kAafId,
        aaf_spec.format.aaf_format,
        rate,
        static_cast<uint16_t>(aaf_spec.format.channels + 1),
        aaf_spec.format.bit_depth,
        0};
    uint64_t const pts0 = 9'000'000'000ULL;
    ls.on_stream_rx_frame(aaf_frame(good, spp, pts0), static_cast<int64_t>(pts0 - 1'000'000));
    ls.on_stream_rx_frame(aaf_frame(wrong, spp, pts0 + 125'000), static_cast<int64_t>(pts0 - 1'000'000));
    EXPECT_EQ(sink.seen.size(), size_t{1});
    auto const* slot = ls.slot_for(1);
    EXPECT_EQ(slot->counters.unsupported_format.load(), uint32_t{1});
    EXPECT_EQ(slot->counters.frames_rx.load(), uint32_t{2});
    EXPECT_EQ(slot->rx_packets.load(), uint64_t{1});
}

// MEDIA_LOCKED follows each stream's own rate: a 192 kHz input's 24 frames a
// packet lock on their 125 us step (at 48 kHz that step would be implausible)
TEST(listener_format, media_lock_is_at_the_streams_rate)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    StreamSpec spec{};
    spec.index = 1;
    spec.format_word = 0x0209031800000000ULL | (uint64_t{2} << 22U) | (uint64_t{24} << 12U);  // INT_24, 2 ch, 192 kHz
    spec.format = decode_stream_format(spec.format_word);
    EXPECT_EQ(spec.format.sample_rate_hz, 192000U);
    std::atomic<uint64_t> gptp{0};
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(spec).has_value());
    avtp::AafStreamOutputContext tx{kAafId, spec.format.aaf_format, AafSampleRate::rate_192_khz, 2, 24, 0};
    uint64_t const pts0 = 9'000'000'000ULL;
    for (uint64_t k = 0; k < 12; ++k) {
        ls.on_stream_rx_frame(aaf_frame(tx, 24, pts0 + (k * 125'000)), static_cast<int64_t>(pts0 - 1'000'000));
    }
    auto const& counters = ls.slot_for(1)->counters;
    EXPECT_EQ(counters.unsupported_format.load(), uint32_t{0});
    EXPECT_EQ(counters.media_locked.load(), uint32_t{1});
    EXPECT_EQ(counters.media_unlocked.load(), uint32_t{0});
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
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
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

//
// Receive through a StreamPort
//

namespace {

ieee::Eui48 const kRxPortMac{0x00, 0x1B, 0x21, 0x0D, 0x0E, 0x0F};
ieee::Eui48 const kTalkerMac{0x00, 0x1B, 0x21, 0x01, 0x02, 0x03};
ieee::Eui48 const kAafGroup{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};  // the donor's STREAM_INPUT 1 destination

/// @p avtpdu as an Ethernet frame from the talker to @p dest: untagged, or
/// under one 802.1Q tag (a NIC with receive VLAN offload off), or with
/// another @p ethertype.
auto l2_frame(std::span<uint8_t const> const avtpdu, bool const tagged, uint16_t const ethertype = 0x22F0) -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame{kAafGroup.value.begin(), kAafGroup.value.end()};
    frame.insert(frame.end(), kTalkerMac.value.begin(), kTalkerMac.value.end());
    if (tagged) {
        for (uint8_t const b : {uint8_t{0x81}, uint8_t{0x00}, uint8_t{0x60}, uint8_t{0x02}}) {
            frame.push_back(b);
        }
    }
    frame.push_back(static_cast<uint8_t>(ethertype >> 8U));
    frame.push_back(static_cast<uint8_t>(ethertype));
    frame.insert(frame.end(), avtpdu.begin(), avtpdu.end());
    return frame;
}

}  // namespace

// drain_rx takes frames from the port: an untagged and a tagged AAF frame both
// reach the connected slot with the AVTPDU at its offset (14, 18), and a
// frame of another ethertype reaches nothing.
TEST(listener_port, tagged_and_untagged_frames_reach_their_slot)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& aaf_spec = specs[1];
    std::atomic<uint64_t> gptp{0};
    RecordingSink sink;
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(aaf_spec).has_value());
    ls.set_timed_audio_sink(&sink);
    TestStreamPort port{kRxPortMac};
    itc::StopToken stop{};
    net::MessageReactor reactor{stop, net::monotonic_ns, 0};
    auto handler = ls.attach_rx(port, {}, reactor, /*keep_handler=*/true);
    EXPECT_TRUE(handler.has_value() && *handler != nullptr);
    EXPECT_EQ((*handler)->fd(), -1);  // nothing for the reactor to poll

    auto const rate = *avtp::aaf_sample_rate_from_hz(aaf_spec.format.sample_rate_hz);
    uint16_t const spp = static_cast<uint16_t>(class_a_samples_per_packet(aaf_spec.format.sample_rate_hz));
    avtp::AafStreamOutputContext tx{
        kAafId, aaf_spec.format.aaf_format, rate, aaf_spec.format.channels, aaf_spec.format.bit_depth, 0};
    uint64_t const pts0 = 9'000'000'000ULL;
    auto const untagged = aaf_frame(tx, spp, pts0);
    auto const tagged = aaf_frame(tx, spp, pts0 + 125'000);
    EXPECT_TRUE(port.inject_rx(l2_frame(untagged, false)).has_value());
    EXPECT_TRUE(port.inject_rx(l2_frame(untagged, false, 0x88F7)).has_value());
    EXPECT_TRUE(port.inject_rx(l2_frame(tagged, true)).has_value());

    EXPECT_EQ(ls.drain_rx(static_cast<int64_t>(pts0 - 1'000'000)), size_t{2});
    EXPECT_EQ(ls.drain_rx(static_cast<int64_t>(pts0 - 1'000'000)), size_t{0});
    EXPECT_EQ(sink.seen.size(), size_t{2});
    EXPECT_EQ(ls.slot_for(1)->rx_packets.load(), uint64_t{2});
    if (sink.seen.size() == 2) {
        auto const first = avtp::aaf_get_audio_payload(untagged);
        auto const second = avtp::aaf_get_audio_payload(tagged);
        EXPECT_EQ(sink.seen[0].packet.pts_ns, pts0);
        EXPECT_EQ(sink.seen[1].packet.pts_ns, pts0 + 125'000);
        EXPECT_TRUE(std::ranges::equal(sink.seen[0].bytes, first));
        EXPECT_TRUE(std::ranges::equal(sink.seen[1].bytes, second));
    }
    EXPECT_EQ(port.stats().rx_frames, uint64_t{2});
}

// attach_rx joins the static groups on the port; an ACMP connect joins the
// talker's group and the disconnect leaves it.
TEST(listener_port, groups_are_joined_and_left_on_the_port)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    std::atomic<uint64_t> gptp{0};
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(specs[1]).has_value());
    TestStreamPort port{kRxPortMac};
    itc::StopToken stop{};
    net::MessageReactor reactor{stop, net::monotonic_ns, 0};
    ieee::Eui48 const static_group{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x42};
    std::array<ieee::Eui48, 1> const groups{static_group};
    auto handler = ls.attach_rx(port, groups, reactor);
    EXPECT_TRUE(handler.has_value() && *handler == nullptr);  // added to the reactor
    EXPECT_EQ(port.joined().size(), size_t{1});

    ls.on_listener_connected(1, ieee::Eui64{}, kAafGroup);
    EXPECT_EQ(port.joined().size(), size_t{2});
    EXPECT_TRUE(std::ranges::find(port.joined(), kAafGroup) != port.joined().end());
    ls.on_listener_disconnected(1);
    EXPECT_EQ(port.joined().size(), size_t{1});
    EXPECT_TRUE(port.joined()[0] == static_group);
}

//
// Raw AM824 receive (IEC 61883-6, IEEE 1722-2016 Clause 5)
//

namespace {

/// The presentation time of sample @p index on a 48/96/192 kHz grid anchored at @p t0 for @p i0.
auto grid_pts(uint64_t const t0, uint64_t const i0, uint64_t const index, uint32_t const rate_hz) -> uint64_t
{
    return t0 + ((((index - i0) * 1'000'000'000ULL) + (rate_hz / 2U)) / rate_hz);
}

/// The sample index whose presentation time @p pts is nearest on that grid.
auto grid_index(uint64_t const t0, uint64_t const i0, uint64_t const pts, uint32_t const rate_hz) -> uint64_t
{
    return i0 + ((((pts - t0) * rate_hz) + 500'000'000ULL) / 1'000'000'000ULL);
}

/// An AM824 frame built by the raw talker: block b, channel c carries 0x40 | (index * channels + c).
auto am824_frame(avtp::Am824StreamOutputContext& tx, uint16_t const frames, uint64_t const first_index, uint64_t const pts)
    -> std::vector<uint8_t>
{
    uint8_t const channels = tx.config.channel_count;
    std::vector<uint8_t> frame(avtp::Am824Pdu::HEADER_LENGTH + (size_t{frames} * channels * 4U));
    StreamRawRenderFn const render = [channels](std::span<uint8_t> payload, uint32_t n, uint64_t first, uint64_t) {
        for (uint32_t i = 0; i < n * channels; ++i) {
            uint32_t const v = static_cast<uint32_t>((first * channels) + i) & 0x00FF'FFFFU;
            payload[(i * 4U) + 0] = avtp::AM824_LABEL_MBLA;
            payload[(i * 4U) + 1] = static_cast<uint8_t>(v >> 16U);
            payload[(i * 4U) + 2] = static_cast<uint8_t>(v >> 8U);
            payload[(i * 4U) + 3] = static_cast<uint8_t>(v);
        }
        return true;
    };
    EXPECT_EQ(serialize_am824_raw(tx, frame, frames, first_index, pts, render), frame.size());
    return frame;
}

}  // namespace

// am824_rx_packet places each packet by its first block: tv 3 packets in 4, the rest extrapolated by
// DBC, map to the sample index the talker sent; the quadlets pass as received. A lost packet is one
// DBC discontinuity and one sequence gap, and the next packet still lands at its index.
TEST(listener_timed, am824_rx_packet_frames_land_by_sample_index)
{
    for (auto const& [rate, frames] :
         {std::pair{avtp::Am824SampleRate::rate_48_khz, uint16_t{6}},
          std::pair{avtp::Am824SampleRate::rate_96_khz, uint16_t{12}},
          std::pair{avtp::Am824SampleRate::rate_192_khz, uint16_t{24}}}) {
        uint32_t const rate_hz = avtp::am824_sample_rate_hz(rate);
        avtp::Am824StreamOutputContext tx{kAm824Id, rate, 2, 0};
        avtp::Am824StreamInputContext rx{rate, 2};
        uint64_t const i0 = 480'000;
        uint64_t const t0 = (uint64_t{9} << 32U) - 1'000'000;  // crosses a 2^32 ns wrap
        size_t delivered = 0;
        for (uint64_t k = 0; k < 16; ++k) {
            uint64_t const first = i0 + (k * frames);
            uint64_t const pts = grid_pts(t0, i0, first, rate_hz);
            auto const frame = am824_frame(tx, frames, first, pts);
            if (k == 9) {
                continue;  // lost on the wire
            }
            auto const pdu = avtp::am824_parse_header(frame);
            EXPECT_TRUE(pdu.has_value());
            auto const audio = avtp::am824_get_audio_payload(frame);
            auto const packet = am824_rx_packet(2, rx, *pdu, audio, pts - 2'000'000);
            ++delivered;
            EXPECT_EQ(packet.stream_index, uint16_t{2});
            EXPECT_TRUE(packet.fmt == StreamAudioFormat::am824_mbla);
            EXPECT_EQ(packet.frames, uint32_t{frames});
            EXPECT_EQ(packet.channels, uint16_t{2});
            EXPECT_EQ(packet.bytes_per_sample, uint8_t{4});
            EXPECT_EQ(packet.payload.size(), size_t{frames} * 2U * 4U);
            EXPECT_EQ(packet.payload.data(), audio.data());
            EXPECT_EQ(packet.payload[0], avtp::AM824_LABEL_MBLA);
            EXPECT_TRUE(packet.pts_valid);
            EXPECT_EQ(grid_index(t0, i0, packet.pts_ns, rate_hz), first);
            uint64_t const err = packet.pts_ns > pts ? packet.pts_ns - pts : pts - packet.pts_ns;
            EXPECT_TRUE(err < 50);
        }
        EXPECT_EQ(rx.packets_received, static_cast<uint32_t>(delivered));
        EXPECT_EQ(rx.dbc_discontinuities, uint32_t{1});
        EXPECT_EQ(rx.sequence_gaps, uint32_t{1});
    }
}

// Before the first tv packet there is no anchor: pts_valid is false; an empty payload is refused
// with the context untouched.
TEST(listener_timed, am824_rx_packet_before_an_anchor_and_refusals)
{
    avtp::Am824StreamOutputContext tx{kAm824Id, avtp::Am824SampleRate::rate_48_khz, 2, 0};
    avtp::Am824StreamInputContext rx{avtp::Am824SampleRate::rate_48_khz, 2};
    auto const untimed = am824_frame(tx, 6, 18, 1'000'000'000ULL);  // blocks 18..23: no multiple of 8
    auto const pdu = *avtp::am824_parse_header(untimed);
    EXPECT_FALSE(pdu.tv());
    auto const none = am824_rx_packet(0, rx, pdu, avtp::am824_get_audio_payload(untimed).first(7), 0);
    EXPECT_EQ(none.frames, uint32_t{0});
    EXPECT_EQ(rx.packets_received, uint32_t{0});
    auto const first = am824_rx_packet(0, rx, pdu, avtp::am824_get_audio_payload(untimed), 999'000'000ULL);
    EXPECT_EQ(first.frames, uint32_t{6});
    EXPECT_FALSE(first.pts_valid);
    auto const timed = am824_frame(tx, 6, 24, 1'000'125'000ULL);
    auto const anchored =
        am824_rx_packet(0, rx, *avtp::am824_parse_header(timed), avtp::am824_get_audio_payload(timed), 999'000'000ULL);
    EXPECT_TRUE(anchored.pts_valid);
    EXPECT_EQ(anchored.pts_ns, uint64_t{1'000'125'000ULL});
    EXPECT_EQ(rx.dbc_discontinuities, uint32_t{0});
}

// Through on_stream_rx_frame: with no consumer the timed sink gets the quadlets without the float
// decode, the same packets as the decoding path; a packet whose DBS or SFC is not the input's format
// is discarded and counted in UNSUPPORTED_FORMAT.
TEST(listener_timed, am824_raw_delivery_through_on_stream_rx_frame)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const specs = audio_specs();
    auto const& spec = specs[0];
    EXPECT_TRUE(spec.format.kind == StreamKind::am824);
    std::atomic<uint64_t> gptp{0};
    RecordingSink raw_sink;
    RecordingSink decoded_sink;
    ListenerStreams raw{1'000'000, donor->components(), gptp, nullptr};
    ListenerStreams decoded{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(raw.open_stream(spec).has_value());
    EXPECT_TRUE(decoded.open_stream(spec).has_value());
    raw.set_timed_audio_sink(&raw_sink);
    decoded.set_timed_audio_sink(&decoded_sink);
    int decoded_channels = 0;
    decoded.set_consume(0, [&](uint8_t, std::span<float const>, uint64_t, uint64_t) { ++decoded_channels; });

    auto const rate = *avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
    auto const channels = static_cast<uint8_t>(spec.format.channels);
    auto const spp = static_cast<uint16_t>(class_a_samples_per_packet(spec.format.sample_rate_hz));
    avtp::Am824StreamOutputContext tx{kAm824Id, rate, channels, 0};
    uint64_t const t0 = 9'000'000'000ULL;
    for (uint64_t k = 0; k < 4; ++k) {
        auto frame = am824_frame(tx, spp, k * spp, t0 + (k * 125'000));
        frame.resize(frame.size() + 4);  // padding past stream_data_length
        raw.on_stream_rx_frame(frame, static_cast<int64_t>(t0 - 1'000'000));
        decoded.on_stream_rx_frame(frame, static_cast<int64_t>(t0 - 1'000'000));
    }
    EXPECT_EQ(raw_sink.seen.size(), size_t{4});
    EXPECT_EQ(decoded_sink.seen.size(), size_t{4});
    EXPECT_EQ(decoded_channels, 4 * channels);
    for (size_t i = 0; i < raw_sink.seen.size() && i < decoded_sink.seen.size(); ++i) {
        auto const& a = raw_sink.seen[i];
        auto const& b = decoded_sink.seen[i];
        EXPECT_EQ(a.packet.pts_ns, b.packet.pts_ns);
        EXPECT_EQ(a.packet.pts_valid, b.packet.pts_valid);
        EXPECT_EQ(a.packet.frames, uint32_t{spp});
        EXPECT_EQ(a.bytes.size(), size_t{spp} * channels * 4U);
        EXPECT_TRUE(a.bytes == b.bytes);
    }
    auto const* slot = raw.slot_for(0);
    EXPECT_EQ(slot->rx_packets.load(), decoded.slot_for(0)->rx_packets.load());
    EXPECT_EQ(slot->am824->packets_received, decoded.slot_for(0)->am824->packets_received);

    avtp::Am824StreamOutputContext narrow{kAm824Id, rate, 2, 0};
    raw.on_stream_rx_frame(am824_frame(narrow, spp, 4U * spp, t0 + 500'000), static_cast<int64_t>(t0));
    avtp::Am824StreamOutputContext slow{kAm824Id, avtp::Am824SampleRate::rate_48_khz, channels, 0};
    raw.on_stream_rx_frame(am824_frame(slow, 6, 4U * spp, t0 + 500'000), static_cast<int64_t>(t0));
    EXPECT_EQ(raw_sink.seen.size(), size_t{4});
    EXPECT_EQ(slot->counters.unsupported_format.load(), uint32_t{2});
    EXPECT_EQ(slot->rx_packets.load(), uint64_t{4});
}

// An AAF input re-shaped to AM824 and back (reformat_stream): each kind's frames land while it is the
// slot's, the other kind's are not matched (none parsed as the wrong kind), and a frame arriving while
// the reactor holds the slot is dropped.
TEST(listener_kind, frames_follow_a_change_between_aaf_and_am824)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    auto const aaf_spec = audio_specs()[1];
    uint32_t const rate_hz = aaf_spec.format.sample_rate_hz;
    StreamSpec am824_spec = aaf_spec;
    am824_spec.format_word = avtp::make_am824_stream_format(rate_hz == 96000 ? 4 : 2, 2);
    am824_spec.format = decode_stream_format(am824_spec.format_word);
    EXPECT_TRUE(am824_spec.format.kind == StreamKind::am824 && am824_spec.format.sample_rate_hz == rate_hz);
    std::atomic<uint64_t> gptp{0};
    RecordingSink sink;
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(aaf_spec).has_value());
    ls.set_timed_audio_sink(&sink);
    auto* const slot = ls.slot_for(1);

    auto const spp = static_cast<uint16_t>(class_a_samples_per_packet(rate_hz));
    avtp::AafStreamOutputContext aaf_tx{
        kAafId,
        aaf_spec.format.aaf_format,
        *avtp::aaf_sample_rate_from_hz(rate_hz),
        aaf_spec.format.channels,
        aaf_spec.format.bit_depth,
        0};
    avtp::Am824StreamOutputContext am824_tx{kAafId, *avtp::am824_sample_rate_from_hz(rate_hz), 2, 0};
    uint64_t const t0 = 9'000'000'000ULL;
    auto const now = static_cast<int64_t>(t0 - 1'000'000);
    uint64_t k = 0;
    auto const aaf = [&] { return aaf_frame(aaf_tx, spp, t0 + (k++ * 125'000)); };
    auto const am824 = [&] {
        auto frame = am824_frame(am824_tx, spp, k * spp, t0 + (k * 125'000));
        ++k;
        return frame;
    };

    ls.on_stream_rx_frame(aaf(), now);
    ls.on_stream_rx_frame(am824(), now);  // not this slot's kind yet
    EXPECT_EQ(sink.seen.size(), size_t{1});

    EXPECT_TRUE(ls.reformat_stream(am824_spec).has_value());
    EXPECT_TRUE(slot->kind.load() == StreamKind::am824 && slot->am824.has_value() && !slot->aaf.has_value());
    ls.on_stream_rx_frame(am824(), now);
    ls.on_stream_rx_frame(aaf(), now);  // no longer this slot's kind
    slot->guard.lock();
    ls.on_stream_rx_frame(am824(), now);  // the reactor holds the slot: dropped
    slot->guard.unlock();
    ls.on_stream_rx_frame(am824(), now);
    EXPECT_EQ(sink.seen.size(), size_t{3});

    EXPECT_TRUE(ls.reformat_stream(aaf_spec).has_value());
    EXPECT_TRUE(slot->kind.load() == StreamKind::aaf && slot->aaf.has_value() && !slot->am824.has_value());
    ls.on_stream_rx_frame(am824(), now);
    ls.on_stream_rx_frame(aaf(), now);
    EXPECT_EQ(sink.seen.size(), size_t{4});

    if (sink.seen.size() == 4) {
        EXPECT_TRUE(sink.seen[0].packet.fmt == StreamAudioFormat::aaf_int32);
        EXPECT_TRUE(sink.seen[1].packet.fmt == StreamAudioFormat::am824_mbla);
        EXPECT_TRUE(sink.seen[2].packet.fmt == StreamAudioFormat::am824_mbla);
        EXPECT_TRUE(sink.seen[3].packet.fmt == StreamAudioFormat::aaf_int32);
        EXPECT_EQ(sink.seen[1].packet.channels, uint16_t{2});
        EXPECT_EQ(sink.seen[1].packet.frames, uint32_t{spp});
        EXPECT_EQ(sink.seen[3].packet.channels, aaf_spec.format.channels);
    }
    EXPECT_EQ(slot->rx_packets.load(), uint64_t{4});
    EXPECT_EQ(slot->rx_bad.load(), uint64_t{0});
    EXPECT_EQ(slot->counters.unsupported_format.load(), uint32_t{0});
}

// A CRF slot never changes kind, nor an audio slot to CRF.
TEST(listener_kind, crf_kind_changes_are_refused)
{
    auto donor = make_donor();
    std::atomic<uint64_t> gptp{0};
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    EXPECT_TRUE(ls.open_stream(make_aaf_spec(0)).has_value());
    StreamSpec crf{};
    crf.index = 1;
    crf.format_word = CRF_MILAN_48K;
    crf.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(ls.open_stream(crf).has_value());
    StreamSpec to_crf = crf;
    to_crf.index = 0;
    EXPECT_FALSE(ls.reformat_stream(to_crf).has_value());
    EXPECT_TRUE(ls.slot_for(0)->kind.load() == StreamKind::aaf && ls.slot_for(0)->aaf.has_value());
    StreamSpec to_aaf = make_aaf_spec(1);
    EXPECT_FALSE(ls.reformat_stream(to_aaf).has_value());
    EXPECT_TRUE(ls.slot_for(1)->kind.load() == StreamKind::crf && ls.slot_for(1)->crf.has_value());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_listener_streams_test)

// IEEE 1722-2016 Clause 10.4: each received CRF PDU's timestamps reach the consumer with the
// PDU's mr bit, so the host sees a media clock restart as the bit toggles.
TEST(listener_crf, a_crf_pdu_delivers_its_mr_bit)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    std::atomic<uint64_t> gptp{0};
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    StreamSpec crf{};
    crf.index = 1;  // connected to kAafId
    crf.format_word = CRF_MILAN_48K;
    crf.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(ls.open_stream(crf).has_value());
    std::vector<std::pair<uint64_t, bool>> seen;
    ls.set_crf(1, [&seen](uint64_t const ts, uint16_t, int64_t, bool const mr) { seen.emplace_back(ts, mr); });

    avtp::CrfStreamOutputContext tx{kAafId, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, 1};
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> frame{};
    for (uint64_t k = 0; k < 3; ++k) {
        std::array<uint64_t, 1> const ts{9'000'000'000ULL + (k * 2'000'000)};
        EXPECT_EQ(serialize_crf_raw(tx, frame, ts, /*timestamp_uncertain=*/false, /*media_clock_restart=*/k > 0), frame.size());
        ls.on_stream_rx_frame(frame, 8'999'000'000);
    }
    EXPECT_EQ(seen.size(), size_t{3});
    if (seen.size() == 3) {
        EXPECT_EQ(seen[0].first, 9'000'000'000ULL);
        EXPECT_FALSE(seen[0].second);
        EXPECT_TRUE(seen[1].second && seen[2].second);
        EXPECT_EQ(seen[2].first, 9'004'000'000ULL);
    }
    EXPECT_EQ(ls.slot_for(1)->rx_packets.load(), uint64_t{3});
}

// The CRF consumer's source generation starts at 1 with the first PDU and steps when the input's
// PDUs come from another talker stream or a new ACMP connection, not when the talker's mr toggles.
TEST(listener_crf, a_rebind_steps_the_crf_source_generation)
{
    auto donor = make_connected_donor(kAm824Id, kAafId);
    std::atomic<uint64_t> gptp{0};
    ListenerStreams ls{1'000'000, donor->components(), gptp, nullptr};
    StreamSpec crf{};
    crf.index = 1;  // connected to kAafId
    crf.format_word = CRF_MILAN_48K;
    crf.format = decode_stream_format(CRF_MILAN_48K);
    EXPECT_TRUE(ls.open_stream(crf).has_value());
    std::vector<uint32_t> sources;
    ls.set_crf(1, [&sources](uint64_t, uint16_t, int64_t, bool, uint32_t const source) { sources.push_back(source); });

    auto const send = [&ls](tsn::StreamId const id, bool const mr) {
        avtp::CrfStreamOutputContext tx{id, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, 1};
        std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> frame{};
        std::array<uint64_t, 1> const ts{9'000'000'000ULL};
        EXPECT_EQ(serialize_crf_raw(tx, frame, ts, false, mr), frame.size());
        ls.on_stream_rx_frame(frame, 8'999'000'000);
    };
    tsn::StreamId const other{ieee::Eui48{0x00, 0x1B, 0x21, 0x0A, 0x0B, 0x0C}, 7};
    send(kAafId, false);
    send(kAafId, true);
    send(other, false);  // not connected: dropped
    connect_sink(*donor, 1, kAafId, 3, /*connect=*/false);
    connect_sink(*donor, 1, other, 4);
    send(kAafId, false);  // the old talker's: dropped
    send(other, false);
    send(other, false);
    connect_sink(*donor, 1, other, 5, /*connect=*/false);
    connect_sink(*donor, 1, kAafId, 6);
    send(kAafId, false);
    // The same talker stream connected again (on_listener_connected): a new source all the same.
    ls.on_listener_connected(1, donor->components().acmp_listener.get_stream(1)->stream_id, ieee::Eui48{});
    send(kAafId, false);
    EXPECT_TRUE(sources == (std::vector<uint32_t>{1, 1, 2, 2, 3, 4}));
}
