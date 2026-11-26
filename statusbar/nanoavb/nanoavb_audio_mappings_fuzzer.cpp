// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for ADD_AUDIO_MAPPINGS and REMOVE_AUDIO_MAPPINGS (IEEE 1722.1-2021 Clauses
/// 7.4.45, 7.4.46) on a dynamic stream input or output port: what each command leaves held must
/// keep its direction's rule, hold against the model, and be what the data plane last accepted.

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace {

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

ieee::Eui64 const US{0xAA, 0, 0, 0, 0, 0, 0, 0x70};
ieee::Eui64 const CONTROLLER{0xC0, 0, 0, 0, 0, 0, 0, 0x0A};
ieee::Eui48 const MAC{0x02, 0, 0, 0, 0, 0x0A};
constexpr uint64_t AAF_4CH_48K = 0x0205022001006000ULL;
constexpr uint16_t CLUSTERS = 4;
constexpr uint16_t STREAMS = 2;
constexpr uint16_t STREAM_CHANNELS = 4;

/// First input octet: bit 7 has the data plane refuse with the low bits, bit 6 selects milan13,
/// bit 5 makes each command name the output port's type.
constexpr uint8_t VERDICT_REFUSES = 0x80U;
constexpr uint8_t PROFILE_MILAN = 0x40U;
constexpr uint8_t PORT_OUTPUT = 0x20U;
constexpr uint8_t VERDICT_MASK = 0x1FU;

/// AUDIO_UNIT 0 at 48 kHz with STREAM_PORT_INPUT 0 and STREAM_PORT_OUTPUT 0 (four 1-channel
/// clusters each), STREAM_INPUTs and STREAM_OUTPUTs 0-1 of four channels at 48 kHz.
class Model final : public AemEntityHandler
{
  public:
    auto on_get_audio_unit(DescriptorId id, DescriptorAudioUnit& desc) -> bool override
    {
        desc.number_of_stream_input_ports = uint16_t{1};
        desc.number_of_stream_output_ports = uint16_t{1};
        desc.current_sampling_rate = uint32_t{48000};
        return id.ref.descriptor_index == 0;
    }
    auto on_get_stream_port(DescriptorId id, DescriptorStreamPort& desc) -> bool override
    {
        desc.number_of_clusters = CLUSTERS;
        return id.ref.descriptor_index == 0;
    }
    auto on_get_audio_cluster(DescriptorId id, DescriptorAudioCluster& desc) -> bool override
    {
        desc.channel_count = uint16_t{1};
        return id.ref.descriptor_index < CLUSTERS;
    }
    auto on_get_stream(DescriptorId id, DescriptorStream& desc) -> bool override
    {
        desc.current_format.from_uint64(AAF_4CH_48K);
        return id.ref.descriptor_index < STREAMS;
    }
};

/// What the fuzzer saw: the data plane's last accepted set of each port, and the last response.
struct Observed
{
    std::array<DynamicAudioMaps::Mappings, 2> in_force{};  ///< the input port's, the output port's
    std::array<uint8_t, AemDu::LENGTH + MAX_AEM_RESPONSE_SIZE> response{};
    size_t response_size{0};
};

/// One AEM command of @p type carrying @p body (at most what an AECPDU carries of it), its
/// descriptor_type made @p port_type when that is given.
void command(
    AemCommandHandler& aem, Observed& seen, uint16_t const type, std::span<uint8_t const> const body, uint16_t const port_type = 0)
{
    std::array<uint8_t, AemDu::LENGTH + MAX_AEM_BODY> packet{};
    auto const data = body.first(std::min(body.size(), MAX_AEM_BODY));
    AemDu du{};
    du.init_command(type, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + data.size()));
    du.target_entity_id = US;
    du.controller_entity_id = CONTROLLER;
    span_store(std::span<uint8_t>{packet}.first(AemDu::LENGTH), du);
    std::ranges::copy(data, packet.begin() + AemDu::LENGTH);
    if (port_type != 0 && data.size() >= sizeof(ieee::doublet_t)) {
        span_store(std::span<uint8_t>{packet}.subspan(AemDu::LENGTH), ieee::doublet_t{port_type});
    }
    seen.response_size = 0;
    (void)aem.process_packet(MAC, std::span<uint8_t const>{packet}.first(AemDu::LENGTH + data.size()), US);
}

/// The channel @p m maps on a port of @p port_type: an input's cluster channel, an output's stream channel.
auto mapped_channel(uint16_t const port_type, DynamicAudioMapping const& m) -> uint32_t
{
    return port_type == DESCRIPTOR_STREAM_PORT_INPUT ? m.cluster_offset
                                                     : (uint32_t{m.stream_index} * STREAM_CHANNELS) + m.stream_channel;
}

/// Whether @p m names what the model has.
auto named(DynamicAudioMapping const& m) -> bool
{
    return m.cluster_offset < CLUSTERS && m.cluster_channel == 0 && m.stream_index < STREAMS && m.stream_channel < STREAM_CHANNELS;
}

/// The store holds the two ports' mappings only, each port's sorted, unique, each channel of the
/// port's direction mapped once, naming what the model has, holding, and the set the data plane
/// last accepted for it.
void check_store(AemCommandHandler const& aem, DynamicAudioMaps const& maps, Observed const& seen)
{
    std::array<std::array<bool, size_t{STREAMS} * STREAM_CHANNELS>, 2> mapped{};
    std::optional<std::pair<uint16_t, DynamicAudioMapping>> previous{};
    maps.for_each([&](uint16_t const type, uint16_t const index, DynamicAudioMapping const& m) {
        bool const port = (type == DESCRIPTOR_STREAM_PORT_INPUT || type == DESCRIPTOR_STREAM_PORT_OUTPUT) && index == 0;
        bool const after = !previous || previous->first < type || (previous->first == type && cluster_order(previous->second, m));
        bool const output = type == DESCRIPTOR_STREAM_PORT_OUTPUT;
        if (!port || !named(m) || !after || std::exchange(mapped.at(output ? 1 : 0).at(mapped_channel(type, m)), true)) {
            __builtin_trap();
        }
        previous = std::pair{type, m};
    });
    if (maps.size() > DynamicAudioMaps::MAX_MAPPINGS || !aem.mappings_hold() ||
        !std::ranges::equal(maps.port(DESCRIPTOR_STREAM_PORT_INPUT, 0), seen.in_force[0]) ||
        !std::ranges::equal(maps.port(DESCRIPTOR_STREAM_PORT_OUTPUT, 0), seen.in_force[1])) {
        __builtin_trap();
    }
}

/// GET_AUDIO_MAP of the port's map 0: on SUCCESS the response counts the mappings it carries, every
/// one held, and no more than the port's channels.
void check_get_audio_map(AemCommandHandler& aem, DynamicAudioMaps const& maps, uint16_t const port_type, Observed& seen)
{
    std::array<uint8_t, AemGetAudioMapCommandPayload::LENGTH> get{};
    span_store(get, AemGetAudioMapCommandPayload{.descriptor_type = port_type});
    command(aem, seen, AEM_COMMAND_GET_AUDIO_MAP, get);
    auto const response = std::span<uint8_t const>{seen.response}.first(seen.response_size);
    if (response.size() < AemDu::LENGTH + AemAudioMapResponseHeader::LENGTH) {
        __builtin_trap();  // a dynamic port's map 0 is always answered
    }
    AemDu du{};
    AemAudioMapResponseHeader header{};
    span_load(du, response);
    span_load(header, response.subspan(AemDu::LENGTH));
    size_t const count = header.number_of_mappings.get();
    bool const counted = response.size() == AemDu::LENGTH + AemAudioMapResponseHeader::LENGTH + (count * AemAudioMapping::LENGTH);
    if (du.status() != AEM_STATUS_SUCCESS || !counted || count != maps.port(port_type, 0).size() ||
        count > size_t{STREAMS} * STREAM_CHANNELS) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input{data, size};
    uint8_t const control = input.empty() ? uint8_t{0} : input[0];
    uint8_t const verdict = (control & VERDICT_REFUSES) == 0 ? AEM_STATUS_SUCCESS : (control & VERDICT_MASK);
    uint16_t const port_type = (control & PORT_OUTPUT) != 0 ? DESCRIPTOR_STREAM_PORT_OUTPUT : uint16_t{0};
    auto const bodies = input.subspan(std::min<size_t>(input.size(), 1));
    static DynamicAudioMaps maps;  // the entity's store: 20 KB, kept off the stack
    maps.clear();
    Model model;
    AemCommandHandler aem{model};
    Observed seen{};
    aem.set_entity_id(US);
    aem.set_profile((control & PROFILE_MILAN) != 0 ? AemCommandHandler::Profile::milan13 : AemCommandHandler::Profile::classic);
    aem.set_dynamic_audio_maps(&maps);
    aem.set_send_response([&seen](ieee::Eui48 const&, std::span<uint8_t const> const bytes) {
        seen.response_size = std::min(bytes.size(), seen.response.size());
        std::ranges::copy(bytes.first(seen.response_size), seen.response.begin());
        return true;
    });
    aem.set_on_audio_mappings_changed(
        [verdict, &seen](uint16_t const type, uint16_t, std::span<DynamicAudioMapping const> const mappings) {
            if (verdict == AEM_STATUS_SUCCESS) {
                seen.in_force.at(type == DESCRIPTOR_STREAM_PORT_OUTPUT ? 1 : 0).assign(mappings.begin(), mappings.end());
            }
            return verdict;
        });
    command(aem, seen, AEM_COMMAND_ADD_AUDIO_MAPPINGS, bodies.first(bodies.size() / 2), port_type);
    check_store(aem, maps, seen);
    check_get_audio_map(aem, maps, DESCRIPTOR_STREAM_PORT_INPUT, seen);
    check_get_audio_map(aem, maps, DESCRIPTOR_STREAM_PORT_OUTPUT, seen);
    command(aem, seen, AEM_COMMAND_REMOVE_AUDIO_MAPPINGS, bodies.subspan(bodies.size() / 2), port_type);
    check_store(aem, maps, seen);
    check_get_audio_map(aem, maps, DESCRIPTOR_STREAM_PORT_INPUT, seen);
    check_get_audio_map(aem, maps, DESCRIPTOR_STREAM_PORT_OUTPUT, seen);
    return 0;
}
