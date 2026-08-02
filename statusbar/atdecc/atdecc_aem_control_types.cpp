// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_control_types.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace statusbar::atdecc::aem {

namespace {

/// IEEE 1722.1-2021 Table 7.4, sorted ascending by EUI-64 (the header
/// declares the constants in numeric order; the static_assert below
/// keeps the table honest if entries are added out of order).
constexpr std::array<ControlTypeEntry, 82> STANDARD_CONTROL_TYPES{{
    {.type = CONTROL_TYPE_ENABLE, .name = "ENABLE"},
    {.type = CONTROL_TYPE_IDENTIFY, .name = "IDENTIFY"},
    {.type = CONTROL_TYPE_MUTE, .name = "MUTE"},
    {.type = CONTROL_TYPE_INVERT, .name = "INVERT"},
    {.type = CONTROL_TYPE_GAIN, .name = "GAIN"},
    {.type = CONTROL_TYPE_ATTENUATE, .name = "ATTENUATE"},
    {.type = CONTROL_TYPE_DELAY, .name = "DELAY"},
    {.type = CONTROL_TYPE_SRC_MODE, .name = "SRC_MODE"},
    {.type = CONTROL_TYPE_SNAPSHOT, .name = "SNAPSHOT"},
    {.type = CONTROL_TYPE_POW_LINE_FREQ, .name = "POW_LINE_FREQ"},
    {.type = CONTROL_TYPE_POWER_STATUS, .name = "POWER_STATUS"},
    {.type = CONTROL_TYPE_FAN_STATUS, .name = "FAN_STATUS"},
    {.type = CONTROL_TYPE_TEMPERATURE, .name = "TEMPERATURE"},
    {.type = CONTROL_TYPE_ALTITUDE, .name = "ALTITUDE"},
    {.type = CONTROL_TYPE_ABSOLUTE_HUMIDITY, .name = "ABSOLUTE_HUMIDITY"},
    {.type = CONTROL_TYPE_RELATIVE_HUMIDITY, .name = "RELATIVE_HUMIDITY"},
    {.type = CONTROL_TYPE_ORIENTATION, .name = "ORIENTATION"},
    {.type = CONTROL_TYPE_VELOCITY, .name = "VELOCITY"},
    {.type = CONTROL_TYPE_ACCELERATION, .name = "ACCELERATION"},
    {.type = CONTROL_TYPE_FILTER_RESPONSE, .name = "FILTER_RESPONSE"},
    {.type = CONTROL_TYPE_BAROMETRIC_PRESSURE, .name = "BAROMETRIC_PRESSURE"},
    {.type = CONTROL_TYPE_MANUFACTURER_URL, .name = "MANUFACTURER_URL"},
    {.type = CONTROL_TYPE_ENTITY_URL, .name = "ENTITY_URL"},
    {.type = CONTROL_TYPE_CONFIGURATION_URL, .name = "CONFIGURATION_URL"},
    {.type = CONTROL_TYPE_GENERIC_URL, .name = "GENERIC_URL"},
    {.type = CONTROL_TYPE_FAULT, .name = "FAULT"},
    {.type = CONTROL_TYPE_CONTROLLER_TARGET_ENTITY, .name = "CONTROLLER_TARGET_ENTITY"},
    {.type = CONTROL_TYPE_CONTROLLER_TARGET_OBJECT, .name = "CONTROLLER_TARGET_OBJECT"},
    {.type = CONTROL_TYPE_LATENCY_COMPENSATION, .name = "LATENCY_COMPENSATION"},
    {.type = CONTROL_TYPE_PANPOT, .name = "PANPOT"},
    {.type = CONTROL_TYPE_PHANTOM, .name = "PHANTOM"},
    {.type = CONTROL_TYPE_AUDIO_SCALE, .name = "AUDIO_SCALE"},
    {.type = CONTROL_TYPE_AUDIO_METERS, .name = "AUDIO_METERS"},
    {.type = CONTROL_TYPE_AUDIO_SPECTRUM, .name = "AUDIO_SPECTRUM"},
    {.type = CONTROL_TYPE_SCANNING_MODE, .name = "SCANNING_MODE"},
    {.type = CONTROL_TYPE_AUTO_EXP_MODE, .name = "AUTO_EXP_MODE"},
    {.type = CONTROL_TYPE_AUTO_EXP_PRIO, .name = "AUTO_EXP_PRIO"},
    {.type = CONTROL_TYPE_EXP_TIME, .name = "EXP_TIME"},
    {.type = CONTROL_TYPE_FOCUS, .name = "FOCUS"},
    {.type = CONTROL_TYPE_FOCUS_AUTO, .name = "FOCUS_AUTO"},
    {.type = CONTROL_TYPE_IRIS, .name = "IRIS"},
    {.type = CONTROL_TYPE_ZOOM, .name = "ZOOM"},
    {.type = CONTROL_TYPE_PRIVACY, .name = "PRIVACY"},
    {.type = CONTROL_TYPE_BACKLIGHT, .name = "BACKLIGHT"},
    {.type = CONTROL_TYPE_BRIGHTNESS, .name = "BRIGHTNESS"},
    {.type = CONTROL_TYPE_CONTRAST, .name = "CONTRAST"},
    {.type = CONTROL_TYPE_HUE, .name = "HUE"},
    {.type = CONTROL_TYPE_SATURATION, .name = "SATURATION"},
    {.type = CONTROL_TYPE_SHARPNESS, .name = "SHARPNESS"},
    {.type = CONTROL_TYPE_GAMMA, .name = "GAMMA"},
    {.type = CONTROL_TYPE_WHITE_BAL_TEMP, .name = "WHITE_BAL_TEMP"},
    {.type = CONTROL_TYPE_WHITE_BAL_TEMP_AUTO, .name = "WHITE_BAL_TEMP_AUTO"},
    {.type = CONTROL_TYPE_WHITE_BAL_COMP, .name = "WHITE_BAL_COMP"},
    {.type = CONTROL_TYPE_WHITE_BAL_COMP_AUTO, .name = "WHITE_BAL_COMP_AUTO"},
    {.type = CONTROL_TYPE_DIGITAL_ZOOM, .name = "DIGITAL_ZOOM"},
    {.type = CONTROL_TYPE_MEDIA_PLAYLIST, .name = "MEDIA_PLAYLIST"},
    {.type = CONTROL_TYPE_MEDIA_PLAYLIST_NAME, .name = "MEDIA_PLAYLIST_NAME"},
    {.type = CONTROL_TYPE_MEDIA_DISK, .name = "MEDIA_DISK"},
    {.type = CONTROL_TYPE_MEDIA_DISK_NAME, .name = "MEDIA_DISK_NAME"},
    {.type = CONTROL_TYPE_MEDIA_TRACK, .name = "MEDIA_TRACK"},
    {.type = CONTROL_TYPE_MEDIA_TRACK_NAME, .name = "MEDIA_TRACK_NAME"},
    {.type = CONTROL_TYPE_MEDIA_SPEED, .name = "MEDIA_SPEED"},
    {.type = CONTROL_TYPE_MEDIA_SAMPLE_POSITION, .name = "MEDIA_SAMPLE_POSITION"},
    {.type = CONTROL_TYPE_MEDIA_PLAYBACK_TRANSPORT, .name = "MEDIA_PLAYBACK_TRANSPORT"},
    {.type = CONTROL_TYPE_MEDIA_RECORD_TRANSPORT, .name = "MEDIA_RECORD_TRANSPORT"},
    {.type = CONTROL_TYPE_FREQUENCY, .name = "FREQUENCY"},
    {.type = CONTROL_TYPE_MODULATION, .name = "MODULATION"},
    {.type = CONTROL_TYPE_POLARIZATION, .name = "POLARIZATION"},
    {.type = CONTROL_TYPE_BAUD_RATE, .name = "BAUD_RATE"},
    {.type = CONTROL_TYPE_BIT_WIDTH, .name = "BIT_WIDTH"},
    {.type = CONTROL_TYPE_PARITY, .name = "PARITY"},
    {.type = CONTROL_TYPE_STOP_BITS, .name = "STOP_BITS"},
    {.type = CONTROL_TYPE_INTERFACE_OPERATIONAL, .name = "INTERFACE_OPERATIONAL"},
    {.type = CONTROL_TYPE_INTERFACE_MEDIA_OPTIONS, .name = "INTERFACE_MEDIA_OPTIONS"},
    {.type = CONTROL_TYPE_INTERFACE_MEDIA_STATUS, .name = "INTERFACE_MEDIA_STATUS"},
    {.type = CONTROL_TYPE_INTERFACE_NETWORK_NAME, .name = "INTERFACE_NETWORK_NAME"},
    {.type = CONTROL_TYPE_FQTSS_DELTA_BANDWIDTH, .name = "FQTSS_DELTA_BANDWIDTH"},
    {.type = CONTROL_TYPE_FQTSS_ADMIN_IDLE_SLOPE, .name = "FQTSS_ADMIN_IDLE_SLOPE"},
    {.type = CONTROL_TYPE_FQTSS_OPER_IDLE_SLOPE, .name = "FQTSS_OPER_IDLE_SLOPE"},
    {.type = CONTROL_TYPE_FQTSS_PORT_TRANSMIT_RATE, .name = "FQTSS_PORT_TRANSMIT_RATE"},
    {.type = CONTROL_TYPE_FQTSS_CLASS_MEASUREMENT_INTERVAL, .name = "FQTSS_CLASS_MEASUREMENT_INTERVAL"},
    {.type = CONTROL_TYPE_FQTSS_LOCK_CLASS_BANDWIDTH, .name = "FQTSS_LOCK_CLASS_BANDWIDTH"},
}};

static_assert(
    std::ranges::is_sorted(
        STANDARD_CONTROL_TYPES, [](ControlTypeEntry const& a, ControlTypeEntry const& b) { return a.type < b.type; }),
    "STANDARD_CONTROL_TYPES must be sorted ascending by EUI-64 for the binary search");

}  // namespace

auto is_standard_control_type(Eui64 const& t) noexcept -> bool
{
    auto const s = t.span();
    return s[0] == STANDARD_CONTROL_TYPE_OUI_B0 && s[1] == STANDARD_CONTROL_TYPE_OUI_B1 && s[2] == STANDARD_CONTROL_TYPE_OUI_B2;
}

auto standard_control_type_entries() noexcept -> std::span<ControlTypeEntry const>
{
    return STANDARD_CONTROL_TYPES;
}

auto control_type_name(Eui64 const& t) noexcept -> std::string_view
{
    auto const it = std::ranges::lower_bound(
        STANDARD_CONTROL_TYPES, t, [](Eui64 const& a, Eui64 const& b) { return a < b; }, &ControlTypeEntry::type);
    if (it != STANDARD_CONTROL_TYPES.end() && it->type == t) {
        return it->name;
    }
    return is_standard_control_type(t) ? "UNKNOWN" : "VENDOR_DEFINED";
}

auto control_type_name(Eui64 const& t, std::span<ControlTypeEntry const> const vendor) noexcept -> std::string_view
{
    for (auto const& e : vendor) {
        if (e.type == t) {
            return e.name;
        }
    }
    return control_type_name(t);
}

}  // namespace statusbar::atdecc::aem
