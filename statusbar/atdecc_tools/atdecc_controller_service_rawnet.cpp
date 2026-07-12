// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// The raw-L2-socket ControllerService backend: NanoAvbAemController (our
/// ADP/ACMP/AECP state machines) driven by a RawnetContext. Owns the L2
/// receive/demux/serialize path; the consumer only ever sees decoded PDUs
/// through the ControllerServiceSink and per-command completions.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc_tools/atdecc_controller_service.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"

#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace statusbar::atdecc_tools {

namespace {

using namespace statusbar::atdecc;
using namespace statusbar::ieee;

class RawnetControllerService final
    : public ControllerService
    , public net::Pollable
{
  public:
    RawnetControllerService(net::RawnetContext context, Eui64 controller_id)
        : context_{std::move(context)}
        , controller_{controller_id}
    {
        wire_controller();
    }

    // ---- ControllerService lifecycle --------------------------------------

    void set_sink(ControllerServiceSink sink) override { sink_ = std::move(sink); }

    void start() override { controller_.start(); }

    /// One override serves both bases: ControllerService::tick and
    /// net::Pollable::tick share the signature.
    void tick(int64_t now_ns) override { controller_.tick(now_ns); }

    [[nodiscard]] auto pollable() noexcept -> net::Pollable* override { return this; }

    // ---- Pollable (the raw socket drives this backend) --------------------

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
            auto const len = static_cast<size_t>(*result);
            if (len == 0) {
                break;
            }
            dispatch_frame(now_ns, src_mac, {payload_buf_.data(), len});
        }
    }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

    // ---- Discovery ---------------------------------------------------------

    void discover_all() override { controller_.discover_all(); }

    [[nodiscard]] auto find_entity(Eui64 const& id) const -> DiscoveredEntity const* override
    {
        return controller_.find_entity(id);
    }

    // ---- AEM ---------------------------------------------------------------

    auto send_aem_command(
        Eui64 const& target, uint16_t command_code, std::span<uint8_t const> payload, AemCommandCompletion completion)
        -> bool override
    {
        return controller_.send_aem_command(target, command_code, payload, std::move(completion));
    }

    [[nodiscard]] auto aem_inflight_count() const -> size_t override { return controller_.aem_inflight_count(); }

    auto read_descriptor(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return controller_.read_descriptor(target, desc_type, desc_index);
    }

    auto set_identify(Eui64 const& target, bool on, AemCommandCompletion completion) -> bool override
    {
        return controller_.set_identify(target, on, std::move(completion));
    }

    auto get_counters(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return controller_.get_counters(target, desc_type, desc_index);
    }

    auto set_stream_format(Eui64 const& target, uint16_t desc_type, uint16_t desc_index, uint64_t stream_format) -> bool override
    {
        return controller_.set_stream_format(target, desc_type, desc_index, stream_format);
    }

    auto start_streaming(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return controller_.start_streaming(target, desc_type, desc_index);
    }

    auto stop_streaming(Eui64 const& target, uint16_t desc_type, uint16_t desc_index) -> bool override
    {
        return controller_.stop_streaming(target, desc_type, desc_index);
    }

    auto set_clock_source(Eui64 const& target, uint16_t desc_index, uint16_t clock_source_index, AemCommandCompletion completion)
        -> bool override
    {
        return controller_.set_clock_source(target, desc_index, clock_source_index, std::move(completion));
    }

    auto get_clock_source(Eui64 const& target, uint16_t desc_index, AemCommandCompletion completion) -> bool override
    {
        return controller_.get_clock_source(target, desc_index, std::move(completion));
    }

    auto set_signal_selector(
        Eui64 const& target,
        uint16_t desc_index,
        uint16_t signal_type,
        uint16_t signal_index,
        uint16_t signal_output,
        AemCommandCompletion completion) -> bool override
    {
        return controller_.set_signal_selector(target, desc_index, signal_type, signal_index, signal_output, std::move(completion));
    }

    auto get_signal_selector(Eui64 const& target, uint16_t desc_index, AemCommandCompletion completion) -> bool override
    {
        return controller_.get_signal_selector(target, desc_index, std::move(completion));
    }

    // ---- ACMP ---------------------------------------------------------------

    auto connect_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return controller_.connect_stream(talker, talker_uid, listener, listener_uid);
    }

    auto disconnect_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return controller_.disconnect_stream(talker, talker_uid, listener, listener_uid);
    }

    auto connect_tx_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return controller_.connect_tx_stream(talker, talker_uid, listener, listener_uid);
    }

    auto disconnect_tx_stream(Eui64 const& talker, uint16_t talker_uid, Eui64 const& listener, uint16_t listener_uid)
        -> bool override
    {
        return controller_.disconnect_tx_stream(talker, talker_uid, listener, listener_uid);
    }

    auto get_rx_state(Eui64 const& listener, uint16_t listener_uid) -> bool override
    {
        return controller_.get_rx_state(listener, listener_uid);
    }

    auto get_tx_state(Eui64 const& talker, uint16_t talker_uid) -> bool override
    {
        return controller_.get_tx_state(talker, talker_uid);
    }

  private:
    void wire_controller()
    {
        controller_.set_callbacks({
            .send_atdecc_multicast = [this](std::span<uint8_t const> pkt) -> bool {
                return context_.send(&ATDECC_MULTICAST_MAC, pkt).has_value();
            },
            .send_atdecc_unicast = [this](Eui48 const& dst, std::span<uint8_t const> pkt) -> bool {
                return context_.send(&dst, pkt).has_value();
            },
            .on_entity_available =
                [this](DiscoveredEntity const& e) {
                    if (sink_.on_entity_added) {
                        sink_.on_entity_added(e);
                    }
                },
            .on_entity_updated =
                [this](DiscoveredEntity const& e) {
                    if (sink_.on_entity_updated) {
                        sink_.on_entity_updated(e);
                    }
                },
            .on_entity_departing =
                [this](Eui64 id) {
                    if (sink_.on_entity_departed) {
                        sink_.on_entity_departed(id);
                    }
                },
            .on_aem_response =
                [this](
                    Eui64 target,
                    uint16_t cmd,
                    uint8_t status,
                    std::span<uint8_t const> sent_payload,
                    std::span<uint8_t const> data) {
                    if (sink_.on_aem_response) {
                        sink_.on_aem_response(target, cmd, status, sent_payload, data);
                    }
                },
            .on_aem_timeout =
                [this](Eui64 target, uint16_t cmd) {
                    if (sink_.on_aem_timeout) {
                        sink_.on_aem_timeout(target, cmd);
                    }
                },
            .on_acmp_response =
                [this](AcmpCommandResponse const& resp) {
                    if (sink_.on_acmp_response) {
                        sink_.on_acmp_response(resp);
                    }
                },
            .on_acmp_timeout =
                [this](AcmpCommandResponse const& cmd) {
                    if (sink_.on_acmp_timeout) {
                        sink_.on_acmp_timeout(cmd);
                    }
                },
        });
    }

    void dispatch_frame(int64_t now_ns, Eui48 const& src_mac, std::span<uint8_t const> payload)
    {
        if (payload.empty()) {
            return;
        }
        switch (payload[0]) {
            case avtp::AvtpSubtype::adp:
                dispatch_adp(now_ns, src_mac, payload);
                break;
            case avtp::AvtpSubtype::aecp:
                controller_.receive_aecp(payload, now_ns);
                break;
            case avtp::AvtpSubtype::acmp:
                dispatch_acmp(payload, now_ns);
                break;
            default:
                break;
        }
    }

    void dispatch_adp(int64_t now_ns, Eui48 const& src_mac, std::span<uint8_t const> payload)
    {
        if (payload.size() < AdpDu::LENGTH) {
            return;
        }
        AdpDu adp{};
        span_load(adp, payload);
        controller_.receive_adp(adp, src_mac, now_ns);
    }

    void dispatch_acmp(std::span<uint8_t const> payload, int64_t now_ns)
    {
        if (payload.size() < AcmpDu::LENGTH) {
            return;
        }
        AcmpDu acmp{};
        span_load(acmp, payload);
        // Observation first (commands included), then response injection —
        // the same order the pre-service demux used, so a consumer's trace
        // sees the PDU before any completion it triggers.
        if (sink_.on_acmp_observed) {
            sink_.on_acmp_observed(acmp);
        }
        if (!acmp.is_response()) {
            return;
        }
        auto resp = acmp_command_response_from_pdu(acmp);
        controller_.receive_acmp(resp, now_ns);
    }

    net::RawnetContext context_;
    nanoavb::NanoAvbAemController controller_;
    ControllerServiceSink sink_{};
    std::array<uint8_t, 2048> payload_buf_{};
};

}  // namespace

auto make_rawnet_controller_service(net::RawnetContext context, Eui64 controller_id) -> std::unique_ptr<ControllerService>
{
    return std::make_unique<RawnetControllerService>(std::move(context), controller_id);
}

auto make_controller_service(std::string const& backend, std::string const& interface_name, Eui64 controller_id, std::string& error)
    -> std::unique_ptr<ControllerService>
{
    if (backend == "avb") {
#if defined(__APPLE__)
        auto service = make_macos_avb_controller_service(interface_name, controller_id);
        if (service == nullptr) {
            error = std::format("no AVB framework support on interface '{}'", interface_name);
        }
        return service;
#else
        error = "the 'avb' backend is only available on macOS";
        return nullptr;
#endif
    }
    net::RawnetContext rawnet;
    if (!rawnet.open(interface_name, avtp::AVTP_ETHERTYPE, &atdecc::ATDECC_MULTICAST_MAC)) {
        error = std::format("failed to open raw socket on '{}' (root/cap_net_raw required)", interface_name);
        return nullptr;
    }
    return make_rawnet_controller_service(std::move(rawnet), controller_id);
}

}  // namespace statusbar::atdecc_tools
