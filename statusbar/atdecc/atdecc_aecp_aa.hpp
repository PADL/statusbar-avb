#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AECP Address Access Protocol - IEEE 1722.1 Clause 9.2.1.3
/// Provides register-based read/write/execute to exposed address spaces.
///
/// Wire format:
///   Bytes 0-21:  AECP common header (AecpDuCommon)
///   Bytes 22-23: tlv_count (uint16 big-endian)
///   Bytes 24+:   TLV data (variable)
///
/// Each TLV:
///   Bits 0-3:    mode (4 bits)
///   Bits 4-15:   length (12 bits)
///   Bytes 2-9:   address (uint64 big-endian)
///   Bytes 10+:   memory_data (length octets)

#include "statusbar/atdecc/atdecc_aecp.hpp"
#include "statusbar/atdecc/atdecc_error.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

namespace statusbar::atdecc {

using ieee::doublet_t;
using ieee::Eui64;
using ieee::octet_t;
using ieee::octlet_t;
using statusbar::Status;
using statusbar::StatusValue;

// ---------------------------------------------------------------------------
// Address Access Constants
// ---------------------------------------------------------------------------

/// Address Access TLV mode values - Clause 9.2.1.3
constexpr uint8_t AA_MODE_READ = 0;
constexpr uint8_t AA_MODE_WRITE = 1;
constexpr uint8_t AA_MODE_EXECUTE = 2;

/// Address Access status codes - Clause 9.2.1.3
constexpr uint8_t AA_STATUS_SUCCESS = 0;
constexpr uint8_t AA_STATUS_NOT_IMPLEMENTED = 1;
constexpr uint8_t AA_STATUS_ADDRESS_TOO_LOW = 2;
constexpr uint8_t AA_STATUS_ADDRESS_TOO_HIGH = 3;
constexpr uint8_t AA_STATUS_ADDRESS_INVALID = 4;
constexpr uint8_t AA_STATUS_TLV_INVALID = 5;
constexpr uint8_t AA_STATUS_DATA_INVALID = 6;
constexpr uint8_t AA_STATUS_UNSUPPORTED = 7;

/// Address Access timeout - Clause 9.2.1.3
constexpr uint32_t AA_TIMEOUT_MS = 250;

/// Minimum TLV header size (2 bytes mode/length + 8 bytes address)
constexpr size_t AA_TLV_HEADER_SIZE = 10;

/// Get human-readable name for AA mode
[[nodiscard]] auto aa_mode_name(uint8_t mode) noexcept -> std::string_view;

/// Get human-readable name for AA status code
[[nodiscard]] auto aa_status_name(uint8_t status) noexcept -> std::string_view;

// ---------------------------------------------------------------------------
// Address Access PDU header (fixed part after AECP common)
// ---------------------------------------------------------------------------

/// Address Access AECPDU - extends AECP common header with tlv_count.
/// Wire format: AecpDuCommon (22 bytes) + tlv_count (2 bytes) = 24 bytes fixed.
struct AecpAaDu
{
    static constexpr size_t LENGTH = 24;

    // AECP common header (22 bytes)
    AecpDuCommon common{};

    // Address Access specific field
    doublet_t tlv_count{0};

    /// Initialize as an Address Access command
    constexpr void init_command(Eui64 target, Eui64 controller, uint16_t seq_id, uint16_t num_tlvs, uint16_t payload_len)
    {
        common.init_command(
            AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND, static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 2 + payload_len));
        common.target_entity_id = target;
        common.controller_entity_id = controller;
        common.sequence_id = seq_id;
        tlv_count = num_tlvs;
    }

    /// Initialize as an Address Access response
    constexpr void init_response(
        Eui64 target, Eui64 controller, uint16_t seq_id, uint8_t status, uint16_t num_tlvs, uint16_t payload_len)
    {
        common.init_response(
            AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE,
            status,
            static_cast<uint16_t>(AecpDuCommon::COMMON_DATA_LENGTH + 2 + payload_len));
        common.target_entity_id = target;
        common.controller_entity_id = controller;
        common.sequence_id = seq_id;
        tlv_count = num_tlvs;
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return common.is_valid() && common.is_address_access(); }

    auto operator<=>(AecpAaDu const& rhs) const noexcept -> std::strong_ordering = default;
};

static_assert(sizeof(AecpAaDu) == 24, "AecpAaDu must be exactly 24 bytes");
static_assert(offsetof(AecpAaDu, common) == 0, "common must be at offset 0");
static_assert(offsetof(AecpAaDu, tlv_count) == 22, "tlv_count must be at offset 22");

// ---------------------------------------------------------------------------
// Address Access TLV
// ---------------------------------------------------------------------------

/// Fixed wire header of one Address Access TLV (Clause 9.2.1.3):
/// mode/length word + 64-bit base address, followed on the wire by
/// `length()` octets of memory_data.
struct AaTlvHeader
{
    static constexpr size_t LENGTH = AA_TLV_HEADER_SIZE;

    static constexpr uint16_t MODE_SHIFT = 12;
    static constexpr uint16_t MODE_MASK = 0x0F;
    static constexpr uint16_t LENGTH_MASK = 0x0FFF;

    /// Bytes 0-1: mode (bits 15-12) | length (bits 11-0)
    doublet_t mode_length{0};

    /// Bytes 2-9: base address
    octlet_t address{0};

    [[nodiscard]] constexpr auto mode() const noexcept -> uint8_t
    {
        return static_cast<uint8_t>((mode_length.get() >> MODE_SHIFT) & MODE_MASK);
    }
    [[nodiscard]] constexpr auto length() const noexcept -> uint16_t
    {
        return static_cast<uint16_t>(mode_length.get() & LENGTH_MASK);
    }
    constexpr void set_mode_length(uint8_t const mode, uint16_t const length) noexcept
    {
        mode_length = static_cast<uint16_t>((static_cast<uint16_t>(mode & MODE_MASK) << MODE_SHIFT) | (length & LENGTH_MASK));
    }

    auto operator<=>(AaTlvHeader const&) const noexcept = default;
};

static_assert(sizeof(AaTlvHeader) == AA_TLV_HEADER_SIZE, "AaTlvHeader must be exactly 10 bytes");

/// Parsed Address Access TLV (not a wire-format struct).
struct AaTlv
{
    uint8_t mode{0};                       ///< AA_MODE_READ / WRITE / EXECUTE
    uint16_t length{0};                    ///< Number of octets in memory_data
    uint64_t address{0};                   ///< Base address
    std::span<uint8_t const> memory_data;  ///< View into the packet data
};

// ---------------------------------------------------------------------------
// TLV Parsing (callback-based)
// ---------------------------------------------------------------------------

/// Parse Address Access TLVs from raw payload following the AecpAaDu header.
///
/// Calls `on_tlv(tlv_index, mode, address, memory_data)` for each valid TLV.
/// Returns failure if the TLV data is malformed.
///
/// @param tlv_count Number of TLVs expected
/// @param tlv_data Raw bytes following the tlv_count field
/// @param on_tlv Callback invoked for each TLV
template <typename Func>
    requires std::invocable<Func, uint16_t, uint8_t, uint64_t, std::span<uint8_t const>>
auto aa_parse_tlvs(uint16_t tlv_count, std::span<uint8_t const> tlv_data, Func const& on_tlv) -> Status
{
    size_t offset = 0;

    for (uint16_t i = 0; i < tlv_count; ++i) {
        if (offset + AaTlvHeader::LENGTH > tlv_data.size()) {
            return failure(make_error_code(AtdeccError::truncated));
        }

        AaTlvHeader header{};
        span_load(header, tlv_data.subspan(offset));
        offset += AaTlvHeader::LENGTH;

        // Validate memory_data fits in remaining buffer
        auto const length = header.length();
        if (offset + length > tlv_data.size()) {
            return failure(make_error_code(AtdeccError::truncated));
        }

        auto const data = tlv_data.subspan(offset, length);
        on_tlv(i, header.mode(), header.address.get(), data);

        offset += length;
    }

    return success();
}

// ---------------------------------------------------------------------------
// TLV Building
// ---------------------------------------------------------------------------

/// Builder for constructing Address Access TLV payload.
///
/// Usage:
///   AaTlvBuilder builder;
///   builder.add_read(0x1000, 64);
///   builder.add_write(0x2000, data_span);
///   auto pdu = builder.build_command(target, controller, seq_id);
class AaTlvBuilder
{
  public:
    /// @param memory_resource Memory resource for the internal TLV
    ///        payload buffer. nullptr → std::pmr::get_default_resource().
    ///        The same resource is used for vectors returned from
    ///        build_command() / build_response().
    explicit AaTlvBuilder(std::pmr::memory_resource* memory_resource = nullptr)
        : payload_{memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource()}
    {}

    /// Add a READ TLV (memory_data is zero-filled in command).
    void add_read(uint64_t address, uint16_t length) { append_tlv(AA_MODE_READ, address, length, {}); }

    /// Add a WRITE TLV with data.
    void add_write(uint64_t address, std::span<uint8_t const> data)
    {
        append_tlv(AA_MODE_WRITE, address, static_cast<uint16_t>(data.size()), data);
    }

    /// Add an EXECUTE TLV with optional data.
    void add_execute(uint64_t address, std::span<uint8_t const> data = {})
    {
        append_tlv(AA_MODE_EXECUTE, address, static_cast<uint16_t>(data.size()), data);
    }

    /// Get the number of TLVs added.
    [[nodiscard]] auto tlv_count() const noexcept -> uint16_t { return tlv_count_; }

    /// Get the raw TLV payload bytes.
    [[nodiscard]] auto tlv_data() const noexcept -> std::span<uint8_t const> { return payload_; }

    /// Build a complete Address Access command frame (header + TLV payload).
    /// Returns the full PDU bytes. Allocated through the builder's
    /// memory_resource.
    [[nodiscard]] auto build_command(Eui64 target, Eui64 controller, uint16_t seq_id) const -> std::pmr::vector<uint8_t>;

    /// Build a complete Address Access response frame.
    [[nodiscard]] auto build_response(Eui64 target, Eui64 controller, uint16_t seq_id, uint8_t status) const
        -> std::pmr::vector<uint8_t>;

    /// Clear all TLVs.
    void clear()
    {
        payload_.clear();
        tlv_count_ = 0;
    }

  private:
    void append_tlv(uint8_t mode, uint64_t address, uint16_t length, std::span<uint8_t const> data);

    std::pmr::vector<uint8_t> payload_;
    uint16_t tlv_count_{0};
};

}  // namespace statusbar::atdecc

// Serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AecpAaDu> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::AaTlvHeader> : std::true_type
{};

namespace statusbar::atdecc {
using protocol::load_unchecked;
using protocol::store_unchecked;
}  // namespace statusbar::atdecc
