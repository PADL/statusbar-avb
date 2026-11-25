// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_children.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using statusbar::ieee::doublet_t;

namespace {

using Found = std::vector<std::pair<uint16_t, uint16_t>>;

/// @p descriptor as its octets on the wire.
template <class T>
auto octets(T const& descriptor) -> std::vector<uint8_t>
{
    std::vector<uint8_t> body(sizeof(T), 0);
    span_store(std::span{body}, descriptor);
    return body;
}

auto ranges_of(DescriptorRanges const& ranges) -> std::vector<DescriptorRange>
{
    return {ranges.begin(), ranges.end()};
}

/// An AUDIO_UNIT (or the same of another unit type) with stream ports:
/// @p inputs from index 0 and @p outputs from index 0.
template <class Unit>
auto unit_with_ports(uint16_t const inputs, uint16_t const outputs) -> std::vector<uint8_t>
{
    Unit unit{};
    unit.number_of_stream_input_ports = doublet_t{inputs};
    unit.number_of_stream_output_ports = doublet_t{outputs};
    return octets(unit);
}

/// Stream port @p index of @p type: @p clusters from @p base_cluster and one map, of the same index as the port.
auto stream_port(uint16_t const type, uint16_t const index, uint16_t const base_cluster, uint16_t const clusters)
    -> std::vector<uint8_t>
{
    DescriptorStreamPort port{};
    port.descriptor_type = doublet_t{type};
    port.descriptor_index = doublet_t{index};
    port.base_cluster = doublet_t{base_cluster};
    port.number_of_clusters = doublet_t{clusters};
    port.base_map = doublet_t{index};
    port.number_of_maps = doublet_t{1};
    return octets(port);
}

/// A walk and what it has found so far, in order.
struct Walked
{
    DescriptorChildWalk walk;
    Found found;

    Walked() = default;
    explicit Walked(size_t const max_steps, uint16_t const index_limit = DescriptorChildWalk::DEFAULT_INDEX_LIMIT)
        : walk{max_steps, index_limit}
    {}

    void read(std::vector<uint8_t> const& descriptor)
    {
        walk.descriptor(descriptor, [this](uint16_t const type, uint16_t const index) { found.emplace_back(type, index); });
    }

    /// What was found since the last call.
    auto take() -> Found { return std::exchange(found, {}); }
};

}  // namespace

TEST(atdecc_aem_children, a_unit_names_its_sixteen_runs_and_leaves_out_the_empty_ones)
{
    // IEEE 1722.1-2021 Table 7-5: each run is a count and the index of its first.
    DescriptorAudioUnit unit{};
    unit.number_of_stream_input_ports = doublet_t{2};
    unit.base_stream_input_port = doublet_t{0};
    unit.number_of_stream_output_ports = doublet_t{3};
    unit.base_stream_output_port = doublet_t{4};
    unit.number_of_external_input_ports = doublet_t{1};
    unit.base_external_input_port = doublet_t{7};
    unit.number_of_controls = doublet_t{5};
    unit.base_control = doublet_t{10};
    unit.number_of_control_blocks = doublet_t{2};
    unit.base_control_block = doublet_t{1};
    unit.base_mixer = doublet_t{9};  // a base with no count names nothing

    std::vector<DescriptorRange> const expected{
        {DESCRIPTOR_STREAM_PORT_INPUT, 0, 2},
        {DESCRIPTOR_STREAM_PORT_OUTPUT, 4, 3},
        {DESCRIPTOR_EXTERNAL_PORT_INPUT, 7, 1},
        {DESCRIPTOR_CONTROL, 10, 5},
        {DESCRIPTOR_CONTROL_BLOCK, 1, 2}};
    EXPECT_TRUE(ranges_of(child_descriptor_ranges(octets(unit))) == expected);
}

TEST(atdecc_aem_children, every_run_of_a_unit_is_read_from_its_own_field)
{
    DescriptorVideoUnit unit{};
    unit.number_of_stream_input_ports = doublet_t{1};
    unit.number_of_stream_output_ports = doublet_t{1};
    unit.number_of_external_input_ports = doublet_t{1};
    unit.number_of_external_output_ports = doublet_t{1};
    unit.number_of_internal_input_ports = doublet_t{1};
    unit.number_of_internal_output_ports = doublet_t{1};
    unit.number_of_controls = doublet_t{1};
    unit.number_of_signal_selectors = doublet_t{1};
    unit.number_of_mixers = doublet_t{1};
    unit.number_of_matrices = doublet_t{1};
    unit.number_of_splitters = doublet_t{1};
    unit.number_of_combiners = doublet_t{1};
    unit.number_of_demultiplexers = doublet_t{1};
    unit.number_of_multiplexers = doublet_t{1};
    unit.number_of_transcoders = doublet_t{1};
    unit.number_of_control_blocks = doublet_t{1};

    std::vector<uint16_t> types;
    for (auto const& range : child_descriptor_ranges(octets(unit))) {
        types.push_back(range.type);
    }
    std::vector<uint16_t> const expected{
        DESCRIPTOR_STREAM_PORT_INPUT,
        DESCRIPTOR_STREAM_PORT_OUTPUT,
        DESCRIPTOR_EXTERNAL_PORT_INPUT,
        DESCRIPTOR_EXTERNAL_PORT_OUTPUT,
        DESCRIPTOR_INTERNAL_PORT_INPUT,
        DESCRIPTOR_INTERNAL_PORT_OUTPUT,
        DESCRIPTOR_CONTROL,
        DESCRIPTOR_SIGNAL_SELECTOR,
        DESCRIPTOR_MIXER,
        DESCRIPTOR_MATRIX,
        DESCRIPTOR_SIGNAL_SPLITTER,
        DESCRIPTOR_SIGNAL_COMBINER,
        DESCRIPTOR_SIGNAL_DEMULTIPLEXER,
        DESCRIPTOR_SIGNAL_MULTIPLEXER,
        DESCRIPTOR_SIGNAL_TRANSCODER,
        DESCRIPTOR_CONTROL_BLOCK};
    EXPECT_TRUE(types == expected);
}

TEST(atdecc_aem_children, the_other_owners_name_what_their_tables_give_them)
{
    // Table 7-23: a stream port's CONTROLs, clusters and maps.
    DescriptorStreamPort port{};
    port.descriptor_type = doublet_t{DESCRIPTOR_STREAM_PORT_OUTPUT};
    port.number_of_controls = doublet_t{1};
    port.base_control = doublet_t{6};
    port.number_of_clusters = doublet_t{8};
    port.base_cluster = doublet_t{16};
    port.number_of_maps = doublet_t{1};
    port.base_map = doublet_t{2};
    std::vector<DescriptorRange> const of_port{
        {DESCRIPTOR_CONTROL, 6, 1}, {DESCRIPTOR_AUDIO_CLUSTER, 16, 8}, {DESCRIPTOR_AUDIO_MAP, 2, 1}};
    EXPECT_TRUE(ranges_of(child_descriptor_ranges(octets(port))) == of_port);

    // Table 7-21: a LOCALE's STRINGS, which no descriptor_counts lists.
    DescriptorLocale locale{};
    locale.number_of_strings = doublet_t{3};
    locale.base_strings = doublet_t{9};
    EXPECT_TRUE(ranges_of(child_descriptor_ranges(octets(locale))) == (std::vector<DescriptorRange>{{DESCRIPTOR_STRINGS, 9, 3}}));

    // A PTP_INSTANCE's CONTROLs and PTP_PORTs.
    DescriptorPtpInstance instance{};
    instance.number_of_controls = doublet_t{1};
    instance.base_control = doublet_t{4};
    instance.number_of_ptp_ports = doublet_t{2};
    instance.base_ptp_port = doublet_t{0};
    std::vector<DescriptorRange> const of_instance{{DESCRIPTOR_CONTROL, 4, 1}, {DESCRIPTOR_PTP_PORT, 0, 2}};
    EXPECT_TRUE(ranges_of(child_descriptor_ranges(octets(instance))) == of_instance);

    // Table 7-45: the MATRIX_SIGNALs of a MATRIX's sources.
    DescriptorMatrix matrix{};
    matrix.number_of_sources = doublet_t{2};
    matrix.base_source = doublet_t{5};
    EXPECT_TRUE(
        ranges_of(child_descriptor_ranges(octets(matrix))) == (std::vector<DescriptorRange>{{DESCRIPTOR_MATRIX_SIGNAL, 5, 2}}));

    // A jack's CONTROLs.
    DescriptorJack jack{};
    jack.descriptor_type = doublet_t{DESCRIPTOR_JACK_OUTPUT};
    jack.number_of_controls = doublet_t{2};
    jack.base_control = doublet_t{3};
    EXPECT_TRUE(ranges_of(child_descriptor_ranges(octets(jack))) == (std::vector<DescriptorRange>{{DESCRIPTOR_CONTROL, 3, 2}}));
}

TEST(atdecc_aem_children, a_descriptor_that_owns_nothing_or_is_cut_short_names_nothing)
{
    DescriptorAudioCluster cluster{};
    EXPECT_TRUE(child_descriptor_ranges(octets(cluster)).empty());
    EXPECT_TRUE(child_descriptor_ranges({}).empty());

    // An AVB_INTERFACE at its 2013 length ends before its CONTROLs (octet 98).
    DescriptorAvbInterface interface{};
    interface.number_of_controls = doublet_t{2};
    auto const whole = octets(interface);
    EXPECT_EQ(child_descriptor_ranges(whole).size(), 1U);
    EXPECT_TRUE(child_descriptor_ranges(std::span{whole}.first(98)).empty());
}

TEST(atdecc_aem_children, a_stream_ports_clusters_and_maps_take_their_units_type)
{
    DescriptorRange const clusters{DESCRIPTOR_AUDIO_CLUSTER, 4, 2};
    DescriptorRange const maps{DESCRIPTOR_AUDIO_MAP, 1, 1};
    DescriptorRange const controls{DESCRIPTOR_CONTROL, 0, 3};
    EXPECT_EQ(range_under_unit(clusters, DESCRIPTOR_AUDIO_UNIT).type, DESCRIPTOR_AUDIO_CLUSTER);
    EXPECT_EQ(range_under_unit(clusters, DESCRIPTOR_VIDEO_UNIT).type, DESCRIPTOR_VIDEO_CLUSTER);
    EXPECT_EQ(range_under_unit(clusters, DESCRIPTOR_SENSOR_UNIT).type, DESCRIPTOR_SENSOR_CLUSTER);
    EXPECT_EQ(range_under_unit(maps, DESCRIPTOR_VIDEO_UNIT).type, DESCRIPTOR_VIDEO_MAP);
    EXPECT_EQ(range_under_unit(maps, DESCRIPTOR_SENSOR_UNIT).type, DESCRIPTOR_SENSOR_MAP);
    EXPECT_EQ(range_under_unit(controls, DESCRIPTOR_VIDEO_UNIT).type, DESCRIPTOR_CONTROL);
    // Base and count are the port's own; a type that is no unit changes nothing.
    EXPECT_TRUE(range_under_unit(clusters, DESCRIPTOR_VIDEO_UNIT) == (DescriptorRange{DESCRIPTOR_VIDEO_CLUSTER, 4, 2}));
    EXPECT_TRUE(range_under_unit(clusters, DESCRIPTOR_CONTROL) == clusters);
}

TEST(atdecc_aem_children_walk, a_unit_then_its_port_gives_the_ports_then_the_clusters_and_map)
{
    Walked w;
    w.read(unit_with_ports<DescriptorAudioUnit>(0, 2));
    EXPECT_TRUE(w.take() == (Found{{DESCRIPTOR_STREAM_PORT_OUTPUT, 0}, {DESCRIPTOR_STREAM_PORT_OUTPUT, 1}}));

    w.read(stream_port(DESCRIPTOR_STREAM_PORT_OUTPUT, 1, 8, 3));
    Found const expected{
        {DESCRIPTOR_AUDIO_CLUSTER, 8}, {DESCRIPTOR_AUDIO_CLUSTER, 9}, {DESCRIPTOR_AUDIO_CLUSTER, 10}, {DESCRIPTOR_AUDIO_MAP, 1}};
    EXPECT_TRUE(w.take() == expected);
    EXPECT_FALSE(w.walk.exhausted());
}

TEST(atdecc_aem_children_walk, a_port_read_before_its_unit_waits_for_it)
{
    // A reader with several reads in flight may be answered out of order; the
    // port alone does not say whether its clusters are audio, video or sensor.
    Walked w;
    w.read(stream_port(DESCRIPTOR_STREAM_PORT_INPUT, 0, 0, 2));
    EXPECT_TRUE(w.take().empty());

    w.read(unit_with_ports<DescriptorVideoUnit>(1, 0));
    Found const expected{
        {DESCRIPTOR_VIDEO_CLUSTER, 0}, {DESCRIPTOR_VIDEO_CLUSTER, 1}, {DESCRIPTOR_VIDEO_MAP, 0}, {DESCRIPTOR_STREAM_PORT_INPUT, 0}};
    EXPECT_TRUE(w.take() == expected);

    // Told once: the unit read again finds no port waiting.
    w.read(unit_with_ports<DescriptorVideoUnit>(1, 0));
    EXPECT_TRUE(w.take() == (Found{{DESCRIPTOR_STREAM_PORT_INPUT, 0}}));
}

TEST(atdecc_aem_children_walk, each_port_follows_its_own_unit)
{
    DescriptorSensorUnit sensors{};
    sensors.descriptor_index = doublet_t{0};
    sensors.number_of_stream_output_ports = doublet_t{1};
    sensors.base_stream_output_port = doublet_t{1};

    Walked w;
    w.read(unit_with_ports<DescriptorAudioUnit>(0, 1));
    w.read(octets(sensors));
    (void)w.take();
    w.read(stream_port(DESCRIPTOR_STREAM_PORT_OUTPUT, 0, 0, 1));
    EXPECT_TRUE(w.take() == (Found{{DESCRIPTOR_AUDIO_CLUSTER, 0}, {DESCRIPTOR_AUDIO_MAP, 0}}));
    w.read(stream_port(DESCRIPTOR_STREAM_PORT_OUTPUT, 1, 1, 1));
    EXPECT_TRUE(w.take() == (Found{{DESCRIPTOR_SENSOR_CLUSTER, 1}, {DESCRIPTOR_SENSOR_MAP, 1}}));
}

TEST(atdecc_aem_children_walk, any_other_owner_is_followed_as_read)
{
    DescriptorLocale locale{};
    locale.number_of_strings = doublet_t{2};
    locale.base_strings = doublet_t{7};
    Walked w;
    w.read(octets(locale));
    EXPECT_TRUE(w.take() == (Found{{DESCRIPTOR_STRINGS, 7}, {DESCRIPTOR_STRINGS, 8}}));

    DescriptorAudioCluster cluster{};
    w.read(octets(cluster));
    EXPECT_TRUE(w.take().empty());
}

TEST(atdecc_aem_children_walk, no_index_at_or_above_the_limit_is_named_and_none_past_the_last)
{
    DescriptorLocale locale{};
    locale.number_of_strings = doublet_t{0xFFFF};
    locale.base_strings = doublet_t{98};
    Walked limited{DescriptorChildWalk::DEFAULT_MAX_STEPS, 100};
    limited.read(octets(locale));
    EXPECT_TRUE(limited.take() == (Found{{DESCRIPTOR_STRINGS, 98}, {DESCRIPTOR_STRINGS, 99}}));
    EXPECT_FALSE(limited.walk.exhausted());

    // A run that would pass index 65535 ends there and does not wrap to 0.
    locale.base_strings = doublet_t{0xFFFE};
    Walked wide{DescriptorChildWalk::DEFAULT_MAX_STEPS, 0xFFFF};
    wide.read(octets(locale));
    EXPECT_TRUE(wide.take() == (Found{{DESCRIPTOR_STRINGS, 0xFFFE}}));
}

TEST(atdecc_aem_children_walk, the_walk_stops_for_good_when_its_steps_are_spent)
{
    DescriptorLocale locale{};
    locale.number_of_strings = doublet_t{0xFFFF};
    Walked w{5};
    w.read(octets(locale));
    EXPECT_EQ(w.take().size(), 5U);
    EXPECT_TRUE(w.walk.exhausted());
    w.read(octets(locale));
    EXPECT_TRUE(w.take().empty());
}

TEST_MAIN(statusbar_atdecc, atdecc_aem_children_test)
