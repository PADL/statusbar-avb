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
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

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
    std::vector<ieee::Eui64> control_types;  // per control, in index order

    void finish()
    {
        DescriptorEntity e;
        add(DESCRIPTOR_ENTITY, 0, make_const_span(e));
        DescriptorConfiguration cfg;
        add(DESCRIPTOR_CONFIGURATION, 0, make_const_span(cfg).first(cfg.wire_size()));

        DescriptorAudioUnit unit;
        unit.number_of_controls = std::uint16_t(control_types.size());
        unit.base_control = 0;
        unit.number_of_control_blocks = 1;
        unit.base_control_block = 0;
        add(DESCRIPTOR_AUDIO_UNIT, 0, make_const_span(unit).first(unit.wire_size()));

        // One block over the first two controls; the rest are unit-level.
        DescriptorControlBlock block;
        block.base_control = 0;
        block.number_of_controls = 2;
        add(DESCRIPTOR_CONTROL_BLOCK, 0, make_const_span(block));

        for (std::uint16_t i = 0; i < control_types.size(); ++i) {
            DescriptorControl c;
            c.descriptor_index = i;
            c.control_type = control_types[i];
            c.control_value_type = CONTROL_LINEAR_FLOAT;
            c.number_of_values = 1;
            auto view = make_const_span(c);
            AemRawDescriptor raw{0, DESCRIPTOR_CONTROL, i, {view.begin(), view.begin() + long(c.wire_size())}};
            descriptors.push_back(std::move(raw));
        }
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
    // A control no unit references, and a config-level AVB_INTERFACE.
    DescriptorControl stray;
    stray.descriptor_index = 9;
    stray.control_type = CONTROL_TYPE_GAIN;
    stray.control_value_type = CONTROL_LINEAR_FLOAT;
    stray.number_of_values = 1;
    auto view = make_const_span(stray);
    m.descriptors.push_back({0, DESCRIPTOR_CONTROL, 9, {view.begin(), view.begin() + long(stray.wire_size())}});
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

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_aem_symbols_test)
