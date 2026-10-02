// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_i2c.hpp"

#include "statusbar/avtp/avtp_acf_i2c_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace statusbar;
using namespace statusbar::avtp;

static_assert(sizeof(AcfI2cMessage) == AcfI2cMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 16> LAYOUT{
    0x1E, 0x04, 0x22, 0xF1, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x48, 0x33, 0x59, 0xA3};

/// A complete message on the wire: the fixed part alone (4 quadlets; this type carries no payload).

constexpr std::array<uint8_t, 16> IMAGE{
    0x1E, 0x04, 0x22, 0xF1, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x48, 0x33, 0x59, 0xA3};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfI2cMessage& m)
{
    m.set_pad_bits(0x0U);
    m.set_mtv(true);
    m.set_i2c_bus_id(0x2F1U);
    m.set_message_timestamp(0x102030405060708U);
    m.set_i2c_code(0x4U);
    m.set_trr(true);
    m.set_transaction_num(0x33U);
    m.set_evt(0x5U);
    m.set_exception_code(0x9U);
    m.set_i2c_data(0xA3U);
}

/// Check every field against its test value
void expect_test_values(AcfI2cMessage const& m)
{
    EXPECT_EQ(m.pad_bits(), 0x0U);
    EXPECT_TRUE(m.mtv());
    EXPECT_EQ(m.i2c_bus_id(), 0x2F1U);
    EXPECT_EQ(m.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(m.i2c_code(), 0x4U);
    EXPECT_TRUE(m.trr());
    EXPECT_EQ(m.transaction_num(), 0x33U);
    EXPECT_EQ(m.evt(), 0x5U);
    EXPECT_EQ(m.exception_code(), 0x9U);
    EXPECT_EQ(m.i2c_data(), 0xA3U);
}

}  // namespace

TEST(acf_i2c, init_and_layout)
{
    AcfI2cMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::i2c);
    EXPECT_EQ(m.header.msg_length(), 4U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 16> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_i2c, parse_wire_image)
{
    auto const view = acf_i2c_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 0U);
    EXPECT_EQ(view->fixed.pad_bits(), 0x0U);
    EXPECT_TRUE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.i2c_bus_id(), 0x2F1U);
    EXPECT_EQ(view->fixed.get_message_timestamp(), 0x102030405060708U);
    EXPECT_EQ(view->fixed.i2c_code(), 0x4U);
    EXPECT_TRUE(view->fixed.trr());
    EXPECT_EQ(view->fixed.transaction_num(), 0x33U);
    EXPECT_EQ(view->fixed.evt(), 0x5U);
    EXPECT_EQ(view->fixed.exception_code(), 0x9U);
    EXPECT_EQ(view->fixed.i2c_data(), 0xA3U);
    EXPECT_TRUE(view->padded_payload.empty());
    EXPECT_TRUE(view->payload.empty());
}

TEST(acf_i2c, build_round_trip)
{
    AcfI2cMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 16> out{};
    EXPECT_EQ(acf_i2c_build(std::span<uint8_t>(out), m, std::span<uint8_t const>{}), 16U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_i2c_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 0U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 15> small{};
    EXPECT_EQ(acf_i2c_build(std::span<uint8_t>(small), m, std::span<uint8_t const>{}), 0U);

    // This type carries no payload: any payload is refused, as is a longer length on the wire.
    std::array<uint8_t, 20> room{};
    EXPECT_EQ(acf_i2c_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(PAYLOAD).first(1)), 0U);
    auto longer = room;
    for (size_t i = 0; i < IMAGE.size(); ++i) {
        longer[i] = IMAGE[i];
    }
    longer[1] = 5;
    EXPECT_FALSE(acf_i2c_parse(std::span<uint8_t const>(longer)).has_value());
}

TEST(acf_i2c, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_i2c_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_i2c_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_i2c_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_i2c_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_i2c, code_tables_and_address_byte)
{
    EXPECT_EQ(acf_i2c_code_name(AcfI2cCode::cr1_start), "CR1");
    EXPECT_EQ(acf_i2c_code_name(AcfI2cCode::cr8_read_restart), "CR8");
    EXPECT_EQ(acf_i2c_code_name(AcfI2cCode::tr1_nack), "TR1");
    EXPECT_EQ(acf_i2c_code_name(AcfI2cCode::tr5_stop), "TR5");
    EXPECT_EQ(acf_i2c_code_name(0xDU), "RESERVED");
    EXPECT_TRUE(acf_i2c_code_is_request(AcfI2cCode::cr8_read_restart));
    EXPECT_FALSE(acf_i2c_code_is_request(AcfI2cCode::tr1_nack));
    EXPECT_EQ(acf_i2c_exception_name(AcfI2cException::none), "NONE");
    EXPECT_EQ(acf_i2c_exception_name(0x5U), "USER");
    EXPECT_EQ(acf_i2c_exception_name(AcfI2cException::sequence_error), "SEQUENCE_ERROR");
    EXPECT_EQ(acf_i2c_exception_name(0xEU), "RESERVED");
    // <addr,rw>: 0xA3 = address 0x51, read
    EXPECT_EQ(acf_i2c_address(0xA3U), 0x51U);
    EXPECT_TRUE(acf_i2c_is_read(0xA3U));
    EXPECT_FALSE(acf_i2c_is_read(0xA2U));
}

TEST(acf_i2c, format_to_output)
{
    auto const view = acf_i2c_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_I2C"));
    EXPECT_TRUE(result.contains("payload=0 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_i2c_test)
