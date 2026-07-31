// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"

#include <string_view>

namespace statusbar::atdecc::aem {

auto is_standard_control_type(Eui64 const& t) noexcept -> bool
{
    auto const s = t.span();
    return s[0] == STANDARD_CONTROL_TYPE_OUI_B0 && s[1] == STANDARD_CONTROL_TYPE_OUI_B1 && s[2] == STANDARD_CONTROL_TYPE_OUI_B2;
}

auto control_type_name(Eui64 const& t) noexcept -> std::string_view
{
    if (!is_standard_control_type(t)) {
        return "VENDOR_DEFINED";
    }
    // Below the OUI we compare the low 40 bits; the high 3 bytes are
    // fixed by is_standard_control_type().
    auto const s = t.span();
    uint64_t const key = (static_cast<uint64_t>(s[3]) << 32) | (static_cast<uint64_t>(s[4]) << 24) |
        (static_cast<uint64_t>(s[5]) << 16) | (static_cast<uint64_t>(s[6]) << 8) | static_cast<uint64_t>(s[7]);

    switch (key) {
        // General (category 0x0000)
        case 0x0000000000ULL:
            return "ENABLE";
        case 0x0000000001ULL:
            return "IDENTIFY";
        case 0x0000000002ULL:
            return "MUTE";
        case 0x0000000003ULL:
            return "INVERT";
        case 0x0000000004ULL:
            return "GAIN";
        case 0x0000000005ULL:
            return "ATTENUATE";
        case 0x0000000006ULL:
            return "DELAY";
        case 0x0000000007ULL:
            return "SRC_MODE";
        case 0x0000000008ULL:
            return "SNAPSHOT";
        case 0x0000000009ULL:
            return "POW_LINE_FREQ";
        case 0x000000000AULL:
            return "POWER_STATUS";
        case 0x000000000BULL:
            return "FAN_STATUS";
        case 0x000000000CULL:
            return "TEMPERATURE";
        case 0x000000000DULL:
            return "ALTITUDE";
        case 0x000000000EULL:
            return "ABSOLUTE_HUMIDITY";
        case 0x000000000FULL:
            return "RELATIVE_HUMIDITY";
        case 0x0000000010ULL:
            return "ORIENTATION";
        case 0x0000000011ULL:
            return "VELOCITY";
        case 0x0000000012ULL:
            return "ACCELERATION";
        case 0x0000000013ULL:
            return "FILTER_RESPONSE";
        case 0x0000000014ULL:
            return "BAROMETRIC_PRESSURE";
        case 0x0000000015ULL:
            return "MANUFACTURER_URL";
        case 0x0000000016ULL:
            return "ENTITY_URL";
        case 0x0000000017ULL:
            return "CONFIGURATION_URL";
        case 0x0000000018ULL:
            return "GENERIC_URL";
        case 0x0000000019ULL:
            return "FAULT";
        case 0x000000001AULL:
            return "CONTROLLER_TARGET_ENTITY";
        case 0x000000001BULL:
            return "CONTROLLER_TARGET_OBJECT";
        case 0x000000001CULL:
            return "LATENCY_COMPENSATION";

        // Audio (category 0x0001)
        case 0x0000010000ULL:
            return "PANPOT";
        case 0x0000010001ULL:
            return "PHANTOM";
        case 0x0000010002ULL:
            return "AUDIO_SCALE";
        case 0x0000010003ULL:
            return "AUDIO_METERS";
        case 0x0000010004ULL:
            return "AUDIO_SPECTRUM";

        // Video (category 0x0002)
        case 0x0000020000ULL:
            return "SCANNING_MODE";
        case 0x0000020001ULL:
            return "AUTO_EXP_MODE";
        case 0x0000020002ULL:
            return "AUTO_EXP_PRIO";
        case 0x0000020003ULL:
            return "EXP_TIME";
        case 0x0000020004ULL:
            return "FOCUS";
        case 0x0000020005ULL:
            return "FOCUS_AUTO";
        case 0x0000020006ULL:
            return "IRIS";
        case 0x0000020007ULL:
            return "ZOOM";
        case 0x0000020008ULL:
            return "PRIVACY";
        case 0x0000020009ULL:
            return "BACKLIGHT";
        case 0x000002000AULL:
            return "BRIGHTNESS";
        case 0x000002000BULL:
            return "CONTRAST";
        case 0x000002000CULL:
            return "HUE";
        case 0x000002000DULL:
            return "SATURATION";
        case 0x000002000EULL:
            return "SHARPNESS";
        case 0x000002000FULL:
            return "GAMMA";
        case 0x0000020010ULL:
            return "WHITE_BAL_TEMP";
        case 0x0000020011ULL:
            return "WHITE_BAL_TEMP_AUTO";
        case 0x0000020012ULL:
            return "WHITE_BAL_COMP";
        case 0x0000020013ULL:
            return "WHITE_BAL_COMP_AUTO";
        case 0x0000020014ULL:
            return "DIGITAL_ZOOM";

        // Media (category 0x0003)
        case 0x0000030000ULL:
            return "MEDIA_PLAYLIST";
        case 0x0000030001ULL:
            return "MEDIA_PLAYLIST_NAME";
        case 0x0000030002ULL:
            return "MEDIA_DISK";
        case 0x0000030003ULL:
            return "MEDIA_DISK_NAME";
        case 0x0000030004ULL:
            return "MEDIA_TRACK";
        case 0x0000030005ULL:
            return "MEDIA_TRACK_NAME";
        case 0x0000030006ULL:
            return "MEDIA_SPEED";
        case 0x0000030007ULL:
            return "MEDIA_SAMPLE_POSITION";
        case 0x0000030008ULL:
            return "MEDIA_PLAYBACK_TRANSPORT";
        case 0x0000030009ULL:
            return "MEDIA_RECORD_TRANSPORT";

        // Radio (category 0x0004)
        case 0x0000040000ULL:
            return "FREQUENCY";
        case 0x0000040001ULL:
            return "MODULATION";
        case 0x0000040002ULL:
            return "POLARIZATION";

        // Serial (category 0x0005)
        case 0x0000050000ULL:
            return "BAUD_RATE";
        case 0x0000050001ULL:
            return "BIT_WIDTH";
        case 0x0000050002ULL:
            return "PARITY";
        case 0x0000050003ULL:
            return "STOP_BITS";

        // Network (category 0x0006)
        case 0x0000060000ULL:
            return "INTERFACE_OPERATIONAL";
        case 0x0000060001ULL:
            return "INTERFACE_MEDIA_OPTIONS";
        case 0x0000060002ULL:
            return "INTERFACE_MEDIA_STATUS";
        case 0x0000060003ULL:
            return "INTERFACE_NETWORK_NAME";
        case 0x0000060004ULL:
            return "FQTSS_DELTA_BANDWIDTH";
        case 0x0000060005ULL:
            return "FQTSS_ADMIN_IDLE_SLOPE";
        case 0x0000060006ULL:
            return "FQTSS_OPER_IDLE_SLOPE";
        case 0x0000060007ULL:
            return "FQTSS_PORT_TRANSMIT_RATE";
        case 0x0000060008ULL:
            return "FQTSS_CLASS_MEASUREMENT_INTERVAL";
        case 0x0000060009ULL:
            return "FQTSS_LOCK_CLASS_BANDWIDTH";

        default:
            return "UNKNOWN";
    }
}

}  // namespace statusbar::atdecc::aem
