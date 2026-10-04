// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_aem_entity_handler.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
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

TEST(nanoavb_entity_stubs, get_control_not_implemented)
{
    auto model = create_test_model();
    AemCommandHandler handler{model};

    auto header = create_aem_header(AEM_COMMAND_GET_CONTROL);
    std::array<uint8_t, 4> const cmd{0, 0, 0, 0};
    auto result = make_test_result(handler, header, cmd);

    EXPECT_EQ(result.status, AEM_STATUS_NOT_IMPLEMENTED);
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
    handler.set_get_stream_info([](uint16_t descriptor_type, uint16_t descriptor_index, AemStreamInfoPayload& out) -> bool {
        if (descriptor_type != DESCRIPTOR_STREAM_OUTPUT || descriptor_index != 1) {
            return false;
        }
        out.stream_id = Eui64{0x02, 0x00, 0x00, 0xe0, 0x13, 0x60, 0x00, 0x01};
        out.stream_dest_mac = {0x91, 0xe0, 0xf0, 0x00, 0xfe, 0x01};
        out.stream_format = {0x02, 0x70, 0x08, 0x20, 0x00, 0x00, 0x00, 0x00};
        out.stream_vlan_id = doublet_t{2};
        out.msrp_accumulated_latency = quadlet_t{2'000'000};
        out.flags = quadlet_t{
            stream_info_flags::STREAM_ID_VALID | stream_info_flags::STREAM_FORMAT_VALID | stream_info_flags::STREAM_DEST_MAC_VALID |
            stream_info_flags::STREAM_VLAN_ID_VALID | stream_info_flags::MSRP_ACC_LAT_VALID | stream_info_flags::CONNECTED};
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
    handler.set_get_stream_info([](uint16_t, uint16_t, AemStreamInfoPayload&) -> bool { return false; });

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

}  // namespace

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

TEST(nanoavb_entity_acquire, identify_control_exempt_from_acquire)
{
    // Milan: SET_CONTROL of the IDENTIFY control works even while another
    // controller holds the entity; every other control honors the claim.
    SetAcceptingHandler app_handler;
    AemCommandHandler handler{app_handler};
    handler.set_identify_control_index(3);

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

    // Solicited SUCCESS to A, unsolicited ACQUIRE_ENTITY to registered B, and to
    // A as well: the acquiring controller is implicitly registered (7.5.2).
    auto const* reply = solicited(sends);
    EXPECT_TRUE(reply != nullptr);
    EXPECT_EQ(header_of(*reply).status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_ACQUIRE_ENTITY, kMacB), 1);
    EXPECT_EQ(count_unsolicited(sends, AEM_COMMAND_ACQUIRE_ENTITY, kMacA), 1);
    EXPECT_EQ(sends.size(), size_t{3});
    for (auto const& s : sends) {
        AemAcquireEntityPayload payload{};
        span_load(payload, body_of(s).first(AemAcquireEntityPayload::LENGTH));
        EXPECT_EQ(payload.owner_entity_id, kControllerA);
    }
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
        return command_type == AEM_COMMAND_SET_STREAM_FORMAT ? AEM_STATUS_IN_PROGRESS : AEM_STATUS_SUCCESS;
    }
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
    }
    EXPECT_EQ(handler.pending_command_count(), AemCommandHandler::MAX_PENDING_COMMANDS);
}

//
// Test Runner
//
int statusbar_nanoavb_nanoavb_entity_test(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    int result = 0;
    result |= ::statusbar::test::TestRegister::run_section("nanoavb_entity_test");
    return result;
}