#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace statusbar::owlm {

/// Build an EUI-64 from a MAC (EUI-48) by inserting two bytes between
/// the OUI (bytes 0–2) and the NIC ID (bytes 3–5).
///
/// MAC = AA:BB:CC:DD:EE:FF, mid = 0xFFFE
///   → EUI-64 = AA:BB:CC:FF:FE:DD:EE:FF
///
/// Delegates to ieee::Eui48::to_eui64_with_index, which performs the
/// same insert pattern. The U/L bit is intentionally NOT flipped — we
/// want a stable identifier directly tied to the MAC, not an IPv6
/// modified-EUI-64 interface ID per RFC 4291.
[[nodiscard]] constexpr auto make_owlm_eui64(ieee::Eui48 mac, uint16_t mid_bytes) noexcept -> ieee::Eui64
{
    return mac.to_eui64_with_index(mid_bytes);
}

/// EUI-64 mid bytes assigned by the redundancy scheme: primary stream uses
/// 0x0000, redundant stream uses 0x0001. Anything else (e.g. legacy 0xFFFE
/// senders) is treated as a non-redundant single stream.
inline constexpr uint16_t PRIMARY_MID_BYTES = 0x0000;
inline constexpr uint16_t REDUNDANT_MID_BYTES = 0x0001;

/// Extract the two middle bytes from an EUI-64 (positions 3 and 4).
[[nodiscard]] constexpr auto eui64_mid_bytes(ieee::Eui64 const& e) noexcept -> uint16_t
{
    auto const s = e.span();
    std::array<uint8_t, 2> const be{s[3], s[4]};
    return std::bit_cast<ieee::doublet_t>(be).get();
}

/// Pair identifier — EUI-64 with the middle two bytes zeroed. Primary
/// (mid=0x0000) and redundant (mid=0x0001) copies of the same logical
/// sender share the same pair id, so the receiver can match them.
[[nodiscard]] constexpr auto eui64_pair_id(ieee::Eui64 const& e) noexcept -> ieee::Eui64
{
    ieee::Eui64 out = e;
    auto sp = out.span();
    sp[3] = 0;
    sp[4] = 0;
    return out;
}

/// True when `mid` is one of the two redundancy-stream identifiers
/// (PRIMARY_MID_BYTES / REDUNDANT_MID_BYTES). False for legacy single
/// streams (e.g. mid = 0xFFFE).
[[nodiscard]] constexpr auto is_redundancy_pair_mid(uint16_t mid) noexcept -> bool
{
    return mid == PRIMARY_MID_BYTES || mid == REDUNDANT_MID_BYTES;
}

}  // namespace statusbar::owlm
