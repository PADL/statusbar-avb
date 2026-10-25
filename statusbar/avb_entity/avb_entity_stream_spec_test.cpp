// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// StreamSpec tests — the blob-driven stream topology layer (Entity
// Construction Kit phase 1). Structured format decode is checked against the
// wire-verified format words the shipped entities use; the spec derivation is
// checked against the committed testdata blobs; the TSpec math is checked
// against the MSRP frame sizes real bridges registered for these streams
// (AAF 8ch/96k/int32 -> 440 bytes, Milan CRF -> 28 bytes).

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

// The three format words the shipped entities put on the wire (see
// avtp_stream_format.hpp's decoder docs; AAF/CRF byte-verified against
// reference-device captures).
constexpr uint64_t AAF_96K_INT32_8CH = 0x020702200200C000ULL;  // AAF 96k INT_32 8ch 12 samples/frame
constexpr uint64_t AM824_48K_8CH = 0x00A0020840000800ULL;      // AM824 48k 8ch
constexpr uint64_t CRF_MILAN_48K = 0x041060010000BB80ULL;      // CRF AUDIO_SAMPLE 48k interval=96 1 ts/pdu

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

auto testdata(char const* name) -> std::vector<uint8_t>
{
    return load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / name);
}

auto storage_for(std::vector<uint8_t> const& blob) -> atdecc::aem::DescriptorStorage
{
    auto storage = atdecc::aem::DescriptorStorage::create(blob);
    EXPECT_TRUE(storage.has_value());
    return *storage;
}

TEST(stream_spec_decode, aaf_fields)
{
    constexpr auto f = decode_stream_format(AAF_96K_INT32_8CH);
    static_assert(f.kind == StreamKind::aaf);
    EXPECT_EQ(f.sample_rate_hz, 96'000U);
    EXPECT_EQ(f.channels, 8);
    EXPECT_EQ(f.aaf_format, avtp::AafFormat::int_32bit);
    EXPECT_EQ(f.bit_depth, 32);
    EXPECT_EQ(f.aaf_samples_per_frame, 12);
}

TEST(stream_spec_decode, am824_fields)
{
    constexpr auto f = decode_stream_format(AM824_48K_8CH);
    static_assert(f.kind == StreamKind::am824);
    EXPECT_EQ(f.sample_rate_hz, 48'000U);
    EXPECT_EQ(f.channels, 8);
    EXPECT_EQ(f.bit_depth, 24);
}

TEST(stream_spec_decode, crf_fields)
{
    constexpr auto f = decode_stream_format(CRF_MILAN_48K);
    static_assert(f.kind == StreamKind::crf);
    EXPECT_EQ(f.crf_base_frequency_hz, 48'000U);
    EXPECT_EQ(f.crf_timestamp_interval, 96);
    EXPECT_EQ(f.crf_timestamps_per_pdu, 1);
    EXPECT_EQ(f.crf_pull, 0);
    EXPECT_EQ(f.crf_type, 1);  // AUDIO_SAMPLE
}

TEST(stream_spec_decode, non_am824_61883_is_other)
{
    // 61883 subtype but not sf=1/fmt=0x10 (e.g. 61883-4 MPEG-TS): must not be
    // mislabeled as AM824 audio.
    constexpr auto f = decode_stream_format(0x00C0000000000000ULL);
    static_assert(f.kind == StreamKind::other);
    EXPECT_EQ(f.channels, 0);
}

TEST(stream_spec_decode, unknown_subtype_is_other)
{
    EXPECT_EQ(decode_stream_format(0xFF00000000000000ULL).kind, StreamKind::other);
    EXPECT_EQ(decode_stream_format(0).kind, StreamKind::other);
}

TEST(stream_spec_derive, tone_aaf_crf_blob)
{
    auto const blob = testdata("entity_tone_aaf_crf.bin");
    EXPECT_FALSE(blob.empty());
    auto const storage = storage_for(blob);

    auto const talkers = talker_stream_specs(storage, 0);
    EXPECT_TRUE(talkers.has_value());
    EXPECT_EQ(talkers->size(), 2U);
    EXPECT_EQ((*talkers)[0].index, 0);
    EXPECT_EQ((*talkers)[0].format.kind, StreamKind::aaf);
    EXPECT_EQ((*talkers)[0].format.sample_rate_hz, 96'000U);
    EXPECT_EQ((*talkers)[0].format.channels, 8);
    EXPECT_EQ((*talkers)[1].index, 1);
    EXPECT_EQ((*talkers)[1].format.kind, StreamKind::crf);
    EXPECT_EQ((*talkers)[1].format.crf_base_frequency_hz, 48'000U);

    // One CRF clock input (the media clock can slave to a remote CRF).
    auto const listeners = listener_stream_specs(storage, 0);
    EXPECT_TRUE(listeners.has_value());
    EXPECT_EQ(listeners->size(), 1U);
    EXPECT_EQ(listeners->front().index, 0);
    EXPECT_EQ(listeners->front().format.kind, StreamKind::crf);
    EXPECT_EQ(listeners->front().format.crf_base_frequency_hz, 48'000U);

    // The hardcoded index constants this layer replaces.
    EXPECT_EQ(find_stream(*talkers, StreamKind::aaf), std::optional<uint16_t>{0});
    EXPECT_EQ(find_stream(*talkers, StreamKind::crf), std::optional<uint16_t>{1});
    EXPECT_EQ(find_stream(*talkers, StreamKind::am824), std::nullopt);

    // One media clock rate for the table (CRF does not constrain it).
    auto const rate = common_audio_sample_rate(*talkers);
    EXPECT_TRUE(rate.has_value());
    EXPECT_EQ(*rate, 96'000U);
}

TEST(stream_spec_derive, audio_blob_three_out_three_in)
{
    auto const blob = testdata("entity_audio.bin");
    EXPECT_FALSE(blob.empty());
    auto const storage = storage_for(blob);

    auto const talkers = talker_stream_specs(storage, 0);
    EXPECT_TRUE(talkers.has_value());
    EXPECT_EQ(talkers->size(), 3U);
    EXPECT_EQ((*talkers)[0].format.kind, StreamKind::am824);
    EXPECT_EQ((*talkers)[1].format.kind, StreamKind::aaf);
    EXPECT_EQ((*talkers)[2].format.kind, StreamKind::crf);

    auto const listeners = listener_stream_specs(storage, 0);
    EXPECT_TRUE(listeners.has_value());
    EXPECT_EQ(listeners->size(), 3U);
    EXPECT_EQ((*listeners)[0].format.kind, StreamKind::am824);
    EXPECT_EQ((*listeners)[1].format.kind, StreamKind::aaf);
    EXPECT_EQ((*listeners)[2].format.kind, StreamKind::crf);  // media-clock input (kit 3c)

    // All audio streams in this model run 96 kHz.
    auto const rate = common_audio_sample_rate(*talkers);
    EXPECT_TRUE(rate.has_value());
    EXPECT_EQ(*rate, 96'000U);
}

TEST(stream_spec_tspec, milan_table_4_4_sizes)
{
    // Milan v1.2 Section 4.3.3.2, Table 4.4: AVTPDU header + payload + 1.
    // AAF PCM32 96 kHz 8 ch: 48*8 + 24 + 1 = 409; CRF 1 ts/pdu: 28 + 1 = 29.
    EXPECT_EQ(srp_max_frame_size(decode_stream_format(AAF_96K_INT32_8CH)), 409);
    EXPECT_EQ(srp_max_frame_size(decode_stream_format(CRF_MILAN_48K)), 29);

    // AM824 (not a Milan format) 48k 8ch: 32-byte header + (6+1 samples * 8ch * 4B) = 256.
    EXPECT_EQ(srp_max_frame_size(decode_stream_format(AM824_48K_8CH)), 256);

    // Unknown formats size to 0 (callers must treat as authoring error).
    EXPECT_EQ(srp_max_frame_size(decode_stream_format(0)), 0);
}

TEST(stream_spec_rate, mixed_audio_rates_rejected)
{
    StreamSpecs specs{};
    StreamSpec a{};
    a.format = decode_stream_format(AAF_96K_INT32_8CH);  // 96 kHz
    StreamSpec b{};
    b.index = 1;
    b.format = decode_stream_format(AM824_48K_8CH);  // 48 kHz
    specs.push_back(a);
    specs.push_back(b);
    EXPECT_FALSE(common_audio_sample_rate(specs).has_value());

    // CRF alongside audio never conflicts.
    StreamSpecs ok{};
    ok.push_back(a);
    StreamSpec c{};
    c.index = 1;
    c.format = decode_stream_format(CRF_MILAN_48K);
    ok.push_back(c);
    auto const rate = common_audio_sample_rate(ok);
    EXPECT_TRUE(rate.has_value());
    EXPECT_EQ(*rate, 96'000U);

    // CRF-only tables take the fallback; no fallback is an error.
    StreamSpecs crf_only{};
    crf_only.push_back(c);
    auto const fb = common_audio_sample_rate(crf_only, 96'000U);
    EXPECT_TRUE(fb.has_value());
    EXPECT_EQ(*fb, 96'000U);
    EXPECT_FALSE(common_audio_sample_rate(crf_only).has_value());
}

// with_format re-decodes the word it is given, keeping the spec's index
TEST(stream_spec_rate, with_format_redecodes)
{
    StreamSpec spec{};
    spec.index = 3;
    auto const moved = with_format(spec, 0x020702200200C000ULL);
    EXPECT_EQ(moved.index, uint16_t{3});
    EXPECT_EQ(moved.format.sample_rate_hz, 96'000U);
    EXPECT_EQ(moved.format.aaf_samples_per_frame, uint16_t{12});
}

}  // namespace

// The 16A's input (nb + ut) and output (nb + ut + sc) words are carried as DBS channels.
TEST(stream_spec_decode, am824_motu_16a_words_are_accepted)
{
    constexpr auto in = decode_stream_format(0x00A0040860000800ULL);
    static_assert(in.kind == StreamKind::am824);
    EXPECT_EQ(in.sample_rate_hz, 96'000U);
    EXPECT_EQ(in.channels, 8);
    constexpr auto out = decode_stream_format(0x00A0020870000800ULL);
    static_assert(out.kind == StreamKind::am824);
    EXPECT_EQ(out.sample_rate_hz, 48'000U);
    EXPECT_EQ(out.channels, 8);
    EXPECT_EQ(decode_stream_format(0x00A0060140000100ULL).kind, StreamKind::am824);  // 192 kHz, 1 ch
}

// The kit carries 1, 2, 4, 6 or 8 AM824 channels; any other MBLA count is unsupported.
TEST(stream_spec_decode, am824_channel_counts_outside_1_2_4_6_8_are_other)
{
    for (uint8_t ch = 0; ch <= 64; ++ch) {
        auto const f = decode_stream_format(avtp::make_am824_stream_format(2, ch));
        bool const carried = ch == 1 || ch == 2 || ch == 4 || ch == 6 || ch == 8;
        EXPECT_EQ(f.kind == StreamKind::am824, carried);
        EXPECT_EQ(f.channels, carried ? ch : 0);
    }
}

// Blocking, mixed-label, mismatched-DBS, non-AM824 event and unknown-rate words are refused.
TEST(stream_spec_decode, am824_blocking_mixed_and_odd_words_are_other)
{
    EXPECT_EQ(decode_stream_format(0x00A0020880000800ULL).kind, StreamKind::other);  // blocking
    EXPECT_EQ(decode_stream_format(0x00A0020800000800ULL).kind, StreamKind::other);  // neither b nor nb
    EXPECT_EQ(decode_stream_format(0x00A0020840020600ULL).kind, StreamKind::other);  // 2 IEC 60958 + 6 MBLA
    EXPECT_EQ(decode_stream_format(0x00A0020940000810ULL).kind, StreamKind::other);  // 8 MBLA + 1 MIDI
    EXPECT_EQ(decode_stream_format(0x00A0020840000600ULL).kind, StreamKind::other);  // DBS 8, 6 MBLA
    EXPECT_EQ(decode_stream_format(0x00A00A0840000800ULL).kind, StreamKind::other);  // FDF evt 1
    EXPECT_EQ(decode_stream_format(0x00A0070840000800ULL).kind, StreamKind::other);  // SFC 7
    EXPECT_EQ(decode_stream_format(0x00A0021040001000ULL).kind, StreamKind::other);  // 16 MBLA
}

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_spec_test)
