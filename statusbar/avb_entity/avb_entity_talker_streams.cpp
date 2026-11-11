// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// TalkerStreams methods — the local AVB stream TX path. Slots are shaped by
// StreamSpecs derived from the entity blob (kit phase 1); the per-kind
// serialization bodies are unchanged from the named-slot era.

#include "statusbar/avb_entity/avb_entity_talker_streams.hpp"

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/buffer/buffer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <system_error>

namespace statusbar::avb_entity {

namespace {

/// One slot's serializer for an AM824 spec.
auto open_am824(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id) -> Status
{
    auto const rate = avtp::am824_sample_rate_from_hz(spec.format.sample_rate_hz);
    if (!rate || spec.format.channels == 0) {
        return failure(std::errc::invalid_argument);
    }
    slot.am824.emplace(stream_id, *rate, static_cast<uint8_t>(spec.format.channels), /*pres_offset=*/0);
    return success();
}

/// One slot's serializer and reframer for an AAF spec. The frame is the
/// format's: its samples_per_frame, else the Table 18 count for its rate (6
/// at 44.1 kHz, not rate/8000 = 5).
auto open_aaf(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id, std::pmr::memory_resource* mr)
    -> Status
{
    auto const rate = avtp::aaf_sample_rate_from_hz(spec.format.sample_rate_hz);
    slot.aaf_samples = static_cast<uint16_t>(aaf_samples_per_pdu(spec.format));
    if (!rate || spec.format.channels == 0 || slot.aaf_samples == 0) {
        return failure(std::errc::invalid_argument);
    }
    slot.aaf.emplace(stream_id, spec.format.aaf_format, *rate, spec.format.channels, spec.format.bit_depth, /*pres_offset=*/0);
    slot.reframer.emplace(spec.format.channels, slot.aaf_samples, 4, mr);
    return success();
}

/// One slot's serializer for a CRF spec.
auto open_crf(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id) -> Status
{
    if (spec.format.crf_base_frequency_hz == 0 || spec.format.crf_timestamps_per_pdu == 0) {
        return failure(std::errc::invalid_argument);
    }
    slot.crf.emplace(
        stream_id,
        static_cast<avtp::CrfType>(spec.format.crf_type),
        spec.format.crf_base_frequency_hz,
        static_cast<avtp::CrfPull>(spec.format.crf_pull),
        spec.format.crf_timestamp_interval,
        spec.format.crf_timestamps_per_pdu);
    return success();
}

/// The serializer @p spec's kind calls for, engaged on @p slot.
auto open_kind(TalkerStreamSlot& slot, StreamSpec const& spec, tsn::StreamId const stream_id, std::pmr::memory_resource* mr)
    -> Status
{
    switch (spec.format.kind) {
        case StreamKind::am824:
            return open_am824(slot, spec, stream_id);
        case StreamKind::aaf:
            return open_aaf(slot, spec, stream_id, mr);
        case StreamKind::crf:
            return open_crf(slot, spec, stream_id);
        case StreamKind::other:
        default:
            return failure(std::errc::not_supported);
    }
}

}  // namespace

auto TalkerStreams::open_stream(StreamSpec const& spec, tsn::StreamId const stream_id, ieee::Eui48 const dest_mac) -> Status
{
    if (slots_.size() == MAX_ENTITY_STREAMS) {
        return failure(std::errc::result_out_of_range);
    }
    TalkerStreamSlot slot{.spec = spec, .dest_mac = dest_mac};
    if (auto opened = open_kind(slot, spec, stream_id, mem_resource_); !opened) {
        return opened;
    }
    slots_.push_back(std::move(slot));
    return success();
}

auto TalkerStreams::slot_for(uint16_t const stream_index) noexcept -> TalkerStreamSlot*
{
    for (auto& slot : slots_) {
        if (slot.spec.index == stream_index) {
            return &slot;
        }
    }
    return nullptr;
}

auto TalkerStreams::slot_for(uint16_t const stream_index) const noexcept -> TalkerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.spec.index == stream_index) {
            return &slot;
        }
    }
    return nullptr;
}

auto TalkerStreams::slot_of(StreamKind const kind) noexcept -> TalkerStreamSlot*
{
    for (auto& slot : slots_) {
        if (slot.spec.format.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

auto TalkerStreams::slot_of(StreamKind const kind) const noexcept -> TalkerStreamSlot const*
{
    for (auto const& slot : slots_) {
        if (slot.spec.format.kind == kind) {
            return &slot;
        }
    }
    return nullptr;
}

void TalkerStreams::transmit_am824(
    TalkerStreamSlot& slot, realtime::GptpClock0::time_point const presentation, uint32_t const samples, std::span<float const> src)
{  // NOLINT(statusbar-block-statements) one frame, built field by field; pre-dates the gate
    if (!slot.am824 || port_ == nullptr) {
        return;
    }
    // The wire takes the presentation time as unsigned nanoseconds (and keeps 32 bits of it).
    auto const presentation_ns = static_cast<uint64_t>(realtime::GptpClock0::to_ns(presentation));
    static constexpr size_t MAX_FRAME =
        avtp::Am824Pdu::HEADER_LENGTH + (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);
    std::array<uint8_t, MAX_FRAME> frame{};

    size_t const channels = slot.spec.format.channels;
    avtp::Am824Pdu pdu{};
    // Nominal AM824 rate (CIP FDF) from the configured sample rate: hardcoding
    // 96 kHz would mislabel a 48 kHz stream on the wire; value_or keeps the
    // 96 kHz default for any unmapped rate.
    auto const am824_rate = avtp::am824_sample_rate_from_hz(config_.sample_rate).value_or(avtp::Am824SampleRate::rate_96_khz);
    pdu.init(slot.am824->stream_id, static_cast<uint8_t>(channels), am824_rate);

    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::Am824Pdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::am824_serialize_mbla(
        *slot.am824,
        pdu,
        payload,
        static_cast<uint8_t>(samples),
        presentation_ns,
        [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    span_store(std::span<uint8_t>{frame}.first(avtp::Am824Pdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::Am824Pdu::HEADER_LENGTH + audio_bytes;
    // AVB stream frames must be VLAN-tagged with the SR class VID + PCP so bridges
    // admit them to the reserved SR class (untagged AVTP is not part of any SR class).
    // pcap stamp = the gPTP WALL-CLOCK transmit time (not the presentation time,
    // which is the media-clock timestamp written into the AVTP header). Using
    // the wall clock lets a capture reveal the real presentation lead
    // (avtp_ts - wall_clock = presentation_offset).
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    send_stream_frame(slot.dest_mac, std::span<uint8_t const>{frame.data(), frame_len});
    ++slot.tx_packets;
}

void TalkerStreams::transmit_aaf(
    TalkerStreamSlot& slot, realtime::GptpClock0::time_point const presentation, uint16_t const samples, std::span<float const> src)
{  // NOLINT(statusbar-block-statements) one frame, built field by field; pre-dates the gate
    if (!slot.aaf || port_ == nullptr) {
        return;
    }
    auto const presentation_ns = static_cast<uint64_t>(realtime::GptpClock0::to_ns(presentation));
    static constexpr size_t MAX_FRAME =
        avtp::AafPdu::HEADER_LENGTH + (avtp::Am824Pdu::MAX_SAMPLES_PER_PACKET * avtp::Am824Pdu::MAX_CHANNELS * 4);
    std::array<uint8_t, MAX_FRAME> frame{};

    size_t const channels = slot.spec.format.channels;
    avtp::AafPdu pdu{};
    std::span<uint8_t> const payload = std::span<uint8_t>{frame}.subspan(avtp::AafPdu::HEADER_LENGTH);
    size_t const audio_bytes = avtp::aaf_stream_serialize(
        *slot.aaf, pdu, payload, samples, presentation_ns, [src, channels](uint8_t ch, std::span<float> dest) {
            for (size_t s = 0; s < dest.size(); ++s) {
                dest[s] = src[(s * channels) + ch];
            }
        });
    if (audio_bytes == 0) {
        return;
    }

    span_store(std::span<uint8_t>{frame}.first(avtp::AafPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::AafPdu::HEADER_LENGTH + audio_bytes;
    // pcap stamp = gPTP WALL-CLOCK transmit time (see transmit_am824); the
    // presentation time is the media-clock timestamp written into the AVTP header.
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);
    send_stream_frame(slot.dest_mac, std::span<uint8_t const>{frame.data(), frame_len});
    ++slot.tx_packets;
}

void fill_crf_timestamps(
    ptpclient::MediaClockGenerator const& media_clock,
    uint64_t const base_index,
    uint32_t const sample_stride,
    uint16_t const n_ts,
    std::span<uint8_t> ts_data) noexcept
{
    // Timestamps come from the media clock, so the CRF conveys exactly our media
    // clock (event times, evenly spaced by sample_stride in the SAMPLE_RATE domain).
    // timestamp_for() already carries the presentation offset (Open1722's CRF
    // talker likewise offsets by the max transit time).
    for (uint16_t i = 0; i < n_ts; ++i) {
        uint64_t const ts = media_clock.timestamp_for(base_index + (static_cast<uint64_t>(i) * sample_stride));
        (void)avtp::crf_set_timestamp(ts_data, i, ts);
    }
}

void TalkerStreams::transmit_crf(TalkerStreamSlot& slot, uint64_t const base_index)
{  // NOLINT(statusbar-block-statements) one frame, built field by field; pre-dates the gate
    if (!slot.crf || port_ == nullptr || !media_clock_.anchored()) {
        return;
    }
    uint16_t const n_ts = slot.crf->timestamps_per_packet;
    uint16_t const interval = slot.crf->timestamp_interval;
    // Convert the DECLARED interval (in CRF base-frequency events) into the audio
    // sample-index domain that media_clock_.timestamp_for() speaks. With a 48 kHz
    // CRF base and 96 kHz audio, each declared CRF event spans SAMPLE_RATE/base
    // (= 2) audio samples, so the emitted timestamp VALUES stay spaced at
    // interval/base seconds regardless of the base we advertise.
    uint32_t const sample_stride = crf_sample_stride(interval, slot.crf->base_frequency, config_.sample_rate);

    static constexpr size_t MAX_FRAME = avtp::CrfPdu::HEADER_LENGTH + (64 * avtp::CrfPdu::TIMESTAMP_SIZE);
    std::array<uint8_t, MAX_FRAME> frame{};

    avtp::CrfPdu pdu{};
    pdu.init_audio_sample(slot.crf->stream_id, slot.crf->base_frequency, slot.crf->pull, interval, n_ts);
    pdu.set_sequence_num(slot.crf->sequence_num);

    std::span<uint8_t> const ts_data = std::span<uint8_t>{frame}.subspan(avtp::CrfPdu::HEADER_LENGTH);
    fill_crf_timestamps(media_clock_, base_index, sample_stride, n_ts, ts_data);
    slot.crf->sequence_num = static_cast<uint8_t>((slot.crf->sequence_num + 1U) & 0xFFU);
    ++slot.crf->packets_sent;

    span_store(std::span<uint8_t>{frame}.first(avtp::CrfPdu::HEADER_LENGTH), pdu);
    size_t const frame_len = avtp::CrfPdu::HEADER_LENGTH + (static_cast<size_t>(n_ts) * avtp::CrfPdu::TIMESTAMP_SIZE);
    last_tx_gptp_ns_ = last_gptp_ns_.load(std::memory_order_relaxed);  // gPTP timestamp for the optional TX pcap tap
    send_stream_frame(slot.dest_mac, std::span<uint8_t const>{frame.data(), frame_len});
    ++slot.tx_packets;
}

auto TalkerStreams::open_tx(StreamPort& port, TxPcapConfig const& pcap) -> Status
{
    // A port that did not open is not bound: the slots then stay silent, as
    // they did when the socket would not open.
    port_ = port.valid() ? &port : nullptr;
    // Optional TX stream capture: the egress of a bypass socket or a ring
    // is invisible to any local capture, so the frames are recorded here as
    // they are built, gPTP-timestamped.
    if (!pcap.path.empty()) {
        tx_pcap_recorder_.configure(
            std::string{pcap.path}, pcap.max_bytes, /*snaplen=*/1522, static_cast<uint64_t>(pcap.seconds) * 1'000'000'000ULL);
    }
    return port.valid() ? success() : failure(std::errc::not_connected);
}

void TalkerStreams::close_tx() noexcept
{
    slots_.clear();
    port_ = nullptr;
    external_kick_ = false;
    for (auto& stopped : stream_stopped_) {
        stopped.store(false, std::memory_order_release);
    }
}

void TalkerStreams::send_stream_frame(ieee::Eui48 const& dest_mac, std::span<uint8_t const> const payload)
{
    StreamPort::Guard const guard{*port_};
    auto slot = port_->port().tx_start();
    if (!slot) {
        return;
    }
    size_t const length =
        build_stream_frame(slot->buffer, dest_mac, port_->port().hardware_address(), config_.vlan_id, config_.stream_pcp, payload);
    if (length == 0) {
        (void)port_->port().tx_cancel(slot->handle);
        return;
    }
    commit_stream_frame(*slot, length);
}

void TalkerStreams::commit_stream_frame(net::EthernetTxSlot const& slot, size_t const length)
{
    // Recorded before the commit: a ring slot belongs to the kernel after it.
    if (tx_pcap_recorder_.recording()) {
        tx_pcap_recorder_.record(slot.buffer.first(length), last_tx_gptp_ns_);
    }
    (void)port_->port().tx_commit(slot.handle, length);
}

void TalkerStreams::flush_tx()
{
    if (port_ == nullptr) {
        return;
    }
    StreamPort::Guard const guard{*port_};
    if (!external_kick_) {
        (void)port_->port().tx_flush();
    }
    (void)port_->port().end_cycle();
}

auto TalkerStreams::set_external_kick(bool const enable) noexcept -> bool
{
    bool const kickable = port_ != nullptr;
    external_kick_ = enable && kickable;
    return !enable || kickable;
}

void TalkerStreams::kick_tx()
{
    if (port_ != nullptr) {
        (void)port_->port().tx_flush();
    }
}

void TalkerStreams::transmit_if_due(
    TalkerStreamSlot& slot,
    ptpclient::MediaClockGenerator::Emit const& tick,
    bool const srp_gate_open,
    size_t const samples,
    std::span<float const> src)
{
    // A STOP_STREAMING'd stream behaves exactly like a closed SRP gate:
    // no emission, AAF FIFO cleared, CRF phase reset for a clean restart.
    bool const gate_open = srp_gate_open && !stream_stopped(slot.spec.index);
    switch (slot.spec.format.kind) {
        case StreamKind::am824: {
            if (gate_open && samples > 0) {
                transmit_am824(slot, presentation_for(tick.first_index), tick.samples, src);
            }
            break;
        }
        case StreamKind::aaf:
            serve_aaf(slot, tick, gate_open, samples, src);
            break;
        case StreamKind::crf:
            serve_crf(slot, tick, gate_open, samples);
            break;
        case StreamKind::other:
        default:
            break;
    }
}

void TalkerStreams::serve_aaf(
    TalkerStreamSlot& slot,
    ptpclient::MediaClockGenerator::Emit const& tick,
    bool const gate_open,
    size_t const samples,
    std::span<float const> src)
{
    if (!slot.reframer) {
        return;
    }
    if (!gate_open) {
        // Gate closed: drop any partial block so a later reconnect starts
        // clean (no stale samples / stale timestamps).
        slot.reframer->clear();
        return;
    }
    // AAF frames are constant-size (1722-2025 7.7): buffer this wake's
    // variable samples and emit only whole blocks of the format's samples per
    // AVTPDU (0, 1, or 2+ this wake); the remainder carries to the next wake.
    // The cadence is the media clock's, not the wake's: 6 samples at 44.1 kHz
    // is one frame per ~136 us, with a wake now and then that sends none. Each
    // block's avtp_timestamp is the jitter-free media-clock time of its first
    // sample.
    slot.reframer->push(src.first(samples * slot.spec.format.channels), static_cast<uint16_t>(samples), tick.first_index);
    slot.reframer->drain([this, &slot](uint64_t first_index, std::span<float const> block) {
        transmit_aaf(slot, presentation_for(first_index), slot.aaf_samples, block);
    });
}

void TalkerStreams::serve_crf(
    TalkerStreamSlot& slot, ptpclient::MediaClockGenerator::Emit const& tick, bool const gate_open, size_t const samples)
{
    if (!slot.crf) {
        return;
    }
    if (!gate_open) {
        slot.crf_due.reset();  // gate closed: the next PDU starts from the live position
        return;
    }
    // One PDU carries timestamps_per_packet timestamps, each spaced
    // sample_stride audio samples, so a PDU is owed every span samples of the
    // media clock (the Milan 48 kHz/interval-96/1-ts format under 96 kHz audio:
    // every 192 samples, 500 PDU/s; the same format at 44.1 kHz: every 96
    // samples, 459.375 PDU/s). Counting samples rather than wakes is what
    // gives a rate the wake cadence does not divide its exact count.
    uint32_t const sample_stride = crf_sample_stride(slot.crf->timestamp_interval, slot.crf->base_frequency, config_.sample_rate);
    auto const plan = crf_schedule(
        slot.crf_due,
        tick.first_index,
        static_cast<uint32_t>(samples),
        sample_stride,
        static_cast<uint64_t>(slot.crf->timestamps_per_packet) * sample_stride);
    slot.crf_due = plan.due;
    if (plan.send) {
        transmit_crf(slot, *plan.send);
    }
}

}  // namespace statusbar::avb_entity
