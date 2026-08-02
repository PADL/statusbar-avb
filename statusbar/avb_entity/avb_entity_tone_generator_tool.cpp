// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AVB Entity Tone Generator Tool — talker-only AM824 + AAF + CRF
//
// Runs an AVB entity with THREE talker stream sources and no listeners:
//   stream 0 AM824, stream 1 AAF (32-bit PCM), stream 2 CRF (Milan 48 kHz media
//   clock). Each audio channel carries a continuous sine; by default the 8
//   channels are the white piano keys C4..C5. The media clock is locked to gPTP
//   (r = 1.0). No inter-site tunnel, no listener.
//
// Usage: statusbar-avb-tone-generator --interface=eth0 --descriptor-storage=entity_tone.bin
//   (the blob must declare 0 stream inputs + 3 stream outputs; build with
//    `statusbar-aemxml json2bin examples/tone.json entity_tone.bin`)
//
// The main loop, common CLI arguments, PTP setup and telemetry all come
// from the shared kit runner (avb_entity_runner.hpp, kit phase 5d) — this
// file keeps only the tone-specific pieces.

#include "statusbar/avb_entity/avb_entity_identity.hpp"
#include "statusbar/avb_entity/avb_entity_runner.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avb_entity/avb_entity_version.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <print>
#include <string>

namespace {

using namespace statusbar;
using Entity = avb_entity::AvbEntityToneGenerator;

/// Note names for the 12 chromatic pitch classes (display only).
constexpr std::array<std::string_view, 12> kNoteNames{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

[[nodiscard]] auto note_name(uint8_t midi) -> std::string
{
    int const octave = (static_cast<int>(midi) / 12) - 1;  // MIDI 60 = C4
    return std::string{kNoteNames[static_cast<size_t>(midi % 12)]} + std::to_string(octave);
}

struct Config
{
    ptpclient::PtpAppConfig ptp_app;
    avb_entity::EntityToolOptions tool;

    avb_entity::AvbEntityAudioIOConfig entity{
        .entity_id = ieee::Eui64{},
        // Well-formed EUI-64 model id: OUI-24 70:b3:d5 + FF:FE insertion + device
        // bits (NOT OUI-36 bits jammed in right after the OUI-24, which is malformed).
        .entity_model_id = ieee::Eui64{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0xC0, 0x00},
        // JDKS OUI-36 multicast (NOT the 91:E0:F0 MAAP pool). CRF = top of range.
        .am824_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFD},
        .aaf_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFE},
        .crf_talker_dest_mac = {0x71, 0xB3, 0xD5, 0xED, 0xCF, 0xFF},
        .vlan_id = 2,
        // The media clock is always gPTP-locked (r = 1.0) for the tone generator.
        .media_lock_to_gptp = true,
        .tone_amplitude = 0.1F,  // -20 dBFS
        .entity_name = "",
        .firmware_version = std::string{statusbar::avb_entity::build_version},
    };

    /// Lowest white-key tone (MIDI note number). Default 60 = C4; each channel
    /// takes the next white key up, so 8 channels = C4..C5.
    uint8_t base_midi_note{avb_entity::TONE_DEFAULT_BASE_MIDI_NOTE};

    /// Stream set: "all" = AM824 + AAF + CRF (3 outputs); "aaf" = a single AAF
    /// 8-ch stream (diagnostic — a clean device for listeners that stall on the
    /// mixed AM824+AAF aggregate). "aaf" auto-selects the entity_tone_aaf.bin blob.
    std::string streams_mode{"all"};
};

auto build_arg_specs(Config& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    ptpclient::add_ptp_arg_specs(specs, config.ptp_app);
    avb_entity::add_common_entity_arg_specs(specs, config.entity, config.tool);

    specs.add<double>(
        "tone.amplitude",
        "Per-channel tone amplitude, 0..1 linear (1.0 = 0 dBFS; default 0.1 = -20 dBFS)",
        static_cast<double>(config.entity.tone_amplitude),
        [&](auto v) { config.entity.tone_amplitude = static_cast<float>(v); });
    specs.add<uint64_t>(
        "tone.base_note",
        "MIDI note number of the lowest channel's white key (default 60 = C4). Each channel takes the next white "
        "key up, so 8 channels span C4..C5.",
        static_cast<uint64_t>(config.base_midi_note),
        [&](auto v) { config.base_midi_note = static_cast<uint8_t>(v & 0x7FULL); });
    specs.add_choice(
        "streams",
        "Stream set: 'all' = AM824 + AAF + CRF (3 outputs); 'aaf' = a single 8-ch AAF stream; 'aaf+crf' = AAF audio "
        "+ CRF media clock (clean 8-ch device WITH a clock reference for the listener). 'aaf'/'aaf+crf' auto-select "
        "their blob unless --descriptor-storage is given",
        {"all", "aaf", "aaf+crf"},
        "all",
        [&](auto v) { config.streams_mode = std::string{v}; });

    return specs;
}

void print_usage(char const* program_name, args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --descriptor-storage=entity_tone.bin",
        "--interface=eth0 --tone.base_note=60 --tone.amplitude=0.1",
    };
    config::default_print_usage(
        program_name,
        specs,
        "AVB Entity Tone Generator (talker-only AM824 + AAF + CRF)\n"
        "Transmits 8 channels of continuous sine tones (white piano keys C4..C5 by default) as both an AM824 and an\n"
        "AAF 96 kHz stream, plus a CRF media clock. The media clock is locked to gPTP (r = 1.0).",
        examples);
    std::print(stderr, "\nGenerate the blob with: statusbar-aemxml json2bin examples/tone.json entity_tone.bin\n");
    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

void print_entity_config(Config const& config, Entity const& entity)
{
    auto const& ecfg = entity.config();
    std::print("AVB Entity Tone Generator (talker-only)\n");
    std::print("=======================================\n");
    std::print("Descriptor Storage: {}\n", config.tool.descriptor_storage_path);
    std::print("Entity ID:        {}\n", ieee::to_string(ecfg.entity_id).view());
    std::print("Entity Name:      {}\n", ecfg.entity_name);
    std::print("Interface:        {}\n", ecfg.interface_name);
    std::print("VLAN ID:          {}\n", ecfg.vlan_id);
    std::print("Stream 0 (AM824): dest {}\n", ieee::to_string(ecfg.am824_talker_dest_mac).view());
    std::print("Stream 1 (AAF):   dest {}  (int32 PCM)\n", ieee::to_string(ecfg.aaf_talker_dest_mac).view());
    std::print(
        "Stream 2 (CRF):   dest {}  (48 kHz media clock, {}x{} ts/pkt)\n",
        ieee::to_string(ecfg.crf_talker_dest_mac).view(),
        ecfg.crf_timestamps_per_packet,
        ecfg.crf_timestamp_interval);
    std::print(
        "Channels:         {}  (tone level {:.3f} = {:+.1f} dBFS)\n",
        entity.channels(),
        ecfg.tone_amplitude,
        ecfg.tone_amplitude > 0.0F ? 20.0 * std::log10(static_cast<double>(ecfg.tone_amplitude)) : -120.0);
    for (size_t ch = 0; ch < entity.channels(); ++ch) {
        auto const midi =
            static_cast<uint8_t>(config.base_midi_note + ((ch / 7) * 12) + std::array<int, 7>{0, 2, 4, 5, 7, 9, 11}[ch % 7]);
        std::print(
            "  ch {}: {:<4} {:8.3f} Hz\n", ch, note_name(midi), avb_entity::white_key_frequency_hz(config.base_midi_note, ch));
    }
    std::print(
        "Sample Rate:      {} Hz, Samples/Packet: {}\n",
        entity.sample_rate(),
        entity.sample_rate() / Entity::CLASS_A_PACKETS_PER_SEC);
    std::print("Media clock rate: r=1.0 PINNED to gPTP\n");
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    using namespace statusbar;

    Config config;
#ifdef STATUSBAR_AVB_DEFAULT_TONE_BLOB
    config.tool.descriptor_storage_path = STATUSBAR_AVB_DEFAULT_TONE_BLOB;
#endif
    auto specs = build_arg_specs(config);

    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-avb-tone-generator");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.entity.interface_name.empty()) {
        std::print(stderr, "Error: --interface=<iface> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }
    // --streams only picks the matching default blob (unless the user gave an
    // explicit --descriptor-storage path); the entity's stream topology is
    // derived FROM the blob (kit phase 1), not from a C++ stream-set enum.
#ifdef STATUSBAR_AVB_DEFAULT_TONE_BLOB
    if (config.tool.descriptor_storage_path == STATUSBAR_AVB_DEFAULT_TONE_BLOB) {
#    ifdef STATUSBAR_AVB_DEFAULT_TONE_AAF_BLOB
        if (config.streams_mode == "aaf") {
            config.tool.descriptor_storage_path = STATUSBAR_AVB_DEFAULT_TONE_AAF_BLOB;
        }
#    endif
#    ifdef STATUSBAR_AVB_DEFAULT_TONE_AAF_CRF_BLOB
        if (config.streams_mode == "aaf+crf") {
            config.tool.descriptor_storage_path = STATUSBAR_AVB_DEFAULT_TONE_AAF_CRF_BLOB;
        }
#    endif
    }
#endif
    if (config.tool.descriptor_storage_path.empty()) {
        std::print(stderr, "Error: --descriptor-storage=<path> is required\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    avb_entity::apply_node_identity_defaults(
        config.entity.interface_name,
        config.entity.entity_id,
        config.entity.entity_name,
        ieee::Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x03},
        "Statusbar Tone Generator");

    auto blob = avb_entity::load_blob_file(config.tool.descriptor_storage_path);
    if (blob.empty()) {
        std::print(stderr, "Error: Failed to load descriptor storage file: {}\n", config.tool.descriptor_storage_path);
        return EXIT_FAILURE;
    }
    config.entity.descriptor_storage_blob = std::move(blob);

    realtime::setup_shutdown_signal_handlers();

    auto const base_midi_note = config.base_midi_note;
    auto entity_result = avb_entity::AvbEntityToneGenerator::create(std::move(config.entity), base_midi_note);
    if (!entity_result) {
        std::print(stderr, "Error: Failed to create entity: {}\n", entity_result.error().message());
        return EXIT_FAILURE;
    }
    auto& entity = **entity_result;

    print_entity_config(config, entity);

    std::string ptp_summary;
    ptpclient::format_ptp_config_to(std::back_inserter(ptp_summary), config.ptp_app, "\nPTP Configuration");
    std::print("{}\n", ptp_summary);

    itc::StopToken shutdown_flag{};
    net::MessageReactor reactor{shutdown_flag, net::monotonic_ns, 10};

    auto start_result = entity.start(reactor);
    if (!start_result) {
        std::print(stderr, "Error: Failed to start entity: {}\n", start_result.error().message());
        return EXIT_FAILURE;
    }
    std::print("\nEntity started. Network handlers registered.\n");

    auto ctx_result = avb_entity::setup_entity_ptp(config.ptp_app, config.tool.require_ptp);
    if (!ctx_result) {
        (void)entity.stop();
        if (realtime::is_shutdown_requested()) {
            std::print("Shutdown requested during setup\n");
            return EXIT_SUCCESS;
        }
        std::print(stderr, "Error: Failed to setup PTP: {}\n", ctx_result.error().message());
        return EXIT_FAILURE;
    }
    auto& ctx = *ctx_result;
    std::print("\nStarting main loop (Ctrl-C to stop)...\n\n");

    // One wake per Class A packet interval (125 us), scaled by packets_per_wake;
    // the rate is blob-derived so the period comes from the entity.
    int64_t const packet_period_ns =
        (1'000'000'000LL / Entity::CLASS_A_PACKETS_PER_SEC) * static_cast<int64_t>(entity.config().packets_per_wake);
    auto loop_result = avb_entity::run_entity_main_loop(reactor, ctx, entity, config.tool, packet_period_ns);

    std::print("\n\nShutting down...\n");
    auto stop_time = sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    entity.on_link_down(stop_time);
    (void)entity.stop();
    std::print("Entity stopped.\n");

    avb_entity::print_final_status(entity, loop_result, config.tool.dump_stats_on_exit);
    return loop_result.exit_code;
}
