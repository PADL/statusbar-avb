// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// ATDECC Monitor Tool — monitors ATDECC entities via ADP, reads ENTITY descriptors,
// and tracks ACMP stream connections/disconnections. Frames can come from a live
// interface (--interface=eth0) or a pcap/pcapng capture (--pcap=file.pcapng).
//
// Usage: statusbar-atdecc-monitor --interface=eth0 [--instance=N] [--controller-entity-id=EUI64] [--show-descriptors]
//        statusbar-atdecc-monitor --pcap=file.pcapng [--show-descriptors]

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_format.hpp"
#include "statusbar/atdecc_tools/atdecc_tools_common.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_controller.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_rawnet.hpp"
#include "statusbar/pcap/pcapng_reader.hpp"
#include "statusbar/sg14/inplace_function.h"
#include "statusbar/stats/stats_format.hpp"
#include "statusbar/status/catch_or_status.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::ieee;
using namespace statusbar::net;

// Defensive caps against a hostile/corrupt CONFIGURATION descriptor: real entities
// have at most tens of any descriptor type and far fewer than 108 types, so these
// bound enumeration hard (a raw 65535 x 65535 would otherwise be ~4 billion enqueues).
constexpr uint16_t MAX_DESCRIPTORS_PER_TYPE = 512;
constexpr uint16_t MAX_DESCRIPTOR_TYPES = 108;  // total AVDECC descriptor types

using atdecc_tools::CommonConfig;

// Env-gated tracing for --enumerate (set STATUSBAR_ENUM_DEBUG=1).
[[nodiscard]] auto enum_debug() -> bool
{
    static bool const v = std::getenv("STATUSBAR_ENUM_DEBUG") != nullptr;
    return v;
}

// Stream connection tracking types

struct StreamConnection
{
    Eui64 talker_entity_id{};
    uint16_t talker_unique_id{0};
    Eui64 listener_entity_id{};
    uint16_t listener_unique_id{0};
    Eui64 stream_id{};
    Eui48 stream_dest_mac{};
    int64_t connect_time_ns{0};
    uint16_t connection_count{0};
};

using StreamKey = std::pair<Eui64, uint16_t>;
using StreamMap = std::map<StreamKey, StreamConnection>;

// Monitor callbacks

struct MonitorCallbacks
{
    statusbar::sg14::inplace_function<void(DiscoveredEntity const&), 64> on_entity_available;
    statusbar::sg14::inplace_function<void(DiscoveredEntity const&), 64> on_entity_updated;
    statusbar::sg14::inplace_function<void(Eui64), 64> on_entity_departing;
    statusbar::sg14::inplace_function<void(nanoavb::NanoAvbAemController&, Eui64, uint16_t, uint8_t, std::span<uint8_t const>), 64>
        on_aem_response;
    statusbar::sg14::inplace_function<void(nanoavb::NanoAvbAemController&, Eui64, uint16_t), 64> on_aem_timeout;
    statusbar::sg14::inplace_function<void(AcmpDu const&, StreamConnection const&), 64> on_stream_connected;
    statusbar::sg14::inplace_function<void(AcmpDu const&), 64> on_stream_disconnected;
    statusbar::sg14::inplace_function<void(AcmpDu const&), 64> on_acmp_state;
    statusbar::sg14::inplace_function<void(AcmpDu const&), 64> on_acmp_error;
};

// ---------------------------------------------------------------------------
// Timestamps
// ---------------------------------------------------------------------------

auto const g_start_time = std::chrono::steady_clock::now();

auto elapsed_ns() -> int64_t
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start_time).count();
}

// Pcap mode uses an injected clock that returns the capture timestamp of the
// packet currently being dispatched. The MonitorCore uses a function pointer
// so callbacks can print consistent timestamps in both modes.
int64_t g_pcap_now_ns = 0;
auto pcap_now_ns() -> int64_t
{
    return g_pcap_now_ns;
}
using NowFn = int64_t (*)();
NowFn g_now_fn = elapsed_ns;

auto ts() -> std::string
{
    return stats::format_timestamp_ns(g_now_fn());
}

auto format_stream_port(Eui64 entity_id, uint16_t unique_id) -> std::string
{
    return std::format("{}:{}", ieee::to_string(entity_id).view(), unique_id);
}

// ---------------------------------------------------------------------------
// READ_DESCRIPTOR wire-level printing (enabled by --show-descriptors)
// ---------------------------------------------------------------------------

/// Parse an AECP/AEM packet and, if it's a READ_DESCRIPTOR command or response,
/// pretty-print it to stdout. For responses with status==SUCCESS the full
/// descriptor body is formatted via the format_descriptor dispatcher.
///
/// Returns true iff the packet was a READ_DESCRIPTOR (command or response)
/// and something was printed.
auto maybe_print_read_descriptor(std::span<uint8_t const> payload) -> bool
{
    if (payload.size() < AemDu::LENGTH) {
        return false;
    }
    AemDu aem{};
    span_load(aem, payload);
    if (!aem.is_command() && !aem.is_response()) {
        return false;
    }
    if (aem.command_code() != AEM_COMMAND_READ_DESCRIPTOR) {
        return false;
    }

    bool const is_response = aem.is_response();
    auto const aecp_body = payload.subspan(AemDu::LENGTH);

    if (!is_response) {
        // Command: configuration_index + descriptor_type + descriptor_index
        if (aecp_body.size() < AemReadDescriptorCommandPayload::LENGTH) {
            std::println(
                "{} READ_DESC  CMD  controller={} target={} seq={} (truncated)",
                ts(),
                ieee::to_string(aem.controller_entity_id).view(),
                ieee::to_string(aem.target_entity_id).view(),
                aem.sequence_id.get());
            return true;
        }
        AemReadDescriptorCommandPayload cmd{};
        span_load(cmd, aecp_body);
        std::println(
            "{} READ_DESC  CMD  controller={} target={} seq={} conf_idx={} type={} ({:#06x}) index={}",
            ts(),
            ieee::to_string(aem.controller_entity_id).view(),
            ieee::to_string(aem.target_entity_id).view(),
            aem.sequence_id.get(),
            cmd.configuration_index.get(),
            descriptor_type_name(cmd.descriptor_type.get()),
            cmd.descriptor_type.get(),
            cmd.descriptor_index.get());
        return true;
    }

    // Response
    if (aecp_body.size() < AemReadDescriptorResponsePayload::LENGTH) {
        std::println(
            "{} READ_DESC  RESP controller={} target={} seq={} status={} (truncated)",
            ts(),
            ieee::to_string(aem.controller_entity_id).view(),
            ieee::to_string(aem.target_entity_id).view(),
            aem.sequence_id.get(),
            aem_status_name(aem.status()));
        return true;
    }
    AemReadDescriptorResponsePayload hdr{};
    span_load(hdr, aecp_body);
    auto const desc_body = aecp_body.subspan(AemReadDescriptorResponsePayload::LENGTH);

    if (aem.status() != AEM_STATUS_SUCCESS) {
        std::println(
            "{} READ_DESC  RESP controller={} target={} seq={} status={} conf_idx={}",
            ts(),
            ieee::to_string(aem.controller_entity_id).view(),
            ieee::to_string(aem.target_entity_id).view(),
            aem.sequence_id.get(),
            aem_status_name(aem.status()),
            hdr.configuration_index.get());
        return true;
    }

    std::println(
        "{} READ_DESC  RESP controller={} target={} seq={} status=SUCCESS conf_idx={}",
        ts(),
        ieee::to_string(aem.controller_entity_id).view(),
        ieee::to_string(aem.target_entity_id).view(),
        aem.sequence_id.get(),
        hdr.configuration_index.get());
    std::string body;
    format_descriptor(std::back_inserter(body), desc_body);
    std::print("{}", body);
    return true;
}

// ---------------------------------------------------------------------------
// Print callbacks
// ---------------------------------------------------------------------------

MonitorCallbacks make_print_callbacks(bool show_descriptors)
{
    return {
        .on_entity_available =
            [](DiscoveredEntity const& e) {
                auto const& adp = e.adpdu;
                std::println(
                    "{} AVAILABLE  entity_id={} model_id={} caps=[{}] mac={}",
                    ts(),
                    ieee::to_string(adp.entity_id).view(),
                    ieee::to_string(adp.entity_model_id).view(),
                    entity_capabilities_to_string(adp.entity_capabilities.get()),
                    ieee::to_string(e.source_mac).view());
            },
        .on_entity_updated =
            [](DiscoveredEntity const& e) {
                std::println(
                    "{} UPDATED    entity_id={} available_index={}",
                    ts(),
                    ieee::to_string(e.adpdu.entity_id).view(),
                    e.adpdu.available_index.get());
            },
        .on_entity_departing = [](Eui64 id) { std::println("{} DEPARTING  entity_id={}", ts(), ieee::to_string(id).view()); },
        .on_aem_response =
            [show_descriptors](
                nanoavb::NanoAvbAemController& /*controller*/,
                Eui64 target,
                uint16_t cmd,
                uint8_t status,
                std::span<uint8_t const> data) {
                if (show_descriptors) {
                    // Wire-level path already printed a full pretty-print for
                    // this response; skip the short-form summary here.
                    return;
                }
                if (cmd != AEM_COMMAND_READ_DESCRIPTOR || status != AEM_STATUS_SUCCESS) {
                    return;
                }
                if (data.size() < AemReadDescriptorResponsePayload::LENGTH + DescriptorEntity::LENGTH) {
                    return;
                }
                DescriptorEntity desc{};
                span_load(desc, data.subspan(AemReadDescriptorResponsePayload::LENGTH));
                std::println(
                    "{} DESCRIPTOR entity_id={} name=\"{}\" fw=\"{}\" serial=\"{}\"",
                    ts(),
                    ieee::to_string(target).view(),
                    desc.entity_name.as_string_view(),
                    desc.firmware_version.as_string_view(),
                    desc.serial_number.as_string_view());
            },
        .on_aem_timeout =
            [](nanoavb::NanoAvbAemController& /*controller*/, Eui64 target, uint16_t cmd) {
                if (cmd == AEM_COMMAND_READ_DESCRIPTOR) {
                    std::println("{} TIMEOUT    entity_id={} (READ_DESCRIPTOR)", ts(), ieee::to_string(target).view());
                }
            },
        .on_stream_connected =
            [](AcmpDu const& acmp, StreamConnection const&) {
                std::println(
                    "{} CONNECT    talker={} listener={} stream_id={} dest_mac={} connections={}",
                    ts(),
                    format_stream_port(acmp.talker_entity_id, acmp.talker_unique_id.get()),
                    format_stream_port(acmp.listener_entity_id, acmp.listener_unique_id.get()),
                    ieee::to_string(acmp.stream_id).view(),
                    ieee::to_string(acmp.stream_dest_mac).view(),
                    acmp.connection_count.get());
            },
        .on_stream_disconnected =
            [](AcmpDu const& acmp) {
                std::println(
                    "{} DISCONNECT talker={} listener={}",
                    ts(),
                    format_stream_port(acmp.talker_entity_id, acmp.talker_unique_id.get()),
                    format_stream_port(acmp.listener_entity_id, acmp.listener_unique_id.get()));
            },
        .on_acmp_state =
            [](AcmpDu const& acmp) {
                std::println(
                    "{} STATE      {} talker={} listener={}",
                    ts(),
                    acmp_message_type_name(acmp.message_type()),
                    format_stream_port(acmp.talker_entity_id, acmp.talker_unique_id.get()),
                    format_stream_port(acmp.listener_entity_id, acmp.listener_unique_id.get()));
            },
        .on_acmp_error =
            [](AcmpDu const& acmp) {
                std::println(
                    "{} ACMP_ERROR {} status={} talker={} listener={}",
                    ts(),
                    acmp_message_type_name(acmp.message_type()),
                    acmp_status_name(acmp.status()),
                    format_stream_port(acmp.talker_entity_id, acmp.talker_unique_id.get()),
                    format_stream_port(acmp.listener_entity_id, acmp.listener_unique_id.get()));
            },
    };
}

// ---------------------------------------------------------------------------
// MonitorCore — frame dispatch, controller, callbacks, stream tracking.
// Shared between live (MonitorPollable) and offline (run_pcap_file) drivers.
// ---------------------------------------------------------------------------

using SendUnicastFn = statusbar::sg14::inplace_function<bool(Eui48 const&, std::span<uint8_t const>), 64>;
using SendMulticastFn = statusbar::sg14::inplace_function<bool(std::span<uint8_t const>), 64>;

class MonitorCore
{
  public:
    MonitorCore(Eui64 controller_id, MonitorCallbacks callbacks, bool show_descriptors)
        : controller_{controller_id}
        , callbacks_{std::move(callbacks)}
        , show_descriptors_{show_descriptors}
    {
        wire_controller();
        controller_.start();
    }

    void set_senders(SendMulticastFn send_mcast, SendUnicastFn send_unicast)
    {
        send_mcast_ = std::move(send_mcast);
        send_unicast_ = std::move(send_unicast);
    }

    /// Send initial ENTITY_DISCOVER broadcast. Call after set_senders in live
    /// mode. Skipped in pcap mode (no sender).
    void discover_all() { controller_.discover_all(); }

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
                if (show_descriptors_) {
                    (void)maybe_print_read_descriptor(payload);
                }
                controller_.receive_aecp(payload, now_ns);
                break;
            case avtp::AvtpSubtype::acmp:
                dispatch_acmp(payload, now_ns);
                break;
            default:
                break;
        }
    }

    void tick(int64_t now_ns) { controller_.tick(now_ns); }

    [[nodiscard]] auto active_streams() const noexcept -> StreamMap const& { return active_streams_; }

  private:
    void wire_controller()
    {
        controller_.set_callbacks({
            .send_atdecc_multicast = [this](std::span<uint8_t const> pkt) -> bool {
                return send_mcast_ ? send_mcast_(pkt) : false;
            },
            .send_atdecc_unicast = [this](Eui48 const& dst, std::span<uint8_t const> pkt) -> bool {
                return send_unicast_ ? send_unicast_(dst, pkt) : false;
            },
            .on_entity_available =
                [this](DiscoveredEntity const& e) {
                    if (callbacks_.on_entity_available) {
                        callbacks_.on_entity_available(e);
                    }
                    controller_.read_descriptor(e.adpdu.entity_id, DESCRIPTOR_ENTITY, 0);
                },
            .on_entity_updated =
                [this](DiscoveredEntity const& e) {
                    if (callbacks_.on_entity_updated) {
                        callbacks_.on_entity_updated(e);
                    }
                    controller_.read_descriptor(e.adpdu.entity_id, DESCRIPTOR_ENTITY, 0);
                },
            .on_entity_departing =
                [this](Eui64 id) {
                    if (callbacks_.on_entity_departing) {
                        callbacks_.on_entity_departing(id);
                    }
                },
            .on_aem_response =
                [this](Eui64 t, uint16_t c, uint8_t s, std::span<uint8_t const> /*sent*/, std::span<uint8_t const> d) {
                    if (callbacks_.on_aem_response) {
                        callbacks_.on_aem_response(controller_, t, c, s, d);
                    }
                },
            .on_aem_timeout =
                [this](Eui64 t, uint16_t c) {
                    if (callbacks_.on_aem_timeout) {
                        callbacks_.on_aem_timeout(controller_, t, c);
                    }
                },
        });
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
        if (!acmp.is_response()) {
            return;
        }
        handle_acmp_response(acmp, now_ns);
    }

    void handle_acmp_response(AcmpDu const& acmp, int64_t now_ns)
    {
        if (acmp.status() != ACMP_STATUS_SUCCESS) {
            if (callbacks_.on_acmp_error) {
                callbacks_.on_acmp_error(acmp);
            }
            return;
        }
        switch (acmp.message_type()) {
            case ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE:
            case ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE:
                handle_connect(acmp, now_ns);
                break;
            case ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE:
            case ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE:
                handle_disconnect(acmp);
                break;
            case ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE:
            case ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE:
                if (callbacks_.on_acmp_state) {
                    callbacks_.on_acmp_state(acmp);
                }
                break;
            default:
                break;
        }
    }

    void handle_connect(AcmpDu const& acmp, int64_t now_ns)
    {
        StreamConnection conn{
            .talker_entity_id = acmp.talker_entity_id,
            .talker_unique_id = acmp.talker_unique_id.get(),
            .listener_entity_id = acmp.listener_entity_id,
            .listener_unique_id = acmp.listener_unique_id.get(),
            .stream_id = acmp.stream_id,
            .stream_dest_mac = acmp.stream_dest_mac,
            .connect_time_ns = now_ns,
            .connection_count = acmp.connection_count.get(),
        };
        StreamKey const key{acmp.listener_entity_id, acmp.listener_unique_id.get()};
        active_streams_[key] = conn;
        if (callbacks_.on_stream_connected) {
            callbacks_.on_stream_connected(acmp, conn);
        }
    }

    void handle_disconnect(AcmpDu const& acmp)
    {
        StreamKey const key{acmp.listener_entity_id, acmp.listener_unique_id.get()};
        active_streams_.erase(key);
        if (callbacks_.on_stream_disconnected) {
            callbacks_.on_stream_disconnected(acmp);
        }
    }

    nanoavb::NanoAvbAemController controller_;
    MonitorCallbacks callbacks_;
    StreamMap active_streams_;
    SendMulticastFn send_mcast_;
    SendUnicastFn send_unicast_;
    bool show_descriptors_;
};

// ---------------------------------------------------------------------------
// MonitorPollable — live-socket driver wrapping MonitorCore.
// ---------------------------------------------------------------------------

class MonitorPollable : public Pollable
{
  public:
    MonitorPollable(RawnetContext context, MonitorCore& core)
        : context_{std::move(context)}
        , core_{core}
    {
        core_.set_senders(
            [this](std::span<uint8_t const> pkt) -> bool { return context_.send(&ATDECC_MULTICAST_MAC, pkt).has_value(); },
            [this](Eui48 const& dst, std::span<uint8_t const> pkt) -> bool { return context_.send(&dst, pkt).has_value(); });
        core_.discover_all();
    }

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
            core_.dispatch_frame(now_ns, src_mac, {payload_buf_.data(), len});
        }
    }

    void tick(int64_t now_ns) override { core_.tick(now_ns); }

    [[nodiscard]] auto finished() const noexcept -> bool override { return false; }

  private:
    RawnetContext context_;
    MonitorCore& core_;
    std::array<uint8_t, 2048> payload_buf_{};
};

// ---------------------------------------------------------------------------
// Offline driver — read pcap/pcapng and feed frames into MonitorCore.
// ---------------------------------------------------------------------------

auto run_pcap_file(MonitorCore& core, std::string const& path) -> bool
{
    auto reader_result = pcap::PcapngReader::open(path);
    if (!reader_result) {
        std::println(stderr, "Error: open pcap '{}': {}", path, reader_result.error().message());
        return false;
    }
    g_now_fn = pcap_now_ns;
    size_t packet_count = 0;
    size_t avtp_count = 0;
    auto const walk = pcap::for_each_packet(
        *reader_result,
        [&](uint64_t timestamp_us, Eui48 const& /*da*/, Eui48 const& sa, uint16_t ethertype, pcap::Packet const& payload) {
            ++packet_count;
            if (ethertype != avtp::AVTP_ETHERTYPE) {
                return;
            }
            ++avtp_count;
            g_pcap_now_ns = static_cast<int64_t>(timestamp_us) * 1000;
            core.dispatch_frame(g_pcap_now_ns, sa, std::span<uint8_t const>{payload.data(), payload.size()});
            core.tick(g_pcap_now_ns);
        });
    if (!walk) {
        std::println(stderr, "Error: pcap read failed at packet {}: {}", packet_count, walk.error().message());
        return false;
    }
    std::println(stderr, "\nProcessed {} packets ({} AVTP/ATDECC) from {}", packet_count, avtp_count, path);
    return true;
}

// ---------------------------------------------------------------------------
// Stream summary
// ---------------------------------------------------------------------------

void print_active_streams(StreamMap const& streams, int64_t now_ns)
{
    if (streams.empty()) {
        std::println("\nActive streams: 0");
        return;
    }
    std::println("\nActive streams: {}", streams.size());
    for (auto const& [key, conn] : streams) {
        int64_t const age_ms = (now_ns - conn.connect_time_ns) / 1'000'000;
        std::println(
            "  talker={} -> listener={} stream_id={} (connected {}.{}s ago)",
            format_stream_port(conn.talker_entity_id, conn.talker_unique_id),
            format_stream_port(conn.listener_entity_id, conn.listener_unique_id),
            ieee::to_string(conn.stream_id).view(),
            age_ms / 1000,
            age_ms % 1000);
    }
}

// ---------------------------------------------------------------------------
// Enumerator — headless full descriptor-tree walk (--enumerate).
//
// The passive monitor only auto-reads the ENTITY descriptor. The enumerator
// actively walks the whole AEM model of each discovered entity (optionally
// filtered to --target): ENTITY -> CONFIGURATION -> every (type, index) listed
// in the configuration's descriptor_counts, pretty-printing each, then signals
// completion so the tool exits (vs the monitor which runs until Ctrl-C).
//
// MonitorCore already auto-issues READ_DESCRIPTOR(ENTITY,0) on discovery; that
// response seeds the walk via on_response(). Subsequent reads are issued here,
// bounded by MAX_INFLIGHT to stay under the AEM controller's in-flight cap.
// ---------------------------------------------------------------------------

[[nodiscard]] auto be16_at(std::span<uint8_t const> b, size_t off) -> uint16_t
{
    return (off + 1 < b.size()) ? static_cast<uint16_t>((static_cast<uint16_t>(b[off]) << 8) | b[off + 1]) : uint16_t{0};
}

class Enumerator
{
  public:
    Enumerator(std::optional<Eui64> target, int64_t quiet_ns)
        : target_filter_{target}
        , quiet_ns_{quiet_ns}
    {}

    // From MonitorCallbacks.on_entity_available / on_entity_updated. MonitorCore
    // auto-issues the ENTITY read; we just register the target and note timing.
    void on_entity(DiscoveredEntity const& e, int64_t now_ns)
    {
        Eui64 const id = e.adpdu.entity_id;
        if (target_filter_ && !eui_eq(id, *target_filter_)) {
            return;
        }
        last_discovery_ns_ = now_ns;
        if (targets_.emplace(id, TargetState{}).second) {
            std::println(
                "\n=== entity {}  caps=[{}] ===",
                ieee::to_string(id).view(),
                entity_capabilities_to_string(e.adpdu.entity_capabilities.get()));
        }
    }

    // From MonitorCallbacks.on_aem_response.
    void on_response(
        nanoavb::NanoAvbAemController& controller, Eui64 target, uint16_t cmd, uint8_t status, std::span<uint8_t const> data)
    {
        controller_ = &controller;
        if (cmd != AEM_COMMAND_READ_DESCRIPTOR) {
            return;
        }
        auto it = targets_.find(target);
        if (it == targets_.end()) {
            return;  // not one we're walking
        }
        if (data.size() < AemReadDescriptorResponsePayload::LENGTH) {
            return;
        }
        auto const body = data.subspan(AemReadDescriptorResponsePayload::LENGTH);

        if (status != AEM_STATUS_SUCCESS || body.size() < 4) {
            // A read we issued failed; release its slot and keep going. (The
            // ENTITY read is auto-issued by MonitorCore and not counted.)
            if (enum_debug()) {
                std::println(stderr, "[enum] error resp: status={} data_len={}", status, data.size());
            }
            release_one();
            return;
        }

        uint16_t const dtype = be16_at(body, 0);
        uint16_t const dindex = be16_at(body, 2);
        if (enum_debug()) {
            std::println(
                stderr,
                "[enum] resp type={:#06x} idx={} status={} blen={} q={} out={}",
                dtype,
                dindex,
                status,
                body.size(),
                queue_.size(),
                outstanding_);
        }
        bool const is_entity = (dtype == DESCRIPTOR_ENTITY);
        if (!is_entity) {
            release_one();
        }

        ReadKey const key{target, dtype, dindex};
        if (printed_.insert(key).second) {
            std::string out;
            format_descriptor(std::back_inserter(out), body);
            std::print("{}", out);
        }
        if (expanded_.insert(key).second) {
            if (dtype == DESCRIPTOR_ENTITY) {
                it->second.seen_entity = true;
                // configurations_count at offset 308 of the ENTITY descriptor.
                uint16_t const n_configs = be16_at(body, 308);
                if (enum_debug()) {
                    std::println(stderr, "[enum] ENTITY n_configs={}", n_configs);
                }
                for (uint16_t c = 0; c < n_configs; ++c) {
                    enqueue(target, DESCRIPTOR_CONFIGURATION, c);
                }
            } else if (dtype == DESCRIPTOR_CONFIGURATION) {
                it->second.seen_config = true;
                // descriptor_counts: count at offset 70, table offset at 72.
                // Both counts are untrusted uint16 -- bound them so a hostile
                // CONFIGURATION can't drive ~4 billion enqueues (M2 DoS).
                uint16_t const counts = std::min<uint16_t>(be16_at(body, 70), MAX_DESCRIPTOR_TYPES);
                uint16_t off = be16_at(body, 72);
                if (off < 74) {
                    off = 74;
                }
                for (uint16_t i = 0; i < counts; ++i) {
                    size_t const base = static_cast<size_t>(off) + (static_cast<size_t>(i) * 4);
                    uint16_t const t = be16_at(body, base);
                    uint16_t const cnt = std::min<uint16_t>(be16_at(body, base + 2), MAX_DESCRIPTORS_PER_TYPE);
                    if (t == DESCRIPTOR_ENTITY || t == DESCRIPTOR_CONFIGURATION) {
                        continue;  // already covered; avoid recursion
                    }
                    for (uint16_t k = 0; k < cnt; ++k) {
                        enqueue(target, t, k);
                    }
                }
            }
        }
        // NOTE: do not issue reads here. This callback runs inside the
        // controller's receive_aecp(); issuing read_descriptor re-entrantly can
        // mutate its in-flight list mid-dispatch. The main loop calls pump().
    }

    // From MonitorCallbacks.on_aem_timeout.
    void on_timeout(Eui64 /*target*/, uint16_t /*cmd*/) { release_one(); }

    // Issue queued reads up to the in-flight cap. Safe to call from the main
    // loop too, so progress continues even if responses are sparse.
    void pump()
    {
        if (controller_ == nullptr) {
            return;
        }
        while (outstanding_ < MAX_INFLIGHT && !queue_.empty()) {
            auto [t, type, index] = queue_.front();
            queue_.pop_front();
            bool const ok = controller_->read_descriptor(t, type, index);
            if (enum_debug()) {
                std::println(stderr, "[enum] read type={:#06x} idx={} ok={}", type, index, ok);
            }
            if (ok) {
                ++outstanding_;
            } else {
                queue_.push_front({t, type, index});  // controller busy; retry next pump
                break;
            }
        }
    }

    [[nodiscard]] auto finished(int64_t now_ns) const -> bool
    {
        if (targets_.empty() || !queue_.empty() || outstanding_ > 0) {
            return false;
        }
        for (auto const& [id, st] : targets_) {
            if (!st.seen_entity || !st.seen_config) {
                return false;
            }
        }
        // Explicit single target: done as soon as its tree is walked. Otherwise
        // wait out a quiet window so slow-to-advertise entities aren't missed.
        return target_filter_.has_value() || (now_ns - last_discovery_ns_) > quiet_ns_;
    }

    [[nodiscard]] auto entity_count() const noexcept -> size_t { return targets_.size(); }

  private:
    struct TargetState
    {
        bool seen_entity{false};
        bool seen_config{false};
    };
    using ReadKey = std::tuple<Eui64, uint16_t, uint16_t>;
    static constexpr int MAX_INFLIGHT = 4;

    static auto eui_eq(Eui64 const& a, Eui64 const& b) -> bool { return !(a < b) && !(b < a); }

    void release_one() noexcept
    {
        if (outstanding_ > 0) {
            --outstanding_;
        }
    }

    void enqueue(Eui64 t, uint16_t type, uint16_t index)
    {
        if (queued_.insert(ReadKey{t, type, index}).second) {
            queue_.push_back({t, type, index});
        }
    }

    std::optional<Eui64> target_filter_;
    int64_t quiet_ns_;
    int64_t last_discovery_ns_{0};
    nanoavb::NanoAvbAemController* controller_{nullptr};
    std::map<Eui64, TargetState> targets_;
    std::deque<std::tuple<Eui64, uint16_t, uint16_t>> queue_;
    std::set<ReadKey> queued_;
    std::set<ReadKey> printed_;
    std::set<ReadKey> expanded_;
    int outstanding_{0};
};

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

struct MonitorConfig : CommonConfig
{
    std::string pcap_file;
    bool show_descriptors{false};
    bool enumerate{false};
    std::optional<Eui64> target;
    double enumerate_secs{15.0};
};

auto build_arg_specs(MonitorConfig& config) -> args::ArgumentSpecs
{
    args::ArgumentSpecs specs;
    atdecc_tools::add_common_arg_specs(specs, config);
    specs.add_file("pcap", "Read frames from a pcap/pcapng file instead of a live interface", "", [&](auto v) {
        config.pcap_file = std::string{v};
    });
    specs.add_flag("show-descriptors", "Pretty-print every READ_DESCRIPTOR command/response on the wire", [&](auto v) {
        config.show_descriptors = v;
    });
    specs.add_flag(
        "enumerate",
        "Actively walk the full AEM descriptor tree of each discovered entity, print it, then exit (headless)",
        [&](auto v) { config.enumerate = v; });
    specs.add<ieee::Eui64>(
        "target", "Limit --enumerate to this entity ID (EUI-64); default: all discovered", ieee::Eui64{}, [&](auto v) {
            // Only engage the filter for a real (non-zero) EUI-64 — the arg
            // framework may invoke this with the all-zero default.
            if (ieee::Eui64{} < v || v < ieee::Eui64{}) {
                config.target = v;
            }
        });
    specs.add<double>("enumerate-secs", "Overall timeout for --enumerate (seconds)", config.enumerate_secs, [&](auto v) {
        config.enumerate_secs = v;
    });
    return specs;
}

void print_usage(char const* prog, args::ArgumentSpecs const& specs)
{
    config::default_print_usage(
        prog,
        specs,
        "Monitor ATDECC entities (ADP), descriptors (AEM), and stream connections (ACMP).\n"
        "Frames are read from a live network interface (--interface=<name>) or a "
        "pcap/pcapng file (--pcap=<file>).");
}

auto run_live(MonitorConfig const& config) -> int
{
    auto id_result = atdecc_tools::resolve_controller_id(config, atdecc_tools::TOOL_ID_DISCOVER);
    if (!id_result.has_value()) {
        std::println(stderr, "Error: cannot open interface '{}': {}", config.interface_name, id_result.error().message());
        return 1;
    }
    Eui64 const controller_id = *id_result;
    std::println(
        "statusbar-atdecc-monitor on interface '{}' controller_id={}",
        config.interface_name,
        ieee::to_string(controller_id).view());

    RawnetContext rawnet;
    auto open_result = rawnet.open(config.interface_name, avtp::AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC);
    if (!open_result) {
        std::println(stderr, "Error: failed to open raw socket on '{}' (root/cap_net_raw required)", config.interface_name);
        return 1;
    }

    g_now_fn = elapsed_ns;
    auto core =
        std::make_unique<MonitorCore>(controller_id, make_print_callbacks(config.show_descriptors), config.show_descriptors);
    auto* core_ptr = core.get();
    auto monitor = std::make_unique<MonitorPollable>(std::move(rawnet), *core_ptr);

    auto& stop = statusbar::itc::install_stop_signal();
    MessageReactor reactor{stop, elapsed_ns, 10};
    reactor.add(std::move(monitor));

    std::println("Listening... (Ctrl-C to stop)");
    reactor.run();

    print_active_streams(core_ptr->active_streams(), elapsed_ns());
    std::println("\nShutdown.");
    return 0;
}

auto run_offline(MonitorConfig const& config) -> int
{
    // In pcap mode we have no real MAC, so synthesize a controller ID. It
    // never goes on the wire — the controller's senders are no-ops.
    Eui64 const controller_id{};
    std::println("statusbar-atdecc-monitor reading pcap '{}'", config.pcap_file);

    MonitorCore core{controller_id, make_print_callbacks(config.show_descriptors), config.show_descriptors};
    // No set_senders — send_mcast_ / send_unicast_ stay null, outgoing frames
    // silently drop.
    auto const result = statusbar::catch_or_status(
        [&]() -> statusbar::Status {
            (void)run_pcap_file(core, config.pcap_file);
            return {};
        },
        std::errc::io_error);
    if (!result) {
        std::println(stderr, "Error reading pcap '{}': {}", config.pcap_file, result.error().message());
        return 1;
    }
    print_active_streams(core.active_streams(), g_pcap_now_ns);
    return 0;
}

auto run_enumerate(MonitorConfig const& config) -> int
{
    auto id_result = atdecc_tools::resolve_controller_id(config, atdecc_tools::TOOL_ID_DISCOVER);
    if (!id_result.has_value()) {
        std::println(stderr, "Error: cannot open interface '{}': {}", config.interface_name, id_result.error().message());
        return 1;
    }
    Eui64 const controller_id = *id_result;
    std::println(
        "statusbar-atdecc-monitor enumerate on '{}' controller_id={}",
        config.interface_name,
        ieee::to_string(controller_id).view());

    RawnetContext rawnet;
    auto open_result = rawnet.open(config.interface_name, avtp::AVTP_ETHERTYPE, &ATDECC_MULTICAST_MAC);
    if (!open_result) {
        std::println(stderr, "Error: failed to open raw socket on '{}' (root/cap_net_raw required)", config.interface_name);
        return 1;
    }

    g_now_fn = elapsed_ns;

    constexpr int64_t QUIET_NS = 2'500'000'000;  // no-new-entity window for enumerate-all
    Enumerator enumerator{config.target, QUIET_NS};

    MonitorCallbacks cbs{};
    cbs.on_entity_available = [&enumerator](DiscoveredEntity const& e) { enumerator.on_entity(e, elapsed_ns()); };
    cbs.on_entity_updated = [&enumerator](DiscoveredEntity const& e) { enumerator.on_entity(e, elapsed_ns()); };
    cbs.on_aem_response =
        [&enumerator](nanoavb::NanoAvbAemController& c, Eui64 t, uint16_t cmd, uint8_t s, std::span<uint8_t const> d) {
            enumerator.on_response(c, t, cmd, s, d);
        };
    cbs.on_aem_timeout = [&enumerator](nanoavb::NanoAvbAemController& /*c*/, Eui64 t, uint16_t cmd) {
        enumerator.on_timeout(t, cmd);
    };

    auto core = std::make_unique<MonitorCore>(controller_id, std::move(cbs), /*show_descriptors=*/false);
    auto* core_ptr = core.get();
    auto monitor = std::make_unique<MonitorPollable>(std::move(rawnet), *core_ptr);

    auto& stop = statusbar::itc::install_stop_signal();
    MessageReactor reactor{stop, elapsed_ns, 10};
    reactor.add(std::move(monitor));

    int64_t const deadline = elapsed_ns() + static_cast<int64_t>(config.enumerate_secs * 1e9);
    std::println("Enumerating (timeout {:.0f}s, Ctrl-C to stop)...", config.enumerate_secs);
    while (!stop.stop_requested()) {
        (void)reactor.poll_once(100);
        enumerator.pump();
        int64_t const now = elapsed_ns();
        if (enumerator.finished(now) || now >= deadline) {
            break;
        }
    }

    bool const done = enumerator.finished(elapsed_ns());
    size_t const n = enumerator.entity_count();
    std::println(
        "\n{} ({} entit{} enumerated).", done ? "Enumeration complete" : "Enumeration stopped (timeout)", n, n == 1 ? "y" : "ies");
    return done ? 0 : 2;
}

}  // namespace

int main(int argc, char* argv[])
{
    using namespace statusbar;

    MonitorConfig config;
    auto specs = build_arg_specs(config);

    auto cli_result = config::parse_cli_args(argc, argv, specs, print_usage, "statusbar-atdecc-monitor");
    if (!cli_result) {
        return config::handled_builtin_command(cli_result) ? 0 : 1;
    }

    bool const has_iface = !config.interface_name.empty();
    bool const has_pcap = !config.pcap_file.empty();
    if (has_iface == has_pcap) {
        std::println(stderr, "Error: exactly one of --interface or --pcap is required");
        return 1;
    }

    if (config.enumerate) {
        if (has_pcap) {
            std::println(stderr, "Error: --enumerate requires --interface (it actively queries entities)");
            return 1;
        }
        return run_enumerate(config);
    }

    return has_pcap ? run_offline(config) : run_live(config);
}
