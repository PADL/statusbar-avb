// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPort tests: the tagged header and the receive payload offset, the
/// PacketStreamPort before it opens, and TestStreamPort's frame contract.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"

#include "statusbar/avb_entity/avb_entity_test_stream_port.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;

namespace {

ieee::Eui48 const kMac{0x00, 0x1B, 0x21, 0x0A, 0x0B, 0x0C};
ieee::Eui48 const kDest{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};

}  // namespace

// The header is the one RawnetContext::send_vlan writes: dest, source, TPID
// 0x8100, TCI pcp<<13 | vid (out-of-range bits masked), ethertype 0x22F0.
TEST(stream_port_header, matches_the_send_vlan_layout)
{
    std::array<uint8_t, StreamPort::TX_PAYLOAD_OFFSET> header{};
    write_stream_header(header, kDest, kMac, 0x0123, 3);
    std::array<uint8_t, 18> const expected{
        0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01, 0x00, 0x1B, 0x21, 0x0A, 0x0B, 0x0C, 0x81, 0x00, 0x61, 0x23, 0x22, 0xF0};
    EXPECT_TRUE(std::ranges::equal(header, expected));
    write_stream_header(header, kDest, kMac, 0xF002, 0x0F);
    EXPECT_EQ(header[14], uint8_t{0xE0});
    EXPECT_EQ(header[15], uint8_t{0x02});
}

TEST(stream_port_header, payload_offset_is_14_untagged_18_tagged_0_otherwise)
{
    std::array<uint8_t, 24> frame{};
    write_stream_header(frame, kDest, kMac, 2, 3);
    EXPECT_EQ(stream_payload_offset(frame), size_t{18});
    frame[12] = 0x22;
    frame[13] = 0xF0;
    EXPECT_EQ(stream_payload_offset(frame), size_t{14});
    frame[13] = 0xEA;  // MSRP
    EXPECT_EQ(stream_payload_offset(frame), size_t{0});
    write_stream_header(frame, kDest, kMac, 2, 3);
    frame[17] = 0xF7;  // tagged, not AVTP
    EXPECT_EQ(stream_payload_offset(frame), size_t{0});
    EXPECT_EQ(stream_payload_offset(std::span{frame}.first(13)), size_t{0});
}

// Before it opens, the packet port neither transmits nor receives.
TEST(packet_stream_port, a_closed_port_has_no_window_and_receives_nothing)
{
    PacketStreamPort port;
    EXPECT_TRUE(port.tx_begin(64).empty());
    EXPECT_FALSE(port.tx_commit({}, kDest, 2, 3).has_value());
    EXPECT_TRUE(port.tx_flush().has_value());
    EXPECT_EQ(port.rx_poll([](std::span<uint8_t const>, std::span<uint8_t const>) {}, 8), size_t{0});
    EXPECT_EQ(port.rx_fd(), -1);
    EXPECT_FALSE(port.tx_open());
    EXPECT_FALSE(port.rx_open());
    EXPECT_FALSE(port.zero_copy());
    EXPECT_EQ(port.stats().tx_errors, uint64_t{1});
    EXPECT_FALSE(port.open_tx("no-such-if0", 3).has_value());
    EXPECT_FALSE(port.open_rx("no-such-if0").has_value());
}

// The window starts TX_PAYLOAD_OFFSET into the frame, an uncommitted window
// is handed out again, and a span that is not the window is refused.
TEST(test_stream_port, the_window_sits_behind_the_tagged_header)
{
    TestStreamPort port{kMac};
    EXPECT_TRUE(port.tx_begin(StreamPort::MAX_TX_PAYLOAD + 1).empty());
    auto const window = port.tx_begin(4);
    EXPECT_EQ(window.size(), size_t{4});
    EXPECT_EQ(port.tx_begin(8).data(), window.data());  // not committed: reused

    std::array<uint8_t, 4> const elsewhere{};
    EXPECT_FALSE(port.tx_commit(elsewhere, kDest, 2, 3).has_value());

    std::vector<uint8_t> tapped;
    port.set_tx_tap([&tapped](std::span<uint8_t const> frame) { tapped.assign(frame.begin(), frame.end()); });
    std::ranges::copy(std::array<uint8_t, 4>{0x02, 0x81, 0x00, 0x00}, window.begin());
    EXPECT_TRUE(port.tx_commit(window.first(4), kDest, 2, 3).has_value());
    auto const sent = port.pop_tx();
    EXPECT_TRUE(sent.has_value());
    std::span<uint8_t const> const frame = sent->frame;
    EXPECT_EQ(frame.size(), StreamPort::TX_PAYLOAD_OFFSET + 4);
    EXPECT_EQ(stream_payload_offset(frame), StreamPort::TX_PAYLOAD_OFFSET);
    EXPECT_EQ(frame[18], uint8_t{0x02});
    EXPECT_TRUE(std::ranges::equal(tapped, frame));
    EXPECT_EQ(port.stats().tx_frames, uint64_t{1});
    EXPECT_EQ(port.stats().tx_errors, uint64_t{1});
    EXPECT_FALSE(port.tx_commit(window.first(4), kDest, 2, 3).has_value());  // already committed
}

TEST(test_stream_port, rx_poll_stops_at_its_bound)
{
    TestStreamPort port{kMac};
    std::array<uint8_t, 24> frame{};
    write_stream_header(frame, kDest, kMac, 2, 3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(port.inject_rx(frame).has_value());
    }
    size_t payload_size = 0;
    auto const fn = [&payload_size](std::span<uint8_t const> payload, std::span<uint8_t const>) { payload_size = payload.size(); };
    EXPECT_EQ(port.rx_poll(fn, 2), size_t{2});
    EXPECT_EQ(payload_size, size_t{6});
    EXPECT_EQ(port.rx_poll(fn, 2), size_t{1});
    EXPECT_EQ(port.rx_poll(fn, 2), size_t{0});
}

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_port_test)
