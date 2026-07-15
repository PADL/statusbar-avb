#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_udptun_format.hpp
/// @brief The Class-A 96 kHz media format + tunnel stream-id constants, shared
/// by AvbEntityAudioIO's AVB streams and the inter-site tunnel data plane.
///
/// One definition (refactor phase C): the entity and the tunnel bridge each
/// carried a verbatim copy of these, and the tunnel only works because the two
/// agree — so they are literally one fact, defined here. AvbEntityAudioIO
/// re-exports them as class-scope aliases for its existing API.

#include "statusbar/avtp/avtp_aaf.hpp"

#include <cstddef>
#include <cstdint>

namespace statusbar::avb_entity {

/// 96 kHz at SR class A cadence (125 us interval = 8000 packets/s), so
/// samples-per-packet = 96000/8000 = 12; AAF carries 32-bit signed PCM.
struct ClassA96k
{
    static constexpr uint32_t CLASS_A_PACKETS_PER_SEC = 8000;
    static constexpr uint32_t SAMPLE_RATE = 96000;
    static constexpr size_t SAMPLES_PER_PACKET = SAMPLE_RATE / CLASS_A_PACKETS_PER_SEC;
    /// One packet's worth of time: 12 samples @ 96 kHz = 125 us.
    static constexpr uint64_t PACKET_INTERVAL_NS = (static_cast<uint64_t>(SAMPLES_PER_PACKET) * 1'000'000'000ULL) / SAMPLE_RATE;
    /// AAF stream wire format: 32-bit signed PCM at 96 kHz.
    static constexpr avtp::AafFormat AAF_FORMAT = avtp::AafFormat::int_32bit;
    static constexpr avtp::AafSampleRate AAF_SAMPLE_RATE = avtp::AafSampleRate::rate_96_khz;
    static constexpr uint8_t AAF_BIT_DEPTH = 32;
};

/// Redundancy flag bit in the tunnel stream_id: the redundant copy is sent with
/// `primary | UDPTUN_REDUN_BIT`, so the egress tells primary from redundant. It
/// MUST sit in the modified-EUI-64 middle bytes (b3/b4 = the inserted 0xFF:0xFE,
/// uint64 bits 24..39) — the bytes owlm_analyze masks when grouping primary +
/// redundant into one logical sender — otherwise the two copies land in different
/// pair_ids and recovery accounting breaks. Bit 24 (LSB of b4 = 0xFE) is reliably
/// clear in a MAC-derived id and inside owlm's mask. (A NIC-half bit like 1<<23
/// would be wrong on both counts: it can be set in the MAC, and lies outside
/// owlm's mask.) The primary id force-clears this bit so the redundant id is always
/// distinct. The b3/b4 mask owlm applies for pairing is OWLM_PAIR_MASK_MIDBYTES.
inline constexpr uint64_t UDPTUN_REDUN_BIT = (1ULL << 24);
inline constexpr uint64_t OWLM_PAIR_MASK_MIDBYTES = 0x000000FF'FF000000ULL;
static_assert((UDPTUN_REDUN_BIT & (UDPTUN_REDUN_BIT - 1)) == 0, "REDUN_BIT must be a single bit");
static_assert(
    (UDPTUN_REDUN_BIT & ~OWLM_PAIR_MASK_MIDBYTES) == 0,
    "REDUN_BIT must live in the EUI-64 b3/b4 bytes that owlm masks for pair grouping");

}  // namespace statusbar::avb_entity
