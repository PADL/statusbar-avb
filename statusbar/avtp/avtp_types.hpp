#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVTP Types - IEEE 1722-2016 Table 6
/// Complete list of AVTP subtype values and header format classification

#include <cstdint>
#include <string_view>

namespace statusbar::avtp {

//
// AVTP EtherType
//

/// AVTP EtherType - IEEE 1722-2016
constexpr uint16_t AVTP_ETHERTYPE = 0x22F0U;

//
// AVTP Header Types - IEEE 1722-2016 Section 4.4
//

/// AVTP header format types
/// Determines which header structure to use for parsing/creating packets
enum class AvtpHeaderType : uint8_t
{
    stream,       ///< Stream data header (4.4.4) - for continuous media streams
    control,      ///< Control header (4.4.5) - for discrete control messages
    alternative,  ///< Alternative header (4.4.6) - for special formats
    reserved,     ///< Reserved/unknown subtype
};

/// Get human-readable name for header type
/// @param type The AVTP header type to name
[[nodiscard]] auto avtp_header_type_name(AvtpHeaderType type) noexcept -> std::string_view;

//
// AVTP Encapsulation Styles - IEEE 1722-2016 Section 4.4
//

/// AVTP encapsulation style
/// Indicates whether data is continuous (streaming) or discrete (messages)
enum class AvtpEncapsulation : uint8_t
{
    continuous,  ///< Continuous streaming data (audio, video)
    discrete,    ///< Discrete messages (control, discovery)
    reserved,    ///< Reserved/unknown
};

/// Get human-readable name for encapsulation style
/// @param encap The AVTP encapsulation style to name
[[nodiscard]] auto avtp_encapsulation_name(AvtpEncapsulation encap) noexcept -> std::string_view;

//
// AVTP Subtype Values - IEEE 1722-2016 Table 6
//

/// AVTP subtype constants - complete list from IEEE 1722-2016 Table 6
namespace AvtpSubtype {

// Continuous stream formats (0x00 - 0x07)
constexpr uint8_t iec_61883_iidc = 0x00U;  ///< IEC 61883/IIDC format (Clause 5)
constexpr uint8_t mma_stream = 0x01U;      ///< MMA streams (Clause 1)
constexpr uint8_t aaf = 0x02U;             ///< AVTP Audio Format (Clause 7)
constexpr uint8_t cvf = 0x03U;             ///< Compressed Video Format (Clause 7)
constexpr uint8_t crf = 0x04U;             ///< Clock Reference Format (Clause 8)
constexpr uint8_t tscf = 0x05U;            ///< Time-Synchronous Control Format (Clause 9.3)
constexpr uint8_t svf = 0x06U;             ///< SDI Video Format (Clause 8)
constexpr uint8_t rvf = 0x07U;             ///< Raw Video Format (Clause 12)

// Reserved range: 0x08 - 0x6D

// Encrypted and vendor-specific continuous formats (0x6E - 0x7F)
constexpr uint8_t aef_continuous = 0x6EU;  ///< AES Encrypted Format Continuous (Clause 11)
constexpr uint8_t vsf_stream = 0x6FU;      ///< Vendor Specific Format Stream (Clause 11)
// Reserved range: 0x70 - 0x7E
constexpr uint8_t ef_stream = 0x7FU;  ///< Experimental Format Stream (Clause 15)

// Reserved range: 0x80 - 0x81

// Discrete control formats (0x82 - 0xEE)
constexpr uint8_t ntscf = 0x82U;  ///< Non-Time-Synchronous Control Format (Clause 9.2)
// Reserved range: 0x83 - 0xEB
constexpr uint8_t escf = 0xECU;          ///< ECC Signed Control Format (Clause 16)
constexpr uint8_t eecf = 0xEDU;          ///< ECC Encrypted Control Format (Clause 17)
constexpr uint8_t aef_discrete = 0xEEU;  ///< AES Encrypted Format Discrete (Clause 11)

// Reserved range: 0xEF - 0xF9

// ATDECC protocols (0xFA - 0xFC) - IEEE 1722.1
constexpr uint8_t adp = 0xFAU;   ///< ATDECC Discovery Protocol (IEEE 1722.1-2013 Clause 6)
constexpr uint8_t aecp = 0xFBU;  ///< ATDECC Enumeration and Control Protocol (IEEE 1722.1-2013 Clause 9)
constexpr uint8_t acmp = 0xFCU;  ///< ATDECC Connection Management Protocol (IEEE 1722.1-2013 Clause 8)

// Reserved: 0xFD

// MAAP and experimental (0xFE - 0xFF)
constexpr uint8_t maap = 0xFEU;        ///< MAAP Protocol (Annex B)
constexpr uint8_t ef_control = 0xFFU;  ///< Experimental Format Control (Clause 15)

}  // namespace AvtpSubtype

//
// Subtype Information Structure
//

/// Complete information about an AVTP subtype
struct AvtpSubtypeInfo
{
    uint8_t subtype;                  ///< The subtype value
    std::string_view name;            ///< Short name (e.g., "AAF", "MAAP")
    std::string_view description;     ///< Full description
    AvtpHeaderType header_type;       ///< Which header format to use
    AvtpEncapsulation encapsulation;  ///< Continuous or discrete
};

//
// Subtype Lookup Functions
//

/// Get the header type for a given subtype value
/// \param subtype The AVTP subtype value (0x00-0xFF)
/// \return The header type to use for parsing this subtype
[[nodiscard]] constexpr auto avtp_get_header_type(uint8_t const subtype) noexcept -> AvtpHeaderType
{
    switch (subtype) {
        // Stream header (4.4.4) - continuous media
        case AvtpSubtype::iec_61883_iidc:
        case AvtpSubtype::mma_stream:
        case AvtpSubtype::aaf:
        case AvtpSubtype::cvf:
        case AvtpSubtype::tscf:
        case AvtpSubtype::svf:
        case AvtpSubtype::rvf:
        case AvtpSubtype::vsf_stream:
        case AvtpSubtype::ef_stream:
            return AvtpHeaderType::stream;

        // Alternative header (4.4.6)
        case AvtpSubtype::crf:
        case AvtpSubtype::aef_continuous:
        case AvtpSubtype::ntscf:
        case AvtpSubtype::escf:
        case AvtpSubtype::eecf:
        case AvtpSubtype::aef_discrete:
            return AvtpHeaderType::alternative;

        // Control header (4.4.5) - ATDECC and MAAP
        case AvtpSubtype::adp:
        case AvtpSubtype::aecp:
        case AvtpSubtype::acmp:
        case AvtpSubtype::maap:
        case AvtpSubtype::ef_control:
            return AvtpHeaderType::control;

        default:
            return AvtpHeaderType::reserved;
    }
}

/// Get the encapsulation style for a given subtype value
/// \param subtype The AVTP subtype value (0x00-0xFF)
/// \return The encapsulation style (continuous or discrete)
[[nodiscard]] constexpr auto avtp_get_encapsulation(uint8_t const subtype) noexcept -> AvtpEncapsulation
{
    switch (subtype) {
        // Continuous encapsulation - streaming data
        case AvtpSubtype::iec_61883_iidc:
        case AvtpSubtype::mma_stream:
        case AvtpSubtype::aaf:
        case AvtpSubtype::cvf:
        case AvtpSubtype::crf:
        case AvtpSubtype::tscf:
        case AvtpSubtype::svf:
        case AvtpSubtype::rvf:
        case AvtpSubtype::aef_continuous:
        case AvtpSubtype::vsf_stream:
        case AvtpSubtype::ef_stream:
            return AvtpEncapsulation::continuous;

        // Discrete encapsulation - control messages
        case AvtpSubtype::ntscf:
        case AvtpSubtype::escf:
        case AvtpSubtype::eecf:
        case AvtpSubtype::aef_discrete:
        case AvtpSubtype::adp:
        case AvtpSubtype::aecp:
        case AvtpSubtype::acmp:
        case AvtpSubtype::maap:
        case AvtpSubtype::ef_control:
            return AvtpEncapsulation::discrete;

        default:
            return AvtpEncapsulation::reserved;
    }
}

/// Get the short name for a subtype value
/// \param subtype The AVTP subtype value (0x00-0xFF)
/// \return Short name string (e.g., "AAF", "MAAP", "Reserved")
[[nodiscard]] auto avtp_subtype_name(uint8_t subtype) noexcept -> std::string_view;

/// Get a full description for a subtype value
/// \param subtype The AVTP subtype value (0x00-0xFF)
/// \return Full description string
[[nodiscard]] constexpr auto avtp_subtype_description(uint8_t const subtype) noexcept -> std::string_view
{
    switch (subtype) {
        case AvtpSubtype::iec_61883_iidc:
            return "IEC 61883/IIDC format";
        case AvtpSubtype::mma_stream:
            return "MMA streams";
        case AvtpSubtype::aaf:
            return "AVTP Audio Format";
        case AvtpSubtype::cvf:
            return "Compressed Video Format";
        case AvtpSubtype::crf:
            return "Clock Reference Format";
        case AvtpSubtype::tscf:
            return "Time-Synchronous Control Format";
        case AvtpSubtype::svf:
            return "SDI Video Format";
        case AvtpSubtype::rvf:
            return "Raw Video Format";
        case AvtpSubtype::aef_continuous:
            return "AES Encrypted Format Continuous";
        case AvtpSubtype::vsf_stream:
            return "Vendor Specific Format Stream";
        case AvtpSubtype::ef_stream:
            return "Experimental Format Stream";
        case AvtpSubtype::ntscf:
            return "Non-Time-Synchronous Control Format";
        case AvtpSubtype::escf:
            return "ECC Signed Control Format";
        case AvtpSubtype::eecf:
            return "ECC Encrypted Control Format";
        case AvtpSubtype::aef_discrete:
            return "AES Encrypted Format Discrete";
        case AvtpSubtype::adp:
            return "ATDECC Discovery Protocol";
        case AvtpSubtype::aecp:
            return "ATDECC Enumeration and Control Protocol";
        case AvtpSubtype::acmp:
            return "ATDECC Connection Management Protocol";
        case AvtpSubtype::maap:
            return "MAAP Protocol";
        case AvtpSubtype::ef_control:
            return "Experimental Format Control";
        default:
            return "Reserved";
    }
}

/// Get complete subtype information
/// \param subtype The AVTP subtype value (0x00-0xFF)
/// \return AvtpSubtypeInfo structure with all information
[[nodiscard]] inline auto avtp_get_subtype_info(uint8_t const subtype) noexcept -> AvtpSubtypeInfo
{
    return AvtpSubtypeInfo{
        .subtype = subtype,
        .name = avtp_subtype_name(subtype),
        .description = avtp_subtype_description(subtype),
        .header_type = avtp_get_header_type(subtype),
        .encapsulation = avtp_get_encapsulation(subtype),
    };
}

//
// Subtype Classification Predicates
//

/// Check if a subtype is a valid (non-reserved) AVTP subtype
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_valid_subtype(uint8_t const subtype) noexcept
{
    return avtp_get_header_type(subtype) != AvtpHeaderType::reserved;
}

/// Check if a subtype uses the stream header format
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_stream_subtype(uint8_t const subtype) noexcept
{
    return avtp_get_header_type(subtype) == AvtpHeaderType::stream;
}

/// Check if a subtype uses the control header format
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_control_subtype(uint8_t const subtype) noexcept
{
    return avtp_get_header_type(subtype) == AvtpHeaderType::control;
}

/// Check if a subtype uses the alternative header format
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_alternative_subtype(uint8_t const subtype) noexcept
{
    return avtp_get_header_type(subtype) == AvtpHeaderType::alternative;
}

/// Check if a subtype is for continuous (streaming) data
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_continuous(uint8_t const subtype) noexcept
{
    return avtp_get_encapsulation(subtype) == AvtpEncapsulation::continuous;
}

/// Check if a subtype is for discrete (message) data
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_discrete(uint8_t const subtype) noexcept
{
    return avtp_get_encapsulation(subtype) == AvtpEncapsulation::discrete;
}

/// Check if a subtype is an ATDECC protocol (ADP, AECP, or ACMP)
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_atdecc_subtype(uint8_t const subtype) noexcept
{
    return subtype == AvtpSubtype::adp || subtype == AvtpSubtype::aecp || subtype == AvtpSubtype::acmp;
}

/// Check if a subtype is an audio format (AAF or 61883/IIDC AM824)
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_audio_subtype(uint8_t const subtype) noexcept
{
    return subtype == AvtpSubtype::iec_61883_iidc || subtype == AvtpSubtype::aaf;
}

/// Check if a subtype is a video format (CVF, SVF, or RVF)
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_video_subtype(uint8_t const subtype) noexcept
{
    return subtype == AvtpSubtype::cvf || subtype == AvtpSubtype::svf || subtype == AvtpSubtype::rvf;
}

/// Check if a subtype is an encrypted format
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_encrypted_subtype(uint8_t const subtype) noexcept
{
    return subtype == AvtpSubtype::aef_continuous || subtype == AvtpSubtype::escf || subtype == AvtpSubtype::eecf ||
        subtype == AvtpSubtype::aef_discrete;
}

/// Check if a subtype is experimental
/// @param subtype The AVTP subtype value (0x00-0xFF)
[[nodiscard]] constexpr auto avtp_is_experimental_subtype(uint8_t const subtype) noexcept
{
    return subtype == AvtpSubtype::ef_stream || subtype == AvtpSubtype::ef_control;
}

}  // namespace statusbar::avtp
