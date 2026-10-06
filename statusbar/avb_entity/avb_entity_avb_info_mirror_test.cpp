// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#include "statusbar/avb_entity/avb_entity_avb_info_mirror.hpp"

#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aecp_aem.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using statusbar::avb_entity::AvbInfoMirror;
using statusbar::ieee::Eui48;
using statusbar::ieee::Eui64;

namespace {

Eui64 const kController{0x70, 0xB3, 0xD5, 0xED, 0xC2, 0x00, 0xC8, 0xF0};
Eui64 const kTarget{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0xC8, 0xF0};
Eui48 const kTargetMac{0x00, 0x1C, 0xAB, 0x00, 0xC8, 0xF0};
Eui64 const kGm{0x00, 0x01, 0xF2, 0xFF, 0xFE, 0x00, 0x68, 0x82};

constexpr int64_t SEC = 1'000'000'000;

/// The mirror under test with its sent commands captured.
struct Rig
{
    std::vector<std::vector<uint8_t>> multicast;
    std::vector<std::vector<uint8_t>> unicast;
    std::vector<std::pair<uint16_t, std::optional<DescriptorAvbInterface>>> descriptors;
    std::vector<std::pair<uint16_t, nanoavb::AvbInfo>> infos;
    AvbInfoMirror mirror;

    Rig()
        : mirror{
              AvbInfoMirror::Config{.controller_entity_id = kController, .target_entity_id = kTarget, .poll_interval_ns = 5 * SEC},
              [this](std::span<uint8_t const> p) {
                  multicast.emplace_back(p.begin(), p.end());
                  return true;
              },
              [this](Eui48 const&, std::span<uint8_t const> p) {
                  unicast.emplace_back(p.begin(), p.end());
                  return true;
              }}
    {
        mirror.set_on_update([this](uint16_t index, DescriptorAvbInterface const* d, nanoavb::AvbInfo const* i) {
            if (d != nullptr) {
                descriptors.emplace_back(index, *d);
            }
            if (i != nullptr) {
                infos.emplace_back(index, *i);
            }
        });
    }

    /// Announce the target so the mirror learns its MAC.
    void target_available(int64_t now)
    {
        AdpDu adp{};
        adp.init_entity_available(kTarget, 31);
        std::vector<uint8_t> frame(AdpDu::LENGTH);
        span_store(std::span<uint8_t>{frame}, adp);
        mirror.receive_frame(kTargetMac, frame, now);
    }

    /// The last unicast AEM command of @p code, or nullopt.
    auto last_command(uint16_t code) -> std::optional<AemDu>
    {
        for (auto it = unicast.rbegin(); it != unicast.rend(); ++it) {
            if (it->size() >= AemDu::LENGTH) {
                AemDu hdr{};
                span_load(hdr, std::span<uint8_t const>{*it}.first(AemDu::LENGTH));
                if (hdr.command_code() == code) {
                    return hdr;
                }
            }
        }
        return std::nullopt;
    }

    /// Answer the command @p cmd with @p status and @p body.
    void respond(AemDu const& cmd, uint8_t status, std::span<uint8_t const> body, int64_t now, bool unsolicited = false)
    {
        AemDu hdr = cmd;
        hdr.init_response(cmd.command_code(), status, static_cast<uint16_t>(AemDu::AEM_DATA_LENGTH + body.size()), unsolicited);
        std::vector<uint8_t> frame(AemDu::LENGTH + body.size());
        span_store(std::span<uint8_t>{frame}.first(AemDu::LENGTH), hdr);
        std::copy(body.begin(), body.end(), frame.begin() + static_cast<std::ptrdiff_t>(AemDu::LENGTH));
        mirror.receive_frame(kTargetMac, frame, now);
    }
};

auto interface_descriptor_response(uint16_t index, uint8_t priority1) -> std::vector<uint8_t>
{
    std::vector<uint8_t> body(AemReadDescriptorResponsePayload::LENGTH + DescriptorAvbInterface::LENGTH, 0);
    DescriptorAvbInterface d{};
    d.descriptor_index = index;
    d.mac_address = kTargetMac;
    d.clock_identity = Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0xC8, 0xF0};
    d.priority1 = priority1;
    d.priority2 = 248;
    d.domain_number = 0;
    d.port_number = static_cast<uint16_t>(index + 1);
    span_store(std::span<uint8_t>{body}.subspan(AemReadDescriptorResponsePayload::LENGTH), d);
    return body;
}

auto avb_info_response(uint16_t index, Eui64 const& gm, uint32_t delay, uint8_t flags) -> std::vector<uint8_t>
{
    std::vector<uint8_t> body(AemAvbInfoPayload::LENGTH + 4, 0);
    AemAvbInfoPayload p{};
    p.descriptor_type = DESCRIPTOR_AVB_INTERFACE;
    p.descriptor_index = index;
    p.gptp_grandmaster_id = tsn::ClockIdentity{gm};
    p.propagation_delay = delay;
    p.flags = flags;
    p.msrp_mappings_count = 1;
    span_store(std::span<uint8_t>{body}.first(AemAvbInfoPayload::LENGTH), p);
    body[AemAvbInfoPayload::LENGTH + 1] = 3;  // class A, pcp 3
    body[AemAvbInfoPayload::LENGTH + 3] = 2;  // vlan 2
    return body;
}

}  // namespace

TEST(avb_entity_avb_info_mirror, discovers_reads_registers_and_polls)
{
    Rig rig;
    int64_t now = SEC;

    // Until the target answers ADP the mirror only sends ENTITY_DISCOVER.
    rig.mirror.tick(now);
    EXPECT_FALSE(rig.mirror.target_seen());
    EXPECT_EQ(rig.multicast.size(), size_t{1});
    EXPECT_TRUE(rig.unicast.empty());

    rig.target_available(now);
    EXPECT_TRUE(rig.mirror.target_seen());

    // Descriptor reads, one at a time, until NO_SUCH_DESCRIPTOR.
    now += SEC;
    rig.mirror.tick(now);
    auto read0 = rig.last_command(AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_TRUE(read0.has_value());
    rig.respond(*read0, AEM_STATUS_SUCCESS, interface_descriptor_response(0, 246), now);
    EXPECT_EQ(rig.descriptors.size(), size_t{1});
    EXPECT_EQ(rig.descriptors[0].first, 0);
    EXPECT_EQ(static_cast<uint8_t>(rig.descriptors[0].second->priority1), 246);

    now += SEC;
    rig.mirror.tick(now);
    auto read1 = rig.last_command(AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_TRUE(read1.has_value() && read1->sequence_id.get() != read0->sequence_id.get());
    rig.respond(*read1, AEM_STATUS_SUCCESS, interface_descriptor_response(1, 250), now);
    EXPECT_EQ(rig.descriptors.size(), size_t{2});

    now += SEC;
    rig.mirror.tick(now);
    auto read2 = rig.last_command(AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_TRUE(read2.has_value());
    rig.respond(*read2, AEM_STATUS_NO_SUCH_DESCRIPTOR, {}, now);
    EXPECT_EQ(rig.descriptors.size(), size_t{2});
    EXPECT_EQ(rig.mirror.interfaces().size(), size_t{2});

    // Descriptors done: registration, then one GET_AVB_INFO per interface.
    now += SEC;
    rig.mirror.tick(now);
    EXPECT_TRUE(rig.last_command(AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION).has_value());
    int polls = 0;
    for (auto const& p : rig.unicast) {
        AemDu hdr{};
        span_load(hdr, std::span<uint8_t const>{p}.first(AemDu::LENGTH));
        if (hdr.command_code() == AEM_COMMAND_GET_AVB_INFO) {
            ++polls;
        }
    }
    EXPECT_EQ(polls, 2);

    // An answer surfaces once; the same answer again is quiet.
    auto poll = rig.last_command(AEM_COMMAND_GET_AVB_INFO);
    EXPECT_TRUE(poll.has_value());
    auto const flags = avb_info_flags::AS_CAPABLE | avb_info_flags::GPTP_ENABLED | avb_info_flags::SRP_ENABLED;
    rig.respond(*poll, AEM_STATUS_SUCCESS, avb_info_response(1, kGm, 412, flags), now);
    EXPECT_EQ(rig.infos.size(), size_t{1});
    EXPECT_EQ(rig.infos[0].first, 1);
    EXPECT_EQ(rig.infos[0].second.gptp_grandmaster_id, kGm);
    EXPECT_EQ(rig.infos[0].second.propagation_delay, 412U);
    EXPECT_EQ(rig.infos[0].second.msrp_mappings.size(), size_t{1});
    EXPECT_EQ(rig.infos[0].second.msrp_mappings[0].vlan_id, 2);

    // Re-poll after the interval; an unchanged answer is not reported.
    now += 6 * SEC;
    rig.mirror.tick(now);
    poll = rig.last_command(AEM_COMMAND_GET_AVB_INFO);
    rig.respond(*poll, AEM_STATUS_SUCCESS, avb_info_response(1, kGm, 412, flags), now);
    EXPECT_EQ(rig.infos.size(), size_t{1});

    // The device's unsolicited GET_AVB_INFO with a new grandmaster is reported.
    AemDu unsol = *poll;
    unsol.sequence_id = ieee::doublet_t{0x4242};
    rig.respond(
        unsol,
        AEM_STATUS_SUCCESS,
        avb_info_response(0, Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04}, 400, flags),
        now,
        true);
    EXPECT_EQ(rig.infos.size(), size_t{2});
    EXPECT_EQ(rig.infos[1].first, 0);
    EXPECT_EQ(rig.infos[1].second.gptp_grandmaster_id, (Eui64{0x00, 0x1C, 0xAB, 0xFF, 0xFE, 0x00, 0x76, 0x04}));
    EXPECT_TRUE(rig.mirror.find(0) != nullptr && rig.mirror.find(0)->info.has_value());
}

TEST_MAIN(statusbar_avb_entity, avb_entity_avb_info_mirror_test)
