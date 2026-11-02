// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/test/test.hpp"

#include <chrono>
#include <expected>
#include <system_error>

using namespace statusbar::nanoavb;

//
// Error Code Tests
//
TEST(nanoavb_base_error, error_names)
{
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::Success)), "Success");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::InvalidDescriptorType)), "Invalid Descriptor Type");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::InvalidDescriptorIndex)), "Invalid Descriptor Index");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::DescriptorNotFound)), "Descriptor Not Found");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::DescriptorStorageFull)), "Descriptor Storage Full");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::InvalidConfiguration)), "Invalid Configuration");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::EntityNotInitialized)), "Entity Not Initialized");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::CommandNotSupported)), "Command Not Supported");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::InvalidStreamIndex)), "Invalid Stream Index");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::StreamNotConnected)), "Stream Not Connected");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::StreamAlreadyConnected)), "Stream Already Connected");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::SrpRegistrationFailed)), "SRP Registration Failed");
    EXPECT_EQ(std::string(nanoavb_error_name(NanoAvbError::InvalidVlanId)), "Invalid VLAN ID");
}

TEST(nanoavb_base_error, error_code_creation)
{
    auto ec = make_error_code(NanoAvbError::InvalidDescriptorType);
    EXPECT_EQ(ec.value(), static_cast<int>(NanoAvbError::InvalidDescriptorType));
    EXPECT_EQ(std::string(ec.category().name()), "statusbar.nanoavb");
    EXPECT_EQ(ec.message(), "Invalid Descriptor Type");
}

TEST(nanoavb_base_error, success_is_zero)
{
    auto ec = make_error_code(NanoAvbError::Success);
    EXPECT_EQ(ec.value(), 0);
}

//
// Constants Tests
//
TEST(nanoavb_base_constants, default_max_values)
{
    EXPECT_TRUE(default_max_configurations >= 1);
    EXPECT_TRUE(default_max_audio_units >= 1);
    EXPECT_TRUE(default_max_stream_inputs >= 1);
    EXPECT_TRUE(default_max_stream_outputs >= 1);
    EXPECT_TRUE(default_max_avb_interfaces >= 1);
    EXPECT_TRUE(default_max_clock_sources >= 1);
    EXPECT_TRUE(default_max_clock_domains >= 1);
}

TEST(nanoavb_base_constants, entity_capabilities)
{
    // Check that default capabilities include essential features (use fully qualified names)
    namespace caps = statusbar::nanoavb::entity_capabilities;
    EXPECT_TRUE((caps::NANOAVB_DEFAULT & caps::AEM_SUPPORTED) != 0);
    EXPECT_TRUE((caps::NANOAVB_DEFAULT & caps::CLASS_A_SUPPORTED) != 0);
    EXPECT_TRUE((caps::NANOAVB_DEFAULT & caps::GPTP_SUPPORTED) != 0);
}

TEST(nanoavb_base_constants, talker_capabilities)
{
    namespace caps = statusbar::nanoavb::talker_capabilities;
    EXPECT_TRUE((caps::NANOAVB_AUDIO_TALKER & caps::IMPLEMENTED) != 0);
    EXPECT_TRUE((caps::NANOAVB_AUDIO_TALKER & caps::HAS_AUDIO_SOURCE) != 0);
}

TEST(nanoavb_base_constants, listener_capabilities)
{
    namespace caps = statusbar::nanoavb::listener_capabilities;
    EXPECT_TRUE((caps::NANOAVB_AUDIO_LISTENER & caps::IMPLEMENTED) != 0);
    EXPECT_TRUE((caps::NANOAVB_AUDIO_LISTENER & caps::HAS_AUDIO_SINK) != 0);
}

//
// SRP Constants Tests
//
TEST(nanoavb_base_srp, ethertype_values)
{
    EXPECT_EQ(srp::MVRP_ETHERTYPE, 0x88F5);
    EXPECT_EQ(srp::MSRP_ETHERTYPE, 0x22EA);
}

TEST(nanoavb_base_srp, multicast_addresses)
{
    EXPECT_EQ(srp::MVRP_MULTICAST_ADDRESS, 0x0180C2000021ULL);
    EXPECT_EQ(srp::MSRP_MULTICAST_ADDRESS, 0x0180C200000EULL);
}

TEST(nanoavb_base_srp, sr_class_defaults)
{
    EXPECT_EQ(srp::DEFAULT_SR_CLASS_A_VID, 2);
    EXPECT_EQ(srp::DEFAULT_SR_CLASS_B_VID, 2);
    EXPECT_EQ(srp::SR_CLASS_A_PRIORITY, 3);
    EXPECT_EQ(srp::SR_CLASS_B_PRIORITY, 2);
}

//
// NanoAvbComponentsBuilder Tests
//
TEST(nanoavb_base_builder, build_without_model_fails)
{
    // Building without setting entity model should fail
    auto result = NanoAvbComponentsBuilder{}.build();

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), make_error_code(NanoAvbError::InvalidConfiguration));
}

TEST(nanoavb_base_builder, build_with_invalid_vlan_fails)
{
    using namespace statusbar::atdecc::aem;

    EntityModel model{EntityModelConfig{}};
    DescriptorEntity entity{};
    entity.entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    model.set_entity(entity);

    // VLAN 0 is invalid
    auto result = NanoAvbComponentsBuilder{}.with_entity_model(std::move(model)).with_default_vlan(0).build();

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), make_error_code(NanoAvbError::InvalidVlanId));
}

TEST(nanoavb_base_builder, build_with_model_succeeds)
{
    using namespace statusbar::atdecc::aem;

    EntityModel model{EntityModelConfig{}};
    DescriptorEntity entity{};
    entity.entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    model.set_entity(entity);

    auto result = NanoAvbComponentsBuilder{}.with_entity_model(std::move(model)).build();

    EXPECT_TRUE(result.has_value());
}

TEST(nanoavb_base_builder, full_configuration)
{
    using namespace statusbar::atdecc::aem;

    EntityModel model{EntityModelConfig{.max_configurations = 1, .max_stream_inputs = 2, .max_stream_outputs = 2}};

    DescriptorEntity entity{};
    entity.entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    entity.talker_stream_sources = 2;
    entity.listener_stream_sinks = 2;
    model.set_entity(entity);

    auto result = NanoAvbComponentsBuilder{}
                      .with_entity_model(std::move(model))
                      .with_adp_config(AdpAdvertiserConfig{.valid_time = 31})
                      .with_acmp_talker_streams(2, 4)
                      .with_acmp_listener_streams(2)
                      .with_default_vlan(100)
                      .with_sr_class(DomainInfo{6, 3, 100})
                      .build();

    EXPECT_TRUE(result.has_value());
    // build() now yields a unique_ptr<NanoAvbComponents> (non-movable type):
    // deref the StatusValue, then the unique_ptr.
    EXPECT_EQ((*result)->acmp_talker.max_streams(), 2);
    EXPECT_EQ((*result)->acmp_listener.max_streams(), 2);
    EXPECT_EQ((*result)->adp_advertiser.valid_time(), 31);
}

// A Milan entity runs MSRP and MVRP with Milan's timers (Milan v1.2 Section 4.2.7.1.1); a
// classic one keeps IEEE 802.1Q-2018's.
TEST(nanoavb_base_builder, the_profile_picks_the_msrp_timers)
{
    using namespace statusbar::atdecc::aem;
    auto const components = [](bool const milan) {
        EntityModel model{EntityModelConfig{}};
        model.set_entity(DescriptorEntity{});
        return std::make_unique<NanoAvbComponents>(std::move(model), AdpAdvertiserConfig{}, 1, 1, 1, milan);
    };
    auto const milan = components(true);
    EXPECT_TRUE(milan->msrp_handler.msrp_config().timers == MILAN_MRP_TIMERS);
    auto const classic = components(false);
    EXPECT_TRUE(classic->msrp_handler.msrp_config().timers == statusbar::srp::mrp::TimerConfig{});
    EXPECT_EQ(classic->msrp_handler.msrp_config().timers.leave_time.count(), 1000);
    EXPECT_TRUE(milan->mvrp_handler.mvrp_config().timers == MILAN_MRP_TIMERS);
    EXPECT_TRUE(classic->mvrp_handler.mvrp_config().timers == statusbar::srp::mrp::TimerConfig{});
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_base_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_base_test");
    return result;
}