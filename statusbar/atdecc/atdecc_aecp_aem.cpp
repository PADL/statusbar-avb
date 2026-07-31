// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aecp_aem.hpp"

#include <string_view>

namespace statusbar::atdecc {

auto aem_status_name(uint8_t const status) noexcept -> std::string_view
{
    switch (status) {
        case AEM_STATUS_SUCCESS:
            return "Success";
        case AEM_STATUS_NOT_IMPLEMENTED:
            return "Not Implemented";
        case AEM_STATUS_NO_SUCH_DESCRIPTOR:
            return "No Such Descriptor";
        case AEM_STATUS_ENTITY_LOCKED:
            return "Entity Locked";
        case AEM_STATUS_ENTITY_ACQUIRED:
            return "Entity Acquired";
        case AEM_STATUS_NOT_AUTHENTICATED:
            return "Not Authenticated";
        case AEM_STATUS_AUTHENTICATION_DISABLED:
            return "Authentication Disabled";
        case AEM_STATUS_BAD_ARGUMENTS:
            return "Bad Arguments";
        case AEM_STATUS_NO_RESOURCES:
            return "No Resources";
        case AEM_STATUS_IN_PROGRESS:
            return "In Progress";
        case AEM_STATUS_ENTITY_MISBEHAVING:
            return "Entity Misbehaving";
        case AEM_STATUS_NOT_SUPPORTED:
            return "Not Supported";
        case AEM_STATUS_STREAM_IS_RUNNING:
            return "Stream Is Running";
        default:
            return "Unknown";
    }
}

auto aem_command_name(uint16_t const cmd) noexcept -> std::string_view
{
    switch (cmd & 0x7FFF) {  // Mask off U bit
        case AEM_COMMAND_ACQUIRE_ENTITY:
            return "ACQUIRE_ENTITY";
        case AEM_COMMAND_LOCK_ENTITY:
            return "LOCK_ENTITY";
        case AEM_COMMAND_ENTITY_AVAILABLE:
            return "ENTITY_AVAILABLE";
        case AEM_COMMAND_CONTROLLER_AVAILABLE:
            return "CONTROLLER_AVAILABLE";
        case AEM_COMMAND_READ_DESCRIPTOR:
            return "READ_DESCRIPTOR";
        case AEM_COMMAND_WRITE_DESCRIPTOR:
            return "WRITE_DESCRIPTOR";
        case AEM_COMMAND_SET_CONFIGURATION:
            return "SET_CONFIGURATION";
        case AEM_COMMAND_GET_CONFIGURATION:
            return "GET_CONFIGURATION";
        case AEM_COMMAND_SET_STREAM_FORMAT:
            return "SET_STREAM_FORMAT";
        case AEM_COMMAND_GET_STREAM_FORMAT:
            return "GET_STREAM_FORMAT";
        case AEM_COMMAND_SET_VIDEO_FORMAT:
            return "SET_VIDEO_FORMAT";
        case AEM_COMMAND_GET_VIDEO_FORMAT:
            return "GET_VIDEO_FORMAT";
        case AEM_COMMAND_SET_SENSOR_FORMAT:
            return "SET_SENSOR_FORMAT";
        case AEM_COMMAND_GET_SENSOR_FORMAT:
            return "GET_SENSOR_FORMAT";
        case AEM_COMMAND_SET_STREAM_INFO:
            return "SET_STREAM_INFO";
        case AEM_COMMAND_GET_STREAM_INFO:
            return "GET_STREAM_INFO";
        case AEM_COMMAND_SET_NAME:
            return "SET_NAME";
        case AEM_COMMAND_GET_NAME:
            return "GET_NAME";
        case AEM_COMMAND_SET_ASSOCIATION_ID:
            return "SET_ASSOCIATION_ID";
        case AEM_COMMAND_GET_ASSOCIATION_ID:
            return "GET_ASSOCIATION_ID";
        case AEM_COMMAND_SET_SAMPLING_RATE:
            return "SET_SAMPLING_RATE";
        case AEM_COMMAND_GET_SAMPLING_RATE:
            return "GET_SAMPLING_RATE";
        case AEM_COMMAND_SET_CLOCK_SOURCE:
            return "SET_CLOCK_SOURCE";
        case AEM_COMMAND_GET_CLOCK_SOURCE:
            return "GET_CLOCK_SOURCE";
        case AEM_COMMAND_SET_CONTROL:
            return "SET_CONTROL";
        case AEM_COMMAND_GET_CONTROL:
            return "GET_CONTROL";
        case AEM_COMMAND_INCREMENT_CONTROL:
            return "INCREMENT_CONTROL";
        case AEM_COMMAND_DECREMENT_CONTROL:
            return "DECREMENT_CONTROL";
        case AEM_COMMAND_SET_SIGNAL_SELECTOR:
            return "SET_SIGNAL_SELECTOR";
        case AEM_COMMAND_GET_SIGNAL_SELECTOR:
            return "GET_SIGNAL_SELECTOR";
        case AEM_COMMAND_SET_MIXER:
            return "SET_MIXER";
        case AEM_COMMAND_GET_MIXER:
            return "GET_MIXER";
        case AEM_COMMAND_SET_MATRIX:
            return "SET_MATRIX";
        case AEM_COMMAND_GET_MATRIX:
            return "GET_MATRIX";
        case AEM_COMMAND_START_STREAMING:
            return "START_STREAMING";
        case AEM_COMMAND_STOP_STREAMING:
            return "STOP_STREAMING";
        case AEM_COMMAND_REGISTER_UNSOLICITED_NOTIFICATION:
            return "REGISTER_UNSOLICITED_NOTIFICATION";
        case AEM_COMMAND_DEREGISTER_UNSOLICITED_NOTIFICATION:
            return "DEREGISTER_UNSOLICITED_NOTIFICATION";
        case AEM_COMMAND_IDENTIFY_NOTIFICATION:
            return "IDENTIFY_NOTIFICATION";
        case AEM_COMMAND_GET_AVB_INFO:
            return "GET_AVB_INFO";
        case AEM_COMMAND_GET_AS_PATH:
            return "GET_AS_PATH";
        case AEM_COMMAND_GET_COUNTERS:
            return "GET_COUNTERS";
        case AEM_COMMAND_REBOOT:
            return "REBOOT";
        case AEM_COMMAND_GET_AUDIO_MAP:
            return "GET_AUDIO_MAP";
        case AEM_COMMAND_ADD_AUDIO_MAPPINGS:
            return "ADD_AUDIO_MAPPINGS";
        case AEM_COMMAND_REMOVE_AUDIO_MAPPINGS:
            return "REMOVE_AUDIO_MAPPINGS";
        case AEM_COMMAND_GET_VIDEO_MAP:
            return "GET_VIDEO_MAP";
        case AEM_COMMAND_ADD_VIDEO_MAPPINGS:
            return "ADD_VIDEO_MAPPINGS";
        case AEM_COMMAND_REMOVE_VIDEO_MAPPINGS:
            return "REMOVE_VIDEO_MAPPINGS";
        case AEM_COMMAND_GET_SENSOR_MAP:
            return "GET_SENSOR_MAP";
        case AEM_COMMAND_ADD_SENSOR_MAPPINGS:
            return "ADD_SENSOR_MAPPINGS";
        case AEM_COMMAND_REMOVE_SENSOR_MAPPINGS:
            return "REMOVE_SENSOR_MAPPINGS";
        case AEM_COMMAND_START_OPERATION:
            return "START_OPERATION";
        case AEM_COMMAND_ABORT_OPERATION:
            return "ABORT_OPERATION";
        case AEM_COMMAND_OPERATION_STATUS:
            return "OPERATION_STATUS";
        case AEM_COMMAND_AUTH_ADD_KEY:
            return "AUTH_ADD_KEY";
        case AEM_COMMAND_AUTH_DELETE_KEY:
            return "AUTH_DELETE_KEY";
        case AEM_COMMAND_AUTH_GET_KEY_LIST:
            return "AUTH_GET_KEY_LIST";
        case AEM_COMMAND_AUTH_GET_KEY:
            return "AUTH_GET_KEY";
        case AEM_COMMAND_AUTH_ADD_KEY_TO_CHAIN:
            return "AUTH_ADD_KEY_TO_CHAIN";
        case AEM_COMMAND_AUTH_DELETE_KEY_FROM_CHAIN:
            return "AUTH_DELETE_KEY_FROM_CHAIN";
        case AEM_COMMAND_AUTH_GET_KEYCHAIN_LIST:
            return "AUTH_GET_KEYCHAIN_LIST";
        case AEM_COMMAND_AUTH_GET_IDENTITY:
            return "AUTH_GET_IDENTITY";
        case AEM_COMMAND_AUTH_ADD_TOKEN:
            return "AUTH_ADD_TOKEN";
        case AEM_COMMAND_AUTH_DELETE_TOKEN:
            return "AUTH_DELETE_TOKEN";
        case AEM_COMMAND_AUTHENTICATE:
            return "AUTHENTICATE";
        case AEM_COMMAND_DEAUTHENTICATE:
            return "DEAUTHENTICATE";
        case AEM_COMMAND_ENABLE_TRANSPORT_SECURITY:
            return "ENABLE_TRANSPORT_SECURITY";
        case AEM_COMMAND_DISABLE_TRANSPORT_SECURITY:
            return "DISABLE_TRANSPORT_SECURITY";
        case AEM_COMMAND_ENABLE_STREAM_ENCRYPTION:
            return "ENABLE_STREAM_ENCRYPTION";
        case AEM_COMMAND_DISABLE_STREAM_ENCRYPTION:
            return "DISABLE_STREAM_ENCRYPTION";
        case AEM_COMMAND_SET_MEMORY_OBJECT_LENGTH:
            return "SET_MEMORY_OBJECT_LENGTH";
        case AEM_COMMAND_GET_MEMORY_OBJECT_LENGTH:
            return "GET_MEMORY_OBJECT_LENGTH";
        case AEM_COMMAND_SET_STREAM_BACKUP:
            return "SET_STREAM_BACKUP";
        case AEM_COMMAND_GET_STREAM_BACKUP:
            return "GET_STREAM_BACKUP";
        case AEM_COMMAND_GET_DYNAMIC_INFO:
            return "GET_DYNAMIC_INFO";
        case AEM_COMMAND_SET_MAX_TRANSIT_TIME:
            return "SET_MAX_TRANSIT_TIME";
        case AEM_COMMAND_GET_MAX_TRANSIT_TIME:
            return "GET_MAX_TRANSIT_TIME";
        case AEM_COMMAND_SET_SAMPLING_RATE_RANGE:
            return "SET_SAMPLING_RATE_RANGE";
        case AEM_COMMAND_GET_SAMPLING_RATE_RANGE:
            return "GET_SAMPLING_RATE_RANGE";
        case AEM_COMMAND_SET_PTP_INSTANCE_INFO:
            return "SET_PTP_INSTANCE_INFO";
        case AEM_COMMAND_GET_PTP_INSTANCE_INFO:
            return "GET_PTP_INSTANCE_INFO";
        case AEM_COMMAND_GET_PTP_INSTANCE_EXTENDED_INFO:
            return "GET_PTP_INSTANCE_EXTENDED_INFO";
        case AEM_COMMAND_GET_PTP_INSTANCE_GRANDMASTER_INFO:
            return "GET_PTP_INSTANCE_GRANDMASTER_INFO";
        case AEM_COMMAND_GET_PTP_INSTANCE_PATH_COUNT:
            return "GET_PTP_INSTANCE_PATH_COUNT";
        case AEM_COMMAND_GET_PTP_INSTANCE_PATH_TRACE:
            return "GET_PTP_INSTANCE_PATH_TRACE";
        case AEM_COMMAND_GET_PTP_INSTANCE_PERF_MON_COUNT:
            return "GET_PTP_INSTANCE_PERF_MON_COUNT";
        case AEM_COMMAND_GET_PTP_INSTANCE_PERF_MON_RECORD:
            return "GET_PTP_INSTANCE_PERF_MON_RECORD";
        case AEM_COMMAND_SET_PTP_PORT_INITIAL_INTERVALS:
            return "SET_PTP_PORT_INITIAL_INTERVALS";
        case AEM_COMMAND_GET_PTP_PORT_INITIAL_INTERVALS:
            return "GET_PTP_PORT_INITIAL_INTERVALS";
        case AEM_COMMAND_GET_PTP_PORT_CURRENT_INTERVALS:
            return "GET_PTP_PORT_CURRENT_INTERVALS";
        case AEM_COMMAND_SET_PTP_PORT_REMOTE_INTERVALS:
            return "SET_PTP_PORT_REMOTE_INTERVALS";
        case AEM_COMMAND_GET_PTP_PORT_REMOTE_INTERVALS:
            return "GET_PTP_PORT_REMOTE_INTERVALS";
        case AEM_COMMAND_SET_PTP_PORT_INFO:
            return "SET_PTP_PORT_INFO";
        case AEM_COMMAND_GET_PTP_PORT_INFO:
            return "GET_PTP_PORT_INFO";
        case AEM_COMMAND_SET_PTP_PORT_OVERRIDES:
            return "SET_PTP_PORT_OVERRIDES";
        case AEM_COMMAND_GET_PTP_PORT_OVERRIDES:
            return "GET_PTP_PORT_OVERRIDES";
        case AEM_COMMAND_GET_PTP_PORT_PDELAY_MON_COUNT:
            return "GET_PTP_PORT_PDELAY_MON_COUNT";
        case AEM_COMMAND_GET_PTP_PORT_PDELAY_MON_RECORD:
            return "GET_PTP_PORT_PDELAY_MON_RECORD";
        case AEM_COMMAND_GET_PTP_PORT_PERF_MON_COUNT:
            return "GET_PTP_PORT_PERF_MON_COUNT";
        case AEM_COMMAND_GET_PTP_PORT_PERF_MON_RECORD:
            return "GET_PTP_PORT_PERF_MON_RECORD";
        case AEM_COMMAND_GET_PATH_LATENCY:
            return "GET_PATH_LATENCY";
        case AEM_COMMAND_AUTH_GET_NONCE:
            return "AUTH_GET_NONCE";
        case AEM_COMMAND_AUTH_ADD_KEY_NONCE:
            return "AUTH_ADD_KEY_NONCE";
        case AEM_COMMAND_EXPANSION:
            return "EXPANSION";
        default:
            return "Unknown";
    }
}

}  // namespace statusbar::atdecc
