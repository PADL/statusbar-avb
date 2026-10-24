// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// StreamPort helpers and PacketStreamPort.

#include "statusbar/avb_entity/avb_entity_stream_port.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace statusbar::avb_entity {

void write_stream_header(
    std::span<uint8_t> const header,
    ieee::Eui48 const& dest,
    ieee::Eui48 const& src,
    uint16_t const vid,
    uint8_t const pcp) noexcept
{
    ieee::EthernetFrame const l2{
        .dest_mac = dest, .src_mac = src, .vlan_tag = ieee::VlanTag{vid, false, pcp}, .ethertype = avtp::AVTP_ETHERTYPE};
    (void)ieee::store_unchecked(header.first(StreamPort::TX_PAYLOAD_OFFSET), l2);
}

auto stream_payload_offset(std::span<uint8_t const> const frame) noexcept -> size_t
{
    ieee::EthernetFrame l2{};
    if (!ieee::can_load(frame, &l2)) {
        return 0;
    }
    size_t const offset = ieee::load_unchecked(frame, &l2);
    return l2.ethertype == avtp::AVTP_ETHERTYPE ? offset : 0;
}

auto PacketStreamPort::open_tx(std::string_view const interface_name, uint8_t const priority) -> Status
{
    // qdisc-bypass: our own egress must not be re-received by our RX socket.
    // Transmit-only: nothing reads this socket, so it must not be handed (and
    // drop) a copy of every AVTP frame on the host.
    if (auto status = tx_.open(interface_name, avtp::AVTP_ETHERTYPE, nullptr, /*qdisc_bypass=*/true, /*transmit_only=*/true);
        !status) {
        return status;
    }
    // Bypass frames take their TX queue from skb->priority through the
    // prio-to-TC map. Best effort: priority 7 needs CAP_NET_ADMIN, and
    // without it the streams flow on the default queue.
    auto const set = tx_.set_priority(priority);
    tx_priority_error_ = set ? std::error_code{} : set.error();
    return success();
}

auto PacketStreamPort::open_rx(std::string_view const interface_name) -> Status
{
    if (auto status = rx_.open(interface_name, avtp::AVTP_ETHERTYPE, nullptr, /*qdisc_bypass=*/false); !status) {
        return status;
    }
    // Only the stream formats a listener decodes: no ATDECC/MAAP control
    // frames on the media path.
    static constexpr std::array<uint8_t, 3> stream_subtypes{
        avtp::AvtpSubtype::iec_61883_iidc, avtp::AvtpSubtype::aaf, avtp::AvtpSubtype::crf};
    (void)rx_.set_subtype_filter(stream_subtypes);
    return success();
}

void PacketStreamPort::close() noexcept
{
    tx_.close();
    rx_.close();
}

auto PacketStreamPort::tx_begin(size_t const capacity) noexcept -> std::span<uint8_t>
{
    if (!tx_.valid() || capacity > tx_payload_.size()) {
        return {};
    }
    return std::span<uint8_t>{tx_payload_}.first(capacity);
}

auto PacketStreamPort::tx_commit(
    std::span<uint8_t const> const payload, ieee::Eui48 const& dest, uint16_t const vid, uint8_t const pcp) noexcept -> Status
{
    if (payload.data() != tx_payload_.data() || payload.size() > tx_payload_.size()) {
        count_tx(false);
        return failure(std::errc::invalid_argument);
    }
    auto const sent = tx_.send_vlan(&dest, payload, vid, pcp);
    count_tx(sent.has_value());
    if (!sent) {
        return failure(sent.error());
    }
    return success();
}

auto PacketStreamPort::rx_poll(RxFn const& fn, size_t const max_frames) noexcept -> size_t
{
    if (!rx_.valid()) {
        return 0;
    }
    std::span<uint8_t> const frame{rx_frame_};
    size_t delivered = 0;
    while (delivered < max_frames) {
        ieee::Eui48 src{};
        ieee::Eui48 dst{};
        auto const received = rx_.recv(&src, &dst, frame.subspan(UNTAGGED_PAYLOAD_OFFSET));
        if (!received || *received <= 0) {
            break;
        }
        auto const length = static_cast<size_t>(*received);
        ieee::EthernetFrame const l2{.dest_mac = dst, .src_mac = src, .vlan_tag = {}, .ethertype = avtp::AVTP_ETHERTYPE};
        (void)ieee::store_unchecked(frame, l2);
        count_rx();
        fn(frame.subspan(UNTAGGED_PAYLOAD_OFFSET, length), frame.first(UNTAGGED_PAYLOAD_OFFSET + length));
        ++delivered;
    }
    return delivered;
}

}  // namespace statusbar::avb_entity
