// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Pure (socket-free) tests for the CRF timestamp math extracted from
// TalkerStreams::transmit_crf. The regression under test (entity#4): CRF
// timestamps must track the LIVE media-clock position, not the media-clock
// anchor, so a stream whose gate opens seconds after start still conveys
// gPTP-now. transmit_crf() itself early-returns without a socket, but the
// timestamp math is now a pure function driven by a software MediaClockGenerator.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

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
