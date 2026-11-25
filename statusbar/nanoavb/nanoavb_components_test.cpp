// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
//
// parse_announce_path_trace: the PATH_TRACE TLV of a gPTP Announce
// (IEEE 802.1AS-2020 Clause 10.6.3.3), read from crafted frames.

#include "statusbar/nanoavb/nanoavb_components.hpp"

#include "statusbar/buffer/buffer.hpp"
#include "statusbar/gptp/gptp.hpp"
#include "statusbar/gptp/gptp_tlv.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;

namespace {

ieee::Eui64 const kGm{0x00, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
ieee::Eui64 const kBridge{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x02};

/// A TLV of @p type carrying @p value.
auto tlv(uint16_t const type, std::span<uint8_t const> const value) -> std::vector<uint8_t>
{
    std::vector<uint8_t> bytes(gptp::TlvHeader::LENGTH + value.size());
    span_store(std::span{bytes}, gptp::TlvHeader{type, static_cast<uint16_t>(value.size())});
    span_copy(std::span{bytes}.subspan(gptp::TlvHeader::LENGTH), value);
    return bytes;
}

/// A PATH_TRACE TLV of @p ids.
auto path_trace_tlv(std::span<ieee::Eui64 const> const ids) -> std::vector<uint8_t>
{
    std::vector<uint8_t> value(ids.size() * ieee::Eui64::LENGTH);
    for (size_t i = 0; i < ids.size(); ++i) {
        span_store(std::span{value}.subspan(i * ieee::Eui64::LENGTH), ids[i]);
    }
    return tlv(gptp::TLV_TYPE_PATH_TRACE, value);
}

/// An Announce followed by @p tlvs, its messageLength @p message_length (the whole frame when 0).
auto announce_with(std::vector<uint8_t> const& tlvs, size_t const message_length = 0) -> std::vector<uint8_t>
{
    std::vector<uint8_t> frame(gptp::AnnounceMessage::LENGTH);
    frame.insert(frame.end(), tlvs.begin(), tlvs.end());
    gptp::AnnounceMessage announce{};
    announce.init(1);
    announce.grandmaster_identity = gptp::ClockIdentity{kGm};
    announce.header.message_length = static_cast<uint16_t>(message_length != 0 ? message_length : frame.size());
    span_store(std::span{frame}, announce);
    return frame;
}

}  // namespace

TEST(nanoavb_announce_path_trace, the_tlvs_identities_are_read_grandmaster_first)
{
    std::array const ids{kGm, kBridge};
    auto const path = parse_announce_path_trace(announce_with(path_trace_tlv(ids)));
    EXPECT_TRUE(std::ranges::equal(path, ids));
}

TEST(nanoavb_announce_path_trace, a_path_trace_after_another_tlv_is_found)
{
    std::array<uint8_t, 12> const other_value{};
    auto tlvs = tlv(gptp::TLV_TYPE_ORGANIZATION_EXTENSION, other_value);
    std::array const ids{kBridge};
    auto const trace = path_trace_tlv(ids);
    tlvs.insert(tlvs.end(), trace.begin(), trace.end());
    auto const path = parse_announce_path_trace(announce_with(tlvs));
    EXPECT_TRUE(std::ranges::equal(path, ids));
}

TEST(nanoavb_announce_path_trace, a_tlv_running_past_message_length_is_not_read)
{
    std::array const ids{kGm, kBridge};
    auto const tlvs = path_trace_tlv(ids);
    // messageLength stops one identity short of the TLV's end.
    auto const frame = announce_with(tlvs, gptp::AnnounceMessage::LENGTH + tlvs.size() - ieee::Eui64::LENGTH);
    EXPECT_TRUE(parse_announce_path_trace(frame).empty());
}

TEST(nanoavb_announce_path_trace, an_announce_without_the_tlv_or_cut_short_has_no_path_trace)
{
    EXPECT_TRUE(parse_announce_path_trace(announce_with({})).empty());
    auto const frame = announce_with({});
    EXPECT_TRUE(parse_announce_path_trace(std::span{frame}.first(gptp::AnnounceMessage::LENGTH - 1)).empty());
    std::array<uint8_t, 2> const stub{0x00, 0x08};  // a TLV header cut in half
    std::vector<uint8_t> const half(stub.begin(), stub.end());
    EXPECT_TRUE(parse_announce_path_trace(announce_with(half)).empty());
}

TEST(nanoavb_announce_path_trace, identities_past_the_capacity_are_dropped)
{
    std::vector<ieee::Eui64> ids(MAX_AS_PATH_LENGTH + 3, kBridge);
    ids.front() = kGm;
    auto const path = parse_announce_path_trace(announce_with(path_trace_tlv(ids)));
    EXPECT_TRUE(std::ranges::equal(path, std::span{ids}.first(MAX_AS_PATH_LENGTH)));
}

TEST_MAIN(statusbar_nanoavb, nanoavb_components_test)
