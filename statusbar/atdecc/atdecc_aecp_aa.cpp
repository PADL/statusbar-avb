// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp_aa.hpp"

#include "statusbar/buffer/span_utils.hpp"

namespace statusbar::atdecc {

auto aa_mode_name(uint8_t const mode) noexcept -> char const*
{
    switch (mode) {
        case AA_MODE_READ:
            return "READ";
        case AA_MODE_WRITE:
            return "WRITE";
        case AA_MODE_EXECUTE:
            return "EXECUTE";
        default:
            return "Unknown";
    }
}

auto aa_status_name(uint8_t const status) noexcept -> char const*
{
    switch (status) {
        case AA_STATUS_SUCCESS:
            return "SUCCESS";
        case AA_STATUS_NOT_IMPLEMENTED:
            return "NOT_IMPLEMENTED";
        case AA_STATUS_ADDRESS_TOO_LOW:
            return "ADDRESS_TOO_LOW";
        case AA_STATUS_ADDRESS_TOO_HIGH:
            return "ADDRESS_TOO_HIGH";
        case AA_STATUS_ADDRESS_INVALID:
            return "ADDRESS_INVALID";
        case AA_STATUS_TLV_INVALID:
            return "TLV_INVALID";
        case AA_STATUS_DATA_INVALID:
            return "DATA_INVALID";
        case AA_STATUS_UNSUPPORTED:
            return "UNSUPPORTED";
        default:
            return "Unknown";
    }
}

void AaTlvBuilder::append_tlv(uint8_t const mode, uint64_t const address, uint16_t const length, std::span<uint8_t const> data)
{
    AaTlvHeader header{};
    header.set_mode_length(mode, length);
    header.address = address;
    auto const base = payload_.size();
    payload_.resize(base + AaTlvHeader::LENGTH);
    span_store(make_span(payload_, {.start = base}), header);

    // Memory data
    if (!data.empty()) {
        payload_.insert(payload_.end(), data.begin(), data.end());
    } else if (length > 0) {
        // READ command: zero-fill memory_data
        payload_.resize(payload_.size() + length, 0);
    }

    ++tlv_count_;
}

auto AaTlvBuilder::build_command(Eui64 const target, Eui64 const controller, uint16_t const seq_id) const
    -> std::pmr::vector<uint8_t>
{
    auto const payload_len = static_cast<uint16_t>(payload_.size());
    std::pmr::vector<uint8_t> frame(AecpAaDu::LENGTH + payload_len, uint8_t{0}, payload_.get_allocator().resource());

    AecpAaDu pdu{};
    pdu.init_command(target, controller, seq_id, tlv_count_, payload_len);

    (void)span_pack_header_payload(frame, pdu, make_const_span(payload_));
    return frame;
}

auto AaTlvBuilder::build_response(Eui64 const target, Eui64 const controller, uint16_t const seq_id, uint8_t const status) const
    -> std::pmr::vector<uint8_t>
{
    auto const payload_len = static_cast<uint16_t>(payload_.size());
    std::pmr::vector<uint8_t> frame(AecpAaDu::LENGTH + payload_len, uint8_t{0}, payload_.get_allocator().resource());

    AecpAaDu pdu{};
    pdu.init_response(target, controller, seq_id, status, tlv_count_, payload_len);

    (void)span_pack_header_payload(frame, pdu, make_const_span(payload_));
    return frame;
}

}  // namespace statusbar::atdecc
