// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for AvbEntityHost's symbol-aware construction path: building the host from
// a DescriptorStorageHandler (instead of a parsed EntityModel) and the
// symbol <-> descriptor lookups it then exposes. The legacy parsed-model path
// returns empty/failure from those lookups.

#include "statusbar/avb_entity/avb_entity_host.hpp"

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using statusbar::atdecc::aem::DESCRIPTOR_ENTITY;
using statusbar::atdecc::aem::DescriptorEntity;
using statusbar::atdecc::aem::DescriptorStorage;
using statusbar::avb_entity::AvbEntityHost;
using statusbar::nanoavb::AdpAdvertiserConfig;
using statusbar::nanoavb::DescriptorStorageHandler;
using statusbar::nanoavb::EntityModel;

namespace {

/// Minimal .aem blob: one ENTITY descriptor (config 0, index 0) plus one symbol
/// (0xC0FFEE) bound to it. (Same byte layout as nanoavb's storage tests.)
auto make_entity_blob() -> std::vector<uint8_t>
{
    constexpr uint32_t header_size = 20;
    constexpr uint32_t toc_entry_size = 12;
    constexpr uint32_t symbol_entry_size = 10;
    constexpr uint32_t desc_size = DescriptorEntity::LENGTH;  // 312
    constexpr uint32_t toc_offset = header_size;
    constexpr uint32_t symbol_offset = toc_offset + toc_entry_size;
    constexpr uint32_t desc_offset = symbol_offset + symbol_entry_size;
    constexpr uint32_t total = desc_offset + desc_size;

    std::vector<uint8_t> blob(total, 0);
    blob[0] = 0x41;  // "AEM1"
    blob[1] = 0x45;
    blob[2] = 0x4D;
    blob[3] = 0x31;
    blob[7] = 0x01;                                  // toc_count = 1
    blob[11] = static_cast<uint8_t>(toc_offset);     // toc_offset = 20
    blob[15] = 0x01;                                 // symbol_count = 1
    blob[19] = static_cast<uint8_t>(symbol_offset);  // symbol_offset = 32

    // TOC entry: type=ENTITY(0), index=0, config=0, length=312, offset=desc_offset
    blob[toc_offset + 6] = static_cast<uint8_t>((desc_size >> 8) & 0xFF);
    blob[toc_offset + 7] = static_cast<uint8_t>(desc_size & 0xFF);
    blob[toc_offset + 11] = static_cast<uint8_t>(desc_offset);

    // Symbol entry: type=ENTITY, index=0, config=0, symbol=0x00C0FFEE
    blob[symbol_offset + 7] = 0xC0;
    blob[symbol_offset + 8] = 0xFF;
    blob[symbol_offset + 9] = 0xEE;

    // A real DescriptorEntity in the slot.
    DescriptorEntity desc{};
    desc.configurations_count = 1;
    statusbar::span_store(statusbar::make_span(blob, {.start = desc_offset}), desc);
    return blob;
}

auto make_host_from_blob(std::vector<uint8_t> const& blob) -> std::unique_ptr<AvbEntityHost>
{
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    auto handler = std::make_unique<DescriptorStorageHandler>(*storage);
    return std::make_unique<AvbEntityHost>(std::move(handler), AdpAdvertiserConfig{}, 1, 4, 1);
}

/// Blob with an ENTITY descriptor plus two CONTROL descriptors: index 0 a MUTE,
/// index 1 the standard IDENTIFY — so the identify scan must pass over a
/// non-identify control and land on index 1.
auto make_blob_with_identify_control() -> std::vector<uint8_t>
{
    using statusbar::atdecc::aem::DESCRIPTOR_CONTROL;
    using statusbar::atdecc::aem::DescriptorControl;

    constexpr uint32_t header_size = 20;
    constexpr uint32_t toc_entry_size = 12;
    constexpr uint32_t entity_size = DescriptorEntity::LENGTH;
    constexpr uint32_t control_size = DescriptorControl::LENGTH;
    constexpr uint32_t toc_offset = header_size;
    constexpr uint32_t entity_offset = toc_offset + (3 * toc_entry_size);
    constexpr uint32_t control0_offset = entity_offset + entity_size;
    constexpr uint32_t control1_offset = control0_offset + control_size;
    constexpr uint32_t total = control1_offset + control_size;

    std::vector<uint8_t> blob(total, 0);
    blob[0] = 0x41;  // "AEM1"
    blob[1] = 0x45;
    blob[2] = 0x4D;
    blob[3] = 0x31;
    blob[7] = 0x03;                               // toc_count = 3
    blob[11] = static_cast<uint8_t>(toc_offset);  // toc_offset = 20 (no symbols)

    // TOC entry: type(2) index(2) config(2) length(2) offset(4), big-endian.
    auto put_toc = [&](uint32_t slot, uint16_t type, uint16_t index, uint16_t length, uint32_t offset) {
        auto const base = toc_offset + (slot * toc_entry_size);
        blob[base + 0] = static_cast<uint8_t>(type >> 8);
        blob[base + 1] = static_cast<uint8_t>(type & 0xFF);
        blob[base + 2] = static_cast<uint8_t>(index >> 8);
        blob[base + 3] = static_cast<uint8_t>(index & 0xFF);
        blob[base + 6] = static_cast<uint8_t>(length >> 8);
        blob[base + 7] = static_cast<uint8_t>(length & 0xFF);
        blob[base + 8] = static_cast<uint8_t>(offset >> 24);
        blob[base + 9] = static_cast<uint8_t>((offset >> 16) & 0xFF);
        blob[base + 10] = static_cast<uint8_t>((offset >> 8) & 0xFF);
        blob[base + 11] = static_cast<uint8_t>(offset & 0xFF);
    };
    put_toc(0, DESCRIPTOR_ENTITY, 0, entity_size, entity_offset);
    put_toc(1, DESCRIPTOR_CONTROL, 0, control_size, control0_offset);
    put_toc(2, DESCRIPTOR_CONTROL, 1, control_size, control1_offset);

    DescriptorEntity entity{};
    entity.configurations_count = 1;
    statusbar::span_store(statusbar::make_span(blob, {.start = entity_offset}), entity);

    DescriptorControl mute{};
    mute.descriptor_index = 0;
    mute.control_type = statusbar::atdecc::aem::CONTROL_TYPE_MUTE;
    // A realistic one-value LINEAR_UINT8 control (a default-constructed
    // number_of_values = 0 would make every SET payload size-invalid).
    mute.control_value_type = 0x0001;  // CONTROL_LINEAR_UINT8
    mute.number_of_values = 1;
    // Blob slots hold the 104-byte wire header only; span_copy truncates the
    // 508-byte in-memory struct to the destination range.
    statusbar::span_copy(
        statusbar::make_span(blob, {.start = control0_offset, .length = control_size}), statusbar::make_const_span(mute));

    DescriptorControl identify{};
    identify.descriptor_index = 1;
    identify.control_type = statusbar::atdecc::aem::CONTROL_TYPE_IDENTIFY;
    identify.control_value_type = 0x0001;  // CONTROL_LINEAR_UINT8 (the standard identify shape)
    identify.number_of_values = 1;
    statusbar::span_copy(
        statusbar::make_span(blob, {.start = control1_offset, .length = control_size}), statusbar::make_const_span(identify));
    return blob;
}

}  // namespace

TEST(avb_entity_host_symbol, constructs_symbol_aware_and_resolves_forward)
{
    auto blob = make_entity_blob();
    auto host = make_host_from_blob(blob);

    auto sym = host->symbol_of(DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(sym.has_value());
    EXPECT_EQ(*sym, 0x00C0FFEEu);
}

TEST(avb_entity_host_symbol, resolves_reverse)
{
    auto blob = make_entity_blob();
    auto host = make_host_from_blob(blob);

    auto loc = host->descriptor_for_symbol(0x00C0FFEEu);
    EXPECT_TRUE(loc.has_value());
    EXPECT_EQ(static_cast<uint16_t>(loc->descriptor_type), static_cast<uint16_t>(DESCRIPTOR_ENTITY));
    EXPECT_EQ(static_cast<uint16_t>(loc->descriptor_index), 0u);
}

TEST(avb_entity_host_symbol, get_descriptor_returns_blob_bytes)
{
    auto blob = make_entity_blob();
    auto host = make_host_from_blob(blob);

    auto desc = host->get_descriptor(DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(desc.has_value());
    EXPECT_EQ(desc->size(), static_cast<size_t>(DescriptorEntity::LENGTH));
}

TEST(avb_entity_host_symbol, unknown_symbol_and_descriptor_fail)
{
    auto blob = make_entity_blob();
    auto host = make_host_from_blob(blob);

    EXPECT_FALSE(host->symbol_of(DESCRIPTOR_ENTITY, 7).has_value());  // no such index
    EXPECT_FALSE(host->descriptor_for_symbol(0x12345678u).has_value());
    EXPECT_FALSE(host->get_descriptor(DESCRIPTOR_ENTITY, 7).has_value());
}

TEST(avb_entity_host_symbol, identify_control_wired_from_blob)
{
    // The host scans configuration 0's CONTROL descriptors for the standard
    // IDENTIFY control_type and mirrors its index into the ADP advertisement.
    auto blob = make_blob_with_identify_control();
    auto host = make_host_from_blob(blob);

    auto const& adpdu = host->components().adp_advertiser.adpdu();
    EXPECT_EQ(static_cast<uint16_t>(adpdu.identify_control_index), 1u);
    EXPECT_TRUE(adpdu.entity_capabilities.has_flag(statusbar::atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID));
}

TEST(avb_entity_host_symbol, no_identify_control_leaves_adp_untouched)
{
    // A blob without an IDENTIFY-typed control must not set the capability.
    auto blob = make_entity_blob();
    auto host = make_host_from_blob(blob);

    auto const& adpdu = host->components().adp_advertiser.adpdu();
    EXPECT_FALSE(adpdu.entity_capabilities.has_flag(statusbar::atdecc::entity_capabilities::AEM_IDENTIFY_CONTROL_INDEX_VALID));
}

TEST(avb_entity_host_symbol, identify_control_value_set_get)
{
    // The storage handler itself accepts SET_CONTROL for the blob's IDENTIFY
    // control and serves the stored value back on GET_CONTROL; since kit
    // phase 4 EVERY blob CONTROL gets the same treatment (generic value
    // store), so the mute control accepts a size-valid SET too.
    auto blob = make_blob_with_identify_control();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    DescriptorStorageHandler handler{*storage};

    using statusbar::atdecc::AEM_COMMAND_GET_CONTROL;
    using statusbar::atdecc::AEM_COMMAND_SET_CONTROL;
    using statusbar::atdecc::AEM_STATUS_NOT_IMPLEMENTED;
    using statusbar::atdecc::AEM_STATUS_SUCCESS;
    using statusbar::atdecc::aem::DESCRIPTOR_CONTROL;

    statusbar::nanoavb::DescriptorId const identify{
        .ref = {.configuration_index = 0, .descriptor_type = DESCRIPTOR_CONTROL, .descriptor_index = 1}, .symbol = 0};
    statusbar::nanoavb::DescriptorId const mute{
        .ref = {.configuration_index = 0, .descriptor_type = DESCRIPTOR_CONTROL, .descriptor_index = 0}, .symbol = 0};

    std::array<uint8_t, 1> const on{0xFF};
    EXPECT_EQ(handler.on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, identify, on), AEM_STATUS_SUCCESS);
    EXPECT_EQ(handler.identify_value(), 0xFF);
    std::array<uint8_t, 8> out{};
    EXPECT_EQ(handler.on_get_descriptor_value(AEM_COMMAND_GET_CONTROL, identify, {}, out), 1u);
    EXPECT_EQ(out[0], 0xFF);

    // Kit phase 4: the generic CONTROL built-in accepts the (size-valid) mute
    // value and serves it back — a JSON-authored control no longer answers
    // NOT_IMPLEMENTED with zero per-entity code.
    EXPECT_EQ(handler.on_set_descriptor_value(AEM_COMMAND_SET_CONTROL, mute, on), AEM_STATUS_SUCCESS);
    std::array<uint8_t, 8> mute_out{};
    EXPECT_EQ(handler.on_get_descriptor_value(AEM_COMMAND_GET_CONTROL, mute, {}, mute_out), 1u);
    EXPECT_EQ(mute_out[0], 0xFF);
}

TEST(avb_entity_host_symbol, local_identify_trigger)
{
    // The host-level GPIO/front-panel path: with an identify control in the
    // blob, set_local_identify applies the value through the storage handler
    // and reports success; without one it reports failure.
    auto blob = make_blob_with_identify_control();
    auto host = make_host_from_blob(blob);
    EXPECT_TRUE(host->set_local_identify(true));
    EXPECT_TRUE(host->set_local_identify(false));

    auto plain = make_entity_blob();
    auto no_identify = make_host_from_blob(plain);
    EXPECT_FALSE(no_identify->set_local_identify(true));
}

TEST(avb_entity_host_symbol, legacy_parsed_model_path_has_no_storage)
{
    // The legacy (EntityModel) ctor leaves the host with no backing blob, so the
    // symbol lookups return empty/failure.
    AvbEntityHost host{EntityModel{}, AdpAdvertiserConfig{}, 1, 4, 1};
    EXPECT_EQ(host.descriptor_storage(), nullptr);
    EXPECT_FALSE(host.symbol_of(DESCRIPTOR_ENTITY, 0).has_value());
    EXPECT_FALSE(host.descriptor_for_symbol(0x00C0FFEEu).has_value());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_host_test)
