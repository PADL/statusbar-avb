// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit at its stream bound, with no network: MAX_ENTITY_STREAMS outputs and
// inputs (16 AAF 32-channel + 4 CRF each way) all open; one more is refused when
// the tables are derived, and an extra slot by the stream paths themselves.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_acmp.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_shaper.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/nanoavb/nanoavb_test_support.hpp"
#include "statusbar/net/net_ethernet_port.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_test_port.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/srp/srp_msrp_test_support.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using atdecc::aem::DESCRIPTOR_AVB_INTERFACE;
using atdecc::aem::DESCRIPTOR_CONFIGURATION;
using atdecc::aem::DESCRIPTOR_ENTITY;
using atdecc::aem::DESCRIPTOR_STREAM_INPUT;
using atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;

namespace {

/// AAF 48 kHz INT_24, 32 channels, 6 frames per packet (class A): subtype
/// 0x02, nsr 5, format 3, bit depth 24, channels << 22, frames << 12.
constexpr uint64_t AAF_48K_INT24_32CH = 0x0205031800000000ULL | (uint64_t{32} << 22) | (uint64_t{6} << 12);
/// Milan CRF: AUDIO_SAMPLE, 48 kHz base, interval 96, one timestamp a PDU.
constexpr uint64_t CRF_MILAN_48K = 0x041060010000BB80ULL;

template <typename Descriptor>
auto wire(Descriptor const& desc) -> std::span<uint8_t const>
{
    return std::span{reinterpret_cast<uint8_t const*>(&desc), desc.wire_size()};
}

/// ENTITY + CONFIGURATION + AVB_INTERFACE (with @p priority1, @p clock_identity), @p audio AAF streams then @p crf CRF
/// streams in each direction; with @p identify, CONTROL 0 is an IDENTIFY control.
auto make_blob(
    uint16_t const audio_outputs,
    uint16_t const audio_inputs,
    uint16_t const crf = 1,
    bool const identify = false,
    uint8_t const priority1 = 0,
    ieee::Eui64 const clock_identity = {}) -> std::vector<uint8_t>
{
    nanoavb::AemBlobWriter w;
    atdecc::aem::DescriptorEntity entity{};
    entity.configurations_count = 1;
    entity.talker_stream_sources = static_cast<uint16_t>(audio_outputs + crf);
    entity.listener_stream_sinks = static_cast<uint16_t>(audio_inputs + crf);
    w.add(0, DESCRIPTOR_ENTITY, 0, make_const_span(entity));
    atdecc::aem::DescriptorConfiguration config{};
    w.add(0, DESCRIPTOR_CONFIGURATION, 0, make_const_span(config));
    for (auto const [type, audio] :
         {std::pair{DESCRIPTOR_STREAM_OUTPUT, audio_outputs}, std::pair{DESCRIPTOR_STREAM_INPUT, audio_inputs}}) {
        for (uint16_t index = 0; index < audio + crf; ++index) {
            atdecc::aem::DescriptorStream stream{};
            stream.descriptor_type = type;
            stream.descriptor_index = index;
            stream.current_format.from_uint64(index < audio ? AAF_48K_INT24_32CH : CRF_MILAN_48K);
            w.add(0, type, index, wire(stream));
        }
    }
    atdecc::aem::DescriptorAvbInterface iface{};
    iface.priority1 = priority1;
    iface.clock_identity = clock_identity;
    w.add(0, DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));
    if (identify) {
        atdecc::aem::DescriptorControl control{};
        control.control_type = atdecc::aem::CONTROL_TYPE_IDENTIFY;
        control.control_value_type = 0x0001;  // CONTROL_LINEAR_UINT8
        control.number_of_values = 1;
        w.add(0, atdecc::aem::DESCRIPTOR_CONTROL, 0, wire(control));
    }
    return w.write();
}

/// The multi-rate model's AAF formats: INT_24 8 ch, and the Base formats
/// (Base Audio Formats FIS v1.1 Table 4: INT_32, class A framing).
constexpr uint64_t INT24_8CH_48K = 0x0205031802006000ULL;
constexpr uint64_t INT24_8CH_96K = 0x020703180200C000ULL;
constexpr auto base_format(uint32_t const rate, uint16_t const channels) -> uint64_t
{
    uint64_t const nsr = rate == 96000 ? 0x07 : 0x05;
    return (uint64_t{0x02} << 56) | (nsr << 48) | (uint64_t{0x02} << 40) | (uint64_t{32} << 32) | (uint64_t{channels} << 22) |
        (uint64_t{rate / 8000} << 12);
}
static_assert(base_format(48000, 2) == 0x0205022000806000ULL && base_format(96000, 8) == 0x020702200200C000ULL);

/// IEC 61883-6 AM824, 8 MBLA channels at 48 kHz (SFC 2).
constexpr uint64_t AM824_8CH_48K = 0x00a0020840000800ULL;

/// A multi-rate model: 8 ch INT_24 output and input (+ CRF each) at 48/96 kHz.
/// With @p ports, STREAM_PORT_INPUT 0 has eight 1-channel clusters and no
/// AUDIO_MAP, plus every Milan v1.2 Section 5.3.13 name's descriptor. With
/// @p one_rate the unit lists 48 kHz only and STREAM_PORT_OUTPUT 0 is dynamic.
/// With @p am824 both audio streams also list AM824_8CH_48K.
/// With @p narrow_output the output lists the 2-channel Base format too (and has no map to forbid it).
/// @p port_flags are both stream ports' (IEEE 1722.1-2021 Clause 7.2.13.1).
auto make_multirate_blob(
    bool const ports = false,
    bool const one_rate = false,
    bool const am824 = false,
    bool const narrow_output = false,
    uint16_t const port_flags = 0,
    bool const dynamic_output = false) -> std::vector<uint8_t>
{
    nanoavb::AemBlobWriter w;
    atdecc::aem::DescriptorEntity entity{};
    entity.configurations_count = 1;
    entity.talker_stream_sources = 2;
    entity.listener_stream_sinks = 2;
    w.add(0, DESCRIPTOR_ENTITY, 0, make_const_span(entity));
    atdecc::aem::DescriptorConfiguration config{};
    w.add(0, DESCRIPTOR_CONFIGURATION, 0, make_const_span(config));
    for (auto const type : {DESCRIPTOR_STREAM_OUTPUT, DESCRIPTOR_STREAM_INPUT}) {
        atdecc::aem::DescriptorStream stream{};
        stream.descriptor_type = type;
        stream.descriptor_index = 0;
        stream.current_format.from_uint64(INT24_8CH_48K);
        for (uint32_t const rate : {48000U, 96000U}) {
            (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(rate == 48000 ? INT24_8CH_48K : INT24_8CH_96K));
            for (uint16_t const channels : {1, 2, 4, 6, 8}) {
                if (type == DESCRIPTOR_STREAM_INPUT || channels == 8 || ports || (narrow_output && channels == 2)) {
                    (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(base_format(rate, channels)));
                }
            }
        }
        if (am824) {
            // And each kind the other stream has, which the kit refuses.
            (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(AM824_8CH_48K));
            (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(CRF_MILAN_48K));
        }
        w.add(0, type, 0, wire(stream));
        atdecc::aem::DescriptorStream crf{};
        crf.descriptor_type = type;
        crf.descriptor_index = 1;
        crf.current_format.from_uint64(CRF_MILAN_48K);
        if (am824) {
            (void)crf.push_stream_format(ieee::Eui64{}.from_uint64(AM824_8CH_48K));
        }
        w.add(0, type, 1, wire(crf));
    }
    atdecc::aem::DescriptorAudioUnit unit{};
    unit.current_sampling_rate = uint32_t{48000};
    unit.sampling_rates_count = static_cast<uint16_t>(one_rate ? 1 : 2);
    unit.sampling_rates[0] = uint32_t{48000};
    unit.sampling_rates[1] = static_cast<uint32_t>(one_rate ? 0 : 96000);
    if (ports) {
        unit.number_of_stream_input_ports = uint16_t{1};
        atdecc::aem::DescriptorStreamPort port{};
        port.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
        port.port_flags = port_flags;
        port.number_of_clusters = uint16_t{8};
        w.add(0, atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, 0, make_const_span(port));
        for (uint16_t c = 0; c < 8; ++c) {
            atdecc::aem::DescriptorAudioCluster cluster{};
            cluster.descriptor_index = c;
            cluster.channel_count = uint16_t{1};
            w.add(0, atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER, c, make_const_span(cluster));
        }
        // STREAM_PORT_OUTPUT 0: one 8-channel cluster under a static
        // identity AUDIO_MAP (and the output lists narrower Base formats
        // than its map: the kit's backstop refuses them).
        unit.number_of_stream_output_ports = uint16_t{1};
        atdecc::aem::DescriptorStreamPort out_port{};
        out_port.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;
        out_port.port_flags = port_flags;
        out_port.number_of_clusters = uint16_t{1};
        out_port.base_cluster = uint16_t{8};
        out_port.number_of_maps = static_cast<uint16_t>(one_rate || dynamic_output ? 0 : 1);
        w.add(0, atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, 0, make_const_span(out_port));
        atdecc::aem::DescriptorAudioCluster out_cluster{};
        out_cluster.descriptor_index = uint16_t{8};
        out_cluster.channel_count = uint16_t{8};
        w.add(0, atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER, 8, make_const_span(out_cluster));
        atdecc::aem::DescriptorAudioMap map{};
        for (uint16_t ch = 0; ch < 8; ++ch) {
            (void)map.push_mapping(
                {.mapping_stream_index = uint16_t{0},
                 .mapping_stream_channel = ch,
                 .mapping_cluster_offset = uint16_t{0},
                 .mapping_cluster_channel = ch});
        }
        if (!one_rate && !dynamic_output) {
            w.add(0, atdecc::aem::DESCRIPTOR_AUDIO_MAP, 0, wire(map));
        }
        atdecc::aem::DescriptorClockSource source{};
        w.add(0, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, 0, make_const_span(source));
        atdecc::aem::DescriptorClockDomain domain{};
        w.add(0, atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, 0, wire(domain));
        atdecc::aem::DescriptorControl control{};
        control.control_type = atdecc::aem::CONTROL_TYPE_IDENTIFY;
        w.add(0, atdecc::aem::DESCRIPTOR_CONTROL, 0, wire(control));
        atdecc::aem::DescriptorTiming timing{};
        w.add(0, atdecc::aem::DESCRIPTOR_TIMING, 0, wire(timing));
        atdecc::aem::DescriptorPtpInstance instance{};
        w.add(0, atdecc::aem::DESCRIPTOR_PTP_INSTANCE, 0, make_const_span(instance));
        atdecc::aem::DescriptorPtpPort ptp_port{};
        w.add(0, atdecc::aem::DESCRIPTOR_PTP_PORT, 0, make_const_span(ptp_port));
    }
    w.add(0, atdecc::aem::DESCRIPTOR_AUDIO_UNIT, 0, wire(unit));
    atdecc::aem::DescriptorAvbInterface iface{};
    w.add(0, DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));
    return w.write();
}

auto stream_id(uint64_t const value) -> tsn::StreamId
{
    tsn::StreamId sid{};
    ieee::Eui64 const eui = ieee::Eui64{}.from_uint64(value);
    (void)tsn::load_unchecked(eui.span(), &sid);
    return sid;
}

/// Whether ADP is advertising: up, waiting out TMR_ADVERTISE or TMR_DELAY (Milan v1.2 Table 5.51).
auto adp_advertising(nanoavb::NanoAvbAdpAdvertiser const& adp) -> bool
{
    auto const state = adp.state();
    return state == nanoavb::AdpAdvertiserState::Waiting || state == nanoavb::AdpAdvertiserState::Delay;
}

}  // namespace

namespace {

/// The controller the fixtures' AEM commands come from.
ieee::Eui64 const TEST_CONTROLLER{0x02, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x01};

struct MilanKit
{
    explicit MilanKit(
        std::optional<bool> const milan,
        uint64_t const entity_low,
        bool const gate = true,
        std::vector<uint8_t> model = make_blob(1, 1),
        std::optional<uint16_t> const crf_clock_source = std::nullopt,
        net::ClockFn clock = net::monotonic_ns,
        bool const signal_presence = false)
        : blob{milan == false ? nanoavb::with_classic_profile(model) : std::move(model)}
        , storage{*atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob})}
        , talkers{*talker_stream_specs(storage, 0)}
        , listeners{*listener_stream_specs(storage, 0)}
        , config{make_config(entity_low, gate, signal_presence, milan != false)}
        , handler{storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit milan", std::nullopt, /*patch_avb_interface=*/false}
        , kit{config,
              handler,
              talkers,
              listeners,
              48000,
              0,
              crf_clock_source,
              media_clock,
              last_gptp_ns,
              nullptr,
              std::pmr::get_default_resource()}
        , reactor{stop, std::move(clock), 0}
    {
        // A Milan entity's MSRP leaves at once on rLv! (Milan v1.3 Section 4.2.7.2.2), as atdecc.profile = "milan13".
        kit.host().components().msrp_handler.set_instant_leave(milan != false);
    }
    MilanKit(MilanKit const&) = delete;
    auto operator=(MilanKit const&) -> MilanKit& = delete;
    MilanKit(MilanKit&&) = delete;
    auto operator=(MilanKit&&) -> MilanKit& = delete;
    ~MilanKit() { (void)kit.stop(); }

    /// The kit's configuration.
    static auto make_config(uint64_t const entity_low, bool const gate, bool const signal_presence = false, bool const milan = true)
        -> AvbEntityAudioIOConfig
    {
        AvbEntityAudioIOConfig c{};
        c.signal_presence = signal_presence;
        c.gate_talker_on_listener = gate;
        c.interface_name = "kit-milan-test0";  // no such interface: no MAC, no sockets
        c.entity_id = ieee::Eui64{}.from_uint64(0x020000fffe000000ULL | entity_low);
        c.stream_address_mode = "static";
        c.milan = milan;          // a Milan model (entity.milan): milan13, and GET_MILAN_INFO to answer
        c.kit_data_plane = true;  // the kit's engines carry the audio: they re-time and route the identity
        return c;
    }

    /// GET_STREAM_INFO in Milan v1.2's 56 octets (flags_ex, pbsta, acmpsta) and SET_STREAM_INFO's
    /// presentation offset, for the tests of that form (AemCommandHandler::set_stream_info_layout).
    void use_milan_v1_2_stream_info()
    {
        kit.host().components().aem_handler.set_stream_info_layout(atdecc::aem::StreamInfoLayout::milan_v1_2);
    }

    /// Start, capture what the ACMP talker and the AEM handler send, and bring
    /// the control plane to Ready (link up, gPTP locked).
    auto start_ready() -> bool
    {
        if (!kit.start(reactor)) {
            return false;
        }
        auto& c = kit.host().components();
        c.acmp_talker.set_tx_response([this](atdecc::AcmpCommandResponse const& r) {
            acmp.push_back(r);
            return true;
        });
        if (c.milan_acmp_talker != nullptr) {
            c.milan_acmp_talker->set_tx_response([this](atdecc::AcmpCommandResponse const& r) {
                acmp.push_back(r);
                return true;
            });
        }
        c.aem_handler.set_send_response([this](ieee::Eui48 const&, std::span<uint8_t const> bytes) {
            aem.emplace_back(bytes.begin(), bytes.end());
            return true;
        });
        auto const now = sm::Clock::now();
        kit.on_link_up(now);
        kit.on_gptp_announce(now, true);
        return kit.host().is_ready();
    }

    [[nodiscard]] auto sid(uint16_t const idx) const -> tsn::StreamId
    {
        tsn::StreamId s{};
        (void)tsn::load_unchecked(kit.host().components().talker_stream(idx)->stream_id.span(), &s);
        return s;
    }

    [[nodiscard]] auto advertising(uint16_t const idx) const -> bool
    {
        auto const* t = kit.host().components().msrp_handler.get_talker_stream(sid(idx));
        using State = nanoavb::TalkerReservationState;
        return t != nullptr && t->state != State::Idle && t->state != State::Waiting;  // Waiting declares nothing
    }

    auto acmp_command(uint8_t const type, uint16_t const idx) -> atdecc::AcmpCommandResponse const&
    {
        atdecc::AcmpCommandResponse cmd{};
        cmd.init_command(type);
        cmd.talker_entity_id = config.entity_id;
        cmd.talker_unique_id = idx;
        cmd.listener_entity_id = ieee::Eui64{}.from_uint64(0x0200001111110001ULL);
        auto& c = kit.host().components();
        if (c.milan_acmp_talker != nullptr) {
            (void)c.milan_acmp_talker->receive_command(cmd, sm::Clock::now());
        } else {
            (void)c.acmp_talker.receive_command(cmd, sm::Clock::now());
        }
        return acmp.back();
    }

    struct AemResult
    {
        uint8_t status{0};
        std::vector<uint8_t> body;
    };
    auto aem_command(uint16_t const command, std::span<uint8_t const> body) -> AemResult
    {
        nanoavb::AemDu header{};
        header.init_command(command, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + body.size()));
        header.controller_entity_id = TEST_CONTROLLER;  // a real one: zero is nobody's (Milan v1.3 5.4.5.3)
        std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
        auto const r = kit.host().components().aem_handler.handle_command(header, body, out);
        return {.status = r.status, .body = std::vector<uint8_t>(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(r.size))};
    }
    auto stream_info(uint16_t const idx) -> AemResult
    {
        std::array<uint8_t, 4> body{0x00, 0x06, 0x00, static_cast<uint8_t>(idx)};
        return aem_command(atdecc::AEM_COMMAND_GET_STREAM_INFO, body);
    }

    /// A bridge port declaring a Listener @p decl for talker stream @p idx,
    /// its MSRPDUs fed to the entity until registered.
    void bridge_listener(uint16_t const idx, srp::msrp::ListenerDeclaration const decl)
    {
        if (!bridge_started) {
            bridge_now = sm::Clock::now();
            bridge.set_send_pdu([this](std::span<uint8_t const> p) {
                bridge_out.emplace_back(p.begin(), p.end());
                return true;
            });
            bridge.start(bridge_now);
            bridge_started = true;
        }
        (void)bridge.declare_listener(sid(idx), decl, bridge_now);
        for (int i = 0; i < 4; ++i) {
            bridge_now += std::chrono::milliseconds(250);
            bridge.tick(bridge_now);
            for (auto const& p : bridge_out) {
                kit.host().components().msrp_handler.receive_packet(p, bridge_now);
            }
            bridge_out.clear();
        }
    }

    /// The bridge port withdraws its Listener for talker stream @p idx.
    void bridge_withdraw(uint16_t const idx)
    {
        (void)bridge.withdraw_listener(sid(idx), bridge_now);
        for (int i = 0; i < 4; ++i) {
            bridge_now += std::chrono::milliseconds(250);
            bridge.tick(bridge_now);
            for (auto const& p : bridge_out) {
                kit.host().components().msrp_handler.receive_packet(p, bridge_now);
            }
            bridge_out.clear();
        }
    }

    static auto steady_ns() -> int64_t
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    /// The gate's clock (CLOCK_MONOTONIC_RAW), as the reactor reads it once per event.
    static auto raw_now() -> AvbEntityKit::GateTimePoint { return net::rx_clock_now(); }

    std::vector<uint8_t> blob;
    atdecc::aem::DescriptorStorage storage;
    StreamSpecs talkers;
    StreamSpecs listeners;
    AvbEntityAudioIOConfig config;
    EntityIdentityDescriptorHandler handler;
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<int64_t> last_gptp_ns{0};
    AvbEntityKit kit;
    itc::StopToken stop{};
    net::MessageReactor reactor;
    std::vector<atdecc::AcmpCommandResponse> acmp;
    std::vector<std::vector<uint8_t>> aem;
    srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9e};
    std::vector<std::vector<uint8_t>> bridge_out;
    sm::TimePoint bridge_now{};  ///< the bridge's clock, monotonic across declarations
    bool bridge_started{false};
};

auto load_info(std::vector<uint8_t> const& body)
    -> std::pair<atdecc::aem::AemStreamInfoPayload, atdecc::aem::AemStreamInfoMilanExtension>
{
    atdecc::aem::AemStreamInfoPayload info{};
    atdecc::aem::AemStreamInfoMilanExtension ext{};
    span_load(info, std::span<uint8_t const>{body}.first(atdecc::aem::AemStreamInfoPayload::LENGTH));
    if (body.size() == atdecc::aem::AEM_STREAM_INFO_MILAN_LENGTH) {
        span_load(ext, std::span<uint8_t const>{body}.subspan(atdecc::aem::AemStreamInfoPayload::LENGTH, 8));
    }
    return {info, ext};
}

}  // namespace

TEST(avb_entity_kit_bounds, a_full_model_derives_its_tables_and_one_more_is_refused)
{
    static_assert(MAX_ENTITY_STREAMS == MAX_ENTITY_AUDIO_STREAMS + MAX_ENTITY_CRF_STREAMS);
    constexpr auto audio = static_cast<uint16_t>(MAX_ENTITY_AUDIO_STREAMS);
    constexpr auto crf = static_cast<uint16_t>(MAX_ENTITY_CRF_STREAMS);

    auto const full = make_blob(audio, audio, crf);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{full});
    EXPECT_TRUE(storage.has_value());
    auto const talkers = talker_stream_specs(*storage, 0);
    auto const listeners = listener_stream_specs(*storage, 0);
    EXPECT_TRUE(talkers.has_value() && listeners.has_value());
    EXPECT_EQ(talkers->size(), MAX_ENTITY_STREAMS);
    EXPECT_EQ(listeners->size(), MAX_ENTITY_STREAMS);
    EXPECT_EQ(talkers->front().format.channels, uint16_t{32});
    EXPECT_EQ(talkers->back().format.kind, StreamKind::crf);
    EXPECT_EQ(listeners->back().format.kind, StreamKind::crf);
    EXPECT_EQ((*talkers)[audio].format.kind, StreamKind::crf);
    EXPECT_EQ((*listeners)[audio].format.kind, StreamKind::crf);

    // A 17th audio stream (21 descriptors with the CRFs) is past the bound, as is a fifth CRF.
    auto const outputs_over = make_blob(audio + 1, audio, crf);
    auto const over_out = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{outputs_over});
    EXPECT_TRUE(talker_stream_specs(*over_out, 0).error() == std::errc::result_out_of_range);
    EXPECT_TRUE(listener_stream_specs(*over_out, 0).has_value());
    auto const inputs_over = make_blob(audio, audio + 1, crf);
    auto const over_in = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{inputs_over});
    EXPECT_TRUE(listener_stream_specs(*over_in, 0).error() == std::errc::result_out_of_range);
    auto const crf_over = make_blob(audio, audio, crf + 1);
    auto const over_crf = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{crf_over});
    EXPECT_TRUE(talker_stream_specs(*over_crf, 0).error() == std::errc::result_out_of_range);
    EXPECT_TRUE(listener_stream_specs(*over_crf, 0).error() == std::errc::result_out_of_range);
}

TEST(avb_entity_kit_bounds, every_stream_of_a_full_model_opens)
{
    constexpr auto audio = static_cast<uint16_t>(MAX_ENTITY_AUDIO_STREAMS);
    constexpr auto crf = static_cast<uint16_t>(MAX_ENTITY_CRF_STREAMS);
    auto const blob = make_blob(audio, audio, crf);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob});
    EXPECT_TRUE(storage.has_value());
    auto const talkers = *talker_stream_specs(*storage, 0);
    auto const listeners = *listener_stream_specs(*storage, 0);

    AvbEntityAudioIOConfig config{};
    config.interface_name = "kit-bounds-test0";  // no such interface: no MAC, no sockets
    config.entity_id = ieee::Eui64{0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x01};
    config.stream_address_mode = "static";
    EntityIdentityDescriptorHandler handler{
        *storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit bounds", std::nullopt, /*patch_avb_interface=*/false};
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<int64_t> last_gptp_ns{0};
    AvbEntityKit kit{
        config,
        handler,
        talkers,
        listeners,
        48000,
        /*initial_clock_source=*/0,
        /*crf_clock_source_index=*/std::nullopt,
        media_clock,
        last_gptp_ns,
        /*audio_sink=*/nullptr,
        std::pmr::get_default_resource()};
    auto& components = kit.host().components();

    // TX: one ACMP talker stream and one serializer slot per STREAM_OUTPUT.
    for (auto const& spec : talkers) {
        auto const* stream = components.acmp_talker.get_stream(spec.index);
        EXPECT_TRUE(stream != nullptr);
        tsn::StreamId sid{};
        (void)tsn::load_unchecked(stream->stream_id.span(), &sid);
        EXPECT_TRUE(kit.talker().open_stream(spec, sid, stream->stream_dest_mac).has_value());
    }
    EXPECT_EQ(kit.talker().slots_.size(), MAX_ENTITY_STREAMS);
    EXPECT_TRUE(kit.talker().slot_for(0)->aaf.has_value());
    for (uint16_t k = 0; k < crf; ++k) {
        EXPECT_TRUE(kit.talker().slot_for(audio + k)->crf.has_value());
        EXPECT_TRUE(kit.talker().toggle_media_clock_restart(audio + k).has_value());
    }
    EXPECT_TRUE(
        kit.talker().open_stream(talkers.front(), tsn::StreamId{}, ieee::Eui48{}).error() == std::errc::result_out_of_range);

    // RX: one deserializer slot per STREAM_INPUT.
    EXPECT_TRUE(kit.listener() != nullptr);
    for (auto const& spec : listeners) {
        EXPECT_TRUE(kit.listener()->open_stream(spec).has_value());
    }
    EXPECT_EQ(kit.listener()->slots_.size(), MAX_ENTITY_STREAMS);
    EXPECT_EQ(kit.listener()->slot_for(0)->aaf->channel_count, uint16_t{32});
    for (uint16_t k = 0; k < crf; ++k) {
        EXPECT_TRUE(kit.listener()->slot_for(audio + k)->crf.has_value());
    }
    EXPECT_TRUE(kit.listener()->open_stream(listeners.front()).error() == std::errc::result_out_of_range);

    // MSRP: a Talker Advertise per talker stream and a Listener Ready per
    // listener stream all fit the participant's tables (MsrpConfig's default
    // 16 refused the 17th).
    auto const now = sm::Clock::now();
    for (auto const& spec : talkers) {
        nanoavb::TalkerStreamSrpInfo info{};
        info.stream_id = stream_id(0x0200000000000100ULL + spec.index);
        info.vlan_id = 2;
        info.max_frame_size = AvbEntityKit::talker_tspec(spec, true).max_frame_size;
        info.max_interval_frames = 1;
        EXPECT_TRUE(components.msrp_handler.talker_advertise(info, now).has_value());
    }
    for (auto const& spec : listeners) {
        EXPECT_TRUE(components.msrp_handler.listener_ready(stream_id(0x0200000000000200ULL + spec.index), now).has_value());
    }
    for (auto const& spec : talkers) {
        EXPECT_TRUE(components.msrp_handler.get_talker_stream(stream_id(0x0200000000000100ULL + spec.index)) != nullptr);
    }

    // AEM: every STREAM_INPUT and STREAM_OUTPUT latches its own runtime state
    // (40 entries in a table both directions share); the outputs' STOP
    // reaches the talker slots.
    for (auto const type : {DESCRIPTOR_STREAM_OUTPUT, DESCRIPTOR_STREAM_INPUT}) {
        for (uint16_t index = 0; index < MAX_ENTITY_STREAMS; ++index) {
            nanoavb::DescriptorId const id{
                .ref = nanoavb::DescriptorRef{.configuration_index = 0, .descriptor_type = type, .descriptor_index = index}};
            EXPECT_EQ(handler.on_set_descriptor_value(atdecc::AEM_COMMAND_STOP_STREAMING, id, {}), atdecc::AEM_STATUS_SUCCESS);
        }
    }
    for (auto const& spec : talkers) {
        EXPECT_TRUE(kit.talker().stream_stopped(spec.index));
    }
}

//
// The talker streams and the SR class domain: ACMP's picture of each stream and the Talker
// Advertise carry the VLAN and priority the frames are tagged with (Milan v1.3 4.2.7.2.1)
//

namespace kit_domain {

using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;
using statusbar::nanoavb::DomainInfo;
using TimePoint = statusbar::sm::TimePoint;

namespace {

auto load_file(std::filesystem::path const& path) -> std::vector<uint8_t>
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Talker-only descriptor blob (committed fixture; source: examples/tone.json).
auto entity_tone_bin_path() -> std::filesystem::path
{
    return std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_tone.bin";
}

/// The reactor's clock for a kit started in a test: deterministic, 1 ms a read.
struct FakeClock
{
    std::atomic<int64_t>* now_ns;
    auto operator()() noexcept -> int64_t { return now_ns->fetch_add(1'000'000LL); }
};

/// A kit over the tone blob, the way the tone generator builds its own, with
/// the collaborators the kit binds declared before it. start() brings up its
/// control plane on an interface that does not exist: every socket fails to
/// open (the handlers say so and carry on), so no frame leaves the test, and
/// the protocol handlers are fed by hand.
class KitUnderTest
{
  public:
    explicit KitUnderTest(AvbEntityAudioIOConfig config)
        : config_{std::move(config)}
        , storage_{*atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{config_.descriptor_storage_blob})}
        , handler_{storage_, config_.entity_id, config_.entity_model_id, "1.0", "kit test", std::nullopt, true}
        , talker_specs_{*talker_stream_specs(storage_, 0)}
        , listener_specs_{*listener_stream_specs(storage_, 0)}
        , kit_{
              config_,
              handler_,
              talker_specs_,
              listener_specs_,
              48000,
              0,
              std::nullopt,
              media_clock_,
              last_gptp_ns_,
              nullptr,
              std::pmr::get_default_resource()}
    {}

    auto operator->() noexcept -> AvbEntityKit* { return &kit_; }

    /// The ENTITY descriptor's entity_capabilities, as a controller reads them.
    [[nodiscard]] auto entity_capabilities() -> uint32_t
    {
        atdecc::aem::DescriptorEntity entity{};
        nanoavb::DescriptorId const id{
            .ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_ENTITY, .descriptor_index = 0},
            .symbol = 0};
        return handler_.on_get_entity(id, entity) ? entity.entity_capabilities.get() : 0;
    }
    [[nodiscard]] auto talker_specs() const noexcept -> StreamSpecs const& { return talker_specs_; }
    /// The media clock the kit's talker timestamps from, driven by hand as the media thread would.
    [[nodiscard]] auto media_clock() noexcept -> ptpclient::MediaClockGenerator& { return media_clock_; }

    /// Bring the control plane up (or up again), as the tool does at @p now.
    [[nodiscard]] auto start(TimePoint const now) -> bool { return kit_.start(reactor_, now).has_value(); }
    [[nodiscard]] auto stop(TimePoint const now) -> bool { return kit_.stop(now).has_value(); }

  private:
    AvbEntityAudioIOConfig config_;
    itc::StopToken stop_{};
    std::atomic<int64_t> now_ns_{1};
    net::MessageReactor reactor_{stop_, FakeClock{&now_ns_}, 5};
    atdecc::aem::DescriptorStorage storage_;
    EntityIdentityDescriptorHandler handler_;
    StreamSpecs talker_specs_;
    StreamSpecs listener_specs_;
    ptpclient::MediaClockGenerator media_clock_;
    std::atomic<int64_t> last_gptp_ns_{0};
    AvbEntityKit kit_;
};

auto make_config(uint16_t const vlan_id, std::string_view const profile, bool const milan = false) -> AvbEntityAudioIOConfig
{
    AvbEntityAudioIOConfig config{};
    config.entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    config.entity_model_id = Eui64{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    config.descriptor_storage_blob = load_file(entity_tone_bin_path());
    config.vlan_id = vlan_id;
    config.atdecc_profile = std::string{profile};
    config.milan = milan;
    config.interface_name = "no-such-interface-0";
    return config;
}

using Profile = nanoavb::AemCommandHandler::Profile;

auto profile_of(KitUnderTest& kit) -> Profile
{
    return kit->host().components().aem_handler.profile();
}

/// The entity_capabilities @p kit advertises in ADP.
auto advertised_capabilities(KitUnderTest& kit) -> uint32_t
{
    return kit->host().components().adp_advertiser.adpdu().entity_capabilities.get();
}

auto milan_info_of(KitUnderTest& kit) -> std::optional<nanoavb::AemCommandHandler::MilanInfo> const&
{
    return kit->host().components().aem_handler.milan_info();
}

/// What the kit says the Class A VLAN is, from every place it is said:
/// the SRP handler's Domain, the frames, ACMP's picture of each stream.
void expect_kit_on_vlan(KitUnderTest& kit, uint16_t const vlan_id, uint8_t const priority)
{
    EXPECT_EQ(kit->host().components().msrp_handler.domain().sr_class_vid, vlan_id);
    EXPECT_EQ(kit->host().components().msrp_handler.domain().sr_class_priority, priority);
    EXPECT_EQ(kit->talker().vlan_id(), vlan_id);
    for (auto const& spec : kit.talker_specs()) {
        auto const stream = kit->host().components().talker_stream(spec.index);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
            EXPECT_EQ(stream->stream_vlan_id, vlan_id);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, vlan_id);
        EXPECT_EQ(info.priority, priority);
    }
}

/// A bridge on the wire declaring a Class A Domain, its PDUs handed to the
/// kit's MSRP handler.
void bridge_declares_domain(KitUnderTest& kit, DomainInfo const& domain)
{
    std::vector<std::vector<uint8_t>> out;
    nanoavb::MsrpHandler<> bridge{
        statusbar::srp::msrp::MsrpConfig{}, nanoavb::MsrpCallbacks{.send_packet = [&out](std::span<uint8_t const> p) {
            out.emplace_back(p.begin(), p.end());
            return true;
        }}};
    bridge.set_domain(domain);
    TimePoint now{std::chrono::seconds{1}};
    (void)bridge.declare_domain(now);
    for (int round = 0; round < 6; ++round) {
        now += std::chrono::milliseconds{120};
        bridge.tick(now);
        kit->host().components().msrp_handler.tick(now);
        for (auto const& p : out) {
            kit->host().components().msrp_handler.receive_packet(p, now);
        }
        out.clear();
    }
}

}  // namespace

TEST(avb_entity_kit_domain, acmp_and_the_talker_advertise_carry_the_configured_vlan)
{
    // VLAN 7, not the headers' default of 2: what ACMP answers (and
    // GET_STREAM_INFO shows) and what the Talker Advertise declares.
    KitUnderTest kit{make_config(7, "classic")};
    EXPECT_FALSE(kit.talker_specs().empty());
    for (auto const& spec : kit.talker_specs()) {
        auto const stream = kit->host().components().talker_stream(spec.index);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
            EXPECT_EQ(stream->stream_vlan_id, 7U);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 7U);
        EXPECT_EQ(info.priority, 3U);
    }
    EXPECT_EQ(kit->talker().vlan_id(), 7U);
}

TEST(avb_entity_kit_domain, an_adopted_domain_moves_acmp_and_the_talker_advertise_with_the_frames)
{
    // Milan v1.3 4.2.7.2.1: the bridge's Class A Domain (priority 5, VLAN 5)
    // replaces ours. The frames moved already; ACMP's picture and the
    // Talker Advertise must say the same, or a listener reserves on VLAN 7
    // priority 3 for frames that arrive on VLAN 5 priority 5.
    KitUnderTest kit{make_config(7, "milan13")};
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});

    auto const& domain = kit->host().components().msrp_handler.domain();
    EXPECT_EQ(domain.sr_class_vid, 5U);
    EXPECT_EQ(domain.sr_class_priority, 5U);
    EXPECT_EQ(kit->talker().vlan_id(), 5U);
    EXPECT_EQ(kit->talker().stream_pcp(), 5U);
    for (auto const& spec : kit.talker_specs()) {
        auto const stream = kit->host().components().talker_stream(spec.index);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
            EXPECT_EQ(stream->stream_vlan_id, 5U);
        }
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 5U);
        EXPECT_EQ(info.priority, 5U);
    }
}

TEST(avb_entity_kit_domain, a_started_kit_follows_the_domain_it_adopts)
{
    // The adoption hook is installed at construction; bringing the control
    // plane up installs the senders. A Domain adopted after start must still
    // move the frames, ACMP and the Talker Advertise, or the SRP handler
    // declares VLAN 5 for frames that go out on VLAN 7.
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_domain, after_a_restart_mvrp_registers_the_vlan_the_domain_declares)
{
    // stop() withdraws the VLAN and the Domain; start() registers the VLAN
    // again. It must be the one the Domain in force names, whatever that
    // is after an adoption, or MVRP and the declared Domain disagree until
    // the next adoption.
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{3}}));

    auto const& domain = kit->host().components().msrp_handler.domain();
    auto const& mvrp = kit->host().components().mvrp_handler;
    EXPECT_EQ(kit->talker().vlan_id(), domain.sr_class_vid);
    for (uint16_t const vlan_id : {uint16_t{5}, uint16_t{7}}) {
        auto const want = vlan_id == domain.sr_class_vid ? nanoavb::VlanState::Pending : nanoavb::VlanState::Unregistered;
        EXPECT_EQ(mvrp.get_vlan_state(vlan_id), want);
    }
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{4}}));
}

/// MVRP's registrations, from the kit's point of view: @p registered Pending
/// (or Registered), every other VLAN in @p others Unregistered.
void expect_mvrp_on(KitUnderTest& kit, uint16_t const registered, std::initializer_list<uint16_t> const others)
{
    auto const& mvrp = kit->host().components().mvrp_handler;
    EXPECT_NE(mvrp.get_vlan_state(registered), nanoavb::VlanState::Unregistered);
    for (uint16_t const other : others) {
        EXPECT_EQ(mvrp.get_vlan_state(other), nanoavb::VlanState::Unregistered);
    }
}

TEST(avb_entity_kit_domain, start_returns_to_the_configured_domain)
{
    // Milan v1.3 4.2.7.2.1: "After startup ... a PAAD shall use SR Class
    // Priority 3 and Default VLAN ID 2 for Class A" (the configured Domain).
    // Whatever was adopted before, start() brings the frames, ACMP, the
    // Talker Advertise and MVRP back to it.
    KitUnderTest kit{make_config(7, "milan13")};
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);

    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    expect_kit_on_vlan(kit, 7, 3);
    expect_mvrp_on(kit, 7, {5});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_domain, a_link_up_returns_to_the_configured_domain)
{
    // Milan v1.3 4.2.7.2.1: "... or a Link Up event". A link that bounces
    // onto another bridge starts from the configured Domain and adopts what
    // that bridge declares, not what the last one did.
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);
    expect_mvrp_on(kit, 5, {7});

    kit->on_link_up(TimePoint{std::chrono::seconds{3}});
    expect_kit_on_vlan(kit, 7, 3);
    expect_mvrp_on(kit, 7, {5});
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{4}}));
}

TEST(avb_entity_kit_domain, a_classic_kit_keeps_its_configured_domain)
{
    KitUnderTest kit{make_config(7, "classic")};
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    EXPECT_EQ(kit->talker().vlan_id(), 7U);
    for (auto const& spec : kit.talker_specs()) {
        auto const info = kit->make_talker_srp_info(spec);
        EXPECT_EQ(info.vlan_id, 7U);
        EXPECT_EQ(info.priority, 3U);
    }
}

TEST(avb_entity_kit_maap, a_maap_kit_is_stopped_started_and_bounced_without_a_port_left_behind)
{
    // The interface does not exist, so the MAAP socket never opens and the
    // kit keeps its static addresses — the fallback. The stop/start and Link
    // Up bookkeeping around the acquirer must hold all the same: no deadline
    // from a port that is not there, TX never gated, nothing dangling across
    // a restart (Table B.7 Release! on stop, PortOperational! on Link Up).
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    for (int round = 0; round < 2; ++round) {
        auto const base = std::chrono::seconds{1 + (round * 10)};
        EXPECT_TRUE(kit.start(TimePoint{base}));
        kit->on_link_up(TimePoint{base + std::chrono::seconds{1}});
        EXPECT_EQ(kit->poll_timeout_ms(100, 0), 100);                // no MAAP deadline bounds the loop
        kit->on_link_up(TimePoint{base + std::chrono::seconds{2}});  // a bounce
        EXPECT_TRUE(kit.stop(TimePoint{base + std::chrono::seconds{3}}));
    }
}

TEST(avb_entity_kit_maap, an_adopted_vlan_survives_the_acquisition_of_a_block)
{
    // The block is acquired on every Link Up (B.3.5.9); publishing it writes
    // each stream's dest MAC into ACMP's picture of the stream, and that
    // picture also carries the VLAN. The VLAN is the Domain's - here the one
    // adopted from the bridge - and must come through the publication as it
    // was, or every Link Up would move the Talker Advertise, the
    // CONNECT_TX_RESPONSE and GET_STREAM_INFO back to the default VLAN while
    // the frames stayed on the adopted one.
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));  // the talker slots exist from here
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    expect_kit_on_vlan(kit, 5, 5);

    std::vector<uint16_t> indices;
    for (auto const& spec : kit.talker_specs()) {
        indices.push_back(spec.index);
    }
    Eui48 const block{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
    MaapAddressAcquirer::publish_block(kit->host().components(), kit->talker(), indices, block);

    for (size_t pos = 0; pos < indices.size(); ++pos) {
        auto const stream = kit->host().components().talker_stream(indices[pos]);
        EXPECT_TRUE(stream.has_value());
        if (stream.has_value()) {
            EXPECT_EQ(stream->stream_vlan_id, 5U);
            EXPECT_TRUE(stream->stream_dest_mac == avtp::maap_block_address(block, static_cast<uint16_t>(pos)));
        }
        auto const* slot = kit->talker().slot_for(indices[pos]);
        EXPECT_TRUE(slot != nullptr && slot->dest_mac == avtp::maap_block_address(block, static_cast<uint16_t>(pos)));
    }
    expect_kit_on_vlan(kit, 5, 5);
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{2}}));
}

TEST(avb_entity_kit_maap, a_link_up_while_the_link_is_up_is_a_repeat_and_a_restart_starts_afresh)
{
    // The runner raises the start-up Link Up itself and then installs a link
    // monitor that reports the first state it sees, so a kit sees two Link
    // Ups within milliseconds. The second must not reach MAAP, or it restarts
    // the probe sequence the first began (B.3.5.9 PortOperational! from
    // PROBE: five PROBEs for four). What on_link_up() tells the two apart by
    // is the link-up state, which a Link Down ends, and which stop() ends so
    // that the Link Up after a restart begins the acquisition again.
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_FALSE(kit->link_up());

    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    EXPECT_FALSE(kit->link_up());
    auto const down = std::string{kit->host().state_string()};
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(kit->link_up());
    auto const after_first = std::string{kit->host().state_string()};
    EXPECT_NE(after_first, down);                                                        // the supervisor left Down on the first
    kit->on_link_up(TimePoint{std::chrono::seconds{2} + std::chrono::milliseconds{3}});  // the monitor's first report
    EXPECT_TRUE(kit->link_up());
    EXPECT_EQ(std::string{kit->host().state_string()}, after_first);  // and the repeat changed nothing

    kit->on_link_down(TimePoint{std::chrono::seconds{3}});
    EXPECT_FALSE(kit->link_up());
    kit->on_link_up(TimePoint{std::chrono::seconds{4}});  // a genuine bounce
    EXPECT_TRUE(kit->link_up());

    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{5}}));
    EXPECT_FALSE(kit->link_up());  // the next start's Link Up is a first one
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{6}}));
    EXPECT_FALSE(kit->link_up());
    kit->on_link_up(TimePoint{std::chrono::seconds{7}});
    EXPECT_TRUE(kit->link_up());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{8}}));
}

TEST(avb_entity_kit_maap, the_first_entity_available_waits_for_the_talker_addresses)
{
    // A Milan listener that hears ENTITY_AVAILABLE while the block is still
    // being probed probes at once, is told TALKER_DEST_MAC_FAIL and sits out
    // TMR_RETRY. Under the hold the host's Link Up starts everything but ADP
    // and owes it; the acquirer's ready callback releases it. A Link Down
    // cancels the debt and the next Link Up incurs it again. Driven at the
    // host, since the fixture's MAAP socket never opens (see below).
    KitUnderTest kit{make_config(2, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    auto& host = kit->host();
    auto const advertising = [&host] { return adp_advertising(host.components().adp_advertiser); };
    host.hold_adp(true);
    host.on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_FALSE(advertising());  // held
    EXPECT_TRUE(host.adp_owed());
    host.release_adp(TimePoint{std::chrono::seconds{3}});  // the block is defended
    EXPECT_TRUE(advertising());
    EXPECT_FALSE(host.adp_owed());
    host.release_adp(TimePoint{std::chrono::seconds{3}});  // a re-defend owes nothing
    EXPECT_TRUE(advertising());

    host.on_link_down(TimePoint{std::chrono::seconds{4}});
    EXPECT_FALSE(host.adp_owed());
    EXPECT_FALSE(advertising());
    host.on_link_up(TimePoint{std::chrono::seconds{5}});
    EXPECT_TRUE(host.adp_owed());  // the bounce asks again
    host.on_link_down(TimePoint{std::chrono::seconds{6}});
    EXPECT_FALSE(host.adp_owed());  // and a Link Down before the defend cancels it
    host.release_adp(TimePoint{std::chrono::seconds{7}});
    EXPECT_FALSE(advertising());

    host.hold_adp(false);
    host.on_link_up(TimePoint{std::chrono::seconds{8}});
    EXPECT_TRUE(advertising());  // no hold: at once, as a static kit
    EXPECT_FALSE(host.adp_owed());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{9}}));
}

TEST(avb_entity_kit_maap, without_a_maap_socket_adp_starts_at_link_up_as_before)
{
    // The fixture's interface does not exist, so the acquirer falls back to
    // the static addresses and is ready from the start: the kit's Link Up
    // holds nothing and ADP starts at once, as it does in static mode.
    auto config = make_config(2, "milan13");
    config.stream_address_mode = "maap";
    KitUnderTest kit{std::move(config)};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    kit->on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(adp_advertising(kit->host().components().adp_advertiser));
    EXPECT_FALSE(kit->host().adp_owed());
    EXPECT_TRUE(kit.stop(TimePoint{std::chrono::seconds{3}}));
}

TEST(avb_entity_kit_maap, the_range_file_holds_one_line_and_reads_back_what_was_written)
{
    SavedMaapRange const range{.start = Eui48{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34}, .count = 3};
    EXPECT_EQ(encode_maap_range(range), std::string{"91e0f0001234 3\n"});
    EXPECT_TRUE(decode_maap_range("91e0f0001234 3\n") == std::optional{range});
    EXPECT_TRUE(decode_maap_range("91e0f0001234 3") == std::optional{range});  // the newline is a courtesy
    EXPECT_FALSE(decode_maap_range("").has_value());
    EXPECT_FALSE(decode_maap_range("91e0f0001234").has_value());      // no count
    EXPECT_FALSE(decode_maap_range("91e0f0001234 0\n").has_value());  // a count of 0
    EXPECT_FALSE(decode_maap_range("91e0f0001234 x\n").has_value());
    EXPECT_FALSE(decode_maap_range("91e0f000123g 3\n").has_value());  // not hex
    EXPECT_FALSE(decode_maap_range("001122334455 3\n").has_value());  // outside the dynamic pool
    EXPECT_FALSE(decode_maap_range("91e0f000fdff 2\n").has_value());  // runs off the pool's end
    EXPECT_TRUE(decode_maap_range("91e0f000fdff 1\n").has_value());

    auto const dir = std::filesystem::temp_directory_path() / "statusbar-maap-range-test";
    std::filesystem::create_directories(dir);
    auto const path = (dir / "range").string();
    std::filesystem::remove(path);
    EXPECT_FALSE(load_maap_range(path).has_value());  // no file: a fresh draw
    EXPECT_TRUE(save_maap_range(path, range).has_value());
    EXPECT_TRUE(load_maap_range(path) == std::optional{range});
    SavedMaapRange const moved{.start = Eui48{0x91, 0xE0, 0xF0, 0x00, 0xAB, 0xCD}, .count = 2};
    EXPECT_TRUE(save_maap_range(path, moved).has_value());  // replaced, not appended
    EXPECT_TRUE(load_maap_range(path) == std::optional{moved});
    {
        std::ofstream damaged{path, std::ios::trunc};
        damaged << "not a range\n";
    }
    EXPECT_FALSE(load_maap_range(path).has_value());
    std::filesystem::remove_all(dir);
}

// IEEE 1722-2025 Annex B.3: the streams have their addresses while the block is defended; each change of
// that is told on the reactor, and the addresses of a block defended are the streams' before it is told.
TEST(avb_entity_kit_maap, ready_is_told_at_each_change_and_only_then)
{
    constexpr int64_t SEC = 1'000'000'000;
    KitUnderTest kit{make_config(2, "classic")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    itc::StopToken stop{};
    std::atomic<int64_t> now_ns{1};
    net::MessageReactor reactor{stop, FakeClock{&now_ns}, 5};
    Eui48 const mac{0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    std::array<uint16_t, 1> const indices{1};
    MaapAddressAcquirer maap;
    std::vector<bool> told;
    maap.set_on_ready_changed([&told, &kit](bool const ready) {
        told.push_back(ready);
        auto const dest = kit->talker().slot_for(1)->dest_mac.to_uint64();
        bool const in_pool = dest >= avtp::MAAP_DYNAMIC_POOL_START.to_uint64() && dest <= avtp::MAAP_DYNAMIC_POOL_END.to_uint64();
        if (ready) {
            EXPECT_TRUE(in_pool);
        }
    });
    // No MAAP socket here: the static addresses stand and the streams stay ready, which is no change.
    EXPECT_TRUE(maap.setup(
                        reactor,
                        "no-such-interface-0",
                        mac,
                        kit->host().components(),
                        kit->talker(),
                        indices,
                        kit->host().ctl_log(),
                        {},
                        TimePoint{})
                    .has_value());
    EXPECT_TRUE(told.empty());

    avtp::MaapHandler handler{mac, 1};
    handler.set_send([](Eui48 const&, std::span<uint8_t const>) { return true; });
    maap.wire_handler(handler, kit->host().components(), kit->talker());
    handler.prepare(1);
    auto defend = [&handler](int64_t const from_ns) {
        handler.port_operational(from_ns);
        for (int k = 1; k <= 12 && !handler.is_acquired(); ++k) {
            handler.tick(from_ns + (k * SEC));
        }
    };
    defend(SEC);
    EXPECT_TRUE(told.empty());  // defended while ready: no change
    handler.release(20 * SEC);
    EXPECT_TRUE(told == std::vector<bool>{false});
    EXPECT_FALSE(maap.ready());
    defend(30 * SEC);
    EXPECT_TRUE((told == std::vector<bool>{false, true}));

    maap.set_on_ready_changed({});
    handler.release(60 * SEC);
    EXPECT_EQ(told.size(), size_t{2});
}

TEST(avb_entity_kit_maap, the_seed_mixes_the_mac_with_the_real_time_clock)
{
    // IEEE 1722-2025 B.3.6.1: a station draws a different sequence after every
    // boot, and two stations booted at one instant draw different ones.
    Eui48 const mac_a{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    Eui48 const mac_b{0x00, 0x11, 0x22, 0x33, 0x44, 0x56};
    EXPECT_NE(maap_seed(mac_a, 1'700'000'000'000'000'000LL), maap_seed(mac_a, 1'700'000'001'000'000'000LL));
    EXPECT_NE(maap_seed(mac_a, 1'700'000'000'000'000'000LL), maap_seed(mac_b, 1'700'000'000'000'000'000LL));
    EXPECT_EQ(maap_seed(mac_a, 0), mac_a.to_uint64());
    // Never 0, which the handler would read as "the MAC alone".
    EXPECT_EQ(maap_seed(Eui48{}, 0), 1U);
}

TEST(avb_entity_kit_tspec, a_milan_kit_declares_table_4_4_frame_sizes_and_a_classic_kit_its_headroom)
{
    // Milan v1.3 Table 4.4 sizes the Talker Advertise's TSpec exactly; the
    // classic profile keeps one sample of headroom per channel.
    KitUnderTest milan{make_config(2, "milan13")};
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_FALSE(milan.talker_specs().empty());
    for (auto const& spec : milan.talker_specs()) {
        auto const info = milan->make_talker_srp_info(spec);
        bool const am824 = spec.format.kind == StreamKind::am824;  // not a Milan format: its spare sample stays
        EXPECT_EQ(info.max_frame_size, am824 ? srp_max_frame_size(spec.format) : milan_srp_max_frame_size(spec.format));
        EXPECT_EQ(info.max_interval_frames, 1U);
    }
    for (auto const& spec : classic.talker_specs()) {
        auto const info = classic->make_talker_srp_info(spec);
        EXPECT_EQ(info.max_frame_size, srp_max_frame_size(spec.format));
        if (spec.format.kind != StreamKind::crf) {  // CRF is exact under both
            EXPECT_NE(info.max_frame_size, milan_srp_max_frame_size(spec.format));
        }
    }
}

TEST(avb_entity_kit_profile, a_milan_kit_runs_mrp_on_table_4_3_and_a_classic_kit_on_table_10_7)
{
    // Milan v1.3 Table 4.3: joinTime 200 ms, LeaveTime 5000 ms, a leavealltimer
    // of 10 to 15 s, a 1000 ms periodictimer, on MSRP and MVRP alike. Any
    // other profile keeps IEEE 802.1Q Table 10-7, whose LeaveTime is 1000 ms.
    KitUnderTest milan{make_config(2, "milan13")};
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_TRUE(milan->host().components().msrp_handler.mrp_timers() == nanoavb::MILAN_MRP_TIMERS);
    EXPECT_TRUE(milan->host().components().mvrp_handler.mrp_timers() == nanoavb::MILAN_MRP_TIMERS);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.join_time.count(), 200);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.leave_time.count(), 5000);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.leave_all_time_min.count(), 10000);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.leave_all_time_max.count(), 15000);
    EXPECT_EQ(nanoavb::MILAN_MRP_TIMERS.periodic_time.count(), 1000);
    EXPECT_TRUE(classic->host().components().msrp_handler.mrp_timers() == srp::mrp::TimerConfig{});
    EXPECT_TRUE(classic->host().components().mvrp_handler.mrp_timers() == srp::mrp::TimerConfig{});
}

TEST(avb_entity_kit_profile, the_loop_does_not_sleep_past_an_mrp_timer)
{
    // The reactor loop waits at most 100 ms a cycle. An MRP jointimer falls
    // due 200 ms after a declaration: the wait is cut to what is left of it,
    // so its message goes out on time and not up to 100 ms late.
    KitUnderTest kit{make_config(2, "classic")};
    auto& msrp = kit->host().components().msrp_handler;
    EXPECT_FALSE(msrp.next_deadline_ns().has_value());  // not running yet
    statusbar::tsn::StreamId sid{ieee::Eui48{0x00, 0x01, 0x02, 0x03, 0x04, 0x05}, 1};
    int64_t const start_ns = 5'000'000'000;
    EXPECT_TRUE(msrp.listener_ready(sid, TimePoint{std::chrono::nanoseconds{start_ns}}).has_value());
    EXPECT_EQ(msrp.next_deadline_ns().value_or(0), start_ns + 200'000'000);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns), 100);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + 150'000'000), 50);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + 199'500'000), 1);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + 200'000'000), 0);
}

TEST(avb_entity_kit_counters, the_loop_wakes_for_the_end_of_an_observation_interval)
{
    // The counters' interval is at most a second (Milan v1.3 5.3.8.10), and here half of one: the
    // loop's wait is cut to what is left of it, not up to 100 ms past it.
    constexpr int64_t interval = AvbEntityKit::COUNTERS_INTERVAL_NS;
    constexpr int64_t ms = 1'000'000;
    static_assert(interval <= 1'000'000'000);
    KitUnderTest kit{make_config(2, "classic")};
    int64_t const start_ns = 5'000'000'000;
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns), 100);  // no interval begun
    kit->end_cycle(start_ns);
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + interval - (50 * ms)), 50);
    kit->end_cycle(start_ns + interval - ms);  // not yet over
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + interval - ms), 1);
    kit->end_cycle(start_ns + interval);  // over: the next one begins
    EXPECT_EQ(kit->poll_timeout_ms(100, start_ns + (2 * interval) - (50 * ms)), 50);
}

TEST(avb_entity_kit_profile, a_talker_given_no_presentation_offset_takes_its_profiles)
{
    // Milan v1.3 5.3.7.6: "By default, the PAAD-AE shall use a 2ms Presentation
    // time offset for all its Stream Outputs." An entity held to 1722.1 alone
    // keeps 1 ms. A value the configuration gives is used under either profile.
    auto milan = make_config(2, "milan13");
    auto classic = make_config(2, "classic");
    auto milan_model = make_config(2, "", true);
    EXPECT_EQ(milan.presentation_offset_ns, 0U);
    EXPECT_EQ(effective_presentation_offset_ns(milan), 2'000'000U);
    EXPECT_EQ(effective_presentation_offset_ns(classic), 1'000'000U);
    EXPECT_EQ(effective_presentation_offset_ns(milan_model), 2'000'000U);

    milan.presentation_offset_ns = 1'000'000;
    classic.presentation_offset_ns = 3'000'000;
    EXPECT_EQ(effective_presentation_offset_ns(milan), 1'000'000U);
    EXPECT_EQ(effective_presentation_offset_ns(classic), 3'000'000U);
}

TEST(avb_entity_kit_profile, the_wire_version_and_the_address_mode_given_by_nobody_are_the_profiles)
{
    // Milan v1.3 5.3.3 has the descriptors of IEEE 1722.1-2021, and 4.3.3.1
    // has a talker's addresses come from MAAP. An entity held to 1722.1 alone
    // keeps the 2013/2016 lengths and its configured addresses. What the
    // configuration says is used under either profile.
    auto milan = make_config(2, "milan13");
    auto classic = make_config(2, "classic");
    auto milan_model = make_config(2, "", true);
    auto pinned = make_config(2, "classic", true);
    EXPECT_TRUE(milan.atdecc_version.empty());
    EXPECT_TRUE(milan.stream_address_mode.empty());
    EXPECT_EQ(effective_atdecc_version(milan), std::string_view{"2021"});
    EXPECT_EQ(effective_atdecc_version(classic), std::string_view{"2016"});
    EXPECT_EQ(effective_atdecc_version(milan_model), std::string_view{"2021"});
    EXPECT_EQ(effective_atdecc_version(pinned), std::string_view{"2016"});
    EXPECT_EQ(effective_stream_address_mode(milan), std::string_view{"maap"});
    EXPECT_EQ(effective_stream_address_mode(classic), std::string_view{"static"});
    EXPECT_EQ(effective_stream_address_mode(milan_model), std::string_view{"maap"});
    EXPECT_EQ(effective_stream_address_mode(pinned), std::string_view{"static"});

    milan.atdecc_version = "2016";
    milan.stream_address_mode = "static";
    classic.atdecc_version = "2021";
    classic.stream_address_mode = "maap";
    EXPECT_EQ(effective_atdecc_version(milan), std::string_view{"2016"});
    EXPECT_EQ(effective_stream_address_mode(milan), std::string_view{"static"});
    EXPECT_EQ(effective_atdecc_version(classic), std::string_view{"2021"});
    EXPECT_EQ(effective_stream_address_mode(classic), std::string_view{"maap"});
}

TEST(avb_entity_kit_profile, a_kit_sends_its_descriptors_in_its_profiles_wire_version)
{
    KitUnderTest milan{make_config(2, "milan13")};
    KitUnderTest classic{make_config(2, "classic")};
    EXPECT_FALSE(milan->host().components().aem_handler.legacy_2016());
    EXPECT_TRUE(classic->host().components().aem_handler.legacy_2016());

    auto asked = make_config(2, "milan13");
    asked.atdecc_version = "2016";
    KitUnderTest held{asked};
    EXPECT_TRUE(held->host().components().aem_handler.legacy_2016());
}

TEST(avb_entity_kit_profile, a_milan_entity_says_vendor_unique_and_interface_index_valid)
{
    // Milan v1.3 5.3.3.1 (ENTITY descriptor) and 5.6.2 (ADPDU): both bits are
    // 1. The model authors neither: they follow the entity answering MVU,
    // which a Milan model does under either profile and a plain one never.
    using namespace atdecc::entity_capabilities;
    uint32_t const milan_bits = VENDOR_UNIQUE_SUPPORTED | AEM_INTERFACE_INDEX_VALID;
    // What the tone model has: its three flags, and the IDENTIFY control its generator names.
    uint32_t const authored = AEM_SUPPORTED | CLASS_A_SUPPORTED | GPTP_SUPPORTED | AEM_IDENTIFY_CONTROL_INDEX_VALID;

    KitUnderTest plain{make_config(2, "")};
    EXPECT_EQ(plain.entity_capabilities(), authored);
    EXPECT_EQ(advertised_capabilities(plain), authored);

    KitUnderTest milan{make_config(2, "", true)};
    EXPECT_EQ(milan.entity_capabilities(), authored | milan_bits);
    EXPECT_EQ(advertised_capabilities(milan), authored | milan_bits);

    KitUnderTest pinned{make_config(2, "classic", true)};
    EXPECT_EQ(pinned.entity_capabilities(), authored | milan_bits);
    EXPECT_EQ(advertised_capabilities(pinned), authored | milan_bits);

    KitUnderTest opted_in{make_config(2, "milan13")};
    EXPECT_EQ(opted_in.entity_capabilities(), authored);
    EXPECT_EQ(advertised_capabilities(opted_in), authored);
}

TEST(avb_entity_kit_profile, an_empty_profile_is_the_models_and_an_explicit_one_wins)
{
    // Nothing said: a plain 1722.1 model answers to 1722.1 and has no Milan info.
    KitUnderTest plain{make_config(2, "")};
    EXPECT_FALSE(plain->milan_profile());
    EXPECT_EQ(profile_of(plain), Profile::classic);
    EXPECT_FALSE(milan_info_of(plain).has_value());

    // Nothing said of a Milan model: Milan v1.3, and GET_MILAN_INFO to answer.
    KitUnderTest milan{make_config(2, "", true)};
    EXPECT_TRUE(milan->milan_profile());
    EXPECT_EQ(profile_of(milan), Profile::milan13);
    EXPECT_TRUE(milan_info_of(milan).has_value());

    // An explicit profile wins; the model stays a Milan device for GET_MILAN_INFO.
    KitUnderTest pinned{make_config(2, "classic", true)};
    EXPECT_FALSE(pinned->milan_profile());
    EXPECT_EQ(profile_of(pinned), Profile::classic);
    EXPECT_TRUE(milan_info_of(pinned).has_value());

    KitUnderTest opted_in{make_config(2, "milan13")};
    EXPECT_TRUE(opted_in->milan_profile());
    EXPECT_EQ(profile_of(opted_in), Profile::milan13);
    EXPECT_FALSE(milan_info_of(opted_in).has_value());
}

TEST(avb_entity_kit_profile, the_milan_info_comes_from_the_configuration)
{
    auto config = make_config(2, "", true);
    config.milan_features_flags = atdecc::MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING;
    config.milan_certification_version = "1.2";
    config.milan_specification_version = "1.3.0.0";
    KitUnderTest kit{config};
    auto const& info = milan_info_of(kit);
    EXPECT_TRUE(info.has_value());
    if (info) {
        EXPECT_EQ(
            info->features_flags, atdecc::MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING | atdecc::MVU_FEATURE_MVU_BINDING);
        EXPECT_EQ(info->certification_version, atdecc::mvu_version(1, 2));
        EXPECT_EQ(info->specification_version, atdecc::mvu_version(1, 3));
    }

    // Defaults: no certification, Milan 1.3, and the one feature the Milan profile brings.
    KitUnderTest defaults{make_config(2, "", true)};
    auto const& given = milan_info_of(defaults);
    EXPECT_TRUE(given.has_value());
    if (given) {
        EXPECT_EQ(given->features_flags, atdecc::MVU_FEATURE_MVU_BINDING);
        EXPECT_EQ(given->certification_version, 0U);
        EXPECT_EQ(given->specification_version, atdecc::mvu_version(1, 3));
    }

    // A version that is not dotted numbers falls back to the default, and is logged.
    auto bad = make_config(2, "", true);
    bad.milan_certification_version = "one point two";
    bad.milan_specification_version = "1.3.";
    KitUnderTest fallback{bad};
    auto const& fell = milan_info_of(fallback);
    EXPECT_TRUE(fell.has_value());
    if (fell) {
        EXPECT_EQ(fell->certification_version, 0U);
        EXPECT_EQ(fell->specification_version, atdecc::mvu_version(1, 3));
    }
}

// A Milan device (entity.milan) with no atdecc.profile runs milan13 throughout: its entity profile
// (MRP timers, ACMP) and its AEM handler, and it adopts the bridge's Class A Domain (Milan v1.3 4.2.7.2.1).
TEST(avb_entity_kit_domain, a_milan_device_without_a_profile_runs_milan13)
{
    KitUnderTest kit{make_config(7, "", true)};
    EXPECT_TRUE(kit->host().components().acmp_is_milan());
    EXPECT_TRUE(kit->host().components().aem_handler.profile() == nanoavb::AemCommandHandler::Profile::milan13);
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    EXPECT_EQ(kit->talker().vlan_id(), 5U);
    EXPECT_EQ(kit->host().components().msrp_handler.domain().sr_class_vid, 5U);
}

TEST(avb_entity_kit_profile, mvu_binding_is_advertised_when_bind_stream_is_answered_and_only_then)
{
    // Milan v1.3 5.4.4.6: the flag says the commands are implemented. They
    // are under the Milan profile, whatever the configuration leaves out,
    // and are not under IEEE 1722.1 connection management, whatever it claims.
    auto claimed = make_config(2, "classic", true);
    claimed.milan_features_flags = atdecc::MVU_FEATURE_MVU_BINDING | atdecc::MVU_FEATURE_REDUNDANCY;
    KitUnderTest classic{claimed};
    EXPECT_FALSE(classic->host().components().acmp_is_milan());
    EXPECT_EQ(milan_info_of(classic)->features_flags, atdecc::MVU_FEATURE_REDUNDANCY);

    KitUnderTest milan{make_config(2, "", true)};
    EXPECT_TRUE(milan->host().components().acmp_is_milan());
    EXPECT_EQ(milan_info_of(milan)->features_flags, atdecc::MVU_FEATURE_MVU_BINDING);
}

//
// SET_SAMPLING_RATE - IEEE 1722.1-2021 Clause 7.4.21
//

namespace {

constexpr uint64_t AAF_48K_8CH = 0x0205022002006000ULL;  // AAF 48k INT_32 8ch 6 samples/frame
constexpr uint64_t AAF_96K_8CH = 0x020702200200C000ULL;  // AAF 96k INT_32 8ch 12 samples/frame
constexpr uint64_t CRF_48K = 0x041060010000BB80ULL;
Eui64 const RATE_ENTITY{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};  // make_config's
Eui64 const RATE_CONTROLLER_A{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0xA1};
Eui64 const RATE_CONTROLLER_B{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0xB2};
Eui48 const RATE_MAC_A{0x02, 0x00, 0x00, 0x00, 0x00, 0xA1};
Eui48 const RATE_MAC_B{0x02, 0x00, 0x00, 0x00, 0x00, 0xB2};

/// A model whose AUDIO_UNIT 0 lists 48, 96 and 192 kHz, with an AAF stream input and output listing
/// their format at 48 and 96 kHz, a CRF output, and dynamic stream ports (source: entity_rates.json).
auto make_rates_config(std::string_view const profile) -> AvbEntityAudioIOConfig
{
    auto config = make_config(2, profile);
    config.descriptor_storage_blob = load_file(std::filesystem::path{__FILE__}.parent_path() / "testdata" / "entity_rates.bin");
    return config;
}

/// One frame the kit's AEM handler sent: where to, and the AECPDU.
struct SentFrame
{
    Eui48 dest{};
    std::vector<uint8_t> frame;

    [[nodiscard]] auto header() const -> atdecc::AemDu
    {
        atdecc::AemDu du{};
        span_load(du, std::span<uint8_t const>{frame});
        return du;
    }
    [[nodiscard]] auto body() const -> std::span<uint8_t const>
    {
        return std::span<uint8_t const>{frame}.subspan(atdecc::AemDu::LENGTH);
    }
};

/// The kit's AEM handler as two controllers see it: commands go in through process_packet(),
/// every frame it sends is kept.
struct RateControllers
{
    KitUnderTest& kit;
    std::vector<SentFrame> sent{};
    uint16_t sequence_id{0};

    explicit RateControllers(KitUnderTest& under_test)
        : kit{under_test}
    {
        kit->host().components().aem_handler.set_send_response([this](Eui48 const& dest, std::span<uint8_t const> const frame) {
            sent.push_back({.dest = dest, .frame = {frame.begin(), frame.end()}});
            return true;
        });
    }

    /// @p controller at @p mac sends AEM @p command with @p body; its response's status.
    auto command(Eui64 const& controller, Eui48 const& mac, uint16_t const command, std::span<uint8_t const> const body) -> uint8_t
    {
        atdecc::AemDu header{};
        header.init_command(command, static_cast<uint16_t>(atdecc::AemDu::AEM_DATA_LENGTH + body.size()));
        header.target_entity_id = RATE_ENTITY;
        header.controller_entity_id = controller;
        header.sequence_id = ++sequence_id;
        std::vector<uint8_t> frame(atdecc::AemDu::LENGTH + body.size());
        span_store(std::span{frame}, header);
        std::ranges::copy(body, frame.begin() + atdecc::AemDu::LENGTH);
        size_t const before = sent.size();
        (void)kit->host().components().aem_handler.process_packet(mac, frame, header.target_entity_id);
        auto const response = std::find_if(sent.begin() + static_cast<std::ptrdiff_t>(before), sent.end(), [](SentFrame const& f) {
            return !f.header().is_unsolicited();
        });
        return response != sent.end() ? response->header().status() : uint8_t{0xFF};
    }

    auto set_rate(uint32_t const rate) -> uint8_t
    {
        std::array<uint8_t, atdecc::aem::AemSamplingRatePayload::LENGTH> body{};
        span_store(
            std::span{body},
            atdecc::aem::AemSamplingRatePayload{
                .descriptor_type = atdecc::aem::DESCRIPTOR_AUDIO_UNIT, .descriptor_index = 0, .sampling_rate = rate});
        return command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_SET_SAMPLING_RATE, body);
    }

    /// The rate GET_SAMPLING_RATE answers for AUDIO_UNIT 0.
    auto rate() -> uint32_t
    {
        std::array<uint8_t, 4> const body{0x00, 0x02, 0x00, 0x00};
        (void)command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_GET_SAMPLING_RATE, body);
        atdecc::aem::AemSamplingRatePayload answer{};
        span_load(answer, sent.back().body());
        return answer.sampling_rate.get();
    }

    /// The format GET_STREAM_FORMAT answers for stream (@p type, @p index).
    auto format(uint16_t const type, uint16_t const index) -> uint64_t
    {
        std::array<uint8_t, 4> const body{0x00, static_cast<uint8_t>(type), 0x00, static_cast<uint8_t>(index)};
        (void)command(RATE_CONTROLLER_A, RATE_MAC_A, atdecc::AEM_COMMAND_GET_STREAM_FORMAT, body);
        ieee::octlet_t word{};
        span_load(word, sent.back().body().subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format)));
        return word.get();
    }

    void register_b()
    {
        (void)command(RATE_CONTROLLER_B, RATE_MAC_B, atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, {});
        sent.clear();
    }

    /// The unsolicited SET_STREAM_FORMATs in sent: (type, index, format).
    [[nodiscard]] auto format_notifications() const -> std::vector<std::tuple<uint16_t, uint16_t, uint64_t>>
    {
        std::vector<std::tuple<uint16_t, uint16_t, uint64_t>> found;
        for (auto const& f : sent) {
            if (f.header().is_unsolicited() && f.header().command_code() == atdecc::AEM_COMMAND_SET_STREAM_FORMAT) {
                atdecc::aem::AemStreamFormatPayload payload{};
                span_load(payload, f.body());
                ieee::octlet_t word{};
                span_load(word, f.body().subspan(offsetof(atdecc::aem::AemStreamFormatPayload, stream_format)));
                found.emplace_back(payload.descriptor_type.get(), payload.descriptor_index.get(), word.get());
            }
        }
        return found;
    }
};

/// What the entity's data plane was asked: each rate with its streams.
struct UnitRateRecorder
{
    std::vector<std::pair<uint32_t, std::vector<AvbEntityKit::RetimedFormat>>> asked{};
    uint8_t answer{atdecc::AEM_STATUS_SUCCESS};

    void install(KitUnderTest& kit)
    {
        kit->set_on_unit_rate_changed([this](uint32_t const rate, std::span<AvbEntityKit::RetimedFormat const> const streams) {
            asked.emplace_back(rate, std::vector<AvbEntityKit::RetimedFormat>{streams.begin(), streams.end()});
            return answer;
        });
    }
};

}  // namespace

TEST(avb_entity_kit_rate, without_a_unit_rate_hook_only_the_rate_in_force_is_accepted)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    EXPECT_EQ(controllers.set_rate(48'000), atdecc::AEM_STATUS_SUCCESS);
    // The data plane keeps its rate, so the unit does not report another (IEEE 1722.1-2021 Clause 7.4.21).
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(controllers.rate(), 48'000U);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0), AAF_48K_8CH);
    EXPECT_EQ(kit->sample_rate(), 48'000U);
}

TEST(avb_entity_kit_rate, a_unit_rate_hook_retimes_the_units_streams_notified_after_the_response)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    controllers.register_b();

    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_SUCCESS);
    // The AAF output and input, their layout kept; the CRF output stays.
    EXPECT_EQ(data_plane.asked.size(), size_t{1});
    if (data_plane.asked.size() == 1) {
        auto const& [rate, streams] = data_plane.asked.front();
        EXPECT_EQ(rate, 96'000U);
        EXPECT_EQ(streams.size(), size_t{2});
        EXPECT_TRUE(std::ranges::all_of(streams, [](auto const& s) { return s.from == AAF_48K_8CH && s.to == AAF_96K_8CH; }));
    }
    // The response first, to A; then, to B alone, the rate's notification and each stream's.
    EXPECT_FALSE(controllers.sent.empty());
    EXPECT_FALSE(controllers.sent.front().header().is_unsolicited());
    EXPECT_TRUE(std::ranges::all_of(
        controllers.sent.begin() + 1, controllers.sent.end(), [](SentFrame const& f) { return f.dest == RATE_MAC_B; }));
    EXPECT_EQ(controllers.sent.size(), size_t{4});
    using Notice = std::tuple<uint16_t, uint16_t, uint64_t>;
    EXPECT_TRUE(
        controllers.format_notifications() ==
        (std::vector<Notice>{
            {atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0, AAF_96K_8CH}, {atdecc::aem::DESCRIPTOR_STREAM_INPUT, 0, AAF_96K_8CH}}));

    // The descriptors report what the data plane runs.
    EXPECT_EQ(controllers.rate(), 96'000U);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 0), AAF_96K_8CH);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_INPUT, 0), AAF_96K_8CH);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_OUTPUT, 1), CRF_48K);
    // The kit's specs and engines keep the rate they were built with: the media thread reads them.
    EXPECT_EQ(kit->sample_rate(), 48'000U);
    EXPECT_EQ(kit->talker_specs().front().format.sample_rate_hz, 48'000U);
    EXPECT_EQ(kit->talker_specs().front().format_word, AAF_48K_8CH);
}

TEST(avb_entity_kit_rate, a_refused_or_unlisted_rate_moves_nothing)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    controllers.register_b();

    // The unit lists 192 kHz, its AAF streams do not: the data plane is not asked.
    EXPECT_EQ(controllers.set_rate(192'000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(data_plane.asked.empty());
    // The data plane refuses: its status, and the rate and formats in force.
    data_plane.answer = atdecc::AEM_STATUS_STREAM_IS_RUNNING;
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(data_plane.asked.size(), size_t{1});
    EXPECT_TRUE(controllers.format_notifications().empty());
    EXPECT_EQ(controllers.rate(), 48'000U);
    EXPECT_EQ(controllers.format(atdecc::aem::DESCRIPTOR_STREAM_INPUT, 0), AAF_48K_8CH);
    EXPECT_EQ(kit->sample_rate(), 48'000U);
}

TEST(avb_entity_kit_rate, a_bound_milan_input_keeps_the_rate)
{
    KitUnderTest kit{make_rates_config("milan13")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    auto& sinks = *kit->host().components().milan_acmp_listener;
    EXPECT_TRUE(sinks.restore_binding(
        atdecc::MilanBinding{.listener_unique_id = 0, .talker_entity_id = RATE_CONTROLLER_B}, TimePoint{std::chrono::seconds{1}}));

    // Milan v1.3 Section 5.4.2.7: as the input's own SET_STREAM_FORMAT would be.
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_TRUE(data_plane.asked.empty());
    EXPECT_EQ(controllers.rate(), 48'000U);
}

// Milan v1.3 Section 5.4.2.13: a mapping held on a stream at the rate in force refuses a re-time,
// the data plane not asked; once it is removed the unit re-times.
TEST(avb_entity_kit_rate, a_held_mapping_refuses_a_retime)
{
    KitUnderTest kit{make_rates_config("classic")};
    RateControllers controllers{kit};
    UnitRateRecorder data_plane;
    data_plane.install(kit);
    auto& aem = kit->host().components().aem_handler;
    auto const maps = std::make_unique<nanoavb::DynamicAudioMaps>();
    aem.set_dynamic_audio_maps(maps.get());
    aem.set_on_audio_mappings_changed(
        [](uint16_t, uint16_t, std::span<nanoavb::DynamicAudioMapping const>) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    auto const mapping = [&controllers](uint16_t const command) {
        std::array<uint8_t, atdecc::aem::AemAudioMappingsCommandHeader::LENGTH + atdecc::aem::AemAudioMapping::LENGTH> body{};
        span_store(
            std::span{body},
            atdecc::aem::AemAudioMappingsCommandHeader{
                .descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, .descriptor_index = 0, .number_of_mappings = 1});
        span_store(
            std::span{body}.subspan(atdecc::aem::AemAudioMappingsCommandHeader::LENGTH),
            atdecc::aem::AemAudioMapping{.stream_index = 0, .stream_channel = 0, .cluster_offset = 0, .cluster_channel = 0});
        return controllers.command(RATE_CONTROLLER_A, RATE_MAC_A, command, body);
    };
    EXPECT_EQ(mapping(atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(data_plane.asked.empty());
    EXPECT_EQ(controllers.rate(), 48'000U);

    EXPECT_EQ(mapping(atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(controllers.set_rate(96'000), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(data_plane.asked.size(), size_t{1});
    EXPECT_TRUE(aem.mappings_hold());
    aem.set_dynamic_audio_maps(nullptr);
}

}  // namespace kit_domain

// Talker admission through the kit, driven by MSRP listener events and grace
// expiry once each stream is connected; the callback fires on every transition.
TEST(avb_entity_kit_admission, follows_msrp_listener_ready_and_grace_expiry)
{
    // A classic kit at Ready declares both streams (a stream is admitted only
    // while declaring its Talker Advertise).
    MilanKit m{false, 0x02};
    EXPECT_TRUE(m.config.gate_talker_on_listener);
    EXPECT_TRUE(m.start_ready());
    auto& kit = m.kit;
    EXPECT_TRUE(m.advertising(0) && m.advertising(1));
    auto& components = kit.host().components();
    auto const aaf = components.talker_stream(0);
    auto const crf = components.talker_stream(1);
    EXPECT_TRUE(aaf.has_value() && crf.has_value());
    if (!aaf.has_value() || !crf.has_value()) {
        return;
    }
    nanoavb::StreamId aaf_sid{};
    aaf_sid.from_uint64(aaf->stream_id.to_uint64());
    nanoavb::StreamId crf_sid{};
    crf_sid.from_uint64(crf->stream_id.to_uint64());
    // IEEE 1722.1: each stream connected; the MSRP Listener admits it.
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    EXPECT_EQ(components.acmp_talker.connection_count(0), size_t{1});

    std::vector<std::vector<uint16_t>> reports;
    kit.set_on_admission_changed(
        [r = &reports](std::span<uint16_t const> admitted) { r->emplace_back(admitted.begin(), admitted.end()); });
    using V = std::vector<uint16_t>;
    using std::chrono::nanoseconds;
    // The gate's clock and the reactor's: the gate is told the first, the declarations the second.
    constexpr auto t0 = AvbEntityKit::GateTimePoint{std::chrono::seconds{1}};
    constexpr auto grace = TalkerGate::GRACE;
    int64_t const now_ns = MilanKit::steady_ns();

    EXPECT_TRUE(kit.admitted_talker_streams(t0).empty());
    kit.poll_admission(t0);
    EXPECT_TRUE(reports.empty());  // no change, no report

    kit.notify_listener_ready(aaf_sid, true, now_ns, t0);
    EXPECT_EQ(reports.size(), size_t{1});
    EXPECT_TRUE(reports.back() == V{0});
    kit.notify_listener_ready(crf_sid, true, now_ns, t0);
    EXPECT_TRUE(reports.back() == (V{0, 1}));
    EXPECT_EQ(reports.size(), size_t{2});

    // AAF's Ready withdrawn: still admitted through the grace window (no report), its end the
    // deadline armed; the timer at it reports the drop -- no MSRP event needed.
    kit.notify_listener_ready(aaf_sid, false, now_ns, t0 + nanoseconds{10});
    EXPECT_EQ(reports.size(), size_t{2});
    kit.poll_admission(t0 + nanoseconds{10} + grace - nanoseconds{1});
    EXPECT_EQ(reports.size(), size_t{2});
    kit.poll_admission(t0 + nanoseconds{10} + grace);
    EXPECT_EQ(reports.size(), size_t{3});
    EXPECT_TRUE(reports.back() == V{1});
    kit.poll_admission(t0 + 10 * grace);
    EXPECT_EQ(reports.size(), size_t{3});

    // Re-registered: back in.
    kit.notify_listener_ready(aaf_sid, true, now_ns, t0 + 11 * grace);
    EXPECT_TRUE(reports.back() == (V{0, 1}));
    EXPECT_EQ(reports.size(), size_t{4});
    kit.set_on_admission_changed({});
}
// A restart with the link up declares the Domain again, which resumes MSRP, and the
// bridge's Class A VID 7 is adopted once it re-declares (Milan v1.2 Section 4.2.7.2.1)
TEST(avb_entity_kit_domain, a_restart_with_the_link_up_adopts_a_bridges_vid)
{
    MilanKit m{std::nullopt, 0x31};
    EXPECT_TRUE(m.start_ready());
    auto& components = m.kit.host().components();
    components.msrp_handler.set_adopt_domain(true);  // as atdecc.profile = "milan13"
    std::vector<std::vector<uint8_t>> bridge_out;
    srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9e};
    bridge.set_send_pdu([&bridge_out](std::span<uint8_t const> p) {
        bridge_out.emplace_back(p.begin(), p.end());
        return true;
    });
    auto now = sm::Clock::now();
    auto const exchange = [&] {
        for (int i = 0; i < 4; ++i) {
            now += std::chrono::milliseconds(250);
            bridge.tick(now);
            for (auto const& p : std::exchange(bridge_out, {})) {
                components.msrp_handler.receive_packet(p, now);
            }
        }
    };
    bridge.start(now);
    (void)bridge.declare_domain(srp::msrp::DomainFirstValue{.sr_class_vid = 7}, now);
    exchange();
    EXPECT_EQ(components.msrp_handler.domain().sr_class_vid, uint16_t{7});

    EXPECT_TRUE(m.kit.stop().has_value());
    EXPECT_TRUE(m.start_ready());
    EXPECT_FALSE(components.msrp_handler.withdrawn());
    components.msrp_handler.set_adopt_domain(true);
    bridge.leave_all(now);  // the resumed run's LeaveAll reaching the bridge
    exchange();
    EXPECT_EQ(components.msrp_handler.domain().sr_class_vid, uint16_t{7});
    EXPECT_EQ(components.talker_stream(0)->stream_vlan_id, uint16_t{7});
    EXPECT_TRUE(components.mvrp_handler.get_vlan_state(7) != nanoavb::VlanState::Unregistered);
}

// The auto backend opens xdp, else rawnet, the reason xdp did not open kept: on an interface
// that does not exist both refuse, and both reasons are there.
TEST(avb_entity_kit_stream_port, the_auto_backend_tries_xdp_then_rawnet)
{
    MilanKit m{std::nullopt, 0x0A};
    m.config.net_backend = "auto";
    EXPECT_TRUE(m.start_ready());
    EXPECT_TRUE(static_cast<bool>(m.kit.stream_xdp_error()));
    EXPECT_TRUE(static_cast<bool>(m.kit.stream_tx_error()));

    MilanKit r{std::nullopt, 0x0B};
    EXPECT_TRUE(r.start_ready());  // rawnet alone: no xdp tried
    EXPECT_FALSE(static_cast<bool>(r.kit.stream_xdp_error()));
}

// The Milan talker through the kit on a nonexistent interface (sends fail
// quietly): declarations follow probes (Milan v1.2 Section 4.3.3.1), and the
// presentation offset goes only through the entity's hook, never while streaming.

TEST(avb_entity_kit_milan_talker, presentation_offset_through_the_entity)
{
    MilanKit m{std::nullopt, 0x13};
    EXPECT_TRUE(m.start_ready());
    m.use_milan_v1_2_stream_info();
    atdecc::aem::AemStreamInfoPayload set{};
    set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    set.descriptor_index = uint16_t{1};
    set.flags = atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID;
    set.msrp_accumulated_latency = uint32_t{2'500'000};
    std::array<uint8_t, atdecc::aem::AemStreamInfoPayload::LENGTH> body{};
    span_store(body, set);

    // An entity that cannot apply it: refused, never reported.
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_STREAM_INFO, body).status, atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'000'000U);

    // With the hook: applied, then stored, reported and readable.
    std::vector<std::pair<uint16_t, uint32_t>> applied;
    m.kit.set_on_presentation_offset_changed([&applied](uint16_t const idx, uint32_t const ns) -> uint8_t {
        applied.emplace_back(idx, ns);
        return atdecc::AEM_STATUS_SUCCESS;
    });
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_STREAM_INFO, body).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE((applied.size() == 1 && applied.front() == std::pair<uint16_t, uint32_t>(1, 2'500'000)));
    EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'500'000U);
    EXPECT_EQ(m.kit.presentation_offset_ns(0), 2'000'000U);
    EXPECT_EQ(load_info(m.stream_info(1).body).first.msrp_accumulated_latency.get(), 2'500'000U);
    std::array<uint8_t, 4> get{0x00, 0x06, 0x00, 0x01};
    auto const transit = m.aem_command(atdecc::AEM_COMMAND_GET_MAX_TRANSIT_TIME, get);
    EXPECT_EQ(transit.status, atdecc::AEM_STATUS_SUCCESS);
    atdecc::aem::AemGetMaxTransitTimeResponsePayload resp{};
    span_load(resp, std::span<uint8_t const>{transit.body}.first(atdecc::aem::AemGetMaxTransitTimeResponsePayload::LENGTH));
    EXPECT_EQ(resp.max_transit_time.get(), uint64_t{2'500'000});

    // The entity may veto; the offset stays.
    m.kit.set_on_presentation_offset_changed([](uint16_t, uint32_t) -> uint8_t { return atdecc::AEM_STATUS_BAD_ARGUMENTS; });
    atdecc::aem::AemSetMaxTransitTimePayload transit_set{};
    transit_set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    transit_set.descriptor_index = uint16_t{1};
    transit_set.max_transit_time = uint64_t{1'000'000};
    std::array<uint8_t, atdecc::aem::AemSetMaxTransitTimePayload::LENGTH> transit_body{};
    span_store(transit_body, transit_set);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, transit_body).status, atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'500'000U);

    // Milan stream outputs cannot be stopped.
    std::array<uint8_t, 4> output{0x00, 0x06, 0x00, 0x00};
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_STOP_STREAMING, output).status, atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_FALSE(m.kit.talker().stream_stopped(0));
}

// IEEE 802.1Q-2022 Clause 35.2.2.8.6: the Talker declares its port's latency at the link speed,
// and GET_STREAM_INFO reports the declared value until the next declaration.
TEST(avb_entity_kit_milan_talker, a_talker_advertise_declares_the_port_latency_at_the_link_speed)
{
    MilanKit m{std::nullopt, 0x16};
    EXPECT_TRUE(m.start_ready());
    m.kit.set_link_mbps(100);
    m.kit.host().components().aem_handler.set_stream_info_layout(atdecc::aem::StreamInfoLayout::ieee_2021);
    // Every talker's frames queue in class A behind one 1542-octet frame; 80 ns an octet at 100 Mbit/s.
    uint64_t octets = 1542;
    for (auto const& spec : m.kit.talker_specs()) {
        octets += interval_wire_octets(m.kit.talker_tspec(spec));
    }
    auto const at_100 = static_cast<uint32_t>((octets * 80) + PROPAGATION_DELAY_NS);
    EXPECT_EQ(load_info(m.stream_info(0).body).first.msrp_accumulated_latency.get(), at_100);  // not declared yet
    m.kit.set_mtu(9000);  // the port's MTU frame interferes: 7500 octets more
    EXPECT_EQ(load_info(m.stream_info(0).body).first.msrp_accumulated_latency.get(), at_100 + (7500 * 80));
    m.kit.set_mtu(1500);
    // Under the shaper's cbs the other talkers' frames leave at idleSlope; this one's at the link speed.
    uint64_t const own = interval_wire_octets(m.kit.talker_tspec(m.kit.talker_specs()[0]));
    auto const shaped = (1542 + own) * 80 + cbs_drain_ns(octets - 1542 - own, octets - 1542, CLASS_A_INTERVALS_PER_SEC);
    m.kit.set_class_a_cbs(true);
    EXPECT_EQ(load_info(m.stream_info(0).body).first.msrp_accumulated_latency.get(), shaped + PROPAGATION_DELAY_NS);
    m.kit.set_class_a_cbs(false);

    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    auto const* declared = m.kit.host().components().msrp_handler.get_talker_stream(m.sid(0));
    EXPECT_TRUE(declared != nullptr);
    EXPECT_EQ(declared != nullptr ? declared->accumulated_latency : 0U, at_100);
    m.kit.set_link_mbps(1000);
    EXPECT_EQ(load_info(m.stream_info(0).body).first.msrp_accumulated_latency.get(), at_100);
}

// A link up reads the port again: an interface whose speed is unknown keeps the last, and the declared
// AccumulatedLatency with it (IEEE 802.1Q-2022 Clause 35.2.2.8.6).
TEST(avb_entity_kit_milan_talker, a_link_up_on_an_unknown_port_keeps_its_speed_and_latency)
{
    MilanKit m{std::nullopt, 0x17};
    EXPECT_TRUE(m.start_ready());
    m.kit.set_link_mbps(100);
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    auto const* declared = m.kit.host().components().msrp_handler.get_talker_stream(m.sid(0));
    auto const before = declared != nullptr ? declared->accumulated_latency : 0U;
    auto const now = sm::Clock::now();
    m.kit.on_link_down(now);
    m.kit.on_link_up(now);
    EXPECT_EQ(m.kit.link_mbps(), 100U);
    declared = m.kit.host().components().msrp_handler.get_talker_stream(m.sid(0));
    EXPECT_EQ(declared != nullptr ? declared->accumulated_latency : 1U, before);
}

// Milan v1.3 Section 5.4.2.10.2: in the IEEE 1722.1-2021 layout an output reports the
// AccumulatedLatency its Talker Advertise declares; Milan v1.2's keeps the offset.
TEST(avb_entity_kit_milan_talker, the_2021_layout_reports_the_declared_accumulated_latency)
{
    MilanKit m{std::nullopt, 0x15};
    EXPECT_TRUE(m.start_ready());
    m.kit.set_on_presentation_offset_changed([](uint16_t, uint32_t) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    atdecc::aem::AemSetMaxTransitTimePayload transit{};
    transit.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    transit.max_transit_time = uint64_t{2'500'000};
    std::array<uint8_t, atdecc::aem::AemSetMaxTransitTimePayload::LENGTH> transit_body{};
    span_store(transit_body, transit);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, transit_body).status, atdecc::AEM_STATUS_SUCCESS);
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    auto const* declared = m.kit.host().components().msrp_handler.get_talker_stream(m.sid(0));
    EXPECT_TRUE(declared != nullptr);

    m.kit.host().components().aem_handler.set_stream_info_layout(atdecc::aem::StreamInfoLayout::ieee_2021);
    auto const info = m.stream_info(0);
    EXPECT_EQ(info.status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(info.body.size(), atdecc::aem::AEM_STREAM_INFO_2021_LENGTH);
    auto const payload = load_info(info.body).first;
    EXPECT_TRUE((payload.flags.get() & atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID) != 0);
    EXPECT_EQ(payload.msrp_accumulated_latency.get(), declared != nullptr ? declared->accumulated_latency : 1U);
    uint64_t octets = 1542;  // one interfering frame, then every talker's frames; 8 ns an octet at 1 Gbit/s
    for (auto const& spec : m.kit.talker_specs()) {
        octets += interval_wire_octets(m.kit.talker_tspec(spec));
    }
    EXPECT_EQ(payload.msrp_accumulated_latency.get(), static_cast<uint32_t>((octets * 8) + PROPAGATION_DELAY_NS));

    // Milan v1.2's layout: the presentation time offset, as GET_MAX_TRANSIT_TIME reads it.
    m.kit.host().components().aem_handler.set_stream_info_layout(atdecc::aem::StreamInfoLayout::milan_v1_2);
    auto const v12 = m.stream_info(0);
    EXPECT_EQ(v12.body.size(), atdecc::aem::AEM_STREAM_INFO_MILAN_LENGTH);
    EXPECT_EQ(load_info(v12.body).first.msrp_accumulated_latency.get(), 2'500'000U);
    std::array<uint8_t, 4> get{0x00, 0x06, 0x00, 0x00};
    auto const read = m.aem_command(atdecc::AEM_COMMAND_GET_MAX_TRANSIT_TIME, get);
    atdecc::aem::AemGetMaxTransitTimeResponsePayload resp{};
    span_load(resp, std::span<uint8_t const>{read.body}.first(atdecc::aem::AemGetMaxTransitTimeResponsePayload::LENGTH));
    EXPECT_EQ(resp.max_transit_time.get(), uint64_t{2'500'000});
}

// Milan v1.3 Table 5.9: the SRP flags of an output in the IEEE 1722.1-2021 layout.
TEST(avb_entity_kit_milan_talker, the_2021_layout_reports_v13_output_flags)
{
    MilanKit m{std::nullopt, 0x16};
    EXPECT_TRUE(m.start_ready());
    m.kit.host().components().aem_handler.set_stream_info_layout(atdecc::aem::StreamInfoLayout::ieee_2021);
    namespace f = atdecc::aem::stream_info_flags;
    uint32_t const srp = f::NOT_REGISTERING_SRP | f::REGISTERING_FAILED | f::CONNECTED | f::MSRP_FAILURE_VALID;
    auto const srp_flags = [&m, srp] { return load_info(m.stream_info(0).body).first.flags.get() & srp; };

    // Not declaring: none.
    EXPECT_EQ(srp_flags(), 0U);
    // Declaring, no Listener registered.
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    EXPECT_EQ(srp_flags(), f::NOT_REGISTERING_SRP);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::AskingFailed);
    EXPECT_EQ(srp_flags(), f::REGISTERING_FAILED);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_EQ(srp_flags(), f::CONNECTED);
    uint32_t const always = f::STREAM_FORMAT_VALID | f::MSRP_ACC_LAT_VALID | f::STREAM_ID_VALID | f::STREAM_VLAN_ID_VALID;
    EXPECT_EQ(load_info(m.stream_info(0).body).first.flags.get() & always, always);

    // SET_STREAM_INFO's offset is BAD_ARGUMENTS here (Milan v1.3 Section 5.4.2.9).
    m.kit.set_on_presentation_offset_changed([](uint16_t, uint32_t) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    atdecc::aem::AemStreamInfoPayload set{};
    set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    set.descriptor_index = uint16_t{1};
    set.flags = f::MSRP_ACC_LAT_VALID;
    set.msrp_accumulated_latency = uint32_t{3'000'000};
    std::array<uint8_t, atdecc::aem::AemStreamInfoPayload::LENGTH> body{};
    span_store(body, set);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_STREAM_INFO, body).status, atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'000'000U);
}

// IEEE 1722.1-2021 Clause 7.4.16.2: a classic output's latency is valid while it declares;
// Milan's always (v1.3 Table 5.9), and the older layouts carry the offset, always valid.
TEST(avb_entity_kit_classic_talker, the_2021_layout_marks_the_latency_valid_while_declaring)
{
    using atdecc::aem::StreamInfoLayout;
    auto const valid = [](bool const milan, StreamInfoLayout const layout, bool const declaring) {
        TalkerStreamStatus const status{.milan = milan, .layout = layout, .declaring = declaring};
        return talker_accumulated_latency_flag(status) == atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID;
    };
    EXPECT_FALSE(valid(false, StreamInfoLayout::ieee_2021, false));
    EXPECT_TRUE(valid(false, StreamInfoLayout::ieee_2021, true));
    EXPECT_TRUE(valid(true, StreamInfoLayout::ieee_2021, false));
    EXPECT_TRUE(valid(false, StreamInfoLayout::ieee_2013, false));
}

// With the transmit gate disabled (bench debugging: every stream transmits)
// the presentation offset is still settable while no Listener is registered:
// "streaming" is Milan's (declaring and a Listener Ready registered).
TEST(avb_entity_kit_milan_talker, offset_settable_with_the_gate_disabled)
{
    MilanKit m{std::nullopt, 0x18, /*gate=*/false};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    EXPECT_TRUE(m.advertising(0));
    m.kit.set_on_presentation_offset_changed([](uint16_t, uint32_t) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    atdecc::aem::AemSetMaxTransitTimePayload set{};
    set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    set.descriptor_index = uint16_t{0};
    set.max_transit_time = uint64_t{3'000'000};
    std::array<uint8_t, atdecc::aem::AemSetMaxTransitTimePayload::LENGTH> body{};
    span_store(body, set);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, body).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.presentation_offset_ns(0), 3'000'000U);
}

namespace {

/// A Milan kit's talker stream 0, probed and registered by a bridge's Listener Ready, run past
/// the 15 s probe window on the MRP clock so only the Listener keeps it declared.
struct ListenerDepartureRig
{
    static constexpr auto STEP = std::chrono::milliseconds{100};

    explicit ListenerDepartureRig(uint64_t const entity_low)
        : m{std::nullopt, entity_low}
    {
        ok = m.start_ready();
        (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
        m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
        run(std::chrono::seconds{16}, /*bridge=*/true);
    }

    /// One STEP of the MRP clock: the bridge's MSRPDUs delivered (when @p bridge), the entity ticked.
    void step(bool const bridge)
    {
        m.bridge_now += STEP;
        if (bridge) {
            m.bridge.tick(m.bridge_now);
            deliver();
        }
        m.kit.host().components().msrp_handler.tick(m.bridge_now);
    }
    void run(std::chrono::milliseconds const span, bool const bridge)
    {
        for (auto left = span; left > std::chrono::milliseconds{0}; left -= STEP) {
            step(bridge);
        }
    }
    void deliver()
    {
        for (auto const& p : std::exchange(m.bridge_out, {})) {
            m.kit.host().components().msrp_handler.receive_packet(p, m.bridge_now);
        }
    }
    void leave_all() { m.kit.host().components().msrp_handler.receive_packet(srp::msrp::listener_leave_all_pdu(), m.bridge_now); }

    /// Stream 0 is declared and its gate open, on the gate's clock now.
    [[nodiscard]] auto streaming() -> bool
    {
        return m.advertising(0) && !m.kit.admitted_talker_streams(MilanKit::raw_now()).empty();
    }
    /// Steps (the bridge silent) until stream 0 is neither declared nor admitted; returns the time elapsed.
    auto time_to_withdrawal(std::chrono::milliseconds const limit) -> std::chrono::milliseconds
    {
        auto elapsed = std::chrono::milliseconds{0};
        for (; elapsed < limit && (m.advertising(0) || !m.kit.admitted_talker_streams(MilanKit::raw_now()).empty());
             elapsed += STEP) {
            step(false);
        }
        return elapsed;
    }
    /// SET_MAX_TRANSIT_TIME on stream 0: STREAM_IS_RUNNING while it streams (Milan v1.3 Section 5.4.2.30).
    auto set_offset(uint64_t const offset_ns) -> uint8_t
    {
        atdecc::aem::AemSetMaxTransitTimePayload set{};
        set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        set.descriptor_index = uint16_t{0};
        set.max_transit_time = offset_ns;
        std::array<uint8_t, atdecc::aem::AemSetMaxTransitTimePayload::LENGTH> body{};
        span_store(body, set);
        return m.aem_command(atdecc::AEM_COMMAND_SET_MAX_TRANSIT_TIME, body).status;
    }

    MilanKit m;
    bool ok{false};
};

}  // namespace

// Milan v1.3 Sections 4.2.7.2.2 and 5.5.2.5: the last Listener's Lv withdraws the Talker
// Advertise and stops the stream at once, and its settings are no longer STREAM_IS_RUNNING.
TEST(avb_entity_kit_milan_talker, the_last_listeners_leave_withdraws_at_once)
{
    ListenerDepartureRig rig{0x19};
    EXPECT_TRUE(rig.ok);
    rig.m.kit.set_on_presentation_offset_changed([](uint16_t, uint32_t) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    EXPECT_TRUE(rig.streaming());
    EXPECT_EQ(rig.set_offset(3'000'000), atdecc::AEM_STATUS_STREAM_IS_RUNNING);

    (void)rig.m.bridge.withdraw_listener(rig.m.sid(0), rig.m.bridge_now);
    for (int i = 0; i < 5; ++i) {
        rig.step(true);  // the bridge's Lv goes on its JoinTime
    }
    EXPECT_FALSE(rig.m.advertising(0));
    EXPECT_TRUE(rig.m.kit.admitted_talker_streams(MilanKit::raw_now()).empty());
    EXPECT_FALSE(rig.m.kit.admission_deadline().has_value());
    EXPECT_EQ(rig.set_offset(3'000'000), atdecc::AEM_STATUS_SUCCESS);
}

// A Listener gone without an Lv is retired by the next LeaveAll after LeaveTime (5 s, Milan v1.3
// Table 4.3): the stream is withdrawn then, within 6 s of the LeaveAll, and not before.
TEST(avb_entity_kit_milan_talker, a_listener_lost_through_a_leave_all_withdraws_after_leave_time)
{
    ListenerDepartureRig rig{0x1C};
    EXPECT_TRUE(rig.ok);
    EXPECT_TRUE(rig.streaming());
    rig.leave_all();
    auto const elapsed = rig.time_to_withdrawal(std::chrono::seconds{10});
    EXPECT_TRUE(elapsed >= nanoavb::MILAN_MRP_TIMERS.leave_time);
    EXPECT_TRUE(elapsed <= nanoavb::MILAN_MRP_TIMERS.leave_time + ListenerDepartureRig::STEP);
    EXPECT_FALSE(rig.m.advertising(0));
    EXPECT_FALSE(rig.m.kit.admission_deadline().has_value());
}

// A Listener left in LV by a LeaveAll it was not re-declared in, then withdrawn with an Lv, goes
// LV to MT (IEEE 802.1Q-2022 Clause 10.7.5, Milan v1.3 Section 4.2.7.2.2): the stream stops at once.
TEST(avb_entity_kit_milan_talker, a_listeners_leave_after_a_leave_all_withdraws_at_once)
{
    ListenerDepartureRig rig{0x1E};
    EXPECT_TRUE(rig.ok);
    rig.leave_all();
    EXPECT_TRUE(rig.streaming());
    (void)rig.m.bridge.withdraw_listener(rig.m.sid(0), rig.m.bridge_now);
    for (int i = 0; i < 5; ++i) {
        rig.step(true);  // the bridge's Lv goes on its JoinTime, well within LeaveTime
    }
    EXPECT_FALSE(rig.m.advertising(0));
    EXPECT_TRUE(rig.m.kit.admitted_talker_streams(MilanKit::raw_now()).empty());
}

// A Listener re-declaring within LeaveTime of a LeaveAll (3 s late here) keeps the stream
// declared and streaming throughout.
TEST(avb_entity_kit_milan_talker, a_listener_redeclaring_after_a_leave_all_keeps_streaming)
{
    ListenerDepartureRig rig{0x1D};
    EXPECT_TRUE(rig.ok);
    rig.leave_all();
    rig.m.bridge.receive_pdu(srp::msrp::listener_leave_all_pdu(), rig.m.bridge_now);  // its re-declaration held back
    bool held = true;
    for (int i = 0; i < 30; ++i) {
        rig.step(false);
        held = held && rig.streaming();
    }
    rig.deliver();
    for (int i = 0; i < 100; ++i) {
        rig.step(true);
        held = held && rig.streaming();
    }
    EXPECT_TRUE(held);
}

// ---- The persistent state -----------------------------------------------------

namespace {

/// A fresh, empty directory for one test
auto state_dir(std::string const& name) -> std::filesystem::path
{
    auto dir = std::filesystem::temp_directory_path() / ("avb-kit-state-" + std::to_string(::getpid()) + "-" + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

ieee::Eui48 const CONTROLLER_MAC{0x02, 0x00, 0x00, 0xC0, 0x00, 0x01};

/// The persistent state file of the MilanKit with @p entity_low in state directory @p dir, its directory made.
auto kit_state_path(std::filesystem::path const& dir, uint64_t const entity_low = 0x30) -> std::string
{
    auto const entity_dir = entity_state_dir_path(dir.string(), ieee::Eui64{}.from_uint64(0x020000fffe000000ULL | entity_low));
    std::filesystem::create_directories(entity_dir);
    return entity_state_file_path(entity_dir, entity_state_file::ENTITY_STATE);
}

/// A kit keeping its persistent state at @p path, with a capture of what its
/// MVU handler sends
struct PersistentKit : MilanKit
{
    explicit PersistentKit(
        std::string const& path,
        uint64_t const entity_low = 0x30,
        std::optional<bool> const milan = std::nullopt,
        std::vector<uint8_t> model = make_blob(1, 1),
        bool const gate = true)
        : MilanKit{milan, entity_low, gate, std::move(model)}
    {
        if (!path.empty()) {
            // The state directory the file is in: <state_dir>/<entity ID>/entity-state.
            config.state_dir = std::filesystem::path{path}.parent_path().parent_path().string();
        }
    }

    /// An AEM command through process_packet (as from the network: the
    /// settings hook runs after the response)
    auto aem_packet(uint16_t const command, std::span<uint8_t const> const body) -> uint8_t
    {
        nanoavb::AemDu header{};
        header.init_command(command, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + body.size()));
        header.controller_entity_id = TEST_CONTROLLER;  // a real one: zero is nobody's (Milan v1.3 5.4.5.3)
        header.target_entity_id = config.entity_id;
        std::vector<uint8_t> frame(nanoavb::AemDu::LENGTH + body.size());
        span_store(std::span<uint8_t>{frame}.first(nanoavb::AemDu::LENGTH), header);
        std::ranges::copy(body, frame.begin() + nanoavb::AemDu::LENGTH);
        aem.clear();
        (void)kit.host().components().aem_handler.process_packet(CONTROLLER_MAC, frame, config.entity_id);
        nanoavb::AemDu reply{};
        span_load(reply, std::span<uint8_t const>{aem.front()}.first(nanoavb::AemDu::LENGTH));
        return reply.status();
    }

    /// An MVU command through the MVU handler; the response's status
    auto mvu_packet(uint16_t const command, std::span<uint8_t const> const body) -> uint8_t
    {
        atdecc::AecpMvuDu header{};
        header.init_command(command, static_cast<uint16_t>(body.size()));
        header.common.target_entity_id = config.entity_id;
        std::vector<uint8_t> frame(atdecc::AecpMvuDu::LENGTH + body.size());
        span_store(std::span<uint8_t>{frame}.first(atdecc::AecpMvuDu::LENGTH), header);
        std::ranges::copy(body, frame.begin() + atdecc::AecpMvuDu::LENGTH);
        std::vector<uint8_t> response;
        kit.host().components().aem_handler.set_send_response([&response](ieee::Eui48 const&, std::span<uint8_t const> bytes) {
            response.assign(bytes.begin(), bytes.end());
            return true;
        });
        (void)kit.host().components().aem_handler.process_packet(CONTROLLER_MAC, frame, config.entity_id);
        atdecc::AecpMvuDu reply{};
        span_load(reply, std::span<uint8_t const>{response}.first(atdecc::AecpMvuDu::LENGTH));
        return reply.common.status();
    }

    /// READ_DESCRIPTOR of the ENTITY: its entity_name
    auto entity_name() -> std::string
    {
        atdecc::aem::AemReadDescriptorCommandPayload read{};
        read.descriptor_type = DESCRIPTOR_ENTITY;
        std::array<uint8_t, atdecc::aem::AemReadDescriptorCommandPayload::LENGTH> body{};
        span_store(body, read);
        auto const r = aem_command(atdecc::AEM_COMMAND_READ_DESCRIPTOR, body);
        EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
        atdecc::aem::DescriptorEntity entity{};
        span_load_padded(entity, std::span<uint8_t const>{r.body}.subspan(atdecc::aem::AemReadDescriptorResponsePayload::LENGTH));
        return std::string{entity.entity_name.as_string_view()};
    }

    auto set_entity_name(std::string_view const name) -> uint8_t
    {
        std::array<uint8_t, 8 + 64> body{};  // ENTITY 0, name_index 0, configuration 0, the name
        std::ranges::copy(name, body.begin() + 8);
        return aem_packet(atdecc::AEM_COMMAND_SET_NAME, body);
    }

    /// SET_NAME of a descriptor's object_name (name_index 0)
    auto set_object_name(uint16_t const type, uint16_t const index, std::string_view const name) -> uint8_t
    {
        std::array<uint8_t, 8 + 64> body{};  // descriptor type, index, name_index 0, configuration 0, the name
        body[0] = static_cast<uint8_t>(type >> 8U);
        body[1] = static_cast<uint8_t>(type);
        body[2] = static_cast<uint8_t>(index >> 8U);
        body[3] = static_cast<uint8_t>(index);
        std::ranges::copy(name, body.begin() + 8);
        return aem_packet(atdecc::AEM_COMMAND_SET_NAME, body);
    }

    /// READ_DESCRIPTOR of a stream: its object_name
    auto stream_name(uint16_t const type, uint16_t const index) -> std::string
    {
        atdecc::aem::AemReadDescriptorCommandPayload read{};
        read.descriptor_type = type;
        read.descriptor_index = index;
        std::array<uint8_t, atdecc::aem::AemReadDescriptorCommandPayload::LENGTH> body{};
        span_store(body, read);
        auto const r = aem_command(atdecc::AEM_COMMAND_READ_DESCRIPTOR, body);
        EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
        atdecc::aem::DescriptorStream stream{};
        span_load_padded(stream, std::span<uint8_t const>{r.body}.subspan(atdecc::aem::AemReadDescriptorResponsePayload::LENGTH));
        return std::string{stream.object_name.as_string_view()};
    }
};

}  // namespace

// SET_NAME is saved; a kit rebuilt on the same file serves the name in
// READ_DESCRIPTOR
TEST(avb_entity_kit_persistence, set_name_survives_a_rebuild)
{
    auto const dir = state_dir("name");
    auto const path = kit_state_path(dir, 0x30);
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.entity_name() == "kit milan");
        EXPECT_EQ(m.set_entity_name("FOH bridge"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(m.entity_name() == "FOH bridge");
        auto const saved = load_persistent_state(path, 0, {});
        EXPECT_TRUE(saved.outcome == PersistentStateOutcome::loaded);
        EXPECT_EQ(saved.state.names.size(), size_t{1});
        EXPECT_TRUE(saved.state.configured_entity_name == "kit milan");
    }
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.entity_name() == "FOH bridge");
        // Back to the configured name: nothing left to keep
        EXPECT_EQ(m.set_entity_name("kit milan"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(load_persistent_state(path, 0, {}).state.names.empty());
    }
    std::filesystem::remove_all(dir);
}

// With managed object names, a stream's SET_NAME is served, saved and
// restored, and every name change (the entity's too) is told to the hook
TEST(avb_entity_kit_persistence, stream_names_are_renameable_saved_and_told)
{
    auto const dir = state_dir("stream-name");
    auto const path = kit_state_path(dir, 0x30);
    std::vector<std::tuple<uint16_t, uint16_t, std::string>> told;
    auto const hook = [&told](uint16_t const type, uint16_t const index, atdecc::aem::AtdeccString const& name) {
        told.emplace_back(type, index, std::string{name.as_string_view()});
    };
    {
        // without managed object names a stream's SET_NAME is not implemented
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.set_object_name(DESCRIPTOR_STREAM_INPUT, 0, "Desk"), atdecc::AEM_STATUS_NOT_IMPLEMENTED);
    }
    {
        PersistentKit m{path};
        m.handler.manage_object_names();
        m.handler.set_on_name_changed(hook);
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.set_object_name(DESCRIPTOR_STREAM_INPUT, 0, "Desk"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.set_object_name(DESCRIPTOR_STREAM_OUTPUT, 0, "To FOH"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.set_entity_name("FOH bridge"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(m.stream_name(DESCRIPTOR_STREAM_INPUT, 0) == "Desk");
        EXPECT_TRUE(m.stream_name(DESCRIPTOR_STREAM_OUTPUT, 0) == "To FOH");
        std::vector<std::tuple<uint16_t, uint16_t, std::string>> const expected{
            {DESCRIPTOR_STREAM_INPUT, 0, "Desk"}, {DESCRIPTOR_STREAM_OUTPUT, 0, "To FOH"}, {DESCRIPTOR_ENTITY, 0, "FOH bridge"}};
        EXPECT_TRUE(told == expected);
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), size_t{3});
    }
    told.clear();
    {
        // restored at start, through the hook
        PersistentKit m{path};
        m.handler.manage_object_names();
        m.handler.set_on_name_changed(hook);
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.stream_name(DESCRIPTOR_STREAM_INPUT, 0) == "Desk");
        EXPECT_EQ(told.size(), size_t{3});
        // an empty name clears back to the authored one (empty in this model)
        told.clear();
        EXPECT_EQ(m.set_object_name(DESCRIPTOR_STREAM_INPUT, 0, ""), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(m.stream_name(DESCRIPTOR_STREAM_INPUT, 0).empty());
        EXPECT_TRUE(told.size() == 1 && std::get<2>(told[0]).empty());
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), size_t{2});
        // ... and an empty entity name to the configured one, which GET_NAME,
        // READ_DESCRIPTOR and the SET_NAME response report; nothing left to keep
        told.clear();
        EXPECT_EQ(m.set_entity_name(""), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(m.entity_name() == "kit milan");
        EXPECT_TRUE(told.size() == 1 && std::get<2>(told[0]) == "kit milan");
        nanoavb::AemDu reply{};
        auto const& response = m.aem.front();
        span_load(reply, std::span<uint8_t const>{response}.first(nanoavb::AemDu::LENGTH));
        EXPECT_TRUE(
            std::string_view{reinterpret_cast<char const*>(response.data()) + nanoavb::AemDu::LENGTH + 8}.starts_with("kit milan"));
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), size_t{1});
    }
    std::filesystem::remove_all(dir);
}

namespace {

/// LOCK_ENTITY (@p unlock: UNLOCK) of @p m from another controller, through the AEM handler.
auto lock_entity(MilanKit& m, bool const unlock = false) -> uint8_t
{
    std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};
    lock[3] = unlock ? 0x01 : 0x00;
    nanoavb::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_LOCK_ENTITY, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + lock.size()));
    header.controller_entity_id = ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL);
    std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
    return m.kit.host().components().aem_handler.handle_command(header, lock, out).status;
}

}  // namespace

// Identify set by other than ATDECC, through the AEM handler or the host, is refused while a
// controller holds the lock under milan13 (Milan v1.3 Section 5.4.2.17)
TEST(avb_entity_kit_lock, a_local_identify_is_refused_while_locked)
{
    MilanKit m{std::nullopt, 0x3A, true, make_blob(1, 1, 1, /*identify=*/true)};
    EXPECT_TRUE(m.start_ready());
    auto& aem = m.kit.host().components().aem_handler;
    aem.set_profile(nanoavb::AemCommandHandler::Profile::classic);  // as atdecc.profile = "classic"
    std::vector<bool> told;
    aem.set_identify_changed([&told](bool const on) { told.push_back(on); });
    EXPECT_EQ(aem.set_local_identify(true), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(lock_entity(m), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.kit.host().set_local_identify(false));  // classic
    m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::milan13);
    EXPECT_EQ(aem.set_local_identify(true), atdecc::AEM_STATUS_ENTITY_LOCKED);
    EXPECT_FALSE(m.kit.host().set_local_identify(true));
    EXPECT_TRUE(told == std::vector<bool>({true, false}));  // the refused changes applied nothing
    EXPECT_EQ(lock_entity(m, true), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.kit.host().set_local_identify(true));
}

// Every local path of the AEM handler answers the lock refusal under milan13
// (Milan v1.3 Section 5.3.4.1), its change not applied
TEST(avb_entity_kit_lock, every_local_aem_path_is_refused_while_locked)
{
    PersistentKit m{"", 0x3B};
    EXPECT_TRUE(m.start_ready());
    auto& aem = m.kit.host().components().aem_handler;
    EXPECT_EQ(lock_entity(m), atdecc::AEM_STATUS_SUCCESS);
    m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::milan13);
    std::array<uint8_t, 8 + atdecc::aem::AtdeccString::LENGTH> name{};
    std::ranges::copy(std::string_view{"Locked"}, name.begin() + 8);
    EXPECT_EQ(aem.apply_local_name(name), atdecc::AEM_STATUS_ENTITY_LOCKED);
    EXPECT_TRUE(m.entity_name() == "kit milan");
    EXPECT_EQ(
        aem.apply_local_audio_mappings(atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT, 0, {}, {}), atdecc::AEM_STATUS_ENTITY_LOCKED);
    std::array<uint8_t, atdecc::aem::AemStreamFormatPayload::LENGTH> format{};
    EXPECT_EQ(aem.apply_local_descriptor_value(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format), atdecc::AEM_STATUS_ENTITY_LOCKED);
}

// The entity renamed by other than ATDECC (the bridge's control socket): as
// a controller's SET_NAME, saved and told to every controller; refused while
// a controller has the entity locked; an empty name is the configured one
TEST(avb_entity_kit_persistence, request_entity_name_is_a_local_set_name)
{
    auto const dir = state_dir("request-name");
    auto const path = kit_state_path(dir, 0x30);
    PersistentKit m{path};
    std::vector<std::string> told;
    m.handler.set_on_name_changed(
        [&told](uint16_t, uint16_t, atdecc::aem::AtdeccString const& name) { told.emplace_back(name.as_string_view()); });
    EXPECT_TRUE(m.start_ready());
    m.kit.host().components().aem_handler.set_profile(
        nanoavb::AemCommandHandler::Profile::classic);  // as atdecc.profile = "classic"
    m.aem.clear();
    EXPECT_EQ(m.kit.request_entity_name("Stage bridge"), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.entity_name() == "Stage bridge");
    EXPECT_TRUE(told.size() == 1 && told[0] == "Stage bridge");
    EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), size_t{1});
    EXPECT_EQ(m.kit.request_entity_name(std::string(65, 'x')), atdecc::AEM_STATUS_BAD_ARGUMENTS);
    std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};
    auto const lock_entity = [&m, &lock] {
        nanoavb::AemDu header{};
        header.init_command(atdecc::AEM_COMMAND_LOCK_ENTITY, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + lock.size()));
        header.controller_entity_id = ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL);
        std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
        return m.kit.host().components().aem_handler.handle_command(header, lock, out).status;
    };
    EXPECT_EQ(lock_entity(), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(m.kit.local_change_refusal().has_value());  // classic (Milan v1.3 Section 5.3.4.1 is milan13's)
    EXPECT_EQ(m.kit.request_entity_name("Locked"), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.entity_name() == "Locked");
    m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::milan13);
    EXPECT_EQ(m.kit.request_entity_name("Other"), atdecc::AEM_STATUS_ENTITY_LOCKED);
    EXPECT_TRUE(m.entity_name() == "Locked");
    lock[3] = 0x01;  // UNLOCK
    EXPECT_EQ(lock_entity(), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(m.kit.local_change_refusal().has_value());
    EXPECT_EQ(m.kit.request_entity_name(""), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.entity_name() == "kit milan");
    EXPECT_TRUE(load_persistent_state(path, 0, {}).state.names.empty());
    std::filesystem::remove_all(dir);
}

// The talker presentation offset (SET_STREAM_INFO) is saved and
// handed to the entity's data plane again at the next start
TEST(avb_entity_kit_persistence, presentation_offset_survives_a_rebuild)
{
    auto const dir = state_dir("offset");
    auto const path = kit_state_path(dir, 0x30);
    std::vector<std::pair<uint16_t, uint32_t>> applied;
    auto const hook = [&applied](uint16_t const idx, uint32_t const ns) -> uint8_t {
        applied.emplace_back(idx, ns);
        return atdecc::AEM_STATUS_SUCCESS;
    };
    {
        PersistentKit m{path};
        m.kit.set_on_presentation_offset_changed(hook);
        EXPECT_TRUE(m.start_ready());
        m.use_milan_v1_2_stream_info();
        atdecc::aem::AemStreamInfoPayload set{};
        set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        set.descriptor_index = uint16_t{1};
        set.flags = atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID;
        set.msrp_accumulated_latency = uint32_t{2'500'000};
        std::array<uint8_t, atdecc::aem::AemStreamInfoPayload::LENGTH> body{};
        span_store(body, set);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_INFO, body), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'500'000U);
    }
    applied.clear();
    {
        PersistentKit m{path};
        m.kit.set_on_presentation_offset_changed(hook);
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE((applied.size() == 1 && applied.front() == std::pair<uint16_t, uint32_t>(1, 2'500'000)));
        EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'500'000U);
        EXPECT_EQ(m.kit.presentation_offset_ns(0), 2'000'000U);
        m.use_milan_v1_2_stream_info();
        EXPECT_EQ(load_info(m.stream_info(1).body).first.msrp_accumulated_latency.get(), 2'500'000U);
    }
    // An entity that cannot apply it at the next start drops it
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'000'000U);
        EXPECT_TRUE(load_persistent_state(path, 0, {}).state.presentation_offsets.empty());
    }
    // A changed configured offset wins over a saved one
    {
        PersistentKit m{path};
        m.kit.set_on_presentation_offset_changed(hook);
        EXPECT_TRUE(m.start_ready());
        m.use_milan_v1_2_stream_info();
        atdecc::aem::AemStreamInfoPayload set{};
        set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        set.descriptor_index = uint16_t{1};
        set.flags = atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID;
        set.msrp_accumulated_latency = uint32_t{2'500'000};
        std::array<uint8_t, atdecc::aem::AemStreamInfoPayload::LENGTH> body{};
        span_store(body, set);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_INFO, body), atdecc::AEM_STATUS_SUCCESS);
    }
    applied.clear();
    {
        PersistentKit m{path};
        m.config.presentation_offset_ns = 3'000'000;
        m.kit.set_on_presentation_offset_changed(hook);
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(applied.empty());
        EXPECT_EQ(m.kit.presentation_offset_ns(1), 2'000'000U);  // the constructor's start, before the edit
    }
    std::filesystem::remove_all(dir);
}

// The text form of default_mcr_priorities: comma-separated octets, empty for none
TEST(avb_entity_kit, default_mcr_priorities_parse)
{
    EXPECT_TRUE(parse_default_mcr_priorities("224") == std::vector<uint8_t>{224});
    EXPECT_TRUE(parse_default_mcr_priorities("224,0,255") == (std::vector<uint8_t>{224, 0, 255}));
    EXPECT_TRUE(parse_default_mcr_priorities("")->empty());
    EXPECT_FALSE(parse_default_mcr_priorities("256").has_value());
    EXPECT_FALSE(parse_default_mcr_priorities("224,").has_value());
    EXPECT_FALSE(parse_default_mcr_priorities("-1").has_value());
    EXPECT_FALSE(parse_default_mcr_priorities("0x10").has_value());
}

// No state file yet: the listener bindings file's bindings are imported and
// armed as fast-connect goals (the classic listener), and the state file is
// written with them; the old file is no longer written
TEST(avb_entity_kit_persistence, listener_bindings_imported)
{
    auto const dir = state_dir("bindings");
    auto const path = kit_state_path(dir, 0x30);
    auto const legacy = (dir / "avb-bindings").string();
    std::string const legacy_text = std::string{LISTENER_BINDINGS_MAGIC} + "\n0 0200001111110001 2\n";
    {
        std::ofstream out{legacy};
        out << legacy_text;
    }
    PersistentKit m{path, 0x30, false};
    m.config.listener_bindings_path = legacy;
    EXPECT_TRUE(m.start_ready());
    auto const goals = m.kit.host().components().acmp_listener.fast_connect_goals();
    EXPECT_EQ(goals.size(), size_t{1});
    EXPECT_EQ(goals[0].listener_unique_id, uint16_t{0});
    EXPECT_EQ(goals[0].talker_entity_id.to_uint64(), 0x0200001111110001ULL);
    EXPECT_EQ(goals[0].talker_unique_id, uint16_t{2});
    auto const saved = load_persistent_state(path, 0, {});
    EXPECT_TRUE(saved.outcome == PersistentStateOutcome::loaded);
    EXPECT_TRUE(
        (saved.state.listener_bindings ==
         std::vector<PersistedListenerBinding>{
             {.stream_input = 0, .talker_entity_id = 0x0200001111110001ULL, .talker_unique_id = 2, .format = AAF_48K_INT24_32CH}}));

    // A goal change is saved to the state file only, at the end of the
    // reactor pass (after the ACMP response)
    m.kit.host().components().acmp_listener.clear_fast_connect_goal(0);
    EXPECT_EQ(load_persistent_state(path, 0, {}).state.listener_bindings.size(), size_t{1});
    m.kit.on_declaration_timer(MilanKit::steady_ns());
    EXPECT_TRUE(load_persistent_state(path, 0, {}).state.listener_bindings.empty());
    // ... or at stop(), when that comes first
    m.kit.host().components().acmp_listener.set_fast_connect_goal(0, ieee::Eui64{}.from_uint64(0x0200001111110001ULL), 2);
    EXPECT_TRUE(m.kit.stop().has_value());
    EXPECT_EQ(load_persistent_state(path, 0, {}).state.listener_bindings.size(), size_t{1});
    std::ifstream in{legacy};
    std::string const now{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    EXPECT_TRUE(now == legacy_text);
    std::filesystem::remove_all(dir);
}

// A file for another entity model is discarded (and rewritten for this one);
// a corrupt file is kept as .bad and the kit starts fresh
TEST(avb_entity_kit_persistence, other_model_and_corrupt_files)
{
    auto const dir = state_dir("model");
    auto const path = kit_state_path(dir, 0x30);
    PersistentState other{};
    other.entity_model_id = 0x99;
    other.configured_entity_name = "kit milan";
    other.names.push_back({.descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0, .name_index = 0, .value = "Stale"});
    EXPECT_TRUE(save_persistent_state(path, serialize_persistent_state(other)).has_value());
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.entity_name() == "kit milan");
        auto const rewritten = load_persistent_state(path, 0, {});
        EXPECT_TRUE(rewritten.outcome == PersistentStateOutcome::loaded);
        EXPECT_TRUE(rewritten.state.names.empty());
    }
    {
        std::ofstream out{path};
        out << "this is not toml [";
    }
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.entity_name() == "kit milan");
        EXPECT_TRUE(std::filesystem::exists(path + ".bad"));
        EXPECT_TRUE(load_persistent_state(path, 0, {}).outcome == PersistentStateOutcome::loaded);
    }
    // A later version's file is neither applied nor overwritten
    {
        std::ofstream out{path};
        out << "version = 9\nentity_model_id = \"0x0\"\n";
    }
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.set_entity_name("Not saved"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(load_persistent_state(path, 0, {}).outcome == PersistentStateOutcome::newer_version);
    }
    // A saved entity name gives way to a changed configured name
    other.entity_model_id = 0;
    other.configured_entity_name = "an older configured name";
    EXPECT_TRUE(save_persistent_state(path, serialize_persistent_state(other)).has_value());
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.entity_name() == "kit milan");
    }
    std::filesystem::remove_all(dir);
}

// Without the entity's hooks only the stream's own format and the kit's rate
// are accepted, so nothing else is saved
TEST(avb_entity_kit_persistence, formats_and_rates_follow_the_data_plane)
{
    auto const dir = state_dir("formats");
    auto const path = kit_state_path(dir, 0x30);
    PersistentKit m{path};
    EXPECT_TRUE(m.start_ready());
    std::array<uint8_t, 12> body{0x00, 0x05, 0x00, 0x00};  // STREAM_INPUT 0
    ieee::Eui64 other{};
    other.from_uint64(CRF_MILAN_48K);
    span_store(std::span{body}.subspan(4, 8), other);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, body), atdecc::AEM_STATUS_NOT_SUPPORTED);
    ieee::Eui64 same{};
    same.from_uint64(AAF_48K_INT24_32CH);
    span_store(std::span{body}.subspan(4, 8), same);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, body), atdecc::AEM_STATUS_SUCCESS);
    std::filesystem::remove_all(dir);
}

// --- Formats and the sampling rate ---------------------------------------------

namespace {

/// SET_STREAM_FORMAT's body: the stream, the format.
auto format_body(uint16_t const type, uint16_t const index, uint64_t const format) -> std::array<uint8_t, 12>
{
    std::array<uint8_t, 12> body{
        static_cast<uint8_t>(type >> 8U),
        static_cast<uint8_t>(type),
        static_cast<uint8_t>(index >> 8U),
        static_cast<uint8_t>(index)};
    span_store(std::span{body}.subspan(4, 8), ieee::Eui64{}.from_uint64(format));
    return body;
}

/// SET_SAMPLING_RATE's body: AUDIO_UNIT 0, the rate.
auto rate_body(uint32_t const rate) -> std::array<uint8_t, 8>
{
    return {
        0x00,
        0x02,
        0x00,
        0x00,
        static_cast<uint8_t>(rate >> 24U),
        static_cast<uint8_t>(rate >> 16U),
        static_cast<uint8_t>(rate >> 8U),
        static_cast<uint8_t>(rate)};
}

/// The multi-rate model's kit, with a format hook that records what the
/// entity was asked.
struct RateKit : PersistentKit
{
    explicit RateKit(
        std::string const& path = {},
        uint64_t const entity_low = 0x70,
        bool const ports = false,
        bool const one_rate = false,
        bool const am824 = false,
        bool const gate = true,
        uint16_t const port_flags = 0,
        bool const dynamic_output = false)
        : PersistentKit{
              path,
              entity_low,
              std::nullopt,
              make_multirate_blob(ports || one_rate, one_rate, am824, false, port_flags, dynamic_output),
              gate}
    {
        kit.set_on_stream_format_changed([this](uint16_t const type, uint16_t const index, StreamFormatFields const& format) {
            formats.push_back({type, index, format});
            return format_status;
        });
        kit.set_on_unit_rate_changed([this](uint32_t const rate, std::span<AvbEntityKit::RetimedFormat const> const streams) {
            unit_rates.push_back({rate, streams.size()});
            return unit_status;
        });
    }

    /// GET_STREAM_FORMAT of a stream
    auto current_format(uint16_t const type, uint16_t const index) -> uint64_t
    {
        std::array<uint8_t, 4> body{
            static_cast<uint8_t>(type >> 8U),
            static_cast<uint8_t>(type),
            static_cast<uint8_t>(index >> 8U),
            static_cast<uint8_t>(index)};
        auto const r = aem_command(atdecc::AEM_COMMAND_GET_STREAM_FORMAT, body);
        EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
        if (r.body.size() < 12) {
            return 0;
        }
        ieee::Eui64 format{};
        span_load(format, std::span<uint8_t const>{r.body}.subspan(4, 8));
        return format.to_uint64();
    }

    struct Format
    {
        uint16_t type{0};
        uint16_t index{0};
        StreamFormatFields fields{};
    };
    std::vector<Format> formats;
    uint8_t format_status{atdecc::AEM_STATUS_SUCCESS};
    std::vector<std::pair<uint32_t, size_t>> unit_rates;  ///< each unit-rate hook call: the rate, how many streams
    uint8_t unit_status{atdecc::AEM_STATUS_SUCCESS};
};

/// The unsolicited AEM responses in @p frames with command @p command.
ieee::Eui64 const OTHER_CONTROLLER{0x02, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x02};
ieee::Eui48 const OTHER_CONTROLLER_MAC{0x02, 0x00, 0x00, 0xC0, 0x00, 0x02};

/// Register a second controller for unsolicited notifications.
void register_other_controller(MilanKit& m)
{
    nanoavb::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, nanoavb::AemDu::AEM_DATA_LENGTH);
    header.target_entity_id = m.config.entity_id;
    header.controller_entity_id = OTHER_CONTROLLER;
    std::array<uint8_t, nanoavb::AemDu::LENGTH> frame{};
    span_store(frame, header);
    (void)m.kit.host().components().aem_handler.process_packet(OTHER_CONTROLLER_MAC, frame, m.config.entity_id);
}

auto unsolicited(std::vector<std::vector<uint8_t>> const& frames, uint16_t const command) -> std::vector<std::vector<uint8_t>>
{
    std::vector<std::vector<uint8_t>> out;
    for (auto const& frame : frames) {
        nanoavb::AemDu du{};
        span_load(du, std::span<uint8_t const>{frame}.first(nanoavb::AemDu::LENGTH));
        if (du.is_unsolicited() && du.command_code() == command) {
            out.emplace_back(frame.begin() + nanoavb::AemDu::LENGTH, frame.end());
        }
    }
    return out;
}

}  // namespace

// A re-declaration is per stream: SET_STREAM_FORMAT on one of two declared outputs re-advertises
// that one with its new TSpec at the next reactor pass (the hook is told no time), not the table.
TEST(avb_entity_kit_formats, a_format_change_re_advertises_one_output)
{
    size_t sent = 0;  // before the kit, whose stop at destruction still sends
    RateKit m{"", 0x1C};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 1);
    EXPECT_TRUE(m.advertising(0) && m.advertising(1));
    auto& msrp = m.kit.host().components().msrp_handler;
    msrp.set_send_packet([&sent](std::span<uint8_t const> const p) {
        sent += p.size();
        return true;
    });
    auto now = sm::Clock::now();
    auto const drain = [&] {  // a reactor pass, then the join timers of what it declared
        m.kit.on_declaration_timer(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        for (int i = 0; i < 4; ++i) {
            now += std::chrono::milliseconds{250};
            msrp.tick(now);
        }
    };
    drain();
    sent = 0;
    auto const frame_size = [&](uint16_t const idx) { return msrp.get_talker_stream(m.sid(idx))->max_frame_size; };
    uint16_t const before0 = frame_size(0);
    uint16_t const before1 = frame_size(1);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(sent, size_t{0});  // asked for, run at the next pass
    EXPECT_EQ(frame_size(0), before0);
    drain();
    EXPECT_TRUE(sent > 0);
    EXPECT_TRUE(m.advertising(0) && m.advertising(1));
    EXPECT_EQ(m.kit.talker_specs()[0].format_word, base_format(48000, 8));
    EXPECT_TRUE(frame_size(0) != before0);  // output 0's TSpec re-declared (INT24 -> INT32 samples)
    EXPECT_EQ(frame_size(1), before1);      // output 1's as it was

    // A re-declaration asked for just before a stop is not replayed after the next start.
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_48K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.kit.stop().has_value());
}

// SET_STREAM_FORMAT accepts a listed format of the stream's kind at the kit's
// rate, through the entity's hook, and re-shapes the stream (spec, TX or RX
// slot); refused while the input is bound or the output streams
TEST(avb_entity_kit_formats, set_stream_format_reshapes_the_stream)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());

    // An input accepts a 2-channel Base format: the entity is told, the RX slot
    // follows (what it accepts is the new format).
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 2))),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.formats.size(), size_t{1});
    EXPECT_TRUE(m.formats.back().type == DESCRIPTOR_STREAM_INPUT && m.formats.back().fields.channels == 2);
    EXPECT_TRUE(m.formats.back().fields.aaf_format == avtp::AafFormat::int_32bit);
    EXPECT_EQ(m.kit.listener_specs().front().format_word, base_format(48000, 2));
    EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.channels, uint16_t{2});
    EXPECT_EQ(m.kit.listener()->slot_for(0)->aaf->channel_count, uint16_t{2});
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 2));
    {
        // GET_STREAM_INFO reports the current format too.
        std::array<uint8_t, 4> body{0x00, 0x05, 0x00, 0x00};
        auto const info = load_info(m.aem_command(atdecc::AEM_COMMAND_GET_STREAM_INFO, body).body).first;
        ieee::Eui64 format{};
        span_load(format, make_const_span(info.stream_format));
        EXPECT_EQ(format.to_uint64(), base_format(48000, 2));
    }

    // An output accepts the 8-channel Base format: the TX slot serializes it,
    // and the TSpec follows.
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.kit.talker().slot_for(0)->aaf->format == avtp::AafFormat::int_32bit);
    EXPECT_EQ(m.kit.talker_tspec(m.kit.talker_specs().front()).max_frame_size, uint16_t{(6 * 8 * 4) + 24});

    // Refused: an unlisted format (the handler), the CRF's, and the entity's
    // own refusal (nothing changes).
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 2))),
        atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 1, CRF_MILAN_48K ^ 1)),
        atdecc::AEM_STATUS_NOT_SUPPORTED);
    m.format_status = atdecc::AEM_STATUS_BAD_ARGUMENTS;
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, INT24_8CH_48K)),
        atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 2));
    EXPECT_EQ(m.kit.listener()->slot_for(0)->aaf->channel_count, uint16_t{2});  // the RX slot put back
    m.format_status = atdecc::AEM_STATUS_SUCCESS;

    // Milan v1.2 Section 5.4.2.7: a streaming output keeps its format.
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_TRUE(m.kit.output_streaming(0, MilanKit::raw_now()));
    size_t const asked = m.formats.size();
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_48K)),
        atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(m.formats.size(), asked);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), base_format(48000, 8));
}

// A bound input keeps its format; a saved format is
// restored at the next start through the entity's hook
TEST(avb_entity_kit_formats, bound_inputs_and_saved_formats)
{
    auto const dir = state_dir("stage8-formats");
    auto const path = kit_state_path(dir, 0x70);
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 6))),
            atdecc::AEM_STATUS_SUCCESS);
        auto const saved = load_persistent_state(path, 0, {});
        EXPECT_EQ(saved.state.stream_formats.size(), size_t{1});
    }
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.formats.size(), size_t{1});  // the saved format, through the hook
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 6));
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.channels, uint16_t{6});
        EXPECT_TRUE(m.kit.host().components().milan_acmp_listener->restore_binding(
            atdecc::MilanBinding{.listener_unique_id = 0, .talker_entity_id = ieee::Eui64{}.from_uint64(0x0200001111110001ULL)},
            sm::Clock::now()));
        EXPECT_TRUE(m.kit.input_running(0));
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 8))),
            atdecc::AEM_STATUS_STREAM_IS_RUNNING);
        // Milan v1.3 Section 5.4.2.7: the bound input refuses the unit's re-time as its own SET_STREAM_FORMAT would.
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
        EXPECT_EQ(m.kit.sample_rate(), 48000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 6));
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(48000)), atdecc::AEM_STATUS_SUCCESS);
    }
    std::filesystem::remove_all(dir);
}

// IEEE 1722.1-2021 Clause 7.4.21: with no stream running, another listed rate re-times every audio
// stream at the unit's rate through the entity's hook; the CRF stays; an unlisted rate is NOT_SUPPORTED
TEST(avb_entity_kit_formats, an_idle_rate_change_retimes_the_unit_streams)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(192000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.sample_rate(), 96000U);
    EXPECT_EQ(m.formats.size(), size_t{2});
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_96K);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 1), CRF_MILAN_48K);
    EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 96000U);
    std::array<uint8_t, 4> get_rate{0x00, 0x02, 0x00, 0x00};
    auto const r = m.aem_command(atdecc::AEM_COMMAND_GET_SAMPLING_RATE, get_rate);
    EXPECT_TRUE(r.status == atdecc::AEM_STATUS_SUCCESS && r.body.size() >= 8 && r.body[6] == 0x77 && r.body[7] == 0x00);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(48000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
}

// The re-timed streams are notified as SET_STREAM_FORMATs to every other registered controller, after the response
TEST(avb_entity_kit_formats, a_rate_change_notifies_the_retimed_formats)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
    auto const sent = unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT);
    EXPECT_EQ(sent.size(), size_t{2});
    EXPECT_TRUE(sent.size() == 2 && sent[0].size() >= 12 && sent[0][1] == DESCRIPTOR_STREAM_OUTPUT && sent[0][6] == 0x03);
    m.aem.clear();
    m.kit.on_declaration_timer(MilanKit::steady_ns());
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).empty());  // sent once
}

// A stream the entity refuses at the new rate refuses the change: those re-timed go back, nothing is notified
TEST(avb_entity_kit_formats, a_refused_rate_change_is_undone)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    m.kit.set_on_stream_format_changed([&m](uint16_t const type, uint16_t const index, StreamFormatFields const& format) {
        m.formats.push_back({type, index, format});
        bool const refused = type == DESCRIPTOR_STREAM_INPUT && format.sample_rate_hz == 96000;
        return refused ? atdecc::AEM_STATUS_NO_RESOURCES : atdecc::AEM_STATUS_SUCCESS;
    });
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);  // re-timed, then put back
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_48K);
    EXPECT_EQ(m.formats.size(), size_t{3});
    EXPECT_TRUE(m.unit_rates == (std::vector<std::pair<uint32_t, size_t>>{{96000, 2}, {48000, 2}}));  // the data plane back
    m.kit.on_declaration_timer(MilanKit::steady_ns());
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).empty());
}

// Milan v1.2 Section 5.3.4: the unit's rate is saved and restored at the next start, before the formats
TEST(avb_entity_kit_formats, a_saved_sampling_rate_is_restored_at_start)
{
    auto const dir = state_dir("multi-rate");
    auto const path = kit_state_path(dir, 0x70);
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
        auto const saved = load_persistent_state(path, 0, {});
        EXPECT_EQ(saved.state.sampling_rates.size(), size_t{1});
        EXPECT_EQ(saved.state.stream_formats.size(), size_t{2});
    }
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.kit.sample_rate(), 96000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 96000U);
        m.kit.on_declaration_timer(MilanKit::steady_ns());
        EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).empty());  // a restore notifies nobody
    }
    std::filesystem::remove_all(dir);
}

// Milan v1.3 Section 5.4.2.13: a streaming output on a port that converts rates keeps its format while the unit
// changes rate; the idle input follows
TEST(avb_entity_kit_formats, a_running_output_on_a_converting_port_keeps_its_format)
{
    constexpr uint16_t SYNC_SAMPLE_RATE_CONV = 0x0004;
    RateKit m{"", 0x71, true, false, false, true, SYNC_SAMPLE_RATE_CONV};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_TRUE(m.kit.output_streaming(0, MilanKit::raw_now()));
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.sample_rate(), 96000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_96K);
    EXPECT_TRUE(m.unit_rates == (std::vector<std::pair<uint32_t, size_t>>{{96000, 1}}));
}

// The same output on a port that converts no rates cannot follow: NOT_SUPPORTED, the data plane not asked
TEST(avb_entity_kit_formats, a_running_output_on_a_plain_port_refuses_the_rate)
{
    RateKit m{"", 0x72, true};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_48K);
    EXPECT_TRUE(m.unit_rates.empty() && m.formats.empty());
}

// The identity the kit installed on a dynamic output port that converts no rates maps the stream too: a streaming
// output there cannot follow the unit, and the rate is refused rather than the output unmapped under its stream
TEST(avb_entity_kit_formats, a_running_output_under_the_kits_identity_refuses_the_rate)
{
    RateKit m{"", 0x78, true, false, false, true, 0, true};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_TRUE(m.kit.output_streaming(0, MilanKit::raw_now()));
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    EXPECT_TRUE(m.unit_rates.empty() && m.formats.empty());
}

// IEEE 802.1Q-2022 Clause 34.3.1: a re-time past the class A budget is refused before the data plane moves
TEST(avb_entity_kit_formats, a_retime_past_the_class_a_budget_is_refused_first)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    m.kit.set_link_mbps(1);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_TRUE(m.unit_rates.empty() && m.formats.empty());
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
}

namespace {

/// Two AAF outputs (8 ch INT_24, 48 and 96 kHz listed), both carried by STREAM_PORT_OUTPUT 0 of AUDIO_UNIT 0;
/// with @p two_units output 1 and a second unit (port 1) are in clock domain 1, or with @p shared both in 0.
auto make_two_unit_blob(bool const two_units, bool const shared = false) -> std::vector<uint8_t>
{
    nanoavb::AemBlobWriter w;
    atdecc::aem::DescriptorEntity entity{};
    entity.configurations_count = 1;
    entity.talker_stream_sources = 2;
    w.add(0, DESCRIPTOR_ENTITY, 0, make_const_span(entity));
    atdecc::aem::DescriptorConfiguration config{};
    w.add(0, DESCRIPTOR_CONFIGURATION, 0, make_const_span(config));
    for (uint16_t index = 0; index < 2; ++index) {
        atdecc::aem::DescriptorStream stream{};
        stream.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        stream.descriptor_index = index;
        stream.clock_domain_index = static_cast<uint16_t>(two_units && !shared ? index : 0);
        stream.current_format.from_uint64(INT24_8CH_48K);
        (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(INT24_8CH_48K));
        (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(INT24_8CH_96K));
        w.add(0, DESCRIPTOR_STREAM_OUTPUT, index, wire(stream));
    }
    for (uint16_t index = 0; index < (two_units ? 2 : 1); ++index) {
        atdecc::aem::DescriptorStreamPort port{};
        port.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;
        port.descriptor_index = index;
        w.add(0, atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT, index, make_const_span(port));
        atdecc::aem::DescriptorAudioUnit unit{};
        unit.clock_domain_index = static_cast<uint16_t>(shared ? 0 : index);
        unit.current_sampling_rate = uint32_t{48000};
        unit.sampling_rates_count = uint16_t{2};
        unit.sampling_rates[0] = uint32_t{48000};
        unit.sampling_rates[1] = uint32_t{96000};
        unit.number_of_stream_output_ports = uint16_t{1};
        unit.base_stream_output_port = index;
        w.add(0, atdecc::aem::DESCRIPTOR_AUDIO_UNIT, index, wire(unit));
    }
    atdecc::aem::DescriptorAvbInterface iface{};
    w.add(0, DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));
    return w.write();
}

/// The two-unit model's kit, both hooks accepting and counting what they were asked.
struct TwoUnitKit : PersistentKit
{
    explicit TwoUnitKit(bool const two_units, uint64_t const entity_low, bool const shared = false)
        : PersistentKit{{}, entity_low, std::nullopt, make_two_unit_blob(two_units, shared)}
    {
        kit.set_on_stream_format_changed([this](uint16_t, uint16_t, StreamFormatFields const&) {
            ++formats;
            return atdecc::AEM_STATUS_SUCCESS;
        });
        kit.set_on_unit_rate_changed([this](uint32_t, std::span<AvbEntityKit::RetimedFormat const> const streams) {
            asked.push_back(streams.size());
            return atdecc::AEM_STATUS_SUCCESS;
        });
    }

    size_t formats{0};
    std::vector<size_t> asked;
};

}  // namespace

// A unit holds the streams of its clock domain, not another unit's (IEEE 1722.1-2021 Clauses 7.2.3, 7.2.6)
TEST(avb_entity_kit_formats, a_rate_change_retimes_its_own_units_streams_only)
{
    TwoUnitKit m{true, 0x74};
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.asked == std::vector<size_t>{1});
    EXPECT_EQ(m.kit.talker_specs()[0].format_word, INT24_8CH_96K);
    EXPECT_EQ(m.kit.talker_specs()[1].format_word, INT24_8CH_48K);
}

// One port carrying several streams: the unit re-times every one of them, not only the port's own index
TEST(avb_entity_kit_formats, every_stream_one_port_carries_follows_the_unit)
{
    TwoUnitKit m{false, 0x75};
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.asked == std::vector<size_t>{2});
    EXPECT_EQ(m.formats, size_t{2});
    EXPECT_EQ(m.kit.talker_specs()[0].format_word, INT24_8CH_96K);
    EXPECT_EQ(m.kit.talker_specs()[1].format_word, INT24_8CH_96K);
}

// Two units in one clock domain: no stream is unit 0's alone (Clauses 7.2.3, 7.2.6 tie no stream to a unit's
// rate), so its rate stays: NOT_SUPPORTED, the data plane not asked
TEST(avb_entity_kit_formats, a_unit_sharing_its_clock_domain_keeps_its_rate)
{
    TwoUnitKit m{true, 0x76, true};
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(m.asked.empty());
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
}

// A running stream kept at 48 kHz while the unit went to 96 kHz is still at 48 kHz after a restart: the rate is
// restored first, then every stream of the unit at the format it had
TEST(avb_entity_kit_formats, a_stream_kept_at_its_rate_stays_there_across_a_restart)
{
    constexpr uint16_t SYNC_SAMPLE_RATE_CONV = 0x0004;
    auto const dir = state_dir("kept-rate");
    auto const path = kit_state_path(dir, 0x77);
    {
        RateKit m{path, 0x77, true, false, false, true, SYNC_SAMPLE_RATE_CONV};
        EXPECT_TRUE(m.start_ready());
        (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
        m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    }
    {
        RateKit m{path, 0x77, true, false, false, true, SYNC_SAMPLE_RATE_CONV};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.kit.sample_rate(), 96000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_96K);
    }
    std::filesystem::remove_all(dir);
}

// A stream path refuses a format it cannot carry and keeps its own
TEST(avb_entity_kit_formats, a_refused_reshape_leaves_the_slot)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    auto spec = m.kit.talker_specs().front();
    spec.format.sample_rate_hz = 12345;  // no AAF code
    EXPECT_FALSE(m.kit.talker().reformat_stream(spec).has_value());
    EXPECT_TRUE(m.kit.talker().slot_for(0)->aaf->sample_rate == avtp::AafSampleRate::rate_48_khz);
    EXPECT_FALSE(m.kit.listener()->reformat_stream(spec).has_value());  // index 0 is an AAF input too
    EXPECT_EQ(m.kit.listener()->slot_for(0)->aaf->channel_count, uint16_t{8});
    spec = m.kit.talker_specs().back();                             // the CRF
    EXPECT_TRUE(m.kit.talker().reformat_stream(spec).has_value());  // its own format: nothing to do
    spec.format_word ^= 1;
    EXPECT_FALSE(m.kit.talker().reformat_stream(spec).has_value());
}

// IEEE 802.1Q-2022 Clause 34.3.1: a talker format change that pushes every talker's
// TSpec bandwidth (Milan v1.2 Table 4.4) past 75% of the link is NO_RESOURCES; the other
// talkers' current formats count, a change lowering the total is always accepted
TEST(avb_entity_kit_formats, a_talker_change_keeps_within_the_class_a_budget)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.kit.link_mbps(), 1000U);
    auto const output = [&m](uint64_t const word) {
        return m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, word));
    };
    // INT24 8 ch: 13504 kbit/s, INT32 8 ch: 16576, the CRF talker: 5632.
    // At 28 Mbit/s (21000 kbit/s) the INT32 output fits alone, not beside the CRF.
    m.kit.set_link_mbps(28);
    EXPECT_EQ(output(base_format(48000, 8)), atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_TRUE(m.formats.empty());
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    // At 30 Mbit/s (22500 kbit/s) the 22208 fits.
    m.kit.set_link_mbps(30);
    EXPECT_EQ(output(base_format(48000, 8)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), base_format(48000, 8));
    // Over budget already, narrowing back is accepted; listeners never count.
    m.kit.set_link_mbps(1);
    EXPECT_EQ(output(INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
}

// --- The Milan listener -------------------------------------------------------

namespace {

ieee::Eui64 const BINDING_CONTROLLER = ieee::Eui64{}.from_uint64(0x001B92FFFE00C001ULL);

namespace sif = atdecc::aem::stream_info_flags;

}  // namespace

// BIND_RX answered at once; the probe makes the talker declare; the listener
// settles and declares Ready; UNBIND withdraws without a DISCONNECT_TX.

// Settled before the talker's Advertise reaches it, the listener declares
// nothing (Milan v1.2 Section 5.3.8.5); once registered, Listener Ready

// START/STOP_STREAMING on a bound input (Milan v1.2 Sections 5.4.2.19-20):
// stopped discards the stream's AVTPDUs and shows STREAMING_WAIT; no effect
// on an unbound input; outputs still refuse them

// A locked entity accepts BIND_RX / UNBIND_RX only from the locking controller

// A sink settled on a VID other than the Class A one joins it with MVRP,
// and leaves it when it unsettles (Milan v1.2 Section 4.4.1)

// Rebound to another talker's stream, a settled input leaves its group before
// it joins the new one: each group is joined once while it is listened to

// stop() releases the connected group before it closes the port, which drops what else it holds;
// the log outlives the port

// A locked entity's stop() still unbinds its inputs, as the locking controller

// stop() drops the input's connection, not its saved binding: after a restart
// it is unconnected until the binding settles again, declared and joined anew

// A classic input is disconnected by stop() with its fast-connect goal kept;
// after a restart the goal connects it again

// [avb] milan = false keeps the classic listener: CONNECT_RX relayed as
// CONNECT_TX, no Milan sink bound

// The bound state is saved and restored: PRB_W_AVAIL until the talker
// advertises, then a probe (Milan v1.2 Sections 5.3.8.2-3, 5.3.8.7, 5.5.3.5.2).

// --- Counters, GET_AS_PATH and GET_AVB_INFO ----------------------------------

namespace {

/// The unsolicited AEM responses for @p command among @p sent.
auto unsolicited_count(std::vector<std::vector<uint8_t>> const& sent, uint16_t const command) -> int
{
    int n = 0;
    for (auto const& bytes : sent) {
        if (bytes.size() < nanoavb::AemDu::LENGTH) {
            continue;
        }
        nanoavb::AemDu hdr{};
        span_load(hdr, std::span<uint8_t const>{bytes}.first(nanoavb::AemDu::LENGTH));
        if (hdr.is_response() && hdr.is_unsolicited() && hdr.command_code() == command) {
            ++n;
        }
    }
    return n;
}

auto load_counters(std::vector<uint8_t> const& body) -> atdecc::aem::AemCountersPayload
{
    atdecc::aem::AemCountersPayload c{};
    span_load(c, std::span<uint8_t const>{body}.first(sizeof(atdecc::aem::AemCountersPayload)));
    return c;
}

auto counters_of(MilanKit& m, uint16_t const type, uint16_t const index) -> atdecc::aem::AemCountersPayload
{
    std::array<uint8_t, 4> body{
        static_cast<uint8_t>(type >> 8U),
        static_cast<uint8_t>(type),
        static_cast<uint8_t>(index >> 8U),
        static_cast<uint8_t>(index)};
    auto const r = m.aem_command(atdecc::AEM_COMMAND_GET_COUNTERS, body);
    EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
    return load_counters(r.body);
}

}  // namespace

// Milan v1.3 Table 5.14: the output's counters at IEEE 1722.1-2021's bit values (0, 1, 3, 4, 7);
// STREAM_START when a Listener Ready opens the gate, STREAM_STOP when it
// closes, each notified.
TEST(avb_entity_kit_counters, stream_output_start_stop_at_milan_bits)
{
    MilanKit m{std::nullopt, 0x60};
    EXPECT_TRUE(m.start_ready());
    m.kit.host().components().aem_handler.tick(sm::Clock::now());
    auto c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters_valid.get(), 0x9BU);
    EXPECT_EQ(c.counters[0].get(), 0U);

    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters[0].get(), 1U);  // STREAM_START
    EXPECT_EQ(c.counters[1].get(), 0U);  // STREAM_STOP

    // The Listener goes; the grace window runs out: stopped.
    m.bridge_withdraw(0);
    m.kit.poll_admission(MilanKit::raw_now() + TalkerGate::GRACE + std::chrono::seconds{1});
    c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters[0].get(), 1U);
    EXPECT_EQ(c.counters[1].get(), 1U);
    EXPECT_TRUE(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_COUNTERS) == 0);  // nobody registered

    // Classic: IEEE 1722.1-2021's layout (FRAMES_TX at bit 7).
    MilanKit classic{false, 0x61};
    EXPECT_TRUE(classic.start_ready());
    EXPECT_EQ(counters_of(classic, DESCRIPTOR_STREAM_OUTPUT, 0).counters_valid.get(), 0x9BU);
}

// An offload data plane is told each change of a talker stream's gate, as STREAM_START and STREAM_STOP count
// it: a Listener Ready, STOP_STREAMING and START_STREAMING, the grace running out; nothing once the hook is removed.
TEST(avb_entity_kit_offload, the_gate_hook_follows_stream_start_and_stop)
{
    std::vector<std::pair<uint16_t, bool>> told;  // outlives the kit, whose stop() closes the gate
    MilanKit m{false, 0x64};
    m.kit.set_on_talker_gate_changed([&told](uint16_t const idx, bool const open) { told.emplace_back(idx, open); });
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);  // IEEE 1722.1: connected, then admitted
    EXPECT_TRUE(told.empty());
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_TRUE(told == (std::vector<std::pair<uint16_t, bool>>{{0, true}}));
    std::array<uint8_t, 4> const output{0x00, 0x06, 0x00, 0x00};
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_STOP_STREAMING, output).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_START_STREAMING, output).status, atdecc::AEM_STATUS_SUCCESS);
    nanoavb::StreamId sid{};
    sid.from_uint64(m.sid(0).to_uint64());
    auto const left = MilanKit::raw_now();
    m.kit.notify_listener_ready(sid, false, MilanKit::steady_ns(), left);
    EXPECT_EQ(told.size(), size_t{3});  // the grace still admits
    m.kit.on_admission_timer(left + TalkerGate::GRACE);
    EXPECT_TRUE(told == (std::vector<std::pair<uint16_t, bool>>{{0, true}, {0, false}, {0, true}, {0, false}}));
    auto const c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_TRUE(c.counters[0].get() == 2U && c.counters[1].get() == 2U);  // STREAM_START, STREAM_STOP
    m.kit.set_on_talker_gate_changed({});
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_EQ(told.size(), size_t{4});
}

// A stream's new format is told as a change of its frames once the entity's hook accepted it, after the kit's stream
// path has the new shape; a format the hook refuses, and the format in force, tell nothing.
TEST(avb_entity_kit_offload, an_accepted_format_is_told_as_a_change_of_the_frames)
{
    std::vector<uint16_t> told;  // outlives the kit
    MilanKit p{std::nullopt, 0x67, true, make_multirate_blob(false, false, false, true)};
    uint8_t answer = atdecc::AEM_STATUS_SUCCESS;
    p.kit.set_on_stream_format_changed([&answer](uint16_t, uint16_t, StreamFormatFields const&) { return answer; });
    EXPECT_TRUE(p.start_ready());
    p.kit.talker().set_on_frames_changed([&told, &p](uint16_t const idx) {
        told.push_back(idx);
        told.push_back(p.kit.talker().slot_for(idx)->spec.format.channels);  // the frames are the new format's
    });
    auto const set = [&p](uint16_t const channels) {
        return p
            .aem_command(
                atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, channels)))
            .status;
    };
    answer = atdecc::AEM_STATUS_NOT_SUPPORTED;
    EXPECT_EQ(set(2), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(told.empty());
    answer = atdecc::AEM_STATUS_SUCCESS;
    EXPECT_EQ(set(2), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(told == (std::vector<uint16_t>{0, 2}));
    EXPECT_EQ(set(2), atdecc::AEM_STATUS_SUCCESS);  // the format in force
    EXPECT_EQ(told.size(), size_t{2});
    p.kit.talker().set_on_frames_changed({});
}

// What an offload data plane sent of a talker stream reaches GET_COUNTERS as the kit's own AVTPDUs do: the
// classic totals of AVTPDUs, tu AVTPDUs and mr toggles, and Milan v1.3's interval counters.
TEST(avb_entity_kit_offload, what_was_sent_offloaded_reaches_get_counters)
{
    MilanKit classic{false, 0x65};
    EXPECT_TRUE(classic.start_ready());
    auto& talker = classic.kit.talker();
    auto const first = talker.offload_header(0);
    EXPECT_TRUE(first.has_value() && !first->mr);
    EXPECT_TRUE(talker.notify_offload_sent(0, {.pdus = 40, .tu_pdus = 8, .next_sequence = 40}).has_value());
    EXPECT_TRUE(talker.toggle_media_clock_restart(0).has_value());
    auto const toggled = talker.notify_offload_sent(0, {.pdus = 0, .tu_pdus = 0, .next_sequence = 40});
    EXPECT_TRUE(toggled.has_value() && toggled->mr);
    EXPECT_TRUE(talker.notify_offload_sent(0, {.pdus = 8, .tu_pdus = 0, .next_sequence = 48}).has_value());
    auto const c = counters_of(classic, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters[3].get(), 1U);   // mr toggles
    EXPECT_EQ(c.counters[4].get(), 8U);   // tu AVTPDUs
    EXPECT_EQ(c.counters[7].get(), 48U);  // FRAMES_TX

    MilanKit milan{std::nullopt, 0x66};
    EXPECT_TRUE(milan.start_ready());
    int64_t t = MilanKit::steady_ns();
    milan.kit.poll_counters(t);  // the first interval starts
    auto& milan_talker = milan.kit.talker();
    EXPECT_TRUE(milan_talker.offload_header(0).has_value());
    EXPECT_TRUE(milan_talker.notify_offload_sent(0, {.pdus = 8, .tu_pdus = 1, .next_sequence = 8}).has_value());
    EXPECT_TRUE(milan_talker.toggle_media_clock_restart(0).has_value());
    EXPECT_TRUE(milan_talker.notify_offload_sent(0, {.pdus = 0, .tu_pdus = 0, .next_sequence = 8}).has_value());
    EXPECT_TRUE(milan_talker.notify_offload_sent(0, {.pdus = 8, .tu_pdus = 0, .next_sequence = 16}).has_value());
    milan.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    auto const v = counters_of(milan, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(v.counters[3].get(), 1U);  // MEDIA_RESET: an interval with an mr toggle
    EXPECT_EQ(v.counters[4].get(), 1U);  // TIMESTAMP_UNCERTAIN
    EXPECT_EQ(v.counters[7].get(), 1U);  // FRAMES_TX
}

// IEEE 1722.1-2021 Table 7-145: a classic output stopped by STOP_STREAMING reports STREAMING_WAIT
// until START_STREAMING; each command is pushed as an unsolicited GET_STREAM_INFO.
TEST(avb_entity_kit_classic_talker, a_stopped_output_reports_streaming_wait)
{
    MilanKit m{false, 0x63};
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    m.aem.clear();
    auto const waiting = [&m] {
        return (load_info(m.stream_info(0).body).first.flags.get() & atdecc::aem::stream_info_flags::STREAMING_WAIT) != 0;
    };
    std::array<uint8_t, 4> output{0x00, 0x06, 0x00, 0x00};
    EXPECT_FALSE(waiting());

    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_STOP_STREAMING, output).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(waiting());
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_STREAM_INFO), 1);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_STOP_STREAMING, output).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(waiting());
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_STREAM_INFO), 2);

    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_START_STREAMING, output).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(waiting());
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_STREAM_INFO), 3);
}

// Milan v1.3 Table 5.14: the output's counters at IEEE 1722.1-2021's positions; with signal presence
// reported (Section 5.3.7.7.1) ENTITY_SPECIFIC_9 and _10 too, channel 0 at the MSB, from the next fold
TEST(avb_entity_kit_counters, milan_v1_3_output_counters_and_signal_presence)
{
    MilanKit m{std::nullopt, 0x62, true, make_blob(1, 1), std::nullopt, net::monotonic_ns};
    EXPECT_TRUE(m.start_ready());
    auto c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters_valid.get(), 0x9BU);
    m.kit.set_output_signal_presence(0, 0b101);  // not configured: ignored
    int64_t t = MilanKit::steady_ns();
    m.kit.poll_counters(t);
    m.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    EXPECT_EQ(counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0).counters_valid.get(), 0x9BU);  // no presence

    // Configured: both quadlets valid from the start (no signal) and through a fold nothing reported in;
    // a report shows at the next fold, masked to the stream's channels as its format is now (8 here, then
    // 2 after a SET_STREAM_FORMAT: Milan v1.3 Section 5.3.7.7.1).
    MilanKit p{std::nullopt, 0x63, true, make_multirate_blob(false, false, false, true), std::nullopt, net::monotonic_ns, true};
    p.kit.set_on_stream_format_changed([](uint16_t, uint16_t, StreamFormatFields const&) { return atdecc::AEM_STATUS_SUCCESS; });
    EXPECT_TRUE(p.start_ready());
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters_valid.get(), 0x9BU | 0x00C0'0000U);
    t = MilanKit::steady_ns();
    p.kit.poll_counters(t);
    p.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters_valid.get(), 0x9BU | 0x00C0'0000U);
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_0_31].get(), 0U);
    p.kit.set_output_signal_presence(0, (uint64_t{1} << 1U) | (uint64_t{1} << 7U) | (uint64_t{1} << 8U) | (uint64_t{1} << 40U));
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_0_31].get(), 0U);
    p.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_0_31].get(), 0x4100'0000U);
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_32_59].get(), 0U);
    // Narrowed to 2 channels: channel 7 is no longer the stream's, from the next fold, which notifies.
    EXPECT_EQ(
        p.aem_command(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 2)))
            .status,
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(p.kit.talker_channels(0), uint16_t{2});
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_0_31].get(), 0x4100'0000U);
    p.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_0_31].get(), 0x4000'0000U);
    // A report past the stream's channels is cut to them when it is accepted.
    p.kit.set_output_signal_presence(0, (uint64_t{1} << 0U) | (uint64_t{1} << 5U));
    p.kit.poll_counters(t + AvbEntityKit::COUNTERS_INTERVAL_NS);
    EXPECT_EQ(counters_of(p, DESCRIPTOR_STREAM_OUTPUT, 0).counters[StreamOutputIntervals::PRESENCE_0_31].get(), 0x8000'0000U);

    // Reported: the two quadlets beside the five counters.
    StreamOutputIntervals intervals{};
    intervals.set_presence((uint64_t{1} << 0U) | (uint64_t{1} << 31U) | (uint64_t{1} << 32U) | (uint64_t{1} << 59U));
    EXPECT_TRUE(intervals.fold({}));
    uint32_t valid = 0;
    std::array<uint32_t, 32> out{};
    fill_milan_v13_talker_counters(intervals, valid, out);
    EXPECT_EQ(valid, 0x9BU | 0x00C0'0000U);
    EXPECT_EQ(out[StreamOutputIntervals::PRESENCE_0_31], 0x8000'0001U);
    EXPECT_EQ(out[StreamOutputIntervals::PRESENCE_32_59], 0x8000'0010U);  // channel 59 is bit 27
    EXPECT_FALSE(intervals.fold({}));                                     // unchanged
}

// Milan v1.3 Table 4.4: a v1.3 entity declares the AVTPDU as MaxFrameSize, the shaper the drift byte
TEST(avb_entity_kit_formats, milan_v1_3_tspec)
{
    MilanKit m{std::nullopt, 0x65, true, make_blob(1, 1), std::nullopt, net::monotonic_ns};
    auto const tspec = m.kit.talker_tspec(m.kit.talker_specs().front());
    EXPECT_EQ(tspec.max_frame_size, milan_srp_max_frame_size(m.kit.talker_specs().front().format));
    EXPECT_EQ(tspec.clock_drift_bytes, MILAN_TSPEC_SPARE_BYTES);
    EXPECT_TRUE(m.kit.host().components().acmp_is_milan());
    EXPECT_TRUE(m.kit.host().components().aem_handler.stream_info_layout() == atdecc::aem::StreamInfoLayout::ieee_2021);
}

// What an offload data plane received of an input reaches GET_COUNTERS as the kit's own reception does: Milan
// v1.2 Table 5.16's interval counters, MEDIA_LOCKED from its check of the lock.

// Milan v1.2 Table 5.16: the input's ten counters; zeroed when it becomes
// bound (Section 5.3.8.10), not when it is unbound; the fold counts intervals.

// A CRF input locks on a steady timestamp step (timestamp_interval events, IEEE 1722-2016
// Clause 10.4) and unlocks on a lost PDU; a PDU of another format is discarded (Milan v1.2
// Section 4.4.2.2) and counted in UNSUPPORTED_FORMAT.

// Milan v1.3 Section 5.3.8.10: a CRF PDU the input rejects is still a Stream Data AVTPDU, counted
// in UNSUPPORTED_FORMAT and holding its place in the sequence.

// A CRF PDU cut short past its AVTP common header still counts in UNSUPPORTED_FORMAT and holds its
// place in the sequence (Milan v1.3 Section 5.3.8.10).

// A frame of another AVTP subtype carrying a CRF input's stream_id is a Stream Data AVTPDU of
// that stream in a format it does not accept: UNSUPPORTED_FORMAT, in sequence (Milan v1.3 Section 5.3.8.10).

// GET_AS_PATH (Milan v1.2 Section 5.4.2.24) from the Announces' path trace,
// and its notification; GET_AVB_INFO notified asynchronously when the
// grandmaster changes (Table 5.22).
TEST(avb_entity_kit_counters, as_path_and_avb_info_follow_gptp)
{
    MilanKit m{std::nullopt, 0x62};
    EXPECT_TRUE(m.start_ready());
    auto* handlers = m.kit.host().net_handlers();
    EXPECT_TRUE(handlers != nullptr);
    std::array<uint8_t, 0> none{};
    (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
    m.aem.clear();

    std::array<uint8_t, 4> query{};
    auto r = m.aem_command(atdecc::AEM_COMMAND_GET_AS_PATH, query);
    EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(r.body.size(), size_t{4});  // no grandmaster: an empty path

    gptp::AnnounceMessage announce{};
    announce.init();
    announce.grandmaster_identity = gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL};
    nanoavb::AsPath trace{};
    trace.push_back(ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL));
    trace.push_back(ieee::Eui64{}.from_uint64(0x001B'21FF'FE00'0002ULL));
    handlers->gptp_handler().set_callbacks(nanoavb::GptpAnnounceCallbacks{
        .grandmaster_id_changed = [&m](int64_t, gptp::ClockIdentity const& gm, gptp::AnnounceMessage const&) {
            m.kit.notify_grandmaster_changed(gm);
        }});
    handlers->gptp_handler().set_on_path_trace_changed([&m](int64_t) { m.kit.notify_as_path_changed(); });
    handlers->gptp_handler().observe_announce(MilanKit::steady_ns(), announce, trace);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AS_PATH), 1);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AVB_INFO), 1);
    r = m.aem_command(atdecc::AEM_COMMAND_GET_AS_PATH, query);
    EXPECT_EQ(r.body.size(), size_t{4 + 16});  // no MAC here: our own identity is not known
    EXPECT_EQ(r.body[3], 2);
    EXPECT_EQ(r.body[4 + 8 + 7], 0x02);

    // The same Announce again: nothing new.
    handlers->gptp_handler().observe_announce(MilanKit::steady_ns(), announce, trace);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AS_PATH), 1);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AVB_INFO), 1);
}

// GPTP_GM_CHANGED (Milan v1.3 Section 5.3.6.3, IEEE 1722.1-2021 Clause 7.4.42.2.2) follows the grandmaster
// in force: the first is none; lost to the Announce receipt timeout, this grandmaster-capable entity is its
// own (one), the same one found again another, and a different one another.
TEST(avb_entity_kit_counters, gptp_gm_changed_follows_the_grandmaster_in_force)
{
    MilanKit m{std::nullopt, 0x9A};
    EXPECT_TRUE(m.start_ready());
    auto& announces = m.kit.host().net_handlers()->gptp_handler();
    announces.set_callbacks(nanoavb::GptpAnnounceCallbacks{
        .grandmaster_id_changed =
            [&m](int64_t, gptp::ClockIdentity const& gm, gptp::AnnounceMessage const&) { m.kit.notify_grandmaster_changed(gm); },
        .grandmaster_lost = [&m](int64_t) { m.kit.notify_grandmaster_lost(); }});
    gptp::AnnounceMessage announce{};
    announce.init();
    announce.header.log_message_interval = 0;  // 1 s: lost after 3 s
    announce.grandmaster_identity = gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL};

    int64_t t = MilanKit::steady_ns();
    announces.observe_announce(t, announce);
    announces.tick(t += 10'000'000'000);
    EXPECT_FALSE(announces.has_grandmaster());
    announces.observe_announce(t += 1'000'000'000, announce);
    EXPECT_TRUE(announces.has_grandmaster());
    EXPECT_EQ(counters_of(m, DESCRIPTOR_AVB_INTERFACE, 0).counters[5].get(), 2U);  // GPTP_GM_CHANGED

    announce.grandmaster_identity = gptp::ClockIdentity{0x0011'22FF'FE33'6677ULL};
    announces.observe_announce(t + 1'000'000'000, announce);
    EXPECT_EQ(counters_of(m, DESCRIPTOR_AVB_INTERFACE, 0).counters[5].get(), 3U);
}

// The control plane's own gPTP wiring (setup_nanoavb_callbacks): a new path trace is an unsolicited GET_AS_PATH
// (IEEE 1722.1-2021 Clause 7.4.41), and a lost grandmaster leaves ADP naming what GET_AVB_INFO answers, a
// grandmaster-capable entity's own clock (Milan v1.3 Section 4.2.6.2.1), not zero.
TEST(avb_entity_kit_counters, the_default_gptp_wiring_notifies_the_path_and_names_the_own_clock)
{
    auto const own = ieee::Eui64{}.from_uint64(0x0200'00FF'FE00'00C1ULL);
    MilanKit m{std::nullopt, 0xC1, true, make_blob(1, 1, 1, false, 0, own)};
    EXPECT_TRUE(m.start_ready());
    auto* handlers = m.kit.host().net_handlers();
    EXPECT_TRUE(handlers != nullptr);
    std::array<uint8_t, 0> none{};
    (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
    m.aem.clear();
    auto const& adp = m.kit.host().components().adp_advertiser;

    gptp::AnnounceMessage announce{};
    announce.init();
    announce.grandmaster_identity = gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL};
    nanoavb::AsPath trace{};
    trace.push_back(ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL));
    int64_t const t0 = MilanKit::steady_ns();
    handlers->gptp_handler().observe_announce(t0, announce, trace);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AS_PATH), 1);
    EXPECT_EQ(adp.adpdu().gptp_grandmaster_id.to_uint64(), 0x0011'22FF'FE33'4455ULL);

    handlers->gptp_handler().tick(t0 + 10'000'000'000);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AS_PATH), 2);  // the path cleared
    EXPECT_EQ(m.kit.host().avb_info(0).gptp_grandmaster_id.to_uint64(), own.to_uint64());
    EXPECT_EQ(adp.adpdu().gptp_grandmaster_id.to_uint64(), own.to_uint64());
}

namespace {

/// A bridge port declaring Class A Domains to a started kit, and registering the kit's.
struct DomainBridge
{
    explicit DomainBridge(MilanKit& kit)
        : m{kit}
    {
        bridge.set_send_pdu([this](std::span<uint8_t const> p) {
            to_entity.emplace_back(p.begin(), p.end());
            return true;
        });
        (void)bridge.subscribe(srp::msrp::Observer{.on_domain = [this](srp::msrp::DomainFirstValue const& fv, srp::mrp::Operation) {
            registered.push_back(fv.sr_class_vid.get());
        }});
        m.kit.host().components().msrp_handler.set_send_packet([this](std::span<uint8_t const> p) {
            to_bridge.emplace_back(p.begin(), p.end());
            return true;
        });
        bridge.start(now);
    }
    /// A second of MRP traffic both ways.
    void exchange()
    {
        for (int i = 0; i < 4; ++i) {
            now += std::chrono::milliseconds(250);
            bridge.tick(now);
            m.kit.host().components().msrp_handler.tick(now);
            for (auto const& p : std::exchange(to_entity, {})) {
                m.kit.host().components().msrp_handler.receive_packet(p, now);
            }
            for (auto const& p : std::exchange(to_bridge, {})) {
                bridge.receive_pdu(p, now);
            }
        }
    }
    /// The kit's streams, ACMP, Talker Advertise and MVRP all on @p vid.
    [[nodiscard]] auto all_on(uint16_t const vid) const -> bool
    {
        auto& c = m.kit.host().components();
        auto const stream = c.talker_stream(0);
        return c.msrp_handler.domain().sr_class_vid == vid && m.kit.talker().vlan_id() == vid && stream.has_value() &&
            stream->stream_vlan_id == vid && m.kit.make_talker_srp_info(m.kit.talker_specs()[0]).vlan_id == vid &&
            c.mvrp_handler.get_vlan_state(vid) != nanoavb::VlanState::Unregistered;
    }

    MilanKit& m;
    sm::TimePoint now{sm::Clock::now()};
    srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9f};
    std::vector<std::vector<uint8_t>> to_entity;
    std::vector<std::vector<uint8_t>> to_bridge;
    std::vector<uint16_t> registered;  ///< the VIDs of the kit's Domain declarations, as the bridge registered them
};

}  // namespace

// An adopted Domain is told to GET_AVB_INFO's subscribers through the host's refresh (Milan v1.3 Table 5.20),
// so only a changed answer is notified, and once: here no MSRP socket is open, the answer lists no
// msrp_mappings, and neither the adoption nor a later refresh notifies anything.
TEST(avb_entity_kit_domain, an_adopted_domain_notifies_avb_info_only_for_a_changed_answer)
{
    MilanKit m{std::nullopt, 0x34};
    EXPECT_TRUE(m.start_ready());
    std::array<uint8_t, 0> none{};
    (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
    DomainBridge b{m};
    m.aem.clear();
    auto const before = m.kit.host().avb_info(0);
    EXPECT_TRUE(before.msrp_mappings.empty());
    (void)b.bridge.declare_domain(srp::msrp::DomainFirstValue{.sr_class_vid = 100}, b.now);
    b.exchange();
    EXPECT_TRUE(b.all_on(100));
    EXPECT_EQ(m.kit.host().avb_info(0).msrp_mappings.size(), before.msrp_mappings.size());  // none listed
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AVB_INFO), 0);
    m.kit.host().notify_port_as_capable(std::nullopt);  // a refresh with nothing changed
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AVB_INFO), 0);
}

namespace {

/// Whether the kit's talker sets AVTP tu now and one second from now (past Milan v1.3 Annex B.1.1's window).
auto tu_now_and_later(MilanKit& m) -> std::pair<bool, bool>
{
    int64_t const now = net::monotonic_ns();
    return {m.kit.talker().timestamp_uncertain(now), m.kit.talker().timestamp_uncertain(now + 1'000'000'000)};
}

}  // namespace

// AVTP tu on the Announce path (IEEE 1722-2016 Clause 4.4.4.7): a change opens the window; a grandmaster-capable
// entity whose Announces time out is its own grandmaster, certain at once; one with priority1 255 is uncertain.
TEST(avb_entity_kit_tu, an_announce_timeout_is_certain_for_a_capable_entity_alone)
{
    gptp::ClockIdentity const gm_a{0x0011'22FF'FE33'4455ULL};
    MilanKit capable{std::nullopt, 0xA1};
    capable.kit.notify_grandmaster_changed(gm_a);
    EXPECT_TRUE((tu_now_and_later(capable) == std::pair{true, false}));
    capable.kit.notify_grandmaster_lost();
    EXPECT_TRUE((tu_now_and_later(capable) == std::pair{false, false}));

    MilanKit incapable{std::nullopt, 0xA2, true, make_blob(1, 1, 1, false, 255)};
    incapable.kit.notify_grandmaster_changed(gm_a);
    incapable.kit.notify_grandmaster_lost();
    EXPECT_TRUE((tu_now_and_later(incapable) == std::pair{true, true}));
}

// AVTP tu following a gPTP stack: none named yet or none named is uncertain until one is, another clock opens the
// window, the integrator's own clock is certain at once, and the Announce handler changes nothing.
TEST(avb_entity_kit_tu, tu_follows_the_gptp_stack_once_used)
{
    MilanKit m{std::nullopt, 0xA3};
    auto& host = m.kit.host();
    host.use_parent_grandmaster();
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{true, true}));
    host.notify_parent_grandmaster(ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL));
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{true, false}));
    host.notify_own_grandmaster(ieee::Eui64{}.from_uint64(0x001B'21FF'FE00'0001ULL));
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{false, false}));
    m.kit.notify_grandmaster_changed(gptp::ClockIdentity{0x0011'22FF'FE33'6677ULL});
    m.kit.notify_grandmaster_lost();
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{false, false}));
    host.notify_parent_grandmaster_loss();
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{true, true}));
    host.notify_parent_grandmaster(ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'6677ULL));
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{true, false}));
}

// While a gPTP stack (ptp4l) names the grandmaster, its state alone decides tu: the Announce handler's receipt
// timeout, which may fire in linuxptp's randomised Announce gap with nothing lost, leaves tu and ADP alone.
TEST(avb_entity_kit_tu, an_announce_timeout_changes_nothing_while_the_stack_names_a_grandmaster)
{
    MilanKit m{std::nullopt, 0xA4};
    auto& host = m.kit.host();
    auto const gm = ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL);
    host.notify_parent_grandmaster(gm);
    m.kit.notify_grandmaster_changed(gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL});
    m.kit.notify_grandmaster_lost();
    EXPECT_TRUE((tu_now_and_later(m) == std::pair{true, false}));  // the stack's first: the window alone
    EXPECT_TRUE(host.avb_info(0).gptp_grandmaster_id == gm);
}

// Milan v1.2 Section 5.5.2.7 / Table 5.47: connection_count stays 0 whatever
// listens; a Listener's registration and its departure reach a controller as
// GET_STREAM_INFO notifications with REGISTERING set and cleared (Table 5.12).
TEST(avb_entity_kit_milan_talker, listeners_are_notified_through_registering_not_connection_count)
{
    MilanKit m{std::nullopt, 0x1B};
    EXPECT_TRUE(m.start_ready());
    m.use_milan_v1_2_stream_info();
    std::array<uint8_t, 0> none{};
    (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.aem.clear();

    auto const registering = [&m]() -> std::optional<uint32_t> {
        auto const notes = unsolicited(m.aem, atdecc::AEM_COMMAND_GET_STREAM_INFO);
        if (notes.empty()) {
            return std::nullopt;
        }
        auto const [info, ext] = load_info(notes.back());
        EXPECT_EQ(info.descriptor_type.get(), DESCRIPTOR_STREAM_OUTPUT);
        return ext.flags_ex.get() & atdecc::aem::stream_info_flags_ex::REGISTERING;
    };
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_EQ(registering(), std::optional<uint32_t>{atdecc::aem::stream_info_flags_ex::REGISTERING});
    EXPECT_EQ(m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 0).connection_count.get(), uint16_t{0});

    // The Leave ages the registration out on the entity's leave timer (Milan LeaveTime, 5 s).
    m.aem.clear();
    m.bridge_withdraw(0);
    for (int i = 0; i < 60; ++i) {
        m.bridge_now += std::chrono::milliseconds(100);
        m.kit.host().components().msrp_handler.tick(m.bridge_now);
    }
    EXPECT_EQ(registering(), std::optional<uint32_t>{0U});
    EXPECT_EQ(m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 0).connection_count.get(), uint16_t{0});
}

TEST(avb_entity_kit_counters, as_path_ends_with_this_entity)
{
    nanoavb::AsPath trace{};
    auto const gm = ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL);
    auto const self = ieee::Eui64{}.from_uint64(0x001B'21FF'FE00'00AAULL);
    EXPECT_TRUE(compose_as_path(trace, self).empty());
    trace.push_back(gm);
    auto path = compose_as_path(trace, self);
    EXPECT_EQ(path.size(), size_t{2});
    EXPECT_TRUE(path.back() == self);
    EXPECT_EQ(compose_as_path(trace, std::nullopt).size(), size_t{1});
    // This entity's gPTP daemon is the grandmaster: its own Announce's trace.
    nanoavb::AsPath own{};
    own.push_back(self);
    EXPECT_EQ(compose_as_path(own, self).size(), size_t{1});
}

// A binding restored at start is bound: unbinding it is no interruption, and
// is a change of binding (notified, counters kept); a stopped input's
// discarded stream is none either.

// ---- Names and dynamic input mappings ------------------------------------------

namespace {

/// SET_NAME of (@p type, @p index, @p name_index).
auto set_name(PersistentKit& m, uint16_t const type, uint16_t const index, uint16_t const name_index, std::string_view const name)
    -> uint8_t
{
    std::array<uint8_t, 8 + 64> body{};
    body[0] = static_cast<uint8_t>(type >> 8U);
    body[1] = static_cast<uint8_t>(type);
    body[2] = static_cast<uint8_t>(index >> 8U);
    body[3] = static_cast<uint8_t>(index);
    body[5] = static_cast<uint8_t>(name_index);
    std::ranges::copy(name, body.begin() + 8);
    return m.aem_packet(atdecc::AEM_COMMAND_SET_NAME, body);
}

/// READ_DESCRIPTOR of (@p type, @p index): its object_name (offset 4; the
/// ENTITY's group_name at @p name_at).
auto read_name(PersistentKit& m, uint16_t const type, uint16_t const index, size_t const name_at = 4) -> std::string
{
    atdecc::aem::AemReadDescriptorCommandPayload read{};
    read.descriptor_type = type;
    read.descriptor_index = index;
    std::array<uint8_t, atdecc::aem::AemReadDescriptorCommandPayload::LENGTH> body{};
    span_store(body, read);
    auto const r = m.aem_command(atdecc::AEM_COMMAND_READ_DESCRIPTOR, body);
    size_t const at = atdecc::aem::AemReadDescriptorResponsePayload::LENGTH + name_at;
    if (r.status != atdecc::AEM_STATUS_SUCCESS || r.body.size() < at + 64) {
        return "(none)";
    }
    auto const* p = reinterpret_cast<char const*>(r.body.data()) + at;
    return std::string{p, ::strnlen(p, 64)};
}

/// ADD (@p add) or REMOVE_AUDIO_MAPPINGS of @p list on STREAM_PORT_INPUT 0.
auto map_command(PersistentKit& m, bool const add, std::vector<nanoavb::DynamicAudioMapping> const& list) -> uint8_t
{
    std::vector<uint8_t> body(8 + (list.size() * 8));
    atdecc::aem::AemAudioMappingsCommandHeader header{};
    header.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
    header.number_of_mappings = static_cast<uint16_t>(list.size());
    span_store(std::span{body}.first(8), header);
    for (size_t i = 0; i < list.size(); ++i) {
        atdecc::aem::AemAudioMapping wire{};
        wire.stream_index = list[i].stream_index;
        wire.stream_channel = list[i].stream_channel;
        wire.cluster_offset = list[i].cluster_offset;
        wire.cluster_channel = list[i].cluster_channel;
        span_store(std::span{body}.subspan(8 + (i * 8), 8), wire);
    }
    return m.aem_packet(add ? atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS : atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, body);
}

auto mapping(uint16_t const stream_ch, uint16_t const cluster) -> nanoavb::DynamicAudioMapping
{
    return {.stream_index = 0, .stream_channel = stream_ch, .cluster_offset = cluster, .cluster_channel = 0};
}

}  // namespace

// Milan v1.2 Section 5.3.13: every kept name is settable, notified, saved,
// restored, and cleared by an empty name; group_name is ENTITY name_index 1.
TEST(avb_entity_kit_names, every_milan_name_is_settable_saved_and_notified)
{
    auto const dir = state_dir("stage7-names");
    auto const path = kit_state_path(dir, 0x71);
    using namespace atdecc::aem;
    std::vector<std::pair<uint16_t, uint16_t>> const named{
        {DESCRIPTOR_CONFIGURATION, 0},
        {DESCRIPTOR_AUDIO_UNIT, 0},
        {DESCRIPTOR_STREAM_INPUT, 0},
        {DESCRIPTOR_STREAM_OUTPUT, 0},
        {DESCRIPTOR_AVB_INTERFACE, 0},
        {DESCRIPTOR_CLOCK_SOURCE, 0},
        {DESCRIPTOR_AUDIO_CLUSTER, 3},
        {DESCRIPTOR_CLOCK_DOMAIN, 0},
        {DESCRIPTOR_CONTROL, 0},  // the IDENTIFY control
        {DESCRIPTOR_TIMING, 0},
        {DESCRIPTOR_PTP_INSTANCE, 0},
        {DESCRIPTOR_PTP_PORT, 0}};
    auto const name_of = [](uint16_t const type) { return "name " + std::to_string(type); };
    constexpr size_t GROUP_NAME_AT = offsetof(DescriptorEntity, group_name);
    {
        RateKit m{path, 0x71, true};
        m.handler.manage_object_names();
        std::vector<uint16_t> told;
        m.handler.set_on_name_changed([&told](uint16_t const type, uint16_t, AtdeccString const&) { told.push_back(type); });
        EXPECT_TRUE(m.start_ready());
        register_other_controller(m);
        for (auto const& [type, index] : named) {
            EXPECT_EQ(set_name(m, type, index, 0, name_of(type)), atdecc::AEM_STATUS_SUCCESS);
            EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_NAME).size(), size_t{1});
            EXPECT_TRUE(read_name(m, type, index) == name_of(type));
        }
        EXPECT_EQ(set_name(m, DESCRIPTOR_ENTITY, 0, 1, "Stage left"), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_NAME).size(), size_t{1});
        EXPECT_TRUE(read_name(m, DESCRIPTOR_ENTITY, 0, GROUP_NAME_AT) == "Stage left");
        EXPECT_EQ(told.size(), named.size());  // the group name is no entity name: not told
        // No such descriptor, or a name the entity does not keep.
        EXPECT_EQ(set_name(m, DESCRIPTOR_AUDIO_CLUSTER, 9, 0, "x"), atdecc::AEM_STATUS_NOT_IMPLEMENTED);
        EXPECT_EQ(set_name(m, DESCRIPTOR_ENTITY, 0, 2, "x"), atdecc::AEM_STATUS_NOT_IMPLEMENTED);
        // Milan v1.2 Section 5.4.2.11: refused while another controller holds the lock.
        std::array<uint8_t, AemLockEntityPayload::LENGTH> lock{};
        nanoavb::AemDu header{};
        header.init_command(atdecc::AEM_COMMAND_LOCK_ENTITY, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + lock.size()));
        header.controller_entity_id = OTHER_CONTROLLER;
        std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
        EXPECT_EQ(m.kit.host().components().aem_handler.handle_command(header, lock, out).status, atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(set_name(m, DESCRIPTOR_CLOCK_DOMAIN, 0, 0, "locked out"), atdecc::AEM_STATUS_ENTITY_LOCKED);
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), named.size() + 1);
    }
    {
        RateKit m{path, 0x71, true};
        m.handler.manage_object_names();
        EXPECT_TRUE(m.start_ready());
        for (auto const& [type, index] : named) {
            EXPECT_TRUE(read_name(m, type, index) == name_of(type));
        }
        EXPECT_TRUE(read_name(m, DESCRIPTOR_ENTITY, 0, GROUP_NAME_AT) == "Stage left");
        // Empty: the authored name again (empty in this model), nothing kept.
        EXPECT_EQ(set_name(m, DESCRIPTOR_ENTITY, 0, 1, ""), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(set_name(m, DESCRIPTOR_AUDIO_CLUSTER, 3, 0, ""), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(read_name(m, DESCRIPTOR_ENTITY, 0, GROUP_NAME_AT).empty());
        EXPECT_TRUE(read_name(m, DESCRIPTOR_AUDIO_CLUSTER, 3).empty());
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), named.size() - 1);
    }
    std::filesystem::remove_all(dir);
}

// Input port mappings: identity default, hook, notify, persist, restore; a
// format dropping a mapped channel is BAD_ARGUMENTS (Milan v1.2 Section 5.4.2.7);
// mappings change while bound (Section 5.3.10.1).
TEST(avb_entity_kit_mappings, default_change_persist_restore_and_formats)
{
    auto const dir = state_dir("stage7-maps");
    auto const path = kit_state_path(dir, 0x72);
    std::vector<nanoavb::DynamicAudioMapping> identity;
    for (uint16_t ch = 0; ch < 8; ++ch) {
        identity.push_back(mapping(ch, ch));
    }
    {
        RateKit m{path, 0x72, true};
        std::vector<std::vector<nanoavb::DynamicAudioMapping>> routed;
        m.kit.set_on_input_mappings_changed([&routed](uint16_t const port, std::span<nanoavb::DynamicAudioMapping const> list) {
            EXPECT_EQ(port, uint16_t{0});
            routed.emplace_back(list.begin(), list.end());
            return atdecc::AEM_STATUS_SUCCESS;
        });
        EXPECT_TRUE(m.start_ready());
        register_other_controller(m);
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), identity));
        EXPECT_TRUE(routed.size() == 1 && routed.back() == identity);  // the default, told at start

        // A 2-channel format would drop mapped channels 2-7.
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 2))),
            atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_TRUE(m.formats.empty());
        std::vector<nanoavb::DynamicAudioMapping> upper(identity.begin() + 2, identity.end());
        EXPECT_EQ(map_command(m, false, upper), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).size(), size_t{1});
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 2))),
            atdecc::AEM_STATUS_SUCCESS);
        // The identity for a 2-channel stream is the default: nothing kept.
        EXPECT_TRUE(load_persistent_state(path, 0, {}).state.audio_maps.empty());

        // Swap the two channels and feed cluster 5 from channel 0 as well.
        EXPECT_EQ(map_command(m, false, {mapping(0, 0), mapping(1, 1)}), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(map_command(m, true, {mapping(1, 0), mapping(0, 1), mapping(0, 5)}), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS).size(), size_t{1});
        std::vector<nanoavb::DynamicAudioMapping> const swapped{mapping(1, 0), mapping(0, 1), mapping(0, 5)};
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
        EXPECT_TRUE(!routed.empty() && routed.back() == swapped);
        // A channel the 2-channel format lacks.
        EXPECT_EQ(map_command(m, true, {mapping(2, 2)}), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        auto const saved = load_persistent_state(path, 0, {}).state;
        EXPECT_EQ(saved.audio_maps.size(), size_t{1});
        EXPECT_TRUE(saved.audio_maps.size() == 1 && saved.audio_maps[0].mappings.size() == 3);
    }
    // Restart, then bound: the saved format, then the saved mappings, through the hooks.
    {
        RateKit m{path, 0x72, true};
        std::vector<std::vector<nanoavb::DynamicAudioMapping>> routed;
        m.kit.set_on_input_mappings_changed([&routed](uint16_t, std::span<nanoavb::DynamicAudioMapping const> list) {
            routed.emplace_back(list.begin(), list.end());
            return atdecc::AEM_STATUS_SUCCESS;
        });
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 2));
        std::vector<nanoavb::DynamicAudioMapping> const swapped{mapping(1, 0), mapping(0, 1), mapping(0, 5)};
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
        EXPECT_TRUE(routed.size() == 1 && routed.back() == swapped);  // the saved set, once
        EXPECT_TRUE(m.kit.host().components().milan_acmp_listener->restore_binding(
            atdecc::MilanBinding{.listener_unique_id = 0, .talker_entity_id = ieee::Eui64{}.from_uint64(0x0200001111110001ULL)},
            sm::Clock::now()));
        EXPECT_TRUE(m.kit.input_running(0));
        // Bound: the format stays, the mappings change.
        EXPECT_EQ(map_command(m, false, {mapping(0, 5)}), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(map_command(m, true, {mapping(1, 7)}), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.kit.input_mappings(0).size(), size_t{3});
        // The entity refusing a set changes nothing.
        m.kit.set_on_input_mappings_changed(
            [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_BAD_ARGUMENTS; });
        EXPECT_EQ(map_command(m, true, {mapping(0, 6)}), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_EQ(m.kit.input_mappings(0).size(), size_t{3});
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.audio_maps[0].mappings.size(), size_t{3});
    }
    // A saved set the model no longer accepts falls back to the default.
    PersistentState state = load_persistent_state(path, 0, {}).state;
    state.audio_maps[0].mappings.push_back({.stream_index = 0, .stream_channel = 9, .cluster_offset = 6, .cluster_channel = 0});
    state.listener_bindings.clear();
    EXPECT_TRUE(save_persistent_state(path, serialize_persistent_state(state)).has_value());
    {
        RateKit m{path, 0x72, true};
        m.kit.set_on_input_mappings_changed(
            [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), m.kit.default_input_mappings(0)));
        EXPECT_EQ(m.kit.input_mappings(0).size(), size_t{2});  // the 2-channel identity
    }
    std::filesystem::remove_all(dir);
}

// request_audio_mappings: a local REMOVE then ADD, all or nothing, through the hook,
// notified and saved; refused as a controller's would be, and while locked
TEST(avb_entity_kit_mappings, request_audio_mappings_is_a_local_remove_and_add)
{
    auto const dir = state_dir("local-maps");
    auto const path = kit_state_path(dir, 0x75);
    std::vector<nanoavb::DynamicAudioMapping> identity;
    for (uint16_t ch = 0; ch < 8; ++ch) {
        identity.push_back(mapping(ch, ch));
    }
    std::vector<nanoavb::DynamicAudioMapping> const upper(identity.begin() + 2, identity.end());
    std::vector<nanoavb::DynamicAudioMapping> const swapped{mapping(1, 0), mapping(0, 1)};
    {
        RateKit m{path, 0x75, true};
        std::vector<std::vector<nanoavb::DynamicAudioMapping>> routed;
        uint8_t verdict = atdecc::AEM_STATUS_SUCCESS;
        m.kit.set_on_input_mappings_changed([&](uint16_t, std::span<nanoavb::DynamicAudioMapping const> list) {
            routed.emplace_back(list.begin(), list.end());
            return verdict;
        });
        EXPECT_TRUE(m.start_ready());
        register_other_controller(m);
        routed.clear();

        // Drop channels 2-7, then the narrower format is accepted.
        m.aem.clear();
        EXPECT_EQ(m.kit.request_audio_mappings(0, {}, upper), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).size(), size_t{1});
        EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS).empty());
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 2))),
            atdecc::AEM_STATUS_SUCCESS);

        // Swap the two channels in one call: one hook call, a REMOVE and an ADD told, saved.
        m.aem.clear();
        std::vector<nanoavb::DynamicAudioMapping> const both{mapping(0, 0), mapping(1, 1)};
        EXPECT_EQ(m.kit.request_audio_mappings(0, swapped, both), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(routed.size() == 2 && routed.back() == swapped);
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).size(), size_t{1});
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS).size(), size_t{1});
        auto saved = load_persistent_state(path, 0, {}).state;
        EXPECT_TRUE(saved.audio_maps.size() == 1 && saved.audio_maps[0].mappings.size() == 2);

        // No change: no hook, nothing told.
        m.aem.clear();
        EXPECT_EQ(m.kit.request_audio_mappings(0, swapped, std::vector{mapping(0, 5)}), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(routed.size(), size_t{2});
        EXPECT_TRUE(m.aem.empty());

        // Refused as a controller's: a channel the format lacks, a cluster channel
        // mapped twice, no such port, the hook's verdict; nothing changes or is told.
        EXPECT_EQ(m.kit.request_audio_mappings(0, std::vector{mapping(2, 2)}, {}), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_EQ(m.kit.request_audio_mappings(0, std::vector{mapping(0, 0)}, {}), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_EQ(m.kit.request_audio_mappings(0, {}, std::vector{mapping(0, 99)}), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_EQ(m.kit.request_audio_mappings(9, std::vector{mapping(0, 2)}, {}), atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR);
        verdict = atdecc::AEM_STATUS_NOT_SUPPORTED;
        EXPECT_EQ(m.kit.request_audio_mappings(0, std::vector{mapping(0, 2)}, {}), atdecc::AEM_STATUS_NOT_SUPPORTED);
        verdict = atdecc::AEM_STATUS_SUCCESS;
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
        EXPECT_TRUE(m.aem.empty());

        // Milan v1.3 Section 5.3.4.1, under milan13: no local change while a controller holds the lock.
        std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};
        nanoavb::AemDu header{};
        header.init_command(atdecc::AEM_COMMAND_LOCK_ENTITY, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + lock.size()));
        header.controller_entity_id = OTHER_CONTROLLER;
        std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
        EXPECT_EQ(m.kit.host().components().aem_handler.handle_command(header, lock, out).status, atdecc::AEM_STATUS_SUCCESS);
        m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::milan13);
        EXPECT_EQ(m.kit.request_audio_mappings(0, std::vector{mapping(0, 2)}, {}), atdecc::AEM_STATUS_ENTITY_LOCKED);
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
        m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::classic);
    }
    {
        RateKit m{path, 0x75, true};
        m.kit.set_on_input_mappings_changed(
            [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));  // restored
    }
    std::filesystem::remove_all(dir);
}

// Milan v1.2 Section 5.4.2.7 for an output: a format narrower than the
// channels its static AUDIO_MAP names is BAD_ARGUMENTS (after
// STREAM_IS_RUNNING), a wide enough one accepted
TEST(avb_entity_kit_mappings, an_output_keeps_the_channels_its_static_map_names)
{
    RateKit m{{}, 0x73, true};
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.kit.output_static_map_channels(0), uint16_t{8});
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 2))),
        atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(m.formats.empty());
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
    // Streaming, STREAM_IS_RUNNING comes first.
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 2))),
        atdecc::AEM_STATUS_STREAM_IS_RUNNING);
}

// A kit started before the entity's mapping hook (its data plane not up
// yet) keeps the saved mappings, validated; a later save keeps them too
TEST(avb_entity_kit_mappings, saved_mappings_survive_a_start_before_the_hook)
{
    auto const dir = state_dir("stage7-maps-nohook");
    auto const path = kit_state_path(dir, 0x74);
    std::vector<nanoavb::DynamicAudioMapping> const swapped{mapping(1, 0), mapping(0, 1), mapping(2, 2)};
    {
        RateKit m{path, 0x74, true};
        m.kit.set_on_input_mappings_changed(
            [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
        EXPECT_TRUE(m.start_ready());
        std::vector<nanoavb::DynamicAudioMapping> rest;
        for (uint16_t ch = 0; ch < 8; ++ch) {
            rest.push_back(mapping(ch, ch));
        }
        EXPECT_EQ(map_command(m, false, rest), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(map_command(m, true, swapped), atdecc::AEM_STATUS_SUCCESS);
    }
    {
        RateKit m{path, 0x74, true};  // no hook
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
        m.kit.persist();
        auto const saved = load_persistent_state(path, 0, {}).state;
        EXPECT_TRUE(saved.audio_maps.size() == 1 && saved.audio_maps[0].mappings.size() == 3);
    }
    std::filesystem::remove_all(dir);
}

// --- Streams at another rate than the unit's ------------------------------------

namespace {

/// GET_AUDIO_MAP of STREAM_PORT_OUTPUT 0, map 0: the status and the mappings' count.
auto output_map(PersistentKit& m) -> std::pair<uint8_t, uint16_t>
{
    atdecc::aem::AemGetAudioMapCommandPayload cmd{};
    cmd.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;
    std::array<uint8_t, atdecc::aem::AemGetAudioMapCommandPayload::LENGTH> body{};
    span_store(body, cmd);
    auto const r = m.aem_command(atdecc::AEM_COMMAND_GET_AUDIO_MAP, body);
    atdecc::aem::AemAudioMapResponseHeader resp{};
    if (r.body.size() >= atdecc::aem::AemAudioMapResponseHeader::LENGTH) {
        span_load(resp, std::span<uint8_t const>{r.body}.first(atdecc::aem::AemAudioMapResponseHeader::LENGTH));
    }
    return {r.status, resp.number_of_mappings.get()};
}

}  // namespace

// Milan v1.2 Sections 5.4.2.7, 5.4.2.27: an input accepts a format at another rate
// than the unit's only while no mapping names it, keeps no default there, is
// notified and saved, and comes back to the unit's rate unmapped
TEST(avb_entity_kit_mixed_rates, an_unmapped_input_accepts_another_rate)
{
    auto const dir = state_dir("mixed-input");
    auto const path = kit_state_path(dir, 0x76);
    std::vector<nanoavb::DynamicAudioMapping> identity;
    for (uint16_t ch = 0; ch < 8; ++ch) {
        identity.push_back(mapping(ch, ch));
    }
    {
        RateKit m{path, 0x76, false, true};
        m.kit.set_on_input_mappings_changed(
            [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
        EXPECT_TRUE(m.start_ready());
        register_other_controller(m);
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), identity));
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, INT24_8CH_96K)),
            atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_TRUE(m.formats.empty());
        // Unmapped (the bridge removes them first, as a controller would), it is accepted.
        EXPECT_EQ(m.kit.request_audio_mappings(0, {}, identity), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, INT24_8CH_96K)),
            atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).size(), size_t{1});
        EXPECT_TRUE(m.formats.size() == 1 && m.formats.back().fields.sample_rate_hz == 96000U);
        EXPECT_EQ(m.kit.sample_rate(), 48000U);
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 96000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_96K);
        EXPECT_TRUE(m.kit.default_input_mappings(0).empty());
        EXPECT_EQ(map_command(m, true, {mapping(0, 0)}), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        // Another format at the same rate needs no mapping either.
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(96000, 2))),
            atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, INT24_8CH_96K)),
            atdecc::AEM_STATUS_SUCCESS);
        auto const saved = load_persistent_state(path, 0, {}).state;
        EXPECT_TRUE(saved.stream_formats.size() == 1 && saved.stream_formats[0].format == INT24_8CH_96K);
        EXPECT_TRUE(saved.audio_maps.empty());
    }
    {
        // Restored at 96 kHz before the mappings: unmapped, the RX slot at its rate.
        RateKit m{path, 0x76, false, true};
        m.kit.set_on_input_mappings_changed(
            [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_96K);
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 96000U);
        EXPECT_TRUE(m.kit.input_mappings(0).empty());
        // Back at the unit's rate the port is still empty: the caller restores its mappings.
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, INT24_8CH_48K)),
            atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(m.kit.input_mappings(0).empty());
        EXPECT_EQ(m.kit.request_audio_mappings(0, identity, {}), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), m.kit.default_input_mappings(0)));
    }
    std::filesystem::remove_all(dir);
}

// Milan v1.2 Sections 5.4.2.13, 5.3.3.3: a unit listing one rate accepts that rate
// as a no-op and refuses any other, with streams at other rates or not
TEST(avb_entity_kit_mixed_rates, a_one_rate_unit_keeps_its_rate)
{
    RateKit m{{}, 0x77, false, true};
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(48000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(48000)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
}

// A dynamic output port carries the kit's identity while its stream is at the
// unit's rate and none at another (Milan v1.2 Section 5.4.2.27), told as REMOVE
// and ADD_AUDIO_MAPPINGS; controllers cannot change it; the class A budget holds
TEST(avb_entity_kit_mixed_rates, a_dynamic_output_identity_follows_the_rate)
{
    RateKit m{{}, 0x78, false, true};
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    EXPECT_TRUE(m.kit.has_dynamic_output_map(0));
    EXPECT_EQ(m.kit.output_static_map_channels(0), uint16_t{0});
    EXPECT_EQ(m.kit.output_mappings(0).size(), size_t{8});
    EXPECT_TRUE(std::ranges::equal(m.kit.output_mappings(0), m.kit.default_output_mappings(0)));
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_SUCCESS, uint16_t{8}}));
    {
        std::vector<uint8_t> body(16);
        atdecc::aem::AemAudioMappingsCommandHeader header{};
        header.descriptor_type = atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;
        header.number_of_mappings = uint16_t{1};
        span_store(std::span{body}.first(8), header);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, body), atdecc::AEM_STATUS_NOT_SUPPORTED);
    }
    // 96 kHz doubles the AAF talker past a 28 Mbit/s link's budget (see above).
    m.kit.set_link_mbps(28);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_EQ(m.kit.output_mappings(0).size(), size_t{8});
    m.kit.set_link_mbps(1000);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).size(), size_t{1});
    EXPECT_TRUE(m.kit.output_mappings(0).empty());
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_SUCCESS, uint16_t{0}}));
    EXPECT_EQ(m.kit.talker().slot_for(0)->spec.format.sample_rate_hz, 96000U);
    EXPECT_EQ(m.kit.talker_tspec(m.kit.talker_specs().front()).max_frame_size, uint16_t{(12 * 8 * 3) + 24});
    EXPECT_EQ(m.kit.talker().config_.sample_rate, 48000U);  // the media clock stays at the unit's rate
    // Back at the unit's rate the identity returns.
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 4))),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS).size(), size_t{1});
    EXPECT_EQ(m.kit.output_mappings(0).size(), size_t{4});
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_SUCCESS, uint16_t{4}}));
}

// A static output map names its stream: no format at another rate while it does (the default)
TEST(avb_entity_kit_mixed_rates, a_static_output_map_keeps_the_unit_rate)
{
    RateKit m{{}, 0x79, true};
    EXPECT_FALSE(m.kit.output_mappings_across_rates());
    EXPECT_TRUE(m.start_ready());
    EXPECT_FALSE(m.kit.has_dynamic_output_map(0));
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(m.formats.empty());
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_NOT_SUPPORTED, uint16_t{0}}));  // its AUDIO_MAP says it
}

// Opted out of Milan v1.2 Section 5.4.2.27's rate should: an output under a static identity
// map accepts a listed format at another rate, still mapped; its channels still bound it (Section 5.4.2.7)
TEST(avb_entity_kit_mixed_rates, a_static_output_map_holds_across_rates_when_opted_in)
{
    RateKit m{{}, 0x7A, true};
    m.kit.set_output_mappings_across_rates(true);
    EXPECT_TRUE(m.kit.output_mappings_across_rates());
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.formats.size() == 1 && m.formats.back().fields.sample_rate_hz == 96000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
    EXPECT_EQ(m.kit.output_static_map_channels(0), uint16_t{8});
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_NOT_SUPPORTED, uint16_t{0}}));
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(96000, 2))),
        atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_48K)),
        atdecc::AEM_STATUS_SUCCESS);
}

// Opted out, an output's saved 96 kHz format is restored under its static map after a restart;
// a kit that follows the should refuses it at restore and keeps the model's format
TEST(avb_entity_kit_mixed_rates, a_saved_off_rate_output_format_is_restored_when_opted_in)
{
    auto const dir = state_dir("mixed-output-restore");
    auto const path = kit_state_path(dir, 0x7D);
    {
        RateKit m{path, 0x7D, true};
        m.kit.set_output_mappings_across_rates(true);
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
            atdecc::AEM_STATUS_SUCCESS);
        auto const saved = load_persistent_state(path, 0, {}).state;
        EXPECT_TRUE(saved.stream_formats.size() == 1 && saved.stream_formats[0].format == INT24_8CH_96K);
    }
    {
        RateKit m{path, 0x7D, true};
        m.kit.set_output_mappings_across_rates(true);
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
        EXPECT_EQ(m.kit.talker().slot_for(0)->spec.format.sample_rate_hz, 96000U);
        EXPECT_EQ(m.kit.output_static_map_channels(0), uint16_t{8});
        EXPECT_EQ(m.kit.sample_rate(), 48000U);
    }
    {
        RateKit m{path, 0x7D, true};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    }
    std::filesystem::remove_all(dir);
}

// Opted out, a dynamic output port keeps the kit's identity at another rate: nothing removed or told
TEST(avb_entity_kit_mixed_rates, a_dynamic_output_identity_holds_across_rates_when_opted_in)
{
    RateKit m{{}, 0x7B, false, true};
    m.kit.set_output_mappings_across_rates(true);
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).empty());
    EXPECT_EQ(m.kit.output_mappings(0).size(), size_t{8});
    EXPECT_TRUE(std::ranges::equal(m.kit.output_mappings(0), m.kit.default_output_mappings(0)));
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_SUCCESS, uint16_t{8}}));
}

// The switch covers outputs only: a mapped input still keeps the unit's rate
TEST(avb_entity_kit_mixed_rates, a_mapped_input_keeps_the_unit_rate_when_outputs_opt_in)
{
    RateKit m{{}, 0x7C, true};
    m.kit.set_output_mappings_across_rates(true);
    m.kit.set_on_input_mappings_changed(
        [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(m.kit.input_mappings(0).size(), size_t{8});
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(m.formats.empty());
}

// Milan v1.3 Section 5.3.3.7: ports that set SYNC_SAMPLE_RATE_CONV map a stream at another rate than the unit's
// like any other: a 96 kHz input keeps the identity, mappings hold across 48 <-> 96 kHz, an output's static map
// too without set_output_mappings_across_rates; Section 5.4.2.7's channel rule still holds
TEST(avb_entity_kit_mixed_rates, a_converting_port_keeps_its_mappings_across_rates)
{
    constexpr uint16_t SYNC_SAMPLE_RATE_CONV = 0x0004;
    PersistentKit m{{}, 0x7E, std::nullopt, make_multirate_blob(true, false, false, false, SYNC_SAMPLE_RATE_CONV)};
    m.kit.set_on_stream_format_changed([](uint16_t, uint16_t, StreamFormatFields const&) { return atdecc::AEM_STATUS_SUCCESS; });
    m.kit.set_on_input_mappings_changed(
        [](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    auto const set = [&m](uint16_t const type, uint64_t const word) {
        return m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(type, 0, word));
    };
    std::vector<nanoavb::DynamicAudioMapping> identity;
    identity.reserve(8);
    for (uint16_t ch = 0; ch < 8; ++ch) {
        identity.push_back(mapping(ch, ch));
    }
    EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), identity));
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, INT24_8CH_96K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 96000U);
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).empty());
    EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), identity));
    EXPECT_TRUE(std::ranges::equal(m.kit.default_input_mappings(0), identity));  // mapped with the identity at 96 kHz
    EXPECT_EQ(map_command(m, false, {mapping(0, 0), mapping(1, 1)}), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(map_command(m, true, {mapping(1, 0), mapping(0, 1)}), atdecc::AEM_STATUS_SUCCESS);
    auto const swapped = m.kit.input_mappings(0);
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, base_format(96000, 2)), atdecc::AEM_STATUS_BAD_ARGUMENTS);  // channels 2-7 mapped
    EXPECT_FALSE(m.kit.output_mappings_across_rates());
    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, INT24_8CH_96K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.output_static_map_channels(0), uint16_t{8});
}

// ASYNC_SAMPLE_RATE_CONV converts as well: a dynamic output port keeps the kit's identity at 96 kHz, nothing removed
TEST(avb_entity_kit_mixed_rates, a_converting_dynamic_output_keeps_its_identity_across_rates)
{
    constexpr uint16_t ASYNC_SAMPLE_RATE_CONV = 0x0002;
    PersistentKit m{{}, 0x7F, std::nullopt, make_multirate_blob(true, true, false, false, ASYNC_SAMPLE_RATE_CONV)};
    m.kit.set_on_stream_format_changed([](uint16_t, uint16_t, StreamFormatFields const&) { return atdecc::AEM_STATUS_SUCCESS; });
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    EXPECT_TRUE(m.kit.has_dynamic_output_map(0));
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_REMOVE_AUDIO_MAPPINGS).empty());
    EXPECT_EQ(m.kit.output_mappings(0).size(), size_t{8});
}

// --- An audio stream's kind (AAF, AM824) ---------------------------------------

// SET_STREAM_FORMAT moves a stream that lists both kinds between AAF and AM824
// while it is not running: the entity is told, controllers notified, the format
// reads back, and the TX slot's sequence number and sample count go on
TEST(avb_entity_kit_kind, an_audio_stream_switches_between_aaf_and_am824)
{
    RateKit m{{}, 0x74, false, false, /*am824=*/true};
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    auto const set = [&m](uint16_t const type, uint64_t const word) {
        return m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(type, 0, word));
    };

    m.kit.talker().slot_for(0)->aaf->sequence_num = 41;
    m.kit.talker().slot_for(0)->aaf->running_sample_count = 600;
    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, AM824_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).size(), size_t{1});
    EXPECT_TRUE(m.formats.size() == 1 && m.formats.back().fields.kind == StreamKind::am824);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), AM824_8CH_48K);
    auto const* out = m.kit.talker().slot_for(0);
    EXPECT_TRUE(out->am824.has_value() && !out->aaf.has_value() && !out->reframer.has_value());
    EXPECT_EQ(out->am824->sequence_num, uint8_t{41});
    EXPECT_EQ(out->am824->running_dbc, uint32_t{600});
    EXPECT_EQ(
        m.kit.talker_tspec(m.kit.talker_specs().front()).max_frame_size, srp_max_frame_size(decode_stream_format(AM824_8CH_48K)));

    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, AM824_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    auto const* in = m.kit.listener()->slot_for(0);
    EXPECT_TRUE(in->kind.load() == StreamKind::am824 && in->am824.has_value() && !in->aaf.has_value());
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), AM824_8CH_48K);

    m.kit.talker().slot_for(0)->am824->sequence_num = 7;
    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(out->aaf.has_value() && !out->am824.has_value() && out->reframer.has_value());
    EXPECT_EQ(out->aaf->sequence_num, uint8_t{7});
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(in->kind.load() == StreamKind::aaf && in->aaf.has_value() && !in->am824.has_value());
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), INT24_8CH_48K);
    EXPECT_EQ(m.formats.size(), size_t{4});
}

// Milan v1.2 Section 5.4.2.7: a streaming output keeps its kind as any format;
// a CRF stream never changes kind, nor an audio stream to CRF
TEST(avb_entity_kit_kind, refused_while_running_and_for_crf)
{
    RateKit m{{}, 0x75, false, false, /*am824=*/true};
    EXPECT_TRUE(m.start_ready());
    auto const set = [&m](uint16_t const type, uint16_t const index, uint64_t const word) {
        return m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(type, index, word));
    };
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_TRUE(m.kit.output_streaming(0, MilanKit::raw_now()));
    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, 0, AM824_8CH_48K), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_TRUE(m.kit.talker().slot_for(0)->aaf.has_value());
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    EXPECT_TRUE(m.formats.empty());

    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, 1, AM824_8CH_48K), atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, 1, AM824_8CH_48K), atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, 0, CRF_MILAN_48K), atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 1), CRF_MILAN_48K);
    EXPECT_TRUE(m.formats.empty());
    EXPECT_TRUE(m.kit.talker().slot_for(1)->crf.has_value());
    EXPECT_TRUE(m.kit.listener()->slot_for(0)->kind.load() == StreamKind::aaf);
}

// An input pre-joins the group of the first talker output of its kind: a kind
// change on either side leaves the old group and joins the new one
TEST(avb_entity_kit_kind, the_static_rx_group_follows_the_kind)
{
    RateKit m{{}, 0x76, false, false, /*am824=*/true};
    EXPECT_TRUE(m.kit.stream_port().open_test({.hardware_address = ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x76}}).has_value());
    auto& wire = *m.kit.stream_port().test_port();
    EXPECT_TRUE(m.start_ready());
    auto const group = m.kit.talker().slot_for(0)->dest_mac;
    auto const joined = [&wire, group] { return std::ranges::contains(wire.joined_multicast_groups(), group); };
    auto const set = [&m](uint16_t const type, uint64_t const word) {
        return m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(type, 0, word));
    };
    EXPECT_TRUE(joined());
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, AM824_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(joined());  // no AM824 output
    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, AM824_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(joined());
    EXPECT_EQ(set(DESCRIPTOR_STREAM_OUTPUT, INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(joined());
    EXPECT_EQ(set(DESCRIPTOR_STREAM_INPUT, INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(joined());
}

// A stop forgets the static groups with the port that held them: a port opened again before the
// next start sees no kind change leave or join, and the start joins each group once
TEST(avb_entity_kit_kind, a_port_reopened_before_start_holds_each_static_group_once)
{
    std::vector<net::TestPortContext::MembershipChange> log;  // outlives the port and the fixture
    RateKit m{{}, 0x78, false, false, /*am824=*/true};
    auto const mac = ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x78};
    EXPECT_TRUE(m.kit.stream_port().open_test({.hardware_address = mac}).has_value());
    EXPECT_TRUE(m.start_ready());
    auto const group = m.kit.talker().slot_for(0)->dest_mac;
    EXPECT_TRUE(m.kit.stop().has_value());

    EXPECT_TRUE(m.kit.stream_port().open_test({.hardware_address = mac}).has_value());
    m.kit.stream_port().test_port()->record_memberships(&log);
    auto const set = [&m](uint64_t const word) {
        return m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, word));
    };
    EXPECT_EQ(set(AM824_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(set(INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(log.empty());
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(std::ranges::count(m.kit.stream_port().test_port()->joined_multicast_groups(), group), 1);
}

// A Milan input settled on its own output's pre-joined group holds two memberships of it: the output's kind
// change leaves the static one, the unbind the connection's (one leave per join)
TEST(avb_entity_kit_kind, a_static_group_shared_with_a_connection_is_left_by_each)
{
    std::vector<net::TestPortContext::MembershipChange> log;  // outlives the port and the fixture
    RateKit m{{}, 0x79, false, false, /*am824=*/true};
    EXPECT_TRUE(m.kit.stream_port().open_test({.hardware_address = ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x79}}).has_value());
    m.kit.stream_port().test_port()->record_memberships(&log);
    EXPECT_TRUE(m.start_ready());
    auto const group = m.kit.talker().slot_for(0)->dest_mac;
    auto const held = [&log, group] {
        return std::ranges::count(log, net::TestPortContext::MembershipChange{.group = group, .joined = true}) -
            std::ranges::count(log, net::TestPortContext::MembershipChange{.group = group, .joined = false});
    };
    EXPECT_EQ(held(), 1);  // pre-joined

    std::vector<atdecc::AcmpCommandResponse> sent;
    auto const keep = [&sent](atdecc::AcmpCommandResponse const& pdu) {
        sent.push_back(pdu);
        return true;
    };
    m.kit.host().components().milan_acmp_listener->set_senders(keep, keep);
    auto const receive = [&m](atdecc::AcmpCommandResponse const& pdu) {
        std::array<uint8_t, atdecc::AcmpDu2021::LENGTH> buf{};
        m.kit.host().net_handlers()->atdecc_handler().dispatch_frame(
            MilanKit::steady_ns(), ieee::Eui48{0x02, 0, 0, 0, 0, 0x09}, atdecc::acmp_serialize_2016(pdu, buf));
    };
    atdecc::AcmpCommandResponse bind{};
    bind.init_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);  // BIND_RX
    bind.controller_entity_id = TEST_CONTROLLER;
    bind.talker_entity_id = ieee::Eui64{}.from_uint64(0x0200000000000042ULL);
    bind.listener_entity_id = m.config.entity_id;
    receive(bind);
    auto probe = std::ranges::find_if(
        sent, [](auto const& pdu) { return pdu.message_type() == atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND; });
    EXPECT_TRUE(probe != sent.end());
    if (probe != sent.end()) {
        auto answer = *probe;
        answer.set_message_type(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
        answer.stream_id = ieee::Eui64{}.from_uint64(0x0200000000000777ULL);
        answer.stream_dest_mac = group;
        answer.stream_vlan_id = uint16_t{2};
        receive(answer);
    }
    EXPECT_TRUE(m.kit.host().components().milan_acmp_listener->sink(0)->is_settled());
    EXPECT_EQ(held(), 2);

    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, AM824_8CH_48K)),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(held(), 1);
    bind.set_message_type(atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);  // UNBIND_RX
    receive(bind);
    EXPECT_EQ(held(), 0);
}

// ---- Clock sources and clock domain lock (IEEE 1722.1-2021 Clauses 7.2.32, 7.4.23-24, 7.4.42) ----

namespace {

/// make_blob(1, 1) plus three CLOCK_SOURCEs and two CLOCK_DOMAINs: domain 0 lists sources 0 and 1,
/// domain 1 sources 1 and 2, each at its first.
auto make_clock_domains_blob() -> std::vector<uint8_t>
{
    auto const base = make_blob(1, 1);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{base});
    nanoavb::AemBlobWriter w;
    for (uint16_t const type : {DESCRIPTOR_ENTITY, DESCRIPTOR_CONFIGURATION, DESCRIPTOR_AVB_INTERFACE}) {
        w.add(0, type, 0, *storage->get_descriptor(0, type, 0));
    }
    for (uint16_t const type : {DESCRIPTOR_STREAM_OUTPUT, DESCRIPTOR_STREAM_INPUT}) {
        for (uint16_t index = 0; index < 2; ++index) {
            w.add(0, type, index, *storage->get_descriptor(0, type, index));
        }
    }
    for (uint16_t index = 0; index < 3; ++index) {
        atdecc::aem::DescriptorClockSource source{};
        source.descriptor_index = index;
        w.add(0, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, index, make_const_span(source));
    }
    for (uint16_t index = 0; index < 2; ++index) {
        atdecc::aem::DescriptorClockDomain domain{};
        domain.descriptor_index = index;
        domain.clock_source_index = index;
        (void)domain.push_clock_source(index);
        (void)domain.push_clock_source(static_cast<uint16_t>(index + 1));
        w.add(0, atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, index, wire(domain));
    }
    return w.write();
}

/// The SET/GET_CLOCK_SOURCE body for CLOCK_DOMAIN @p domain and source @p source.
auto clock_source_body(uint16_t const domain, uint16_t const source) -> std::array<uint8_t, 8>
{
    return {
        0x00,
        static_cast<uint8_t>(atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN),
        0x00,
        static_cast<uint8_t>(domain),
        0x00,
        static_cast<uint8_t>(source),
        0x00,
        0x00};
}

/// GET_CLOCK_SOURCE of CLOCK_DOMAIN @p domain: its clock_source_index.
auto get_clock_source(MilanKit& m, uint16_t const domain) -> uint16_t
{
    auto const body = clock_source_body(domain, 0);
    auto const r = m.aem_command(atdecc::AEM_COMMAND_GET_CLOCK_SOURCE, std::span{body}.first(4));
    EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
    return r.body.size() >= 6 ? static_cast<uint16_t>((r.body[4] << 8U) | r.body[5]) : uint16_t{0xFFFF};
}

}  // namespace

// Milan v1.3 Sections 5.4.4.2 and 5.4.4.4: SET_SYSTEM_UNIQUE_ID and SET_MEDIA_CLOCK_REFERENCE_INFO are
// non-volatile: a rebuilt kit answers what the last run was given.
TEST(avb_entity_kit_persistence, mvu_values_survive_a_rebuild)
{
    auto const dir = state_dir("mvu");
    auto const path = kit_state_path(dir, 0x84);
    {
        PersistentKit m{path, 0x84, std::nullopt, make_clock_domains_blob()};
        EXPECT_TRUE(m.start_ready());
        atdecc::MvuSystemUniqueIdPayloadV13 id{};
        id.number = uint64_t{4242};
        EXPECT_EQ(m.mvu_packet(atdecc::MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, make_const_span(id)), atdecc::MVU_STATUS_SUCCESS);
        atdecc::MvuMediaClockReferenceInfoPayload mcr{};
        mcr.flags = atdecc::MVU_MCR_FLAGS_SUPPORTED;
        mcr.user_mcr_prio = uint8_t{210};
        mcr.media_clock_domain_name = atdecc::aem::AtdeccString{"Stage"};
        EXPECT_EQ(
            m.mvu_packet(atdecc::MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, make_const_span(mcr)), atdecc::MVU_STATUS_SUCCESS);
        EXPECT_EQ(load_persistent_state(path, 0, {}).state.media_clock_references.size(), size_t{1});
    }
    {
        PersistentKit m{path, 0x84, std::nullopt, make_clock_domains_blob()};
        EXPECT_TRUE(m.start_ready());
        auto const& aem = m.kit.host().components().aem_handler;
        EXPECT_EQ(aem.system_unique_id(), uint64_t{4242});
        auto const info = aem.media_clock_reference_info(0);
        EXPECT_EQ(info.user_mcr_prio, uint8_t{210});
        EXPECT_TRUE(info.media_clock_domain_name.value == atdecc::aem::AtdeccString{"Stage"}.value);
    }
    std::filesystem::remove_all(dir);
}

// SET_CLOCK_SOURCE on CLOCK_DOMAIN 1 reaches the entity's hook; GET_CLOCK_SOURCE reports it, it is
// notified and saved, and the saved source reaches the hook again at the next start.
TEST(avb_entity_kit_clock_source, set_clock_source_on_domain_one_reaches_the_host)
{
    auto const dir = state_dir("clock-source");
    auto const path = kit_state_path(dir, 0x80);
    std::vector<std::pair<uint16_t, uint16_t>> calls;
    auto const hook = [&calls](uint16_t const domain, uint16_t const source) -> uint8_t {
        calls.emplace_back(domain, source);
        return atdecc::AEM_STATUS_SUCCESS;
    };
    {
        PersistentKit m{path, 0x80, std::nullopt, make_clock_domains_blob()};
        m.kit.set_on_clock_source_changed(hook);
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(calls.empty());
        EXPECT_EQ(get_clock_source(m, 1), uint16_t{1});
        register_other_controller(m);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, clock_source_body(1, 2)), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(calls == (std::vector<std::pair<uint16_t, uint16_t>>{{1, 2}}));
        EXPECT_EQ(m.kit.clock_source(1).value_or(0), uint16_t{2});
        EXPECT_EQ(get_clock_source(m, 1), uint16_t{2});
        EXPECT_EQ(m.kit.active_clock_source(), uint16_t{0});  // domain 0's is unchanged
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_CLOCK_SOURCE).size(), size_t{1});
        // A source the domain does not list never reaches the hook.
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, clock_source_body(1, 0)), atdecc::AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_EQ(calls.size(), size_t{1});
    }
    calls.clear();
    {
        PersistentKit m{path, 0x80, std::nullopt, make_clock_domains_blob()};
        m.kit.set_on_clock_source_changed(hook);
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(calls == (std::vector<std::pair<uint16_t, uint16_t>>{{1, 2}}));
        EXPECT_EQ(m.kit.clock_source(1).value_or(0), uint16_t{2});
        EXPECT_FALSE(m.kit.clock_source(2).has_value());
    }
    std::filesystem::remove_all(dir);
}

// A refused SET_CLOCK_SOURCE answers the hook's status and changes, notifies and saves nothing; on
// domain 0 an accepted one is the active source media_rate() reads.
TEST(avb_entity_kit_clock_source, a_vetoed_clock_source_leaves_the_domain_unchanged)
{
    auto const dir = state_dir("clock-source-veto");
    auto const path = kit_state_path(dir, 0x81);
    PersistentKit m{path, 0x81, std::nullopt, make_clock_domains_blob()};
    m.kit.set_on_clock_source_changed([](uint16_t const domain, uint16_t) -> uint8_t {
        return domain == 1 ? atdecc::AEM_STATUS_NOT_SUPPORTED : atdecc::AEM_STATUS_SUCCESS;
    });
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, clock_source_body(1, 2)), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(get_clock_source(m, 1), uint16_t{1});
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_CLOCK_SOURCE).empty());
    EXPECT_TRUE(m.kit.collect_persistent_state().clock_sources.empty());

    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, clock_source_body(0, 1)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.active_clock_source(), uint16_t{1});
    EXPECT_EQ(m.kit.collect_persistent_state().clock_sources.size(), size_t{1});
    std::filesystem::remove_all(dir);
}

// A source the entity applied itself is reported by GET_CLOCK_SOURCE, notified and saved without
// reaching its hook; a source the domain does not list is refused and changes nothing.
TEST(avb_entity_kit_clock_source, a_host_clock_source_is_notified_without_the_hook)
{
    auto const dir = state_dir("clock-source-host");
    auto const path = kit_state_path(dir, 0x83);
    PersistentKit m{path, 0x83, std::nullopt, make_clock_domains_blob()};
    int calls = 0;
    m.kit.set_on_clock_source_changed([&calls](uint16_t, uint16_t) -> uint8_t {
        ++calls;
        return atdecc::AEM_STATUS_SUCCESS;
    });
    EXPECT_TRUE(m.start_ready());
    register_other_controller(m);
    EXPECT_EQ(m.kit.request_clock_source(1, 2), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(get_clock_source(m, 1), uint16_t{2});
    EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_CLOCK_SOURCE).size(), size_t{1});
    EXPECT_EQ(m.kit.collect_persistent_state().clock_sources.size(), size_t{1});

    EXPECT_EQ(m.kit.request_clock_source(1, 0), atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(get_clock_source(m, 1), uint16_t{2});
    // A source the host has applied is followed while locked under milan13.
    EXPECT_EQ(lock_entity(m), atdecc::AEM_STATUS_SUCCESS);
    m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::milan13);
    EXPECT_EQ(m.kit.request_clock_source(1, 2), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(lock_entity(m, true), atdecc::AEM_STATUS_SUCCESS);
    m.kit.host().components().aem_handler.set_profile(nanoavb::AemCommandHandler::Profile::classic);
    // A controller's change still asks the hook.
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_CLOCK_SOURCE, clock_source_body(1, 1)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(calls, 1);
    std::filesystem::remove_all(dir);
}

// IEEE 1722.1-2021 Clause 7.4.42.2.3: the entity's lock reports drive each CLOCK_DOMAIN's LOCKED and
// UNLOCKED counters, a transition counting once and notified; a domain the model lacks is refused.
TEST(avb_entity_kit_clock_source, clock_domain_counters_follow_the_host_lock)
{
    using atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN;
    MilanKit m{std::nullopt, 0x82, true, make_clock_domains_blob()};
    EXPECT_TRUE(m.start_ready());
    std::array<uint8_t, 0> none{};
    (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
    m.aem.clear();
    EXPECT_EQ(counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 1).counters_valid.get(), 0U);  // not driven yet

    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, false));  // already unlocked: no transition
    auto c = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 1);
    EXPECT_EQ(c.counters_valid.get(), 0x3U);
    EXPECT_EQ(c.counters[0].get(), 0U);
    EXPECT_EQ(c.counters[1].get(), 0U);
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, true));
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, true));
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, false));
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, true));
    c = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 1);
    EXPECT_EQ(c.counters[0].get(), 2U);  // LOCKED
    EXPECT_EQ(c.counters[1].get(), 1U);  // UNLOCKED
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_COUNTERS), 3);

    // Domain 0 is the entity's from its first report.
    auto const before = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0);
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(0, false));
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(0, true));
    auto const after = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0);
    EXPECT_EQ(after.counters[0].get(), before.counters[0].get() + 1);
    EXPECT_TRUE(after.counters[1].get() >= before.counters[1].get());
    EXPECT_FALSE(m.kit.notify_clock_domain_locked(2, true));
}

// Released, CLOCK_DOMAIN 0 follows gPTP again from its lock as it is now; another domain counts as
// unlocked and reports no counters until the entity notes it again.
TEST(avb_entity_kit_clock_source, a_released_clock_domain_hands_its_counters_back)
{
    using atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN;
    MilanKit m{std::nullopt, 0x83, true, make_clock_domains_blob()};
    EXPECT_TRUE(m.start_ready());
    EXPECT_TRUE(m.kit.host().gptp_locked());
    auto const start = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0);
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(0, false));
    m.kit.on_link_down(sm::Clock::now());  // the entity's domain 0: gPTP's loss does not count
    m.kit.on_link_up(sm::Clock::now());
    m.kit.on_gptp_announce(sm::Clock::now(), true);
    auto c = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0);
    EXPECT_EQ(c.counters[0].get(), start.counters[0].get());
    EXPECT_EQ(c.counters[1].get(), start.counters[1].get() + 1);

    EXPECT_TRUE(m.kit.release_clock_domain_lock(0));  // gPTP is locked: one LOCKED
    c = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0);
    EXPECT_EQ(c.counters_valid.get(), 0x3U);
    EXPECT_EQ(c.counters[0].get(), start.counters[0].get() + 1);
    m.kit.on_link_down(sm::Clock::now());  // gPTP's again
    c = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0);
    EXPECT_EQ(c.counters[1].get(), start.counters[1].get() + 2);
    EXPECT_TRUE(m.kit.release_clock_domain_lock(0));  // already gPTP's: no transition
    EXPECT_EQ(counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 0).counters[1].get(), start.counters[1].get() + 2);

    // Domain 1 released while locked counts UNLOCKED, so its next lock counts again.
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, true));
    EXPECT_EQ(counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 1).counters_valid.get(), 0x3U);
    EXPECT_TRUE(m.kit.release_clock_domain_lock(1));
    EXPECT_EQ(counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 1).counters_valid.get(), 0U);
    EXPECT_TRUE(m.kit.notify_clock_domain_locked(1, true));
    c = counters_of(m, DESCRIPTOR_CLOCK_DOMAIN, 1);
    EXPECT_EQ(c.counters[0].get(), 2U);
    EXPECT_EQ(c.counters[1].get(), 1U);
    EXPECT_FALSE(m.kit.release_clock_domain_lock(2));
}

// ---- CRF inputs and the kit's media clock recovery ----

namespace {

/// make_blob(1, 1, 3) (CRF STREAM_INPUTs 1-3) plus CLOCK_SOURCE 0, the INPUT_STREAM at STREAM_INPUT 2.
auto make_crf_inputs_blob() -> std::vector<uint8_t>
{
    auto const base = make_blob(1, 1, 3);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{base});
    nanoavb::AemBlobWriter w;
    for (uint16_t const type : {DESCRIPTOR_ENTITY, DESCRIPTOR_CONFIGURATION, DESCRIPTOR_AVB_INTERFACE}) {
        w.add(0, type, 0, *storage->get_descriptor(0, type, 0));
    }
    for (uint16_t const type : {DESCRIPTOR_STREAM_OUTPUT, DESCRIPTOR_STREAM_INPUT}) {
        for (uint16_t index = 0; index < 4; ++index) {
            w.add(0, type, index, *storage->get_descriptor(0, type, index));
        }
    }
    atdecc::aem::DescriptorClockSource source{};
    source.clock_source_type = atdecc::aem::CLOCK_SOURCE_TYPE_INPUT_STREAM;
    source.clock_source_location_type = DESCRIPTOR_STREAM_INPUT;
    source.clock_source_location_index = 2;
    w.add(0, atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, 0, make_const_span(source));
    return w.write();
}

/// Hand STREAM_INPUT @p index's CRF consumer one timestamp, as the RX path does; false with none bound.
auto feed_crf(AvbEntityKit& kit, uint16_t const index) -> bool
{
    auto* const slot = kit.listener()->slot_for(index);
    if (slot == nullptr || !slot->on_crf) {
        return false;
    }
    slot->on_crf(1'000'000'000ULL, 0, 1'000'000'000, false, 1);
    return true;
}

}  // namespace

// start() binds the kit's recovery to the one CRF input its CRF clock source locates, and to none
// the host bound with set_crf before start.
TEST(avb_entity_kit_crf, the_recovery_follows_the_crf_clock_sources_input_only)
{
    MilanKit m{std::nullopt, 0x90, true, make_crf_inputs_blob(), /*crf_clock_source=*/0};
    EXPECT_TRUE(m.start_ready());
    EXPECT_FALSE(feed_crf(m.kit, 1));
    EXPECT_FALSE(feed_crf(m.kit, 3));
    EXPECT_TRUE(feed_crf(m.kit, 2));
    EXPECT_EQ(m.kit.crf_recovery().estimate().samples, uint64_t{1});
}

TEST(avb_entity_kit_crf, without_a_crf_clock_source_the_recovery_follows_the_first_crf_input)
{
    MilanKit m{std::nullopt, 0x91, true, make_crf_inputs_blob()};
    EXPECT_TRUE(m.start_ready());
    EXPECT_TRUE(feed_crf(m.kit, 1));
    EXPECT_FALSE(feed_crf(m.kit, 2));
    EXPECT_EQ(m.kit.crf_recovery().estimate().samples, uint64_t{1});
}

TEST(avb_entity_kit_crf, a_host_binding_made_before_start_is_kept)
{
    MilanKit m{std::nullopt, 0x92, true, make_crf_inputs_blob(), /*crf_clock_source=*/0};
    std::array<int, 4> host_calls{};
    for (uint16_t index = 1; index < 4; ++index) {
        m.kit.listener()->set_crf(index, [&host_calls, index](uint64_t, uint16_t, int64_t, bool) { ++host_calls[index]; });
    }
    EXPECT_TRUE(m.start_ready());
    for (uint16_t index = 1; index < 4; ++index) {
        EXPECT_TRUE(feed_crf(m.kit, index));
        EXPECT_EQ(host_calls[index], 1);
    }
    EXPECT_EQ(m.kit.crf_recovery().estimate().samples, uint64_t{0});
}

#if defined(__linux__)
namespace {

/// A TcControl that accepts every request (the shaper's qdisc requests are ShaperQdiscs' tests').
class AcceptingTc final : public net::tc::TcControl
{
  public:
    auto replace_mqprio(int, net::tc::MqprioQdisc const&) -> Status override { return success(); }
    auto replace_cbs(int, net::tc::CbsQdisc const&) -> Status override { return success(); }
    auto replace_etf(int, net::tc::EtfQdisc const&) -> Status override { return success(); }
    auto replace_kind(int, uint32_t, std::string_view) -> Status override { return success(); }
    auto remove(int, uint32_t, uint32_t) -> Status override { return success(); }
    auto get(int, uint32_t) -> StatusValue<net::tc::QdiscInfo> override
    {
        return failure(std::make_error_code(std::errc::no_such_file_or_directory));
    }
    [[nodiscard]] auto last_message() const noexcept -> std::string const& override { return message_; }

  private:
    std::string message_{};
};

}  // namespace

// With the gate off every output is admitted; a stopped one may change format (Milan v1.2
// Section 5.4.2.7), and the shaper's class A follows the new TSpec at its next tick.
TEST(avb_entity_kit_formats, the_shaper_follows_an_admitted_streams_format)
{
    RateKit m{{}, 0x72, false, false, false, /*gate=*/false};
    EXPECT_TRUE(m.start_ready());
    AcceptingTc tc{};
    EntityShaper shaper{ShaperConfig{.interface_name = "kit-shaper0"}, m.kit, tc};
    EXPECT_TRUE(shaper.install(ShaperPort{.ifindex = 3, .driver = "igb", .tx_queues = 4, .mtu = 1500}).has_value());
    auto const before = shaper.class_a().idleslope_kbps;
    EXPECT_FALSE(shaper.class_a().placeholder);

    m.kit.talker().set_stream_stopped(0, true);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
    shaper.tick(MilanKit::steady_ns());
    EXPECT_TRUE(shaper.class_a().idleslope_kbps > before);
    shaper.shutdown();
}
#endif

#if defined(__linux__)

namespace {

/// Records each VLAN link request as "add <name>" or "del <name>"; every one succeeds.
class VlanRecorder final : public net::vlan::VlanControl
{
  public:
    std::vector<std::string> requests;

    auto add_vlan(int /*parent_ifindex*/, std::string_view const name, uint16_t /*vid*/)
        -> StatusValue<net::vlan::LinkOwnership> override
    {
        requests.push_back("add " + std::string{name});
        return net::vlan::LinkOwnership::created;
    }
    auto remove_link(std::string_view const name) -> Status override
    {
        requests.push_back("del " + std::string{name});
        return success();
    }
    auto bring_up(std::string_view /*name*/) -> Status override { return success(); }
    [[nodiscard]] auto last_message() const noexcept -> std::string const& override { return message_; }

  private:
    std::string message_{};
};

}  // namespace

// A Milan input settled on VLAN 3 (its talker's probe answer) holds VID 3 on the NIC's filter beside the Class A
// VID 2. When an adopted Domain moves Class A onto 3, the link both use stays and 2's goes; the input's unbind
// leaves 3 to the Class A VID.

// A probe answer naming no VLAN (0) matches the Talker Advertise on any; the input joins the VLAN that
// Advertise is on (3) once it is registered, and leaves it at its unbind.

#endif  // __linux__
TEST_MAIN(statusbar_avb_entity, avb_entity_kit_test)
