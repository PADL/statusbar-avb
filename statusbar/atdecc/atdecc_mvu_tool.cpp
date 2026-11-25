// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// statusbar-atdecc-mvu - send one Milan vendor-unique (MVU) command to an
// entity and print its answer (Milan v1.3 5.4.3, 5.4.4).
//
//   statusbar-atdecc-mvu --interface=eth0 --command=milan-info \
//     --target-entity-id=00:11:22:ff:fe:33:44:55
//   statusbar-atdecc-mvu --interface=eth0 --command=bind \
//     --target-entity-id=<listener> --stream-input=0 \
//     --talker-entity-id=<talker> --talker-stream=0
//   statusbar-atdecc-mvu --interface=eth0 --command=stream-input-info \
//     --target-entity-id=<listener> --stream-input=0
//   statusbar-atdecc-mvu --interface=eth0 --command=unbind \
//     --target-entity-id=<listener> --stream-input=0
//
// It finds the target by ADP to learn its address, sends the command there,
// and waits 250 ms for the response (5.4.3.4), sending once more before it
// gives up: a retry is always done before a command has timed out (the rule
// of IEEE 1722.1 clause 9.3.6, as Milan v1.3 cites it). Exit status: 0 for a
// response of SUCCESS, 1 for a response of any other status, 2 for none.

#include "statusbar/atdecc/atdecc_addresses.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_mvu_request.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/config/config.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::avtp;
using namespace statusbar::net;
using namespace statusbar::ieee;

// J. D. Koftinoff Software, Ltd. OUI-36 based EUI-64.
constexpr Eui64 DEFAULT_CONTROLLER_ENTITY_ID(0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x01);

/// How long the target has to show itself in ADP.
constexpr int64_t DISCOVERY_TIMEOUT_NS = 5'000'000'000;
/// Milan v1.3 5.4.3.4.
constexpr int64_t RESPONSE_TIMEOUT_NS = int64_t{MVU_TIMEOUT_MS} * 1'000'000;
/// The command and its one retry.
constexpr int SENDS = 2;

constexpr int EXIT_ANSWERED_SUCCESS = 0;
constexpr int EXIT_ANSWERED_OTHERWISE = 1;
constexpr int EXIT_NO_ANSWER = 2;

struct Config
{
    std::string interface_name;
    std::string command{"milan-info"};
    Eui64 target_entity_id{};
    Eui64 controller_entity_id{DEFAULT_CONTROLLER_ENTITY_ID};
    Eui64 talker_entity_id{};
    uint16_t stream_input{0};
    uint16_t talker_stream{0};
    bool streaming_wait{false};
};

/// The command @p config asks for, with @p sequence_id.
auto request_for(Config const& config, uint16_t const sequence_id) -> MvuRequest
{
    MvuParties const parties{
        .target_entity_id = config.target_entity_id,
        .controller_entity_id = config.controller_entity_id,
        .sequence_id = sequence_id};
    if (config.command == "bind") {
        return mvu_bind_stream_request(
            parties,
            MvuBindTarget{
                .stream_input = config.stream_input,
                .talker_entity_id = config.talker_entity_id,
                .talker_stream_index = config.talker_stream,
                .streaming_wait = config.streaming_wait});
    }
    if (config.command == "unbind") {
        return mvu_unbind_stream_request(parties, config.stream_input);
    }
    if (config.command == "stream-input-info") {
        return mvu_get_stream_input_info_ex_request(parties, config.stream_input);
    }
    return mvu_get_milan_info_request(parties);
}

/// A sequence_id two runs a moment apart do not share.
auto sequence_from_clock() -> uint16_t
{
    return static_cast<uint16_t>(monotonic_ns() / 1'000'000);
}

class MvuSender : public net::Pollable
{
  public:
    MvuSender(Config const& config, itc::StopToken& stop, int& exit_code_out)
        : config_{config}
        , stop_{stop}
        , exit_code_{exit_code_out}
        , controller_{config.controller_entity_id}
        , request_{request_for(config, sequence_from_clock())}
    {
        (void)context_.open(config.interface_name, AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC);
        controller_.set_callbacks(discovery_callbacks());
        controller_.start();
        controller_.discover(config_.target_entity_id);
    }

    [[nodiscard]] auto valid() const noexcept -> bool { return context_.fd() >= 0; }

    [[nodiscard]] auto fd() const noexcept -> int override { return context_.fd(); }

    void on_ready(int64_t const now_ns) override
    {
        Eui48 src_mac{};
        Eui48 dest_mac{};
        while (true) {
            auto const result = context_.recv(&src_mac, &dest_mac, payload_buf_);
            if (!result || *result <= 0) {
                break;
            }
            dispatch_frame(now_ns, src_mac, {payload_buf_.data(), static_cast<size_t>(*result)});
        }
    }

    void tick(int64_t const now_ns) override
    {
        if (start_ns_ == 0) {
            start_ns_ = now_ns;
        }
        controller_.tick(now_ns);
        if (done_) {
            return;
        }
        if (sends_ == 0) {
            send_when_discovered(now_ns);
        } else if (now_ns >= deadline_ns_) {
            send_again_or_give_up(now_ns);
        }
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return done_; }

  private:
    /// The controller is here for ADP alone: it finds the target and keeps its address.
    auto discovery_callbacks() -> nanoavb::AemControllerEntityCallbacks
    {
        nanoavb::AemControllerEntityCallbacks callbacks;
        callbacks.send_atdecc_multicast = [this](std::span<uint8_t const> const packet) -> bool {
            return context_.send(&ATDECC_MULTICAST_MAC, packet).has_value();
        };
        callbacks.send_atdecc_unicast = [this](Eui48 const& dest_mac, std::span<uint8_t const> const packet) -> bool {
            return context_.send(&dest_mac, packet).has_value();
        };
        return callbacks;
    }

    void dispatch_frame(int64_t const now_ns, Eui48 const& src_mac, std::span<uint8_t const> const payload)
    {
        if (payload.empty()) {
            return;
        }
        if (payload[0] == AvtpSubtype::adp) {
            receive_adp(now_ns, src_mac, payload);
        } else if (payload[0] == AvtpSubtype::aecp) {
            receive_aecp(payload);
        }
    }

    void receive_adp(int64_t const now_ns, Eui48 const& src_mac, std::span<uint8_t const> const payload)
    {
        if (payload.size() < AdpDu::LENGTH) {
            return;
        }
        AdpDu adp{};
        span_load(adp, payload);
        controller_.receive_adp(adp, src_mac, now_ns);
    }

    void receive_aecp(std::span<uint8_t const> const payload)
    {
        auto const answer = sends_ > 0 && !done_ ? mvu_answer_to(request_, payload) : std::nullopt;
        if (!answer) {
            return;
        }
        std::print("[mvu] <- {} ({} us)\n", mvu_answer_text(request_.command_type, *answer), (monotonic_ns() - sent_ns_) / 1000);
        finish(answer->status == MVU_STATUS_SUCCESS ? EXIT_ANSWERED_SUCCESS : EXIT_ANSWERED_OTHERWISE);
    }

    void send_when_discovered(int64_t const now_ns)
    {
        if (controller_.find_entity(config_.target_entity_id) != nullptr) {
            send(now_ns);
            return;
        }
        if (now_ns - start_ns_ > DISCOVERY_TIMEOUT_NS) {
            std::print("[mvu] no ADP from {}: nothing sent\n", to_string(config_.target_entity_id).view());
            finish(EXIT_NO_ANSWER);
        }
    }

    void send_again_or_give_up(int64_t const now_ns)
    {
        if (sends_ < SENDS) {
            send(now_ns);
            return;
        }
        std::print(
            "[mvu] no response to {} from {} after {} sends\n",
            mvu_command_name(request_.command_type),
            to_string(config_.target_entity_id).view(),
            sends_);
        finish(EXIT_NO_ANSWER);
    }

    void send(int64_t const now_ns)
    {
        auto const* const target = controller_.find_entity(config_.target_entity_id);
        if (target == nullptr) {
            return finish(EXIT_NO_ANSWER);  // it departed between the sends
        }
        std::array<uint8_t, MvuRequest::MAX_PDU> buffer{};
        bool const sent = context_.send(&target->source_mac, mvu_request_pdu(request_, buffer)).has_value();
        note_sent(now_ns);
        print_sent(target->source_mac, sent);
    }

    /// One more send counted, and the wait for its response started. The
    /// response is timed from the clock as it reads now: the reactor's time
    /// is a tick old, and an answer comes within a millisecond.
    void note_sent(int64_t const now_ns) noexcept
    {
        ++sends_;
        sent_ns_ = monotonic_ns();
        deadline_ns_ = now_ns + RESPONSE_TIMEOUT_NS;
    }

    void print_sent(Eui48 const& mac, bool const sent) const
    {
        std::print(
            "[mvu] -> {} to {} at {}, sequence_id {}{}{}\n",
            mvu_command_name(request_.command_type),
            to_string(config_.target_entity_id).view(),
            to_string(mac).view(),
            request_.parties.sequence_id,
            sends_ > 1 ? " (retry)" : "",
            sent ? "" : " (send failed)");
    }

    void finish(int const code)
    {
        if (!done_) {
            exit_code_ = code;
            done_ = true;
            stop_.request_stop();
        }
    }

    Config config_;
    itc::StopToken& stop_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    int& exit_code_;        // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    RawnetContext context_{};
    nanoavb::NanoAvbAemController controller_;
    MvuRequest request_;
    std::array<uint8_t, 2048> payload_buf_{};
    int64_t start_ns_{0};
    int64_t sent_ns_{0};
    int64_t deadline_ns_{0};
    int sends_{0};
    bool done_{false};
};

void add_party_arg_specs(Config& config, statusbar::args::ArgumentSpecs& specs)
{
    specs.add<Eui64>(
        "target-entity-id", "Target Entity ID (EUI-64)", config.target_entity_id, [&](auto v) { config.target_entity_id = v; });
    specs.add<Eui64>(
        "controller-entity-id",
        "The controller_entity_id the command carries; a bind saves it with the binding",
        config.controller_entity_id,
        [&](auto v) { config.controller_entity_id = v; });
}

void add_stream_arg_specs(Config& config, statusbar::args::ArgumentSpecs& specs)
{
    specs.add<int64_t>("stream-input", "STREAM_INPUT index of the target (bind, unbind, stream-input-info)", 0, [&](auto v) {
        config.stream_input = static_cast<uint16_t>(v);
    });
    specs.add<Eui64>("talker-entity-id", "bind: the talker's Entity ID (EUI-64)", config.talker_entity_id, [&](auto v) {
        config.talker_entity_id = v;
    });
    specs.add<int64_t>("talker-stream", "bind: the talker's STREAM_OUTPUT index", 0, [&](auto v) {
        config.talker_stream = static_cast<uint16_t>(v);
    });
    specs.add<bool>("streaming-wait", "bind: bind the stream input stopped (STREAMING_WAIT)", false, [&](auto v) {
        config.streaming_wait = v;
    });
}

auto build_arg_specs(Config& config) -> statusbar::args::ArgumentSpecs
{
    statusbar::args::ArgumentSpecs specs;
    specs.add_device(
        "interface", "Network interface (e.g., en0, eth0)", "", [&](auto v) { config.interface_name = std::string{v}; });
    specs.add_choice(
        "command",
        "milan-info (GET_MILAN_INFO), bind (BIND_STREAM), unbind (UNBIND_STREAM) or stream-input-info "
        "(GET_STREAM_INPUT_INFO_EX)",
        {"milan-info", "bind", "unbind", "stream-input-info"},
        "milan-info",
        [&](auto v) { config.command = std::string{v}; });
    add_party_arg_specs(config, specs);
    add_stream_arg_specs(config, specs);
    return specs;
}

void print_usage(char const* prog, statusbar::args::ArgumentSpecs const& specs)
{
    static constexpr std::array<std::string_view, 3> examples{
        "--interface=eth0 --command=milan-info --target-entity-id=00:11:22:ff:fe:33:44:55",
        "--interface=eth0 --command=bind --target-entity-id=00:11:22:ff:fe:33:44:55 --stream-input=0 \\\n"
        "    --talker-entity-id=00:11:22:ff:fe:66:77:88 --talker-stream=0",
        "--interface=eth0 --command=stream-input-info --target-entity-id=00:11:22:ff:fe:33:44:55 --stream-input=0",
    };
    statusbar::config::default_print_usage(prog, specs, "", examples);
}

/// What @p config lacks to be sent, empty when nothing.
auto missing(Config const& config) -> std::string_view
{
    if (config.interface_name.empty() || !config.target_entity_id.is_set()) {
        return "--interface and --target-entity-id are required";
    }
    if (config.command == "bind" && !config.talker_entity_id.is_set()) {
        return "--command=bind needs --talker-entity-id";
    }
    return {};
}

/// Run @p sender in a reactor until it has finished or is stopped.
void run_to_the_end(statusbar::itc::StopToken& stop, std::unique_ptr<MvuSender> sender)
{
    MessageReactor reactor{stop, monotonic_ns, 10};
    reactor.add(std::move(sender));
    reactor.run();
}

auto run(Config const& config) -> int
{
    auto& stop = statusbar::itc::install_stop_signal();
    int exit_code = EXIT_NO_ANSWER;
    auto sender = std::make_unique<MvuSender>(config, stop, exit_code);
    if (!sender->valid()) {
        std::print(stderr, "Error: Failed to open raw socket on '{}' (needs root/cap_net_raw)\n", config.interface_name);
        return EXIT_FAILURE;
    }
    run_to_the_end(stop, std::move(sender));
    return exit_code;
}

}  // namespace

auto main(int argc, char** argv) -> int
{
    Config config;
    auto specs = build_arg_specs(config);
    auto cli_result = statusbar::config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-atdecc-mvu");
    if (!cli_result) {
        return statusbar::config::handled_builtin_command(cli_result) ? 0 : 1;
    }
    if (auto const problem = missing(config); !problem.empty()) {
        std::print(stderr, "Error: {}\n\n", problem);
        print_usage(argv[0], specs);
        return EXIT_FAILURE;
    }
    return run(config);
}
