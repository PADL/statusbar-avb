#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// For tests of the ptp4l management messages and of the client that reads
/// them: answers as ptp4l sends them, and the pieces to build others from.
/// Nothing here opens a socket or reads a clock.

#include "statusbar/gptp/gptp.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/ptpclient/ptpclient_management.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace statusbar::ptpclient::management_test_support {

// ptp4l's RESPONSE to `pmc -u -b 0 'GET PARENT_DATA_SET'` (IEEE 1588-2019 15.5.3.3.1): clock 00:1b:21:ff:fe:aa:bb:cc
// port 1, domain 0; grandmaster 00:1d:c1:ff:fe:12:34:56 (priority1 128, class 6, accuracy 0x21, variance 0x4e5d).
// clang-format off
inline constexpr std::array<uint8_t, 86> PARENT_RESPONSE{
    // common header
    0x0D, 0x02, 0x00, 0x56,                          // management, PTPv2, messageLength 86
    0x00, 0x00, 0x00, 0x00,                          // domain 0, minorSdoId, flags
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // correctionField
    0x00, 0x00, 0x00, 0x00,                          // messageTypeSpecific
    0x00, 0x1B, 0x21, 0xFF, 0xFE, 0xAA, 0xBB, 0xCC,  // sourcePortIdentity.clockIdentity
    0x00, 0x01,                                      // sourcePortIdentity.portNumber
    0x00, 0x07,                                      // sequenceId 7
    0x04, 0x7F,                                      // controlField, logMessageInterval
    // management header
    0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x30, 0x39, 0x00, 0x01,  // targetPortIdentity (pmc's)
    0x00, 0x00,                                                  // startingBoundaryHops, boundaryHops
    0x02, 0x00,                                                  // actionField RESPONSE, reserved
    // MANAGEMENT TLV
    0x00, 0x01, 0x00, 0x22,                                      // tlvType MANAGEMENT, lengthField 34
    0x20, 0x02,                                                  // managementId PARENT_DATA_SET
    0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56, 0x00, 0x01,  // parentPortIdentity
    0x00, 0x00,                                                  // parentStats, reserved
    0xFF, 0xFF,                                                  // observedParentOffsetScaledLogVariance
    0x7F, 0xFF, 0xFF, 0xFF,                                      // observedParentClockPhaseChangeRate
    0x80,                                                        // grandmasterPriority1
    0x06, 0x21, 0x4E, 0x5D,                                      // grandmasterClockQuality
    0x80,                                                        // grandmasterPriority2
    0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56,              // grandmasterIdentity
};
// clang-format on

inline ieee::Eui64 const GM{0x00, 0x1D, 0xC1, 0xFF, 0xFE, 0x12, 0x34, 0x56};

// ptp4l's RESPONSE to `pmc -u -b 0 'GET PORT_DATA_SET'` (IEEE 1588-2019 15.5.3.6.1, linuxptp's struct portDS) from
// a gPTP master port: 8c1f64.fffe.e89000-2, MASTER, peerMeanPathDelay 74.25 ns, P2P, version 2.
// clang-format off
inline constexpr std::array<uint8_t, 80> PORT_RESPONSE{
    // common header
    0x0D, 0x02, 0x00, 0x50,                          // management, PTPv2, messageLength 80
    0x00, 0x00, 0x00, 0x00,                          // domain 0, minorSdoId, flags
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // correctionField
    0x00, 0x00, 0x00, 0x00,                          // messageTypeSpecific
    0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00,  // sourcePortIdentity.clockIdentity
    0x00, 0x02,                                      // sourcePortIdentity.portNumber
    0x00, 0x09,                                      // sequenceId 9
    0x04, 0x7F,                                      // controlField, logMessageInterval
    // management header
    0x00, 0x00, 0x00, 0xFF, 0xFE, 0x00, 0x30, 0x39, 0x00, 0x01,  // targetPortIdentity (pmc's)
    0x00, 0x00,                                                  // startingBoundaryHops, boundaryHops
    0x02, 0x00,                                                  // actionField RESPONSE, reserved
    // MANAGEMENT TLV
    0x00, 0x01, 0x00, 0x1C,                                      // tlvType MANAGEMENT, lengthField 28
    0x20, 0x04,                                                  // managementId PORT_DATA_SET
    0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00, 0x00, 0x02,  // portIdentity
    0x06,                                                        // portState MASTER
    0x00,                                                        // logMinDelayReqInterval
    0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x40, 0x00,              // peerMeanPathDelay 74.25 ns
    0x00, 0x03,                                                  // logAnnounceInterval, announceReceiptTimeout
    0xFD,                                                        // logSyncInterval -3
    0x02,                                                        // delayMechanism P2P
    0x00, 0x02,                                                  // logMinPdelayReqInterval, versionNumber
};
// clang-format on

inline gptp::ClockIdentity const BRIDGE{0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00};

/// PORT_DATA_SET's data for `port` of BRIDGE with `delay_scaled`.
inline auto port_data(uint16_t const port, int64_t const delay_scaled) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data(PORT_RESPONSE.begin() + 54, PORT_RESPONSE.end());
    data[9] = static_cast<uint8_t>(port);
    for (int i = 0; i < 8; ++i) {
        data[12 + static_cast<size_t>(i)] = static_cast<uint8_t>(static_cast<uint64_t>(delay_scaled) >> (56 - (8 * i)));
    }
    return data;
}

/// linuxptp's PORT_PROPERTIES_NP data for `port` of BRIDGE on `name` (MASTER, hardware timestamping).
inline auto port_properties(uint16_t const port, std::string_view const name) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data{0x8C, 0x1F, 0x64, 0xFF, 0xFE, 0xE8, 0x90, 0x00, 0x00, static_cast<uint8_t>(port), 0x06, 0x02};
    data.push_back(static_cast<uint8_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
    if (data.size() % 2 != 0) {
        data.push_back(0);  // ptp4l pads the TLV to an even length
    }
    return data;
}

/// The 48 octets a RESPONSE of `length` octets starts with, in `domain` and transportSpecific `ts`: PARENT_RESPONSE's
/// with those three set.
inline auto response_start(size_t const length, uint8_t const domain, uint8_t const ts) -> std::vector<uint8_t>
{
    std::vector<uint8_t> out(PARENT_RESPONSE.begin(), PARENT_RESPONSE.begin() + 48);
    out[0] = static_cast<uint8_t>((ts << 4U) | 0x0DU);
    out[4] = domain;
    out[2] = static_cast<uint8_t>(length >> 8U);
    out[3] = static_cast<uint8_t>(length);
    return out;
}

/// A RESPONSE carrying `data` for `id`, in `domain` and transportSpecific `ts`, built as the bytes above are.
inline auto response(uint16_t const id, std::vector<uint8_t> const& data, uint8_t const domain = 0, uint8_t const ts = 0)
    -> std::vector<uint8_t>
{
    auto out = response_start(48 + 6 + data.size(), domain, ts);
    uint16_t const tlv_length = static_cast<uint16_t>(2 + data.size());
    out.insert(out.end(), {0x00, 0x01, static_cast<uint8_t>(tlv_length >> 8U), static_cast<uint8_t>(tlv_length)});
    out.insert(out.end(), {static_cast<uint8_t>(id >> 8U), static_cast<uint8_t>(id)});
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

inline auto parent_data(ieee::Eui64 const& gm) -> std::vector<uint8_t>
{
    std::vector<uint8_t> data(PARENT_RESPONSE.begin() + 54, PARENT_RESPONSE.end());
    uint64_t const v = gm.get();
    for (int i = 0; i < 8; ++i) {
        data[24 + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (56 - (8 * i)));
    }
    return data;
}

}  // namespace statusbar::ptpclient::management_test_support
