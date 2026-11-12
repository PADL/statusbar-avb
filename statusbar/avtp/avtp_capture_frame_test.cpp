// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_capture_frame.hpp"

#include "statusbar/avtp/avtp_types.hpp"
#include "statusbar/ieee/ieee_protocols.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <span>

using namespace statusbar::avtp;

TEST(avtp_capture_frame, classic_pcap_magics_in_either_byte_order_are_pcap)
{
    std::array<uint8_t, 4> const little_us{0xD4, 0xC3, 0xB2, 0xA1};  // as a little-endian writer lays it down
    std::array<uint8_t, 4> const big_us{0xA1, 0xB2, 0xC3, 0xD4};
    std::array<uint8_t, 4> const little_ns{0x4D, 0x3C, 0xB2, 0xA1};
    std::array<uint8_t, 4> const big_ns{0xA1, 0xB2, 0x3C, 0x4D};
    EXPECT_TRUE(capture_format_of(little_us) == CaptureFormat::pcap);
    EXPECT_TRUE(capture_format_of(big_us) == CaptureFormat::pcap);
    EXPECT_TRUE(capture_format_of(little_ns) == CaptureFormat::pcap);
    EXPECT_TRUE(capture_format_of(big_ns) == CaptureFormat::pcap);
}

TEST(avtp_capture_frame, a_section_header_block_is_pcapng_and_anything_else_is_unknown)
{
    std::array<uint8_t, 4> const shb{0x0A, 0x0D, 0x0D, 0x0A};
    std::array<uint8_t, 4> const text{'R', 'I', 'F', 'F'};
    std::array<uint8_t, 4> const zeros{};
    EXPECT_TRUE(capture_format_of(shb) == CaptureFormat::pcapng);
    EXPECT_TRUE(capture_format_of(text) == CaptureFormat::unknown);
    EXPECT_TRUE(capture_format_of(zeros) == CaptureFormat::unknown);
}

TEST(avtp_capture_frame, an_untagged_frame_passes_through_unchanged)
{
    std::array<uint8_t, 6> const payload{0x02, 0x80, 0x00, 0x01, 0x02, 0x03};  // an AVTP payload, subtype 0x02
    auto const inner = strip_vlan_tag(statusbar::ieee::protocols::ETHERTYPE_AVTP, payload);
    EXPECT_EQ(inner.ethertype, statusbar::ieee::protocols::ETHERTYPE_AVTP);
    EXPECT_EQ(inner.payload.size(), payload.size());
    EXPECT_EQ(inner.payload.data(), payload.data());
    EXPECT_FALSE(inner.vlan_tag.is_set());
}

// A stream frame on the wire is 802.1Q tagged (SR class A: VID 2, PCP 3). A reader
// that splits at the 14-octet header reports the TPID as the ethertype and the TCI,
// inner ethertype and data as the payload; the tool filtered on the TPID and skipped
// every tagged AVTP frame.
TEST(avtp_capture_frame, a_tagged_frame_yields_the_inner_ethertype_payload_and_tag)
{
    // TCI: PCP 3, DEI 0, VID 2 = 0x6002; then the AVTP ethertype 0x22F0; then the AVTPDU.
    std::array<uint8_t, 8> const payload{0x60, 0x02, 0x22, 0xF0, 0x02, 0x80, 0x00, 0x01};
    auto const inner = strip_vlan_tag(statusbar::ieee::VlanTag::ETHERTYPE, payload);
    EXPECT_EQ(inner.ethertype, statusbar::ieee::protocols::ETHERTYPE_AVTP);
    EXPECT_EQ(inner.payload.size(), 4U);
    EXPECT_EQ(inner.payload[0], 0x02);
    EXPECT_TRUE(inner.vlan_tag.is_set());
    EXPECT_EQ(inner.vlan_tag.get_vid(), 2U);
    EXPECT_EQ(inner.vlan_tag.get_pcp(), 3U);
}

TEST(avtp_capture_frame, a_tag_with_nothing_inside_is_left_alone)
{
    std::array<uint8_t, 3> const short_payload{0x60, 0x02, 0x22};
    auto const inner = strip_vlan_tag(statusbar::ieee::VlanTag::ETHERTYPE, short_payload);
    EXPECT_EQ(inner.ethertype, statusbar::ieee::VlanTag::ETHERTYPE);
    EXPECT_EQ(inner.payload.size(), 3U);
}

TEST_MAIN(statusbar_avtp, avtp_capture_frame_test)
