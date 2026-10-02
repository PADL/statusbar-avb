#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF messages for the Wireshark golden capture (see wireshark_golden_acf.cpp).

#include "statusbar/avtp/avtp_acf_crc.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace statusbar::avtp::golden {

/// One built ACF message and the schema name of its type
struct AcfGoldenMessage
{
    std::string name;
    std::vector<uint8_t> octets;
};

/// One message of every clause 9.4 type, fields at their table test values
[[nodiscard]] auto acf_golden_messages() -> std::vector<AcfGoldenMessage>;

/// @p message followed by its ACF_CHECKSUM trailer
[[nodiscard]] auto acf_with_checksum(std::vector<uint8_t> message) -> std::vector<uint8_t>;

/// @p message followed by an ACF_CRC trailer of @p type
[[nodiscard]] auto acf_with_crc(std::vector<uint8_t> message, AcfCrcType type) -> std::vector<uint8_t>;

/// A two-quadlet ACF_USER message
[[nodiscard]] auto acf_user_message() -> std::vector<uint8_t>;

}  // namespace statusbar::avtp::golden
