// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for AemModelStore: blob + symbol table persisted per
// fingerprint, regenerated only for unseen fingerprints, round-trippable.

#include "statusbar/nanoavb/nanoavb_aem_model_store.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <filesystem>

using namespace statusbar;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

namespace {

std::vector<AemRawDescriptor> tiny_model()
{
    DescriptorEntity e;
    DescriptorConfiguration cfg;
    std::vector<AemRawDescriptor> out;
    auto ev = make_const_span(e);
    out.push_back({0, DESCRIPTOR_ENTITY, 0, {ev.begin(), ev.end()}});
    auto cv = make_const_span(cfg).first(cfg.wire_size());
    out.push_back({0, DESCRIPTOR_CONFIGURATION, 0, {cv.begin(), cv.end()}});
    return out;
}

std::string temp_dir()
{
    auto dir = std::filesystem::temp_directory_path() / ("aem-store-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    return dir.string();
}

}  // namespace

TEST(aem_model_store, store_load_list_round_trip)
{
    auto dir = temp_dir();
    AemModelStore store(dir);
    EXPECT_EQ(store.list().size(), 0u);

    auto model = tiny_model();
    auto fp = store.store(model);
    EXPECT_TRUE(fp.has_value());
    EXPECT_TRUE(store.contains(*fp));
    EXPECT_EQ(store.list().size(), 1u);
    EXPECT_TRUE(store.list()[0] == *fp);

    // Storing the same model again is a no-op with the same key.
    auto fp2 = store.store(model);
    EXPECT_TRUE(fp2.has_value());
    EXPECT_TRUE(*fp2 == *fp);
    EXPECT_EQ(store.list().size(), 1u);

    // The blob reads back through the standard storage reader.
    auto blob = store.load_blob(*fp);
    EXPECT_TRUE(blob.has_value());
    auto storage = DescriptorStorage::create(*blob);
    EXPECT_TRUE(storage.has_value());
    EXPECT_TRUE(storage->get_descriptor(0, DESCRIPTOR_ENTITY, 0).has_value());

    // The stored symbol table reads back with its flags.
    auto symbols = store.load_symbols(*fp);
    EXPECT_TRUE(symbols.has_value());
    EXPECT_EQ(symbols->size(), model.size());
    bool saw_entity = false;
    for (auto const& s : *symbols) {
        if (s.descriptor_type == DESCRIPTOR_ENTITY) {
            saw_entity = true;
            EXPECT_TRUE(s.symbol == "entity");
            EXPECT_FALSE(s.orphan);
        }
    }
    EXPECT_TRUE(saw_entity);

    EXPECT_FALSE(store.load_blob("no-such-fingerprint").has_value());
    std::filesystem::remove_all(dir);
}

//
// Test Runner
//

TEST_MAIN(statusbar_nanoavb, nanoavb_aem_model_store_test)
