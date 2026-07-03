// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Unit tests for ATDECC module
// Tests ACMP, ADP, and AECP protocol data units

#include "statusbar/atdecc/atdecc_format.hpp"
#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

using namespace statusbar;
using namespace statusbar::atdecc;
using namespace statusbar::avtp;
using namespace statusbar::ieee;

//
// Runtime verification of name functions
//

TEST(acmp_names, message_type_name)
{
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND)[0], 'C');
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE)[0], 'C');
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND)[0], 'D');
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND)[0], 'G');
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND)[0], 'C');
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND)[0], 'G');
    EXPECT_EQ(acmp_message_type_name(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND)[0], 'G');
    EXPECT_EQ(acmp_message_type_name(0xFF)[0], 'U');  // Unknown
}

TEST(acmp_names, status_name)
{
    EXPECT_EQ(acmp_status_name(ACMP_STATUS_SUCCESS)[0], 'S');
    EXPECT_EQ(acmp_status_name(ACMP_STATUS_LISTENER_UNKNOWN_ID)[0], 'L');
    EXPECT_EQ(acmp_status_name(ACMP_STATUS_TALKER_UNKNOWN_ID)[0], 'T');
    EXPECT_EQ(acmp_status_name(ACMP_STATUS_NOT_SUPPORTED)[0], 'N');
    EXPECT_EQ(acmp_status_name(0xFF)[0], 'U');  // Unknown
}

TEST(adp_names, message_type_name)
{
    EXPECT_EQ(adp_message_type_name(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE)[0], 'E');
    EXPECT_EQ(adp_message_type_name(ADP_MESSAGE_TYPE_ENTITY_DEPARTING)[0], 'E');
    EXPECT_EQ(adp_message_type_name(ADP_MESSAGE_TYPE_ENTITY_DISCOVER)[0], 'E');
    EXPECT_EQ(adp_message_type_name(0xFF)[0], 'U');  // Unknown
}

TEST(aecp_names, message_type_name)
{
    EXPECT_EQ(aecp_message_type_name(AECP_MESSAGE_TYPE_AEM_COMMAND)[0], 'A');
    EXPECT_EQ(aecp_message_type_name(AECP_MESSAGE_TYPE_AEM_RESPONSE)[0], 'A');
    EXPECT_EQ(aecp_message_type_name(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND)[0], 'V');
    EXPECT_EQ(aecp_message_type_name(0xFF)[0], 'U');  // Unknown
}

TEST(aecp_names, status_name)
{
    EXPECT_EQ(aecp_status_name(AECP_STATUS_SUCCESS)[0], 'S');
    EXPECT_EQ(aecp_status_name(AECP_STATUS_NOT_IMPLEMENTED)[0], 'N');
    EXPECT_EQ(aecp_status_name(0xFF)[0], 'U');  // Unknown
}

TEST(aem_names, status_name)
{
    EXPECT_EQ(aem_status_name(AEM_STATUS_SUCCESS)[0], 'S');
    EXPECT_EQ(aem_status_name(AEM_STATUS_NOT_IMPLEMENTED)[0], 'N');
    EXPECT_EQ(aem_status_name(AEM_STATUS_NO_SUCH_DESCRIPTOR)[0], 'N');
    EXPECT_EQ(aem_status_name(AEM_STATUS_ENTITY_LOCKED)[0], 'E');
    EXPECT_EQ(aem_status_name(0xFF)[0], 'U');  // Unknown
}

TEST(aem_names, descriptor_type_name_2021)
{
    EXPECT_EQ(std::string_view(aem::descriptor_type_name(aem::DESCRIPTOR_TIMING)), "Timing");
    EXPECT_EQ(std::string_view(aem::descriptor_type_name(aem::DESCRIPTOR_PTP_INSTANCE)), "PTP Instance");
    EXPECT_EQ(std::string_view(aem::descriptor_type_name(aem::DESCRIPTOR_PTP_PORT)), "PTP Port");
}

TEST(aem_names, command_name)
{
    EXPECT_EQ(aem_command_name(AEM_COMMAND_ACQUIRE_ENTITY)[0], 'A');
    EXPECT_EQ(aem_command_name(AEM_COMMAND_LOCK_ENTITY)[0], 'L');
    EXPECT_EQ(aem_command_name(AEM_COMMAND_READ_DESCRIPTOR)[0], 'R');
    EXPECT_EQ(aem_command_name(AEM_COMMAND_START_STREAMING)[0], 'S');
    EXPECT_EQ(aem_command_name(0x7FFE)[0], 'U');  // Unknown
}

//
// 2021 descriptor struct layout tests
//

TEST(aem_descriptor_stream, struct_size_2021)
{
    using namespace aem;
    // sizeof() is now much larger than LENGTH because DescriptorStream
    // carries an inline stream_formats trailer on top of the 138-byte
    // fixed header. LENGTH is the on-wire header size.
    EXPECT_EQ(sizeof(DescriptorStream), DescriptorStream::LENGTH + (DescriptorStream::MAX_STREAM_FORMATS * sizeof(Eui64)));
    EXPECT_EQ(DescriptorStream::LENGTH, 138u);
    EXPECT_EQ(DescriptorStream::MINIMUM_LENGTH, 132u);
    EXPECT_EQ(offsetof(DescriptorStream, redundant_offset), 132u);
    EXPECT_EQ(offsetof(DescriptorStream, number_of_redundant_streams), 134u);
    EXPECT_EQ(offsetof(DescriptorStream, timing), 136u);

    DescriptorStream s{};
    EXPECT_EQ(static_cast<uint16_t>(s.redundant_offset), 0u);
    EXPECT_EQ(static_cast<uint16_t>(s.number_of_redundant_streams), 0u);
    EXPECT_EQ(static_cast<uint16_t>(s.timing), 0u);
}

// A count larger than the inline array capacity (corrupt or future-larger blob)
// must never let wire_size() exceed the struct, and clamp_trailer_count() must
// truncate the count to what fits so a serialized descriptor stays self-consistent.
TEST(aem_descriptor_stream, oversized_count_is_clamped_not_overrun)
{
    using namespace aem;
    DescriptorStream s{};
    s.number_of_formats = static_cast<uint16_t>(200);  // > MAX_STREAM_FORMATS (46)

    // wire_size() is clamped to capacity -> never exceeds the struct storage
    // (this is what stops span_store_wire from over-reading &s).
    EXPECT_EQ(s.wire_size(), DescriptorStream::LENGTH + (DescriptorStream::MAX_STREAM_FORMATS * sizeof(Eui64)));
    EXPECT_TRUE(s.wire_size() <= sizeof(DescriptorStream));
    EXPECT_EQ(s.used_stream_formats().size(), DescriptorStream::MAX_STREAM_FORMATS);

    // clamp_trailer_count() makes the count field honest (matches what is served).
    s.clamp_trailer_count();
    EXPECT_EQ(static_cast<uint16_t>(s.number_of_formats), DescriptorStream::MAX_STREAM_FORMATS);

    // A valid in-range count is left untouched.
    DescriptorStream ok{};
    ok.number_of_formats = static_cast<uint16_t>(3);
    ok.clamp_trailer_count();
    EXPECT_EQ(static_cast<uint16_t>(ok.number_of_formats), 3u);
    EXPECT_EQ(ok.wire_size(), DescriptorStream::LENGTH + (3 * sizeof(Eui64)));
}

TEST(aem_descriptor_avb_interface, struct_size_2021)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorAvbInterface), 102u);
    EXPECT_EQ(DescriptorAvbInterface::LENGTH, 102u);
    EXPECT_EQ(DescriptorAvbInterface::MINIMUM_LENGTH, 98u);
    EXPECT_EQ(offsetof(DescriptorAvbInterface, number_of_controls), 98u);
    EXPECT_EQ(offsetof(DescriptorAvbInterface, base_control), 100u);

    DescriptorAvbInterface d{};
    EXPECT_EQ(static_cast<uint16_t>(d.number_of_controls), 0u);
    EXPECT_EQ(static_cast<uint16_t>(d.base_control), 0u);
}

TEST(aem_descriptor_audio_cluster, struct_size_2021)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorAudioCluster), 90u);
    EXPECT_EQ(DescriptorAudioCluster::LENGTH, 90u);
    EXPECT_EQ(DescriptorAudioCluster::MINIMUM_LENGTH, 87u);
    EXPECT_EQ(offsetof(DescriptorAudioCluster, aes3_data_type_reference), 87u);
    EXPECT_EQ(offsetof(DescriptorAudioCluster, aes3_data_type), 88u);

    DescriptorAudioCluster d{};
    EXPECT_EQ(d.aes3_data_type_reference, 0u);
    EXPECT_EQ(static_cast<uint16_t>(d.aes3_data_type), 0u);
}

TEST(aem_descriptor_control_block, struct_size_2021)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorControlBlock), 82u);
    EXPECT_EQ(DescriptorControlBlock::LENGTH, 82u);
    EXPECT_EQ(DescriptorControlBlock::MINIMUM_LENGTH, 76u);
    EXPECT_EQ(offsetof(DescriptorControlBlock, signal_type), 76u);
    EXPECT_EQ(offsetof(DescriptorControlBlock, signal_index), 78u);
    EXPECT_EQ(offsetof(DescriptorControlBlock, signal_output), 80u);

    DescriptorControlBlock d{};
    EXPECT_EQ(static_cast<uint16_t>(d.signal_type), 0u);
    EXPECT_EQ(static_cast<uint16_t>(d.signal_index), 0u);
    EXPECT_EQ(static_cast<uint16_t>(d.signal_output), 0u);
}

TEST(aem_descriptor_signal_transcoder, struct_size_2021)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorSignalTranscoder), 100u);
    EXPECT_EQ(DescriptorSignalTranscoder::LENGTH, 100u);
    EXPECT_EQ(DescriptorSignalTranscoder::MINIMUM_LENGTH, 92u);
    EXPECT_EQ(offsetof(DescriptorSignalTranscoder, transcoder_type), 92u);

    DescriptorSignalTranscoder d{};
    EXPECT_EQ(d.transcoder_type, ieee::Eui64{});
}

TEST(aem_descriptor_timing, struct_layout)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorTiming), 76u);
    EXPECT_EQ(DescriptorTiming::LENGTH, 76u);
    EXPECT_EQ(offsetof(DescriptorTiming, algorithm), 70u);
    EXPECT_EQ(offsetof(DescriptorTiming, ptp_instances_offset), 72u);
    EXPECT_EQ(offsetof(DescriptorTiming, number_of_ptp_instances), 74u);

    DescriptorTiming d{};
    EXPECT_EQ(static_cast<uint16_t>(d.descriptor_type), aem::DESCRIPTOR_TIMING);
}

TEST(aem_descriptor_ptp_instance, struct_layout)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorPtpInstance), 90u);
    EXPECT_EQ(DescriptorPtpInstance::LENGTH, 90u);
    EXPECT_EQ(offsetof(DescriptorPtpInstance, clock_identity), 70u);
    EXPECT_EQ(offsetof(DescriptorPtpInstance, flags), 78u);
    EXPECT_EQ(offsetof(DescriptorPtpInstance, number_of_controls), 82u);
    EXPECT_EQ(offsetof(DescriptorPtpInstance, base_control), 84u);
    EXPECT_EQ(offsetof(DescriptorPtpInstance, number_of_ptp_ports), 86u);
    EXPECT_EQ(offsetof(DescriptorPtpInstance, base_ptp_port), 88u);

    DescriptorPtpInstance d{};
    EXPECT_EQ(static_cast<uint16_t>(d.descriptor_type), aem::DESCRIPTOR_PTP_INSTANCE);
}

TEST(aem_descriptor_ptp_port, struct_layout)
{
    using namespace aem;
    EXPECT_EQ(sizeof(DescriptorPtpPort), 86u);
    EXPECT_EQ(DescriptorPtpPort::LENGTH, 86u);
    EXPECT_EQ(offsetof(DescriptorPtpPort, port_number), 70u);
    EXPECT_EQ(offsetof(DescriptorPtpPort, port_type), 72u);
    EXPECT_EQ(offsetof(DescriptorPtpPort, flags), 74u);
    EXPECT_EQ(offsetof(DescriptorPtpPort, avb_interface_index), 78u);
    EXPECT_EQ(offsetof(DescriptorPtpPort, profile_identifier), 80u);

    DescriptorPtpPort d{};
    EXPECT_EQ(static_cast<uint16_t>(d.descriptor_type), aem::DESCRIPTOR_PTP_PORT);
}

TEST(aem_descriptor_const, memory_object_types)
{
    using namespace aem;
    EXPECT_EQ(MEMORY_OBJECT_TYPE_FIRMWARE_IMAGE, 0x0000);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_VENDOR_SPECIFIC, 0x0001);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_CRASH_DUMP, 0x0002);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_LOG_OBJECT, 0x0003);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_AUTOSTART_SETTINGS, 0x0004);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_SNAPSHOT_SETTINGS, 0x0005);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_SVG_MANUFACTURER, 0x0006);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_SVG_ENTITY, 0x0007);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_SVG_GENERIC, 0x0008);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_PNG_MANUFACTURER, 0x0009);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_PNG_ENTITY, 0x000a);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_PNG_GENERIC, 0x000b);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_DAE_MANUFACTURER, 0x000c);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_DAE_ENTITY, 0x000d);
    EXPECT_EQ(MEMORY_OBJECT_TYPE_DAE_GENERIC, 0x000e);
}

TEST(aem_parse_descriptor, timing_descriptor)
{
    using namespace aem;
    std::array<uint8_t, 76> buf{};
    buf[0] = 0x00;
    buf[1] = 0x26;  // TIMING
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
}

TEST(aem_parse_descriptor, stream_2013_compat)
{
    using namespace aem;
    // A 2013-era device sends a 132-byte stream descriptor
    std::array<uint8_t, 132> buf{};
    buf[0] = 0x00;
    buf[1] = 0x05;  // STREAM_INPUT
    auto result = parse_descriptor(std::span<uint8_t const>(buf));
    EXPECT_TRUE(result.has_value());
}

// ACMPDU format detection
static_assert(is_acmpdu_2013_format(44) == true);
static_assert(is_acmpdu_2013_format(84) == false);
static_assert(is_acmpdu_2021_format(84) == true);
static_assert(is_acmpdu_2021_format(44) == false);

//
// Tests: ACMP Constants
//

TEST(acmp_const, subtype)
{
    EXPECT_EQ(AvtpSubtype::acmp, 0xFC);
}

TEST(acmp_const, message_types)
{
    EXPECT_EQ(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND, 0);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE, 1);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND, 2);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE, 3);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND, 4);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE, 5);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND, 6);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, 7);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND, 8);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE, 9);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND, 10);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE, 11);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND, 12);
    EXPECT_EQ(ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE, 13);
}

TEST(acmp_const, status_codes)
{
    EXPECT_EQ(ACMP_STATUS_SUCCESS, 0);
    EXPECT_EQ(ACMP_STATUS_LISTENER_UNKNOWN_ID, 1);
    EXPECT_EQ(ACMP_STATUS_TALKER_UNKNOWN_ID, 2);
    EXPECT_EQ(ACMP_STATUS_NOT_SUPPORTED, 31);
}

TEST(acmp_const, flags)
{
    EXPECT_EQ(acmp_flags::CLASS_B, 0x0001);
    EXPECT_EQ(acmp_flags::FAST_CONNECT, 0x0002);
    EXPECT_EQ(acmp_flags::SAVED_STATE, 0x0004);
    EXPECT_EQ(acmp_flags::STREAMING_WAIT, 0x0008);
    EXPECT_EQ(acmp_flags::SUPPORTS_ENCRYPTED, 0x0010);
    EXPECT_EQ(acmp_flags::ENCRYPTED_PDU, 0x0020);
    EXPECT_EQ(acmp_flags::TALKER_FAILED, 0x0040);
}

TEST(acmp_const, timeouts)
{
    EXPECT_EQ(ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS, 2000U);
    EXPECT_EQ(ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS, 200U);
    EXPECT_EQ(ACMP_TIMEOUT_CONNECT_RX_COMMAND_MS, 4500U);
}

//
// Tests: ACMP Message Type Names
//

TEST(acmp_names, connect_tx_command)
{
    EXPECT_TRUE(std::string(acmp_message_type_name(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND)) == "Connect TX Command");
}

TEST(acmp_names, connect_rx_response)
{
    EXPECT_TRUE(std::string(acmp_message_type_name(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE)) == "Connect RX Response");
}

TEST(acmp_names, unknown)
{
    EXPECT_TRUE(std::string(acmp_message_type_name(0xFF)) == "Unknown");
}

//
// Tests: ACMP Status Names
//

TEST(acmp_status_names, success)
{
    EXPECT_TRUE(std::string(acmp_status_name(ACMP_STATUS_SUCCESS)) == "Success");
}

TEST(acmp_status_names, not_supported)
{
    EXPECT_TRUE(std::string(acmp_status_name(ACMP_STATUS_NOT_SUPPORTED)) == "Not Supported");
}

//
// Tests: AcmpDu Structure
//

TEST(acmpdu_struct, size)
{
    EXPECT_EQ(sizeof(AcmpDu), 56U);
    EXPECT_EQ(AcmpDu::LENGTH, 56U);
    EXPECT_EQ(AcmpDu::HEADER_LENGTH, 12U);
    EXPECT_EQ(AcmpDu::DATA_LENGTH, 44U);
}

TEST(acmpdu_struct, default_constructor)
{
    AcmpDu acmp;
    EXPECT_EQ(acmp.subtype.get(), 0);
    EXPECT_EQ(acmp.message_type(), 0);
    EXPECT_EQ(acmp.status(), 0);
}

TEST(acmpdu_fields, message_type)
{
    AcmpDu acmp;
    acmp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(acmp.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
}

TEST(acmpdu_fields, status)
{
    AcmpDu acmp;
    acmp.set_status(ACMP_STATUS_TALKER_NO_BANDWIDTH);
    EXPECT_EQ(acmp.status(), ACMP_STATUS_TALKER_NO_BANDWIDTH);
}

TEST(acmpdu_fields, control_data_length)
{
    AcmpDu acmp;
    acmp.set_control_data_length(44);
    EXPECT_EQ(acmp.control_data_length(), 44);

    acmp.set_control_data_length(0x7FF);  // Max 11-bit value
    EXPECT_EQ(acmp.control_data_length(), 0x7FF);
}

TEST(acmpdu_flags, flag_helpers)
{
    AcmpDu acmp;
    acmp.flags = acmp_flags::CLASS_B | acmp_flags::FAST_CONNECT;

    EXPECT_TRUE(acmp.is_class_b());
    EXPECT_TRUE(acmp.is_fast_connect());
    EXPECT_FALSE(acmp.has_saved_state());
    EXPECT_FALSE(acmp.is_streaming_wait());
}

TEST(acmpdu_type, is_command)
{
    AcmpDu acmp;
    acmp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(acmp.is_command());
    EXPECT_FALSE(acmp.is_response());

    acmp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_FALSE(acmp.is_command());
    EXPECT_TRUE(acmp.is_response());
}

TEST(acmpdu_init, init_command)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);

    EXPECT_TRUE(acmp.subtype == AvtpSubtype::acmp);
    EXPECT_EQ(acmp.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(acmp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(acmp.control_data_length(), 44);
}

TEST(acmpdu_init, init_response)
{
    AcmpDu acmp;
    acmp.init_response(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_TALKER_NO_BANDWIDTH);

    EXPECT_TRUE(acmp.subtype == AvtpSubtype::acmp);
    EXPECT_EQ(acmp.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(acmp.status(), ACMP_STATUS_TALKER_NO_BANDWIDTH);
    EXPECT_EQ(acmp.control_data_length(), 44);
}

TEST(acmpdu_serial, roundtrip)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.talker_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    acmp.listener_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    acmp.sequence_id = 1234;

    std::array<uint8_t, 56> buf{};
    auto stored = store_unchecked(buf, acmp);
    EXPECT_EQ(stored, 56U);

    AcmpDu loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 56U);

    EXPECT_EQ(loaded.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(loaded.talker_entity_id == acmp.talker_entity_id);
    EXPECT_TRUE(loaded.listener_entity_id == acmp.listener_entity_id);
    EXPECT_EQ(loaded.sequence_id.get(), 1234);
}

//
// Tests: ADP Constants
//

TEST(adp_const, subtype)
{
    EXPECT_EQ(AvtpSubtype::adp, 0xFA);
}

TEST(adp_const, message_types)
{
    EXPECT_EQ(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE, 0);
    EXPECT_EQ(ADP_MESSAGE_TYPE_ENTITY_DEPARTING, 1);
    EXPECT_EQ(ADP_MESSAGE_TYPE_ENTITY_DISCOVER, 2);
}

TEST(adp_const, entity_capabilities)
{
    EXPECT_EQ(entity_capabilities::EFU_MODE, 0x00000001U);
    EXPECT_EQ(entity_capabilities::AEM_SUPPORTED, 0x00000008U);
    EXPECT_EQ(entity_capabilities::CLASS_A_SUPPORTED, 0x00000100U);
    EXPECT_EQ(entity_capabilities::CLASS_B_SUPPORTED, 0x00000200U);
    EXPECT_EQ(entity_capabilities::GPTP_SUPPORTED, 0x00000400U);
    EXPECT_EQ(entity_capabilities::ENTITY_NOT_READY, 0x00020000U);
    EXPECT_EQ(entity_capabilities::ACMP_ACQUIRE_WITH_AEM, 0x00040000U);
    EXPECT_EQ(entity_capabilities::ACMP_AUTHENTICATE_WITH_AEM, 0x00080000U);
    EXPECT_EQ(entity_capabilities::SUPPORTS_UDPV4_ATDECC, 0x00100000U);
    EXPECT_EQ(entity_capabilities::SUPPORTS_UDPV4_STREAMING, 0x00200000U);
    EXPECT_EQ(entity_capabilities::SUPPORTS_UDPV6_ATDECC, 0x00400000U);
    EXPECT_EQ(entity_capabilities::SUPPORTS_UDPV6_STREAMING, 0x00800000U);
    EXPECT_EQ(entity_capabilities::MULTIPLE_PTP_INSTANCES, 0x01000000U);
    EXPECT_EQ(entity_capabilities::AEM_CONFIGURATION_INDEX_VALID, 0x02000000U);
}

TEST(adp_const, talker_capabilities)
{
    EXPECT_EQ(talker_capabilities::IMPLEMENTED, 0x0001);
    EXPECT_EQ(talker_capabilities::AUDIO_SOURCE, 0x4000);
    EXPECT_EQ(talker_capabilities::VIDEO_SOURCE, 0x8000);
}

TEST(adp_const, listener_capabilities)
{
    EXPECT_EQ(listener_capabilities::IMPLEMENTED, 0x0001);
    EXPECT_EQ(listener_capabilities::AUDIO_SINK, 0x4000);
    EXPECT_EQ(listener_capabilities::VIDEO_SINK, 0x8000);
}

//
// Tests: ADP Message Type Names
//

TEST(adp_names, entity_available)
{
    EXPECT_TRUE(std::string(adp_message_type_name(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE)) == "Entity Available");
}

TEST(adp_names, entity_departing)
{
    EXPECT_TRUE(std::string(adp_message_type_name(ADP_MESSAGE_TYPE_ENTITY_DEPARTING)) == "Entity Departing");
}

TEST(adp_names, entity_discover)
{
    EXPECT_TRUE(std::string(adp_message_type_name(ADP_MESSAGE_TYPE_ENTITY_DISCOVER)) == "Entity Discover");
}

TEST(adp_names, unknown)
{
    EXPECT_TRUE(std::string(adp_message_type_name(0xFF)) == "Unknown");
}

//
// Tests: AdpDu Structure
//

TEST(adpdu_struct, size)
{
    EXPECT_EQ(sizeof(AdpDu), 68U);
    EXPECT_EQ(AdpDu::LENGTH, 68U);
    EXPECT_EQ(AdpDu::HEADER_LENGTH, 12U);
}

TEST(adpdu_struct, default_constructor)
{
    AdpDu adp;
    EXPECT_EQ(adp.subtype.get(), 0);
    EXPECT_EQ(adp.message_type(), 0);
    EXPECT_EQ(adp.valid_time(), 0);
}

TEST(adpdu_fields, message_type)
{
    AdpDu adp;
    adp.set_message_type(ADP_MESSAGE_TYPE_ENTITY_DEPARTING);
    EXPECT_EQ(adp.message_type(), ADP_MESSAGE_TYPE_ENTITY_DEPARTING);
}

TEST(adpdu_fields, valid_time)
{
    AdpDu adp;
    adp.set_valid_time(31);
    EXPECT_EQ(adp.valid_time(), 31);

    adp.set_valid_time(15);
    EXPECT_EQ(adp.valid_time(), 15);
}

TEST(adpdu_fields, control_data_length)
{
    AdpDu adp;
    adp.set_control_data_length(56);
    EXPECT_EQ(adp.control_data_length(), 56);
}

TEST(adpdu_caps, entity_capabilities)
{
    AdpDu adp;
    adp.entity_capabilities = entity_capabilities::AEM_SUPPORTED | entity_capabilities::GPTP_SUPPORTED;

    EXPECT_TRUE(adp.has_entity_capability(entity_capabilities::AEM_SUPPORTED));
    EXPECT_TRUE(adp.has_entity_capability(entity_capabilities::GPTP_SUPPORTED));
    EXPECT_FALSE(adp.has_entity_capability(entity_capabilities::EFU_MODE));
}

TEST(adpdu_caps, talker_capabilities)
{
    AdpDu adp;
    adp.talker_capabilities = talker_capabilities::IMPLEMENTED | talker_capabilities::AUDIO_SOURCE;

    EXPECT_TRUE(adp.has_talker_capability(talker_capabilities::IMPLEMENTED));
    EXPECT_TRUE(adp.has_talker_capability(talker_capabilities::AUDIO_SOURCE));
    EXPECT_FALSE(adp.has_talker_capability(talker_capabilities::VIDEO_SOURCE));
}

TEST(adpdu_caps, listener_capabilities)
{
    AdpDu adp;
    adp.listener_capabilities = listener_capabilities::IMPLEMENTED | listener_capabilities::AUDIO_SINK;

    EXPECT_TRUE(adp.has_listener_capability(listener_capabilities::IMPLEMENTED));
    EXPECT_TRUE(adp.has_listener_capability(listener_capabilities::AUDIO_SINK));
    EXPECT_FALSE(adp.has_listener_capability(listener_capabilities::VIDEO_SINK));
}

TEST(adpdu_type, is_entity_available)
{
    AdpDu adp;
    adp.set_message_type(ADP_MESSAGE_TYPE_ENTITY_AVAILABLE);

    EXPECT_TRUE(adp.is_entity_available());
    EXPECT_FALSE(adp.is_entity_departing());
    EXPECT_FALSE(adp.is_entity_discover());
}

TEST(adpdu_type, is_entity_departing)
{
    AdpDu adp;
    adp.set_message_type(ADP_MESSAGE_TYPE_ENTITY_DEPARTING);

    EXPECT_FALSE(adp.is_entity_available());
    EXPECT_TRUE(adp.is_entity_departing());
    EXPECT_FALSE(adp.is_entity_discover());
}

TEST(adpdu_type, is_entity_discover)
{
    AdpDu adp;
    adp.set_message_type(ADP_MESSAGE_TYPE_ENTITY_DISCOVER);

    EXPECT_FALSE(adp.is_entity_available());
    EXPECT_FALSE(adp.is_entity_departing());
    EXPECT_TRUE(adp.is_entity_discover());
}

TEST(adpdu_init, entity_available)
{
    AdpDu adp;
    Eui64 entity_id(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    adp.init_entity_available(entity_id, 20);

    EXPECT_TRUE(adp.subtype == AvtpSubtype::adp);
    EXPECT_TRUE(adp.is_entity_available());
    EXPECT_EQ(adp.valid_time(), 20);
    EXPECT_EQ(adp.control_data_length(), 56);
    EXPECT_TRUE(adp.entity_id == entity_id);
}

TEST(adpdu_init, entity_departing)
{
    AdpDu adp;
    Eui64 entity_id(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    adp.init_entity_departing(entity_id);

    EXPECT_TRUE(adp.subtype == AvtpSubtype::adp);
    EXPECT_TRUE(adp.is_entity_departing());
    EXPECT_EQ(adp.valid_time(), 0);
    EXPECT_EQ(adp.control_data_length(), 56);
    EXPECT_TRUE(adp.entity_id == entity_id);
}

TEST(adpdu_init, entity_discover)
{
    AdpDu adp;
    adp.init_entity_discover();

    EXPECT_TRUE(adp.subtype == AvtpSubtype::adp);
    EXPECT_TRUE(adp.is_entity_discover());
    EXPECT_EQ(adp.valid_time(), 0);
    EXPECT_EQ(adp.control_data_length(), 56);
}

TEST(adpdu_serial, roundtrip)
{
    AdpDu adp;
    Eui64 entity_id(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    adp.init_entity_available(entity_id, 31);
    adp.entity_capabilities = entity_capabilities::AEM_SUPPORTED | entity_capabilities::GPTP_SUPPORTED;
    adp.talker_capabilities = talker_capabilities::IMPLEMENTED | talker_capabilities::AUDIO_SOURCE;
    adp.talker_stream_sources = 4;

    std::array<uint8_t, 68> buf{};
    auto stored = store_unchecked(buf, adp);
    EXPECT_EQ(stored, 68U);

    AdpDu loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 68U);

    EXPECT_TRUE(loaded.is_entity_available());
    EXPECT_EQ(loaded.valid_time(), 31);
    EXPECT_TRUE(loaded.entity_id == entity_id);
    EXPECT_TRUE(loaded.has_entity_capability(entity_capabilities::AEM_SUPPORTED));
    EXPECT_TRUE(loaded.has_entity_capability(entity_capabilities::GPTP_SUPPORTED));
    EXPECT_TRUE(loaded.has_talker_capability(talker_capabilities::IMPLEMENTED));
    EXPECT_EQ(loaded.talker_stream_sources.get(), 4);
}

//
// Tests: AECP Constants
//

TEST(aecp_const, subtype)
{
    EXPECT_EQ(AvtpSubtype::aecp, 0xFB);
}

TEST(aecp_const, message_types)
{
    EXPECT_EQ(AECP_MESSAGE_TYPE_AEM_COMMAND, 0);
    EXPECT_EQ(AECP_MESSAGE_TYPE_AEM_RESPONSE, 1);
    EXPECT_EQ(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND, 2);
    EXPECT_EQ(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE, 3);
    EXPECT_EQ(AECP_MESSAGE_TYPE_AVC_COMMAND, 4);
    EXPECT_EQ(AECP_MESSAGE_TYPE_AVC_RESPONSE, 5);
    EXPECT_EQ(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND, 6);
    EXPECT_EQ(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE, 7);
    EXPECT_EQ(AECP_MESSAGE_TYPE_HDCP_APM_COMMAND, 8);
    EXPECT_EQ(AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE, 9);
    EXPECT_EQ(AECP_MESSAGE_TYPE_EXTENDED_COMMAND, 14);
    EXPECT_EQ(AECP_MESSAGE_TYPE_EXTENDED_RESPONSE, 15);
}

TEST(aecp_const, status_codes)
{
    EXPECT_EQ(AECP_STATUS_SUCCESS, 0);
    EXPECT_EQ(AECP_STATUS_NOT_IMPLEMENTED, 1);
}

TEST(aecp_const, max_control_data_length)
{
    EXPECT_EQ(AECP_MAX_CONTROL_DATA_LENGTH, 524);
}

//
// Tests: AECP Message Type Names
//

TEST(aecp_names, aem_command)
{
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_AEM_COMMAND)) == "AEM Command");
}

TEST(aecp_names, aem_response)
{
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_AEM_RESPONSE)) == "AEM Response");
}

TEST(aecp_names, vendor_unique_command)
{
    EXPECT_TRUE(std::string(aecp_message_type_name(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND)) == "Vendor Unique Command");
}

TEST(aecp_names, unknown)
{
    EXPECT_TRUE(std::string(aecp_message_type_name(0x10)) == "Unknown");
}

//
// Tests: AECP Status Names
//

TEST(aecp_status_names, success)
{
    EXPECT_TRUE(std::string(aecp_status_name(AECP_STATUS_SUCCESS)) == "Success");
}

TEST(aecp_status_names, not_implemented)
{
    EXPECT_TRUE(std::string(aecp_status_name(AECP_STATUS_NOT_IMPLEMENTED)) == "Not Implemented");
}

TEST(aecp_status_names, unknown)
{
    EXPECT_TRUE(std::string(aecp_status_name(0xFF)) == "Unknown");
}

//
// Tests: AecpDuCommon Structure
//

TEST(aecpdu_struct, size)
{
    EXPECT_EQ(sizeof(AecpDuCommon), 22U);
    EXPECT_EQ(AecpDuCommon::LENGTH, 22U);
    EXPECT_EQ(AecpDuCommon::HEADER_LENGTH, 12U);
    EXPECT_EQ(AecpDuCommon::COMMON_DATA_LENGTH, 10U);
}

TEST(aecpdu_struct, default_constructor)
{
    AecpDuCommon aecp{};
    EXPECT_EQ(aecp.subtype.get(), 0);
    EXPECT_EQ(aecp.message_type(), 0);
    EXPECT_EQ(aecp.status(), 0);
    EXPECT_FALSE(aecp.sv());
    EXPECT_EQ(aecp.version(), 0);
}

TEST(aecpdu_fields, message_type)
{
    AecpDuCommon aecp{};
    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(aecp.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);

    aecp.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    EXPECT_EQ(aecp.message_type(), AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
}

TEST(aecpdu_fields, status)
{
    AecpDuCommon aecp{};
    aecp.set_status(AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aecp.status(), AECP_STATUS_NOT_IMPLEMENTED);

    aecp.set_status(31);  // Max 5-bit value
    EXPECT_EQ(aecp.status(), 31);
}

TEST(aecpdu_fields, control_data_length)
{
    AecpDuCommon aecp{};
    aecp.set_control_data_length(100);
    EXPECT_EQ(aecp.control_data_length(), 100);

    aecp.set_control_data_length(0x7FF);  // Max 11-bit value
    EXPECT_EQ(aecp.control_data_length(), 0x7FF);
}

TEST(aecpdu_type, is_command)
{
    AecpDuCommon aecp{};
    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_TRUE(aecp.is_command());
    EXPECT_FALSE(aecp.is_response());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_FALSE(aecp.is_command());
    EXPECT_TRUE(aecp.is_response());
}

TEST(aecpdu_type, is_aem)
{
    AecpDuCommon aecp{};
    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_TRUE(aecp.is_aem());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_TRUE(aecp.is_aem());

    aecp.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    EXPECT_FALSE(aecp.is_aem());
}

TEST(aecpdu_type, is_address_access)
{
    AecpDuCommon aecp{};
    aecp.set_message_type(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_COMMAND);
    EXPECT_TRUE(aecp.is_address_access());

    aecp.set_message_type(AECP_MESSAGE_TYPE_ADDRESS_ACCESS_RESPONSE);
    EXPECT_TRUE(aecp.is_address_access());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_FALSE(aecp.is_address_access());
}

TEST(aecpdu_type, is_vendor_unique)
{
    AecpDuCommon aecp{};
    aecp.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);
    EXPECT_TRUE(aecp.is_vendor_unique());

    aecp.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_RESPONSE);
    EXPECT_TRUE(aecp.is_vendor_unique());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_FALSE(aecp.is_vendor_unique());
}

TEST(aecpdu_init, init_command)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 100);

    EXPECT_TRUE(aecp.subtype == AvtpSubtype::aecp);
    EXPECT_EQ(aecp.message_type(), AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_EQ(aecp.status(), AECP_STATUS_SUCCESS);
    EXPECT_EQ(aecp.control_data_length(), 100);
}

TEST(aecpdu_init, init_response)
{
    AecpDuCommon aecp{};
    aecp.init_response(AECP_MESSAGE_TYPE_AEM_RESPONSE, AECP_STATUS_NOT_IMPLEMENTED, 50);

    EXPECT_TRUE(aecp.subtype == AvtpSubtype::aecp);
    EXPECT_EQ(aecp.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(aecp.status(), AECP_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(aecp.control_data_length(), 50);
}

TEST(aecpdu_serial, roundtrip)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 10);
    aecp.target_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    aecp.controller_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    aecp.sequence_id = 1234;

    std::array<uint8_t, 22> buf{};
    auto stored = store_unchecked(buf, aecp);
    EXPECT_EQ(stored, 22U);

    AecpDuCommon loaded{};
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 22U);

    EXPECT_EQ(loaded.message_type(), AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_TRUE(loaded.target_entity_id == aecp.target_entity_id);
    EXPECT_TRUE(loaded.controller_entity_id == aecp.controller_entity_id);
    EXPECT_EQ(loaded.sequence_id.get(), 1234);
}

//
// Tests: AEM Constants
//

TEST(aem_const, status_codes)
{
    EXPECT_EQ(AEM_STATUS_SUCCESS, 0);
    EXPECT_EQ(AEM_STATUS_NOT_IMPLEMENTED, 1);
    EXPECT_EQ(AEM_STATUS_NO_SUCH_DESCRIPTOR, 2);
    EXPECT_EQ(AEM_STATUS_ENTITY_LOCKED, 3);
    EXPECT_EQ(AEM_STATUS_ENTITY_ACQUIRED, 4);
    EXPECT_EQ(AEM_STATUS_NOT_AUTHENTICATED, 5);
    EXPECT_EQ(AEM_STATUS_BAD_ARGUMENTS, 7);
    EXPECT_EQ(AEM_STATUS_NO_RESOURCES, 8);
    EXPECT_EQ(AEM_STATUS_IN_PROGRESS, 9);
    EXPECT_EQ(AEM_STATUS_ENTITY_MISBEHAVING, 10);
    EXPECT_EQ(AEM_STATUS_NOT_SUPPORTED, 11);
    EXPECT_EQ(AEM_STATUS_STREAM_IS_RUNNING, 12);
}

TEST(aem_const, timeouts)
{
    EXPECT_EQ(AEM_TIMEOUT_MS, 250U);
    EXPECT_EQ(AEM_IN_PROGRESS_TIMEOUT_MS, 120U);
    EXPECT_EQ(AEM_LOCK_TIMEOUT_MS, 60000U);
}

TEST(aem_const, command_codes)
{
    EXPECT_EQ(AEM_COMMAND_ACQUIRE_ENTITY, 0x0000);
    EXPECT_EQ(AEM_COMMAND_LOCK_ENTITY, 0x0001);
    EXPECT_EQ(AEM_COMMAND_ENTITY_AVAILABLE, 0x0002);
    EXPECT_EQ(AEM_COMMAND_READ_DESCRIPTOR, 0x0004);
    EXPECT_EQ(AEM_COMMAND_SET_STREAM_FORMAT, 0x0008);
    EXPECT_EQ(AEM_COMMAND_GET_STREAM_FORMAT, 0x0009);
    EXPECT_EQ(AEM_COMMAND_START_STREAMING, 0x0022);
    EXPECT_EQ(AEM_COMMAND_STOP_STREAMING, 0x0023);
    EXPECT_EQ(AEM_COMMAND_GET_COUNTERS, 0x0029);
    EXPECT_EQ(AEM_COMMAND_REBOOT, 0x002A);
    EXPECT_EQ(AEM_COMMAND_EXPANSION, 0x3FFF);
}

TEST(aem_const, cr_bit_accessor)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, AemDu::AEM_DATA_LENGTH);

    // CR bit should be clear after init
    EXPECT_FALSE(aem.is_controller_request());

    // Set CR bit
    aem.set_controller_request(true);
    EXPECT_TRUE(aem.is_controller_request());

    // Command code should be unaffected by CR bit
    EXPECT_EQ(aem.command_code(), AEM_COMMAND_READ_DESCRIPTOR);

    // U bit should still be clear
    EXPECT_FALSE(aem.is_unsolicited());

    // Clear CR bit
    aem.set_controller_request(false);
    EXPECT_FALSE(aem.is_controller_request());
    EXPECT_EQ(aem.command_code(), AEM_COMMAND_READ_DESCRIPTOR);
}

TEST(aem_const, command_code_14bit)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_EXPANSION, AemDu::AEM_DATA_LENGTH);

    // 14-bit max value
    EXPECT_EQ(aem.command_code(), 0x3FFF);

    // Setting U and CR bits should not affect command_code
    aem.set_unsolicited(true);
    aem.set_controller_request(true);
    EXPECT_EQ(aem.command_code(), 0x3FFF);
}

//
// Tests: AEM Command Tracker
//

TEST(aem_command_tracker, basic_flow)
{
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};

    AemCommandTracker tracker;
    tracker.start(42, AEM_COMMAND_READ_DESCRIPTOR, now);
    EXPECT_TRUE(tracker.is_active());
    EXPECT_EQ(tracker.sequence_id(), 42U);
    EXPECT_FALSE(tracker.check_timeout(now));

    // Final response
    auto result = tracker.receive_response(AEM_STATUS_SUCCESS, 42, now);
    EXPECT_EQ(result, AemCommandTracker::Result::Complete);
    EXPECT_FALSE(tracker.is_active());
}

TEST(aem_command_tracker, in_progress_extends_deadline)
{
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};

    AemCommandTracker tracker;
    tracker.start(1, AEM_COMMAND_ACQUIRE_ENTITY, now);

    // Advance to near timeout
    auto near_timeout = now + std::chrono::milliseconds(240);
    EXPECT_FALSE(tracker.check_timeout(near_timeout));

    // Receive IN_PROGRESS - should extend deadline
    auto result = tracker.receive_response(AEM_STATUS_IN_PROGRESS, 1, near_timeout);
    EXPECT_EQ(result, AemCommandTracker::Result::Pending);
    EXPECT_TRUE(tracker.is_active());

    // Original deadline would have passed, but IN_PROGRESS extended it
    auto past_original = now + std::chrono::milliseconds(260);
    EXPECT_FALSE(tracker.check_timeout(past_original));

    // New deadline is near_timeout + 250ms
    auto past_new = near_timeout + std::chrono::milliseconds(260);
    EXPECT_TRUE(tracker.check_timeout(past_new));
}

TEST(aem_command_tracker, timeout)
{
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};

    AemCommandTracker tracker;
    tracker.start(1, AEM_COMMAND_READ_DESCRIPTOR, now);

    auto past_deadline = now + std::chrono::milliseconds(260);
    EXPECT_TRUE(tracker.check_timeout(past_deadline));
}

TEST(aem_pending_command, basic_flow)
{
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};

    AemDu header{};
    header.init_command(AEM_COMMAND_ACQUIRE_ENTITY, AemDu::AEM_DATA_LENGTH);
    header.sequence_id = 42;

    PendingAemCommand pending;
    pending.start(header, now);
    EXPECT_TRUE(pending.active);
    EXPECT_EQ(pending.sequence_id, 42U);
    EXPECT_EQ(pending.command_code, AEM_COMMAND_ACQUIRE_ENTITY);

    // Not yet time for IN_PROGRESS
    EXPECT_FALSE(pending.needs_in_progress(now));

    // After 120ms, need to send IN_PROGRESS
    auto after_120ms = now + std::chrono::milliseconds(125);
    EXPECT_TRUE(pending.needs_in_progress(after_120ms));

    pending.sent_in_progress(after_120ms);
    EXPECT_FALSE(pending.needs_in_progress(after_120ms));

    // After another 120ms
    auto after_240ms = after_120ms + std::chrono::milliseconds(125);
    EXPECT_TRUE(pending.needs_in_progress(after_240ms));

    pending.complete();
    EXPECT_FALSE(pending.active);
}

//
// Tests: AEM Status Names
//

TEST(aem_status_names, success)
{
    EXPECT_TRUE(std::string(aem_status_name(AEM_STATUS_SUCCESS)) == "Success");
}

TEST(aem_status_names, not_implemented)
{
    EXPECT_TRUE(std::string(aem_status_name(AEM_STATUS_NOT_IMPLEMENTED)) == "Not Implemented");
}

TEST(aem_status_names, no_such_descriptor)
{
    EXPECT_TRUE(std::string(aem_status_name(AEM_STATUS_NO_SUCH_DESCRIPTOR)) == "No Such Descriptor");
}

TEST(aem_status_names, entity_locked)
{
    EXPECT_TRUE(std::string(aem_status_name(AEM_STATUS_ENTITY_LOCKED)) == "Entity Locked");
}

TEST(aem_status_names, stream_is_running)
{
    EXPECT_TRUE(std::string(aem_status_name(AEM_STATUS_STREAM_IS_RUNNING)) == "Stream Is Running");
}

TEST(aem_status_names, unknown)
{
    EXPECT_TRUE(std::string(aem_status_name(0xFF)) == "Unknown");
}

//
// Tests: AEM Command Names
//

TEST(aem_cmd_names, acquire_entity)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_ACQUIRE_ENTITY)) == "ACQUIRE_ENTITY");
}

TEST(aem_cmd_names, read_descriptor)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_READ_DESCRIPTOR)) == "READ_DESCRIPTOR");
}

TEST(aem_cmd_names, start_streaming)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_START_STREAMING)) == "START_STREAMING");
}

TEST(aem_cmd_names, reboot)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_REBOOT)) == "REBOOT");
}

TEST(aem_cmd_names, auth_get_nonce)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_AUTH_GET_NONCE)) == "AUTH_GET_NONCE");
}

TEST(aem_cmd_names, auth_add_key_nonce)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_AUTH_ADD_KEY_NONCE)) == "AUTH_ADD_KEY_NONCE");
}

TEST(aem_cmd_names, expansion)
{
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_EXPANSION)) == "EXPANSION");
}

TEST(aem_cmd_names, unknown)
{
    EXPECT_TRUE(std::string(aem_command_name(0x3FFE)) == "Unknown");
}

TEST(aem_cmd_names, with_u_bit)
{
    // Command code with U bit set should still be recognized
    EXPECT_TRUE(std::string(aem_command_name(AEM_COMMAND_READ_DESCRIPTOR | 0x8000)) == "READ_DESCRIPTOR");
}

TEST(aem_names, command_name_2021_ptp)
{
    // 2021 PTP commands
    EXPECT_EQ(std::string_view(aem_command_name(0x004B)), "GET_DYNAMIC_INFO");
    EXPECT_EQ(std::string_view(aem_command_name(0x0050)), "SET_PTP_INSTANCE_INFO");
    EXPECT_EQ(std::string_view(aem_command_name(0x005F)), "GET_PTP_PORT_INFO");
    EXPECT_EQ(std::string_view(aem_command_name(0x0066)), "GET_PATH_LATENCY");
}

//
// Tests: AemDu Structure
//

TEST(aemdu_struct, size)
{
    EXPECT_EQ(sizeof(AemDu), 24U);
    EXPECT_EQ(AemDu::LENGTH, 24U);
    EXPECT_EQ(AemDu::HEADER_LENGTH, 12U);
    EXPECT_EQ(AemDu::AEM_DATA_LENGTH, 12U);
}

TEST(aemdu_struct, default_constructor)
{
    AemDu aem{};
    EXPECT_EQ(aem.subtype.get(), 0);
    EXPECT_EQ(aem.message_type(), 0);
    EXPECT_EQ(aem.status(), 0);
    EXPECT_EQ(aem.command_code(), 0);
    EXPECT_FALSE(aem.is_unsolicited());
}

TEST(aemdu_fields, message_type)
{
    AemDu aem{};
    aem.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(aem.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);
}

TEST(aemdu_fields, status)
{
    AemDu aem{};
    aem.set_status(AEM_STATUS_NO_RESOURCES);
    EXPECT_EQ(aem.status(), AEM_STATUS_NO_RESOURCES);
}

TEST(aemdu_fields, control_data_length)
{
    AemDu aem{};
    aem.set_control_data_length(200);
    EXPECT_EQ(aem.control_data_length(), 200);
}

TEST(aemdu_fields, command_code)
{
    AemDu aem{};
    aem.set_command_code(AEM_COMMAND_GET_STREAM_FORMAT);
    EXPECT_EQ(aem.command_code(), AEM_COMMAND_GET_STREAM_FORMAT);
}

TEST(aemdu_fields, unsolicited)
{
    AemDu aem{};
    EXPECT_FALSE(aem.is_unsolicited());

    aem.set_unsolicited(true);
    EXPECT_TRUE(aem.is_unsolicited());

    aem.set_unsolicited(false);
    EXPECT_FALSE(aem.is_unsolicited());
}

TEST(aemdu_type, is_command)
{
    AemDu aem{};
    aem.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_TRUE(aem.is_command());
    EXPECT_FALSE(aem.is_response());
}

TEST(aemdu_type, is_response)
{
    AemDu aem{};
    aem.set_message_type(AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_FALSE(aem.is_command());
    EXPECT_TRUE(aem.is_response());
}

TEST(aemdu_init, init_command)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);

    EXPECT_TRUE(aem.subtype == AvtpSubtype::aecp);
    EXPECT_EQ(aem.message_type(), AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_EQ(aem.status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(aem.control_data_length(), 12);
    EXPECT_EQ(aem.command_code(), AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_FALSE(aem.is_unsolicited());
}

TEST(aemdu_init, init_response)
{
    AemDu aem{};
    aem.init_response(AEM_COMMAND_READ_DESCRIPTOR, AEM_STATUS_SUCCESS, 100);

    EXPECT_TRUE(aem.subtype == AvtpSubtype::aecp);
    EXPECT_EQ(aem.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(aem.status(), AEM_STATUS_SUCCESS);
    EXPECT_EQ(aem.control_data_length(), 100);
    EXPECT_EQ(aem.command_code(), AEM_COMMAND_READ_DESCRIPTOR);
    EXPECT_FALSE(aem.is_unsolicited());
}

TEST(aemdu_init, init_response_unsolicited)
{
    AemDu aem{};
    aem.init_response(AEM_COMMAND_GET_COUNTERS, AEM_STATUS_SUCCESS, 50, true);

    EXPECT_EQ(aem.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(aem.command_code(), AEM_COMMAND_GET_COUNTERS);
    EXPECT_TRUE(aem.is_unsolicited());
}

TEST(aemdu_serial, roundtrip)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_START_STREAMING, 12);
    aem.target_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    aem.controller_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    aem.sequence_id = 5678;

    std::array<uint8_t, 24> buf{};
    auto stored = store_unchecked(buf, aem);
    EXPECT_EQ(stored, 24U);

    AemDu loaded{};
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 24U);

    EXPECT_EQ(loaded.message_type(), AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_EQ(loaded.command_code(), AEM_COMMAND_START_STREAMING);
    EXPECT_TRUE(loaded.target_entity_id == aem.target_entity_id);
    EXPECT_TRUE(loaded.controller_entity_id == aem.controller_entity_id);
    EXPECT_EQ(loaded.sequence_id.get(), 5678);
}

TEST(aemdu_serial, roundtrip_with_unsolicited)
{
    AemDu aem{};
    aem.init_response(AEM_COMMAND_GET_STREAM_INFO, AEM_STATUS_SUCCESS, 40, true);
    aem.target_entity_id = Eui64(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0);
    aem.sequence_id = 9999;

    std::array<uint8_t, 24> buf{};
    auto stored = store_unchecked(buf, aem);
    EXPECT_EQ(stored, 24U);

    AemDu loaded{};
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 24U);

    EXPECT_EQ(loaded.message_type(), AECP_MESSAGE_TYPE_AEM_RESPONSE);
    EXPECT_EQ(loaded.command_code(), AEM_COMMAND_GET_STREAM_INFO);
    EXPECT_TRUE(loaded.is_unsolicited());
    EXPECT_EQ(loaded.sequence_id.get(), 9999);
}

TEST(aemdu_comparison, equality)
{
    AemDu aem1{};
    aem1.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);
    aem1.target_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    aem1.sequence_id = 1;

    AemDu aem2{};
    aem2.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);
    aem2.target_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    aem2.sequence_id = 1;

    EXPECT_TRUE(aem1 == aem2);

    aem2.sequence_id = 2;
    EXPECT_FALSE(aem1 == aem2);
}

//
// Tests: AcmpDu Validation
//

TEST(acmpdu_valid, valid_pdu)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(acmp.is_valid());
}

TEST(acmpdu_valid, invalid_subtype)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.subtype = 0x00;  // Wrong subtype
    EXPECT_FALSE(acmp.is_valid());
}

TEST(acmpdu_valid, control_data_length_too_small)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.set_control_data_length(AcmpDu::DATA_LENGTH - 1);
    EXPECT_FALSE(acmp.is_valid());
}

TEST(acmpdu_valid, control_data_length_larger_accepted)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.set_control_data_length(AcmpDu::DATA_LENGTH + 40);
    EXPECT_TRUE(acmp.is_valid());
}

TEST(acmpdu_valid, invalid_message_type)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.set_message_type(14);  // Invalid message type (max is 13)
    EXPECT_FALSE(acmp.is_valid());
}

//
// Tests: AcmpDu2021 Structure (Extended ACMP)
//

TEST(acmpdu2021_struct, size)
{
    EXPECT_EQ(sizeof(AcmpDu2021), 96U);
    EXPECT_EQ(AcmpDu2021::LENGTH, 96U);
    EXPECT_EQ(AcmpDu2021::HEADER_LENGTH, 12U);
    EXPECT_EQ(AcmpDu2021::DATA_LENGTH, 84U);
}

TEST(acmpdu2021_struct, default_constructor)
{
    AcmpDu2021 acmp;
    EXPECT_EQ(acmp.subtype.get(), 0);
    EXPECT_EQ(acmp.message_type(), 0);
    EXPECT_EQ(acmp.status(), 0);
}

TEST(acmpdu2021_fields, message_type)
{
    AcmpDu2021 acmp;
    acmp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(acmp.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
}

TEST(acmpdu2021_fields, status)
{
    AcmpDu2021 acmp;
    acmp.set_status(ACMP_STATUS_TALKER_NO_BANDWIDTH);
    EXPECT_EQ(acmp.status(), ACMP_STATUS_TALKER_NO_BANDWIDTH);
}

TEST(acmpdu2021_fields, control_data_length)
{
    AcmpDu2021 acmp;
    acmp.set_control_data_length(84);
    EXPECT_EQ(acmp.control_data_length(), 84);

    acmp.set_control_data_length(0x7FF);  // Max 11-bit value
    EXPECT_EQ(acmp.control_data_length(), 0x7FF);
}

TEST(acmpdu2021_fields, sv_version)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.sv());
    EXPECT_EQ(acmp.version(), 0);
}

TEST(acmpdu2021_flags, flag_helpers)
{
    AcmpDu2021 acmp;
    acmp.flags = acmp_flags::CLASS_B | acmp_flags::FAST_CONNECT | acmp_flags::UDP;

    EXPECT_TRUE(acmp.is_class_b());
    EXPECT_TRUE(acmp.is_fast_connect());
    EXPECT_TRUE(acmp.is_udp());
    EXPECT_FALSE(acmp.has_saved_state());
    EXPECT_FALSE(acmp.is_streaming_wait());
    EXPECT_FALSE(acmp.supports_encrypted());
    EXPECT_FALSE(acmp.is_encrypted_pdu());
    EXPECT_FALSE(acmp.is_talker_failed());
    EXPECT_FALSE(acmp.is_srp_registration_failed());
    EXPECT_FALSE(acmp.is_cl_entries_valid());
    EXPECT_FALSE(acmp.is_no_srp());
}

TEST(acmpdu2021_flags, all_flags)
{
    AcmpDu2021 acmp;
    acmp.flags = acmp_flags::SAVED_STATE | acmp_flags::STREAMING_WAIT | acmp_flags::SUPPORTS_ENCRYPTED | acmp_flags::ENCRYPTED_PDU |
        acmp_flags::SRP_REGISTRATION_FAILED | acmp_flags::CL_ENTRIES_VALID | acmp_flags::NO_SRP;

    EXPECT_TRUE(acmp.has_saved_state());
    EXPECT_TRUE(acmp.is_streaming_wait());
    EXPECT_TRUE(acmp.supports_encrypted());
    EXPECT_TRUE(acmp.is_encrypted_pdu());
    EXPECT_TRUE(acmp.is_talker_failed());
    EXPECT_TRUE(acmp.is_srp_registration_failed());
    EXPECT_TRUE(acmp.is_cl_entries_valid());
    EXPECT_TRUE(acmp.is_no_srp());
}

TEST(acmpdu2021_type, is_command)
{
    AcmpDu2021 acmp;
    acmp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(acmp.is_command());
    EXPECT_FALSE(acmp.is_response());

    acmp.set_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE);
    EXPECT_FALSE(acmp.is_command());
    EXPECT_TRUE(acmp.is_response());
}

TEST(acmpdu2021_init, init_command)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);

    EXPECT_TRUE(acmp.subtype == AvtpSubtype::acmp);
    EXPECT_EQ(acmp.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(acmp.status(), ACMP_STATUS_SUCCESS);
    EXPECT_EQ(acmp.control_data_length(), 84);
}

TEST(acmpdu2021_init, init_response)
{
    AcmpDu2021 acmp;
    acmp.init_response(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_TALKER_NO_BANDWIDTH);

    EXPECT_TRUE(acmp.subtype == AvtpSubtype::acmp);
    EXPECT_EQ(acmp.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_EQ(acmp.status(), ACMP_STATUS_TALKER_NO_BANDWIDTH);
    EXPECT_EQ(acmp.control_data_length(), 84);
}

TEST(acmpdu2021_valid, valid_pdu)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(acmp.is_valid());
}

TEST(acmpdu2021_valid, invalid_subtype)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.subtype = 0x00;  // Wrong subtype
    EXPECT_FALSE(acmp.is_valid());
}

TEST(acmpdu2021_valid, control_data_length_too_small)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.set_control_data_length(AcmpDu2021::DATA_LENGTH - 1);
    EXPECT_FALSE(acmp.is_valid());
}

TEST(acmpdu2021_valid, control_data_length_larger_accepted)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.set_control_data_length(AcmpDu2021::DATA_LENGTH + 20);
    EXPECT_TRUE(acmp.is_valid());
}

TEST(acmpdu2021_valid, invalid_message_type)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.set_message_type(14);  // Invalid message type (max is 13)
    EXPECT_FALSE(acmp.is_valid());
}

TEST(acmpdu2021_serial, roundtrip)
{
    AcmpDu2021 acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp.talker_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    acmp.listener_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    acmp.sequence_id = 1234;
    acmp.source_port = 5000;
    acmp.destination_port = 5001;
    acmp.flags = acmp_flags::UDP;

    std::array<uint8_t, 96> buf{};
    auto stored = store_unchecked(buf, acmp);
    EXPECT_EQ(stored, 96U);

    AcmpDu2021 loaded;
    auto loaded_size = load_unchecked(buf, &loaded);
    EXPECT_EQ(loaded_size, 96U);

    EXPECT_EQ(loaded.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(loaded.talker_entity_id == acmp.talker_entity_id);
    EXPECT_TRUE(loaded.listener_entity_id == acmp.listener_entity_id);
    EXPECT_EQ(loaded.sequence_id.get(), 1234);
    EXPECT_EQ(loaded.source_port.get(), 5000);
    EXPECT_EQ(loaded.destination_port.get(), 5001);
    EXPECT_TRUE(loaded.is_udp());
}

TEST(acmpdu2021_comparison, equality)
{
    AcmpDu2021 acmp1;
    acmp1.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp1.sequence_id = 1;

    AcmpDu2021 acmp2;
    acmp2.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    acmp2.sequence_id = 1;

    EXPECT_TRUE(acmp1 == acmp2);

    acmp2.sequence_id = 2;
    EXPECT_FALSE(acmp1 == acmp2);
}

//
// Tests: ACMP Format Detection
//

TEST(acmp_format, detect_2013)
{
    EXPECT_TRUE(is_acmpdu_2013_format(44));
    EXPECT_FALSE(is_acmpdu_2013_format(84));
    EXPECT_FALSE(is_acmpdu_2013_format(0));
}

TEST(acmp_format, detect_2021)
{
    EXPECT_TRUE(is_acmpdu_2021_format(84));
    EXPECT_FALSE(is_acmpdu_2021_format(44));
    EXPECT_FALSE(is_acmpdu_2021_format(0));
}

//
// Tests: ACMP Command Response Conversion
//

TEST(acmp_convert, from_pdu)
{
    AcmpDu pdu;
    pdu.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    pdu.talker_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    pdu.listener_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    pdu.sequence_id = 42;
    pdu.flags = acmp_flags::CLASS_B;

    AcmpCommandResponse resp = acmp_command_response_from_pdu(pdu);

    EXPECT_TRUE(resp.talker_entity_id == pdu.talker_entity_id);
    EXPECT_TRUE(resp.listener_entity_id == pdu.listener_entity_id);
    EXPECT_EQ(resp.sequence_id.get(), 42);
    EXPECT_TRUE(resp.is_class_b());
    // Extended fields should be zero
    EXPECT_EQ(resp.source_port.get(), 0);
    EXPECT_EQ(resp.destination_port.get(), 0);
}

TEST(acmp_convert, to_pdu)
{
    AcmpCommandResponse resp;
    resp.init_command(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    resp.talker_entity_id = Eui64(0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0);
    resp.sequence_id = 99;
    resp.source_port = 5000;  // Extended field - won't be copied

    AcmpDu pdu;
    acmp_command_response_to_pdu(resp, pdu);

    EXPECT_TRUE(pdu.talker_entity_id == resp.talker_entity_id);
    EXPECT_EQ(pdu.sequence_id.get(), 99);
}

//
// Tests: acmp_serialize_2016 -- L2 sends use the pre-2021 56-byte short form
//

TEST(acmp_l2_serialize, non_udp_emits_short_form)
{
    AcmpCommandResponse resp;
    resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    resp.talker_entity_id = Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77);
    resp.listener_unique_id = 1;
    resp.sequence_id = 7;

    std::array<uint8_t, AcmpDu2021::LENGTH> buf{};
    auto wire = acmp_serialize_2016(resp, buf);

    // 1722.1-2013 short form: 56 bytes total, control_data_length field == 44.
    EXPECT_EQ(wire.size(), AcmpDu::LENGTH);
    AcmpDu got{};
    (void)protocol::load_unchecked(wire, &got);
    EXPECT_EQ(got.control_data_length(), AcmpDu::DATA_LENGTH);
    EXPECT_EQ(got.message_type(), ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_TRUE(got.talker_entity_id == resp.talker_entity_id);
    EXPECT_EQ(got.listener_unique_id.get(), 1);
    EXPECT_EQ(got.sequence_id.get(), 7);
}

TEST(acmp_l2_serialize, udp_emits_extended_form)
{
    AcmpCommandResponse resp;
    resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    resp.flags = acmp_flags::UDP;
    resp.source_port = 5000;

    std::array<uint8_t, AcmpDu2021::LENGTH> buf{};
    auto wire = acmp_serialize_2016(resp, buf);

    // UDP-encapsulated ACMP keeps the 96-byte extended form with cdl == 84.
    EXPECT_EQ(wire.size(), AcmpDu2021::LENGTH);
    AcmpDu2021 got{};
    (void)protocol::load_unchecked(wire, &got);
    EXPECT_EQ(got.control_data_length(), AcmpDu2021::DATA_LENGTH);
    EXPECT_EQ(got.source_port.get(), 5000);
}

// The decisive serialization-correctness test: every ACMP field must land at its
// EXACT IEEE 1722.1-2013 wire offset. Distinct, recognizable bytes per field so a
// swap or shift is caught -- crucially, the round-trip tests above CANNOT catch a
// layout bug that is symmetric across serialize+deserialize (the struct would
// round-trip a wrong layout to itself). This pins the byte positions against the
// spec: stream_id@4, controller@12, talker@20, listener@28, ... (the very offsets
// that are easy to misread by hand -- e.g. forgetting the 8-byte stream_id and
// mistaking controller_entity_id for talker_entity_id).
TEST(acmp_serialize_2016, wire_field_offsets)
{
    AcmpCommandResponse resp{};
    resp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    resp.stream_id = Eui64(0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57);
    resp.controller_entity_id = Eui64(0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7);
    resp.talker_entity_id = Eui64(0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81);
    resp.listener_entity_id = Eui64(0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21);
    resp.talker_unique_id = 0x0102;
    resp.listener_unique_id = 0x0304;
    resp.stream_dest_mac = Eui48(0x91, 0xE0, 0xF0, 0x00, 0x21, 0xE6);
    resp.connection_count = 0x0005;
    resp.sequence_id = 0x0006;
    resp.stream_vlan_id = 0x0002;

    std::array<uint8_t, AcmpDu2021::LENGTH> buf{};
    auto const wire = acmp_serialize_2016(resp, buf);
    EXPECT_EQ(wire.size(), AcmpDu::LENGTH);  // 56-byte 2013/2016 short form

    // Common AVTP control header.
    EXPECT_EQ(wire[0], static_cast<uint8_t>(AvtpSubtype::acmp));        // subtype @0
    EXPECT_EQ(wire[1] & 0x0F, ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);    // message_type @1
    EXPECT_EQ(((wire[2] & 0x07) << 8) | wire[3], AcmpDu::DATA_LENGTH);  // control_data_length @2-3 == 44

    auto const field_at = [&](size_t off, std::span<uint8_t const> bytes) {
        for (size_t i = 0; i < bytes.size(); ++i) {
            EXPECT_EQ(wire[off + i], bytes[i]);
        }
    };
    field_at(4, resp.stream_id.span());              // stream_id            @4-11
    field_at(12, resp.controller_entity_id.span());  // controller_entity_id @12-19
    field_at(20, resp.talker_entity_id.span());      // talker_entity_id     @20-27
    field_at(28, resp.listener_entity_id.span());    // listener_entity_id   @28-35
    EXPECT_EQ(wire[36], 0x01);                       // talker_unique_id     @36-37 (network order)
    EXPECT_EQ(wire[37], 0x02);
    EXPECT_EQ(wire[38], 0x03);  // listener_unique_id   @38-39
    EXPECT_EQ(wire[39], 0x04);
    field_at(40, resp.stream_dest_mac.span());  // stream_dest_mac      @40-45
    EXPECT_EQ(wire[46], 0x00);                  // connection_count     @46-47
    EXPECT_EQ(wire[47], 0x05);
    EXPECT_EQ(wire[48], 0x00);  // sequence_id          @48-49
    EXPECT_EQ(wire[49], 0x06);
    EXPECT_EQ(wire[52], 0x00);  // stream_vlan_id       @52-53
    EXPECT_EQ(wire[53], 0x02);
}

// Round-trip the full field set through serialize -> parse, independent of the
// byte-offset assertions above, so a regression in either path shows.
TEST(acmp_serialize_2016, roundtrip_all_fields)
{
    AcmpCommandResponse resp{};
    resp.init_response(ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE, ACMP_STATUS_SUCCESS);
    resp.stream_id = Eui64(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    resp.controller_entity_id = Eui64(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11);
    resp.talker_entity_id = Eui64(0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17);
    resp.listener_entity_id = Eui64(0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27);
    resp.talker_unique_id = 3;
    resp.listener_unique_id = 4;
    resp.stream_dest_mac = Eui48(0x91, 0xE0, 0xF0, 0x00, 0x12, 0x34);
    resp.connection_count = 1;
    resp.sequence_id = 99;
    resp.stream_vlan_id = 2;

    std::array<uint8_t, AcmpDu2021::LENGTH> buf{};
    auto const wire = acmp_serialize_2016(resp, buf);
    AcmpDu pdu{};
    (void)protocol::load_unchecked(wire, &pdu);
    auto const back = acmp_command_response_from_pdu(pdu);

    EXPECT_EQ(back.message_type(), ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE);
    EXPECT_TRUE(back.stream_id == resp.stream_id);
    EXPECT_TRUE(back.controller_entity_id == resp.controller_entity_id);
    EXPECT_TRUE(back.talker_entity_id == resp.talker_entity_id);
    EXPECT_TRUE(back.listener_entity_id == resp.listener_entity_id);
    EXPECT_EQ(back.talker_unique_id.get(), 3);
    EXPECT_EQ(back.listener_unique_id.get(), 4);
    EXPECT_TRUE(back.stream_dest_mac == resp.stream_dest_mac);
    EXPECT_EQ(back.connection_count.get(), 1);
    EXPECT_EQ(back.sequence_id.get(), 99);
    EXPECT_EQ(back.stream_vlan_id.get(), 2);
}

//
// Tests: ListenerStreamInfo
//

TEST(listener_info, default_constructor)
{
    ListenerStreamInfo info;
    EXPECT_FALSE(info.connected);
    EXPECT_FALSE(info.pending_connection);
    EXPECT_EQ(info.talker_unique_id, 0);
    EXPECT_EQ(info.flags, 0);
    EXPECT_EQ(info.stream_vlan_id, 0);
}

TEST(listener_info, reset)
{
    ListenerStreamInfo info;
    info.connected = true;
    info.pending_connection = true;
    info.talker_unique_id = 5;
    info.flags = 0x1234;
    info.stream_vlan_id = 100;
    info.talker_entity_id = Eui64(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);

    info.reset();

    EXPECT_FALSE(info.connected);
    EXPECT_FALSE(info.pending_connection);
    EXPECT_EQ(info.talker_unique_id, 0);
    EXPECT_EQ(info.flags, 0);
    EXPECT_EQ(info.stream_vlan_id, 0);
}

TEST(listener_info, comparison)
{
    ListenerStreamInfo info1;
    info1.talker_unique_id = 1;
    info1.connected = true;

    ListenerStreamInfo info2;
    info2.talker_unique_id = 1;
    info2.connected = true;

    EXPECT_TRUE(info1 == info2);

    info2.talker_unique_id = 2;
    EXPECT_FALSE(info1 == info2);
}

//
// Tests: ListenerPair
//

TEST(listener_pair, default_constructor)
{
    ListenerPair pair;
    EXPECT_EQ(pair.listener_unique_id, 0);
}

TEST(listener_pair, comparison)
{
    ListenerPair pair1;
    pair1.listener_entity_id = Eui64(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_entity_id = Eui64(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pair2.listener_unique_id = 1;

    EXPECT_TRUE(pair1 == pair2);

    pair2.listener_unique_id = 2;
    EXPECT_FALSE(pair1 == pair2);
}

//
// Tests: TalkerStreamInfo (Template)
//

TEST(talker_info, default_constructor)
{
    TalkerStreamInfo<16> info;
    EXPECT_EQ(info.connection_count, 0);
    EXPECT_EQ(info.stream_vlan_id, 0);
}

TEST(talker_info, add_listener)
{
    TalkerStreamInfo<4> info;

    ListenerPair pair1;
    pair1.listener_entity_id = Eui64(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pair1.listener_unique_id = 1;

    EXPECT_TRUE(info.add_listener(pair1));
    EXPECT_EQ(info.connection_count, 1);
    EXPECT_TRUE(info.contains_listener(pair1));

    // Adding duplicate should fail
    EXPECT_FALSE(info.add_listener(pair1));
    EXPECT_EQ(info.connection_count, 1);
}

TEST(talker_info, add_listener_capacity)
{
    TalkerStreamInfo<2> info;

    ListenerPair pair1;
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_unique_id = 2;

    ListenerPair pair3;
    pair3.listener_unique_id = 3;

    EXPECT_TRUE(info.add_listener(pair1));
    EXPECT_TRUE(info.add_listener(pair2));
    EXPECT_FALSE(info.add_listener(pair3));  // At capacity
    EXPECT_EQ(info.connection_count, 2);
}

TEST(talker_info, remove_listener)
{
    TalkerStreamInfo<4> info;

    ListenerPair pair1;
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_unique_id = 2;

    info.add_listener(pair1);
    info.add_listener(pair2);
    EXPECT_EQ(info.connection_count, 2);

    EXPECT_TRUE(info.remove_listener(pair1));
    EXPECT_EQ(info.connection_count, 1);
    EXPECT_FALSE(info.contains_listener(pair1));
    EXPECT_TRUE(info.contains_listener(pair2));

    // Removing non-existent should fail
    EXPECT_FALSE(info.remove_listener(pair1));
}

TEST(talker_info, get_listener)
{
    TalkerStreamInfo<4> info;

    ListenerPair pair1;
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_unique_id = 2;

    info.add_listener(pair1);
    info.add_listener(pair2);

    auto const* p0 = info.get_listener(0);
    EXPECT_TRUE(p0 != nullptr);
    EXPECT_EQ(p0->listener_unique_id, 1);

    auto const* p1 = info.get_listener(1);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_EQ(p1->listener_unique_id, 2);

    auto const* p2 = info.get_listener(2);
    EXPECT_TRUE(p2 == nullptr);
}

TEST(talker_info, reset)
{
    TalkerStreamInfo<4> info;
    info.stream_vlan_id = 100;

    ListenerPair pair;
    pair.listener_unique_id = 1;
    info.add_listener(pair);

    info.reset();

    EXPECT_EQ(info.connection_count, 0);
    EXPECT_EQ(info.stream_vlan_id, 0);
    EXPECT_FALSE(info.contains_listener(pair));
}

//
// Tests: TalkerStreamInfoDynamic
//

TEST(talker_info_dyn, default_constructor)
{
    TalkerStreamInfoDynamic info;
    EXPECT_EQ(info.connection_count(), 0);
    EXPECT_EQ(info.max_connected_listeners(), 16);
    EXPECT_EQ(info.stream_vlan_id, 0);
}

TEST(talker_info_dyn, custom_capacity)
{
    TalkerStreamInfoDynamic info(32);
    EXPECT_EQ(info.max_connected_listeners(), 32);
}

TEST(talker_info_dyn, add_listener)
{
    TalkerStreamInfoDynamic info(4);

    ListenerPair pair1;
    pair1.listener_entity_id = Eui64(0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88);
    pair1.listener_unique_id = 1;

    EXPECT_TRUE(info.add_listener(pair1));
    EXPECT_EQ(info.connection_count(), 1);
    EXPECT_TRUE(info.contains_listener(pair1));

    // Adding duplicate should fail
    EXPECT_FALSE(info.add_listener(pair1));
    EXPECT_EQ(info.connection_count(), 1);
}

TEST(talker_info_dyn, add_listener_capacity)
{
    TalkerStreamInfoDynamic info(2);

    ListenerPair pair1;
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_unique_id = 2;

    ListenerPair pair3;
    pair3.listener_unique_id = 3;

    EXPECT_TRUE(info.add_listener(pair1));
    EXPECT_TRUE(info.add_listener(pair2));
    EXPECT_FALSE(info.add_listener(pair3));  // At capacity
    EXPECT_EQ(info.connection_count(), 2);
}

TEST(talker_info_dyn, remove_listener)
{
    TalkerStreamInfoDynamic info(4);

    ListenerPair pair1;
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_unique_id = 2;

    info.add_listener(pair1);
    info.add_listener(pair2);
    EXPECT_EQ(info.connection_count(), 2);

    EXPECT_TRUE(info.remove_listener(pair1));
    EXPECT_EQ(info.connection_count(), 1);
    EXPECT_FALSE(info.contains_listener(pair1));
    EXPECT_TRUE(info.contains_listener(pair2));

    // Removing non-existent should fail
    EXPECT_FALSE(info.remove_listener(pair1));
}

TEST(talker_info_dyn, get_listener)
{
    TalkerStreamInfoDynamic info(4);

    ListenerPair pair1;
    pair1.listener_unique_id = 1;

    ListenerPair pair2;
    pair2.listener_unique_id = 2;

    info.add_listener(pair1);
    info.add_listener(pair2);

    auto const* p0 = info.get_listener(0);
    EXPECT_TRUE(p0 != nullptr);
    EXPECT_EQ(p0->listener_unique_id, 1);

    auto const* p1 = info.get_listener(1);
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_EQ(p1->listener_unique_id, 2);

    auto const* p2 = info.get_listener(2);
    EXPECT_TRUE(p2 == nullptr);
}

TEST(talker_info_dyn, reset)
{
    TalkerStreamInfoDynamic info(4);
    info.stream_vlan_id = 100;

    ListenerPair pair;
    pair.listener_unique_id = 1;
    info.add_listener(pair);

    info.reset();

    EXPECT_EQ(info.connection_count(), 0);
    EXPECT_EQ(info.stream_vlan_id, 0);
    EXPECT_FALSE(info.contains_listener(pair));
}

//
// Tests: ACMP Timeout Helpers
//

TEST(acmp_timeout, connect_tx_command)
{
    auto timeout = acmp_timeout_for_message_type(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_EQ(timeout.count(), 2000);
}

TEST(acmp_timeout, disconnect_tx_command)
{
    auto timeout = acmp_timeout_for_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND);
    EXPECT_EQ(timeout.count(), 200);
}

TEST(acmp_timeout, connect_rx_command)
{
    auto timeout = acmp_timeout_for_message_type(ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND);
    EXPECT_EQ(timeout.count(), 4500);
}

TEST(acmp_timeout, disconnect_rx_command)
{
    auto timeout = acmp_timeout_for_message_type(ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND);
    EXPECT_EQ(timeout.count(), 500);
}

TEST(acmp_timeout, unknown_message)
{
    auto timeout = acmp_timeout_for_message_type(0xFF);
    EXPECT_EQ(timeout.count(), 200);  // Default timeout
}

//
// Tests: AdpDu Validation
//

TEST(adpdu_valid, valid_pdu)
{
    AdpDu adp;
    adp.init_entity_available(Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77), 20);
    EXPECT_TRUE(adp.is_valid());
}

TEST(adpdu_valid, invalid_subtype)
{
    AdpDu adp;
    adp.init_entity_available(Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77), 20);
    adp.subtype = 0x00;  // Wrong subtype
    EXPECT_FALSE(adp.is_valid());
}

TEST(adpdu_valid, control_data_length_too_small)
{
    AdpDu adp;
    adp.init_entity_available(Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77), 20);
    adp.set_control_data_length(AdpDu::DATA_LENGTH - 1);
    EXPECT_FALSE(adp.is_valid());
}

TEST(adpdu_valid, control_data_length_larger_accepted)
{
    AdpDu adp;
    adp.init_entity_available(Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77), 20);
    adp.set_control_data_length(AdpDu::DATA_LENGTH + 20);
    EXPECT_TRUE(adp.is_valid());
}

TEST(adpdu_valid, invalid_message_type)
{
    AdpDu adp;
    adp.init_entity_available(Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77), 20);
    adp.set_message_type(3);  // Invalid message type (max is 2)
    EXPECT_FALSE(adp.is_valid());
}

//
// Tests: AecpDuCommon Validation
//

TEST(aecpdu_valid, valid_pdu)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 10);
    EXPECT_TRUE(aecp.is_valid());
}

TEST(aecpdu_valid, invalid_subtype)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 10);
    aecp.subtype = 0x00;  // Wrong subtype
    EXPECT_FALSE(aecp.is_valid());
}

TEST(aecpdu_valid, control_data_too_small)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 10);
    aecp.set_control_data_length(5);  // Too small (minimum is 10)
    EXPECT_FALSE(aecp.is_valid());
}

TEST(aecpdu_valid, control_data_too_large)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 10);
    aecp.set_control_data_length(600);  // Too large (max is 524)
    EXPECT_FALSE(aecp.is_valid());
}

TEST(aecpdu_valid, invalid_message_type)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AEM_COMMAND, 10);
    aecp.set_message_type(10);  // Invalid (10-13 are reserved)
    EXPECT_FALSE(aecp.is_valid());
}

//
// Tests: AemDu Validation
//

TEST(aemdu_valid, valid_pdu)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);
    EXPECT_TRUE(aem.is_valid());
}

TEST(aemdu_valid, invalid_subtype)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);
    aem.subtype = 0x00;  // Wrong subtype
    EXPECT_FALSE(aem.is_valid());
}

TEST(aemdu_valid, control_data_too_small)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);
    aem.set_control_data_length(10);  // Too small (minimum is 12)
    EXPECT_FALSE(aem.is_valid());
}

TEST(aemdu_valid, invalid_message_type)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);
    aem.set_message_type(AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND);  // Not AEM
    EXPECT_FALSE(aem.is_valid());
}

//
// Tests: AcmpDu - Additional Flag Methods
//

TEST(acmpdu_flags_extra, sv_and_version)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);

    // After init, sv should be false and version should be 0
    EXPECT_FALSE(acmp.sv());
    EXPECT_EQ(acmp.version(), 0);
}

TEST(acmpdu_flags_extra, supports_encrypted)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.supports_encrypted());

    acmp.flags = acmp_flags::SUPPORTS_ENCRYPTED;
    EXPECT_TRUE(acmp.supports_encrypted());
}

TEST(acmpdu_flags_extra, is_encrypted_pdu)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.is_encrypted_pdu());

    acmp.flags = acmp_flags::ENCRYPTED_PDU;
    EXPECT_TRUE(acmp.is_encrypted_pdu());
}

TEST(acmpdu_flags_extra, is_talker_failed)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.is_talker_failed());

    acmp.flags = acmp_flags::TALKER_FAILED;
    EXPECT_TRUE(acmp.is_talker_failed());
}

TEST(acmpdu_flags_extra, is_srp_registration_failed)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.is_srp_registration_failed());

    // SRP_REGISTRATION_FAILED is an alias for TALKER_FAILED
    acmp.flags = acmp_flags::TALKER_FAILED;
    EXPECT_TRUE(acmp.is_srp_registration_failed());
}

TEST(acmpdu_flags_extra, is_cl_entries_valid)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.is_cl_entries_valid());

    acmp.flags = acmp_flags::CL_ENTRIES_VALID;
    EXPECT_TRUE(acmp.is_cl_entries_valid());
}

TEST(acmpdu_flags_extra, is_no_srp)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.is_no_srp());

    acmp.flags = acmp_flags::NO_SRP;
    EXPECT_TRUE(acmp.is_no_srp());
}

TEST(acmpdu_flags_extra, is_udp)
{
    AcmpDu acmp;
    acmp.init_command(ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND);
    EXPECT_FALSE(acmp.is_udp());

    acmp.flags = acmp_flags::UDP;
    EXPECT_TRUE(acmp.is_udp());
}

//
// Tests: AdpDu - Additional Methods
//

TEST(adpdu_extra, sv_and_version)
{
    AdpDu adp;
    adp.init_entity_available(Eui64(0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77), 20);

    // After init, sv should be false and version should be 0
    EXPECT_FALSE(adp.sv());
    EXPECT_EQ(adp.version(), 0);
}

TEST(adpdu_caps_extra, controller_capabilities)
{
    AdpDu adp;
    adp.controller_capabilities = controller_capabilities::IMPLEMENTED;

    EXPECT_TRUE(adp.has_controller_capability(controller_capabilities::IMPLEMENTED));
    EXPECT_FALSE(adp.has_controller_capability(0x00000002));  // Not a defined capability
}

//
// Tests: AecpDuCommon - Additional Type Checks
//

TEST(aecpdu_type_extra, is_avc)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_AVC_COMMAND, 10);
    EXPECT_TRUE(aecp.is_avc());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AVC_RESPONSE);
    EXPECT_TRUE(aecp.is_avc());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_FALSE(aecp.is_avc());
}

TEST(aecpdu_type_extra, is_hdcp_apm)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_HDCP_APM_COMMAND, 10);
    EXPECT_TRUE(aecp.is_hdcp_apm());

    aecp.set_message_type(AECP_MESSAGE_TYPE_HDCP_APM_RESPONSE);
    EXPECT_TRUE(aecp.is_hdcp_apm());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_FALSE(aecp.is_hdcp_apm());
}

TEST(aecpdu_type_extra, is_extended)
{
    AecpDuCommon aecp{};
    aecp.init_command(AECP_MESSAGE_TYPE_EXTENDED_COMMAND, 10);
    EXPECT_TRUE(aecp.is_extended());

    aecp.set_message_type(AECP_MESSAGE_TYPE_EXTENDED_RESPONSE);
    EXPECT_TRUE(aecp.is_extended());

    aecp.set_message_type(AECP_MESSAGE_TYPE_AEM_COMMAND);
    EXPECT_FALSE(aecp.is_extended());
}

//
// Tests: AemDu - Additional Methods
//

TEST(aemdu_extra, sv_and_version)
{
    AemDu aem{};
    aem.init_command(AEM_COMMAND_READ_DESCRIPTOR, 12);

    // After init, sv should be false and version should be 0
    EXPECT_FALSE(aem.sv());
    EXPECT_EQ(aem.version(), 0);
}

//
// Tests: AEM Command Payload Methods
//

TEST(aem_acquire_payload, is_persistent)
{
    aem::AemAcquireEntityPayload payload{};
    EXPECT_FALSE(payload.is_persistent());

    payload.set_persistent(true);
    EXPECT_TRUE(payload.is_persistent());

    payload.set_persistent(false);
    EXPECT_FALSE(payload.is_persistent());
}

TEST(aem_acquire_payload, is_release)
{
    aem::AemAcquireEntityPayload payload{};
    EXPECT_FALSE(payload.is_release());

    payload.set_release(true);
    EXPECT_TRUE(payload.is_release());

    payload.set_release(false);
    EXPECT_FALSE(payload.is_release());
}

TEST(aem_lock_payload, is_unlock)
{
    aem::AemLockEntityPayload payload{};
    EXPECT_FALSE(payload.is_unlock());

    payload.set_unlock(true);
    EXPECT_TRUE(payload.is_unlock());

    payload.set_unlock(false);
    EXPECT_FALSE(payload.is_unlock());
}

TEST(aem_stream_info_payload, is_connected)
{
    aem::AemStreamInfoPayload payload{};
    EXPECT_FALSE(payload.is_connected());

    payload.flags = aem::stream_info_flags::CONNECTED;
    EXPECT_TRUE(payload.is_connected());
}

TEST(aem_stream_info_payload, is_class_b)
{
    aem::AemStreamInfoPayload payload{};
    EXPECT_FALSE(payload.is_class_b());

    payload.flags = aem::stream_info_flags::CLASS_B;
    EXPECT_TRUE(payload.is_class_b());
}

TEST(aem_stream_info_payload, is_stream_id_valid)
{
    aem::AemStreamInfoPayload payload{};
    EXPECT_FALSE(payload.is_stream_id_valid());

    payload.flags = aem::stream_info_flags::STREAM_ID_VALID;
    EXPECT_TRUE(payload.is_stream_id_valid());
}

TEST(aem_stream_info_payload, is_stream_format_valid)
{
    aem::AemStreamInfoPayload payload{};
    EXPECT_FALSE(payload.is_stream_format_valid());

    payload.flags = aem::stream_info_flags::STREAM_FORMAT_VALID;
    EXPECT_TRUE(payload.is_stream_format_valid());
}

TEST(aem_avb_info_payload, is_as_capable)
{
    aem::AemAvbInfoPayload payload{};
    EXPECT_FALSE(payload.is_as_capable());

    payload.flags = aem::avb_info_flags::AS_CAPABLE;
    EXPECT_TRUE(payload.is_as_capable());
}

TEST(aem_avb_info_payload, is_gptp_enabled)
{
    aem::AemAvbInfoPayload payload{};
    EXPECT_FALSE(payload.is_gptp_enabled());

    payload.flags = aem::avb_info_flags::GPTP_ENABLED;
    EXPECT_TRUE(payload.is_gptp_enabled());
}

TEST(aem_avb_info_payload, is_srp_enabled)
{
    aem::AemAvbInfoPayload payload{};
    EXPECT_FALSE(payload.is_srp_enabled());

    payload.flags = aem::avb_info_flags::SRP_ENABLED;
    EXPECT_TRUE(payload.is_srp_enabled());
}

TEST(aem_avb_info_payload, is_avtp_down)
{
    aem::AemAvbInfoPayload payload{};
    EXPECT_FALSE(payload.is_avtp_down());
    EXPECT_FALSE(payload.is_avtp_down_valid());

    payload.flags = aem::avb_info_flags::AVTP_DOWN | aem::avb_info_flags::AVTP_DOWN_VALID;
    EXPECT_TRUE(payload.is_avtp_down());
    EXPECT_TRUE(payload.is_avtp_down_valid());
}

TEST(aem_avb_info_payload, struct_size)
{
    EXPECT_EQ(sizeof(aem::AemAvbInfoPayload), 20U);
    EXPECT_EQ(aem::AemAvbInfoPayload::LENGTH, 20U);
}

TEST(aem_ptp_port_info, struct_layout)
{
    using namespace aem;
    EXPECT_EQ(sizeof(AemPtpPortInfoCommandPayload), 36u);
    EXPECT_EQ(sizeof(AemGetPtpPortInfoResponsePayload), 76u);
    EXPECT_EQ(AemGetPtpPortInfoResponsePayload::MINIMUM_LENGTH, 36u);
    EXPECT_EQ(offsetof(AemGetPtpPortInfoResponsePayload, mean_link_delay), 40u);
    EXPECT_EQ(offsetof(AemGetPtpPortInfoResponsePayload, neighbor_rate_ratio), 48u);
    EXPECT_EQ(offsetof(AemGetPtpPortInfoResponsePayload, major_version), 56u);
    EXPECT_EQ(offsetof(AemGetPtpPortInfoResponsePayload, ext_port_flags), 58u);
}

//
// Tests: Runtime coverage for constexpr name/utility functions
//

TEST(atdecc_rt_coverage, acmp_message_type_names)
{
    uint8_t volatile connect_tx_cmd = ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND;
    uint8_t volatile connect_tx_resp = ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE;
    uint8_t volatile disconnect_tx_cmd = ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND;
    uint8_t volatile get_tx_state_cmd = ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND;
    uint8_t volatile connect_rx_cmd = ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND;
    uint8_t volatile get_rx_state_cmd = ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND;
    uint8_t volatile get_tx_conn_cmd = ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND;
    uint8_t volatile unknown = 0xFF;

    EXPECT_TRUE(std::string(acmp_message_type_name(connect_tx_cmd))[0] == 'C');
    EXPECT_TRUE(std::string(acmp_message_type_name(connect_tx_resp))[0] == 'C');
    EXPECT_TRUE(std::string(acmp_message_type_name(disconnect_tx_cmd))[0] == 'D');
    EXPECT_TRUE(std::string(acmp_message_type_name(get_tx_state_cmd))[0] == 'G');
    EXPECT_TRUE(std::string(acmp_message_type_name(connect_rx_cmd))[0] == 'C');
    EXPECT_TRUE(std::string(acmp_message_type_name(get_rx_state_cmd))[0] == 'G');
    EXPECT_TRUE(std::string(acmp_message_type_name(get_tx_conn_cmd))[0] == 'G');
    EXPECT_TRUE(std::string(acmp_message_type_name(unknown))[0] == 'U');
}

TEST(atdecc_rt_coverage, acmp_status_names)
{
    uint8_t volatile success_s = ACMP_STATUS_SUCCESS;
    uint8_t volatile listener_unknown = ACMP_STATUS_LISTENER_UNKNOWN_ID;
    uint8_t volatile talker_unknown = ACMP_STATUS_TALKER_UNKNOWN_ID;
    uint8_t volatile not_supported = ACMP_STATUS_NOT_SUPPORTED;
    uint8_t volatile unknown = 0xFF;

    EXPECT_TRUE(std::string(acmp_status_name(success_s))[0] == 'S');
    EXPECT_TRUE(std::string(acmp_status_name(listener_unknown))[0] == 'L');
    EXPECT_TRUE(std::string(acmp_status_name(talker_unknown))[0] == 'T');
    EXPECT_TRUE(std::string(acmp_status_name(not_supported))[0] == 'N');
    EXPECT_TRUE(std::string(acmp_status_name(unknown))[0] == 'U');
}

TEST(atdecc_rt_coverage, acmpdu_format_detection)
{
    uint16_t volatile len_2013 = 44;
    uint16_t volatile len_2021 = 84;

    EXPECT_TRUE(is_acmpdu_2013_format(len_2013));
    EXPECT_FALSE(is_acmpdu_2013_format(len_2021));
    EXPECT_TRUE(is_acmpdu_2021_format(len_2021));
    EXPECT_FALSE(is_acmpdu_2021_format(len_2013));
}

TEST(atdecc_rt_coverage, adp_message_type_names)
{
    uint8_t volatile available = ADP_MESSAGE_TYPE_ENTITY_AVAILABLE;
    uint8_t volatile departing = ADP_MESSAGE_TYPE_ENTITY_DEPARTING;
    uint8_t volatile discover = ADP_MESSAGE_TYPE_ENTITY_DISCOVER;
    uint8_t volatile unknown = 0xFF;

    EXPECT_TRUE(std::string(adp_message_type_name(available))[0] == 'E');
    EXPECT_TRUE(std::string(adp_message_type_name(departing))[0] == 'E');
    EXPECT_TRUE(std::string(adp_message_type_name(discover))[0] == 'E');
    EXPECT_TRUE(std::string(adp_message_type_name(unknown))[0] == 'U');
}

TEST(atdecc_rt_coverage, aecp_message_type_names)
{
    uint8_t volatile aem_cmd = AECP_MESSAGE_TYPE_AEM_COMMAND;
    uint8_t volatile aem_resp = AECP_MESSAGE_TYPE_AEM_RESPONSE;
    uint8_t volatile vendor_cmd = AECP_MESSAGE_TYPE_VENDOR_UNIQUE_COMMAND;
    uint8_t volatile unknown = 0xFF;

    EXPECT_TRUE(std::string(aecp_message_type_name(aem_cmd))[0] == 'A');
    EXPECT_TRUE(std::string(aecp_message_type_name(aem_resp))[0] == 'A');
    EXPECT_TRUE(std::string(aecp_message_type_name(vendor_cmd))[0] == 'V');
    EXPECT_TRUE(std::string(aecp_message_type_name(unknown))[0] == 'U');
}

TEST(atdecc_rt_coverage, aecp_status_names)
{
    uint8_t volatile success_s = AECP_STATUS_SUCCESS;
    uint8_t volatile not_impl = AECP_STATUS_NOT_IMPLEMENTED;
    uint8_t volatile unknown = 0xFF;

    EXPECT_TRUE(std::string(aecp_status_name(success_s))[0] == 'S');
    EXPECT_TRUE(std::string(aecp_status_name(not_impl))[0] == 'N');
    EXPECT_TRUE(std::string(aecp_status_name(unknown))[0] == 'U');
}

TEST(atdecc_rt_coverage, aem_status_names)
{
    uint8_t volatile success_s = AEM_STATUS_SUCCESS;
    uint8_t volatile not_impl = AEM_STATUS_NOT_IMPLEMENTED;
    uint8_t volatile no_desc = AEM_STATUS_NO_SUCH_DESCRIPTOR;
    uint8_t volatile locked = AEM_STATUS_ENTITY_LOCKED;
    uint8_t volatile unknown = 0xFF;

    EXPECT_TRUE(std::string(aem_status_name(success_s))[0] == 'S');
    EXPECT_TRUE(std::string(aem_status_name(not_impl))[0] == 'N');
    EXPECT_TRUE(std::string(aem_status_name(no_desc))[0] == 'N');
    EXPECT_TRUE(std::string(aem_status_name(locked))[0] == 'E');
    EXPECT_TRUE(std::string(aem_status_name(unknown))[0] == 'U');
}

TEST(atdecc_rt_coverage, aem_command_names)
{
    uint16_t volatile acquire = AEM_COMMAND_ACQUIRE_ENTITY;
    uint16_t volatile lock = AEM_COMMAND_LOCK_ENTITY;
    uint16_t volatile read_desc = AEM_COMMAND_READ_DESCRIPTOR;
    uint16_t volatile start_stream = AEM_COMMAND_START_STREAMING;
    uint16_t volatile unknown = 0x7FFE;

    EXPECT_TRUE(std::string(aem_command_name(acquire))[0] == 'A');
    EXPECT_TRUE(std::string(aem_command_name(lock))[0] == 'L');
    EXPECT_TRUE(std::string(aem_command_name(read_desc))[0] == 'R');
    EXPECT_TRUE(std::string(aem_command_name(start_stream))[0] == 'S');
    EXPECT_TRUE(std::string(aem_command_name(unknown))[0] == 'U');
}

// ===========================================================================
// AEM descriptor type name and AtdeccString tests
// ===========================================================================

TEST(atdecc_aem_names, descriptor_type_name_known)
{
    EXPECT_EQ(std::string(aem::descriptor_type_name(aem::DESCRIPTOR_ENTITY)), "Entity");
    EXPECT_EQ(std::string(aem::descriptor_type_name(aem::DESCRIPTOR_CONFIGURATION)), "Configuration");
    EXPECT_EQ(std::string(aem::descriptor_type_name(aem::DESCRIPTOR_AUDIO_UNIT)), "Audio Unit");
    EXPECT_EQ(std::string(aem::descriptor_type_name(aem::DESCRIPTOR_STREAM_INPUT)), "Stream Input");
    EXPECT_EQ(std::string(aem::descriptor_type_name(aem::DESCRIPTOR_STREAM_OUTPUT)), "Stream Output");
    EXPECT_EQ(std::string(aem::descriptor_type_name(aem::DESCRIPTOR_CLOCK_SOURCE)), "Clock Source");
}

TEST(atdecc_aem_names, descriptor_type_name_unknown)
{
    auto const* name = aem::descriptor_type_name(0xFFFF);
    EXPECT_TRUE(name != nullptr);
    EXPECT_TRUE(std::string(name).size() > 0);
}

TEST(atdecc_aem_string, spaceship_operator)
{
    aem::AtdeccString a{"Alpha"};
    aem::AtdeccString b{"Beta"};
    aem::AtdeccString a2{"Alpha"};

    EXPECT_TRUE((a <=> a2) == std::strong_ordering::equal);
    EXPECT_TRUE((a <=> b) != std::strong_ordering::equal);
    EXPECT_TRUE((a <=> b) == std::strong_ordering::less);
    EXPECT_TRUE((b <=> a) == std::strong_ordering::greater);
}

// ===========================================================================
// AEM descriptor/payload parsing tests
// ===========================================================================

TEST(atdecc_aem_parse, parse_descriptor_insufficient_data)
{
    std::array<uint8_t, 2> small{0x00, 0x00};
    auto result = aem::parse_descriptor(small);
    EXPECT_FALSE(result.has_value());
}

TEST(atdecc_aem_parse, parse_descriptor_valid_entity)
{
    // Entity descriptor needs DescriptorEntity::LENGTH (312) bytes
    std::array<uint8_t, 312> buf{};
    // descriptor_type = DESCRIPTOR_ENTITY (0x0000) in network byte order
    buf[0] = 0x00;
    buf[1] = 0x00;
    // descriptor_index = 0
    buf[2] = 0x00;
    buf[3] = 0x00;
    auto result = aem::parse_descriptor(buf);
    EXPECT_TRUE(result.has_value());
}

TEST(atdecc_aem_parse, parse_aem_insufficient_data)
{
    std::array<uint8_t, 2> small{0x00, 0x00};
    auto result = atdecc::parse_aem(0x0001, false, small);
    EXPECT_FALSE(result.has_value());
}

// ===========================================================================
// JDKS log priority name test
// ===========================================================================

TEST(atdecc_jdks_names, log_priority_name_known)
{
    EXPECT_EQ(std::string(jdks::log_priority_name(jdks::log_priority::ERROR)), "Error");
    EXPECT_EQ(std::string(jdks::log_priority_name(jdks::log_priority::WARNING)), "Warning");
    EXPECT_EQ(std::string(jdks::log_priority_name(jdks::log_priority::INFO)), "Info");
    EXPECT_EQ(std::string(jdks::log_priority_name(jdks::log_priority::DEBUG1)), "Debug1");
    EXPECT_EQ(std::string(jdks::log_priority_name(jdks::log_priority::CONSOLE)), "Console");
}

TEST(atdecc_jdks_names, log_priority_name_unknown)
{
    auto const* name = jdks::log_priority_name(42);
    EXPECT_TRUE(name != nullptr);
}

// ===========================================================================
// Phase 1 AEM refactor primitives — DescriptorRef/NameRef, wire_size(),
// span_*_wire helpers, and MAX_AEM_DESCRIPTOR_SIZE constant.
// ===========================================================================

TEST(aem_refactor_primitives, max_aem_descriptor_size_constant)
{
    // Derived from IEEE 1722.1 Clause 9.2.2.6:
    //   524 (CDL max) - 12 (AEM_DATA_LENGTH) - 4 (READ_DESCRIPTOR hdr) = 508
    EXPECT_EQ(MAX_AEM_DESCRIPTOR_SIZE, static_cast<size_t>(508));
}

TEST(aem_refactor_primitives, descriptor_ref_default_and_equality)
{
    aem::DescriptorRef const a{};
    aem::DescriptorRef const b{};
    EXPECT_TRUE(a == b);

    aem::DescriptorRef const c{.configuration_index = 0, .descriptor_type = aem::DESCRIPTOR_ENTITY, .descriptor_index = 0};
    aem::DescriptorRef const d{.configuration_index = 0, .descriptor_type = aem::DESCRIPTOR_CONFIGURATION, .descriptor_index = 0};
    EXPECT_FALSE(c == d);
}

TEST(aem_refactor_primitives, name_ref_composes_descriptor_ref)
{
    aem::NameRef const a{};
    EXPECT_EQ(a.name_index, static_cast<uint16_t>(0));
    EXPECT_EQ(a.descriptor.descriptor_type, static_cast<uint16_t>(0));

    aem::NameRef const b{
        .descriptor = {.configuration_index = 1, .descriptor_type = aem::DESCRIPTOR_ENTITY, .descriptor_index = 0},
        .name_index = 1};
    EXPECT_EQ(b.name_index, static_cast<uint16_t>(1));
    EXPECT_EQ(b.descriptor.configuration_index, static_cast<uint16_t>(1));
    EXPECT_FALSE(a == b);
}

TEST(aem_refactor_primitives, wire_size_returns_length_for_fixed_descriptors)
{
    // Every fixed descriptor's wire_size() must equal its declared LENGTH,
    // which in turn equals sizeof(T) (enforced by existing static_asserts).
    EXPECT_EQ(aem::DescriptorEntity::wire_size(), aem::DescriptorEntity::LENGTH);
    EXPECT_EQ(aem::DescriptorJack::wire_size(), aem::DescriptorJack::LENGTH);
    EXPECT_EQ(aem::DescriptorClockSource::wire_size(), aem::DescriptorClockSource::LENGTH);
    EXPECT_EQ(aem::DescriptorMemoryObject::wire_size(), aem::DescriptorMemoryObject::LENGTH);
    EXPECT_EQ(aem::DescriptorLocale::wire_size(), aem::DescriptorLocale::LENGTH);
    EXPECT_EQ(aem::DescriptorStrings::wire_size(), aem::DescriptorStrings::LENGTH);
    EXPECT_EQ(aem::DescriptorStreamPort::wire_size(), aem::DescriptorStreamPort::LENGTH);
    EXPECT_EQ(aem::DescriptorExternalPort::wire_size(), aem::DescriptorExternalPort::LENGTH);
    EXPECT_EQ(aem::DescriptorInternalPort::wire_size(), aem::DescriptorInternalPort::LENGTH);
    EXPECT_EQ(aem::DescriptorAudioCluster::wire_size(), aem::DescriptorAudioCluster::LENGTH);
    EXPECT_EQ(aem::DescriptorVideoCluster::wire_size(), aem::DescriptorVideoCluster::LENGTH);
    EXPECT_EQ(aem::DescriptorSensorCluster::wire_size(), aem::DescriptorSensorCluster::LENGTH);
    EXPECT_EQ(aem::DescriptorControlBlock::wire_size(), aem::DescriptorControlBlock::LENGTH);
    EXPECT_EQ(aem::DescriptorPtpInstance::wire_size(), aem::DescriptorPtpInstance::LENGTH);
    EXPECT_EQ(aem::DescriptorPtpPort::wire_size(), aem::DescriptorPtpPort::LENGTH);
}

TEST(aem_refactor_primitives, every_fixed_descriptor_fits_max)
{
    // No fixed descriptor may exceed MAX_AEM_DESCRIPTOR_SIZE on the wire.
    // Strings is the largest at 452 bytes; Entity is 312.
    EXPECT_TRUE(aem::DescriptorStrings::wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
    EXPECT_TRUE(aem::DescriptorEntity::wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
    EXPECT_TRUE(aem::DescriptorPtpPort::wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
}

TEST(aem_refactor_primitives, span_store_wire_round_trip_via_span_load_padded)
{
    // Populate an Entity descriptor, store via span_store_wire, then load
    // via span_load_padded — round-trip should preserve all populated bytes.
    // Note: span_load_padded is the correct load primitive — the source
    // buffer's size is authoritative on the way in (we can't call
    // dst.wire_size() before loading, since the fields that determine it
    // haven't been populated yet).
    // Compared by bytes (make_const_span) since AtdeccString does not
    // implement operator==.
    aem::DescriptorEntity src{};
    src.entity_name = aem::AtdeccString{"RoundTripEntity"};
    src.configurations_count = 1;
    src.current_configuration = 0;

    std::array<uint8_t, MAX_AEM_DESCRIPTOR_SIZE> buf{};
    span_store_wire(make_span(buf), src);

    aem::DescriptorEntity dst{};
    span_load_padded(dst, std::span<uint8_t const>{buf.data(), aem::DescriptorEntity::wire_size()});

    auto const src_bytes = make_const_span(src);
    auto const dst_bytes = make_const_span(dst);
    EXPECT_EQ(src_bytes.size(), dst_bytes.size());
    bool equal = true;
    for (size_t i = 0; i < src_bytes.size(); ++i) {
        if (src_bytes[i] != dst_bytes[i]) {
            equal = false;
            break;
        }
    }
    EXPECT_TRUE(equal);
}

TEST(aem_refactor_primitives, wire_span_returns_wire_size_bytes)
{
    aem::DescriptorJack const desc{};
    auto const view = wire_span(desc);
    EXPECT_EQ(view.size(), aem::DescriptorJack::wire_size());
}

// ===========================================================================
// Phase 2 — DescriptorAudioMap with typed inline mapping storage.
// ===========================================================================

TEST(aem_audio_map, empty_map_has_header_wire_size)
{
    aem::DescriptorAudioMap const desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorAudioMap::LENGTH);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(8));
}

TEST(aem_audio_map, wire_size_scales_with_number_of_mappings)
{
    aem::DescriptorAudioMap desc{};
    desc.number_of_mappings = 3;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorAudioMap::LENGTH + (3 * sizeof(aem::AudioMapping)));
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(32));
}

TEST(aem_audio_map, max_mappings_fits_in_aecp_budget)
{
    // 62 mappings + 8 byte header = 504 bytes, must stay <= MAX_AEM_DESCRIPTOR_SIZE (508)
    aem::DescriptorAudioMap desc{};
    desc.number_of_mappings = aem::DescriptorAudioMap::MAX_MAPPINGS;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(504));
}

TEST(aem_audio_map, audio_mapping_is_eight_bytes)
{
    EXPECT_EQ(sizeof(aem::AudioMapping), static_cast<size_t>(8));
}

TEST(aem_audio_map, populated_map_round_trips_through_wire_span)
{
    aem::DescriptorAudioMap src{};
    src.descriptor_index = 5;
    src.number_of_mappings = 2;
    src.mappings[0].mapping_stream_index = 0;
    src.mappings[0].mapping_stream_channel = 0;
    src.mappings[0].mapping_cluster_offset = 0;
    src.mappings[0].mapping_cluster_channel = 1;
    src.mappings[1].mapping_stream_index = 0;
    src.mappings[1].mapping_stream_channel = 1;
    src.mappings[1].mapping_cluster_offset = 0;
    src.mappings[1].mapping_cluster_channel = 2;

    // Serialize via wire_span — exactly wire_size() bytes, header + 2 mappings.
    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), static_cast<size_t>(8 + (2 * 8)));

    // Round-trip: load the view into a fresh descriptor via span_load_padded,
    // which uses the source span's size (not the destination's wire_size)
    // to decide how many bytes to copy. This is the correct load primitive
    // for variable descriptors because we don't know the wire size until
    // the header fields have been populated.
    aem::DescriptorAudioMap dst{};
    span_load_padded(dst, view);

    EXPECT_EQ(dst.descriptor_index.get(), static_cast<uint16_t>(5));
    EXPECT_EQ(dst.number_of_mappings.get(), static_cast<uint16_t>(2));
    EXPECT_EQ(dst.mappings[0].mapping_cluster_channel.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(dst.mappings[1].mapping_cluster_channel.get(), static_cast<uint16_t>(2));

    // After the load, dst.wire_size() should match src.wire_size().
    EXPECT_EQ(dst.wire_size(), src.wire_size());
    EXPECT_EQ(dst.wire_size(), view.size());
}

// ---- Video Map -----------------------------------------------------------

TEST(aem_video_map, empty_and_populated_wire_size)
{
    aem::DescriptorVideoMap desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorVideoMap::LENGTH);

    desc.number_of_mappings = 4;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorVideoMap::LENGTH + (4 * sizeof(aem::VideoMapping)));
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(40));
}

TEST(aem_video_map, max_mappings_fits_in_aecp_budget)
{
    aem::DescriptorVideoMap desc{};
    desc.number_of_mappings = aem::DescriptorVideoMap::MAX_MAPPINGS;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
    EXPECT_EQ(sizeof(aem::VideoMapping), static_cast<size_t>(8));
}

TEST(aem_video_map, populated_map_round_trips_through_wire_span)
{
    aem::DescriptorVideoMap src{};
    src.descriptor_index = 2;
    src.number_of_mappings = 1;
    src.mappings[0].mapping_stream_index = 3;
    src.mappings[0].mapping_program_stream = 7;
    src.mappings[0].mapping_elementary_stream = 0x100;
    src.mappings[0].mapping_cluster_offset = 1;

    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), static_cast<size_t>(16));

    aem::DescriptorVideoMap dst{};
    span_load_padded(dst, view);
    EXPECT_EQ(dst.descriptor_index.get(), static_cast<uint16_t>(2));
    EXPECT_EQ(dst.number_of_mappings.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(dst.mappings[0].mapping_stream_index.get(), static_cast<uint16_t>(3));
    EXPECT_EQ(dst.mappings[0].mapping_program_stream.get(), static_cast<uint16_t>(7));
    EXPECT_EQ(dst.mappings[0].mapping_elementary_stream.get(), static_cast<uint16_t>(0x100));
    EXPECT_EQ(dst.mappings[0].mapping_cluster_offset.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(dst.wire_size(), src.wire_size());
}

// ---- Sensor Map ----------------------------------------------------------

TEST(aem_sensor_map, empty_and_populated_wire_size)
{
    aem::DescriptorSensorMap desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorSensorMap::LENGTH);

    desc.number_of_mappings = 5;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorSensorMap::LENGTH + (5 * sizeof(aem::SensorMapping)));
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(48));
}

TEST(aem_sensor_map, max_mappings_fits_in_aecp_budget)
{
    aem::DescriptorSensorMap desc{};
    desc.number_of_mappings = aem::DescriptorSensorMap::MAX_MAPPINGS;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
    EXPECT_EQ(sizeof(aem::SensorMapping), static_cast<size_t>(8));
}

TEST(aem_sensor_map, populated_map_round_trips_through_wire_span)
{
    aem::DescriptorSensorMap src{};
    src.descriptor_index = 11;
    src.number_of_mappings = 1;
    src.mappings[0].mapping_stream_index = 2;
    src.mappings[0].mapping_stream_channel = 4;
    src.mappings[0].mapping_cluster_offset = 1;
    src.mappings[0].mapping_cluster_channel = 3;

    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), static_cast<size_t>(16));

    aem::DescriptorSensorMap dst{};
    span_load_padded(dst, view);
    EXPECT_EQ(dst.descriptor_index.get(), static_cast<uint16_t>(11));
    EXPECT_EQ(dst.mappings[0].mapping_stream_channel.get(), static_cast<uint16_t>(4));
    EXPECT_EQ(dst.mappings[0].mapping_cluster_channel.get(), static_cast<uint16_t>(3));
    EXPECT_EQ(dst.wire_size(), src.wire_size());
}

// ---- DescriptorConfiguration ---------------------------------------------

TEST(aem_configuration, empty_has_header_wire_size)
{
    aem::DescriptorConfiguration const desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorConfiguration::LENGTH);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(74));
    EXPECT_EQ(desc.descriptor_counts_offset.get(), static_cast<uint16_t>(aem::DescriptorConfiguration::LENGTH));
}

TEST(aem_configuration, wire_size_scales_with_descriptor_counts)
{
    aem::DescriptorConfiguration desc{};
    desc.descriptor_counts_count = 5;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorConfiguration::LENGTH + (5 * sizeof(aem::DescriptorCountEntry)));
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(94));
}

TEST(aem_configuration, max_counts_fits_in_aecp_budget)
{
    aem::DescriptorConfiguration desc{};
    desc.descriptor_counts_count = aem::DescriptorConfiguration::MAX_DESCRIPTOR_COUNTS;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(506));  // 74 + 108*4
    EXPECT_EQ(sizeof(aem::DescriptorCountEntry), static_cast<size_t>(4));
}

TEST(aem_configuration, populated_counts_round_trip_through_wire_span)
{
    aem::DescriptorConfiguration src{};
    src.descriptor_index = 0;
    src.descriptor_counts_count = 3;
    src.descriptor_counts[0].descriptor_type = aem::DESCRIPTOR_AUDIO_UNIT;
    src.descriptor_counts[0].count = 1;
    src.descriptor_counts[1].descriptor_type = aem::DESCRIPTOR_STREAM_INPUT;
    src.descriptor_counts[1].count = 2;
    src.descriptor_counts[2].descriptor_type = aem::DESCRIPTOR_STREAM_OUTPUT;
    src.descriptor_counts[2].count = 2;

    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), aem::DescriptorConfiguration::LENGTH + (3 * sizeof(aem::DescriptorCountEntry)));

    aem::DescriptorConfiguration dst{};
    span_load_padded(dst, view);
    EXPECT_EQ(dst.descriptor_counts_count.get(), static_cast<uint16_t>(3));
    EXPECT_EQ(dst.descriptor_counts[0].descriptor_type.get(), aem::DESCRIPTOR_AUDIO_UNIT);
    EXPECT_EQ(dst.descriptor_counts[0].count.get(), static_cast<uint16_t>(1));
    EXPECT_EQ(dst.descriptor_counts[1].descriptor_type.get(), aem::DESCRIPTOR_STREAM_INPUT);
    EXPECT_EQ(dst.descriptor_counts[1].count.get(), static_cast<uint16_t>(2));
    EXPECT_EQ(dst.descriptor_counts[2].descriptor_type.get(), aem::DESCRIPTOR_STREAM_OUTPUT);
    EXPECT_EQ(dst.descriptor_counts[2].count.get(), static_cast<uint16_t>(2));
    EXPECT_EQ(dst.wire_size(), src.wire_size());
}

// ---- DescriptorAudioUnit -------------------------------------------------

TEST(aem_audio_unit, empty_wire_size)
{
    aem::DescriptorAudioUnit const desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorAudioUnit::LENGTH);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(144));
    EXPECT_EQ(desc.sampling_rates_offset.get(), static_cast<uint16_t>(aem::DescriptorAudioUnit::LENGTH));
}

TEST(aem_audio_unit, wire_size_scales_with_sampling_rates)
{
    aem::DescriptorAudioUnit desc{};
    desc.sampling_rates_count = 4;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorAudioUnit::LENGTH + (4 * sizeof(quadlet_t)));
}

TEST(aem_audio_unit, max_sampling_rates_fits_in_aecp_budget)
{
    aem::DescriptorAudioUnit desc{};
    desc.sampling_rates_count = aem::DescriptorAudioUnit::MAX_SAMPLING_RATES;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
}

TEST(aem_audio_unit, populated_round_trips_through_wire_span)
{
    aem::DescriptorAudioUnit src{};
    src.descriptor_index = 0;
    src.sampling_rates_count = 3;
    src.sampling_rates[0] = quadlet_t{48000};
    src.sampling_rates[1] = quadlet_t{96000};
    src.sampling_rates[2] = quadlet_t{192000};

    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), aem::DescriptorAudioUnit::LENGTH + (3 * sizeof(quadlet_t)));

    aem::DescriptorAudioUnit dst{};
    span_load_padded(dst, view);
    EXPECT_EQ(dst.sampling_rates_count.get(), static_cast<uint16_t>(3));
    EXPECT_EQ(dst.sampling_rates[0].get(), static_cast<uint32_t>(48000));
    EXPECT_EQ(dst.sampling_rates[1].get(), static_cast<uint32_t>(96000));
    EXPECT_EQ(dst.sampling_rates[2].get(), static_cast<uint32_t>(192000));
    EXPECT_EQ(dst.wire_size(), src.wire_size());
}

// ---- DescriptorStream ----------------------------------------------------

TEST(aem_stream, empty_wire_size)
{
    aem::DescriptorStream const desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorStream::LENGTH);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(138));
    EXPECT_EQ(desc.formats_offset.get(), static_cast<uint16_t>(aem::DescriptorStream::LENGTH));
}

TEST(aem_stream, wire_size_scales_with_number_of_formats)
{
    aem::DescriptorStream desc{};
    desc.number_of_formats = 2;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorStream::LENGTH + (2 * sizeof(Eui64)));
}

TEST(aem_stream, max_stream_formats_fits_in_aecp_budget)
{
    aem::DescriptorStream desc{};
    desc.number_of_formats = aem::DescriptorStream::MAX_STREAM_FORMATS;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
}

TEST(aem_stream, populated_round_trips_through_wire_span)
{
    aem::DescriptorStream src{};
    src.descriptor_type = aem::DESCRIPTOR_STREAM_INPUT;
    src.descriptor_index = 0;
    src.number_of_formats = 2;
    src.stream_formats[0] = Eui64{0x00, 0xA0, 0x20, 0x41, 0x00, 0x00, 0x00, 0x00};
    src.stream_formats[1] = Eui64{0x00, 0xA0, 0x20, 0x42, 0x00, 0x00, 0x00, 0x00};

    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), aem::DescriptorStream::LENGTH + (2 * sizeof(Eui64)));

    aem::DescriptorStream dst{};
    span_load_padded(dst, view);
    EXPECT_EQ(dst.number_of_formats.get(), static_cast<uint16_t>(2));
    EXPECT_TRUE(dst.stream_formats[0] == src.stream_formats[0]);
    EXPECT_TRUE(dst.stream_formats[1] == src.stream_formats[1]);
    EXPECT_EQ(dst.wire_size(), src.wire_size());
}

// ---- DescriptorClockDomain -----------------------------------------------

TEST(aem_clock_domain, empty_wire_size)
{
    aem::DescriptorClockDomain const desc{};
    EXPECT_EQ(desc.wire_size(), aem::DescriptorClockDomain::LENGTH);
    EXPECT_EQ(desc.wire_size(), static_cast<size_t>(76));
    EXPECT_EQ(desc.clock_sources_offset.get(), static_cast<uint16_t>(aem::DescriptorClockDomain::LENGTH));
}

TEST(aem_clock_domain, wire_size_scales_with_clock_sources)
{
    aem::DescriptorClockDomain desc{};
    desc.clock_sources_count = 5;
    EXPECT_EQ(desc.wire_size(), aem::DescriptorClockDomain::LENGTH + (5 * sizeof(doublet_t)));
}

TEST(aem_clock_domain, max_clock_sources_fits_in_aecp_budget)
{
    aem::DescriptorClockDomain desc{};
    desc.clock_sources_count = aem::DescriptorClockDomain::MAX_CLOCK_SOURCES;
    EXPECT_TRUE(desc.wire_size() <= MAX_AEM_DESCRIPTOR_SIZE);
}

TEST(aem_clock_domain, populated_round_trips_through_wire_span)
{
    aem::DescriptorClockDomain src{};
    src.clock_source_index = 0;
    src.clock_sources_count = 3;
    src.clock_sources[0] = 0;
    src.clock_sources[1] = 1;
    src.clock_sources[2] = 2;

    auto const view = wire_span(src);
    EXPECT_EQ(view.size(), aem::DescriptorClockDomain::LENGTH + (3 * sizeof(doublet_t)));

    aem::DescriptorClockDomain dst{};
    span_load_padded(dst, view);
    EXPECT_EQ(dst.clock_sources_count.get(), static_cast<uint16_t>(3));
    EXPECT_EQ(dst.clock_sources[0].get(), static_cast<uint16_t>(0));
    EXPECT_EQ(dst.clock_sources[1].get(), static_cast<uint16_t>(1));
    EXPECT_EQ(dst.clock_sources[2].get(), static_cast<uint16_t>(2));
    EXPECT_EQ(dst.wire_size(), src.wire_size());
}

// ---- Fixed-in-practice wire_size() for Unit / Interface descriptors ------

TEST(aem_units_fixed_wire_size, video_unit_sensor_unit_avb_interface)
{
    // These descriptors have a conceptual variable trailer per IEEE 1722.1
    // but the current C++ struct models only the fixed header, so
    // wire_size() == LENGTH until the inline representation is extended.
    EXPECT_EQ(aem::DescriptorVideoUnit::wire_size(), aem::DescriptorVideoUnit::LENGTH);
    EXPECT_EQ(aem::DescriptorSensorUnit::wire_size(), aem::DescriptorSensorUnit::LENGTH);
    EXPECT_EQ(aem::DescriptorAvbInterface::wire_size(), aem::DescriptorAvbInterface::LENGTH);
}

//
// Test Runner
//

TEST_MAIN(statusbar_atdecc, atdecc_test)