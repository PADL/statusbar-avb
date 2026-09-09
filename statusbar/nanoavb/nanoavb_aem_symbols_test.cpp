// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for derive_aem_symbols: the §6.3 rules — ownership-tree walk,
// per-control_type ordinals, block scoping, orphan flagging — and the
// stability property the rules exist for: additive change does not
// renumber unrelated symbols.

#include "statusbar/nanoavb/nanoavb_aem_symbols.hpp"

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_jdks.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

namespace {

struct TestModel
{
    std::vector<AemRawDescriptor> descriptors;
    std::vector<ieee::Eui64> control_types;         // unit-owned, in index order from 0
    std::vector<ieee::Eui64> config_control_types;  // configuration-owned, indexed after the unit's
    std::uint16_t stream_input_ports = 0;           // STREAM_PORT_INPUT descriptors the unit owns from 0
    std::uint16_t mixers = 0;                       // MIXER descriptors the unit owns from 0

    void finish()
    {
        DescriptorEntity e;
        add(DESCRIPTOR_ENTITY, 0, make_const_span(e));
        DescriptorConfiguration cfg;
        (void)cfg.push_descriptor_count({.descriptor_type = DESCRIPTOR_AUDIO_UNIT, .count = 1});
        if (!config_control_types.empty()) {
            (void)cfg.push_descriptor_count(
                {.descriptor_type = DESCRIPTOR_CONTROL, .count = std::uint16_t(config_control_types.size())});
        }
        add(DESCRIPTOR_CONFIGURATION, 0, make_const_span(cfg).first(cfg.wire_size()));

        DescriptorAudioUnit unit;
        unit.number_of_controls = std::uint16_t(control_types.size());
        unit.base_control = 0;
        unit.number_of_control_blocks = 1;
        unit.base_control_block = 0;
        unit.number_of_stream_input_ports = stream_input_ports;
        unit.base_stream_input_port = 0;
        unit.number_of_mixers = mixers;
        unit.base_mixer = 0;
        add(DESCRIPTOR_AUDIO_UNIT, 0, make_const_span(unit).first(unit.wire_size()));

        // One block over the first two unit controls; the rest are unit-level.
        DescriptorControlBlock block;
        block.base_control = 0;
        block.number_of_controls = std::uint16_t(std::min<std::size_t>(2, control_types.size()));
        add(DESCRIPTOR_CONTROL_BLOCK, 0, make_const_span(block));

        std::uint16_t index = 0;
        for (auto const& type : control_types) {
            add_control(index++, type);
        }
        for (auto const& type : config_control_types) {
            add_control(index++, type);
        }
    }

    void add_control(std::uint16_t index, ieee::Eui64 const& type)
    {
        DescriptorControl c;
        c.descriptor_index = index;
        c.control_type = type;
        c.control_value_type = CONTROL_LINEAR_FLOAT;
        c.number_of_values = 1;
        add(DESCRIPTOR_CONTROL, index, make_const_span(c).first(c.wire_size()));
    }

    void add(std::uint16_t type, std::uint16_t index, std::span<std::uint8_t const> bytes)
    {
        descriptors.push_back({0, type, index, {bytes.begin(), bytes.end()}});
    }
};

std::map<std::uint16_t, std::string> control_symbols(std::vector<AemSymbol> const& symbols)
{
    std::map<std::uint16_t, std::string> out;
    for (auto const& s : symbols) {
        if (s.descriptor_type == DESCRIPTOR_CONTROL) {
            out[s.descriptor_index] = s.symbol;
        }
    }
    return out;
}

AemSymbol const* find_symbol(std::vector<AemSymbol> const& symbols, std::uint16_t type, std::uint16_t index)
{
    for (auto const& s : symbols) {
        if (s.descriptor_type == type && s.descriptor_index == index) {
            return &s;
        }
    }
    return nullptr;
}

}  // namespace

TEST(aem_symbols, ownership_walk_with_type_ordinals)
{
    TestModel m;
    m.control_types = {CONTROL_TYPE_GAIN, CONTROL_TYPE_MUTE, CONTROL_TYPE_GAIN, CONTROL_TYPE_GAIN, CONTROL_TYPE_DELAY};
    m.finish();

    auto symbols = derive_aem_symbols(m.descriptors);
    auto ctl = control_symbols(symbols);

    // Block scope covers controls 0-1; unit scope the rest, with per-type
    // ordinals (two unit-level gains, one delay).
    EXPECT_TRUE(ctl[0] == "cfg0/au0/cb0/ctl:gain/0");
    EXPECT_TRUE(ctl[1] == "cfg0/au0/cb0/ctl:mute/0");
    EXPECT_TRUE(ctl[2] == "cfg0/au0/ctl:gain/0");
    EXPECT_TRUE(ctl[3] == "cfg0/au0/ctl:gain/1");
    EXPECT_TRUE(ctl[4] == "cfg0/au0/ctl:delay/0");

    for (auto const& s : symbols) {
        EXPECT_FALSE(s.orphan);
    }
}

TEST(aem_symbols, additive_change_does_not_renumber)
{
    TestModel before;
    before.control_types = {CONTROL_TYPE_GAIN, CONTROL_TYPE_MUTE, CONTROL_TYPE_GAIN, CONTROL_TYPE_GAIN};
    before.finish();

    // The next model revision inserts a DELAY control *between* the gains
    // (indexes shift). Rule 3: gains keep their per-type ordinals.
    TestModel after;
    after.control_types = {CONTROL_TYPE_GAIN, CONTROL_TYPE_MUTE, CONTROL_TYPE_GAIN, CONTROL_TYPE_DELAY, CONTROL_TYPE_GAIN};
    after.finish();

    auto a = control_symbols(derive_aem_symbols(before.descriptors));
    auto b = control_symbols(derive_aem_symbols(after.descriptors));

    EXPECT_TRUE(a[2] == "cfg0/au0/ctl:gain/0");
    EXPECT_TRUE(b[2] == "cfg0/au0/ctl:gain/0");  // unchanged
    EXPECT_TRUE(a[3] == "cfg0/au0/ctl:gain/1");
    EXPECT_TRUE(b[4] == "cfg0/au0/ctl:gain/1");  // same symbol, shifted index
    EXPECT_TRUE(b[3] == "cfg0/au0/ctl:delay/0");
}

TEST(aem_symbols, orphans_flagged_config_types_enumerated)
{
    TestModel m;
    m.control_types = {CONTROL_TYPE_GAIN};
    m.finish();
    // A control no owner references and the configuration does not
    // declare, and a config-level AVB_INTERFACE.
    m.add_control(9, CONTROL_TYPE_GAIN);
    DescriptorAvbInterface iface;
    m.add(DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));

    auto symbols = derive_aem_symbols(m.descriptors);
    bool saw_orphan = false, saw_iface = false;
    for (auto const& s : symbols) {
        if (s.descriptor_type == DESCRIPTOR_CONTROL && s.descriptor_index == 9) {
            saw_orphan = true;
            EXPECT_TRUE(s.orphan);
            EXPECT_TRUE(s.symbol == "orphan/001a/9");
        }
        if (s.descriptor_type == DESCRIPTOR_AVB_INTERFACE) {
            saw_iface = true;
            EXPECT_FALSE(s.orphan);
            EXPECT_TRUE(s.symbol == "cfg0/avbif0");
        }
    }
    EXPECT_TRUE(saw_orphan);
    EXPECT_TRUE(saw_iface);
}

TEST(aem_symbols, configuration_controls_claim_by_declared_count)
{
    // The venue shape (Meyer Q1 / RZ): an AUDIO_UNIT with no controls of
    // its own and the configuration declaring every CONTROL as top level
    // — Milan IDENTIFY plus vendor chassis controls.
    TestModel m;
    m.config_control_types = {CONTROL_TYPE_IDENTIFY, CONTROL_TYPE_GAIN, CONTROL_TYPE_ENABLE, CONTROL_TYPE_GAIN};
    m.finish();

    auto symbols = derive_aem_symbols(m.descriptors);
    auto ctl = control_symbols(symbols);
    EXPECT_TRUE(ctl[0] == "cfg0/ctl:identify/0");
    EXPECT_TRUE(ctl[1] == "cfg0/ctl:gain/0");
    EXPECT_TRUE(ctl[2] == "cfg0/ctl:enable/0");
    EXPECT_TRUE(ctl[3] == "cfg0/ctl:gain/1");
    for (auto const& s : symbols) {
        EXPECT_FALSE(s.orphan);
    }
}

TEST(aem_symbols, configuration_controls_are_the_unreached_ones_up_to_the_count)
{
    // Unit-owned controls 0-2 and two declared top-level controls (3, 4):
    // the declared count picks the unreached controls in index order and
    // never re-claims a unit's. Control 9 is beyond the count: orphan.
    TestModel m;
    m.control_types = {CONTROL_TYPE_GAIN, CONTROL_TYPE_MUTE, CONTROL_TYPE_GAIN};
    m.config_control_types = {CONTROL_TYPE_IDENTIFY, CONTROL_TYPE_GAIN};
    m.finish();
    m.add_control(9, CONTROL_TYPE_GAIN);

    auto symbols = derive_aem_symbols(m.descriptors);
    auto ctl = control_symbols(symbols);
    EXPECT_TRUE(ctl[2] == "cfg0/au0/ctl:gain/0");
    EXPECT_TRUE(ctl[3] == "cfg0/ctl:identify/0");
    EXPECT_TRUE(ctl[4] == "cfg0/ctl:gain/0");
    EXPECT_TRUE(ctl[9] == "orphan/001a/9");
    auto const* stray = find_symbol(symbols, DESCRIPTOR_CONTROL, 9);
    EXPECT_TRUE(stray != nullptr && stray->orphan);
    auto const* top = find_symbol(symbols, DESCRIPTOR_CONTROL, 3);
    EXPECT_TRUE(top != nullptr && !top->orphan);
}

TEST(aem_symbols, jack_interface_and_ptp_controls_scope_under_their_owner)
{
    TestModel m;
    m.control_types = {CONTROL_TYPE_GAIN};
    m.finish();
    m.add_control(5, CONTROL_TYPE_GAIN);
    m.add_control(6, CONTROL_TYPE_MUTE);
    m.add_control(7, CONTROL_TYPE_MUTE);
    m.add_control(8, CONTROL_TYPE_ENABLE);

    DescriptorJack jack;
    jack.descriptor_type = DESCRIPTOR_JACK_INPUT;
    jack.number_of_controls = 1;
    jack.base_control = 5;
    m.add(DESCRIPTOR_JACK_INPUT, 0, make_const_span(jack));

    // A 2021 interface (102 bytes) owns control 6; a 2013 payload stops at
    // 98 bytes and must claim nothing even when the bytes that would hold
    // its control range are absent.
    DescriptorAvbInterface iface;
    iface.number_of_controls = 1;
    iface.base_control = 6;
    m.add(DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(iface));
    DescriptorAvbInterface legacy;
    legacy.number_of_controls = 1;
    legacy.base_control = 7;
    m.add(DESCRIPTOR_AVB_INTERFACE, 1, make_const_span(legacy).first(DescriptorAvbInterface::MINIMUM_LENGTH));

    DescriptorPtpInstance ptp;
    ptp.number_of_controls = 1;
    ptp.base_control = 8;
    m.add(DESCRIPTOR_PTP_INSTANCE, 0, make_const_span(ptp));

    auto symbols = derive_aem_symbols(m.descriptors);
    auto ctl = control_symbols(symbols);
    EXPECT_TRUE(ctl[5] == "cfg0/jackin0/ctl:gain/0");
    EXPECT_TRUE(ctl[6] == "cfg0/avbif0/ctl:mute/0");
    EXPECT_TRUE(ctl[7] == "orphan/001a/7");
    EXPECT_TRUE(ctl[8] == "cfg0/ptpinst0/ctl:enable/0");
    auto const* ptp_symbol = find_symbol(symbols, DESCRIPTOR_PTP_INSTANCE, 0);
    EXPECT_TRUE(ptp_symbol != nullptr && ptp_symbol->symbol == "cfg0/ptpinst0");
}

TEST(aem_symbols, ports_clusters_maps_and_mixers_walk_under_the_unit)
{
    TestModel m;
    m.control_types = {CONTROL_TYPE_GAIN};
    m.stream_input_ports = 1;
    m.mixers = 2;
    m.finish();
    m.add_control(5, CONTROL_TYPE_GAIN);

    DescriptorStreamPort port;
    port.descriptor_type = DESCRIPTOR_STREAM_PORT_INPUT;
    port.number_of_controls = 1;
    port.base_control = 5;
    port.number_of_clusters = 2;
    port.base_cluster = 0;
    port.number_of_maps = 1;
    port.base_map = 0;
    m.add(DESCRIPTOR_STREAM_PORT_INPUT, 0, make_const_span(port));
    // A second input port no unit reaches.
    m.add(DESCRIPTOR_STREAM_PORT_INPUT, 7, make_const_span(port));

    DescriptorAudioCluster cluster;
    m.add(DESCRIPTOR_AUDIO_CLUSTER, 0, make_const_span(cluster));
    m.add(DESCRIPTOR_AUDIO_CLUSTER, 1, make_const_span(cluster));
    DescriptorAudioMap map;
    m.add(DESCRIPTOR_AUDIO_MAP, 0, make_const_span(map).first(map.wire_size()));
    DescriptorMixer mixer;
    m.add(DESCRIPTOR_MIXER, 0, make_const_span(mixer));
    m.add(DESCRIPTOR_MIXER, 1, make_const_span(mixer));

    auto symbols = derive_aem_symbols(m.descriptors);
    auto expect = [&](std::uint16_t type, std::uint16_t index, std::string const& symbol, bool orphan) {
        auto const* s = find_symbol(symbols, type, index);
        EXPECT_TRUE(s != nullptr);
        if (s != nullptr) {
            EXPECT_TRUE(s->symbol == symbol);
            EXPECT_EQ(s->orphan, orphan);
        }
    };
    expect(DESCRIPTOR_STREAM_PORT_INPUT, 0, "cfg0/au0/spin0", false);
    expect(DESCRIPTOR_CONTROL, 5, "cfg0/au0/spin0/ctl:gain/0", false);
    expect(DESCRIPTOR_AUDIO_CLUSTER, 1, "cfg0/au0/spin0/clus1", false);
    expect(DESCRIPTOR_AUDIO_MAP, 0, "cfg0/au0/spin0/map0", false);
    expect(DESCRIPTOR_MIXER, 1, "cfg0/au0/mix1", false);
    expect(DESCRIPTOR_STREAM_PORT_INPUT, 7, "orphan/000e/7", true);
}

TEST(aem_symbols, unregistered_vendor_control_types_keep_their_eui64)
{
    // The registry names every unregistered non-standard control_type
    // VENDOR_DEFINED; that is a placeholder, not an identity — two
    // different vendor controls must not share a segment (a reorder
    // would renumber them).
    ieee::Eui64 const erase{0x00, 0x1c, 0xab, 0x00, 0x00, 0x00, 0x00, 0x01};
    ieee::Eui64 const info{0x00, 0x1c, 0xab, 0x00, 0x00, 0x00, 0x00, 0x02};
    TestModel m;
    m.config_control_types = {CONTROL_TYPE_IDENTIFY, erase, info};
    m.finish();

    auto ctl = control_symbols(derive_aem_symbols(m.descriptors));
    EXPECT_TRUE(ctl[0] == "cfg0/ctl:identify/0");
    EXPECT_TRUE(ctl[1] == "cfg0/ctl:001cab0000000001/0");
    EXPECT_TRUE(ctl[2] == "cfg0/ctl:001cab0000000002/0");
}

TEST(aem_symbols, known_vendor_control_types_use_their_registry_name)
{
    // The Q1's configuration-level controls as crawled, plus the JDKS
    // types (a digit in the name must survive the identifier check), and
    // one Meyer value nobody registered.
    ieee::Eui64 const unregistered{0x00, 0x1c, 0xab, 0x00, 0x00, 0x10, 0x00, 0x30};
    TestModel m;
    m.config_control_types = {
        CONTROL_TYPE_IDENTIFY,
        meyer::CONTROL_TYPE_LOGGER,
        meyer::CONTROL_TYPE_ERASE_IDENTITY,
        meyer::CONTROL_TYPE_HARDWARE_INFO,
        atdecc::jdks::CONTROL_LOG_TEXT,
        atdecc::jdks::CONTROL_IPV4_PARAMETERS,
        unregistered,
        meyer::CONTROL_TYPE_LOGGER,
    };
    m.finish();

    auto ctl = control_symbols(derive_aem_symbols(m.descriptors));
    EXPECT_TRUE(ctl[0] == "cfg0/ctl:identify/0");
    EXPECT_TRUE(ctl[1] == "cfg0/ctl:meyer_logger/0");
    EXPECT_TRUE(ctl[2] == "cfg0/ctl:meyer_erase_identity/0");
    EXPECT_TRUE(ctl[3] == "cfg0/ctl:meyer_hardware_info/0");
    EXPECT_TRUE(ctl[4] == "cfg0/ctl:jdks_log_text/0");
    EXPECT_TRUE(ctl[5] == "cfg0/ctl:jdks_ipv4_parameters/0");
    EXPECT_TRUE(ctl[6] == "cfg0/ctl:001cab0000100030/0");
    EXPECT_TRUE(ctl[7] == "cfg0/ctl:meyer_logger/1");
}

TEST(aem_symbols, generator_version_is_current)
{
    // Every refinement above bumps the version so stored tables and
    // project files authored against v1 keep resolving through v1.
    EXPECT_EQ(AEM_SYMBOL_GENERATOR_VERSION, 3U);
}

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_aem_symbols_test)
