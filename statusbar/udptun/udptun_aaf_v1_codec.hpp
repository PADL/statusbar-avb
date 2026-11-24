#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// udptun codec for IEEE 1722-2025 AAF version 1 carried over IEEE 1722 Annex J
/// UDP encapsulation. This is the inter-site audio transport's wire format.
///
/// Wire layout of the UDP datagram payload (header_size() == 44 bytes):
///   Bytes 0-3:   Annex J encapsulation_sequence_num (32-bit)
///   Bytes 4-43:  AAF v1 common stream header (40 bytes)
///   Bytes 44+:   PCM audio payload (samples_per_packet * channels * bytes/sample)
///
/// TAI timebase. The AAF v1 64-bit avtp_timestamp carries the sample's
/// presentation time in **TAI** nanoseconds (International Atomic Time, not GPS
/// and not UTC — leap-immune), and ptp_grandmaster_identity is set to
/// GRANDMASTER_TAI_FROM_GPS to tag the clock domain at the transport layer. The
/// framework's `tx_gptp_ns(pkt)` accessor therefore returns the TAI presentation
/// time, which the udptun session uses as the slot key and lateness basis. The
/// TAI value is produced by `make_realtime_tai_translator` (CLOCK_REALTIME + a
/// configured leap offset) — no PTP/PHC involvement.
///
/// Stateful: the codec carries the stream's format/rate/channels/bit_depth and
/// the primary (and optional redundant) stream_id. Primary vs redundant is
/// distinguished by which configured stream_id a received packet carries;
/// `sender_pair_id` collapses both to the primary so the per-source tracker
/// shows one logical row per stream.
///
/// `encode()` writes only the 44-byte header (per the udptun::Codec contract);
/// the PCM payload at [header_size()..] is filled by the ingest path and left
/// untouched here. `decode()` exposes the payload span via DecodedPacket::audio.

#include "statusbar/avtp/avtp_aaf.hpp"
#include "statusbar/avtp/avtp_aaf_v1.hpp"
#include "statusbar/avtp/avtp_ip_encap.hpp"
#include "statusbar/buffer/span_utils.hpp"
#include "statusbar/ieee/ieee_ethernet.hpp"
#include "statusbar/tsn/tsn_clock_identity.hpp"
#include "statusbar/tsn/tsn_stream_id.hpp"
#include "statusbar/tsn/tsn_well_known_grandmasters.hpp"
#include "statusbar/udptun/udptun_codec_concept.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace statusbar::udptun {

class AafV1OverAnnexJCodec
{
  public:
    /// Per-stream configuration. `stream_id` is the primary stream identity;
    /// `redundant_stream_id` defaults equal to it (no redundancy → packets
    /// classify as legacy). The grandmaster defaults to the TAI-from-GPS
    /// well-known identity so the wire tags the TAI clock domain.
    struct Config
    {
        tsn::StreamId stream_id{};
        tsn::StreamId redundant_stream_id{};
        avtp::AafFormat format{avtp::AafFormat::int_32bit};
        avtp::AafSampleRate sample_rate{avtp::AafSampleRate::rate_96_khz};
        uint16_t channels{8};
        uint8_t bit_depth{32};
        uint16_t samples_per_packet{12};  // frames per channel per packet
        uint32_t interval_us{125};        // announced TX interval (cadence hint)
        tsn::ClockIdentity grandmaster{tsn::GRANDMASTER_TAI_FROM_GPS};
    };

    /// Parsed packet. `audio` is a view into the decode() source buffer and is
    /// valid only while that buffer lives (the framework processes RX inline).
    struct DecodedPacket
    {
        avtp::AafV1Pdu pdu{};
        uint32_t encap_seq{0};
        std::span<uint8_t const> audio{};
    };

    AafV1OverAnnexJCodec() = default;
    explicit AafV1OverAnnexJCodec(Config cfg) noexcept
        : cfg_{cfg}
    {}

    [[nodiscard]] auto config() const noexcept -> Config const& { return cfg_; }

    /// Wire size of the [encap][AAF v1] header (44 bytes), excluding PCM payload.
    [[nodiscard]] static constexpr auto header_size() noexcept -> size_t
    {
        return avtp::IpAvtpduHeader::LENGTH + avtp::AafV1Pdu::HEADER_LENGTH;
    }

    /// PCM payload size for one packet at this codec's configured dimensions.
    [[nodiscard]] static constexpr auto payload_bytes(Config const& c) noexcept -> size_t
    {
        return static_cast<size_t>(c.samples_per_packet) * c.channels * avtp::aaf_bytes_per_sample(c.format);
    }
    [[nodiscard]] auto payload_bytes() const noexcept -> size_t { return payload_bytes(cfg_); }

    /// Encode the 44-byte header into `buf` at offset 0. The PCM payload at
    /// [header_size()..] is written by the caller (ingest) and left untouched.
    /// `tx_tai_ns` is the sample's TAI presentation time (goes into the 64-bit
    /// avtp_timestamp). `sender_id` becomes the AAF stream_id.
    auto encode(
        std::span<uint8_t> buf,
        ieee::Eui64 const sender_id,
        uint32_t const sequence,
        int64_t const tx_tai_ns,
        uint32_t const interval_us) const noexcept -> size_t
    {
        return encode(buf, sender_id, sequence, tx_tai_ns, interval_us, /*timestamp_uncertain=*/false);
    }

    /// As above, with the header's tu (timestamp uncertain) bit. A talker sets
    /// it while it cannot vouch for the timeline its timestamps are on: here,
    /// while the clock the TAI is made from is outside the talker's configured
    /// accuracy limit. The timestamp is still the talker's best, and the
    /// grandmaster identity still names the timeline it is meant to be on.
    auto encode(
        std::span<uint8_t> buf,
        ieee::Eui64 const sender_id,
        uint32_t const sequence,
        int64_t const tx_tai_ns,
        uint32_t const interval_us,
        bool const timestamp_uncertain) const noexcept -> size_t
    {
        store_encapsulation(buf, sequence);
        auto pdu = stream_pdu(sender_id, sequence);
        stamp(pdu, tx_tai_ns, timestamp_uncertain);
        statusbar::span_store(buf.subspan(avtp::IpAvtpduHeader::LENGTH), pdu);
        (void)interval_us;  // AAF carries cadence via avtp_timestamp, not on the wire
        return header_size();
    }

    [[nodiscard]] auto decode(std::span<uint8_t const> const bytes) const noexcept -> std::optional<DecodedPacket>
    {
        if (bytes.size() < header_size()) {
            return std::nullopt;
        }
        auto const encap = avtp::ip_avtpdu_parse_header(bytes);
        if (!encap) {
            return std::nullopt;
        }
        auto const avtpdu = avtp::ip_avtpdu_get_payload(bytes);
        auto const pdu = avtp::aaf_v1_parse_header(avtpdu);
        if (!pdu) {
            return std::nullopt;
        }
        DecodedPacket out{};
        out.pdu = *pdu;
        out.encap_seq = encap->encapsulation_sequence_num();
        out.audio = avtp::aaf_v1_get_audio_payload(avtpdu);
        return out;
    }

    [[nodiscard]] auto validate_for_reflect(std::span<uint8_t const> const bytes) const noexcept -> bool
    {
        return decode(bytes).has_value();
    }

    [[nodiscard]] auto classify(DecodedPacket const& p, ieee::Eui64 const& my_pair_id) const noexcept -> PacketRole
    {
        bool const self = (sender_pair_id(p) == my_pair_id);
        if (has_redundancy()) {
            auto const sid = p.pdu.stream_id();
            if (sid == cfg_.redundant_stream_id) {
                return self ? PacketRole::SelfRedundant : PacketRole::RemoteRedundant;
            }
            if (sid == cfg_.stream_id) {
                return self ? PacketRole::SelfPrimary : PacketRole::RemotePrimary;
            }
        }
        return self ? PacketRole::SelfLegacy : PacketRole::RemoteLegacy;
    }

    [[nodiscard]] auto sender_id(DecodedPacket const& p) const noexcept -> ieee::Eui64 { return to_eui64(p.pdu.stream_id()); }

    /// Canonical logical sender: a redundant packet collapses to the primary
    /// stream_id so the per-source tracker shows one row per logical stream.
    [[nodiscard]] auto sender_pair_id(DecodedPacket const& p) const noexcept -> ieee::Eui64
    {
        if (has_redundancy() && p.pdu.stream_id() == cfg_.redundant_stream_id) {
            return to_eui64(cfg_.stream_id);
        }
        return to_eui64(p.pdu.stream_id());
    }

    [[nodiscard]] auto sequence(DecodedPacket const& p) const noexcept -> uint32_t { return p.pdu.get_sequence_num(); }

    /// Presentation time in **TAI** nanoseconds (the 64-bit avtp_timestamp).
    /// Field accessor name kept for the udptun::Codec concept.
    [[nodiscard]] auto tx_gptp_ns(DecodedPacket const& p) const noexcept -> int64_t
    {
        return static_cast<int64_t>(p.pdu.get_avtp_timestamp());
    }

    [[nodiscard]] auto announced_interval_us(DecodedPacket const& /*p*/) const noexcept -> uint32_t { return cfg_.interval_us; }

  private:
    /// The Annex J encapsulation header at the front of the datagram.
    static void store_encapsulation(std::span<uint8_t> buf, uint32_t const sequence) noexcept
    {
        avtp::IpAvtpduHeader encap{};
        encap.init(sequence);
        statusbar::span_store(buf, encap);
    }

    /// The stream's header before it is stamped: identity, format and dimensions.
    [[nodiscard]] auto stream_pdu(ieee::Eui64 const sender_id, uint32_t const sequence) const noexcept -> avtp::AafV1Pdu
    {
        avtp::AafV1Pdu pdu{};
        pdu.init(to_stream_id(sender_id), cfg_.format, cfg_.sample_rate, cfg_.channels, cfg_.bit_depth);
        pdu.set_sequence_num(sequence);
        pdu.set_dimensions(cfg_.samples_per_packet, cfg_.channels);
        return pdu;
    }

    /// The presentation time, whether the talker vouches for it, and the
    /// timeline it is on.
    void stamp(avtp::AafV1Pdu& pdu, int64_t const tx_tai_ns, bool const timestamp_uncertain) const noexcept
    {
        pdu.set_tv(true);
        pdu.set_tu(timestamp_uncertain);
        pdu.set_avtp_timestamp(static_cast<uint64_t>(tx_tai_ns));  // TAI presentation time
        pdu.set_ptp_grandmaster_identity(cfg_.grandmaster);        // GRANDMASTER_TAI_FROM_GPS
    }

    [[nodiscard]] auto has_redundancy() const noexcept -> bool { return cfg_.redundant_stream_id != cfg_.stream_id; }

    [[nodiscard]] static auto to_stream_id(ieee::Eui64 const& e) noexcept -> tsn::StreamId
    {
        tsn::StreamId sid{};
        sid.from_uint64(e.to_uint64());
        return sid;
    }

    [[nodiscard]] static auto to_eui64(tsn::StreamId const& sid) noexcept -> ieee::Eui64
    {
        ieee::Eui64 e{};
        e.from_uint64(sid.to_uint64());
        return e;
    }

    Config cfg_{};
};

static_assert(udptun::Codec<AafV1OverAnnexJCodec>, "AafV1OverAnnexJCodec must satisfy the udptun::Codec concept");

}  // namespace statusbar::udptun
