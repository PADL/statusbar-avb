#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// A Milan vendor-unique (MVU) command as a controller sends it, and the
/// reading of the response that answers it - Milan v1.3 5.4.3 and 5.4.4.
///
/// The entity side of MVU is in nanoavb (AemCommandHandler). This is the
/// other end, as far as a tool that sends one command and prints the answer
/// needs it: the four commands a Milan device answers here, the AECPDU of
/// each, which received AECPDU is its answer, and that answer as a line of
/// text. Nothing here opens a socket or keeps time.

#include "statusbar/atdecc/atdecc_aecp_mvu.hpp"
#include "statusbar/ieee/ieee.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace statusbar::atdecc {

/// Who an MVU command is for and from, and its sequence_id.
struct MvuParties
{
    Eui64 target_entity_id{};
    Eui64 controller_entity_id{};
    uint16_t sequence_id{0};
};

/// One MVU command: its parties, its type and its command-specific data.
struct MvuRequest
{
    /// The longest command-specific data of the commands made here: BIND_STREAM's.
    static constexpr size_t MAX_DATA = MvuBindStream::LENGTH;
    /// The longest AECPDU, from the AVTP subtype octet on.
    static constexpr size_t MAX_PDU = AecpMvuDu::LENGTH + MAX_DATA;

    MvuParties parties{};
    uint16_t command_type{0};
    std::array<uint8_t, MAX_DATA> data{};
    size_t data_size{0};

    [[nodiscard]] auto command_data() const noexcept -> std::span<uint8_t const> { return std::span{data}.first(data_size); }
};

/// What a BIND_STREAM binds: a stream input of the target to a talker's stream output.
struct MvuBindTarget
{
    uint16_t stream_input{0};
    Eui64 talker_entity_id{};
    uint16_t talker_stream_index{0};
    bool streaming_wait{false};  ///< bind it stopped (Table 5.19)
};

/// GET_MILAN_INFO (5.4.4.1).
[[nodiscard]] auto mvu_get_milan_info_request(MvuParties const& parties) noexcept -> MvuRequest;

/// BIND_STREAM (5.4.4.6).
[[nodiscard]] auto mvu_bind_stream_request(MvuParties const& parties, MvuBindTarget const& bind) noexcept -> MvuRequest;

/// UNBIND_STREAM of STREAM_INPUT @p stream_input (5.4.4.7).
[[nodiscard]] auto mvu_unbind_stream_request(MvuParties const& parties, uint16_t stream_input) noexcept -> MvuRequest;

/// GET_STREAM_INPUT_INFO_EX of STREAM_INPUT @p stream_input (5.4.4.8).
[[nodiscard]] auto mvu_get_stream_input_info_ex_request(MvuParties const& parties, uint16_t stream_input) noexcept -> MvuRequest;

/// The AECPDU of @p request written to @p out, from the AVTP subtype octet
/// on: what goes behind the Ethernet header, unicast to the target. Empty
/// when @p out is shorter than it (MvuRequest::MAX_PDU always holds one).
[[nodiscard]] auto mvu_request_pdu(MvuRequest const& request, std::span<uint8_t> out) noexcept -> std::span<uint8_t const>;

/// The response to a request: its status (Table 5.16) and the
/// command-specific data its control_data_length covers.
struct MvuAnswer
{
    uint8_t status{MVU_STATUS_SUCCESS};
    std::span<uint8_t const> data{};
};

/// The answer to @p request, when the received AECPDU @p pdu is it: an MVU
/// response from the request's target to its controller, with its
/// sequence_id and command_type, and not a notification (u clear,
/// 5.4.3.2.2). nullopt for anything else. The data points into @p pdu.
[[nodiscard]] auto mvu_answer_to(MvuRequest const& request, std::span<uint8_t const> pdu) noexcept -> std::optional<MvuAnswer>;

/// @p answer to a command of @p command_type as one line: the command, the
/// status by name, and each field of the response's data by name. Data
/// shorter than the command's response format is given as a count of octets.
[[nodiscard]] auto mvu_answer_text(uint16_t command_type, MvuAnswer const& answer) -> std::string;

}  // namespace statusbar::atdecc
