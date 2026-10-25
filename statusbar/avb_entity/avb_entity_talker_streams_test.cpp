// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) tests for the CRF timestamp math extracted from
// TalkerStreams::transmit_crf. The regression under test (entity#4): CRF
// timestamps must track the LIVE media-clock position, not the media-clock
// anchor, so a stream whose gate opens seconds after start still conveys
// gPTP-now. transmit_crf() itself early-returns without a socket, but the
// timestamp math is now a pure function driven by a software MediaClockGenerator.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avb_entity/avb_entity_test_stream_port.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

using namespace statusbar;
using statusbar::avb_entity::crf_aligned_base;
using statusbar::avb_entity::crf_sample_stride;
using statusbar::avb_entity::fill_crf_timestamps;

namespace {

// Milan-style CRF: 48 kHz base, 96 kHz audio -> each declared event spans 2
// audio samples; interval 96 -> stride 192 samples.
constexpr uint16_t kInterval = 96;
constexpr uint32_t kCrfBase = 48'000;
constexpr uint32_t kAudioRate = 96'000;  // 96 kHz audio

// Advance a fresh media clock by `ticks` packets of `nominal` samples at r=1.0
// and return the resulting live first_index. Anchors on the first advance.
auto advance_media_clock(ptpclient::MediaClockGenerator& mc, uint64_t t0_ns, uint32_t nominal, int ticks) -> uint64_t
{
    uint64_t const tick_ns = (static_cast<uint64_t>(nominal) * 1'000'000'000ULL) / kAudioRate;
    uint64_t first_index = 0;
    for (int k = 0; k < ticks; ++k) {
        auto const emit = mc.advance(t0_ns + (static_cast<uint64_t>(k) * tick_ns), 1.0, nominal);
        first_index = emit.first_index;
    }
    return first_index;
}

}  // namespace

TEST(crf_math, sample_stride)
{
    EXPECT_EQ(crf_sample_stride(kInterval, kCrfBase, kAudioRate), 192U);  // 96 * 96000 / 48000
    EXPECT_EQ(crf_sample_stride(1, 96'000, kAudioRate), 1U);
    EXPECT_EQ(crf_sample_stride(96, 0, kAudioRate), 0U);              // degenerate: base 0 -> 0, no div-by-zero
    EXPECT_EQ(crf_sample_stride(kInterval, kCrfBase, 48'000U), 96U);  // 48 kHz audio: 96 * 48000 / 48000
}

TEST(crf_math, aligned_base_floors_to_stride)
{
    EXPECT_EQ(crf_aligned_base(1000, 192), 960U);  // floor(1000/192)*192 = 5*192
    EXPECT_EQ(crf_aligned_base(960, 192), 960U);   // already aligned
    EXPECT_EQ(crf_aligned_base(1234, 0), 1234U);   // stride 0 -> passthrough
    // Successive PDUs advance first_index by a whole multiple of stride, so the
    // aligned base advances by exactly that multiple (contiguous, no gap).
    EXPECT_EQ(crf_aligned_base(960 + 192, 192) - crf_aligned_base(960, 192), 192U);
}

TEST(crf_math, fill_matches_media_clock_timestamps)
{
    ptpclient::MediaClockGenerator mc{};
    uint64_t const base = advance_media_clock(mc, /*t0_ns=*/1'000'000'000ULL, /*nominal=*/12, /*ticks=*/100);
    uint32_t const stride = crf_sample_stride(kInterval, kCrfBase, kAudioRate);
    uint64_t const aligned = crf_aligned_base(base, stride);

    constexpr uint16_t n_ts = 6;
    std::array<uint64_t, n_ts> timestamps{};
    fill_crf_timestamps(mc, aligned, stride, timestamps);

    for (uint16_t i = 0; i < n_ts; ++i) {
        // Each timestamp is exactly the media clock's presentation time for that
        // sample index -- evenly spaced by `stride` samples.
        EXPECT_EQ(timestamps[i], mc.timestamp_for(aligned + (static_cast<uint64_t>(i) * stride)));
    }
}

// The regression itself: after the clock has been running ~1 s, a CRF PDU based
// on the LIVE position carries a timestamp ~1 s ahead of one based on the anchor
// (index 0). Before the fix, transmit_crf always used an anchor-relative counter
// starting at 0, so a gate that opened late emitted permanently-stale timestamps.
TEST(crf_math, live_base_tracks_now_not_anchor)
{
    ptpclient::MediaClockGenerator mc{};
    // 8000 ticks * 12 samples / 96 kHz = 1.0 s of media time elapsed.
    uint64_t const base = advance_media_clock(mc, /*t0_ns=*/1'000'000'000ULL, /*nominal=*/12, /*ticks=*/8000);
    uint32_t const stride = crf_sample_stride(kInterval, kCrfBase, kAudioRate);

    uint64_t const ts_live = mc.timestamp_for(crf_aligned_base(base, stride));
    uint64_t const ts_anchor = mc.timestamp_for(0);  // the old, stale behavior

    EXPECT_TRUE(ts_live > ts_anchor);
    uint64_t const delta = ts_live - ts_anchor;
    // ~1 s elapsed; allow a few ms of slack for pacing quantization.
    EXPECT_TRUE(delta > 995'000'000ULL);
    EXPECT_TRUE(delta < 1'005'000'000ULL);
}

// The raw-bytes render seam (the media bridge's fast path): the payload is
// exactly what the render wrote, the header carries the presentation time and
// dimensions, and a refused render sends nothing and consumes no sequence number.
TEST(raw_aaf, render_fills_the_payload_and_the_header_carries_the_pts)
{
    avtp::AafStreamOutputContext ctx{
        avtp::StreamId{}, avtp::AafFormat::int_24bit, avtp::AafSampleRate::rate_48_khz, 8, 24, /*pres_offset=*/0};
    std::array<uint8_t, 512> frame{};
    uint64_t seen_index = 0;
    uint64_t seen_pts = 0;
    uint32_t seen_frames = 0;
    avb_entity::StreamRawRenderFn const render =
        [&](std::span<uint8_t> payload, uint32_t frames, uint64_t first_index, uint64_t pts_ns) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<uint8_t>(i);
            }
            seen_index = first_index;
            seen_pts = pts_ns;
            seen_frames = frames;
            return true;
        };

    uint64_t const pts = 0x0000'0012'3456'789AULL;
    size_t const len = avb_entity::serialize_aaf_raw(ctx, frame, 6, 4800, pts, render);
    EXPECT_EQ(len, avtp::AafPdu::HEADER_LENGTH + (6U * 8U * 3U));
    EXPECT_EQ(seen_index, uint64_t{4800});
    EXPECT_EQ(seen_pts, pts);
    EXPECT_EQ(seen_frames, uint32_t{6});

    auto const pdu = avtp::aaf_parse_header(std::span<uint8_t const>{frame.data(), len});
    EXPECT_TRUE(pdu.has_value());
    EXPECT_EQ(pdu->get_avtp_timestamp(), static_cast<uint32_t>(pts));
    EXPECT_TRUE(pdu->tv());
    EXPECT_EQ(pdu->sample_count(), uint16_t{6});
    EXPECT_EQ(pdu->get_sequence_num(), uint8_t{0});
    auto const payload = avtp::aaf_get_audio_payload(std::span<uint8_t const>{frame.data(), len});
    EXPECT_EQ(payload.size(), size_t{144});
    for (size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(payload[i], static_cast<uint8_t>(i));
    }
    EXPECT_EQ(ctx.sequence_num, uint8_t{1});

    // a refused packet: nothing built, the sequence number kept
    avb_entity::StreamRawRenderFn const refuse = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return false; };
    EXPECT_EQ(avb_entity::serialize_aaf_raw(ctx, frame, 6, 4806, pts + 125'000, refuse), size_t{0});
    EXPECT_EQ(ctx.sequence_num, uint8_t{1});
    // a frame buffer too small for the payload
    EXPECT_EQ(avb_entity::serialize_aaf_raw(ctx, std::span<uint8_t>{frame}.first(64), 6, 4806, pts, render), size_t{0});
    EXPECT_EQ(ctx.sequence_num, uint8_t{1});
}

// The raw path's pts is the wire presentation time: a context presentation
// offset is not added on top (the caller has applied its own).
TEST(raw_aaf, pts_is_the_wire_timestamp_whatever_the_context_offset)
{
    avtp::AafStreamOutputContext ctx{
        avtp::StreamId{}, avtp::AafFormat::int_16bit, avtp::AafSampleRate::rate_48_khz, 2, 16, /*pres_offset=*/2'000'000};
    std::array<uint8_t, 128> frame{};
    avb_entity::StreamRawRenderFn const render = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return true; };
    uint64_t const pts = 0x0000'0001'0000'1000ULL;
    size_t const len = avb_entity::serialize_aaf_raw(ctx, frame, 6, 0, pts, render);
    EXPECT_NE(len, size_t{0});
    auto const pdu = avtp::aaf_parse_header(std::span<uint8_t const>{frame.data(), len});
    EXPECT_TRUE(pdu.has_value());
    EXPECT_EQ(pdu->get_avtp_timestamp(), static_cast<uint32_t>(pts));
}

// The raw CRF serializer: the header carries the context's CRF parameters and sequence, the 64-bit
// timestamps land verbatim, and a wrong count is refused without touching the context.
TEST(raw_crf, serialize_writes_the_header_and_the_timestamps_verbatim)
{
    avtp::CrfStreamOutputContext ctx{avtp::StreamId{}, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, 2};
    ctx.sequence_num = 254;
    std::array<uint64_t, 2> const ts{0x0123'4567'89AB'CDEFULL, 0x0123'4567'89AC'0000ULL};
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + (2 * avtp::CrfPdu::TIMESTAMP_SIZE)> frame{};

    size_t const len = avb_entity::serialize_crf_raw(ctx, frame, ts);
    EXPECT_EQ(len, frame.size());
    auto const pdu = avtp::crf_parse_header(frame);
    EXPECT_TRUE(pdu.has_value());
    EXPECT_EQ(pdu->base_frequency(), uint32_t{48'000});
    EXPECT_EQ(pdu->pull(), uint8_t{0});
    EXPECT_EQ(pdu->timestamp_interval(), uint16_t{96});
    EXPECT_EQ(pdu->timestamp_count(), uint16_t{2});
    EXPECT_EQ(pdu->get_sequence_num(), uint8_t{254});
    auto const data = avtp::crf_get_timestamp_data(frame);
    EXPECT_EQ(avtp::crf_get_timestamp(data, 0).value_or(0), ts[0]);
    EXPECT_EQ(avtp::crf_get_timestamp(data, 1).value_or(0), ts[1]);
    EXPECT_EQ(ctx.sequence_num, uint8_t{255});
    EXPECT_EQ(ctx.packets_sent, uint32_t{1});
    EXPECT_EQ(ctx.total_timestamps_sent, uint64_t{2});

    // The sequence number wraps.
    EXPECT_EQ(avb_entity::serialize_crf_raw(ctx, frame, ts), frame.size());
    EXPECT_EQ(avtp::crf_parse_header(frame)->get_sequence_num(), uint8_t{255});
    EXPECT_EQ(ctx.sequence_num, uint8_t{0});

    // Refused: not the declared count, or a frame too small. The context stays.
    std::array<uint64_t, 1> const one{1};
    EXPECT_EQ(avb_entity::serialize_crf_raw(ctx, frame, one), size_t{0});
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH> small{};
    EXPECT_EQ(avb_entity::serialize_crf_raw(ctx, small, ts), size_t{0});
    EXPECT_EQ(ctx.sequence_num, uint8_t{0});
    EXPECT_EQ(ctx.packets_sent, uint32_t{2});
}

// open_stream caps the timestamps a CRF PDU may declare; with no TX socket
// transmit_crf_raw sends nothing and leaves the stream's state as it was,
// although the timestamps themselves would serialize.
TEST(raw_crf, transmit_without_a_socket_sends_nothing)
{
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3}, media_clock, last_gptp_ns, nullptr};
    avb_entity::StreamSpec too_many{};
    too_many.index = 2;
    too_many.format_word = 0x041060FF0000BB80ULL;  // 255 timestamps a PDU
    too_many.format = avb_entity::decode_stream_format(too_many.format_word);
    EXPECT_EQ(too_many.format.crf_timestamps_per_pdu, uint8_t{255});
    EXPECT_FALSE(talker.open_stream(too_many, tsn::StreamId{}, ieee::Eui48{}).has_value());

    avb_entity::StreamSpec crf{};
    crf.index = 1;
    crf.format_word = 0x041060010000BB80ULL;
    crf.format = avb_entity::decode_stream_format(crf.format_word);
    EXPECT_TRUE(talker.open_stream(crf, tsn::StreamId{}, ieee::Eui48{}).has_value());
    auto* slot = talker.slot_for(1);
    EXPECT_TRUE(slot != nullptr && slot->crf.has_value());
    std::array<uint64_t, 1> const one{1'000'000};
    // What the slot's context would serialize, on a copy:
    auto copy = *slot->crf;
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> frame{};
    EXPECT_EQ(avb_entity::serialize_crf_raw(copy, frame, one), frame.size());
    // ...but with no socket nothing is sent and nothing advances.
    EXPECT_FALSE(talker.transmit_crf_raw(*slot, one));
    EXPECT_EQ(slot->crf->sequence_num, uint8_t{0});
    EXPECT_EQ(slot->crf->packets_sent, uint32_t{0});
    EXPECT_EQ(slot->tx_packets, uint64_t{0});
    avb_entity::TalkerStreamSlot not_crf{};
    EXPECT_FALSE(talker.transmit_crf_raw(not_crf, one));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_talker_streams_test)

// Milan v1.2 Section 4.3.5.2 / AVTP 4.4.4.7: the raw serializers carry the tu
// bit they are given (TalkerStreams passes timestamp_uncertain()).
TEST(raw_tu, aaf_and_crf_carry_the_timestamp_uncertain_bit)
{
    avtp::AafStreamOutputContext aaf{
        avtp::StreamId{}, avtp::AafFormat::int_32bit, avtp::AafSampleRate::rate_48_khz, 2, 32, /*pres_offset=*/0};
    std::array<uint8_t, 128> frame{};
    avb_entity::StreamRawRenderFn const render = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return true; };
    size_t len = avb_entity::serialize_aaf_raw(aaf, frame, 6, 0, 1'000, render);
    EXPECT_FALSE(avtp::aaf_parse_header(std::span<uint8_t const>{frame.data(), len})->tu());
    len = avb_entity::serialize_aaf_raw(aaf, frame, 6, 6, 126'000, render, /*timestamp_uncertain=*/true);
    EXPECT_TRUE(avtp::aaf_parse_header(std::span<uint8_t const>{frame.data(), len})->tu());

    avtp::CrfStreamOutputContext crf{avtp::StreamId{}, avtp::CrfType::audio_sample, 48'000, avtp::CrfPull::multiply_1_0, 96, 1};
    std::array<uint64_t, 1> const ts{0x0123'4567'89AB'CDEFULL};
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> crf_frame{};
    EXPECT_EQ(avb_entity::serialize_crf_raw(crf, crf_frame, ts), crf_frame.size());
    EXPECT_FALSE(avtp::crf_parse_header(crf_frame)->tu());
    EXPECT_EQ(avb_entity::serialize_crf_raw(crf, crf_frame, ts, /*timestamp_uncertain=*/true), crf_frame.size());
    EXPECT_TRUE(avtp::crf_parse_header(crf_frame)->tu());
}

// Milan v1.2 Annex B.1.1: "In case of a change of grandmaster, the tu bit
// shall be set to 1 for the duration of 0.25 seconds"; and while no
// grandmaster is known at all.
TEST(talker_tu, set_for_a_quarter_second_after_a_grandmaster_change_and_while_lost)
{
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<uint64_t> last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3}, media_clock, last_gptp_ns, nullptr};
    constexpr int64_t t0 = 5'000'000'000;
    EXPECT_FALSE(talker.timestamp_uncertain(t0));  // never told: certain
    talker.note_grandmaster_changed(t0);
    EXPECT_TRUE(talker.timestamp_uncertain(t0));
    EXPECT_TRUE(talker.timestamp_uncertain(t0 + 249'999'999));
    EXPECT_FALSE(talker.timestamp_uncertain(t0 + 250'000'000));
    talker.note_grandmaster_lost();
    EXPECT_TRUE(talker.timestamp_uncertain(t0 + 10'000'000'000));
    talker.note_grandmaster_changed(t0 + 20'000'000'000);  // a grandmaster again
    EXPECT_TRUE(talker.timestamp_uncertain(t0 + 20'100'000'000));
    EXPECT_FALSE(talker.timestamp_uncertain(t0 + 20'300'000'000));
}

// GET_COUNTERS' transmit totals (Milan v1.2 Table 5.4): every PDU handed to
// the socket, those with tu set (octet 3 of an AAF PDU, octet 1 of a CRF
// one), and each mr toggle, per STREAM_OUTPUT.
TEST(talker_tx_counters, pdus_tu_and_mr_toggles)
{
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<uint64_t> last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3}, media_clock, last_gptp_ns, nullptr};
    avb_entity::TalkerStreamSlot slot{};
    slot.spec.index = 3;
    std::array<uint8_t, 24> aaf{avtp::AvtpSubtype::aaf, 0x81, 0x00, 0x00};
    talker.send_stream_frame(slot, aaf);
    aaf[3] = 0x01;  // tu
    talker.send_stream_frame(slot, aaf);
    aaf[1] = 0x89;  // mr toggled
    talker.send_stream_frame(slot, aaf);
    talker.send_stream_frame(slot, aaf);
    auto const* c = talker.tx_counters(3);
    EXPECT_TRUE(c != nullptr);
    EXPECT_EQ(c->pdus.load(), 4U);
    EXPECT_EQ(c->tu_pdus.load(), 3U);
    EXPECT_EQ(c->mr_toggles.load(), 1U);

    avb_entity::TalkerStreamSlot crf_slot{};
    crf_slot.spec.index = 4;
    std::array<uint8_t, 24> crf{avtp::AvtpSubtype::crf, 0x81, 0x00, 0x00};  // tu in octet 1
    talker.send_stream_frame(crf_slot, crf);
    EXPECT_EQ(talker.tx_counters(4)->tu_pdus.load(), 1U);
    EXPECT_TRUE(talker.tx_counters(static_cast<uint16_t>(avb_entity::MAX_ENTITY_STREAMS)) == nullptr);
}

//
// The whole transmit path over a TestStreamPort
//

namespace {

ieee::Eui48 const kPortMac{0x00, 0x1B, 0x21, 0x0A, 0x0B, 0x0C};
ieee::Eui48 const kDest{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
tsn::StreamId const kStreamId{ieee::Eui48{0x00, 0x1B, 0x21, 0x0A, 0x0B, 0x0C}, 7};

/// The frame RawnetContext::send_vlan puts on the wire for @p payload: dest,
/// source, TPID 0x8100, TCI pcp<<13 | vid, ethertype 0x22F0, payload.
auto vlan_frame(std::span<uint8_t const> const payload, uint16_t const vid, uint8_t const pcp) -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame{kDest.value.begin(), kDest.value.end()};
    frame.insert(frame.end(), kPortMac.value.begin(), kPortMac.value.end());
    auto const tci = static_cast<uint16_t>((pcp << 13U) | vid);
    for (uint8_t const b :
         {uint8_t{0x81}, uint8_t{0x00}, static_cast<uint8_t>(tci >> 8U), static_cast<uint8_t>(tci), uint8_t{0x22}, uint8_t{0xF0}}) {
        frame.push_back(b);
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

auto spec_of(uint16_t const index, uint64_t const format_word) -> avb_entity::StreamSpec
{
    avb_entity::StreamSpec spec{};
    spec.index = index;
    spec.format_word = format_word;
    spec.format = avb_entity::decode_stream_format(format_word);
    return spec;
}

auto same_bytes(std::span<uint8_t const> const a, std::span<uint8_t const> const b) -> bool
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

// An AAF frame committed on the port is byte-for-byte the frame the AF_PACKET
// path sends: the tagged header with the talker's VID and PCP in front of the
// AVTPDU serialize_aaf_raw builds; a VID change retags the next frame.
TEST(talker_port, aaf_frame_is_the_tagged_frame_send_vlan_builds)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 96'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    auto const spec = spec_of(0, 0x020702200200C000ULL);  // AAF 96 kHz INT_32 8 ch
    EXPECT_TRUE(talker.open_stream(spec, kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* slot = talker.slot_for(0);
    EXPECT_TRUE(slot != nullptr && slot->aaf.has_value());

    avb_entity::StreamRawRenderFn const render = [](std::span<uint8_t> payload, uint32_t, uint64_t first_index, uint64_t) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>(first_index + i);
        }
        return true;
    };
    auto twin = *slot->aaf;
    uint64_t const pts = 0x0000'0002'0000'1000ULL;
    EXPECT_TRUE(talker.transmit_aaf_raw(*slot, 1200, pts, 12, render));
    std::array<uint8_t, 512> expected{};
    size_t const len = avb_entity::serialize_aaf_raw(twin, expected, 12, 1200, pts, render);
    EXPECT_EQ(len, avtp::AafPdu::HEADER_LENGTH + (12U * 8U * 4U));
    auto const first = port.pop_tx();
    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(same_bytes(first->frame, vlan_frame(std::span{expected}.first(len), 2, 3)));

    talker.set_vlan_id(0x123);
    EXPECT_TRUE(talker.transmit_aaf_raw(*slot, 1212, pts + 125'000, 12, render));
    size_t const len2 = avb_entity::serialize_aaf_raw(twin, expected, 12, 1212, pts + 125'000, render);
    auto const second = port.pop_tx();
    EXPECT_TRUE(second.has_value());
    EXPECT_TRUE(same_bytes(second->frame, vlan_frame(std::span{expected}.first(len2), 0x123, 3)));
    EXPECT_EQ(slot->tx_packets, uint64_t{2});
    EXPECT_EQ(port.stats().tx_frames, uint64_t{2});
}

// A CRF PDU committed on the port is likewise the tagged frame around what
// serialize_crf_raw builds.
TEST(talker_port, crf_frame_is_the_tagged_frame_send_vlan_builds)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3}, media_clock, last_gptp_ns, nullptr};
    EXPECT_TRUE(talker.open_stream(spec_of(1, 0x041060010000BB80ULL), kStreamId, kDest).has_value());  // Milan CRF
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* slot = talker.slot_for(1);
    EXPECT_TRUE(slot != nullptr && slot->crf.has_value());

    auto twin = *slot->crf;
    std::array<uint64_t, 1> const ts{0x0123'4567'89AB'CDEFULL};
    EXPECT_TRUE(talker.transmit_crf_raw(*slot, ts));
    std::array<uint8_t, avtp::CrfPdu::HEADER_LENGTH + avtp::CrfPdu::TIMESTAMP_SIZE> expected{};
    EXPECT_EQ(avb_entity::serialize_crf_raw(twin, expected, ts), expected.size());
    auto const sent = port.pop_tx();
    EXPECT_TRUE(sent.has_value());
    EXPECT_TRUE(same_bytes(sent->frame, vlan_frame(expected, 2, 3)));
    EXPECT_EQ(sent->frame.size(), avb_entity::StreamPort::TX_PAYLOAD_OFFSET + expected.size());
}

// The float AM824 and AAF paths serialize into the port's window too: the
// frame carries the tagged header and their AVTPDU.
TEST(talker_port, float_paths_serialize_into_the_port_window)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 96'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    EXPECT_TRUE(talker.open_stream(spec_of(0, 0x020702200200C000ULL), kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.open_tx(port).has_value());
    std::vector<float> const src(12U * 8U, 0.5F);

    talker.transmit_aaf(*talker.slot_for(0), 1'000'000, 12, src);
    auto const aaf = port.pop_tx();
    EXPECT_TRUE(aaf.has_value());
    EXPECT_EQ(aaf->frame.size(), avb_entity::StreamPort::TX_PAYLOAD_OFFSET + avtp::AafPdu::HEADER_LENGTH + (12U * 8U * 4U));
    std::span<uint8_t const> const frame = aaf->frame;
    auto const header = vlan_frame({}, 2, 3);
    EXPECT_TRUE(same_bytes(frame.first(18), header));
    EXPECT_EQ(frame[18], avtp::AvtpSubtype::aaf);
}

// A transmit that sends nothing leaves no frame on the port, and flush()
// reaches the port once a call.
TEST(talker_port, a_refused_render_commits_nothing_and_flush_reaches_the_port)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 96'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    EXPECT_TRUE(talker.flush().has_value());  // no port yet: nothing to flush
    EXPECT_TRUE(talker.open_stream(spec_of(0, 0x020702200200C000ULL), kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* slot = talker.slot_for(0);
    avb_entity::StreamRawRenderFn const refuse = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return false; };
    EXPECT_FALSE(talker.transmit_aaf_raw(*slot, 0, 1'000'000, 12, refuse));
    EXPECT_EQ(port.tx_pending(), size_t{0});
    EXPECT_EQ(slot->aaf->sequence_num, uint8_t{0});
    EXPECT_TRUE(talker.flush().has_value());
    EXPECT_TRUE(talker.flush().has_value());
    EXPECT_EQ(port.flushes(), size_t{2});
}

// A frame the port refuses (a full ring) consumes no sequence number and is
// not counted; the next accepted frame carries the number it would have.
TEST(talker_port, a_refused_commit_consumes_no_sequence_number)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 96'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    EXPECT_TRUE(talker.open_stream(spec_of(0, 0x020702200200C000ULL), kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.open_stream(spec_of(1, 0x041060010000BB80ULL), kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* aaf = talker.slot_for(0);
    auto* crf = talker.slot_for(1);
    avb_entity::StreamRawRenderFn const render = [](std::span<uint8_t> payload, uint32_t, uint64_t, uint64_t) {
        std::ranges::fill(payload, uint8_t{0x5A});
        return true;
    };
    std::array<uint64_t, 1> const ts{1'000'000};

    port.refuse_commits(true);
    EXPECT_FALSE(talker.transmit_aaf_raw(*aaf, 0, 1'000'000, 12, render));
    EXPECT_FALSE(talker.transmit_crf_raw(*crf, ts));
    EXPECT_EQ(aaf->aaf->sequence_num, uint8_t{0});
    EXPECT_EQ(crf->crf->sequence_num, uint8_t{0});
    EXPECT_EQ(aaf->tx_packets, uint64_t{0});
    EXPECT_EQ(talker.tx_counters(0)->pdus.load(), 0U);
    EXPECT_EQ(port.stats().tx_errors, uint64_t{2});
    EXPECT_EQ(port.tx_pending(), size_t{0});

    port.refuse_commits(false);
    EXPECT_TRUE(talker.transmit_aaf_raw(*aaf, 0, 1'000'000, 12, render));
    EXPECT_TRUE(talker.transmit_crf_raw(*crf, ts));
    auto const sent_aaf = port.pop_tx();
    auto const sent_crf = port.pop_tx();
    EXPECT_TRUE(sent_aaf.has_value() && sent_crf.has_value());
    if (sent_aaf && sent_crf) {
        EXPECT_EQ(sent_aaf->frame[18 + 2], uint8_t{0});  // AAF sequence_num
        EXPECT_EQ(sent_crf->frame[18 + 2], uint8_t{0});  // CRF sequence_num
    }
    EXPECT_EQ(aaf->aaf->sequence_num, uint8_t{1});
    EXPECT_EQ(aaf->tx_packets, uint64_t{1});
}

//
// Raw AM824 (IEC 61883-6, IEEE 1722-2016 Clause 5)
//

namespace {

/// A render writing MBLA quadlets 0x40 | (index of the quadlet in the stream) for each block and channel.
auto am824_render(uint8_t const channels) -> avb_entity::StreamRawRenderFn
{
    return [channels](std::span<uint8_t> payload, uint32_t frames, uint64_t first_index, uint64_t) {
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint8_t ch = 0; ch < channels; ++ch) {
                size_t const at = ((size_t{f} * channels) + ch) * 4U;
                uint32_t const sample = static_cast<uint32_t>(((first_index + f) * channels) + ch) & 0x00FF'FFFFU;
                payload[at] = avtp::AM824_LABEL_MBLA;
                payload[at + 1] = static_cast<uint8_t>(sample >> 16U);
                payload[at + 2] = static_cast<uint8_t>(sample >> 8U);
                payload[at + 3] = static_cast<uint8_t>(sample);
            }
        }
        return true;
    };
}

struct Am824Cadence
{
    avtp::Am824SampleRate rate;
    uint16_t frames;
};
constexpr std::array<Am824Cadence, 3> kAm824Cadences{
    Am824Cadence{avtp::Am824SampleRate::rate_48_khz, 6},
    Am824Cadence{avtp::Am824SampleRate::rate_96_khz, 12},
    Am824Cadence{avtp::Am824SampleRate::rate_192_khz, 24}};

}  // namespace

// One class A packet at 48, 96 and 192 kHz: 6, 12 and 24 blocks of DBS quadlets, the payload as
// rendered, the FDF the stream's rate, and the DBC the low byte of the first sample index.
TEST(raw_am824, one_packet_per_cadence_at_each_rate)
{
    for (auto const& c : kAm824Cadences) {
        for (uint8_t const channels : {uint8_t{1}, uint8_t{2}, uint8_t{8}}) {
            avtp::Am824StreamOutputContext ctx{kStreamId, c.rate, channels, /*pres_offset=*/0};
            std::array<uint8_t, 1500> frame{};
            uint64_t const first = 1'000'003;
            size_t const len =
                avb_entity::serialize_am824_raw(ctx, frame, c.frames, first, 5'000'000'000ULL, am824_render(channels));
            size_t const payload_len = size_t{c.frames} * channels * 4U;
            EXPECT_EQ(len, avtp::Am824Pdu::HEADER_LENGTH + payload_len);
            auto const pdu = avtp::am824_parse_header(std::span<uint8_t const>{frame.data(), len});
            EXPECT_TRUE(pdu.has_value());
            if (!pdu) {
                continue;
            }
            EXPECT_TRUE(pdu->stream_id() == kStreamId);
            EXPECT_EQ(pdu->channel_count(), channels);
            EXPECT_EQ(pdu->sample_count(), static_cast<uint8_t>(c.frames));
            EXPECT_TRUE(pdu->sample_rate() == c.rate);
            EXPECT_EQ(pdu->cip_header.format_dependent_field(), static_cast<uint8_t>(c.rate));
            EXPECT_EQ(pdu->data_block_count(), static_cast<uint8_t>(first & 0xFFU));
            EXPECT_EQ(pdu->stream_data_length(), static_cast<uint16_t>(avtp::Cip61883Header::LENGTH + payload_len));
            std::vector<uint8_t> expected(payload_len);
            EXPECT_TRUE(am824_render(channels)(expected, c.frames, first, 0));
            EXPECT_TRUE(same_bytes(std::span<uint8_t const>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH, payload_len), expected));
            EXPECT_EQ(ctx.sequence_num, uint8_t{1});
        }
    }
}

// Packet after packet the DBC advances by the block count (mod 256) and the sequence by one, from any
// starting index: the DBC is the sample index, so a restart lands on the same count.
TEST(raw_am824, dbc_is_continuous_across_packets)
{
    for (auto const& c : kAm824Cadences) {
        avtp::Am824StreamOutputContext ctx{kStreamId, c.rate, 2, 0};
        std::array<uint8_t, 512> frame{};
        uint64_t const first = 777;
        uint8_t previous = 0;
        for (uint64_t k = 0; k < 64; ++k) {
            uint64_t const index = first + (k * c.frames);
            EXPECT_NE(
                avb_entity::serialize_am824_raw(ctx, frame, c.frames, index, 1'000'000'000ULL + (k * 125'000), am824_render(2)),
                size_t{0});
            auto const pdu = avtp::am824_parse_header(frame);
            EXPECT_TRUE(pdu.has_value());
            EXPECT_EQ(pdu->data_block_count(), static_cast<uint8_t>(index & 0xFFU));
            EXPECT_EQ(pdu->sequence_num(), static_cast<uint8_t>(k));
            if (k > 0) {
                EXPECT_EQ(pdu->data_block_count(), static_cast<uint8_t>(previous + c.frames));
            }
            previous = pdu->data_block_count();
        }
    }
}

// tv, the AVTP timestamp and the SYT mark the block whose DBC is a multiple of SYT_INTERVAL (8, 16, 32):
// 3 packets in 4 at each class A cadence; the timestamp is that block's time (the context's integer
// sample period on from pts), the SYT the same time.
TEST(raw_am824, syt_marks_the_syt_interval_block_three_packets_in_four)
{
    for (auto const& c : kAm824Cadences) {
        uint32_t const rate_hz = avtp::am824_sample_rate_hz(c.rate);
        uint64_t const interval = avtp::am824_syt_interval(c.rate);
        avtp::Am824StreamOutputContext ctx{kStreamId, c.rate, 2, /*pres_offset=*/3'000'000};
        std::array<uint8_t, 512> frame{};
        int timed = 0;
        for (uint64_t k = 0; k < 16; ++k) {
            uint64_t const index = 480'000 + (k * c.frames);
            uint64_t const pts = 10'000'000'000ULL + ((index - 480'000) * 1'000'000'000ULL / rate_hz);
            EXPECT_NE(avb_entity::serialize_am824_raw(ctx, frame, c.frames, index, pts, am824_render(2)), size_t{0});
            auto const pdu = avtp::am824_parse_header(frame);
            EXPECT_TRUE(pdu.has_value());
            uint64_t const block = (interval - (index % interval)) % interval;
            bool const expect_tv = block < c.frames;
            EXPECT_EQ(pdu->tv(), expect_tv);
            EXPECT_EQ(pdu->tv(), (k % 4) != 3);
            if (expect_tv) {
                ++timed;
                uint64_t const ts = pts + (block * ctx.config.sample_period_ns);
                EXPECT_EQ(pdu->avtp_timestamp(), static_cast<uint32_t>(ts));
                EXPECT_EQ(pdu->syt_timestamp(), avtp::am824_presentation_to_syt(ts));
                EXPECT_EQ(static_cast<uint8_t>(pdu->data_block_count() + block) % interval, uint64_t{0});
            } else {
                EXPECT_EQ(pdu->avtp_timestamp(), uint32_t{0});
                EXPECT_EQ(pdu->syt_timestamp(), uint16_t{0xFFFF});
            }
        }
        EXPECT_EQ(timed, 12);
        EXPECT_EQ(ctx.timestamp_inserts, uint32_t{12});
    }
    // At 48 kHz from index 0 the timestamps fall on blocks 0, 2 and 4 of the first three packets.
    avtp::Am824StreamOutputContext ctx{kStreamId, avtp::Am824SampleRate::rate_48_khz, 2, 0};
    std::array<uint8_t, 512> frame{};
    std::array<uint32_t, 3> const offsets{0, 2 * 20'833, 4 * 20'833};
    for (uint64_t k = 0; k < 3; ++k) {
        uint64_t const pts = 2'000'000'000ULL + (k * 125'000);
        EXPECT_NE(avb_entity::serialize_am824_raw(ctx, frame, 6, k * 6, pts, am824_render(2)), size_t{0});
        EXPECT_EQ(avtp::am824_parse_header(frame)->avtp_timestamp(), static_cast<uint32_t>(pts + offsets[k]));
    }
}

// A refused render, a short frame or more than 32 blocks builds nothing and keeps the sequence; tu is
// carried as given.
TEST(raw_am824, refusals_keep_the_context_and_tu_is_carried)
{
    avtp::Am824StreamOutputContext ctx{kStreamId, avtp::Am824SampleRate::rate_48_khz, 8, 0};
    std::array<uint8_t, 1500> frame{};
    avb_entity::StreamRawRenderFn const refuse = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return false; };
    EXPECT_EQ(avb_entity::serialize_am824_raw(ctx, frame, 6, 0, 1'000, refuse), size_t{0});
    EXPECT_EQ(avb_entity::serialize_am824_raw(ctx, std::span<uint8_t>{frame}.first(64), 6, 0, 1'000, am824_render(8)), size_t{0});
    EXPECT_EQ(avb_entity::serialize_am824_raw(ctx, frame, 33, 0, 1'000, am824_render(8)), size_t{0});
    EXPECT_EQ(avb_entity::serialize_am824_raw(ctx, frame, 0, 0, 1'000, am824_render(8)), size_t{0});
    EXPECT_EQ(ctx.sequence_num, uint8_t{0});
    EXPECT_EQ(ctx.packets_sent, uint32_t{0});
    EXPECT_NE(avb_entity::serialize_am824_raw(ctx, frame, 6, 0, 1'000, am824_render(8), /*timestamp_uncertain=*/true), size_t{0});
    EXPECT_TRUE(avtp::am824_parse_header(frame)->stream_header.tu());
    EXPECT_NE(avb_entity::serialize_am824_raw(ctx, frame, 6, 6, 126'000, am824_render(8)), size_t{0});
    EXPECT_FALSE(avtp::am824_parse_header(frame)->stream_header.tu());
}

// A 96 kHz AM824 stream under a 48 kHz media clock (reformatted from 48 kHz) is labelled 96 kHz on
// the wire by both the raw and the float paths; the raw frame is the tagged frame around what
// serialize_am824_raw builds, and a refused commit consumes no sequence number.
TEST(talker_port, am824_frames_carry_the_streams_own_rate)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    EXPECT_TRUE(talker.open_stream(spec_of(0, avtp::make_am824_stream_format(2, 2)), kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.reformat_stream(spec_of(0, avtp::make_am824_stream_format(4, 2))).has_value());
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* slot = talker.slot_for(0);
    EXPECT_TRUE(slot != nullptr && slot->am824.has_value());

    auto twin = *slot->am824;
    uint64_t const pts = 0x0000'0002'0000'1000ULL;
    EXPECT_TRUE(talker.transmit_am824_raw(*slot, 9600, pts, 12, am824_render(2)));
    std::array<uint8_t, 512> expected{};
    size_t const len = avb_entity::serialize_am824_raw(twin, expected, 12, 9600, pts, am824_render(2));
    EXPECT_EQ(len, avtp::Am824Pdu::HEADER_LENGTH + (12U * 2U * 4U));
    auto const raw = port.pop_tx();
    EXPECT_TRUE(raw.has_value());
    EXPECT_TRUE(same_bytes(raw->frame, vlan_frame(std::span{expected}.first(len), 2, 3)));
    auto const raw_pdu = avtp::am824_parse_header(std::span<uint8_t const>{raw->frame}.subspan(18));
    EXPECT_TRUE(raw_pdu.has_value() && raw_pdu->sample_rate() == avtp::Am824SampleRate::rate_96_khz);

    std::vector<float> const src(12U * 2U, 0.25F);
    talker.transmit_am824(*slot, 1'000'000, 12, src);
    auto const decoded = port.pop_tx();
    EXPECT_TRUE(decoded.has_value());
    auto const float_pdu = avtp::am824_parse_header(std::span<uint8_t const>{decoded->frame}.subspan(18));
    EXPECT_TRUE(float_pdu.has_value() && float_pdu->sample_rate() == avtp::Am824SampleRate::rate_96_khz);

    port.refuse_commits(true);
    uint8_t const seq = slot->am824->sequence_num;
    EXPECT_FALSE(talker.transmit_am824_raw(*slot, 9612, pts + 125'000, 12, am824_render(2)));
    EXPECT_EQ(slot->am824->sequence_num, seq);
    port.refuse_commits(false);

    // not an AM824 slot: nothing sent
    EXPECT_TRUE(talker.open_stream(spec_of(1, 0x020702200200C000ULL), kStreamId, kDest).has_value());
    EXPECT_FALSE(talker.transmit_am824_raw(*talker.slot_for(1), 0, pts, 12, am824_render(2)));
    EXPECT_EQ(port.tx_pending(), size_t{0});
}

// A stream re-shaped from AAF to AM824 and back (reformat_stream) sends the new kind's PDUs, its
// sequence numbers going on across each change; the AAF reframer goes and comes with the kind.
TEST(talker_port, a_kind_change_sends_the_new_kind_with_continuing_sequence_numbers)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    constexpr uint64_t AAF_48K_INT32_2CH = 0x0205022000806000ULL;
    EXPECT_TRUE(talker.open_stream(spec_of(0, AAF_48K_INT32_2CH), kStreamId, kDest).has_value());
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* const slot = talker.slot_for(0);
    avb_entity::StreamRawRenderFn const render = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return true; };
    std::vector<std::pair<uint8_t, uint8_t>> sent;  // (subtype, sequence_num)
    auto const pop = [&port, &sent] {
        while (auto const frame = port.pop_tx()) {
            sent.emplace_back(frame->frame[18], frame->frame[18 + 2]);
        }
    };
    uint64_t index = 0;
    for (int i = 0; i < 3; ++i, index += 6) {
        EXPECT_TRUE(talker.transmit_aaf_raw(*slot, index, 1'000'000 + index, 6, render));
    }
    EXPECT_TRUE(talker.reformat_stream(spec_of(0, avtp::make_am824_stream_format(2, 2))).has_value());
    EXPECT_TRUE(slot->am824.has_value() && !slot->aaf.has_value() && !slot->reframer.has_value());
    EXPECT_FALSE(talker.transmit_aaf_raw(*slot, index, 1'000'000 + index, 6, render));
    for (int i = 0; i < 2; ++i, index += 6) {
        EXPECT_TRUE(talker.transmit_am824_raw(*slot, index, 1'000'000 + index, 6, am824_render(2)));
    }
    EXPECT_TRUE(talker.reformat_stream(spec_of(0, AAF_48K_INT32_2CH)).has_value());
    EXPECT_TRUE(slot->aaf.has_value() && !slot->am824.has_value() && slot->reframer.has_value());
    EXPECT_FALSE(talker.transmit_am824_raw(*slot, index, 1'000'000 + index, 6, am824_render(2)));
    EXPECT_TRUE(talker.transmit_aaf_raw(*slot, index, 1'000'000 + index, 6, render));
    pop();
    EXPECT_EQ(sent.size(), size_t{6});
    for (size_t i = 0; i < sent.size(); ++i) {
        bool const am824 = i == 3 || i == 4;
        EXPECT_EQ(sent[i].first, am824 ? avtp::AvtpSubtype::iec_61883_iidc : avtp::AvtpSubtype::aaf);
        EXPECT_EQ(sent[i].second, static_cast<uint8_t>(i));
    }
    auto const crf = spec_of(0, 0x041060010000BB80ULL);
    EXPECT_FALSE(talker.reformat_stream(crf).has_value());  // never to CRF
    EXPECT_TRUE(slot->aaf.has_value());
}

// IEEE 1722-2016 Clause 10.4: a media clock restart toggles mr on the next CRF PDU and the bit
// stays toggled; two requests before a PDU toggle it once; an AAF slot has no restart to toggle.
TEST(talker_crf, a_toggled_restart_sets_mr_on_the_next_pdu)
{
    avb_entity::TestStreamPort port{kPortMac};
    ptpclient::MediaClockGenerator const media_clock{};
    std::atomic<uint64_t> const last_gptp_ns{0};
    avb_entity::TalkerStreams talker{
        avb_entity::TalkerStreamsConfig{.sample_rate = 48'000, .vlan_id = 2, .stream_pcp = 3},
        media_clock,
        last_gptp_ns,
        std::pmr::get_default_resource()};
    EXPECT_TRUE(talker.open_stream(spec_of(0, 0x0205022000806000ULL), kStreamId, kDest).has_value());  // AAF
    EXPECT_TRUE(talker.open_stream(spec_of(1, 0x041060010000BB80ULL), kStreamId, kDest).has_value());  // Milan CRF
    EXPECT_TRUE(talker.open_tx(port).has_value());
    auto* slot = talker.slot_for(1);
    EXPECT_FALSE(talker.toggle_media_clock_restart(0).has_value());
    EXPECT_FALSE(talker.toggle_media_clock_restart(5).has_value());

    std::array<uint64_t, 1> const ts{1'000'000};
    std::vector<bool> mr;
    auto const send = [&] {
        EXPECT_TRUE(talker.transmit_crf_raw(*slot, ts));
        auto const sent = port.pop_tx();
        EXPECT_TRUE(sent.has_value());
        auto const pdu =
            avtp::crf_parse_header(std::span<uint8_t const>{sent->frame}.subspan(avb_entity::StreamPort::TX_PAYLOAD_OFFSET));
        mr.push_back(pdu.has_value() && pdu->mr());
    };
    send();
    EXPECT_TRUE(talker.toggle_media_clock_restart(1).has_value());
    send();
    send();
    EXPECT_TRUE(talker.toggle_media_clock_restart(1).has_value());
    EXPECT_TRUE(talker.toggle_media_clock_restart(1).has_value());
    send();
    send();
    EXPECT_TRUE(mr == (std::vector<bool>{false, true, true, false, false}));
    EXPECT_EQ(talker.tx_counters(1)->mr_toggles.load(), uint32_t{2});
}
