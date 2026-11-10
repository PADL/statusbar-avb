// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for AemCommandHandler::process_packet (IEEE 1722.1-2021 Clause 7.4) over the
/// entity_audio.bin model: the input as one AECPDU, then as a run of commands to the entity, each a
/// length, a command type and its data, the hooks accepting or refusing as the first octet says.

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/sm/sm.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

ieee::Eui64 const US{0xAA, 0, 0, 0, 0, 0, 0, 0x71};
std::array<ieee::Eui64, 2> const CONTROLLERS{{{0xC0, 0, 0, 0, 0, 0, 0, 0x0A}, {0xC0, 0, 0, 0, 0, 0, 0, 0x0B}}};
ieee::Eui48 const MAC{0x02, 0, 0, 0, 0, 0x0A};

/// The commands the entity's hooks and state serve, picked by a type octet's low bits when its top
/// bit is set; otherwise the octet is the command type itself.
constexpr std::array<uint16_t, 24> COMMANDS{
    AEM_COMMAND_SET_STREAM_FORMAT,
    AEM_COMMAND_GET_STREAM_FORMAT,
    AEM_COMMAND_SET_SAMPLING_RATE,
    AEM_COMMAND_GET_SAMPLING_RATE,
    AEM_COMMAND_SET_NAME,
    AEM_COMMAND_GET_NAME,
    AEM_COMMAND_GET_COUNTERS,
    AEM_COMMAND_GET_STREAM_INFO,
    AEM_COMMAND_SET_STREAM_INFO,
    AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION,
    AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION,
    AEM_COMMAND_LOCK_ENTITY,
    AEM_COMMAND_ACQUIRE_ENTITY,
    AEM_COMMAND_READ_DESCRIPTOR,
    AEM_COMMAND_START_STREAMING,
    AEM_COMMAND_STOP_STREAMING,
    AEM_COMMAND_ADD_AUDIO_MAPPINGS,
    AEM_COMMAND_REMOVE_AUDIO_MAPPINGS,
    AEM_COMMAND_GET_AUDIO_MAP,
    AEM_COMMAND_SET_CLOCK_SOURCE,
    AEM_COMMAND_GET_AVB_INFO,
    AEM_COMMAND_SET_MAX_TRANSIT_TIME,
    AEM_COMMAND_GET_MAX_TRANSIT_TIME,
    AEM_COMMAND_SET_CONTROL,
};

/// avb_entity's committed test model, compiled in; a tree without it (the block gate's copy) parses
/// a one-octet model, which model_storage() refuses.
// NOLINTNEXTLINE(*-avoid-c-arrays): #embed fills a braced array
constexpr uint8_t MODEL[] = {
#if __has_embed("../avb_entity/testdata/entity_audio.bin")
#    embed "../avb_entity/testdata/entity_audio.bin"
#else
    0
#endif
};

/// The test model, parsed once; a malformed one aborts the run.
auto model_storage() -> DescriptorStorage const&
{
    static auto const storage = DescriptorStorage::create(std::span<uint8_t const>{MODEL});
    if (!storage) {
        std::fputs("nanoavb_aem_fuzzer: entity_audio.bin missing or malformed\n", stderr);
        std::abort();
    }
    return *storage;
}

/// One AEM command of @p type from @p controller carrying @p body (at most 512 octets of it).
void command(AemCommandHandler& aem, ieee::Eui64 const& controller, uint16_t const type, std::span<uint8_t const> const body)
{
    std::array<uint8_t, AemDu::LENGTH + 512> packet{};
    auto const data = body.first(std::min<size_t>(body.size(), 512));
    AemDu du{};
    du.init_command(type, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + data.size()));
    du.target_entity_id = US;
    du.controller_entity_id = controller;
    span_store(std::span<uint8_t>{packet}.first(AemDu::LENGTH), du);
    std::ranges::copy(data, packet.begin() + AemDu::LENGTH);
    (void)aem.process_packet(MAC, std::span<uint8_t const>{packet}.first(AemDu::LENGTH + data.size()), US);
}

/// The hooks a kit installs, each answering @p verdict.
void install_hooks(DescriptorStorageHandler& model, AemCommandHandler& aem, uint8_t const verdict)
{
    model.manage_object_names();
    model.set_on_stream_format_changed([verdict](uint16_t, uint16_t, uint64_t) { return verdict; });
    model.set_on_sampling_rate_changed([verdict](uint16_t, uint32_t) { return verdict; });
    model.set_on_entity_name_changed([verdict](AtdeccString const&) { return verdict; });
    model.set_on_streaming_changed([verdict](uint16_t, uint16_t, bool) { return verdict; });
    aem.set_send_response([](ieee::Eui48 const&, std::span<uint8_t const>) { return true; });
    aem.set_get_counters([](uint16_t const type, uint16_t const index, uint32_t& valid, std::array<uint32_t, 32>& out) {
        valid = 0x9BU;
        out[0] = index;
        return type == DESCRIPTOR_STREAM_INPUT || type == DESCRIPTOR_STREAM_OUTPUT;
    });
    aem.set_get_stream_info([](uint16_t const type, uint16_t, AemStreamInfoPayload& out, AemStreamInfoMilanExtension&) {
        out.msrp_accumulated_latency = uint32_t{2'000'000};
        return type == DESCRIPTOR_STREAM_OUTPUT || type == DESCRIPTOR_STREAM_INPUT;
    });
    aem.set_presentation_offset_hooks(
        [](uint16_t const index) { return index < 2 ? std::optional<uint32_t>{2'000'000} : std::nullopt; },
        [verdict](uint16_t, uint32_t) { return verdict; });
}

/// The commands in @p input, in turn: a length octet, a type octet (COMMANDS' with the top bit), a
/// controller octet, then that much data; the handler's clock moves 100 ms each.
void run_commands(AemCommandHandler& aem, std::span<uint8_t const> input)
{
    sm::TimePoint now{std::chrono::seconds{1}};
    while (input.size() >= 3) {
        size_t const length = std::min<size_t>(input[0], input.size() - 3);
        uint16_t const type = (input[1] & 0x80U) != 0 ? COMMANDS[input[1] % COMMANDS.size()] : input[1];
        auto const& controller = CONTROLLERS[input[2] & 1U];
        command(aem, controller, type, input.subspan(3, length));
        input = input.subspan(3 + length);
        now += std::chrono::milliseconds{100};
        aem.tick(now);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    auto const& storage = model_storage();
    if (input.empty()) {
        return 0;
    }
    uint8_t const verdict = (input[0] & 0x80U) == 0 ? AEM_STATUS_SUCCESS : input[0] & 0x7FU;
    DescriptorStorageHandler model{storage};
    AemCommandHandler aem{model};
    aem.set_entity_id(US);
    aem.set_entity_profile(EntityProfile{.milan = (input[0] & 0x40U) == 0});
    install_hooks(model, aem, verdict);
    auto const rest = input.subspan(1);
    (void)aem.process_packet(MAC, rest, US);
    run_commands(aem, rest);
    return 0;
}
