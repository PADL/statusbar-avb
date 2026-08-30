#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <span>
#include <vector>

// Writer for the `.aem` descriptor-storage blob format (the jdksavdecc
// AEM1 layout that statusbar-avb's DescriptorStorage reads zero-copy):
//
//   [20-byte header: magic 'AEM1', toc count/offset, symbol count/offset]
//   [TOC: N x 12-byte (type, index, config, length, offset)]
//   [symbols: M x 10-byte (type, index, config, symbol)]
//   [raw descriptor bytes]
//
// Descriptors are stored as raw READ_DESCRIPTOR response payloads
// (starting at descriptor_type), variable trailers included — the format
// the standard itself defines, byte-verifiable against a live device
//.

namespace statusbar::nanoavb {

class AemBlobWriter
{
  public:
    /// Appends one descriptor's wire bytes. Order is preserved in the TOC.
    void add(
        std::uint16_t configuration,
        std::uint16_t descriptor_type,
        std::uint16_t descriptor_index,
        std::span<std::uint8_t const> bytes);

    /// Appends one symbol-table entry.
    void add_symbol(
        std::uint16_t configuration, std::uint16_t descriptor_type, std::uint16_t descriptor_index, std::uint32_t symbol);

    std::size_t descriptor_count() const noexcept { return entries_.size(); }

    /// Serializes the blob image.
    std::vector<std::uint8_t> write() const;

  private:
    struct Entry
    {
        std::uint16_t configuration;
        std::uint16_t type;
        std::uint16_t index;
        std::vector<std::uint8_t> bytes;
    };
    struct Symbol
    {
        std::uint16_t configuration;
        std::uint16_t type;
        std::uint16_t index;
        std::uint32_t symbol;
    };

    std::vector<Entry> entries_;
    std::vector<Symbol> symbols_;
};

}  // namespace statusbar::nanoavb
