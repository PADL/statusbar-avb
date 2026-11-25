// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Unit tests for the --enumerate descriptor walk's in-flight accounting.

#include "statusbar/atdecc_tools/atdecc_enumerator.hpp"

#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::atdecc_tools;
using statusbar::ieee::Eui64;

namespace {

Eui64 const ENTITY_A{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x01};

void put_be16(std::vector<uint8_t>& v, size_t off, uint16_t value)
{
    ieee::doublet_t const d{value};
    span_store(std::span{v}.subspan(off, 2), d);
}

/// A READ_DESCRIPTOR response: the 4-byte header, then a body of `body_size`
/// zero bytes with its type and index first.
auto read_response(uint16_t type, uint16_t index, size_t body_size) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data(AemReadDescriptorResponsePayload::LENGTH + body_size, 0);
    put_be16(data, AemReadDescriptorResponsePayload::LENGTH + 0, type);
    put_be16(data, AemReadDescriptorResponsePayload::LENGTH + 2, index);
    return data;
}

/// A successful READ_DESCRIPTOR response carrying @p descriptor.
template <class T>
auto response_with(T const& descriptor) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data(AemReadDescriptorResponsePayload::LENGTH + sizeof(T), 0);
    span_store(std::span{data}.subspan(AemReadDescriptorResponsePayload::LENGTH), descriptor);
    return data;
}

/// A CONFIGURATION whose descriptor_counts are @p entries of (type, count).
auto configuration_listing(std::vector<std::pair<uint16_t, uint16_t>> const& entries) -> std::vector<uint8_t>
{
    DescriptorConfiguration cfg{};
    cfg.descriptor_counts_count = ieee::doublet_t{static_cast<uint16_t>(entries.size())};
    for (size_t i = 0; i < entries.size(); ++i) {
        cfg.descriptor_counts[i].descriptor_type = ieee::doublet_t{entries[i].first};
        cfg.descriptor_counts[i].count = ieee::doublet_t{entries[i].second};
    }
    return response_with(cfg);
}

using Reads = std::vector<std::pair<uint16_t, uint16_t>>;

struct Walk
{
    std::vector<std::tuple<Eui64, uint16_t, uint16_t>> issued;
    int printed{0};
    Enumerator e;

    Walk()
        : e{ENTITY_A, 0, hooks()}
    {}

    /// Issue what is queued: the (type, index) of each read that went out.
    auto pump() -> Reads
    {
        auto const before = issued.size();
        e.pump();
        Reads reads;
        for (auto i = before; i < issued.size(); ++i) {
            reads.emplace_back(std::get<1>(issued[i]), std::get<2>(issued[i]));
        }
        return reads;
    }

    void answer(std::vector<uint8_t> const& response)
    {
        e.on_response(ENTITY_A, AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_SUCCESS, response);
    }

    /// Discover the entity and read it down to a CONFIGURATION listing @p entries.
    void read_to_configuration(std::vector<std::pair<uint16_t, uint16_t>> const& entries)
    {
        DiscoveredEntity entity{};
        entity.adpdu.entity_id = ENTITY_A;
        e.on_entity(entity, 0);
        (void)pump();
        answer(one_configuration_entity());
        (void)pump();
        answer(configuration_listing(entries));
    }

    static auto one_configuration_entity() -> std::vector<uint8_t>
    {
        auto ent = read_response(DESCRIPTOR_ENTITY, 0, DescriptorEntity::LENGTH);
        put_be16(ent, AemReadDescriptorResponsePayload::LENGTH + 308, 1);
        return ent;
    }

    auto hooks() -> EnumeratorHooks
    {
        EnumeratorHooks h{};
        h.read_descriptor = [this](Eui64 const& t, uint16_t type, uint16_t index) {
            issued.emplace_back(t, type, index);
            return true;
        };
        h.on_descriptor = [this](Eui64 const&, std::span<uint8_t const>) { ++printed; };
        return h;
    }
};

}  // namespace

// The walk counts a read out only for a READ_DESCRIPTOR it counted in: a
// response or timeout for any other command leaves the count alone. It used to
// release a slot for every timeout and every error response, including the
// host's own reads, and could report the walk finished with reads in flight.
TEST(atdecc_enumerator, counts_only_the_reads_it_issued)
{
    Walk w;
    DiscoveredEntity entity{};
    entity.adpdu.entity_id = ENTITY_A;

    // Discovery queues the ENTITY read once, however often ADP re-announces.
    w.e.on_entity(entity, 0);
    w.e.on_entity(entity, 1);
    EXPECT_EQ(w.e.outstanding(), 0);
    w.e.pump();
    EXPECT_EQ(w.issued.size(), 1U);
    EXPECT_EQ(w.e.outstanding(), 1);
    EXPECT_FALSE(w.e.finished(1));

    // A timeout and a response for a command the walk never issued: no release.
    w.e.on_timeout(ENTITY_A, AEM_COMMAND_GET_COUNTERS);
    w.e.on_response(ENTITY_A, AEM_COMMAND_GET_COUNTERS, AEM_STATUS_SUCCESS, {});
    EXPECT_EQ(w.e.outstanding(), 1);
    EXPECT_FALSE(w.e.finished(1));

    // ENTITY with one configuration: released, printed, CONFIGURATION 0 queued.
    auto ent = read_response(DESCRIPTOR_ENTITY, 0, DescriptorEntity::LENGTH);
    put_be16(ent, AemReadDescriptorResponsePayload::LENGTH + 308, 1);
    w.e.on_response(ENTITY_A, AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_SUCCESS, ent);
    EXPECT_EQ(w.e.outstanding(), 0);
    EXPECT_EQ(w.printed, 1);
    EXPECT_FALSE(w.e.finished(2));  // CONFIGURATION still queued
    w.e.pump();
    EXPECT_EQ(w.issued.size(), 2U);
    EXPECT_EQ(w.e.outstanding(), 1);

    // CONFIGURATION with two CONTROLs: two more reads.
    auto cfg = read_response(DESCRIPTOR_CONFIGURATION, 0, DescriptorConfiguration::LENGTH + 4);
    put_be16(cfg, AemReadDescriptorResponsePayload::LENGTH + 70, 1);
    put_be16(cfg, AemReadDescriptorResponsePayload::LENGTH + 72, DescriptorConfiguration::LENGTH);
    put_be16(cfg, AemReadDescriptorResponsePayload::LENGTH + 74, DESCRIPTOR_CONTROL);
    put_be16(cfg, AemReadDescriptorResponsePayload::LENGTH + 76, 2);
    w.e.on_response(ENTITY_A, AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_SUCCESS, cfg);
    EXPECT_EQ(w.e.outstanding(), 0);
    EXPECT_EQ(w.printed, 2);
    w.e.pump();
    EXPECT_EQ(w.issued.size(), 4U);
    EXPECT_EQ(w.e.outstanding(), 2);
    EXPECT_TRUE(std::get<1>(w.issued[2]) == DESCRIPTOR_CONTROL && std::get<2>(w.issued[3]) == 1);
    EXPECT_FALSE(w.e.finished(3));

    // One CONTROL read fails, the other times out: both release, and only then
    // is the walk finished (single target: no quiet window).
    w.e.on_response(ENTITY_A, AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_NO_SUCH_DESCRIPTOR, {});
    EXPECT_EQ(w.e.outstanding(), 1);
    EXPECT_FALSE(w.e.finished(4));
    w.e.on_timeout(ENTITY_A, AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_EQ(w.e.outstanding(), 0);
    EXPECT_TRUE(w.e.finished(4));
    EXPECT_EQ(w.printed, 2);
    EXPECT_EQ(w.e.entity_count(), 1U);
}

// IEEE 1722.1-2021 7.2.2: descriptor_counts lists the top level descriptors.
// A unit's stream ports, a port's clusters and map and a LOCALE's STRINGS are
// named by their owners, and the walk goes on from each descriptor it reads.
TEST(atdecc_enumerator, follows_each_descriptor_to_the_ones_it_owns)
{
    Walk w;
    w.read_to_configuration({{DESCRIPTOR_AUDIO_UNIT, 1}, {DESCRIPTOR_LOCALE, 1}});
    EXPECT_TRUE(w.pump() == (Reads{{DESCRIPTOR_AUDIO_UNIT, 0}, {DESCRIPTOR_LOCALE, 0}}));

    DescriptorAudioUnit unit{};
    unit.number_of_stream_output_ports = ieee::doublet_t{1};
    w.answer(response_with(unit));
    DescriptorLocale locale{};
    locale.number_of_strings = ieee::doublet_t{2};
    w.answer(response_with(locale));
    EXPECT_TRUE(w.pump() == (Reads{{DESCRIPTOR_STREAM_PORT_OUTPUT, 0}, {DESCRIPTOR_STRINGS, 0}, {DESCRIPTOR_STRINGS, 1}}));
    EXPECT_FALSE(w.e.finished(1));

    w.answer(read_response(DESCRIPTOR_STRINGS, 0, 8));
    w.answer(read_response(DESCRIPTOR_STRINGS, 1, 8));
    DescriptorStreamPort port{};
    port.descriptor_type = ieee::doublet_t{DESCRIPTOR_STREAM_PORT_OUTPUT};
    port.number_of_clusters = ieee::doublet_t{2};
    port.number_of_maps = ieee::doublet_t{1};
    w.answer(response_with(port));
    EXPECT_TRUE(w.pump() == (Reads{{DESCRIPTOR_AUDIO_CLUSTER, 0}, {DESCRIPTOR_AUDIO_CLUSTER, 1}, {DESCRIPTOR_AUDIO_MAP, 0}}));

    // The clusters and the map own nothing: answered, the walk is finished.
    w.answer(read_response(DESCRIPTOR_AUDIO_CLUSTER, 0, 8));
    w.answer(read_response(DESCRIPTOR_AUDIO_CLUSTER, 1, 8));
    EXPECT_FALSE(w.e.finished(2));
    w.answer(read_response(DESCRIPTOR_AUDIO_MAP, 0, 8));
    EXPECT_TRUE(w.pump().empty());
    EXPECT_TRUE(w.e.finished(2));
    EXPECT_EQ(w.printed, 10);
}

// An entity that lists its stream ports in the counts as well has each read once.
TEST(atdecc_enumerator, what_the_counts_list_and_an_owner_names_is_read_once)
{
    Walk w;
    w.read_to_configuration({{DESCRIPTOR_AUDIO_UNIT, 1}, {DESCRIPTOR_STREAM_PORT_OUTPUT, 1}});
    EXPECT_TRUE(w.pump() == (Reads{{DESCRIPTOR_AUDIO_UNIT, 0}, {DESCRIPTOR_STREAM_PORT_OUTPUT, 0}}));

    DescriptorAudioUnit unit{};
    unit.number_of_stream_output_ports = ieee::doublet_t{1};
    w.answer(response_with(unit));
    EXPECT_TRUE(w.pump().empty());
}

// A read the controller cannot send now stays queued and is not counted out.
TEST(atdecc_enumerator, a_refused_read_is_kept_and_not_counted)
{
    int refusals = 0;
    EnumeratorHooks h{};
    h.read_descriptor = [&](Eui64 const&, uint16_t, uint16_t) {
        ++refusals;
        return false;
    };
    Enumerator e{ENTITY_A, 0, h};
    DiscoveredEntity entity{};
    entity.adpdu.entity_id = ENTITY_A;
    e.on_entity(entity, 0);
    e.pump();
    e.pump();
    EXPECT_EQ(refusals, 2);
    EXPECT_EQ(e.outstanding(), 0);
    EXPECT_FALSE(e.finished(0));
}

TEST_MAIN(statusbar_atdecc_tools, atdecc_enumerator_test)
