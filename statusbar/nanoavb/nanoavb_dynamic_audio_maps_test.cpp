// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_dynamic_audio_maps.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

using namespace statusbar::nanoavb;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_PORT_OUTPUT;

namespace {

auto m(uint16_t const stream, uint16_t const stream_ch, uint16_t const cluster, uint16_t const cluster_ch = 0)
    -> DynamicAudioMapping
{
    return {.stream_index = stream, .stream_channel = stream_ch, .cluster_offset = cluster, .cluster_channel = cluster_ch};
}

auto port_of(DynamicAudioMaps const& maps, uint16_t const type, uint16_t const index) -> std::vector<DynamicAudioMapping>
{
    auto const view = maps.port(type, index);
    return {view.begin(), view.end()};
}

/// (port_type, port_index, mapping) of every mapping, as for_each() visits them.
auto visited(DynamicAudioMaps const& maps) -> std::vector<std::tuple<uint16_t, uint16_t, DynamicAudioMapping>>
{
    std::vector<std::tuple<uint16_t, uint16_t, DynamicAudioMapping>> out;
    maps.for_each([&out](uint16_t const type, uint16_t const index, DynamicAudioMapping const& mapping) {
        out.emplace_back(type, index, mapping);
    });
    return out;
}

}  // namespace

TEST(nanoavb_dynamic_audio_maps, a_port_keeps_its_mappings_in_cluster_order_without_duplicates)
{
    DynamicAudioMaps maps;
    std::vector<DynamicAudioMapping> const given{m(0, 1, 2), m(1, 0, 0, 1), m(0, 3, 0), m(0, 1, 2), m(0, 0, 0)};
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 0, given));
    std::vector<DynamicAudioMapping> const ordered{m(0, 0, 0), m(0, 3, 0), m(1, 0, 0, 1), m(0, 1, 2)};
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_INPUT, 0) == ordered);
    EXPECT_EQ(maps.size(), size_t{4});
}

TEST(nanoavb_dynamic_audio_maps, each_port_is_replaced_alone)
{
    DynamicAudioMaps maps;
    std::vector<DynamicAudioMapping> const a{m(0, 0, 0), m(0, 1, 1)};
    std::vector<DynamicAudioMapping> const b{m(1, 0, 0)};
    std::vector<DynamicAudioMapping> const c{m(2, 2, 2)};
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 1, b));
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 0, a));
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_OUTPUT, 0, c));
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 0, c));
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_INPUT, 0) == c);
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_INPUT, 1) == b);
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_OUTPUT, 0) == c);
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_INPUT, 2).empty());

    // Visited by port, input ports before output ports.
    auto const all = visited(maps);
    EXPECT_EQ(all.size(), size_t{3});
    EXPECT_TRUE(all[0] == std::tuple(DESCRIPTOR_STREAM_PORT_INPUT, uint16_t{0}, m(2, 2, 2)));
    EXPECT_TRUE(all[1] == std::tuple(DESCRIPTOR_STREAM_PORT_INPUT, uint16_t{1}, m(1, 0, 0)));
    EXPECT_TRUE(all[2] == std::tuple(DESCRIPTOR_STREAM_PORT_OUTPUT, uint16_t{0}, m(2, 2, 2)));

    // Emptied, the port has nothing; clear() forgets every port.
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 1, {}));
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_INPUT, 1).empty());
    EXPECT_EQ(maps.size(), size_t{2});
    maps.clear();
    EXPECT_EQ(maps.size(), size_t{0});
}

TEST(nanoavb_dynamic_audio_maps, a_set_past_the_capacity_changes_nothing)
{
    DynamicAudioMaps maps;
    std::vector<DynamicAudioMapping> full;
    full.reserve(DynamicAudioMaps::MAX_MAPPINGS);
    for (uint16_t ch = 0; ch < DynamicAudioMaps::MAX_MAPPINGS; ++ch) {
        full.push_back(m(0, 0, 0, ch));
    }
    std::vector<DynamicAudioMapping> const one{m(1, 1, 1)};
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 1, one));
    EXPECT_FALSE(maps.fits(DESCRIPTOR_STREAM_PORT_INPUT, 0, full.size()));
    EXPECT_FALSE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 0, full));
    EXPECT_EQ(maps.size(), size_t{1});
    EXPECT_TRUE(port_of(maps, DESCRIPTOR_STREAM_PORT_INPUT, 1) == one);

    // In place of the port's own it fits.
    EXPECT_TRUE(maps.fits(DESCRIPTOR_STREAM_PORT_INPUT, 1, full.size()));
    EXPECT_TRUE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 1, full));
    EXPECT_EQ(maps.size(), DynamicAudioMaps::MAX_MAPPINGS);
    EXPECT_FALSE(maps.replace(DESCRIPTOR_STREAM_PORT_INPUT, 0, one));
}

TEST_MAIN(statusbar_nanoavb, nanoavb_dynamic_audio_maps_test)
