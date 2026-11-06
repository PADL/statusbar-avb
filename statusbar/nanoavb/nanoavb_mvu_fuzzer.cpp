// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for the MVU command handler (Milan v1.3 and v1.2 Section 5.4.4): the input as a
/// whole AECPDU, and as command_type and data behind a well-formed MVU header to the entity, so every
/// command (BIND_STREAM against two Milan sinks included) sees arbitrary data.

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using namespace statusbar;

ieee::Eui64 const US{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x03};
ieee::Eui48 const SRC{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

/// An MVU command to US: command_type from the first two octets of @p input, the rest its data.
void addressed(nanoavb::MvuCommandHandler& handler, std::span<uint8_t const> const input)
{
    constexpr size_t MAX_DATA = 506;  // the largest AECPDU less the MVU header
    std::array<uint8_t, atdecc::mvu::MvuDu::LENGTH + MAX_DATA> packet{};
    size_t const skip = std::min<size_t>(input.size(), 2);
    auto const data = input.subspan(skip).first(std::min<size_t>(input.size() - skip, MAX_DATA));
    uint16_t const type = input.size() >= 2 ? static_cast<uint16_t>((input[0] << 8U) | input[1]) : 0;
    atdecc::mvu::MvuDu du{};
    du.init_command(type, static_cast<uint16_t>(data.size()));
    du.common.target_entity_id = US;
    span_store(std::span<uint8_t>{packet}.first(atdecc::mvu::MvuDu::LENGTH), du);
    std::ranges::copy(data, packet.begin() + atdecc::mvu::MvuDu::LENGTH);
    (void)handler.process_packet(SRC, std::span<uint8_t const>{packet}.first(atdecc::mvu::MvuDu::LENGTH + data.size()), US, {});
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    atdecc::MilanListener listener{US, 2};
    listener.set_send([](atdecc::AcmpCommandResponse const&) { return true; });
    for (auto const version : {nanoavb::MilanVersion::v1_3, nanoavb::MilanVersion::v1_2}) {
        nanoavb::MvuCommandHandler handler{};
        handler.set_profile(nanoavb::EntityProfile{.milan = true, .milan_version = version});
        handler.set_milan_listener(&listener);
        handler.set_send_response([](ieee::Eui48 const&, std::span<uint8_t const>) { return true; });
        (void)handler.process_packet(SRC, input, US, {});
        addressed(handler, input);
    }
    return 0;
}
