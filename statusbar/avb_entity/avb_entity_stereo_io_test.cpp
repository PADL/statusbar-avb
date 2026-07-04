// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Stereo I/O Unit Tests

#include "statusbar/avb_entity/avb_entity_stereo_io.hpp"

#include "statusbar/dsp/dsp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::avb_entity;
using namespace statusbar::ieee;

namespace {

/// Helper to check approximate equality for floats
constexpr bool approx_equal(float a, float b, float tolerance = 1e-5f)
{
    return std::abs(a - b) < tolerance;
}

}  // namespace

//
// Configuration Tests
//

TEST(stereo_io_config, default_values)
{
    AvbEntityStereoIOConfig config{};

    // Check default values
    EXPECT_EQ(config.vlan_id, 2);
    EXPECT_EQ(config.filter_freq_hz, 1000.0);
    EXPECT_EQ(config.filter_gain_db, -12.0);
    EXPECT_EQ(config.filter_q, 2.0);
    EXPECT_EQ(config.entity_name, "AVB Stereo IO");
    EXPECT_EQ(config.firmware_version, "1.0.0");
}

TEST(stereo_io_config, custom_mac)
{
    AvbEntityStereoIOConfig config{};
    config.talker_dest_mac = Eui48{0x71, 0xB3, 0xD5, 0xED, 0xCF, 0x00};

    EXPECT_EQ(config.talker_dest_mac.value[0], 0x71);
    EXPECT_EQ(config.talker_dest_mac.value[1], 0xB3);
    EXPECT_EQ(config.talker_dest_mac.value[2], 0xD5);
    EXPECT_EQ(config.talker_dest_mac.value[3], 0xED);
    EXPECT_EQ(config.talker_dest_mac.value[4], 0xCF);
    EXPECT_EQ(config.talker_dest_mac.value[5], 0x00);
}

//
// Entity Construction Tests
//

TEST(stereo_io_construct, basic_construction)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.interface_name = "en0";

    AvbEntityStereoIO entity{config};

    // Verify entity is not running after construction
    EXPECT_FALSE(entity.is_running());
    EXPECT_FALSE(entity.is_ready());
}

TEST(stereo_io_construct, entity_model_created)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    AvbEntityStereoIO entity{config};

    // Verify entity model was created with correct configuration
    auto const& components = entity.components();
    auto const& model = components.entity_model;

    EXPECT_EQ(model.configuration_count(), 1);
    EXPECT_EQ(model.stream_input_count(), 1);
    EXPECT_EQ(model.stream_output_count(), 1);
    EXPECT_EQ(model.avb_interface_count(), 1);
}

TEST(stereo_io_construct, entity_descriptor_values)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    config.entity_model_id = Eui64{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    config.entity_name = "Test Entity";
    config.firmware_version = "2.0.0";

    AvbEntityStereoIO entity{config};

    auto const& desc = entity.components().entity_model.get_entity();

    EXPECT_EQ(desc.entity_id, config.entity_id);
    EXPECT_EQ(desc.entity_model_id, config.entity_model_id);
    EXPECT_EQ(desc.talker_stream_sources.get(), 1);
    EXPECT_EQ(desc.listener_stream_sinks.get(), 1);
}

//
// State Query Tests
//

TEST(stereo_io_state, initial_state_string)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};

    // Initial state should be Start
    auto state = entity.state_string();
    EXPECT_EQ(state, "Start");
}

//
// DSP Filter Tests
//

TEST(stereo_io_filter, initial_filter_config)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.filter_freq_hz = 1000.0;
    config.filter_gain_db = -12.0;
    config.filter_q = 2.0;

    AvbEntityStereoIO entity{config};

    // Filter should be configured (we can't directly test coefficients,
    // but we verify it doesn't throw during construction)
    EXPECT_FALSE(entity.is_running());
}

TEST(stereo_io_filter, reconfigure_filter)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};

    // Reconfigure filter with different parameters
    entity.configure_filter(2000.0, -6.0, 1.0);

    // Should not throw and entity should still be valid
    EXPECT_FALSE(entity.is_running());
}

//
// BiQuad Filter Coefficient Tests (standalone)
//

TEST(biquad_filter, peak_cut_coefficients)
{
    dsp::BiQuad<float> filter;

    // Configure: 12 dB cut at 1 kHz, Q=2, 48 kHz sample rate
    filter.coeffs.calculate_peak(
        {.sample_rate_recip = 1.0 / 48000.0, .frequency = 1000.0, .q = 2.0, .gain_db = -12.0, .channel = 0});

    // Filter should pass DC (a0 + a1 + a2 should be close to 1 + b1 + b2)
    // For a peak filter with cut, the DC gain should be unity
    // Test by processing a DC signal
    filter.reset();

    float dc_input = 1.0f;
    float output = dc_input;
    for (int i = 0; i < 1000; ++i) {
        output = filter(dc_input);
    }

    // DC gain should be approximately 1.0 (unity)
    EXPECT_TRUE(approx_equal(output, 1.0f, 0.01f));
}

TEST(biquad_filter, peak_boost_coefficients)
{
    dsp::BiQuad<float> filter;

    // Configure: 6 dB boost at 1 kHz, Q=2, 48 kHz sample rate
    filter.coeffs.calculate_peak({.sample_rate_recip = 1.0 / 48000.0, .frequency = 1000.0, .q = 2.0, .gain_db = 6.0, .channel = 0});

    // Test DC gain (should be unity for peak filter)
    filter.reset();

    float dc_input = 1.0f;
    float output = dc_input;
    for (int i = 0; i < 1000; ++i) {
        output = filter(dc_input);
    }

    // DC gain should be approximately 1.0 (unity)
    EXPECT_TRUE(approx_equal(output, 1.0f, 0.01f));
}

TEST(biquad_filter, stereo_processing)
{
    dsp::BiQuad<float> biquad_left;
    dsp::BiQuad<float> biquad_right;

    // Configure both filters identically
    biquad_left.coeffs.calculate_peak(
        {.sample_rate_recip = 1.0 / 48000.0, .frequency = 1000.0, .q = 2.0, .gain_db = -12.0, .channel = 0});
    biquad_right.coeffs.calculate_peak(
        {.sample_rate_recip = 1.0 / 48000.0, .frequency = 1000.0, .q = 2.0, .gain_db = -12.0, .channel = 0});

    biquad_left.reset();
    biquad_right.reset();

    // Process identical signals - should produce identical outputs
    constexpr size_t SAMPLES = 6;
    std::array<float, SAMPLES * 2> buffer{};

    // Fill with DC signal
    for (size_t i = 0; i < SAMPLES; ++i) {
        buffer[i * 2] = 0.5f;        // Left
        buffer[(i * 2) + 1] = 0.5f;  // Right
    }

    // Process
    for (size_t i = 0; i < SAMPLES; ++i) {
        buffer[i * 2] = biquad_left(buffer[i * 2]);
        buffer[(i * 2) + 1] = biquad_right(buffer[(i * 2) + 1]);
    }

    // Left and right should be equal
    for (size_t i = 0; i < SAMPLES; ++i) {
        EXPECT_TRUE(approx_equal(buffer[i * 2], buffer[(i * 2) + 1], 1e-6f));
    }
}

//
// Audio Callback Tests
//

TEST(stereo_io_callback, set_callback)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};

    bool callback_called = false;
    size_t callback_sample_count = 0;

    entity.set_audio_callback([&](std::span<float> samples, size_t sample_count) {
        callback_called = true;
        callback_sample_count = sample_count;
        (void)samples;
    });

    // The callback is called during process_audio() which requires running entity
    // Here we just verify the callback can be set
    EXPECT_FALSE(callback_called);  // Not called yet
}

//
// Constants Tests
//

TEST(stereo_io_constants, sample_rate)
{
    EXPECT_EQ(AvbEntityStereoIO::SAMPLE_RATE, 48000U);
}

TEST(stereo_io_constants, channels)
{
    EXPECT_EQ(AvbEntityStereoIO::CHANNELS, 2U);
}

TEST(stereo_io_constants, samples_per_packet)
{
    EXPECT_EQ(AvbEntityStereoIO::SAMPLES_PER_PACKET, 6U);
}

//
// State Machine Transition Tests
//

TEST(stereo_io_sm, link_up_transitions_from_start)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};

    // Initial state is Start
    EXPECT_EQ(entity.state_string(), "Start");

    // link_up should trigger a transition
    entity.on_link_up(now);
    EXPECT_TRUE(entity.state_string() != "Start");
}

TEST(stereo_io_sm, link_down_after_up)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};

    entity.on_link_up(now);
    auto state_after_up = entity.state_string();
    entity.on_link_down(now);
    // After link_down, state should change (typically back to Down)
    auto state_after_down = entity.state_string();
    EXPECT_TRUE(state_after_down != state_after_up || state_after_down == "Down");
}

TEST(stereo_io_sm, timeout_does_not_crash)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};

    // on_timeout should not crash regardless of current state
    entity.on_timeout(now);
    EXPECT_FALSE(entity.is_running());
}

TEST(stereo_io_sm, gptp_announce_no_grandmaster)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};

    // gPTP announce without grandmaster is a no-op (safe from any state)
    entity.on_gptp_announce(now, false);
    EXPECT_FALSE(entity.is_running());
    EXPECT_EQ(entity.state_string(), "Start");
}

//
// Process Audio Tests (without running entity)
//

TEST(stereo_io_audio, process_audio_does_not_crash)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};

    // Process audio without starting entity - should not crash
    entity.process_audio(now);
    EXPECT_FALSE(entity.is_running());
}

TEST(stereo_io_audio, process_audio_no_input_no_callback)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};

    // The loopback is input-driven: the audio callback (and the DSP) run only for
    // blocks the RX path published to the compensation pipe and whose presentation
    // time is due. With no received audio the pipe is empty, so process_audio must
    // do nothing -- the callback never fires.
    bool callback_invoked = false;
    entity.set_audio_callback([&](std::span<float> /*samples*/, size_t /*count*/) { callback_invoked = true; });

    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.process_audio(now);

    EXPECT_FALSE(callback_invoked);
}

//
// Component Access Tests
//

TEST(stereo_io_components, components_accessible)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

    AvbEntityStereoIO entity{config};
    auto const& components = entity.components();

    // Entity model should be configured
    EXPECT_EQ(components.entity_model.stream_input_count(), 1);
    EXPECT_EQ(components.entity_model.stream_output_count(), 1);
}

TEST(stereo_io_components, net_handlers_null_before_start)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    EXPECT_TRUE(entity.net_handlers() == nullptr);
}

TEST(stereo_io_components, config_accessible)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_name = "Test Config Access";
    config.firmware_version = "3.0.0";

    AvbEntityStereoIO entity{config};
    EXPECT_EQ(entity.config().entity_name, "Test Config Access");
    EXPECT_EQ(entity.config().firmware_version, "3.0.0");
}

//
// Filter Reconfiguration Tests
//

TEST(stereo_io_filter, reconfigure_multiple_times)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};

    // Reconfigure several times with different parameters
    entity.configure_filter(500.0, -6.0, 0.5);
    entity.configure_filter(2000.0, 6.0, 2.0);
    entity.configure_filter(10000.0, 0.0, 1.0);

    // Verify entity is still valid
    EXPECT_FALSE(entity.is_running());
}

TEST(stereo_io_filter, bypass_with_zero_gain)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.filter_gain_db = 0.0;

    AvbEntityStereoIO entity{config};

    // With 0 dB gain, the filter should pass through (unity gain at all freqs)
    EXPECT_FALSE(entity.is_running());
}

//
// Print State Test
//

TEST(stereo_io_print, print_state_does_not_crash)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    entity.print_state();
    // No crash is the test
    EXPECT_FALSE(entity.is_running());
}

//
// Stop Without Start Test
//

TEST(stereo_io_lifecycle, stop_without_start_fails)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto result = entity.stop();
    EXPECT_FALSE(result.has_value());
}

//
// Entity Model Stream Descriptor Tests
//

TEST(stereo_io_model, stream_descriptors_present)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    config.entity_model_id = Eui64{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

    AvbEntityStereoIO entity{config};
    auto const& model = entity.components().entity_model;

    // Verify configuration, stream, and interface descriptors
    EXPECT_EQ(model.configuration_count(), 1);
    EXPECT_EQ(model.stream_input_count(), 1);
    EXPECT_EQ(model.stream_output_count(), 1);
    EXPECT_EQ(model.avb_interface_count(), 1);
}

//
// Multiple Link Toggles
//

TEST(stereo_io_sm, multiple_link_toggles)
{
    AvbEntityStereoIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    AvbEntityStereoIO entity{config};
    auto const now = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};

    // Toggle link multiple times
    for (int i = 0; i < 5; ++i) {
        entity.on_link_up(now);
        entity.on_link_down(now);
    }
    // Should not crash
    EXPECT_FALSE(entity.is_running());
}

//
// Test Runner
//

TEST_MAIN(statusbar_avb_entity, avb_entity_stereo_io_test)