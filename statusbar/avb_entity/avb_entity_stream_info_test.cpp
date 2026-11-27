// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Milan v1.3 5.4.2.10.1, Table 5.8: GET_STREAM_INFO of a stream input, as its sink's state decides it.

#include "statusbar/avb_entity/avb_entity_stream_info.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_aem_command.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb_srp.hpp"
#include "statusbar/test/test.hpp"

#include <cstdint>
#include <optional>

using namespace statusbar;
using namespace statusbar::avb_entity;
using atdecc::aem::AemStreamInfoPayload;
namespace flag = atdecc::aem::stream_info_flags;

namespace {

constexpr ieee::Eui64 STREAM{0x00, 0x1C, 0xAB, 0x00, 0x00, 0x01, 0x00, 0x02};
constexpr ieee::Eui48 DEST{0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34};
constexpr ieee::Eui64 BRIDGE{0x80, 0x00, 0x00, 0x1C, 0xAB, 0x00, 0x00, 0x09};
constexpr uint32_t BOUND = flag::FAST_CONNECT | flag::SAVED_STATE | flag::CONNECTED;
constexpr uint32_t SETTLED = flag::STREAM_ID_VALID | flag::STREAM_DEST_MAC_VALID | flag::STREAM_VLAN_ID_VALID;

/// The SRP parameters a talker's PROBE_TX_RESPONSE gave.
constexpr atdecc::MilanSrpParameters PROBED{.stream_id = STREAM, .stream_dest_mac = DEST, .stream_vlan_id = 2};
/// A matching Talker Advertise, and a matching Talker Failed. The Advertise is given failure
/// fields it would never carry, to show that only a Talker Failed's are answered.
constexpr nanoavb::RegisteredTalker ADVERTISE{
    .dest_address = DEST,
    .vlan_id = 2,
    .failed = false,
    .accumulated_latency = 123'456,
    .failure_code = 7,
    .failure_bridge_id = BRIDGE};
constexpr nanoavb::RegisteredTalker FAILED{
    .dest_address = DEST,
    .vlan_id = 2,
    .failed = true,
    .accumulated_latency = 654'321,
    .failure_code = 1,
    .failure_bridge_id = BRIDGE};

/// The answer for @p sink, begun from a payload with every field set: a field Table 5.8 does
/// not mark valid has to come out zero.
auto answer(MilanSinkStreamInfo const& sink) -> AemStreamInfoPayload
{
    AemStreamInfoPayload out{};
    out.stream_id = BRIDGE;
    out.stream_dest_mac = ieee::Eui48{1, 2, 3, 4, 5, 6};
    out.stream_vlan_id = ieee::doublet_t{99};
    out.msrp_accumulated_latency = ieee::quadlet_t{99};
    out.msrp_failure_code = ieee::octet_t{99};
    out.msrp_failure_bridge_id = STREAM;
    out.flags = ieee::quadlet_t{fill_milan_sink_stream_info(sink, out)};
    return out;
}

/// The stream's id, address and VLAN of @p info are zero, and so are the MSRP fields.
auto nothing_valid(AemStreamInfoPayload const& info) -> bool
{
    return info.stream_id == ieee::Eui64{} && info.stream_dest_mac == ieee::Eui48{} && info.stream_vlan_id.get() == 0 &&
        info.msrp_accumulated_latency.get() == 0 && info.msrp_failure_code.get() == 0 &&
        info.msrp_failure_bridge_id == ieee::Eui64{};
}

}  // namespace

TEST(milan_sink_stream_info, an_input_that_is_not_bound_sets_no_flag_and_no_field)
{
    auto const info = answer(MilanSinkStreamInfo{});
    EXPECT_EQ(info.flags.get(), 0U);
    EXPECT_TRUE(nothing_valid(info));
    // STREAMING_WAIT is undefined for an input that is not bound: it is left clear.
    EXPECT_EQ(answer(MilanSinkStreamInfo{.stopped = true}).flags.get(), 0U);
}

TEST(milan_sink_stream_info, a_bound_input_that_is_not_settled_is_connected_and_registers_nothing)
{
    // FAST_CONNECT, SAVED_STATE and CONNECTED "if the Stream Input is bound"; the stream's
    // fields are valid "if, and only if, the Stream Input is settled".
    auto const info = answer(MilanSinkStreamInfo{.bound = true, .srp = PROBED, .talker = ADVERTISE});
    EXPECT_EQ(info.flags.get(), BOUND | flag::NOT_REGISTERING_SRP);
    EXPECT_TRUE(nothing_valid(info));

    auto const stopped = answer(MilanSinkStreamInfo{.bound = true, .stopped = true});
    EXPECT_EQ(stopped.flags.get(), BOUND | flag::NOT_REGISTERING_SRP | flag::STREAMING_WAIT);
}

TEST(milan_sink_stream_info, a_settled_input_gives_what_its_talker_answered_the_probe_with)
{
    auto const info = answer(MilanSinkStreamInfo{.bound = true, .settled = true, .srp = PROBED});
    EXPECT_EQ(info.flags.get(), BOUND | SETTLED | flag::NOT_REGISTERING_SRP);
    EXPECT_EQ(info.stream_id, STREAM);
    EXPECT_EQ(info.stream_dest_mac, DEST);
    EXPECT_EQ(info.stream_vlan_id.get(), 2);
    EXPECT_EQ(info.msrp_accumulated_latency.get(), 0U);
    EXPECT_EQ(info.msrp_failure_code.get(), 0);

    // A talker that named no VLAN: the field is the 0 it gave, and still marked valid.
    auto no_vlan = PROBED;
    no_vlan.stream_vlan_id = 0;
    auto const zero = answer(MilanSinkStreamInfo{.bound = true, .settled = true, .srp = no_vlan});
    EXPECT_EQ(zero.flags.get(), BOUND | SETTLED | flag::NOT_REGISTERING_SRP);
    EXPECT_EQ(zero.stream_vlan_id.get(), 0);
}

TEST(milan_sink_stream_info, a_registered_talker_advertise_gives_the_latency_and_no_failure)
{
    // MSRP_ACC_LAT_VALID "has the opposite value of the NOT_REGISTERING_SRP bit".
    auto const info = answer(MilanSinkStreamInfo{.bound = true, .settled = true, .srp = PROBED, .talker = ADVERTISE});
    EXPECT_EQ(info.flags.get(), BOUND | SETTLED | flag::MSRP_ACC_LAT_VALID);
    EXPECT_EQ(info.msrp_accumulated_latency.get(), 123'456U);
    EXPECT_EQ(info.msrp_failure_code.get(), 0);
    EXPECT_EQ(info.msrp_failure_bridge_id, ieee::Eui64{});

    auto const stopped =
        answer(MilanSinkStreamInfo{.bound = true, .settled = true, .stopped = true, .srp = PROBED, .talker = ADVERTISE});
    EXPECT_EQ(stopped.flags.get(), BOUND | SETTLED | flag::MSRP_ACC_LAT_VALID | flag::STREAMING_WAIT);
}

TEST(milan_sink_stream_info, a_registered_talker_failed_gives_the_latency_the_bridge_and_the_code)
{
    // MSRP_FAILURE_VALID "has the same value as the SRP_REGISTRATION_FAILED bit".
    auto const info = answer(MilanSinkStreamInfo{.bound = true, .settled = true, .srp = PROBED, .talker = FAILED});
    EXPECT_EQ(
        info.flags.get(), BOUND | SETTLED | flag::MSRP_ACC_LAT_VALID | flag::SRP_REGISTRATION_FAILED | flag::MSRP_FAILURE_VALID);
    EXPECT_EQ(info.msrp_accumulated_latency.get(), 654'321U);
    EXPECT_EQ(info.msrp_failure_code.get(), 1);
    EXPECT_EQ(info.msrp_failure_bridge_id, BRIDGE);
    EXPECT_EQ(info.stream_id, STREAM);
}

TEST(milan_sink_stream_info, the_flags_have_the_values_of_table_5_8)
{
    EXPECT_EQ(flag::FAST_CONNECT, 0x00000002U);
    EXPECT_EQ(flag::SAVED_STATE, 0x00000004U);
    EXPECT_EQ(flag::STREAMING_WAIT, 0x00000008U);
    EXPECT_EQ(flag::SRP_REGISTRATION_FAILED, 0x00000040U);
    EXPECT_EQ(flag::NOT_REGISTERING_SRP, 0x01000000U);
    EXPECT_EQ(flag::STREAM_VLAN_ID_VALID, 0x02000000U);
    EXPECT_EQ(flag::CONNECTED, 0x04000000U);
    EXPECT_EQ(flag::MSRP_FAILURE_VALID, 0x08000000U);
    EXPECT_EQ(flag::STREAM_DEST_MAC_VALID, 0x10000000U);
    EXPECT_EQ(flag::MSRP_ACC_LAT_VALID, 0x20000000U);
    EXPECT_EQ(flag::STREAM_ID_VALID, 0x40000000U);
    EXPECT_EQ(flag::STREAM_FORMAT_VALID, 0x80000000U);
}

TEST_MAIN(statusbar_avb_entity, avb_entity_stream_info_test)
