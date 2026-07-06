// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Audio I/O (dual-format AM824 + AAF) Unit Tests
///
/// Covers the two things specific to the dual entity:
///  - the AAF int32 data plane round-trips (serialize -> parse -> deserialize)
///    and recovers the input samples, mirroring the AM824 path;
///  - the entity builds from a dual-format descriptor blob (2 stream inputs +
///    2 stream outputs) and drives both talkers from process_audio().

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_stream_input.hpp"
#include "statusbar/avtp/avtp_aaf_stream_output.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

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

// Dual-format descriptor blob (committed fixture; source: examples/dual.json).
auto entity_audio_bin_path() -> std::filesystem::path
{
    return std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_audio.bin";
}

auto make_config_with_blob(std::vector<uint8_t> blob) -> AvbEntityAudioIOConfig
{
    AvbEntityAudioIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.descriptor_storage_blob = std::move(blob);
    return config;
}

// Build a controller CONNECT_RX_COMMAND addressed at this entity's listener.
auto make_connect_rx(Eui64 talker_id, uint16_t talker_uid, Eui64 listener_id, uint16_t listener_uid, uint16_t seq = 1)
    -> atdecc::AcmpCommandResponse
{
    atdecc::AcmpCommandResponse cmd{};
    cmd.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    cmd.talker_entity_id = talker_id;
    cmd.talker_unique_id = talker_uid;
    cmd.listener_entity_id = listener_id;
    cmd.listener_unique_id = listener_uid;
    cmd.sequence_id = seq;
    return cmd;
}

}  // namespace

//
// AAF int32 data-plane round-trip (no network, no blob)
//

TEST(audio_io_aaf, int32_roundtrip_recovers_samples)
{
    constexpr uint16_t kChannels = 8;
    constexpr uint16_t kSamples = 6;
    constexpr uint8_t kDepth = 32;

    avtp::AafStreamOutputContext out{
        avtp::StreamId{}, avtp::AafFormat::int_32bit, avtp::AafSampleRate::rate_96_khz, kChannels, kDepth, 2'000'000};

    // Distinct constant per channel, well inside [-1, 1].
    std::array<float, kChannels> in_vals{};
    for (uint16_t ch = 0; ch < kChannels; ++ch) {
        in_vals[ch] = -0.9F + (static_cast<float>(ch) * 0.2F);
    }

    std::array<uint8_t, 1024> frame{};
    avtp::AafPdu pdu{};
    auto payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    size_t const n = avtp::aaf_stream_serialize(out, pdu, payload, kSamples, 1'000'000, [&](uint8_t ch, std::span<float> dest) {
        for (auto& d : dest) {
            d = in_vals[ch];
        }
    });
    EXPECT_EQ(n, static_cast<size_t>(kSamples) * kChannels * 4U);

    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::AafPdu::HEADER_LENGTH + n;
    std::span<uint8_t const> const packet{frame.data(), frame_len};

    // Frame must be recognizable as AAF on the wire.
    EXPECT_EQ(frame[0], avtp::AvtpSubtype::aaf);

    auto parsed = avtp::aaf_parse_header(packet);
    EXPECT_TRUE(parsed.has_value());

    auto const audio = avtp::aaf_get_audio_payload(packet);
    avtp::AafStreamInputContext in{avtp::AafFormat::int_32bit, avtp::AafSampleRate::rate_96_khz, kChannels, kDepth};

    std::array<float, kChannels> recovered{};
    bool count_ok = true;
    avtp::aaf_stream_deserialize(
        in, *parsed, audio, 1'000'000, [&](uint8_t ch, std::span<float> s, uint64_t /*pts*/, uint64_t /*period*/) {
            if (s.size() != kSamples) {
                count_ok = false;
            }
            if (ch < kChannels && !s.empty()) {
                recovered[ch] = s[0];
            }
        });

    EXPECT_TRUE(count_ok);
    for (uint16_t ch = 0; ch < kChannels; ++ch) {
        EXPECT_TRUE(std::abs(recovered[ch] - in_vals[ch]) < 1.0e-4F);
    }
}

TEST(audio_io_aaf, format_constants)
{
    EXPECT_EQ(AvbEntityAudioIO::AAF_FORMAT, avtp::AafFormat::int_32bit);
    EXPECT_EQ(AvbEntityAudioIO::AAF_BIT_DEPTH, 32U);
    EXPECT_EQ(AvbEntityAudioIO::SAMPLE_RATE, 96000U);
    EXPECT_EQ(AvbEntityAudioIO::SAMPLES_PER_PACKET, 12U);
    EXPECT_EQ(AvbEntityAudioIO::AM824_STREAM_INDEX, 0U);
    EXPECT_EQ(AvbEntityAudioIO::AAF_STREAM_INDEX, 1U);
    // CRF media clock is the Milan 48 kHz reference (48/96 kHz clients lock to it);
    // it must divide the audio sample rate so each CRF event spans a whole number
    // of audio samples (96000/48000 = 2).
    EXPECT_EQ(AvbEntityAudioIO::CRF_BASE_FREQUENCY, 48000U);
    EXPECT_EQ(AvbEntityAudioIO::SAMPLE_RATE % AvbEntityAudioIO::CRF_BASE_FREQUENCY, 0U);
}

//
// Entity construction from the dual-format blob
//

TEST(audio_io_create, dual_blob_succeeds)
{
    auto blob = load_file(entity_audio_bin_path());
    EXPECT_TRUE(blob.size() > 0);
    auto result = AvbEntityAudioIO::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
}

TEST(audio_io_create, empty_blob_fails)
{
    auto result = AvbEntityAudioIO::create(make_config_with_blob({}));
    EXPECT_FALSE(result.has_value());
}

TEST(audio_io_udptun, redundancy_bit_is_collision_proof_and_owlm_maskable)
{
    // The tunnel redundancy flag bit must (1) be a single bit, (2) live in the
    // EUI-64 middle bytes b3/b4 that owlm_analyze masks when grouping primary +
    // redundant into one logical sender, and (3) yield a DISTINCT redundant id for
    // ANY entity_id — including ones whose MAC half has that region set. (The old
    // 1<<23 was in the NIC half: it could equal a set MAC bit -> redundant == primary
    // -> every packet mis-tagged redundant, AND it was outside owlm's mask -> the two
    // copies would split into different pair_ids. This pins both invariants.)
    constexpr uint64_t BIT = AvbEntityAudioIO::UDPTUN_REDUN_BIT;
    constexpr uint64_t MID = AvbEntityAudioIO::OWLM_PAIR_MASK_MIDBYTES;  // b3/b4 owlm zeroes
    EXPECT_EQ(BIT & (BIT - 1), uint64_t{0});                             // exactly one bit
    EXPECT_EQ(BIT & ~MID, uint64_t{0});                                  // inside owlm's masked middle bytes
    for (uint64_t const eid :
         {0x020000fffedfd196ULL,              // node-a (b4 = 0xFE)
          0x020000fffee01360ULL,              // node-e
          0xFFFFFFFFFFFFFFFFULL}) {           // adversarial: every bit set
        uint64_t const primary = eid & ~BIT;  // entity force-clears the flag
        uint64_t const redundant = primary | BIT;
        EXPECT_EQ(primary & BIT, uint64_t{0});        // primary: flag clear
        EXPECT_EQ(redundant & BIT, BIT);              // redundant: flag set
        EXPECT_NE(primary, redundant);                // always distinct on the wire
        EXPECT_EQ(primary & ~MID, redundant & ~MID);  // same owlm pair_id (mask b3/b4)
    }
}

TEST(audio_io_model, declares_streams_and_ports)
{
    // The symbol-aware entity serves descriptors straight from the .aem blob, so
    // verify the fixture's shape via the DescriptorStorage (the source of truth).
    auto blob = load_file(entity_audio_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const count = [&](uint16_t type) -> size_t {
        size_t n = 0;
        while (storage->get_descriptor(0, type, static_cast<uint16_t>(n)).has_value()) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_INPUT), 2u);   // AM824 + AAF
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT), 3u);  // AM824 + AAF + CRF media clock
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER), 4u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_MAP), 4u);
}

TEST(audio_io_model, crf_stream_output_is_milan_48k)
{
    // The CRF (STREAM_OUTPUT[2]) descriptor must advertise the exact Milan Clock
    // Reference Format (Milan v1.3 §7.3.1 Table 7.1): interval=96, 1 ts/pdu, pull=0,
    // base_frequency=48000 -> ATDECC format string 0x041060010000BB80. Anything else
    // (e.g. a 96 kHz base, or the right base with a wrong interval) is rejected by
    // Milan listeners as UNSUPPORTED_FORMAT. This pins the fixture so it cannot drift.
    auto blob = load_file(entity_audio_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const desc = storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, AvbEntityAudioIO::CRF_STREAM_INDEX);
    EXPECT_TRUE(desc.has_value());
    if (!desc.has_value()) {
        return;
    }
    atdecc::aem::DescriptorStream so{};
    span_load_padded(so, *desc);
    std::array<uint8_t, 8> const milan_crf{0x04, 0x10, 0x60, 0x01, 0x00, 0x00, 0xBB, 0x80};
    auto const fmt = so.current_format.span();
    EXPECT_EQ(fmt.size(), milan_crf.size());  // Eui64 span is 8 bytes
    bool bytes_match = fmt.size() == milan_crf.size();
    for (size_t i = 0; i < milan_crf.size(); ++i) {
        bytes_match = bytes_match && (fmt[i] == milan_crf[i]);
    }
    EXPECT_TRUE(bytes_match);  // CRF STREAM_OUTPUT format == Milan 0x041060010000BB80
}

TEST(audio_io_channels, extracts_8_channels)
{
    auto blob = load_file(entity_audio_bin_path());
    auto result = AvbEntityAudioIO::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((*result)->channels(), 8U);
}

TEST(audio_io_audio, process_audio_drives_both_formats)
{
    auto blob = load_file(entity_audio_bin_path());
    auto result = AvbEntityAudioIO::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());

    size_t callback_buffer_size = 0;
    (*result)->set_audio_callback([&](std::span<float> samples, size_t /*count*/) { callback_buffer_size = samples.size(); });

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->process_audio(now);  // not started: serialize is skipped, audio engine still runs
    EXPECT_EQ(callback_buffer_size, AvbEntityAudioIO::SAMPLES_PER_PACKET * 8U);
    EXPECT_FALSE((*result)->is_running());
}

TEST(audio_io_state, initial_state)
{
    auto blob = load_file(entity_audio_bin_path());
    auto result = AvbEntityAudioIO::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE((*result)->is_running());
    EXPECT_EQ((*result)->state_string(), "Start");
}

TEST(audio_io_print, print_state_does_not_crash)
{
    auto blob = load_file(entity_audio_bin_path());
    auto result = AvbEntityAudioIO::create(make_config_with_blob(std::move(blob)));
    EXPECT_TRUE(result.has_value());
    (*result)->print_state();
    EXPECT_FALSE((*result)->is_running());
}

//
// Talker-on-listener transmit gate (don't put a stream on the wire when no
// listener is ready/connected). Hardware-verified on node-a: gate on + no
// listener -> AM824/AAF tx=0; gate off -> tx climbs at the Class-A rate.
//

TEST(audio_io_gate, default_suppresses_all_talkers_without_listener)
{
    // Default config gates on listener readiness. Fresh entity has no MSRP
    // Listener Ready and no ACMP connection on any stream, so every talker
    // (AM824, AAF, CRF) must stay off the wire.
    auto blob = load_file(entity_audio_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    EXPECT_TRUE(config.gate_talker_on_listener);  // gated by default
    auto result = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    EXPECT_FALSE((*result)->talker_should_transmit(AvbEntityAudioIO::AM824_STREAM_INDEX, 0));
    EXPECT_FALSE((*result)->talker_should_transmit(AvbEntityAudioIO::AAF_STREAM_INDEX, 0));
    EXPECT_FALSE((*result)->talker_should_transmit(AvbEntityAudioIO::CRF_STREAM_INDEX, 0));
}

TEST(audio_io_gate, disabled_allows_all_talkers)
{
    // The escape hatch: --gate.talker_on_listener=false restores the legacy
    // link-up-driven unconditional transmit for every stream.
    auto blob = load_file(entity_audio_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    config.gate_talker_on_listener = false;
    auto result = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    EXPECT_TRUE((*result)->talker_should_transmit(AvbEntityAudioIO::AM824_STREAM_INDEX, 0));
    EXPECT_TRUE((*result)->talker_should_transmit(AvbEntityAudioIO::AAF_STREAM_INDEX, 0));
    EXPECT_TRUE((*result)->talker_should_transmit(AvbEntityAudioIO::CRF_STREAM_INDEX, 0));
}

//
// ACMP CONNECT_RX relay (regression guard).
//
// When a controller connects a remote talker (e.g. a third-party devices output) to this
// entity, it sends a CONNECT_RX_COMMAND to OUR listener; our listener must relay
// a CONNECT_TX_COMMAND to that talker, and on the talker's CONNECT_TX_RESPONSE
// answer the controller with CONNECT_RX_RESPONSE(Success) and mark the sink
// connected. Hardware symptom this guards against: a "connect the audio interface -> node-a"
// timing out because our listener never emitted the CONNECT_TX relay. The entity
// MUST construct a listener whose entity id matches what it advertises (so the
// listener_entity_id in the command matches) and whose stream count covers the
// AM824/AAF sink indices -- both verified end-to-end here against a real blob.
//

TEST(audio_io_acmp, connect_rx_relays_connect_tx_to_talker)
{
    auto blob = load_file(entity_audio_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    Eui64 const listener_id = config.entity_id;  // what the entity advertises + targets
    auto result = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto& listener = (*result)->components().acmp_listener;
    // The listener must answer to the SAME id the entity advertises, else every
    // CONNECT_RX is silently dropped as "not for us".
    EXPECT_EQ(listener.entity_id(), listener_id);

    atdecc::AcmpCommandResponse captured{};
    bool relayed = false;
    nanoavb::AcmpListenerCallbacks cb;
    cb.tx_command = [&](atdecc::AcmpCommandResponse const& c) {
        captured = c;
        relayed = true;
        return true;
    };
    listener.set_callbacks(cb);
    // NOTE: deliberately NO listener.start() here -- the production entity never
    // starts the listener SM, relying on the SM engine's auto-UCT chain to leave
    // the initial Start state on the first received command. This reproduces the
    // exact production state so the first CONNECT_RX must NOT be silently dropped.

    Eui64 const talker_id{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};  // remote talker (e.g. the audio interface)
    auto cmd = make_connect_rx(talker_id, 0, listener_id, AvbEntityAudioIO::AM824_STREAM_INDEX);
    auto const now = sm::TimePoint{};

    EXPECT_TRUE(listener.receive_controller_command(cmd, now));
    EXPECT_TRUE(relayed);
    EXPECT_EQ(captured.message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(captured.talker_entity_id, talker_id);
    EXPECT_EQ(captured.listener_entity_id, listener_id);
    EXPECT_TRUE(listener.has_pending());
}

TEST(audio_io_acmp, connect_rx_relays_after_wire_parse)
{
    // Faithful to the on-wire dispatch path: a controller serializes CONNECT_RX to
    // the 56-byte L2 short form, and the entity span_loads an AcmpDu and rebuilds
    // the command via acmp_command_response_from_pdu before relaying. Guards a
    // serialize/parse regression that would silently drop the command on the wire
    // even though the in-memory SM relay (above) stays green.
    auto blob = load_file(entity_audio_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    Eui64 const listener_id = config.entity_id;
    auto result = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto& listener = (*result)->components().acmp_listener;
    atdecc::AcmpCommandResponse captured{};
    bool relayed = false;
    nanoavb::AcmpListenerCallbacks cb;
    cb.tx_command = [&](atdecc::AcmpCommandResponse const& c) {
        captured = c;
        relayed = true;
        return true;
    };
    listener.set_callbacks(cb);
    listener.start();

    Eui64 const talker_id{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    auto cmd = make_connect_rx(talker_id, 0, listener_id, AvbEntityAudioIO::AM824_STREAM_INDEX, 7);

    // Controller serializes to the wire; entity parses exactly as dispatch_frame does.
    std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> wire{};
    auto const bytes = atdecc::acmp_serialize_2016(cmd, wire);
    atdecc::AcmpDu pdu{};
    span_load(pdu, bytes);
    auto const parsed = atdecc::acmp_command_response_from_pdu(pdu);

    EXPECT_EQ(parsed.message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(parsed.listener_entity_id, listener_id);
    EXPECT_EQ(parsed.talker_entity_id, talker_id);

    EXPECT_TRUE(listener.receive_controller_command(parsed, sm::TimePoint{}));
    EXPECT_TRUE(relayed);
    EXPECT_EQ(captured.message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(captured.talker_entity_id, talker_id);
    EXPECT_EQ(captured.sequence_id, 7);
}

TEST(audio_io_acmp, connect_rx_full_handshake_connects_sink)
{
    auto blob = load_file(entity_audio_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    Eui64 const listener_id = config.entity_id;
    auto result = AvbEntityAudioIO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto& listener = (*result)->components().acmp_listener;

    atdecc::AcmpCommandResponse to_controller{};
    bool responded = false;
    nanoavb::AcmpListenerCallbacks cb;
    cb.tx_command = [](atdecc::AcmpCommandResponse const&) { return true; };  // relay to talker (ignored here)
    cb.tx_response = [&](atdecc::AcmpCommandResponse const& resp) {
        to_controller = resp;
        responded = true;
        return true;
    };
    listener.set_callbacks(cb);
    listener.start();

    Eui64 const talker_id{0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    constexpr uint16_t kSink = AvbEntityAudioIO::AAF_STREAM_INDEX;
    auto cmd = make_connect_rx(talker_id, 0, listener_id, kSink);
    auto const now = sm::TimePoint{};
    EXPECT_TRUE(listener.receive_controller_command(cmd, now));
    EXPECT_TRUE(listener.has_pending());

    // Talker answers our relayed CONNECT_TX with success.
    atdecc::AcmpCommandResponse talker_resp{};
    talker_resp.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    talker_resp.set_status(atdecc::ACMP_STATUS_SUCCESS);
    talker_resp.talker_entity_id = talker_id;
    talker_resp.talker_unique_id = 0;
    talker_resp.listener_entity_id = listener_id;
    talker_resp.listener_unique_id = kSink;
    talker_resp.sequence_id = cmd.sequence_id;
    talker_resp.stream_dest_mac = ieee::Eui48{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};

    EXPECT_TRUE(listener.receive_talker_response(talker_resp, now));
    EXPECT_TRUE(responded);
    EXPECT_EQ(to_controller.message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(to_controller.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_FALSE(listener.has_pending());
    EXPECT_TRUE(listener.is_connected(kSink));
}

TEST_MAIN(statusbar_avb_entity, avb_entity_audio_io_test)
