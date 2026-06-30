// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Unit tests for validate_aem_model — the AEM model compliance checker behind
/// `statusbar-atdecc-ctl --command=validate`.

#include "statusbar/atdecc_tools/atdecc_aem_validate.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <cstring>
#include <string_view>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::atdecc_tools;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

// Serialize a wire descriptor struct to its on-wire bytes (matching the blob
// tool: memcpy wire_size() bytes -- the fixed header + any inline trailer).
template <typename T>
auto to_raw(T const& d, uint16_t type, uint16_t index) -> RawDescriptor
{
    size_t n = T::LENGTH;
    if constexpr (requires { d.wire_size(); }) {
        n = d.wire_size();
    }
    std::vector<uint8_t> bytes(n);
    std::memcpy(bytes.data(), &d, n);
    return {.type = type, .index = index, .data = std::move(bytes)};
}

auto count_sev(std::vector<ModelFinding> const& f, Severity s) -> int
{
    int n = 0;
    for (auto const& x : f) {
        if (x.severity == s) {
            ++n;
        }
    }
    return n;
}

auto has_msg(std::vector<ModelFinding> const& f, std::string_view where_substr, Severity s) -> bool
{
    for (auto const& x : f) {
        if (x.severity == s && x.where.find(where_substr) != std::string::npos) {
            return true;
        }
    }
    return false;
}

auto good_entity() -> DescriptorEntity
{
    DescriptorEntity d{};
    d.entity_model_id = Eui64{0x70, 0xB3, 0xD5, 0xED, 0xCF, 0x00, 0x00, 0x00};
    d.configurations_count = 1;
    return d;
}

auto good_avb_interface() -> DescriptorAvbInterface
{
    DescriptorAvbInterface d{};
    d.mac_address = Eui48{0x02, 0x00, 0x00, 0xDF, 0xD1, 0x96};
    d.clock_identity = Eui64{0x02, 0x00, 0x00, 0xFF, 0xFE, 0xDF, 0xD1, 0x96};
    d.clock_class = 248;
    d.clock_accuracy = 0xFE;
    return d;
}

auto stream(uint16_t type, uint16_t index, bool with_format) -> DescriptorStream
{
    DescriptorStream d{};
    d.descriptor_type = type;
    d.descriptor_index = index;
    if (with_format) {
        d.current_format = Eui64{0x00, 0xA0, 0x04, 0x08, 0x60, 0x00, 0x08, 0x00};
        d.number_of_formats = 1;
    }
    return d;
}

}  // namespace

TEST(aem_validate, zero_avb_interface_and_model_id_are_errors)
{
    std::vector<RawDescriptor> descs;
    DescriptorEntity ent{};  // entity_model_id = 0, configurations_count = 0
    descs.push_back(to_raw(ent, DESCRIPTOR_ENTITY, 0));
    DescriptorAvbInterface avb{};  // mac=0, clock_identity=0
    descs.push_back(to_raw(avb, DESCRIPTOR_AVB_INTERFACE, 0));

    auto const f = validate_aem_model(descs);
    EXPECT_TRUE(has_msg(f, "ENTITY", Severity::Error));            // model_id / configs
    EXPECT_TRUE(has_msg(f, "AVB_INTERFACE[0]", Severity::Error));  // mac + clock_identity
    EXPECT_TRUE(count_sev(f, Severity::Error) >= 3);
}

TEST(aem_validate, well_formed_model_has_no_errors)
{
    std::vector<RawDescriptor> descs;
    descs.push_back(to_raw(good_entity(), DESCRIPTOR_ENTITY, 0));
    descs.push_back(to_raw(good_avb_interface(), DESCRIPTOR_AVB_INTERFACE, 0));
    descs.push_back(to_raw(stream(DESCRIPTOR_STREAM_INPUT, 0, /*with_format=*/true), DESCRIPTOR_STREAM_INPUT, 0));
    descs.push_back(to_raw(stream(DESCRIPTOR_STREAM_OUTPUT, 0, /*with_format=*/true), DESCRIPTOR_STREAM_OUTPUT, 0));

    auto const f = validate_aem_model(descs);
    EXPECT_EQ(count_sev(f, Severity::Error), 0);
}

TEST(aem_validate, stream_with_zero_format_is_error)
{
    std::vector<RawDescriptor> descs;
    descs.push_back(to_raw(good_entity(), DESCRIPTOR_ENTITY, 0));
    descs.push_back(to_raw(good_avb_interface(), DESCRIPTOR_AVB_INTERFACE, 0));
    descs.push_back(to_raw(stream(DESCRIPTOR_STREAM_INPUT, 0, /*with_format=*/false), DESCRIPTOR_STREAM_INPUT, 0));

    auto const f = validate_aem_model(descs);
    EXPECT_TRUE(has_msg(f, "STREAM_INPUT[0]", Severity::Error));
}

TEST(aem_validate, configuration_count_shortfall_is_error)
{
    std::vector<RawDescriptor> descs;
    descs.push_back(to_raw(good_entity(), DESCRIPTOR_ENTITY, 0));
    descs.push_back(to_raw(good_avb_interface(), DESCRIPTOR_AVB_INTERFACE, 0));
    DescriptorConfiguration cfg{};
    (void)cfg.push_descriptor_count({.descriptor_type = DESCRIPTOR_STREAM_INPUT, .count = 2});
    descs.push_back(to_raw(cfg, DESCRIPTOR_CONFIGURATION, 0));
    // Only ONE STREAM_INPUT actually present though config declares two.
    descs.push_back(to_raw(stream(DESCRIPTOR_STREAM_INPUT, 0, true), DESCRIPTOR_STREAM_INPUT, 0));

    auto const f = validate_aem_model(descs);
    EXPECT_TRUE(has_msg(f, "CONFIGURATION", Severity::Error));
}

TEST(aem_validate, no_jacks_is_info_not_error)
{
    std::vector<RawDescriptor> descs;
    descs.push_back(to_raw(good_entity(), DESCRIPTOR_ENTITY, 0));
    descs.push_back(to_raw(good_avb_interface(), DESCRIPTOR_AVB_INTERFACE, 0));
    auto const f = validate_aem_model(descs);
    EXPECT_TRUE(has_msg(f, "JACK", Severity::Info));
    EXPECT_EQ(count_sev(f, Severity::Error), 0);
}

TEST_MAIN(statusbar_atdecc_tools, atdecc_aem_validate_test)
