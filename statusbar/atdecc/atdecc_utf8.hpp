#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Well-formed UTF-8 (RFC 3629 Section 4), which the text of an ATDECC string is (IEEE
/// 1722.1-2021's 64-octet string fields): what a string a controller sends is checked by.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace statusbar::atdecc {

/// The length of the well-formed UTF-8 sequence at the head of @p text (RFC 3629 Section 4: no
/// overlong form, no surrogate, nothing past U+10FFFF); 0 for a malformed or cut one, or no text.
[[nodiscard]] constexpr auto utf8_sequence_length(std::string_view const text) noexcept -> size_t
{  // NOLINT(statusbar-block-statements) a codec: one case per lead-octet range of RFC 3629 Section 4
    if (text.empty()) {
        return 0;
    }
    auto const at = [&text](size_t const k) { return static_cast<uint8_t>(text[k]); };
    auto const continuation = [&](size_t const k) { return k < text.size() && (at(k) & 0xC0U) == 0x80U; };
    uint8_t const lead = at(0);
    if (lead < 0x80U) {
        return 1;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return continuation(1) ? 2 : 0;
    }
    if (lead >= 0xE0U && lead <= 0xEFU) {
        bool const overlong = lead == 0xE0U && continuation(1) && at(1) < 0xA0U;
        bool const surrogate = lead == 0xEDU && continuation(1) && at(1) >= 0xA0U;
        return continuation(1) && continuation(2) && !overlong && !surrogate ? 3 : 0;
    }
    if (lead >= 0xF0U && lead <= 0xF4U) {
        bool const overlong = lead == 0xF0U && continuation(1) && at(1) < 0x90U;
        bool const beyond = lead == 0xF4U && continuation(1) && at(1) >= 0x90U;
        return continuation(1) && continuation(2) && continuation(3) && !overlong && !beyond ? 4 : 0;
    }
    return 0;
}

/// Whether all of @p text is well-formed UTF-8; an empty text is.
[[nodiscard]] constexpr auto utf8_well_formed(std::string_view text) noexcept -> bool
{
    while (!text.empty()) {
        size_t const n = utf8_sequence_length(text);
        if (n == 0) {
            return false;
        }
        text.remove_prefix(n);
    }
    return true;
}

}  // namespace statusbar::atdecc
