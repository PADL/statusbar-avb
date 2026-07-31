#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// ACMP Message type constants, status codes, flags, and name functions
/// IEEE 1722.1 Clause 8

#include "statusbar/avtp/avtp.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace statusbar::atdecc {

//
// ACMP Constants - IEEE 1722.1 Clause 8
//
namespace AvtpSubtype = avtp::AvtpSubtype;

/// ACMP Message Types - Clause 8.2.1.5
constexpr uint8_t ACMP_MESSAGE_TYPE_CONNECT_TX_COMMAND = 0;
constexpr uint8_t ACMP_MESSAGE_TYPE_CONNECT_TX_RESPONSE = 1;
constexpr uint8_t ACMP_MESSAGE_TYPE_DISCONNECT_TX_COMMAND = 2;
constexpr uint8_t ACMP_MESSAGE_TYPE_DISCONNECT_TX_RESPONSE = 3;
constexpr uint8_t ACMP_MESSAGE_TYPE_GET_TX_STATE_COMMAND = 4;
constexpr uint8_t ACMP_MESSAGE_TYPE_GET_TX_STATE_RESPONSE = 5;
constexpr uint8_t ACMP_MESSAGE_TYPE_CONNECT_RX_COMMAND = 6;
constexpr uint8_t ACMP_MESSAGE_TYPE_CONNECT_RX_RESPONSE = 7;
constexpr uint8_t ACMP_MESSAGE_TYPE_DISCONNECT_RX_COMMAND = 8;
constexpr uint8_t ACMP_MESSAGE_TYPE_DISCONNECT_RX_RESPONSE = 9;
constexpr uint8_t ACMP_MESSAGE_TYPE_GET_RX_STATE_COMMAND = 10;
constexpr uint8_t ACMP_MESSAGE_TYPE_GET_RX_STATE_RESPONSE = 11;
constexpr uint8_t ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_COMMAND = 12;
constexpr uint8_t ACMP_MESSAGE_TYPE_GET_TX_CONNECTION_RESPONSE = 13;

/// Get human-readable name for ACMP message type
/// @param type ACMP message type code (0-13)
[[nodiscard]] auto acmp_message_type_name(uint8_t type) noexcept -> std::string_view;

//
// ACMP Status Codes - Clause 8.2.1.6
//
constexpr uint8_t ACMP_STATUS_SUCCESS = 0;
constexpr uint8_t ACMP_STATUS_LISTENER_UNKNOWN_ID = 1;
constexpr uint8_t ACMP_STATUS_TALKER_UNKNOWN_ID = 2;
constexpr uint8_t ACMP_STATUS_TALKER_DEST_MAC_FAIL = 3;
constexpr uint8_t ACMP_STATUS_TALKER_NO_STREAM_INDEX = 4;
constexpr uint8_t ACMP_STATUS_TALKER_NO_BANDWIDTH = 5;
constexpr uint8_t ACMP_STATUS_TALKER_EXCLUSIVE = 6;
constexpr uint8_t ACMP_STATUS_LISTENER_TALKER_TIMEOUT = 7;
constexpr uint8_t ACMP_STATUS_LISTENER_EXCLUSIVE = 8;
constexpr uint8_t ACMP_STATUS_STATE_UNAVAILABLE = 9;
constexpr uint8_t ACMP_STATUS_NOT_CONNECTED = 10;
constexpr uint8_t ACMP_STATUS_NO_SUCH_CONNECTION = 11;
constexpr uint8_t ACMP_STATUS_COULD_NOT_SEND_MESSAGE = 12;
constexpr uint8_t ACMP_STATUS_TALKER_MISBEHAVING = 13;
constexpr uint8_t ACMP_STATUS_LISTENER_MISBEHAVING = 14;
constexpr uint8_t ACMP_STATUS_RESERVED = 15;
constexpr uint8_t ACMP_STATUS_CONTROLLER_NOT_AUTHORIZED = 16;
constexpr uint8_t ACMP_STATUS_INCOMPATIBLE_REQUEST = 17;
constexpr uint8_t ACMP_STATUS_LISTENER_INVALID_CONNECTION = 18;
constexpr uint8_t ACMP_STATUS_LISTENER_CAN_ONLY_LISTEN_ONCE = 19;
constexpr uint8_t ACMP_STATUS_NOT_SUPPORTED = 31;

/// Get human-readable name for ACMP status code
/// @param status ACMP status code
[[nodiscard]] auto acmp_status_name(uint8_t status) noexcept -> std::string_view;

//
// ACMP Flags - Clause 8.2.1.16 (IEEE 1722.1-2021)
//
namespace acmp_flags {
constexpr uint16_t CLASS_B = 0x0001;                  // Bit 15: Stream is Class B (default Class A)
constexpr uint16_t FAST_CONNECT = 0x0002;             // Bit 14: Deprecated in IEEE 1722.1-2021
constexpr uint16_t SAVED_STATE = 0x0004;              // Bit 13: Deprecated in IEEE 1722.1-2021
constexpr uint16_t STREAMING_WAIT = 0x0008;           // Bit 12: Wait for control protocol before streaming
constexpr uint16_t SUPPORTS_ENCRYPTED = 0x0010;       // Bit 11: Stream supports encrypted PDUs
constexpr uint16_t ENCRYPTED_PDU = 0x0020;            // Bit 10: Stream is using encrypted PDUs
constexpr uint16_t SRP_REGISTRATION_FAILED = 0x0040;  // Bit 9: SRP registration failed (Get State only)
constexpr uint16_t TALKER_FAILED = 0x0040;            // Bit 9: Alias for SRP_REGISTRATION_FAILED
constexpr uint16_t CL_ENTRIES_VALID = 0x0080;         // Bit 8: connected_listeners_entries field is valid
constexpr uint16_t NO_SRP = 0x0100;                   // Bit 7: SRP not being used for stream
constexpr uint16_t UDP = 0x0200;                      // Bit 6: Stream uses UDP transport (not Layer 2)
}  // namespace acmp_flags

//
// ACMP Timeouts - Clause 8.2.2
//
constexpr uint32_t ACMP_TIMEOUT_CONNECT_TX_COMMAND_MS = 2000;
constexpr uint32_t ACMP_TIMEOUT_DISCONNECT_TX_COMMAND_MS = 200;
constexpr uint32_t ACMP_TIMEOUT_GET_TX_STATE_COMMAND_MS = 200;
constexpr uint32_t ACMP_TIMEOUT_CONNECT_RX_COMMAND_MS = 4500;
constexpr uint32_t ACMP_TIMEOUT_DISCONNECT_RX_COMMAND_MS = 500;
constexpr uint32_t ACMP_TIMEOUT_GET_RX_STATE_COMMAND_MS = 200;
constexpr uint32_t ACMP_TIMEOUT_GET_TX_CONNECTION_COMMAND_MS = 200;

//
// ACMP Timeout Helpers
//
/// Get timeout duration for a given message type
/// @param message_type ACMP message type code
[[nodiscard]] auto acmp_timeout_for_message_type(uint8_t message_type) noexcept -> std::chrono::milliseconds;

}  // namespace statusbar::atdecc
