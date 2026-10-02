// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_acf.hpp"

#include "statusbar/avtp/avtp_acf_checksum.hpp"
#include "statusbar/avtp/avtp_acf_crc.hpp"
#include "statusbar/avtp/avtp_acf_format.hpp"
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

namespace {

/// A payload of three messages: GPC (3 quadlets), USER 0x78 (1 quadlet),
/// SERIAL (2 quadlets) = 24 octets.
auto three_messages() -> std::array<uint8_t, 24>
{
    std::array<uint8_t, 24> buf{};
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf).subspan(0), AcfMsgType::gpc, 3));
    buf[2] = 0xAA;
    buf[11] = 0xBB;
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf).subspan(12), AcfMsgType::user_first, 1));
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf).subspan(16), AcfMsgType::serial, 2));
    buf[23] = 0xCC;
    return buf;
}

}  // namespace

TEST(acf, header_bit_packing)
{
    AcfMessageHeader header{};
    header.init(AcfMsgType::gpc, 3);
    EXPECT_EQ(header.type_length.get(), 0x0A03U);  // 0x05 << 9 | 3
    EXPECT_EQ(header.msg_type(), AcfMsgType::gpc);
    EXPECT_EQ(header.msg_length(), 3U);
    EXPECT_EQ(header.msg_length_octets(), 12U);
    EXPECT_TRUE(header.is_valid());

    header.init(AcfMsgType::user_last, ACF_MSG_LENGTH_MAX_QUADLETS);
    EXPECT_EQ(header.type_length.get(), 0xFFFFU);
    EXPECT_EQ(header.msg_type(), 0x7FU);
    EXPECT_EQ(header.msg_length(), 511U);

    header.set_msg_length(0);
    EXPECT_FALSE(header.is_valid());
    EXPECT_EQ(header.msg_type(), 0x7FU);  // length edit leaves the type alone
}

TEST(acf, quadlet_helpers_table_24)
{
    EXPECT_EQ(acf_quadlets_for_octets(0), 0U);
    EXPECT_EQ(acf_quadlets_for_octets(1), 1U);
    EXPECT_EQ(acf_quadlets_for_octets(2), 1U);
    EXPECT_EQ(acf_quadlets_for_octets(7), 2U);
    EXPECT_EQ(acf_quadlets_for_octets(32), 8U);
    EXPECT_EQ(acf_quadlets_for_octets(64), 16U);
    EXPECT_EQ(acf_pad_for_octets(0), 0U);
    EXPECT_EQ(acf_pad_for_octets(1), 3U);
    EXPECT_EQ(acf_pad_for_octets(2), 2U);
    EXPECT_EQ(acf_pad_for_octets(7), 1U);
    EXPECT_EQ(acf_pad_for_octets(32), 0U);
}

TEST(acf, msg_type_names)
{
    EXPECT_EQ(acf_msg_type_name(AcfMsgType::can), "ACF_CAN");
    EXPECT_EQ(acf_msg_type_name(AcfMsgType::lin_v2), "ACF_LIN_V2");
    EXPECT_EQ(acf_msg_type_name(AcfMsgType::crc), "ACF_CRC");
    EXPECT_EQ(acf_msg_type_name(0x7AU), "ACF_USER");
    EXPECT_EQ(acf_msg_type_name(0x13U), "ACF_RESERVED");
    EXPECT_EQ(acf_msg_type_name(0x75U), "ACF_RESERVED");
}

TEST(acf, parse_message_bounds)
{
    auto const buf = three_messages();
    auto const all = std::span<uint8_t const>(buf);

    auto const first = acf_parse_message(all);
    EXPECT_TRUE(first.has_value());
    EXPECT_EQ(first->msg_type(), AcfMsgType::gpc);
    EXPECT_EQ(first->message.size(), 12U);
    EXPECT_EQ(first->after_header().size(), 10U);
    EXPECT_EQ(first->after_header()[0], 0xAAU);

    // Declared length past the end of the buffer is rejected.
    EXPECT_FALSE(acf_parse_message(all.first(8)).has_value());
    // A length of zero is rejected (9.4.1.3: minimum one quadlet).
    std::array<uint8_t, 4> zero_length{0x0A, 0x00, 0x00, 0x00};
    EXPECT_FALSE(acf_parse_message(std::span<uint8_t const>(zero_length)).has_value());
    // Shorter than the common header.
    EXPECT_FALSE(acf_parse_message(all.first(1)).has_value());
    EXPECT_FALSE(acf_parse_message({}).has_value());
}

TEST(acf, store_header_limits)
{
    std::array<uint8_t, 2> two{};
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(two), AcfMsgType::serial, 2));
    EXPECT_EQ(two[0], 0x0CU);  // 0x06 << 1
    EXPECT_EQ(two[1], 0x02U);
    std::array<uint8_t, 1> one{};
    EXPECT_FALSE(acf_store_header(std::span<uint8_t>(one), AcfMsgType::serial, 2));
    EXPECT_FALSE(acf_store_header(std::span<uint8_t>(two), AcfMsgType::serial, 0));
    EXPECT_FALSE(acf_store_header(std::span<uint8_t>(two), AcfMsgType::serial, 0x200));
    EXPECT_FALSE(acf_store_header(std::span<uint8_t>(two), 0x80U, 1));
}

TEST(acf, walker_visits_every_message)
{
    auto const buf = three_messages();
    AcfMessageWalker walker{std::span<uint8_t const>(buf)};
    EXPECT_FALSE(walker.done());

    auto const a = walker.next();
    EXPECT_TRUE(a.has_value() && a->msg_type() == AcfMsgType::gpc && a->message.size() == 12U);
    auto const b = walker.next();
    EXPECT_TRUE(b.has_value() && b->msg_type() == AcfMsgType::user_first && b->message.size() == 4U);
    auto const c = walker.next();
    EXPECT_TRUE(c.has_value() && c->msg_type() == AcfMsgType::serial && c->message.size() == 8U);
    EXPECT_EQ(c->after_header()[5], 0xCCU);

    EXPECT_FALSE(walker.next().has_value());
    EXPECT_TRUE(walker.done());
    EXPECT_FALSE(walker.malformed());
    EXPECT_TRUE(walker.remaining().empty());
}

TEST(acf, walker_stops_on_malformed_length)
{
    // Trailing bytes that cannot hold a message.
    auto const buf = three_messages();
    std::vector<uint8_t> tail(buf.begin(), buf.end());
    tail.push_back(0x0A);
    tail.push_back(0x01);
    AcfMessageWalker walker{std::span<uint8_t const>(tail)};
    EXPECT_TRUE(walker.next().has_value());
    EXPECT_TRUE(walker.next().has_value());
    EXPECT_TRUE(walker.next().has_value());
    EXPECT_FALSE(walker.next().has_value());
    EXPECT_TRUE(walker.malformed());
    EXPECT_TRUE(walker.done());
    EXPECT_EQ(walker.remaining().size(), 2U);

    // A zero length mid-stream.
    auto zero = three_messages();
    zero[13] = 0x00;  // USER message length -> 0
    AcfMessageWalker w2{std::span<uint8_t const>(zero)};
    EXPECT_TRUE(w2.next().has_value());
    EXPECT_FALSE(w2.next().has_value());
    EXPECT_TRUE(w2.malformed());
    EXPECT_FALSE(w2.next().has_value());  // stays stopped
}

TEST(acf, next_plain_yields_trailers_as_messages)
{
    std::array<uint8_t, 16> buf{};
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf), AcfMsgType::gpc, 3));
    EXPECT_TRUE(acf_checksum_build(std::span<uint8_t>(buf).subspan(12), std::span<uint8_t const>(buf).first(12)));
    AcfMessageWalker walker{std::span<uint8_t const>(buf)};
    EXPECT_TRUE(walker.next().has_value());
    auto const trailer = walker.next();
    EXPECT_TRUE(trailer.has_value() && trailer->msg_type() == AcfMsgType::checksum);
    EXPECT_FALSE(walker.next().has_value());
}

TEST(acf, next_verified_checksum)
{
    std::array<uint8_t, 20> buf{};
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf), AcfMsgType::gpc, 3));
    buf[5] = 0x5A;
    EXPECT_TRUE(acf_checksum_build(std::span<uint8_t>(buf).subspan(12), std::span<uint8_t const>(buf).first(12)));
    EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf).subspan(16), AcfMsgType::serial, 1));

    {
        AcfMessageWalker walker{std::span<uint8_t const>(buf)};
        auto const first = walker.next_verified();
        EXPECT_TRUE(first.has_value());
        EXPECT_EQ(first->message.msg_type(), AcfMsgType::gpc);
        EXPECT_TRUE(first->trailer == AcfTrailerStatus::checksum_ok);
        EXPECT_TRUE(first->ok());
        // The trailer was consumed; the SERIAL message follows with no trailer.
        auto const second = walker.next_verified();
        EXPECT_TRUE(second.has_value() && second->message.msg_type() == AcfMsgType::serial);
        EXPECT_TRUE(second->trailer == AcfTrailerStatus::none && second->ok());
        EXPECT_FALSE(walker.next_verified().has_value());
    }
    {
        auto corrupt = buf;
        corrupt[5] ^= 0x01U;
        AcfMessageWalker walker{std::span<uint8_t const>(corrupt)};
        auto const first = walker.next_verified();
        EXPECT_TRUE(first.has_value());
        EXPECT_TRUE(first->trailer == AcfTrailerStatus::checksum_bad);
        EXPECT_FALSE(first->ok());
        // Both the message and its trailer were skipped.
        auto const second = walker.next_verified();
        EXPECT_TRUE(second.has_value() && second->message.msg_type() == AcfMsgType::serial);
    }
    {
        // A Checksum message with the wrong length is a malformed trailer.
        auto bad = buf;
        bad[13] = 0x02;  // msg_length 2 (but the message is still inside the buffer: SERIAL quadlet absorbed)
        AcfMessageWalker walker{std::span<uint8_t const>(bad)};
        auto const first = walker.next_verified();
        EXPECT_TRUE(first.has_value());
        EXPECT_TRUE(first->trailer == AcfTrailerStatus::trailer_malformed);
        EXPECT_FALSE(first->ok());
        EXPECT_FALSE(walker.next_verified().has_value());
    }
}

TEST(acf, next_verified_crc)
{
    for (auto const type : {AcfCrcType::crc_eth, AcfCrcType::crc_32p4}) {
        std::array<uint8_t, 20> buf{};
        EXPECT_TRUE(acf_store_header(std::span<uint8_t>(buf), AcfMsgType::gpc, 3));
        buf[7] = 0x33;
        EXPECT_TRUE(acf_crc_build(std::span<uint8_t>(buf).subspan(12), type, std::span<uint8_t const>(buf).first(12)));

        AcfMessageWalker ok_walker{std::span<uint8_t const>(buf)};
        auto const verified = ok_walker.next_verified();
        EXPECT_TRUE(verified.has_value());
        EXPECT_TRUE(verified->trailer == AcfTrailerStatus::crc_ok);
        EXPECT_TRUE(verified->ok());
        EXPECT_FALSE(ok_walker.next_verified().has_value());

        auto corrupt = buf;
        corrupt[7] ^= 0x80U;
        AcfMessageWalker bad_walker{std::span<uint8_t const>(corrupt)};
        auto const bad = bad_walker.next_verified();
        EXPECT_TRUE(bad.has_value());
        EXPECT_TRUE(bad->trailer == AcfTrailerStatus::crc_bad);
        EXPECT_FALSE(bad->ok());

        auto user = buf;
        user[15] = 0x0F;  // crc_type -> CRC_USER
        AcfMessageWalker user_walker{std::span<uint8_t const>(user)};
        auto const unsupported = user_walker.next_verified();
        EXPECT_TRUE(unsupported.has_value());
        EXPECT_TRUE(unsupported->trailer == AcfTrailerStatus::crc_unsupported);
        EXPECT_FALSE(unsupported->ok());
    }
}

TEST(acf, next_verified_leading_trailer_is_a_message)
{
    // A trailer with nothing before it is just a message of its type.
    std::array<uint8_t, 4> buf{};
    AcfChecksumMessage message{};
    message.init(0x1234);
    span_store(buf, message);
    AcfMessageWalker walker{std::span<uint8_t const>(buf)};
    auto const only = walker.next_verified();
    EXPECT_TRUE(only.has_value());
    EXPECT_EQ(only->message.msg_type(), AcfMsgType::checksum);
    EXPECT_TRUE(only->trailer == AcfTrailerStatus::none);
}

TEST(acf, format_to_output)
{
    auto const buf = three_messages();
    AcfMessageWalker walker{std::span<uint8_t const>(buf)};
    auto const first = walker.next_verified();
    EXPECT_TRUE(first.has_value());
    std::string result;
    format_to(std::back_inserter(result), *first);
    EXPECT_TRUE(result.contains("ACF_GPC"));
    EXPECT_TRUE(result.contains("msg_length=3q"));
    EXPECT_TRUE(result.contains("octets=12"));
    EXPECT_TRUE(result.contains("trailer=none"));
}

TEST_MAIN(statusbar_avtp, avtp_acf_test)
