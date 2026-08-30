// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for AemBlobWriter: the written image must read back through
// statusbar-avb's DescriptorStorage zero-copy reader.

#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"

#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;

TEST(aem_blob_writer, round_trips_through_descriptor_storage)
{
    AemBlobWriter w;
    std::vector<std::uint8_t> entity{0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};
    std::vector<std::uint8_t> control{0x00, 0x1A, 0x00, 0x07, 0x01, 0x02, 0x03};
    w.add(0, 0x0000, 0, entity);
    w.add(0, 0x001A, 7, control);
    w.add_symbol(0, 0x001A, 7, 0x12345678);
    EXPECT_EQ(w.descriptor_count(), 2u);

    auto blob = w.write();
    auto storage = atdecc::aem::DescriptorStorage::create(blob);
    EXPECT_TRUE(storage.has_value());

    auto e = storage->get_descriptor(0, 0x0000, 0);
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e->size(), entity.size());
    EXPECT_EQ((*e)[4], 0xAA);

    auto c = storage->get_descriptor(0, 0x001A, 7);
    EXPECT_TRUE(c.has_value());
    EXPECT_EQ(c->size(), control.size());
    EXPECT_EQ((*c)[6], 0x03);

    // Absent descriptors are absent.
    EXPECT_FALSE(storage->get_descriptor(0, 0x001A, 8).has_value());
    EXPECT_FALSE(storage->get_descriptor(1, 0x0000, 0).has_value());
}

TEST(aem_blob_writer, empty_blob_is_valid)
{
    AemBlobWriter w;
    auto blob = w.write();
    EXPECT_EQ(blob.size(), 20u);
    auto storage = atdecc::aem::DescriptorStorage::create(blob);
    EXPECT_TRUE(storage.has_value());
}

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_aem_blob_writer_test)
