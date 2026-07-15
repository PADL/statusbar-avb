// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Tone Generator (talker-only AM824 + AAF + CRF) Unit Tests
///
/// Covers what is specific to the tone generator:
///  - the white-key frequency table (C2..C3) is correct;
///  - the entity builds from the talker-only blob (0 stream inputs + 3 stream
///    outputs) and reports 8 channels;
///  - the descriptor model has no listener sinks and the CRF output is Milan 48k;
///  - the transmit gate suppresses all three talkers with no listener (and the
///    escape hatch opens them);
///  - process_audio runs (gPTP-locked media clock, r = 1.0) without a talker.

#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avb_entity/avb_entity_talker_gate.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using namespace statusbar::ieee;

namespace {

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

// Talker-only descriptor blob (committed fixture; source: examples/tone.json).
auto entity_tone_bin_path() -> std::filesystem::path
{
    return std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone.bin";
}

// AAF-only blob (committed fixture; source: examples/tone-aaf.json).
auto entity_tone_aaf_bin_path() -> std::filesystem::path
{
    return std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone_aaf.bin";
}

auto make_config_with_blob(std::vector<uint8_t> blob) -> AvbEntityAudioIOConfig
{
    AvbEntityAudioIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.descriptor_storage_blob = std::move(blob);
    return config;
}

constexpr bool approx(double a, double b, double tol = 1.0e-2)
{
    return std::abs(a - b) < tol;
}

}  // namespace

//
// White-key frequency table (the piano notes the 8 channels carry)
//

TEST(tone_notes, white_keys_c2_to_c3)
{
    // base 36 = C2; the 8 white keys C2..C3 (equal temperament, A4 = 440).
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 0), 65.406));   // C2
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 1), 73.416));   // D2
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 2), 82.407));   // E2
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 3), 87.307));   // F2
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 4), 97.999));   // G2
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 5), 110.000));  // A2 (exact: A4/4)
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 6), 123.471));  // B2
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 7), 130.813));  // C3 (octave above ch0)
    // Octave wrap: the 8th white key is exactly 2x the first.
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 7) / white_key_frequency_hz(36, 0), 2.0, 1.0e-6));
    // A2 is exactly 110 Hz (a clean anchor).
    EXPECT_TRUE(approx(white_key_frequency_hz(36, 5), 110.0, 1.0e-6));
}

//
// Blob-derived stream topology (kit phase 1): kinds/indices/rate follow the
// blob's STREAM_OUTPUT formats instead of per-entity C++ constants.
//

TEST(tone_format, specs_follow_the_blob)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    auto const& specs = (*result)->stream_specs();
    EXPECT_EQ(specs.size(), 3U);  // tone.json: AM824@0, AAF@1, CRF@2
    EXPECT_EQ(specs[0].format.kind, StreamKind::am824);
    EXPECT_EQ(specs[1].format.kind, StreamKind::aaf);
    EXPECT_EQ(specs[1].format.aaf_format, avtp::AafFormat::int_32bit);
    EXPECT_EQ(specs[1].format.bit_depth, 32U);
    EXPECT_EQ(specs[2].format.kind, StreamKind::crf);
    EXPECT_EQ(specs[2].format.crf_base_frequency_hz, 48000U);
    EXPECT_EQ((*result)->sample_rate(), 96000U);
    EXPECT_EQ((*result)->sample_rate() % specs[2].format.crf_base_frequency_hz, 0U);
}

//
// Entity construction from the talker-only blob
//

TEST(tone_create, tone_blob_succeeds)
{
    auto blob = load_file(entity_tone_bin_path());
    EXPECT_TRUE(blob.size() > 0);
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
}

TEST(tone_create, empty_blob_fails)
{
    auto result = AvbEntityToneGenerator::create(make_config_with_blob({}));
    EXPECT_FALSE(result.has_value());
}

TEST(tone_channels, extracts_8_channels)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((*result)->channels(), 8U);
}

//
// Descriptor model shape: talker-only (no listener sinks), 3 outputs, Milan CRF.
//

TEST(tone_model, declares_three_outputs_no_inputs)
{
    auto blob = load_file(entity_tone_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const count = [&](uint16_t type) -> size_t {
        size_t n = 0;
        while (storage->get_descriptor(0, type, static_cast<uint16_t>(n)).has_value()) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_INPUT), 0u);   // talker-only: no listener sinks
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT), 3u);  // AM824 + AAF + CRF
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER), 2u);  // AM824 + AAF output clusters
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_MAP), 2u);
}

TEST(tone_model, crf_stream_output_is_milan_48k)
{
    auto blob = load_file(entity_tone_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const desc = storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 2);
    EXPECT_TRUE(desc.has_value());
    if (!desc.has_value()) {
        return;
    }
    atdecc::aem::DescriptorStream so{};
    span_load_padded(so, *desc);
    std::array<uint8_t, 8> const milan_crf{0x04, 0x10, 0x60, 0x01, 0x00, 0x00, 0xBB, 0x80};
    auto const fmt = so.current_format.span();
    bool bytes_match = fmt.size() == milan_crf.size();
    for (size_t i = 0; i < milan_crf.size(); ++i) {
        bytes_match = bytes_match && (fmt[i] == milan_crf[i]);
    }
    EXPECT_TRUE(bytes_match);
}

//
// Transmit gate (talker-only): default suppresses all three with no listener.
//

TEST(tone_gate, default_suppresses_all_talkers_without_listener)
{
    auto blob = load_file(entity_tone_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    EXPECT_TRUE(config.gate_talker_on_listener);
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE((*result)->talker_should_transmit(0, 0));
    EXPECT_FALSE((*result)->talker_should_transmit(1, 0));
    EXPECT_FALSE((*result)->talker_should_transmit(2, 0));
}

TEST(tone_gate, disabled_allows_all_talkers)
{
    auto blob = load_file(entity_tone_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    config.gate_talker_on_listener = false;
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE((*result)->talker_should_transmit(0, 0));
    EXPECT_TRUE((*result)->talker_should_transmit(1, 0));
    EXPECT_TRUE((*result)->talker_should_transmit(2, 0));
}

// Each talker stream gates INDEPENDENTLY on its own ACMP connection + reservation.
// Regression for the CRF-follows-audio bug: CRF is a first-class stream and must
// NOT transmit merely because an audio stream is admitted. Drives real ACMP
// connections through the shared acmp_talker; a second TalkerGate over the same
// components lets the test set listener-ready / stream-started via the public API.
TEST(tone_gate, crf_gates_independently_of_audio_streams)
{
    auto blob = load_file(entity_tone_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    EXPECT_TRUE(config.gate_talker_on_listener);
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        return;
    }
    auto& comps = (*result)->components();

    // Gate under test: gating enabled, over the entity's live components. Reads the
    // shared acmp_talker connection state; owns its own listener-ready/started flags.
    TalkerGate gate{/*gate_enabled=*/true, comps};

    constexpr uint16_t k_aaf = 1;  // tone.json: AM824@0, AAF@1, CRF@2
    constexpr uint16_t k_crf = 2;
    auto const* aaf_stream = comps.acmp_talker.get_stream(k_aaf);
    auto const* crf_stream = comps.acmp_talker.get_stream(k_crf);
    EXPECT_NE(aaf_stream, nullptr);
    EXPECT_NE(crf_stream, nullptr);
    if (aaf_stream == nullptr || crf_stream == nullptr) {
        return;
    }
    auto const as_stream_id = [](Eui64 const& e) {
        nanoavb::StreamId sid;
        sid.from_uint64(e.to_uint64());
        return sid;
    };
    auto const aaf_sid = as_stream_id(aaf_stream->stream_id);
    auto const crf_sid = as_stream_id(crf_stream->stream_id);

    Eui64 const entity_id{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};  // == make_config_with_blob
    Eui64 const listener_id{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    auto const connect = [&](uint16_t talker_unique, uint16_t listener_unique) {
        atdecc::AcmpCommandResponse cmd{};
        cmd.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
        cmd.talker_entity_id = entity_id;
        cmd.talker_unique_id = talker_unique;
        cmd.listener_entity_id = listener_id;
        cmd.listener_unique_id = listener_unique;
        cmd.sequence_id = 1;
        (void)comps.acmp_talker.receive_command(cmd, sm::TimePoint{});
        // Mirror what the entity's on_connect callback does: publish the fresh
        // ACMP connection count into the gate (the media thread reads only this,
        // never the reactor-mutated connection list).
        gate.note_acmp_connections(talker_unique, static_cast<uint32_t>(comps.acmp_talker.connection_count(talker_unique)));
    };

    // Nothing connected -> nothing transmits.
    EXPECT_FALSE(gate.should_transmit(k_aaf, 0));
    EXPECT_FALSE(gate.should_transmit(k_crf, 0));

    // Fully admit the AAF stream (ACMP connection + MSRP Listener Ready). CRF is
    // left untouched -- it must stay suppressed.
    connect(k_aaf, 0);
    gate.note_listener_ready(aaf_sid, true);
    EXPECT_EQ(comps.acmp_talker.connection_count(k_aaf), size_t{1});
    EXPECT_EQ(comps.acmp_talker.connection_count(k_crf), size_t{0});
    EXPECT_TRUE(gate.should_transmit(k_aaf, 0));
    EXPECT_FALSE(gate.should_transmit(k_crf, 0));  // the regression: CRF must not ride AAF

    // Admit CRF on its OWN merits -> now it transmits, independently.
    connect(k_crf, 1);
    gate.note_listener_ready(crf_sid, true);
    EXPECT_TRUE(gate.should_transmit(k_crf, 0));

    // Stream Started defaults true; Stopping AAF suppresses it, leaving CRF alone.
    gate.note_stream_started(k_aaf, false);
    EXPECT_FALSE(gate.should_transmit(k_aaf, 0));
    EXPECT_TRUE(gate.should_transmit(k_crf, 0));
}

//
// Lifecycle / data plane (no network).
//

TEST(tone_state, initial_state)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE((*result)->is_running());
    EXPECT_EQ((*result)->state_string(), "Start");
}

TEST(tone_audio, process_audio_runs_without_talker)
{
    // Not started: the talker serialize contexts are unset and the gate is closed,
    // so process_audio only advances the gPTP-locked media clock + oscillators. It
    // must not crash and must leave the entity not-running.
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->process_audio(now);
    (*result)->process_audio(now);
    EXPECT_FALSE((*result)->is_running());
}

TEST(tone_print, print_state_does_not_crash)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    (*result)->print_state();
    EXPECT_FALSE((*result)->is_running());
}

//
// AAF-only variant (single 8-ch AAF stream output at index 0).
//

TEST(tone_aaf_model, declares_one_aaf_output_no_inputs)
{
    auto blob = load_file(entity_tone_aaf_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const count = [&](uint16_t type) -> size_t {
        size_t n = 0;
        while (storage->get_descriptor(0, type, static_cast<uint16_t>(n)).has_value()) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_INPUT), 0u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT), 1u);  // AAF only
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_MAP), 1u);
    // STREAM_OUTPUT[0] must be the AAF int32 format (0x0207022002 00C000).
    auto const desc = storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_TRUE(desc.has_value());
    if (desc.has_value()) {
        atdecc::aem::DescriptorStream so{};
        span_load_padded(so, *desc);
        EXPECT_EQ(so.current_format.span()[0], 0x02);  // AAF subtype
    }
}

TEST(tone_aaf_create, aaf_only_builds_and_gates)
{
    auto blob = load_file(entity_tone_aaf_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        return;
    }
    EXPECT_EQ((*result)->channels(), 8U);
    // Gate closed (no listener): the AAF stream (index 0 in AafOnly) stays off.
    EXPECT_FALSE((*result)->talker_should_transmit(0, 0));
    (*result)->process_audio(sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()});
    EXPECT_FALSE((*result)->is_running());
}

TEST(tone_aaf_create, aaf_only_gate_disabled_transmits_index0)
{
    auto blob = load_file(entity_tone_aaf_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    config.gate_talker_on_listener = false;
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        return;
    }
    EXPECT_TRUE((*result)->talker_should_transmit(0, 0));  // AAF at index 0
}

//
// AAF + CRF variant (AAF audio idx0 + CRF media clock idx1).
//

TEST(tone_aafcrf_model, declares_aaf_plus_crf_no_inputs)
{
    auto blob = load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone_aaf_crf.bin");
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const count = [&](uint16_t type) -> size_t {
        size_t n = 0;
        while (storage->get_descriptor(0, type, static_cast<uint16_t>(n)).has_value()) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_INPUT), 0u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT), 2u);  // AAF + CRF
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER), 1u);  // AAF audio only
    // STREAM_OUTPUT[0] = AAF, STREAM_OUTPUT[1] = Milan CRF.
    auto const aaf = storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0);
    auto const crf = storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 1);
    EXPECT_TRUE(aaf.has_value() && crf.has_value());
    if (aaf.has_value() && crf.has_value()) {
        atdecc::aem::DescriptorStream a{};
        atdecc::aem::DescriptorStream c{};
        span_load_padded(a, *aaf);
        span_load_padded(c, *crf);
        EXPECT_EQ(a.current_format.span()[0], 0x02);  // AAF subtype
        EXPECT_EQ(c.current_format.span()[0], 0x04);  // CRF subtype
    }
}

TEST(tone_aafcrf_create, aaf_at_0_crf_at_1_transmit_when_ungated)
{
    auto blob = load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone_aaf_crf.bin");
    auto config = make_config_with_blob(std::move(blob));
    config.gate_talker_on_listener = false;
    auto result = AvbEntityToneGenerator::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        return;
    }
    EXPECT_EQ((*result)->channels(), 8U);
    EXPECT_TRUE((*result)->talker_should_transmit(0, 0));  // AAF at index 0
    EXPECT_TRUE((*result)->talker_should_transmit(1, 0));  // CRF at index 1
    (*result)->process_audio(sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()});
    EXPECT_FALSE((*result)->is_running());
}

//
// Per-stream TX render hooks (kit phase 2): registrations bind by blob symbol
// or stream index; the code is a menu, the model is the selection — bindings
// the model does not declare stay inert, never error.
//

TEST(tone_render, symbol_and_index_bindings_fire_with_timing)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    auto& entity = **result;

    struct Seen
    {
        size_t calls{0};
        size_t span_size{0};
        uint32_t frames{0};
        uint64_t pts{0};
    };
    static Seen by_symbol{};
    static Seen by_index{};
    by_symbol = {};
    by_index = {};

    // tone.json streams carry symbols: am824_out@0, aaf_out@1, crf_out@2.
    entity.set_render("aaf_out", [](std::span<float> audio, uint32_t frames, uint64_t /*first*/, uint64_t pts) {
        ++by_symbol.calls;
        by_symbol.span_size = audio.size();
        by_symbol.frames = frames;
        by_symbol.pts = pts;
        for (auto& s : audio) {
            s = 0.25F;
        }
    });
    entity.set_render(uint16_t{0}, [](std::span<float> audio, uint32_t frames, uint64_t /*first*/, uint64_t /*pts*/) {
        ++by_index.calls;
        by_index.span_size = audio.size();
        by_index.frames = frames;
    });

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.process_audio(now);

    EXPECT_TRUE(by_symbol.calls > 0);                                            // AAF@1 bound via its blob symbol
    EXPECT_TRUE(by_index.calls > 0);                                             // AM824@0 bound via its index
    EXPECT_EQ(by_symbol.span_size, static_cast<size_t>(by_symbol.frames) * 8U);  // frames x 8ch interleaved
    EXPECT_TRUE(by_symbol.frames > 0);
    EXPECT_TRUE(by_symbol.pts > 0);  // media-clock presentation time supplied
}

TEST(tone_render, unbound_registrations_are_inert)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    auto& entity = **result;

    static size_t calls = 0;
    calls = 0;
    auto const count_call = [](std::span<float>, uint32_t, uint64_t, uint64_t) { ++calls; };

    // The menu is bigger than the model: none of these exist in tone.json's
    // symbol table as audio STREAM_OUTPUTs, so all must be recorded but inert.
    entity.set_render("aux_out", count_call);    // symbol not in the model
    entity.set_render("identify", count_call);   // a CONTROL symbol, not a stream
    entity.set_render("crf_out", count_call);    // a stream, but not an audio one
    entity.set_render(uint16_t{2}, count_call);  // CRF index: not an audio stream
    entity.set_render(uint16_t{7}, count_call);  // index not in the model

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.process_audio(now);
    entity.process_audio(now);

    EXPECT_EQ(calls, 0U);  // inert, and creation/processing never errored
}

//
// Per-control value handlers (kit phase 4): on_control("symbol", fn) binds via
// the blob symbol table; the handler sees each accepted SET_CONTROL payload
// and can veto; unknown symbols are inert (menu/selection).
//

TEST(tone_control, on_control_binds_by_symbol_and_can_veto)
{
    auto blob = load_file(entity_tone_bin_path());
    auto result = AvbEntityToneGenerator::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    auto& entity = **result;

    static uint8_t seen = 0;
    static int calls = 0;
    seen = 0;
    calls = 0;
    // tone.json authors the identify control with symbol "identify".
    entity.on_control("identify", [](std::span<uint8_t const> value) -> uint8_t {
        ++calls;
        seen = value.empty() ? 0 : value[0];
        return atdecc::AEM_STATUS_SUCCESS;
    });
    entity.on_control("phantom_power", [](std::span<uint8_t const>) -> uint8_t {
        ++calls;  // not in the model: must never fire
        return atdecc::AEM_STATUS_SUCCESS;
    });

    auto const run_set_control = [&entity](uint16_t control_index, uint8_t value) -> uint8_t {
        atdecc::aem::AemControlPayloadHeader const payload{
            .descriptor_type = atdecc::aem::DESCRIPTOR_CONTROL, .descriptor_index = control_index};
        std::vector<uint8_t> body(atdecc::aem::AemControlPayloadHeader::LENGTH, 0);
        span_store(std::span<uint8_t>{body}, payload);
        body.push_back(value);  // the control's current values: LINEAR_UINT8 x 1
        atdecc::AemDu header{};
        header.init_command(atdecc::AEM_COMMAND_SET_CONTROL, static_cast<uint16_t>(atdecc::AemDu::AEM_DATA_LENGTH + body.size()));
        std::array<uint8_t, 128> out{};
        auto const resp = entity.components().aem_handler.handle_command(header, body, std::span<uint8_t>{out});
        return resp.status;
    };

    // The identify control is CONTROL[0] in tone.json.
    EXPECT_EQ(run_set_control(0, 0xFF), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(calls, 1);    // only the bound symbol fired
    EXPECT_EQ(seen, 0xFF);  // with the delivered payload

    // A vetoing handler propagates its status over the wire.
    entity.on_control("identify", [](std::span<uint8_t const>) -> uint8_t { return atdecc::AEM_STATUS_NOT_SUPPORTED; });
    EXPECT_EQ(run_set_control(0, 0x00), atdecc::AEM_STATUS_NOT_SUPPORTED);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_tone_generator_test)
