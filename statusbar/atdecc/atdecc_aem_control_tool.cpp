// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// AEM Control Tool - gets or sets a target entity's CONTROL descriptor
// current values via AECP GET_CONTROL / SET_CONTROL (IEEE 1722.1 Clause
// 7.4.25/7.4.26). The tool first reads the CONTROL descriptor to learn its
// control_value_type and number_of_values, so values are encoded/decoded in
// the control's own linear/array element format — no per-control flags.
//
//   statusbar-aem-control --interface=eth0 \
//     --target-entity-id=70:b3:d5:ed:c2:00:c8:f0 --descriptor-index=1
//   statusbar-aem-control --interface=eth0 \
//     --target-entity-id=... --descriptor-index=1 --set=-3.0
//
// One-shot: exits 0 on SUCCESS with the (new) current values printed,
// non-zero on timeout or a non-SUCCESS AEM status.

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
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

#include <array>
#include <charconv>
#include <cstdint>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::avtp;
using namespace statusbar::net;
using namespace statusbar::ieee;

constexpr Eui64 MY_CONTROLLER_ENTITY_ID(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x03);

struct Config
{
    std::string interface_name;
    Eui64 target_entity_id{};
    uint16_t descriptor_index{0};
    bool do_set{false};
    std::vector<double> set_values;
    int64_t timeout_ms{5000};
};

/// Parse a comma-separated list of numbers ("-3.0,1.5,0").
[[nodiscard]] auto parse_values(std::string_view text) -> std::vector<double>
{
    std::vector<double> out;
    size_t at = 0;
    while (at <= text.size()) {
        auto comma = text.find(',', at);
        if (comma == std::string_view::npos) {
            comma = text.size();
        }
        auto item = std::string(text.substr(at, comma - at));
        if (!item.empty()) {
            out.push_back(std::strtod(item.c_str(), nullptr));
        }
        at = comma + 1;
    }
    return out;
}

/// Decode one element of the control's linear/array family at @p offset.
[[nodiscard]] auto decode_element(uint16_t value_type, std::span<uint8_t const> bytes) -> double
{
    auto load = [&]<typename T>() -> double {
        std::array<uint8_t, sizeof(T)> b{};
        std::copy_n(bytes.begin(), sizeof(T), b.begin());
        return static_cast<double>(statusbar::atdecc::aem::detail::decode_linear_field<T>(b));
    };
    switch (value_type) {
        case CONTROL_LINEAR_INT8:
        case CONTROL_ARRAY_INT8:
            return load.operator()<int8_t>();
        case CONTROL_LINEAR_UINT8:
        case CONTROL_ARRAY_UINT8:
            return load.operator()<uint8_t>();
        case CONTROL_LINEAR_INT16:
        case CONTROL_ARRAY_INT16:
            return load.operator()<int16_t>();
        case CONTROL_LINEAR_UINT16:
        case CONTROL_ARRAY_UINT16:
            return load.operator()<uint16_t>();
        case CONTROL_LINEAR_INT32:
        case CONTROL_ARRAY_INT32:
            return load.operator()<int32_t>();
        case CONTROL_LINEAR_UINT32:
        case CONTROL_ARRAY_UINT32:
            return load.operator()<uint32_t>();
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_ARRAY_FLOAT:
            return load.operator()<float>();
        case CONTROL_LINEAR_DOUBLE:
        case CONTROL_ARRAY_DOUBLE:
            return load.operator()<double>();
        default:
            return 0.0;
    }
}

void encode_element(uint16_t value_type, double value, std::vector<uint8_t>& out)
{
    auto store = [&]<typename T>() {
        std::array<uint8_t, sizeof(T)> b{};
        statusbar::atdecc::aem::detail::encode_linear_field(static_cast<T>(value), b);
        out.insert(out.end(), b.begin(), b.end());
    };
    switch (value_type) {
        case CONTROL_LINEAR_INT8:
        case CONTROL_ARRAY_INT8:
            store.operator()<int8_t>();
            return;
        case CONTROL_LINEAR_UINT8:
        case CONTROL_ARRAY_UINT8:
            store.operator()<uint8_t>();
            return;
        case CONTROL_LINEAR_INT16:
        case CONTROL_ARRAY_INT16:
            store.operator()<int16_t>();
            return;
        case CONTROL_LINEAR_UINT16:
        case CONTROL_ARRAY_UINT16:
            store.operator()<uint16_t>();
            return;
        case CONTROL_LINEAR_INT32:
        case CONTROL_ARRAY_INT32:
            store.operator()<int32_t>();
            return;
        case CONTROL_LINEAR_UINT32:
        case CONTROL_ARRAY_UINT32:
            store.operator()<uint32_t>();
            return;
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_ARRAY_FLOAT:
            store.operator()<float>();
            return;
        case CONTROL_LINEAR_DOUBLE:
        case CONTROL_ARRAY_DOUBLE:
            store.operator()<double>();
            return;
        default:
            return;
    }
}

class AemControlHandler : public net::Pollable
{
  public:
    AemControlHandler(Config const& config, itc::StopToken& stop, int& exit_code)
        : config_{config}
        , stop_{stop}
        , exit_code_{exit_code}
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
                on_response(cmd, status, data);
            };
        callbacks.on_aem_timeout = [this](Eui64 /*t*/, uint16_t cmd) {
            std::print(stderr, "{}: no response (timeout)\n", aem_command_name(cmd));
            finish(EXIT_FAILURE);
        };
        controller_.set_callbacks(std::move(callbacks));
        controller_.start();
        controller_.discover(config_.target_entity_id);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return context_.fd() >= 0; }
    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }
    [[nodiscard]] auto finished() const noexcept -> bool override { return done_; }

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
        if (start_ns_ == 0) {
            start_ns_ = now_ns;
        }
        if (!sent_read_ && controller_.find_entity(config_.target_entity_id) != nullptr) {
            sent_read_ = true;
            (void)controller_.read_descriptor(config_.target_entity_id, DESCRIPTOR_CONTROL, config_.descriptor_index);
        }
        // Follow-up commands are queued by the response callback and sent
        // here: send_aem_command re-enters handle_aem_response, so sending
        // from inside on_aem_response recurses until the stack dies.
        if (pending_cmd_ != 0) {
            auto const cmd = pending_cmd_;
            pending_cmd_ = 0;
            (void)controller_.send_aem_command(config_.target_entity_id, cmd, pending_payload_);
        }
        if (!done_ && (now_ns - start_ns_) > config_.timeout_ms * 1'000'000) {
            std::print(stderr, "timed out ({} ms) — entity not discovered or no response\n", config_.timeout_ms);
            finish(EXIT_FAILURE);
        }
    }

  private:
    void on_response(uint16_t cmd, uint8_t status, std::span<uint8_t const> data)
    {
        if (status != AEM_STATUS_SUCCESS) {
            std::print(stderr, "{}: status {} ({})\n", aem_command_name(cmd), status, aem_status_name(status));
            finish(EXIT_FAILURE);
            return;
        }
        if (cmd == AEM_COMMAND_READ_DESCRIPTOR) {
            on_descriptor(data);
        } else if (cmd == AEM_COMMAND_GET_CONTROL || cmd == AEM_COMMAND_SET_CONTROL) {
            print_values(cmd, data);
        }
    }

    /// READ_DESCRIPTOR response: 4-byte config/reserved echo + descriptor.
    void on_descriptor(std::span<uint8_t const> data)
    {
        if (data.size() < 4 + DescriptorControl::LENGTH) {
            std::print(stderr, "short CONTROL descriptor ({} bytes)\n", data.size());
            finish(EXIT_FAILURE);
            return;
        }
        DescriptorControl desc{};
        span_load_padded(desc, data.subspan(4));
        auto const bits = unpack_control_value_type(static_cast<uint16_t>(desc.control_value_type.get()));
        value_type_ = bits.value_type;
        count_ = desc.number_of_values.get();
        element_size_ = control_value_element_size(value_type_);
        object_name_ = std::string{desc.object_name.as_string_view()};
        if (element_size_ == 0 || count_ == 0) {
            std::print(
                stderr,
                "CONTROL[{}] '{}' has unsupported value_type {} ({})\n",
                config_.descriptor_index,
                object_name_,
                value_type_,
                control_value_type_name(value_type_));
            finish(EXIT_FAILURE);
            return;
        }

        AemControlPayloadHeader header{};
        header.descriptor_type = doublet_t{DESCRIPTOR_CONTROL};
        header.descriptor_index = doublet_t{config_.descriptor_index};
        std::vector<uint8_t> payload(AemControlPayloadHeader::LENGTH);
        span_store(std::span{payload.data(), AemControlPayloadHeader::LENGTH}, header);

        if (!config_.do_set) {
            pending_payload_ = std::move(payload);
            pending_cmd_ = AEM_COMMAND_GET_CONTROL;
            return;
        }

        // SET: exactly N values, or one value fanned out to all N.
        auto values = config_.set_values;
        if (values.size() == 1 && count_ > 1) {
            values.assign(count_, values[0]);
        }
        if (values.size() != count_) {
            std::print(
                stderr,
                "CONTROL[{}] '{}' carries {} values; --set gave {}\n",
                config_.descriptor_index,
                object_name_,
                count_,
                config_.set_values.size());
            finish(EXIT_FAILURE);
            return;
        }
        for (auto v : values) {
            encode_element(value_type_, v, payload);
        }
        pending_payload_ = std::move(payload);
        pending_cmd_ = AEM_COMMAND_SET_CONTROL;
    }

    void print_values(uint16_t cmd, std::span<uint8_t const> data)
    {
        auto const values = data.size() > AemControlPayloadHeader::LENGTH ? data.subspan(AemControlPayloadHeader::LENGTH)
                                                                          : std::span<uint8_t const>{};
        std::string line = std::format(
            "CONTROL[{}] '{}' ({} x {}): ", config_.descriptor_index, object_name_, count_, control_value_type_name(value_type_));
        for (size_t i = 0; i + element_size_ <= values.size() && i / element_size_ < count_; i += element_size_) {
            std::format_to(std::back_inserter(line), "{}{:g}", i == 0 ? "" : " ", decode_element(value_type_, values.subspan(i)));
        }
        std::print("{}{}\n", cmd == AEM_COMMAND_SET_CONTROL ? "SET ok — " : "", line);
        finish(EXIT_SUCCESS);
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
    uint16_t value_type_{0};
    uint16_t count_{0};
    size_t element_size_{0};
    std::string object_name_;
    int64_t start_ns_{0};
    uint16_t pending_cmd_{0};
    std::vector<uint8_t> pending_payload_;
    bool sent_read_{false};
    bool done_{false};
};

auto build_arg_specs(Config& config, std::string& set_text) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;
    specs.add_device(
        "interface", "Network interface (e.g., en0, eth0)", "", [&](auto v) { config.interface_name = std::string{v}; });
    specs.add<Eui64>(
        "target-entity-id", "Target Entity ID (EUI-64)", config.target_entity_id, [&](auto v) { config.target_entity_id = v; });
    specs.add<int64_t>("descriptor-index", "CONTROL descriptor index (default 0)", 0, [&](auto v) {
        config.descriptor_index = static_cast<uint16_t>(v);
    });
    specs.add<std::string>("set", "Set values, comma-separated (omit to GET)", "", [&](auto v) { set_text = std::string{v}; });
    specs.add<int64_t>("timeout-ms", "Overall deadline (default 5000)", 5000, [&](auto v) { config.timeout_ms = v; });
    return specs;
}

void print_usage(char const* prog, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 2> examples{
        "--interface=eth0 --target-entity-id=70:b3:d5:ed:c2:00:c8:f0 --descriptor-index=1",
        "--interface=eth0 --target-entity-id=70:b3:d5:ed:c2:00:c8:f0 --descriptor-index=1 --set=-3.0",
    };
    statusbar::config::default_print_usage(prog, specs, "", examples);
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    Config config;
    std::string set_text;
    auto specs = build_arg_specs(config, set_text);

    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-aem-control");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (!set_text.empty()) {
        config.do_set = true;
        config.set_values = parse_values(set_text);
    }
    if (config.interface_name.empty() || !config.target_entity_id.is_set() || (config.do_set && config.set_values.empty())) {
        std::print(stderr, "Error: --interface and --target-entity-id are required (--set needs values)\n\n");
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }

    auto& stop = statusbar::itc::install_stop_signal();
    int exit_code = EXIT_SUCCESS;
    auto handler_ptr = std::make_unique<AemControlHandler>(config, stop, exit_code);
    if (!handler_ptr->valid()) {
        std::print(stderr, "Error: Failed to open raw socket on '{}' (needs root/cap_net_raw)\n", config.interface_name);
        return EXIT_FAILURE;
    }

    MessageReactor reactor{stop, monotonic_ns, 100};
    reactor.add(std::move(handler_ptr));
    reactor.run();

    return exit_code;
}
