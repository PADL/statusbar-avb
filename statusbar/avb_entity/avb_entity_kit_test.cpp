// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit at its stream bound, with no network: MAX_ENTITY_STREAMS outputs and
// inputs (16 AAF 32-channel + CRF each way) all open; one more is refused when
// the tables are derived, and an extra slot by the stream paths themselves.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/test/test.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <system_error>
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
    explicit MilanKit(std::optional<bool> const milan, uint64_t const entity_low, bool const gate = true)
        : blob{make_blob(1, 1)}
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
        EXPECT_TRUE(components.msrp_handler.talker_advertise_registered(stream_id(0x0200000000000100ULL + spec.index)));
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
