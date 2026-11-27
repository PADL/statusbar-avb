// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// What EntityIdentityDescriptorHandler lays over a model's descriptors when
// they are read: the identity only the running entity knows.

#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"

#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_aem_blob_writer.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <optional>
#include <vector>

using namespace statusbar;
using namespace statusbar::avb_entity;
using atdecc::aem::DescriptorAvbInterface;
using atdecc::aem::DescriptorPtpInstance;

namespace {

constexpr ieee::Eui48 MAC{0x00, 0x1C, 0xAB, 0x00, 0x12, 0x34};
constexpr ieee::Eui64 MAC_CLOCK{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x12, 0x34};
constexpr ieee::Eui64 NAMED_CLOCK{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x07};
constexpr ieee::Eui64 ENTITY{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x12, 0x34};

/// A model of one AVB_INTERFACE and two PTP_INSTANCEs, both
/// GRANDMASTER_CAPABLE: instance 0 names no clock identity, as a model
/// leaves it, and instance 1 names NAMED_CLOCK.
auto model_blob() -> std::vector<uint8_t>
{
    nanoavb::AemBlobWriter writer;
    DescriptorAvbInterface const interface{};
    writer.add(0, atdecc::aem::DESCRIPTOR_AVB_INTERFACE, 0, make_const_span(interface).first(interface.wire_size()));
    for (uint16_t index = 0; index < 2; ++index) {
        DescriptorPtpInstance instance{};
        instance.descriptor_index = index;
        instance.clock_identity = index == 1 ? NAMED_CLOCK : ieee::Eui64{};
        instance.flags = atdecc::aem::PTP_INSTANCE_FLAG_GRANDMASTER_CAPABLE;
        instance.number_of_ptp_ports = 1;
        instance.base_ptp_port = index;
        writer.add(0, atdecc::aem::DESCRIPTOR_PTP_INSTANCE, index, make_const_span(instance).first(instance.wire_size()));
    }
    return writer.write();
}

/// The model read through the handler of an entity on @p mac, which lays its
/// interface's facts over the model when @p patch.
struct ReadModel
{
    std::vector<uint8_t> blob{model_blob()};
    EntityIdentityDescriptorHandler handler;

    ReadModel(std::optional<ieee::Eui48> const mac, bool const patch)
        : handler{*atdecc::aem::DescriptorStorage::create(blob), ENTITY, ENTITY, "1.0", "name", mac, patch}
    {}

    [[nodiscard]] auto instance(uint16_t const index) -> std::optional<DescriptorPtpInstance>
    {
        nanoavb::DescriptorId const id{
            .ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_PTP_INSTANCE, .descriptor_index = index}};
        DescriptorPtpInstance desc{};
        return handler.on_get_ptp_instance(id, desc) ? std::optional{desc} : std::nullopt;
    }

    [[nodiscard]] auto interface_clock() -> ieee::Eui64
    {
        nanoavb::DescriptorId const id{.ref = {.configuration_index = 0, .descriptor_type = atdecc::aem::DESCRIPTOR_AVB_INTERFACE}};
        DescriptorAvbInterface desc{};
        EXPECT_TRUE(handler.on_get_avb_interface(id, desc));
        return desc.clock_identity;
    }
};

}  // namespace

TEST(avb_entity_descriptor_helpers, a_ptp_instance_with_no_clock_identity_reads_with_the_interfaces)
{
    // IEEE 1722.1-2021 7.2.35, Milan v1.3 7.5.4.
    ReadModel model{MAC, true};
    auto const instance = model.instance(0);
    EXPECT_TRUE(instance.has_value());
    EXPECT_EQ(instance->clock_identity, MAC_CLOCK);
    EXPECT_EQ(instance->clock_identity, model.interface_clock());
    // What the model wrote is as it wrote it.
    EXPECT_EQ(instance->flags.get(), atdecc::aem::PTP_INSTANCE_FLAG_GRANDMASTER_CAPABLE);
    EXPECT_EQ(instance->number_of_ptp_ports.get(), 1);
    EXPECT_EQ(instance->descriptor_index.get(), 0);
}

TEST(avb_entity_descriptor_helpers, a_ptp_instance_that_names_its_clock_identity_keeps_it)
{
    ReadModel model{MAC, true};
    auto const instance = model.instance(1);
    EXPECT_TRUE(instance.has_value());
    EXPECT_EQ(instance->clock_identity, NAMED_CLOCK);
    EXPECT_EQ(instance->base_ptp_port.get(), 1);
}

TEST(avb_entity_descriptor_helpers, a_ptp_instance_is_as_the_model_has_it_without_an_interface_to_take_it_from)
{
    // No interface address known, and an entity told not to lay its interface over the model.
    ReadModel no_mac{std::nullopt, true};
    EXPECT_EQ(no_mac.instance(0)->clock_identity, ieee::Eui64{});
    ReadModel unpatched{MAC, false};
    EXPECT_EQ(unpatched.instance(0)->clock_identity, ieee::Eui64{});
    EXPECT_EQ(unpatched.instance(1)->clock_identity, NAMED_CLOCK);
}

TEST(avb_entity_descriptor_helpers, a_ptp_instance_the_model_lacks_is_not_read)
{
    ReadModel model{MAC, true};
    EXPECT_FALSE(model.instance(2).has_value());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_descriptor_helpers_test)
