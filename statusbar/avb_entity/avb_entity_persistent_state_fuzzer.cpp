// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for parse_persistent_state: the entity's saved-state TOML, whatever is in the
/// file. A state that parses must serialize to text that parses back to the same state.

#include "statusbar/avb_entity/avb_entity_persistent_state.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - the fuzzer's bytes as text
    std::string_view const text{reinterpret_cast<char const*>(data), size};
    auto const state = statusbar::avb_entity::parse_persistent_state(text);
    if (state) {
        auto const again = statusbar::avb_entity::parse_persistent_state(statusbar::avb_entity::serialize_persistent_state(*state));
        if (!again || !(*again == *state)) {
            std::abort();
        }
    }
    return 0;
}
