// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit at its stream bound, with no network: MAX_ENTITY_STREAMS outputs and
// inputs (16 AAF 32-channel + CRF each way) all open; one more is refused when
// the tables are derived, and an extra slot by the stream paths themselves.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
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
    auto const blob = make_blob(1, 1);
    auto const storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>{blob});
    EXPECT_TRUE(storage.has_value());
    auto const talkers = *talker_stream_specs(*storage, 0);
    auto const listeners = *listener_stream_specs(*storage, 0);

    AvbEntityAudioIOConfig config{};
    config.interface_name = "kit-admission-test0";  // no such interface: no MAC, no sockets
    config.entity_id = ieee::Eui64{0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x02};
    config.stream_address_mode = "static";
    EXPECT_TRUE(config.gate_talker_on_listener);
    EntityIdentityDescriptorHandler handler{
        *storage, config.entity_id, ieee::Eui64{}, "1.0.0", "kit admission", std::nullopt, /*patch_avb_interface=*/false};
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
