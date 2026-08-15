// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// clang-format off
// Include order differs between clang-19 and clang-22 here — wrap-off
// pins it so both versions produce the same layout.
#include "statusbar/owlm/owlm_packet.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/status/statusbar_assert.hpp"

#include <cstring>
// clang-format on

namespace statusbar::owlm {

namespace {

using ieee::doublet_t;
using ieee::octlet_t;
using ieee::quadlet_t;

/// The 32-byte OWLM header as it appears on the wire — every multi-byte
/// field is an ieee network-ordered type, so encode/decode are a single
/// struct copy with no per-field byte marshalling.
struct OwlmWireHeader
{
    quadlet_t magic{0};
    doublet_t version{0};
    doublet_t flags{0};
    ieee::Eui64 sender_eui64{};
    quadlet_t sequence{0};
    octlet_t tx_gptp_ns{0};  ///< int64 carried in two's-complement bits
    quadlet_t tx_interval_us{0};
};

static_assert(sizeof(OwlmWireHeader) == OwlmPacket::HEADER_SIZE, "OwlmWireHeader must match the wire header size");
static_assert(alignof(OwlmWireHeader) == 1);

}  // namespace

auto encode_owlm_packet(OwlmPacket const& p, std::span<uint8_t> buf) -> size_t
{
    auto const buf_size = buf.size();
    STATUSBAR_ASSERT(buf_size >= OwlmPacket::HEADER_SIZE);

    OwlmWireHeader hdr{};
    hdr.magic = OwlmPacket::MAGIC;
    hdr.version = OwlmPacket::VERSION;
    hdr.flags = 0;
    hdr.sender_eui64 = p.sender_eui64;
    hdr.sequence = p.sequence;
    hdr.tx_gptp_ns = static_cast<uint64_t>(p.tx_gptp_ns);
    hdr.tx_interval_us = p.tx_interval_us;

    statusbar::span_copy(buf.first(OwlmPacket::HEADER_SIZE), statusbar::make_const_span(hdr));
    return OwlmPacket::HEADER_SIZE;
}

auto decode_owlm_packet(std::span<uint8_t const> buf, OwlmPacket& out) -> std::error_code
{
    if (buf.size() < OwlmPacket::HEADER_SIZE) {
        return make_error_code(OwlmError::DatagramTooShort);
    }

    OwlmWireHeader hdr{};
    statusbar::span_load(hdr, buf.first(OwlmPacket::HEADER_SIZE));

    if (hdr.magic != OwlmPacket::MAGIC) {
        return make_error_code(OwlmError::InvalidMagic);
    }
    if (hdr.version != OwlmPacket::VERSION) {
        return make_error_code(OwlmError::UnsupportedVersion);
    }
    if (hdr.flags != 0) {
        return make_error_code(OwlmError::ReservedFlagsSet);
    }

    out.sender_eui64 = hdr.sender_eui64;
    out.sequence = hdr.sequence.get();
    out.tx_gptp_ns = static_cast<int64_t>(hdr.tx_gptp_ns.get());
    out.tx_interval_us = hdr.tx_interval_us.get();

    if (out.tx_interval_us == 0 || out.tx_interval_us > OwlmPacket::max_interval_us) {
        return make_error_code(OwlmError::InvalidInterval);
    }
    if (out.tx_gptp_ns <= 0) {
        return make_error_code(OwlmError::SenderNotSynced);
    }
    return {};
}

}  // namespace statusbar::owlm
