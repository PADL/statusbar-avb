// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_mvu_request.hpp"

#include "statusbar/atdecc/atdecc_acmp_types.hpp"
#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace statusbar::atdecc {

namespace {

/// The request of @p command_type whose command-specific data is @p data.
template <typename Data>
[[nodiscard]] auto request_with(MvuParties const& parties, uint16_t const command_type, Data const& data) noexcept -> MvuRequest
{
    MvuRequest request{.parties = parties, .command_type = command_type, .data_size = Data::LENGTH};
    span_store(std::span{request.data}, data);
    return request;
}

[[nodiscard]] auto stream_input_named(uint16_t const stream_input) noexcept -> MvuStreamInput
{
    MvuStreamInput input{};
    input.descriptor_type = aem::DESCRIPTOR_STREAM_INPUT;
    input.descriptor_index = stream_input;
    return input;
}

/// One bit of GET_MILAN_INFO's features_flags and its name (Table 5.17).
struct Feature
{
    uint32_t bit;
    std::string_view name;
};
constexpr std::array<Feature, 4> FEATURES{
    Feature{MVU_FEATURE_REDUNDANCY, "REDUNDANCY"},
    Feature{MVU_FEATURE_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING, "TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING"},
    Feature{MVU_FEATURE_MVU_BINDING, "MVU_BINDING"},
    Feature{MVU_FEATURE_TALKER_SIGNAL_PRESENCE, "TALKER_SIGNAL_PRESENCE"}};

/// The names of the bits set in @p flags, "none" for none.
[[nodiscard]] auto features_text(uint32_t const flags) -> std::string
{
    std::string text{};
    for (auto const& feature : FEATURES) {
        if ((flags & feature.bit) != 0) {
            text += (text.empty() ? "" : " ");
            text += feature.name;
        }
    }
    return text.empty() ? std::string{"none"} : text;
}

[[nodiscard]] auto milan_info_text(std::span<uint8_t const> const data) -> std::string
{
    MvuGetMilanInfoResponse info{};
    span_load(info, data.first(MvuGetMilanInfoResponse::LENGTH));
    return std::format(
        "protocol_version={} features_flags=0x{:08x} ({}) certification_version={} specification_version={}",
        info.protocol_version.get(),
        info.features_flags.get(),
        features_text(info.features_flags.get()),
        mvu_version_to_dotted(info.certification_version.get()),
        mvu_version_to_dotted(info.specification_version.get()));
}

[[nodiscard]] auto bind_stream_text(std::span<uint8_t const> const data) -> std::string
{
    MvuBindStream bind{};
    span_load(bind, data.first(MvuBindStream::LENGTH));
    return std::format(
        "descriptor_type={} stream_input={} talker={} talker_stream={} streaming_wait={}",
        bind.descriptor_type.get(),
        bind.descriptor_index.get(),
        ieee::to_string(bind.talker_entity_id).view(),
        bind.talker_stream_index.get(),
        bind.streaming_wait() ? 1 : 0);
}

[[nodiscard]] auto stream_input_text(std::span<uint8_t const> const data) -> std::string
{
    MvuStreamInput input{};
    span_load(input, data.first(MvuStreamInput::LENGTH));
    return std::format("descriptor_type={} stream_input={}", input.descriptor_type.get(), input.descriptor_index.get());
}

[[nodiscard]] auto stream_input_info_ex_text(std::span<uint8_t const> const data) -> std::string
{
    MvuGetStreamInputInfoExResponse info{};
    span_load(info, data.first(MvuGetStreamInputInfoExResponse::LENGTH));
    return std::format(
        "descriptor_type={} stream_input={} talker={} talker_unique_id={} pbsta={} ({}) acmpsta={} ({})",
        info.descriptor_type.get(),
        info.descriptor_index.get(),
        ieee::to_string(info.talker_entity_id).view(),
        info.talker_unique_id.get(),
        info.pbsta(),
        mvu_probing_status_name(info.pbsta()),
        info.acmpsta(),
        acmp_status_name(info.acmpsta()));
}

/// How long the response data of @p command_type is, and how it reads.
struct ResponseFormat
{
    size_t length{0};
    std::string (*text)(std::span<uint8_t const>){nullptr};
};

[[nodiscard]] auto response_format(uint16_t const command_type) noexcept -> ResponseFormat
{
    switch (command_type) {
        case MVU_COMMAND_GET_MILAN_INFO:
            return {.length = MvuGetMilanInfoResponse::LENGTH, .text = milan_info_text};
        case MVU_COMMAND_BIND_STREAM:
            return {.length = MvuBindStream::LENGTH, .text = bind_stream_text};
        case MVU_COMMAND_UNBIND_STREAM:
            return {.length = MvuStreamInput::LENGTH, .text = stream_input_text};
        case MVU_COMMAND_GET_STREAM_INPUT_INFO_EX:
            return {.length = MvuGetStreamInputInfoExResponse::LENGTH, .text = stream_input_info_ex_text};
        default:
            return {};
    }
}

/// Whether @p header is the direct response to @p request.
[[nodiscard]] auto answers(MvuRequest const& request, AecpMvuDu const& header) noexcept -> bool
{
    bool const ours = header.common.target_entity_id == request.parties.target_entity_id &&
        header.common.controller_entity_id == request.parties.controller_entity_id &&
        header.common.sequence_id.get() == request.parties.sequence_id;
    return header.is_valid() && header.is_response() && !header.is_unsolicited() && ours &&
        header.command_type() == request.command_type;
}

/// The MVU header @p request goes out behind.
[[nodiscard]] auto request_header(MvuRequest const& request) noexcept -> AecpMvuDu
{
    AecpMvuDu header{};
    header.init_command(request.command_type, static_cast<uint16_t>(request.data_size));
    header.common.target_entity_id = request.parties.target_entity_id;
    header.common.controller_entity_id = request.parties.controller_entity_id;
    header.common.sequence_id = request.parties.sequence_id;
    return header;
}

/// The command-specific data behind @p header in @p pdu: what its
/// control_data_length covers, not the padding of a short frame.
[[nodiscard]] auto data_behind(AecpMvuDu const& header, std::span<uint8_t const> const pdu) noexcept -> std::span<uint8_t const>
{
    size_t const declared = header.common.control_data_length() - AecpMvuDu::MVU_DATA_LENGTH;
    return pdu.subspan(AecpMvuDu::LENGTH, std::min(declared, pdu.size() - AecpMvuDu::LENGTH));
}

}  // namespace

auto mvu_get_milan_info_request(MvuParties const& parties) noexcept -> MvuRequest
{
    // Two reserved octets behind the command_type (Figure 5.5).
    return MvuRequest{
        .parties = parties, .command_type = MVU_COMMAND_GET_MILAN_INFO, .data_size = MVU_GET_MILAN_INFO_COMMAND_LENGTH};
}

auto mvu_bind_stream_request(MvuParties const& parties, MvuBindTarget const& bind) noexcept -> MvuRequest
{
    MvuBindStream const data{
        .flags = doublet_t{bind.streaming_wait ? MVU_BIND_STREAM_FLAG_STREAMING_WAIT : uint16_t{0}},
        .descriptor_type = doublet_t{aem::DESCRIPTOR_STREAM_INPUT},
        .descriptor_index = doublet_t{bind.stream_input},
        .talker_entity_id = bind.talker_entity_id,
        .talker_stream_index = doublet_t{bind.talker_stream_index}};
    return request_with(parties, MVU_COMMAND_BIND_STREAM, data);
}

auto mvu_unbind_stream_request(MvuParties const& parties, uint16_t const stream_input) noexcept -> MvuRequest
{
    return request_with(parties, MVU_COMMAND_UNBIND_STREAM, stream_input_named(stream_input));
}

auto mvu_get_stream_input_info_ex_request(MvuParties const& parties, uint16_t const stream_input) noexcept -> MvuRequest
{
    return request_with(parties, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, stream_input_named(stream_input));
}

auto mvu_request_pdu(MvuRequest const& request, std::span<uint8_t> const out) noexcept -> std::span<uint8_t const>
{
    size_t const length = AecpMvuDu::LENGTH + request.data_size;
    if (out.size() < length) {
        return {};
    }
    span_store(out.first(AecpMvuDu::LENGTH), request_header(request));
    span_copy(out.subspan(AecpMvuDu::LENGTH, request.data_size), request.command_data());
    return out.first(length);
}

auto mvu_answer_to(MvuRequest const& request, std::span<uint8_t const> const pdu) noexcept -> std::optional<MvuAnswer>
{
    if (pdu.size() < AecpMvuDu::LENGTH) {
        return std::nullopt;
    }
    AecpMvuDu header{};
    span_load(header, pdu.first(AecpMvuDu::LENGTH));
    if (!answers(request, header)) {
        return std::nullopt;
    }
    return MvuAnswer{.status = header.common.status(), .data = data_behind(header, pdu)};
}

auto mvu_answer_text(uint16_t const command_type, MvuAnswer const& answer) -> std::string
{
    auto const format = response_format(command_type);
    bool const readable = format.text != nullptr && answer.data.size() >= format.length;
    return std::format(
        "{} {}: {}",
        mvu_command_name(command_type),
        mvu_status_name(answer.status),
        readable ? format.text(answer.data) : std::format("{} octets of data", answer.data.size()));
}

}  // namespace statusbar::atdecc
