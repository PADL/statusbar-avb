// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"

namespace statusbar::nanoavb {

using atdecc::aem::DescriptorStorageHeader;
using atdecc::aem::DescriptorStorageSymbolEntry;
using atdecc::aem::DescriptorStorageTocEntry;

void AemBlobWriter::add(
    std::uint16_t configuration, std::uint16_t descriptor_type, std::uint16_t descriptor_index, std::span<std::uint8_t const> bytes)
{
    entries_.push_back({configuration, descriptor_type, descriptor_index, {bytes.begin(), bytes.end()}});
}

void AemBlobWriter::add_symbol(
    std::uint16_t configuration, std::uint16_t descriptor_type, std::uint16_t descriptor_index, std::uint32_t symbol)
{
    symbols_.push_back({configuration, descriptor_type, descriptor_index, symbol});
}

std::vector<std::uint8_t> AemBlobWriter::write() const
{
    auto append_struct = [](std::vector<std::uint8_t>& out, auto const& pod) {
        auto view = make_const_span(pod);
        out.insert(out.end(), view.begin(), view.end());
    };

    auto const toc_offset = std::uint32_t(DescriptorStorageHeader::LENGTH);
    auto const symbol_offset = std::uint32_t(toc_offset + entries_.size() * DescriptorStorageTocEntry::LENGTH);
    auto const data_offset = std::uint32_t(symbol_offset + symbols_.size() * DescriptorStorageSymbolEntry::LENGTH);

    std::vector<std::uint8_t> out;
    std::size_t total = data_offset;
    for (auto const& e : entries_) {
        total += e.bytes.size();
    }
    out.reserve(total);

    DescriptorStorageHeader header;
    header.magic = atdecc::aem::DescriptorStorage::MAGIC;
    header.toc_count = std::uint32_t(entries_.size());
    header.toc_offset = toc_offset;
    header.symbol_count = std::uint32_t(symbols_.size());
    header.symbol_offset = symbol_offset;
    append_struct(out, header);

    auto offset = data_offset;
    for (auto const& e : entries_) {
        DescriptorStorageTocEntry toc;
        toc.descriptor_type = e.type;
        toc.descriptor_index = e.index;
        toc.configuration_index = e.configuration;
        toc.length = std::uint16_t(e.bytes.size());
        toc.offset = offset;
        append_struct(out, toc);
        offset += std::uint32_t(e.bytes.size());
    }
    for (auto const& s : symbols_) {
        DescriptorStorageSymbolEntry entry;
        entry.descriptor_type = s.type;
        entry.descriptor_index = s.index;
        entry.configuration_index = s.configuration;
        entry.symbol = s.symbol;
        append_struct(out, entry);
    }
    for (auto const& e : entries_) {
        out.insert(out.end(), e.bytes.begin(), e.bytes.end());
    }
    return out;
}

}  // namespace statusbar::nanoavb
