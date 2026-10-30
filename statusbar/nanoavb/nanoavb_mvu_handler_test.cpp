// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for the AECP Milan Vendor Unique handler (Milan v1.2 Section
// 5.4.3-5.4.4)

#include "statusbar/nanoavb/nanoavb_mvu_handler.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_entity_profile.hpp"
#include "statusbar/test/test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace statusbar;
using namespace statusbar::nanoavb;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::mvu;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

Eui64 const US{0x70, 0xB3, 0xD5, 0xED, 0xC0, 0x00, 0x00, 0x03};
Eui64 const CONTROLLER{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
Eui48 const SRC{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

/// An MVU command AVTPDU: the 30-byte MVU header with @p command_type and
/// @p data as command_specific_data, then @p padding zero octets that
/// control_data_length does not count (an Ethernet-padded frame).
auto mvu_command(uint16_t const command_type, std::span<uint8_t const> const data, size_t const padding = 0) -> std::vector<uint8_t>
{
    MvuDu du{};
    du.init_command(command_type, static_cast<uint16_t>(data.size()));
    du.common.target_entity_id = US;
    du.common.controller_entity_id = CONTROLLER;
    du.common.sequence_id = 0x1234;
    std::vector<uint8_t> out(MvuDu::LENGTH + data.size() + padding, 0);
    span_store(std::span<uint8_t>{out}.first(MvuDu::LENGTH), du);
    std::ranges::copy(data, out.begin() + MvuDu::LENGTH);
    return out;
}

auto get_milan_info_command(size_t const padding = 0) -> std::vector<uint8_t>
{
    std::array<uint8_t, MvuGetMilanInfoCommand::LENGTH> const reserved{};
    return mvu_command(MVU_COMMAND_GET_MILAN_INFO, reserved, padding);
}

/// A handler whose responses land in @p sent
auto make_handler(std::vector<std::vector<uint8_t>>& sent, bool const milan = true) -> MvuCommandHandler
{
    MvuCommandHandler handler{};
    handler.set_profile(EntityProfile{.milan = milan});
    handler.set_send_response([&sent](Eui48 const&, std::span<uint8_t const> const resp) {
        sent.emplace_back(resp.begin(), resp.end());
        return true;
    });
    return handler;
}

auto reply_header(std::vector<uint8_t> const& reply) -> MvuDu
{
    MvuDu du{};
    span_load(du, std::span<uint8_t const>{reply}.first(MvuDu::LENGTH));
    return du;
}

/// The response is a VENDOR_UNIQUE_RESPONSE with the MVU protocol_id, r = 0,
/// @p status, the command's identity, and @p command_type
void expect_mvu_response(std::vector<uint8_t> const& reply, uint16_t const command_type, uint8_t const status)
{
    EXPECT_TRUE(reply.size() >= MvuDu::LENGTH);
    auto const du = reply_header(reply);
    EXPECT_EQ(du.common.subtype.get(), AvtpSubtype::aecp);
    EXPECT_EQ(du.common.message_type(), AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_EQ(du.common.status(), status);
    EXPECT_TRUE(du.common.target_entity_id == US);
    EXPECT_TRUE(du.common.controller_entity_id == CONTROLLER);
    EXPECT_EQ(du.common.sequence_id.get(), uint16_t{0x1234});
    EXPECT_TRUE(du.is_mvu_protocol());
    EXPECT_EQ(du.r_command_type.get(), command_type);
}

}  // namespace

// GET_MILAN_INFO (Milan v1.2 Section 5.4.4.1, Figure 5.4), padded to the Ethernet minimum;
// control_data_length counts no padding.
TEST(nanoavb_mvu, get_milan_info_reply_bytes)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    // A command as received: Ethernet-padded beyond its control_data_length
    EXPECT_TRUE(handler.process_packet(SRC, get_milan_info_command(14), US));
    EXPECT_EQ(sent.size(), size_t{1});
    auto const& reply = sent[0];
    expect_mvu_response(reply, MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(reply).common.control_data_length(), uint16_t{32});
    EXPECT_EQ(reply.size(), MVU_MIN_PDU_LENGTH);
    EXPECT_EQ(MVU_MIN_PDU_LENGTH, size_t{46});  // 64-octet frame less header and FCS
    std::array<uint8_t, 14> const expected{
        0x00,
        0x00,  // reserved
        0x00,
        0x00,
        0x00,
        0x01,  // protocol_version
        0x00,
        0x00,
        0x00,
        0x00,  // features_flags
        0x00,
        0x00,
        0x00,
        0x00,  // certification_version
    };
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), reply.begin() + MvuDu::LENGTH));
    // The padding is zero
    EXPECT_TRUE(std::all_of(reply.begin() + MvuDu::LENGTH + 14, reply.end(), [](uint8_t const b) { return b == 0; }));

    MvuGetMilanInfoResponse info{};
    span_load(info, std::span<uint8_t const>{reply}.subspan(MvuDu::LENGTH, MvuGetMilanInfoResponse::LENGTH));
    EXPECT_EQ(info.protocol_version.get(), MVU_PROTOCOL_VERSION);
    EXPECT_EQ(info.features_flags.get() & MVU_FEATURES_REDUNDANCY, uint32_t{0});
    EXPECT_EQ(info.features_flags.get() & MVU_FEATURES_TALKER_DYNAMIC_MAPPINGS_WHILE_RUNNING, uint32_t{0});
}

TEST(nanoavb_mvu, get_milan_info_certification_version)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    handler.set_certification_version(milan_certification_version(1, 2, 0, 0));
    EXPECT_TRUE(handler.process_packet(SRC, get_milan_info_command(), US));
    EXPECT_EQ(sent.size(), size_t{1});
    std::array<uint8_t, 4> const expected{0x01, 0x02, 0x00, 0x00};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), sent[0].begin() + 40));
}

// A command whose r bit is set is still answered, with r = 0
TEST(nanoavb_mvu, response_clears_r)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    auto cmd = get_milan_info_command();
    cmd[28] |= 0x80;
    EXPECT_TRUE(handler.process_packet(SRC, cmd, US));
    auto cmd_unknown = mvu_command(0x0100, {});
    cmd_unknown[28] |= 0x80;
    EXPECT_TRUE(handler.process_packet(SRC, cmd_unknown, US));
    EXPECT_EQ(sent.size(), size_t{2});
    expect_mvu_response(sent[0], MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);
    expect_mvu_response(sent[1], 0x0100, MVU_STATUS_NOT_IMPLEMENTED);
}

// The classic profile must not claim Milan: GET_MILAN_INFO gets no reply at
// all (Section 5.4.4.1 NOTE), the other MVU commands NOT_IMPLEMENTED.
TEST(nanoavb_mvu, classic_profile)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent, false);
    EXPECT_FALSE(handler.process_packet(SRC, get_milan_info_command(14), US));
    EXPECT_TRUE(sent.empty());

    std::array<uint8_t, MvuGetMilanInfoCommand::LENGTH> const reserved{};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, reserved, 14), US));
    EXPECT_EQ(sent.size(), size_t{1});
    expect_mvu_response(sent[0], MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, MVU_STATUS_NOT_IMPLEMENTED);
    // The command echoed (cdl 20: common 10 + protocol_id 6 + command_type 2 +
    // reserved 2), padded; the received padding is not echoed as data
    EXPECT_EQ(reply_header(sent[0]).common.control_data_length(), uint16_t{20});
    EXPECT_EQ(sent[0].size(), MVU_MIN_PDU_LENGTH);
}

namespace {

Eui64 const OTHER{0x00, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
Eui48 const OTHER_MAC{0x22, 0x33, 0x44, 0x55, 0x66, 0x77};

auto get_system_unique_id_command() -> std::vector<uint8_t>
{
    std::array<uint8_t, MvuGetMilanInfoCommand::LENGTH> const reserved{};
    return mvu_command(MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, reserved);
}

auto set_system_unique_id_command(uint32_t const id) -> std::vector<uint8_t>
{
    MvuSystemUniqueIdPayload payload{};
    payload.system_unique_id = id;
    return mvu_command(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, make_const_span(payload));
}

auto get_mcr_command(uint16_t const domain) -> std::vector<uint8_t>
{
    MvuGetMediaClockReferenceInfoCommand payload{};
    payload.clock_domain_index = domain;
    return mvu_command(MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, make_const_span(payload));
}

auto set_mcr_command(uint16_t const domain, uint8_t const flags, uint8_t const prio, std::string_view const name)
    -> std::vector<uint8_t>
{
    MvuMediaClockReferenceInfoPayload payload{};
    payload.clock_domain_index = domain;
    payload.flags = flags;
    payload.default_mcr_prio = 7;  // ignored in a command
    payload.user_mcr_prio = prio;
    payload.media_clock_domain_name = mcr_domain_name(name);
    return mvu_command(MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, make_const_span(payload));
}

template <typename T>
auto reply_payload(std::vector<uint8_t> const& reply) -> T
{
    T payload{};
    span_load(payload, std::span<uint8_t const>{reply}.subspan(MvuDu::LENGTH, T::LENGTH));
    return payload;
}

/// An AEM command from @p controller with @p payload
auto aem_command(uint16_t const code, Eui64 const& controller, std::span<uint8_t const> const payload = {}) -> std::vector<uint8_t>
{
    AemDu du{};
    du.init_command(code, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + payload.size()));
    du.target_entity_id = US;
    du.controller_entity_id = controller;
    std::vector<uint8_t> out(AemDu::LENGTH + payload.size(), 0);
    span_store(std::span<uint8_t>{out}.first(AemDu::LENGTH), du);
    std::ranges::copy(payload, out.begin() + AemDu::LENGTH);
    return out;
}

/// An AEM handler beside the MVU handler (the lock and the unsolicited
/// registrations), its responses dropped
struct AemSide
{
    EntityModel model{[] {
        DescriptorEntity entity{};
        entity.entity_id = US;
        entity.configurations_count = 1;
        return EntityModelBuilder{}.entity(entity).configuration(DescriptorConfiguration{}).build();
    }()};
    AemCommandHandler aem{model};

    AemSide()
    {
        aem.set_send_response([](Eui48 const&, std::span<uint8_t const>) { return true; });
    }
};

}  // namespace

// Sections 5.4.4.2-3: the system unique id is 0 until a SET; a SET of 0 is
// refused; a change is reported once for saving
TEST(nanoavb_mvu, system_unique_id_set_get)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    int changes = 0;
    handler.set_on_settings_changed([&changes] { ++changes; });

    EXPECT_TRUE(handler.process_packet(SRC, get_system_unique_id_command(), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_GET_SYSTEM_UNIQUE_ID, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(sent.back()).common.control_data_length(), uint16_t{MvuDu::BASE_CONTROL_DATA_LENGTH + 6});
    EXPECT_EQ(reply_payload<MvuSystemUniqueIdPayload>(sent.back()).system_unique_id.get(), uint32_t{0});

    EXPECT_TRUE(handler.process_packet(SRC, set_system_unique_id_command(0x12345678), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_payload<MvuSystemUniqueIdPayload>(sent.back()).system_unique_id.get(), uint32_t{0x12345678});
    EXPECT_EQ(handler.system_unique_id(), uint32_t{0x12345678});
    EXPECT_EQ(changes, 1);

    EXPECT_TRUE(handler.process_packet(SRC, get_system_unique_id_command(), US));
    EXPECT_EQ(reply_payload<MvuSystemUniqueIdPayload>(sent.back()).system_unique_id.get(), uint32_t{0x12345678});

    // The same id again changes nothing
    EXPECT_TRUE(handler.process_packet(SRC, set_system_unique_id_command(0x12345678), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_SUCCESS);
    EXPECT_EQ(changes, 1);

    // 0 is the default, never a new id
    EXPECT_TRUE(handler.process_packet(SRC, set_system_unique_id_command(0), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(reply_payload<MvuSystemUniqueIdPayload>(sent.back()).system_unique_id.get(), uint32_t{0x12345678});
    EXPECT_EQ(handler.system_unique_id(), uint32_t{0x12345678});

    // A SET without the id
    std::array<uint8_t, 2> const reserved{};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, reserved), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(changes, 1);
}

// la_avdecc always sends the v1.3 form (u64 number + 64-octet name): its number
// is taken when it fits the v1.2 u32, and the answer is the v1.2 layout
TEST(nanoavb_mvu, system_unique_id_v13_command)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    MvuSystemUniqueIdPayloadV13 v13{};
    v13.number = uint64_t{0xABCDEF01};
    v13.name = mcr_domain_name("System A");
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, make_const_span(v13)), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(sent.back()).common.control_data_length(), uint16_t{MvuDu::BASE_CONTROL_DATA_LENGTH + 6});
    EXPECT_EQ(reply_payload<MvuSystemUniqueIdPayload>(sent.back()).system_unique_id.get(), uint32_t{0xABCDEF01});
    EXPECT_EQ(handler.system_unique_id(), uint32_t{0xABCDEF01});

    // Shorter than the full v1.3 payload: the v1.2 id, whatever follows it
    std::array<uint8_t, 10> const long_v12{0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, long_v12), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_SUCCESS);
    EXPECT_EQ(handler.system_unique_id(), uint32_t{7});

    v13.number = uint64_t{0x100000000};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, make_const_span(v13)), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(handler.system_unique_id(), uint32_t{7});
}

// Milan v1.2 Sections 5.4.4.4-5, 7.6: default priority 192, user priority following it, name
// "DEFAULT"; a SET changes only the flagged fields, the response carries every supported flag
TEST(nanoavb_mvu, media_clock_reference_info)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    int changes = 0;
    handler.set_on_settings_changed([&changes] { ++changes; });

    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(0), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(sent.back()).common.control_data_length(), uint16_t{MvuDu::BASE_CONTROL_DATA_LENGTH + 74});
    auto info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.clock_domain_index.get(), uint16_t{0});
    EXPECT_EQ(info.flags.get(), MVU_MCR_FLAGS_SUPPORTED);
    EXPECT_EQ(info.default_mcr_prio.get(), uint8_t{192});
    EXPECT_EQ(info.user_mcr_prio.get(), uint8_t{192});
    EXPECT_TRUE(info.media_clock_domain_name == mcr_domain_name("DEFAULT"));

    // The priority only: the name stays
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(0, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 200, "x"), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_SUCCESS);
    info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.flags.get(), MVU_MCR_FLAGS_SUPPORTED);
    EXPECT_EQ(info.default_mcr_prio.get(), uint8_t{192});
    EXPECT_EQ(info.user_mcr_prio.get(), uint8_t{200});
    EXPECT_TRUE(info.media_clock_domain_name == mcr_domain_name("DEFAULT"));
    EXPECT_EQ(changes, 1);

    // The name only, a full 64 octets with no NUL
    std::string const long_name(64, 'n');
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(0, MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID, 1, long_name), US));
    info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.user_mcr_prio.get(), uint8_t{200});
    EXPECT_TRUE(info.media_clock_domain_name == mcr_domain_name(long_name));
    EXPECT_EQ(changes, 2);

    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(0), US));
    info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.user_mcr_prio.get(), uint8_t{200});
    EXPECT_TRUE(info.media_clock_domain_name == mcr_domain_name(long_name));

    // No flags: nothing changes, nothing to save
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(0, 0, 9, "y"), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(changes, 2);

    // One clock domain by default: index 1 does not exist
    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(1), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_NO_SUCH_DESCRIPTOR);
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(1, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 1, ""), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_NO_SUCH_DESCRIPTOR);
    // Short commands
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, {}), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_BAD_ARGUMENTS);
    std::array<uint8_t, 8> const partial{};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, partial), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_BAD_ARGUMENTS);
    EXPECT_EQ(changes, 2);

    // A new default carries a user priority still at the old default along
    handler.set_default_mcr_priority(224);
    handler.set_clock_domain_count(2);
    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(1), US));
    info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.default_mcr_prio.get(), uint8_t{224});
    EXPECT_EQ(info.user_mcr_prio.get(), uint8_t{224});
    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(0), US));
    EXPECT_EQ(reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back()).user_mcr_prio.get(), uint8_t{200});
}

// Milan v1.3 Section 7.6.1.1: each clock domain has its own default priority; a user
// priority set away from the old default stays, one at it follows, and a domain
// the entity lacks is refused
TEST(nanoavb_mvu, default_priority_per_clock_domain)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    handler.set_clock_domain_count(2);
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(1, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 100, ""), US));

    EXPECT_TRUE(handler.set_default_mcr_priority(0, MVU_MCR_PRIO_ROUTER));
    EXPECT_TRUE(handler.set_default_mcr_priority(1, MVU_MCR_PRIO_ROUTER));
    EXPECT_FALSE(handler.set_default_mcr_priority(2, MVU_MCR_PRIO_ROUTER));
    EXPECT_EQ(handler.default_mcr_priority(0), MVU_MCR_PRIO_ROUTER);
    EXPECT_EQ(handler.default_mcr_priority(2), MVU_MCR_PRIO_NETWORK_INTERFACE);
    EXPECT_EQ(handler.default_mcr_priority(MvuCommandHandler::MAX_CLOCK_DOMAINS), MVU_MCR_PRIO_DEFAULT);

    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(0), US));
    auto info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.default_mcr_prio.get(), MVU_MCR_PRIO_ROUTER);
    EXPECT_EQ(info.user_mcr_prio.get(), MVU_MCR_PRIO_ROUTER);
    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(1), US));
    info = reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back());
    EXPECT_EQ(info.default_mcr_prio.get(), MVU_MCR_PRIO_ROUTER);
    EXPECT_EQ(info.user_mcr_prio.get(), uint8_t{100});
}

// The SETs honour the AEM lock: another controller's lock refuses them
// ENTITY_LOCKED (the GETs still answer); the locking controller may SET
TEST(nanoavb_mvu, sets_refused_while_locked)
{
    AemSide side;
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    handler.set_aem_handler(&side.aem);
    int changes = 0;
    handler.set_on_settings_changed([&changes] { ++changes; });

    AemLockEntityPayload lock{};
    EXPECT_TRUE(side.aem.process_packet(OTHER_MAC, aem_command(AEM_COMMAND_LOCK_ENTITY, OTHER, make_const_span(lock)), US));
    EXPECT_TRUE(side.aem.is_locked());

    EXPECT_TRUE(handler.process_packet(SRC, set_system_unique_id_command(5), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_SYSTEM_UNIQUE_ID, MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(handler.system_unique_id(), uint32_t{0});
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(0, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 50, ""), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_ENTITY_LOCKED);
    EXPECT_EQ(reply_payload<MvuMediaClockReferenceInfoPayload>(sent.back()).user_mcr_prio.get(), uint8_t{192});
    EXPECT_TRUE(handler.process_packet(SRC, get_mcr_command(0), US));
    expect_mvu_response(sent.back(), MVU_COMMAND_GET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(changes, 0);

    // The lock holder's SET goes through
    auto from_holder = set_mcr_command(0, MVU_MCR_FLAGS_MEDIA_CLOCK_REFERENCE_PRIORITY_VALID, 50, "");
    MvuDu du = reply_header(from_holder);
    du.common.controller_entity_id = OTHER;
    span_store(std::span<uint8_t>{from_holder}.first(MvuDu::LENGTH), du);
    EXPECT_TRUE(handler.process_packet(OTHER_MAC, from_holder, US));
    EXPECT_EQ(reply_header(sent.back()).common.status(), MVU_STATUS_SUCCESS);
    EXPECT_EQ(handler.mcr_info(0)->user_mcr_prio, uint8_t{50});
    EXPECT_EQ(changes, 1);
}

// A SET that changes something is sent, u bit set, to every controller
// registered for unsolicited notifications but the one that asked (Milan
// v1.2 Section 5.4.5.1), with its own controller id
TEST(nanoavb_mvu, set_notifies_registered_controllers)
{
    AemSide side;
    struct Sent
    {
        Eui48 mac;
        std::vector<uint8_t> bytes;
    };
    std::vector<Sent> sent;
    MvuCommandHandler handler{};
    handler.set_aem_handler(&side.aem);
    handler.set_send_response([&sent](Eui48 const& mac, std::span<uint8_t const> const resp) {
        sent.push_back({mac, {resp.begin(), resp.end()}});
        return true;
    });
    EXPECT_TRUE(side.aem.process_packet(SRC, aem_command(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, CONTROLLER), US));
    EXPECT_TRUE(side.aem.process_packet(OTHER_MAC, aem_command(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION, OTHER), US));

    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(0, MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID, 0, "Stage"), US));
    EXPECT_EQ(sent.size(), size_t{2});
    expect_mvu_response(sent[0].bytes, MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO, MVU_STATUS_SUCCESS);
    EXPECT_TRUE(sent[0].mac == SRC);
    EXPECT_TRUE(sent[1].mac == OTHER_MAC);
    auto const note = reply_header(sent[1].bytes);
    EXPECT_EQ(note.common.message_type(), AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_TRUE(note.unsolicited());
    EXPECT_EQ(note.command_type(), MVU_COMMAND_SET_MEDIA_CLOCK_REFERENCE_INFO);
    EXPECT_TRUE(note.common.target_entity_id == US);
    EXPECT_TRUE(note.common.controller_entity_id == OTHER);
    EXPECT_TRUE(
        reply_payload<MvuMediaClockReferenceInfoPayload>(sent[1].bytes).media_clock_domain_name == mcr_domain_name("Stage"));

    // No change, no notification
    EXPECT_TRUE(handler.process_packet(SRC, set_mcr_command(0, MVU_MCR_FLAGS_MEDIA_CLOCK_DOMAIN_NAME_VALID, 0, "Stage"), US));
    EXPECT_EQ(sent.size(), size_t{3});

    EXPECT_TRUE(handler.process_packet(SRC, set_system_unique_id_command(77), US));
    EXPECT_EQ(sent.size(), size_t{5});
    EXPECT_TRUE(sent[4].mac == OTHER_MAC);
    EXPECT_TRUE(reply_header(sent[4].bytes).unsolicited());
    EXPECT_EQ(
        reply_header(sent[4].bytes).common.sequence_id.get(), uint16_t(reply_header(sent[1].bytes).common.sequence_id.get() + 1));
    EXPECT_EQ(sent[4].bytes.size(), MVU_MIN_PDU_LENGTH);
}

// A reserved command type (v1.2 Table 5.18: 0x0005-0x7FFF, which v1.3 uses
// for BIND_STREAM and others) is NOT_IMPLEMENTED
TEST(nanoavb_mvu, unknown_command_type_not_implemented)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    std::array<uint8_t, 20> const bind{};
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(0x0005, bind), US));
    EXPECT_TRUE(handler.process_packet(SRC, mvu_command(0x7FFF, {}), US));
    EXPECT_EQ(sent.size(), size_t{2});
    expect_mvu_response(sent[0], 0x0005, MVU_STATUS_NOT_IMPLEMENTED);
    expect_mvu_response(sent[1], 0x7FFF, MVU_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(reply_header(sent[1]).common.control_data_length(), MvuDu::BASE_CONTROL_DATA_LENGTH);
    EXPECT_EQ(sent[1].size(), MVU_MIN_PDU_LENGTH);
}

// A GET_MILAN_INFO without its reserved field (cdl 18) is answered: the
// receiver ignores reserved. A frame shorter than its control_data_length, or
// declaring more than AECP allows, is dropped.
TEST(nanoavb_mvu, short_payload)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    // Declared and received without the reserved field (Ethernet padding
    // present, but not declared)
    auto const no_reserved = mvu_command(MVU_COMMAND_GET_MILAN_INFO, {}, 16);
    EXPECT_TRUE(handler.process_packet(SRC, no_reserved, US));
    EXPECT_EQ(sent.size(), size_t{1});
    expect_mvu_response(sent[0], MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);
    EXPECT_EQ(reply_header(sent[0]).common.control_data_length(), uint16_t{32});
    EXPECT_EQ(sent[0][35], uint8_t{0x01});  // protocol_version

    // Declares the reserved field but the frame stops before it: malformed
    auto truncated = get_milan_info_command();
    truncated.resize(MvuDu::LENGTH + 1);
    EXPECT_FALSE(handler.process_packet(SRC, truncated, US));
    // Declares more than AECP allows (600 > 524), all of it received
    auto oversized = get_milan_info_command(600);
    oversized[2] = 0x02;
    oversized[3] = 0x58;
    EXPECT_FALSE(handler.process_packet(SRC, oversized, US));
    EXPECT_EQ(sent.size(), size_t{1});
}

// Commands for another entity, and responses, are not answered
TEST(nanoavb_mvu, ignores_other_targets_and_responses)
{
    std::vector<std::vector<uint8_t>> sent;
    auto handler = make_handler(sent);
    EXPECT_FALSE(handler.process_packet(SRC, get_milan_info_command(), Eui64{0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09}));
    auto response = get_milan_info_command();
    response[1] = static_cast<uint8_t>((response[1] & 0xF0) | AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_FALSE(handler.process_packet(SRC, response, US));
    EXPECT_TRUE(sent.empty());
}

// dispatch_aecp: MVU commands reach the MVU handler; an unknown vendor unique
// protocol_id and AEM commands reach the AEM handler (NOT_IMPLEMENTED for the
// former, unchanged from before the MVU handler existed)
TEST(nanoavb_mvu, dispatch_aecp)
{
    DescriptorEntity entity{};
    entity.entity_id = US;
    entity.configurations_count = 1;
    auto model = EntityModelBuilder{}.entity(entity).configuration(DescriptorConfiguration{}).build();
    AemCommandHandler aem{model};
    std::vector<std::vector<uint8_t>> aem_sent;
    aem.set_send_response([&aem_sent](Eui48 const&, std::span<uint8_t const> const resp) {
        aem_sent.emplace_back(resp.begin(), resp.end());
        return true;
    });
    std::vector<std::vector<uint8_t>> mvu_sent;
    auto mvu = make_handler(mvu_sent);

    EXPECT_TRUE(dispatch_aecp(aem, mvu, SRC, get_milan_info_command(), US));
    EXPECT_EQ(mvu_sent.size(), size_t{1});
    EXPECT_TRUE(aem_sent.empty());
    expect_mvu_response(mvu_sent[0], MVU_COMMAND_GET_MILAN_INFO, MVU_STATUS_SUCCESS);

    // Unknown protocol_id: the AEM handler's NOT_IMPLEMENTED echo
    auto other = get_milan_info_command();
    other[27] = 0x01;
    EXPECT_TRUE(dispatch_aecp(aem, mvu, SRC, other, US));
    EXPECT_EQ(mvu_sent.size(), size_t{1});
    EXPECT_EQ(aem_sent.size(), size_t{1});
    AecpDuCommon reply{};
    span_load(reply, std::span<uint8_t const>{aem_sent[0]}.first(AecpDuCommon::LENGTH));
    EXPECT_EQ(reply.message_type(), AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_EQ(reply.status(), AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aem_sent[0][27], uint8_t{0x01});

    // An AEM command still reaches the AEM handler
    AemDu read{};
    read.init_command(AEM_COMMAND_GET_CONFIGURATION, AemDu::AEM_DATA_LENGTH);
    read.target_entity_id = US;
    std::array<uint8_t, AemDu::LENGTH> bytes{};
    span_store(bytes, read);
    EXPECT_TRUE(dispatch_aecp(aem, mvu, SRC, bytes, US));
    EXPECT_EQ(aem_sent.size(), size_t{2});
    EXPECT_EQ(mvu_sent.size(), size_t{1});
}

TEST_MAIN(statusbar_nanoavb, nanoavb_mvu_handler_test)
