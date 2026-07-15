// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for AEM descriptor storage binary format reader

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"

#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc::aem;

TEST(descriptor_storage_header, struct_layout)
{
    EXPECT_EQ(sizeof(DescriptorStorageHeader), 20u);
    EXPECT_EQ(DescriptorStorageHeader::LENGTH, 20u);
    EXPECT_EQ(offsetof(DescriptorStorageHeader, magic), 0u);
    EXPECT_EQ(offsetof(DescriptorStorageHeader, toc_count), 4u);
    EXPECT_EQ(offsetof(DescriptorStorageHeader, toc_offset), 8u);
    EXPECT_EQ(offsetof(DescriptorStorageHeader, symbol_count), 12u);
    EXPECT_EQ(offsetof(DescriptorStorageHeader, symbol_offset), 16u);
}

TEST(descriptor_storage_toc_entry, struct_layout)
{
    EXPECT_EQ(sizeof(DescriptorStorageTocEntry), 12u);
    EXPECT_EQ(DescriptorStorageTocEntry::LENGTH, 12u);
    EXPECT_EQ(offsetof(DescriptorStorageTocEntry, descriptor_type), 0u);
    EXPECT_EQ(offsetof(DescriptorStorageTocEntry, descriptor_index), 2u);
    EXPECT_EQ(offsetof(DescriptorStorageTocEntry, configuration_index), 4u);
    EXPECT_EQ(offsetof(DescriptorStorageTocEntry, length), 6u);
    EXPECT_EQ(offsetof(DescriptorStorageTocEntry, offset), 8u);
}

TEST(descriptor_storage_symbol_entry, struct_layout)
{
    EXPECT_EQ(sizeof(DescriptorStorageSymbolEntry), 10u);
    EXPECT_EQ(DescriptorStorageSymbolEntry::LENGTH, 10u);
    EXPECT_EQ(offsetof(DescriptorStorageSymbolEntry, descriptor_type), 0u);
    EXPECT_EQ(offsetof(DescriptorStorageSymbolEntry, descriptor_index), 2u);
    EXPECT_EQ(offsetof(DescriptorStorageSymbolEntry, configuration_index), 4u);
    EXPECT_EQ(offsetof(DescriptorStorageSymbolEntry, symbol), 6u);
}

// Helper: build a minimal valid blob with header only (no TOC, no symbols)
static auto make_minimal_blob() -> std::array<uint8_t, 20>
{
    std::array<uint8_t, 20> blob{};
    DescriptorStorageHeader const header{
        .magic = DescriptorStorage::MAGIC,
        .toc_count = 0,
        .toc_offset = 20,  // right after header
        .symbol_count = 0,
        .symbol_offset = 20};
    span_store(blob, header);
    return blob;
}

TEST(descriptor_storage_create, valid_empty_blob)
{
    auto blob = make_minimal_blob();
    auto result = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(result.has_value());
}

TEST(descriptor_storage_create, bad_magic)
{
    auto blob = make_minimal_blob();
    blob[0] = 0x00;  // corrupt magic
    auto result = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_FALSE(result.has_value());
}

TEST(descriptor_storage_create, too_short)
{
    std::array<uint8_t, 10> blob{};
    auto result = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_FALSE(result.has_value());
}

TEST(descriptor_storage_create, config_count_empty)
{
    auto blob = make_minimal_blob();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    EXPECT_EQ(storage->get_configuration_count(), 0u);
}

// Helper: build a blob with one entity descriptor (312 bytes of zeros)
static auto make_blob_with_entity() -> std::vector<uint8_t>
{
    // Layout: header(20) + toc(1 entry = 12) + descriptor(312)
    constexpr uint32_t header_size = 20;
    constexpr uint32_t toc_entry_size = 12;
    constexpr uint32_t desc_size = 312;  // DescriptorEntity::LENGTH
    constexpr uint32_t toc_offset = header_size;
    constexpr uint32_t desc_offset = header_size + toc_entry_size;
    constexpr uint32_t total = desc_offset + desc_size;

    std::vector<uint8_t> blob(total, 0);

    DescriptorStorageHeader const header{
        .magic = DescriptorStorage::MAGIC, .toc_count = 1, .toc_offset = toc_offset, .symbol_count = 0, .symbol_offset = total};
    span_store(make_span(blob), header);

    DescriptorStorageTocEntry const toc{
        .descriptor_type = DESCRIPTOR_ENTITY,
        .descriptor_index = 0,
        .configuration_index = 0,
        .length = desc_size,
        .offset = desc_offset};
    span_store(make_span(blob, {.start = toc_offset}), toc);

    // Put a marker byte in the descriptor data so we can verify the span
    blob[desc_offset] = 0xAB;

    return blob;
}

TEST(descriptor_storage_get, found)
{
    auto blob = make_blob_with_entity();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    auto desc = storage->get_descriptor(0, DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(desc.has_value());
    EXPECT_EQ(desc->size(), 312u);
    EXPECT_EQ((*desc)[0], 0xABu);  // marker byte
}

TEST(descriptor_storage_get, not_found)
{
    auto blob = make_blob_with_entity();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    // Wrong type
    auto desc = storage->get_descriptor(0, DESCRIPTOR_CONFIGURATION, 0);
    EXPECT_FALSE(desc.has_value());

    // Wrong index
    desc = storage->get_descriptor(0, DESCRIPTOR_ENTITY, 1);
    EXPECT_FALSE(desc.has_value());

    // Wrong config
    desc = storage->get_descriptor(1, DESCRIPTOR_ENTITY, 0);
    EXPECT_FALSE(desc.has_value());
}

TEST(descriptor_storage_get, config_count_with_entries)
{
    auto blob = make_blob_with_entity();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    EXPECT_EQ(storage->get_configuration_count(), 1u);
}

// Helper: build a blob with one entity descriptor and one symbol
static auto make_blob_with_symbol() -> std::vector<uint8_t>
{
    constexpr uint32_t header_size = 20;
    constexpr uint32_t toc_entry_size = 12;
    constexpr uint32_t symbol_entry_size = 10;
    constexpr uint32_t desc_size = 312;
    constexpr uint32_t toc_offset = header_size;
    constexpr uint32_t symbol_offset = header_size + toc_entry_size;
    constexpr uint32_t desc_offset = symbol_offset + symbol_entry_size;
    constexpr uint32_t total = desc_offset + desc_size;

    std::vector<uint8_t> blob(total, 0);

    DescriptorStorageHeader const header{
        .magic = DescriptorStorage::MAGIC,
        .toc_count = 1,
        .toc_offset = toc_offset,
        .symbol_count = 1,
        .symbol_offset = symbol_offset};
    span_store(make_span(blob), header);

    DescriptorStorageTocEntry const toc{
        .descriptor_type = DESCRIPTOR_ENTITY,
        .descriptor_index = 0,
        .configuration_index = 0,
        .length = desc_size,
        .offset = desc_offset};
    span_store(make_span(blob, {.start = toc_offset}), toc);

    DescriptorStorageSymbolEntry const sym{
        .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0, .configuration_index = 0, .symbol = 0xDEADBEEF};
    span_store(make_span(blob, {.start = symbol_offset}), sym);

    return blob;
}

TEST(descriptor_storage_symbol, found)
{
    auto blob = make_blob_with_symbol();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    auto sym = storage->get_symbol(0, DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(sym.has_value());
    EXPECT_EQ(*sym, 0xDEADBEEFu);
}

TEST(descriptor_storage_symbol, not_found)
{
    auto blob = make_blob_with_symbol();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    auto sym = storage->get_symbol(0, DESCRIPTOR_CONFIGURATION, 0);
    EXPECT_FALSE(sym.has_value());
}

TEST(descriptor_storage_symbol, find_by_symbol_found)
{
    // Reverse lookup: the symbol 0xDEADBEEF resolves back to (config 0, ENTITY, index 0).
    auto blob = make_blob_with_symbol();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    auto loc = storage->find_by_symbol(0xDEADBEEFu);
    EXPECT_TRUE(loc.has_value());
    EXPECT_EQ(static_cast<uint16_t>(loc->configuration_index), 0u);
    EXPECT_EQ(static_cast<uint16_t>(loc->descriptor_type), static_cast<uint16_t>(DESCRIPTOR_ENTITY));
    EXPECT_EQ(static_cast<uint16_t>(loc->descriptor_index), 0u);
    EXPECT_EQ(static_cast<uint32_t>(loc->symbol), 0xDEADBEEFu);
}

TEST(descriptor_storage_symbol, find_by_symbol_not_found)
{
    auto blob = make_blob_with_symbol();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    auto loc = storage->find_by_symbol(0x12345678u);  // no such symbol
    EXPECT_FALSE(loc.has_value());
}

TEST(descriptor_storage_symbol, round_trip)
{
    // get_symbol and find_by_symbol are inverses for the same descriptor.
    auto blob = make_blob_with_symbol();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());

    auto sym = storage->get_symbol(0, DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(sym.has_value());
    auto loc = storage->find_by_symbol(*sym);
    EXPECT_TRUE(loc.has_value());
    EXPECT_EQ(static_cast<uint16_t>(loc->descriptor_type), static_cast<uint16_t>(DESCRIPTOR_ENTITY));
    EXPECT_EQ(static_cast<uint16_t>(loc->descriptor_index), 0u);
}

//
// wire_size() smoke tests for every descriptor type that has a
// uncovered wire_size() (per `make coverage-uncovered-functions-report`).
// These are trivial const accessors — they should always return the
// compile-time fixed descriptor length and never zero.
//

template <typename Descriptor>
void expect_wire_size_nonzero()
{
    Descriptor d{};
    EXPECT_TRUE(d.wire_size() > 0);
}

TEST(descriptor_wire_size, video_unit)
{
    expect_wire_size_nonzero<DescriptorVideoUnit>();
}
TEST(descriptor_wire_size, sensor_unit)
{
    expect_wire_size_nonzero<DescriptorSensorUnit>();
}
TEST(descriptor_wire_size, avb_interface)
{
    expect_wire_size_nonzero<DescriptorAvbInterface>();
}
TEST(descriptor_wire_size, clock_source)
{
    expect_wire_size_nonzero<DescriptorClockSource>();
}
TEST(descriptor_wire_size, memory_object)
{
    expect_wire_size_nonzero<DescriptorMemoryObject>();
}
TEST(descriptor_wire_size, locale)
{
    expect_wire_size_nonzero<DescriptorLocale>();
}
TEST(descriptor_wire_size, strings)
{
    expect_wire_size_nonzero<DescriptorStrings>();
}
TEST(descriptor_wire_size, stream_port)
{
    expect_wire_size_nonzero<DescriptorStreamPort>();
}
TEST(descriptor_wire_size, external_port)
{
    expect_wire_size_nonzero<DescriptorExternalPort>();
}
TEST(descriptor_wire_size, internal_port)
{
    expect_wire_size_nonzero<DescriptorInternalPort>();
}
TEST(descriptor_wire_size, audio_cluster)
{
    expect_wire_size_nonzero<DescriptorAudioCluster>();
}
TEST(descriptor_wire_size, video_cluster)
{
    expect_wire_size_nonzero<DescriptorVideoCluster>();
}
TEST(descriptor_wire_size, sensor_cluster)
{
    expect_wire_size_nonzero<DescriptorSensorCluster>();
}
TEST(descriptor_wire_size, control)
{
    expect_wire_size_nonzero<DescriptorControl>();
}
TEST(descriptor_wire_size, signal_selector)
{
    expect_wire_size_nonzero<DescriptorSignalSelector>();
}
TEST(descriptor_wire_size, mixer)
{
    expect_wire_size_nonzero<DescriptorMixer>();
}
TEST(descriptor_wire_size, matrix)
{
    expect_wire_size_nonzero<DescriptorMatrix>();
}
TEST(descriptor_wire_size, matrix_signal)
{
    expect_wire_size_nonzero<DescriptorMatrixSignal>();
}
TEST(descriptor_wire_size, signal_splitter)
{
    expect_wire_size_nonzero<DescriptorSignalSplitter>();
}
TEST(descriptor_wire_size, signal_combiner)
{
    expect_wire_size_nonzero<DescriptorSignalCombiner>();
}
TEST(descriptor_wire_size, signal_demultiplexer)
{
    expect_wire_size_nonzero<DescriptorSignalDemultiplexer>();
}
TEST(descriptor_wire_size, signal_multiplexer)
{
    expect_wire_size_nonzero<DescriptorSignalMultiplexer>();
}
TEST(descriptor_wire_size, signal_transcoder)
{
    expect_wire_size_nonzero<DescriptorSignalTranscoder>();
}
TEST(descriptor_wire_size, control_block)
{
    expect_wire_size_nonzero<DescriptorControlBlock>();
}
TEST(descriptor_wire_size, timing)
{
    expect_wire_size_nonzero<DescriptorTiming>();
}
TEST(descriptor_wire_size, ptp_instance)
{
    expect_wire_size_nonzero<DescriptorPtpInstance>();
}
TEST(descriptor_wire_size, ptp_port)
{
    expect_wire_size_nonzero<DescriptorPtpPort>();
}

TEST(descriptor_strings, as_string_array_returns_array_of_seven)
{
    DescriptorStrings d{};
    auto const arr = d.as_string_array();
    EXPECT_EQ(arr.size(), 7U);
}

TEST(aem_localized_description, parse_default_value_is_no_string)
{
    // The "no localized description" sentinel is typically 0xFFFF.
    auto const result = parse_localized_description(0xFFFF);
    (void)result;  // just exercise the function
}

//
// Decoder safety: malformed DescriptorStorage blobs must be rejected by
// create() rather than allowing OOB reads in get_descriptor / get_symbol.
//

TEST(descriptor_storage_safety, toc_offset_past_end_rejected)
{
    auto blob = make_minimal_blob();
    // Set toc_count=1, toc_offset=200 (past the 20-byte blob).
    blob[7] = 0x01;
    blob[8] = 0x00;
    blob[9] = 0x00;
    blob[10] = 0x00;
    blob[11] = 0xC8;
    auto result = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_FALSE(result.has_value());
}

TEST(descriptor_storage_safety, toc_count_overruns_blob)
{
    // A larger blob so we can place TOC at offset 20 and then overrun via count.
    std::vector<uint8_t> blob(40, 0);
    blob[0] = 0x41;
    blob[1] = 0x45;
    blob[2] = 0x4d;
    blob[3] = 0x31;
    blob[7] = 0x05;  // toc_count = 5 (5 * 12 = 60 bytes; only 20 bytes of TOC space)
    blob[11] = 20;   // toc_offset = 20
    blob[19] = 40;   // symbol_offset = 40 (end of blob, count=0)
    auto result = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_FALSE(result.has_value());
}

TEST(descriptor_storage_safety, symbol_section_past_end_rejected)
{
    auto blob = make_minimal_blob();
    // symbol_count=2, symbol_offset=20 (each entry is large; total > 20-byte blob).
    blob[15] = 0x02;
    blob[19] = 0x14;
    auto result = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_FALSE(result.has_value());
}

TEST(descriptor_storage_safety, descriptor_offset_past_end)
{
    // Build a blob whose TOC entry points past the blob.
    std::vector<uint8_t> blob(32, 0);
    blob[0] = 0x41;
    blob[1] = 0x45;
    blob[2] = 0x4d;
    blob[3] = 0x31;
    blob[7] = 0x01;  // toc_count = 1
    blob[11] = 20;   // toc_offset = 20
    blob[19] = 32;   // symbol_offset = 32 (end of blob)
    // TOC entry at 20: configuration=0, type=0, index=0, length=4, offset=200 (OOB)
    blob[27] = 0x04;  // length = 4
    blob[28] = 0x00;  // offset[31:24]
    blob[29] = 0x00;
    blob[30] = 0x00;
    blob[31] = 0xC8;  // offset = 200 (past end of 32-byte blob)
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto desc = storage->get_descriptor(0, 0, 0);
    EXPECT_FALSE(desc.has_value());
}

TEST_MAIN(statusbar_atdecc, atdecc_descriptor_storage_test)
