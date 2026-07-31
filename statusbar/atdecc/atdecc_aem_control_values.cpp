// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/atdecc/atdecc_aem_control_values.hpp"

#include <string_view>

namespace statusbar::atdecc::aem {

auto control_value_element_size(uint16_t const vt) noexcept -> size_t
{
    switch (vt) {
        // 1-byte element types
        case CONTROL_LINEAR_INT8:
        case CONTROL_LINEAR_UINT8:
        case CONTROL_SELECTOR_INT8:
        case CONTROL_SELECTOR_UINT8:
        case CONTROL_ARRAY_INT8:
        case CONTROL_ARRAY_UINT8:
            return 1;
        // 2-byte element types (incl. SELECTOR_STRING = localized_string_ref)
        case CONTROL_LINEAR_INT16:
        case CONTROL_LINEAR_UINT16:
        case CONTROL_SELECTOR_INT16:
        case CONTROL_SELECTOR_UINT16:
        case CONTROL_SELECTOR_STRING:
        case CONTROL_ARRAY_INT16:
        case CONTROL_ARRAY_UINT16:
            return 2;
        // 4-byte element types
        case CONTROL_LINEAR_INT32:
        case CONTROL_LINEAR_UINT32:
        case CONTROL_LINEAR_FLOAT:
        case CONTROL_SELECTOR_INT32:
        case CONTROL_SELECTOR_UINT32:
        case CONTROL_SELECTOR_FLOAT:
        case CONTROL_ARRAY_INT32:
        case CONTROL_ARRAY_UINT32:
        case CONTROL_ARRAY_FLOAT:
        case CONTROL_SAMPLE_RATE:
            return 4;
        // 8-byte element types
        case CONTROL_LINEAR_INT64:
        case CONTROL_LINEAR_UINT64:
        case CONTROL_LINEAR_DOUBLE:
        case CONTROL_SELECTOR_INT64:
        case CONTROL_SELECTOR_UINT64:
        case CONTROL_SELECTOR_DOUBLE:
        case CONTROL_ARRAY_INT64:
        case CONTROL_ARRAY_UINT64:
        case CONTROL_ARRAY_DOUBLE:
            return 8;
        // Fixed-size specials
        case CONTROL_SMPTE_TIME:
            return 10;
        case CONTROL_GPTP_TIME:
            return 10;
        case CONTROL_BODE_PLOT:
            return 12;  // one {freq, mag, phase} triple
        // Variable / blob
        case CONTROL_UTF8:
        case CONTROL_VENDOR:
        case CONTROL_VALUE_TYPE_EXPANSION:
            return 0;
        default:
            return 0;
    }
}

auto control_value_type_name(uint16_t const vt) noexcept -> std::string_view
{
    switch (vt) {
        case CONTROL_LINEAR_INT8:
            return "CONTROL_LINEAR_INT8";
        case CONTROL_LINEAR_UINT8:
            return "CONTROL_LINEAR_UINT8";
        case CONTROL_LINEAR_INT16:
            return "CONTROL_LINEAR_INT16";
        case CONTROL_LINEAR_UINT16:
            return "CONTROL_LINEAR_UINT16";
        case CONTROL_LINEAR_INT32:
            return "CONTROL_LINEAR_INT32";
        case CONTROL_LINEAR_UINT32:
            return "CONTROL_LINEAR_UINT32";
        case CONTROL_LINEAR_INT64:
            return "CONTROL_LINEAR_INT64";
        case CONTROL_LINEAR_UINT64:
            return "CONTROL_LINEAR_UINT64";
        case CONTROL_LINEAR_FLOAT:
            return "CONTROL_LINEAR_FLOAT";
        case CONTROL_LINEAR_DOUBLE:
            return "CONTROL_LINEAR_DOUBLE";
        case CONTROL_SELECTOR_INT8:
            return "CONTROL_SELECTOR_INT8";
        case CONTROL_SELECTOR_UINT8:
            return "CONTROL_SELECTOR_UINT8";
        case CONTROL_SELECTOR_INT16:
            return "CONTROL_SELECTOR_INT16";
        case CONTROL_SELECTOR_UINT16:
            return "CONTROL_SELECTOR_UINT16";
        case CONTROL_SELECTOR_INT32:
            return "CONTROL_SELECTOR_INT32";
        case CONTROL_SELECTOR_UINT32:
            return "CONTROL_SELECTOR_UINT32";
        case CONTROL_SELECTOR_INT64:
            return "CONTROL_SELECTOR_INT64";
        case CONTROL_SELECTOR_UINT64:
            return "CONTROL_SELECTOR_UINT64";
        case CONTROL_SELECTOR_FLOAT:
            return "CONTROL_SELECTOR_FLOAT";
        case CONTROL_SELECTOR_DOUBLE:
            return "CONTROL_SELECTOR_DOUBLE";
        case CONTROL_SELECTOR_STRING:
            return "CONTROL_SELECTOR_STRING";
        case CONTROL_ARRAY_INT8:
            return "CONTROL_ARRAY_INT8";
        case CONTROL_ARRAY_UINT8:
            return "CONTROL_ARRAY_UINT8";
        case CONTROL_ARRAY_INT16:
            return "CONTROL_ARRAY_INT16";
        case CONTROL_ARRAY_UINT16:
            return "CONTROL_ARRAY_UINT16";
        case CONTROL_ARRAY_INT32:
            return "CONTROL_ARRAY_INT32";
        case CONTROL_ARRAY_UINT32:
            return "CONTROL_ARRAY_UINT32";
        case CONTROL_ARRAY_INT64:
            return "CONTROL_ARRAY_INT64";
        case CONTROL_ARRAY_UINT64:
            return "CONTROL_ARRAY_UINT64";
        case CONTROL_ARRAY_FLOAT:
            return "CONTROL_ARRAY_FLOAT";
        case CONTROL_ARRAY_DOUBLE:
            return "CONTROL_ARRAY_DOUBLE";
        case CONTROL_UTF8:
            return "CONTROL_UTF8";
        case CONTROL_BODE_PLOT:
            return "CONTROL_BODE_PLOT";
        case CONTROL_SMPTE_TIME:
            return "CONTROL_SMPTE_TIME";
        case CONTROL_SAMPLE_RATE:
            return "CONTROL_SAMPLE_RATE";
        case CONTROL_GPTP_TIME:
            return "CONTROL_GPTP_TIME";
        case CONTROL_VENDOR:
            return "CONTROL_VENDOR";
        case CONTROL_VALUE_TYPE_EXPANSION:
            return "EXPANSION";
        default:
            return "Reserved";
    }
}

auto control_value_details_length(uint16_t const control_value_type, uint16_t const number_of_values) noexcept -> size_t
{
    uint16_t const vt = static_cast<uint16_t>(control_value_type & CONTROL_VALUE_TYPE_MASK);
    size_t const n = number_of_values;
    size_t const v = control_value_element_size(vt);

    if (is_linear_value_type(vt)) {
        return n * ((5 * v) + 4);
    }
    if (is_selector_value_type(vt)) {
        return ((n + 2) * v) + 2;
    }
    if (is_array_value_type(vt)) {
        return 4 + (4 * v) + (n * v);
    }
    switch (vt) {
        case CONTROL_SMPTE_TIME:
            return 10;
        case CONTROL_SAMPLE_RATE:
            return 16;
        case CONTROL_GPTP_TIME:
            return 10;
        case CONTROL_BODE_PLOT:
            return 48 + (n * 12);
        // UTF-8 and VENDOR have no fixed length deducible from
        // number_of_values; callers infer it from the AECP
        // control_data_length field.
        case CONTROL_UTF8:
        case CONTROL_VENDOR:
        case CONTROL_VALUE_TYPE_EXPANSION:
        default:
            return 0;
    }
}

}  // namespace statusbar::atdecc::aem
