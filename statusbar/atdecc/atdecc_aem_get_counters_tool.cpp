// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AEM Get Counters Tool - polls a target entity's descriptor counters (default
// STREAM_INPUT) once per interval via AECP GET_COUNTERS (IEEE 1722.1 Clause
// 7.4.42) and prints the named counter values + per-poll deltas, so stream
// errors (unlocks, interruptions, sequence-number mismatches, late/early
// timestamps) are visible at a glance.
//
//   statusbar-aem-get-counters --interface=eth0 \
//     --target-entity-id=00:11:22:ff:fe:33:44:55 --descriptor-index=0
//
// Runs until Ctrl-C (or --count polls).

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_format.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/status/throw_or_abort.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::avtp;
using namespace statusbar::net;
using namespace statusbar::ieee;

constexpr Eui64 MY_CONTROLLER_ENTITY_ID(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x02);

// STREAM_INPUT counters (IEEE 1722.1 / Milan), indexed by bit position in the
// counters_valid bitmap. `is_error` marks counters whose increase indicates a
// streaming problem (vs. the healthy MEDIA_LOCKED / TIMESTAMP_VALID / FRAMES_RX).
struct CounterDef
{
    char const* name;
    bool is_error;
};
constexpr std::array<CounterDef, 13> STREAM_INPUT_COUNTERS{{
    {.name = "MEDIA_LOCKED", .is_error = false},
    {.name = "MEDIA_UNLOCKED", .is_error = true},
    {.name = "STREAM_INTERRUPTED", .is_error = true},
    {.name = "SEQ_NUM_MISMATCH", .is_error = true},
    {.name = "MEDIA_RESET", .is_error = true},
    {.name = "TIMESTAMP_UNCERTAIN", .is_error = true},
    {.name = "TIMESTAMP_VALID", .is_error = false},
    {.name = "TIMESTAMP_NOT_VALID", .is_error = true},
    {.name = "UNSUPPORTED_FORMAT", .is_error = true},
    {.name = "LATE_TIMESTAMP", .is_error = true},
    {.name = "EARLY_TIMESTAMP", .is_error = true},
    {.name = "FRAMES_RX", .is_error = false},
    {.name = "FRAMES_TX", .is_error = false},
}};

// STREAM_OUTPUT counters (IEEE 1722.1 Clause 7.4.43), indexed by bit position.
// FRAMES_TX (bit 6) is the talker frame count -- the actual on-wire transmit rate.
constexpr std::array<CounterDef, 7> STREAM_OUTPUT_COUNTERS{{
    {.name = "STREAM_START", .is_error = false},
    {.name = "STREAM_STOP", .is_error = false},
    {.name = "MEDIA_RESET", .is_error = true},
    {.name = "TIMESTAMP_UNCERTAIN", .is_error = true},
    {.name = "TIMESTAMP_VALID", .is_error = false},
    {.name = "TIMESTAMP_NOT_VALID", .is_error = true},
    {.name = "FRAMES_TX", .is_error = false},
}};

[[nodiscard]] auto counter_name(size_t bit, bool is_output) -> std::string
{
    auto const& table =
        is_output ? std::span<CounterDef const>{STREAM_OUTPUT_COUNTERS} : std::span<CounterDef const>{STREAM_INPUT_COUNTERS};
    if (bit < table.size()) {
        return table[bit].name;
    }
    if (bit >= 24) {
        return std::format("ENTITY_SPECIFIC_{}", bit - 23);
    }
    return std::format("counter[{}]", bit);
}

[[nodiscard]] auto counter_is_error(size_t bit, bool is_output) -> bool
{
    auto const& table =
        is_output ? std::span<CounterDef const>{STREAM_OUTPUT_COUNTERS} : std::span<CounterDef const>{STREAM_INPUT_COUNTERS};
    return bit < table.size() && table[bit].is_error;
}

struct Config
{
    std::string interface_name;
    Eui64 target_entity_id{};
    uint16_t descriptor_type{DESCRIPTOR_STREAM_INPUT};
    uint16_t descriptor_index{0};
    int64_t interval_ms{1000};
    int64_t count{0};  // 0 = run forever
};

class GetCountersHandler : public net::Pollable
{
  public:
    GetCountersHandler(Config const& config, itc::StopToken& stop)
        : config_{config}
        , stop_{stop}
        , controller_{MY_CONTROLLER_ENTITY_ID}
    {
        (void)context_.open(config.interface_name, AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC);

        nanoavb::AemControllerEntityCallbacks callbacks;
        callbacks.send_atdecc_multicast = [this](std::span<uint8_t const> packet) -> bool {
            return context_.send(&ATDECC_MULTICAST_MAC, packet).has_value();
        };
        callbacks.send_atdecc_unicast = [this](Eui48 const& dest_mac, std::span<uint8_t const> packet) -> bool {
            return context_.send(&dest_mac, packet).has_value();
        };
        callbacks.on_aem_response =
            [this](Eui64 /*t*/, uint16_t cmd, uint8_t status, std::span<uint8_t const> /*sent*/, std::span<uint8_t const> data) {
                if (cmd == AEM_COMMAND_GET_COUNTERS) {
                    on_counters(status, data);
                }
            };
        callbacks.on_aem_timeout = [this](Eui64 /*t*/, uint16_t cmd) {
            if (cmd == AEM_COMMAND_GET_COUNTERS) {
                std::print("[counters] (no response - timeout)\n");
                awaiting_response_ = false;
            }
        };
        controller_.set_callbacks(std::move(callbacks));
        controller_.start();
        controller_.discover(config_.target_entity_id);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return context_.fd() >= 0; }

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t now_ns) override
    {
        Eui48 src_mac{};
        Eui48 dest_mac{};
        while (true) {
            auto result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            auto const payload = std::span<uint8_t const>{payload_buf_.data(), static_cast<size_t>(*result)};
            if (payload.empty()) {
                continue;
            }
            if (payload[0] == AvtpSubtype::adp) {
                if (payload.size() >= AdpDu::LENGTH) {
                    AdpDu adp{};
                    span_load(adp, payload);
                    controller_.receive_adp(adp, src_mac, now_ns);
                }
            } else if (payload[0] == AvtpSubtype::aecp) {
                controller_.receive_aecp(payload, now_ns);
            }
        }
    }

    void tick(int64_t now_ns) override
    {
        controller_.tick(now_ns);
        bool const discovered = controller_.find_entity(config_.target_entity_id) != nullptr;
        if (!discovered) {
            return;
        }
        int64_t const interval_ns = config_.interval_ms * 1'000'000;
        if (last_poll_ns_ != 0 && (now_ns - last_poll_ns_) < interval_ns) {
            return;
        }
        last_poll_ns_ = now_ns;
        send_get_counters();
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return done_; }

  private:
    void send_get_counters()
    {
        AemGetCountersCommandPayload payload{};
        payload.descriptor_type = doublet_t{config_.descriptor_type};
        payload.descriptor_index = doublet_t{config_.descriptor_index};
        std::array<uint8_t, AemGetCountersCommandPayload::LENGTH> bytes{};
        span_store(std::span{bytes}, payload);
        awaiting_response_ = true;
        (void)controller_.send_aem_command(config_.target_entity_id, AEM_COMMAND_GET_COUNTERS, bytes);
    }

    void on_counters(uint8_t status, std::span<uint8_t const> data)
    {
        awaiting_response_ = false;
        ++poll_count_;

        if (status != AEM_STATUS_SUCCESS) {
            std::print("[poll {}] GET_COUNTERS status={} ({})\n", poll_count_, status, aem_status_name(status));
            maybe_finish();
            return;
        }
        if (data.size() < sizeof(AemCountersPayload)) {
            std::print("[poll {}] short counters response ({} bytes)\n", poll_count_, data.size());
            maybe_finish();
            return;
        }

        AemCountersPayload resp{};
        span_load(resp, data.subspan(0, sizeof(AemCountersPayload)));
        uint32_t const valid = resp.counters_valid.get();

        bool const is_output = (config_.descriptor_type == DESCRIPTOR_STREAM_OUTPUT);
        char const* const desc_name = is_output ? "STREAM_OUTPUT" : "STREAM_INPUT";
        std::string line = std::format("[poll {}] {}[{}] valid={:#010x}", poll_count_, desc_name, config_.descriptor_index, valid);
        std::string errors;
        for (size_t bit = 0; bit < 32; ++bit) {
            if ((valid & (1U << bit)) == 0) {
                continue;
            }
            uint32_t const value = resp.counters[bit].get();
            uint32_t const prev = have_prev_ ? prev_counters_[bit] : value;
            int64_t const delta = static_cast<int64_t>(value) - static_cast<int64_t>(prev);
            std::format_to(std::back_inserter(line), "  {}={}", counter_name(bit, is_output), value);
            if (delta != 0) {
                std::format_to(std::back_inserter(line), "(Δ{:+})", delta);
            }
            if (counter_is_error(bit, is_output) && delta > 0) {
                std::format_to(std::back_inserter(errors), "  {} +{}", counter_name(bit, is_output), delta);
            }
            prev_counters_[bit] = value;
        }
        have_prev_ = true;
        std::print("{}\n", line);
        if (!errors.empty()) {
            std::print("           *** ERRORS this poll:{} ***\n", errors);
        }
        maybe_finish();
    }

    void maybe_finish()
    {
        if (config_.count > 0 && poll_count_ >= config_.count) {
            done_ = true;
            stop_.request_stop();
        }
    }

    Config config_;
    itc::StopToken& stop_;
    RawnetContext context_{};
    nanoavb::NanoAvbAemController controller_;
    std::array<uint8_t, 2048> payload_buf_{};
    std::array<uint32_t, 32> prev_counters_{};
    int64_t last_poll_ns_{0};
    int64_t poll_count_{0};
    bool have_prev_{false};
    bool awaiting_response_{false};
    bool done_{false};
};

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;
    specs.add_device(
        "interface", "Network interface (e.g., en0, eth0)", "", [&](auto v) { config.interface_name = std::string{v}; });
    specs.add<Eui64>(
        "target-entity-id", "Target Entity ID (EUI-64)", config.target_entity_id, [&](auto v) { config.target_entity_id = v; });
    specs.add<int64_t>("descriptor-index", "STREAM_INPUT descriptor index (default 0)", 0, [&](auto v) {
        config.descriptor_index = static_cast<uint16_t>(v);
    });
    specs.add<int64_t>("descriptor-type", "Descriptor type (default 0x0005 STREAM_INPUT)", DESCRIPTOR_STREAM_INPUT, [&](auto v) {
        config.descriptor_type = static_cast<uint16_t>(v);
    });
    specs.add<int64_t>("interval-ms", "Poll interval in milliseconds (default 1000)", 1000, [&](auto v) {
        config.interval_ms = (v < 50) ? 50 : v;
    });
    specs.add<int64_t>("count", "Stop after N polls (0 = run until Ctrl-C)", 0, [&](auto v) { config.count = v; });
    return specs;
}

void print_usage(char const* prog, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 1> examples{
        "--interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 --descriptor-index=0",
    };
    statusbar::config::default_print_usage(prog, specs, "", examples);
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    Config config;
    auto specs = build_arg_specs(config);

    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-aem-get-counters");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.interface_name.empty() || !config.target_entity_id.is_set()) {
        std::print(stderr, "Error: --interface and --target-entity-id are required\n\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    std::print(
        "Polling GET_COUNTERS for {}[{}] of {} every {} ms (Ctrl-C to stop)\n",
        config.descriptor_type == DESCRIPTOR_STREAM_OUTPUT ? "STREAM_OUTPUT" : "STREAM_INPUT",
        config.descriptor_index,
        to_string(config.target_entity_id).view(),
        config.interval_ms);

    auto& stop = statusbar::itc::install_stop_signal();
    auto handler_ptr = std::make_unique<GetCountersHandler>(config, stop);
    if (!handler_ptr->valid()) {
        std::print(stderr, "Error: Failed to open raw socket on '{}' (needs root/cap_net_raw)\n", config.interface_name);
        return EXIT_FAILURE;
    }

    MessageReactor reactor{stop, monotonic_ns, 100};
    reactor.add(std::move(handler_ptr));
    reactor.run();

    return EXIT_SUCCESS;
}
