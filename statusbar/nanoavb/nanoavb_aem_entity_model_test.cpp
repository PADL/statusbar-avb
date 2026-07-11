// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for AemEntityHandler + AemEntityModel (Phase 3 refactor).
// Exercises the handler-backed dispatch path: a trivial handler that
// implements only a few descriptor types, and verifies that the model
// routes READ_DESCRIPTOR / GET_NAME / SET_NAME correctly.

#include "statusbar/nanoavb/nanoavb_aem_entity_model.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using statusbar::atdecc::AEM_STATUS_BAD_ARGUMENTS;
using statusbar::atdecc::AEM_STATUS_NOT_IMPLEMENTED;
using statusbar::atdecc::AEM_STATUS_SUCCESS;
using statusbar::atdecc::MAX_AEM_DESCRIPTOR_SIZE;
using statusbar::atdecc::aem::DESCRIPTOR_AUDIO_MAP;
using statusbar::atdecc::aem::DESCRIPTOR_CONFIGURATION;
using statusbar::atdecc::aem::DESCRIPTOR_ENTITY;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_OUTPUT;
using statusbar::atdecc::aem::DescriptorAudioMap;
using statusbar::atdecc::aem::DescriptorConfiguration;
using statusbar::atdecc::aem::DescriptorEntity;
using statusbar::atdecc::aem::DescriptorRef;
using statusbar::atdecc::aem::DescriptorStream;
using statusbar::atdecc::aem::NameRef;

namespace {

/// A minimal test handler that implements a handful of descriptor
/// types and records some state so the tests can observe it.
class TestHandler : public AemEntityHandler
{
  public:
    auto on_get_entity(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorEntity& desc) -> bool override
    {
        desc.entity_name = AtdeccString{"TestEntity"};
        desc.configurations_count = 1;
        desc.current_configuration = 0;
        return true;
    }

    auto on_get_configuration(DescriptorRef ref, uint32_t /*symbol*/, DescriptorConfiguration& desc) -> bool override
    {
        if (ref.descriptor_index != 0) {
            return false;
        }
        desc.object_name = AtdeccString{"Config0"};
        desc.descriptor_counts_count = 2;
        desc.descriptor_counts[0].descriptor_type = DESCRIPTOR_STREAM_INPUT;
        desc.descriptor_counts[0].count = 1;
        desc.descriptor_counts[1].descriptor_type = DESCRIPTOR_STREAM_OUTPUT;
        desc.descriptor_counts[1].count = 1;
        return true;
    }

    auto on_get_stream(DescriptorRef ref, uint32_t /*symbol*/, DescriptorStream& desc) -> bool override
    {
        // Distinguish direction by ref.descriptor_type, same handler method.
        if (ref.descriptor_index != 0) {
            return false;
        }
        if (ref.descriptor_type == DESCRIPTOR_STREAM_INPUT) {
            desc.object_name = AtdeccString{"StreamIn0"};
        } else if (ref.descriptor_type == DESCRIPTOR_STREAM_OUTPUT) {
            desc.object_name = AtdeccString{"StreamOut0"};
        } else {
            return false;
        }
        desc.number_of_formats = 1;
        desc.stream_formats[0] = ieee::Eui64{0x00, 0xA0, 0x20, 0x41, 0x00, 0x00, 0x00, 0x00};
        return true;
    }

    auto on_get_audio_map(DescriptorRef ref, uint32_t /*symbol*/, DescriptorAudioMap& desc) -> bool override
    {
        if (ref.descriptor_index != 0) {
            return false;
        }
        desc.number_of_mappings = 2;
        desc.mappings[0].mapping_stream_channel = 0;
        desc.mappings[0].mapping_cluster_channel = 1;
        desc.mappings[1].mapping_stream_channel = 1;
        desc.mappings[1].mapping_cluster_channel = 2;
        return true;
    }

    // GET_NAME: entity_name at name_index 0, group_name at name_index 1.
    auto on_get_name(NameRef ref, uint32_t /*symbol*/) const -> std::optional<AtdeccString> override
    {
        if (ref.descriptor.descriptor_type == DESCRIPTOR_ENTITY && ref.descriptor.descriptor_index == 0) {
            if (ref.name_index == 0) {
                return AtdeccString{"TestEntity"};
            }
            if (ref.name_index == 1) {
                return set_group_name_;  // initially empty
            }
        }
        return std::nullopt;
    }

    auto on_set_name(NameRef ref, uint32_t /*symbol*/, AtdeccString const& name) -> uint8_t override
    {
        if (ref.descriptor.descriptor_type == DESCRIPTOR_ENTITY && ref.descriptor.descriptor_index == 0 && ref.name_index == 1) {
            set_group_name_ = name;
            return AEM_STATUS_SUCCESS;
        }
        return AEM_STATUS_NOT_IMPLEMENTED;
    }

    AtdeccString set_group_name_{};
};

}  // namespace

// ===========================================================================
// AemEntityModel dispatch tests
// ===========================================================================

TEST(aem_model_dispatch, unsupported_descriptor_type_returns_zero)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};

    // No VIDEO_UNIT in the test handler — should return 0.
    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = 0x0003 /*VIDEO_UNIT*/, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, static_cast<size_t>(0));
}

TEST(aem_model_dispatch, out_of_range_descriptor_type_returns_zero)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};

    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = 0xFFFF, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, static_cast<size_t>(0));
}

TEST(aem_model_dispatch, entity_descriptor_round_trips)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};

    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, DescriptorEntity::wire_size());

    // The written bytes should deserialize back to an equivalent struct.
    DescriptorEntity parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{buf.data(), n});
    EXPECT_EQ(parsed.configurations_count.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(parsed.current_configuration.get(), static_cast<uint16_t>(0));
    EXPECT_EQ(parsed.descriptor_type.get(), DESCRIPTOR_ENTITY);
}

TEST(aem_model_dispatch, configuration_round_trips_with_inline_trailer)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};

    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_CONFIGURATION, .descriptor_index = 0},
        make_span(buf));
    EXPECT_EQ(n, DescriptorConfiguration::LENGTH + (2 * sizeof(statusbar::atdecc::aem::DescriptorCountEntry)));

    // Deserialize and verify the trailer made it onto the wire.
    DescriptorConfiguration parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{buf.data(), n});
    EXPECT_EQ(parsed.descriptor_counts_count.get(), static_cast<uint16_t>(2));
    EXPECT_EQ(parsed.descriptor_counts[0].descriptor_type.get(), DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(parsed.descriptor_counts[0].count.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(parsed.descriptor_counts[1].descriptor_type.get(), DESCRIPTOR_STREAM_OUTPUT);
    EXPECT_EQ(parsed.descriptor_counts[1].count.get(), static_cast<uint16_t>(1));
}

TEST(aem_model_dispatch, stream_input_and_output_share_handler_method)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf_in{};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf_out{};

    auto const n_in = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_STREAM_INPUT, .descriptor_index = 0},
        make_span(buf_in));
    auto const n_out = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_STREAM_OUTPUT, .descriptor_index = 0},
        make_span(buf_out));

    EXPECT_EQ(n_in, n_out);
    EXPECT_EQ(n_in, DescriptorStream::LENGTH + sizeof(ieee::Eui64));

    // The descriptor_type field in the serialized bytes should reflect
    // the requested variant, proving the dispatch wrapper set it from ref.
    DescriptorStream in_parsed{};
    span_load_padded(in_parsed, std::span<uint8_t const>{buf_in.data(), n_in});
    EXPECT_EQ(in_parsed.descriptor_type.get(), DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(in_parsed.number_of_formats.get(), static_cast<uint16_t>(1));

    DescriptorStream out_parsed{};
    span_load_padded(out_parsed, std::span<uint8_t const>{buf_out.data(), n_out});
    EXPECT_EQ(out_parsed.descriptor_type.get(), DESCRIPTOR_STREAM_OUTPUT);
}

TEST(aem_model_dispatch, variable_trailer_audio_map)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};

    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_AUDIO_MAP, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, DescriptorAudioMap::LENGTH + (2 * sizeof(statusbar::atdecc::aem::AudioMapping)));

    DescriptorAudioMap parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{buf.data(), n});
    EXPECT_EQ(parsed.number_of_mappings.get(), static_cast<uint16_t>(2));
    EXPECT_EQ(parsed.mappings[0].mapping_cluster_channel.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(parsed.mappings[1].mapping_cluster_channel.get(), static_cast<uint16_t>(2));
}

TEST(aem_model_dispatch, small_output_buffer_returns_zero)
{
    TestHandler handler;
    AemEntityModel model{handler};
    // 10 bytes is too small for even the 74-byte CONFIGURATION wire size.
    std::array<uint8_t, 10> buf{};

    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_CONFIGURATION, .descriptor_index = 0},
        make_span(buf));
    EXPECT_EQ(n, static_cast<size_t>(0));
}

// ===========================================================================
// GET_NAME / SET_NAME
// ===========================================================================

TEST(aem_model_names, get_name_missing_returns_zero)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, 72> buf{};

    // name_index 5 is not handled.
    NameRef const ref{
        .descriptor = {.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, .name_index = 5};
    auto const n = model.get_name_for_wire(ref, make_span(buf));
    EXPECT_EQ(n, static_cast<size_t>(0));
}

TEST(aem_model_names, get_name_entity_name_round_trips)
{
    TestHandler handler;
    AemEntityModel model{handler};
    std::array<uint8_t, 72> buf{};

    NameRef const ref{
        .descriptor = {.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, .name_index = 0};
    auto const n = model.get_name_for_wire(ref, make_span(buf));
    EXPECT_EQ(n, static_cast<size_t>(72));

    // Parse the response header.
    uint16_t const wire_type = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];
    uint16_t const wire_index = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
    uint16_t const wire_name_index = (static_cast<uint16_t>(buf[4]) << 8) | buf[5];
    EXPECT_EQ(wire_type, DESCRIPTOR_ENTITY);
    EXPECT_EQ(wire_index, static_cast<uint16_t>(0));
    EXPECT_EQ(wire_name_index, static_cast<uint16_t>(0));

    // Parse the 64-byte name bytes as a string_view via the project helper
    // (avoids a raw reinterpret_cast) and compare the "TestEntity" prefix.
    auto const name_view = statusbar::as_string_view(std::span<uint8_t const>{buf.data() + 8, 10});
    EXPECT_EQ(std::string{name_view}, std::string{"TestEntity"});
}

TEST(aem_model_names, set_name_persists_and_round_trips_through_get)
{
    TestHandler handler;
    AemEntityModel model{handler};

    // Build a SET_NAME command body: header (8 bytes) + AtdeccString (64 bytes).
    std::array<uint8_t, 72> cmd{};
    // descriptor_type = ENTITY (0x0000), index 0, name_index 1 (group_name), config 0.
    cmd[0] = 0x00;
    cmd[1] = 0x00;  // descriptor_type = 0x0000
    cmd[2] = 0x00;
    cmd[3] = 0x00;  // descriptor_index = 0
    cmd[4] = 0x00;
    cmd[5] = 0x01;  // name_index = 1
    cmd[6] = 0x00;
    cmd[7] = 0x00;  // configuration_index = 0
    char const* new_name = "HelloGroup";
    std::memcpy(cmd.data() + 8, new_name, std::strlen(new_name));

    auto const status = model.apply_set_name(std::span<uint8_t const>{cmd});
    EXPECT_EQ(status, AEM_STATUS_SUCCESS);

    // Now GET_NAME for the same NameRef and verify the new value round-trips.
    std::array<uint8_t, 72> resp{};
    NameRef const ref{
        .descriptor = {.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, .name_index = 1};
    auto const n = model.get_name_for_wire(ref, make_span(resp));
    EXPECT_EQ(n, static_cast<size_t>(72));

    auto const parsed_view = statusbar::as_string_view(std::span<uint8_t const>{resp.data() + 8, std::strlen(new_name)});
    EXPECT_EQ(std::string{parsed_view}, std::string{new_name});
}

TEST(aem_model_names, set_name_undersized_body_returns_bad_arguments)
{
    TestHandler handler;
    AemEntityModel model{handler};

    std::array<uint8_t, 10> short_cmd{};
    auto const status = model.apply_set_name(std::span<uint8_t const>{short_cmd});
    EXPECT_EQ(status, AEM_STATUS_BAD_ARGUMENTS);
}

TEST(aem_model_names, set_name_to_unknown_field_reports_not_implemented)
{
    TestHandler handler;
    AemEntityModel model{handler};

    // Build a SET_NAME for a name the handler doesn't support
    // (descriptor_type CONFIGURATION, name_index 3).
    std::array<uint8_t, 72> cmd{};
    cmd[1] = DESCRIPTOR_CONFIGURATION;  // 0x0001 in little-endian-of-the-high-byte
    cmd[5] = 0x03;                      // name_index = 3
    auto const status = model.apply_set_name(std::span<uint8_t const>{cmd});
    EXPECT_EQ(status, AEM_STATUS_NOT_IMPLEMENTED);
}

// ===========================================================================
// DescriptorStorageHandler — the reusable "serve everything from a blob"
// handler. Tests build a tiny in-memory .aem blob with exactly one ENTITY
// descriptor and verify that the handler approves lookups for what's there
// and rejects lookups for what isn't.
// ===========================================================================

namespace {

/// Build a minimal .aem blob containing one ENTITY descriptor (312 bytes)
/// with a distinctive entity_name, plus one symbol (42) for that descriptor.
auto make_blob_with_entity(std::string_view entity_name = "BlobEntity") -> std::vector<uint8_t>
{
    using atdecc::aem::AtdeccString;
    using atdecc::aem::DESCRIPTOR_ENTITY;
    using atdecc::aem::DescriptorEntity;

    constexpr uint32_t header_size = 20;
    constexpr uint32_t toc_entry_size = 12;
    constexpr uint32_t symbol_entry_size = 10;
    constexpr uint32_t desc_size = DescriptorEntity::LENGTH;  // 312

    constexpr uint32_t toc_offset = header_size;
    constexpr uint32_t symbol_offset = toc_offset + toc_entry_size;
    constexpr uint32_t desc_offset = symbol_offset + symbol_entry_size;
    constexpr uint32_t total = desc_offset + desc_size;

    std::vector<uint8_t> blob(total, 0);

    // Header: "AEM1" magic + toc_count=1 + toc_offset=20 + symbol_count=1 + symbol_offset
    blob[0] = 0x41;
    blob[1] = 0x45;
    blob[2] = 0x4D;
    blob[3] = 0x31;
    // toc_count (uint32 network byte order) = 1
    blob[7] = 0x01;
    // toc_offset (uint32 NBO) = 20
    blob[11] = 0x14;
    // symbol_count = 1
    blob[15] = 0x01;
    // symbol_offset = 32 (header 20 + 1 TOC entry 12)
    blob[19] = static_cast<uint8_t>(symbol_offset);

    // TOC entry: type=0x0000 (ENTITY), index=0, config=0, length=312, offset=desc_offset
    // offsets within the TOC entry: type:2, index:2, config:2, length:2, offset:4
    blob[toc_offset + 0] = 0x00;
    blob[toc_offset + 1] = 0x00;  // descriptor_type = ENTITY
    blob[toc_offset + 6] = static_cast<uint8_t>((desc_size >> 8) & 0xFF);
    blob[toc_offset + 7] = static_cast<uint8_t>(desc_size & 0xFF);
    blob[toc_offset + 11] = static_cast<uint8_t>(desc_offset);

    // Symbol entry: type=ENTITY, index=0, config=0, symbol=42
    blob[symbol_offset + 0] = 0x00;
    blob[symbol_offset + 1] = 0x00;  // descriptor_type = ENTITY
    blob[symbol_offset + 9] = 0x2A;  // symbol = 42 (LSB of big-endian uint32)

    // Write a real DescriptorEntity into the descriptor slot so the
    // struct round-trips through span_load_padded.
    DescriptorEntity desc{};
    desc.entity_name = AtdeccString{std::string{entity_name}.c_str()};
    desc.configurations_count = 1;
    std::memcpy(blob.data() + desc_offset, &desc, sizeof(desc));

    return blob;
}

}  // namespace

TEST(descriptor_storage_handler, approves_descriptor_that_exists_in_storage)
{
    auto blob = make_blob_with_entity("StorageTest");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};
    AemEntityModel model{handler, *storage_result};

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};
    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, DescriptorEntity::wire_size());

    // Round-trip: the preloaded struct should carry the entity_name we wrote.
    DescriptorEntity parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{buf.data(), n});
    EXPECT_EQ(parsed.configurations_count.get(), static_cast<uint16_t>(1));
    auto const name_view = parsed.entity_name.as_string_view();
    // as_string_view trims trailing NULs so the length equals "StorageTest".
    EXPECT_EQ(std::string{name_view}, std::string{"StorageTest"});
}

TEST(descriptor_storage_handler, exposes_backing_storage_for_symbol_resolution)
{
    auto blob = make_blob_with_entity("StorageTest");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};
    // The handler now exposes its blob so the dispatch can resolve symbols.
    AemEntityHandler& base = handler;
    EXPECT_NE(base.descriptor_storage(), nullptr);
    auto sym = base.descriptor_storage()->get_symbol(0, DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(sym.has_value());
    EXPECT_EQ(*sym, 42u);
}

namespace {
/// Records the symbol the dispatch passes to on_get_entity, then defers to the
/// storage-backed base. Used to prove the symbol resolves through the HANDLER's
/// storage even when the AemEntityModel itself was built handler-only.
class SymbolRecordingHandler : public DescriptorStorageHandler
{
  public:
    using DescriptorStorageHandler::DescriptorStorageHandler;
    auto on_get_entity(DescriptorRef ref, uint32_t symbol, DescriptorEntity& desc) -> bool override
    {
        last_entity_symbol = symbol;
        return DescriptorStorageHandler::on_get_entity(ref, symbol, desc);
    }
    uint32_t last_entity_symbol{0xFFFFFFFF};
};
}  // namespace

TEST(descriptor_storage_handler, symbol_resolves_via_handler_storage_when_model_built_handler_only)
{
    // This is the Stage-2 behavior: the command path builds AemEntityModel{handler}
    // WITHOUT explicit storage; symbol_for must fall back to the handler's storage so
    // on_get_* still receives the designer-assigned symbol (42 for the ENTITY here).
    auto blob = make_blob_with_entity("SymTest");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    SymbolRecordingHandler handler{*storage_result};
    AemEntityModel model{handler};  // handler-only -- no explicit storage attached

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};
    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, DescriptorEntity::wire_size());
    EXPECT_EQ(handler.last_entity_symbol, 42u);  // resolved from the handler's blob
}

TEST(descriptor_storage_handler, rejects_descriptor_not_in_storage)
{
    auto blob = make_blob_with_entity();
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};
    AemEntityModel model{handler, *storage_result};

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};

    // Wrong descriptor_type: the blob only has an ENTITY.
    auto const n_conf = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_CONFIGURATION, .descriptor_index = 0},
        make_span(buf));
    EXPECT_EQ(n_conf, static_cast<size_t>(0));

    // Wrong index.
    auto const n_idx = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 1}, make_span(buf));
    EXPECT_EQ(n_idx, static_cast<size_t>(0));
}

namespace {

/// Derived handler that uses the storage preload but patches the
/// entity_id at runtime — the canonical pattern for applications that
/// load most descriptor fields from a .aem blob and inject a few
/// runtime-dependent fields.
class PatchingHandler : public DescriptorStorageHandler
{
  public:
    PatchingHandler(DescriptorStorage storage, ieee::Eui64 eid)
        : DescriptorStorageHandler{storage}
        , entity_id_{eid}
    {}

    auto on_get_entity(DescriptorRef ref, uint32_t symbol, DescriptorEntity& desc) -> bool override
    {
        if (!DescriptorStorageHandler::on_get_entity(ref, symbol, desc)) {
            return false;
        }
        desc.entity_id = entity_id_;
        return true;
    }

  private:
    ieee::Eui64 entity_id_;
};

}  // namespace

TEST(descriptor_storage_handler, derived_handler_patches_runtime_fields)
{
    auto blob = make_blob_with_entity();
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    ieee::Eui64 const runtime_eid{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    PatchingHandler handler{*storage_result, runtime_eid};
    AemEntityModel model{handler, *storage_result};

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};
    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, make_span(buf));
    EXPECT_EQ(n, DescriptorEntity::wire_size());

    DescriptorEntity parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{buf.data(), n});
    // The runtime-patched entity_id should be on the wire, not whatever
    // was in the blob (which defaulted to zero).
    EXPECT_TRUE(parsed.entity_id == runtime_eid);
    // The storage-loaded configurations_count should still be present.
    EXPECT_EQ(parsed.configurations_count.get(), static_cast<uint16_t>(1));
}

// ===========================================================================
// AemCommandHandler(AemEntityHandler&) new-world constructor end-to-end.
// Exercises the handler-based constructor through a full READ_DESCRIPTOR
// AEM command dispatch — no legacy EntityModel involved.
// ===========================================================================

TEST(aem_command_handler_new_ctor, read_descriptor_flows_through_handler)
{
    auto blob = make_blob_with_entity("NewCtorEntity");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    // Construct an AemCommandHandler from a user-supplied AemEntityHandler.
    // No EntityModel anywhere in this code path.
    DescriptorStorageHandler storage_handler{*storage_result};
    AemCommandHandler aem_handler{storage_handler};

    // Build a READ_DESCRIPTOR command header (parsed by process_packet in
    // production; here we call handle_command directly via make_test_result-
    // style wiring by constructing an AemDu and command body by hand).
    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_READ_DESCRIPTOR, atdecc::AemDu::AEM_DATA_LENGTH + 8);

    // Command body: config_index (2) + reserved (2) + descriptor_type (2) + descriptor_index (2)
    std::array<uint8_t, 8> command_data{};
    // descriptor_type = ENTITY (0x0000) at bytes 4..5 — already zero.
    // descriptor_index = 0 at bytes 6..7 — already zero.

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE + 16> out{};
    auto const response = aem_handler.handle_command(header, command_data, make_span(out));
    EXPECT_EQ(response.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(response.size, AemReadDescriptorResponsePayload::LENGTH + DescriptorEntity::wire_size());

    // The response body starts with the 4-byte READ_DESCRIPTOR header
    // (configuration_index + reserved) followed by the ENTITY descriptor wire
    // bytes (which begin with the descriptor's own descriptor_type +
    // descriptor_index). Parse the descriptor back out and verify it's the one
    // we put in the blob.
    DescriptorEntity parsed{};
    span_load_padded(
        parsed, std::span<uint8_t const>{out.data() + AemReadDescriptorResponsePayload::LENGTH, DescriptorEntity::wire_size()});
    auto const name_view = parsed.entity_name.as_string_view();
    EXPECT_EQ(std::string{name_view}, std::string{"NewCtorEntity"});
    EXPECT_EQ(parsed.configurations_count.get(), static_cast<uint16_t>(1));
}

TEST(aem_command_handler_new_ctor, read_descriptor_unknown_returns_no_such_descriptor)
{
    auto blob = make_blob_with_entity();
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler storage_handler{*storage_result};
    AemCommandHandler aem_handler{storage_handler};

    atdecc::AemDu header{};
    header.init_command(atdecc::AEM_COMMAND_READ_DESCRIPTOR, atdecc::AemDu::AEM_DATA_LENGTH + 8);

    // Request a CONFIGURATION descriptor — the blob only has an ENTITY,
    // so this should be rejected as NO_SUCH_DESCRIPTOR.
    std::array<uint8_t, 8> command_data{};
    command_data[5] = 0x01;  // descriptor_type = CONFIGURATION (0x0001)

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE + 16> out{};
    auto const response = aem_handler.handle_command(header, command_data, make_span(out));
    EXPECT_EQ(response.status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

// ===========================================================================
// DescriptorStorageHandler -- comprehensive on_get_* method tests
// ===========================================================================

namespace {

struct TocInput
{
    uint16_t descriptor_type;
    uint16_t descriptor_index;
    uint16_t configuration_index;
    uint16_t length;
};

auto make_single_descriptor_blob(TocInput entry) -> std::vector<uint8_t>
{
    constexpr uint32_t hdr = 20;
    constexpr uint32_t toc = 12;
    uint32_t const desc_off = hdr + toc;
    uint32_t const total = desc_off + entry.length;
    std::vector<uint8_t> b(total, 0);
    b[0] = 0x41;
    b[1] = 0x45;
    b[2] = 0x4D;
    b[3] = 0x31;
    b[7] = 0x01;
    b[11] = 0x14;
    b[19] = static_cast<uint8_t>(desc_off);
    uint32_t const t = hdr;
    b[t + 0] = static_cast<uint8_t>((entry.descriptor_type >> 8) & 0xFF);
    b[t + 1] = static_cast<uint8_t>(entry.descriptor_type & 0xFF);
    b[t + 2] = static_cast<uint8_t>((entry.descriptor_index >> 8) & 0xFF);
    b[t + 3] = static_cast<uint8_t>(entry.descriptor_index & 0xFF);
    b[t + 4] = static_cast<uint8_t>((entry.configuration_index >> 8) & 0xFF);
    b[t + 5] = static_cast<uint8_t>(entry.configuration_index & 0xFF);
    b[t + 6] = static_cast<uint8_t>((entry.length >> 8) & 0xFF);
    b[t + 7] = static_cast<uint8_t>(entry.length & 0xFF);
    b[t + 11] = static_cast<uint8_t>(desc_off);
    return b;
}

template <typename DescT>
void verify_sh(uint16_t dtype, uint16_t dlen, bool (AemEntityHandler::*method)(DescriptorRef, uint32_t, DescT&))
{
    auto blob = make_single_descriptor_blob({dtype, 0, 0, dlen});
    auto sr = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(sr.has_value());
    DescriptorStorageHandler handler{*sr};
    DescriptorRef ref_ok{.configuration_index = 0, .descriptor_type = dtype, .descriptor_index = 0};
    DescT desc{};
    EXPECT_TRUE((handler.*method)(ref_ok, 0, desc));
    DescriptorRef ref_bad{.configuration_index = 0, .descriptor_type = dtype, .descriptor_index = 99};
    DescT desc2{};
    EXPECT_FALSE((handler.*method)(ref_bad, 0, desc2));
}

}  // namespace

TEST(storage_handler_all, on_get_audio_unit)
{
    verify_sh<atdecc::aem::DescriptorAudioUnit>(
        atdecc::aem::DESCRIPTOR_AUDIO_UNIT, atdecc::aem::DescriptorAudioUnit::LENGTH, &AemEntityHandler::on_get_audio_unit);
}
TEST(storage_handler_all, on_get_video_unit)
{
    verify_sh<atdecc::aem::DescriptorVideoUnit>(
        atdecc::aem::DESCRIPTOR_VIDEO_UNIT, atdecc::aem::DescriptorVideoUnit::LENGTH, &AemEntityHandler::on_get_video_unit);
}
TEST(storage_handler_all, on_get_sensor_unit)
{
    verify_sh<atdecc::aem::DescriptorSensorUnit>(
        atdecc::aem::DESCRIPTOR_SENSOR_UNIT, atdecc::aem::DescriptorSensorUnit::LENGTH, &AemEntityHandler::on_get_sensor_unit);
}
TEST(storage_handler_all, on_get_stream)
{
    verify_sh<DescriptorStream>(DESCRIPTOR_STREAM_INPUT, DescriptorStream::MINIMUM_LENGTH, &AemEntityHandler::on_get_stream);
}
TEST(storage_handler_all, on_get_jack)
{
    verify_sh<atdecc::aem::DescriptorJack>(
        atdecc::aem::DESCRIPTOR_JACK_INPUT, atdecc::aem::DescriptorJack::LENGTH, &AemEntityHandler::on_get_jack);
}
TEST(storage_handler_all, on_get_avb_interface)
{
    verify_sh<atdecc::aem::DescriptorAvbInterface>(
        atdecc::aem::DESCRIPTOR_AVB_INTERFACE,
        atdecc::aem::DescriptorAvbInterface::MINIMUM_LENGTH,
        &AemEntityHandler::on_get_avb_interface);
}
TEST(storage_handler_all, on_get_clock_source)
{
    verify_sh<atdecc::aem::DescriptorClockSource>(
        atdecc::aem::DESCRIPTOR_CLOCK_SOURCE, atdecc::aem::DescriptorClockSource::LENGTH, &AemEntityHandler::on_get_clock_source);
}
TEST(storage_handler_all, on_get_clock_domain)
{
    verify_sh<atdecc::aem::DescriptorClockDomain>(
        atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN, atdecc::aem::DescriptorClockDomain::LENGTH, &AemEntityHandler::on_get_clock_domain);
}
TEST(storage_handler_all, on_get_memory_object)
{
    verify_sh<atdecc::aem::DescriptorMemoryObject>(
        atdecc::aem::DESCRIPTOR_MEMORY_OBJECT,
        atdecc::aem::DescriptorMemoryObject::LENGTH,
        &AemEntityHandler::on_get_memory_object);
}
TEST(storage_handler_all, on_get_locale)
{
    verify_sh<atdecc::aem::DescriptorLocale>(
        atdecc::aem::DESCRIPTOR_LOCALE, atdecc::aem::DescriptorLocale::LENGTH, &AemEntityHandler::on_get_locale);
}
TEST(storage_handler_all, on_get_strings)
{
    verify_sh<atdecc::aem::DescriptorStrings>(
        atdecc::aem::DESCRIPTOR_STRINGS, atdecc::aem::DescriptorStrings::LENGTH, &AemEntityHandler::on_get_strings);
}
TEST(storage_handler_all, on_get_stream_port)
{
    verify_sh<atdecc::aem::DescriptorStreamPort>(
        atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT,
        atdecc::aem::DescriptorStreamPort::LENGTH,
        &AemEntityHandler::on_get_stream_port);
}
TEST(storage_handler_all, on_get_external_port)
{
    verify_sh<atdecc::aem::DescriptorExternalPort>(
        atdecc::aem::DESCRIPTOR_EXTERNAL_PORT_INPUT,
        atdecc::aem::DescriptorExternalPort::LENGTH,
        &AemEntityHandler::on_get_external_port);
}
TEST(storage_handler_all, on_get_internal_port)
{
    verify_sh<atdecc::aem::DescriptorInternalPort>(
        atdecc::aem::DESCRIPTOR_INTERNAL_PORT_INPUT,
        atdecc::aem::DescriptorInternalPort::LENGTH,
        &AemEntityHandler::on_get_internal_port);
}
TEST(storage_handler_all, on_get_audio_cluster)
{
    verify_sh<atdecc::aem::DescriptorAudioCluster>(
        atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER,
        atdecc::aem::DescriptorAudioCluster::MINIMUM_LENGTH,
        &AemEntityHandler::on_get_audio_cluster);
}
TEST(storage_handler_all, on_get_video_cluster)
{
    verify_sh<atdecc::aem::DescriptorVideoCluster>(
        atdecc::aem::DESCRIPTOR_VIDEO_CLUSTER,
        atdecc::aem::DescriptorVideoCluster::LENGTH,
        &AemEntityHandler::on_get_video_cluster);
}
TEST(storage_handler_all, on_get_sensor_cluster)
{
    verify_sh<atdecc::aem::DescriptorSensorCluster>(
        atdecc::aem::DESCRIPTOR_SENSOR_CLUSTER,
        atdecc::aem::DescriptorSensorCluster::LENGTH,
        &AemEntityHandler::on_get_sensor_cluster);
}
TEST(storage_handler_all, on_get_audio_map)
{
    verify_sh<DescriptorAudioMap>(DESCRIPTOR_AUDIO_MAP, DescriptorAudioMap::LENGTH, &AemEntityHandler::on_get_audio_map);
}
TEST(storage_handler_all, on_get_video_map)
{
    verify_sh<atdecc::aem::DescriptorVideoMap>(
        atdecc::aem::DESCRIPTOR_VIDEO_MAP, atdecc::aem::DescriptorVideoMap::LENGTH, &AemEntityHandler::on_get_video_map);
}
TEST(storage_handler_all, on_get_sensor_map)
{
    verify_sh<atdecc::aem::DescriptorSensorMap>(
        atdecc::aem::DESCRIPTOR_SENSOR_MAP, atdecc::aem::DescriptorSensorMap::LENGTH, &AemEntityHandler::on_get_sensor_map);
}
TEST(storage_handler_all, on_get_control)
{
    verify_sh<atdecc::aem::DescriptorControl>(
        atdecc::aem::DESCRIPTOR_CONTROL, atdecc::aem::DescriptorControl::LENGTH, &AemEntityHandler::on_get_control);
}
TEST(storage_handler_all, on_get_control_block)
{
    verify_sh<atdecc::aem::DescriptorControlBlock>(
        atdecc::aem::DESCRIPTOR_CONTROL_BLOCK,
        atdecc::aem::DescriptorControlBlock::MINIMUM_LENGTH,
        &AemEntityHandler::on_get_control_block);
}
TEST(storage_handler_all, on_get_signal_selector)
{
    verify_sh<atdecc::aem::DescriptorSignalSelector>(
        atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR,
        atdecc::aem::DescriptorSignalSelector::LENGTH,
        &AemEntityHandler::on_get_signal_selector);
}
TEST(storage_handler_all, on_get_mixer)
{
    verify_sh<atdecc::aem::DescriptorMixer>(
        atdecc::aem::DESCRIPTOR_MIXER, atdecc::aem::DescriptorMixer::LENGTH, &AemEntityHandler::on_get_mixer);
}
TEST(storage_handler_all, on_get_matrix)
{
    verify_sh<atdecc::aem::DescriptorMatrix>(
        atdecc::aem::DESCRIPTOR_MATRIX, atdecc::aem::DescriptorMatrix::LENGTH, &AemEntityHandler::on_get_matrix);
}
TEST(storage_handler_all, on_get_matrix_signal)
{
    verify_sh<atdecc::aem::DescriptorMatrixSignal>(
        atdecc::aem::DESCRIPTOR_MATRIX_SIGNAL,
        atdecc::aem::DescriptorMatrixSignal::LENGTH,
        &AemEntityHandler::on_get_matrix_signal);
}
TEST(storage_handler_all, on_get_signal_splitter)
{
    verify_sh<atdecc::aem::DescriptorSignalSplitter>(
        atdecc::aem::DESCRIPTOR_SIGNAL_SPLITTER,
        atdecc::aem::DescriptorSignalSplitter::LENGTH,
        &AemEntityHandler::on_get_signal_splitter);
}
TEST(storage_handler_all, on_get_signal_combiner)
{
    verify_sh<atdecc::aem::DescriptorSignalCombiner>(
        atdecc::aem::DESCRIPTOR_SIGNAL_COMBINER,
        atdecc::aem::DescriptorSignalCombiner::LENGTH,
        &AemEntityHandler::on_get_signal_combiner);
}
TEST(storage_handler_all, on_get_signal_demux)
{
    verify_sh<atdecc::aem::DescriptorSignalDemultiplexer>(
        atdecc::aem::DESCRIPTOR_SIGNAL_DEMULTIPLEXER,
        atdecc::aem::DescriptorSignalDemultiplexer::LENGTH,
        &AemEntityHandler::on_get_signal_demultiplexer);
}
TEST(storage_handler_all, on_get_signal_mux)
{
    verify_sh<atdecc::aem::DescriptorSignalMultiplexer>(
        atdecc::aem::DESCRIPTOR_SIGNAL_MULTIPLEXER,
        atdecc::aem::DescriptorSignalMultiplexer::LENGTH,
        &AemEntityHandler::on_get_signal_multiplexer);
}
TEST(storage_handler_all, on_get_signal_transcoder)
{
    verify_sh<atdecc::aem::DescriptorSignalTranscoder>(
        atdecc::aem::DESCRIPTOR_SIGNAL_TRANSCODER,
        atdecc::aem::DescriptorSignalTranscoder::MINIMUM_LENGTH,
        &AemEntityHandler::on_get_signal_transcoder);
}
TEST(storage_handler_all, on_get_timing)
{
    verify_sh<atdecc::aem::DescriptorTiming>(
        atdecc::aem::DESCRIPTOR_TIMING, atdecc::aem::DescriptorTiming::LENGTH, &AemEntityHandler::on_get_timing);
}
TEST(storage_handler_all, on_get_ptp_instance)
{
    verify_sh<atdecc::aem::DescriptorPtpInstance>(
        atdecc::aem::DESCRIPTOR_PTP_INSTANCE, atdecc::aem::DescriptorPtpInstance::LENGTH, &AemEntityHandler::on_get_ptp_instance);
}
TEST(storage_handler_all, on_get_ptp_port)
{
    verify_sh<atdecc::aem::DescriptorPtpPort>(
        atdecc::aem::DESCRIPTOR_PTP_PORT, atdecc::aem::DescriptorPtpPort::LENGTH, &AemEntityHandler::on_get_ptp_port);
}
TEST(storage_handler_all, on_get_configuration)
{
    verify_sh<DescriptorConfiguration>(
        DESCRIPTOR_CONFIGURATION, DescriptorConfiguration::LENGTH, &AemEntityHandler::on_get_configuration);
}

TEST(storage_handler_all, storage_accessor)
{
    auto blob = make_blob_with_entity("AccessorTest2");
    auto sr = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(sr.has_value());
    DescriptorStorageHandler handler{*sr};
    auto const& storage = handler.storage();
    auto desc = storage.get_descriptor(0, DESCRIPTOR_ENTITY, 0);
    EXPECT_TRUE(desc.has_value());
}

//
// All-descriptor-types smoke test.
//
// A handler that implements *every* on_get_X and returns true. The
// test then calls get_descriptor_for_wire for each descriptor type
// constant, which drives each per-type dispatch_fixed template
// instantiation in nanoavb_aem_entity_model.cpp and the corresponding
// virtual-function override. The assertion is that each call produces
// at least the fixed wire_size() worth of bytes — every overload is
// exercised.
//

namespace {

using statusbar::atdecc::aem::DescriptorAudioUnit;
using statusbar::atdecc::aem::DescriptorAvbInterface;
using statusbar::atdecc::aem::DescriptorClockDomain;
using statusbar::atdecc::aem::DescriptorClockSource;
using statusbar::atdecc::aem::DescriptorControl;
using statusbar::atdecc::aem::DescriptorControlBlock;
using statusbar::atdecc::aem::DescriptorExternalPort;
using statusbar::atdecc::aem::DescriptorInternalPort;
using statusbar::atdecc::aem::DescriptorJack;
using statusbar::atdecc::aem::DescriptorLocale;
using statusbar::atdecc::aem::DescriptorMatrix;
using statusbar::atdecc::aem::DescriptorMatrixSignal;
using statusbar::atdecc::aem::DescriptorMemoryObject;
using statusbar::atdecc::aem::DescriptorMixer;
using statusbar::atdecc::aem::DescriptorPtpInstance;
using statusbar::atdecc::aem::DescriptorPtpPort;
using statusbar::atdecc::aem::DescriptorSensorCluster;
using statusbar::atdecc::aem::DescriptorSensorMap;
using statusbar::atdecc::aem::DescriptorSensorUnit;
using statusbar::atdecc::aem::DescriptorSignalCombiner;
using statusbar::atdecc::aem::DescriptorSignalDemultiplexer;
using statusbar::atdecc::aem::DescriptorSignalMultiplexer;
using statusbar::atdecc::aem::DescriptorSignalSelector;
using statusbar::atdecc::aem::DescriptorSignalSplitter;
using statusbar::atdecc::aem::DescriptorSignalTranscoder;
using statusbar::atdecc::aem::DescriptorStreamPort;
using statusbar::atdecc::aem::DescriptorStrings;
using statusbar::atdecc::aem::DescriptorTiming;
using statusbar::atdecc::aem::DescriptorVideoCluster;
using statusbar::atdecc::aem::DescriptorVideoMap;
using statusbar::atdecc::aem::DescriptorVideoUnit;

using statusbar::atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER;
using statusbar::atdecc::aem::DESCRIPTOR_AUDIO_UNIT;
using statusbar::atdecc::aem::DESCRIPTOR_AVB_INTERFACE;
using statusbar::atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN;
using statusbar::atdecc::aem::DESCRIPTOR_CLOCK_SOURCE;
using statusbar::atdecc::aem::DESCRIPTOR_CONTROL;
using statusbar::atdecc::aem::DESCRIPTOR_CONTROL_BLOCK;
using statusbar::atdecc::aem::DESCRIPTOR_EXTERNAL_PORT_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_INTERNAL_PORT_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_JACK_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_LOCALE;
using statusbar::atdecc::aem::DESCRIPTOR_MATRIX;
using statusbar::atdecc::aem::DESCRIPTOR_MATRIX_SIGNAL;
using statusbar::atdecc::aem::DESCRIPTOR_MEMORY_OBJECT;
using statusbar::atdecc::aem::DESCRIPTOR_MIXER;
using statusbar::atdecc::aem::DESCRIPTOR_PTP_INSTANCE;
using statusbar::atdecc::aem::DESCRIPTOR_PTP_PORT;
using statusbar::atdecc::aem::DESCRIPTOR_SENSOR_CLUSTER;
using statusbar::atdecc::aem::DESCRIPTOR_SENSOR_MAP;
using statusbar::atdecc::aem::DESCRIPTOR_SENSOR_UNIT;
using statusbar::atdecc::aem::DESCRIPTOR_SIGNAL_COMBINER;
using statusbar::atdecc::aem::DESCRIPTOR_SIGNAL_DEMULTIPLEXER;
using statusbar::atdecc::aem::DESCRIPTOR_SIGNAL_MULTIPLEXER;
using statusbar::atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR;
using statusbar::atdecc::aem::DESCRIPTOR_SIGNAL_SPLITTER;
using statusbar::atdecc::aem::DESCRIPTOR_SIGNAL_TRANSCODER;
using statusbar::atdecc::aem::DESCRIPTOR_STREAM_PORT_INPUT;
using statusbar::atdecc::aem::DESCRIPTOR_STRINGS;
using statusbar::atdecc::aem::DESCRIPTOR_TIMING;
using statusbar::atdecc::aem::DESCRIPTOR_VIDEO_CLUSTER;
using statusbar::atdecc::aem::DESCRIPTOR_VIDEO_MAP;
using statusbar::atdecc::aem::DESCRIPTOR_VIDEO_UNIT;

class AllDescriptorsHandler : public AemEntityHandler
{
  public:
    auto on_get_audio_unit(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorAudioUnit& /*d*/) -> bool override { return true; }
    auto on_get_video_unit(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorVideoUnit& /*d*/) -> bool override { return true; }
    auto on_get_sensor_unit(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSensorUnit& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_jack(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorJack& /*d*/) -> bool override { return true; }
    auto on_get_avb_interface(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorAvbInterface& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_clock_source(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorClockSource& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_clock_domain(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorClockDomain& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_memory_object(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorMemoryObject& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_locale(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorLocale& /*d*/) -> bool override { return true; }
    auto on_get_strings(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorStrings& /*d*/) -> bool override { return true; }
    auto on_get_stream_port(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorStreamPort& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_external_port(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorExternalPort& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_internal_port(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorInternalPort& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_audio_cluster(DescriptorRef /*ref*/, uint32_t /*symbol*/, statusbar::atdecc::aem::DescriptorAudioCluster& /*d*/)
        -> bool override
    {
        return true;
    }
    auto on_get_video_cluster(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorVideoCluster& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_sensor_cluster(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSensorCluster& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_video_map(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorVideoMap& /*d*/) -> bool override { return true; }
    auto on_get_sensor_map(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSensorMap& /*d*/) -> bool override { return true; }
    auto on_get_control(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorControl& /*d*/) -> bool override { return true; }
    auto on_get_control_block(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorControlBlock& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_signal_selector(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSignalSelector& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_mixer(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorMixer& /*d*/) -> bool override { return true; }
    auto on_get_matrix(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorMatrix& /*d*/) -> bool override { return true; }
    auto on_get_matrix_signal(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorMatrixSignal& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_signal_splitter(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSignalSplitter& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_signal_combiner(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSignalCombiner& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_signal_demultiplexer(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSignalDemultiplexer& /*d*/)
        -> bool override
    {
        return true;
    }
    auto on_get_signal_multiplexer(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSignalMultiplexer& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_signal_transcoder(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorSignalTranscoder& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_timing(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorTiming& /*d*/) -> bool override { return true; }
    auto on_get_ptp_instance(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorPtpInstance& /*d*/) -> bool override
    {
        return true;
    }
    auto on_get_ptp_port(DescriptorRef /*ref*/, uint32_t /*symbol*/, DescriptorPtpPort& /*d*/) -> bool override { return true; }
};

}  // namespace

TEST(aem_model_dispatch, all_descriptor_types_dispatch_through_handler)
{
    AllDescriptorsHandler handler;
    AemEntityModel model{handler};

    auto run_type = [&](uint16_t type) {
        std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};
        auto const n = model.get_descriptor_for_wire(
            DescriptorRef{.configuration_index = 0, .descriptor_type = type, .descriptor_index = 0}, make_span(buf));
        EXPECT_TRUE(n > 0);
    };

    run_type(DESCRIPTOR_AUDIO_UNIT);
    run_type(DESCRIPTOR_VIDEO_UNIT);
    run_type(DESCRIPTOR_SENSOR_UNIT);
    run_type(DESCRIPTOR_JACK_INPUT);
    run_type(DESCRIPTOR_AVB_INTERFACE);
    run_type(DESCRIPTOR_CLOCK_SOURCE);
    run_type(DESCRIPTOR_MEMORY_OBJECT);
    run_type(DESCRIPTOR_LOCALE);
    run_type(DESCRIPTOR_STRINGS);
    run_type(DESCRIPTOR_STREAM_PORT_INPUT);
    run_type(DESCRIPTOR_EXTERNAL_PORT_INPUT);
    run_type(DESCRIPTOR_INTERNAL_PORT_INPUT);
    run_type(DESCRIPTOR_AUDIO_CLUSTER);
    run_type(DESCRIPTOR_VIDEO_CLUSTER);
    run_type(DESCRIPTOR_SENSOR_CLUSTER);
    run_type(DESCRIPTOR_VIDEO_MAP);
    run_type(DESCRIPTOR_SENSOR_MAP);
    run_type(DESCRIPTOR_CONTROL);
    run_type(DESCRIPTOR_SIGNAL_SELECTOR);
    run_type(DESCRIPTOR_MIXER);
    run_type(DESCRIPTOR_MATRIX);
    run_type(DESCRIPTOR_MATRIX_SIGNAL);
    run_type(DESCRIPTOR_SIGNAL_SPLITTER);
    run_type(DESCRIPTOR_SIGNAL_COMBINER);
    run_type(DESCRIPTOR_SIGNAL_DEMULTIPLEXER);
    run_type(DESCRIPTOR_SIGNAL_MULTIPLEXER);
    run_type(DESCRIPTOR_SIGNAL_TRANSCODER);
    run_type(DESCRIPTOR_CLOCK_DOMAIN);
    run_type(DESCRIPTOR_CONTROL_BLOCK);
    run_type(DESCRIPTOR_TIMING);
    run_type(DESCRIPTOR_PTP_INSTANCE);
    run_type(DESCRIPTOR_PTP_PORT);
}

// ===========================================================================
// General symbol-keyed SET/GET descriptor-value dispatch (the AEM command
// family: SET/GET_CONTROL, SET/GET_STREAM_FORMAT, ...). One handler pair serves
// them all, keyed by (command_type, symbol).
// ===========================================================================

namespace {
/// A storage-backed handler that stores a descriptor "value" and records the
/// command_type + symbol it was dispatched with.
class ValueHandler : public DescriptorStorageHandler
{
  public:
    using DescriptorStorageHandler::DescriptorStorageHandler;

    auto on_set_descriptor_value(uint16_t command_type, DescriptorRef /*ref*/, uint32_t symbol, std::span<uint8_t const> value)
        -> uint8_t override
    {
        last_set_command = command_type;
        last_set_symbol = symbol;
        store.assign(value.begin(), value.end());
        return atdecc::AEM_STATUS_SUCCESS;
    }

    auto on_get_descriptor_value(uint16_t command_type, DescriptorRef /*ref*/, uint32_t symbol, std::span<uint8_t> out)
        -> size_t override
    {
        last_get_command = command_type;
        last_get_symbol = symbol;
        if (out.size() < store.size()) {
            return 0;
        }
        std::copy(store.begin(), store.end(), out.begin());
        return store.size();
    }

    std::vector<uint8_t> store{};
    uint16_t last_set_command{0};
    uint16_t last_get_command{0};
    uint32_t last_set_symbol{0xFFFFFFFFu};
    uint32_t last_get_symbol{0xFFFFFFFFu};
};
}  // namespace

TEST(aem_value_commands, set_then_get_round_trips_keyed_by_symbol)
{
    // make_blob_with_entity binds symbol 42 to the ENTITY descriptor (type 0). The
    // value commands are descriptor-type-agnostic -- they resolve the symbol for the
    // targeted (type, index) and pass it to the hook. Target ENTITY(0) for simplicity.
    auto blob = make_blob_with_entity("ValTest");
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    ValueHandler handler{*storage};
    AemEntityModel model{handler};  // handler-only -> symbol resolves via the handler's storage

    // SET: descriptor_type(0) + descriptor_index(0) + value{0xAA,0xBB}
    std::array<uint8_t, 6> const set_cmd{0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};
    std::array<uint8_t, 64> out{};
    auto const sr = model.apply_set_descriptor_value(atdecc::AEM_COMMAND_SET_CONTROL, set_cmd, out);
    EXPECT_EQ(sr.status, atdecc::AEM_STATUS_SUCCESS);
    EXPECT_EQ(sr.size, set_cmd.size());  // response echoes the command
    EXPECT_EQ(handler.last_set_command, atdecc::AEM_COMMAND_SET_CONTROL);
    EXPECT_EQ(handler.last_set_symbol, 42u);  // resolved from the blob symbol table
    EXPECT_EQ(handler.store.size(), 2u);
    EXPECT_EQ(static_cast<int>(handler.store[0]), 0xAA);

    // GET: descriptor_type(0) + descriptor_index(0) -> response = type/index + value
    std::array<uint8_t, 4> const get_cmd{0x00, 0x00, 0x00, 0x00};
    std::array<uint8_t, 64> out2{};
    auto const n = model.get_descriptor_value_for_wire(atdecc::AEM_COMMAND_GET_CONTROL, get_cmd, out2);
    EXPECT_EQ(n, size_t{6});  // 4-byte type/index header + 2-byte value
    EXPECT_EQ(handler.last_get_command, atdecc::AEM_COMMAND_GET_CONTROL);
    EXPECT_EQ(handler.last_get_symbol, 42u);
    EXPECT_EQ(static_cast<int>(out2[4]), 0xAA);
    EXPECT_EQ(static_cast<int>(out2[5]), 0xBB);
}

TEST(aem_value_commands, unhandled_value_command_reports_not_implemented)
{
    // A handler that does not override the value hooks (read-only) -> NOT_IMPLEMENTED / 0.
    auto blob = make_blob_with_entity();
    auto storage = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    DescriptorStorageHandler handler{*storage};
    AemEntityModel model{handler};

    std::array<uint8_t, 6> const set_cmd{0, 0, 0, 0, 0, 0};
    std::array<uint8_t, 64> out{};
    EXPECT_EQ(
        model.apply_set_descriptor_value(atdecc::AEM_COMMAND_SET_CONTROL, set_cmd, out).status, atdecc::AEM_STATUS_NOT_IMPLEMENTED);
    std::array<uint8_t, 4> const get_cmd{0, 0, 0, 0};
    EXPECT_EQ(model.get_descriptor_value_for_wire(atdecc::AEM_COMMAND_GET_CONTROL, get_cmd, out), size_t{0});
}

// ===========================================================================
// Built-in entity-name GET_NAME / SET_NAME (DescriptorStorageHandler).
//
// A blob-backed handler that opts in via manage_entity_name() serves GET_NAME and
// SET_NAME for the ENTITY descriptor's entity_name (descriptor 0, name 0) with no
// per-entity code, reflects the current value in READ_DESCRIPTOR, fires the change
// callback, and rejects names for any other (descriptor, name_index).
// ===========================================================================

namespace {

/// Drive a single AEM command through AemCommandHandler::handle_command and return
/// {status, response bytes}. Mirrors the wire path a controller's command takes.
struct NameCmdResult
{
    uint8_t status{AEM_STATUS_SUCCESS};
    std::vector<uint8_t> bytes;
};

auto run_aem_command(AemCommandHandler& h, uint16_t cmd, std::span<uint8_t const> body) -> NameCmdResult
{
    atdecc::AemDu header{};
    header.init_command(cmd, static_cast<uint16_t>(atdecc::AemDu::AEM_DATA_LENGTH + body.size()));
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> buf{};
    auto const resp = h.handle_command(header, body, make_span(buf));
    return {.status = resp.status, .bytes = std::vector<uint8_t>(buf.data(), buf.data() + resp.size)};
}

/// GET_NAME command body: the 8-byte AemNameCommandPayload (type/index/name_index/config).
auto make_get_name_body(uint16_t descriptor_type, uint16_t descriptor_index, uint16_t name_index) -> std::vector<uint8_t>
{
    atdecc::aem::AemNameCommandPayload cmd{
        .descriptor_type = descriptor_type,
        .descriptor_index = descriptor_index,
        .name_index = name_index,
        .configuration_index = 0};
    std::vector<uint8_t> body(atdecc::aem::AemNameCommandPayload::LENGTH, 0);
    span_store(make_span(body), cmd);
    return body;
}

/// SET_NAME command body: the 72-byte AemNamePayload (8-byte header + 64-byte name).
auto make_set_name_body(uint16_t descriptor_type, uint16_t descriptor_index, uint16_t name_index, std::string_view name)
    -> std::vector<uint8_t>
{
    atdecc::aem::AemNamePayload payload{
        .descriptor_type = descriptor_type,
        .descriptor_index = descriptor_index,
        .name_index = name_index,
        .configuration_index = 0};
    std::memcpy(payload.name.data(), name.data(), std::min<size_t>(name.size(), payload.name.size()));
    std::vector<uint8_t> body(atdecc::aem::AemNamePayload::LENGTH, 0);
    span_store(make_span(body), payload);
    return body;
}

/// Parse a GET_NAME/SET_NAME wire response (an AemNamePayload) and return its name.
auto name_from_response(std::span<uint8_t const> bytes) -> std::string
{
    atdecc::aem::AemNamePayload resp{};
    span_load(resp, bytes);
    return std::string{AtdeccString{resp.name.data()}.as_string_view()};
}

}  // namespace

TEST(entity_name, get_set_round_trip_and_callback_and_read_descriptor)
{
    auto blob = make_blob_with_entity("SeedName");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};
    handler.manage_entity_name(AtdeccString{"SeedName"});

    std::string changed_to;
    handler.set_on_entity_name_changed([&](AtdeccString const& n) {
        changed_to = std::string{n.as_string_view()};
        return AEM_STATUS_SUCCESS;
    });

    AemCommandHandler cmd_handler{handler};

    // GET_NAME of the seed value.
    auto const get0 = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_NAME, make_get_name_body(DESCRIPTOR_ENTITY, 0, 0));
    EXPECT_EQ(get0.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(get0.bytes.size(), atdecc::aem::AemNamePayload::LENGTH);
    EXPECT_EQ(name_from_response(get0.bytes), std::string{"SeedName"});

    // SET_NAME to a new value; the callback fires and the in-memory name updates.
    auto const set =
        run_aem_command(cmd_handler, atdecc::AEM_COMMAND_SET_NAME, make_set_name_body(DESCRIPTOR_ENTITY, 0, 0, "LiveName"));
    EXPECT_EQ(set.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(changed_to, std::string{"LiveName"});
    EXPECT_EQ(std::string{handler.entity_name().as_string_view()}, std::string{"LiveName"});

    // GET_NAME now returns the new value.
    auto const get1 = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_NAME, make_get_name_body(DESCRIPTOR_ENTITY, 0, 0));
    EXPECT_EQ(get1.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(name_from_response(get1.bytes), std::string{"LiveName"});

    // READ_DESCRIPTOR of the ENTITY now reflects the updated name too.
    AemEntityModel const model{handler, *storage_result};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> dbuf{};
    auto const dn = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0}, make_span(dbuf));
    EXPECT_EQ(dn, DescriptorEntity::wire_size());
    DescriptorEntity parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{dbuf.data(), dn});
    EXPECT_EQ(std::string{parsed.entity_name.as_string_view()}, std::string{"LiveName"});
}

TEST(entity_name, callback_rejection_keeps_old_name)
{
    auto blob = make_blob_with_entity("SeedName");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};
    handler.manage_entity_name(AtdeccString{"SeedName"});
    handler.set_on_entity_name_changed([](AtdeccString const&) { return AEM_STATUS_NOT_IMPLEMENTED; });

    AemCommandHandler cmd_handler{handler};
    auto const set =
        run_aem_command(cmd_handler, atdecc::AEM_COMMAND_SET_NAME, make_set_name_body(DESCRIPTOR_ENTITY, 0, 0, "Rejected"));
    EXPECT_EQ(set.status, AEM_STATUS_NOT_IMPLEMENTED);
    // Rejected: the in-memory name is unchanged.
    EXPECT_EQ(std::string{handler.entity_name().as_string_view()}, std::string{"SeedName"});
}

TEST(entity_name, unmanaged_field_reports_not_implemented)
{
    auto blob = make_blob_with_entity("SeedName");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};
    handler.manage_entity_name(AtdeccString{"SeedName"});
    AemCommandHandler cmd_handler{handler};

    // group_name (name_index 1) is NOT the managed entity_name.
    auto const get_group = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_NAME, make_get_name_body(DESCRIPTOR_ENTITY, 0, 1));
    EXPECT_EQ(get_group.status, AEM_STATUS_NOT_IMPLEMENTED);

    auto const set_group =
        run_aem_command(cmd_handler, atdecc::AEM_COMMAND_SET_NAME, make_set_name_body(DESCRIPTOR_ENTITY, 0, 1, "Nope"));
    EXPECT_EQ(set_group.status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(entity_name, unmanaged_handler_does_not_serve_entity_name)
{
    // Without manage_entity_name(), the handler serves no GET_NAME/SET_NAME at all.
    auto blob = make_blob_with_entity("SeedName");
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());

    DescriptorStorageHandler handler{*storage_result};  // not managed
    AemCommandHandler cmd_handler{handler};
    auto const get0 = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_NAME, make_get_name_body(DESCRIPTOR_ENTITY, 0, 0));
    EXPECT_EQ(get0.status, AEM_STATUS_NOT_IMPLEMENTED);
}

// ===========================================================================
// Built-in SIGNAL_SELECTOR (DescriptorStorageHandler).
//
// A blob-backed handler accepts SET_SIGNAL_SELECTOR for sources authored in
// the descriptor, stores the selection, and reflects it in
// GET_SIGNAL_SELECTOR and READ_DESCRIPTOR. Sources not in the descriptor's
// list are rejected; the change callback can veto.
// ===========================================================================

namespace {

using atdecc::aem::DESCRIPTOR_AUDIO_CLUSTER;
using atdecc::aem::DESCRIPTOR_JACK_INPUT;
using atdecc::aem::DESCRIPTOR_SIGNAL_SELECTOR;
using atdecc::aem::DescriptorSignalSelector;

/// Build a blob with one SIGNAL_SELECTOR descriptor (index 0) offering two
/// sources: AUDIO_CLUSTER 0 and AUDIO_CLUSTER 1. Current/default = cluster 0.
auto make_blob_with_signal_selector() -> std::vector<uint8_t>
{
    constexpr uint32_t header_size = 20;
    constexpr uint32_t toc_entry_size = 12;
    constexpr uint32_t desc_size = DescriptorSignalSelector::LENGTH + (size_t{2} * 6);  // 96 + sources
    constexpr uint32_t toc_offset = header_size;
    constexpr uint32_t desc_offset = toc_offset + toc_entry_size;

    std::vector<uint8_t> blob(desc_offset + desc_size, 0);
    blob[0] = 0x41;  // "AEM1"
    blob[1] = 0x45;
    blob[2] = 0x4D;
    blob[3] = 0x31;
    blob[7] = 0x01;  // toc_count = 1
    blob[11] = static_cast<uint8_t>(toc_offset);
    // symbol_count = 0; symbol_offset points just past the TOC.
    blob[19] = static_cast<uint8_t>(desc_offset);

    // TOC entry: type=SIGNAL_SELECTOR, index=0, config=0, length, offset.
    blob[toc_offset + 0] = static_cast<uint8_t>((DESCRIPTOR_SIGNAL_SELECTOR >> 8) & 0xFF);
    blob[toc_offset + 1] = static_cast<uint8_t>(DESCRIPTOR_SIGNAL_SELECTOR & 0xFF);
    blob[toc_offset + 6] = static_cast<uint8_t>((desc_size >> 8) & 0xFF);
    blob[toc_offset + 7] = static_cast<uint8_t>(desc_size & 0xFF);
    blob[toc_offset + 11] = static_cast<uint8_t>(desc_offset);

    DescriptorSignalSelector desc{};
    desc.descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR;
    desc.sources_offset = DescriptorSignalSelector::LENGTH;
    desc.number_of_sources = 2;
    desc.current_signal_type = DESCRIPTOR_AUDIO_CLUSTER;
    desc.current_signal_index = 0;
    desc.current_signal_output = 0;
    desc.default_signal_type = DESCRIPTOR_AUDIO_CLUSTER;
    std::memcpy(blob.data() + desc_offset, &desc, sizeof(desc));

    // Two 6-byte sources: AUDIO_CLUSTER 0 and AUDIO_CLUSTER 1.
    size_t const src0 = desc_offset + DescriptorSignalSelector::LENGTH;
    blob[src0 + 0] = static_cast<uint8_t>((DESCRIPTOR_AUDIO_CLUSTER >> 8) & 0xFF);
    blob[src0 + 1] = static_cast<uint8_t>(DESCRIPTOR_AUDIO_CLUSTER & 0xFF);
    blob[src0 + 6] = blob[src0 + 0];
    blob[src0 + 7] = blob[src0 + 1];
    blob[src0 + 9] = 0x01;  // second source: signal_index = 1
    return blob;
}

/// SET_SIGNAL_SELECTOR / GET_SIGNAL_SELECTOR command body.
auto make_signal_selector_body(uint16_t descriptor_index, std::optional<std::array<uint16_t, 3>> const source = std::nullopt)
    -> std::vector<uint8_t>
{
    std::vector<uint8_t> body;
    auto push_u16 = [&body](uint16_t v) {
        body.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    push_u16(DESCRIPTOR_SIGNAL_SELECTOR);
    push_u16(descriptor_index);
    if (source) {
        for (auto const v : *source) {
            push_u16(v);
        }
    }
    return body;
}

/// The {signal_type, signal_index, signal_output} triple in a
/// SET/GET_SIGNAL_SELECTOR response (after the 4-byte descriptor header).
auto response_source(std::span<uint8_t const> bytes) -> std::array<uint16_t, 3>
{
    std::array<uint16_t, 3> out{};
    for (size_t i = 0; i < 3; ++i) {
        out[i] = static_cast<uint16_t>((bytes[4 + (2 * i)] << 8) | bytes[5 + (2 * i)]);
    }
    return out;
}

}  // namespace

TEST(signal_selector, get_serves_blob_default_and_set_round_trips)
{
    auto blob = make_blob_with_signal_selector();
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());
    DescriptorStorageHandler handler{*storage_result};
    AemCommandHandler cmd_handler{handler};

    // GET before any SET: the blob's authored current (cluster 0).
    auto const get0 = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR, make_signal_selector_body(0));
    EXPECT_EQ(get0.status, AEM_STATUS_SUCCESS);
    // 4-byte descriptor header + source triple + reserved doublet.
    EXPECT_EQ(get0.bytes.size(), size_t{12});
    EXPECT_TRUE(response_source(get0.bytes) == (std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 0, 0}));

    // SET to the second authored source (cluster 1) succeeds and echoes.
    auto const set1 = run_aem_command(
        cmd_handler,
        atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR,
        make_signal_selector_body(0, std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 1, 0}));
    EXPECT_EQ(set1.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(response_source(set1.bytes) == (std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 1, 0}));

    // GET reflects the new selection.
    auto const get1 = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR, make_signal_selector_body(0));
    EXPECT_EQ(get1.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(response_source(get1.bytes) == (std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 1, 0}));

    // READ_DESCRIPTOR agrees: the served descriptor carries the runtime current.
    AemEntityModel model{handler};
    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};
    auto const n = model.get_descriptor_for_wire(
        DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_SIGNAL_SELECTOR, .descriptor_index = 0},
        make_span(buf));
    EXPECT_TRUE(n >= DescriptorSignalSelector::LENGTH);
    DescriptorSignalSelector parsed{};
    span_load_padded(parsed, std::span<uint8_t const>{buf.data(), n});
    EXPECT_EQ(parsed.current_signal_index.get(), static_cast<uint16_t>(1));
}

TEST(signal_selector, source_not_in_descriptor_is_rejected)
{
    auto blob = make_blob_with_signal_selector();
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());
    DescriptorStorageHandler handler{*storage_result};
    AemCommandHandler cmd_handler{handler};

    // JACK_INPUT 0 is not one of the authored sources.
    auto const set = run_aem_command(
        cmd_handler,
        atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR,
        make_signal_selector_body(0, std::array<uint16_t, 3>{DESCRIPTOR_JACK_INPUT, 0, 0}));
    EXPECT_EQ(set.status, AEM_STATUS_BAD_ARGUMENTS);

    // A truncated value (no source triple) is also rejected.
    auto const short_set = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR, make_signal_selector_body(0));
    EXPECT_EQ(short_set.status, AEM_STATUS_BAD_ARGUMENTS);

    // Selection is unchanged.
    auto const get = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR, make_signal_selector_body(0));
    EXPECT_TRUE(response_source(get.bytes) == (std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 0, 0}));

    // A selector index the blob doesn't have: SET names the missing
    // descriptor, GET reports not-implemented.
    auto const set_missing = run_aem_command(
        cmd_handler,
        atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR,
        make_signal_selector_body(7, std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 0, 0}));
    EXPECT_EQ(set_missing.status, atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR);
    auto const get_missing = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR, make_signal_selector_body(7));
    EXPECT_EQ(get_missing.status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(signal_selector, change_callback_gates_and_observes)
{
    auto blob = make_blob_with_signal_selector();
    auto storage_result = atdecc::aem::DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage_result.has_value());
    DescriptorStorageHandler handler{*storage_result};
    AemCommandHandler cmd_handler{handler};

    // A vetoing callback: the status propagates and the selection stays.
    handler.set_on_signal_selector_changed(
        [](uint16_t, DescriptorStorageHandler::SignalSourceRef const&) -> uint8_t { return atdecc::AEM_STATUS_NOT_SUPPORTED; });
    auto const vetoed = run_aem_command(
        cmd_handler,
        atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR,
        make_signal_selector_body(0, std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 1, 0}));
    EXPECT_EQ(vetoed.status, atdecc::AEM_STATUS_NOT_SUPPORTED);
    auto const get0 = run_aem_command(cmd_handler, atdecc::AEM_COMMAND_GET_SIGNAL_SELECTOR, make_signal_selector_body(0));
    EXPECT_TRUE(response_source(get0.bytes) == (std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 0, 0}));

    // An accepting callback observes the validated request.
    static uint16_t seen_index = 0xFFFF;
    static uint16_t seen_signal_index = 0xFFFF;
    handler.set_on_signal_selector_changed(
        [](uint16_t const descriptor_index, DescriptorStorageHandler::SignalSourceRef const& src) -> uint8_t {
            seen_index = descriptor_index;
            seen_signal_index = src.signal_index;
            return AEM_STATUS_SUCCESS;
        });
    auto const accepted = run_aem_command(
        cmd_handler,
        atdecc::AEM_COMMAND_SET_SIGNAL_SELECTOR,
        make_signal_selector_body(0, std::array<uint16_t, 3>{DESCRIPTOR_AUDIO_CLUSTER, 1, 0}));
    EXPECT_EQ(accepted.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(seen_index, static_cast<uint16_t>(0));
    EXPECT_EQ(seen_signal_index, static_cast<uint16_t>(1));
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_aem_entity_model_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_aem_entity_model_test");
    return result;
}
