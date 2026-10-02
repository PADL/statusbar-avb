#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ATDECC AVTPDUs for the Wireshark golden capture (see wireshark_golden_atdecc.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace statusbar::atdecc::golden {

/// One built AVTPDU (ADP, AECP or ACMP) and a short name
struct AtdeccGoldenFrame
{
    std::string name;
    std::vector<uint8_t> octets;
};

/// ADP, ACMP and AECP (AEM commands/responses with payloads and descriptors,
/// Address Access, Vendor Unique) built by this repository's atdecc builders
[[nodiscard]] auto atdecc_golden_frames() -> std::vector<AtdeccGoldenFrame>;

}  // namespace statusbar::atdecc::golden
