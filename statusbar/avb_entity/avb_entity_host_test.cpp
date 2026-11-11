// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for AvbEntityHost's symbol-aware construction path: building the host from
// a DescriptorStorageHandler (instead of a parsed EntityModel) and the
// symbol <-> descriptor lookups it then exposes. The legacy parsed-model path
// returns empty/failure from those lookups.

#include "statusbar/avb_entity/avb_entity_host.hpp"

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_control_types.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_stream_info.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/test/test.hpp"

#ifdef __linux__
#    include "statusbar/itc/itc_stop_token.hpp"
#    include "statusbar/net/net_message_reactor.hpp"

#    include <poll.h>
#    include <sched.h>
#    include <unistd.h>

#    include <csignal>
#    include <cstdio>

#    include <net/if.h>
#    include <sys/ioctl.h>
#    include <sys/socket.h>
#    include <sys/wait.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

using statusbar::atdecc::AEM_COMMAND_GET_AVB_INFO;
using statusbar::atdecc::AEM_STATUS_NO_SUCH_DESCRIPTOR;
using statusbar::atdecc::AEM_STATUS_SUCCESS;
using statusbar::atdecc::AemDu;
using statusbar::atdecc::aem::AemAvbInfoPayload;
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

    using statusbar::atdecc::aem::DescriptorStorageHeader;
    using statusbar::atdecc::aem::DescriptorStorageSymbolEntry;
    using statusbar::atdecc::aem::DescriptorStorageTocEntry;

    std::vector<uint8_t> blob(total, 0);
    DescriptorStorageHeader const header{
        .magic = DescriptorStorage::MAGIC,
        .toc_count = 1,
        .toc_offset = toc_offset,
        .symbol_count = 1,
        .symbol_offset = symbol_offset};
    statusbar::span_store(statusbar::make_span(blob), header);

    DescriptorStorageTocEntry const toc{
        .descriptor_type = DESCRIPTOR_ENTITY,
        .descriptor_index = 0,
        .configuration_index = 0,
        .length = desc_size,
        .offset = desc_offset};
    statusbar::span_store(statusbar::make_span(blob, {.start = toc_offset}), toc);

    DescriptorStorageSymbolEntry const sym{
        .descriptor_type = DESCRIPTOR_ENTITY, .descriptor_index = 0, .configuration_index = 0, .symbol = 0x00C0FFEE};
    statusbar::span_store(statusbar::make_span(blob, {.start = symbol_offset}), sym);

    // A real DescriptorEntity in the slot.
    DescriptorEntity desc{};
    desc.configurations_count = 1;
    statusbar::span_store(statusbar::make_span(blob, {.start = desc_offset}), desc);
    return blob;
}

/// A symbol-aware host together with the handler it serves through (the host
/// binds the handler, so the handler is declared first and outlives it).
class SymbolAwareHost
{
  public:
    explicit SymbolAwareHost(DescriptorStorage storage)
        : handler_{storage}
        , host_{handler_, AdpAdvertiserConfig{}, 1, 4, 1}
    {}

    auto operator->() noexcept -> AvbEntityHost* { return &host_; }

  private:
    DescriptorStorageHandler handler_;
    AvbEntityHost host_;
};

auto make_host_from_blob(std::vector<uint8_t> const& blob) -> SymbolAwareHost
{
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    return SymbolAwareHost{*storage};
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

    using statusbar::atdecc::aem::DescriptorStorageHeader;
    using statusbar::atdecc::aem::DescriptorStorageTocEntry;

    std::vector<uint8_t> blob(total, 0);
    DescriptorStorageHeader const header{
        .magic = DescriptorStorage::MAGIC,
        .toc_count = 3,
        .toc_offset = toc_offset,
        .symbol_count = 0,
        .symbol_offset = toc_offset};  // no symbols
    statusbar::span_store(statusbar::make_span(blob), header);

    auto put_toc = [&](uint32_t slot, uint16_t type, uint16_t index, uint16_t length, uint32_t offset) {
        DescriptorStorageTocEntry const toc{
            .descriptor_type = type, .descriptor_index = index, .configuration_index = 0, .length = length, .offset = offset};
        statusbar::span_store(statusbar::make_span(blob, {.start = toc_offset + (slot * toc_entry_size)}), toc);
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

//
// Live gPTP state: AVB_INTERFACE runtime fields and GET_AVB_INFO
//

namespace {

/// ENTITY + CONFIGURATION + one AVB_INTERFACE of @p priority1, through the blob writer.
auto make_interface_blob(uint8_t const priority1 = 248) -> std::vector<uint8_t>
{
    using statusbar::atdecc::aem::DESCRIPTOR_AVB_INTERFACE;
    using statusbar::atdecc::aem::DESCRIPTOR_CONFIGURATION;
    using statusbar::atdecc::aem::DescriptorAvbInterface;
    using statusbar::atdecc::aem::DescriptorConfiguration;
    statusbar::nanoavb::AemBlobWriter w;
    DescriptorEntity entity{};
    entity.configurations_count = 1;
    w.add(0, DESCRIPTOR_ENTITY, 0, statusbar::make_const_span(entity));
    DescriptorConfiguration config{};
    w.add(0, DESCRIPTOR_CONFIGURATION, 0, statusbar::make_const_span(config));
    DescriptorAvbInterface iface{};
    iface.priority1 = priority1;
    iface.priority2 = 248;
    iface.clock_identity = statusbar::ieee::Eui64{0x70, 0xB3, 0xD5, 0xFF, 0xFE, 0xED, 0x00, 0x01};
    w.add(0, DESCRIPTOR_AVB_INTERFACE, 0, statusbar::make_const_span(iface));
    return w.write();
}

/// AVB_INTERFACE 0's GPTP_GM_CHANGED.
auto gm_changed(SymbolAwareHost& host) -> uint32_t
{
    std::array<uint32_t, 32> counters{};
    uint32_t valid = 0;
    EXPECT_TRUE(host->fill_counters(statusbar::atdecc::aem::DESCRIPTOR_AVB_INTERFACE, 0, valid, counters));
    return counters[5];
}

}  // namespace

TEST(avb_entity_host_avb_info, storage_handler_reflects_runtime_interface_fields)
{
    using statusbar::atdecc::aem::DESCRIPTOR_AVB_INTERFACE;
    using statusbar::atdecc::aem::DescriptorAvbInterface;
    using statusbar::nanoavb::AvbInterfaceRuntime;
    using statusbar::nanoavb::DescriptorId;
    using statusbar::nanoavb::DescriptorRef;
    auto const blob = make_interface_blob();
    auto storage = DescriptorStorage::create(std::span<uint8_t const>(blob));
    EXPECT_TRUE(storage.has_value());
    DescriptorStorageHandler handler{*storage};
    DescriptorId const id{
        .ref = DescriptorRef{.configuration_index = 0, .descriptor_type = DESCRIPTOR_AVB_INTERFACE, .descriptor_index = 0}};

    DescriptorAvbInterface desc{};
    EXPECT_TRUE(handler.on_get_avb_interface(id, desc));
    EXPECT_EQ(static_cast<uint8_t>(desc.priority1), 248);  // authored

    AvbInterfaceRuntime rt{};
    rt.identity_valid = true;
    rt.mac_address = statusbar::ieee::Eui48{0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    rt.clock_identity = statusbar::ieee::Eui64{0x02, 0x11, 0x22, 0xFF, 0xFE, 0x33, 0x44, 0x55};
    rt.port_number = 1;
    rt.gptp_valid = true;
    rt.priority1 = 128;
    rt.priority2 = 200;
    rt.domain_number = 3;
    rt.log_sync_interval = -3;
    handler.set_avb_interface_runtime(0, rt);

    EXPECT_TRUE(handler.on_get_avb_interface(id, desc));
    EXPECT_EQ(desc.mac_address, rt.mac_address);
    EXPECT_EQ(desc.clock_identity, rt.clock_identity);
    EXPECT_EQ(desc.port_number.get(), 1);
    EXPECT_EQ(static_cast<uint8_t>(desc.priority1), 128);
    EXPECT_EQ(static_cast<uint8_t>(desc.priority2), 200);
    EXPECT_EQ(static_cast<uint8_t>(desc.domain_number), 3);
    EXPECT_EQ(static_cast<int8_t>(static_cast<uint8_t>(desc.log_sync_interval)), -3);

    // Identity-only update leaves the gPTP group as it was.
    AvbInterfaceRuntime identity_only{};
    identity_only.identity_valid = true;
    identity_only.mac_address = statusbar::ieee::Eui48{0x02, 0, 0, 0, 0, 0x01};
    handler.set_avb_interface_runtime(0, identity_only);
    EXPECT_TRUE(handler.on_get_avb_interface(id, desc));
    EXPECT_EQ(desc.mac_address, identity_only.mac_address);
    EXPECT_EQ(static_cast<uint8_t>(desc.priority1), 248);  // back to authored: the gPTP group is not valid now
}

TEST(avb_entity_host_avb_info, host_answers_get_avb_info_from_observed_state)
{
    using namespace statusbar::atdecc::aem::avb_info_flags;
    auto const blob = make_interface_blob();
    auto host = make_host_from_blob(blob);
    // Before the control plane is up: link down, no grandmaster observed.
    auto info = host->avb_info(0);
    EXPECT_TRUE((info.flags & GPTP_ENABLED) != 0);
    EXPECT_TRUE((info.flags & AVTP_DOWN_VALID) != 0);
    EXPECT_TRUE((info.flags & AVTP_DOWN) != 0);
    EXPECT_TRUE((info.flags & AS_CAPABLE) == 0);
    EXPECT_EQ(info.gptp_grandmaster_id, statusbar::ieee::Eui64{});
    EXPECT_EQ(info.propagation_delay, 0U);
    EXPECT_FALSE(host->mirror_target().has_value());

    host->set_propagation_delay_ns(1234);
    EXPECT_EQ(host->avb_info(0).propagation_delay, 1234U);

    // The AEM handler serves GET_AVB_INFO through the host's provider.
    std::array<uint8_t, 4> const request{0x00, 0x09, 0x00, 0x00};  // AVB_INTERFACE 0
    AemDu header{};
    header.init_command(AEM_COMMAND_GET_AVB_INFO, AemDu::AEM_DATA_LENGTH + 4);
    std::array<uint8_t, 256> out{};
    auto const r = host->components().aem_handler.handle_command(header, request, out);
    EXPECT_EQ(r.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(r.size, AemAvbInfoPayload::LENGTH);
    AemAvbInfoPayload payload{};
    statusbar::span_load(payload, std::span<uint8_t const>{out}.first(AemAvbInfoPayload::LENGTH));
    EXPECT_EQ(payload.propagation_delay.get(), 1234U);
    EXPECT_TRUE(payload.is_avtp_down());

    // An index the model does not author is NO_SUCH_DESCRIPTOR.
    std::array<uint8_t, 4> const other{0x00, 0x09, 0x00, 0x05};
    EXPECT_EQ(host->components().aem_handler.handle_command(header, other, out).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(avb_entity_host_stream_info, stopped_talker_reports_streaming_wait)
{
    // IEEE 1722.1-2021 Table 7-145: a stream "stopped with STOP_STREAMING
    // command" is reported as STREAMING_WAIT; a streaming one is not.
    using namespace statusbar::avb_entity;
    namespace stream_info_flags = statusbar::atdecc::aem::stream_info_flags;
    auto const blob = make_interface_blob();
    auto host = make_host_from_blob(blob);
    auto& talker = host->components().acmp_talker;
    statusbar::ieee::Eui64 const stream_id{0x70, 0xB3, 0xD5, 0xED, 0xC4, 0x00, 0x00, 0x01};
    statusbar::ieee::Eui48 const dest{0x91, 0xE0, 0xF0, 0x00, 0xFE, 0x01};
    EXPECT_TRUE(talker.get_stream(0) != nullptr);
    AvbEntityHost const& h = *host.operator->();
    (void)talker.configure_stream(0, stream_id, dest);

    statusbar::atdecc::aem::AemStreamInfoMilanExtension ext{};
    TalkerStreamStatus status{.milan = false, .presentation_offset_ns = 2'000'000};
    statusbar::atdecc::aem::AemStreamInfoPayload streaming{};
    EXPECT_TRUE(fill_talker_stream_info(h, 0, status, streaming, ext));
    EXPECT_TRUE((streaming.flags.get() & stream_info_flags::STREAMING_WAIT) == 0);
    EXPECT_TRUE((streaming.flags.get() & stream_info_flags::STREAM_ID_VALID) != 0);
    EXPECT_EQ(streaming.stream_id, stream_id);

    status.stopped = true;
    statusbar::atdecc::aem::AemStreamInfoPayload stopped{};
    EXPECT_TRUE(fill_talker_stream_info(h, 0, status, stopped, ext));
    EXPECT_TRUE((stopped.flags.get() & stream_info_flags::STREAMING_WAIT) != 0);
    EXPECT_EQ(stopped.stream_id, stream_id);

    // An index the talker does not have is declined either way.
    statusbar::atdecc::aem::AemStreamInfoPayload none{};
    EXPECT_FALSE(fill_talker_stream_info(h, 200, status, none, ext));

    // IEEE 1722.1-2021 Table 7-158: FRAMES_TX is STREAM_OUTPUT counter bit 7.
    EXPECT_EQ(stream_output_frames_tx_bit, 7U);
}

TEST(avb_entity_host_counters, entity_avb_interface_and_clock_domain)
{
    using statusbar::atdecc::aem::DESCRIPTOR_AVB_INTERFACE;
    using statusbar::atdecc::aem::DESCRIPTOR_CLOCK_DOMAIN;
    using statusbar::atdecc::aem::DESCRIPTOR_STREAM_INPUT;
    using TimePoint = statusbar::sm::TimePoint;
    auto const blob = make_interface_blob();
    auto host = make_host_from_blob(blob);
    std::array<uint32_t, 32> counters{};
    uint32_t valid = 0;

    // ENTITY: answered, but IEEE 1722.1 defines only entity-specific counters.
    EXPECT_TRUE(host->fill_counters(DESCRIPTOR_ENTITY, 0, valid, counters));
    EXPECT_EQ(valid, 0U);

    // AVB_INTERFACE: link transitions (not repeats) and grandmaster changes.
    host->on_link_up(TimePoint{});
    host->on_link_up(TimePoint{});
    host->on_link_down(TimePoint{});
    host->on_link_up(TimePoint{});
    statusbar::gptp::ClockIdentity const gm_a{0x0011'22FF'FE33'4455ULL};
    host->notify_grandmaster_changed(gm_a);  // the first: found, not changed
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'6677ULL});
    EXPECT_TRUE(host->fill_counters(DESCRIPTOR_AVB_INTERFACE, 0, valid, counters));
    EXPECT_EQ(valid, 0x23U);
    EXPECT_EQ(counters[0], 2U);  // LINK_UP
    EXPECT_EQ(counters[1], 1U);  // LINK_DOWN: up now, so one fewer
    EXPECT_EQ(counters[5], 1U);  // GPTP_GM_CHANGED
    EXPECT_FALSE(host->fill_counters(DESCRIPTOR_AVB_INTERFACE, 1, valid, counters));

    // The model has no CLOCK_DOMAIN: not ours to answer.
    EXPECT_FALSE(host->fill_counters(DESCRIPTOR_CLOCK_DOMAIN, 0, valid, counters));
    EXPECT_FALSE(host->fill_counters(DESCRIPTOR_STREAM_INPUT, 0, valid, counters));
}

// GET_AVB_INFO's AS_CAPABLE (IEEE 1722.1-2021 Clause 7.4.40.2) follows the gPTP stack's port asCapable once
// reported, whatever the entity's own gPTP lock.
TEST(avb_entity_host_avb_info, as_capable_follows_the_gptp_stack_s_port)
{
    using statusbar::atdecc::aem::avb_info_flags::AS_CAPABLE;
    using TimePoint = statusbar::sm::TimePoint;
    auto const blob = make_interface_blob();
    auto host = make_host_from_blob(blob);
    host->on_link_up(TimePoint{});
    EXPECT_TRUE((host->avb_info(0).flags & AS_CAPABLE) == 0);  // the entity has no gPTP lock
    host->notify_port_as_capable(true);
    EXPECT_TRUE((host->avb_info(0).flags & AS_CAPABLE) != 0);
    host->notify_port_as_capable(false);
    EXPECT_TRUE((host->avb_info(0).flags & AS_CAPABLE) == 0);
    host->notify_port_as_capable(true);
    host->on_link_down(TimePoint{});  // a link that is down is never asCapable
    EXPECT_TRUE((host->avb_info(0).flags & AS_CAPABLE) == 0);
    host->on_link_up(TimePoint{});
    host->notify_port_as_capable(std::nullopt);  // ptp4l gone: the entity's own state again
    EXPECT_TRUE((host->avb_info(0).flags & AS_CAPABLE) == 0);
}

// GPTP_GM_CHANGED (Milan v1.3 Section 5.3.6.3, IEEE 1722.1-2021 Clause 7.4.42.2.2) follows the grandmaster
// in force: with the Announces gone a grandmaster-capable entity is its own, so A lost and found again is two.
TEST(avb_entity_host_counters, a_grandmaster_lost_and_found_again_is_two_changes)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    statusbar::gptp::ClockIdentity const gm_a{0x0011'22FF'FE33'4455ULL};
    host->notify_grandmaster_changed(gm_a);
    EXPECT_EQ(gm_changed(host), 0U);  // the first
    host->notify_grandmaster_lost();
    EXPECT_EQ(gm_changed(host), 1U);  // to itself
    host->notify_grandmaster_changed(gm_a);
    EXPECT_EQ(gm_changed(host), 2U);
}

// priority1 255 is no grandmaster-capable entity (IEEE 802.1AS-2020 Clause 8.6.2.1): a loss leaves the
// last grandmaster in force, so only another one counts.
TEST(avb_entity_host_counters, a_grandmaster_lost_on_an_entity_not_capable_is_no_change)
{
    auto const blob = make_interface_blob(255);
    auto host = make_host_from_blob(blob);
    statusbar::gptp::ClockIdentity const gm_a{0x0011'22FF'FE33'4455ULL};
    host->notify_grandmaster_changed(gm_a);
    host->notify_grandmaster_lost();
    host->notify_grandmaster_changed(gm_a);
    EXPECT_EQ(gm_changed(host), 0U);
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'6677ULL});
    EXPECT_EQ(gm_changed(host), 1U);
}

// The gPTP stack's PARENT_DATA_SET outranks the Announce handler once reported: its own clock as
// grandmaster is a change like any other, and the Announce handler's reports no longer count.
TEST(avb_entity_host_counters, the_parent_data_set_outranks_the_announce_handler)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    auto const gm_a = statusbar::ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL);
    auto const self = statusbar::ieee::Eui64{}.from_uint64(0x70B3'D5FF'FEED'0001ULL);
    host->notify_parent_grandmaster(gm_a);
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'6677ULL});
    host->notify_grandmaster_lost();
    EXPECT_EQ(gm_changed(host), 0U);
    host->notify_parent_grandmaster(self);
    host->notify_parent_grandmaster(self);
    host->notify_parent_grandmaster(gm_a);
    EXPECT_EQ(gm_changed(host), 2U);
}

// GET_AVB_INFO's gptp_grandmaster_id is ADP's (IEEE 1722.1-2021 Clauses 6.2.2.16, 7.4.40.2): the grandmaster in
// force, through an Announce, a loss, the parent data set and an Announce while that is the source.
TEST(avb_entity_host_avb_info, get_avb_info_names_the_grandmaster_adp_names)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    auto& adp = host->components().adp_advertiser;
    auto const same = [&] {
        return host->avb_info(0).gptp_grandmaster_id.to_uint64() == adp.adpdu().gptp_grandmaster_id.to_uint64();
    };
    statusbar::gptp::ClockIdentity const gm_a{0x0011'22FF'FE33'4455ULL};
    host->notify_grandmaster_changed(gm_a);
    EXPECT_EQ(host->avb_info(0).gptp_grandmaster_id.to_uint64(), gm_a.get());
    EXPECT_TRUE(same());
    host->notify_grandmaster_lost();
    EXPECT_TRUE(same());
    auto const gm_b = statusbar::ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'6677ULL);
    host->notify_parent_grandmaster(gm_b);
    EXPECT_EQ(host->avb_info(0).gptp_grandmaster_id, gm_b);
    EXPECT_TRUE(same());
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'8899ULL});
    EXPECT_EQ(host->avb_info(0).gptp_grandmaster_id, gm_b);
    EXPECT_TRUE(same());
}

namespace {

/// GET_AVB_INFO's and ADP's gptp_grandmaster_id, which must be the same.
auto named_grandmaster(SymbolAwareHost& host) -> uint64_t
{
    auto const reported = host->avb_info(0).gptp_grandmaster_id.to_uint64();
    EXPECT_EQ(host->components().adp_advertiser.adpdu().gptp_grandmaster_id.to_uint64(), reported);
    return reported;
}

constexpr uint64_t OWN_CLOCK = 0x70B3'D5FF'FEED'0001ULL;  // make_interface_blob's clock_identity

}  // namespace

// Milan v1.3 Section 4.2.6.2.1: with nothing announced, as after a restart with no peer, a
// grandmaster-capable entity is its own grandmaster, and GET_AVB_INFO and ADP name its clock.
TEST(avb_entity_host_avb_info, an_entity_with_nothing_announced_names_its_own_clock)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    host->on_link_up(statusbar::sm::TimePoint{});
    EXPECT_EQ(named_grandmaster(host), OWN_CLOCK);
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL});
    EXPECT_EQ(named_grandmaster(host), uint64_t{0x0011'22FF'FE33'4455ULL});
    host->notify_grandmaster_lost();
    EXPECT_EQ(named_grandmaster(host), OWN_CLOCK);
}

// An entity that cannot be grandmaster (priority1 255) names none once its grandmaster is lost.
TEST(avb_entity_host_avb_info, an_entity_that_cannot_be_grandmaster_names_none_after_a_loss)
{
    auto const blob = make_interface_blob(255);
    auto host = make_host_from_blob(blob);
    host->on_link_up(statusbar::sm::TimePoint{});
    EXPECT_EQ(named_grandmaster(host), uint64_t{0});
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL});
    EXPECT_EQ(named_grandmaster(host), uint64_t{0x0011'22FF'FE33'4455ULL});
    host->notify_grandmaster_lost();
    EXPECT_EQ(named_grandmaster(host), uint64_t{0});
}

// A clientOnly gPTP stack following no grandmaster names none, whatever the Announces say; its
// parent data set's grandmaster once it reports one.
TEST(avb_entity_host_avb_info, a_parent_data_set_naming_none_names_none)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'4455ULL});
    host->use_parent_grandmaster();
    EXPECT_EQ(named_grandmaster(host), uint64_t{0});
    host->notify_grandmaster_lost();
    EXPECT_EQ(named_grandmaster(host), uint64_t{0});
    host->notify_parent_grandmaster(statusbar::ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'6677ULL));
    EXPECT_EQ(named_grandmaster(host), uint64_t{0x0011'22FF'FE33'6677ULL});
}

// The parent data set naming no grandmaster after A (a clientOnly gPTP stack that lost it): GET_AVB_INFO and
// ADP name none, and GPTP_GM_CHANGED compares the next with A, so A, none, A is no change and A, none, B one.
TEST(avb_entity_host_counters, a_parent_data_set_losing_its_grandmaster_names_none_and_keeps_the_count)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    auto const gm_a = statusbar::ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL);
    auto const gm_b = statusbar::ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'6677ULL);
    host->use_parent_grandmaster();
    host->notify_parent_grandmaster(gm_a);
    EXPECT_EQ(named_grandmaster(host), gm_a.to_uint64());
    host->notify_parent_grandmaster_loss();
    EXPECT_EQ(named_grandmaster(host), uint64_t{0});
    host->notify_parent_grandmaster(gm_a);
    EXPECT_EQ(named_grandmaster(host), gm_a.to_uint64());
    EXPECT_EQ(gm_changed(host), 0U);
    host->notify_parent_grandmaster_loss();
    EXPECT_EQ(named_grandmaster(host), uint64_t{0});
    EXPECT_EQ(gm_changed(host), 0U);
    host->notify_parent_grandmaster(gm_b);
    EXPECT_EQ(named_grandmaster(host), gm_b.to_uint64());
    EXPECT_EQ(gm_changed(host), 1U);
}

// use_parent_grandmaster after an Announce: the Announce handler's grandmaster in force is dropped, its
// grandmasters found or lost (the local ptp4l's own among them) count nothing, and the stack's first
// report is no change.
TEST(avb_entity_host_counters, use_parent_grandmaster_drops_the_announce_handler_s_grandmaster)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'7788ULL});  // before: in force
    host->use_parent_grandmaster();
    host->use_parent_grandmaster();                                                              // again: nothing more
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x70B3'D5FF'FEED'0001ULL});  // ptp4l's own Announces
    host->notify_grandmaster_lost();
    host->notify_grandmaster_changed(statusbar::gptp::ClockIdentity{0x0011'22FF'FE33'6677ULL});
    EXPECT_EQ(gm_changed(host), 0U);
    host->notify_parent_grandmaster(statusbar::ieee::Eui64{}.from_uint64(0x0011'22FF'FE33'4455ULL));
    EXPECT_EQ(gm_changed(host), 0U);  // the first
    host->notify_parent_grandmaster(statusbar::ieee::Eui64{}.from_uint64(0x70B3'D5FF'FEED'0001ULL));
    EXPECT_EQ(gm_changed(host), 1U);
}

// use_parent_grandmaster hands GPTP_GM_CHANGED to the gPTP stack before it names a grandmaster: a
// clientOnly ptp4l following none names none, and the Announce handler's reports count nothing.
TEST(avb_entity_host_counters, the_parent_data_set_can_be_the_source_before_its_first_report)
{
    auto const blob = make_interface_blob(248);
    auto host = make_host_from_blob(blob);
    host->use_parent_grandmaster();
    statusbar::gptp::ClockIdentity const gm_a{0x0011'22FF'FE33'4455ULL};
    host->notify_grandmaster_changed(gm_a);
    host->notify_grandmaster_lost();
    host->notify_grandmaster_changed(gm_a);
    EXPECT_EQ(gm_changed(host), 0U);
}

// A link back after a down (IEEE 802.1Q-2022 Clause 10.7): MSRP sends a LeaveAll at once, re-declaring the
// Domain, rather than waiting for its timers; the first link up waits for the join timer
TEST(avb_entity_host_link, a_link_back_after_a_down_sends_a_leave_all_at_once)
{
    using TimePoint = statusbar::sm::TimePoint;
    auto const blob = make_interface_blob();
    auto host = make_host_from_blob(blob);
    std::vector<std::vector<uint8_t>> sent;
    host->components().msrp_handler.set_callbacks(
        statusbar::nanoavb::MsrpCallbacks{.send_packet = [&sent](std::span<uint8_t const> p) {
            sent.emplace_back(p.begin(), p.end());
            return true;
        }});
    host->components().msrp_handler.tick(TimePoint{});  // hands the sender over
    host->on_link_up(TimePoint{});
    auto const first = sent.size();
    host->on_link_down(TimePoint{std::chrono::seconds{1}});
    host->on_link_up(TimePoint{std::chrono::seconds{2}});
    EXPECT_TRUE(sent.size() > first);  // the return after a down: a LeaveAll PDU at once, before any tick
    EXPECT_TRUE(!sent.empty() && (sent.back().size() > 4));
}

#ifdef __linux__

//
// GET_AVB_INFO's msrp_mappings on a live control plane - IEEE 1722.1-2021 Clause 7.4.40.2.1
//
// avb_info() maps a class only while the MSRP socket is open. A fork()ed child (unshare() needs a
// single-threaded process) opens the control plane on its own network namespace's loopback.

namespace {

/// What the child saw: POD, to cross the pipe.
struct MappingReport
{
    bool entered{false};  ///< the namespace was entered and its loopback brought up
    bool live{false};     ///< the control plane started with its MSRP socket open
    size_t mappings{0};
    uint8_t traffic_class{0};
};

/// The child's report, and whether it exited 0 with all of it within the probe's time.
struct ProbeResult
{
    bool finished{false};
    MappingReport report{};
};

constexpr int PROBE_TIMEOUT_MS = 5000;

auto write_own_map(char const* path, char const* line) -> bool
{
    FILE* const f = std::fopen(path, "we");
    if (f == nullptr) {
        return false;
    }
    bool const wrote = std::fputs(line, f) >= 0;
    return std::fclose(f) == 0 && wrote;
}

/// Root in a new user namespace over a new network namespace, with its loopback up.
auto enter_namespace_with_loopback_up() -> bool
{
    std::array<char, 32> uid_line{};
    std::array<char, 32> gid_line{};
    (void)std::snprintf(uid_line.data(), uid_line.size(), "0 %u 1", static_cast<unsigned>(::getuid()));
    (void)std::snprintf(gid_line.data(), gid_line.size(), "0 %u 1", static_cast<unsigned>(::getgid()));
    if (::unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
        return false;
    }
    (void)write_own_map("/proc/self/setgroups", "deny");
    if (!write_own_map("/proc/self/uid_map", uid_line.data()) || !write_own_map("/proc/self/gid_map", gid_line.data())) {
        return false;
    }
    int const control = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (control < 0) {
        return false;
    }
    ifreq request{};
    std::strncpy(request.ifr_name, "lo", sizeof(request.ifr_name) - 1);
    bool up = ::ioctl(control, SIOCGIFFLAGS, &request) == 0;
    request.ifr_flags = static_cast<short>(request.ifr_flags | IFF_UP);
    up = up && ::ioctl(control, SIOCSIFFLAGS, &request) == 0;
    (void)::close(control);
    return up;
}

/// In the child: the msrp_mapping a host declaring @p domain reports on the namespace's loopback.
void report_mapping(statusbar::nanoavb::DomainInfo const& domain, MappingReport& out)
{
    out.entered = enter_namespace_with_loopback_up();
    if (!out.entered) {
        return;
    }
    auto const blob = make_interface_blob();
    auto host = make_host_from_blob(blob);
    host->components().msrp_handler.set_domain(domain);
    statusbar::itc::StopToken stop{};
    statusbar::net::MessageReactor reactor{stop, statusbar::net::monotonic_ns, 0};
    if (!host->start_control_plane(reactor, "lo").has_value()) {
        return;
    }
    auto const info = host->avb_info(0);
    out.live = (info.flags & statusbar::atdecc::aem::avb_info_flags::SRP_ENABLED) != 0;
    out.mappings = info.msrp_mappings.size();
    out.traffic_class = info.msrp_mappings.empty() ? 0 : info.msrp_mappings[0].traffic_class;
}

/// The child's report through @p fd, if it comes within PROBE_TIMEOUT_MS.
auto read_report(int const fd, MappingReport& report) -> bool
{
    pollfd ready{.fd = fd, .events = POLLIN, .revents = 0};
    if (::poll(&ready, 1, PROBE_TIMEOUT_MS) != 1) {
        return false;
    }
    return ::read(fd, &report, sizeof(report)) == static_cast<ssize_t>(sizeof(report));
}

/// report_mapping() for @p domain, run in a child; a child that does not report in time is killed.
auto probe_mapping(statusbar::nanoavb::DomainInfo const& domain) -> ProbeResult
{
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) != 0) {
        return {};
    }
    pid_t const child = ::fork();
    if (child == 0) {
        MappingReport report{};
        report_mapping(domain, report);
        ::_exit(::write(fds[1], &report, sizeof(report)) == static_cast<ssize_t>(sizeof(report)) ? 0 : 1);
    }
    (void)::close(fds[1]);
    ProbeResult result{};
    bool const reported = child > 0 && read_report(fds[0], result.report);
    (void)::close(fds[0]);
    if (child <= 0) {
        return result;
    }
    if (!reported) {
        (void)::kill(child, SIGKILL);
    }
    int status = 0;
    bool const exited = ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    result.finished = reported && exited;
    return result;
}

}  // namespace

// Milan devices name a class by its SRclassID (6 class A, 5 class B), not 0 and 1.
TEST(avb_entity_host_avb_info, an_msrp_mapping_names_its_class_by_sr_class_id)
{
    auto const class_a = probe_mapping(statusbar::nanoavb::DomainInfo{});
    auto const class_b = probe_mapping(statusbar::nanoavb::CLASS_B_DOMAIN);
    EXPECT_TRUE(class_a.finished && class_b.finished);
    if (!class_a.report.entered || !class_b.report.entered) {
        SKIP("no user and network namespace for this uid");
    }
    EXPECT_TRUE(class_a.report.live && class_b.report.live);
    EXPECT_EQ(class_a.report.mappings, size_t{1});
    EXPECT_EQ(class_a.report.traffic_class, uint8_t{6});
    EXPECT_EQ(class_b.report.mappings, size_t{1});
    EXPECT_EQ(class_b.report.traffic_class, uint8_t{5});
}

#endif  // __linux__

TEST_MAIN(statusbar_avb_entity, avb_entity_host_test)
