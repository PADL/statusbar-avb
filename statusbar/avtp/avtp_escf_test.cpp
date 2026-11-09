// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/avtp/avtp_escf.hpp"

#include "statusbar/avtp/avtp_escf_format.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <array>

using namespace statusbar;
using namespace statusbar::avtp;
using namespace statusbar::ieee;

// Compile-time layout check (IEEE 1722-2025 Figure 109: 84 octets through signature_value)
static_assert(sizeof(EscfPdu) == EscfPdu::HEADER_LENGTH);

namespace {

auto test_signature() -> std::array<uint8_t, EscfPdu::SIGNATURE_LENGTH>
{
    std::array<uint8_t, EscfPdu::SIGNATURE_LENGTH> sig{};
    for (size_t i = 0; i < sig.size(); ++i) {
        sig[i] = static_cast<uint8_t>(0xA0 + i);
    }
    return sig;
}

}  // namespace

TEST(escf, init_and_accessors)
{
    EscfPdu pdu{};
    Eui64 kid(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    pdu.init(0, kid);

    EXPECT_EQ(pdu.subtype.get(), AvtpSubtype::escf);
    EXPECT_EQ(pdu.version(), 0);
    EXPECT_EQ(pdu.sig(), 0);
    EXPECT_EQ(pdu.encapsulated_avtpdu_length(), 0);
    EXPECT_EQ(pdu.key_id(), kid);
    EXPECT_EQ(pdu.signing_timestamp(), 0ULL);
    EXPECT_TRUE((pdu.signature_value() == std::array<uint8_t, 64>{}));
    EXPECT_TRUE(pdu.is_valid());

    pdu.init(static_cast<uint8_t>(EscfSigMode::ed25519), kid);
    EXPECT_EQ(pdu.sig(), 1);
}

TEST(escf, encapsulated_avtpdu_length)
{
    EscfPdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);

    pdu.set_encapsulated_avtpdu_length(1000);
    EXPECT_EQ(pdu.encapsulated_avtpdu_length(), 1000);

    // Figure 109: a full 16-bit field (16.2.6's 1416-octet limit is a frame rule, not a mask).
    pdu.set_encapsulated_avtpdu_length(0xFFFF);
    EXPECT_EQ(pdu.encapsulated_avtpdu_length(), 0xFFFF);
    std::array<uint8_t, EscfPdu::HEADER_LENGTH> buf{};
    span_store(buf, pdu);
    EXPECT_EQ(buf[2], 0xFF);
    EXPECT_EQ(buf[3], 0xFF);
}

TEST(escf, timestamp_and_signature_on_the_wire)
{
    EscfPdu pdu{};
    Eui64 kid(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pdu.init(0, kid);
    pdu.set_signing_timestamp(0x0123456789ABCDEFULL);
    pdu.set_signature_value(test_signature());

    std::array<uint8_t, EscfPdu::HEADER_LENGTH> buf{};
    span_store(buf, pdu);
    EXPECT_EQ(buf[12], 0x01);
    EXPECT_EQ(buf[19], 0xEF);
    EXPECT_EQ(buf[20], 0xA0);
    EXPECT_EQ(buf[83], 0xDF);
    EXPECT_EQ(pdu.signing_timestamp(), 0x0123456789ABCDEFULL);
    EXPECT_TRUE(pdu.signature_value() == test_signature());
}

TEST(escf, parse_valid)
{
    EscfPdu pdu{};
    Eui64 kid(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pdu.init(1, kid);
    pdu.set_encapsulated_avtpdu_length(300);
    pdu.set_signing_timestamp(42);
    pdu.set_signature_value(test_signature());

    std::array<uint8_t, 100> buf{};
    span_store(buf, pdu);

    auto parsed = escf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sig(), 1);
    EXPECT_EQ(parsed->key_id(), kid);
    EXPECT_EQ(parsed->encapsulated_avtpdu_length(), 300);
    EXPECT_EQ(parsed->signing_timestamp(), 42ULL);
    EXPECT_TRUE(parsed->signature_value() == test_signature());
}

TEST(escf, parse_truncated)
{
    std::array<uint8_t, EscfPdu::HEADER_LENGTH - 1> buf{};
    buf[0] = AvtpSubtype::escf;
    auto parsed = escf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(escf, parse_wrong_subtype)
{
    EscfPdu pdu{};
    Eui64 kid{};
    pdu.init(0, kid);
    pdu.subtype = AvtpSubtype::eecf;  // wrong

    std::array<uint8_t, EscfPdu::HEADER_LENGTH> buf{};
    span_store(buf, pdu);
    auto parsed = escf_parse_header(std::span<uint8_t const>(buf));
    EXPECT_FALSE(parsed.has_value());
}

TEST(escf, get_encapsulated_avtpdu)
{
    std::array<uint8_t, EscfPdu::HEADER_LENGTH + 12> buf{};
    buf[0] = AvtpSubtype::escf;

    auto payload = escf_get_encapsulated_avtpdu(std::span<uint8_t const>(buf));
    EXPECT_EQ(payload.size(), 12U);

    auto empty = escf_get_encapsulated_avtpdu(std::span<uint8_t const>(buf.data(), EscfPdu::HEADER_LENGTH));
    EXPECT_TRUE(empty.empty());
}

TEST(escf, sig_mode_names)
{
    EXPECT_EQ(std::string(escf_sig_mode_name(0)), "ECSSA_DSA_EMSA1_SHA256");
    EXPECT_EQ(std::string(escf_sig_mode_name(1)), "ED25519");
    EXPECT_EQ(std::string(escf_sig_mode_name(2)), "Reserved");
}

TEST(escf, format_to_output)
{
    EscfPdu pdu{};
    Eui64 kid(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.init(1, kid);
    pdu.set_encapsulated_avtpdu_length(64);
    pdu.set_signing_timestamp(1234);

    std::string result;
    format_to(std::back_inserter(result), pdu);
    EXPECT_TRUE(result.find("ESCF") != std::string::npos);
    EXPECT_TRUE(result.find("ED25519") != std::string::npos);
    EXPECT_TRUE(result.find("encapsulated_avtpdu_length=64") != std::string::npos);
    EXPECT_TRUE(result.find("signing_timestamp=1234") != std::string::npos);
}

TEST_MAIN(statusbar_avtp, avtp_escf_test)
