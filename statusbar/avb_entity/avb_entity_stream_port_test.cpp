// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// StreamPort: the backend name is parsed and refused when it is not one,
// the guard is free on a closed port, and the frame builder writes the
// tagged header the stream entities have always put on the wire.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"

#include "statusbar/net/net_error.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <span>

using namespace statusbar;
using namespace statusbar::avb_entity;

TEST(stream_port, a_name_that_is_not_a_backend_is_refused_before_any_socket)
{
    StreamPort port;
    auto const status = port.open({.backend = "packet", .interface_name = "lo"});
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error(), make_error_code(net::NetError::invalid_argument));
    EXPECT_FALSE(port.valid());
    EXPECT_FALSE(port.serialized());
    EXPECT_EQ(port.fd(), -1);
}

TEST(stream_port, a_closed_port_is_not_serialised_and_its_guard_is_free)
{
    StreamPort port;
    {
        StreamPort::Guard const guard{port};
        StreamPort::Guard const another{port};  // would deadlock if the lock were taken
    }
    EXPECT_FALSE(port.serialized());
    EXPECT_FALSE(port.port().tx_start().has_value());
}

TEST(stream_port, build_stream_frame_writes_a_tagged_avtp_header_then_the_payload)
{
    std::array<uint8_t, 64> buffer{};
    ieee::Eui48 const dest{0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80};
    ieee::Eui48 const src{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::array<uint8_t const, 4> const payload{0x02, 0x81, 0x00, 0x00};

    auto const length = build_stream_frame(buffer, dest, src, 2, 3, payload);
    EXPECT_EQ(length, size_t{18 + 4});

    std::array<uint8_t const, 22> const expected{
        0x91, 0xE0, 0xF0, 0x00, 0x0E, 0x80,  // dest
        0x02, 0x11, 0x22, 0x33, 0x44, 0x55,  // src
        0x81, 0x00, 0x60, 0x02,              // 802.1Q: PCP 3, DEI 0, VID 2
        0x22, 0xF0,                          // AVTP
        0x02, 0x81, 0x00, 0x00,              // payload
    };
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(buffer[i], expected[i]);
    }
}

TEST(stream_port, build_stream_frame_refuses_a_buffer_too_small_for_the_frame)
{
    std::array<uint8_t, 20> buffer{};
    std::array<uint8_t const, 4> const payload{0x02, 0x81, 0x00, 0x00};
    EXPECT_EQ(build_stream_frame(buffer, ieee::Eui48{}, ieee::Eui48{}, 2, 3, payload), size_t{0});
    for (auto const byte : buffer) {
        EXPECT_EQ(byte, uint8_t{0});
    }
}

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_port_test)
