// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <span>

using namespace statusbar;
using namespace statusbar::atdecc::aem;

TEST(aem_control_types, oui_detection)
{
    EXPECT_TRUE(is_standard_control_type(CONTROL_TYPE_GAIN));
    EXPECT_TRUE(is_standard_control_type(CONTROL_TYPE_FQTSS_LOCK_CLASS_BANDWIDTH));
    // Random non-IEEE OUI
    Eui64 const vendor{0x70, 0xB3, 0xD5, 0x00, 0x00, 0x00, 0x00, 0x00};
    EXPECT_FALSE(is_standard_control_type(vendor));
    EXPECT_FALSE(is_standard_control_type(Eui64{}));  // all zeros
}

TEST(aem_control_types, standard_names)
{
    EXPECT_EQ(control_type_name(CONTROL_TYPE_ENABLE), "ENABLE");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_IDENTIFY), "IDENTIFY");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_MUTE), "MUTE");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_GAIN), "GAIN");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_LATENCY_COMPENSATION), "LATENCY_COMPENSATION");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_PANPOT), "PANPOT");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_SCANNING_MODE), "SCANNING_MODE");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_DIGITAL_ZOOM), "DIGITAL_ZOOM");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_MEDIA_PLAYLIST), "MEDIA_PLAYLIST");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_FREQUENCY), "FREQUENCY");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_BAUD_RATE), "BAUD_RATE");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_FQTSS_LOCK_CLASS_BANDWIDTH), "FQTSS_LOCK_CLASS_BANDWIDTH");
}

TEST(aem_control_types, vendor_defined_returns_vendor_defined)
{
    Eui64 const vendor{0x70, 0xB3, 0xD5, 0x00, 0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(control_type_name(vendor), "VENDOR_DEFINED");
}

TEST(aem_control_types, reserved_in_ieee_oui_returns_unknown)
{
    // 0x90:e0:f0:00:00:00:00:1d is in the reserved gap at the end of the
    // general category per Table 7.4.
    Eui64 const reserved{0x90, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x1D};
    EXPECT_EQ(control_type_name(reserved), "UNKNOWN");
}

TEST(aem_control_types, standard_entries_enumerable_and_sorted)
{
    auto const entries = standard_control_type_entries();
    EXPECT_EQ(entries.size(), size_t{82});
    for (size_t i = 1; i < entries.size(); ++i) {
        EXPECT_TRUE(entries[i - 1].type < entries[i].type);
    }
    // Every enumerated entry names itself through the lookup.
    for (auto const& e : entries) {
        EXPECT_EQ(control_type_name(e.type), e.name);
    }
}

TEST(aem_control_types, vendor_table_names_vendor_types)
{
    constexpr Eui64 acme_special{0x70, 0xB3, 0xD5, 0x00, 0x00, 0x00, 0x12, 0x34};
    constexpr Eui64 acme_other{0x70, 0xB3, 0xD5, 0x00, 0x00, 0x00, 0x56, 0x78};
    constexpr std::array<ControlTypeEntry, 2> vendor{{
        {.type = acme_special, .name = "ACME_SPECIAL"},
        {.type = acme_other, .name = "ACME_OTHER"},
    }};

    EXPECT_EQ(control_type_name(acme_special, vendor), "ACME_SPECIAL");
    EXPECT_EQ(control_type_name(acme_other, vendor), "ACME_OTHER");

    // Standard types still resolve through the fallback.
    EXPECT_EQ(control_type_name(CONTROL_TYPE_GAIN, vendor), "GAIN");

    // Unlisted vendor types keep the generic label.
    Eui64 const unlisted{0x70, 0xB3, 0xD5, 0x00, 0x00, 0x00, 0xFF, 0xFF};
    EXPECT_EQ(control_type_name(unlisted, vendor), "VENDOR_DEFINED");

    // An empty table behaves exactly like the single-argument lookup.
    EXPECT_EQ(control_type_name(acme_special, {}), "VENDOR_DEFINED");
}

TEST_MAIN(statusbar_atdecc, atdecc_aem_control_types_test)
