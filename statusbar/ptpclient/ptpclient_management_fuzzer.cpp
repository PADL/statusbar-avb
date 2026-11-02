// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for ptpclient::parse_management_message: the bytes a ptp4l management socket
/// answers with (IEEE 1588-2019 Clause 15), whole, and as the data of a well-formed RESPONSE for
/// each data set the client decodes (so every decoder sees arbitrary data past a valid header).

#include "statusbar/ptpclient/ptpclient_management.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using namespace statusbar::ptpclient;

constexpr std::array<uint16_t, 9> DATA_SETS{
    management_id::parent_data_set,
    management_id::current_data_set,
    management_id::time_properties_data_set,
    management_id::port_data_set,
    management_id::port_properties_np,
    management_id::time_status_np,
    management_id::grandmaster_settings_np,
    management_id::subscribe_events_np,
    management_id::default_data_set,
};

/// A RESPONSE for @p id carrying @p data (at most a SET's largest), parsed.
void parse_as_response(uint16_t const id, std::span<uint8_t const> const data)
{
    std::array<uint8_t, MANAGEMENT_GET_SIZE + 256> message{};
    size_t const length =
        build_management_message(message, ManagementAction::response, id, data.first(std::min<size_t>(data.size(), 256)), {});
    (void)parse_management_message(std::span<uint8_t const>{message}.first(length));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const bytes{data, size};
    (void)parse_management_message(bytes);
    for (uint16_t const id : DATA_SETS) {
        parse_as_response(id, bytes);
    }
    return 0;
}
