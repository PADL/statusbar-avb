// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_units.hpp"

#include <string_view>

namespace statusbar::atdecc::aem {

auto control_unit_code_name(uint8_t const code) noexcept -> std::string_view
{
    switch (code) {
        // Unitless
        case UNIT_CODE_UNITLESS:
            return "UNITLESS";
        case UNIT_CODE_COUNT:
            return "COUNT";
        case UNIT_CODE_PERCENT:
            return "PERCENT";
        case UNIT_CODE_FSTOP:
            return "FSTOP";

        // Time
        case UNIT_CODE_SECONDS:
            return "SECONDS";
        case UNIT_CODE_MINUTES:
            return "MINUTES";
        case UNIT_CODE_HOURS:
            return "HOURS";
        case UNIT_CODE_DAYS:
            return "DAYS";
        case UNIT_CODE_MONTHS:
            return "MONTHS";
        case UNIT_CODE_YEARS:
            return "YEARS";
        case UNIT_CODE_SAMPLES:
            return "SAMPLES";
        case UNIT_CODE_FRAMES:
            return "FRAMES";

        // Frequency
        case UNIT_CODE_HERTZ:
            return "HERTZ";
        case UNIT_CODE_SEMITONES:
            return "SEMITONES";
        case UNIT_CODE_CENTS:
            return "CENTS";
        case UNIT_CODE_OCTAVES:
            return "OCTAVES";
        case UNIT_CODE_FPS:
            return "FPS";

        // Distance
        case UNIT_CODE_METRES:
            return "METRES";

        // Temperature
        case UNIT_CODE_KELVIN:
            return "KELVIN";

        // Mass
        case UNIT_CODE_GRAMS:
            return "GRAMS";

        // Voltage
        case UNIT_CODE_VOLTS:
            return "VOLTS";
        case UNIT_CODE_DBV:
            return "DBV";
        case UNIT_CODE_DBU:
            return "DBU";

        // Current
        case UNIT_CODE_AMPS:
            return "AMPS";

        // Power
        case UNIT_CODE_WATTS:
            return "WATTS";
        case UNIT_CODE_DBM:
            return "DBM";
        case UNIT_CODE_DBW:
            return "DBW";

        // Pressure
        case UNIT_CODE_PASCALS:
            return "PASCALS";

        // Memory
        case UNIT_CODE_BITS:
            return "BITS";
        case UNIT_CODE_BYTES:
            return "BYTES";
        case UNIT_CODE_KIBIBYTES:
            return "KIBIBYTES";
        case UNIT_CODE_MEBIBYTES:
            return "MEBIBYTES";
        case UNIT_CODE_GIBIBYTES:
            return "GIBIBYTES";
        case UNIT_CODE_TEBIBYTES:
            return "TEBIBYTES";

        // Memory Bandwidth
        case UNIT_CODE_BITS_PER_SEC:
            return "BITS_PER_SEC";
        case UNIT_CODE_BYTES_PER_SEC:
            return "BYTES_PER_SEC";
        case UNIT_CODE_KIBIBYTES_PER_SEC:
            return "KIBIBYTES_PER_SEC";
        case UNIT_CODE_MEBIBYTES_PER_SEC:
            return "MEBIBYTES_PER_SEC";
        case UNIT_CODE_GIBIBYTES_PER_SEC:
            return "GIBIBYTES_PER_SEC";
        case UNIT_CODE_TEBIBYTES_PER_SEC:
            return "TEBIBYTES_PER_SEC";

        // Luminosity
        case UNIT_CODE_CANDELAS:
            return "CANDELAS";

        // Energy
        case UNIT_CODE_JOULES:
            return "JOULES";

        // Angle
        case UNIT_CODE_RADIANS:
            return "RADIANS";

        // Force
        case UNIT_CODE_NEWTONS:
            return "NEWTONS";

        // Resistance
        case UNIT_CODE_OHMS:
            return "OHMS";

        // Velocity
        case UNIT_CODE_METRES_PER_SEC:
            return "METRES_PER_SEC";
        case UNIT_CODE_RADIANS_PER_SEC:
            return "RADIANS_PER_SEC";

        // Acceleration
        case UNIT_CODE_METRES_PER_SEC_SQUARED:
            return "METRES_PER_SEC_SQUARED";
        case UNIT_CODE_RADIANS_PER_SEC_SQUARED:
            return "RADIANS_PER_SEC_SQUARED";

        // Magnetic flux
        case UNIT_CODE_TESLAS:
            return "TESLAS";
        case UNIT_CODE_WEBERS:
            return "WEBERS";
        case UNIT_CODE_AMPS_PER_METRE:
            return "AMPS_PER_METRE";

        // Area
        case UNIT_CODE_METRES_SQUARED:
            return "METRES_SQUARED";

        // Volume
        case UNIT_CODE_METRES_CUBED:
            return "METRES_CUBED";
        case UNIT_CODE_LITRES:
            return "LITRES";

        // Levels and loudness
        case UNIT_CODE_DB:
            return "DB";
        case UNIT_CODE_DB_PEAK:
            return "DB_PEAK";
        case UNIT_CODE_DB_RMS:
            return "DB_RMS";
        case UNIT_CODE_DBFS:
            return "DBFS";
        case UNIT_CODE_DBFS_PEAK:
            return "DBFS_PEAK";
        case UNIT_CODE_DBFS_RMS:
            return "DBFS_RMS";
        case UNIT_CODE_DBTP:
            return "DBTP";
        case UNIT_CODE_DB_SPL_A:
            return "DB_SPL_A";
        case UNIT_CODE_DB_Z:
            return "DB_Z";
        case UNIT_CODE_DB_SPL_C:
            return "DB_SPL_C";
        case UNIT_CODE_DB_SPL:
            return "DB_SPL";
        case UNIT_CODE_LU:
            return "LU";
        case UNIT_CODE_LUFS:
            return "LUFS";
        case UNIT_CODE_DB_A:
            return "DB_A";

        default:
            return (code >= 0xC0) ? "Reserved (>= 0xc0)" : "Reserved";
    }
}

auto control_unit_code_suffix(uint8_t const code) noexcept -> std::string_view
{
    switch (code) {
        case UNIT_CODE_PERCENT:
            return "%";
        case UNIT_CODE_SECONDS:
            return "s";
        case UNIT_CODE_MINUTES:
            return "m";
        case UNIT_CODE_HOURS:
            return "h";
        case UNIT_CODE_DAYS:
            return "d";
        case UNIT_CODE_MONTHS:
            return "M";
        case UNIT_CODE_YEARS:
            return "Y";
        case UNIT_CODE_SAMPLES:
            return "samples";
        case UNIT_CODE_FRAMES:
            return "f";
        case UNIT_CODE_HERTZ:
            return "Hz";
        case UNIT_CODE_SEMITONES:
            return "Note";
        case UNIT_CODE_CENTS:
            return "Cent";
        case UNIT_CODE_OCTAVES:
            return "Octave";
        case UNIT_CODE_FPS:
            return "FPS";
        case UNIT_CODE_METRES:
            return "m";
        case UNIT_CODE_KELVIN:
            return "K";
        case UNIT_CODE_GRAMS:
            return "gram";
        case UNIT_CODE_VOLTS:
            return "V";
        case UNIT_CODE_DBV:
            return "dBV";
        case UNIT_CODE_DBU:
            return "dBu";
        case UNIT_CODE_AMPS:
            return "A";
        case UNIT_CODE_WATTS:
            return "W";
        case UNIT_CODE_DBM:
            return "dBm";
        case UNIT_CODE_DBW:
            return "dBW";
        case UNIT_CODE_PASCALS:
            return "Pa";
        case UNIT_CODE_BITS:
            return "b";
        case UNIT_CODE_BYTES:
            return "B";
        case UNIT_CODE_KIBIBYTES:
            return "KiB";
        case UNIT_CODE_MEBIBYTES:
            return "MiB";
        case UNIT_CODE_GIBIBYTES:
            return "GiB";
        case UNIT_CODE_TEBIBYTES:
            return "TiB";
        case UNIT_CODE_BITS_PER_SEC:
            return "b/s";
        case UNIT_CODE_BYTES_PER_SEC:
            return "B/s";
        case UNIT_CODE_KIBIBYTES_PER_SEC:
            return "KiB/s";
        case UNIT_CODE_MEBIBYTES_PER_SEC:
            return "MiB/s";
        case UNIT_CODE_GIBIBYTES_PER_SEC:
            return "GiB/s";
        case UNIT_CODE_TEBIBYTES_PER_SEC:
            return "TiB/s";
        case UNIT_CODE_CANDELAS:
            return "cd";
        case UNIT_CODE_JOULES:
            return "J";
        case UNIT_CODE_RADIANS:
            return "rad";
        case UNIT_CODE_NEWTONS:
            return "N";
        case UNIT_CODE_OHMS:
            return "\u03A9";  // Ω
        case UNIT_CODE_METRES_PER_SEC:
            return "m/s";
        case UNIT_CODE_RADIANS_PER_SEC:
            return "rad/s";
        case UNIT_CODE_METRES_PER_SEC_SQUARED:
            return "m/s/s";
        case UNIT_CODE_RADIANS_PER_SEC_SQUARED:
            return "rad/s/s";
        case UNIT_CODE_TESLAS:
            return "T";
        case UNIT_CODE_WEBERS:
            return "Wb";
        case UNIT_CODE_AMPS_PER_METRE:
            return "A/m";
        case UNIT_CODE_METRES_SQUARED:
            return "m\u00B7m";  // m·m
        case UNIT_CODE_METRES_CUBED:
            return "m\u00B7m\u00B7m";  // m·m·m
        case UNIT_CODE_LITRES:
            return "L";
        case UNIT_CODE_DB:
            return "dB";
        case UNIT_CODE_DB_PEAK:
            return "dB (Peak)";
        case UNIT_CODE_DB_RMS:
            return "dB (RMS)";
        case UNIT_CODE_DBFS:
            return "dBFS";
        case UNIT_CODE_DBFS_PEAK:
            return "dBFS (Peak)";
        case UNIT_CODE_DBFS_RMS:
            return "dBFS (RMS)";
        case UNIT_CODE_DBTP:
            return "dBTP";
        case UNIT_CODE_DB_SPL_A:
            return "dB(A) SPL";
        case UNIT_CODE_DB_Z:
            return "dB(Z)";
        case UNIT_CODE_DB_SPL_C:
            return "dB(C) SPL";
        case UNIT_CODE_DB_SPL:
            return "dB SPL";
        case UNIT_CODE_LU:
            return "LU";
        case UNIT_CODE_LUFS:
            return "LUFS";
        case UNIT_CODE_DB_A:
            return "dB(A)";

        // Unitless / count / fstop / reserved: no symbolic suffix
        default:
            return "";
    }
}

}  // namespace statusbar::atdecc::aem
