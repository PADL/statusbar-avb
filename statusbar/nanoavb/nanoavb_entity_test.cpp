// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

//
// Helper Functions
//
static EntityModel create_test_model()
{
    DescriptorEntity entity{};
    entity.entity_name = AtdeccString{"Test Entity"};
    entity.configurations_count = 1;

    DescriptorConfiguration config{};
    config.object_name = AtdeccString{"Default"};

    DescriptorStream stream_in{};
    stream_in.object_name = AtdeccString{"Audio In"};

    return EntityModelBuilder{}.entity(entity).configuration(config).stream_input(stream_in).build();
}

static AemDu create_aem_header(uint16_t command_code)
{
    AemDu header{};
    header.init_command(command_code, 12);  // 12 bytes for controller entity id + sequence id + command type
    return header;
}

//
// Test helper: wrap AemCommandHandler::handle_command with a locally-owned
// output buffer and expose an object shape matching the old AemCommandResult,
// so existing test assertions (result.status, result.response_data()) keep
// working after the output-buffer API refactor.
//
struct TestCommandResult
{
    uint8_t status{AEM_STATUS_SUCCESS};
    std::vector<uint8_t> bytes;

    [[nodiscard]] auto response_data() const noexcept -> std::span<uint8_t const> { return bytes; }
};

static auto make_test_result(AemCommandHandler& h, AemDu const& header, std::span<uint8_t const> command_data) -> TestCommandResult
{
    std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> buf{};
    auto const response = h.handle_command(header, command_data, buf);
    return {.status = response.status, .bytes = std::vector<uint8_t>(buf.data(), buf.data() + response.size)};
}

//
// Entity Available Tests
//
TEST(nanoavb_entity_aem, entity_available)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_ENTITY_AVAILABLE);
    auto result = make_test_result(handler, header, {});

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(result.response_data().empty());
}

//
// Read Descriptor Tests
//
TEST(nanoavb_entity_commands, read_entity_descriptor)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);

    // Build READ_DESCRIPTOR command data
    std::array<uint8_t, 8> command_data = {
        0x00,
        0x00,  // Configuration index
        0x00,
        0x00,  // Reserved
        0x00,
        0x00,  // Descriptor type = ENTITY
        0x00,
        0x00  // Descriptor index = 0
    };

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    // Response = 4-byte READ_DESCRIPTOR header (configuration_index + reserved)
    // + entity descriptor. The descriptor carries its own descriptor_type +
    // descriptor_index, so the header is 4 bytes, not 8.
    EXPECT_EQ(result.response_data().size(), AemReadDescriptorResponsePayload::LENGTH + sizeof(DescriptorEntity));
}

TEST(nanoavb_entity_commands, read_configuration_descriptor)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);

    // Build READ_DESCRIPTOR command data for CONFIGURATION
    std::array<uint8_t, 8> command_data = {
        0x00,
        0x00,  // Configuration index
        0x00,
        0x00,  // Reserved
        0x00,
        0x01,  // Descriptor type = CONFIGURATION
        0x00,
        0x00  // Descriptor index = 0
    };

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    // DescriptorConfiguration now has an inline descriptor_counts trailer,
    // so sizeof() is larger than the wire size. The response body is the
    // 4-byte READ_DESCRIPTOR header + wire_size() = LENGTH for an empty
    // config (descriptor_counts_count == 0).
    EXPECT_EQ(result.response_data().size(), AemReadDescriptorResponsePayload::LENGTH + DescriptorConfiguration::LENGTH);
}

TEST(nanoavb_entity_commands, read_stream_input_descriptor)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);

    // Build READ_DESCRIPTOR command data for STREAM_INPUT
    std::array<uint8_t, 8> command_data = {
        0x00,
        0x00,  // Configuration index
        0x00,
        0x00,  // Reserved
        0x00,
        0x05,  // Descriptor type = STREAM_INPUT
        0x00,
        0x00  // Descriptor index = 0
    };

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    // DescriptorStream now carries an inline stream_formats trailer,
    // so sizeof() is larger than the on-wire size. For a freshly
    // constructed stream with number_of_formats = 0, wire_size() equals
    // LENGTH (138 bytes), so the full response is 4 (READ_DESCRIPTOR
    // header) + 138.
    EXPECT_EQ(result.response_data().size(), AemReadDescriptorResponsePayload::LENGTH + DescriptorStream::LENGTH);
}

TEST(nanoavb_entity_legacy_2016, stream_input_descriptor_truncated_to_2016)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_legacy_2016(true);  // emit 1722.1-2013/2016 descriptor lengths

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);
    std::array<uint8_t, 8> command_data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00};  // STREAM_INPUT[0]
    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    // 2016 mode truncates the stream descriptor from 138 (2021) to 132 (2013);
    // this stream has no stream_formats trailer, so the body is exactly 132.
    EXPECT_EQ(result.response_data().size(), AemReadDescriptorResponsePayload::LENGTH + DescriptorStream::MINIMUM_LENGTH);

    // formats_offset (descriptor bytes 82..83, big-endian) is rewritten to 132
    // so a 2016 controller finds the (empty) stream_formats array correctly.
    auto const resp = result.response_data();
    size_t const fo = AemReadDescriptorResponsePayload::LENGTH + 82;
    ieee::doublet_t formats_offset{0};
    span_load(formats_offset, resp.subspan(fo, 2));
    EXPECT_EQ(formats_offset.get(), static_cast<uint16_t>(DescriptorStream::MINIMUM_LENGTH));
}

TEST(nanoavb_entity_legacy_2016, default_emits_2021_stream_length)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};  // default: legacy_2016 off

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);
    std::array<uint8_t, 8> command_data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00};  // STREAM_INPUT[0]
    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(result.response_data().size(), AemReadDescriptorResponsePayload::LENGTH + DescriptorStream::LENGTH);
}

TEST(nanoavb_entity_commands, read_invalid_descriptor)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);

    // Request a descriptor that doesn't exist
    std::array<uint8_t, 8> command_data = {
        0x00,
        0x00,  // Configuration index
        0x00,
        0x00,  // Reserved
        0x00,
        0x05,  // Descriptor type = STREAM_INPUT
        0x00,
        0x63  // Descriptor index = 99 (invalid)
    };

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_commands, read_descriptor_bad_arguments)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_READ_DESCRIPTOR);

    // Too short command data
    std::array<uint8_t, 4> command_data = {0x00, 0x00, 0x00, 0x00};

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_BAD_ARGUMENTS);
}

//
// Acquire Entity Tests
//
TEST(nanoavb_entity_acquire, acquire_success)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    EXPECT_FALSE(handler.is_acquired());

    auto header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    // Build ACQUIRE_ENTITY command data
    std::array<uint8_t, 16> command_data = {
        0x00,
        0x00,
        0x00,
        0x00,  // Flags
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,  // Owner ID
        0x00,
        0x00,  // Descriptor type = ENTITY
        0x00,
        0x00  // Descriptor index = 0
    };

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(handler.is_acquired());
    EXPECT_EQ(handler.acquiring_controller(), header.controller_entity_id);
}

TEST(nanoavb_entity_acquire, acquire_already_acquired)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header1 = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header1.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    auto header2 = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header2.controller_entity_id = statusbar::ieee::Eui64{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};

    std::array<uint8_t, 16> command_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // First acquire succeeds
    auto result1 = make_test_result(handler, header1, command_data);
    EXPECT_EQ(result1.status, AEM_STATUS_SUCCESS);

    // Second acquire fails (different controller)
    auto result2 = make_test_result(handler, header2, command_data);
    EXPECT_EQ(result2.status, AEM_STATUS_ENTITY_ACQUIRED);

    // Same controller acquire succeeds
    auto result3 = make_test_result(handler, header1, command_data);
    EXPECT_EQ(result3.status, AEM_STATUS_SUCCESS);
}

// Regression (nano#1): while acquired by controller A, a mutating command from
// controller B must be rejected with ENTITY_ACQUIRED; the owner A is not blocked.
TEST(nanoavb_entity_acquire, mutating_command_blocked_while_acquired_by_other)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    statusbar::ieee::Eui64 const controller_a{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    statusbar::ieee::Eui64 const controller_b{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};

    // A acquires the entity (all-zero payload = acquire).
    auto acq = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    acq.controller_entity_id = controller_a;
    std::array<uint8_t, 16> const acquire_data{};
    EXPECT_EQ(make_test_result(handler, acq, acquire_data).status, AEM_STATUS_SUCCESS);

    // B's SET_CONTROL / SET_NAME are rejected before any mutation.
    std::array<uint8_t, 8> const set_data{};
    auto set_ctrl_b = create_aem_header(AEM_COMMAND_SET_CONTROL);
    set_ctrl_b.controller_entity_id = controller_b;
    EXPECT_EQ(make_test_result(handler, set_ctrl_b, set_data).status, AEM_STATUS_ENTITY_ACQUIRED);
    auto set_name_b = create_aem_header(AEM_COMMAND_SET_NAME);
    set_name_b.controller_entity_id = controller_b;
    EXPECT_EQ(make_test_result(handler, set_name_b, set_data).status, AEM_STATUS_ENTITY_ACQUIRED);

    // The owner A is NOT blocked (whatever the handler returns, it is not ACQUIRED/LOCKED).
    auto set_ctrl_a = create_aem_header(AEM_COMMAND_SET_CONTROL);
    set_ctrl_a.controller_entity_id = controller_a;
    auto const owner = make_test_result(handler, set_ctrl_a, set_data);
    EXPECT_NE(owner.status, AEM_STATUS_ENTITY_ACQUIRED);
    EXPECT_NE(owner.status, AEM_STATUS_ENTITY_LOCKED);
}

// Regression (nano#1): while locked by controller A, a mutating command from
// controller B must be rejected with ENTITY_LOCKED; the owner A is not blocked.
TEST(nanoavb_entity_lock, mutating_command_blocked_while_locked_by_other)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    statusbar::ieee::Eui64 const controller_a{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    statusbar::ieee::Eui64 const controller_b{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};

    auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock.controller_entity_id = controller_a;
    std::array<uint8_t, 16> const lock_data{};  // no UNLOCK flag = lock
    EXPECT_EQ(make_test_result(handler, lock, lock_data).status, AEM_STATUS_SUCCESS);

    std::array<uint8_t, 8> const set_data{};
    auto set_ctrl_b = create_aem_header(AEM_COMMAND_SET_CONTROL);
    set_ctrl_b.controller_entity_id = controller_b;
    EXPECT_EQ(make_test_result(handler, set_ctrl_b, set_data).status, AEM_STATUS_ENTITY_LOCKED);

    auto set_ctrl_a = create_aem_header(AEM_COMMAND_SET_CONTROL);
    set_ctrl_a.controller_entity_id = controller_a;
    EXPECT_NE(make_test_result(handler, set_ctrl_a, set_data).status, AEM_STATUS_ENTITY_LOCKED);
}

TEST(nanoavb_entity_acquire, release_acquisition)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    // Acquire first
    std::array<uint8_t, 16> acquire_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    (void)make_test_result(handler, header, acquire_data);
    EXPECT_TRUE(handler.is_acquired());

    // Release
    std::array<uint8_t, 16> release_data = {
        0x80,
        0x00,
        0x00,
        0x00,  // RELEASE flag
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00};

    auto result = make_test_result(handler, header, release_data);
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_FALSE(handler.is_acquired());
}

TEST(nanoavb_entity_acquire, controller_available_handshake_owner_alive)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto controller_a = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    auto controller_b = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08};

    // Controller A acquires
    auto header_a = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header_a.controller_entity_id = controller_a;
    std::array<uint8_t, 16> acquire_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto now = sm::TimePoint{};
    handler.set_event_time(now);
    (void)make_test_result(handler, header_a, acquire_data);
    EXPECT_TRUE(handler.is_acquired());

    // Set up CONTROLLER_AVAILABLE callback
    bool controller_available_sent = false;
    Eui64 controller_available_target{};
    handler.send_controller_available = [&](Eui64 const& owner_id) {
        controller_available_sent = true;
        controller_available_target = owner_id;
        return true;
    };

    // Controller B tries to acquire
    auto header_b = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header_b.controller_entity_id = controller_b;
    handler.set_event_time(now + std::chrono::milliseconds(100));
    auto result = make_test_result(handler, header_b, acquire_data);

    // Should send CONTROLLER_AVAILABLE to A and return IN_PROGRESS
    EXPECT_TRUE(controller_available_sent);
    EXPECT_TRUE(controller_available_target == controller_a);
    EXPECT_EQ(result.status, AEM_STATUS_IN_PROGRESS);
    EXPECT_TRUE(handler.has_pending_acquire());

    // A responds (is alive) - B should be denied
    handler.controller_available_response_received();
    EXPECT_FALSE(handler.has_pending_acquire());
    EXPECT_TRUE(handler.is_acquired());
    EXPECT_TRUE(handler.acquiring_controller() == controller_a);  // A still owns
}

TEST(nanoavb_entity_acquire, controller_available_handshake_owner_timeout)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto controller_a = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    auto controller_b = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08};

    auto header_a = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header_a.controller_entity_id = controller_a;
    std::array<uint8_t, 16> acquire_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto now = sm::TimePoint{};
    handler.set_event_time(now);
    (void)make_test_result(handler, header_a, acquire_data);

    handler.send_controller_available = [](Eui64 const&) { return true; };

    auto header_b = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header_b.controller_entity_id = controller_b;
    handler.set_event_time(now + std::chrono::milliseconds(100));
    (void)make_test_result(handler, header_b, acquire_data);
    EXPECT_TRUE(handler.has_pending_acquire());

    // Timeout via tick - acquisition transfers to B
    handler.tick(now + std::chrono::milliseconds(400));
    EXPECT_FALSE(handler.has_pending_acquire());
    EXPECT_TRUE(handler.is_acquired());
    EXPECT_TRUE(handler.acquiring_controller() == controller_b);
}

TEST(nanoavb_entity_acquire, persistent_skips_controller_available)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto controller_a = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    auto controller_b = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08};

    // Controller A acquires with PERSISTENT flag
    auto header_a = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header_a.controller_entity_id = controller_a;
    std::array<uint8_t, 16> persistent_data = {
        0x00,
        0x00,
        0x00,
        0x01,  // PERSISTENT flag
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00};

    handler.set_event_time(sm::TimePoint{});
    (void)make_test_result(handler, header_a, persistent_data);

    bool controller_available_sent = false;
    handler.send_controller_available = [&](Eui64 const&) {
        controller_available_sent = true;
        return true;
    };

    // Controller B tries to acquire (non-persistent)
    auto header_b = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header_b.controller_entity_id = controller_b;
    std::array<uint8_t, 16> acquire_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto result = make_test_result(handler, header_b, acquire_data);

    // PERSISTENT acquisition should deny immediately without CONTROLLER_AVAILABLE
    EXPECT_FALSE(controller_available_sent);
    EXPECT_EQ(result.status, AEM_STATUS_ENTITY_ACQUIRED);
    EXPECT_FALSE(handler.has_pending_acquire());
}

//
// Unsolicited Notification Tests
//
TEST(nanoavb_entity_unsolicited, register_success)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    auto result = make_test_result(handler, header, {});
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(handler.unsolicited_registration_count(), 1U);
}

TEST(nanoavb_entity_unsolicited, register_idempotent)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    (void)make_test_result(handler, header, {});
    auto result = make_test_result(handler, header, {});
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(handler.unsolicited_registration_count(), 1U);
}

TEST(nanoavb_entity_unsolicited, deregister_success)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    (void)make_test_result(handler, header, {});
    EXPECT_EQ(handler.unsolicited_registration_count(), 1U);

    auto dereg_header = create_aem_header(AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION);
    dereg_header.controller_entity_id = header.controller_entity_id;
    auto result = make_test_result(handler, dereg_header, {});
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(handler.unsolicited_registration_count(), 0U);
}

TEST(nanoavb_entity_unsolicited, remove_for_departed_controller)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto reg = create_aem_header(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
    auto const ctrl_a = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    auto const ctrl_b = statusbar::ieee::Eui64{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    reg.controller_entity_id = ctrl_a;
    (void)make_test_result(handler, reg, {});
    reg.controller_entity_id = ctrl_b;
    (void)make_test_result(handler, reg, {});
    EXPECT_EQ(handler.unsolicited_registration_count(), 2U);

    // A departed controller's registration is pruned; the other survives.
    EXPECT_EQ(handler.remove_unsolicited_registrations_for(ctrl_a), 1U);
    EXPECT_EQ(handler.unsolicited_registration_count(), 1U);

    // Removing an unregistered / already-removed id is a no-op.
    EXPECT_EQ(handler.remove_unsolicited_registrations_for(ctrl_a), 0U);
    EXPECT_EQ(handler.unsolicited_registration_count(), 1U);
}

TEST(nanoavb_entity_unsolicited, max_registrations)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    // Fill all slots
    for (size_t i = 0; i < AemCommandHandler::MAX_UNSOLICITED_REGISTRATIONS; ++i) {
        auto header = create_aem_header(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
        header.controller_entity_id = statusbar::ieee::Eui64{0, 0, 0, 0, 0, 0, 0, static_cast<uint8_t>(i + 1)};
        auto result = make_test_result(handler, header, {});
        EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    }

    EXPECT_EQ(handler.unsolicited_registration_count(), AemCommandHandler::MAX_UNSOLICITED_REGISTRATIONS);

    // One more should fail
    auto header = create_aem_header(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION);
    header.controller_entity_id = statusbar::ieee::Eui64{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    auto result = make_test_result(handler, header, {});
    EXPECT_EQ(result.status, AEM_STATUS_NO_RESOURCES);
}

//
// Lock Entity Tests
//
TEST(nanoavb_entity_lock, lock_success)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    EXPECT_FALSE(handler.is_locked());

    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    std::array<uint8_t, 16> command_data = {
        0x00,
        0x00,
        0x00,
        0x00,  // Flags (no UNLOCK)
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,  // Locked ID
        0x00,
        0x00,  // Descriptor type = ENTITY
        0x00,
        0x00  // Descriptor index = 0
    };

    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(handler.is_locked());
}

TEST(nanoavb_entity_lock, unlock_success)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    // Lock first
    std::array<uint8_t, 16> lock_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    (void)make_test_result(handler, header, lock_data);
    EXPECT_TRUE(handler.is_locked());

    // Unlock
    std::array<uint8_t, 16> unlock_data = {
        0x00,
        0x00,
        0x00,
        0x01,  // UNLOCK flag
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00};

    auto result = make_test_result(handler, header, unlock_data);
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_FALSE(handler.is_locked());
}

TEST(nanoavb_entity_lock, timeout_expiration)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    std::array<uint8_t, 16> lock_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto now = sm::TimePoint{};
    handler.set_event_time(now);
    (void)make_test_result(handler, header, lock_data);
    EXPECT_TRUE(handler.is_locked());

    // Tick at 59 seconds - still locked
    auto before_timeout = now + std::chrono::seconds(59);
    handler.tick(before_timeout);
    EXPECT_TRUE(handler.is_locked());

    // Tick at 61 seconds - lock expired
    auto after_timeout = now + std::chrono::seconds(61);
    handler.tick(after_timeout);
    EXPECT_FALSE(handler.is_locked());
}

TEST(nanoavb_entity_lock, refresh_extends_timeout)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    std::array<uint8_t, 16> lock_data = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    auto now = sm::TimePoint{};
    handler.set_event_time(now);
    (void)make_test_result(handler, header, lock_data);
    EXPECT_TRUE(handler.is_locked());

    // Refresh at 30 seconds
    auto refresh_time = now + std::chrono::seconds(30);
    handler.set_event_time(refresh_time);
    (void)make_test_result(handler, header, lock_data);

    // Original timeout (60s) should have been extended
    auto past_original = now + std::chrono::seconds(61);
    handler.tick(past_original);
    EXPECT_TRUE(handler.is_locked());  // Still locked because refresh extended it

    // Now past the refreshed timeout (30 + 60 = 90s)
    auto past_refreshed = now + std::chrono::seconds(91);
    handler.tick(past_refreshed);
    EXPECT_FALSE(handler.is_locked());
}

//
// Get Configuration Tests
//

namespace {

/// A 16-octet ACQUIRE_ENTITY / LOCK_ENTITY body: flags, zero owner, then the descriptor.
auto exclusive_access_body(uint32_t const flags, uint16_t const descriptor_type, uint16_t const descriptor_index)
    -> std::array<uint8_t, 16>
{
    std::array<uint8_t, 16> body{};
    body[0] = static_cast<uint8_t>(flags >> 24U);
    body[3] = static_cast<uint8_t>(flags & 0xFFU);
    body[12] = static_cast<uint8_t>(descriptor_type >> 8U);
    body[13] = static_cast<uint8_t>(descriptor_type & 0xFFU);
    body[14] = static_cast<uint8_t>(descriptor_index >> 8U);
    body[15] = static_cast<uint8_t>(descriptor_index & 0xFFU);
    return body;
}

}  // namespace

TEST(nanoavb_entity_acquire, acquire_of_a_descriptor_other_than_entity_is_not_supported)
{
    // 7.4.1.1: an entity that acquires only ENTITY answers NOT_SUPPORTED to any
    // other descriptor type, and takes nothing.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    auto const result = make_test_result(handler, header, exclusive_access_body(0, DESCRIPTOR_CONFIGURATION, 0));
    EXPECT_EQ(result.status, AEM_STATUS_NOT_SUPPORTED);
    EXPECT_FALSE(handler.is_acquired());
    // The response still echoes the descriptor it refused.
    EXPECT_EQ(result.response_data()[13], DESCRIPTOR_CONFIGURATION);

    // A release naming that descriptor is refused the same way.
    auto const release = make_test_result(handler, header, exclusive_access_body(0x00000001U, DESCRIPTOR_CONFIGURATION, 0));
    EXPECT_EQ(release.status, AEM_STATUS_NOT_SUPPORTED);
}

TEST(nanoavb_entity_lock, lock_of_a_descriptor_other_than_entity_is_not_supported)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    auto const result = make_test_result(handler, header, exclusive_access_body(0, DESCRIPTOR_STREAM_INPUT, 0));
    EXPECT_EQ(result.status, AEM_STATUS_NOT_SUPPORTED);
    EXPECT_FALSE(handler.is_locked());
    EXPECT_EQ(result.response_data()[13], DESCRIPTOR_STREAM_INPUT);
}

namespace {

/// A handler acquired by @p owner under @p mode, and the controller commands of the tests below.
struct AcquiredHandler
{
    statusbar::ieee::Eui64 const owner{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    statusbar::ieee::Eui64 const other{0xBB, 0, 0, 0, 0, 0, 0, 0x01};
    EntityModel model{create_test_model()};
    AemCommandHandler handler{model};

    explicit AcquiredHandler(AemCommandHandler::AcquireMode const mode)
    {
        handler.set_acquire_mode(mode);
        EXPECT_EQ(
            command(AEM_COMMAND_ACQUIRE_ENTITY, owner, exclusive_access_body(0, DESCRIPTOR_ENTITY, 0)).status, AEM_STATUS_SUCCESS);
    }

    auto command(uint16_t const type, statusbar::ieee::Eui64 const& controller, std::span<uint8_t const> const body)
        -> TestCommandResult
    {
        auto header = create_aem_header(type);
        header.controller_entity_id = controller;
        return make_test_result(handler, header, body);
    }

    /// A SET_CONTROL and a SET_NAME from @p controller: the two statuses answered.
    auto sets_from(statusbar::ieee::Eui64 const& controller) -> std::array<uint8_t, 2>
    {
        std::array<uint8_t, 8> const body{};
        return {command(AEM_COMMAND_SET_CONTROL, controller, body).status, command(AEM_COMMAND_SET_NAME, controller, body).status};
    }
};

}  // namespace

TEST(nanoavb_entity_acquire, a_shared_acquisition_is_granted_and_shuts_no_other_controller_out_of_a_set)
{
    AcquiredHandler shared{AemCommandHandler::AcquireMode::shared};
    AcquiredHandler exclusive{AemCommandHandler::AcquireMode::exclusive};
    EXPECT_TRUE(shared.handler.acquire_mode() == AemCommandHandler::AcquireMode::shared);

    // The other controller's SETs are answered as the owner's are, whatever that is for this model.
    EXPECT_TRUE(shared.sets_from(shared.other) == shared.sets_from(shared.owner));
    for (uint8_t const status : shared.sets_from(shared.other)) {
        EXPECT_NE(status, AEM_STATUS_ENTITY_ACQUIRED);
    }
    // The same two commands under the mode of the standard.
    for (uint8_t const status : exclusive.sets_from(exclusive.other)) {
        EXPECT_EQ(status, AEM_STATUS_ENTITY_ACQUIRED);
    }
}

TEST(nanoavb_entity_acquire, a_shared_acquisition_is_still_an_acquisition_to_whoever_asks_for_one)
{
    // The owner is kept and reported: another controller's ACQUIRE_ENTITY is not granted over it
    // (nobody carries the CONTROLLER_AVAILABLE question here), and the response names the owner.
    AcquiredHandler shared{AemCommandHandler::AcquireMode::shared};
    auto const contender = shared.command(AEM_COMMAND_ACQUIRE_ENTITY, shared.other, exclusive_access_body(0, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(contender.status, AEM_STATUS_ENTITY_ACQUIRED);
    EXPECT_EQ(contender.response_data()[11], 0x07);
    // Only the owner releases it.
    auto const release_by_other =
        shared.command(AEM_COMMAND_ACQUIRE_ENTITY, shared.other, exclusive_access_body(0x80000000U, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(release_by_other.status, AEM_STATUS_ENTITY_ACQUIRED);
    auto const release_by_owner =
        shared.command(AEM_COMMAND_ACQUIRE_ENTITY, shared.owner, exclusive_access_body(0x80000000U, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(release_by_owner.status, AEM_STATUS_SUCCESS);
}

TEST(nanoavb_entity_lock, under_a_shared_acquisition_any_controller_may_lock_and_the_lock_is_exclusive)
{
    AcquiredHandler shared{AemCommandHandler::AcquireMode::shared};
    auto const locked = shared.command(AEM_COMMAND_LOCK_ENTITY, shared.other, exclusive_access_body(0, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(locked.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(shared.handler.is_locked());
    // The lock is the exclusive access: it shuts the acquirer out like anyone else.
    for (uint8_t const status : shared.sets_from(shared.owner)) {
        EXPECT_EQ(status, AEM_STATUS_ENTITY_LOCKED);
    }
    for (uint8_t const status : shared.sets_from(shared.other)) {
        EXPECT_NE(status, AEM_STATUS_ENTITY_LOCKED);
    }
    auto const unlocked =
        shared.command(AEM_COMMAND_LOCK_ENTITY, shared.other, exclusive_access_body(0x00000001U, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(unlocked.status, AEM_STATUS_SUCCESS);
    EXPECT_FALSE(shared.handler.is_locked());
}

TEST(nanoavb_entity_lock, only_the_acquirer_may_lock_or_unlock_an_acquired_entity)
{
    // 7.4.2.2: while acquired, a LOCK_ENTITY from any other controller answers
    // ENTITY_ACQUIRED, lock and unlock alike; the acquirer's own lock proceeds.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    statusbar::ieee::Eui64 const owner{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    statusbar::ieee::Eui64 const other{0xBB, 0, 0, 0, 0, 0, 0, 0x01};

    auto acquire = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    acquire.controller_entity_id = owner;
    EXPECT_EQ(make_test_result(handler, acquire, exclusive_access_body(0, DESCRIPTOR_ENTITY, 0)).status, AEM_STATUS_SUCCESS);

    auto lock_from_other = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock_from_other.controller_entity_id = other;
    auto const refused = make_test_result(handler, lock_from_other, exclusive_access_body(0, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(refused.status, AEM_STATUS_ENTITY_ACQUIRED);
    EXPECT_FALSE(handler.is_locked());

    auto lock_from_owner = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock_from_owner.controller_entity_id = owner;
    EXPECT_EQ(
        make_test_result(handler, lock_from_owner, exclusive_access_body(0, DESCRIPTOR_ENTITY, 0)).status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(handler.is_locked());

    // The other controller cannot unlock it either, and the response names the holder.
    auto const unlock_refused =
        make_test_result(handler, lock_from_other, exclusive_access_body(0x00000001U, DESCRIPTOR_ENTITY, 0));
    EXPECT_EQ(unlock_refused.status, AEM_STATUS_ENTITY_ACQUIRED);
    EXPECT_TRUE(handler.is_locked());
    EXPECT_EQ(unlock_refused.response_data()[11], 0x07);
}

TEST(nanoavb_entity_config, get_configuration)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_current_configuration(0);

    auto header = create_aem_header(AEM_COMMAND_GET_CONFIGURATION);
    auto result = make_test_result(handler, header, {});

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(result.response_data().size(), 4);
    // Check configuration index is 0 (network byte order)
    EXPECT_EQ(result.response_data()[2], 0);
    EXPECT_EQ(result.response_data()[3], 0);
}

//
// Not Implemented Command Tests
//
TEST(nanoavb_entity_commands, unsupported_command)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_REBOOT);  // Not implemented
    auto result = make_test_result(handler, header, {});

    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
}

//
// Stub Command Tests
//
TEST(nanoavb_entity_stubs, set_control_not_implemented)
{
    // A model whose handler does not override the value hooks reports NOT_IMPLEMENTED
    // for a well-formed SET_CONTROL (descriptor_type + descriptor_index header).
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_SET_CONTROL);
    std::array<uint8_t, 4> const cmd{0, 0, 0, 0};  // CONTROL (type 0... index 0), no value
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(nanoavb_entity_stubs, get_control_on_a_control_the_model_lacks_is_no_such_descriptor)
{
    auto model = create_test_model();  // no CONTROL descriptor at all
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_GET_CONTROL);
    std::array<uint8_t, 4> const cmd{0x00, 0x1A, 0x00, 0x00};  // CONTROL 0
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

//
// Table 7-141: a GET on a descriptor the entity does not have answers
// NO_SUCH_DESCRIPTOR. One the entity has but serves no value for keeps
// NOT_IMPLEMENTED. The model here has a CLOCK_DOMAIN and a CLOCK_SOURCE at
// index 0 and nothing at index 1; it has no CONTROL, SIGNAL_SELECTOR, MATRIX
// or MIXER at any index.
//
static EntityModel create_clock_model()
{
    DescriptorEntity entity{};
    entity.configurations_count = 1;
    DescriptorConfiguration config{};
    DescriptorStream stream_in{};
    stream_in.object_name = AtdeccString{"Audio In"};
    DescriptorClockSource source{};
    DescriptorClockDomain domain{};
    return EntityModelBuilder{}
        .entity(entity)
        .configuration(config)
        .stream_input(stream_in)
        .clock_source(source)
        .clock_domain(domain)
        .build();
}

static void put_u16(std::span<uint8_t> bytes, size_t const at, uint16_t const value)
{
    doublet_t const v{value};
    span_store(bytes.subspan(at, 2), v);
}

static auto get_value_status(uint16_t const command, uint16_t const descriptor_type, uint16_t const descriptor_index) -> uint8_t
{
    auto model = create_clock_model();
    AemCommandHandler handler{model};
    std::array<uint8_t, 4> cmd{};
    put_u16(cmd, 0, descriptor_type);
    put_u16(cmd, 2, descriptor_index);
    return make_test_result(handler, create_aem_header(command), cmd).status;
}

TEST(nanoavb_entity_no_such_descriptor, get_control_on_a_missing_control)
{
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_CONTROL, DESCRIPTOR_CONTROL, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_CONTROL, DESCRIPTOR_CONTROL, 7), AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_no_such_descriptor, get_signal_selector_on_a_missing_selector)
{
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_SIGNAL_SELECTOR, DESCRIPTOR_SIGNAL_SELECTOR, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_no_such_descriptor, get_matrix_on_a_missing_matrix)
{
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_MATRIX, DESCRIPTOR_MATRIX, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_no_such_descriptor, get_mixer_on_a_missing_mixer)
{
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_MIXER, DESCRIPTOR_MIXER, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_no_such_descriptor, get_clock_source_tells_a_missing_domain_from_an_unserved_one)
{
    // CLOCK_DOMAIN 1 does not exist; CLOCK_DOMAIN 0 does, and the legacy
    // model adapter serves no clock-source value for it.
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_CLOCK_SOURCE, DESCRIPTOR_CLOCK_DOMAIN, 1), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(get_value_status(AEM_COMMAND_GET_CLOCK_SOURCE, DESCRIPTOR_CLOCK_DOMAIN, 0), AEM_STATUS_NOT_IMPLEMENTED);
}

static auto get_name_status(uint16_t const descriptor_type, uint16_t const descriptor_index, uint16_t const name_index) -> uint8_t
{
    auto model = create_clock_model();
    AemCommandHandler handler{model};
    std::array<uint8_t, 8> cmd{};
    put_u16(cmd, 0, descriptor_type);
    put_u16(cmd, 2, descriptor_index);
    put_u16(cmd, 4, name_index);
    return make_test_result(handler, create_aem_header(AEM_COMMAND_GET_NAME), cmd).status;
}

TEST(nanoavb_entity_no_such_descriptor, get_name_tells_a_missing_descriptor_from_an_unserved_name)
{
    EXPECT_EQ(get_name_status(DESCRIPTOR_STREAM_INPUT, 3, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(get_name_status(DESCRIPTOR_CONTROL, 0, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    // STREAM_INPUT 0 exists; the legacy model adapter serves no name for it,
    // so the answer stays NOT_IMPLEMENTED rather than becoming NO_SUCH_DESCRIPTOR.
    EXPECT_EQ(get_name_status(DESCRIPTOR_STREAM_INPUT, 0, 0), AEM_STATUS_NOT_IMPLEMENTED);
    // ENTITY 0 exists too; whatever the adapter answers for its name, it is
    // not "no such descriptor".
    EXPECT_TRUE(get_name_status(DESCRIPTOR_ENTITY, 0, 0) != AEM_STATUS_NO_SUCH_DESCRIPTOR);
    // A body too short to name a descriptor is a bad argument, as before.
    auto model = create_clock_model();
    AemCommandHandler handler{model};
    std::array<uint8_t, 4> const short_cmd{};
    EXPECT_EQ(make_test_result(handler, create_aem_header(AEM_COMMAND_GET_NAME), short_cmd).status, AEM_STATUS_BAD_ARGUMENTS);
}

TEST(nanoavb_entity_stubs, get_counters_not_implemented)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_GET_COUNTERS);
    auto result = make_test_result(handler, header, {});

    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(nanoavb_entity_get_counters, returns_counters_from_callback)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_counters(
        [](uint16_t descriptor_type, uint16_t descriptor_index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
            if (descriptor_type != DESCRIPTOR_STREAM_INPUT || descriptor_index != 0) {
                return false;
            }
            valid = (1U << 0) | (1U << 11);  // MEDIA_LOCKED + FRAMES_RX
            counters[0] = 1;                 // media_locked
            counters[11] = 4242;             // frames_rx
            return true;
        });

    // GET_COUNTERS command: descriptor_type=STREAM_INPUT(0x0005), index=0 (big-endian).
    std::array<uint8_t, AemGetCountersCommandPayload::LENGTH> cmd{0x00, 0x05, 0x00, 0x00};
    auto header = create_aem_header(AEM_COMMAND_GET_COUNTERS);
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(result.response_data().size(), sizeof(AemCountersPayload));

    AemCountersPayload resp{};
    span_load(resp, result.response_data().first(sizeof(AemCountersPayload)));
    EXPECT_EQ(resp.descriptor_type.get(), DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(resp.descriptor_index.get(), 0);
    EXPECT_EQ(resp.counters_valid.get(), (1U << 0) | (1U << 11));
    EXPECT_EQ(resp.counters[0].get(), 1U);
    EXPECT_EQ(resp.counters[11].get(), 4242U);
}

TEST(nanoavb_entity_get_counters, no_such_descriptor_when_callback_declines)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_counters([](uint16_t, uint16_t, uint32_t&, std::array<uint32_t, 32>&) -> bool { return false; });

    std::array<uint8_t, AemGetCountersCommandPayload::LENGTH> cmd{0x00, 0x05, 0x00, 0x09};  // index 9 (no such)
    auto header = create_aem_header(AEM_COMMAND_GET_COUNTERS);
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

//
// GET_STREAM_INFO Tests
//
TEST(nanoavb_entity_stubs, get_stream_info_not_implemented)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    // No get_stream_info callback wired => NOT_IMPLEMENTED (the gap that made the
    // the DSP processor tear down its FAST_CONNECT to the talker).
    std::array<uint8_t, AemGetStreamInfoCommandPayload::LENGTH> cmd{0x00, 0x06, 0x00, 0x00};  // STREAM_OUTPUT, idx 0
    auto header = create_aem_header(AEM_COMMAND_GET_STREAM_INFO);
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(nanoavb_entity_get_stream_info, returns_info_from_callback)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_stream_info(
        [](uint16_t descriptor_type, uint16_t descriptor_index, AemStreamInfoPayload& out, AemStreamInfoMilanExtension&) -> bool {
            if (descriptor_type != DESCRIPTOR_STREAM_OUTPUT || descriptor_index != 1) {
                return false;
            }
            out.stream_id = Eui64{0x02, 0x00, 0x00, 0xe0, 0x13, 0x60, 0x00, 0x01};
            out.stream_dest_mac = {0x91, 0xe0, 0xf0, 0x00, 0xfe, 0x01};
            out.stream_format = {0x02, 0x70, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00};
            out.stream_vlan_id = doublet_t{2};
            out.msrp_accumulated_latency = quadlet_t{2'000'000};
            out.flags = quadlet_t{
                stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_FORMAT_VALID |
                stream_info_flags::STREAM_DEST_MAC_VALID | stream_info_flags::STREAM_VLAN_ID_VALID |
                stream_info_flags::MSRP_ACC_LAT_VALID | stream_info_flags::CONNECTED};
            return true;
        });

    // GET_STREAM_INFO command: descriptor_type=STREAM_OUTPUT(0x0006), index=1.
    std::array<uint8_t, AemGetStreamInfoCommandPayload::LENGTH> cmd{0x00, 0x06, 0x00, 0x01};
    auto header = create_aem_header(AEM_COMMAND_GET_STREAM_INFO);
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(result.response_data().size(), AemStreamInfoPayload::LENGTH);

    AemStreamInfoPayload resp{};
    span_load(resp, result.response_data().first(AemStreamInfoPayload::LENGTH));
    EXPECT_EQ(resp.descriptor_type.get(), DESCRIPTOR_STREAM_OUTPUT);
    EXPECT_EQ(resp.descriptor_index.get(), 1);
    EXPECT_TRUE(resp.is_connected());
    EXPECT_TRUE(resp.is_stream_id_valid());
    EXPECT_TRUE(resp.is_stream_format_valid());
    EXPECT_EQ(resp.stream_id, (Eui64{0x02, 0x00, 0x00, 0xe0, 0x13, 0x60, 0x00, 0x01}));
    EXPECT_EQ(resp.stream_vlan_id.get(), 2);
    EXPECT_EQ(resp.msrp_accumulated_latency.get(), 2'000'000U);
}

TEST(nanoavb_entity_get_stream_info, no_such_descriptor_when_callback_declines)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_stream_info(
        [](uint16_t, uint16_t, AemStreamInfoPayload&, AemStreamInfoMilanExtension&) -> bool { return false; });

    std::array<uint8_t, AemGetStreamInfoCommandPayload::LENGTH> cmd{0x00, 0x05, 0x00, 0x09};  // STREAM_INPUT, idx 9
    auto header = create_aem_header(AEM_COMMAND_GET_STREAM_INFO);
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

//
// AECP Packet Processing Tests
//
TEST(nanoavb_entity_aecp, process_packet_read_descriptor)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    // Track sent responses
    std::vector<uint8_t> captured_response;
    Eui48 captured_dest{};

    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
        captured_dest = dest;
        captured_response.assign(resp.begin(), resp.end());
        return true;
    }});

    // Build AECP AEM READ_DESCRIPTOR command packet
    std::array<uint8_t, 32> packet{};
    AemDu cmd_header{};
    cmd_header.init_command(AEM_COMMAND_READ_DESCRIPTOR, 20);  // 12 + 8 command data
    cmd_header.target_entity_id = model.get_entity().entity_id;
    cmd_header.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    span_copy(make_span(packet).first(AemDu::LENGTH), make_const_span(cmd_header));

    // Add READ_DESCRIPTOR command data (entity descriptor)
    packet[24] = 0x00;
    packet[25] = 0x00;  // Config index
    packet[26] = 0x00;
    packet[27] = 0x00;  // Reserved
    packet[28] = 0x00;
    packet[29] = 0x00;  // Descriptor type = ENTITY
    packet[30] = 0x00;
    packet[31] = 0x00;  // Descriptor index = 0

    Eui48 src_mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    bool processed = handler.process_packet(src_mac, packet, model.get_entity().entity_id);

    EXPECT_TRUE(processed);
    EXPECT_EQ(captured_dest, src_mac);

    // Verify response was sent
    EXPECT_FALSE(captured_response.empty());

    // Verify response header
    AemDu resp_header{};
    span_copy(make_span(resp_header), std::span<uint8_t const>(captured_response).first(AemDu::LENGTH));
    EXPECT_TRUE(resp_header.is_response());
    EXPECT_EQ(resp_header.status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(resp_header.command_code(), AEM_COMMAND_READ_DESCRIPTOR);
}

TEST(nanoavb_entity_aecp, process_packet_wrong_target)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    bool callback_called = false;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const&, std::span<uint8_t const>) {
        callback_called = true;
        return true;
    }});

    // Build packet with wrong target_entity_id
    std::array<uint8_t, 32> packet{};
    AemDu cmd_header{};
    cmd_header.init_command(AEM_COMMAND_READ_DESCRIPTOR, 20);
    cmd_header.target_entity_id = Eui64{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // Wrong ID
    cmd_header.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    span_copy(make_span(packet).first(AemDu::LENGTH), make_const_span(cmd_header));

    Eui48 src_mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    bool processed = handler.process_packet(src_mac, packet, model.get_entity().entity_id);

    EXPECT_FALSE(processed);  // Should be ignored
    EXPECT_FALSE(callback_called);
}

TEST(nanoavb_entity_aecp, process_packet_too_short)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    bool callback_called = false;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const&, std::span<uint8_t const>) {
        callback_called = true;
        return true;
    }});

    // Packet too short to contain AemDu header
    std::array<uint8_t, 10> packet{};

    Eui48 src_mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    bool processed = handler.process_packet(src_mac, packet, model.get_entity().entity_id);

    EXPECT_FALSE(processed);
    EXPECT_FALSE(callback_called);
}

TEST(nanoavb_entity_aecp, process_packet_invalid_header)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    bool callback_called = false;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const&, std::span<uint8_t const>) {
        callback_called = true;
        return true;
    }});

    // Build packet with invalid subtype
    std::array<uint8_t, 32> packet{};
    AemDu cmd_header{};
    cmd_header.init_command(AEM_COMMAND_READ_DESCRIPTOR, 20);
    cmd_header.subtype = 0x00;  // Invalid - should be 0xFB for AECP
    cmd_header.target_entity_id = model.get_entity().entity_id;
    span_copy(make_span(packet).first(AemDu::LENGTH), make_const_span(cmd_header));

    Eui48 src_mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    bool processed = handler.process_packet(src_mac, packet, model.get_entity().entity_id);

    EXPECT_FALSE(processed);
    EXPECT_FALSE(callback_called);
}

TEST(nanoavb_entity_aecp, process_packet_no_callback)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    // No callbacks set

    std::array<uint8_t, 32> packet{};
    AemDu cmd_header{};
    cmd_header.init_command(AEM_COMMAND_READ_DESCRIPTOR, 20);
    cmd_header.target_entity_id = model.get_entity().entity_id;
    span_copy(make_span(packet).first(AemDu::LENGTH), make_const_span(cmd_header));

    Eui48 src_mac{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    bool processed = handler.process_packet(src_mac, packet, model.get_entity().entity_id);

    EXPECT_FALSE(processed);  // No callback to send response
}

// ===========================================================================
// release_acquisition / handle_controller_available / locking_controller
// ===========================================================================

TEST(nanoavb_entity_release, release_acquisition_clears_state)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::array<uint8_t, 16> data = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    (void)make_test_result(handler, header, data);
    EXPECT_TRUE(handler.is_acquired());
    handler.release_acquisition();
    EXPECT_FALSE(handler.is_acquired());
    EXPECT_TRUE(handler.acquiring_controller() == Eui64{});
}

TEST(nanoavb_entity_release, release_allows_new_acquire)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto ha = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    ha.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::array<uint8_t, 16> data = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    (void)make_test_result(handler, ha, data);
    handler.release_acquisition();
    auto hb = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    hb.controller_entity_id = Eui64{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    auto r = make_test_result(handler, hb, data);
    EXPECT_EQ(r.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(handler.acquiring_controller() == hb.controller_entity_id);
}

TEST(nanoavb_entity_controller_avail, returns_success)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto header = create_aem_header(AEM_COMMAND_CONTROLLER_AVAILABLE);
    auto result = make_test_result(handler, header, {});
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(result.response_data().empty());
}

TEST(nanoavb_entity_lock_ctrl, locking_controller_zero_when_unlocked)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    EXPECT_FALSE(handler.is_locked());
    EXPECT_TRUE(handler.locking_controller() == Eui64{});
}

TEST(nanoavb_entity_lock_ctrl, locking_controller_returns_locker)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto ctrl = Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = ctrl;
    std::array<uint8_t, 16> data = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    handler.set_event_time(sm::TimePoint{});
    (void)make_test_result(handler, header, data);
    EXPECT_TRUE(handler.is_locked());
    EXPECT_TRUE(handler.locking_controller() == ctrl);
}

TEST(nanoavb_entity_lock_ctrl, release_lock_clears_state)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    auto header = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    header.controller_entity_id = Eui64{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11};
    std::array<uint8_t, 16> data = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    handler.set_event_time(sm::TimePoint{});
    (void)make_test_result(handler, header, data);
    EXPECT_TRUE(handler.is_locked());
    handler.release_lock();
    EXPECT_FALSE(handler.is_locked());
    EXPECT_TRUE(handler.locking_controller() == Eui64{});
}

TEST(nanoavb_entity_commands, get_stream_format_implemented)
{
    // Regression: GET_STREAM_FORMAT (0x0009) must be implemented. When the entity
    // answers NOT_IMPLEMENTED, some controllers cannot finish
    // enumerating the stream's format and re-query the entity indefinitely
    // (an ~80 s AECP storm observed on the wire from one such controller).
    auto model = create_test_model();  // has STREAM_INPUT[0]
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_GET_STREAM_FORMAT);
    std::array<uint8_t, 4> command_data = {0x00, 0x05, 0x00, 0x00};  // STREAM_INPUT[0]
    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);  // GET_STREAM_INFO is implemented (not NOT_IMPLEMENTED)
    EXPECT_EQ(result.response_data().size(), AemStreamFormatPayload::LENGTH);  // 12
    EXPECT_EQ(result.response_data()[1], 0x05);                                // echoes descriptor_type = STREAM_INPUT
}

TEST(nanoavb_entity_commands, get_sampling_rate_implemented)
{
    // Regression: GET_SAMPLING_RATE (0x0015) must be implemented for the AUDIO_UNIT.
    DescriptorEntity entity{};
    entity.entity_name = AtdeccString{"SR Test"};
    entity.configurations_count = 1;
    DescriptorConfiguration config{};
    config.object_name = AtdeccString{"Default"};
    DescriptorAudioUnit au{};
    au.current_sampling_rate = 96000;  // 96 kHz
    auto model = EntityModelBuilder{}.entity(entity).configuration(config).audio_unit(au).build();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_GET_SAMPLING_RATE);
    std::array<uint8_t, 4> command_data = {0x00, 0x02, 0x00, 0x00};  // AUDIO_UNIT[0]
    auto result = make_test_result(handler, header, command_data);

    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);  // GET_STREAM_INFO is implemented (not NOT_IMPLEMENTED)
    EXPECT_EQ(result.response_data().size(), AemSamplingRatePayload::LENGTH);  // 8
    auto const r = result.response_data();
    atdecc::aem::AemSamplingRatePayload parsed{};
    span_load(parsed, r);
    EXPECT_EQ(parsed.sampling_rate.get(), 96000u);
}

//
// Unsolicited notification fan-out (IEEE 1722.1 9.6) — a control change is
// pushed to every registered controller, and the entity can change its own
// controls via apply_local_descriptor_value().
//

namespace {

/// Minimal handler that accepts every SET descriptor-value and records it, so the
/// fan-out path (which only fires on AEM_STATUS_SUCCESS) can be exercised.
class SetAcceptingHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> value) -> uint8_t override
    {
        last_command_type = command_type;
        last_value.assign(value.begin(), value.end());
        ++set_count;
        return AEM_STATUS_SUCCESS;
    }

    uint16_t last_command_type{0};
    std::vector<uint8_t> last_value;
    int set_count{0};
};

/// One captured unsolicited/solicited response off the send_response callback.
struct CapturedSend
{
    Eui48 dest{};
    std::vector<uint8_t> bytes;
};

/// Build + inject a REGISTER_UNSOLICITED_NOTIFICATION from a controller identified
/// by (controller_id, src_mac), so its MAC is recorded for later notifications.
void register_controller(AemCommandHandler& handler, Eui64 const& entity_id, Eui64 const& controller_id, Eui48 const& src_mac)
{
    AemDu cmd{};
    cmd.init_command(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, AemDu::AEM_DATA_LENGTH);
    cmd.target_entity_id = entity_id;
    cmd.controller_entity_id = controller_id;
    std::array<uint8_t, AemDu::LENGTH> packet{};
    span_copy(make_span(packet), make_const_span(cmd));
    (void)handler.process_packet(src_mac, packet, entity_id);
}

/// Build + inject a DEREGISTER_UNSOLICITED_NOTIFICATION from (controller_id, src_mac).
void deregister_controller(AemCommandHandler& handler, Eui64 const& entity_id, Eui64 const& controller_id, Eui48 const& src_mac)
{
    AemDu cmd{};
    cmd.init_command(AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, AemDu::AEM_DATA_LENGTH);
    cmd.target_entity_id = entity_id;
    cmd.controller_entity_id = controller_id;
    std::array<uint8_t, AemDu::LENGTH> packet{};
    span_copy(make_span(packet), make_const_span(cmd));
    (void)handler.process_packet(src_mac, packet, entity_id);
}

/// The AemDu header at the front of a captured send.
[[nodiscard]] auto captured_header(CapturedSend const& s) -> AemDu
{
    AemDu hdr{};
    span_copy(make_span(hdr), std::span<uint8_t const>(s.bytes).first(AemDu::LENGTH));
    return hdr;
}

}  // namespace

TEST(nanoavb_entity_unsolicited_notify, a_deregistration_is_told_to_the_others_under_classic_and_to_nobody_under_milan)
{
    // 1722.1-2021 7.5.2 lists DEREGISTER_UNSOLICITED_NOTIFICATION, so a classic
    // entity tells the remaining controllers. A Milan entity must not: v1.3
    // 5.4.5.1 has every copy carry the recipient's own controller_entity_id,
    // so B would receive a successful DEREGISTER addressed to B and take it
    // for its own deregistration (la_avdecc drops its subscription on it),
    // while the entity still holds B's registration. Table 5.20 sends a
    // deregistration only to the controller removed.
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x05};
    Eui48 const mac_a{0x02, 0, 0, 0, 0, 0x0A};
    Eui48 const mac_b{0x02, 0, 0, 0, 0, 0x0B};
    Eui64 const ctrl_a{0xC0, 0, 0, 0, 0, 0, 0, 0x0A};
    Eui64 const ctrl_b{0xC0, 0, 0, 0, 0, 0, 0, 0x0B};

    for (auto const profile : {AemCommandHandler::Profile::classic, AemCommandHandler::Profile::milan13}) {
        auto model = create_test_model();
        AemCommandHandler handler{model};
        handler.set_entity_id(entity_id);
        handler.set_profile(profile);
        std::vector<CapturedSend> sends;
        handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
            sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
            return true;
        }});
        register_controller(handler, entity_id, ctrl_a, mac_a);
        register_controller(handler, entity_id, ctrl_b, mac_b);
        EXPECT_EQ(handler.unsolicited_registration_count(), size_t{2});
        sends.clear();

        deregister_controller(handler, entity_id, ctrl_a, mac_a);
        EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});

        // A's own response, addressed to A, under both profiles.
        size_t to_a = 0;
        size_t unsolicited_to_b = 0;
        for (auto const& s : sends) {
            auto const hdr = captured_header(s);
            EXPECT_EQ(hdr.command_code(), AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION);
            EXPECT_EQ(hdr.status(), AEM_STATUS_SUCCESS);
            if (s.dest == mac_a && !hdr.is_unsolicited()) {
                EXPECT_EQ(hdr.controller_entity_id, ctrl_a);
                ++to_a;
            }
            if (s.dest == mac_b && hdr.is_unsolicited()) {
                // The copy that reads to B as B's own deregistration.
                EXPECT_EQ(hdr.controller_entity_id, ctrl_b);
                ++unsolicited_to_b;
            }
        }
        EXPECT_EQ(to_a, size_t{1});
        if (profile == AemCommandHandler::Profile::classic) {
            EXPECT_EQ(unsolicited_to_b, size_t{1});
            EXPECT_EQ(sends.size(), size_t{2});
        } else {
            EXPECT_EQ(unsolicited_to_b, size_t{0});
            EXPECT_EQ(sends.size(), size_t{1});
        }
    }
}

TEST(nanoavb_entity_unsolicited_notify, fan_out_to_all_registered_controllers)
{
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x01};
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_entity_id(entity_id);

    std::vector<CapturedSend> sends;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    }});

    Eui48 const mac_a{0x02, 0, 0, 0, 0, 0x0A};
    Eui48 const mac_b{0x02, 0, 0, 0, 0, 0x0B};
    register_controller(handler, entity_id, Eui64{0xC0, 0, 0, 0, 0, 0, 0, 0x0A}, mac_a);
    register_controller(handler, entity_id, Eui64{0xC0, 0, 0, 0, 0, 0, 0, 0x0B}, mac_b);
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{2});

    sends.clear();
    // SET_CONTROL body: descriptor_type(2)=CONTROL + descriptor_index(2)=0 + value.
    std::array<uint8_t, 6> body{0x00, 0x1A, 0x00, 0x00, 0xDE, 0xAD};
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body), AEM_STATUS_SUCCESS);

    // The handler applied the value once; both controllers were notified.
    EXPECT_EQ(app_handler.set_count, 1);
    EXPECT_EQ(sends.size(), size_t{2});

    bool saw_a = false;
    bool saw_b = false;
    for (auto const& s : sends) {
        AemDu hdr{};
        span_copy(make_span(hdr), std::span<uint8_t const>(s.bytes).first(AemDu::LENGTH));
        EXPECT_TRUE(hdr.is_response());
        EXPECT_TRUE(hdr.is_unsolicited());
        EXPECT_EQ(hdr.command_code(), AEM_COMMAND_SET_CONTROL);
        EXPECT_EQ(hdr.target_entity_id, entity_id);
        // Body (after the 24-byte AemDu header) echoes the SET payload.
        EXPECT_TRUE(s.bytes.size() >= AemDu::LENGTH + body.size());
        EXPECT_EQ(s.bytes[AemDu::LENGTH + 4], 0xDE);
        EXPECT_EQ(s.bytes[AemDu::LENGTH + 5], 0xAD);
        if (s.dest == mac_a) {
            saw_a = true;
        }
        if (s.dest == mac_b) {
            saw_b = true;
        }
    }
    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);
}

TEST(nanoavb_entity_unsolicited_notify, no_registrations_emits_nothing)
{
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x02};
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_entity_id(entity_id);

    int sends = 0;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const&, std::span<uint8_t const>) {
        ++sends;
        return true;
    }});

    std::array<uint8_t, 6> body{0x00, 0x1A, 0x00, 0x00, 0x01, 0x02};
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body), AEM_STATUS_SUCCESS);
    EXPECT_EQ(app_handler.set_count, 1);  // value applied
    EXPECT_EQ(sends, 0);                  // but nobody to notify
}

TEST(nanoavb_entity_unsolicited_notify, controller_set_notifies_subscribers)
{
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x03};
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};

    std::vector<CapturedSend> sends;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    }});

    Eui48 const sub_mac{0x02, 0, 0, 0, 0, 0x0C};
    register_controller(handler, entity_id, Eui64{0xC0, 0, 0, 0, 0, 0, 0, 0x0C}, sub_mac);

    // A different controller now SETs a control via the wire.
    sends.clear();
    Eui48 const setter_mac{0x02, 0, 0, 0, 0, 0xFF};
    AemDu cmd{};
    cmd.init_command(AEM_COMMAND_SET_CONTROL, AemDu::AEM_DATA_LENGTH + 6);
    cmd.target_entity_id = entity_id;
    cmd.controller_entity_id = Eui64{0xC0, 0, 0, 0, 0, 0, 0, 0xFF};
    std::array<uint8_t, AemDu::LENGTH + 6> packet{};
    span_copy(make_span(packet).first(AemDu::LENGTH), make_const_span(cmd));
    packet[AemDu::LENGTH + 1] = 0x1A;  // descriptor_type = CONTROL
    packet[AemDu::LENGTH + 4] = 0xBE;
    packet[AemDu::LENGTH + 5] = 0xEF;
    EXPECT_TRUE(handler.process_packet(setter_mac, packet, entity_id));

    // Expect a direct response to the setter AND an unsolicited notify to the subscriber.
    bool direct_to_setter = false;
    bool unsolicited_to_sub = false;
    for (auto const& s : sends) {
        AemDu hdr{};
        span_copy(make_span(hdr), std::span<uint8_t const>(s.bytes).first(AemDu::LENGTH));
        if (s.dest == setter_mac && !hdr.is_unsolicited()) {
            direct_to_setter = true;
        }
        if (s.dest == sub_mac && hdr.is_unsolicited() && hdr.command_code() == AEM_COMMAND_SET_CONTROL) {
            unsolicited_to_sub = true;
        }
    }
    EXPECT_TRUE(direct_to_setter);
    EXPECT_TRUE(unsolicited_to_sub);
}

TEST(nanoavb_entity_unsolicited_notify, milan13_tells_the_locking_controller_its_lock_expired)
{
    // Milan v1.3 5.4.5.3 leaves the controller whose command it is out of
    // that command's fan-out. The lock expiring later is nobody's command:
    // the controller that locked must hear it, or it never learns the lock
    // is gone. Before, the requester of the last command stayed excluded.
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x04};
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(entity_id);
    handler.set_profile(AemCommandHandler::Profile::milan13);

    std::vector<CapturedSend> sends;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    }});

    Eui48 const mac_a{0x02, 0, 0, 0, 0, 0x0A};
    Eui64 const ctrl_a{0xC0, 0, 0, 0, 0, 0, 0, 0x0A};
    register_controller(handler, entity_id, ctrl_a, mac_a);
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});

    auto const now = sm::TimePoint{} + std::chrono::seconds(10);
    handler.set_event_time(now);
    sends.clear();
    AemDu cmd{};
    cmd.init_command(AEM_COMMAND_LOCK_ENTITY, AemDu::AEM_DATA_LENGTH + AemLockEntityPayload::LENGTH);
    cmd.target_entity_id = entity_id;
    cmd.controller_entity_id = ctrl_a;
    std::array<uint8_t, AemDu::LENGTH + AemLockEntityPayload::LENGTH> packet{};
    span_copy(make_span(packet).first(AemDu::LENGTH), make_const_span(cmd));
    EXPECT_TRUE(handler.process_packet(mac_a, packet, entity_id));
    EXPECT_TRUE(handler.is_locked());

    // The LOCK itself: its response to A, and no unsolicited copy to A (5.4.5.3).
    size_t unsolicited_locks = 0;
    for (auto const& s : sends) {
        AemDu hdr{};
        span_copy(make_span(hdr), std::span<uint8_t const>(s.bytes).first(AemDu::LENGTH));
        if (hdr.is_unsolicited()) {
            ++unsolicited_locks;
        }
    }
    EXPECT_EQ(unsolicited_locks, size_t{0});

    // The lock expires on a tick, nobody's command: A is told.
    sends.clear();
    handler.tick(now + std::chrono::seconds(61));
    EXPECT_FALSE(handler.is_locked());
    bool a_told = false;
    for (auto const& s : sends) {
        AemDu hdr{};
        span_copy(make_span(hdr), std::span<uint8_t const>(s.bytes).first(AemDu::LENGTH));
        if (s.dest == mac_a && hdr.is_unsolicited() && hdr.command_code() == AEM_COMMAND_LOCK_ENTITY) {
            a_told = true;
        }
    }
    EXPECT_TRUE(a_told);
}

TEST(nanoavb_entity_unsolicited_notify, identify_control_change_hits_identify_multicast)
{
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x04};
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_entity_id(entity_id);
    handler.set_identify_control_index(3);  // CONTROL index 3 is the IDENTIFY control

    std::vector<CapturedSend> sends;
    std::vector<bool> identify_changes;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    }});
    handler.set_identify_changed([&](bool const active) { identify_changes.push_back(active); });

    // No registered controllers — but an IDENTIFY change still reaches the multicast.
    // SET_CONTROL body: descriptor_type=CONTROL(0x001A), descriptor_index=3, value=0x01.
    std::array<uint8_t, 5> body{0x00, 0x1A, 0x00, 0x03, 0x01};
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body), AEM_STATUS_SUCCESS);

    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(sends[0].dest, atdecc::ATDECC_IDENTIFY_MULTICAST_MAC);
    AemDu hdr{};
    span_copy(make_span(hdr), std::span<uint8_t const>(sends[0].bytes).first(AemDu::LENGTH));
    EXPECT_TRUE(hdr.is_unsolicited());
    EXPECT_EQ(hdr.command_code(), AEM_COMMAND_SET_CONTROL);
    EXPECT_EQ(hdr.target_entity_id, entity_id);

    // The identify observer saw the change (non-zero value = active), and a
    // subsequent clear reports inactive.
    std::array<uint8_t, 5> clear{0x00, 0x1A, 0x00, 0x03, 0x00};
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, clear), AEM_STATUS_SUCCESS);
    EXPECT_EQ(identify_changes.size(), size_t{2});
    EXPECT_TRUE(identify_changes[0]);
    EXPECT_FALSE(identify_changes[1]);
}

TEST(nanoavb_entity_unsolicited_notify, non_identify_control_skips_multicast)
{
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x05};
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_entity_id(entity_id);
    handler.set_identify_control_index(3);

    std::vector<CapturedSend> sends;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    }});

    // A SET_CONTROL on a DIFFERENT control index (5) — not the identify control,
    // and nobody registered — so nothing is emitted.
    std::array<uint8_t, 5> body{0x00, 0x1A, 0x00, 0x05, 0x01};
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, body), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{0});
}

TEST(nanoavb_entity_unsolicited_notify, set_local_identify_convenience)
{
    // The GPIO/front-panel entry point: set_local_identify builds the
    // SET_CONTROL for the wired identify control, multicasts the unsolicited
    // notification, and fires the identify_changed observer.
    Eui64 const entity_id{0xAA, 0, 0, 0, 0, 0, 0, 0x06};
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_entity_id(entity_id);

    // Without a wired identify control the trigger reports NOT_IMPLEMENTED.
    EXPECT_EQ(handler.set_local_identify(true), AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_FALSE(handler.identify_control_index().has_value());

    handler.set_identify_control_index(3);
    EXPECT_EQ(handler.identify_control_index().value_or(0xFFFF), 3);

    std::vector<CapturedSend> sends;
    std::vector<bool> identify_changes;
    handler.set_send_response([&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    });
    handler.set_identify_changed([&](bool const active) { identify_changes.push_back(active); });

    EXPECT_EQ(handler.set_local_identify(true), AEM_STATUS_SUCCESS);
    EXPECT_EQ(handler.set_local_identify(false), AEM_STATUS_SUCCESS);

    EXPECT_EQ(sends.size(), size_t{2});  // IDENTIFY multicast, on + off
    EXPECT_EQ(sends[0].dest, atdecc::ATDECC_IDENTIFY_MULTICAST_MAC);
    EXPECT_EQ(sends[1].dest, atdecc::ATDECC_IDENTIFY_MULTICAST_MAC);
    EXPECT_EQ(identify_changes.size(), size_t{2});
    EXPECT_TRUE(identify_changes[0]);
    EXPECT_FALSE(identify_changes[1]);
}

TEST(nanoavb_entity_acquire, identify_control_honours_acquire_by_default)
{
    // 7.4.25.2 makes no exception for IDENTIFY: while another controller holds
    // the entity, its SET_CONTROL is refused like any other.
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_identify_control_index(3);

    auto acquire_header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    acquire_header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::array<uint8_t, 16> acquire_data{};
    EXPECT_EQ(make_test_result(handler, acquire_header, acquire_data).status, AEM_STATUS_SUCCESS);

    auto other_header = create_aem_header(AEM_COMMAND_SET_CONTROL);
    other_header.controller_entity_id = statusbar::ieee::Eui64{0xBB, 0, 0, 0, 0, 0, 0, 0x01};
    std::array<uint8_t, 5> identify_body{0x00, 0x1A, 0x00, 0x03, 0xFF};
    EXPECT_EQ(make_test_result(handler, other_header, identify_body).status, AEM_STATUS_ENTITY_ACQUIRED);
    EXPECT_EQ(app_handler.set_count, 0);

    // The acquirer's own identify proceeds.
    acquire_header.init_command(AEM_COMMAND_SET_CONTROL, 12);
    EXPECT_EQ(make_test_result(handler, acquire_header, identify_body).status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(app_handler.set_count, 1);
}

TEST(nanoavb_entity_acquire, identify_control_bypasses_acquire_when_asked)
{
    // The option restores the pre-2.6 behaviour: SET_CONTROL of the IDENTIFY
    // control works while another controller holds the entity; every other
    // control still honours the claim.
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_identify_control_index(3);
    handler.set_identify_bypasses_exclusive_access(true);

    auto acquire_header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    acquire_header.controller_entity_id = statusbar::ieee::Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::array<uint8_t, 16> acquire_data{};
    EXPECT_EQ(make_test_result(handler, acquire_header, acquire_data).status, AEM_STATUS_SUCCESS);
    EXPECT_TRUE(handler.is_acquired());

    auto other_header = create_aem_header(AEM_COMMAND_SET_CONTROL);
    other_header.controller_entity_id = statusbar::ieee::Eui64{0xBB, 0, 0, 0, 0, 0, 0, 0x01};

    // Identify control (index 3): allowed despite the foreign acquire.
    std::array<uint8_t, 5> identify_body{0x00, 0x1A, 0x00, 0x03, 0xFF};
    EXPECT_EQ(make_test_result(handler, other_header, identify_body).status, AEM_STATUS_SUCCESS);

    // A different control (index 5): still refused with ENTITY_ACQUIRED.
    std::array<uint8_t, 5> other_body{0x00, 0x1A, 0x00, 0x05, 0xFF};
    EXPECT_EQ(make_test_result(handler, other_header, other_body).status, AEM_STATUS_ENTITY_ACQUIRED);
}

//
// IEEE 1722.1-2021 7.5.2 as amended by Cor 1-2025: ACQUIRE_ENTITY, LOCK_ENTITY,
// DEREGISTER_UNSOLICITED_NOTIFICATION, REBOOT and SET_MEMORY_OBJECT_LENGTH
// notify; GET_COUNTERS / GET_STREAM_INFO pushes; GET_DYNAMIC_INFO; deferred
// (IN_PROGRESS) SET commands.
//

namespace {

/// Build + inject an AEM command from (controller_id, src_mac) with @p body.
void inject_command(
    AemCommandHandler& handler,
    Eui64 const& entity_id,
    Eui64 const& controller_id,
    Eui48 const& src_mac,
    uint16_t const command,
    std::span<uint8_t const> body,
    uint16_t const sequence = 1)
{
    AemDu cmd{};
    cmd.init_command(command, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + body.size()));
    cmd.target_entity_id = entity_id;
    cmd.controller_entity_id = controller_id;
    cmd.sequence_id = statusbar::ieee::doublet_t{sequence};
    std::vector<uint8_t> packet(AemDu::LENGTH + body.size());
    span_copy(std::span<uint8_t>{packet}.first(AemDu::LENGTH), make_const_span(cmd));
    std::copy(body.begin(), body.end(), packet.begin() + static_cast<std::ptrdiff_t>(AemDu::LENGTH));
    (void)handler.process_packet(src_mac, packet, entity_id);
}

auto header_of(CapturedSend const& s) -> AemDu
{
    AemDu hdr{};
    span_load(hdr, std::span<uint8_t const>(s.bytes).first(AemDu::LENGTH));
    return hdr;
}

auto body_of(CapturedSend const& s) -> std::span<uint8_t const>
{
    return std::span<uint8_t const>(s.bytes).subspan(AemDu::LENGTH);
}

/// Count the unsolicited responses for @p command sent to @p dest.
auto count_unsolicited(std::vector<CapturedSend> const& sends, uint16_t const command, Eui48 const& dest) -> int
{
    int n = 0;
    for (auto const& s : sends) {
        auto const hdr = header_of(s);
        if (s.dest == dest && hdr.is_response() && hdr.is_unsolicited() && hdr.command_code() == command) {
            ++n;
        }
    }
    return n;
}

/// The one solicited response in @p sends, or nullptr.
auto solicited(std::vector<CapturedSend> const& sends) -> CapturedSend const*
{
    for (auto const& s : sends) {
        auto const hdr = header_of(s);
        if (hdr.is_response() && !hdr.is_unsolicited()) {
            return &s;
        }
    }
    return nullptr;
}

Eui64 const kEntity{0xAA, 0, 0, 0, 0, 0, 0, 0x50};
Eui64 const kControllerA{0xC0, 0, 0, 0, 0, 0, 0, 0x0A};
Eui64 const kControllerB{0xC0, 0, 0, 0, 0, 0, 0, 0x0B};
Eui48 const kMacA{0x02, 0, 0, 0, 0, 0x0A};
Eui48 const kMacB{0x02, 0, 0, 0, 0, 0x0B};

auto capture_sends(AemCommandHandler& handler, std::vector<CapturedSend>& sends) -> void
{
    handler.set_send_response([&sends](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return true;
    });
}

}  // namespace

// 7.4.1: while the entity asks its owner whether it is still there on one
// contender's behalf, it is still the owner's. That contender asking again
// (its retransmit) is told IN_PROGRESS once more without the owner being
// asked twice; another contender is told ENTITY_ACQUIRED at once, naming the
// owner; and the handshake stays the first's: when the owner stays silent it
// is the contender that was waiting, not the last to ask, that takes the entity.
TEST(nanoavb_entity_acquire, a_contender_during_the_handshake_does_not_take_it_over)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    int asked = 0;
    handler.send_controller_available = [&](Eui64 const&) {
        ++asked;
        return true;
    };
    auto const now = sm::TimePoint{};
    handler.set_event_time(now);
    Eui64 const controller_c{0xC0, 0, 0, 0, 0, 0, 0, 0x0C};
    Eui48 const mac_c{0x02, 0, 0, 0, 0, 0x0C};
    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> const acquire{};

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 1);
    EXPECT_TRUE(handler.acquiring_controller() == kControllerA);

    // B contends: the owner is asked, B waits.
    sends.clear();
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 2);
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_TRUE(handler.has_pending_acquire());

    // B again, its retransmit: IN_PROGRESS again, the owner not asked twice.
    sends.clear();
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 2);
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);

    // C contends meanwhile: denied at once, told who owns the entity; B's wait stands.
    sends.clear();
    inject_command(handler, kEntity, controller_c, mac_c, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 3);
    EXPECT_EQ(asked, 1);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(sends[0].dest, mac_c);
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_ENTITY_ACQUIRED);
    AemAcquireEntityPayload denied{};
    span_load(denied, body_of(sends[0]).first(AemAcquireEntityPayload::LENGTH));
    EXPECT_TRUE(denied.owner_entity_id == kControllerA);
    EXPECT_TRUE(handler.has_pending_acquire());

    // The owner stays silent: B takes the entity; C hears nothing more.
    sends.clear();
    handler.tick(now + std::chrono::milliseconds(AEM_TIMEOUT_MS));
    EXPECT_FALSE(handler.has_pending_acquire());
    EXPECT_TRUE(handler.acquiring_controller() == kControllerB);
    auto const* granted = solicited(sends);
    EXPECT_TRUE(granted != nullptr);
    EXPECT_EQ(granted->dest, kMacB);
    EXPECT_EQ(header_of(*granted).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(header_of(*granted).sequence_id.get(), 2);
    for (auto const& s : sends) {
        EXPECT_FALSE(s.dest == mac_c);
    }
}

// The owner releasing the entity while the handshake is out answers what the
// handshake was asking it: the entity is free, and the contender waiting on
// that answer takes it then and there, told SUCCESS as its owner. The owner's
// CONTROLLER_AVAILABLE response arriving afterwards has nothing left to decide.
TEST(nanoavb_entity_acquire, the_owner_releasing_during_the_handshake_hands_the_entity_to_the_contender)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    handler.send_controller_available = [](Eui64 const&) { return true; };
    auto const now = sm::TimePoint{};
    handler.set_event_time(now);
    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> const acquire{};

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 1);
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 2);
    EXPECT_TRUE(handler.has_pending_acquire());

    AemAcquireEntityPayload release{};
    release.set_release(true);
    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> release_bytes{};
    span_store(release_bytes, release);
    sends.clear();
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, release_bytes, 5);
    EXPECT_FALSE(handler.has_pending_acquire());
    EXPECT_TRUE(handler.is_acquired());
    EXPECT_TRUE(handler.acquiring_controller() == kControllerB);

    // A's release is answered SUCCESS; B's wait is answered SUCCESS naming B.
    int to_a = 0;
    int to_b = 0;
    for (auto const& s : sends) {
        auto const hdr = header_of(s);
        if (hdr.is_unsolicited()) {
            continue;
        }
        AemAcquireEntityPayload answer{};
        span_load(answer, body_of(s).first(AemAcquireEntityPayload::LENGTH));
        EXPECT_EQ(hdr.status(), AEM_STATUS_SUCCESS);
        if (s.dest == kMacA) {
            ++to_a;
            EXPECT_EQ(hdr.sequence_id.get(), 5);
        }
        if (s.dest == kMacB) {
            ++to_b;
            EXPECT_EQ(hdr.sequence_id.get(), 2);
            EXPECT_TRUE(answer.owner_entity_id == kControllerB);
        }
    }
    EXPECT_EQ(to_a, 1);
    EXPECT_EQ(to_b, 1);

    // The owner's late answer, and the handshake's timeout, change nothing.
    sends.clear();
    handler.controller_available_response_received();
    handler.tick(now + std::chrono::milliseconds(AEM_TIMEOUT_MS));
    EXPECT_TRUE(sends.empty());
    EXPECT_TRUE(handler.acquiring_controller() == kControllerB);
}

TEST(nanoavb_entity_notify_cor1, acquire_notifies_registered_and_acquirer)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> acquire{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, acquire);
    EXPECT_TRUE(handler.is_acquired());

    // Solicited SUCCESS to A and unsolicited ACQUIRE_ENTITY to registered B.
    // A is now implicitly registered (7.5.2) but executed this command, so
    // it hears nothing more of it.
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_ACQUIRE_ENTITY, kMacB), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_ACQUIRE_ENTITY, kMacA), 0);
    EXPECT_EQ(sends.size(), size_t{2});
    for (auto const& s : sends) {
        AemAcquireEntityPayload payload{};
        span_load(payload, body_of(s).first(AemAcquireEntityPayload::LENGTH));
        EXPECT_EQ(payload.owner_entity_id, kControllerA);
    }

    // The acquirer's implicit registration is for what others do: B's own
    // DEREGISTER is notified to A (7.5.2 lists it) and not back to B.
    sends.clear();
    inject_command(
        handler, kEntity, kControllerB, kMacB, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, std::array<uint8_t, 0>{});
    EXPECT_EQ(header_of(*solicited(sends)).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacA), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacB), 0);
}

TEST(nanoavb_entity_notify_cor1, lock_expiry_notifies_unlock)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);

    auto const now = sm::TimePoint{};
    handler.set_event_time(now);
    std::array<uint8_t, AemLockEntityPayload::LENGTH> lock{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_LOCK_ENTITY, lock);
    EXPECT_TRUE(handler.is_locked());
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_LOCK_ENTITY, kMacB), 1);

    sends.clear();
    handler.tick(now + std::chrono::seconds(61));
    EXPECT_FALSE(handler.is_locked());
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_LOCK_ENTITY, kMacB), 1);
    AemLockEntityPayload payload{};
    span_load(payload, body_of(sends.front()).first(AemLockEntityPayload::LENGTH));
    EXPECT_TRUE(payload.is_unlock());
    EXPECT_EQ(payload.locked_entity_id, Eui64{});
}

TEST(nanoavb_entity_notify_cor1, deregister_notifies_remaining_controllers)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, {});
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacA), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacB), 0);
    EXPECT_EQ(sends.size(), size_t{2});  // B's own response + A's notification
}

TEST(nanoavb_entity_notify_cor1, notify_counters_changed_pushes_get_counters)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_get_counters([](uint16_t type, uint16_t index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
        if (type != DESCRIPTOR_STREAM_INPUT || index != 0) {
            return false;
        }
        valid = (1U << 0) | (1U << 11);
        counters[0] = 1;
        counters[11] = 4242;
        return true;
    });
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, 0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 1);
    AemCountersPayload payload{};
    span_load(payload, body_of(sends.front()).first(sizeof(AemCountersPayload)));
    EXPECT_EQ(payload.descriptor_type.get(), DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(payload.counters[11].get(), 4242U);

    sends.clear();
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, 1), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_TRUE(sends.empty());
}

TEST(nanoavb_entity_notify_cor1, notify_stream_info_changed_pushes_get_stream_info)
{
    // IEEE 1722.1-2021 7.5.2: a stream-info change (here STOP_STREAMING, which
    // Table 7-145 reports as STREAMING_WAIT) goes out as an unsolicited
    // GET_STREAM_INFO response to every registered controller.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_get_stream_info([](uint16_t type, uint16_t index, AemStreamInfoPayload& out, AemStreamInfoMilanExtension&) -> bool {
        if (type != DESCRIPTOR_STREAM_OUTPUT || index != 0) {
            return false;
        }
        out.flags = quadlet_t{stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAMING_WAIT};
        return true;
    });
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    EXPECT_EQ(handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, 0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_STREAM_INFO, kMacB), 1);
    AemStreamInfoPayload payload{};
    span_load(payload, body_of(sends.front()).first(AemStreamInfoPayload::LENGTH));
    EXPECT_EQ(payload.descriptor_type.get(), DESCRIPTOR_STREAM_OUTPUT);
    EXPECT_EQ(payload.descriptor_index.get(), 0);
    EXPECT_TRUE((payload.flags.get() & stream_info_flags::STREAMING_WAIT) != 0);
    EXPECT_TRUE((stream_info_flags::STREAMING_WAIT == 0x00000008U));  // Table 7-145 bit 28

    sends.clear();
    EXPECT_EQ(handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, 1), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_TRUE(sends.empty());
}

TEST(nanoavb_entity_get_stream_info, a_milan_entity_answers_and_notifies_in_84_octets_and_a_classic_one_in_48)
{
    // Milan v1.3 Section 5.4.2.10: IEEE 1722.1-2021's form (Clause 7.4.15.1), its IP fields zero
    // for a stream over AVTP; a classic entity keeps 1722.1-2013's 48 octets.
    for (auto const profile : {AemCommandHandler::Profile::classic, AemCommandHandler::Profile::milan13}) {
        auto model = create_test_model();
        AemCommandHandler handler{model};
        handler.set_entity_id(kEntity);
        handler.set_profile(profile);
        handler.set_get_stream_info([](uint16_t, uint16_t const index, AemStreamInfoPayload& out) -> bool {
            out.flags = quadlet_t{stream_info_flags::STREAM_ID_VALID};
            out.stream_id = Eui64{0x02, 0x00, 0x00, 0xe0, 0x13, 0x60, 0x00, 0x01};
            out.ip_flags = doublet_t{0xFFFF};  // the handler leaves what the callback fills
            return index == 0;
        });
        std::vector<CapturedSend> sends;
        capture_sends(handler, sends);
        register_controller(handler, kEntity, kControllerB, kMacB);
        sends.clear();
        std::array<uint8_t, AemGetStreamInfoCommandPayload::LENGTH> const asked{0x00, 0x06, 0x00, 0x00};
        inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_GET_STREAM_INFO, asked);
        EXPECT_EQ(handler.notify_stream_info_changed(DESCRIPTOR_STREAM_OUTPUT, 0), AEM_STATUS_SUCCESS);

        size_t const length = profile == AemCommandHandler::Profile::milan13 ? size_t{84} : size_t{48};
        EXPECT_EQ(handler.stream_info_length(), length);
        EXPECT_EQ(sends.size(), size_t{2});
        for (auto const& sent : sends) {
            EXPECT_EQ(header_of(sent).control_data_length(), AemDu::AEM_DATA_LENGTH + length);
            EXPECT_EQ(body_of(sent).size(), length);
            AemStreamInfoPayload payload{};
            span_load(payload, body_of(sent).first(AemStreamInfoPayload::LENGTH));
            EXPECT_EQ(payload.stream_id, (Eui64{0x02, 0x00, 0x00, 0xe0, 0x13, 0x60, 0x00, 0x01}));
            EXPECT_EQ(payload.ip_flags.get(), 0xFFFF);
            auto const tail = body_of(sent).subspan(AemStreamInfoPayload::LENGTH);
            EXPECT_TRUE(std::ranges::all_of(tail, [](uint8_t const b) { return b == 0; }));
        }
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_STREAM_INFO, kMacB), 1);
    }
}

TEST(nanoavb_entity_notify_cor1, reboot_not_implemented_without_callback)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    std::array<uint8_t, AemRebootPayload::LENGTH> body{};
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_REBOOT), body);
    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(nanoavb_entity_notify_cor1, reboot_runs_callback_and_notifies)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    int reboots = 0;
    handler.set_on_reboot([&reboots](uint16_t type, uint16_t index) -> uint8_t {
        EXPECT_EQ(type, DESCRIPTOR_ENTITY);
        EXPECT_EQ(index, 0);
        ++reboots;
        return AEM_STATUS_SUCCESS;
    });
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    std::array<uint8_t, AemRebootPayload::LENGTH> body{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_REBOOT, body);
    EXPECT_EQ(reboots, 1);
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(body_of(*reply).size(), AemRebootPayload::LENGTH);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_REBOOT, kMacB), 1);
}

namespace {

/// Encode one dynamic_info entry (Figure 7-94) for @p command with @p data.
void append_dynamic_info(std::vector<uint8_t>& out, uint16_t const command, std::span<uint8_t const> data, uint8_t const status = 0)
{
    out.push_back(static_cast<uint8_t>(data.size() >> 8U));
    out.push_back(static_cast<uint8_t>(data.size() & 0xFFU));
    out.push_back(0);
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(status << 3U));
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(command >> 8U));
    out.push_back(static_cast<uint8_t>(command & 0xFFU));
    out.insert(out.end(), data.begin(), data.end());
}

struct DynamicInfoEntry
{
    uint16_t length{};
    uint8_t status{};
    uint16_t command{};
    std::span<uint8_t const> data;
};

auto parse_dynamic_info(std::span<uint8_t const> body) -> std::vector<DynamicInfoEntry>
{
    std::vector<DynamicInfoEntry> entries;
    size_t at = 0;
    while (at + 8 <= body.size()) {
        DynamicInfoEntry e{};
        e.length = static_cast<uint16_t>((body[at] << 8U) | body[at + 1]);
        e.status = static_cast<uint8_t>(body[at + 4] >> 3U);
        e.command = static_cast<uint16_t>((body[at + 6] << 8U) | body[at + 7]);
        e.data = body.subspan(at + 8, e.length);
        entries.push_back(e);
        at += 8 + e.length;
    }
    return entries;
}

}  // namespace

TEST(nanoavb_entity_dynamic_info, packs_fixed_size_gets)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_counters([](uint16_t type, uint16_t index, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
        if (type != DESCRIPTOR_STREAM_INPUT || index != 0) {
            return false;
        }
        valid = 1U << 11;
        counters[11] = 7;
        return true;
    });

    std::vector<uint8_t> command;
    append_dynamic_info(command, AEM_COMMAND_GET_CONFIGURATION, {});
    std::array<uint8_t, 4> const stream_in{0x00, 0x05, 0x00, 0x00};
    append_dynamic_info(command, AEM_COMMAND_GET_COUNTERS, stream_in);
    append_dynamic_info(command, AEM_COMMAND_GET_VIDEO_FORMAT, stream_in);  // allowed, but not implemented here

    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_DYNAMIC_INFO), command);
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    auto const entries = parse_dynamic_info(result.response_data());
    EXPECT_EQ(entries.size(), size_t{3});

    EXPECT_EQ(entries[0].command, AEM_COMMAND_GET_CONFIGURATION);
    EXPECT_EQ(entries[0].status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(entries[0].length, AemSetConfigurationPayload::LENGTH);

    EXPECT_EQ(entries[1].command, AEM_COMMAND_GET_COUNTERS);
    EXPECT_EQ(entries[1].status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(entries[1].length, sizeof(AemCountersPayload));
    AemCountersPayload counters{};
    span_load(counters, entries[1].data.first(sizeof(AemCountersPayload)));
    EXPECT_EQ(counters.counters[11].get(), 7U);

    // A failed entry carries its status and echoes the request.
    EXPECT_EQ(entries[2].command, AEM_COMMAND_GET_VIDEO_FORMAT);
    EXPECT_EQ(entries[2].status, AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(entries[2].length, 4);
    EXPECT_EQ(entries[2].data[1], 0x05);
}

TEST(nanoavb_entity_dynamic_info, rejects_variable_size_commands)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    std::vector<uint8_t> command;
    std::array<uint8_t, 4> const control{0x00, 0x1A, 0x00, 0x00};
    append_dynamic_info(command, AEM_COMMAND_GET_CONFIGURATION, {});
    append_dynamic_info(command, AEM_COMMAND_GET_CONTROL, control);  // variable size: whole command refused

    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_DYNAMIC_INFO), command);
    EXPECT_EQ(result.status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(result.response_data().size(), command.size());  // request echoed, nothing processed
}

TEST(nanoavb_entity_dynamic_info, rejects_truncated_entry)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    std::vector<uint8_t> command;
    std::array<uint8_t, 4> const stream_in{0x00, 0x05, 0x00, 0x00};
    append_dynamic_info(command, AEM_COMMAND_GET_COUNTERS, stream_in);
    command.pop_back();  // entry claims 4 octets of data but carries 3

    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_DYNAMIC_INFO), command);
    EXPECT_EQ(result.status, AEM_STATUS_BAD_ARGUMENTS);
}

namespace {

class MemoryObjectHandler : public SetAcceptingHandler
{
  public:
    auto on_set_memory_object_length(DescriptorId id, uint64_t length) -> uint8_t override
    {
        last_id = id.ref;
        last_length = length;
        return AEM_STATUS_SUCCESS;
    }
    auto on_get_memory_object_length(DescriptorId id, uint64_t& length) -> bool override
    {
        if (id.ref.descriptor_type != DESCRIPTOR_MEMORY_OBJECT || id.ref.descriptor_index != 0) {
            return false;
        }
        length = last_length;
        return true;
    }
    DescriptorRef last_id{};
    uint64_t last_length{4096};
};

}  // namespace

TEST(nanoavb_entity_memory_object_length, default_handler_declines)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    std::array<uint8_t, AemMemoryObjectLengthPayload::LENGTH> set{};
    EXPECT_EQ(
        make_test_result(handler, create_aem_header(AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH), set).status, AEM_STATUS_NOT_IMPLEMENTED);
    std::array<uint8_t, AemGetMemoryObjectLengthCommandPayload::LENGTH> get{};
    EXPECT_EQ(
        make_test_result(handler, create_aem_header(AEM_COMMAND_GET_MEMORY_OBJECT_LENGTH), get).status,
        AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_memory_object_length, set_applies_and_notifies_then_get_reads_back)
{
    MemoryObjectHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    AemMemoryObjectLengthPayload set{};
    set.descriptor_index = 0;
    set.configuration_index = 0;
    set.length = 123456;
    std::array<uint8_t, AemMemoryObjectLengthPayload::LENGTH> set_bytes{};
    span_store(set_bytes, set);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH, set_bytes);
    EXPECT_EQ(app.last_length, 123456U);
    EXPECT_EQ(app.last_id.descriptor_type, DESCRIPTOR_MEMORY_OBJECT);
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH, kMacB), 1);

    std::array<uint8_t, AemGetMemoryObjectLengthCommandPayload::LENGTH> get{};
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_MEMORY_OBJECT_LENGTH), get);
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    AemMemoryObjectLengthPayload read{};
    span_load(read, result.response_data().first(AemMemoryObjectLengthPayload::LENGTH));
    EXPECT_EQ(read.length.get(), 123456U);
}

namespace {

/// Defers every SET_STREAM_FORMAT (as a talker waiting on an SRP re-reservation would).
class DeferringHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> /*value*/) -> uint8_t override
    {
        ++set_calls;
        return command_type == AEM_COMMAND_SET_STREAM_FORMAT ? AEM_STATUS_IN_PROGRESS : AEM_STATUS_SUCCESS;
    }

    int set_calls{0};
};

std::array<uint8_t, AemStreamFormatPayload::LENGTH> const kSetFormatOut0{
    0x00, 0x06, 0x00, 0x00, 0x02, 0x02, 0x01, 0x40, 0x08, 0x01, 0x00, 0x00};

}  // namespace

TEST(nanoavb_entity_in_progress, deferred_set_repeats_in_progress_until_completed)
{
    DeferringHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    auto const now = sm::TimePoint{};
    handler.set_event_time(now);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 7);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_EQ(sends[0].dest, kMacA);
    EXPECT_EQ(handler.pending_command_count(), size_t{1});

    // Repeats every AEM_IN_PROGRESS_TIMEOUT_MS (120 ms), not before.
    sends.clear();
    handler.tick(now + std::chrono::milliseconds(100));
    EXPECT_TRUE(sends.empty());
    handler.tick(now + std::chrono::milliseconds(121));
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_EQ(header_of(sends[0]).sequence_id.get(), 7);
    EXPECT_EQ(body_of(sends[0]).size(), AemStreamFormatPayload::LENGTH);

    // Completion: final SUCCESS to A (echoing the command) and the unsolicited
    // SET_STREAM_FORMAT to registered B.
    sends.clear();
    EXPECT_TRUE(handler.complete_pending_command(AEM_COMMAND_SET_STREAM_FORMAT, DESCRIPTOR_STREAM_OUTPUT, 0, AEM_STATUS_SUCCESS));
    EXPECT_EQ(handler.pending_command_count(), size_t{0});
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    EXPECT_EQ(reply->dest, kMacA);
    EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(header_of(*reply).sequence_id.get(), 7);
    EXPECT_TRUE(std::equal(kSetFormatOut0.begin(), kSetFormatOut0.end(), body_of(*reply).begin()));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_FORMAT, kMacB), 1);

    EXPECT_FALSE(handler.complete_pending_command(AEM_COMMAND_SET_STREAM_FORMAT, DESCRIPTOR_STREAM_OUTPUT, 0, AEM_STATUS_SUCCESS));
}

// 9.2.2.3.1.2: a controller that heard nothing within 250 ms sends its
// command again under the same sequence_id. The deferred command it repeats
// is answered IN_PROGRESS once more from its slot: the hook does not run
// twice and no second slot is taken. A different sequence_id is a new command.
TEST(nanoavb_entity_in_progress, a_retransmitted_deferred_set_is_answered_from_its_slot)
{
    DeferringHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    handler.set_event_time(sm::TimePoint{});

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 7);
    EXPECT_EQ(app.set_calls, 1);
    EXPECT_EQ(handler.pending_command_count(), size_t{1});

    sends.clear();
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 7);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_EQ(header_of(sends[0]).sequence_id.get(), 7);
    EXPECT_TRUE(std::equal(kSetFormatOut0.begin(), kSetFormatOut0.end(), body_of(sends[0]).begin()));
    EXPECT_EQ(app.set_calls, 1);
    EXPECT_EQ(handler.pending_command_count(), size_t{1});

    // The same command from another controller, or under another sequence_id,
    // is not a retransmit of it.
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 7);
    EXPECT_EQ(app.set_calls, 2);
    EXPECT_EQ(handler.pending_command_count(), size_t{2});
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 8);
    EXPECT_EQ(app.set_calls, 3);
    EXPECT_EQ(handler.pending_command_count(), size_t{3});
}

// A deferred command the application never completes is not held for ever:
// at its deadline it is answered PENDING_COMMAND_EXPIRED_STATUS with the value
// in force, nobody is notified, and its slot is free again.
TEST(nanoavb_entity_in_progress, a_deferred_set_nobody_completes_is_answered_at_its_deadline)
{
    DeferringHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    auto const now = sm::TimePoint{};
    handler.set_event_time(now);
    auto const deadline = std::chrono::milliseconds(AemCommandHandler::PENDING_COMMAND_DEADLINE_MS);

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 7);
    EXPECT_EQ(handler.pending_command_count(), size_t{1});

    // Up to the deadline: IN_PROGRESS repeats, nothing final.
    sends.clear();
    handler.tick(now + deadline - std::chrono::milliseconds(1));
    EXPECT_EQ(handler.pending_command_count(), size_t{1});
    for (auto const& s : sends) {
        EXPECT_EQ(header_of(s).status(), AEM_STATUS_IN_PROGRESS);
    }

    // At the deadline: one final response to A with the value in force, no
    // notification to B, the slot given back.
    sends.clear();
    handler.tick(now + deadline);
    EXPECT_EQ(handler.pending_command_count(), size_t{0});
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(sends[0].dest, kMacA);
    EXPECT_EQ(header_of(sends[0]).status(), AemCommandHandler::PENDING_COMMAND_EXPIRED_STATUS);
    EXPECT_EQ(header_of(sends[0]).sequence_id.get(), 7);
    EXPECT_EQ(body_of(sends[0]).size(), AemStreamFormatPayload::LENGTH);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_FORMAT, kMacB), 0);

    // A completion that comes late finds nothing to complete.
    EXPECT_FALSE(handler.complete_pending_command(AEM_COMMAND_SET_STREAM_FORMAT, DESCRIPTOR_STREAM_OUTPUT, 0, AEM_STATUS_SUCCESS));
}

TEST(nanoavb_entity_in_progress, failure_completion_sends_no_notification)
{
    DeferringHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    sends.clear();
    EXPECT_TRUE(
        handler.complete_pending_command(AEM_COMMAND_SET_STREAM_FORMAT, DESCRIPTOR_STREAM_OUTPUT, 0, AEM_STATUS_NOT_SUPPORTED));
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_FORMAT, kMacB), 0);
}

TEST(nanoavb_entity_in_progress, table_full_answers_no_resources)
{
    DeferringHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    for (uint16_t index = 0; index <= AemCommandHandler::MAX_PENDING_COMMANDS; ++index) {
        auto body = kSetFormatOut0;
        body[3] = static_cast<uint8_t>(index);
        sends.clear();
        inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, body, index);
        EXPECT_EQ(sends.size(), size_t{1});
        auto const expected = index < AemCommandHandler::MAX_PENDING_COMMANDS ? AEM_STATUS_IN_PROGRESS : AEM_STATUS_NO_RESOURCES;
        EXPECT_EQ(header_of(sends[0]).status(), expected);
        // The refusal carries the command's shape, as every failed SET does.
        EXPECT_EQ(body_of(sends[0]).size(), kSetFormatOut0.size());
    }
    EXPECT_EQ(handler.pending_command_count(), AemCommandHandler::MAX_PENDING_COMMANDS);
    // The ninth SET was refused before its hook ran: nothing was begun that
    // complete_pending_command() could not finish.
    EXPECT_EQ(app.set_calls, static_cast<int>(AemCommandHandler::MAX_PENDING_COMMANDS));

    // A slot given back admits the next SET, hook and all.
    EXPECT_TRUE(handler.complete_pending_command(AEM_COMMAND_SET_STREAM_FORMAT, DESCRIPTOR_STREAM_OUTPUT, 0, AEM_STATUS_SUCCESS));
    sends.clear();
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0, 20);
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_EQ(app.set_calls, static_cast<int>(AemCommandHandler::MAX_PENDING_COMMANDS) + 1);
}

//
// 7.4.9.1 and its siblings: a SET's response "always contains the current
// value, that is it contains the new value if the command succeeds or the old
// value if it fails" — however it failed: the hook, the lock, or a deferred
// completion.
//

namespace {

/// Refuses every SET, and serves GET_CONTROL and GET_MATRIX so a refused SET has
/// a value in force to answer with. Records what each GET asked.
class RefusingHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t /*command_type*/, DescriptorId /*id*/, std::span<uint8_t const> /*value*/)
        -> uint8_t override
    {
        return AEM_STATUS_BAD_ARGUMENTS;
    }

    auto on_get_descriptor_value(
        uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> request, std::span<uint8_t> out) -> size_t override
    {
        last_get = command_type;
        last_request.assign(request.begin(), request.end());
        if (command_type == AEM_COMMAND_GET_CONTROL && !out.empty()) {
            out[0] = current_control;
            return 1;
        }
        if (command_type == AEM_COMMAND_GET_MATRIX && out.size() >= request.size() + 2) {
            span_copy(out.first(request.size()), request);
            out[request.size()] = 0x12;
            out[request.size() + 1] = 0x34;
            return request.size() + 2;
        }
        return 0;
    }

    uint8_t current_control{0x40};
    uint16_t last_get{0};
    std::vector<uint8_t> last_request;
};

/// Has a name but takes no new one.
class NamedHandler : public AemEntityHandler
{
  public:
    auto on_get_name(DescriptorId /*id*/, uint16_t /*name_index*/) const -> std::optional<AtdeccString> override
    {
        return AtdeccString{"Old name"};
    }
};

/// Defers every SET_CONTROL and serves GET_CONTROL.
class DeferringControlHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> /*value*/) -> uint8_t override
    {
        return command_type == AEM_COMMAND_SET_CONTROL ? AEM_STATUS_IN_PROGRESS : AEM_STATUS_SUCCESS;
    }

    auto on_get_descriptor_value(
        uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> /*request*/, std::span<uint8_t> out) -> size_t override
    {
        if (command_type != AEM_COMMAND_GET_CONTROL || out.empty()) {
            return 0;
        }
        out[0] = 0x40;
        return 1;
    }
};

// CONTROL 5 := 255
std::array<uint8_t, 5> const kSetControl5{0x00, DESCRIPTOR_CONTROL, 0x00, 0x05, 0xFF};

}  // namespace

TEST(nanoavb_entity_set_response, a_refused_set_answers_the_value_in_force)
{
    RefusingHandler app;
    AemCommandHandler handler{app};
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_SET_CONTROL), kSetControl5);
    EXPECT_EQ(result.status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(result.response_data().size(), size_t{5});
    EXPECT_EQ(result.response_data()[3], 0x05);
    EXPECT_EQ(result.response_data()[4], 0x40);  // the old value, not the 0xFF asked for
    EXPECT_EQ(app.last_get, AEM_COMMAND_GET_CONTROL);
}

TEST(nanoavb_entity_set_response, a_set_refused_for_a_lock_answers_the_value_in_force)
{
    RefusingHandler app;
    AemCommandHandler handler{app};
    auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::array<uint8_t, 16> const lock_body{};
    EXPECT_EQ(make_test_result(handler, lock, lock_body).status, AEM_STATUS_SUCCESS);

    auto set = create_aem_header(AEM_COMMAND_SET_CONTROL);
    set.controller_entity_id = Eui64{0xBB, 0, 0, 0, 0, 0, 0, 0x01};
    auto const result = make_test_result(handler, set, kSetControl5);
    EXPECT_EQ(result.status, AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(result.response_data().size(), size_t{5});
    EXPECT_EQ(result.response_data()[4], 0x40);
}

TEST(nanoavb_entity_set_response, a_refused_set_matrix_reads_back_the_region_it_named)
{
    RefusingHandler app;
    AemCommandHandler handler{app};
    // MATRIX 2; region column 1, row 2, 1 x 1; horizontal, one value, no offset; one 2-octet value.
    std::array<uint8_t, 18> const set_body{
        0x00, DESCRIPTOR_MATRIX, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xAB, 0xCD};
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_SET_MATRIX), set_body);
    EXPECT_EQ(result.status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(app.last_get, AEM_COMMAND_GET_MATRIX);
    EXPECT_EQ(app.last_request.size(), size_t{12});  // the region header, without the values
    EXPECT_EQ(result.response_data().size(), size_t{18});
    EXPECT_EQ(result.response_data()[16], 0x12);
    EXPECT_EQ(result.response_data()[17], 0x34);
}

TEST(nanoavb_entity_set_response, a_set_with_nothing_to_read_back_echoes_the_command)
{
    RefusingHandler app;  // serves no GET_SIGNAL_SELECTOR
    AemCommandHandler handler{app};
    std::array<uint8_t, 12> const set_body{
        0x00, DESCRIPTOR_SIGNAL_SELECTOR, 0x00, 0x00, 0x00, 0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_SET_SIGNAL_SELECTOR), set_body);
    EXPECT_EQ(result.status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(result.response_data().size(), set_body.size());
    EXPECT_TRUE(std::equal(set_body.begin(), set_body.end(), result.response_data().begin()));
}

TEST(nanoavb_entity_set_response, a_refused_set_name_answers_the_name_in_force)
{
    NamedHandler app;
    AemCommandHandler handler{app};
    std::array<uint8_t, 72> set_body{};  // ENTITY 0, entity_name, configuration 0
    std::string_view const wanted{"New name"};
    std::copy(wanted.begin(), wanted.end(), set_body.begin() + 8);
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_SET_NAME), set_body);
    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(result.response_data().size(), size_t{72});
    std::string_view const in_force{"Old name"};
    EXPECT_TRUE(std::equal(in_force.begin(), in_force.end(), result.response_data().begin() + 8));
}

TEST(nanoavb_entity_in_progress, a_failed_completion_without_a_body_answers_the_value_in_force)
{
    DeferringControlHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, kSetControl5, 9);
    EXPECT_EQ(handler.pending_command_count(), size_t{1});

    sends.clear();
    EXPECT_TRUE(handler.complete_pending_command(AEM_COMMAND_SET_CONTROL, DESCRIPTOR_CONTROL, 5, AEM_STATUS_BAD_ARGUMENTS));
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(header_of(sends[0]).sequence_id.get(), 9);
    EXPECT_EQ(body_of(sends[0]).size(), size_t{5});
    EXPECT_EQ(body_of(sends[0])[4], 0x40);
}

// IEEE 1722.1-2021 Clause 9.2.1.1.7: octets past control_data_length are not the command's, nor echoed or held.
TEST(nanoavb_entity_in_progress, octets_past_control_data_length_are_ignored)
{
    DeferringControlHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    AemDu cmd{};
    cmd.init_command(AEM_COMMAND_SET_CONTROL, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + kSetControl5.size()));
    cmd.target_entity_id = kEntity;
    cmd.controller_entity_id = kControllerA;
    std::vector<uint8_t> packet(AemDu::LENGTH + kSetControl5.size() + 80, uint8_t{0xEE});  // 80 trailing octets
    span_copy(std::span<uint8_t>{packet}.first(AemDu::LENGTH), make_const_span(cmd));
    std::ranges::copy(kSetControl5, packet.begin() + static_cast<std::ptrdiff_t>(AemDu::LENGTH));
    (void)handler.process_packet(kMacA, packet, kEntity);

    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_EQ(size_t{header_of(sends[0]).control_data_length()}, AemDu::AEM_DATA_LENGTH + kSetControl5.size());
    EXPECT_TRUE(std::ranges::equal(body_of(sends[0]), kSetControl5));
    sends.clear();
    EXPECT_TRUE(handler.complete_pending_command(AEM_COMMAND_SET_CONTROL, DESCRIPTOR_CONTROL, 5, AEM_STATUS_SUCCESS));
    EXPECT_TRUE(std::ranges::equal(body_of(sends[0]), kSetControl5));
}

// IEEE 1722.1-2021 Table 7-141: a deferred SET with a body over 64 octets is held, completed and answered.
TEST(nanoavb_entity_in_progress, a_deferred_set_with_a_long_body_is_completed_and_answered)
{
    DeferringControlHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    std::array<uint8_t, 100> body{0x00, static_cast<uint8_t>(DESCRIPTOR_CONTROL), 0x00, 0x00};
    std::ranges::fill(std::span{body}.subspan(4), uint8_t{0x5A});
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, body);
    EXPECT_EQ(header_of(sends[0]).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_TRUE(std::ranges::equal(body_of(sends[0]), body));
    EXPECT_EQ(handler.pending_command_count(), size_t{1});

    sends.clear();
    EXPECT_TRUE(handler.complete_pending_command(AEM_COMMAND_SET_CONTROL, DESCRIPTOR_CONTROL, 0, AEM_STATUS_SUCCESS));
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    if (reply != nullptr) {
        EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
        EXPECT_TRUE(std::ranges::equal(body_of(*reply), body));
    }
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 1);
}

//
// Profiles: 7.5.2 as written notifies every SET answered SUCCESS; Milan v1.3
// 5.4.5.3 notifies only a SET that modifies the entity, so under milan13 a SET
// to the value already in force is neither applied nor notified.
//

namespace {

/// Accepts every SET and serves GET_CONTROL from one stored octet.
class StoringControlHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> value) -> uint8_t override
    {
        ++set_calls;
        if (command_type == AEM_COMMAND_SET_CONTROL && !value.empty()) {
            current = value[0];
        }
        return AEM_STATUS_SUCCESS;
    }

    auto on_get_descriptor_value(
        uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> /*request*/, std::span<uint8_t> out) -> size_t override
    {
        if (command_type != AEM_COMMAND_GET_CONTROL || out.empty()) {
            return 0;
        }
        out[0] = current;
        return 1;
    }

    uint8_t current{0x40};
    int set_calls{0};
};

}  // namespace

TEST(nanoavb_entity_profile, classic_notifies_a_set_to_the_value_in_force)
{
    StoringControlHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    std::array<uint8_t, 5> const same{0x00, DESCRIPTOR_CONTROL, 0x00, 0x05, 0x40};  // CONTROL 5 := its current 0x40
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, same, 3);
    EXPECT_EQ(header_of(*solicited(sends)).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(app.set_calls, 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 1);
}

TEST(nanoavb_entity_profile, milan13_leaves_a_set_to_the_value_in_force_unapplied_and_unnotified)
{
    StoringControlHandler app;
    AemCommandHandler handler{app};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    std::array<uint8_t, 5> const same{0x00, DESCRIPTOR_CONTROL, 0x00, 0x05, 0x40};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, same, 3);
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(body_of(*reply).size(), size_t{5});
    EXPECT_EQ(body_of(*reply)[4], 0x40);
    EXPECT_EQ(app.set_calls, 0);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 0);

    // A different value is applied and notified as ever.
    sends.clear();
    std::array<uint8_t, 5> const changed{0x00, DESCRIPTOR_CONTROL, 0x00, 0x05, 0xFF};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, changed, 4);
    EXPECT_EQ(header_of(*solicited(sends)).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(app.set_calls, 1);
    EXPECT_EQ(app.current, 0xFF);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 1);
}

TEST(nanoavb_entity_profile, milan13_never_grants_an_acquisition)
{
    // Milan v1.3 5.4.2.1: ACQUIRE_ENTITY is answered NOT_SUPPORTED, never SUCCESS.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    auto header = create_aem_header(AEM_COMMAND_ACQUIRE_ENTITY);
    header.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    std::array<uint8_t, 16> const body{};
    EXPECT_EQ(make_test_result(handler, header, body).status, AEM_STATUS_NOT_SUPPORTED);
    EXPECT_FALSE(handler.is_acquired());
}

TEST(nanoavb_entity_profile, milan13_locked_entity_refuses_a_local_identify)
{
    // Milan v1.3 5.3.4.1 / 5.4.2.17: while locked, the identification state is
    // not changed by non-ATDECC means either. Classic has no such rule.
    for (auto const profile : {AemCommandHandler::Profile::classic, AemCommandHandler::Profile::milan13}) {
        bool const milan = profile == AemCommandHandler::Profile::milan13;
        SetAcceptingHandler app;
        AemCommandHandler handler{app};
        handler.set_profile(profile);
        handler.set_identify_control_index(3);
        auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
        lock.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
        std::array<uint8_t, 16> const lock_body{};
        EXPECT_EQ(make_test_result(handler, lock, lock_body).status, AEM_STATUS_SUCCESS);

        EXPECT_EQ(handler.set_local_identify(true), milan ? AEM_STATUS_ENTITY_LOCKED : AEM_STATUS_SUCCESS);
        EXPECT_EQ(app.set_count, milan ? 0 : 1);

        // Unlocked, the local identify proceeds under either profile.
        std::array<uint8_t, 16> unlock_body{};
        unlock_body[3] = 0x01;  // UNLOCK
        EXPECT_EQ(make_test_result(handler, lock, unlock_body).status, AEM_STATUS_SUCCESS);
        EXPECT_EQ(handler.set_local_identify(false), AEM_STATUS_SUCCESS);
    }
}

TEST(nanoavb_entity_profile, milan13_locked_entity_refuses_a_local_value_and_applies_nothing)
{
    // Milan v1.3 5.3.4.1 / 5.4.2.17: one refusal for every local change, which a host can ask too.
    for (auto const profile : {AemCommandHandler::Profile::classic, AemCommandHandler::Profile::milan13}) {
        bool const milan = profile == AemCommandHandler::Profile::milan13;
        StoringControlHandler app;
        AemCommandHandler handler{app};
        handler.set_profile(profile);
        handler.set_entity_id(kEntity);
        std::vector<CapturedSend> sends;
        capture_sends(handler, sends);
        register_controller(handler, kEntity, kControllerB, kMacB);
        auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
        lock.controller_entity_id = kControllerA;
        std::array<uint8_t, 16> const lock_body{};
        EXPECT_EQ(make_test_result(handler, lock, lock_body).status, AEM_STATUS_SUCCESS);
        EXPECT_EQ(handler.local_change_refusal().has_value(), milan);
        sends.clear();

        // CONTROL 5 := 0xFF, by the entity itself.
        EXPECT_EQ(
            handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl5),
            milan ? AEM_STATUS_ENTITY_LOCKED : AEM_STATUS_SUCCESS);
        std::array<uint8_t, 4> const get_control5{0x00, DESCRIPTOR_CONTROL, 0x00, 0x05};
        auto const get = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_CONTROL), get_control5);
        EXPECT_EQ(get.response_data().back(), milan ? 0x40 : 0xFF);
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), milan ? 0 : 1);

        // Unlocked, the change goes through under either profile.
        std::array<uint8_t, 16> unlock_body{};
        unlock_body[3] = 0x01;  // UNLOCK
        EXPECT_EQ(make_test_result(handler, lock, unlock_body).status, AEM_STATUS_SUCCESS);
        EXPECT_FALSE(handler.local_change_refusal().has_value());
        EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl5), AEM_STATUS_SUCCESS);
        EXPECT_EQ(app.current, 0xFF);
    }
}

TEST(nanoavb_entity_profile, the_requesting_controller_is_left_out_of_the_fan_out_under_both_profiles)
{
    // IEEE 1722.1-2021 7.5.2: the unsolicited response goes to the acquired
    // and registered controllers "without sending the unsolicited response
    // back to the ATDECC Controller which executed the command". Milan v1.3
    // 5.4.5.3 says the same, so the profile makes no difference here.
    for (auto const profile : {AemCommandHandler::Profile::classic, AemCommandHandler::Profile::milan13}) {
        StoringControlHandler app;
        AemCommandHandler handler{app};
        handler.set_profile(profile);
        handler.set_entity_id(kEntity);
        std::vector<CapturedSend> sends;
        capture_sends(handler, sends);
        register_controller(handler, kEntity, kControllerA, kMacA);
        register_controller(handler, kEntity, kControllerB, kMacB);
        sends.clear();

        std::array<uint8_t, 5> const changed{0x00, DESCRIPTOR_CONTROL, 0x00, 0x05, 0xFF};
        inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, changed, 5);
        EXPECT_EQ(header_of(*solicited(sends)).status(), AEM_STATUS_SUCCESS);
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 1);
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacA), 0);

        // A change nobody asked for over ATDECC reaches every registered controller.
        sends.clear();
        handler.set_identify_control_index(5);
        EXPECT_EQ(handler.set_local_identify(true), AEM_STATUS_SUCCESS);
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacA), 1);
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 1);
    }
}

//
// GET_AVB_INFO (7.4.40) through the get_avb_info callback, and its
// unsolicited notification (7.5.2).
//

TEST(nanoavb_entity_avb_info, not_implemented_without_callback)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    std::array<uint8_t, 4> const request{0x00, 0x09, 0x00, 0x00};  // AVB_INTERFACE 0
    EXPECT_EQ(make_test_result(handler, create_aem_header(AEM_COMMAND_GET_AVB_INFO), request).status, AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(nanoavb_entity_avb_info, response_carries_state_and_msrp_mappings)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_avb_info([](uint16_t index, AvbInfo& out) -> bool {
        if (index != 0) {
            return false;
        }
        out.gptp_grandmaster_id = Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04};
        out.propagation_delay = 412;
        out.gptp_domain_number = 0;
        out.flags = avb_info_flags::AS_CAPABLE | avb_info_flags::GPTP_ENABLED | avb_info_flags::SRP_ENABLED;
        out.msrp_mappings.push_back({.traffic_class = 6, .priority = 3, .vlan_id = 2});  // SRclassID 6: class A
        out.msrp_mappings.push_back({.traffic_class = 5, .priority = 2, .vlan_id = 2});  // SRclassID 5: class B
        return true;
    });

    std::array<uint8_t, 4> const request{0x00, 0x09, 0x00, 0x00};
    auto const result = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_AVB_INFO), request);
    EXPECT_EQ(result.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(result.response_data().size(), AemAvbInfoPayload::LENGTH + 8);
    AemAvbInfoPayload payload{};
    span_load(payload, result.response_data().first(AemAvbInfoPayload::LENGTH));
    EXPECT_EQ(payload.descriptor_type.get(), DESCRIPTOR_AVB_INTERFACE);
    EXPECT_EQ(payload.gptp_grandmaster_id.to_uint64(), 0x001CABFFFE007604ULL);
    EXPECT_EQ(payload.propagation_delay.get(), 412U);
    EXPECT_TRUE(payload.is_as_capable());
    EXPECT_TRUE(payload.is_srp_enabled());
    EXPECT_EQ(payload.msrp_mappings_count.get(), 2);
    auto const tail = result.response_data().subspan(AemAvbInfoPayload::LENGTH);
    EXPECT_EQ(tail[0], 6);  // class A, as its SRclassID
    EXPECT_EQ(tail[1], 3);  // PCP 3
    EXPECT_EQ(tail[3], 2);  // VLAN 2
    EXPECT_EQ(tail[4], 5);  // class B
    EXPECT_EQ(tail[5], 2);

    // The wrong descriptor type, or an index the callback declines: NO_SUCH_DESCRIPTOR.
    std::array<uint8_t, 4> const stream{0x00, 0x05, 0x00, 0x00};
    EXPECT_EQ(make_test_result(handler, create_aem_header(AEM_COMMAND_GET_AVB_INFO), stream).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
    std::array<uint8_t, 4> const other{0x00, 0x09, 0x00, 0x01};
    EXPECT_EQ(make_test_result(handler, create_aem_header(AEM_COMMAND_GET_AVB_INFO), other).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
}

TEST(nanoavb_entity_avb_info, notify_pushes_unsolicited_get_avb_info)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    handler.set_get_avb_info([](uint16_t, AvbInfo& out) -> bool {
        out.gptp_grandmaster_id = Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04};
        out.flags = avb_info_flags::AS_CAPABLE | avb_info_flags::GPTP_ENABLED;
        return true;
    });
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    EXPECT_EQ(handler.notify_avb_info_changed(0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_AVB_INFO, kMacB), 1);
    AemAvbInfoPayload payload{};
    span_load(payload, body_of(sends.front()).first(AemAvbInfoPayload::LENGTH));
    EXPECT_TRUE(payload.is_as_capable());
    EXPECT_EQ(payload.gptp_grandmaster_id.to_uint64(), 0x001CABFFFE007604ULL);
}

//
// Test Runner
//
// ──────────────────────────────────────────────
// Milan vendor unique (MVU) - Milan v1.3 5.4.3, 5.4.4
// ──────────────────────────────────────────────

namespace {

/// Inject one MVU command for kEntity from (controller_id, src_mac): the
/// header, then @p body as the command-specific data; @p protocol_id for a
/// vendor-unique message that is not Milan's.
auto inject_mvu(
    AemCommandHandler& handler,
    Eui64 const& controller_id,
    Eui48 const& src_mac,
    uint16_t const command,
    std::span<uint8_t const> body,
    uint16_t const sequence = 1,
    Eui48 const& protocol_id = MVU_PROTOCOL_ID) -> bool
{
    AecpMvuDu cmd{};
    cmd.init_command(command, static_cast<uint16_t>(body.size()));
    cmd.protocol_id = protocol_id;
    cmd.common.target_entity_id = kEntity;
    cmd.common.controller_entity_id = controller_id;
    cmd.common.sequence_id = statusbar::ieee::doublet_t{sequence};
    std::vector<uint8_t> packet(AecpMvuDu::LENGTH + body.size());
    span_copy(std::span<uint8_t>{packet}.first(AecpMvuDu::LENGTH), make_const_span(cmd));
    std::copy(body.begin(), body.end(), packet.begin() + static_cast<std::ptrdiff_t>(AecpMvuDu::LENGTH));
    return handler.process_packet(src_mac, packet, kEntity);
}

auto mvu_header_of(CapturedSend const& s) -> AecpMvuDu
{
    AecpMvuDu hdr{};
    span_load(hdr, std::span<uint8_t const>(s.bytes).first(AecpMvuDu::LENGTH));
    return hdr;
}

auto mvu_body_of(CapturedSend const& s) -> std::span<uint8_t const>
{
    return std::span<uint8_t const>(s.bytes).subspan(AecpMvuDu::LENGTH);
}

AemCommandHandler::MilanInfo const kMilanInfo{
    .features_flags = MVU_FEATURE_REDUNDANCY | MVU_FEATURE_TALKER_SIGNAL_PRESENCE,
    .certification_version = mvu_version(1, 2),
    .specification_version = mvu_version(1, 3)};

std::array<uint8_t, MVU_GET_MILAN_INFO_COMMAND_LENGTH> const kGetMilanInfo{};

}  // namespace

TEST(nanoavb_entity_mvu, a_plain_entity_lets_get_milan_info_pass_unanswered)
{
    // Milan v1.3 5.4.4.1 Note: a Configuration that is not Milan does not reply.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    EXPECT_FALSE(handler.milan_info().has_value());
    EXPECT_FALSE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_MILAN_INFO, kGetMilanInfo, 60));
    EXPECT_TRUE(sends.empty());
}

TEST(nanoavb_entity_mvu, a_milan_entity_answers_get_milan_info_as_figure_5_6)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_MILAN_INFO, kGetMilanInfo, 60));
    EXPECT_EQ(sends.size(), size_t{1});
    if (sends.size() != 1) {
        return;
    }
    EXPECT_EQ(sends[0].dest, kMacA);
    auto const hdr = mvu_header_of(sends[0]);
    EXPECT_TRUE(hdr.is_valid());
    EXPECT_TRUE(hdr.is_response());
    EXPECT_FALSE(hdr.is_unsolicited());
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(hdr.command_type(), MVU_COMMAND_GET_MILAN_INFO);
    EXPECT_EQ(hdr.common.control_data_length(), 36);
    EXPECT_EQ(hdr.common.sequence_id.get(), 60);
    EXPECT_EQ(hdr.common.target_entity_id, kEntity);
    EXPECT_EQ(hdr.common.controller_entity_id, kControllerA);
    EXPECT_EQ(sends[0].bytes.size(), AecpMvuDu::LENGTH + MvuGetMilanInfoResponse::LENGTH);

    MvuGetMilanInfoResponse info{};
    span_load(info, mvu_body_of(sends[0]));
    EXPECT_EQ(info.reserved.get(), 0);
    EXPECT_EQ(info.protocol_version.get(), MVU_PROTOCOL_VERSION);
    EXPECT_EQ(info.features_flags.get(), kMilanInfo.features_flags);
    EXPECT_EQ(info.certification_version.get(), mvu_version(1, 2));
    EXPECT_EQ(info.specification_version.get(), mvu_version(1, 3));
}

TEST(nanoavb_entity_mvu, the_other_mvu_commands_are_not_implemented_and_echo_their_data)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    // A command type Table 5.15 reserves.
    constexpr uint16_t RESERVED_COMMAND = 0x0008;
    std::array<uint8_t, 10> const unique_id{0x00, 0x00, 0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x51, 0x1D};
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, RESERVED_COMMAND, unique_id, 61));
    EXPECT_EQ(sends.size(), size_t{1});
    if (sends.size() != 1) {
        return;
    }
    auto const hdr = mvu_header_of(sends[0]);
    EXPECT_TRUE(hdr.is_response());
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(hdr.command_type(), RESERVED_COMMAND);
    EXPECT_EQ(hdr.common.sequence_id.get(), 61);
    EXPECT_EQ(hdr.common.control_data_length(), AecpMvuDu::MVU_DATA_LENGTH + unique_id.size());
    auto const body = mvu_body_of(sends[0]);
    EXPECT_TRUE(std::equal(unique_id.begin(), unique_id.end(), body.begin(), body.end()));
}

// Milan v1.3 Sections 5.4.4.2, 5.4.4.3: SET_SYSTEM_UNIQUE_ID keeps a u64 number and a name, GET
// answers them, a name that is not UTF-8 is refused, and a change reaches the settings hook.
TEST(nanoavb_entity_mvu, set_system_unique_id_is_kept_and_answered_by_get)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    int saved = 0;
    handler.set_on_mvu_settings_changed([&saved](uint16_t) { ++saved; });

    MvuSystemUniqueIdPayloadV13 const wanted{.number = uint64_t{0x0102030405060708}, .name = aem::AtdeccString{"FOH"}};
    std::array<uint8_t, MvuSystemUniqueIdPayloadV13::LENGTH> set{};
    span_store(set, wanted);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, set, 7));
    EXPECT_EQ(mvu_header_of(sends.back()).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(handler.system_unique_id(), uint64_t{0x0102030405060708});
    EXPECT_EQ(handler.system_unique_name().as_string_view(), std::string_view{"FOH"});
    EXPECT_EQ(saved, 1);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, set, 8));
    EXPECT_EQ(saved, 1);  // the same again changes nothing

    std::array<uint8_t, 2> const get{};
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, get, 9));
    MvuSystemUniqueIdPayloadV13 answered{};
    span_load(answered, mvu_body_of(sends.back()));
    EXPECT_EQ(answered.number.get(), uint64_t{0x0102030405060708});

    auto bad = set;
    bad[MvuSystemUniqueIdPayloadV13::LENGTH - 64] = 0xFF;  // not UTF-8
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, bad, 10));
    EXPECT_EQ(mvu_header_of(sends.back()).common.status(), MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(handler.system_unique_name().as_string_view(), std::string_view{"FOH"});
}

TEST(nanoavb_entity_mvu, another_vendors_protocol_and_a_response_are_not_ours)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    Eui48 const other_protocol{0x00, 0x1B, 0xC5, 0x0A, 0xC2, 0x00};
    EXPECT_FALSE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_MILAN_INFO, kGetMilanInfo, 62, other_protocol));
    EXPECT_TRUE(sends.empty());

    // A Milan response (another entity's answer, seen on the wire) is not a command for us.
    AecpMvuDu response{};
    response.init_response(MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS, 0);
    response.common.target_entity_id = kEntity;
    std::array<uint8_t, AecpMvuDu::LENGTH> packet{};
    span_store(std::span<uint8_t>{packet}, response);
    EXPECT_FALSE(handler.process_packet(kMacA, packet, kEntity));
    EXPECT_TRUE(sends.empty());
}

TEST(nanoavb_entity_mvu, a_get_milan_info_without_its_reserved_octets_is_payload_too_short)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_MILAN_INFO, std::span<uint8_t const>{}, 63));
    EXPECT_EQ(sends.size(), size_t{1});
    if (sends.size() != 1) {
        return;
    }
    auto const hdr = mvu_header_of(sends[0]);
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
    EXPECT_EQ(hdr.common.control_data_length(), AecpMvuDu::MVU_DATA_LENGTH);
    EXPECT_EQ(sends[0].bytes.size(), AecpMvuDu::LENGTH);
}

TEST(nanoavb_entity_mvu, ethernet_padding_is_not_command_data)
{
    // A 32-octet command arrives in a 60-octet frame: the padding behind
    // control_data_length is not echoed back.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    std::array<uint8_t, 2> const data{0xAB, 0xCD};
    AecpMvuDu cmd{};
    cmd.init_command(MVU_COMMAND_BIND_STREAM, static_cast<uint16_t>(data.size()));
    cmd.common.target_entity_id = kEntity;
    cmd.common.controller_entity_id = kControllerA;
    std::array<uint8_t, 46> padded{};  // the AECP payload of a minimum-length frame
    span_store(std::span<uint8_t>{padded}.first(AecpMvuDu::LENGTH), cmd);
    padded[AecpMvuDu::LENGTH] = data[0];
    padded[AecpMvuDu::LENGTH + 1] = data[1];
    EXPECT_TRUE(handler.process_packet(kMacA, padded, kEntity));
    EXPECT_EQ(sends.size(), size_t{1});
    if (sends.size() != 1) {
        return;
    }
    EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(mvu_body_of(sends[0]).size(), data.size());
}

//
// The stream input commands - Milan v1.3 5.4.4.6 to 5.4.4.8
//

namespace {

Eui64 const kTalker{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x09};
constexpr uint16_t kStreamInput = 0x0005;
constexpr uint16_t kStreamOutput = 0x0006;

/// What stands in for Milan connection management: two stream inputs, the
/// commands it was given, and the ACMP status it answers a bind or unbind with.
struct StreamInputs
{
    std::vector<statusbar::atdecc::AcmpCommandResponse> commands{};
    std::vector<size_t> sends_when_answered{};
    std::vector<CapturedSend> const* sends{nullptr};
    std::optional<uint8_t> acmp_status{statusbar::atdecc::ACMP_STATUS_SUCCESS};

    void attach(AemCommandHandler& handler, std::vector<CapturedSend> const& captured)
    {
        sends = &captured;
        handler.attach_mvu_stream_inputs(
            this,
            MvuStreamInputCallbacks{
                .binding_command =
                    [this](statusbar::atdecc::AcmpCommandResponse const& command, MvuStreamInputCallbacks::Respond const& respond) {
                        on_binding_command(command, respond);
                    },
                .stream_input_info = [](uint16_t const index) { return info_of(index); }});
    }

    void on_binding_command(statusbar::atdecc::AcmpCommandResponse const& command, MvuStreamInputCallbacks::Respond const& respond)
    {
        commands.push_back(command);
        if (!acmp_status.has_value()) {
            return;
        }
        auto response = command;
        response.set_status(*acmp_status);
        respond(response);
        sends_when_answered.push_back(sends->size());
    }

    static auto info_of(uint16_t const index) -> std::optional<MvuGetStreamInputInfoExResponse>
    {
        if (index >= 2) {
            return std::nullopt;
        }
        MvuGetStreamInputInfoExResponse info{};
        info.descriptor_type = kStreamInput;
        info.descriptor_index = index;
        info.talker_entity_id = kTalker;
        info.talker_unique_id = 4;
        info.set_status(2, 7);
        return info;
    }
};

auto stream_input(uint16_t const descriptor_type, uint16_t const index, uint16_t const reserved = 0)
    -> std::array<uint8_t, MvuStreamInput::LENGTH>
{
    MvuStreamInput input{};
    input.reserved = reserved;
    input.descriptor_type = descriptor_type;
    input.descriptor_index = index;
    std::array<uint8_t, MvuStreamInput::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, input);
    return bytes;
}

auto bind_stream(uint16_t const descriptor_type, uint16_t const index, uint16_t const flags = 0)
    -> std::array<uint8_t, MvuBindStream::LENGTH>
{
    MvuBindStream bind{};
    bind.flags = flags;
    bind.descriptor_type = descriptor_type;
    bind.descriptor_index = index;
    bind.talker_entity_id = kTalker;
    bind.talker_stream_index = 4;
    bind.reserved = 0xBEEF;  // a sender's mistake, not to come back
    std::array<uint8_t, MvuBindStream::LENGTH> bytes{};
    span_store(std::span<uint8_t>{bytes}, bind);
    return bytes;
}

}  // namespace

TEST(nanoavb_entity_mvu, without_milan_connection_management_the_stream_input_commands_are_not_implemented)
{
    // An entity that answers MVU but runs IEEE 1722.1 connection management
    // has no sink to bind and no Probing status to report.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    auto const bind = bind_stream(kStreamInput, 0);
    auto const input = stream_input(kStreamInput, 0);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind, 70));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, input, 71));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, input, 72));
    EXPECT_EQ(sends.size(), size_t{3});
    for (auto const& each : sends) {
        EXPECT_EQ(mvu_header_of(each).common.status(), MVU_STATUS_NOT_IMPLEMENTED);
    }
}

TEST(nanoavb_entity_mvu, get_stream_input_info_ex_answers_as_figure_5_12)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);

    // The command's reserved field is ignored (5.4.4.8).
    auto const asked = stream_input(kStreamInput, 1, 0xFFFF);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, asked, 73));
    EXPECT_EQ(sends.size(), size_t{1});
    if (sends.size() != 1) {
        return;
    }
    auto const hdr = mvu_header_of(sends[0]);
    EXPECT_TRUE(hdr.is_response());
    EXPECT_FALSE(hdr.is_unsolicited());
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(hdr.command_type(), MVU_COMMAND_GET_STREAM_INPUT_INFO_EX);
    EXPECT_EQ(hdr.common.sequence_id.get(), 73);
    EXPECT_EQ(hdr.common.control_data_length(), AecpMvuDu::MVU_DATA_LENGTH + MvuGetStreamInputInfoExResponse::LENGTH);
    EXPECT_EQ(sends[0].bytes.size(), size_t{48});
    EXPECT_EQ(sends[0].dest, kMacA);

    MvuGetStreamInputInfoExResponse info{};
    span_load(info, mvu_body_of(sends[0]));
    EXPECT_EQ(info.reserved.get(), 0);
    EXPECT_EQ(info.descriptor_type.get(), kStreamInput);
    EXPECT_EQ(info.descriptor_index.get(), 1);
    EXPECT_EQ(info.talker_entity_id, kTalker);
    EXPECT_EQ(info.talker_unique_id.get(), 4);
    EXPECT_EQ(info.pbsta(), 2);
    EXPECT_EQ(info.acmpsta(), 7);
    EXPECT_TRUE(inputs.commands.empty());
}

TEST(nanoavb_entity_mvu, get_stream_input_info_ex_refuses_what_is_not_a_stream_input_of_the_entity)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);

    auto const output = stream_input(kStreamOutput, 0, 0xFFFF);
    auto const beyond = stream_input(kStreamInput, 2);
    std::array<uint8_t, 4> const cut{0x00, 0x00, 0x00, 0x05};
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, output, 74));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, beyond, 75));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, cut, 76));
    EXPECT_EQ(sends.size(), size_t{3});
    if (sends.size() != 3) {
        return;
    }
    // A refusal has the response's own format: the descriptor named, the
    // reserved field cleared, and nothing said of a talker or of probing.
    MvuGetStreamInputInfoExResponse expected{};
    expected.descriptor_type = kStreamOutput;
    EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(std::ranges::equal(mvu_body_of(sends[0]), make_const_span(expected)));
    expected.descriptor_type = kStreamInput;
    expected.descriptor_index = 2;
    EXPECT_EQ(mvu_header_of(sends[1]).common.status(), MVU_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_TRUE(std::ranges::equal(mvu_body_of(sends[1]), make_const_span(expected)));
    EXPECT_EQ(mvu_header_of(sends[2]).common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
    EXPECT_TRUE(mvu_body_of(sends[2]).empty());
}

TEST(nanoavb_entity_mvu, a_bind_stream_reaches_its_sink_as_the_acmp_command_and_is_answered_from_inside)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);

    auto const bind = bind_stream(kStreamInput, 1, MVU_BIND_STREAM_FLAG_STREAMING_WAIT);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind, 80));
    EXPECT_EQ(inputs.commands.size(), size_t{1});
    EXPECT_EQ(sends.size(), size_t{1});
    if (inputs.commands.size() != 1 || sends.size() != 1) {
        return;
    }
    // Table 5.22
    auto const& command = inputs.commands.front();
    EXPECT_EQ(command.message_type(), statusbar::atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(command.controller_entity_id, kControllerA);
    EXPECT_EQ(command.listener_entity_id, kEntity);
    EXPECT_EQ(command.listener_unique_id.get(), 1);
    EXPECT_EQ(command.talker_entity_id, kTalker);
    EXPECT_EQ(command.talker_unique_id.get(), 4);
    EXPECT_TRUE(command.is_streaming_wait());
    // 5.5.3.6.3 step 3 comes before the probe of step 5: the response was
    // on its way when the sink's treatment went on.
    EXPECT_EQ(inputs.sends_when_answered.front(), size_t{1});

    // Table 5.35: the command's fields, flags included; reserved is ours to clear.
    auto const hdr = mvu_header_of(sends[0]);
    EXPECT_TRUE(hdr.is_response());
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(hdr.command_type(), MVU_COMMAND_BIND_STREAM);
    EXPECT_EQ(hdr.common.sequence_id.get(), 80);
    EXPECT_EQ(hdr.common.controller_entity_id, kControllerA);
    EXPECT_EQ(sends[0].dest, kMacA);
    MvuBindStream answer{};
    span_load(answer, mvu_body_of(sends[0]));
    EXPECT_TRUE(answer.streaming_wait());
    EXPECT_EQ(answer.descriptor_type.get(), kStreamInput);
    EXPECT_EQ(answer.descriptor_index.get(), 1);
    EXPECT_EQ(answer.talker_entity_id, kTalker);
    EXPECT_EQ(answer.talker_stream_index.get(), 4);
    EXPECT_EQ(answer.reserved.get(), 0);
    EXPECT_EQ(mvu_body_of(sends[0]).size(), MvuBindStream::LENGTH);
}

TEST(nanoavb_entity_mvu, an_unbind_stream_reaches_its_sink_and_answers_with_the_stream_input)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);

    auto const unbind = stream_input(kStreamInput, 0, 0xFFFF);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, unbind, 81));
    EXPECT_EQ(inputs.commands.size(), size_t{1});
    EXPECT_EQ(sends.size(), size_t{1});
    if (inputs.commands.size() != 1 || sends.size() != 1) {
        return;
    }
    EXPECT_EQ(inputs.commands.front().message_type(), statusbar::atdecc::ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    EXPECT_EQ(inputs.commands.front().listener_unique_id.get(), 0);
    // Table 5.41
    EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(mvu_header_of(sends[0]).command_type(), MVU_COMMAND_UNBIND_STREAM);
    auto const expected = stream_input(kStreamInput, 0);
    EXPECT_TRUE(std::ranges::equal(mvu_body_of(sends[0]), expected));
}

TEST(nanoavb_entity_mvu, a_sinks_refusal_of_a_bind_or_unbind_becomes_the_mvu_status)
{
    // Tables 5.28, 5.33 and 5.39: what ACMP calls LISTENER_UNKNOWN_ID and
    // CONTROLLER_NOT_AUTHORIZED are NO_SUCH_DESCRIPTOR and ENTITY_LOCKED here.
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);

    auto const bind = bind_stream(kStreamInput, 9);
    auto const unbind = stream_input(kStreamInput, 0);
    inputs.acmp_status = statusbar::atdecc::ACMP_STATUS_LISTENER_UNKNOWN_ID;
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind, 82));
    inputs.acmp_status = statusbar::atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED;
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind, 83));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, unbind, 84));
    EXPECT_EQ(sends.size(), size_t{3});
    if (sends.size() != 3) {
        return;
    }
    EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(mvu_header_of(sends[1]).common.status(), MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(mvu_header_of(sends[2]).common.status(), MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(mvu_body_of(sends[1]).size(), MvuBindStream::LENGTH);
    EXPECT_EQ(mvu_body_of(sends[2]).size(), MvuStreamInput::LENGTH);
}

TEST(nanoavb_entity_mvu, a_bind_or_unbind_the_handler_can_refuse_never_reaches_a_sink)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);

    // 5.4.4.6: a descriptor_type other than STREAM_INPUT is BAD_ARGUMENTS.
    auto const bind_output = bind_stream(kStreamOutput, 0);
    auto const unbind_output = stream_input(kStreamOutput, 0);
    // Shorter than its format: an UNBIND_STREAM's six octets sent as a BIND_STREAM.
    auto const short_bind = stream_input(kStreamInput, 0);
    std::array<uint8_t, 2> const short_unbind{};
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_output, 85));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, unbind_output, 86));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, short_bind, 87));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, short_unbind, 88));
    EXPECT_TRUE(inputs.commands.empty());
    EXPECT_EQ(sends.size(), size_t{4});
    if (sends.size() != 4) {
        return;
    }
    EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(mvu_body_of(sends[0]).size(), MvuBindStream::LENGTH);
    EXPECT_EQ(mvu_header_of(sends[1]).common.status(), MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_TRUE(std::ranges::equal(mvu_body_of(sends[1]), unbind_output));
    EXPECT_EQ(mvu_header_of(sends[2]).common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
    EXPECT_TRUE(mvu_body_of(sends[2]).empty());
    EXPECT_EQ(mvu_header_of(sends[3]).common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
}

TEST(nanoavb_entity_mvu, a_bind_no_sink_answers_is_entity_misbehaving_and_not_silence)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);
    inputs.acmp_status = std::nullopt;

    auto const bind = bind_stream(kStreamInput, 0);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind, 89));
    EXPECT_EQ(inputs.commands.size(), size_t{1});
    EXPECT_EQ(sends.size(), size_t{1});
    if (sends.size() == 1) {
        EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_ENTITY_MISBEHAVING);
    }
}

TEST(nanoavb_entity_mvu, the_stream_inputs_are_taken_away_only_by_whoever_attached_them)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    StreamInputs inputs;
    inputs.attach(handler, sends);
    auto const asked = stream_input(kStreamInput, 0);

    int const someone_else = 0;
    handler.detach_mvu_stream_inputs(&someone_else);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, asked, 90));
    handler.detach_mvu_stream_inputs(&inputs);
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, asked, 91));
    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, asked, 92));
    EXPECT_EQ(sends.size(), size_t{3});
    if (sends.size() != 3) {
        return;
    }
    EXPECT_EQ(mvu_header_of(sends[0]).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(mvu_header_of(sends[1]).common.status(), MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(mvu_header_of(sends[2]).common.status(), MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_TRUE(inputs.commands.empty());
}

TEST(nanoavb_entity_mvu, aem_commands_are_answered_as_before_beside_the_mvu_ones)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ENTITY_AVAILABLE, std::span<uint8_t const>{}, 5);
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    if (reply != nullptr) {
        EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
        EXPECT_EQ(header_of(*reply).command_code(), AEM_COMMAND_ENTITY_AVAILABLE);
    }
}

//
// Registered controllers - Milan v1.3 Sections 5.3.4.2, 5.4.5.1, 5.4.5.2
//

namespace {

/// The unsolicited AEM responses for @p command sent to @p dest, their sequence ids in order.
auto unsolicited_sequence_ids(std::vector<CapturedSend> const& sends, uint16_t const command, Eui48 const& dest)
    -> std::vector<uint16_t>
{
    std::vector<uint16_t> ids;
    for (auto const& s : sends) {
        auto const hdr = header_of(s);
        if (s.dest == dest && hdr.is_response() && hdr.is_unsolicited() && hdr.command_code() == command) {
            ids.push_back(hdr.sequence_id.get());
        }
    }
    return ids;
}

/// The unsolicited MVU responses for @p command sent to @p dest, their sequence ids in order.
auto mvu_unsolicited_sequence_ids(std::vector<CapturedSend> const& sends, uint16_t const command, Eui48 const& dest)
    -> std::vector<uint16_t>
{
    std::vector<uint16_t> ids;
    for (auto const& s : sends) {
        if (s.bytes.size() < AecpMvuDu::LENGTH) {
            continue;
        }
        auto const hdr = mvu_header_of(s);
        if (s.dest == dest && hdr.is_mvu() && hdr.is_response() && hdr.is_unsolicited() && hdr.command_type() == command) {
            ids.push_back(hdr.common.sequence_id.get());
        }
    }
    return ids;
}

// CONTROL 0 := 0x0102
std::array<uint8_t, 6> const kSetControl0{0x00, DESCRIPTOR_CONTROL, 0x00, 0x00, 0x01, 0x02};

}  // namespace

TEST(nanoavb_entity_registered_controllers, requester_is_left_out_and_sequence_ids_are_per_controller)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    // B sets a control twice: A is told each time, B (the requester) never.
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_SET_CONTROL, kSetControl0, 1);
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_SET_CONTROL, kSetControl0, 2);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 0);
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacA) == std::vector<uint16_t>{0, 1}));

    // Then A sets it: B's first notification carries B's own sequence id 0.
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, kSetControl0, 3);
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacB) == std::vector<uint16_t>{0}));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacA), 2);

    // A change of the entity's own has no requester: both, each with its next id.
    sends.clear();
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacA) == std::vector<uint16_t>{2}));
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacB) == std::vector<uint16_t>{1}));
}

TEST(nanoavb_entity_registered_controllers, sequence_id_advances_when_the_send_fails)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    bool fail = false;
    handler.set_send_response([&](Eui48 const& dest, std::span<uint8_t const> resp) {
        sends.push_back({dest, std::vector<uint8_t>(resp.begin(), resp.end())});
        return !fail;
    });
    register_controller(handler, kEntity, kControllerA, kMacA);
    fail = true;
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    fail = false;
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacA) == std::vector<uint16_t>{0, 1}));
}

TEST(nanoavb_entity_registered_controllers, sixteen_controllers_register_and_are_each_notified)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    EXPECT_EQ(AemCommandHandler::MAX_UNSOLICITED_REGISTRATIONS, size_t{16});
    for (uint8_t i = 0; i < 16; ++i) {
        register_controller(handler, kEntity, Eui64{0xC0, 0, 0, 0, 0, 0, 1, i}, Eui48{0x02, 0, 0, 0, 1, i});
        EXPECT_EQ(header_of(sends.back()).status(), AEM_STATUS_SUCCESS);
    }
    register_controller(handler, kEntity, Eui64{0xC0, 0, 0, 0, 0, 0, 2, 0}, Eui48{0x02, 0, 0, 0, 2, 0});
    EXPECT_EQ(header_of(sends.back()).status(), AEM_STATUS_NO_RESOURCES);
    sends.clear();

    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{16});
    for (uint8_t i = 0; i < 16; ++i) {
        EXPECT_TRUE(
            (unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, Eui48{0x02, 0, 0, 0, 1, i}) == std::vector<uint16_t>{0}));
    }
}

TEST(nanoavb_entity_registered_controllers, re_registration_from_a_new_mac_starts_the_sequence_again)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    register_controller(handler, kEntity, kControllerA, kMacA);  // the same station: kept
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacA) == std::vector<uint16_t>{0, 1}));
    register_controller(handler, kEntity, kControllerA, kMacB);  // moved
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacB) == std::vector<uint16_t>{0}));
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacA) == std::vector<uint16_t>{0, 1}));
}

TEST(nanoavb_entity_registered_controllers, mvu_sequence_ids_are_per_controller_and_apart_from_the_aem_ones)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    auto const info = StreamInputs::info_of(1);
    EXPECT_TRUE(info.has_value());
    std::array<uint8_t, MvuGetStreamInputInfoExResponse::LENGTH> body{};
    span_store(std::span<uint8_t>{body}, info.value_or(MvuGetStreamInputInfoExResponse{}));
    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);

    // Milan v1.3 Section 5.4.5.2: the MVU ids count from zero apart from the AEM ones.
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacA) == std::vector<uint16_t>{0, 1}));
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacB) == std::vector<uint16_t>{0, 1}));
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacA) == std::vector<uint16_t>{1}));
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_CONTROL, kMacB) == std::vector<uint16_t>{0}));

    auto const hdr = mvu_header_of(sends[0]);
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(hdr.common.target_entity_id, kEntity);
    EXPECT_EQ(hdr.common.controller_entity_id, sends[0].dest == kMacA ? kControllerA : kControllerB);
    EXPECT_EQ(size_t{hdr.common.control_data_length()}, AecpMvuDu::MVU_DATA_LENGTH + body.size());
    EXPECT_TRUE(std::ranges::equal(mvu_body_of(sends[0]), body));
}

// As emit_unsolicited() does: with nothing to send through, no controller's MVU id is spent.
TEST(nanoavb_entity_registered_controllers, an_mvu_notification_without_a_sender_spends_no_id)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    std::array<uint8_t, MvuGetStreamInputInfoExResponse::LENGTH> const body{};
    handler.set_send_response({});
    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
    capture_sends(handler, sends);
    sends.clear();
    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacA) == std::vector<uint16_t>{0}));
}

TEST(nanoavb_entity_registered_controllers, an_mvu_notification_raised_by_a_command_leaves_out_its_controller)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    std::array<uint8_t, MvuGetStreamInputInfoExResponse::LENGTH> body{};
    handler.attach_mvu_stream_inputs(
        &sends,
        MvuStreamInputCallbacks{
            .binding_command =
                [&](statusbar::atdecc::AcmpCommandResponse const& command, MvuStreamInputCallbacks::Respond const& respond) {
                    respond(command);
                    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
                },
            .stream_input_info = [](uint16_t const index) { return StreamInputs::info_of(index); }});
    sends.clear();

    EXPECT_TRUE(inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 7));
    EXPECT_TRUE(mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacA).empty());
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacB) == std::vector<uint16_t>{0}));

    // Once the command is answered there is no requester: both are told.
    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacA) == std::vector<uint16_t>{0}));
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(sends, MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacB) == std::vector<uint16_t>{0, 1}));
}

TEST(nanoavb_entity_registered_controllers, a_plain_entity_sends_no_mvu_notification)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    sends.clear();
    std::array<uint8_t, MvuGetStreamInputInfoExResponse::LENGTH> const body{};
    handler.emit_mvu_unsolicited(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, body);
    EXPECT_TRUE(sends.empty());
}

//
// Notifications from connection management - Milan v1.3 Section 5.4.5.3, Table 5.21
//

namespace {

/// What stands in for Milan connection management where a command changes a
/// binding: two stream inputs, each bound or not, whose sink takes a bind or
/// unbind unless it is told to answer another status.
struct BoundInputs
{
    std::array<std::optional<statusbar::atdecc::MilanBinding>, 2> bindings{};
    uint8_t acmp_status{statusbar::atdecc::ACMP_STATUS_SUCCESS};

    void attach(AemCommandHandler& handler)
    {
        handler.attach_mvu_stream_inputs(
            this,
            MvuStreamInputCallbacks{
                .binding_command =
                    [this](statusbar::atdecc::AcmpCommandResponse const& command, MvuStreamInputCallbacks::Respond const& respond) {
                        on_binding_command(command, respond);
                    },
                .stream_input_info = [](uint16_t const index) { return StreamInputs::info_of(index); },
                .binding = [this](uint16_t const index) { return index < bindings.size() ? bindings.at(index) : std::nullopt; }});
    }

    void on_binding_command(statusbar::atdecc::AcmpCommandResponse const& command, MvuStreamInputCallbacks::Respond const& respond)
    {
        if (acmp_status == statusbar::atdecc::ACMP_STATUS_SUCCESS) {
            bindings.at(command.listener_unique_id.get()) = binding_asked(command);
        }
        auto response = command;
        response.set_status(acmp_status);
        respond(response);
    }

    static auto binding_asked(statusbar::atdecc::AcmpCommandResponse const& command)
        -> std::optional<statusbar::atdecc::MilanBinding>
    {
        if (command.message_type() != statusbar::atdecc::ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND) {
            return std::nullopt;
        }
        return statusbar::atdecc::MilanBinding{
            .listener_unique_id = command.listener_unique_id.get(),
            .talker_entity_id = command.talker_entity_id,
            .talker_unique_id = command.talker_unique_id.get(),
            .controller_entity_id = command.controller_entity_id,
            .streaming_wait = command.is_streaming_wait()};
    }
};

/// A Milan entity with controllers A and B registered and nothing sent yet.
struct NotifiedEntity
{
    SetAcceptingHandler app{};
    AemCommandHandler handler{app};
    std::vector<CapturedSend> sends{};

    NotifiedEntity()
    {
        handler.set_entity_id(kEntity);
        handler.set_milan_info(kMilanInfo);
        capture_sends(handler, sends);
        register_controller(handler, kEntity, kControllerA, kMacA);
        register_controller(handler, kEntity, kControllerB, kMacB);
        sends.clear();
    }
    NotifiedEntity(NotifiedEntity const&) = delete;
    auto operator=(NotifiedEntity const&) -> NotifiedEntity& = delete;

    [[nodiscard]] auto told(uint16_t const command, Eui48 const& dest) const -> std::vector<uint16_t>
    {
        return mvu_unsolicited_sequence_ids(sends, command, dest);
    }
};

auto same_octets(std::span<uint8_t const> const a, std::span<uint8_t const> const b) -> bool
{
    return std::ranges::equal(a, b);
}

}  // namespace

TEST(nanoavb_entity_mvu_notify, a_bind_stream_that_changes_the_binding_is_told_to_the_others_after_its_response)
{
    NotifiedEntity e;
    BoundInputs inputs;
    inputs.attach(e.handler);

    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 7));
    EXPECT_EQ(e.sends.size(), size_t{2});
    if (e.sends.size() != 2) {
        return;
    }
    // The response to A first, then the notification to B alone, with B's first MVU sequence id.
    EXPECT_EQ(e.sends[0].dest, kMacA);
    EXPECT_FALSE(mvu_header_of(e.sends[0]).is_unsolicited());
    EXPECT_EQ(mvu_header_of(e.sends[0]).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(e.sends[1].dest, kMacB);
    EXPECT_TRUE((e.told(MVU_COMMAND_BIND_STREAM, kMacB) == std::vector<uint16_t>{0}));
    EXPECT_TRUE(e.told(MVU_COMMAND_BIND_STREAM, kMacA).empty());
    // It is addressed to B and says what the response said.
    auto const hdr = mvu_header_of(e.sends[1]);
    EXPECT_EQ(hdr.common.controller_entity_id, kControllerB);
    EXPECT_EQ(hdr.common.target_entity_id, kEntity);
    EXPECT_EQ(hdr.common.status(), MVU_STATUS_SUCCESS);
    EXPECT_TRUE(same_octets(mvu_body_of(e.sends[1]), mvu_body_of(e.sends[0])));
    EXPECT_EQ(mvu_body_of(e.sends[1]).size(), MvuBindStream::LENGTH);
}

TEST(nanoavb_entity_mvu_notify, a_bind_stream_that_changes_nothing_is_answered_and_not_told)
{
    NotifiedEntity e;
    BoundInputs inputs;
    inputs.attach(e.handler);
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 7));
    e.sends.clear();

    // The same bind from the same controller: SUCCESS, and the state is as it was.
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 8));
    EXPECT_EQ(e.sends.size(), size_t{1});
    EXPECT_TRUE(e.told(MVU_COMMAND_BIND_STREAM, kMacB).empty());

    // Each binding parameter counts (5.3.8.3): the stopped state, then the controller.
    auto const stopped = bind_stream(kStreamInput, 1, MVU_BIND_STREAM_FLAG_STREAMING_WAIT);
    // (B's first MVU notification was the first bind's, so this is its second.)
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, stopped, 9));
    EXPECT_TRUE((e.told(MVU_COMMAND_BIND_STREAM, kMacB) == std::vector<uint16_t>{1}));
    EXPECT_TRUE(inject_mvu(e.handler, kControllerB, kMacB, MVU_COMMAND_BIND_STREAM, stopped, 10));
    EXPECT_TRUE((e.told(MVU_COMMAND_BIND_STREAM, kMacA) == std::vector<uint16_t>{0}));
    EXPECT_TRUE((e.told(MVU_COMMAND_BIND_STREAM, kMacB) == std::vector<uint16_t>{1}));
}

TEST(nanoavb_entity_mvu_notify, an_unbind_stream_is_told_when_the_input_was_bound)
{
    NotifiedEntity e;
    BoundInputs inputs;
    inputs.attach(e.handler);

    // Not bound: SUCCESS from the sink, nothing changed, nobody told.
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, stream_input(kStreamInput, 0), 1));
    EXPECT_EQ(e.sends.size(), size_t{1});

    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 0), 2));
    e.sends.clear();
    EXPECT_TRUE(inject_mvu(e.handler, kControllerB, kMacB, MVU_COMMAND_UNBIND_STREAM, stream_input(kStreamInput, 0), 3));
    EXPECT_EQ(e.sends.size(), size_t{2});
    if (e.sends.size() != 2) {
        return;
    }
    // B asked: A is told, with A's first MVU sequence id (the bind was A's own, told to B).
    EXPECT_TRUE(e.told(MVU_COMMAND_UNBIND_STREAM, kMacB).empty());
    EXPECT_TRUE((e.told(MVU_COMMAND_UNBIND_STREAM, kMacA) == std::vector<uint16_t>{0}));
    EXPECT_TRUE(same_octets(mvu_body_of(e.sends[1]), mvu_body_of(e.sends[0])));
    EXPECT_EQ(mvu_body_of(e.sends[1]).size(), MvuStreamInput::LENGTH);
}

TEST(nanoavb_entity_mvu_notify, a_bind_or_unbind_that_is_refused_is_not_told)
{
    NotifiedEntity e;
    BoundInputs inputs;
    inputs.attach(e.handler);
    inputs.acmp_status = statusbar::atdecc::ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED;
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 1));
    // One the handler refuses itself, and one too short to name an input.
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamOutput, 1), 2));
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_UNBIND_STREAM, std::array<uint8_t, 2>{}, 3));
    EXPECT_EQ(e.sends.size(), size_t{3});
    EXPECT_TRUE(e.told(MVU_COMMAND_BIND_STREAM, kMacB).empty());
    EXPECT_TRUE(e.told(MVU_COMMAND_UNBIND_STREAM, kMacB).empty());
}

TEST(nanoavb_entity_mvu_notify, stream_inputs_that_do_not_tell_their_binding_have_no_bind_told)
{
    NotifiedEntity e;
    StreamInputs inputs;  // answers SUCCESS and gives no binding callback
    inputs.attach(e.handler, e.sends);
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 1));
    EXPECT_EQ(e.sends.size(), size_t{1});
    EXPECT_TRUE(e.told(MVU_COMMAND_BIND_STREAM, kMacB).empty());
}

TEST(nanoavb_entity_mvu_notify, a_changed_stream_input_is_told_as_its_info_ex_response_to_every_controller)
{
    NotifiedEntity e;
    BoundInputs inputs;
    inputs.attach(e.handler);

    EXPECT_TRUE(e.handler.notify_stream_input_info_ex(1));
    EXPECT_EQ(e.sends.size(), size_t{2});
    EXPECT_TRUE((e.told(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacA) == std::vector<uint16_t>{0}));
    EXPECT_TRUE((e.told(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacB) == std::vector<uint16_t>{0}));
    if (e.sends.size() != 2) {
        return;
    }
    // Figure 5.12, as a GET_STREAM_INPUT_INFO_EX of that input would be answered.
    MvuGetStreamInputInfoExResponse told{};
    span_load(told, mvu_body_of(e.sends[0]));
    EXPECT_TRUE(told == *StreamInputs::info_of(1));
    EXPECT_EQ(mvu_body_of(e.sends[0]).size(), MvuGetStreamInputInfoExResponse::LENGTH);
    EXPECT_EQ(mvu_header_of(e.sends[0]).common.status(), MVU_STATUS_SUCCESS);

    // Each controller's MVU sequence id goes on from its last, whatever raised the notification.
    EXPECT_TRUE(inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_BIND_STREAM, bind_stream(kStreamInput, 1), 1));
    EXPECT_TRUE((e.told(MVU_COMMAND_BIND_STREAM, kMacB) == std::vector<uint16_t>{1}));
    EXPECT_TRUE(e.handler.notify_stream_input_info_ex(0));
    EXPECT_TRUE((e.told(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacA) == std::vector<uint16_t>{0, 1}));
    EXPECT_TRUE((e.told(MVU_COMMAND_GET_STREAM_INPUT_INFO_EX, kMacB) == std::vector<uint16_t>{0, 2}));
}

TEST(nanoavb_entity_mvu_notify, a_stream_input_the_entity_does_not_have_is_not_told)
{
    NotifiedEntity e;
    EXPECT_FALSE(e.handler.notify_stream_input_info_ex(0));  // IEEE 1722.1 connection management: nothing attached
    BoundInputs inputs;
    inputs.attach(e.handler);
    EXPECT_FALSE(e.handler.notify_stream_input_info_ex(2));
    EXPECT_TRUE(e.sends.empty());
}

//
// Controller monitor - Milan v1.3 Section 5.4.5.4
//

namespace {

/// The CONTROLLER_AVAILABLE commands sent to @p dest.
auto controller_available_commands(std::vector<CapturedSend> const& sends, Eui48 const& dest) -> std::vector<AemDu>
{
    std::vector<AemDu> out;
    for (auto const& s : sends) {
        auto const hdr = header_of(s);
        if (s.dest == dest && hdr.is_command() && hdr.command_code() == AEM_COMMAND_CONTROLLER_AVAILABLE) {
            out.push_back(hdr);
        }
    }
    return out;
}

/// The response, with @p status, of the controller at @p src_mac to our CONTROLLER_AVAILABLE @p command.
void answer_controller_available(
    AemCommandHandler& handler, AemDu const& command, Eui48 const& src_mac, uint8_t const status = AEM_STATUS_SUCCESS)
{
    AemDu response = command;
    response.init_response(AEM_COMMAND_CONTROLLER_AVAILABLE, status, AemDu::AEM_DATA_LENGTH, false);
    std::array<uint8_t, AemDu::LENGTH> packet{};
    span_store(packet, response);
    (void)handler.process_packet(src_mac, packet, kEntity);
}

}  // namespace

TEST(nanoavb_entity_controller_monitor, a_silent_controller_is_deregistered_after_the_retry)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    auto const t0 = sm::TimePoint{} + std::chrono::seconds(1000);
    handler.set_event_time(t0);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    // Nothing within the 30 s minimum.
    handler.tick(t0 + std::chrono::seconds(29));
    EXPECT_TRUE(sends.empty());

    // By 60 s both have been asked; A answers (any status will do), B never does.
    handler.tick(t0 + std::chrono::seconds(61));
    auto const to_a = controller_available_commands(sends, kMacA);
    auto const to_b = controller_available_commands(sends, kMacB);
    EXPECT_EQ(to_a.size(), size_t{1});
    EXPECT_EQ(to_b.size(), size_t{1});
    if (to_a.size() != 1 || to_b.size() != 1) {
        return;
    }
    EXPECT_EQ(to_b.front().target_entity_id, kControllerB);
    EXPECT_EQ(to_b.front().controller_entity_id, kEntity);
    EXPECT_EQ(size_t{to_b.front().control_data_length()}, AemDu::AEM_DATA_LENGTH);
    handler.set_event_time(t0 + std::chrono::seconds(61));
    answer_controller_available(handler, to_a.front(), kMacA, AEM_STATUS_NOT_IMPLEMENTED);

    // B is retried once with the same sequence id after 250 ms (IEEE 1722.1-2021 Clause 9.3.6) ...
    handler.tick(t0 + std::chrono::seconds(61) + std::chrono::milliseconds(250));
    auto const retry = controller_available_commands(sends, kMacB);
    EXPECT_EQ(retry.size(), size_t{2});
    EXPECT_EQ(retry.back().sequence_id.get(), retry.front().sequence_id.get());
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{2});

    // ... then deregistered and told so, alone.
    handler.tick(t0 + std::chrono::seconds(61) + std::chrono::milliseconds(500));
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacB), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacA), 0);
    EXPECT_EQ(controller_available_commands(sends, kMacA).size(), size_t{1});  // A answered: not retried

    // A notification now reaches A alone.
    sends.clear();
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacA), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 0);
}

// Clause 9.2.1.2.4: only the answer to the command asked, by its sequence id and from the
// controller asked, keeps a registration; any other is no answer.
TEST(nanoavb_entity_controller_monitor, an_answer_to_another_command_keeps_no_registration)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    auto const t0 = sm::TimePoint{} + std::chrono::seconds(1000);
    handler.set_event_time(t0);
    register_controller(handler, kEntity, kControllerA, kMacA);
    sends.clear();
    handler.tick(t0 + std::chrono::seconds(61));
    auto const asked = controller_available_commands(sends, kMacA);
    EXPECT_EQ(asked.size(), size_t{1});
    if (asked.size() != 1) {
        return;
    }
    AemDu other_id = asked.front();
    other_id.sequence_id = static_cast<uint16_t>(asked.front().sequence_id.get() + 1U);
    answer_controller_available(handler, other_id, kMacA);
    AemDu other_controller = asked.front();
    other_controller.target_entity_id = kControllerB;
    answer_controller_available(handler, other_controller, kMacB);
    handler.tick(t0 + std::chrono::seconds(61) + std::chrono::milliseconds(250));
    handler.tick(t0 + std::chrono::seconds(61) + std::chrono::milliseconds(500));
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{0});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION, kMacA), 1);
}

// A host that drives process_packet() and tick() alone: a registration is timed from the last tick.
TEST(nanoavb_entity_controller_monitor, without_set_event_time_the_timer_runs_from_the_last_tick)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    auto const t0 = sm::TimePoint{} + std::chrono::seconds(1000);
    handler.tick(t0);
    register_controller(handler, kEntity, kControllerA, kMacA);
    sends.clear();
    handler.tick(t0 + std::chrono::seconds(29));
    EXPECT_TRUE(controller_available_commands(sends, kMacA).empty());
    handler.tick(t0 + std::chrono::seconds(61));
    EXPECT_EQ(controller_available_commands(sends, kMacA).size(), size_t{1});
}

TEST(nanoavb_entity_controller_monitor, any_aem_or_mvu_command_restarts_the_timer)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_milan_info(kMilanInfo);
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    auto const t0 = sm::TimePoint{} + std::chrono::seconds(1000);
    handler.set_event_time(t0);
    register_controller(handler, kEntity, kControllerA, kMacA);

    // A command every 29 s, AEM and MVU in turn, keeps it from ever being asked.
    for (int i = 1; i <= 4; ++i) {
        auto const t = t0 + std::chrono::seconds(29 * i);
        handler.tick(t);
        handler.set_event_time(t);
        auto const sequence = static_cast<uint16_t>(i);
        if (i % 2 == 0) {
            (void)inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_MILAN_INFO, kGetMilanInfo, sequence);
            continue;
        }
        inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_GET_CONFIGURATION, std::span<uint8_t const>{}, sequence);
    }
    handler.tick(t0 + std::chrono::seconds((29 * 4) + 29));
    EXPECT_TRUE(controller_available_commands(sends, kMacA).empty());
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});

    // Silent from then on: asked within 60 s.
    handler.tick(t0 + std::chrono::seconds((29 * 4) + 61));
    EXPECT_EQ(controller_available_commands(sends, kMacA).size(), size_t{1});
}

TEST(nanoavb_entity_controller_monitor, classic_has_no_monitor)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    handler.tick(sm::TimePoint{} + std::chrono::seconds(120));
    handler.tick(sm::TimePoint{} + std::chrono::seconds(240));
    EXPECT_TRUE(controller_available_commands(sends, kMacA).empty());
    EXPECT_EQ(handler.unsolicited_registration_count(), size_t{1});
}

// IEEE 1722.1-2021 Clause 7.4.1: the holder's answer to CONTROLLER_AVAILABLE, received
// as a packet, denies the contender the acquisition.
TEST(nanoavb_entity_controller_monitor, the_holders_answer_ends_the_acquire_handshake)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    handler.send_controller_available = [](Eui64 const&) { return true; };
    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> const acquire{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 1);
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 2);
    EXPECT_TRUE(handler.has_pending_acquire());
    sends.clear();

    AemDu question{};
    question.init_command(AEM_COMMAND_CONTROLLER_AVAILABLE, AemDu::AEM_DATA_LENGTH);
    question.target_entity_id = kControllerA;
    question.controller_entity_id = kEntity;
    answer_controller_available(handler, question, kMacA);
    EXPECT_FALSE(handler.has_pending_acquire());
    EXPECT_EQ(handler.acquiring_controller(), kControllerA);
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    if (reply != nullptr) {
        EXPECT_EQ(reply->dest, kMacB);
        EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_ENTITY_ACQUIRED);
    }
}

// IEEE 1722.1-2021 Clauses 7.4.1 and 9.2.1.2.4: with no callback installed the handler asks the
// holder itself, and only the answer with that command's sequence id ends the handshake.
TEST(nanoavb_entity_controller_monitor, the_holder_is_asked_by_the_handler_and_answered_by_sequence_id)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> const acquire{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 1);
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 2);
    EXPECT_TRUE(handler.has_pending_acquire());
    auto const asked = controller_available_commands(sends, kMacA);
    EXPECT_EQ(asked.size(), size_t{1});
    if (asked.size() != 1) {
        return;
    }
    EXPECT_EQ(asked.front().target_entity_id, kControllerA);
    EXPECT_EQ(asked.front().controller_entity_id, kEntity);
    sends.clear();

    AemDu other_id = asked.front();
    other_id.sequence_id = static_cast<uint16_t>(asked.front().sequence_id.get() + 1U);
    answer_controller_available(handler, other_id, kMacA);
    EXPECT_TRUE(handler.has_pending_acquire());
    EXPECT_TRUE(solicited(sends) == nullptr);

    answer_controller_available(handler, asked.front(), kMacA);
    EXPECT_FALSE(handler.has_pending_acquire());
    EXPECT_EQ(handler.acquiring_controller(), kControllerA);
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    if (reply != nullptr) {
        EXPECT_EQ(reply->dest, kMacB);
        EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_ENTITY_ACQUIRED);
    }
}

// An entity that installs send_controller_available asks the holder through it; the handler frames nothing.
TEST(nanoavb_entity_controller_monitor, an_installed_callback_still_asks_the_holder)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    std::vector<Eui64> asked;
    handler.send_controller_available = [&asked](Eui64 const& owner) {
        asked.push_back(owner);
        return true;
    };
    std::array<uint8_t, AemAcquireEntityPayload::LENGTH> const acquire{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 1);
    inject_command(handler, kEntity, kControllerB, kMacB, AEM_COMMAND_ACQUIRE_ENTITY, acquire, 2);
    EXPECT_TRUE(handler.has_pending_acquire());
    EXPECT_TRUE(asked == std::vector<Eui64>{kControllerA});
    EXPECT_TRUE(controller_available_commands(sends, kMacA).empty());
}

//
// Notification order - Milan v1.3 Section 5.4.5.3: a command's notifications follow its response
//

namespace {

/// Accepts every SET, raising @p side_effects notifications of its own from the hook.
class NotifyingHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t /*command_type*/, DescriptorId /*id*/, std::span<uint8_t const> /*value*/)
        -> uint8_t override
    {
        for (size_t n = 0; n < side_effects; ++n) {
            std::array<uint8_t, 4> const body{0x00, 0x06, 0x00, static_cast<uint8_t>(n)};  // STREAM_OUTPUT n
            aem->emit_unsolicited(AEM_COMMAND_GET_STREAM_INFO, body);
        }
        return AEM_STATUS_SUCCESS;
    }

    AemCommandHandler* aem{nullptr};
    size_t side_effects{0};
};

/// Accepts every SET, its hook raising a notification of the command's type, its own (true), or a side
/// effect (false), in turn.
class ScriptedNotifyingHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> /*value*/) -> uint8_t override
    {
        std::array<uint8_t, 4> const body{0x00, 0x06, 0x00, 0x00};
        for (bool const own : script) {
            aem->emit_unsolicited(own ? command_type : AEM_COMMAND_GET_STREAM_INFO, body);
        }
        return AEM_STATUS_SUCCESS;
    }

    AemCommandHandler* aem{nullptr};
    std::vector<bool> script;
};

/// The first solicited response in @p sends.
auto first_response(std::vector<CapturedSend> const& sends) -> std::vector<CapturedSend>::const_iterator
{
    return std::ranges::find_if(sends, [](CapturedSend const& s) { return !header_of(s).is_unsolicited(); });
}

}  // namespace

TEST(nanoavb_entity_notification_order, the_response_is_sent_before_its_notification)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONTROL, kSetControl0);
    EXPECT_EQ(sends.size(), size_t{2});
    if (sends.size() != 2) {
        return;
    }
    EXPECT_FALSE(header_of(sends[0]).is_unsolicited());
    EXPECT_EQ(sends[0].dest, kMacA);
    EXPECT_TRUE(header_of(sends[1]).is_unsolicited());
    EXPECT_EQ(sends[1].dest, kMacB);
}

TEST(nanoavb_entity_notification_order, the_commands_own_notification_follows_its_response)
{
    NotifyingHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.side_effects = 6;
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    EXPECT_EQ(sends.size(), size_t{8});
    auto const response = first_response(sends);
    EXPECT_TRUE(response != sends.end() && response->dest == kMacA);
    EXPECT_TRUE(header_of(sends.back()).is_unsolicited());
    EXPECT_EQ(header_of(sends.back()).command_code(), AEM_COMMAND_SET_STREAM_FORMAT);
    EXPECT_EQ(
        std::ranges::count_if(response, sends.cend(), [](CapturedSend const& s) { return header_of(s).is_unsolicited(); }),
        4);  // three side effects and its own
}

TEST(nanoavb_entity_notification_order, every_own_notification_follows_the_response)
{
    ScriptedNotifyingHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.script = {true, false, false, true};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    auto const response = first_response(sends);
    EXPECT_TRUE(response != sends.end());
    auto const own_before = std::ranges::count_if(
        sends.cbegin(), response, [](CapturedSend const& s) { return header_of(s).command_code() != AEM_COMMAND_GET_STREAM_INFO; });
    EXPECT_EQ(own_before, 0);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_FORMAT, kMacB), 3);
}

// Milan v1.3 Sections 5.4.5.1 and 5.4.5.3: six of the command's own type (five raised by its hook,
// then its own) find four slots; none goes ahead of the response, and the two not held spend the
// other controller's sequence ids, so it sees the gap.
TEST(nanoavb_entity_notification_order, own_notifications_past_the_queue_never_precede_the_response)
{
    ScriptedNotifyingHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.script = {true, true, true, true, true};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    EXPECT_TRUE(first_response(sends) == sends.begin());
    EXPECT_TRUE((unsolicited_sequence_ids(sends, AEM_COMMAND_SET_STREAM_FORMAT, kMacB) == std::vector<uint16_t>{2, 3, 4, 5}));
    EXPECT_EQ(sends.size(), size_t{1} + AemCommandHandler::MAX_QUEUED_NOTIFICATIONS);
}

TEST(nanoavb_entity_notification_order, notifications_past_the_queue_keep_their_order)
{
    NotifyingHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.side_effects = 7;
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    std::vector<uint8_t> raised;
    for (auto const& s : sends) {
        if (header_of(s).is_unsolicited() && header_of(s).command_code() == AEM_COMMAND_GET_STREAM_INFO) {
            raised.push_back(body_of(s)[3]);
        }
    }
    EXPECT_TRUE(raised == (std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(header_of(sends.back()).command_code(), AEM_COMMAND_SET_STREAM_FORMAT);
    EXPECT_TRUE(first_response(sends) < sends.end() - 1);
}

TEST(nanoavb_entity_notification_order, a_notification_raised_outside_a_command_goes_at_once)
{
    NotifyingHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.side_effects = 1;
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    // The entity's own change: hook side effect, then the change's own, as raised.
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{2});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_STREAM_INFO, kMacB), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_CONTROL, kMacB), 1);
}

namespace {

/// Accepts every SET, its hook raising a notification of the command's type whose body fills a
/// whole response, longer than one IEEE 1722.1 AECPDU carries (Milan v1.3 Section 5.4.1).
class LongNotifyingHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t command_type, DescriptorId /*id*/, std::span<uint8_t const> /*value*/) -> uint8_t override
    {
        std::array<uint8_t, MAX_AEM_RESPONSE_SIZE> body{};
        body.fill(0x5A);
        aem->emit_unsolicited(command_type, body);
        return AEM_STATUS_SUCCESS;
    }

    AemCommandHandler* aem{nullptr};
};

}  // namespace

TEST(nanoavb_entity_notification_order, a_notification_longer_than_an_aecpdu_follows_the_response)
{
    LongNotifyingHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    EXPECT_EQ(sends.size(), size_t{3});
    EXPECT_TRUE(first_response(sends) == sends.begin());
    auto const long_one = std::ranges::find_if(sends, [](CapturedSend const& s) { return body_of(s).size() > MAX_AEM_BODY; });
    EXPECT_TRUE(long_one != sends.end());
    if (long_one == sends.end()) {
        return;
    }
    EXPECT_TRUE(header_of(*long_one).is_unsolicited());
    EXPECT_EQ(body_of(*long_one).size(), MAX_AEM_RESPONSE_SIZE);
}

namespace {

/// Accepts every SET, its hook leaving @p side_effects notifications for after the response.
class AfterResponseHandler : public AemEntityHandler
{
  public:
    auto on_set_descriptor_value(uint16_t /*command_type*/, DescriptorId /*id*/, std::span<uint8_t const> /*value*/)
        -> uint8_t override
    {
        aem->call_after_response([this] {
            for (size_t n = 0; n < side_effects; ++n) {
                std::array<uint8_t, 4> const body{0x00, 0x06, 0x00, static_cast<uint8_t>(n)};  // STREAM_OUTPUT n
                aem->emit_unsolicited(AEM_COMMAND_GET_STREAM_INFO, body);
            }
        });
        return AEM_STATUS_SUCCESS;
    }

    AemCommandHandler* aem{nullptr};
    size_t side_effects{0};
};

/// The STREAM_OUTPUT indexes of the GET_STREAM_INFO notifications in @p sends to @p dest, in order.
auto stream_info_indexes(std::vector<CapturedSend> const& sends, Eui48 const& dest) -> std::vector<uint8_t>
{
    std::vector<uint8_t> raised;
    for (auto const& s : sends) {
        if (s.dest == dest && header_of(s).is_unsolicited() && header_of(s).command_code() == AEM_COMMAND_GET_STREAM_INFO) {
            raised.push_back(body_of(s)[3]);
        }
    }
    return raised;
}

}  // namespace

TEST(nanoavb_entity_notification_order, work_left_for_after_the_response_follows_it_and_its_notifications)
{
    AfterResponseHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.side_effects = 7;  // more than the queue holds
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_FORMAT, kSetFormatOut0);
    EXPECT_EQ(sends.size(), size_t{16});
    if (sends.size() != 16) {
        return;
    }
    EXPECT_TRUE(first_response(sends) == sends.begin());
    EXPECT_EQ(sends[0].dest, kMacA);
    EXPECT_EQ(header_of(sends[1]).command_code(), AEM_COMMAND_SET_STREAM_FORMAT);  // its own, held, to B alone
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_FORMAT, kMacA), 0);
    // The work's, beyond what A requested, to every registered controller (Milan v1.3 Section 5.4.5.3).
    EXPECT_TRUE(stream_info_indexes(sends, kMacA) == (std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6}));
    EXPECT_TRUE(stream_info_indexes(sends, kMacB) == (std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6}));
}

TEST(nanoavb_entity_notification_order, work_left_outside_a_command_runs_at_once)
{
    AfterResponseHandler app;
    AemCommandHandler handler{app};
    app.aem = &handler;
    app.side_effects = 2;
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    // The entity's own change: the hook's work, then the change's own notification, as raised.
    EXPECT_EQ(handler.apply_local_descriptor_value(AEM_COMMAND_SET_CONTROL, kSetControl0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(sends.size(), size_t{3});
    EXPECT_TRUE(stream_info_indexes(sends, kMacB) == (std::vector<uint8_t>{0, 1}));
    EXPECT_EQ(header_of(sends.back()).command_code(), AEM_COMMAND_SET_CONTROL);
}

//
// GET_AS_PATH - IEEE 1722.1-2021 Clause 7.4.41
//

namespace {

Eui64 const kPathGm{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x01};
Eui64 const kPathBridge{0x00, 0x1B, 0x21, 0xFF, 0xFE, 0x00, 0x00, 0x02};

/// A GET_AS_PATH command body for AVB_INTERFACE @p index.
auto as_path_command(uint16_t const index) -> std::array<uint8_t, AemGetAsPathCommandPayload::LENGTH>
{
    std::array<uint8_t, AemGetAsPathCommandPayload::LENGTH> body{};
    span_store(body, AemGetAsPathCommandPayload{.descriptor_index = index});
    return body;
}

/// The path a GET_AS_PATH response @p body carries, as many identities as its count says.
auto path_in(std::span<uint8_t const> const body) -> std::vector<Eui64>
{
    AemAsPathResponseHeader header{};
    span_load(header, body);
    std::vector<Eui64> path(header.count.get());
    for (size_t i = 0; i < path.size(); ++i) {
        span_load(path[i], body.subspan(AemAsPathResponseHeader::LENGTH + (i * Eui64::LENGTH)));
    }
    return path;
}

/// Answers AVB_INTERFACE 0 with @p path.
void serve_as_path(AemCommandHandler& handler, AsPath const& path)
{
    handler.set_get_as_path([&path](uint16_t const index, AsPath& out) -> bool {
        out = path;
        return index == 0;
    });
}

}  // namespace

TEST(nanoavb_entity_get_as_path, not_implemented_without_a_callback)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_GET_AS_PATH, as_path_command(0));
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends.front()).status(), AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(handler.notify_as_path_changed(0), AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(sends.size(), size_t{1});
}

TEST(nanoavb_entity_get_as_path, the_path_is_answered_grandmaster_first)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    AsPath path{};
    path.push_back(kPathGm);
    path.push_back(kPathBridge);
    serve_as_path(handler, path);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_GET_AS_PATH, as_path_command(0));
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends.back()).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(body_of(sends.back()).size(), AemAsPathResponseHeader::LENGTH + (2 * Eui64::LENGTH));
    EXPECT_TRUE(path_in(body_of(sends.back())) == (std::vector<Eui64>{kPathGm, kPathBridge}));
}

TEST(nanoavb_entity_get_as_path, another_interface_or_a_short_command_is_refused)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    AsPath const path{};
    serve_as_path(handler, path);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);

    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_GET_AS_PATH, as_path_command(1));
    EXPECT_EQ(header_of(sends.back()).status(), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    std::array<uint8_t, 2> const short_command{};
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_GET_AS_PATH, short_command);
    EXPECT_EQ(header_of(sends.back()).status(), AEM_STATUS_BAD_ARGUMENTS);
}

TEST(nanoavb_entity_get_as_path, a_changed_path_is_notified_to_every_registered_controller)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    AsPath path{};
    path.push_back(kPathGm);
    serve_as_path(handler, path);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    EXPECT_EQ(handler.notify_as_path_changed(0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_AS_PATH, kMacA), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_AS_PATH, kMacB), 1);
    EXPECT_TRUE(path_in(body_of(sends.front())) == (std::vector<Eui64>{kPathGm}));
    // The grandmaster lost: an empty path, notified too.
    path.clear();
    EXPECT_EQ(handler.notify_as_path_changed(0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(body_of(sends.back()).size(), AemAsPathResponseHeader::LENGTH);
    EXPECT_EQ(handler.notify_as_path_changed(1), AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_AS_PATH, kMacA), 2);
}

//
// SET_MAX_TRANSIT_TIME / GET_MAX_TRANSIT_TIME - IEEE 1722.1-2021 Clauses 7.4.76, 7.4.77
//

namespace {

/// The talkers' presentation offsets: STREAM_OUTPUT 0 and 1, the set hook answering @p refusal when set.
struct OffsetStore
{
    std::array<uint32_t, 2> offsets{2'000'000, 2'000'000};
    std::optional<uint8_t> refusal{};
    int sets{0};

    void serve(AemCommandHandler& handler)
    {
        handler.set_presentation_offset_hooks(
            [this](uint16_t const index) -> std::optional<uint32_t> {
                return index < offsets.size() ? std::optional<uint32_t>{offsets.at(index)} : std::nullopt;
            },
            [this](uint16_t const index, uint32_t const offset_ns) -> uint8_t {
                ++sets;
                if (refusal) {
                    return *refusal;
                }
                offsets.at(index) = offset_ns;
                return AEM_STATUS_SUCCESS;
            });
    }
};

/// A MAX_TRANSIT_TIME body for (@p type, @p index), @p time_ns in it.
auto max_transit_time_body(uint16_t const type, uint16_t const index, uint64_t const time_ns = 0)
    -> std::array<uint8_t, AemSetMaxTransitTimePayload::LENGTH>
{
    std::array<uint8_t, AemSetMaxTransitTimePayload::LENGTH> body{};
    span_store(body, AemSetMaxTransitTimePayload{.descriptor_type = type, .descriptor_index = index, .max_transit_time = time_ns});
    return body;
}

/// The max_transit_time a MAX_TRANSIT_TIME response @p body carries.
auto transit_time_in(std::span<uint8_t const> const body) -> uint64_t
{
    AemSetMaxTransitTimePayload payload{};
    span_load(payload, body);
    return payload.max_transit_time.get();
}

/// GET_MAX_TRANSIT_TIME of STREAM_OUTPUT @p index: the time answered, 0 when refused.
auto transit_time_of(AemCommandHandler& handler, uint16_t const index) -> uint64_t
{
    auto const body = max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, index);
    auto const r = make_test_result(handler, create_aem_header(AEM_COMMAND_GET_MAX_TRANSIT_TIME), std::span{body}.first(4));
    return r.status == AEM_STATUS_SUCCESS ? transit_time_in(r.response_data()) : 0;
}

}  // namespace

TEST(nanoavb_entity_max_transit_time, not_implemented_without_the_hooks)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    auto const body = max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 0, 1'000'000);
    EXPECT_EQ(
        make_test_result(handler, create_aem_header(AEM_COMMAND_SET_MAX_TRANSIT_TIME), body).status, AEM_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(
        make_test_result(handler, create_aem_header(AEM_COMMAND_GET_MAX_TRANSIT_TIME), std::span{body}.first(4)).status,
        AEM_STATUS_NOT_IMPLEMENTED);
}

TEST(nanoavb_entity_max_transit_time, a_set_is_put_in_force_answered_and_notified_to_the_others)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    OffsetStore store;
    store.serve(handler);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    inject_command(
        handler,
        kEntity,
        kControllerA,
        kMacA,
        AEM_COMMAND_SET_MAX_TRANSIT_TIME,
        max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 1, 1'500'000));
    EXPECT_EQ(store.offsets[1], 1'500'000U);
    EXPECT_EQ(transit_time_of(handler, 1), uint64_t{1'500'000});
    EXPECT_EQ(transit_time_of(handler, 0), uint64_t{2'000'000});
    auto const response = first_response(sends);
    EXPECT_TRUE(response == sends.begin());  // the response first, then the notification
    EXPECT_EQ(header_of(sends.front()).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(transit_time_in(body_of(sends.front())), uint64_t{1'500'000});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MAX_TRANSIT_TIME, kMacA), 0);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MAX_TRANSIT_TIME, kMacB), 1);
    EXPECT_EQ(transit_time_in(body_of(sends.back())), uint64_t{1'500'000});
}

// IEEE 1722.1-2021 Table 7-141: a hook that needs time answers IN_PROGRESS; the SET is held as
// a deferred one, repeated, and finished by complete_pending_command() with the final response.
TEST(nanoavb_entity_max_transit_time, a_set_whose_hook_needs_time_is_deferred_and_completed)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    OffsetStore store;
    store.refusal = AEM_STATUS_IN_PROGRESS;
    store.serve(handler);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    auto const body = max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 1, 1'500'000);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_MAX_TRANSIT_TIME, body);
    EXPECT_EQ(sends.size(), size_t{1});
    EXPECT_EQ(header_of(sends.front()).status(), AEM_STATUS_IN_PROGRESS);
    EXPECT_EQ(handler.pending_command_count(), size_t{1});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MAX_TRANSIT_TIME, kMacB), 0);

    store.offsets[1] = 1'500'000;
    EXPECT_TRUE(
        handler.complete_pending_command(AEM_COMMAND_SET_MAX_TRANSIT_TIME, DESCRIPTOR_STREAM_OUTPUT, 1, AEM_STATUS_SUCCESS));
    EXPECT_EQ(handler.pending_command_count(), size_t{0});
    EXPECT_EQ(sends.size(), size_t{3});
    EXPECT_EQ(header_of(sends[1]).status(), AEM_STATUS_SUCCESS);
    EXPECT_FALSE(header_of(sends[1]).is_unsolicited());
    EXPECT_TRUE(std::ranges::equal(body_of(sends[1]), body));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MAX_TRANSIT_TIME, kMacB), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MAX_TRANSIT_TIME, kMacA), 0);
}

TEST(nanoavb_entity_max_transit_time, a_refused_set_answers_the_time_in_force)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    OffsetStore store;
    store.serve(handler);
    store.refusal = AEM_STATUS_STREAM_IS_RUNNING;
    auto const set = create_aem_header(AEM_COMMAND_SET_MAX_TRANSIT_TIME);
    auto r = make_test_result(handler, set, max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 0, 1'000'000));
    EXPECT_EQ(r.status, AEM_STATUS_STREAM_IS_RUNNING);
    EXPECT_EQ(transit_time_in(r.response_data()), uint64_t{2'000'000});

    // Past the most a presentation offset may be: refused before the hook.
    store.refusal.reset();
    r = make_test_result(
        handler, set, max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 0, uint64_t{MILAN_PRESENTATION_OFFSET_MAX_NS} + 1));
    EXPECT_EQ(r.status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(transit_time_in(r.response_data()), uint64_t{2'000'000});
    EXPECT_EQ(store.sets, 1);

    // Another controller's lock: ENTITY_LOCKED with the time in force, the hook not asked.
    auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock.controller_entity_id = kControllerB;
    std::array<uint8_t, 16> const lock_body{};
    EXPECT_EQ(make_test_result(handler, lock, lock_body).status, AEM_STATUS_SUCCESS);
    auto locked_out = set;
    locked_out.controller_entity_id = kControllerA;
    r = make_test_result(handler, locked_out, max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 0, 1'000'000));
    EXPECT_EQ(r.status, AEM_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(transit_time_in(r.response_data()), uint64_t{2'000'000});
    EXPECT_EQ(store.sets, 1);
    EXPECT_EQ(store.offsets[0], 2'000'000U);
}

TEST(nanoavb_entity_max_transit_time, only_a_talkers_stream_output_has_one)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    OffsetStore store;
    store.serve(handler);
    auto const set = create_aem_header(AEM_COMMAND_SET_MAX_TRANSIT_TIME);
    auto const get = create_aem_header(AEM_COMMAND_GET_MAX_TRANSIT_TIME);
    auto const input = max_transit_time_body(DESCRIPTOR_STREAM_INPUT, 0, 1'000'000);
    EXPECT_EQ(make_test_result(handler, set, input).status, AEM_STATUS_NOT_SUPPORTED);
    EXPECT_EQ(make_test_result(handler, get, std::span{input}.first(4)).status, AEM_STATUS_NOT_SUPPORTED);
    auto const unknown = max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 7, 1'000'000);
    EXPECT_EQ(make_test_result(handler, set, unknown).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(make_test_result(handler, get, std::span{unknown}.first(4)).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
    auto const entity = max_transit_time_body(DESCRIPTOR_ENTITY, 0, 1'000'000);
    EXPECT_EQ(make_test_result(handler, set, entity).status, AEM_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(make_test_result(handler, set, std::span{entity}.first(6)).status, AEM_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(store.sets, 0);
}

TEST(nanoavb_entity_max_transit_time, under_milan13_the_time_in_force_again_is_neither_applied_nor_notified)
{
    for (auto const profile : {AemCommandHandler::Profile::milan13, AemCommandHandler::Profile::classic}) {
        SetAcceptingHandler app;
        AemCommandHandler handler{app};
        handler.set_profile(profile);
        handler.set_entity_id(kEntity);
        OffsetStore store;
        store.serve(handler);
        std::vector<CapturedSend> sends;
        capture_sends(handler, sends);
        register_controller(handler, kEntity, kControllerB, kMacB);
        sends.clear();
        inject_command(
            handler,
            kEntity,
            kControllerA,
            kMacA,
            AEM_COMMAND_SET_MAX_TRANSIT_TIME,
            max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 0, 2'000'000));
        bool const milan = profile == AemCommandHandler::Profile::milan13;
        EXPECT_EQ(header_of(sends.front()).status(), AEM_STATUS_SUCCESS);
        EXPECT_EQ(store.sets, milan ? 0 : 1);
        EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_MAX_TRANSIT_TIME, kMacB), milan ? 0 : 1);
    }
}

//
// SET_MEDIA_CLOCK_REFERENCE_INFO / GET_MEDIA_CLOCK_REFERENCE_INFO - Milan v1.3 Sections 5.4.4.4, 5.4.4.5
//

namespace {

/// A SET_MEDIA_CLOCK_REFERENCE_INFO body for CLOCK_DOMAIN @p index with @p flags, @p prio and @p name.
auto mcr_set_body(uint16_t const index, uint8_t const flags, uint8_t const prio, std::string_view const name)
    -> std::array<uint8_t, MvuMediaClockReferenceInfoPayload::LENGTH>
{
    MvuMediaClockReferenceInfoPayload payload{};
    payload.clock_domain_index = index;
    payload.flags = flags;
    payload.default_mcr_prio = 0x11;  // ignored in a command
    payload.user_mcr_prio = prio;
    payload.media_clock_domain_name = AtdeccString{name};
    std::array<uint8_t, MvuMediaClockReferenceInfoPayload::LENGTH> body{};
    span_store(body, payload);
    return body;
}

/// The settings an MVU MEDIA_CLOCK_REFERENCE_INFO response @p s carries.
auto mcr_of(CapturedSend const& s) -> MvuMediaClockReferenceInfoPayload
{
    MvuMediaClockReferenceInfoPayload payload{};
    if (mvu_body_of(s).size() >= MvuMediaClockReferenceInfoPayload::LENGTH) {
        span_load(payload, mvu_body_of(s));
    }
    return payload;
}

/// What GET_MEDIA_CLOCK_REFERENCE_INFO of CLOCK_DOMAIN @p index answers @p handler's controller A.
auto get_mcr(AemCommandHandler& handler, std::vector<CapturedSend>& sends, uint16_t const index) -> CapturedSend
{
    std::array<uint8_t, MvuGetMediaClockReferenceInfoCommand::LENGTH> body{};
    span_store(body, MvuGetMediaClockReferenceInfoCommand{.clock_domain_index = index});
    (void)inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, body);
    return sends.back();
}

/// A Milan entity over the clock model (CLOCK_DOMAIN 0 only), its sends captured.
struct McrEntity
{
    EntityModel model{create_clock_model()};
    AemCommandHandler handler{model};
    std::vector<CapturedSend> sends;

    McrEntity()
    {
        handler.set_entity_id(kEntity);
        handler.set_milan_info(kMilanInfo);
        capture_sends(handler, sends);
    }
};

}  // namespace

TEST(nanoavb_entity_mvu_mcr, get_answers_the_defaults_for_a_clock_domain_the_model_has)
{
    McrEntity e;
    auto const reply = get_mcr(e.handler, e.sends, 0);
    EXPECT_EQ(mvu_header_of(reply).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(mvu_body_of(reply).size(), MvuMediaClockReferenceInfoPayload::LENGTH);
    auto const info = mcr_of(reply);
    EXPECT_EQ(info.flags.get(), MVU_MCR_FLAGS_SUPPORTED);
    EXPECT_EQ(info.default_mcr_prio.get(), MVU_MCR_PRIO_DEFAULT);
    EXPECT_EQ(info.user_mcr_prio.get(), MVU_MCR_PRIO_DEFAULT);
    EXPECT_EQ(info.media_clock_domain_name.as_string_view(), MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME);

    EXPECT_EQ(mvu_header_of(get_mcr(e.handler, e.sends, 1)).common.status(), MVU_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_EQ(mvu_body_of(e.sends.back()).size(), MvuGetMediaClockReferenceInfoCommand::LENGTH);
    std::array<uint8_t, 1> const short_command{};
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, short_command);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
}

TEST(nanoavb_entity_mvu_mcr, a_set_applies_the_fields_its_flags_mark_and_notifies_the_others_after_it)
{
    McrEntity e;
    register_controller(e.handler, kEntity, kControllerA, kMacA);
    register_controller(e.handler, kEntity, kControllerB, kMacB);
    e.sends.clear();

    // The priority alone: the name in the command is not applied.
    auto const prio_only = mcr_set_body(0, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 200, "ignored");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, prio_only);
    EXPECT_EQ(e.sends.size(), size_t{2});
    EXPECT_FALSE(mvu_header_of(e.sends.front()).is_unsolicited());  // the response first
    EXPECT_EQ(mvu_header_of(e.sends.front()).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(mcr_of(e.sends.front()).user_mcr_prio.get(), 200);
    EXPECT_EQ(mcr_of(e.sends.front()).default_mcr_prio.get(), MVU_MCR_PRIO_DEFAULT);
    EXPECT_EQ(mcr_of(e.sends.front()).media_clock_domain_name.as_string_view(), MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME);
    EXPECT_TRUE(
        mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, kMacB) == std::vector<uint16_t>{0});
    EXPECT_TRUE(mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, kMacA).empty());
    EXPECT_EQ(mcr_of(e.sends.back()).user_mcr_prio.get(), 200);

    // The name alone, then the same name again: applied and notified once.
    auto const name_only = mcr_set_body(0, MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID, 0, "Front of house");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, name_only);
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, name_only);
    EXPECT_TRUE(
        (mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, kMacB) == std::vector<uint16_t>{0, 1}));
    auto const info = mcr_of(get_mcr(e.handler, e.sends, 0));
    EXPECT_EQ(info.user_mcr_prio.get(), 200);
    EXPECT_EQ(info.media_clock_domain_name.as_string_view(), std::string_view{"Front of house"});
}

TEST(nanoavb_entity_mvu_mcr, a_bad_name_an_unknown_domain_or_another_controllers_lock_changes_nothing)
{
    McrEntity e;
    register_controller(e.handler, kEntity, kControllerB, kMacB);
    e.sends.clear();
    auto const malformed = mcr_set_body(0, MVU_MCR_FLAGS_SUPPORTED, 200, "Caf\xC3");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, malformed);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(mcr_of(e.sends.back()).user_mcr_prio.get(), MVU_MCR_PRIO_DEFAULT);  // the settings in force

    auto const unknown = mcr_set_body(1, MVU_MCR_FLAGS_SUPPORTED, 200, "Front of house");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, unknown);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_NO_SUCH_DESCRIPTOR);

    auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock.controller_entity_id = kControllerB;
    std::array<uint8_t, 16> const lock_body{};
    EXPECT_EQ(make_test_result(e.handler, lock, lock_body).status, AEM_STATUS_SUCCESS);
    auto const valid = mcr_set_body(0, MVU_MCR_FLAGS_SUPPORTED, 200, "Front of house");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, valid);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(mcr_of(e.sends.back()).media_clock_domain_name.as_string_view(), MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME);
    // The lock is asked first: a locked-out controller learns nothing of its arguments.
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, malformed);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_ENTITY_LOCKED);
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, unknown);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(mcr_of(e.sends.back()).user_mcr_prio.get(), 200);  // the command given back
    EXPECT_TRUE(mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, kMacB).empty());
    auto const info = mcr_of(get_mcr(e.handler, e.sends, 0));
    EXPECT_EQ(info.user_mcr_prio.get(), MVU_MCR_PRIO_DEFAULT);
}

//
// SET_SYSTEM_UNIQUE_ID / GET_SYSTEM_UNIQUE_ID - Milan v1.3 Sections 5.4.4.2, 5.4.4.3
//

namespace {

/// A SET_SYSTEM_UNIQUE_ID body of @p id and @p name.
auto system_unique_id_body(uint64_t const id, std::string_view const name) -> std::array<uint8_t, MvuSystemUniqueIdPayload::LENGTH>
{
    MvuSystemUniqueIdPayload payload{};
    payload.system_unique_id = id;
    payload.system_name = AtdeccString{name};
    std::array<uint8_t, MvuSystemUniqueIdPayload::LENGTH> body{};
    span_store(body, payload);
    return body;
}

/// The id an MVU SYSTEM_UNIQUE_ID response @p s carries.
auto system_unique_id_of(CapturedSend const& s) -> MvuSystemUniqueIdPayload
{
    MvuSystemUniqueIdPayload payload{};
    if (mvu_body_of(s).size() >= MvuSystemUniqueIdPayload::LENGTH) {
        span_load(payload, mvu_body_of(s));
    }
    return payload;
}

/// What GET_SYSTEM_UNIQUE_ID answers @p e's controller A.
auto get_system_unique_id(McrEntity& e) -> CapturedSend
{
    std::array<uint8_t, MVU_GET_SYSTEM_UNIQUE_ID_COMMAND_LENGTH> const reserved{};
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, reserved);
    return e.sends.back();
}

}  // namespace

TEST(nanoavb_entity_mvu_system_unique_id, get_answers_zero_and_no_name_until_a_controller_sets_them)
{
    McrEntity e;
    auto const reply = get_system_unique_id(e);
    EXPECT_EQ(mvu_header_of(reply).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(mvu_body_of(reply).size(), MvuSystemUniqueIdPayload::LENGTH);
    EXPECT_TRUE(std::ranges::all_of(mvu_body_of(reply), [](uint8_t const b) { return b == 0; }));

    std::array<uint8_t, 1> const short_command{};
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, short_command);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
    EXPECT_TRUE(mvu_body_of(e.sends.back()).empty());
    auto const set = system_unique_id_body(7, "Stage");
    (void)inject_mvu(
        e.handler,
        kControllerA,
        kMacA,
        MVU_COMMAND_SET_SYSTEM_UNIQUE_ID,
        std::span{set}.first(MvuSystemUniqueIdPayload::LENGTH - 1));
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_PAYLOAD_TOO_SHORT);
    EXPECT_EQ(system_unique_id_of(get_system_unique_id(e)).system_unique_id.get(), uint64_t{0});
}

TEST(nanoavb_entity_mvu_system_unique_id, a_set_is_answered_with_the_id_in_force_and_notified_to_the_others_once)
{
    // Milan v1.3 Section 5.4.5.3: a notification after the response, only when the command changed something.
    McrEntity e;
    register_controller(e.handler, kEntity, kControllerA, kMacA);
    register_controller(e.handler, kEntity, kControllerB, kMacB);
    e.sends.clear();
    auto const body = system_unique_id_body(0x0102'0304'0506'0708, "Front of house");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, body);
    EXPECT_EQ(e.sends.size(), size_t{2});
    EXPECT_FALSE(mvu_header_of(e.sends.front()).is_unsolicited());  // the response first
    EXPECT_EQ(mvu_header_of(e.sends.front()).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(system_unique_id_of(e.sends.front()).system_unique_id.get(), uint64_t{0x0102'0304'0506'0708});
    EXPECT_EQ(system_unique_id_of(e.sends.back()).system_name.as_string_view(), std::string_view{"Front of house"});

    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, body);  // the same again
    EXPECT_TRUE(mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, kMacB) == std::vector<uint16_t>{0});
    EXPECT_TRUE(mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, kMacA).empty());
    auto const in_force = system_unique_id_of(get_system_unique_id(e));
    EXPECT_EQ(in_force.system_unique_id.get(), uint64_t{0x0102'0304'0506'0708});
    EXPECT_EQ(in_force.system_name.as_string_view(), std::string_view{"Front of house"});
    EXPECT_EQ(e.handler.system_unique_id().system_unique_id.get(), uint64_t{0x0102'0304'0506'0708});
}

TEST(nanoavb_entity_mvu_system_unique_id, a_name_alone_changed_is_notified_and_told_to_the_hook)
{
    McrEntity e;
    std::vector<MvuSystemUniqueIdPayload> told;
    e.handler.set_on_system_unique_id_changed([&told](MvuSystemUniqueIdPayload const& id) { told.push_back(id); });
    register_controller(e.handler, kEntity, kControllerB, kMacB);
    e.sends.clear();
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Stage"));
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Stage"));
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Studio"));
    EXPECT_TRUE((mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, kMacB) == std::vector<uint16_t>{0, 1}));
    EXPECT_EQ(told.size(), size_t{2});
    EXPECT_EQ(told.back().system_unique_id.get(), uint64_t{7});
    EXPECT_EQ(told.back().system_name.as_string_view(), std::string_view{"Studio"});
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Caf\xC3"));
    EXPECT_EQ(told.size(), size_t{2});  // refused: not told
}

TEST(nanoavb_entity_mvu_system_unique_id, the_reserved_octets_and_those_after_the_name_are_answered_zero)
{
    McrEntity e;
    auto body = system_unique_id_body(7, "Stage");
    body[0] = 0xAA;  // reserved
    body[1] = 0x55;
    body[MvuSystemUniqueIdPayload::LENGTH - 1] = 0xFF;  // after the name's NUL
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, body);
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_SUCCESS);
    auto const expected = system_unique_id_body(7, "Stage");
    for (auto const& reply : {e.sends.back(), get_system_unique_id(e)}) {
        EXPECT_TRUE(std::ranges::equal(mvu_body_of(reply), expected));
    }
}

TEST(nanoavb_entity_mvu_system_unique_id, a_restored_id_is_answered_and_nobody_is_told)
{
    // Milan v1.3 Section 5.4.4.2: the id outlasts a power cycle; the entity puts its saved one back.
    McrEntity e;
    int told = 0;
    e.handler.set_on_system_unique_id_changed([&told](MvuSystemUniqueIdPayload const&) { ++told; });
    register_controller(e.handler, kEntity, kControllerB, kMacB);
    e.sends.clear();
    EXPECT_FALSE(e.handler.restore_system_unique_id(9, AtdeccString{"Caf\xC3"}));
    EXPECT_EQ(e.handler.system_unique_id().system_unique_id.get(), uint64_t{0});
    EXPECT_TRUE(e.handler.restore_system_unique_id(9, AtdeccString{"Front of house"}));
    EXPECT_TRUE(e.sends.empty());
    EXPECT_EQ(told, 0);
    auto const in_force = system_unique_id_of(get_system_unique_id(e));
    EXPECT_EQ(in_force.system_unique_id.get(), uint64_t{9});
    EXPECT_EQ(in_force.system_name.as_string_view(), std::string_view{"Front of house"});
}

TEST(nanoavb_entity_mvu_system_unique_id, a_configuration_switch_keeps_it)
{
    // The id names the system the entity belongs to, not one of its configurations.
    DescriptorEntity entity{};
    entity.configurations_count = 2;
    DescriptorConfiguration const config{};
    DescriptorClockDomain const domain{};
    EntityModel model = EntityModelBuilder{}.entity(entity).configuration(config).clock_domain(domain).build();
    AemCommandHandler handler{model};
    std::vector<CapturedSend> sends;
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    capture_sends(handler, sends);
    handler.set_on_configuration_changed([](uint16_t) { return AEM_STATUS_SUCCESS; });
    (void)inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Stage"));

    std::array<uint8_t, AemSetConfigurationPayload::LENGTH> to_1{};
    span_store(to_1, AemSetConfigurationPayload{.reserved = uint16_t{0}, .configuration_index = uint16_t{1}});
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONFIGURATION, to_1);
    EXPECT_EQ(handler.current_configuration(), uint16_t{1});
    std::array<uint8_t, MVU_GET_SYSTEM_UNIQUE_ID_COMMAND_LENGTH> const reserved{};
    (void)inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, reserved);
    EXPECT_EQ(system_unique_id_of(sends.back()).system_unique_id.get(), uint64_t{7});
    EXPECT_EQ(system_unique_id_of(sends.back()).system_name.as_string_view(), std::string_view{"Stage"});
}

TEST(nanoavb_entity_mvu_system_unique_id, a_bad_name_or_another_controllers_lock_changes_nothing)
{
    McrEntity e;
    register_controller(e.handler, kEntity, kControllerB, kMacB);
    e.sends.clear();
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Caf\xC3"));
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(system_unique_id_of(e.sends.back()).system_unique_id.get(), uint64_t{0});  // the id in force

    auto lock = create_aem_header(AEM_COMMAND_LOCK_ENTITY);
    lock.controller_entity_id = kControllerB;
    std::array<uint8_t, 16> const lock_body{};
    EXPECT_EQ(make_test_result(e.handler, lock, lock_body).status, AEM_STATUS_SUCCESS);
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Stage"));
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_ENTITY_LOCKED);
    // The lock is asked first: a locked-out controller learns nothing of its arguments.
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, system_unique_id_body(7, "Caf\xC3"));
    EXPECT_EQ(mvu_header_of(e.sends.back()).common.status(), MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(system_unique_id_of(e.sends.back()).system_unique_id.get(), uint64_t{0});
    EXPECT_TRUE(mvu_unsolicited_sequence_ids(e.sends, MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, kMacB).empty());
    EXPECT_TRUE(system_unique_id_of(get_system_unique_id(e)).system_name.as_string_view().empty());
}

// Milan v1.3 Section 7.6: the settings belong to a configuration's CLOCK_DOMAINs; a switch starts
// the new configuration's from their defaults.
TEST(nanoavb_entity_mvu_mcr, a_configuration_switch_resets_the_settings)
{
    DescriptorEntity entity{};
    entity.configurations_count = 2;
    DescriptorConfiguration config{};
    DescriptorClockDomain domain{};
    EntityModel model = EntityModelBuilder{}.entity(entity).configuration(config).clock_domain(domain).build();
    AemCommandHandler handler{model};
    std::vector<CapturedSend> sends;
    handler.set_entity_id(kEntity);
    handler.set_milan_info(kMilanInfo);
    capture_sends(handler, sends);
    handler.set_on_configuration_changed([](uint16_t) { return AEM_STATUS_SUCCESS; });
    auto const named = mcr_set_body(0, MVU_MCR_FLAGS_SUPPORTED, 200, "Front of house");
    (void)inject_mvu(handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, named);
    EXPECT_EQ(mcr_of(get_mcr(handler, sends, 0)).user_mcr_prio.get(), 200);

    std::array<uint8_t, AemSetConfigurationPayload::LENGTH> to_1{};
    span_store(to_1, AemSetConfigurationPayload{.reserved = uint16_t{0}, .configuration_index = uint16_t{1}});
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_CONFIGURATION, to_1);
    EXPECT_EQ(handler.current_configuration(), uint16_t{1});
    auto const info = mcr_of(get_mcr(handler, sends, 0));
    EXPECT_EQ(info.user_mcr_prio.get(), MVU_MCR_PRIO_DEFAULT);
    EXPECT_EQ(info.media_clock_domain_name.as_string_view(), MVU_DEFAULT_MEDIA_CLOCK_DOMAIN_NAME);
}

TEST(nanoavb_entity_mvu_mcr, the_user_priority_follows_the_default_until_a_controller_sets_one)
{
    McrEntity e;
    EXPECT_TRUE(e.handler.set_default_mcr_priority(0, MVU_MCR_PRIO_AUDIO_INTERFACE));
    EXPECT_FALSE(e.handler.set_default_mcr_priority(AemCommandHandler::MAX_CLOCK_DOMAINS, MVU_MCR_PRIO_ROUTER));
    auto info = mcr_of(get_mcr(e.handler, e.sends, 0));
    EXPECT_EQ(info.default_mcr_prio.get(), MVU_MCR_PRIO_AUDIO_INTERFACE);
    EXPECT_EQ(info.user_mcr_prio.get(), MVU_MCR_PRIO_AUDIO_INTERFACE);

    auto const set = mcr_set_body(0, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 100, "");
    (void)inject_mvu(e.handler, kControllerA, kMacA, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, set);
    EXPECT_TRUE(e.handler.set_default_mcr_priority(0, MVU_MCR_PRIO_ROUTER));
    info = mcr_of(get_mcr(e.handler, e.sends, 0));
    EXPECT_EQ(info.default_mcr_prio.get(), MVU_MCR_PRIO_ROUTER);
    EXPECT_EQ(info.user_mcr_prio.get(), 100);
}

namespace {

/// A two-output presentation offset store for SET_STREAM_INFO, as the kit keeps it.
struct StreamInfoOffsetStore
{
    std::array<uint32_t, 2> offsets{2'000'000, 2'000'000};
    uint8_t set_status{AEM_STATUS_SUCCESS};  ///< what the next set answers
    int sets{0};

    void install(AemCommandHandler& handler)
    {
        handler.set_presentation_offset_hooks(
            [this](uint16_t const index) -> std::optional<uint32_t> {
                return index < offsets.size() ? std::optional<uint32_t>{offsets.at(index)} : std::nullopt;
            },
            [this](uint16_t const index, uint32_t const ns) -> uint8_t {
                if (set_status == AEM_STATUS_SUCCESS) {
                    offsets.at(index) = ns;
                    ++sets;
                }
                return set_status;
            });
    }
};

auto set_stream_info_body(uint16_t const type, uint16_t const index, uint32_t const flags, uint32_t const latency)
    -> std::array<uint8_t, AemStreamInfoPayload::LENGTH>
{
    AemStreamInfoPayload p{};
    p.descriptor_type = doublet_t{type};
    p.descriptor_index = doublet_t{index};
    p.flags = quadlet_t{flags};
    p.msrp_accumulated_latency = quadlet_t{latency};
    std::array<uint8_t, AemStreamInfoPayload::LENGTH> bytes{};
    span_store(bytes, p);
    return bytes;
}

auto solicited_status(std::vector<CapturedSend> const& sends) -> int
{
    auto const* reply = solicited(sends);
    return reply != nullptr ? header_of(*reply).status() : -1;
}

void serve_offsets_as_stream_info(AemCommandHandler& handler, StreamInfoOffsetStore& store)
{
    handler.set_get_stream_info(
        [&store](uint16_t const type, uint16_t const index, AemStreamInfoPayload& out, AemStreamInfoMilanExtension&) -> bool {
            if (type != DESCRIPTOR_STREAM_OUTPUT || index >= store.offsets.size()) {
                return false;
            }
            out.flags = quadlet_t{stream_info_flags::MSRP_ACC_LAT_VALID};
            out.msrp_accumulated_latency = quadlet_t{store.offsets.at(index)};
            return true;
        });
}

}  // namespace

// A non-AEM AECP command (MVU, ADDRESS_ACCESS) is answered NOT_IMPLEMENTED (IEEE 1722.1-2021
// Clause 9.2.1.1.6, Milan v1.2 Section 5.4.3.3); responses and other targets get nothing.
TEST(nanoavb_entity_aecp, non_aem_commands_get_not_implemented)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    std::vector<std::vector<uint8_t>> sent;
    handler.set_callbacks(AemCommandHandlerCallbacks{.send_response = [&](Eui48 const&, std::span<uint8_t const> resp) {
        sent.emplace_back(resp.begin(), resp.end());
        return true;
    }});
    auto const us = model.get_entity().entity_id;
    // VENDOR_UNIQUE_COMMAND: 22-byte common header, protocol_id 00-1B-C5-0A-C1-00,
    // command_type GET_MILAN_INFO (0), 2 reserved bytes.
    std::array<uint8_t, 32> mvu{};
    AecpDuCommon common{};
    common.subtype = AvtpSubtype::aecp;
    common.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    common.set_control_data_length(10 + 6 + 2 + 2);
    common.target_entity_id = us;
    common.controller_entity_id = Eui64{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    common.sequence_id = 0x1234;
    span_store(std::span<uint8_t>{mvu}.first(AecpDuCommon::LENGTH), common);
    std::array<uint8_t, 6> const protocol_id{0x00, 0x1B, 0xC5, 0x0A, 0xC1, 0x00};
    std::copy(protocol_id.begin(), protocol_id.end(), mvu.begin() + 22);
    Eui48 const src{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    // A VENDOR_UNIQUE command is Milan's, and this entity has no MilanInfo: not answered (Milan v1.3 5.4.4.1).
    EXPECT_FALSE(handler.process_packet(src, mvu, us));
    EXPECT_TRUE(sent.empty());

    common.set_message_type(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND);
    span_store(std::span<uint8_t>{mvu}.first(AecpDuCommon::LENGTH), common);
    EXPECT_TRUE(handler.process_packet(src, mvu, us));
    EXPECT_EQ(sent.size(), size_t{1});
    AecpDuCommon reply{};
    span_load(reply, std::span<uint8_t const>{sent[0]}.first(AecpDuCommon::LENGTH));
    EXPECT_EQ(reply.message_type(), AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE);
    EXPECT_EQ(reply.status(), AECP_STATUS_NOT_IMPLEMENTED);

    // Malformed: control_data_length short of the AECP common fields
    // (controller_entity_id + sequence_id): dropped, nothing read past it.
    common.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    common.set_control_data_length(4);
    span_store(std::span<uint8_t>{mvu}.first(AecpDuCommon::LENGTH), common);
    EXPECT_FALSE(handler.process_packet(src, mvu, us));
    EXPECT_EQ(sent.size(), size_t{1});
    common.set_control_data_length(10 + 6 + 2 + 2);
    // A reserved command type (10): not answered.
    common.set_message_type(10);
    span_store(std::span<uint8_t>{mvu}.first(AecpDuCommon::LENGTH), common);
    EXPECT_FALSE(handler.process_packet(src, mvu, us));
    EXPECT_EQ(sent.size(), size_t{1});

    // A response, or a command for someone else: no reply.
    common.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    span_store(std::span<uint8_t>{mvu}.first(AecpDuCommon::LENGTH), common);
    EXPECT_FALSE(handler.process_packet(src, mvu, us));
    common.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    common.target_entity_id = Eui64{0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09};
    span_store(std::span<uint8_t>{mvu}.first(AecpDuCommon::LENGTH), common);
    EXPECT_FALSE(handler.process_packet(src, mvu, us));
    EXPECT_EQ(sent.size(), size_t{1});
}

TEST(nanoavb_entity_get_stream_info, the_layout_sets_the_response_length)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_get_stream_info([](uint16_t, uint16_t, AemStreamInfoPayload& out, AemStreamInfoMilanExtension& ext) -> bool {
        ext.flags_ex = quadlet_t{stream_info_flags_ex::REGISTERING};
        out.flags = quadlet_t{stream_info_flags::MSRP_ACC_LAT_VALID | stream_info_flags::NOT_REGISTERING_SRP};
        out.msrp_accumulated_latency = quadlet_t{1234};
        return true;
    });
    std::array<uint8_t, AemGetStreamInfoCommandPayload::LENGTH> cmd{0x00, 0x06, 0x00, 0x00};
    auto header = create_aem_header(AEM_COMMAND_GET_STREAM_INFO);

    // The profile's form until a layout is set (Milan v1.3 Section 5.4.2.10.2): Milan's is
    // IEEE 1722.1-2021's, classic's IEEE 1722.1-2013's.
    handler.set_profile(AemCommandHandler::Profile::milan13);
    EXPECT_EQ(handler.stream_info_layout(), StreamInfoLayout::ieee_2021);
    handler.set_profile(AemCommandHandler::Profile::classic);
    EXPECT_EQ(handler.stream_info_layout(), StreamInfoLayout::ieee_2013);
    handler.set_profile(AemCommandHandler::Profile::milan13);
    handler.set_stream_info_layout(StreamInfoLayout::ieee_2021);
    auto const v2021 = make_test_result(handler, header, cmd);
    EXPECT_EQ(v2021.status, AEM_STATUS_SUCCESS);
    EXPECT_EQ(v2021.response_data().size(), size_t{84});
    AemStreamInfoPayload resp{};
    span_load(resp, v2021.response_data().first(AemStreamInfoPayload::LENGTH));
    EXPECT_EQ(resp.flags.get(), stream_info_flags::MSRP_ACC_LAT_VALID | stream_info_flags::NOT_REGISTERING_SRP);
    EXPECT_EQ(resp.msrp_accumulated_latency.get(), 1234U);
    // No flags_ex in the IP tail: no IP transport, all zero.
    auto const tail = v2021.response_data().subspan(AemStreamInfoPayload::LENGTH);
    EXPECT_TRUE(std::ranges::all_of(tail, [](uint8_t const b) { return b == 0; }));

    // Unset again: Milan's IEEE 1722.1-2021 84 octets, the classic profile's 48.
    handler.set_stream_info_layout(std::nullopt);
    EXPECT_EQ(make_test_result(handler, header, cmd).response_data().size(), AEM_STREAM_INFO_2021_LENGTH);
    handler.set_profile(AemCommandHandler::Profile::classic);
    EXPECT_EQ(make_test_result(handler, header, cmd).response_data().size(), AemStreamInfoPayload::LENGTH);
}

TEST(nanoavb_entity_milan_notify, get_counters_notifications_at_most_once_a_second_per_descriptor)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};
    handler.set_entity_id(kEntity);
    uint32_t value = 0;
    handler.set_get_counters([&value](uint16_t type, uint16_t, uint32_t& valid, std::array<uint32_t, 32>& counters) -> bool {
        if (type != DESCRIPTOR_STREAM_INPUT && type != DESCRIPTOR_AVB_INTERFACE) {
            return false;
        }
        valid = 1U;
        counters[0] = value;
        return true;
    });
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    auto const t0 = sm::TimePoint{} + std::chrono::seconds(100);
    handler.set_event_time(t0);  // B's monitor timer runs from here
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    handler.tick(t0);
    value = 1;
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, 0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 0);  // sent by tick()
    handler.tick(t0 + std::chrono::milliseconds(1));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 1);  // the first at the next tick
    // A descriptor without counters is refused as GET_COUNTERS would refuse it, and nothing is held.
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_OUTPUT, 0), AEM_STATUS_NO_SUCH_DESCRIPTOR);

    // Changes within the second are held back; another descriptor is not.
    value = 2;
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, 0), AEM_STATUS_SUCCESS);
    value = 3;
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, 0), AEM_STATUS_SUCCESS);
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_AVB_INTERFACE, 0), AEM_STATUS_SUCCESS);
    handler.tick(t0 + std::chrono::milliseconds(300));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 2);
    handler.tick(t0 + std::chrono::milliseconds(1000));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 2);

    // A second after the first: one notification, with the counters as they are then.
    handler.tick(t0 + std::chrono::milliseconds(1001));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 3);
    AemCountersPayload payload{};
    span_load(payload, body_of(sends.back()).first(sizeof(AemCountersPayload)));
    EXPECT_EQ(payload.descriptor_type.get(), DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(payload.counters[0].get(), 3U);
    handler.tick(t0 + std::chrono::seconds(3));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 3);  // nothing pending

    // Quiet for over a second: the next change goes at the next tick.
    EXPECT_EQ(handler.notify_counters_changed(DESCRIPTOR_STREAM_INPUT, 0), AEM_STATUS_SUCCESS);
    handler.tick(t0 + std::chrono::seconds(3) + std::chrono::milliseconds(1));
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_COUNTERS, kMacB), 4);
}

// IEEE 1722.1-2021 Clause 7.4.15.1: a refused SET_STREAM_INFO answers the stream's values in force.
TEST(nanoavb_entity_presentation_offset, a_refused_set_stream_info_answers_the_values_in_force)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    StreamInfoOffsetStore store;
    store.install(handler);
    serve_offsets_as_stream_info(handler, store);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    handler.set_stream_info_layout(StreamInfoLayout::ieee_2021);

    using namespace stream_info_flags;
    auto const body = set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID, 3'000'000);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_INFO, body);
    EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_BAD_ARGUMENTS});
    EXPECT_EQ(body_of(*solicited(sends)).size(), AEM_STREAM_INFO_2021_LENGTH);
    AemStreamInfoPayload answered{};
    span_load(answered, body_of(*solicited(sends)).first(AemStreamInfoPayload::LENGTH));
    EXPECT_EQ(answered.msrp_accumulated_latency.get(), 2'000'000U);
}

TEST(nanoavb_entity_presentation_offset, set_stream_info_in_the_2021_layout)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    handler.set_stream_info_layout(StreamInfoLayout::ieee_2021);
    auto const run = [&](std::span<uint8_t const> const body) {
        sends.clear();
        inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_INFO, body);
        return solicited_status(sends);
    };
    using namespace stream_info_flags;

    // MSRP_ACC_LAT_VALID is BAD_ARGUMENTS before anything else (Milan v1.3 Section 5.4.2.9):
    // without the offset hooks, on an input, with another sub-command.
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_BAD_ARGUMENTS});
    StreamInfoOffsetStore store;
    store.install(handler);
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_INPUT, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_BAD_ARGUMENTS});
    EXPECT_EQ(
        run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID | STREAM_ID_VALID, 1)),
        int{AEM_STATUS_BAD_ARGUMENTS});
    EXPECT_EQ(store.sets, 0);

    // The echo is the 84-octet form: an 84-octet command's IP fields kept, a 48-octet one padded.
    std::array<uint8_t, AEM_STREAM_INFO_2021_LENGTH> long_form{};
    span_copy(
        std::span<uint8_t>{long_form}.first(AemStreamInfoPayload::LENGTH), set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, 0, 0));
    long_form[AemStreamInfoPayload::LENGTH + 1] = 0x42;  // source_port
    EXPECT_EQ(run(long_form), int{AEM_STATUS_SUCCESS});
    EXPECT_EQ(body_of(*solicited(sends)).size(), AEM_STREAM_INFO_2021_LENGTH);
    EXPECT_EQ(body_of(*solicited(sends))[AemStreamInfoPayload::LENGTH + 1], uint8_t{0x42});
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_BAD_ARGUMENTS});
    EXPECT_EQ(body_of(*solicited(sends)).size(), AEM_STREAM_INFO_2021_LENGTH);
    EXPECT_EQ(body_of(*solicited(sends))[AemStreamInfoPayload::LENGTH + 1], uint8_t{0});

    // The IP sub-commands (IEEE 1722.1-2021 Table 7-145) are refused as unsupported, in either layout.
    for (uint32_t const bit : {IP_FLAGS_VALID, IP_SRC_PORT_VALID, IP_DST_PORT_VALID, IP_SRC_ADDR_VALID, IP_DST_ADDR_VALID}) {
        EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, bit, 0)), int{AEM_STATUS_NOT_SUPPORTED});
    }
    handler.set_stream_info_layout(StreamInfoLayout::milan_v1_2);
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, IP_DST_ADDR_VALID, 0)), int{AEM_STATUS_NOT_SUPPORTED});
    EXPECT_EQ(body_of(*solicited(sends)).size(), AemStreamInfoPayload::LENGTH);
}

TEST(nanoavb_entity_presentation_offset, set_stream_info_refusals)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_stream_info_layout(StreamInfoLayout::milan_v1_2);  // Milan v1.2's GET_STREAM_INFO
    handler.set_entity_id(kEntity);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    auto const run = [&](std::array<uint8_t, AemStreamInfoPayload::LENGTH> const& body) {
        sends.clear();
        inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_INFO, body);
        return solicited_status(sends);
    };
    using namespace stream_info_flags;

    // No store: not implemented.
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_NOT_IMPLEMENTED});

    StreamInfoOffsetStore store;
    store.install(handler);
    // Stream inputs never (Milan v1.2 Section 5.4.2.9).
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_INPUT, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_NOT_SUPPORTED});
    // Any other sub-command refuses the whole command.
    EXPECT_EQ(
        run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID | STREAM_ID_VALID, 1)),
        int{AEM_STATUS_NOT_SUPPORTED});
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, STREAM_FORMAT_VALID, 1)), int{AEM_STATUS_NOT_SUPPORTED});
    // Past 0x7FFFFFFF ns.
    EXPECT_EQ(
        run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID, 0x80000000U)), int{AEM_STATUS_BAD_ARGUMENTS});
    // No such stream output, or not a stream at all.
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 5, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_NO_SUCH_DESCRIPTOR});
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_ENTITY, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_NO_SUCH_DESCRIPTOR});
    // While streaming.
    store.set_status = AEM_STATUS_STREAM_IS_RUNNING;
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, MSRP_ACC_LAT_VALID, 1)), int{AEM_STATUS_STREAM_IS_RUNNING});
    EXPECT_EQ(store.sets, 0);
    EXPECT_EQ(store.offsets[0], 2'000'000U);
    // No sub-command at all: nothing to do.
    store.set_status = AEM_STATUS_SUCCESS;
    EXPECT_EQ(run(set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, CLASS_B, 1)), int{AEM_STATUS_SUCCESS});
    EXPECT_EQ(store.sets, 0);
}

TEST(nanoavb_entity_presentation_offset, set_stream_info_sets_it_and_notifies_a_change)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_stream_info_layout(StreamInfoLayout::milan_v1_2);  // Milan v1.2's GET_STREAM_INFO
    handler.set_entity_id(kEntity);
    handler.set_profile(AemCommandHandler::Profile::milan13);  // the requester is left out
    StreamInfoOffsetStore store;
    store.install(handler);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    register_controller(handler, kEntity, kControllerA, kMacA);
    register_controller(handler, kEntity, kControllerB, kMacB);
    sends.clear();

    auto const body = set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 1, stream_info_flags::MSRP_ACC_LAT_VALID, 3'000'000);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_INFO, body);
    EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_SUCCESS});
    EXPECT_EQ(store.offsets[1], 3'000'000U);
    EXPECT_EQ(store.offsets[0], 2'000'000U);
    // The response echoes MSRP_ACC_LAT_VALID and the offset.
    AemStreamInfoPayload echoed{};
    span_load(echoed, body_of(*solicited(sends)).first(AemStreamInfoPayload::LENGTH));
    EXPECT_EQ(echoed.flags.get(), stream_info_flags::MSRP_ACC_LAT_VALID);
    EXPECT_EQ(echoed.msrp_accumulated_latency.get(), 3'000'000U);
    EXPECT_EQ(body_of(*solicited(sends)).size(), AemStreamInfoPayload::LENGTH);
    // Milan v1.2 Section 5.4.5.2: the response alone, to the other registered controller.
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_INFO, kMacB), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_STREAM_INFO, kMacB), 0);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_GET_MAX_TRANSIT_TIME, kMacB), 0);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_INFO, kMacA), 0);

    // The same offset again changes nothing: no notification.
    sends.clear();
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_INFO, body);
    EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_SUCCESS});
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_SET_STREAM_INFO, kMacB), 0);

    // Both ends of the range are accepted.
    for (uint32_t const ns : {0U, MILAN_PRESENTATION_OFFSET_MAX_NS}) {
        sends.clear();
        inject_command(
            handler,
            kEntity,
            kControllerA,
            kMacA,
            AEM_COMMAND_SET_STREAM_INFO,
            set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, stream_info_flags::MSRP_ACC_LAT_VALID, ns));
        EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_SUCCESS});
        EXPECT_EQ(store.offsets[0], ns);
    }
}

TEST(nanoavb_entity_presentation_offset, the_2021_layout_sets_it_by_max_transit_time_only)
{
    SetAcceptingHandler app;
    AemCommandHandler handler{app};
    handler.set_entity_id(kEntity);
    StreamInfoOffsetStore store;
    store.install(handler);
    std::vector<CapturedSend> sends;
    capture_sends(handler, sends);
    handler.set_stream_info_layout(StreamInfoLayout::ieee_2021);

    // Milan v1.3 Section 5.4.2.9: MSRP_ACC_LAT_VALID is BAD_ARGUMENTS, the offset unchanged.
    auto const body = set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, stream_info_flags::MSRP_ACC_LAT_VALID, 3'000'000);
    inject_command(handler, kEntity, kControllerA, kMacA, AEM_COMMAND_SET_STREAM_INFO, body);
    EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_BAD_ARGUMENTS});
    EXPECT_EQ(store.sets, 0);
    EXPECT_EQ(store.offsets[0], 2'000'000U);

    // SET_MAX_TRANSIT_TIME sets it (Milan v1.3 Section 5.4.2.30).
    sends.clear();
    inject_command(
        handler,
        kEntity,
        kControllerA,
        kMacA,
        AEM_COMMAND_SET_MAX_TRANSIT_TIME,
        max_transit_time_body(DESCRIPTOR_STREAM_OUTPUT, 0, 3'000'000));
    EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_SUCCESS});
    EXPECT_EQ(store.offsets[0], 3'000'000U);

    // Milan v1.2's layout keeps SET_STREAM_INFO's offset (v1.2 Section 5.4.2.9).
    handler.set_stream_info_layout(StreamInfoLayout::milan_v1_2);
    sends.clear();
    inject_command(
        handler,
        kEntity,
        kControllerA,
        kMacA,
        AEM_COMMAND_SET_STREAM_INFO,
        set_stream_info_body(DESCRIPTOR_STREAM_OUTPUT, 0, stream_info_flags::MSRP_ACC_LAT_VALID, 2'500'000));
    EXPECT_EQ(solicited_status(sends), int{AEM_STATUS_SUCCESS});
    EXPECT_EQ(store.offsets[0], 2'500'000U);
}
int statusbar_nanoavb_nanoavb_entity_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_entity_test");
    return result;
}