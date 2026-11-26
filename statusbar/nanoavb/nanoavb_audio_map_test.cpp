// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Dynamic audio mappings of STREAM_PORT_INPUTs and STREAM_PORT_OUTPUTs (IEEE 1722.1-2021 Clauses
// 7.4.44-7.4.46, Milan v1.3 Sections 5.4.2.26-5.4.2.28): partitions, the BAD_ARGUMENTS rules of
// each direction, atomicity, the lock and notifications.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

Eui64 const US{0xAA, 0, 0, 0, 0, 0, 0, 0x70};
Eui64 const CONTROLLER_A{0xC0, 0, 0, 0, 0, 0, 0, 0x0A};
Eui64 const CONTROLLER_B{0xC0, 0, 0, 0, 0, 0, 0, 0x0B};
Eui48 const MAC_A{0x02, 0, 0, 0, 0, 0x0A};
Eui48 const MAC_B{0x02, 0, 0, 0, 0, 0x0B};

constexpr uint64_t AAF_4CH_48K = 0x0205022001006000ULL;
constexpr uint64_t AAF_8CH_96K = 0x020702200200C000ULL;
constexpr uint64_t AAF_200CH_48K = 0x0205022032006000ULL;
constexpr uint64_t CRF_48K = 0x041060010000BB80ULL;

/// AUDIO_UNIT 0 at 48 kHz. STREAM_PORT_INPUT 0: four 1-channel clusters, 1: one 200-channel
/// cluster, 2: a static map; STREAM_INPUT 0: 4 ch 48 kHz, 1: 8 ch 96 kHz, 2: CRF.
/// STREAM_PORT_OUTPUT 0: two 1-channel clusters; STREAM_OUTPUT 0: 4 ch 48 kHz listing 8 ch 96 kHz
/// too, 1: 200 ch 48 kHz, 2: 8 ch 96 kHz.
class Model final : public AemEntityHandler
{
  public:
    auto on_get_entity(DescriptorId /*id*/, DescriptorEntity& desc) -> bool override
    {
        desc.configurations_count = uint16_t{2};  // the same ports in both
        return true;
    }
    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override
    {
        if (id.ref.descriptor_index != 0) {
            return false;
        }
        desc.number_of_stream_input_ports = uint16_t{3};
        desc.base_stream_input_port = uint16_t{0};
        desc.number_of_stream_output_ports = uint16_t{1};
        desc.current_sampling_rate = unit_rate;
        return true;
    }
    auto on_get_stream_port(DescriptorId id, DescriptorStreamPort& desc) -> bool override
    {
        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_PORT_OUTPUT && id.ref.descriptor_index == 0) {
            desc.number_of_clusters = uint16_t{2};
            desc.base_cluster = uint16_t{6};
            desc.port_flags = output_flags;
            return true;
        }
        if (id.ref.descriptor_type != DESCRIPTOR_STREAM_PORT_INPUT || id.ref.descriptor_index > 2) {
            return false;
        }
        std::array<uint16_t, 3> const base{0, 4, 5};
        desc.number_of_clusters = static_cast<uint16_t>(id.ref.descriptor_index == 0 ? 4 : 1);
        desc.base_cluster = base.at(id.ref.descriptor_index);
        desc.port_flags = id.ref.descriptor_index == 0 ? port0_flags : uint16_t{0};
        desc.number_of_maps = static_cast<uint16_t>(id.ref.descriptor_index == 2 ? 1 : 0);
        return true;
    }
    auto on_get_audio_cluster(DescriptorId id, DescriptorAudioCluster& desc) -> bool override
    {
        if (id.ref.descriptor_index > 7) {
            return false;
        }
        desc.channel_count = static_cast<uint16_t>(id.ref.descriptor_index == 4 ? 200 : 1);
        return true;
    }
    auto on_get_stream(DescriptorId id, DescriptorStream& desc) -> bool override
    {
        bool const output = id.ref.descriptor_type == DESCRIPTOR_STREAM_OUTPUT;
        auto const& formats = output ? output_formats : input_formats;
        if ((!output && id.ref.descriptor_type != DESCRIPTOR_STREAM_INPUT) || id.ref.descriptor_index >= formats.size()) {
            return false;
        }
        desc.current_format.from_uint64(formats.at(id.ref.descriptor_index));
        if (output && id.ref.descriptor_index == 0) {
            (void)desc.push_stream_format(Eui64{}.from_uint64(AAF_4CH_48K));
            (void)desc.push_stream_format(Eui64{}.from_uint64(AAF_8CH_96K));
        }
        return true;
    }

    uint32_t unit_rate{48000};
    uint16_t port0_flags{0};
    uint16_t output_flags{0};
    std::array<uint64_t, 3> input_formats{AAF_4CH_48K, AAF_8CH_96K, CRF_48K};
    std::array<uint64_t, 3> output_formats{AAF_4CH_48K, AAF_200CH_48K, AAF_8CH_96K};
};

struct Sent
{
    Eui48 dest{};
    AemDu header{};
    std::vector<uint8_t> body;
};

struct Bench
{
    Model model;
    AemCommandHandler aem{model};
    std::unique_ptr<DynamicAudioMaps> maps{std::make_unique<DynamicAudioMaps>()};
    std::vector<Sent> sent;
    /// What the data plane was handed, the port it was for, and what it answers.
    std::vector<std::vector<DynamicAudioMapping>> applied;
    std::vector<std::pair<uint16_t, uint16_t>> applied_to;
    uint8_t verdict{AEM_STATUS_SUCCESS};

    Bench()
    {
        aem.set_profile(AemCommandHandler::Profile::milan13);  // the requester left out of the fan-out
        aem.set_entity_id(US);
        aem.set_dynamic_audio_maps(maps.get());
        aem.set_send_response([this](Eui48 const& dest, std::span<uint8_t const> bytes) {
            Sent s{.dest = dest};
            span_load(s.header, bytes.first(AemDu::LENGTH));
            s.body.assign(bytes.begin() + AemDu::LENGTH, bytes.end());
            sent.push_back(std::move(s));
            return true;
        });
        aem.set_on_audio_mappings_changed([this](uint16_t const type, uint16_t const port, std::span<DynamicAudioMapping const> m) {
            applied.emplace_back(m.begin(), m.end());
            applied_to.emplace_back(type, port);
            return verdict;
        });
        command(CONTROLLER_A, MAC_A, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, {});
        command(CONTROLLER_B, MAC_B, AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, {});
        sent.clear();
    }

    /// Send a command; the solicited response's status and body.
    auto command(Eui64 const& controller, Eui48 const& mac, uint16_t const type, std::span<uint8_t const> body) -> Sent
    {
        AemDu cmd{};
        cmd.init_command(type, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + body.size()));
        cmd.target_entity_id = US;
        cmd.controller_entity_id = controller;
        std::vector<uint8_t> packet(AemDu::LENGTH + body.size());
        span_store(std::span<uint8_t>{packet}.first(AemDu::LENGTH), cmd);
        std::ranges::copy(body, packet.begin() + AemDu::LENGTH);
        size_t const before = sent.size();
        (void)aem.process_packet(mac, packet, US);
        for (size_t i = before; i < sent.size(); ++i) {
            if (!sent[i].header.is_unsolicited()) {
                return sent[i];
            }
        }
        return {};
    }

    auto mappings(
        bool const add,
        uint16_t const port,
        std::vector<DynamicAudioMapping> const& list,
        Eui64 const& from = CONTROLLER_A,
        uint16_t const type = DESCRIPTOR_STREAM_PORT_INPUT) -> uint8_t
    {
        std::vector<uint8_t> body(8 + (list.size() * 8));
        AemAudioMappingsCommandHeader header{};
        header.descriptor_type = type;
        header.descriptor_index = port;
        header.number_of_mappings = static_cast<uint16_t>(list.size());
        span_store(std::span{body}.first(8), header);
        for (size_t i = 0; i < list.size(); ++i) {
            AemAudioMapping wire{};
            wire.stream_index = list[i].stream_index;
            wire.stream_channel = list[i].stream_channel;
            wire.cluster_offset = list[i].cluster_offset;
            wire.cluster_channel = list[i].cluster_channel;
            span_store(std::span{body}.subspan(8 + (i * 8), 8), wire);
        }
        auto const r = command(
            from,
            from == CONTROLLER_A ? MAC_A : MAC_B,
            add ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS,
            body);
        // Every response echoes the command.
        EXPECT_TRUE(r.body == body);
        return r.header.status();
    }

    struct Map
    {
        uint8_t status{0};
        uint16_t number_of_maps{0};
        std::vector<DynamicAudioMapping> mappings;
    };
    auto get_map(uint16_t const port, uint16_t const map_index, uint16_t const type = DESCRIPTOR_STREAM_PORT_INPUT) -> Map
    {
        AemGetAudioMapCommandPayload cmd{};
        cmd.descriptor_type = type;
        cmd.descriptor_index = port;
        cmd.map_index = map_index;
        std::array<uint8_t, AemGetAudioMapCommandPayload::LENGTH> body{};
        span_store(body, cmd);
        auto const r = command(CONTROLLER_A, MAC_A, AEM_COMMAND_GET_AUDIO_MAP, body);
        Map out{.status = r.header.status()};
        if (r.body.size() >= AemAudioMapResponseHeader::LENGTH) {
            AemAudioMapResponseHeader resp{};
            span_load(resp, std::span<uint8_t const>{r.body}.first(AemAudioMapResponseHeader::LENGTH));
            out.number_of_maps = resp.number_of_maps.get();
            EXPECT_EQ(r.body.size(), AemAudioMapResponseHeader::LENGTH + (size_t{resp.number_of_mappings.get()} * 8));
            for (size_t i = 0; i < resp.number_of_mappings.get(); ++i) {
                AemAudioMapping wire{};
                span_load(wire, std::span<uint8_t const>{r.body}.subspan(AemAudioMapResponseHeader::LENGTH + (i * 8), 8));
                out.mappings.push_back(
                    {.stream_index = wire.stream_index.get(),
                     .stream_channel = wire.stream_channel.get(),
                     .cluster_offset = wire.cluster_offset.get(),
                     .cluster_channel = wire.cluster_channel.get()});
            }
        }
        return out;
    }

    auto notified(uint16_t const type, Eui48 const& dest) const -> int
    {
        int n = 0;
        for (auto const& s : sent) {
            n += (s.header.is_unsolicited() && s.header.command_code() == type && s.dest == dest) ? 1 : 0;
        }
        return n;
    }

    /// Lock the entity for controller B.
    void lock_for_b()
    {
        std::array<uint8_t, AemLockEntityPayload::LENGTH> lock_body{};
        span_store(lock_body, AemLockEntityPayload{});
        EXPECT_EQ(command(CONTROLLER_B, MAC_B, AEM_COMMAND_LOCK_ENTITY, lock_body).header.status(), AEM_STATUS_SUCCESS);
    }
};

auto m(uint16_t const stream, uint16_t const stream_ch, uint16_t const cluster, uint16_t const cluster_ch = 0)
    -> DynamicAudioMapping
{
    return {.stream_index = stream, .stream_channel = stream_ch, .cluster_offset = cluster, .cluster_channel = cluster_ch};
}

/// Every cluster channel of input port 1, fed from stream 0's four channels in turn.
auto all_of_port_1() -> std::vector<DynamicAudioMapping>
{
    std::vector<DynamicAudioMapping> all;
    all.reserve(200);
    for (uint16_t ch = 0; ch < 200; ++ch) {
        all.push_back(m(0, static_cast<uint16_t>(ch % 4), 0, ch));
    }
    return all;
}

}  // namespace

// The commands answered NOT_IMPLEMENTED before the entity has dynamic ports to keep; so they still
// are for an entity that gives the handler no store.
TEST(nanoavb_audio_map, without_a_store_get_audio_map_is_not_implemented)
{
    Bench b;
    b.aem.set_dynamic_audio_maps(nullptr);
    EXPECT_EQ(b.get_map(0, 0).status, AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_TRUE(b.aem.dynamic_audio_maps() == nullptr);
}

// IEEE 1722.1-2021 Clause 7.4.44, Milan v1.3 Section 5.4.2.26: a dynamic port answers its maps;
// a static one is NOT_SUPPORTED, a port the model lacks NO_SUCH_DESCRIPTOR.
TEST(nanoavb_audio_map, a_dynamic_port_answers_its_maps)
{
    Bench b;
    auto const empty = b.get_map(0, 0);
    EXPECT_EQ(empty.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(empty.number_of_maps, uint16_t{1});
    EXPECT_TRUE(empty.mappings.empty());
    std::vector<DynamicAudioMapping> const held{m(0, 3, 0), m(0, 1, 1), m(0, 1, 2)};
    EXPECT_TRUE(b.maps->replace(DESCRIPTOR_STREAM_PORT_INPUT, 0, held));
    EXPECT_TRUE(b.get_map(0, 0).mappings == held);
    EXPECT_EQ(b.get_map(0, 1).status, AEM_STATUS_BAD_ARGUMENTS);  // one partition only
    EXPECT_EQ(b.get_map(2, 0).status, AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(b.get_map(3, 0).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_TRUE(b.aem.has_dynamic_map(DESCRIPTOR_STREAM_PORT_INPUT, 0));
    EXPECT_FALSE(b.aem.has_dynamic_map(DESCRIPTOR_STREAM_PORT_INPUT, 2));
    EXPECT_TRUE(b.aem.has_dynamic_map(DESCRIPTOR_STREAM_PORT_OUTPUT, 0));
    EXPECT_FALSE(b.aem.has_dynamic_map(DESCRIPTOR_STREAM_PORT_OUTPUT, 1));
    EXPECT_TRUE(std::ranges::none_of(b.sent, [](Sent const& s) { return s.header.is_unsolicited(); }));
}

// Milan v1.3 Sections 5.4.1 and 5.4.2.26: under milan13, 200 cluster channels make two fixed
// partitions (176 + 24); number_of_maps is 2 whatever is mapped; map_index P answers the
// mappings of partition P only.
TEST(nanoavb_audio_map, partitions_of_176_cluster_channels)
{
    Bench b;
    auto empty = b.get_map(1, 1);
    EXPECT_EQ(empty.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(empty.number_of_maps, uint16_t{2});
    EXPECT_TRUE(empty.mappings.empty());
    EXPECT_EQ(b.get_map(1, 2).status, AEM_STATUS_BAD_ARGUMENTS);

    EXPECT_TRUE(b.maps->replace(DESCRIPTOR_STREAM_PORT_INPUT, 1, all_of_port_1()));
    auto const p0 = b.get_map(1, 0);
    auto const p1 = b.get_map(1, 1);
    EXPECT_EQ(p0.number_of_maps, uint16_t{2});
    EXPECT_EQ(p0.mappings.size(), size_t{176});
    EXPECT_EQ(p1.mappings.size(), size_t{24});
    EXPECT_TRUE(p0.mappings.front() == m(0, 0, 0, 0) && p0.mappings.back() == m(0, 175 % 4, 0, 175));
    EXPECT_TRUE(p1.mappings.front() == m(0, 176 % 4, 0, 176) && p1.mappings.back() == m(0, 199 % 4, 0, 199));
    EXPECT_TRUE(b.get_map(0, 0).mappings.empty());  // port 0 is a port of its own
}

// IEEE 1722.1-2021 Clause 9.2.1.1.7: under classic a partition stays within 524 octets of control
// data, so 200 cluster channels make four (62 + 62 + 62 + 14).
TEST(nanoavb_audio_map, a_classic_partition_fits_an_aecpdu)
{
    Bench b;
    b.aem.set_profile(AemCommandHandler::Profile::classic);
    EXPECT_TRUE(b.maps->replace(DESCRIPTOR_STREAM_PORT_INPUT, 1, all_of_port_1()));
    auto const p0 = b.get_map(1, 0);
    EXPECT_EQ(p0.number_of_maps, uint16_t{4});
    EXPECT_EQ(p0.mappings.size(), AemCommandHandler::AUDIO_MAP_MAPPINGS);
    EXPECT_EQ(AemAudioMapResponseHeader::LENGTH + (p0.mappings.size() * AemAudioMapping::LENGTH) <= MAX_AEM_BODY, true);
    auto const p3 = b.get_map(1, 3);
    EXPECT_EQ(p3.mappings.size(), size_t{14});
    EXPECT_TRUE(p3.mappings.back() == m(0, 199 % 4, 0, 199));
    EXPECT_EQ(b.get_map(1, 4).status, AEM_STATUS_BAD_ARGUMENTS);
}

// Milan v1.3 Section 5.4.2.26: an output port partitions the Stream Outputs' channels, each stream
// counted at the most channels any format it lists has (8 + 200 + 8 here), so a format change
// moves no partition: 176 + 40 under milan13.
TEST(nanoavb_audio_map, an_output_port_is_partitioned_by_stream_channel)
{
    Bench b;
    std::vector<DynamicAudioMapping> const held{m(0, 3, 0), m(1, 167, 1), m(1, 168, 0), m(2, 0, 1)};
    EXPECT_TRUE(b.maps->replace(DESCRIPTOR_STREAM_PORT_OUTPUT, 0, held));
    auto const p0 = b.get_map(0, 0, DESCRIPTOR_STREAM_PORT_OUTPUT);
    auto const p1 = b.get_map(0, 1, DESCRIPTOR_STREAM_PORT_OUTPUT);
    EXPECT_EQ(p0.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(p0.number_of_maps, uint16_t{2});
    EXPECT_TRUE(p0.mappings == (std::vector<DynamicAudioMapping>{m(0, 3, 0), m(1, 167, 1)}));
    EXPECT_TRUE(p1.mappings == (std::vector<DynamicAudioMapping>{m(1, 168, 0), m(2, 0, 1)}));
    EXPECT_EQ(b.get_map(0, 2, DESCRIPTOR_STREAM_PORT_OUTPUT).status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.get_map(0, 0).mappings.empty());  // the input ports' are apart
}

// IEEE 1722.1-2021 Clauses 7.4.45, 7.4.46: an ADD and a REMOVE on a dynamic input port, each put in
// force through the data plane, answered first and notified to the other controller only.
TEST(nanoavb_audio_map, add_get_remove_and_notify)
{
    Bench b;
    // Channel 3 to cluster 0, channel 1 fanned out to clusters 1 and 2.
    EXPECT_EQ(b.mappings(true, 0, {m(0, 3, 0), m(0, 1, 2), m(0, 1, 1)}), AEM_STATUS_SUCCESS);
    std::vector<DynamicAudioMapping> const in_order{m(0, 3, 0), m(0, 1, 1), m(0, 1, 2)};
    EXPECT_TRUE(b.applied == std::vector<std::vector<DynamicAudioMapping>>{in_order});
    EXPECT_TRUE((b.applied_to.front() == std::pair<uint16_t, uint16_t>{DESCRIPTOR_STREAM_PORT_INPUT, 0}));
    EXPECT_TRUE(b.get_map(0, 0).mappings == in_order);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_A), 0);  // the requester has the response

    // The same mapping again changes nothing: no data plane call, no
    // notification; nor does a refused REMOVE of what is not there.
    EXPECT_EQ(b.mappings(true, 0, {m(0, 3, 0)}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.mappings(false, 0, {m(0, 2, 3)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(b.applied.size(), size_t{1});
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 0);

    // REMOVE: duplicates ignored.
    EXPECT_EQ(b.mappings(false, 0, {m(0, 1, 1), m(0, 1, 1)}), AEM_STATUS_SUCCESS);
    std::vector<DynamicAudioMapping> const left{m(0, 3, 0), m(0, 1, 2)};
    EXPECT_TRUE(b.get_map(0, 0).mappings == left);
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_EQ(b.applied.size(), size_t{2});

    // A static map, and no such port.
    EXPECT_EQ(b.mappings(true, 2, {m(0, 0, 0)}), AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(b.mappings(true, 3, {m(0, 0, 0)}), AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

// IEEE 1722.1-2021 Clause 7.4.46.1: a REMOVE listing one mapping the port does not hold removes none.
TEST(nanoavb_audio_map, a_remove_listing_a_mapping_not_held_removes_nothing)
{
    Bench b;
    std::vector<DynamicAudioMapping> const held{m(0, 0, 0), m(0, 1, 1)};
    EXPECT_EQ(b.mappings(true, 0, held), AEM_STATUS_SUCCESS);
    b.sent.clear();
    EXPECT_EQ(b.mappings(false, 0, {m(0, 0, 0), m(0, 1, 1), m(0, 2, 2)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.get_map(0, 0).mappings == held);
    EXPECT_EQ(b.applied.size(), size_t{1});
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 0);
}

// IEEE 1722.1-2021 Clause 7.4.46.1: a REMOVE on a port holding none of the listed mappings is refused.
TEST(nanoavb_audio_map, a_remove_on_an_empty_port_is_refused)
{
    Bench b;
    EXPECT_EQ(b.mappings(false, 0, {m(0, 0, 0)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.applied.empty());
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 0);
}

// Milan v1.3 Sections 5.4.2.28 and 5.4.5.3: a REMOVE listing each held mapping twice removes them
// and is notified to the other registered controllers.
TEST(nanoavb_audio_map, a_remove_listing_mappings_twice_removes_them_and_is_notified)
{
    Bench b;
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0), m(0, 1, 1)}), AEM_STATUS_SUCCESS);
    b.sent.clear();
    EXPECT_EQ(b.mappings(false, 0, {m(0, 0, 0), m(0, 0, 0), m(0, 1, 1), m(0, 1, 1)}), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(b.get_map(0, 0).mappings.empty());
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 1);
}

// Milan v1.3 Section 5.4.2.27: each rule refuses BAD_ARGUMENTS and adds nothing.
TEST(nanoavb_audio_map, every_bad_arguments_rule_adds_nothing)
{
    Bench b;
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0)}), AEM_STATUS_SUCCESS);
    b.applied.clear();
    std::vector<DynamicAudioMapping> const before{m(0, 0, 0)};
    auto const refused = [&b, &before](std::vector<DynamicAudioMapping> const& list) {
        // A valid mapping first: the whole command is refused, not just the bad one.
        auto with_good = list;
        with_good.insert(with_good.begin(), m(0, 2, 3));
        EXPECT_EQ(b.mappings(true, 0, with_good), AEM_STATUS_BAD_ARGUMENTS);
        EXPECT_TRUE(b.get_map(0, 0).mappings == before);
    };
    refused({m(0, 4, 1)});     // a stream channel past the current format's 4
    refused({m(3, 0, 1)});     // no such stream input
    refused({m(2, 0, 1)});     // a CRF stream: no channels
    refused({m(0, 0, 4)});     // no cluster at offset 4 of port 0
    refused({m(0, 0, 1, 1)});  // cluster 1 has one channel
    refused({m(1, 0, 1)});     // stream 1 runs at 96 kHz, the unit at 48 kHz
    // Two in one command naming one cluster channel from different stream channels.
    refused({m(0, 1, 2), m(0, 2, 2)});
    // A cluster channel already mapped, from another stream channel.
    refused({m(0, 1, 0)});
    EXPECT_TRUE(b.applied.empty());
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 1);  // only the first ADD
    // A count past the command's data.
    std::array<uint8_t, 16> short_body{0x00, 0x0E, 0x00, 0x00, 0x00, 0x05};
    EXPECT_EQ(b.command(CONTROLLER_A, MAC_A, AEM_COMMAND_ADD_AUDIO_MAPPINGS, short_body).header.status(), AEM_STATUS_BAD_ARGUMENTS);
    // REMOVE of a mapping naming nothing in the model.
    EXPECT_EQ(b.mappings(false, 0, {m(0, 0, 0), m(0, 0, 9)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.get_map(0, 0).mappings == before);

    // A port that converts rates accepts the 96 kHz stream (Section 5.4.2.27's
    // "if none of the SSRC and ASRC bits are set").
    b.model.port0_flags = PORT_FLAG_ASYNC_SAMPLE_RATE_CONV;
    EXPECT_EQ(b.mappings(true, 0, {m(1, 7, 1)}), AEM_STATUS_SUCCESS);
}

// Atomic: the data plane refusing leaves the mappings as they were; the lock
// keeps every other controller out.
TEST(nanoavb_audio_map, refused_by_the_data_plane_or_the_lock_changes_nothing)
{
    Bench b;
    b.verdict = AEM_STATUS_BAD_ARGUMENTS;
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0), m(0, 1, 1)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.get_map(0, 0).mappings.empty());
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 0);
    b.verdict = AEM_STATUS_SUCCESS;
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0)}), AEM_STATUS_SUCCESS);

    b.lock_for_b();
    EXPECT_EQ(b.mappings(true, 0, {m(0, 1, 1)}), AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(b.mappings(false, 0, {m(0, 0, 0)}), AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(b.get_map(0, 0).mappings.size(), size_t{1});
    EXPECT_EQ(b.mappings(true, 0, {m(0, 1, 1)}, CONTROLLER_B), AEM_STATUS_SUCCESS);  // the holder may

    // Without a data plane hook, or a store, the commands are not implemented.
    b.aem.set_on_audio_mappings_changed({});
    EXPECT_EQ(b.mappings(true, 0, {m(0, 2, 2)}, CONTROLLER_B), AEM_STATUS_NOT_IMPLEMENTED);
    b.aem.set_dynamic_audio_maps(nullptr);
    EXPECT_EQ(b.mappings(false, 2, {m(0, 0, 0)}, CONTROLLER_B), AEM_STATUS_NOT_IMPLEMENTED);
}

// Every port's mappings share the store: an ADD that does not fit beside the other ports' is
// NO_RESOURCES, the port and the data plane left as they were.
TEST(nanoavb_audio_map, an_add_past_the_stores_capacity_is_refused)
{
    Bench b;
    std::vector<DynamicAudioMapping> others;
    others.reserve(DynamicAudioMaps::MAX_MAPPINGS - 1);
    for (uint16_t i = 0; i < DynamicAudioMaps::MAX_MAPPINGS - 1; ++i) {
        others.push_back(m(0, 0, 0, i));
    }
    EXPECT_TRUE(b.maps->replace(DESCRIPTOR_STREAM_PORT_INPUT, 9, others));  // a port the entity seeded
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0)}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.mappings(true, 0, {m(0, 1, 1)}), AEM_STATUS_NO_RESOURCES);
    EXPECT_EQ(b.applied.size(), size_t{1});
    EXPECT_TRUE(b.get_map(0, 0).mappings == (std::vector<DynamicAudioMapping>{m(0, 0, 0)}));
    EXPECT_EQ(b.maps->size(), DynamicAudioMaps::MAX_MAPPINGS);
}

// Milan v1.3 Section 5.4.5.3: the largest ADD an AECPDU carries (63 mappings, 512 octets) is
// answered first, then notified to the other controller as the command it was.
TEST(nanoavb_audio_map, a_full_add_is_answered_before_it_is_notified)
{
    Bench b;
    std::vector<DynamicAudioMapping> full;
    full.reserve(63);
    for (uint16_t ch = 0; ch < 63; ++ch) {
        full.push_back(m(0, static_cast<uint16_t>(ch % 4), 0, ch));
    }
    EXPECT_EQ(b.mappings(true, 1, full), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.sent.size(), size_t{2});
    if (b.sent.size() != 2) {
        return;
    }
    EXPECT_FALSE(b.sent[0].header.is_unsolicited());
    EXPECT_TRUE(b.sent[0].dest == MAC_A);
    EXPECT_TRUE(b.sent[1].header.is_unsolicited());
    EXPECT_TRUE(b.sent[1].dest == MAC_B);
    EXPECT_EQ(b.sent[1].body.size(), MAX_AEM_BODY);
    EXPECT_TRUE(b.sent[1].body == b.sent[0].body);
}

TEST_MAIN(statusbar_nanoavb, nanoavb_audio_map_test)
