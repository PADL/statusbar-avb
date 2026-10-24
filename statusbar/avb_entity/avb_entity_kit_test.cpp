// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit at its stream bound, with no network: MAX_ENTITY_STREAMS outputs and
// inputs (16 AAF 32-channel + CRF each way) all open; one more is refused when
// the tables are derived, and an extra slot by the stream paths themselves.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_test_stream_port.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
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

/// ENTITY + CONFIGURATION + AVB_INTERFACE, @p audio AAF streams then one CRF
/// in each direction.
auto make_blob(uint16_t const audio_outputs, uint16_t const audio_inputs) -> std::vector<uint8_t>
{
    nanoavb::AemBlobWriter w;
    atdecc::aem::DescriptorEntity entity{};
    entity.configurations_count = 1;
    entity.talker_stream_sources = static_cast<uint16_t>(audio_outputs + 1);
    entity.listener_stream_sinks = static_cast<uint16_t>(audio_inputs + 1);
    w.add(0, DESCRIPTOR_ENTITY, 0, make_const_span(entity));
    atdecc::aem::DescriptorConfiguration config{};
    w.add(0, DESCRIPTOR_CONFIGURATION, 0, make_const_span(config));
    for (auto const [type, audio] :
         {std::pair{DESCRIPTOR_STREAM_OUTPUT, audio_outputs}, std::pair{DESCRIPTOR_STREAM_INPUT, audio_inputs}}) {
        for (uint16_t index = 0; index <= audio; ++index) {
            atdecc::aem::DescriptorStream stream{};
            stream.descriptor_type = type;
            stream.descriptor_index = index;
            stream.current_format.from_uint64(index < audio ? AAF_48K_INT24_32CH : CRF_MILAN_48K);
            w.add(0, type, index, wire(stream));
        }
    }
    atdecc::aem::DescriptorAvbInterface iface{};
    w.add(0, DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));
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

/// A multi-rate model: 8 ch INT_24 output and input (+ CRF each) at 48/96 kHz.
/// With @p ports, STREAM_PORT_INPUT 0 has eight 1-channel clusters and no
/// AUDIO_MAP, plus every Milan v1.2 Section 5.3.13 name's descriptor. With
/// @p one_rate the unit lists 48 kHz only and STREAM_PORT_OUTPUT 0 is dynamic.
auto make_multirate_blob(bool const ports = false, bool const one_rate = false) -> std::vector<uint8_t>
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
                if (type == DESCRIPTOR_STREAM_INPUT || channels == 8 || ports) {
                    (void)stream.push_stream_format(ieee::Eui64{}.from_uint64(base_format(rate, channels)));
                }
            }
        }
        w.add(0, type, 0, wire(stream));
        atdecc::aem::DescriptorStream crf{};
        crf.descriptor_type = type;
        crf.descriptor_index = 1;
        crf.current_format.from_uint64(CRF_MILAN_48K);
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
        out_port.number_of_clusters = uint16_t{1};
        out_port.base_cluster = uint16_t{8};
        out_port.number_of_maps = static_cast<uint16_t>(one_rate ? 0 : 1);
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
        if (!one_rate) {
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

}  // namespace

namespace {

struct MilanKit
{
    explicit MilanKit(
        std::optional<bool> const milan, uint64_t const entity_low, bool const gate = true, std::vector<uint8_t> model = make_blob(1, 1))
        : blob{std::move(model)}
        , storage{*atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob})}
        , talkers{*talker_stream_specs(storage, 0)}
        , listeners{*listener_stream_specs(storage, 0)}
        , config{make_config(milan, entity_low, gate)}
        , handler{storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit milan", std::nullopt, /*patch_avb_interface=*/false}
        , kit{config,
              handler,
              talkers,
              listeners,
              48000,
              0,
              std::nullopt,
              media_clock,
              last_gptp_ns,
              nullptr,
              std::pmr::get_default_resource()}
        , reactor{stop, net::monotonic_ns, 0}
    {}
    MilanKit(MilanKit const&) = delete;
    auto operator=(MilanKit const&) -> MilanKit& = delete;
    MilanKit(MilanKit&&) = delete;
    auto operator=(MilanKit&&) -> MilanKit& = delete;
    ~MilanKit() { (void)kit.stop(); }

    static auto make_config(std::optional<bool> const milan, uint64_t const entity_low, bool const gate) -> AvbEntityAudioIOConfig
    {
        AvbEntityAudioIOConfig c{};
        c.gate_talker_on_listener = gate;
        c.interface_name = "kit-milan-test0";  // no such interface: no MAC, no sockets
        c.entity_id = ieee::Eui64{}.from_uint64(0x020000fffe000000ULL | entity_low);
        c.stream_address_mode = "static";
        c.milan = milan;
        return c;
    }

    /// Start, capture what the ACMP talker and the AEM handler send, and bring
    /// the control plane to Ready (link up, gPTP locked).
    auto start_ready() -> bool
    {
        if (!kit.start(reactor)) {
            return false;
        }
        auto& c = kit.host().components();
        c.acmp_talker.set_callbacks(nanoavb::AcmpTalkerCallbacks{.tx_response = [this](atdecc::AcmpCommandResponse const& r) {
            acmp.push_back(r);
            return true;
        }});
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
        (void)tsn::load_unchecked(kit.host().components().acmp_talker.get_stream(idx)->stream_id.span(), &s);
        return s;
    }

    [[nodiscard]] auto advertising(uint16_t const idx) const -> bool
    {
        auto const* t = kit.host().components().msrp_handler.get_talker_stream(sid(idx));
        return t != nullptr && t->state != nanoavb::TalkerReservationState::Idle;
    }

    auto acmp_command(uint8_t const type, uint16_t const idx) -> atdecc::AcmpCommandResponse const&
    {
        atdecc::AcmpCommandResponse cmd{};
        cmd.init_command(type);
        cmd.talker_entity_id = config.entity_id;
        cmd.talker_unique_id = idx;
        cmd.listener_entity_id = ieee::Eui64{}.from_uint64(0x0200001111110001ULL);
        (void)kit.host().components().acmp_talker.receive_command(cmd, sm::Clock::now());
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

    std::vector<uint8_t> blob;
    atdecc::aem::DescriptorStorage storage;
    StreamSpecs talkers;
    StreamSpecs listeners;
    AvbEntityAudioIOConfig config;
    EntityIdentityDescriptorHandler handler;
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<uint64_t> last_gptp_ns{0};
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
    if (body.size() >= atdecc::aem::AEM_STREAM_INFO_MILAN_LENGTH) {
        span_load(ext, std::span<uint8_t const>{body}.subspan(atdecc::aem::AemStreamInfoPayload::LENGTH, 8));
    }
    return {info, ext};
}

}  // namespace

TEST(avb_entity_kit_bounds, a_full_model_derives_its_tables_and_one_more_is_refused)
{
    static_assert(MAX_ENTITY_STREAMS == MAX_ENTITY_AUDIO_STREAMS + MAX_ENTITY_CRF_STREAMS);
    constexpr auto audio = static_cast<uint16_t>(MAX_ENTITY_AUDIO_STREAMS);

    auto const full = make_blob(audio, audio);
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

    // A 17th audio stream (18 descriptors with the CRF) is past the bound.
    auto const outputs_over = make_blob(audio + 1, audio);
    auto const over_out = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{outputs_over});
    EXPECT_TRUE(talker_stream_specs(*over_out, 0).error() == std::errc::result_out_of_range);
    EXPECT_TRUE(listener_stream_specs(*over_out, 0).has_value());
    auto const inputs_over = make_blob(audio, audio + 1);
    auto const over_in = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{inputs_over});
    EXPECT_TRUE(listener_stream_specs(*over_in, 0).error() == std::errc::result_out_of_range);
}

TEST(avb_entity_kit_bounds, every_stream_of_a_full_model_opens)
{
    constexpr auto audio = static_cast<uint16_t>(MAX_ENTITY_AUDIO_STREAMS);
    auto const blob = make_blob(audio, audio);
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
    std::atomic<uint64_t> last_gptp_ns{0};
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
    EXPECT_TRUE(kit.talker().slot_for(audio)->crf.has_value());
    EXPECT_TRUE(
        kit.talker().open_stream(talkers.front(), tsn::StreamId{}, ieee::Eui48{}).error() == std::errc::result_out_of_range);

    // RX: one deserializer slot per STREAM_INPUT.
    EXPECT_TRUE(kit.listener() != nullptr);
    for (auto const& spec : listeners) {
        EXPECT_TRUE(kit.listener()->open_stream(spec).has_value());
    }
    EXPECT_EQ(kit.listener()->slots_.size(), MAX_ENTITY_STREAMS);
    EXPECT_EQ(kit.listener()->slot_for(0)->aaf->channel_count, uint16_t{32});
    EXPECT_TRUE(kit.listener()->open_stream(listeners.front()).error() == std::errc::result_out_of_range);

    // MSRP: a Talker Advertise per talker stream and a Listener Ready per
    // listener stream all fit the participant's tables (MsrpConfig's default
    // 16 refused the 17th).
    auto const now = sm::Clock::now();
    for (auto const& spec : talkers) {
        nanoavb::TalkerStreamSrpInfo info{};
        info.stream_id = stream_id(0x0200000000000100ULL + spec.index);
        info.vlan_id = 2;
        info.max_frame_size = AvbEntityKit::talker_tspec(spec).max_frame_size;
        info.max_interval_frames = 1;
        EXPECT_TRUE(components.msrp_handler.talker_advertise(info, now).has_value());
    }
    for (auto const& spec : listeners) {
        EXPECT_TRUE(components.msrp_handler.listener_ready(stream_id(0x0200000000000200ULL + spec.index), now).has_value());
    }
    for (auto const& spec : talkers) {
        EXPECT_TRUE(components.msrp_handler.registered_talker_advertise(stream_id(0x0200000000000100ULL + spec.index)) != nullptr);
    }

    // AEM: every STREAM_INPUT and STREAM_OUTPUT latches its own runtime state
    // (34 entries in a table both directions share); the outputs' STOP
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

TEST_MAIN(statusbar_avb_entity, avb_entity_kit_test)

// Talker admission through the kit, driven by MSRP listener events and grace
// expiry with no ACMP connection; the callback fires on every transition.
TEST(avb_entity_kit_admission, follows_msrp_listener_ready_and_grace_expiry)
{
    // A classic kit at Ready declares both streams (a stream is admitted only
    // while declaring its Talker Advertise).
    MilanKit m{false, 0x02};
    EXPECT_TRUE(m.config.gate_talker_on_listener);
    EXPECT_TRUE(m.start_ready());
    auto& kit = m.kit;
    EXPECT_TRUE(kit.talker_declared(0) && kit.talker_declared(1));
    auto& components = kit.host().components();
    auto const* aaf = components.acmp_talker.get_stream(0);
    auto const* crf = components.acmp_talker.get_stream(1);
    EXPECT_TRUE(aaf != nullptr && crf != nullptr);
    if (aaf == nullptr || crf == nullptr) {
        return;
    }
    nanoavb::StreamId aaf_sid{};
    aaf_sid.from_uint64(aaf->stream_id.to_uint64());
    nanoavb::StreamId crf_sid{};
    crf_sid.from_uint64(crf->stream_id.to_uint64());
    EXPECT_EQ(components.acmp_talker.connection_count(0), size_t{0});

    std::vector<std::vector<uint16_t>> reports;
    kit.set_on_admission_changed(
        [r = &reports](std::span<uint16_t const> admitted) { r->emplace_back(admitted.begin(), admitted.end()); });
    using V = std::vector<uint16_t>;
    constexpr int64_t t0 = 1'000'000'000;
    constexpr int64_t grace = TalkerGate::GRACE_NS;

    EXPECT_TRUE(kit.admitted_talker_streams(t0).empty());
    kit.poll_admission(t0);
    EXPECT_TRUE(reports.empty());  // no change, no report

    kit.note_listener_ready(aaf_sid, true, t0);
    EXPECT_EQ(reports.size(), size_t{1});
    EXPECT_TRUE(reports.back() == V{0});
    kit.note_listener_ready(crf_sid, true, t0);
    EXPECT_TRUE(reports.back() == (V{0, 1}));
    EXPECT_EQ(reports.size(), size_t{2});

    // AAF's Ready withdrawn: still admitted through the grace window (no report),
    // then the poll past it reports the drop -- no MSRP event needed.
    kit.note_listener_ready(aaf_sid, false, t0 + 10);
    EXPECT_EQ(reports.size(), size_t{2});
    kit.poll_admission(t0 + 10 + grace - 1);
    EXPECT_EQ(reports.size(), size_t{2});
    kit.poll_admission(t0 + 10 + grace);
    EXPECT_EQ(reports.size(), size_t{3});
    EXPECT_TRUE(reports.back() == V{1});
    kit.poll_admission(t0 + 10 * grace);
    EXPECT_EQ(reports.size(), size_t{3});

    // Re-registered: back in.
    kit.note_listener_ready(aaf_sid, true, t0 + 11 * grace);
    EXPECT_TRUE(reports.back() == (V{0, 1}));
    EXPECT_EQ(reports.size(), size_t{4});
    kit.set_on_admission_changed({});
}

// Milan v1.2 Section 4.2.7.2.1: an adopted Class A VID reaches the MVRP join,
// ACMP stream_vlan_id and the frames' tag; a link up restores the configured VID.
TEST(avb_entity_kit_domain, adopted_class_a_vid_reaches_mvrp_acmp_and_tx)
{
    auto const blob = make_blob(1, 1);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob});
    EXPECT_TRUE(storage.has_value());
    auto const talkers = *talker_stream_specs(*storage, 0);
    auto const listeners = *listener_stream_specs(*storage, 0);

    AvbEntityAudioIOConfig config{};
    config.interface_name = "kit-domain-test0";  // no such interface: no MAC, no sockets
    config.entity_id = ieee::Eui64{0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x03};
    config.stream_address_mode = "static";
    EntityIdentityDescriptorHandler handler{
        *storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit domain", std::nullopt, /*patch_avb_interface=*/false};
    ptpclient::MediaClockGenerator media_clock{};
    std::atomic<uint64_t> last_gptp_ns{0};
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
    EXPECT_EQ(components.acmp_talker.get_stream(0)->stream_vlan_id, uint16_t{2});
    EXPECT_EQ(kit.talker().vlan_id(), uint16_t{2});

    // The bridge port: a bare participant declaring Class A on VID 7.
    std::vector<std::vector<uint8_t>> bridge_out;
    srp::msrp::MsrpParticipant bridge{srp::msrp::MsrpConfig{}, 0xb1d9e};
    bridge.set_send_pdu([&bridge_out](std::span<uint8_t const> p) {
        bridge_out.emplace_back(p.begin(), p.end());
        return true;
    });
    auto now = sm::Clock::now();
    bridge.start(now);
    srp::msrp::DomainFirstValue class_a{};
    class_a.sr_class_vid = 7;
    (void)bridge.declare_domain(class_a, now);
    for (int i = 0; i < 4; ++i) {
        now += std::chrono::milliseconds(250);
        bridge.tick(now);
        for (auto const& p : bridge_out) {
            components.msrp_handler.receive_packet(p, now);
        }
        bridge_out.clear();
    }
    EXPECT_EQ(components.msrp_handler.domain().sr_class_vid, uint16_t{7});
    EXPECT_EQ(components.acmp_talker.get_stream(0)->stream_vlan_id, uint16_t{7});
    EXPECT_EQ(components.acmp_talker.get_stream(1)->stream_vlan_id, uint16_t{7});
    EXPECT_EQ(kit.talker().vlan_id(), uint16_t{7});
    EXPECT_TRUE(components.mvrp_handler.get_vlan_state(7) != nanoavb::VlanState::Unregistered);
    EXPECT_TRUE(components.mvrp_handler.get_vlan_state(2) == nanoavb::VlanState::Unregistered);

    kit.on_link_up(now);
    EXPECT_EQ(components.msrp_handler.domain().sr_class_vid, uint16_t{2});
    EXPECT_EQ(components.acmp_talker.get_stream(0)->stream_vlan_id, uint16_t{2});
    EXPECT_EQ(kit.talker().vlan_id(), uint16_t{2});
    EXPECT_TRUE(components.mvrp_handler.get_vlan_state(2) != nanoavb::VlanState::Unregistered);
}

// The kit's `milan` setting reaches the entity profile every handler reads
// (nanoavb::EntityProfile): unset takes the model's (this test model says
// nothing: Milan), false runs classic IEEE 1722.1.
TEST(avb_entity_kit_profile, config_milan_overrides_the_model)
{
    auto const blob = make_blob(1, 1);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob});
    EXPECT_TRUE(storage.has_value());
    auto const talkers = *talker_stream_specs(*storage, 0);
    auto const listeners = *listener_stream_specs(*storage, 0);
    for (auto const setting : {std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true}}) {
        AvbEntityAudioIOConfig config{};
        config.interface_name = "kit-profile-test0";
        config.entity_id = ieee::Eui64{0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x04};
        config.stream_address_mode = "static";
        config.milan = setting;
        EntityIdentityDescriptorHandler handler{
            *storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit profile", std::nullopt, /*patch_avb_interface=*/false};
        ptpclient::MediaClockGenerator media_clock{};
        std::atomic<uint64_t> last_gptp_ns{0};
        AvbEntityKit kit{
            config,
            handler,
            talkers,
            listeners,
            48000,
            0,
            std::nullopt,
            media_clock,
            last_gptp_ns,
            nullptr,
            std::pmr::get_default_resource()};
        bool const expected = setting.value_or(true);
        EXPECT_EQ(kit.host().components().profile.milan, expected);
        EXPECT_EQ(kit.host().components().aem_handler.profile().milan, expected);
        EXPECT_EQ(kit.host().components().mvu_handler.profile().milan, expected);
    }
}

// The Milan talker through the kit on a nonexistent interface (sends fail
// quietly): declarations follow probes (Milan v1.2 Section 4.3.3.1), and the
// presentation offset goes only through the entity's hook, never while streaming.

TEST(avb_entity_kit_milan_talker, declares_after_a_probe_and_withdraws_after_15_s)
{
    MilanKit m{std::nullopt, 0x11};
    EXPECT_TRUE(m.start_ready());
    EXPECT_TRUE(m.kit.host().components().acmp_talker.milan());

    // Ready, address valid, nobody asked: no Talker Advertise.
    EXPECT_FALSE(m.kit.talker_declared(0));
    EXPECT_FALSE(m.advertising(0));

    // A PROBE_TX: answered SUCCESS with connection_count 0, and declared.
    auto const& probe = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    EXPECT_EQ(probe.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(probe.connection_count.get(), uint16_t{0});
    EXPECT_EQ(probe.stream_id.to_uint64(), m.sid(0).to_uint64());
    EXPECT_TRUE(m.kit.talker_declared(0));
    EXPECT_TRUE(m.advertising(0));
    EXPECT_FALSE(m.kit.talker_declared(1));  // streams are independent
    EXPECT_EQ(m.kit.host().components().acmp_talker.connection_count(0), size_t{0});

    // GET_STREAM_INFO: the Milan 56-octet form; declaring, no listener yet.
    auto const info = m.stream_info(0);
    EXPECT_EQ(info.status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(info.body.size(), atdecc::aem::AEM_STREAM_INFO_MILAN_LENGTH);
    auto const [payload, ext] = load_info(info.body);
    namespace f = atdecc::aem::stream_info_flags;
    uint32_t const flags = payload.flags.get();
    EXPECT_TRUE((flags & f::STREAM_FORMAT_VALID) != 0 && (flags & f::STREAM_ID_VALID) != 0);
    EXPECT_TRUE((flags & f::MSRP_ACC_LAT_VALID) != 0 && (flags & f::STREAM_VLAN_ID_VALID) != 0);
    EXPECT_EQ(flags & (f::BOUND | f::FAST_CONNECT | f::SAVED_STATE | f::STREAMING_WAIT | f::REGISTERING_FAILED), 0U);
    EXPECT_EQ(payload.msrp_accumulated_latency.get(), 2'000'000U);  // Milan's default offset
    EXPECT_EQ(ext.flags_ex.get(), 0U);
    EXPECT_EQ(ext.probing_status(), atdecc::aem::probing_status::DISABLED);

    // The window closes 15 s after the probe with no Listener: withdrawn.
    int64_t const now = MilanKit::steady_ns();
    m.kit.update_talker_declarations(now + 5 * int64_t{1'000'000'000});
    EXPECT_TRUE(m.kit.talker_declared(0));
    m.kit.update_talker_declarations(now + TalkerDeclarations::PROBE_WINDOW_NS + 1);
    EXPECT_FALSE(m.kit.talker_declared(0));
    EXPECT_FALSE(m.advertising(0));
}

TEST(avb_entity_kit_milan_talker, a_registered_listener_keeps_it_and_reports_registering)
{
    MilanKit m{std::nullopt, 0x12};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    EXPECT_TRUE(m.kit.talker_declared(0));

    // A Listener Asking Failed: REGISTERING and REGISTERING_FAILED; not admitted.
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::AskingFailed);
    auto const& state = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 0);
    EXPECT_EQ(state.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(state.flags.get(), uint16_t{atdecc::acmp_flags::REGISTERING_FAILED});
    EXPECT_EQ(state.connection_count.get(), uint16_t{0});
    auto [info, ext] = load_info(m.stream_info(0).body);
    EXPECT_TRUE((info.flags.get() & atdecc::aem::stream_info_flags::REGISTERING_FAILED) != 0);
    EXPECT_EQ(ext.flags_ex.get(), atdecc::aem::stream_info_flags_ex::REGISTERING);
    EXPECT_TRUE(m.kit.admitted_talker_streams().empty());

    // Listener Ready: admitted, REGISTERING without the failure.
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    std::tie(info, ext) = load_info(m.stream_info(0).body);
    EXPECT_EQ(info.flags.get() & atdecc::aem::stream_info_flags::REGISTERING_FAILED, 0U);
    EXPECT_EQ(ext.flags_ex.get(), atdecc::aem::stream_info_flags_ex::REGISTERING);
    EXPECT_EQ(m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 0).flags.get(), uint16_t{0});
    auto const admitted = m.kit.admitted_talker_streams();
    EXPECT_TRUE(admitted.size() == 1 && admitted.front() == 0);

    // Long after the probe window the registered Listener keeps it declared.
    m.kit.update_talker_declarations(MilanKit::steady_ns() + 10 * TalkerDeclarations::PROBE_WINDOW_NS);
    EXPECT_TRUE(m.kit.talker_declared(0));

    // Streaming: the presentation offset cannot change.
    m.kit.set_on_presentation_offset_changed([](uint16_t, uint32_t) -> uint8_t { return atdecc::AEM_STATUS_SUCCESS; });
    atdecc::aem::AemStreamInfoPayload set{};
    set.descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
    set.descriptor_index = uint16_t{0};
    set.flags = atdecc::aem::stream_info_flags::MSRP_ACC_LAT_VALID;
    set.msrp_accumulated_latency = uint32_t{3'000'000};
    std::array<uint8_t, atdecc::aem::AemStreamInfoPayload::LENGTH> body{};
    span_store(body, set);
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_SET_STREAM_INFO, body).status, atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(m.kit.presentation_offset_ns(0), 2'000'000U);
}

TEST(avb_entity_kit_milan_talker, presentation_offset_through_the_entity)
{
    MilanKit m{std::nullopt, 0x13};
    EXPECT_TRUE(m.start_ready());
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

TEST(avb_entity_kit_milan_talker, classic_declares_at_ready_and_counts_connect_tx)
{
    MilanKit m{false, 0x14};
    EXPECT_TRUE(m.start_ready());
    EXPECT_FALSE(m.kit.host().components().acmp_talker.milan());
    // A classic talker declares every stream once the address is valid.
    EXPECT_TRUE(m.kit.talker_declared(0) && m.kit.talker_declared(1));
    EXPECT_TRUE(m.advertising(0));
    auto const& connect = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    EXPECT_EQ(connect.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(connect.connection_count.get(), uint16_t{1});
    // GET_STREAM_INFO stays IEEE 1722.1-2013's 48 octets, CONNECTED from ACMP.
    auto const info = m.stream_info(0);
    EXPECT_EQ(info.body.size(), atdecc::aem::AemStreamInfoPayload::LENGTH);
    EXPECT_TRUE((load_info(info.body).first.flags.get() & atdecc::aem::stream_info_flags::CONNECTED) != 0);
    // No probe window to run out.
    m.kit.update_talker_declarations(MilanKit::steady_ns() + 10 * TalkerDeclarations::PROBE_WINDOW_NS);
    EXPECT_TRUE(m.kit.talker_declared(0));
}

// A probe or connect before MAAP has defended the block is refused
// TALKER_DEST_MAC_FAIL with no address; once defended, it gets the MAAP address.
TEST(avb_entity_kit_milan_talker, probe_or_connect_before_maap_gets_dest_mac_fail)
{
    ieee::Eui48 const maap_mac{0x91, 0xE0, 0xF0, 0x00, 0x83, 0x7E};
    for (bool const milan : {true, false}) {
        MilanKit m{milan, milan ? 0x15U : 0x16U};
        EXPECT_TRUE(m.start_ready());
        auto& talker = m.kit.host().components().acmp_talker;
        int64_t const now = MilanKit::steady_ns();
        // MAAP acquiring (as on a restart, or after a conflict): no address.
        for (uint16_t idx = 0; idx < 2; ++idx) {
            auto const* s = talker.get_stream(idx);
            (void)talker.configure_stream(idx, s->stream_id, ieee::Eui48{}, s->stream_vlan_id);
        }
        m.kit.note_talker_addresses(false, now);
        EXPECT_FALSE(m.kit.talker_declared(0));

        auto const& refused = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
        EXPECT_EQ(refused.status(), atdecc::ACMP_STATUS_TALKER_DEST_MAC_FAIL);
        EXPECT_FALSE(m.kit.talker_declared(0));
        EXPECT_EQ(talker.connection_count(0), size_t{0});
        auto const state = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 0);
        EXPECT_EQ(state.stream_dest_mac, ieee::Eui48{});
        // GET_STREAM_INFO claims no destination address (either profile).
        EXPECT_EQ(load_info(m.stream_info(0).body).first.flags.get() & atdecc::aem::stream_info_flags::STREAM_DEST_MAC_VALID, 0U);

        // MAAP defends the block: the stream's address, then the callback.
        auto const* s0 = talker.get_stream(0);
        (void)talker.configure_stream(0, s0->stream_id, maap_mac, s0->stream_vlan_id);
        m.kit.note_talker_addresses(true, now + 1);
        auto const& accepted = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
        EXPECT_EQ(accepted.status(), atdecc::ACMP_STATUS_SUCCESS);
        EXPECT_EQ(accepted.stream_dest_mac, maap_mac);
        EXPECT_EQ(talker.connection_count(0), size_t{milan ? 0U : 1U});
        EXPECT_TRUE(m.kit.talker_declared(0));
        auto const* advertised = m.kit.host().components().msrp_handler.get_talker_stream(m.sid(0));
        EXPECT_TRUE(advertised != nullptr && advertised->dest_address == maap_mac);
    }
}

// A Listener still registered for a withdrawn stream (in its re-declare hold,
// or after its probe window closed) opens no transmit gate: no Talker
// Advertise, no streaming (Milan v1.2 Section 5.3.7.3).
TEST(avb_entity_kit_milan_talker, no_gate_for_an_undeclared_stream)
{
    MilanKit m{std::nullopt, 0x17};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    int64_t const now = MilanKit::steady_ns();
    m.kit.note_listener_ready(m.sid(0), true, now);
    EXPECT_TRUE(m.kit.admitted_talker_streams(now).size() == 1);
    // A MAAP conflict withdraws it; the Listener's next registration event
    // must not reopen the gate.
    m.kit.note_talker_addresses(false, now + 1);
    EXPECT_FALSE(m.kit.talker_declared(0));
    m.kit.note_listener_ready(m.sid(0), true, now + 2);
    EXPECT_TRUE(m.kit.admitted_talker_streams(now + 2).empty());
}

// With the transmit gate disabled (bench debugging: every stream transmits)
// the presentation offset is still settable while no Listener is registered:
// "streaming" is Milan's (declaring and a Listener Ready registered).
TEST(avb_entity_kit_milan_talker, offset_settable_with_the_gate_disabled)
{
    MilanKit m{std::nullopt, 0x18, /*gate=*/false};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    EXPECT_TRUE(m.kit.talker_declared(0));
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

// Past the probe window a Listener that blips out (an MRP LeaveAll
// re-registration) keeps the stream declared through the gate's grace; one
// gone longer withdraws it.
TEST(avb_entity_kit_milan_talker, a_listener_blip_keeps_the_declaration)
{
    MilanKit m{std::nullopt, 0x19};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    m.bridge_withdraw(0);
    int64_t const gone = MilanKit::steady_ns();
    // The probe window is over (15 s on), the Listener gone less than the grace.
    m.kit.update_talker_declarations(gone + TalkerDeclarations::PROBE_WINDOW_NS - 1'000'000'000);
    EXPECT_TRUE(m.kit.talker_declared(0));
    m.kit.update_talker_declarations(gone + TalkerDeclarations::PROBE_WINDOW_NS + 1);
    EXPECT_FALSE(m.kit.talker_declared(0));
}

// stop() withdraws: undeclared, inactive, every gate shut; a restart declares
// again (while Ready) as the predicate says.
TEST(avb_entity_kit_milan_talker, stop_withdraws_and_a_restart_declares_again)
{
    MilanKit m{std::nullopt, 0x1A};
    EXPECT_TRUE(m.start_ready());
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    m.kit.note_listener_ready(m.sid(0), true, MilanKit::steady_ns());
    EXPECT_TRUE(m.kit.talker_declared(0));
    EXPECT_FALSE(m.kit.admitted_talker_streams().empty());

    EXPECT_TRUE(m.kit.stop().has_value());
    EXPECT_FALSE(m.kit.talker_declared(0));
    EXPECT_FALSE(m.kit.talker_declarations().active());
    EXPECT_TRUE(m.kit.admitted_talker_streams().empty());
    EXPECT_FALSE(m.advertising(0));

    // Restarted while the control plane stayed Ready: the probe of a moment
    // ago is still within its 15 s window, so the stream declares again.
    EXPECT_TRUE(m.start_ready());
    EXPECT_TRUE(m.kit.talker_declared(0));
    EXPECT_TRUE(m.kit.admitted_talker_streams().empty());  // no Listener seen since
    auto const& probe = m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    EXPECT_EQ(probe.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_TRUE(m.kit.talker_declared(0));
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

/// A kit keeping its persistent state at @p path, with a capture of what its
/// MVU handler sends
struct PersistentKit : MilanKit
{
    explicit PersistentKit(
        std::string const& path,
        uint64_t const entity_low = 0x30,
        std::optional<bool> const milan = std::nullopt,
        std::vector<uint8_t> model = make_blob(1, 1))
        : MilanKit{milan, entity_low, true, std::move(model)}
    {
        config.persistent_state_path = path;
        kit.host().components().mvu_handler.set_clock_domain_count(1);  // the test model has none
    }

    /// An AEM command through process_packet (as from the network: the
    /// settings hook runs after the response)
    auto aem_packet(uint16_t const command, std::span<uint8_t const> const body) -> uint8_t
    {
        nanoavb::AemDu header{};
        header.init_command(command, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + body.size()));
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
        atdecc::mvu::MvuDu header{};
        header.init_command(command, static_cast<uint16_t>(body.size()));
        header.common.target_entity_id = config.entity_id;
        std::vector<uint8_t> frame(atdecc::mvu::MvuDu::LENGTH + body.size());
        span_store(std::span<uint8_t>{frame}.first(atdecc::mvu::MvuDu::LENGTH), header);
        std::ranges::copy(body, frame.begin() + atdecc::mvu::MvuDu::LENGTH);
        std::vector<uint8_t> response;
        kit.host().components().mvu_handler.set_send_response([&response](ieee::Eui48 const&, std::span<uint8_t const> bytes) {
            response.assign(bytes.begin(), bytes.end());
            return true;
        });
        (void)kit.host().components().mvu_handler.process_packet(CONTROLLER_MAC, frame, config.entity_id);
        atdecc::mvu::MvuDu reply{};
        span_load(reply, std::span<uint8_t const>{response}.first(atdecc::mvu::MvuDu::LENGTH));
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
    auto const path = (dir / "avb-state.toml").string();
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
    auto const path = (dir / "avb-state.toml").string();
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

// The entity renamed by other than ATDECC (the bridge's control socket): as
// a controller's SET_NAME, saved and told to every controller; refused while
// a controller has the entity locked; an empty name is the configured one
TEST(avb_entity_kit_persistence, request_entity_name_is_a_local_set_name)
{
    auto const dir = state_dir("request-name");
    auto const path = (dir / "avb-state.toml").string();
    PersistentKit m{path};
    std::vector<std::string> told;
    m.handler.set_on_name_changed(
        [&told](uint16_t, uint16_t, atdecc::aem::AtdeccString const& name) { told.emplace_back(name.as_string_view()); });
    EXPECT_TRUE(m.start_ready());
    m.aem.clear();
    EXPECT_EQ(m.kit.request_entity_name("Stage bridge"), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.entity_name() == "Stage bridge");
    EXPECT_TRUE(told.size() == 1 && told[0] == "Stage bridge");
    EXPECT_EQ(load_persistent_state(path, 0, {}).state.names.size(), size_t{1});
    EXPECT_EQ(m.kit.request_entity_name(std::string(65, 'x')), atdecc::AEM_STATUS_BAD_ARGUMENTS);
    std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_LOCK_ENTITY, lock).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.kit.request_entity_name("Other"), atdecc::AEM_STATUS_ENTITY_LOCKED);
    lock[3] = 0x01;  // UNLOCK
    EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_LOCK_ENTITY, lock).status, atdecc::AEM_STATUS_SUCCESS);
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
    auto const path = (dir / "avb-state.toml").string();
    std::vector<std::pair<uint16_t, uint32_t>> applied;
    auto const hook = [&applied](uint16_t const idx, uint32_t const ns) -> uint8_t {
        applied.emplace_back(idx, ns);
        return atdecc::AEM_STATUS_SUCCESS;
    };
    {
        PersistentKit m{path};
        m.kit.set_on_presentation_offset_changed(hook);
        EXPECT_TRUE(m.start_ready());
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

// SET_SYSTEM_UNIQUE_ID and SET_MEDIA_CLOCK_REFERENCE_INFO are saved and
// restored
TEST(avb_entity_kit_persistence, mvu_values_survive_a_rebuild)
{
    auto const dir = state_dir("mvu");
    auto const path = (dir / "avb-state.toml").string();
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        atdecc::mvu::MvuSystemUniqueIdPayload id{};
        id.system_unique_id = uint32_t{4242};
        EXPECT_EQ(m.mvu_packet(atdecc::mvu::MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, make_const_span(id)), atdecc::mvu::MVU_STATUS_SUCCESS);
        atdecc::mvu::MvuMediaClockReferenceInfoPayload mcr{};
        mcr.flags = atdecc::mvu::MVU_MCR_FLAGS_SUPPORTED;
        mcr.user_mcr_prio = uint8_t{210};
        mcr.media_clock_domain_name = nanoavb::mcr_domain_name("Stage");
        EXPECT_EQ(
            m.mvu_packet(atdecc::mvu::MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, make_const_span(mcr)),
            atdecc::mvu::MVU_STATUS_SUCCESS);
    }
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        auto const& mvu = m.kit.host().components().mvu_handler;
        EXPECT_EQ(mvu.system_unique_id(), uint32_t{4242});
        EXPECT_EQ(mvu.mcr_info(0)->user_mcr_prio, uint8_t{210});
        EXPECT_TRUE(mvu.mcr_info(0)->domain_name == nanoavb::mcr_domain_name("Stage"));
    }
    std::filesystem::remove_all(dir);
}

// No state file yet: the listener bindings file's bindings are imported and
// armed as fast-connect goals (the classic listener), and the state file is
// written with them; the old file is no longer written
TEST(avb_entity_kit_persistence, listener_bindings_imported)
{
    auto const dir = state_dir("bindings");
    auto const path = (dir / "avb-state.toml").string();
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
    auto const path = (dir / "avb-state.toml").string();
    PersistentState other{};
    other.entity_model_id = 0x99;
    other.configured_entity_name = "kit milan";
    other.names.push_back({.descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0, .name_index = 0, .value = "Stale"});
    EXPECT_TRUE(save_persistent_state(path, other).has_value());
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
    EXPECT_TRUE(save_persistent_state(path, other).has_value());
    {
        PersistentKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.entity_name() == "kit milan");
    }
    std::filesystem::remove_all(dir);
}

// Without the entity's hooks only the stream's own format and the kit's rate
// are taken, so nothing else is saved
TEST(avb_entity_kit_persistence, formats_and_rates_follow_the_data_plane)
{
    auto const dir = state_dir("formats");
    auto const path = (dir / "avb-state.toml").string();
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
        static_cast<uint8_t>(type >> 8U), static_cast<uint8_t>(type), static_cast<uint8_t>(index >> 8U), static_cast<uint8_t>(index)};
    span_store(std::span{body}.subspan(4, 8), ieee::Eui64{}.from_uint64(format));
    return body;
}

/// SET_SAMPLING_RATE's body: AUDIO_UNIT 0, the rate.
auto rate_body(uint32_t const rate) -> std::array<uint8_t, 8>
{
    return {0x00,
            0x02,
            0x00,
            0x00,
            static_cast<uint8_t>(rate >> 24U),
            static_cast<uint8_t>(rate >> 16U),
            static_cast<uint8_t>(rate >> 8U),
            static_cast<uint8_t>(rate)};
}

/// The multi-rate model's kit, with hooks that record what the entity was
/// asked (the rate hook applies the rate, as an entity re-clocking would).
struct RateKit : PersistentKit
{
    explicit RateKit(
        std::string const& path = {}, uint64_t const entity_low = 0x70, bool const ports = false, bool const one_rate = false)
        : PersistentKit{path, entity_low, std::nullopt, make_multirate_blob(ports || one_rate, one_rate)}
    {
        kit.set_on_stream_format_changed([this](uint16_t const type, uint16_t const index, StreamFormatFields const& format) {
            formats.push_back({type, index, format});
            return format_status;
        });
        kit.set_on_sampling_rate_changed([this](uint32_t const rate) {
            rates.push_back(rate);
            if (rate_status == atdecc::AEM_STATUS_SUCCESS) {
                kit.apply_sampling_rate(rate);
            }
            return rate_status;
        });
    }

    /// GET_STREAM_FORMAT of a stream
    auto current_format(uint16_t const type, uint16_t const index) -> uint64_t
    {
        std::array<uint8_t, 4> body{
            static_cast<uint8_t>(type >> 8U), static_cast<uint8_t>(type), static_cast<uint8_t>(index >> 8U), static_cast<uint8_t>(index)};
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
    std::vector<uint32_t> rates;
    uint8_t format_status{atdecc::AEM_STATUS_SUCCESS};
    uint8_t rate_status{atdecc::AEM_STATUS_SUCCESS};
};

/// The unsolicited AEM responses in @p frames with command @p command.
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

// SET_STREAM_FORMAT takes a listed format of the stream's kind at the kit's
// rate, through the entity's hook, and re-shapes the stream (spec, TX or RX
// slot); refused while the input is bound or the output streams
TEST(avb_entity_kit_formats, set_stream_format_reshapes_the_stream)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());

    // An input takes a 2-channel Base format: the entity is told, the RX slot
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

    // An output takes the 8-channel Base format: the TX slot serializes it,
    // and the TSpec follows.
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(m.kit.talker().slot_for(0)->aaf->format == avtp::AafFormat::int_32bit);
    EXPECT_EQ(AvbEntityKit::talker_tspec(m.kit.talker_specs().front()).max_frame_size, uint16_t{6 * 8 * 4 + 24 + 1});

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
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    EXPECT_TRUE(m.kit.output_streaming(0, MilanKit::steady_ns()));
    size_t const asked = m.formats.size();
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_48K)),
        atdecc::AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(m.formats.size(), asked);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), base_format(48000, 8));
}

// A bound input keeps its format (and the rate with it); a saved format is
// restored at the next start through the entity's hook
TEST(avb_entity_kit_formats, bound_inputs_and_saved_formats)
{
    auto const dir = state_dir("stage8-formats");
    auto const path = (dir / "avb-state.toml").string();
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 6))),
            atdecc::AEM_STATUS_SUCCESS);
        auto const saved = load_persistent_state(path, 0, {});
        EXPECT_EQ(saved.state.stream_formats.size(), size_t{1});
    }
    PersistentState state = load_persistent_state(path, 0, {}).state;
    state.listener_bindings.push_back(
        {.stream_input = 0, .talker_entity_id = 0x0200001111110001ULL, .talker_unique_id = 0, .format = base_format(48000, 6)});
    EXPECT_TRUE(save_persistent_state(path, state).has_value());
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.formats.size(), size_t{1});  // the saved format, through the hook
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 6));
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.channels, uint16_t{6});
        EXPECT_TRUE(m.kit.input_running(0));  // the saved binding
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 8))),
            atdecc::AEM_STATUS_STREAM_IS_RUNNING);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
        EXPECT_TRUE(m.rates.empty());
    }
    std::filesystem::remove_all(dir);
}

// SET_SAMPLING_RATE re-times every AAF stream through the entity's hook (the
// CRF keeps its format), notifies the re-timed formats after the response,
// and is restored, rate before formats, at the next start
TEST(avb_entity_kit_formats, set_sampling_rate_retimes_every_stream)
{
    auto const dir = state_dir("stage8-rate");
    auto const path = (dir / "avb-state.toml").string();
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        std::array<uint8_t, 0> none{};
        (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
        EXPECT_EQ(
            m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 2))),
            atdecc::AEM_STATUS_SUCCESS);

        // An unlisted rate, then the entity refusing: nothing changes.
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(192000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
        m.rate_status = atdecc::AEM_STATUS_NO_RESOURCES;
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NO_RESOURCES);
        EXPECT_EQ(m.kit.sample_rate(), 48000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
        m.rate_status = atdecc::AEM_STATUS_SUCCESS;

        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_TRUE(m.rates.size() == 2 && m.rates.back() == 96000U);
        EXPECT_EQ(m.kit.sample_rate(), 96000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(96000, 2));
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 1), CRF_MILAN_48K);
        EXPECT_EQ(m.kit.talker().slot_for(0)->spec.format.aaf_samples_per_frame, uint16_t{12});
        EXPECT_EQ(m.kit.talker().samples_per_packet_, 12U);
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 96000U);
        EXPECT_EQ(AvbEntityKit::talker_tspec(m.kit.talker_specs().front()).max_frame_size, uint16_t{12 * 8 * 3 + 24 + 1});
        // The re-timed formats are notified once the reactor pass is done,
        // to every registered controller (the requester too), as
        // SET_STREAM_FORMATs.
        EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).empty());
        m.aem.clear();
        m.kit.on_declaration_timer(MilanKit::steady_ns());
        auto const notified = unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT);
        EXPECT_EQ(notified.size(), size_t{2});
        if (notified.size() == 2) {
            ieee::Eui64 format{};
            span_load(format, std::span<uint8_t const>{notified[0]}.subspan(4, 8));
            EXPECT_EQ(format.to_uint64(), INT24_8CH_96K);  // STREAM_OUTPUT 0
        }
        // The same rate again changes nothing.
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.rates.size(), size_t{2});
        auto const saved = load_persistent_state(path, 0, {});
        EXPECT_TRUE(saved.state.sampling_rates.size() == 1 && saved.state.sampling_rates.front().rate == 96000U);
        EXPECT_EQ(saved.state.stream_formats.size(), size_t{2});
    }
    {
        RateKit m{path};
        EXPECT_TRUE(m.start_ready());
        EXPECT_TRUE(m.rates.size() == 1 && m.rates.front() == 96000U);
        EXPECT_EQ(m.kit.sample_rate(), 96000U);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_96K);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(96000, 2));
        // The TX and RX slots opened at start() are the restored formats'.
        EXPECT_EQ(m.kit.talker().slot_for(0)->spec.format.sample_rate_hz, 96000U);
        EXPECT_EQ(m.kit.listener()->slot_for(0)->aaf->channel_count, uint16_t{2});
        // Back to 48 kHz.
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(48000)), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 2));
    }
    std::filesystem::remove_all(dir);
}

// An entity that applies the rate and then refuses it leaves the kit exactly
// as it was: rate, formats, stream paths, no notifications
TEST(avb_entity_kit_formats, a_refused_rate_change_is_undone)
{
    RateKit m;
    EXPECT_TRUE(m.start_ready());
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 4))),
        atdecc::AEM_STATUS_SUCCESS);
    m.kit.set_on_sampling_rate_changed([&m](uint32_t const rate) -> uint8_t {
        EXPECT_TRUE(m.kit.apply_sampling_rate(rate));
        EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, rate);
        return atdecc::AEM_STATUS_NO_RESOURCES;
    });
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_EQ(m.kit.talker().samples_per_packet_, 6U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 4));
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), INT24_8CH_48K);
    EXPECT_EQ(m.kit.listener()->slot_for(0)->spec.format.sample_rate_hz, 48000U);
    EXPECT_EQ(m.kit.talker().slot_for(0)->spec.format_word, INT24_8CH_48K);
    std::array<uint8_t, 4> get_rate{0x00, 0x02, 0x00, 0x00};
    auto const r = m.aem_command(atdecc::AEM_COMMAND_GET_SAMPLING_RATE, get_rate);
    EXPECT_TRUE(r.status == atdecc::AEM_STATUS_SUCCESS && r.body.size() >= 8 && r.body[5] == 0x00 && r.body[6] == 0xBB && r.body[7] == 0x80);
    m.aem.clear();
    m.kit.on_declaration_timer(MilanKit::steady_ns());
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).empty());
    // A hook that re-times back itself before refusing (as the bridge's data
    // plane falls back to the old rate) leaves nothing queued either.
    m.kit.set_on_sampling_rate_changed([&m](uint32_t const rate) -> uint8_t {
        EXPECT_TRUE(m.kit.apply_sampling_rate(rate));
        EXPECT_TRUE(m.kit.apply_sampling_rate(48000));
        return atdecc::AEM_STATUS_NO_RESOURCES;
    });
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_INPUT, 0), base_format(48000, 4));
    m.aem.clear();
    m.kit.on_declaration_timer(MilanKit::steady_ns());
    EXPECT_TRUE(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_STREAM_FORMAT).empty());
    // apply_sampling_rate is all or nothing: a rate a stream lists no format at changes nothing.
    EXPECT_FALSE(m.kit.apply_sampling_rate(192000));
    EXPECT_EQ(m.kit.sample_rate(), 48000U);
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
    spec = m.kit.talker_specs().back();  // the CRF
    EXPECT_TRUE(m.kit.talker().reformat_stream(spec).has_value());  // its own format: nothing to do
    spec.format_word ^= 1;
    EXPECT_FALSE(m.kit.talker().reformat_stream(spec).has_value());
}

// SlotGuard: the data-plane thread never waits, the reactor waits for it
TEST(avb_entity_slot_guard, enter_never_waits_lock_waits_for_leave)
{
    SlotGuard guard;
    EXPECT_TRUE(guard.try_enter());
    std::atomic<bool> locked{false};
    std::thread reactor{[&] {
        guard.lock();
        locked = true;
        guard.unlock();
    }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    EXPECT_FALSE(locked.load());  // still in the data-plane thread's packet
    guard.leave();
    reactor.join();
    EXPECT_TRUE(locked.load());
    guard.lock();
    EXPECT_FALSE(guard.try_enter());  // re-shaping: the packet is skipped
    guard.unlock();
    {
        SlotGuard::Entry const entry{guard};
        EXPECT_TRUE(entry.entered());
    }
    EXPECT_TRUE(guard.try_enter());
    guard.leave();
}

// IEEE 802.1Q-2022 Clause 34.3.1: a talker format or rate change that takes every talker's
// TSpec bandwidth (Milan v1.2 Table 4.4) past 75% of the link is NO_RESOURCES; the other
// talkers' current formats count, a change lowering the total is always taken
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
    // 96 kHz doubles the AAF talker (22720 kbit/s): refused as its SET_STREAM_FORMAT.
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NO_RESOURCES);
    EXPECT_TRUE(m.rates.empty());
    // At 30 Mbit/s (22500 kbit/s) the 22208 fits.
    m.kit.set_link_mbps(30);
    EXPECT_EQ(output(base_format(48000, 8)), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(m.current_format(DESCRIPTOR_STREAM_OUTPUT, 0), base_format(48000, 8));
    // Over budget already, narrowing back is taken; listeners never count.
    m.kit.set_link_mbps(1);
    EXPECT_EQ(output(INT24_8CH_48K), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_INPUT, 0, base_format(48000, 8))),
        atdecc::AEM_STATUS_SUCCESS);
    m.kit.set_link_mbps(40);
    EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_SUCCESS);
}

// A streaming output refuses the rate change; so does a kit without the
// entity's hook; a local change is refused while a controller has the entity
// locked, and is otherwise applied and notified like a controller's
TEST(avb_entity_kit_formats, rate_refusals_and_local_changes)
{
    {
        RateKit m;
        EXPECT_TRUE(m.start_ready());
        (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 1);  // the CRF output
        m.bridge_listener(1, srp::msrp::ListenerDeclaration::Ready);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_STREAM_IS_RUNNING);
        EXPECT_TRUE(m.rates.empty());
    }
    {
        RateKit m;
        m.kit.set_on_sampling_rate_changed({});
        EXPECT_TRUE(m.start_ready());
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(96000)), atdecc::AEM_STATUS_NOT_SUPPORTED);
        EXPECT_EQ(m.aem_packet(atdecc::AEM_COMMAND_SET_SAMPLING_RATE, rate_body(48000)), atdecc::AEM_STATUS_SUCCESS);
    }
    {
        RateKit m;
        EXPECT_TRUE(m.start_ready());
        std::array<uint8_t, 0> none{};
        (void)m.aem_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, none);
        std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};
        EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_LOCK_ENTITY, lock).status, atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.kit.request_sampling_rate(96000), atdecc::AEM_STATUS_ENTITY_LOCKED);
        EXPECT_TRUE(m.rates.empty());
        lock[3] = 0x01;  // UNLOCK
        EXPECT_EQ(m.aem_command(atdecc::AEM_COMMAND_LOCK_ENTITY, lock).status, atdecc::AEM_STATUS_SUCCESS);
        m.aem.clear();
        EXPECT_EQ(m.kit.request_sampling_rate(96000), atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.kit.sample_rate(), 96000U);
        EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_SET_SAMPLING_RATE).size(), size_t{1});
        EXPECT_EQ(m.kit.request_sampling_rate(44100), atdecc::AEM_STATUS_NOT_SUPPORTED);
    }
}

// A binding saved for an input whose format has since changed is dropped
TEST(avb_entity_kit_persistence, binding_for_a_changed_input_format_dropped)
{
    auto const dir = state_dir("bindfmt");
    auto const path = (dir / "avb-state.toml").string();
    PersistentState saved{};
    saved.configured_entity_name = "kit milan";
    saved.configured_presentation_offset_ns = 2'000'000;
    saved.listener_bindings.push_back(
        {.stream_input = 0, .talker_entity_id = 0x0200001111110001ULL, .talker_unique_id = 2, .format = CRF_MILAN_48K});
    EXPECT_TRUE(save_persistent_state(path, saved).has_value());
    PersistentKit m{path};
    EXPECT_TRUE(m.start_ready());
    EXPECT_TRUE(m.kit.host().components().acmp_listener.fast_connect_goals().empty());
    EXPECT_FALSE(m.kit.host().components().acmp_listener.milan_sinks().sink(0)->bound());
    std::filesystem::remove_all(dir);
}

// --- The Milan listener -------------------------------------------------------

namespace {

ieee::Eui64 const BINDING_CONTROLLER = ieee::Eui64{}.from_uint64(0x001B92FFFE00C001ULL);

/// A Milan listener kit and a Milan talker kit on one simulated segment: ACMP
/// and MSRP queued between them and delivered by pump().
struct MilanPair
{
    explicit MilanPair(std::string const& path = {}, std::optional<bool> const listener_milan = std::nullopt)
        : l{path, 0x40, listener_milan}
        , t{std::nullopt, 0x41}
    {}

    auto start() -> bool
    {
        if (!l.start_ready() || !t.start_ready()) {
            return false;
        }
        auto const queue = [this](atdecc::AcmpCommandResponse const& pdu) {
            acmp.push_back(pdu);
            return true;
        };
        lc().acmp_listener.set_transmit(queue, queue);
        tc().acmp_talker.set_callbacks(nanoavb::AcmpTalkerCallbacks{.tx_response = queue});
        lc().msrp_handler.set_send_packet([this](std::span<uint8_t const> p) {
            to_t.emplace_back(p.begin(), p.end());
            return true;
        });
        tc().msrp_handler.set_send_packet([this](std::span<uint8_t const> p) {
            to_l.emplace_back(p.begin(), p.end());
            return true;
        });
        now = sm::Clock::now();
        return true;
    }

    auto lc() -> nanoavb::NanoAvbComponents& { return l.kit.host().components(); }
    auto tc() -> nanoavb::NanoAvbComponents& { return t.kit.host().components(); }
    [[nodiscard]] auto sink() const -> atdecc::MilanSink const&
    {
        return *l.kit.host().components().acmp_listener.milan_sinks().sink(0);
    }

    /// One round of the segment every @p step, @p rounds times; MSRP only
    /// with @p msrp.
    void pump(int const rounds = 10, bool const msrp = true, std::chrono::milliseconds const step = std::chrono::milliseconds{100})
    {
        for (int i = 0; i < rounds; ++i) {
            now += step;
            lc().acmp_listener.tick(now);
            auto pending = std::move(acmp);
            acmp.clear();
            for (auto const& pdu : pending) {
                heard.push_back(pdu);
                (void)tc().acmp_talker.receive_command(pdu, now);
                (void)lc().acmp_listener.receive_controller_command(pdu, now);
                (void)lc().acmp_listener.receive_talker_response(pdu, now);
            }
            if (!msrp) {
                continue;
            }
            lc().msrp_handler.tick(now);
            tc().msrp_handler.tick(now);
            for (auto const& p : std::exchange(to_l, {})) {
                lc().msrp_handler.receive_packet(p, now);
            }
            for (auto const& p : std::exchange(to_t, {})) {
                tc().msrp_handler.receive_packet(p, now);
            }
        }
    }

    auto command(uint8_t const type, uint16_t const flags = 0) -> atdecc::AcmpCommandResponse
    {
        atdecc::AcmpCommandResponse cmd{};
        cmd.init_command(type);
        cmd.controller_entity_id = BINDING_CONTROLLER;
        cmd.talker_entity_id = t.config.entity_id;
        cmd.talker_unique_id = uint16_t{0};
        cmd.listener_entity_id = l.config.entity_id;
        cmd.listener_unique_id = uint16_t{0};
        cmd.sequence_id = sequence++;
        cmd.flags = flags;
        return cmd;
    }

    /// A controller's ACMP command to the listener; its response.
    auto controller(uint8_t const type, uint16_t const flags = 0) -> atdecc::AcmpCommandResponse
    {
        auto const cmd = command(type, flags);
        (void)lc().acmp_listener.receive_controller_command(cmd, now);
        for (auto const& pdu : acmp) {
            if (pdu.message_type() == type + 1 && pdu.sequence_id.get() == cmd.sequence_id.get()) {
                return pdu;
            }
        }
        return {};
    }

    /// GET_STREAM_INFO of the listener's STREAM_INPUT 0.
    auto input_info() -> std::pair<atdecc::aem::AemStreamInfoPayload, atdecc::aem::AemStreamInfoMilanExtension>
    {
        std::array<uint8_t, 4> body{0x00, 0x05, 0x00, 0x00};
        auto const r = l.aem_command(atdecc::AEM_COMMAND_GET_STREAM_INFO, body);
        EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(r.body.size(), atdecc::aem::AEM_STREAM_INFO_MILAN_LENGTH);
        return load_info(r.body);
    }

    auto streaming(bool const start) -> uint8_t
    {
        std::array<uint8_t, 4> body{0x00, 0x05, 0x00, 0x00};
        return l.aem_command(start ? atdecc::AEM_COMMAND_START_STREAMING : atdecc::AEM_COMMAND_STOP_STREAMING, body).status;
    }

    /// The talker's ENTITY_AVAILABLE reaching the listener.
    void talker_advertises()
    {
        auto const& ours = lc().adp_advertiser.adpdu();
        lc().acmp_listener.receive_adpdu(
            tc().adp_advertiser.adpdu(), ours.gptp_grandmaster_id, ours.gptp_domain_number.get(), now);
    }

    [[nodiscard]] auto listener_reservation() -> nanoavb::ListenerReservationState
    {
        auto const* s = lc().msrp_handler.get_listener_stream(t.sid(0));
        return s != nullptr ? s->state : nanoavb::ListenerReservationState::Idle;
    }

    PersistentKit l;
    MilanKit t;
    sm::TimePoint now{};
    uint16_t sequence{0x100};
    std::vector<atdecc::AcmpCommandResponse> acmp;
    std::vector<atdecc::AcmpCommandResponse> heard;
    std::vector<std::vector<uint8_t>> to_l;
    std::vector<std::vector<uint8_t>> to_t;
};

namespace sif = atdecc::aem::stream_info_flags;

}  // namespace

// BIND_RX answered at once; the probe makes the talker declare; the listener
// settles and declares Ready; UNBIND withdraws without a DISCONNECT_TX.
TEST(avb_entity_kit_milan_listener, binds_probes_settles_and_reserves)
{
    MilanPair p;
    EXPECT_TRUE(p.start());
    EXPECT_TRUE(p.lc().acmp_listener.milan());

    auto const bound = p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
    EXPECT_EQ(bound.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(bound.connection_count.get(), uint16_t{1});
    EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::prb_w_resp);
    EXPECT_EQ(p.acmp.back().message_type(), atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND);

    p.pump(1, false);  // the probe reaches the talker, which declares
    EXPECT_TRUE(p.t.kit.talker_declared(0));
    p.pump(1, false);  // its response settles the sink
    EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::settled_no_rsv);
    EXPECT_TRUE(p.lc().acmp_listener.is_connected(0));
    EXPECT_EQ(p.lc().acmp_listener.get_stream(0)->stream_id.to_uint64(), p.t.sid(0).to_uint64());

    p.pump(10);
    EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::settled_rsv_ok);
    EXPECT_TRUE(p.listener_reservation() == nanoavb::ListenerReservationState::Ready);
    EXPECT_TRUE(p.tc().msrp_handler.registered_listener(p.t.sid(0)) == srp::msrp::ListenerDeclaration::Ready);
    EXPECT_FALSE(p.t.kit.admitted_talker_streams().empty());

    auto const [info, ext] = p.input_info();
    uint32_t const flags = info.flags.get();
    EXPECT_EQ(
        flags,
        sif::STREAM_FORMAT_VALID | sif::BOUND | sif::FAST_CONNECT | sif::SAVED_STATE | sif::STREAM_ID_VALID |
            sif::STREAM_DEST_MAC_VALID | sif::STREAM_VLAN_ID_VALID | sif::MSRP_ACC_LAT_VALID);
    EXPECT_EQ(info.stream_id.to_uint64(), p.t.sid(0).to_uint64());
    EXPECT_EQ(info.stream_vlan_id.get(), uint16_t{2});
    EXPECT_EQ(ext.flags_ex.get(), atdecc::aem::stream_info_flags_ex::REGISTERING);
    EXPECT_EQ(ext.probing_status(), atdecc::aem::probing_status::COMPLETED);
    EXPECT_EQ(ext.acmp_status(), uint8_t{0});

    auto const rx = p.controller(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND);
    EXPECT_EQ(rx.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_EQ(rx.talker_entity_id, p.t.config.entity_id);
    EXPECT_EQ(rx.connection_count.get(), uint16_t{1});
    EXPECT_EQ(rx.flags.get(), atdecc::acmp_flags::FAST_CONNECT);
    EXPECT_EQ(rx.stream_id.to_uint64(), p.t.sid(0).to_uint64());

    auto const unbound = p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND);
    EXPECT_EQ(unbound.status(), atdecc::ACMP_STATUS_SUCCESS);
    EXPECT_FALSE(p.sink().bound());
    EXPECT_FALSE(p.lc().acmp_listener.is_connected(0));
    EXPECT_TRUE(p.listener_reservation() == nanoavb::ListenerReservationState::Idle);
    p.heard.clear();
    p.pump(10);
    EXPECT_TRUE(p.tc().msrp_handler.registered_listener(p.t.sid(0)) == srp::msrp::ListenerDeclaration::Ignore);
    EXPECT_TRUE(std::ranges::none_of(p.heard, [](auto const& pdu) {
        return pdu.message_type() == atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND;
    }));
    EXPECT_EQ(p.input_info().first.flags.get(), sif::STREAM_FORMAT_VALID);
}

// Settled before the talker's Advertise reaches it, the listener declares
// nothing (Milan v1.2 Section 5.3.8.5); once registered, Listener Ready
TEST(avb_entity_kit_milan_listener, declares_only_once_the_advertise_is_registered)
{
    MilanPair p;
    EXPECT_TRUE(p.start());
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
    p.pump(3, false);
    EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::settled_no_rsv);
    EXPECT_TRUE(p.listener_reservation() == nanoavb::ListenerReservationState::Waiting);
    auto [info, ext] = p.input_info();
    EXPECT_EQ(ext.flags_ex.get(), 0U);
    EXPECT_EQ(info.flags.get() & sif::MSRP_ACC_LAT_VALID, 0U);
    EXPECT_EQ(ext.probing_status(), atdecc::aem::probing_status::COMPLETED);
    p.pump(10);
    EXPECT_TRUE(p.listener_reservation() == nanoavb::ListenerReservationState::Ready);
    EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::settled_rsv_ok);
}

// START/STOP_STREAMING on a bound input (Milan v1.2 Sections 5.4.2.19-20):
// stopped discards the stream's AVTPDUs and shows STREAMING_WAIT; no effect
// on an unbound input; outputs still refuse them
TEST(avb_entity_kit_milan_listener, start_and_stop_streaming_on_inputs)
{
    MilanPair p;
    EXPECT_TRUE(p.start());
    EXPECT_EQ(p.streaming(false), atdecc::AEM_STATUS_SUCCESS);  // unbound: no effect
    EXPECT_FALSE(p.l.kit.listener()->slot_for(0)->stopped.load());
    EXPECT_EQ(p.input_info().first.flags.get() & sif::STREAMING_WAIT, 0U);

    // An unbound input bound with STREAMING_WAIT is stopped from the start,
    // and START / UNBIND act on it
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND, atdecc::acmp_flags::STREAMING_WAIT);
    EXPECT_FALSE(p.sink().binding.started);
    EXPECT_TRUE(p.l.kit.listener()->slot_for(0)->stopped.load());
    EXPECT_EQ(p.streaming(true), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(p.l.kit.listener()->slot_for(0)->stopped.load());
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND);
    EXPECT_FALSE(p.l.kit.listener()->slot_for(0)->stopped.load());

    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
    p.pump(10);
    EXPECT_EQ(p.streaming(false), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_FALSE(p.sink().binding.started);
    EXPECT_TRUE(p.l.kit.listener()->slot_for(0)->stopped.load());
    EXPECT_NE(p.input_info().first.flags.get() & sif::STREAMING_WAIT, 0U);
    EXPECT_EQ(
        p.controller(atdecc::ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND).flags.get(),
        static_cast<uint16_t>(atdecc::acmp_flags::FAST_CONNECT | atdecc::acmp_flags::STREAMING_WAIT));
    EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::settled_rsv_ok);  // still reserved

    EXPECT_EQ(p.streaming(true), atdecc::AEM_STATUS_SUCCESS);
    EXPECT_TRUE(p.sink().binding.started);
    EXPECT_FALSE(p.l.kit.listener()->slot_for(0)->stopped.load());

    // A bind with STREAMING_WAIT binds stopped
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND, atdecc::acmp_flags::STREAMING_WAIT);
    EXPECT_FALSE(p.sink().binding.started);
    EXPECT_TRUE(p.l.kit.listener()->slot_for(0)->stopped.load());

    std::array<uint8_t, 4> output{0x00, 0x06, 0x00, 0x00};
    EXPECT_EQ(p.l.aem_command(atdecc::AEM_COMMAND_STOP_STREAMING, output).status, atdecc::AEM_STATUS_NOT_SUPPORTED);
}

// A locked entity takes BIND_RX / UNBIND_RX only from the locking controller
TEST(avb_entity_kit_milan_listener, lock_refuses_other_controllers)
{
    MilanPair p;
    EXPECT_TRUE(p.start());
    std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};  // flags 0, the ENTITY: locked by controller 0
    EXPECT_EQ(p.l.aem_command(atdecc::AEM_COMMAND_LOCK_ENTITY, lock).status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND).status(), atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
    EXPECT_FALSE(p.sink().bound());
    EXPECT_EQ(p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND).status(), atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED);
}

// A sink settled on a VID other than the Class A one joins it with MVRP,
// and leaves it when it unsettles (Milan v1.2 Section 4.4.1)
TEST(avb_entity_kit_milan_listener, another_vid_joins_mvrp)
{
    MilanPair p;
    EXPECT_TRUE(p.start());
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
    auto resp = p.sink().probe;
    resp.set_message_type(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_RESPONSE);
    resp.stream_id = ieee::Eui64{}.from_uint64(0x0200000000000777ULL);
    resp.stream_dest_mac = ieee::Eui48{0x91, 0xE0, 0xF0, 0x00, 0x07, 0x77};
    resp.stream_vlan_id = uint16_t{5};
    (void)p.lc().acmp_listener.receive_talker_response(resp, p.now);
    EXPECT_TRUE(p.sink().settled());
    EXPECT_TRUE(p.lc().mvrp_handler.get_vlan_state(5) != nanoavb::VlanState::Unregistered);
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND);
    EXPECT_TRUE(p.lc().mvrp_handler.get_vlan_state(5) == nanoavb::VlanState::Unregistered);
    EXPECT_TRUE(p.lc().mvrp_handler.get_vlan_state(2) != nanoavb::VlanState::Unregistered);  // the host's
}

// [avb] milan = false keeps the classic listener: CONNECT_RX relayed as
// CONNECT_TX, no Milan sink bound
TEST(avb_entity_kit_milan_listener, classic_profile_keeps_the_classic_listener)
{
    MilanPair p{{}, false};
    EXPECT_TRUE(p.start());
    EXPECT_FALSE(p.lc().acmp_listener.milan());
    auto const cmd = p.command(atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_TRUE(p.lc().acmp_listener.receive_controller_command(cmd, p.now));
    EXPECT_TRUE(p.lc().acmp_listener.has_pending());
    EXPECT_EQ(p.acmp.back().message_type(), atdecc::ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(p.sink().bound());
}

// The bound state is saved and restored: PRB_W_AVAIL until the talker
// advertises, then a probe (Milan v1.2 Sections 5.3.8.2-3, 5.3.8.7, 5.5.3.5.2).
TEST(avb_entity_kit_persistence, milan_binding_survives_a_restart_and_reprobes)
{
    auto const dir = state_dir("milanbind");
    auto const path = (dir / "avb-state.toml").string();
    {
        MilanPair p{path};
        EXPECT_TRUE(p.start());
        (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
        p.pump(10);
        EXPECT_EQ(p.streaming(false), atdecc::AEM_STATUS_SUCCESS);
        p.l.kit.on_declaration_timer(MilanKit::steady_ns());  // the end of the reactor pass
        auto const saved = load_persistent_state(path, 0, {});
        EXPECT_EQ(saved.state.listener_bindings.size(), size_t{1});
        auto const& b = saved.state.listener_bindings.front();
        EXPECT_EQ(b.stream_input, uint16_t{0});
        EXPECT_EQ(b.talker_entity_id, p.t.config.entity_id.to_uint64());
        EXPECT_EQ(b.talker_unique_id, uint16_t{0});
        EXPECT_EQ(b.controller_entity_id, BINDING_CONTROLLER.to_uint64());
        EXPECT_FALSE(b.started);
        EXPECT_EQ(b.format, AAF_48K_INT24_32CH);
    }
    {
        MilanPair p{path};
        EXPECT_TRUE(p.start());
        EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::prb_w_avail);
        EXPECT_EQ(p.sink().binding.controller_entity_id, BINDING_CONTROLLER);
        EXPECT_FALSE(p.sink().binding.started);
        EXPECT_TRUE(p.l.kit.listener()->slot_for(0)->stopped.load());
        auto const [info, ext] = p.input_info();
        EXPECT_EQ(info.flags.get(), sif::STREAM_FORMAT_VALID | sif::BOUND | sif::FAST_CONNECT | sif::SAVED_STATE | sif::STREAMING_WAIT);
        EXPECT_EQ(ext.probing_status(), atdecc::aem::probing_status::PASSIVE);
        p.pump(30);  // no ADP from the talker: it waits
        EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::prb_w_avail);

        p.talker_advertises();
        EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::prb_w_delay);
        p.pump(20);  // TMR_DELAY (at most 1 s), the probe, the reservation
        EXPECT_TRUE(p.sink().state == atdecc::MilanSinkState::settled_rsv_ok);
        EXPECT_TRUE(p.tc().msrp_handler.registered_listener(p.t.sid(0)) == srp::msrp::ListenerDeclaration::Ready);
        EXPECT_FALSE(p.sink().binding.started);  // still stopped
        // Unbound: nothing is kept
        (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND);
        p.l.kit.on_declaration_timer(MilanKit::steady_ns());
        EXPECT_TRUE(load_persistent_state(path, 0, {}).state.listener_bindings.empty());
    }
    std::filesystem::remove_all(dir);
}

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
        static_cast<uint8_t>(type >> 8U), static_cast<uint8_t>(type), static_cast<uint8_t>(index >> 8U), static_cast<uint8_t>(index)};
    auto const r = m.aem_command(atdecc::AEM_COMMAND_GET_COUNTERS, body);
    EXPECT_EQ(r.status, atdecc::AEM_STATUS_SUCCESS);
    return load_counters(r.body);
}

}  // namespace

// Milan v1.2 Table 5.17: the output's counters at bit values 0x01-0x10;
// STREAM_START when a Listener Ready opens the gate, STREAM_STOP when it
// closes, each notified.
TEST(avb_entity_kit_counters, stream_output_start_stop_at_milan_bits)
{
    MilanKit m{std::nullopt, 0x60};
    EXPECT_TRUE(m.start_ready());
    m.kit.host().components().aem_handler.tick(sm::Clock::now());
    auto c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters_valid.get(), 0x1FU);
    EXPECT_EQ(c.counters[0].get(), 0U);

    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
    m.bridge_listener(0, srp::msrp::ListenerDeclaration::Ready);
    c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters[0].get(), 1U);  // STREAM_START
    EXPECT_EQ(c.counters[1].get(), 0U);  // STREAM_STOP
    EXPECT_TRUE(m.kit.output_counters(0)->streaming);

    // The Listener goes; the grace window runs out: stopped.
    m.bridge_withdraw(0);
    m.kit.poll_admission(MilanKit::steady_ns() + TalkerGate::GRACE_NS + 1'000'000'000);
    c = counters_of(m, DESCRIPTOR_STREAM_OUTPUT, 0);
    EXPECT_EQ(c.counters[0].get(), 1U);
    EXPECT_EQ(c.counters[1].get(), 1U);
    EXPECT_FALSE(m.kit.output_counters(0)->streaming);
    EXPECT_TRUE(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_COUNTERS) == 0);  // nobody registered

    // Classic: IEEE 1722.1-2021's layout (FRAMES_TX at bit 7).
    MilanKit classic{false, 0x61};
    EXPECT_TRUE(classic.start_ready());
    EXPECT_EQ(counters_of(classic, DESCRIPTOR_STREAM_OUTPUT, 0).counters_valid.get(), 0x9BU);
}

// Milan v1.2 Table 5.16: the input's ten counters; zeroed when it becomes
// bound (Section 5.3.8.10), not when it is unbound; the fold counts intervals.
TEST(avb_entity_kit_counters, stream_input_reset_on_bind)
{
    MilanPair p;
    EXPECT_TRUE(p.start());
    auto* slot = p.l.kit.listener()->slot_for(0);
    EXPECT_TRUE(slot != nullptr);
    int64_t t = MilanKit::steady_ns();
    p.l.kit.poll_counters(t);  // the first interval starts
    slot->counters.frames_rx.store(10);
    slot->counters.seq_num_mismatch.store(3);
    p.l.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    auto c = counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0);
    EXPECT_EQ(c.counters_valid.get(), 0x0F3FU);
    EXPECT_EQ(c.counters[11].get(), 1U);  // FRAMES_RX: one interval
    EXPECT_EQ(c.counters[3].get(), 1U);   // SEQ_NUM_MISMATCH

    auto const bound = p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND);
    EXPECT_EQ(bound.status(), atdecc::ACMP_STATUS_SUCCESS);
    c = counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0);
    EXPECT_EQ(c.counters[11].get(), 0U);
    EXPECT_EQ(c.counters[3].get(), 0U);

    slot->counters.frames_rx.store(20);
    p.l.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS - 1);  // not yet a second
    EXPECT_EQ(counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0).counters[11].get(), 0U);
    p.l.kit.poll_counters(t += 1);
    EXPECT_EQ(counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0).counters[11].get(), 1U);

    // Unbound: kept.
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND);
    EXPECT_EQ(counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0).counters[11].get(), 1U);
}

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
        .grandmaster_id_changed = [&m](int64_t, gptp::ClockIdentity const&, gptp::AnnounceMessage const&) {
            m.kit.note_grandmaster_changed();
        },
        .path_trace_changed = [&m](int64_t) { m.kit.note_as_path_changed(); }});
    handlers->gptp_handler().on_announce(announce, MilanKit::steady_ns(), trace);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AS_PATH), 1);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AVB_INFO), 1);
    r = m.aem_command(atdecc::AEM_COMMAND_GET_AS_PATH, query);
    EXPECT_EQ(r.body.size(), size_t{4 + 16});  // no MAC here: our own identity is not known
    EXPECT_EQ(r.body[3], 2);
    EXPECT_EQ(r.body[4 + 8 + 7], 0x02);

    // The same Announce again: nothing new.
    handlers->gptp_handler().on_announce(announce, MilanKit::steady_ns(), trace);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AS_PATH), 1);
    EXPECT_EQ(unsolicited_count(m.aem, atdecc::AEM_COMMAND_GET_AVB_INFO), 1);
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
TEST(avb_entity_kit_counters, a_restored_binding_unbinds_without_an_interruption)
{
    auto const dir = state_dir("restored-counters");
    auto const path = (dir / "avb-state.toml").string();
    PersistentState saved{};
    saved.configured_entity_name = "kit milan";
    saved.configured_presentation_offset_ns = 2'000'000;
    saved.listener_bindings.push_back(
        {.stream_input = 0, .talker_entity_id = 0x020000fffe000041ULL, .talker_unique_id = 0, .started = true});
    EXPECT_TRUE(save_persistent_state(path, saved).has_value());
    MilanPair p{path};
    EXPECT_TRUE(p.start());
    EXPECT_TRUE(p.sink().bound());
    auto& c = p.l.kit.listener()->slot_for(0)->counters;
    int64_t t = MilanKit::steady_ns();
    p.l.kit.poll_counters(t);
    c.media_locked.store(1);  // as if its stream had locked
    c.is_locked.store(true);
    c.frames_rx.store(100);
    p.l.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_UNBIND_RX_COMMAND);
    EXPECT_FALSE(p.sink().bound());
    p.l.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);  // nothing received since
    auto counters = counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0);
    EXPECT_EQ(counters.counters[0].get(), 1U);  // MEDIA_LOCKED
    EXPECT_EQ(counters.counters[1].get(), 1U);  // MEDIA_UNLOCKED
    EXPECT_EQ(counters.counters[2].get(), 0U);  // STREAM_INTERRUPTED

    // Bound again and stopped: its stream discarded is no interruption.
    (void)p.controller(atdecc::ACMP_MESSAGE_TYPE_BIND_RX_COMMAND, atdecc::acmp_flags::STREAMING_WAIT);
    c.media_locked.fetch_add(1);
    c.is_locked.store(true);
    p.l.kit.poll_counters(t += AvbEntityKit::COUNTERS_INTERVAL_NS);
    counters = counters_of(p.l, DESCRIPTOR_STREAM_INPUT, 0);
    EXPECT_EQ(counters.counters[2].get(), 0U);
    EXPECT_EQ(counters.counters[0].get(), counters.counters[1].get());
    std::filesystem::remove_all(dir);
}

// ---- Names and dynamic input mappings ------------------------------------------

namespace {

ieee::Eui64 const OTHER_CONTROLLER{0x02, 0x00, 0x00, 0xC0, 0x00, 0x00, 0x00, 0x02};
ieee::Eui48 const OTHER_CONTROLLER_MAC{0x02, 0x00, 0x00, 0xC0, 0x00, 0x02};

/// Register a second controller for unsolicited notifications.
void register_other_controller(PersistentKit& m)
{
    nanoavb::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, nanoavb::AemDu::AEM_DATA_LENGTH);
    header.target_entity_id = m.config.entity_id;
    header.controller_entity_id = OTHER_CONTROLLER;
    std::array<uint8_t, nanoavb::AemDu::LENGTH> frame{};
    span_store(frame, header);
    (void)m.kit.host().components().aem_handler.process_packet(OTHER_CONTROLLER_MAC, frame, m.config.entity_id);
}

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
    auto const path = (dir / "avb-state.toml").string();
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
    auto const path = (dir / "avb-state.toml").string();
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
    // Restart, bound: the saved format, then the saved mappings, through the hooks.
    PersistentState state = load_persistent_state(path, 0, {}).state;
    state.listener_bindings.push_back(
        {.stream_input = 0, .talker_entity_id = 0x0200001111110001ULL, .talker_unique_id = 0, .format = base_format(48000, 2)});
    EXPECT_TRUE(save_persistent_state(path, state).has_value());
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
    // A saved set the model no longer takes falls back to the default.
    state = load_persistent_state(path, 0, {}).state;
    state.audio_maps[0].mappings.push_back({.stream_index = 0, .stream_channel = 9, .cluster_offset = 6, .cluster_channel = 0});
    state.listener_bindings.clear();
    EXPECT_TRUE(save_persistent_state(path, state).has_value());
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
    auto const path = (dir / "avb-state.toml").string();
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

        // Drop channels 2-7, then the narrower format is taken.
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

        // Milan v1.2 Sections 5.4.2.27-28: no local change while a controller holds the lock.
        std::array<uint8_t, atdecc::aem::AemLockEntityPayload::LENGTH> lock{};
        nanoavb::AemDu header{};
        header.init_command(atdecc::AEM_COMMAND_LOCK_ENTITY, static_cast<uint16_t>(nanoavb::AemDu::AEM_DATA_LENGTH + lock.size()));
        header.controller_entity_id = OTHER_CONTROLLER;
        std::array<uint8_t, nanoavb::MAX_AEM_RESPONSE_SIZE> out{};
        EXPECT_EQ(m.kit.host().components().aem_handler.handle_command(header, lock, out).status, atdecc::AEM_STATUS_SUCCESS);
        EXPECT_EQ(m.kit.request_audio_mappings(0, std::vector{mapping(0, 2)}, {}), atdecc::AEM_STATUS_ENTITY_LOCKED);
        EXPECT_TRUE(std::ranges::equal(m.kit.input_mappings(0), swapped));
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
// STREAM_IS_RUNNING), a wide enough one taken
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
    (void)m.acmp_command(atdecc::ACMP_MESSAGE_TYPE_PROBE_TX_COMMAND, 0);
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
    auto const path = (dir / "avb-state.toml").string();
    std::vector<nanoavb::DynamicAudioMapping> const swapped{mapping(1, 0), mapping(0, 1), mapping(2, 2)};
    {
        RateKit m{path, 0x74, true};
        m.kit.set_on_input_mappings_changed([](uint16_t, std::span<nanoavb::DynamicAudioMapping const>) { return atdecc::AEM_STATUS_SUCCESS; });
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

// Milan v1.2 Sections 5.4.2.7, 5.4.2.27: an input takes a format at another rate
// than the unit's only while no mapping names it, keeps no default there, is
// notified and saved, and comes back to the unit's rate unmapped
TEST(avb_entity_kit_mixed_rates, an_unmapped_input_takes_another_rate)
{
    auto const dir = state_dir("mixed-input");
    auto const path = (dir / "avb-state.toml").string();
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
        // Unmapped (the bridge removes them first, as a controller would), it is taken.
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

// Milan v1.2 Sections 5.4.2.13, 5.3.3.3: a unit listing one rate takes that rate
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
    EXPECT_EQ(m.kit.request_sampling_rate(96000), atdecc::AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(m.rates.empty());  // never asked to re-clock
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
    EXPECT_EQ(AvbEntityKit::talker_tspec(m.kit.talker_specs().front()).max_frame_size, uint16_t{12 * 8 * 3 + 24 + 1});
    EXPECT_EQ(m.kit.talker().samples_per_packet_, 6U);  // the media clock stays at the unit's rate
    // Back at the unit's rate the identity returns.
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, base_format(48000, 4))),
        atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(unsolicited(m.aem, atdecc::AEM_COMMAND_ADD_AUDIO_MAPPINGS).size(), size_t{1});
    EXPECT_EQ(m.kit.output_mappings(0).size(), size_t{4});
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_SUCCESS, uint16_t{4}}));
}

// A static output map names its stream: no format at another rate while it does
TEST(avb_entity_kit_mixed_rates, a_static_output_map_keeps_the_unit_rate)
{
    RateKit m{{}, 0x79, true};
    EXPECT_TRUE(m.start_ready());
    EXPECT_FALSE(m.kit.has_dynamic_output_map(0));
    EXPECT_EQ(
        m.aem_packet(atdecc::AEM_COMMAND_SET_STREAM_FORMAT, format_body(DESCRIPTOR_STREAM_OUTPUT, 0, INT24_8CH_96K)),
        atdecc::AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(m.formats.empty());
    EXPECT_TRUE((output_map(m) == std::pair{atdecc::AEM_STATUS_NOT_SUPPORTED, uint16_t{0}}));  // its AUDIO_MAP says it
}

// A port bound before start() is the kit's stream port: the talker transmits
// on it and the listener joins its static groups on it; begin_stream_tick
// begins the tick and flush_stream_tx flushes and ends it; stop() leaves it.
TEST(avb_entity_kit_stream_port, a_bound_port_carries_the_streams_and_the_tick)
{
    TestStreamPort port{ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x51}};
    MilanKit k{false, 0x51, /*gate=*/false};
    k.kit.bind_stream_port(port);
    EXPECT_TRUE(k.kit.start(k.reactor).has_value());
    EXPECT_FALSE(k.kit.stream_tx_error());
    EXPECT_FALSE(k.kit.stream_rx_error());
    EXPECT_FALSE(k.kit.stream_tx_priority_error());

    auto* const aaf = k.kit.talker().slot_of(StreamKind::aaf);
    EXPECT_TRUE(aaf != nullptr);
    EXPECT_TRUE(std::ranges::find(port.joined(), aaf->dest_mac) != port.joined().end());

    StreamRawRenderFn const render = [](std::span<uint8_t>, uint32_t, uint64_t, uint64_t) { return true; };
    EXPECT_TRUE(k.kit.stream_rx_on_tick());  // no descriptor: the media tick drains it
    EXPECT_EQ(k.kit.tick_stream_rx(1'000'000), size_t{0});
    k.kit.begin_stream_tick();
    EXPECT_TRUE(k.kit.talker().transmit_aaf_raw(*aaf, 0, 2'000'000, 6, render));
    EXPECT_TRUE(k.kit.flush_stream_tx().has_value());
    EXPECT_EQ(port.cycles_begun(), size_t{1});
    EXPECT_EQ(port.flushes(), size_t{1});
    EXPECT_EQ(port.cycles_ended(), size_t{1});
    auto const sent = port.pop_tx();
    EXPECT_TRUE(sent.has_value());
    std::span<uint8_t const> const frame = sent->frame;
    EXPECT_TRUE(std::ranges::equal(frame.first(6), aaf->dest_mac.value));
    EXPECT_TRUE(std::ranges::equal(frame.subspan(6, 6), port.hardware_address().value));

    EXPECT_TRUE(k.kit.stop().has_value());
    EXPECT_TRUE(k.kit.talker().transmit_aaf_raw(*aaf, 6, 2'125'000, 6, render));  // the bound port stays open
}

// A receive port with no descriptor is drained by the media tick: a frame
// injected into a bound one comes out of tick_stream_rx, not the reactor.
TEST(avb_entity_kit_stream_port, the_tick_drains_a_port_without_a_descriptor)
{
    TestStreamPort port{ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x52}};
    MilanKit k{false, 0x52, /*gate=*/false};
    k.kit.bind_stream_port(port);
    EXPECT_TRUE(k.kit.start(k.reactor).has_value());
    EXPECT_TRUE(k.kit.stream_rx_on_tick());
    std::array<uint8_t, 14 + 24> frame{};
    std::ranges::copy(std::array<uint8_t, 6>{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x52}, frame.begin());
    frame[12] = 0x22;
    frame[13] = 0xF0;
    frame[14] = 0x02;  // AAF, for no stream of this kit
    EXPECT_TRUE(port.inject_rx(frame).has_value());
    EXPECT_EQ(k.kit.tick_stream_rx(1'000'000), size_t{1});
    EXPECT_EQ(port.stats().rx_frames, uint64_t{1});
}

// An unpollable receive port has one consumer, the media thread: an RX timer
// thread of its own is refused before anything starts, unless the host says
// it drains from the media thread.
TEST(avb_entity_kit_stream_port, an_rx_timer_thread_is_refused_for_a_port_without_a_descriptor)
{
    {
        TestStreamPort port{ieee::Eui48{0x02, 0x00, 0x00, 0x00, 0x00, 0x53}};
        MilanKit k{false, 0x53, /*gate=*/false};
        k.config.stream_rx_rt_timer = true;
        k.kit.bind_stream_port(port);
        auto const started = k.kit.start(k.reactor);
        EXPECT_TRUE(!started && started.error() == std::errc::invalid_argument);
        EXPECT_FALSE(k.kit.host().is_running());
    }
    {
        MilanKit k{false, 0x54, /*gate=*/false};
        k.config.stream_rx_rt_timer = true;
        k.config.stream_transport = StreamTransport::xdp;
        auto const started = k.kit.start(k.reactor);
        EXPECT_TRUE(!started && started.error() == std::errc::invalid_argument);
    }
    {
        MilanKit k{false, 0x55, /*gate=*/false};
        k.config.stream_rx_rt_timer = true;
        k.config.stream_rx_on_media_thread = true;  // the bridge's tick drains it
        k.config.stream_transport = StreamTransport::xdp;
        EXPECT_TRUE(k.kit.start(k.reactor).has_value());
        EXPECT_FALSE(k.kit.stream_rx_on_tick());  // the host drains it with drain_stream_rx
    }
}

// The transport's fallback is recorded: under xdp a failed AF_XDP open is the
// stream RX error; under auto the kit falls back to AF_PACKET (whose own
// error, here, is the RX error) and keeps the AF_XDP reason.
TEST(avb_entity_kit_stream_port, the_transport_fallback_is_recorded)
{
    {
        MilanKit k{false, 0x56, /*gate=*/false};
        EXPECT_TRUE(k.kit.start(k.reactor).has_value());
        EXPECT_FALSE(k.kit.stream_xdp_error());  // packet: never tried
        EXPECT_TRUE(k.kit.stream_transport_in_force() == StreamTransport::packet);
    }
    {
        MilanKit k{false, 0x57, /*gate=*/false};
        k.config.stream_transport = StreamTransport::xdp;
        EXPECT_TRUE(k.kit.start(k.reactor).has_value());
        EXPECT_TRUE(k.kit.stream_xdp_error());
        EXPECT_TRUE(k.kit.stream_rx_error() == k.kit.stream_xdp_error());
        EXPECT_TRUE(k.kit.stream_transport_in_force() == StreamTransport::packet);
    }
    {
        MilanKit k{false, 0x58, /*gate=*/false};
        k.config.stream_transport = StreamTransport::auto_;
        EXPECT_TRUE(k.kit.start(k.reactor).has_value());
        EXPECT_TRUE(k.kit.stream_xdp_error());
        EXPECT_TRUE(k.kit.stream_rx_error());
        EXPECT_TRUE(k.kit.stream_rx_error() != k.kit.stream_xdp_error());
        EXPECT_TRUE(k.kit.stream_transport_in_force() == StreamTransport::packet);
    }
}
