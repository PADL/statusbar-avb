// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity AM824 I/O Unit Tests

#include "statusbar/avb_entity/avb_entity_am824_io.hpp"

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/buffer/stream_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/realtime/realtime_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using namespace statusbar::ieee;

namespace {

constexpr auto RAW_NOW = realtime::MonotonicRawClock::from_ns(1'000'000'000);

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

auto simple2_bin_path() -> std::filesystem::path
{
    auto source_dir = std::filesystem::path{__FILE__}.parent_path();
    return source_dir / "testdata" / "simple2.bin";
}

auto make_config_with_blob(std::vector<uint8_t> blob) -> AvbEntityAm824IOConfig
{
    AvbEntityAm824IOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.descriptor_storage_blob = std::move(blob);
    return config;
}

/// Read the entity's ENTITY descriptor through the served (symbol-aware) path, so the
/// handler's runtime patches (entity_id / name) are reflected. (The raw blob is
/// unpatched, so a READ_DESCRIPTOR round-trip is the way to observe the served result.)
auto served_entity(AvbEntityAm824IO& entity) -> atdecc::aem::DescriptorEntity
{
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_READ_DESCRIPTOR, 12);
    std::array<uint8_t, 8> const cmd{0, 0, 0, 0, 0, 0, 0, 0};  // config 0, ENTITY (type 0), index 0
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> buf{};
    auto const resp = entity.components().aem_handler.handle_command(header, cmd, buf);
    atdecc::aem::DescriptorEntity desc{};
    constexpr size_t hdr = atdecc::aem::AemReadDescriptorResponsePayload::LENGTH;
    if (resp.status == atdecc::AEM_STATUS_SUCCESS && resp.size > hdr) {
        span_load_padded(desc, std::span<uint8_t const>{buf.data() + hdr, resp.size - hdr});
    }
    return desc;
}

}  // namespace

TEST(am824_io_create, valid_blob_succeeds)
{
    auto blob = load_file(simple2_bin_path());
    EXPECT_TRUE(blob.size() > 0);
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
}

TEST(am824_io_create, empty_blob_fails)
{
    auto config = make_config_with_blob({});
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_FALSE(result.has_value());
}

TEST(am824_io_create, invalid_blob_fails)
{
    std::vector<uint8_t> garbage(100, 0xFF);
    auto config = make_config_with_blob(std::move(garbage));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_FALSE(result.has_value());
}

TEST(am824_io_channels, extracts_8_channels)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((*result)->channels(), 8U);
}

// IEEE 802.1Q-2022 Clause 35.2.2.8.6: the Talker declares its port's latency: one MTU frame, then its own
// 32 + 12 x 8 x 4 = 416-octet MSDU (458 on the wire), 80 ns an octet at 100 Mbit/s.
TEST(am824_io_msrp, a_talker_advertise_declares_the_port_latency)
{
    auto config = make_config_with_blob(load_file(simple2_bin_path()));
    config.link_mbps = 100;
    config.mtu = 1500;
    auto const result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    if (!result.has_value()) {
        return;
    }
    auto const info = (*result)->talker_srp_info();
    EXPECT_EQ(info.max_frame_size, uint16_t{416});
    EXPECT_EQ(info.accumulated_latency, uint32_t{(1542 + 458) * 80});
}

TEST(am824_io_model, descriptor_counts)
{
    // The symbol-aware entity serves descriptors straight from the .aem blob, so
    // verify the fixture's shape via the DescriptorStorage (the source of truth).
    auto blob = load_file(simple2_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const count = [&](uint16_t type) -> size_t {
        size_t n = 0;
        while (storage->get_descriptor(0, type, static_cast<uint16_t>(n)).has_value()) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_CONFIGURATION), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_INPUT), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AVB_INTERFACE), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_CLOCK_SOURCE), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN), 1u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER), 2u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_MAP), 2u);
}

TEST(am824_io_model, entity_id_overridden)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto const wanted = Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    config.entity_id = wanted;
    config.entity_name = "Test Entity";
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    // on_get_entity patches entity_id from config; read it via the served path.
    EXPECT_EQ(served_entity(**result).entity_id, wanted);
}

TEST(am824_io_state, initial_state)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    EXPECT_FALSE((*result)->is_running());
    EXPECT_FALSE((*result)->is_ready());
    EXPECT_EQ((*result)->state_string(), "Start");
}

TEST(am824_io_filter, reconfigure_all_channels)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    (*result)->configure_filter(2000.0, -6.0, 1.0);
    EXPECT_EQ((*result)->channels(), 8U);
}

TEST(am824_io_audio, process_audio_callback_receives_correct_size)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    size_t callback_buffer_size = 0;
    size_t callback_sample_count = 0;
    (*result)->set_audio_callback([&](std::span<float> samples, size_t sample_count) {
        callback_buffer_size = samples.size();
        callback_sample_count = sample_count;
    });

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->process_audio(now, RAW_NOW);

    // Buffer should be SAMPLES_PER_PACKET * channels (6 * 8 = 48)
    EXPECT_EQ(callback_buffer_size, AvbEntityAm824IO::SAMPLES_PER_PACKET * 8U);
    EXPECT_EQ(callback_sample_count, AvbEntityAm824IO::SAMPLES_PER_PACKET);
}

TEST(am824_io_constants, sample_rate)
{
    // 96 kHz to match the descriptor model and the third-party devices endpoints.
    EXPECT_EQ(AvbEntityAm824IO::SAMPLE_RATE, 96000U);
}

TEST(am824_io_constants, samples_per_packet)
{
    // SR class A: sample_rate / 8000 packets-per-second = 96000/8000 = 12.
    EXPECT_EQ(AvbEntityAm824IO::SAMPLES_PER_PACKET, 12U);
}

//
// State Machine Transition Tests
//

TEST(am824_io_sm, link_up_transitions_state)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    EXPECT_EQ((*result)->state_string(), "Start");

    (*result)->on_link_up(now);
    EXPECT_TRUE((*result)->state_string() != "Start");
}

TEST(am824_io_sm, link_down_after_up)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->on_link_up(now);
    auto state_after_up = (*result)->state_string();
    (*result)->on_link_down(now);
    auto state_after_down = (*result)->state_string();
    EXPECT_TRUE(state_after_down != state_after_up || state_after_down == "Down");
}

TEST(am824_io_sm, timeout_does_not_crash)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->on_timeout(now);
    EXPECT_FALSE((*result)->is_running());
}

TEST(am824_io_sm, gptp_announce_no_grandmaster)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    // gPTP announce without grandmaster is a no-op (safe from any state)
    (*result)->on_gptp_announce(now, false);
    EXPECT_FALSE((*result)->is_running());
    EXPECT_EQ((*result)->state_string(), "Start");
}

TEST(am824_io_sm, multiple_link_toggles)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    for (int i = 0; i < 5; ++i) {
        (*result)->on_link_up(now);
        (*result)->on_link_down(now);
    }
    EXPECT_FALSE((*result)->is_running());
}

//
// Process Audio Tests
//

TEST(am824_io_audio, process_audio_does_not_crash)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->process_audio(now, RAW_NOW);
    EXPECT_FALSE((*result)->is_running());
}

TEST(am824_io_audio, process_audio_multiple_times)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    for (int i = 0; i < 10; ++i) {
        (*result)->process_audio(now, RAW_NOW);
    }
    EXPECT_FALSE((*result)->is_running());
}

//
// Component Access Tests
//

TEST(am824_io_components, components_mutable_access)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto& components = (*result)->components();
    // Mutable access: the ctor configured talker stream 0 on the ACMP talker.
    EXPECT_TRUE(components.acmp_talker.get_stream(0) != nullptr);
}

TEST(am824_io_components, net_handlers_null_before_start)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    EXPECT_TRUE((*result)->net_handlers() == nullptr);
}

TEST(am824_io_components, config_accessible)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    config.entity_name = "AM824 Test Entity";
    config.firmware_version = "2.5.0";
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    EXPECT_EQ((*result)->config().entity_name, "AM824 Test Entity");
    EXPECT_EQ((*result)->config().firmware_version, "2.5.0");
}

//
// Filter Tests
//

TEST(am824_io_filter, configure_with_various_params)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    (*result)->configure_filter(500.0, -6.0, 0.5);
    (*result)->configure_filter(2000.0, 6.0, 2.0);
    (*result)->configure_filter(10000.0, 0.0, 1.0);
    EXPECT_EQ((*result)->channels(), 8U);
}

//
// Print State Test
//

TEST(am824_io_print, print_state_does_not_crash)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    (*result)->print_state();
    EXPECT_FALSE((*result)->is_running());
}

//
// Stop Without Start Test
//

TEST(am824_io_lifecycle, stop_without_start_fails)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    auto stop_result = (*result)->stop();
    EXPECT_FALSE(stop_result.has_value());
}

//
// Entity Model Detail Tests
//

TEST(am824_io_model, entity_name_set_in_model)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    config.entity_name = "Named AM824";
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());
    // The config entity_id + name are served via on_get_entity (the served path).
    auto const desc = served_entity(**result);
    EXPECT_EQ(desc.entity_id, (Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}));
    EXPECT_EQ(std::string{desc.entity_name.as_string_view()}, std::string{"Named AM824"});
}

TEST(am824_io_model, clock_source_and_domain)
{
    // Served straight from the blob -> verify the fixture via DescriptorStorage.
    auto blob = load_file(simple2_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    EXPECT_TRUE(storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, 0).has_value());
    EXPECT_TRUE(storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, 0).has_value());
    EXPECT_FALSE(storage->get_descriptor(0, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, 1).has_value());
}

TEST(am824_io_model, audio_clusters_and_maps)
{
    auto blob = load_file(simple2_bin_path());
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto const count = [&](uint16_t type) -> size_t {
        size_t n = 0;
        while (storage->get_descriptor(0, type, static_cast<uint16_t>(n)).has_value()) {
            ++n;
        }
        return n;
    };
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER), 2u);
    EXPECT_EQ(count(atdecc::aem::DESCRIPTOR_AUDIO_MAP), 2u);
}

//
// Audio Callback With Filter Processing
//

TEST(am824_io_audio, callback_receives_filtered_data)
{
    auto blob = load_file(simple2_bin_path());
    auto config = make_config_with_blob(std::move(blob));
    config.filter_gain_db = 0.0;  // flat response
    auto result = AvbEntityAm824IO::create(std::move(config));
    EXPECT_TRUE(result.has_value());

    std::vector<float> captured_samples;
    (*result)->set_audio_callback([&](std::span<float> samples, size_t count) {
        captured_samples.assign(samples.begin(), samples.end());
        (void)count;
    });

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    (*result)->process_audio(now, RAW_NOW);

    // process_audio now drives a per-channel sine source through the (flat)
    // filter, so the callback receives a non-silent, bounded signal.
    EXPECT_EQ(captured_samples.size(), AvbEntityAm824IO::SAMPLES_PER_PACKET * 8U);
    float max_abs = 0.0f;
    for (auto s : captured_samples) {
        EXPECT_TRUE(s >= -1.0f && s <= 1.0f);  // bounded
        max_abs = std::max(max_abs, std::abs(s));
    }
    EXPECT_TRUE(max_abs > 0.0f);  // not silence

    // Each channel uses a distinct initial phase, so no two adjacent channels
    // carry the identical first sample.
    EXPECT_TRUE(captured_samples[0] != captured_samples[1]);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_am824_io_test)
