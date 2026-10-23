#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AEM Descriptor Storage binary format reader - zero-copy access to .aem blobs
/// Based on jdksavdecc descriptor_storage format

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/buffer_traits.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/safe_arith/safe_arith.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::atdecc::aem {

using ieee::doublet_t;
using ieee::quadlet_t;

//
// Descriptor Storage Header - 20 bytes
//
struct DescriptorStorageHeader
{
    static constexpr size_t LENGTH = 20;

    quadlet_t magic{0};          // 0 - "AEM1" = 0x41454d31
    quadlet_t toc_count{0};      // 4
    quadlet_t toc_offset{0};     // 8
    quadlet_t symbol_count{0};   // 12
    quadlet_t symbol_offset{0};  // 16

    auto operator<=>(DescriptorStorageHeader const&) const noexcept = default;
};

static_assert(sizeof(DescriptorStorageHeader) == 20);
static_assert(offsetof(DescriptorStorageHeader, magic) == 0);
static_assert(offsetof(DescriptorStorageHeader, toc_count) == 4);
static_assert(offsetof(DescriptorStorageHeader, toc_offset) == 8);
static_assert(offsetof(DescriptorStorageHeader, symbol_count) == 12);
static_assert(offsetof(DescriptorStorageHeader, symbol_offset) == 16);

//
// Descriptor Storage TOC Entry - 12 bytes
//
struct DescriptorStorageTocEntry
{
    static constexpr size_t LENGTH = 12;

    doublet_t descriptor_type{0};      // 0
    doublet_t descriptor_index{0};     // 2
    doublet_t configuration_index{0};  // 4
    doublet_t length{0};               // 6
    quadlet_t offset{0};               // 8

    auto operator<=>(DescriptorStorageTocEntry const&) const noexcept = default;
};

static_assert(sizeof(DescriptorStorageTocEntry) == 12);
static_assert(offsetof(DescriptorStorageTocEntry, descriptor_type) == 0);
static_assert(offsetof(DescriptorStorageTocEntry, descriptor_index) == 2);
static_assert(offsetof(DescriptorStorageTocEntry, configuration_index) == 4);
static_assert(offsetof(DescriptorStorageTocEntry, length) == 6);
static_assert(offsetof(DescriptorStorageTocEntry, offset) == 8);

//
// Descriptor Storage Symbol Entry - 10 bytes
//
struct DescriptorStorageSymbolEntry
{
    static constexpr size_t LENGTH = 10;

    doublet_t descriptor_type{0};      // 0
    doublet_t descriptor_index{0};     // 2
    doublet_t configuration_index{0};  // 4
    quadlet_t symbol{0};               // 6

    auto operator<=>(DescriptorStorageSymbolEntry const&) const noexcept = default;
};

static_assert(sizeof(DescriptorStorageSymbolEntry) == 10);
static_assert(offsetof(DescriptorStorageSymbolEntry, descriptor_type) == 0);
static_assert(offsetof(DescriptorStorageSymbolEntry, descriptor_index) == 2);
static_assert(offsetof(DescriptorStorageSymbolEntry, configuration_index) == 4);
static_assert(offsetof(DescriptorStorageSymbolEntry, symbol) == 6);

//
// DescriptorStorage - zero-copy reader for .aem blobs
//
class DescriptorStorage
{
  public:
    static constexpr uint32_t MAGIC = 0x41454d31;  // "AEM1"

    [[nodiscard]] static auto create(std::span<uint8_t const> blob) noexcept -> StatusValue<DescriptorStorage>
    {
        if (blob.size() < DescriptorStorageHeader::LENGTH) {
            return failure(BufferError::insufficient_data);
        }

        DescriptorStorageHeader header{};
        span_load(header, blob);

        if (static_cast<uint32_t>(header.magic) != MAGIC) {
            return failure(BufferError::invalid_offset);
        }

        // Bounds-check the TOC and symbol-table regions against the blob.
        // Multiplications use size_t with overflow checking because count
        // and offset are attacker-controlled (32-bit wire fields).
        auto const toc_offset = static_cast<size_t>(static_cast<uint32_t>(header.toc_offset));
        auto const toc_count = static_cast<size_t>(static_cast<uint32_t>(header.toc_count));
        constexpr size_t toc_entry_len = DescriptorStorageTocEntry::LENGTH;
        if (!can_multiply(toc_count, toc_entry_len)) {
            return failure(BufferError::insufficient_data);
        }
        auto const toc_bytes = toc_count * toc_entry_len;
        if (!can_add(toc_offset, toc_bytes) || toc_offset + toc_bytes > blob.size()) {
            return failure(BufferError::insufficient_data);
        }

        auto const sym_offset = static_cast<size_t>(static_cast<uint32_t>(header.symbol_offset));
        auto const sym_count = static_cast<size_t>(static_cast<uint32_t>(header.symbol_count));
        constexpr size_t sym_entry_len = DescriptorStorageSymbolEntry::LENGTH;
        if (!can_multiply(sym_count, sym_entry_len)) {
            return failure(BufferError::insufficient_data);
        }
        auto const sym_bytes = sym_count * sym_entry_len;
        if (!can_add(sym_offset, sym_bytes) || sym_offset + sym_bytes > blob.size()) {
            return failure(BufferError::insufficient_data);
        }

        return success(DescriptorStorage{blob, header});
    }

    [[nodiscard]] auto get_descriptor(uint16_t configuration, uint16_t type, uint16_t index) const noexcept
        -> StatusValue<std::span<uint8_t const>>
    {
        auto const count = static_cast<uint32_t>(header_.toc_count);
        auto const base = static_cast<uint32_t>(header_.toc_offset);

        for (uint32_t i = 0; i < count; ++i) {
            DescriptorStorageTocEntry entry{};
            span_load(entry, blob_.subspan(base + (i * DescriptorStorageTocEntry::LENGTH)));

            if (static_cast<uint16_t>(entry.configuration_index) == configuration &&
                static_cast<uint16_t>(entry.descriptor_type) == type && static_cast<uint16_t>(entry.descriptor_index) == index) {
                auto const desc_offset = static_cast<size_t>(static_cast<uint32_t>(entry.offset));
                auto const desc_length = static_cast<size_t>(static_cast<uint16_t>(entry.length));

                if (!can_add(desc_offset, desc_length) || desc_offset + desc_length > blob_.size()) {
                    return failure(BufferError::insufficient_data);
                }
                return success(blob_.subspan(desc_offset, desc_length));
            }
        }
        return failure(BufferError::invalid_offset);
    }

    /// Call @p fn(configuration, type, index, bytes) for every descriptor, in
    /// table order (one with a bad offset or length is skipped).
    template <typename Fn>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward) - fn is called, not forwarded
    void for_each_descriptor(Fn&& fn) const
    {
        auto const count = static_cast<uint32_t>(header_.toc_count);
        auto const base = static_cast<uint32_t>(header_.toc_offset);
        for (uint32_t i = 0; i < count; ++i) {
            DescriptorStorageTocEntry entry{};
            span_load(entry, blob_.subspan(base + (i * DescriptorStorageTocEntry::LENGTH)));
            auto const offset = static_cast<size_t>(static_cast<uint32_t>(entry.offset));
            auto const length = static_cast<size_t>(static_cast<uint16_t>(entry.length));
            if (!can_add(offset, length) || offset + length > blob_.size()) {
                continue;
            }
            fn(static_cast<uint16_t>(entry.configuration_index),
               static_cast<uint16_t>(entry.descriptor_type),
               static_cast<uint16_t>(entry.descriptor_index),
               blob_.subspan(offset, length));
        }
    }

    /// Forward lookup: the well-known symbol assigned to a given descriptor, or
    /// failure if that descriptor has no symbol entry. The symbol decouples the
    /// designer-chosen descriptor index from code that handles the descriptor.
    [[nodiscard]] auto get_symbol(uint16_t configuration, uint16_t type, uint16_t index) const noexcept -> StatusValue<uint32_t>
    {
        auto const count = static_cast<uint32_t>(header_.symbol_count);
        auto const base = static_cast<uint32_t>(header_.symbol_offset);

        for (uint32_t i = 0; i < count; ++i) {
            DescriptorStorageSymbolEntry entry{};
            span_load(entry, blob_.subspan(base + (i * DescriptorStorageSymbolEntry::LENGTH)));

            if (static_cast<uint16_t>(entry.configuration_index) == configuration &&
                static_cast<uint16_t>(entry.descriptor_type) == type && static_cast<uint16_t>(entry.descriptor_index) == index) {
                return success(static_cast<uint32_t>(entry.symbol));
            }
        }
        return failure(BufferError::invalid_offset);
    }

    /// Reverse lookup: the descriptor (configuration/type/index) carrying a given
    /// well-known @p symbol, or failure if no symbol entry matches. The returned
    /// entry's fields are wire-typed (cast like get_symbol's callers). On duplicate
    /// symbols (a designer error) the first match wins. This is the seam that lets
    /// command-handling code reference a descriptor by stable symbol rather than by
    /// the blob's (designer-chosen, volatile) descriptor index.
    [[nodiscard]] auto find_by_symbol(uint32_t symbol) const noexcept -> StatusValue<DescriptorStorageSymbolEntry>
    {
        auto const count = static_cast<uint32_t>(header_.symbol_count);
        auto const base = static_cast<uint32_t>(header_.symbol_offset);

        for (uint32_t i = 0; i < count; ++i) {
            DescriptorStorageSymbolEntry entry{};
            span_load(entry, blob_.subspan(base + (i * DescriptorStorageSymbolEntry::LENGTH)));

            if (static_cast<uint32_t>(entry.symbol) == symbol) {
                return success(entry);
            }
        }
        return failure(BufferError::invalid_offset);
    }

    [[nodiscard]] auto get_configuration_count() const noexcept -> uint16_t
    {
        uint16_t max_config = 0;
        bool found = false;
        auto const count = static_cast<uint32_t>(header_.toc_count);
        auto const base = static_cast<uint32_t>(header_.toc_offset);

        for (uint32_t i = 0; i < count; ++i) {
            DescriptorStorageTocEntry entry{};
            span_load(entry, blob_.subspan(base + (i * DescriptorStorageTocEntry::LENGTH)));
            auto const cfg = static_cast<uint16_t>(entry.configuration_index);
            if (!found || cfg > max_config) {
                max_config = cfg;
                found = true;
            }
        }
        return found ? static_cast<uint16_t>(max_config + 1) : 0;
    }

    [[nodiscard]] auto blob() const noexcept -> std::span<uint8_t const> { return blob_; }

  private:
    explicit DescriptorStorage(std::span<uint8_t const> blob, DescriptorStorageHeader header)
        : blob_{blob}
        , header_{header}
    {}

    std::span<uint8_t const> blob_;
    DescriptorStorageHeader header_;
};

}  // namespace statusbar::atdecc::aem

// Wire serialization traits
template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorStorageHeader> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorStorageTocEntry> : std::true_type
{};

template <>
struct statusbar::traits::is_serializable_wire_fixed_struct<statusbar::atdecc::aem::DescriptorStorageSymbolEntry> : std::true_type
{};
