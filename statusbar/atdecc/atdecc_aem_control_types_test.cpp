// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"

#include "statusbar/atdecc/atdecc_jdks.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
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

TEST(aem_control_types, known_vendor_types_name_themselves)
{
    // The Meyer telemetry the Q1 / RZ expose at configuration level, and
    // the JDKS types atdecc_jdks.hpp defines — vendor-prefixed, Table 7.4
    // style, through the plain lookup.
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_ERASE_IDENTITY), "MEYER_ERASE_IDENTITY");
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_HARDWARE_INFO), "MEYER_HARDWARE_INFO");
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_FAN_STATUS), "MEYER_FAN_STATUS");
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_LOGGER), "MEYER_LOGGER");
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_ENGINE_LOGGER), "MEYER_ENGINE_LOGGER");
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_LED_INDICATOR_FIRMWARE_VERSION), "MEYER_LED_INDICATOR_FIRMWARE_VERSION");
    EXPECT_EQ(control_type_name(atdecc::jdks::CONTROL_LOG_TEXT), "JDKS_LOG_TEXT");
    EXPECT_EQ(control_type_name(atdecc::jdks::CONTROL_IPV4_PARAMETERS), "JDKS_IPV4_PARAMETERS");

    // The exact wire values the devices report.
    EXPECT_EQ(meyer::CONTROL_TYPE_ERASE_IDENTITY, (Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x10, 0x00, 0x2F}));
    EXPECT_EQ(meyer::CONTROL_TYPE_ENGINE_CODE, (Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0xF0, 0x00, 0x0C}));
    EXPECT_EQ(atdecc::jdks::CONTROL_LOG_TEXT, (Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x00}));

    // Neither vendor's OUI is the standard one; a sibling value in the
    // same vendor range that nobody registered stays generic.
    EXPECT_FALSE(is_standard_control_type(meyer::CONTROL_TYPE_ERASE_IDENTITY));
    EXPECT_FALSE(is_standard_control_type(atdecc::jdks::CONTROL_LOG_TEXT));
    EXPECT_EQ(control_type_name(Eui64{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x10, 0x00, 0x30}), "VENDOR_DEFINED");
    EXPECT_EQ(control_type_name(Eui64{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x02}), "VENDOR_DEFINED");
}

TEST(aem_control_types, known_vendor_entries_enumerable_sorted_and_distinct)
{
    auto const vendor = known_vendor_control_type_entries();
    auto const standard = standard_control_type_entries();
    EXPECT_EQ(vendor.size(), size_t{13});
    for (size_t i = 1; i < vendor.size(); ++i) {
        EXPECT_TRUE(vendor[i - 1].type < vendor[i].type);
    }
    for (auto const& e : vendor) {
        // Every entry names itself, is vendor-owned, and its name is an
        // identifier (the symbol generator lowercases it into a path
        // segment) that no standard or other vendor entry uses.
        EXPECT_EQ(control_type_name(e.type), e.name);
        EXPECT_FALSE(is_standard_control_type(e.type));
        for (char c : e.name) {
            EXPECT_TRUE((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        }
        EXPECT_EQ(std::ranges::count(vendor, e.name, &ControlTypeEntry::name), 1);
        for (auto const& s : standard) {
            EXPECT_NE(s.name, e.name);
        }
    }
}

TEST(aem_control_types, application_table_overrides_known_vendor_names)
{
    constexpr std::array<ControlTypeEntry, 1> vendor{{
        {.type = meyer::CONTROL_TYPE_FAN_STATUS, .name = "FANS"},
    }};
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_FAN_STATUS, vendor), "FANS");
    // Everything the table does not mention still reaches the built-ins.
    EXPECT_EQ(control_type_name(meyer::CONTROL_TYPE_TEMPERATURE, vendor), "MEYER_TEMPERATURE");
    EXPECT_EQ(control_type_name(CONTROL_TYPE_GAIN, vendor), "GAIN");
}

TEST_MAIN(statusbar_atdecc, atdecc_aem_control_types_test)
