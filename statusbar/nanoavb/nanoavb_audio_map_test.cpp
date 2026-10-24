// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Dynamic audio mappings on a STREAM_PORT_INPUT (Milan v1.2 Sections 5.3.10,
// 5.4.2.26-28): partitions, BAD_ARGUMENTS rules, atomicity, the lock and notifications;
// a dynamic STREAM_PORT_OUTPUT's, the entity's own.

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <span>
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
constexpr uint64_t CRF_48K = 0x041060010000BB80ULL;

/// AUDIO_UNIT 0 at 48 kHz; port 0: four 1-channel clusters, port 1: one 200-channel cluster,
/// port 2: a static map. STREAM_INPUT 0: 4 ch 48 kHz, 1: 8 ch 96 kHz, 2: CRF. STREAM_PORT_OUTPUT 0:
/// two 1-channel clusters, dynamic; STREAM_OUTPUT 0: 4 ch 48 kHz.
class Model final : public AemEntityHandler
{
  public:
    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override
    {
        if (id.ref.descriptor_index != 0) {
            return false;
        }
        desc.number_of_stream_input_ports = uint16_t{3};
        desc.base_stream_input_port = uint16_t{0};
        desc.number_of_stream_output_ports = uint16_t{1};
        desc.current_sampling_rate = uint32_t{48000};
        return true;
    }
    auto on_get_stream_port(DescriptorId id, DescriptorStreamPort& desc) -> bool override
    {
        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_PORT_OUTPUT && id.ref.descriptor_index == 0) {
            desc.number_of_clusters = uint16_t{2};
            desc.base_cluster = uint16_t{6};
            return true;
        }
        if (id.ref.descriptor_type != DESCRIPTOR_STREAM_PORT_INPUT || id.ref.descriptor_index > 2) {
            return false;
        }
        switch (id.ref.descriptor_index) {
            case 0:
                desc.number_of_clusters = uint16_t{4};
                desc.base_cluster = uint16_t{0};
                desc.port_flags = port0_flags;
                break;
            case 1:
                desc.number_of_clusters = uint16_t{1};
                desc.base_cluster = uint16_t{4};
                break;
            default:
                desc.number_of_clusters = uint16_t{1};
                desc.base_cluster = uint16_t{5};
                desc.number_of_maps = uint16_t{1};
                break;
        }
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
        if (id.ref.descriptor_type == DESCRIPTOR_STREAM_OUTPUT && id.ref.descriptor_index == 0) {
            desc.current_format.from_uint64(AAF_4CH_48K);
            return true;
        }
        if (id.ref.descriptor_type != DESCRIPTOR_STREAM_INPUT || id.ref.descriptor_index > 2) {
            return false;
        }
        uint64_t const formats[] = {AAF_4CH_48K, AAF_8CH_96K, CRF_48K};
        desc.current_format.from_uint64(formats[id.ref.descriptor_index]);
        return true;
    }

    uint16_t port0_flags{0};
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
    std::vector<Sent> sent;
    /// What the data plane was handed, and what it answers.
    std::vector<std::vector<DynamicAudioMapping>> applied;
    uint8_t verdict{AEM_STATUS_SUCCESS};

    Bench()
    {
        aem.set_entity_id(US);
        aem.set_send_response([this](Eui48 const& dest, std::span<uint8_t const> bytes) {
            Sent s{.dest = dest};
            span_load(s.header, bytes.first(AemDu::LENGTH));
            s.body.assign(bytes.begin() + AemDu::LENGTH, bytes.end());
            sent.push_back(std::move(s));
            return true;
        });
        aem.set_on_audio_mappings_changed([this](uint16_t, uint16_t, std::span<DynamicAudioMapping const> m) {
            applied.emplace_back(m.begin(), m.end());
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
            from, from == CONTROLLER_A ? MAC_A : MAC_B, add ? AEM_COMMAND_ADD_AUDIO_MAPPINGS : AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, body);
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
};

auto m(uint16_t const stream, uint16_t const stream_ch, uint16_t const cluster, uint16_t const cluster_ch = 0) -> DynamicAudioMapping
{
    return {.stream_index = stream, .stream_channel = stream_ch, .cluster_offset = cluster, .cluster_channel = cluster_ch};
}

}  // namespace

TEST(audio_map, add_get_remove_and_notify)
{
    Bench b;
    auto map = b.get_map(0, 0);
    EXPECT_EQ(map.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(map.number_of_maps, uint16_t{1});
    EXPECT_TRUE(map.mappings.empty());

    // Channel 3 to cluster 0, channel 1 fanned out to clusters 1 and 2.
    EXPECT_EQ(b.mappings(true, 0, {m(0, 3, 0), m(0, 1, 2), m(0, 1, 1)}), AEM_STATUS_SUCCESS);
    std::vector<DynamicAudioMapping> const in_order{m(0, 3, 0), m(0, 1, 1), m(0, 1, 2)};
    EXPECT_TRUE(b.applied.size() == 1 && b.applied.back() == in_order);
    EXPECT_TRUE(b.get_map(0, 0).mappings == in_order);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_A), 0);  // the requester has the response

    // The same mapping again changes nothing: no data plane call, no
    // notification, nothing to save (as a REMOVE of what is not there).
    int saves = 0;
    b.aem.set_on_settings_changed([&saves](uint16_t) { ++saves; });
    EXPECT_EQ(b.mappings(true, 0, {m(0, 3, 0)}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.mappings(false, 0, {m(0, 2, 3)}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.applied.size(), size_t{1});
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_EQ(saves, 0);

    // REMOVE: duplicates and absent mappings ignored.
    EXPECT_EQ(b.mappings(false, 0, {m(0, 1, 1), m(0, 1, 1), m(0, 2, 3)}), AEM_STATUS_SUCCESS);
    std::vector<DynamicAudioMapping> const left{m(0, 3, 0), m(0, 1, 2)};
    EXPECT_TRUE(b.get_map(0, 0).mappings == left);
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_EQ(saves, 1);  // a change is saved

    // Other ports and kinds: a static map, no such port, an output.
    EXPECT_EQ(b.get_map(2, 0).status, AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(b.mappings(true, 2, {m(0, 0, 0)}), AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(b.get_map(3, 0).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(b.mappings(true, 3, {m(0, 0, 0)}), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(b.get_map(0, 1).status, AEM_STATUS_BAD_ARGUMENTS);  // one partition only
}

// Milan v1.2 Section 5.4.2.27: each rule refuses BAD_ARGUMENTS and adds nothing.
TEST(audio_map, every_bad_arguments_rule_adds_nothing)
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

    // A port that converts rates takes the 96 kHz stream (Section 5.4.2.27's
    // "unless SSRC/ASRC").
    b.model.port0_flags = 0x0002;  // ASYNC_SAMPLE_RATE_CONV
    EXPECT_EQ(b.mappings(true, 0, {m(1, 7, 1)}), AEM_STATUS_SUCCESS);
}

// Atomic: the data plane refusing leaves the mappings as they were; the lock
// keeps every other controller out.
TEST(audio_map, refused_by_the_data_plane_or_the_lock_changes_nothing)
{
    Bench b;
    b.verdict = AEM_STATUS_BAD_ARGUMENTS;
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0), m(0, 1, 1)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.get_map(0, 0).mappings.empty());
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 0);
    b.verdict = AEM_STATUS_SUCCESS;
    EXPECT_EQ(b.mappings(true, 0, {m(0, 0, 0)}), AEM_STATUS_SUCCESS);

    AemLockEntityPayload lock{};
    std::array<uint8_t, AemLockEntityPayload::LENGTH> lock_body{};
    span_store(lock_body, lock);
    EXPECT_EQ(b.command(CONTROLLER_B, MAC_B, AEM_COMMAND_LOCK_ENTITY, lock_body).header.status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.mappings(true, 0, {m(0, 1, 1)}), AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(b.mappings(false, 0, {m(0, 0, 0)}), AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(b.get_map(0, 0).mappings.size(), size_t{1});
    EXPECT_EQ(b.mappings(true, 0, {m(0, 1, 1)}, CONTROLLER_B), AEM_STATUS_SUCCESS);  // the holder may

    // Without a data plane hook the commands are not implemented.
    b.aem.set_on_audio_mappings_changed({});
    EXPECT_EQ(b.mappings(true, 0, {m(0, 2, 2)}, CONTROLLER_B), AEM_STATUS_NOT_IMPLEMENTED);
}

// Milan v1.2 Section 5.4.2.26: 200 cluster channels make two fixed partitions
// (176 + 24); number_of_maps is 2 whatever is mapped; map_index P answers the
// mappings of partition P only.
TEST(audio_map, partitions_of_176_cluster_channels)
{
    Bench b;
    auto empty = b.get_map(1, 1);
    EXPECT_EQ(empty.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(empty.number_of_maps, uint16_t{2});
    EXPECT_TRUE(empty.mappings.empty());
    EXPECT_EQ(b.get_map(1, 2).status, AEM_STATUS_BAD_ARGUMENTS);

    // Every cluster channel, fed from stream 0's four (fan-out), in chunks of
    // 50 (an AECPDU holds at most 63).
    for (uint16_t first = 0; first < 200; first += 50) {
        std::vector<DynamicAudioMapping> chunk;
        for (uint16_t ch = first; ch < first + 50; ++ch) {
            chunk.push_back(m(0, static_cast<uint16_t>(ch % 4), 0, ch));
        }
        EXPECT_EQ(b.mappings(true, 1, chunk), AEM_STATUS_SUCCESS);
    }
    auto const p0 = b.get_map(1, 0);
    auto const p1 = b.get_map(1, 1);
    EXPECT_EQ(p0.number_of_maps, uint16_t{2});
    EXPECT_EQ(p0.mappings.size(), size_t{176});
    EXPECT_EQ(p1.mappings.size(), size_t{24});
    EXPECT_TRUE(p0.mappings.front() == m(0, 0, 0, 0) && p0.mappings.back() == m(0, 175 % 4, 0, 175));
    EXPECT_TRUE(p1.mappings.front() == m(0, 176 % 4, 0, 176) && p1.mappings.back() == m(0, 199 % 4, 0, 199));
    // Port 0 is unaffected.
    EXPECT_TRUE(b.get_map(0, 0).mappings.empty());
}

// set_audio_mappings (an entity's default, a restore): validated as an ADD
// onto an empty port; with apply, through the data plane; no notification.
TEST(audio_map, set_audio_mappings_for_defaults_and_restores)
{
    Bench b;
    std::vector<DynamicAudioMapping> const identity{m(0, 0, 0), m(0, 1, 1), m(0, 2, 2), m(0, 3, 3)};
    EXPECT_EQ(b.aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, 0, identity, false), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(b.applied.empty());
    EXPECT_TRUE(b.get_map(0, 0).mappings == identity);
    std::vector<DynamicAudioMapping> const swapped{m(0, 1, 0), m(0, 0, 1)};
    EXPECT_EQ(b.aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, 0, swapped, true), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(b.applied.size() == 1 && b.applied.back() == swapped);
    EXPECT_TRUE(b.get_map(0, 0).mappings == swapped);
    std::vector<DynamicAudioMapping> const bad{m(0, 0, 0), m(0, 9, 1)};
    EXPECT_EQ(b.aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, 0, bad, true), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(b.get_map(0, 0).mappings == swapped);
    EXPECT_EQ(b.aem.set_audio_mappings(DESCRIPTOR_STREAM_PORT_INPUT, 2, identity, false), AEM_STATUS_NOT_SUPPORTED);
    EXPECT_TRUE(b.aem.has_dynamic_input_map(DESCRIPTOR_STREAM_PORT_INPUT, 0));
    EXPECT_FALSE(b.aem.has_dynamic_input_map(DESCRIPTOR_STREAM_PORT_INPUT, 2));
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 0);
}

// apply_local_audio_mappings: a REMOVE then an ADD by the entity, one data plane call,
// every controller told what changed (63 mappings an AECPDU), saved; locked out.
TEST(audio_map, a_local_change_is_routed_once_told_in_pdus_and_saved)
{
    Bench b;
    int saves = 0;
    uint16_t saved_as = 0;
    b.aem.set_on_settings_changed([&](uint16_t const command) {
        ++saves;
        saved_as = command;
    });
    std::vector<DynamicAudioMapping> hundred;
    for (uint16_t ch = 0; ch < 100; ++ch) {
        hundred.push_back(m(0, static_cast<uint16_t>(ch % 4), 0, ch));
    }
    EXPECT_EQ(b.aem.apply_local_audio_mappings(1, hundred, {}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.applied.size(), size_t{1});
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_A), 2);  // 63 + 37
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 2);
    EXPECT_EQ(saves, 1);
    size_t told = 0;
    for (auto const& s : b.sent) {
        AemAudioMappingsCommandHeader header{};
        span_load(header, std::span<uint8_t const>{s.body}.first(AemAudioMappingsCommandHeader::LENGTH));
        EXPECT_EQ(s.body.size(), AemAudioMappingsCommandHeader::LENGTH + (size_t{header.number_of_mappings.get()} * 8));
        told += s.dest == MAC_A ? header.number_of_mappings.get() : 0U;
    }
    EXPECT_EQ(told, size_t{100});
    EXPECT_EQ(saved_as, AEM_COMMAND_ADD_AUDIO_MAPPINGS);

    // Remap cluster channel 0 from stream channel 1: a REMOVE and an ADD of one each.
    b.sent.clear();
    std::vector<DynamicAudioMapping> const old_one{m(0, 0, 0, 0)};
    std::vector<DynamicAudioMapping> const new_one{m(0, 1, 0, 0)};
    EXPECT_EQ(b.aem.apply_local_audio_mappings(1, new_one, old_one), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.applied.size(), size_t{2});
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_A), 1);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_A), 1);
    EXPECT_TRUE(b.get_map(1, 0).mappings.front() == new_one.front());
    EXPECT_EQ(saves, 2);

    // The same again changes nothing; a bad ADD refuses the REMOVE with it.
    b.sent.clear();
    EXPECT_EQ(b.aem.apply_local_audio_mappings(1, new_one, {}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.aem.apply_local_audio_mappings(1, std::vector{m(0, 9, 0, 1)}, new_one), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(b.aem.apply_local_audio_mappings(2, new_one, {}), AEM_STATUS_NOT_SUPPORTED);  // static maps
    EXPECT_TRUE(b.sent.empty());
    EXPECT_EQ(b.applied.size(), size_t{2});
    EXPECT_EQ(saves, 2);
    // Only removed: saved as a REMOVE.
    EXPECT_EQ(b.aem.apply_local_audio_mappings(1, {}, std::vector{m(0, 1, 0, 1)}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(saved_as, AEM_COMMAND_REMOVE_AUDIO_MAPPINGS);

    AemLockEntityPayload lock{};
    std::array<uint8_t, AemLockEntityPayload::LENGTH> lock_body{};
    span_store(lock_body, lock);
    EXPECT_EQ(b.command(CONTROLLER_B, MAC_B, AEM_COMMAND_LOCK_ENTITY, lock_body).header.status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.aem.apply_local_audio_mappings(1, {}, new_one), AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(b.get_map(1, 0).mappings.size(), size_t{99});
}

// A dynamic output port's mappings are the entity's own (Milan v1.2 Section 5.3.9.1):
// set and validated by it, told to every controller, served by GET_AUDIO_MAP,
// never changed by a controller
TEST(audio_map, an_output_port_carries_the_entitys_own_mappings)
{
    Bench b;
    EXPECT_TRUE(b.aem.has_dynamic_output_map(0));
    EXPECT_FALSE(b.aem.has_dynamic_output_map(1));
    EXPECT_TRUE(b.get_map(0, 0, DESCRIPTOR_STREAM_PORT_OUTPUT).mappings.empty());
    std::vector<DynamicAudioMapping> const pair{m(0, 0, 0), m(0, 1, 1)};
    EXPECT_EQ(b.aem.set_output_mappings(0, pair), AEM_STATUS_SUCCESS);
    EXPECT_TRUE(b.get_map(0, 0, DESCRIPTOR_STREAM_PORT_OUTPUT).mappings == pair);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_A), 1);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_TRUE(b.applied.empty());  // nothing for the input hook
    // The same set again is told nothing; a channel the stream lacks, a stream that is
    // not there, or no such port changes nothing.
    EXPECT_EQ(b.aem.set_output_mappings(0, pair), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.notified(AEM_COMMAND_ADD_AUDIO_MAPPINGS, MAC_A), 1);
    EXPECT_EQ(b.aem.set_output_mappings(0, std::vector{m(0, 4, 0)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(b.aem.set_output_mappings(0, std::vector{m(1, 0, 0)}), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(b.aem.set_output_mappings(1, pair), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_TRUE(b.get_map(0, 0, DESCRIPTOR_STREAM_PORT_OUTPUT).mappings == pair);
    // A controller's ADD or REMOVE is NOT_SUPPORTED.
    EXPECT_EQ(b.mappings(false, 0, pair, CONTROLLER_A, DESCRIPTOR_STREAM_PORT_OUTPUT), AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(b.mappings(true, 0, {m(0, 2, 0)}, CONTROLLER_A, DESCRIPTOR_STREAM_PORT_OUTPUT), AEM_STATUS_NOT_SUPPORTED);
    // Emptied: a REMOVE told; the inputs' mappings are a store of their own.
    EXPECT_EQ(b.aem.set_output_mappings(0, {}), AEM_STATUS_SUCCESS);
    EXPECT_EQ(b.notified(AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, MAC_B), 1);
    EXPECT_TRUE(b.get_map(0, 0, DESCRIPTOR_STREAM_PORT_OUTPUT).mappings.empty());
    EXPECT_EQ(b.aem.dynamic_audio_maps().size(), size_t{0});
}

TEST_MAIN(statusbar_nanoavb, nanoavb_audio_map_test)
