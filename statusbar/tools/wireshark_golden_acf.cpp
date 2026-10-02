// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACF messages for the Wireshark golden capture: one of every clause 9.4 type
/// with every field at the value python/wireshark_schema/acf_table.py records
/// (test_value), built by the avtp_acf_<name>.hpp builders. Produced from that
/// table; keep the two in step when a test value changes.

#include "wireshark_golden_acf.hpp"

#include "statusbar/avtp/avtp_acf.hpp"
#include "statusbar/avtp/avtp_acf_aecp.hpp"
#include "statusbar/avtp/avtp_acf_ancillary.hpp"
#include "statusbar/avtp/avtp_acf_byte_bus.hpp"
#include "statusbar/avtp/avtp_acf_byte_bus_brief.hpp"
#include "statusbar/avtp/avtp_acf_can.hpp"
#include "statusbar/avtp/avtp_acf_can_brief.hpp"
#include "statusbar/avtp/avtp_acf_can_brief_v2.hpp"
#include "statusbar/avtp/avtp_acf_can_v2.hpp"
#include "statusbar/avtp/avtp_acf_can_xl.hpp"
#include "statusbar/avtp/avtp_acf_can_xl_brief.hpp"
#include "statusbar/avtp/avtp_acf_checksum.hpp"
#include "statusbar/avtp/avtp_acf_crc.hpp"
#include "statusbar/avtp/avtp_acf_flexray.hpp"
#include "statusbar/avtp/avtp_acf_gpc.hpp"
#include "statusbar/avtp/avtp_acf_i2c.hpp"
#include "statusbar/avtp/avtp_acf_i2c_brief.hpp"
#include "statusbar/avtp/avtp_acf_lin.hpp"
#include "statusbar/avtp/avtp_acf_lin_v2.hpp"
#include "statusbar/avtp/avtp_acf_most.hpp"
#include "statusbar/avtp/avtp_acf_parallel.hpp"
#include "statusbar/avtp/avtp_acf_sensor.hpp"
#include "statusbar/avtp/avtp_acf_sensor_brief.hpp"
#include "statusbar/avtp/avtp_acf_serial.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace statusbar::avtp::golden {

using ieee::Eui48;

namespace {

/// The five-octet payload every variable-length message carries (three pad octets follow)
constexpr std::array<uint8_t, 5> PAYLOAD{0xA1, 0xB2, 0xC3, 0xD4, 0xE5};

/// Append @p octets of @p buf to @p out
void append(std::vector<uint8_t>& out, std::span<uint8_t const> const buf, size_t const octets)
{
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(octets));
}

}  // namespace

auto acf_flexray_message() -> std::vector<uint8_t>
{
    AcfFlexrayMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_fr_bus_id(0x15U);
    m.set_chan(0x2U);
    m.set_str(true);
    m.set_syn(false);
    m.set_pre(true);
    m.set_nfi(true);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_fr_frame_id(0x5A5U);
    m.set_cycle(0x2AU);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_flexray_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_can_message() -> std::vector<uint8_t>
{
    AcfCanMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_rtr(false);
    m.set_eff(true);
    m.set_brs(true);
    m.set_fdf(true);
    m.set_esi(false);
    m.set_can_bus_id(0x13U);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_can_identifier(0x1ABCDEFU);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_can_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_can_v2_message() -> std::vector<uint8_t>
{
    AcfCanV2Message m{};
    m.init();
    m.set_mtv(true);
    m.set_rtr(false);
    m.set_eff(true);
    m.set_can_bus_id(0x5ABU);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_brs(true);
    m.set_fdf(true);
    m.set_esi(false);
    m.set_can_identifier(0x1ABCDEFU);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_can_v2_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_can_brief_message() -> std::vector<uint8_t>
{
    AcfCanBriefMessage m{};
    m.init();
    m.set_rtr(false);
    m.set_eff(true);
    m.set_brs(true);
    m.set_fdf(true);
    m.set_esi(false);
    m.set_can_bus_id(0x13U);
    m.set_can_identifier(0x1ABCDEFU);
    std::array<uint8_t, 16> buf{};
    auto const octets = acf_can_brief_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_can_brief_v2_message() -> std::vector<uint8_t>
{
    AcfCanBriefV2Message m{};
    m.init();
    m.set_rtr(false);
    m.set_eff(true);
    m.set_can_bus_id(0x5ABU);
    m.set_brs(true);
    m.set_fdf(true);
    m.set_esi(false);
    m.set_can_identifier(0x1ABCDEFU);
    std::array<uint8_t, 16> buf{};
    auto const octets = acf_can_brief_v2_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_lin_message() -> std::vector<uint8_t>
{
    AcfLinMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_lin_bus_id(0xBU);
    m.set_lin_identifier(0x3CU);
    m.set_message_timestamp(0x102030405060708ULL);
    std::array<uint8_t, 20> buf{};
    auto const octets = acf_lin_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_lin_v2_message() -> std::vector<uint8_t>
{
    AcfLinV2Message m{};
    m.init();
    m.set_mtv(true);
    m.set_lin_bus_id(0x4CDU);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_lin_identifier(0x3CU);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_lin_v2_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_most_message() -> std::vector<uint8_t>
{
    AcfMostMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_most_net_id(0x7U);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_device_id(0x123U);
    m.set_fblock_id(0x45U);
    m.set_inst_id(0x67U);
    m.set_func_id(0x89AU);
    m.set_op_type(0xBU);
    std::array<uint8_t, 28> buf{};
    auto const octets = acf_most_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_can_xl_message() -> std::vector<uint8_t>
{
    AcfCanXlMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_can_bus_id(0x2ABU);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_vcid(0x12U);
    m.set_sdt(0x34U);
    m.set_rrs(true);
    m.set_sec(false);
    m.set_priority_id(0x5A5U);
    m.set_acceptance_field(0xDEADBEEFU);
    m.set_transaction_num(0x77U);
    m.set_ms(true);
    m.set_segment_num(0x123U);
    std::array<uint8_t, 32> buf{};
    auto const octets = acf_can_xl_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_can_xl_brief_message() -> std::vector<uint8_t>
{
    AcfCanXlBriefMessage m{};
    m.init();
    m.set_can_bus_id(0x2ABU);
    m.set_vcid(0x12U);
    m.set_sdt(0x34U);
    m.set_rrs(true);
    m.set_sec(false);
    m.set_priority_id(0x5A5U);
    m.set_acceptance_field(0xDEADBEEFU);
    m.set_transaction_num(0x77U);
    m.set_ms(true);
    m.set_segment_num(0x123U);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_can_xl_brief_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_gpc_message() -> std::vector<uint8_t>
{
    AcfGpcMessage m{};
    m.init();
    m.set_gpc_msg_id(Eui48{0x00, 0x1C, 0xAB, 0x12, 0x34, 0x56});
    std::array<uint8_t, 16> buf{};
    auto const octets = acf_gpc_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_serial_message() -> std::vector<uint8_t>
{
    AcfSerialMessage m{};
    m.init();
    m.set_dcd(true);
    m.set_dtr(false);
    m.set_dsr(true);
    m.set_rts(true);
    m.set_cts(false);
    m.set_ri(true);
    std::array<uint8_t, 12> buf{};
    auto const octets = acf_serial_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_parallel_message() -> std::vector<uint8_t>
{
    AcfParallelMessage m{};
    m.init();
    m.set_bit_width(0x25U);
    std::array<uint8_t, 12> buf{};
    auto const octets = acf_parallel_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_sensor_message() -> std::vector<uint8_t>
{
    AcfSensorMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_num_sensors(0x5U);
    m.set_sz(0x1U);
    m.set_sensor_group(0x2AU);
    m.set_message_timestamp(0x102030405060708ULL);
    std::array<uint8_t, 20> buf{};
    auto const octets = acf_sensor_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_sensor_brief_message() -> std::vector<uint8_t>
{
    AcfSensorBriefMessage m{};
    m.init();
    m.set_num_sensors(0x5U);
    m.set_sz(0x1U);
    m.set_sensor_group(0x2AU);
    std::array<uint8_t, 12> buf{};
    auto const octets = acf_sensor_brief_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_aecp_message() -> std::vector<uint8_t>
{
    AcfAecpMessage m{};
    m.init();
    std::array<uint8_t, 12> buf{};
    auto const octets = acf_aecp_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_ancillary_message() -> std::vector<uint8_t>
{
    AcfAncillaryMessage m{};
    m.init();
    m.set_mode(0x1U);
    m.set_fp(true);
    m.set_lp(false);
    m.set_line_number(0x2A5U);
    m.set_did(0x61U);
    m.set_sdid_dbn(0x2U);
    std::array<uint8_t, 16> buf{};
    auto const octets = acf_ancillary_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_byte_bus_message() -> std::vector<uint8_t>
{
    AcfByteBusMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_byte_bus_id(0x3C5U);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_evt(0xAU);
    m.set_hs(true);
    m.set_cs(false);
    m.set_transaction_num(0x5CU);
    m.set_op(true);
    m.set_rsp(false);
    m.set_err(false);
    m.set_ms(true);
    m.set_read_size_segment_num(0x9ABU);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_byte_bus_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_byte_bus_brief_message() -> std::vector<uint8_t>
{
    AcfByteBusBriefMessage m{};
    m.init();
    m.set_byte_bus_id(0x3C5U);
    m.set_evt(0xAU);
    m.set_hs(true);
    m.set_cs(false);
    m.set_transaction_num(0x5CU);
    m.set_op(true);
    m.set_rsp(false);
    m.set_err(false);
    m.set_ms(true);
    m.set_read_size_segment_num(0x9ABU);
    std::array<uint8_t, 16> buf{};
    auto const octets = acf_byte_bus_brief_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>(PAYLOAD));
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_i2c_message() -> std::vector<uint8_t>
{
    AcfI2cMessage m{};
    m.init();
    m.set_mtv(true);
    m.set_i2c_bus_id(0x2F1U);
    m.set_message_timestamp(0x102030405060708ULL);
    m.set_i2c_code(0x4U);
    m.set_trr(true);
    m.set_transaction_num(0x33U);
    m.set_evt(0x5U);
    m.set_exception_code(0x9U);
    m.set_i2c_data(0xA3U);
    std::array<uint8_t, 24> buf{};
    auto const octets = acf_i2c_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>{});
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_i2c_brief_message() -> std::vector<uint8_t>
{
    AcfI2cBriefMessage m{};
    m.init();
    m.set_i2c_bus_id(0x2F1U);
    m.set_i2c_code(0x4U);
    m.set_trr(true);
    m.set_transaction_num(0x33U);
    m.set_evt(0x5U);
    m.set_exception_code(0x9U);
    m.set_i2c_data(0xA3U);
    std::array<uint8_t, 16> buf{};
    auto const octets = acf_i2c_brief_build(std::span<uint8_t>(buf), m, std::span<uint8_t const>{});
    std::vector<uint8_t> out;
    append(out, std::span<uint8_t const>(buf), octets);
    return out;
}

auto acf_with_checksum(std::vector<uint8_t> message) -> std::vector<uint8_t>
{
    std::array<uint8_t, 4> trailer{};
    (void)acf_checksum_build(std::span<uint8_t>(trailer), std::span<uint8_t const>(message));
    message.insert(message.end(), trailer.begin(), trailer.end());
    return message;
}

auto acf_with_crc(std::vector<uint8_t> message, AcfCrcType const type) -> std::vector<uint8_t>
{
    std::array<uint8_t, 8> trailer{};
    (void)acf_crc_build(std::span<uint8_t>(trailer), type, std::span<uint8_t const>(message));
    message.insert(message.end(), trailer.begin(), trailer.end());
    return message;
}

auto acf_user_message() -> std::vector<uint8_t>
{
    std::vector<uint8_t> out(8, 0);
    (void)acf_store_header(std::span<uint8_t>(out), AcfMsgType::user_first + 2U, 2);
    out[2] = 0xAB;
    out[7] = 0xCD;
    return out;
}

auto acf_golden_messages() -> std::vector<AcfGoldenMessage>
{
    return {
        {"flexray", acf_flexray_message()},
        {"can", acf_can_message()},
        {"can_v2", acf_can_v2_message()},
        {"can_brief", acf_can_brief_message()},
        {"can_brief_v2", acf_can_brief_v2_message()},
        {"lin", acf_lin_message()},
        {"lin_v2", acf_lin_v2_message()},
        {"most", acf_most_message()},
        {"can_xl", acf_can_xl_message()},
        {"can_xl_brief", acf_can_xl_brief_message()},
        {"gpc", acf_gpc_message()},
        {"serial", acf_serial_message()},
        {"parallel", acf_parallel_message()},
        {"sensor", acf_sensor_message()},
        {"sensor_brief", acf_sensor_brief_message()},
        {"aecp", acf_aecp_message()},
        {"ancillary", acf_ancillary_message()},
        {"byte_bus", acf_byte_bus_message()},
        {"byte_bus_brief", acf_byte_bus_brief_message()},
        {"i2c", acf_i2c_message()},
        {"i2c_brief", acf_i2c_brief_message()},
    };
}

}  // namespace statusbar::avtp::golden
