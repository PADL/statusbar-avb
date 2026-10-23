// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// The kit at its stream bound, with no network: MAX_ENTITY_STREAMS outputs and
// inputs (16 AAF 32-channel + CRF each way) all open; one more is refused when
// the tables are derived, and an extra slot by the stream paths themselves.

#include "statusbar/avb_entity/avb_entity_kit.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io.hpp"
#include "statusbar/avb_entity/avb_entity_audio_io_config.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/avb_entity/avb_entity_maap.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"
#include "statusbar/avb_entity/avb_entity_tone_generator.hpp"
#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_maap.hpp"
#include "statusbar/avtp/avtp_maap_handler.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/itc/itc_stop_token.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_test_port.hpp"
#include "statusbar/ptpclient/ptpclient_media_clock.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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
    explicit KitUnderTest(AvbEntityAudioIOConfig config, std::optional<uint16_t> const crf_clock_source = std::nullopt)
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
              crf_clock_source,
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

// A data plane beside the port follows each stream's frames: a Domain adopted from the wire (Milan v1.3
// 4.2.7.2.1) moves every stream's tag, a MAAP block (IEEE 1722-2025 Annex B) the destinations it gives.
TEST(avb_entity_kit_domain, an_adopted_domain_and_a_maap_block_are_told_as_changes_of_the_frames)
{
    KitUnderTest kit{make_config(7, "milan13")};
    EXPECT_TRUE(kit.start(TimePoint{std::chrono::seconds{1}}));
    std::vector<uint16_t> told;
    kit->talker().set_on_frames_changed([&told](uint16_t const index) { told.push_back(index); });
    bridge_declares_domain(kit, DomainInfo{.sr_class_id = 6, .sr_class_priority = 5, .sr_class_vid = 5});
    std::vector<uint16_t> every{};
    for (auto const& spec : kit.talker_specs()) {
        every.push_back(spec.index);
    }
    EXPECT_TRUE(told == every);

    told.clear();
    std::array<uint16_t, 1> const indices{every.front()};
    Eui48 const block{0x91, 0xE0, 0xF0, 0x00, 0x56, 0x78};
    MaapAddressAcquirer::publish_block(kit->host().components(), kit->talker(), indices, block);
    EXPECT_TRUE(told == std::vector<uint16_t>{every.front()});
    MaapAddressAcquirer::publish_block(kit->host().components(), kit->talker(), indices, block);  // the same block
    EXPECT_EQ(told.size(), size_t{1});
    kit->talker().set_on_frames_changed({});
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
        EXPECT_EQ(info.max_frame_size, milan_srp_max_frame_size(spec.format));
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

}  // namespace kit_domain

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
TEST_MAIN(statusbar_avb_entity, avb_entity_kit_test)
