// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for ADD_AUDIO_MAPPINGS and REMOVE_AUDIO_MAPPINGS (IEEE 1722.1-2021 Clauses
/// 7.4.44-45, Milan v1.2 Section 5.4.2.27) to a STREAM_PORT_INPUT of four 1-channel clusters with
/// two 4-channel stream inputs: the input's first octet is the data plane's answer, the rest split
/// in two command bodies, an ADD then a REMOVE.

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

ieee::Eui64 const US{0xAA, 0, 0, 0, 0, 0, 0, 0x70};
ieee::Eui64 const CONTROLLER{0xC0, 0, 0, 0, 0, 0, 0, 0x0A};
ieee::Eui48 const MAC{0x02, 0, 0, 0, 0, 0x0A};
constexpr uint64_t AAF_4CH_48K = 0x0205022001006000ULL;

/// AUDIO_UNIT 0 with STREAM_PORT_INPUT 0 (four 1-channel clusters) and STREAM_INPUTs 0-1.
class Model final : public AemEntityHandler
{
  public:
    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override
    {
        desc.number_of_stream_input_ports = uint16_t{1};
        desc.current_sampling_rate = uint32_t{48000};
        return id.ref.descriptor_index == 0;
    }
    auto on_get_stream_port(DescriptorId id, DescriptorStreamPort& desc) -> bool override
    {
        desc.number_of_clusters = uint16_t{4};
        return id.ref.descriptor_type == DESCRIPTOR_STREAM_PORT_INPUT && id.ref.descriptor_index == 0;
    }
    auto on_get_audio_cluster(DescriptorId id, DescriptorAudioCluster& desc) -> bool override
    {
        desc.channel_count = uint16_t{1};
        return id.ref.descriptor_index < 4;
    }
    auto on_get_stream(DescriptorId id, DescriptorStream& desc) -> bool override
    {
        desc.current_format.from_uint64(AAF_4CH_48K);
        return id.ref.descriptor_type == DESCRIPTOR_STREAM_INPUT && id.ref.descriptor_index < 2;
    }
};

/// One AEM command of @p type carrying @p body (at most 512 octets of it).
void command(AemCommandHandler& aem, uint16_t const type, std::span<uint8_t const> const body)
{
    std::array<uint8_t, AemDu::LENGTH + 512> packet{};
    auto const data = body.first(std::min<size_t>(body.size(), 512));
    AemDu du{};
    du.init_command(type, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + data.size()));
    du.target_entity_id = US;
    du.controller_entity_id = CONTROLLER;
    span_store(std::span<uint8_t>{packet}.first(AemDu::LENGTH), du);
    std::ranges::copy(data, packet.begin() + AemDu::LENGTH);
    (void)aem.process_packet(MAC, std::span<uint8_t const>{packet}.first(AemDu::LENGTH + data.size()), US);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    uint8_t const verdict = input.empty() || (input[0] & 0x80U) == 0 ? AEM_STATUS_SUCCESS : input[0] & 0x7FU;
    auto const bodies = input.subspan(std::min<size_t>(input.size(), 1));
    Model model;
    AemCommandHandler aem{model};
    aem.set_entity_id(US);
    aem.set_send_response([](ieee::Eui48 const&, std::span<uint8_t const>) { return true; });
    aem.set_on_audio_mappings_changed([verdict](uint16_t, uint16_t, std::span<DynamicAudioMapping const>) { return verdict; });
    command(aem, AEM_COMMAND_ADD_AUDIO_MAPPINGS, bodies.first(bodies.size() / 2));
    command(aem, AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, bodies.subspan(bodies.size() / 2));
    return 0;
}
