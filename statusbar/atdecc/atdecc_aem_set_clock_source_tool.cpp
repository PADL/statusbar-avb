// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AEM Set/Get Clock Source Tool - command-line AECP/AEM controller that issues a
// SET_CLOCK_SOURCE (IEEE 1722.1 Clause 7.4.23) or GET_CLOCK_SOURCE (7.4.24)
// command to a target entity's CLOCK_DOMAIN descriptor.
//
// Use case: after connecting our talker to a remote listener's STREAM_INPUT N,
// tell that device to discipline its media clock to the incoming stream by
// selecting the INPUT_STREAM-type CLOCK_SOURCE whose location is Stream Input N.
//
//   statusbar-aem-set-clock-source --interface=eth0 \
//     --target-entity-id=00:11:22:ff:fe:33:44:55 \
//     --clock-domain-index=0 --clock-source-index=5
//
// Discovers the target via ADP (to learn its MAC), sends the command, prints the
// response status (and the resulting clock_source_index), then exits.

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
#include <chrono>
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

// J. D. Koftinoff Software, Ltd. OUI-36 based EUI-64.
constexpr Eui64 MY_CONTROLLER_ENTITY_ID(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01);

// Give discovery + the command/response round trip this long before giving up.
constexpr int64_t OPERATION_TIMEOUT_NS = 5'000'000'000;

struct Config
{
    std::string interface_name;
    Eui64 target_entity_id{};
    bool do_set{true};  // SET (true) vs GET (false)
    uint16_t clock_domain_index{0};
    uint16_t clock_source_index{0};
};

class SetClockSourceHandler : public net::Pollable
{
  public:
    SetClockSourceHandler(Config const& config, itc::StopToken& stop, int& exit_code_out)
        : config_{config}
        , stop_{stop}
        , exit_code_{exit_code_out}
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
        callbacks.on_entity_available = [this](atdecc::DiscoveredEntity const& e) {
            if (e.adpdu.entity_id == config_.target_entity_id) {
                maybe_send_command();
            }
        };
        callbacks.on_aem_response =
            [this](Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> /*sent*/, std::span<uint8_t const> data) {
                on_response(target, cmd, status, data);
            };
        callbacks.on_aem_timeout = [this](Eui64 /*target*/, uint16_t cmd) {
            std::print("[clock] TIMEOUT waiting for {} response\n", aem_command_name(cmd));
            finish(2);
        };
        controller_.set_callbacks(std::move(callbacks));
        controller_.start();
        controller_.discover(config_.target_entity_id);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return context_.fd() >= 0; }
    [[nodiscard]] auto my_mac() const noexcept -> Eui48 const& { return context_.my_mac(); }

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
            dispatch_frame(now_ns, src_mac, {payload_buf_.data(), static_cast<size_t>(*result)});
        }
    }

    void tick(int64_t now_ns) override
    {
        if (start_ns_ == 0) {
            start_ns_ = now_ns;
        }
        controller_.tick(now_ns);
        // Re-issue discovery until the target is found, then send.
        if (!command_sent_ && controller_.find_entity(config_.target_entity_id) != nullptr) {
            maybe_send_command();
        }
        if (!done_ && (now_ns - start_ns_) > OPERATION_TIMEOUT_NS) {
            std::print("[clock] TIMEOUT: target {} not discovered / no response\n", to_string(config_.target_entity_id).view());
            finish(2);
        }
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return done_; }

  private:
    void dispatch_frame(int64_t now_ns, Eui48 const& src_mac, std::span<uint8_t const> payload)
    {
        if (payload.empty()) {
            return;
        }
        switch (payload[0]) {
            case AvtpSubtype::adp: {
                if (payload.size() < AdpDu::LENGTH) {
                    return;
                }
                AdpDu adp{};
                span_load(adp, payload);
                controller_.receive_adp(adp, src_mac, now_ns);
                break;
            }
            case AvtpSubtype::aecp:
                controller_.receive_aecp(payload, now_ns);
                break;
            default:
                break;
        }
    }

    void maybe_send_command()
    {
        if (command_sent_) {
            return;
        }
        command_sent_ = true;

        if (config_.do_set) {
            AemClockSourcePayload payload{};
            payload.descriptor_type = doublet_t{DESCRIPTOR_CLOCK_DOMAIN};
            payload.descriptor_index = doublet_t{config_.clock_domain_index};
            payload.clock_source_index = doublet_t{config_.clock_source_index};
            std::array<uint8_t, AemClockSourcePayload::LENGTH> bytes{};
            span_store(std::span{bytes}, payload);
            std::print(
                "[clock] -> SET_CLOCK_SOURCE target={} clock_domain={} clock_source={}\n",
                to_string(config_.target_entity_id).view(),
                config_.clock_domain_index,
                config_.clock_source_index);
            (void)controller_.send_aem_command(config_.target_entity_id, AEM_COMMAND_SET_CLOCK_SOURCE, bytes);
        } else {
            AemGetClockSourceCommandPayload payload{};
            payload.descriptor_type = doublet_t{DESCRIPTOR_CLOCK_DOMAIN};
            payload.descriptor_index = doublet_t{config_.clock_domain_index};
            std::array<uint8_t, AemGetClockSourceCommandPayload::LENGTH> bytes{};
            span_store(std::span{bytes}, payload);
            std::print(
                "[clock] -> GET_CLOCK_SOURCE target={} clock_domain={}\n",
                to_string(config_.target_entity_id).view(),
                config_.clock_domain_index);
            (void)controller_.send_aem_command(config_.target_entity_id, AEM_COMMAND_GET_CLOCK_SOURCE, bytes);
        }
    }

    void on_response(Eui64 /*target*/, uint16_t cmd, uint8_t status, std::span<uint8_t const> data)
    {
        bool const ok = (status == AEM_STATUS_SUCCESS);
        std::print("[clock] <- {} status={} ({})\n", aem_command_name(cmd), status, aem_status_name(status));
        if (ok && data.size() >= AemClockSourcePayload::LENGTH) {
            AemClockSourcePayload resp{};
            span_load(resp, data.subspan(0, AemClockSourcePayload::LENGTH));
            std::print(
                "[clock] now: clock_domain={} clock_source={}\n", resp.descriptor_index.get(), resp.clock_source_index.get());
        }
        finish(ok ? 0 : 1);
    }

    void finish(int code)
    {
        if (!done_) {
            exit_code_ = code;
            done_ = true;
            stop_.request_stop();
        }
    }

    Config config_;
    itc::StopToken& stop_;
    int& exit_code_;
    RawnetContext context_{};
    nanoavb::NanoAvbAemController controller_;
    std::array<uint8_t, 2048> payload_buf_{};
    int64_t start_ns_{0};
    bool command_sent_{false};
    bool done_{false};
};

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;
    specs.add_device(
        "interface", "Network interface (e.g., en0, eth0)", "", [&](auto v) { config.interface_name = std::string{v}; });
    specs.add_choice("action", "SET (default) or GET the clock source", {"SET", "GET"}, "SET", [&](auto v) {
        config.do_set = !(v == "GET" || v == "get");
    });
    specs.add<Eui64>(
        "target-entity-id", "Target Entity ID (EUI-64)", config.target_entity_id, [&](auto v) { config.target_entity_id = v; });
    specs.add<int64_t>("clock-domain-index", "CLOCK_DOMAIN descriptor index (default 0)", 0, [&](auto v) {
        config.clock_domain_index = static_cast<uint16_t>(v);
    });
    specs.add<int64_t>("clock-source-index", "Clock source index to select (SET only)", 0, [&](auto v) {
        config.clock_source_index = static_cast<uint16_t>(v);
    });
    return specs;
}

void print_usage(char const* prog, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --target-entity-id=00:11:22:ff:fe:33:44:55 \\\n"
        "    --clock-domain-index=0 --clock-source-index=5",
        "--interface=eth0 --action=GET --target-entity-id=00:11:22:ff:fe:33:44:55",
    };
    statusbar::config::default_print_usage(prog, specs, "", examples);
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    Config config;
    auto specs = build_arg_specs(config);

    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-aem-set-clock-source");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    if (config.interface_name.empty() || !config.target_entity_id.is_set()) {
        std::print(stderr, "Error: --interface and --target-entity-id are required\n\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    auto& stop = statusbar::itc::install_stop_signal();
    int exit_code = 0;
    auto handler_ptr = std::make_unique<SetClockSourceHandler>(config, stop, exit_code);
    if (!handler_ptr->valid()) {
        std::print(stderr, "Error: Failed to open raw socket on '{}' (needs root/cap_net_raw)\n", config.interface_name);
        return EXIT_FAILURE;
    }

    MessageReactor reactor{stop, monotonic_ns, 100};
    reactor.add(std::move(handler_ptr));
    reactor.run();

    return exit_code;
}
