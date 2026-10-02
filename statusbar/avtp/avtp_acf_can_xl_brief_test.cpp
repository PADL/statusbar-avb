// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf_can_xl_brief.hpp"

#include "statusbar/avtp/avtp_acf_can_xl_brief_format.hpp"
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

static_assert(sizeof(AcfCanXlBriefMessage) == AcfCanXlBriefMessage::LENGTH);

namespace {

/// The fixed part with every field at its test value and the fixed-only length,
/// encoded independently of the accessors (the layout oracle); the length is the
/// smallest valid message (fixed part plus the minimum payload).
constexpr std::array<uint8_t, 16> LAYOUT{
    0x24, 0x05, 0x42, 0xAB, 0x12, 0x34, 0x15, 0xA5, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x77, 0x11, 0x23};

/// A complete message on the wire: the fixed part (pad = 3, length = 6 quadlets)
/// followed by a 5-octet payload and its three pad octets.
constexpr std::array<uint8_t, 24> IMAGE{0x24, 0x06, 0xC2, 0xAB, 0x12, 0x34, 0x15, 0xA5, 0xDE, 0xAD, 0xBE, 0xEF,
                                        0x00, 0x77, 0x11, 0x23, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0x00, 0x00, 0x00};

constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Set every field to its test value
void set_test_values(AcfCanXlBriefMessage& m)
{
    m.set_pad(0x1U);
    m.set_mtv(false);
    m.set_can_bus_id(0x2ABU);
    m.set_vcid(0x12U);
    m.set_sdt(0x34U);
    m.set_rrs(true);
    m.set_sec(false);
    m.set_priority_id(0x5A5U);
    m.set_acceptance_field(0xDEADBEEFU);
    m.set_transaction_num(0x77U);
    m.set_ms(true);
    m.set_segment_num(0x123U);
}

/// Check every field against its test value
void expect_test_values(AcfCanXlBriefMessage const& m)
{
    EXPECT_EQ(m.pad(), 0x1U);
    EXPECT_FALSE(m.mtv());
    EXPECT_EQ(m.can_bus_id(), 0x2ABU);
    EXPECT_EQ(m.get_vcid(), 0x12U);
    EXPECT_EQ(m.get_sdt(), 0x34U);
    EXPECT_TRUE(m.rrs());
    EXPECT_FALSE(m.sec());
    EXPECT_EQ(m.priority_id(), 0x5A5U);
    EXPECT_EQ(m.get_acceptance_field(), 0xDEADBEEFU);
    EXPECT_EQ(m.get_transaction_num(), 0x77U);
    EXPECT_TRUE(m.ms());
    EXPECT_EQ(m.segment_num(), 0x123U);
}

}  // namespace

TEST(acf_can_xl_brief, init_and_layout)
{
    AcfCanXlBriefMessage m{};
    m.init();
    EXPECT_EQ(m.header.msg_type(), AcfMsgType::can_xl_brief);
    EXPECT_EQ(m.header.msg_length(), 5U);
    EXPECT_TRUE(m.is_valid());

    set_test_values(m);
    expect_test_values(m);
    std::array<uint8_t, 16> bytes{};
    span_store(bytes, m);
    for (size_t i = 0; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], LAYOUT[i]);
    }
}

TEST(acf_can_xl_brief, parse_wire_image)
{
    auto const view = acf_can_xl_brief_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    EXPECT_EQ(view->fixed.pad(), 3U);
    EXPECT_FALSE(view->fixed.mtv());
    EXPECT_EQ(view->fixed.can_bus_id(), 0x2ABU);
    EXPECT_EQ(view->fixed.get_vcid(), 0x12U);
    EXPECT_EQ(view->fixed.get_sdt(), 0x34U);
    EXPECT_TRUE(view->fixed.rrs());
    EXPECT_FALSE(view->fixed.sec());
    EXPECT_EQ(view->fixed.priority_id(), 0x5A5U);
    EXPECT_EQ(view->fixed.get_acceptance_field(), 0xDEADBEEFU);
    EXPECT_EQ(view->fixed.get_transaction_num(), 0x77U);
    EXPECT_TRUE(view->fixed.ms());
    EXPECT_EQ(view->fixed.segment_num(), 0x123U);
    EXPECT_EQ(view->padded_payload.size(), 8U);
    EXPECT_EQ(view->payload.size(), 5U);
    for (size_t i = 0; i < PAYLOAD.size(); ++i) {
        EXPECT_EQ(view->payload[i], PAYLOAD[i]);
    }
}

TEST(acf_can_xl_brief, build_round_trip)
{
    AcfCanXlBriefMessage m{};
    m.init();
    set_test_values(m);
    std::array<uint8_t, 24> out{};
    EXPECT_EQ(acf_can_xl_brief_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 24U);
    // The pad was computed, the payload copied, the tail zeroed: byte-identical to the image.
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i], IMAGE[i]);
    }
    auto const view = acf_can_xl_brief_parse(std::span<uint8_t const>(out));
    EXPECT_TRUE(view.has_value());
    if (view.has_value()) {
        EXPECT_EQ(view->payload.size(), 5U);
    }

    // Too small an output buffer builds nothing.
    std::array<uint8_t, 23> small{};
    EXPECT_EQ(acf_can_xl_brief_build(std::span<uint8_t>(small), m, std::span<uint8_t const>(PAYLOAD)), 0U);

    // The payload is at least one quadlet: an empty one is rejected.
    std::array<uint8_t, 16> bare{};
    EXPECT_EQ(acf_can_xl_brief_build(std::span<uint8_t>(bare), m, std::span<uint8_t const>{}), 0U);

    // The 9-bit acf_msg_length caps the message at 511 quadlets.
    std::vector<uint8_t> const big(2044 - 16 + 1, 0x55);
    std::vector<uint8_t> room(2048);
    EXPECT_EQ(acf_can_xl_brief_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(big)), 0U);
    std::vector<uint8_t> const max_payload(2044 - 16, 0x55);
    EXPECT_EQ(acf_can_xl_brief_build(std::span<uint8_t>(room), m, std::span<uint8_t const>(max_payload)), 2044U);
}

TEST(acf_can_xl_brief, parse_rejects)
{
    // Shorter than the fixed part.
    EXPECT_FALSE(acf_can_xl_brief_parse(std::span<uint8_t const>(IMAGE).first(15)).has_value());
    // Declared length past the buffer.
    EXPECT_FALSE(acf_can_xl_brief_parse(std::span<uint8_t const>(IMAGE).first(23)).has_value());
    // Wrong acf_msg_type.
    auto wrong_type = IMAGE;
    wrong_type[0] = 0xEC;  // ACF_CHECKSUM
    EXPECT_FALSE(acf_can_xl_brief_parse(std::span<uint8_t const>(wrong_type)).has_value());
    // Length below the fixed part.
    auto too_short = IMAGE;
    too_short[1] = 0x01;
    EXPECT_FALSE(acf_can_xl_brief_parse(std::span<uint8_t const>(too_short)).has_value());
}

TEST(acf_can_xl_brief, mtv_is_ignored_and_never_sent)
{
    // Received with mtv set: ignored, still parses.
    auto with_mtv = IMAGE;
    with_mtv[2] |= 0x20U;
    auto const view = acf_can_xl_brief_parse(std::span<uint8_t const>(with_mtv));
    EXPECT_TRUE(view.has_value() && view->fixed.mtv());

    // Built with mtv set: cleared on the wire (9.4.4.1 / 9.4.19.2).
    AcfCanXlBriefMessage m{};
    m.init();
    set_test_values(m);
    m.set_mtv(true);
    std::array<uint8_t, 24> out{};
    EXPECT_EQ(acf_can_xl_brief_build(std::span<uint8_t>(out), m, std::span<uint8_t const>(PAYLOAD)), 24U);
    EXPECT_EQ(out[2] & 0x20U, 0U);
}

TEST(acf_can_xl_brief, format_to_output)
{
    auto const view = acf_can_xl_brief_parse(std::span<uint8_t const>(IMAGE));
    EXPECT_TRUE(view.has_value());
    if (!view.has_value()) {
        return;
    }
    std::string result;
    format_to(std::back_inserter(result), *view);
    EXPECT_TRUE(result.contains("ACF_CAN_XL_BRIEF"));
    EXPECT_TRUE(result.contains("payload=5 octets"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_can_xl_brief_test)
