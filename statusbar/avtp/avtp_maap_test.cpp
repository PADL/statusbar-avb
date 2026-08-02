// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"
#include "statusbar/test/test.hpp"
#include "statusbar/tsn/tsn.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::tsn;
using namespace statusbar::ieee;

//
// Compile-time verification of constexpr functions using static_assert
//

// MAAP message type names - tested at runtime (non-constexpr)

TEST(maap_names, message_type_name)
{
    EXPECT_EQ(maap_message_type_name(MAAP_MESSAGE_TYPE_PROBE)[0], 'P');
    EXPECT_EQ(maap_message_type_name(MAAP_MESSAGE_TYPE_DEFEND)[0], 'D');
    EXPECT_EQ(maap_message_type_name(MAAP_MESSAGE_TYPE_ANNOUNCE)[0], 'A');
    EXPECT_EQ(maap_message_type_name(0xFF)[0], 'U');  // Unknown
}

//
// Tests: MAAP Constants
//

TEST(maap_constants, subtype)
{
    EXPECT_EQ(AvtpSubtype::maap, 0xFE);
}

TEST(maap_constants, message_types)
{
    EXPECT_EQ(MAAP_MESSAGE_TYPE_PROBE, 1);
    EXPECT_EQ(MAAP_MESSAGE_TYPE_DEFEND, 2);
    EXPECT_EQ(MAAP_MESSAGE_TYPE_ANNOUNCE, 3);
}

TEST(maap_constants, timing)
{
    EXPECT_EQ(MAAP_PROBE_RETRANSMITS, 3U);
    EXPECT_EQ(MAAP_PROBE_INTERVAL_BASE_US, 500000U);
    EXPECT_EQ(MAAP_PROBE_INTERVAL_VARIATION_US, 100000U);
    EXPECT_EQ(MAAP_ANNOUNCE_INTERVAL_BASE_US, 30000000U);
    EXPECT_EQ(MAAP_ANNOUNCE_INTERVAL_VARIATION_US, 2000000U);
}

TEST(maap_constants, pool_addresses)
{
    // Dynamic pool: 91:e0:f0:00:00:00 to 91:e0:f0:00:fd:ff
    EXPECT_EQ(MAAP_DYNAMIC_POOL_START.value[0], 0x91);
    EXPECT_EQ(MAAP_DYNAMIC_POOL_START.value[1], 0xe0);
    EXPECT_EQ(MAAP_DYNAMIC_POOL_START.value[2], 0xf0);
    EXPECT_EQ(MAAP_DYNAMIC_POOL_START.value[3], 0x00);
    EXPECT_EQ(MAAP_DYNAMIC_POOL_START.value[4], 0x00);
    EXPECT_EQ(MAAP_DYNAMIC_POOL_START.value[5], 0x00);

    EXPECT_EQ(MAAP_DYNAMIC_POOL_END.value[5], 0xff);
    EXPECT_EQ(MAAP_DYNAMIC_POOL_END.value[4], 0xfd);

    // Local pool: 91:e0:f0:00:fe:00 to 91:e0:f0:00:fe:ff
    EXPECT_EQ(MAAP_LOCAL_POOL_START.value[4], 0xfe);
    EXPECT_EQ(MAAP_LOCAL_POOL_START.value[5], 0x00);
    EXPECT_EQ(MAAP_LOCAL_POOL_END.value[4], 0xfe);
    EXPECT_EQ(MAAP_LOCAL_POOL_END.value[5], 0xff);
}

//
// Tests: MAAP Message Type Names
//

TEST(maap_type_names, probe)
{
    EXPECT_TRUE(std::string(maap_message_type_name(MAAP_MESSAGE_TYPE_PROBE)) == "Probe");
}

TEST(maap_type_names, defend)
{
    EXPECT_TRUE(std::string(maap_message_type_name(MAAP_MESSAGE_TYPE_DEFEND)) == "Defend");
}

TEST(maap_type_names, announce)
{
    EXPECT_TRUE(std::string(maap_message_type_name(MAAP_MESSAGE_TYPE_ANNOUNCE)) == "Announce");
}

TEST(maap_type_names, unknown)
{
    EXPECT_TRUE(std::string(maap_message_type_name(0xFF)) == "Unknown");
}

//
// Tests: MaapDu Structure
//

TEST(maapdu_struct, size)
{
    EXPECT_EQ(sizeof(MaapDu), 28U);
    EXPECT_EQ(MaapDu::LENGTH, 28U);
    EXPECT_EQ(MaapDu::HEADER_LENGTH, 12U);
    EXPECT_EQ(MaapDu::DATA_LENGTH, 16U);
}

TEST(maapdu_struct, default_constructor)
{
    MaapDu maap;
    EXPECT_EQ(maap.subtype.get(), 0);
    EXPECT_EQ(maap.message_type(), 0);
    EXPECT_EQ(maap.requested_count.get(), 0);
    EXPECT_EQ(maap.conflict_count.get(), 0);
}

//
// Tests: MaapDu Header Field Accessors
//

TEST(maapdu_fields, sv_bit)
{
    MaapDu maap;
    maap.sv_version_msgtype = 0x80;  // Set SV bit
    EXPECT_TRUE(maap.sv());

    maap.sv_version_msgtype = 0x00;
    EXPECT_FALSE(maap.sv());
}

TEST(maapdu_fields, version)
{
    MaapDu maap;
    maap.sv_version_msgtype = 0x50;  // Version = 5
    EXPECT_EQ(maap.version(), 5);

    maap.sv_version_msgtype = 0x70;  // Version = 7
    EXPECT_EQ(maap.version(), 7);
}

TEST(maapdu_fields, message_type_get)
{
    MaapDu maap;
    maap.sv_version_msgtype = MAAP_MESSAGE_TYPE_PROBE;
    EXPECT_EQ(maap.message_type(), MAAP_MESSAGE_TYPE_PROBE);

    maap.sv_version_msgtype = MAAP_MESSAGE_TYPE_DEFEND;
    EXPECT_EQ(maap.message_type(), MAAP_MESSAGE_TYPE_DEFEND);

    maap.sv_version_msgtype = MAAP_MESSAGE_TYPE_ANNOUNCE;
    EXPECT_EQ(maap.message_type(), MAAP_MESSAGE_TYPE_ANNOUNCE);
}

TEST(maapdu_fields, message_type_set)
{
    MaapDu maap;
    maap.sv_version_msgtype = 0x70;  // Version = 7

    maap.set_message_type(MAAP_MESSAGE_TYPE_ANNOUNCE);
    EXPECT_EQ(maap.message_type(), MAAP_MESSAGE_TYPE_ANNOUNCE);
    EXPECT_EQ(maap.version(), 7);  // Version should be preserved
}

TEST(maapdu_fields, maap_version)
{
    MaapDu maap;
    maap.set_maap_version(15);
    EXPECT_EQ(maap.maap_version(), 15);

    maap.set_maap_version(0);
    EXPECT_EQ(maap.maap_version(), 0);
}

TEST(maapdu_fields, maap_data_length)
{
    MaapDu maap;
    maap.set_maap_data_length(16);
    EXPECT_EQ(maap.maap_data_length(), 16);

    maap.set_maap_data_length(255);
    EXPECT_EQ(maap.maap_data_length(), 255);

    // Test high bits (11-bit field)
    maap.set_maap_data_length(0x7FF);  // Maximum 11-bit value
    EXPECT_EQ(maap.maap_data_length(), 0x7FF);
}

TEST(maapdu_fields, stream_id)
{
    MaapDu maap;
    Eui48 system_addr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
    StreamId sid(system_addr, 0xABCD);

    maap.set_stream_id(sid);
    StreamId result = maap.stream_id();

    EXPECT_TRUE(result.get_system_address() == system_addr);
    EXPECT_EQ(result.get_unique_id(), 0xABCD);
}

//
// Tests: MaapDu Message Type Helpers
//

TEST(maapdu_type_check, is_probe)
{
    MaapDu maap;
    maap.sv_version_msgtype = MAAP_MESSAGE_TYPE_PROBE;

    EXPECT_TRUE(maap.is_probe());
    EXPECT_FALSE(maap.is_defend());
    EXPECT_FALSE(maap.is_announce());
}

TEST(maapdu_type_check, is_defend)
{
    MaapDu maap;
    maap.sv_version_msgtype = MAAP_MESSAGE_TYPE_DEFEND;

    EXPECT_FALSE(maap.is_probe());
    EXPECT_TRUE(maap.is_defend());
    EXPECT_FALSE(maap.is_announce());
}

TEST(maapdu_type_check, is_announce)
{
    MaapDu maap;
    maap.sv_version_msgtype = MAAP_MESSAGE_TYPE_ANNOUNCE;

    EXPECT_FALSE(maap.is_probe());
    EXPECT_FALSE(maap.is_defend());
    EXPECT_TRUE(maap.is_announce());
}

//
// Tests: MaapDu Initialization
//

TEST(maapdu_init, probe)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);

    EXPECT_TRUE(maap.subtype == AvtpSubtype::maap);
    EXPECT_TRUE(maap.is_probe());
    EXPECT_EQ(maap.maap_version(), 1);
    EXPECT_EQ(maap.maap_data_length(), 16);
    EXPECT_TRUE(maap.requested_start_address == start_addr);
    EXPECT_EQ(maap.requested_count.get(), 8);
    EXPECT_EQ(maap.conflict_count.get(), 0);
}

TEST(maapdu_init, defend)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 req_start(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);
    Eui48 conf_start(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x18);

    maap.init_defend(sid, req_start, 8, conf_start, 4);

    EXPECT_TRUE(maap.subtype == AvtpSubtype::maap);
    EXPECT_TRUE(maap.is_defend());
    EXPECT_EQ(maap.maap_version(), 1);
    EXPECT_EQ(maap.maap_data_length(), 16);
    EXPECT_TRUE(maap.requested_start_address == req_start);
    EXPECT_EQ(maap.requested_count.get(), 8);
    EXPECT_TRUE(maap.conflict_start_address == conf_start);
    EXPECT_EQ(maap.conflict_count.get(), 4);
}

TEST(maapdu_init, announce)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_announce(sid, start_addr, 8);

    EXPECT_TRUE(maap.subtype == AvtpSubtype::maap);
    EXPECT_TRUE(maap.is_announce());
    EXPECT_EQ(maap.maap_version(), 1);
    EXPECT_EQ(maap.maap_data_length(), 16);
    EXPECT_TRUE(maap.requested_start_address == start_addr);
    EXPECT_EQ(maap.requested_count.get(), 8);
    EXPECT_EQ(maap.conflict_count.get(), 0);
}

//
// Tests: MaapDu Serialization
//

TEST(maapdu_serial, roundtrip)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0xABCD);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x12, 0x34);

    maap.init_probe(sid, start_addr, 16);

    std::array<uint8_t, 28> buf{};
    auto stored = store_unchecked(buf, maap);
    EXPECT_EQ(stored, 28U);

    MaapDu loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 28U);

    EXPECT_TRUE(loaded.subtype == AvtpSubtype::maap);
    EXPECT_TRUE(loaded.is_probe());
    EXPECT_EQ(loaded.maap_data_length(), 16);
    EXPECT_TRUE(loaded.requested_start_address == start_addr);
    EXPECT_EQ(loaded.requested_count.get(), 16);
}

TEST(maapdu_serial, defend_roundtrip)
{
    MaapDu maap;
    Eui48 system_addr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
    StreamId sid(system_addr, 0x1234);
    Eui48 req_start(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x00);
    Eui48 conf_start(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x04);

    maap.init_defend(sid, req_start, 8, conf_start, 4);

    std::array<uint8_t, 28> buf{};
    (void)store_unchecked(buf, maap);

    MaapDu loaded;
    (void)load_unchecked(buf, &loaded);

    EXPECT_TRUE(loaded.is_defend());
    EXPECT_TRUE(loaded.requested_start_address == req_start);
    EXPECT_EQ(loaded.requested_count.get(), 8);
    EXPECT_TRUE(loaded.conflict_start_address == conf_start);
    EXPECT_EQ(loaded.conflict_count.get(), 4);
}

//
// Tests: MaapDu is_valid()
//

TEST(maapdu_is_valid, valid_probe)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);

    EXPECT_TRUE(maap.is_valid());
}

TEST(maapdu_is_valid, valid_defend)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 req_start(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);
    Eui48 conf_start(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x18);

    maap.init_defend(sid, req_start, 8, conf_start, 4);

    EXPECT_TRUE(maap.is_valid());
}

TEST(maapdu_is_valid, valid_announce)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_announce(sid, start_addr, 8);

    EXPECT_TRUE(maap.is_valid());
}

TEST(maapdu_is_valid, wrong_subtype)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);
    maap.subtype = 0x02;  // AAF subtype

    EXPECT_FALSE(maap.is_valid());
}

TEST(maapdu_is_valid, invalid_message_type_zero)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);
    maap.set_message_type(0);  // Invalid: message type 0 is reserved

    EXPECT_FALSE(maap.is_valid());
}

TEST(maapdu_is_valid, invalid_message_type_four)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);
    maap.set_message_type(4);  // Invalid: message types are 1-3

    EXPECT_FALSE(maap.is_valid());
}

// IEEE 1722-2025 B.2.2 mandates ignoring a MAAP AVTPDU only on a RESERVED
// message_type -- not on a nonconformant control_data_length. The MAAP fields are
// fixed-offset, so is_valid() tolerates a wrong data length and parses the PDU.
TEST(maapdu_is_valid, nonconformant_data_length_is_tolerated)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);
    maap.set_maap_data_length(20);  // not the spec value of 16 -- still parseable

    EXPECT_TRUE(maap.is_valid());
}

// Interop (receive side): we emit maap_version 1, but must ACCEPT any maap_version
// on ingress. IEEE 1722-2025 B.2.2 gates only on a reserved message_type, and MAAP
// fields are fixed-offset regardless of version. Rejecting version 0 would break
// address defense against older/other stations (incl. our own pre-fix nodes).
TEST(maapdu_is_valid, foreign_maap_version_is_accepted)
{
    MaapDu maap;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    maap.init_probe(sid, start_addr, 8);

    maap.set_maap_version(0);  // an older/other station (or our own pre-fix node)
    EXPECT_TRUE(maap.is_valid());

    maap.set_maap_version(31);  // an unknown future version -- still accepted
    EXPECT_TRUE(maap.is_valid());
}

//
// Tests: MaapDu Comparison
//

TEST(maapdu_compare, equality)
{
    MaapDu a, b;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    a.init_probe(sid, start_addr, 8);
    b.init_probe(sid, start_addr, 8);

    EXPECT_TRUE(a == b);
}

TEST(maapdu_compare, inequality)
{
    MaapDu a, b;
    Eui48 system_addr(0x00, 0x11, 0x22, 0x33, 0x44, 0x55);
    StreamId sid(system_addr, 0x0001);
    Eui48 start_addr(0x91, 0xe0, 0xf0, 0x00, 0x00, 0x10);

    a.init_probe(sid, start_addr, 8);
    b.init_probe(sid, start_addr, 16);  // Different count

    EXPECT_TRUE(a != b);
}

// ===========================================================================
// MAAP message type name test
// ===========================================================================

TEST(avtp_maap_names, message_type_name_known)
{
    EXPECT_EQ(std::string(statusbar::avtp::maap_message_type_name(statusbar::avtp::MAAP_MESSAGE_TYPE_PROBE)), "Probe");
    EXPECT_EQ(std::string(statusbar::avtp::maap_message_type_name(statusbar::avtp::MAAP_MESSAGE_TYPE_DEFEND)), "Defend");
    EXPECT_EQ(std::string(statusbar::avtp::maap_message_type_name(statusbar::avtp::MAAP_MESSAGE_TYPE_ANNOUNCE)), "Announce");
}

TEST(avtp_maap_names, message_type_name_unknown)
{
    auto const name = statusbar::avtp::maap_message_type_name(0);
    EXPECT_TRUE(!name.empty());
    auto const name2 = statusbar::avtp::maap_message_type_name(255);
    EXPECT_TRUE(!name2.empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_avtp, avtp_maap_test)