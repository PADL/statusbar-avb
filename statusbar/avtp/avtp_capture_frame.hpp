#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Two things a capture-reading tool needs before it can look at an AVTPDU: which
/// container the file is (classic pcap or pcapng, from its first four octets), and the
/// frame's inner ethertype and payload when the capture kept the 802.1Q tag that every
/// SR-class stream frame carries on the wire.

#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace statusbar::avtp {

/// The container a capture file is in, by its first four octets.
enum class CaptureFormat : uint8_t
{
    unknown,
    pcap,    ///< classic libpcap: a1b2c3d4 (microsecond) or a1b23c4d (nanosecond), either byte order
    pcapng,  ///< pcapng: a Section Header Block, type 0a0d0d0a, palindromic by design
};

/// Classify a capture by its first four octets, as they lie in the file.
[[nodiscard]] inline auto capture_format_of(std::span<uint8_t const, 4> magic) noexcept -> CaptureFormat
{
    ieee::quadlet_t q{};
    span_load(q, magic);
    uint32_t const as_big_endian = q;
    switch (as_big_endian) {
        case 0x0A0D0D0AU:
            return CaptureFormat::pcapng;
        case 0xA1B2C3D4U:
        case 0xD4C3B2A1U:
        case 0xA1B23C4DU:
        case 0x4D3CB2A1U:
            return CaptureFormat::pcap;
        default:
            return CaptureFormat::unknown;
    }
}

/// An Ethernet payload after its 802.1Q tag, if it had one: the ethertype of the
/// protocol inside and the bytes after it, plus the tag for whoever wants the VID
/// or priority (not set when the frame was untagged).
struct InnerEthernetPayload
{
    uint16_t ethertype{};
    std::span<uint8_t const> payload;
    ieee::VlanTag vlan_tag{};
};

/// The tag a reader split off: the TPID it reported as the ethertype, the TCI it left
/// at the head of the payload.
[[nodiscard]] inline auto vlan_tag_from(uint16_t tpid, std::span<uint8_t const, 2> tci) noexcept -> ieee::VlanTag
{
    ieee::VlanTag tag{};
    tag.tpid = tpid;
    span_load(tag.tci, tci);
    return tag;
}

/// Look through an 802.1Q tag. A capture reader that splits the frame at the 14-octet
/// header reports a tagged frame's ethertype as the TPID (0x8100) and its payload as
/// TCI || inner ethertype || data; this hands back the inner pair, and an untagged
/// frame unchanged. A payload too short to hold a tag is also handed back unchanged.
[[nodiscard]] inline auto strip_vlan_tag(uint16_t ethertype, std::span<uint8_t const> payload) noexcept -> InnerEthernetPayload
{
    if (ethertype != ieee::VlanTag::ETHERTYPE || payload.size() < ieee::VlanTag::LENGTH) {
        return InnerEthernetPayload{.ethertype = ethertype, .payload = payload};
    }
    ieee::doublet_t inner{};
    span_load(inner, payload.subspan(2, 2));
    return InnerEthernetPayload{
        .ethertype = inner,
        .payload = payload.subspan(ieee::VlanTag::LENGTH),
        .vlan_tag = vlan_tag_from(ethertype, payload.first<2>())};
}

}  // namespace statusbar::avtp
