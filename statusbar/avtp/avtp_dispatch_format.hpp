#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVTP Dispatcher - format_avtp() routes to the appropriate subtype
/// formatter for IEEE 1722 stream data payloads (AM824, AAF, CRF, AEF,
/// ESCF, EECF, and TSCF/NTSCF with their ACF messages). Split from
/// avtp_print.hpp so consumers that do not need formatting do not pay the
/// compile-time cost of <format>.

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_format.hpp"
#include "statusbar/avtp/avtp_acf_dispatch_format.hpp"
#include "statusbar/avtp/avtp_aef.hpp"
#include "statusbar/avtp/avtp_aef_format.hpp"
#include "statusbar/avtp/avtp_am824.hpp"
#include "statusbar/avtp/avtp_am824_format.hpp"
#include "statusbar/avtp/avtp_crf.hpp"
#include "statusbar/avtp/avtp_crf_format.hpp"
#include "statusbar/avtp/avtp_eecf.hpp"
#include "statusbar/avtp/avtp_eecf_format.hpp"
#include "statusbar/avtp/avtp_escf.hpp"
#include "statusbar/avtp/avtp_escf_format.hpp"
#include "statusbar/avtp/avtp_ntscf.hpp"
#include "statusbar/avtp/avtp_ntscf_format.hpp"
#include "statusbar/avtp/avtp_ntscf_v1.hpp"
#include "statusbar/avtp/avtp_ntscf_v1_format.hpp"
#include "statusbar/avtp/avtp_tscf.hpp"
#include "statusbar/avtp/avtp_tscf_format.hpp"
#include "statusbar/avtp/avtp_tscf_v1.hpp"
#include "statusbar/avtp/avtp_tscf_v1_format.hpp"
#include "statusbar/avtp/avtp_types.hpp"

#include <cstdint>
#include <format>
#include <span>

namespace statusbar::avtp {

namespace detail {

/// Format a hex dump to an output iterator
/// @param out The output iterator to write formatted text to
/// @param data Raw byte data to dump as hex
template <typename OutputIt>
auto format_hex_dump(OutputIt out, std::span<uint8_t const> data) -> OutputIt
{
    for (auto octet_value : data) {
        out = std::format_to(out, "{:02x} ", octet_value);
    }
    out = std::format_to(out, "\n");
    return out;
}

}  // namespace detail

/// Format an AVTP stream data packet (AM824, AAF, etc.) to an output iterator
/// This handles subtypes < 0xFA (stream data formats)
/// @param out The output iterator to write formatted text to
/// @param payload Raw AVTP packet payload bytes
template <typename OutputIt>
auto format_avtp(OutputIt out, std::span<uint8_t const> payload) -> OutputIt
{
    if (payload.empty()) {
        return std::format_to(out, "  AVTP payload empty\n");
    }

    // Extract subtype from first byte
    uint8_t const subtype = payload[0];

    switch (subtype) {
        case AvtpSubtype::iec_61883_iidc: {
            // AM824 (IEC 61883/IIDC) format
            if (payload.size() < Am824Pdu::HEADER_LENGTH) {
                out = std::format_to(out, "  AVTP AM824 truncated ({} bytes, need {})\n", payload.size(), Am824Pdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }

            auto pdu = am824_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP AM824 invalid header\n");
                return detail::format_hex_dump(out, payload);
            }

            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");

            // Show audio payload info
            auto audio_payload = am824_get_audio_payload(payload);
            if (!audio_payload.empty()) {
                out = std::format_to(out, "       audio payload: {} bytes\n", audio_payload.size());
            }
            break;
        }

        case AvtpSubtype::aaf: {
            // AAF (AVTP Audio Format)
            if (payload.size() < AafPdu::HEADER_LENGTH) {
                out = std::format_to(out, "  AVTP AAF truncated ({} bytes, need {})\n", payload.size(), AafPdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }

            auto pdu = aaf_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP AAF invalid header\n");
                return detail::format_hex_dump(out, payload);
            }

            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");

            // Show audio payload info
            auto audio_payload = aaf_get_audio_payload(payload);
            if (!audio_payload.empty()) {
                out = std::format_to(out, "       audio payload: {} bytes\n", audio_payload.size());
            }
            break;
        }

        case AvtpSubtype::mma_stream:
            out = std::format_to(out, "  AVTP MMA Stream (subtype 0x{:02x})\n", subtype);
            out = std::format_to(out, "       payload: {} bytes\n", payload.size());
            break;

        case AvtpSubtype::cvf:
            out = std::format_to(out, "  AVTP CVF - Compressed Video Format (subtype 0x{:02x})\n", subtype);
            out = std::format_to(out, "       payload: {} bytes\n", payload.size());
            break;

        case AvtpSubtype::crf: {
            // CRF (Clock Reference Format)
            if (payload.size() < CrfPdu::HEADER_LENGTH) {
                out = std::format_to(out, "  AVTP CRF truncated ({} bytes, need {})\n", payload.size(), CrfPdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }

            auto pdu = crf_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP CRF invalid header\n");
                return detail::format_hex_dump(out, payload);
            }

            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");

            // Show first few timestamps if present
            auto timestamp_data = crf_get_timestamp_data(payload);
            if (!timestamp_data.empty()) {
                out = format_timestamps_to(out, timestamp_data, 4);
                out = std::format_to(out, "\n");
            }
            break;
        }

        case AvtpSubtype::aef_continuous: {
            if (payload.size() < AefContinuousPdu::HEADER_LENGTH) {
                out = std::format_to(
                    out, "  AVTP AEF-C truncated ({} bytes, need {})\n", payload.size(), AefContinuousPdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }
            auto pdu = aef_continuous_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP AEF-C invalid header\n");
                return detail::format_hex_dump(out, payload);
            }
            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");
            auto enc_payload = aef_continuous_get_encrypted_payload(payload);
            if (!enc_payload.empty()) {
                out = std::format_to(out, "       encrypted payload: {} bytes\n", enc_payload.size());
            }
            break;
        }

        case AvtpSubtype::aef_discrete: {
            if (payload.size() < AefDiscretePdu::HEADER_LENGTH) {
                out = std::format_to(
                    out, "  AVTP AEF-D truncated ({} bytes, need {})\n", payload.size(), AefDiscretePdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }
            auto pdu = aef_discrete_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP AEF-D invalid header\n");
                return detail::format_hex_dump(out, payload);
            }
            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");
            auto enc_payload = aef_discrete_get_encrypted_payload(payload);
            if (!enc_payload.empty()) {
                out = std::format_to(out, "       encrypted payload: {} bytes\n", enc_payload.size());
            }
            break;
        }

        case AvtpSubtype::escf: {
            if (payload.size() < EscfPdu::HEADER_LENGTH) {
                out = std::format_to(out, "  AVTP ESCF truncated ({} bytes, need {})\n", payload.size(), EscfPdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }
            auto pdu = escf_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP ESCF invalid header\n");
                return detail::format_hex_dump(out, payload);
            }
            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");
            auto sig_payload = escf_get_signed_payload(payload);
            if (!sig_payload.empty()) {
                out = std::format_to(out, "       signed payload: {} bytes\n", sig_payload.size());
            }
            break;
        }

        case AvtpSubtype::eecf: {
            if (payload.size() < EecfPdu::HEADER_LENGTH) {
                out = std::format_to(out, "  AVTP EECF truncated ({} bytes, need {})\n", payload.size(), EecfPdu::HEADER_LENGTH);
                return detail::format_hex_dump(out, payload);
            }
            auto pdu = eecf_parse_header(payload);
            if (!pdu) {
                out = std::format_to(out, "  AVTP EECF invalid header\n");
                return detail::format_hex_dump(out, payload);
            }
            out = std::format_to(out, "  ");
            out = format_to(out, *pdu);
            out = std::format_to(out, "\n");
            auto enc_payload = eecf_get_encrypted_payload(payload);
            if (!enc_payload.empty()) {
                out = std::format_to(out, "       encrypted payload: {} bytes\n", enc_payload.size());
            }
            break;
        }

        case AvtpSubtype::tscf: {
            // TSCF (Time-Synchronous Control Format): the header (version 0 or 1),
            // then the ACF messages it carries (Clause 9.4).
            if (auto const pdu = tscf_parse_header(payload); pdu.has_value()) {
                out = std::format_to(out, "  ");
                out = format_to(out, *pdu);
                out = std::format_to(out, "\n");
                out = format_acf_payload(out, tscf_get_acf_payload(payload));
            } else if (auto const v1 = tscf_v1_parse_header(payload); v1.has_value()) {
                out = std::format_to(out, "  ");
                out = format_to(out, *v1);
                out = std::format_to(out, "\n");
                out = format_acf_payload(out, tscf_v1_get_acf_payload(payload));
            } else {
                out = std::format_to(out, "  AVTP TSCF invalid or truncated header\n");
                return detail::format_hex_dump(out, payload);
            }
            break;
        }

        case AvtpSubtype::ntscf: {
            // NTSCF (Non-Time-Synchronous Control Format), likewise.
            if (auto const pdu = ntscf_parse_header(payload); pdu.has_value()) {
                out = std::format_to(out, "  ");
                out = format_to(out, *pdu);
                out = std::format_to(out, "\n");
                out = format_acf_payload(out, ntscf_get_acf_payload(payload));
            } else if (auto const v1 = ntscf_v1_parse_header(payload); v1.has_value()) {
                out = std::format_to(out, "  ");
                out = format_to(out, *v1);
                out = std::format_to(out, "\n");
                out = format_acf_payload(out, ntscf_v1_get_acf_payload(payload));
            } else {
                out = std::format_to(out, "  AVTP NTSCF invalid or truncated header\n");
                return detail::format_hex_dump(out, payload);
            }
            break;
        }

        default:
            out = std::format_to(out, "  AVTP stream data (subtype 0x{:02x})\n", subtype);
            out = std::format_to(out, "       payload: {} bytes\n", payload.size());
            break;
    }

    return out;
}

}  // namespace statusbar::avtp
