// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// NanoAVB Example - Demonstrates nanoavb module components with state machine integration
//
// Usage: nanoavb_example [options]
//
// On Linux with hardware PTP:
//   nanoavb_example --ptp.driver=linuxptp --ptp.device=/dev/ptp0
//
// On any platform (uses system clock):
//   nanoavb_example --ptp.driver=system
//
// Instantiates EntityModel, ADP Advertiser, ACMP Talker/Listener, MVRP/MSRP handlers
// with periodic tick loop and optional PTP timer for realtime audio packet handling.
// State machines coordinate protocol lifecycle and stream engine control.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/gptp/gptp_format.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/itc/itc_telemetry_counter.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/ptpclient/ptpclient.hpp"
#include "statusbar/realtime/realtime.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/status.hpp"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <system_error>

namespace {

// Config contains PTP fields via composition, adds nanoavb-specific fields
struct Config
{
    statusbar::ptpclient::PtpAppConfig ptp_app;
    std::string interface_name;  // no default — --interface is required
};

//
// Global State
//

// Use realtime module's shutdown handling

//
// State Machine Supervisor
//

/// Supervised NanoAVB - integrates state machines with components
/// The supervisor state machine coordinates:
///   - Link up/down detection
///   - gPTP synchronization status
///   - VLAN base registration
///   - Protocol lifecycle (MVRP, MSRP, ADP, ACMP)
///   - Stream engine control (talker/listener)
///
/// Error Recovery:
///   The supervisor tracks recovery attempts and can automatically retry
///   protocol initialization after transient failures (gPTP timeout, VLAN
///   registration failure). After MAX_RECOVERY_ATTEMPTS, it stays in the
///   degraded state and waits for manual intervention or link cycle.
struct SupervisedNanoAvb
{
    using TimePoint = statusbar::sm::TimePoint;

    // Error recovery configuration
    static constexpr int MAX_RECOVERY_ATTEMPTS = 3;
    static constexpr auto RECOVERY_DELAY = std::chrono::seconds{2};

    // Recovery state
    int recovery_attempts{0};
    TimePoint last_failure_time{};

    // Protocol state machines
    statusbar::nanoavb::supervisor_sm::Context supervisor_ctx{};
    statusbar::nanoavb::supervisor_sm::Machine supervisor{};

    statusbar::nanoavb::gptp_sm::Context gptp_ctx{};
    statusbar::nanoavb::gptp_sm::Machine gptp{};

    statusbar::nanoavb::mvrp_sm::Context mvrp_ctx{};
    statusbar::nanoavb::mvrp_sm::Machine mvrp{};

    statusbar::nanoavb::msrp_talker_sm::Context msrp_talker_ctx{};
    statusbar::nanoavb::msrp_talker_sm::Machine msrp_talker{};

    statusbar::nanoavb::msrp_listener_sm::Context msrp_listener_ctx{};
    statusbar::nanoavb::msrp_listener_sm::Machine msrp_listener{};

    statusbar::nanoavb::talker_engine_sm::Context talker_engine_ctx{};
    statusbar::nanoavb::talker_engine_sm::Machine talker_engine{};

    statusbar::nanoavb::listener_engine_sm::Context listener_engine_ctx{};
    statusbar::nanoavb::listener_engine_sm::Machine listener_engine{};

    /// Stop the talker (with @p talker_event) and listener engines and close their gates.
    void close_stream_gates(TimePoint const time, statusbar::nanoavb::talker_engine_sm::Def::Event const talker_event)
    {
        talker_engine.handle_event(talker_engine_ctx, talker_event, time);
        listener_engine.handle_event(listener_engine_ctx, statusbar::nanoavb::listener_engine_sm::Def::Event::GateStop, time);
        talker_engine_ctx.send_allowed = false;
        listener_engine_ctx.play_allowed = false;
    }

    /// Reset the MVRP and MSRP state machines.
    void reset_srp()
    {
        mvrp.reset();
        msrp_talker.reset();
        msrp_listener.reset();
    }

    /// Wire callbacks to connect state machines with components and each other
    void wire_callbacks(statusbar::nanoavb::NanoAvbComponents& components, statusbar::nanoavb::NanoAvbNetHandlers& handlers)
    {  // NOLINT(statusbar-block-statements): one callback per state machine hook
        //
        // Supervisor callbacks - coordinate protocol lifecycle
        //

        supervisor_ctx.callbacks.init_iface = [this, &components](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[supervisor] init_iface: Initializing NIC and protocol engines\n");
            // Reset protocol state machines to initial state
            gptp.reset();
            mvrp.reset();
            // Reset component state if needed
            (void)components;
        };

        supervisor_ctx.callbacks.start_protocols = [this, &components](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[supervisor] start_protocols: Starting gPTP + ADP (SRP is gated on gPTP lock)\n");
            // gPTP runs independently to acquire lock; ADP advertises immediately
            // (discovery/enumeration is independent of gPTP/SRP). SRP (MVRP+MSRP)
            // is started in enter_ready() once gPTP is locked.
            gptp.handle_event(gptp_ctx, statusbar::nanoavb::gptp_sm::Def::Event::AsCapableUp, time);
            auto result = components.adp_advertiser.start(time);
            if (!result) {
                std::print(stderr, "Warning: ADP start failed: {}\n", result.error().message());
            }
        };

        supervisor_ctx.callbacks.enter_ready = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[supervisor] enter_ready: gPTP locked - starting SRP, enabling streams\n");
            // gPTP locked: bring up SRP (MVRP best-effort) and enable streaming.
            mvrp.handle_event(mvrp_ctx, statusbar::nanoavb::mvrp_sm::Def::Event::Acquire, time);
            talker_engine_ctx.send_allowed = true;
            listener_engine_ctx.play_allowed = true;
        };

        supervisor_ctx.callbacks.degrade_stop_streams = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[supervisor] degrade_stop_streams: gPTP lost - stopping SRP + streams\n");
            close_stream_gates(time, statusbar::nanoavb::talker_engine_sm::Def::Event::GateStop);
            reset_srp();
        };

        supervisor_ctx.callbacks.stop_all = [this, &components](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[supervisor] stop_all: Stopping all protocols and engines\n");
            // Stop ADP
            (void)components.adp_advertiser.stop(time);
            // Stop stream engines, then reset the protocol state machines
            close_stream_gates(time, statusbar::nanoavb::talker_engine_sm::Def::Event::Fatal);
            gptp.reset();
            reset_srp();
        };

        // Timeout handlers - called when state machine times out in a waiting state
        supervisor_ctx.callbacks.timeout_gptp = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print(stderr, "[supervisor] timeout_gptp: gPTP lock timeout after waiting\n");
            last_failure_time = time;
            ++recovery_attempts;
            if (recovery_attempts < MAX_RECOVERY_ATTEMPTS) {
                std::print("[supervisor] Will attempt recovery ({}/{})\n", recovery_attempts, MAX_RECOVERY_ATTEMPTS);
            } else {
                std::print(stderr, "[supervisor] Max recovery attempts reached, manual intervention required\n");
            }
        };

        //
        // gPTP callbacks - notify supervisor of sync status
        //

        gptp_ctx.callbacks.init = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        gptp_ctx.callbacks.start_servo = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[gptp] start_servo: Beginning gPTP synchronization\n");
        };

        gptp_ctx.callbacks.report_locked = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[gptp] report_locked: gPTP synchronized\n");
            // Notify supervisor that gPTP is locked
            supervisor.handle_event(supervisor_ctx, statusbar::nanoavb::supervisor_sm::Def::Event::GptpLocked, time);
        };

        gptp_ctx.callbacks.report_unlocked = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[gptp] report_unlocked: gPTP synchronization lost\n");
            // Notify supervisor that gPTP is lost
            supervisor.handle_event(supervisor_ctx, statusbar::nanoavb::supervisor_sm::Def::Event::GptpLost, time);
        };

        //
        // MVRP callbacks - notify supervisor of VLAN registration
        //

        mvrp_ctx.callbacks.init = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        mvrp_ctx.callbacks.send_join = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[mvrp] send_join: Registering VLANs\n");
        };

        mvrp_ctx.callbacks.mark_joined = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[mvrp] mark_joined: VLANs registered (best-effort; streaming is gated on gPTP)\n");
        };

        mvrp_ctx.callbacks.mark_error = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print(stderr, "[mvrp] mark_error: VLAN registration failed\n");
            last_failure_time = time;
            ++recovery_attempts;
            if (recovery_attempts < MAX_RECOVERY_ATTEMPTS) {
                std::print("[mvrp] Will attempt recovery ({}/{})\n", recovery_attempts, MAX_RECOVERY_ATTEMPTS);
            }
        };

        mvrp_ctx.callbacks.send_leave = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[mvrp] send_leave: Leaving VLAN\n");
        };

        mvrp_ctx.callbacks.mark_left = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[mvrp] mark_left: Left VLAN\n");
        };

        mvrp_ctx.callbacks.reset = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[mvrp] reset: Resetting MVRP state\n");
        };

        //
        // MSRP Talker callbacks - stream reservation
        //

        msrp_talker_ctx.callbacks.init = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        msrp_talker_ctx.callbacks.msrp_talker_advertise = [&components](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[msrp_talker] advertise: Advertising talker stream\n");
            // Use MSRP handler to advertise stream
            (void)components.msrp_handler;
        };

        msrp_talker_ctx.callbacks.mark_ready = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[msrp_talker] mark_ready: Stream reservation ready\n");
            // Can now start talker engine if supervisor is ready
            if (supervisor.current_state() == statusbar::nanoavb::supervisor_sm::Def::State::Ready) {
                talker_engine.handle_event(talker_engine_ctx, statusbar::nanoavb::talker_engine_sm::Def::Event::AudioReady, time);
            }
        };

        msrp_talker_ctx.callbacks.mark_failed = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[msrp_talker] mark_failed: Stream reservation failed\n");
        };

        msrp_talker_ctx.callbacks.msrp_talker_withdraw = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[msrp_talker] withdraw: Withdrawing talker stream\n");
        };

        msrp_talker_ctx.callbacks.mark_idle = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        //
        // MSRP Listener callbacks - stream reservation
        //

        msrp_listener_ctx.callbacks.init = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        msrp_listener_ctx.callbacks.msrp_listener_ready = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[msrp_listener] ready: Joining stream\n");
        };

        msrp_listener_ctx.callbacks.mark_ready = [this](auto& ctx, TimePoint time) {
            (void)ctx;
            std::print("[msrp_listener] mark_ready: Stream reservation ready\n");
            // Can now start listener engine if supervisor is ready
            if (supervisor.current_state() == statusbar::nanoavb::supervisor_sm::Def::State::Ready) {
                listener_engine.handle_event(
                    listener_engine_ctx, statusbar::nanoavb::listener_engine_sm::Def::Event::GateListen, time);
            }
        };

        msrp_listener_ctx.callbacks.mark_failed = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[msrp_listener] mark_failed: Stream reservation failed\n");
        };

        msrp_listener_ctx.callbacks.msrp_listener_leave = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[msrp_listener] leave: Leaving stream\n");
        };

        msrp_listener_ctx.callbacks.mark_idle = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        //
        // Talker Engine callbacks - audio TX pipeline
        //

        talker_engine_ctx.callbacks.init = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        talker_engine_ctx.callbacks.start_audio_source = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] start_audio_source: Opening audio input\n");
        };

        talker_engine_ctx.callbacks.arm_stream = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] arm_stream: Stream armed, ready to transmit\n");
        };

        talker_engine_ctx.callbacks.start_tx = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] start_tx: Beginning AVTP transmission\n");
        };

        talker_engine_ctx.callbacks.stop_tx = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] stop_tx: Stopping transmission\n");
        };

        talker_engine_ctx.callbacks.mute_tx = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] mute_tx: Muting transmission\n");
        };

        talker_engine_ctx.callbacks.unmute_tx = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] unmute_tx: Unmuting transmission\n");
        };

        talker_engine_ctx.callbacks.stop_all = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[talker_engine] stop_all: Stopping audio and TX\n");
        };

        //
        // Listener Engine callbacks - audio RX pipeline
        //

        listener_engine_ctx.callbacks.init = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
        };

        listener_engine_ctx.callbacks.enable_rx_filter = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] enable_rx_filter: Setting up RX filters\n");
        };

        listener_engine_ctx.callbacks.start_sync = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] start_sync: Beginning dejitter buffer fill\n");
        };

        listener_engine_ctx.callbacks.start_audio_sink = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] start_audio_sink: Opening audio output\n");
        };

        listener_engine_ctx.callbacks.stop_all = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] stop_all: Stopping RX and audio\n");
        };

        listener_engine_ctx.callbacks.resync = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] resync: Resynchronizing\n");
        };

        listener_engine_ctx.callbacks.mute_out = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] mute_out: Muting output\n");
        };

        listener_engine_ctx.callbacks.unmute_out = [](auto& ctx, TimePoint time) {
            (void)ctx;
            (void)time;
            std::print("[listener_engine] unmute_out: Unmuting output\n");
        };
    }

    /// Handle link up event from network layer
    void on_link_up(TimePoint time)
    {
        std::print("[event] Link Up detected\n");
        supervisor_ctx.link_up = true;
        supervisor.handle_event(supervisor_ctx, statusbar::nanoavb::supervisor_sm::Def::Event::LinkUp, time);
    }

    /// Handle link down event from network layer
    void on_link_down(TimePoint time)
    {
        std::print("[event] Link Down detected\n");
        supervisor_ctx.link_up = false;
        supervisor.handle_event(supervisor_ctx, statusbar::nanoavb::supervisor_sm::Def::Event::LinkDown, time);
    }

    /// Handle gPTP announce received - check for sync status
    void on_gptp_announce(TimePoint time, bool has_grandmaster)
    {
        if (has_grandmaster && !gptp_ctx.time_locked) {
            // First grandmaster seen - gPTP is now locked
            gptp.handle_event(gptp_ctx, statusbar::nanoavb::gptp_sm::Def::Event::LockedStable, time);
        }
    }

    /// Handle timeout event from watchdog timer
    /// Call this periodically while waiting for gPTP lock (Init).
    void on_timeout(TimePoint time)
    {
        using statusbar::nanoavb::supervisor_sm::Def;
        auto const state = supervisor.current_state();

        // Only Init has a watchdog (gPTP-lock timeout); there is no VLAN gate.
        if (state == Def::State::Init) {
            std::print("[watchdog] Timeout in state Init (gPTP lock)\n");
            supervisor.handle_event(supervisor_ctx, Def::Event::Timeout, time);
        }
    }

    /// Attempt recovery after a failure
    /// Returns true if recovery was attempted, false if max attempts reached
    bool attempt_recovery(TimePoint time)
    {
        if (recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
            return false;
        }

        // Check if enough time has passed since last failure
        auto const elapsed = time - last_failure_time;
        if (elapsed < std::chrono::duration_cast<TimePoint::duration>(RECOVERY_DELAY)) {
            return false;  // Wait before retrying
        }

        std::print("[recovery] Attempting protocol restart (attempt {}/{})\n", recovery_attempts + 1, MAX_RECOVERY_ATTEMPTS);

        // Simulate link cycle to restart protocols
        on_link_down(time);
        on_link_up(time);

        return true;
    }

    /// Reset recovery state (call when system reaches Ready state)
    void reset_recovery()
    {
        if (recovery_attempts > 0) {
            std::print("[recovery] System recovered, resetting attempt counter\n");
        }
        recovery_attempts = 0;
        last_failure_time = TimePoint{};
    }

    /// Check if recovery is needed and possible
    [[nodiscard]] bool needs_recovery() const
    {
        using statusbar::nanoavb::supervisor_sm::Def;
        auto const state = supervisor.current_state();
        return (state == Def::State::Degraded || state == Def::State::Down) && recovery_attempts > 0 &&
            recovery_attempts < MAX_RECOVERY_ATTEMPTS;
    }

    /// Print current state of all state machines
    void print_state() const
    {
        using statusbar::nanoavb::supervisor_sm::Def;
        auto const supervisor_state = supervisor.current_state();
        std::print(
            "State: supervisor={} gptp={} mvrp={} recovery={}/{}\n",
            Def::State::Start == supervisor_state          ? "Start"
                : Def::State::Down == supervisor_state     ? "Down"
                : Def::State::Init == supervisor_state     ? "Init"
                : Def::State::Ready == supervisor_state    ? "Ready"
                : Def::State::Degraded == supervisor_state ? "Degraded"
                                                           : "Unknown",
            gptp_ctx.time_locked ? "Locked" : "Unlocked",
            mvrp_ctx.joined ? "Joined" : "NotJoined",
            recovery_attempts,
            MAX_RECOVERY_ATTEMPTS);
    }
};

//
// Argument Specifications
//

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;

    // Add PTP options with lambda bindings
    statusbar::ptpclient::add_ptp_arg_specs(specs, config.ptp_app);

    // NanoAVB options
    specs.add_device(
        "interface", "Network interface (required, e.g. eth0)", "", [&](auto v) { config.interface_name = std::string{v}; });

    return specs;
}

//
// Help and Usage
//

void print_usage(char const* program_name, statusbar::args::ArgumentSpecs const& specs)
{
#if defined(__linux__)
    static constexpr std::array<std::string_view, 2> examples{
        "--ptp.driver=linuxptp --ptp.device=/dev/ptp0",
        "--ptp.driver=system",
    };
#else
    static constexpr std::array<std::string_view, 1> examples{
        "--ptp.driver=system",
    };
#endif
    statusbar::config::default_print_usage(program_name, specs, "", examples);

    std::print(stderr, "\nPress Ctrl-C to stop.\n");
}

//
// Entity Model Creation
//

statusbar::nanoavb::EntityModel create_entity_model()
{
    using namespace statusbar::nanoavb;
    using namespace statusbar::atdecc::aem;
    using statusbar::ieee::Eui64;

    // Configure for minimal entity
    EntityModelConfig cfg;
    cfg.max_configurations = 1;
    cfg.max_stream_inputs = 2;
    cfg.max_stream_outputs = 2;
    cfg.max_avb_interfaces = 1;

    EntityModel model{cfg};

    // Set up entity descriptor
    DescriptorEntity entity{};
    entity.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    entity.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    entity.entity_capabilities = statusbar::atdecc::entity_capabilities::AEM_SUPPORTED |
        statusbar::atdecc::entity_capabilities::CLASS_A_SUPPORTED | statusbar::atdecc::entity_capabilities::GPTP_SUPPORTED;
    entity.talker_stream_sources = 2;
    entity.talker_capabilities =
        statusbar::atdecc::talker_capabilities::IMPLEMENTED | statusbar::atdecc::talker_capabilities::AUDIO_SOURCE;
    entity.listener_stream_sinks = 2;
    entity.listener_capabilities =
        statusbar::atdecc::listener_capabilities::IMPLEMENTED | statusbar::atdecc::listener_capabilities::AUDIO_SINK;
    entity.entity_name = AtdeccString{"NanoAVB Example"};
    entity.firmware_version = AtdeccString{"1.0.0"};
    entity.group_name = AtdeccString{"Example Group"};
    entity.serial_number = AtdeccString{"00000001"};
    entity.current_configuration = 0;

    model.set_entity(entity);

    // Add a configuration descriptor
    DescriptorConfiguration config{};
    config.object_name = AtdeccString{"Default Config"};
    config.localized_description = 0xFFFF;
    config.descriptor_counts_count = 0;
    (void)model.add_configuration(config);

    return model;
}

//
// NanoAvbComponents Factory
//

// Returns a unique_ptr because NanoAvbComponents is non-movable (it holds
// internal cross-references); the constructor wires AEM/ADP/ACMP against its
// own entity_model member. Site-specific stream/VLAN/MSRP setup is applied to
// the constructed object below.
std::unique_ptr<statusbar::nanoavb::NanoAvbComponents> create_nanoavb_components()
{
    using namespace statusbar::nanoavb;
    using statusbar::ieee::Eui48;
    using statusbar::ieee::Eui64;

    AdpAdvertiserConfig adp_config{};
    adp_config.valid_time = 31;                                        // 62 seconds
    adp_config.reannounce_interval = std::chrono::milliseconds{5000};  // 5 seconds

    // 2 talker streams (4 max listeners each), 2 listener streams.
    auto components = std::make_unique<NanoAvbComponents>(create_entity_model(), adp_config, 2, 4, 2);

    // Configure talker streams
    Eui64 const stream0_id{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x00};
    Eui64 const stream1_id{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x01};
    Eui48 const stream0_dest{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x00};
    Eui48 const stream1_dest{0x91, 0xE0, 0xF0, 0x00, 0x00, 0x01};
    (void)components->acmp_talker.configure_stream(0, stream0_id, stream0_dest);
    (void)components->acmp_talker.configure_stream(1, stream1_id, stream1_dest);

    // Register VLAN with MVRP and set the MSRP SR-class domain.
    (void)components->mvrp_handler.register_vlan(2, statusbar::sm::Clock::now());
    components->msrp_handler.set_domain(DomainInfo{
        .sr_class_id = 6,        // SR Class A
        .sr_class_priority = 3,  // Priority 3
        .sr_class_vid = 2});     // VLAN 2

    return components;
}

//
// Configuration Display
//

void print_entity_info(
    statusbar::atdecc::aem::DescriptorEntity const& entity,
    std::string_view interface_name,
    statusbar::ptpclient::PtpAppConfig const& ptp_config)
{
    using statusbar::ieee::to_string;

    std::print("NanoAVB Example (with State Machine Supervisor)\n");
    std::print("================================================\n");
    std::print("Entity ID:        {}\n", to_string(entity.entity_id).view());
    std::print("Entity Model ID:  {}\n", to_string(entity.entity_model_id).view());
    std::print("Talker Streams:   {}\n", entity.talker_stream_sources.get());
    std::print("Listener Streams: {}\n", entity.listener_stream_sinks.get());
    std::print("Interface:        {}\n", interface_name);
    std::print("Timer Source:     {} ({})\n", ptp_config.driver_name, ptp_config.device_path);
}

//
// Main Loop with PTP Timer
//

/// Result from main loop including exit code and timer statistics
struct MainLoopResult
{
    int exit_code{EXIT_SUCCESS};
    statusbar::stats::AtomicWakeStats::Snapshot wake_stats{};
    int64_t compensation_ns{0};
    int64_t recovery_count{0};
    int64_t missed_cycles{0};
};

MainLoopResult run_main_loop(
    statusbar::net::MessageReactor& reactor,
    statusbar::ptpclient::PtpAppContext& ctx,
    statusbar::itc::TelemetryCounter<int64_t>& ptp_wake_count,
    SupervisedNanoAvb& supervised,
    statusbar::nanoavb::NanoAvbNetHandlers& handlers)
{
    using namespace statusbar::ptpclient;
    using TimePoint = statusbar::sm::TimePoint;

    MainLoopResult result;
    result.compensation_ns = ctx.compensation_ns;

    // Track gPTP announce status for state machine events
    bool had_grandmaster = handlers.gptp_handler().has_grandmaster();

    // Create PTP timer for realtime audio packet handling
    auto timer = make_ptp_timer(
        *ctx.bridge,
        ctx.period_ns,
        [&](statusbar::StatusValue<TimerWakeInfo> const& wake_info) {
            if (!wake_info) {
                std::print(stderr, "Warning: Timer error: {}\n", wake_info.error().message());
                return;
            }

            ptp_wake_count.add();
            auto const now = TimePoint{std::chrono::nanoseconds{wake_info->actual_time_ns}};

            // Check for gPTP grandmaster changes
            bool const has_gm = handlers.gptp_handler().has_grandmaster();
            if (has_gm && !had_grandmaster) {
                supervised.on_gptp_announce(now, true);
            }
            had_grandmaster = has_gm;

            // Print periodic status (every 1000 wakes)
            if (wake_info->wake_count % 1000 == 0) {
                std::print(
                    "PTP Wake: {:8}  Error: {:+8} ns ({:+.3f} us)\n",
                    wake_info->wake_count,
                    wake_info->error_ns,
                    static_cast<double>(wake_info->error_ns) / 1000.0);
                supervised.print_state();
            }
        },
        ctx.compensation_ns,
        ctx.enable_realtime,
        ctx.cpu_affinity);

    // Start PTP timer
    auto start_result = timer.start();
    if (!start_result) {
        std::print(stderr, "Error: Failed to start timer: {}\n", start_result.error().message());
        result.exit_code = EXIT_FAILURE;
        return result;
    }

    // Simulate link up on startup (in a real system, this would come from netlink)
    auto startup_time = TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    supervised.on_link_up(startup_time);

    // Watchdog timeout configuration (gPTP lock only; there is no VLAN gate).
    // In a real system, timeouts would be tuned to the network environment.
    constexpr auto GPTP_LOCK_TIMEOUT = std::chrono::seconds{10};
    auto last_state_change_time = std::chrono::steady_clock::now();
    auto last_supervisor_state = supervised.supervisor.current_state();

    // Poll reactor for network I/O until shutdown
    while (!statusbar::realtime::is_shutdown_requested()) {
        (void)reactor.poll_once(100);

        // Check for state changes to reset watchdog
        auto const current_state = supervised.supervisor.current_state();
        if (current_state != last_supervisor_state) {
            last_state_change_time = std::chrono::steady_clock::now();
            last_supervisor_state = current_state;

            // Reset recovery counter when we reach Ready state
            if (current_state == statusbar::nanoavb::supervisor_sm::Def::State::Ready) {
                supervised.reset_recovery();
            }
        }

        // Check for watchdog timeout in waiting states
        auto const now = std::chrono::steady_clock::now();
        auto const time_in_state = now - last_state_change_time;
        auto const sm_now = TimePoint{now.time_since_epoch()};

        using statusbar::nanoavb::supervisor_sm::Def;
        if (current_state == Def::State::Init && time_in_state > GPTP_LOCK_TIMEOUT) {
            supervised.on_timeout(sm_now);
            last_state_change_time = now;
        }

        // Attempt recovery if needed and possible
        if (supervised.needs_recovery()) {
            (void)supervised.attempt_recovery(sm_now);
            last_state_change_time = now;
        }
    }

    // Stop timers and capture statistics
    timer.stop();
    result.wake_stats = timer.stats();
    result.recovery_count = timer.recovery_count();
    result.missed_cycles = timer.missed_cycles();
    ctx.guard.stop();

    return result;
}

void print_final_status(
    statusbar::nanoavb::NanoAvbComponents const& components,
    SupervisedNanoAvb const& supervised,
    int64_t tick_count,
    statusbar::stats::AtomicWakeStats::Snapshot const& wake_stats,
    int64_t compensation_ns,
    int64_t recovery_count,
    int64_t missed_cycles)
{
    using statusbar::nanoavb::AdpAdvertiserState;

    std::print("\n=== Final Status ===\n");
    std::print("  Total ticks:       {}\n", tick_count);
    std::print("  ADP state:         {}\n", components.adp_advertiser.state() == AdpAdvertiserState::Stopped ? "Stopped" : "Other");
    std::print("  Available index:   {}\n", components.adp_advertiser.available_index());
    std::print("  ADP send failures: {}\n", components.adp_advertiser.consecutive_send_failures());
    std::print("  Protocol recovery: {}/{}\n", supervised.recovery_attempts, SupervisedNanoAvb::MAX_RECOVERY_ATTEMPTS);
    std::print("  Timer recovery:    {}\n", recovery_count);
    std::print("  Missed cycles:     {}\n", missed_cycles);

    // Print final state machine states
    supervised.print_state();

    std::print("====================\n");

    // Print wake statistics
    std::string stats_report;
    wake_stats.format_report_to(std::back_inserter(stats_report), compensation_ns);
    std::print("{}", stats_report);
}

/// Parse the command line into @p config; an exit code when the program should stop now.
[[nodiscard]] auto parse_config(int const argc, char** const argv, Config& config) -> std::optional<int>
{
    auto specs = build_arg_specs(config);
    // --completion, --help, --config-load, --config-save, then the bindings
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage);
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (config.interface_name.empty()) {
        std::println(stderr, "Error: --interface=<iface> is required");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }
    return std::nullopt;
}

/// The demo's bring-up: components (non-movable, so on the heap), network handlers wired to
/// them, the supervised state machines and the reactor the handlers run on.
struct Demo
{
    explicit Demo(Config const& config)
        : components_owner{create_nanoavb_components()}
        , net_handlers{config.interface_name, *components_owner}
    {
        net_handlers.print_warnings(config.interface_name);
        statusbar::nanoavb::setup_nanoavb_callbacks(components(), net_handlers);
        supervised.wire_callbacks(components(), net_handlers);
        net_handlers.add_to_reactor(reactor);
        net_handlers.atdecc_handler().set_ptp_wake_counter(&ptp_wake_count);  // for the status display
        wire_grandmaster_announce();
    }

    /// A grandmaster change updates ADP and notifies the supervised state machine.
    void wire_grandmaster_announce()
    {
        net_handlers.gptp_handler().set_callbacks(statusbar::nanoavb::GptpAnnounceCallbacks{
            .grandmaster_id_changed = [this](
                                          int64_t now_ns,
                                          statusbar::gptp::ClockIdentity const& grandmaster_id,
                                          statusbar::gptp::AnnounceMessage const& announce) {
                std::string gm_str;
                statusbar::gptp::format_to(std::back_inserter(gm_str), grandmaster_id);
                std::print(
                    "gPTP: Grandmaster changed to {} (priority1={}, priority2={}, steps={})\n",
                    gm_str,
                    announce.grandmaster_priority1.get(),
                    announce.grandmaster_priority2.get(),
                    announce.steps_removed.get());
                components().adp_advertiser.set_gptp_info(grandmaster_id, 0);
                components().adp_advertiser.notify_entity_changed();
                supervised.on_gptp_announce(statusbar::sm::TimePoint{std::chrono::nanoseconds{now_ns}}, true);
            }});
    }

    [[nodiscard]] auto components() const noexcept -> statusbar::nanoavb::NanoAvbComponents& { return *components_owner; }

    std::unique_ptr<statusbar::nanoavb::NanoAvbComponents> components_owner;
    statusbar::nanoavb::NanoAvbNetHandlers net_handlers;
    SupervisedNanoAvb supervised{};
    statusbar::itc::StopToken shutdown_flag{};
    statusbar::net::MessageReactor reactor{shutdown_flag, statusbar::net::monotonic_ns, 10};
    statusbar::itc::TelemetryCounter<int64_t> ptp_wake_count{};
};

/// Set up clean shutdown on signals, then the PTP application, falling back to the system clock.
[[nodiscard]] auto setup_ptp(statusbar::ptpclient::PtpAppConfig& ptp_app)
    -> statusbar::StatusValue<statusbar::ptpclient::PtpAppContext>
{
    statusbar::realtime::setup_shutdown_signal_handlers();
    auto is_shutdown = [] { return statusbar::realtime::is_shutdown_requested(); };
    return statusbar::ptpclient::setup_ptp_app_with_fallback(ptp_app, is_shutdown);
}

/// The exit code for a PTP setup that failed: success when Ctrl-C interrupted it.
[[nodiscard]] auto ptp_setup_failed(std::error_code const error) -> int
{
    if (statusbar::realtime::is_shutdown_requested()) {
        std::print("Shutdown requested during setup\n");
        return EXIT_SUCCESS;
    }
    std::print(stderr, "Error: Failed to setup PTP: {}\n", error.message());
    return EXIT_FAILURE;
}

void print_state_machines()
{
    std::print("\nState machines initialized:\n");
    std::print("  - supervisor_sm (coordinates protocol lifecycle)\n");
    std::print("  - gptp_sm (gPTP synchronization)\n");
    std::print("  - mvrp_sm (VLAN registration)\n");
    std::print("  - msrp_talker_sm / msrp_listener_sm (stream reservation)\n");
    std::print("  - talker_engine_sm / listener_engine_sm (audio pipelines)\n");
}

void print_ptp_config(statusbar::ptpclient::PtpAppConfig const& ptp_app)
{
    std::string config_summary;
    statusbar::ptpclient::format_ptp_config_to(std::back_inserter(config_summary), ptp_app, "\nPTP Configuration");
    std::print("{}\n", config_summary);
}

/// The configuration, after PTP setup so a fallback is reflected, and the components.
void print_startup(Demo& demo, Config const& config)
{
    print_entity_info(demo.components().entity_model.get_entity(), config.interface_name, config.ptp_app);
    print_ptp_config(config.ptp_app);
    demo.components().print_info();
    demo.net_handlers.print_status();
    print_state_machines();
}

/// Notify the state machine (a simulated link down), stop ADP and print the final status.
void shut_down_and_report(Demo& demo, MainLoopResult const& loop_result)
{
    std::print("\n\nShutting down...\n");
    auto stop_time = statusbar::sm::TimePoint{std::chrono::steady_clock::now().time_since_epoch()};
    demo.supervised.on_link_down(stop_time);
    (void)demo.components().adp_advertiser.stop(stop_time);  // sends Entity Departing
    std::print("ADP: Sent Entity Departing\n");
    print_final_status(
        demo.components(),
        demo.supervised,
        demo.net_handlers.atdecc_handler().tick_count(),
        loop_result.wake_stats,
        loop_result.compensation_ns,
        loop_result.recovery_count,
        loop_result.missed_cycles);
}

/// Run the loops until Ctrl-C, then shut down; returns the exit code.
[[nodiscard]] auto run(Demo& demo, Config const& config, statusbar::ptpclient::PtpAppContext& ctx) -> int
{
    print_startup(demo, config);
    std::print("\nStarting loops (Ctrl-C to stop)...\n\n");
    auto const loop_result = run_main_loop(demo.reactor, ctx, demo.ptp_wake_count, demo.supervised, demo.net_handlers);
    shut_down_and_report(demo, loop_result);
    return loop_result.exit_code;
}

}  // namespace

//
// Main Entry Point
//

auto main(int argc, char** argv) -> int
{
    Config config;
    if (auto const exit_code = parse_config(argc, argv, config)) {
        return *exit_code;
    }
    Demo demo{config};
    auto ctx_result = setup_ptp(config.ptp_app);
    if (!ctx_result) {
        return ptp_setup_failed(ctx_result.error());
    }
    return run(demo, config, *ctx_result);
}
