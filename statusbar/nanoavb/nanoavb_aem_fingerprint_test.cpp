// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for aem_fingerprint: per-unit volatile fields must not leak
// into identity; real model differences must.

#include "statusbar/nanoavb/nanoavb_aem_fingerprint.hpp"

#include "statusbar/atdecc/atdecc_aem_control_values.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <cstring>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

namespace {

AemRawDescriptor raw_of(std::uint16_t config, std::uint16_t type, std::uint16_t index, std::span<std::uint8_t const> bytes)
{
    return {config, type, index, {bytes.begin(), bytes.end()}};
}

std::vector<AemRawDescriptor> model_for_unit(std::uint64_t serial_ish)
{
    DescriptorEntity e;
    e.entity_id = ieee::Eui64{0, 0x1C, 0xAB, 0xFF, 0xFE, 0, std::uint8_t(serial_ish >> 8), std::uint8_t(serial_ish)};
    e.entity_name = AtdeccString{"Venue Rack " + std::to_string(serial_ish)};
    e.group_name = AtdeccString{"FOH"};
    e.association_id = e.entity_id;
    e.available_index = std::uint32_t(serial_ish * 17);
    e.current_configuration = serial_ish % 2 ? 1 : 0;

    DescriptorAvbInterface i;
    i.mac_address = ieee::Eui48{0, 0x1C, 0xAB, 0, std::uint8_t(serial_ish >> 8), std::uint8_t(serial_ish)};
    i.clock_identity = e.entity_id;
    i.interface_flags = serial_ish % 2 ? 1 : 0;

    DescriptorControl c;
    c.control_value_type = CONTROL_LINEAR_FLOAT;
    c.number_of_values = 1;

    // current_signal is runtime state — which source the unit has selected
    // right now — and must not leak into identity.
    DescriptorSignalSelector s;
    s.number_of_sources = 3;
    s.current_signal = SignalSource{.signal_type = 0xFFFF, .signal_index = std::uint16_t(serial_ish % 3), .signal_output = 0};
    s.default_signal = SignalSource{.signal_type = 0xFFFF, .signal_index = 0, .signal_output = 0};

    std::vector<AemRawDescriptor> out;
    out.push_back(raw_of(0, DESCRIPTOR_ENTITY, 0, make_const_span(e)));
    out.push_back(raw_of(0, DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(i)));
    out.push_back(raw_of(0, DESCRIPTOR_CONTROL, 0, make_const_span(c).first(c.wire_size())));
    out.push_back(raw_of(0, DESCRIPTOR_SIGNAL_SELECTOR, 0, make_const_span(s).first(DescriptorSignalSelector::LENGTH)));
    return out;
}

}  // namespace

TEST(aem_fingerprint, per_unit_fields_do_not_leak_into_identity)
{
    auto q1 = model_for_unit(0xC8F0);
    auto q2 = model_for_unit(0x7604);
    auto fp1 = aem_fingerprint(q1);
    auto fp2 = aem_fingerprint(q2);
    EXPECT_EQ(fp1.size(), 64u);
    EXPECT_TRUE(fp1 == fp2);  // same logical model, two physical units

    // Descriptor order is canonicalized.
    std::swap(q1[0], q1[2]);
    EXPECT_TRUE(aem_fingerprint(q1) == fp2);
}

TEST(aem_fingerprint, model_differences_do_leak)
{
    auto base = model_for_unit(1);
    auto fp = aem_fingerprint(base);

    // A changed control default is a different model.
    auto changed = model_for_unit(1);
    DescriptorControl c;
    span_load_padded(c, changed[2].bytes);
    LinearValueEntry<float> entry;
    entry.set_default_value(3.5f);
    std::memcpy(c.value_details_bytes_mutable().data(), &entry, sizeof entry);
    auto view = make_const_span(c);
    changed[2].bytes.assign(view.begin(), view.begin() + long(c.wire_size()));
    EXPECT_FALSE(aem_fingerprint(changed) == fp);

    // An added descriptor is a different model.
    auto grown = model_for_unit(1);
    grown.push_back(grown[2]);
    grown.back().descriptor_index = 1;
    EXPECT_FALSE(aem_fingerprint(grown) == fp);
}

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_aem_fingerprint_test)
