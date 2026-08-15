// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_control_values.hpp"

#include "statusbar/test/test.hpp"

#include <cstring>

using namespace statusbar;
using namespace statusbar::atdecc::aem;

TEST(aem_control_values, unpack_pack_roundtrip)
{
    uint16_t const raw = pack_control_value_type(true, false, CONTROL_LINEAR_UINT16);
    auto const bits = unpack_control_value_type(raw);
    EXPECT_TRUE(bits.read_only);
    EXPECT_FALSE(bits.unknown);
    EXPECT_EQ(bits.value_type, CONTROL_LINEAR_UINT16);

    uint16_t const raw2 = pack_control_value_type(false, true, CONTROL_SMPTE_TIME);
    auto const bits2 = unpack_control_value_type(raw2);
    EXPECT_FALSE(bits2.read_only);
    EXPECT_TRUE(bits2.unknown);
    EXPECT_EQ(bits2.value_type, CONTROL_SMPTE_TIME);

    uint16_t const raw3 = pack_control_value_type(true, true, CONTROL_VENDOR);
    EXPECT_EQ(raw3 & CONTROL_VALUE_TYPE_MASK, CONTROL_VENDOR);
    EXPECT_TRUE((raw3 & CONTROL_VALUE_TYPE_READONLY_FLAG) != 0U);
    EXPECT_TRUE((raw3 & CONTROL_VALUE_TYPE_UNKNOWN_FLAG) != 0U);
}

TEST(aem_control_values, element_size_spec_table)
{
    EXPECT_EQ(control_value_element_size(CONTROL_LINEAR_INT8), 1U);
    EXPECT_EQ(control_value_element_size(CONTROL_LINEAR_UINT16), 2U);
    EXPECT_EQ(control_value_element_size(CONTROL_LINEAR_INT32), 4U);
    EXPECT_EQ(control_value_element_size(CONTROL_LINEAR_DOUBLE), 8U);
    EXPECT_EQ(control_value_element_size(CONTROL_SELECTOR_STRING), 2U);
    EXPECT_EQ(control_value_element_size(CONTROL_ARRAY_FLOAT), 4U);
    EXPECT_EQ(control_value_element_size(CONTROL_SMPTE_TIME), 10U);
    EXPECT_EQ(control_value_element_size(CONTROL_GPTP_TIME), 10U);
    EXPECT_EQ(control_value_element_size(CONTROL_SAMPLE_RATE), 4U);
    EXPECT_EQ(control_value_element_size(CONTROL_BODE_PLOT), 12U);
    EXPECT_EQ(control_value_element_size(CONTROL_UTF8), 0U);
    EXPECT_EQ(control_value_element_size(CONTROL_VENDOR), 0U);
}

TEST(aem_control_values, family_predicates)
{
    EXPECT_TRUE(is_linear_value_type(CONTROL_LINEAR_INT8));
    EXPECT_TRUE(is_linear_value_type(CONTROL_LINEAR_DOUBLE));
    EXPECT_FALSE(is_linear_value_type(CONTROL_SELECTOR_INT8));

    EXPECT_TRUE(is_selector_value_type(CONTROL_SELECTOR_INT8));
    EXPECT_TRUE(is_selector_value_type(CONTROL_SELECTOR_STRING));
    EXPECT_FALSE(is_selector_value_type(CONTROL_ARRAY_INT8));

    EXPECT_TRUE(is_array_value_type(CONTROL_ARRAY_INT8));
    EXPECT_TRUE(is_array_value_type(CONTROL_ARRAY_DOUBLE));
    EXPECT_FALSE(is_array_value_type(CONTROL_UTF8));
}

TEST(aem_control_values, value_type_names)
{
    EXPECT_EQ(control_value_type_name(CONTROL_LINEAR_UINT16), "CONTROL_LINEAR_UINT16");
    EXPECT_EQ(control_value_type_name(CONTROL_UTF8), "CONTROL_UTF8");
    EXPECT_EQ(control_value_type_name(CONTROL_VENDOR), "CONTROL_VENDOR");
    EXPECT_EQ(control_value_type_name(CONTROL_VALUE_TYPE_EXPANSION), "EXPANSION");
    EXPECT_EQ(control_value_type_name(0x1000), "Reserved");
}

TEST(aem_control_values, linear_entry_uint8_accessors)
{
    LinearValueEntry<uint8_t> e{};
    e.set_minimum(0);
    e.set_maximum(255);
    e.set_step(1);
    e.set_default_value(128);
    e.set_current(42);
    e.unit = ControlUnits{0, UNIT_CODE_UNITLESS};

    EXPECT_EQ(e.minimum(), 0U);
    EXPECT_EQ(e.maximum(), 255U);
    EXPECT_EQ(e.step(), 1U);
    EXPECT_EQ(e.default_value(), 128U);
    EXPECT_EQ(e.current(), 42U);
    EXPECT_EQ(e.unit.code.get(), UNIT_CODE_UNITLESS);
}

TEST(aem_control_values, linear_entry_int16_signed_roundtrip)
{
    LinearValueEntry<int16_t> e{};
    e.set_minimum(-32768);
    e.set_maximum(32767);
    e.set_step(10);
    e.set_default_value(0);
    e.set_current(-1234);

    EXPECT_EQ(e.minimum(), -32768);
    EXPECT_EQ(e.maximum(), 32767);
    EXPECT_EQ(e.current(), -1234);
}

TEST(aem_control_values, linear_entry_float_roundtrip)
{
    LinearValueEntry<float> e{};
    e.set_minimum(-1.0F);
    e.set_maximum(1.0F);
    e.set_step(0.1F);
    e.set_default_value(0.0F);
    e.set_current(0.25F);

    EXPECT_TRUE(e.minimum() == -1.0F);
    EXPECT_TRUE(e.maximum() == 1.0F);
    EXPECT_TRUE(e.current() == 0.25F);
}

TEST(aem_control_values, linear_entry_uint64_roundtrip)
{
    LinearValueEntry<uint64_t> e{};
    e.set_minimum(0);
    e.set_maximum(0xFFFFFFFFFFFFFFFFULL);
    e.set_current(0x0123456789ABCDEFULL);

    EXPECT_EQ(e.minimum(), 0ULL);
    EXPECT_EQ(e.maximum(), 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(e.current(), 0x0123456789ABCDEFULL);
    EXPECT_EQ(sizeof(e), 44U);
}

TEST(aem_control_values, gptp_time_value_seconds_roundtrip)
{
    GptpTimeValue g{};
    g.set_seconds(0x0123456789ABULL);
    g.gptp_nanoseconds = 500'000'000;

    EXPECT_EQ(g.seconds(), 0x0123456789ABULL);
    EXPECT_EQ(g.gptp_nanoseconds.get(), 500'000'000U);
    // Wire bytes are MSB-first
    EXPECT_EQ(g.gptp_seconds.span()[0], 0x01);
    EXPECT_EQ(g.gptp_seconds.span()[5], 0xAB);
}

TEST(aem_control_values, smpte_time_value_sizeof)
{
    EXPECT_EQ(sizeof(SmpteTimeValue), 10U);
}

TEST(aem_control_values, sample_rate_value_sizeof)
{
    EXPECT_EQ(sizeof(SampleRateValue), 16U);
}

TEST(aem_control_values, bode_plot_header_sizeof)
{
    EXPECT_EQ(sizeof(BodePlotHeader), 48U);
    EXPECT_EQ(sizeof(BodePlotPoint), 12U);
}

TEST_MAIN(statusbar_atdecc, atdecc_aem_control_values_test)
