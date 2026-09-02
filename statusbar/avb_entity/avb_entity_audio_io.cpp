// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVB Entity Audio I/O Implementation (dual-format AM824 + AAF)
/// Implements AvbEntityAudioIO: blob-loaded model, shared N-channel DSP source,
/// one AM824 talker/listener (stream 0) and one AAF talker/listener (stream 1).

#include "statusbar/avb_entity/avb_entity_audio_io.hpp"

#include "statusbar/atdecc/atdecc.hpp"
#include "statusbar/atdecc/atdecc_adp.hpp"
#include "statusbar/atdecc/atdecc_aem_descriptor.hpp"
#include "statusbar/atdecc/atdecc_descriptor_storage.hpp"
#include "statusbar/avb_entity/avb_entity_descriptor_helpers.hpp"
#include "statusbar/buffer/buffer.hpp"
#include "statusbar/dsp/dsp.hpp"
#include "statusbar/dsp/dsp_biquad.hpp"
#include "statusbar/ieee/ieee.hpp"
#include "statusbar/nanoavb/nanoavb.hpp"
#include "statusbar/nanoavb/nanoavb_acmp.hpp"
#include "statusbar/nanoavb/nanoavb_adp.hpp"
#include "statusbar/nanoavb/nanoavb_aem_descriptor_storage_handler.hpp"
#include "statusbar/nanoavb/nanoavb_components.hpp"
#include "statusbar/nanoavb/nanoavb_entity.hpp"
#include "statusbar/nanoavb/nanoavb_entity_model.hpp"
#include "statusbar/nanoavb/nanoavb_gptp_sm.hpp"
#include "statusbar/nanoavb/nanoavb_listener_engine_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_listener_sm.hpp"
#include "statusbar/nanoavb/nanoavb_msrp_talker_sm.hpp"
#include "statusbar/nanoavb/nanoavb_mvrp_sm.hpp"
#include "statusbar/nanoavb/nanoavb_supervisor_sm.hpp"
#include "statusbar/nanoavb/nanoavb_talker_engine_sm.hpp"
#include "statusbar/net/net_message_reactor.hpp"
#include "statusbar/net/net_posix_util.hpp"
#include "statusbar/sm/sm.hpp"
#include "statusbar/status/catch_or_status.hpp"
#include "statusbar/status/status.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <memory>
#include <numbers>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace statusbar::avb_entity {

using namespace statusbar::atdecc;
using namespace statusbar::atdecc::aem;
using namespace statusbar::nanoavb;

//
// Factory method
//

auto AvbEntityAudioIO::create(AvbEntityAudioIOConfig config, std::pmr::memory_resource* memory_resource)
    -> StatusValue<std::unique_ptr<AvbEntityAudioIO>>
{
    auto storage_result = DescriptorStorage::create(std::span<uint8_t const>{config.descriptor_storage_blob});
    if (!storage_result) {
        return failure(storage_result.error());
    }

    // Symbol-aware: serve descriptors from the blob through a DescriptorStorageHandler
    // (retains the blob + its symbol table) instead of a parsed EntityModel. The handler
    // patches the runtime ENTITY identity + AVB_INTERFACE network/gPTP fields.
    size_t const channels = channels_from_storage(*storage_result, 2);
    auto const iface_mac = net::read_interface_mac(config.interface_name);
    auto handler = std::make_unique<EntityIdentityDescriptorHandler>(
        *storage_result,
        config.entity_id,
        config.entity_model_id,
        config.firmware_version,
        config.entity_name,
        iface_mac,
        /*patch_avb_interface=*/true);

    // Kit phase 3c: find the clock source backed by the CRF stream input
    // (INPUT_STREAM located at STREAM_INPUT CRF_INPUT_STREAM_INDEX) and the
    // CLOCK_DOMAIN's authored default selection.
    auto const crf_clock_source = find_input_stream_clock_source(*storage_result, CRF_INPUT_STREAM_INDEX);
    uint16_t const initial_clock_source = authored_clock_source(*storage_result);

    // This entity's fixed 3-stream topology (AM824@0, AAF@1, CRF@2) as
    // StreamSpec tables for the kit, mirrored on the input side (the CRF input
    // is the media-clock input). (The tone generator derives its specs from
    // the blob; migrating this entity's whole shape to the blob is future kit
    // work.)
    auto const make_spec = [&](uint16_t index, StreamKind kind) {
        StreamSpec spec{};
        spec.index = index;
        spec.format.kind = kind;
        spec.format.sample_rate_hz = SAMPLE_RATE;
        spec.format.channels = static_cast<uint16_t>(channels);
        spec.format.aaf_format = AAF_FORMAT;
        spec.format.bit_depth = kind == StreamKind::aaf ? AAF_BIT_DEPTH : uint8_t{24};
        if (kind == StreamKind::crf) {
            // Milan 48 kHz audio-sample reference, pull x1.0: both 48 kHz and
            // 96 kHz clients lock to it; our 96 kHz audio rides as a 2x multiple.
            spec.format.crf_type = static_cast<uint8_t>(avtp::CrfType::audio_sample);
            spec.format.crf_base_frequency_hz = CRF_BASE_FREQUENCY;
            spec.format.crf_timestamp_interval = config.crf_timestamp_interval;
            spec.format.crf_timestamps_per_pdu = static_cast<uint8_t>(config.crf_timestamps_per_packet);
        }
        return spec;
    };
    StreamSpecs talker_specs{};
    StreamSpecs listener_specs{};
    for (auto const& [index, kind] :
         {std::pair{AM824_STREAM_INDEX, StreamKind::am824},
          std::pair{AAF_STREAM_INDEX, StreamKind::aaf},
          std::pair{CRF_STREAM_INDEX, StreamKind::crf}}) {
        (void)talker_specs.try_push_back(make_spec(index, kind));
        (void)listener_specs.try_push_back(make_spec(index, kind));
    }

    auto* const storage_handler = handler.get();
    std::pmr::memory_resource* const mr = memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource();
    auto entity = std::make_unique<AvbEntityAudioIO>(
        AvbEntityAudioIO::CreateKey{},
        std::move(config),
        std::move(handler),
        storage_handler,
        talker_specs,
        listener_specs,
        initial_clock_source,
        crf_clock_source,
        channels,
        mr);

    entity->configure_filter(entity->config_.filter_freq_hz, entity->config_.filter_gain_db, entity->config_.filter_q);

    return success(std::move(entity));
}

//
// Constructor / Destructor
//

AvbEntityAudioIO::AvbEntityAudioIO(
    CreateKey,
    AvbEntityAudioIOConfig config,
    std::unique_ptr<nanoavb::AemEntityHandler> handler,
    nanoavb::DescriptorStorageHandler* storage_handler,
    StreamSpecs talker_specs,
    StreamSpecs listener_specs,
    uint16_t initial_clock_source,
    std::optional<uint16_t> crf_clock_source_index,
    size_t channels,
    std::pmr::memory_resource* memory_resource)
    : config_{std::move(config)}
    , channels_{channels}
    , mem_resource_{memory_resource}
    , biquads_(channels, dsp::BiQuad<float>{}, mem_resource_)
    , audio_buffer_((SAMPLES_PER_PACKET + 1) * channels, 0.0f, mem_resource_)  // +1: GPS pacing may emit nominal+1
    , oscillators_(channels, dsp::Oscillator<float>{}, mem_resource_)
    // The kit owns the control plane + stream paths + their wiring; the
    // listener delivers accepted stream audio to the tunnel (StreamRxAudioSink).
    , kit_{
          config_,
          std::move(handler),
          storage_handler,
          talker_specs,
          listener_specs,
          SAMPLE_RATE,
          initial_clock_source,
          crf_clock_source_index,
          media_clock_,
          last_gptp_ns_,
          udptun_.get(),
          mem_resource_}
{
    // Per-channel sine source: identical frequency, distinct initial phase.
    double const sr_recip = 1.0 / static_cast<double>(SAMPLE_RATE);
    for (size_t ch = 0; ch < channels_; ++ch) {
        double const phase =
            channels_ > 0 ? (2.0 * std::numbers::pi * static_cast<double>(ch) / static_cast<double>(channels_)) : 0.0;
        oscillators_[ch].state_.set_frequency(
            dsp::FrequencyParameters<double>{
                .sample_rate_recip = sr_recip, .frequency = config_.tone_freq_hz, .phase_in_radians = phase},
            0);
        oscillators_[ch].coeffs_.set_amplitude(config_.tone_amplitude, 0);
    }
}

AvbEntityAudioIO::~AvbEntityAudioIO()
{
    // Destructors must not throw; catch_or_status contains any exception from
    // stop() (and compiles to a plain call under -fno-exceptions).
    (void)statusbar::catch_or_status(
        [&]() -> statusbar::Status {
            if (is_running()) {
                (void)stop();
            }
            return {};
        },
        std::errc::io_error);
}

//
// Start / Stop
//

auto AvbEntityAudioIO::start(net::MessageReactor& reactor) -> Status
{
    udptun_->set_ctl_logger(kit_.host().ctl_log());

    // The deterministic media clock owns the presentation offset and supplies the
    // avtp_timestamp, so the stream-output contexts add ZERO extra offset -- the
    // timestamp we pass them is already the final presentation time. Assigned
    // before kit_.start() opens the TX slots (the kit's talker references it).
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(SAMPLE_RATE), .presentation_offset_ns = config_.presentation_offset_ns}};
    rate_tracker_.configure(ptpclient::KalmanRatioTracker::Config{.meas_noise_ns = 1000.0, .jerk_psd = 1e-3});

    // Control plane + the whole stream data plane (TX/RX slots, sockets, MAAP,
    // CRF recovery feed, persisted listener bindings) -- see AvbEntityKit.
    if (auto status = kit_.start(reactor); !status) {
        return status;
    }

    // Inter-site UDPTUN (optional; any failure is non-fatal -- the entity runs
    // its local AVB streams normally without the tunnel). The bridge selects
    // the establishment path (STUN punch worker / direct-shared / per-direction
    // direct sockets) from the config.
    udptun_->start();

    return success();
}

auto AvbEntityAudioIO::stop() -> Status
{
    if (!is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }

    // Tear down our data plane, then the shared control plane (the kit's host
    // stops ADP + releases the net handlers + clears running_).
    udptun_->stop();
    return kit_.stop();
}

void AvbEntityAudioIO::print_state() const
{
    auto const* tx_am824 = kit_.talker().slot_of(StreamKind::am824);
    auto const* tx_aaf = kit_.talker().slot_of(StreamKind::aaf);
    auto const* rx_am824 = kit_.listener()->slot_of(StreamKind::am824);
    auto const* rx_aaf = kit_.listener()->slot_of(StreamKind::aaf);
    std::print(
        "State: supervisor={} gptp={} mvrp={} acmp[am824={} aaf={}] channels={} | "
        "AM824 tx={} rx={} rx_samples={} rx_bad={} | AAF tx={} rx={} rx_samples={} rx_bad={} | "
        "egress[resets={} repunch={}]\n",
        state_string(),
        kit_.host().gptp_locked() ? "Locked" : "Unlocked",
        kit_.host().mvrp_joined() ? "Joined" : "NotJoined",
        kit_.host().components().acmp_talker.connection_count(AM824_STREAM_INDEX),
        kit_.host().components().acmp_talker.connection_count(AAF_STREAM_INDEX),
        channels_,
        tx_am824 != nullptr ? tx_am824->tx_packets : 0,
        rx_am824 != nullptr ? rx_am824->rx_packets.load() : 0,
        rx_am824 != nullptr ? rx_am824->rx_samples.load() : 0,
        rx_am824 != nullptr ? rx_am824->rx_bad.load() : 0,
        tx_aaf != nullptr ? tx_aaf->tx_packets : 0,
        rx_aaf != nullptr ? rx_aaf->rx_packets.load() : 0,
        rx_aaf != nullptr ? rx_aaf->rx_samples.load() : 0,
        rx_aaf != nullptr ? rx_aaf->rx_bad.load() : 0,
        udptun_->telemetry().egress_reset_count().load(),
        udptun_->telemetry().egress_repunch_count().load());
}

//
// Audio processing
//

void AvbEntityAudioIO::process_audio(TimePoint time)
{
    auto const base_now_ns = static_cast<uint64_t>(time.time_since_epoch().count());
    // One packet's worth of gPTP time: 12 samples @ 96 kHz = 125 us.
    constexpr uint64_t PACKET_INTERVAL_NS = (static_cast<uint64_t>(SAMPLES_PER_PACKET) * 1'000'000'000ULL) / SAMPLE_RATE;

    // Emit config_.packets_per_wake packets per stream this wake. Each packet
    // carries a fresh SAMPLES_PER_PACKET block (oscillators advance) and an
    // avtp_timestamp one packet interval later than the previous, so the on-wire
    // cadence (8000 x 12-sample Class A frames/s) and the listener's expected
    // 125 us timestamp step are UNCHANGED -- only the media-timer WAKE rate drops
    // by packets_per_wake, giving that many fewer deadlines to miss. The
    // burst-of-N egress is not strict Class A shaping; presentation_offset_ns
    // (>> N*125 us) absorbs it at the listener. See packets_per_wake doc.
    // Re-estimate the GPS frequency ratio that pins the media-clock rate. When
    // media_lock_to_gptp is set, r stays 1.0 (media clock == gPTP) and we skip the
    // CLOCK_REALTIME sampling entirely -- see Config::media_lock_to_gptp.
    if (!config_.media_lock_to_gptp) {
        update_gps_ratio(base_now_ns);
    }

    // Inter-site rendezvous punch-retry (media-thread half): install a freshly
    // hole-punched socket staged by the worker and watchdog the RX. Runs before
    // the drain so a just-installed socket is drained this same wake.
    if (udptun_->punch_service_active()) {
        udptun_->punch_service(realtime_tai_ns(config_.udptun_tai_offset_ns));
    }

    // Inter-site egress: drain the UDP socket and snapshot the TAI playout clock
    // once per wake (CLOCK_REALTIME + offset). Done on this (media-timer) thread so
    // AudioEgress stays single-threaded.
    int64_t udptun_now_tai_ns = 0;
    if (udptun_->egress_active()) {
        udptun_->egress_drain_rx();
        // Tunnel playout clock = GPS-TAI derived from the gPTP master via the TAI
        // translator (NOT raw CLOCK_REALTIME): the gPTP PHC gives a smooth,
        // jitter-free rate and the Kalman offset pins it to absolute GPS-TAI, so
        // this matches the rate the far ingest stamps with -> zero buffer drift.
        // Falls back to raw CLOCK_REALTIME+offset until the translator has a sample.
        if (rate_tracker_.has_tai_sample()) {
            udptun_now_tai_ns = rate_tracker_.tai_ns(static_cast<int64_t>(base_now_ns));
        } else if (int64_t const t = realtime_tai_ns(config_.udptun_tai_offset_ns); t != 0) {
            udptun_now_tai_ns = t;
        }
    }

    // Inter-site ingest silence-source gate (decided once per wake): keeps the
    // tunnel TX alive only while no real AVTP audio flows -- the moment real
    // audio arrives on the reactor thread, this media thread must stand down.
    // See EntityUdptunBridge::should_emit_silence for the full contract.
    bool const udptun_emit_silence = udptun_->should_emit_silence(udptun_now_tai_ns);

    for (size_t p = 0; p < config_.packets_per_wake; ++p) {
        uint64_t const wake_ns = base_now_ns + (static_cast<uint64_t>(p) * PACKET_INTERVAL_NS);
        // Publish the gPTP media time so the RX thread can judge LATE/EARLY_TIMESTAMP
        // against gPTP (the reactor's own clock is monotonic, not gPTP).
        last_gptp_ns_.store(wake_ns, std::memory_order_relaxed);

        // Deterministic, GPS-rate-pinned media clock: how many samples to emit
        // this tick (nominal +/- 1, paced to GPS) and the jitter-free presentation
        // timestamp of the packet's first sample. The wake time only paces the
        // count; the timestamp does NOT carry its jitter.
        // Rate source per the active CLOCK_DOMAIN selection (kit phase 3c):
        // the CRF-input clock source slaves the media clock to the recovered
        // remote rate (nominal 1.0 until the recovery locks); otherwise the
        // GPS-pinned tracker (or 1.0 when locked to gPTP) as before.
        double const r = kit_.media_rate(rate_tracker_.r());
        auto const tick = media_clock_.advance(wake_ns, r, static_cast<uint32_t>(SAMPLES_PER_PACKET));
        auto const samples = static_cast<size_t>(tick.samples);
        if (samples == 0) {
            continue;
        }

        // Source: generate the per-channel sine into the interleaved buffer.
        for (size_t i = 0; i < samples; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                audio_buffer_[(i * channels_) + ch] = oscillators_[ch](0.0F);
            }
        }
        // Per-channel biquad filters (interleaved layout).
        for (size_t i = 0; i < samples; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                audio_buffer_[(i * channels_) + ch] = biquads_[ch](audio_buffer_[(i * channels_) + ch]);
            }
        }

        // Inter-site tunnel, this packet's slot on the TAI timeline (now + p*125us).
        // Egress: replace the oscillator content with the de-tunneled audio for
        // this presentation time (the talkers below then emit the received stream
        // instead of the test tone). Source: the TAI-paced test sweep or the
        // silence filler -- the pacing/gating lives in the bridge (phase C).
        int64_t const udptun_pkt_tai =
            (udptun_now_tai_ns != 0) ? udptun_now_tai_ns + (static_cast<int64_t>(p) * static_cast<int64_t>(PACKET_INTERVAL_NS)) : 0;
        if (udptun_->egress_active() && udptun_pkt_tai != 0) {
            udptun_->egress_fill(udptun_pkt_tai, samples);
        }
        udptun_->source_tick(udptun_pkt_tai, samples, udptun_emit_silence);

        if (audio_callback_) {
            audio_callback_(std::span{audio_buffer_}.first(samples * channels_), samples);
        }

        // Transmit both formats from the same audio with the deterministic
        // presentation timestamp of this packet's first sample. We do NOT gate on
        // the announce-based supervisor lock; process_audio() only runs while the
        // PTP bridge is healthy (the media timer fires), so we have a usable clock.
        uint64_t const pts_base = media_clock_.timestamp_for(tick.first_index);
        // Gate each talker stream on a ready/connected listener (SRP): don't put
        // a stream on the wire when nobody is listening. The CRF media clock is
        // emitted whenever any audio talker is transmitting (its listeners lock
        // to it for the audio streams).
        // Steady-clock now for the MSRP-ready grace window (same clock the
        // on_talker_listener callback stamps msrp_ready_ns_ with).
        int64_t const now_steady_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        // Each slot gates on ITS OWN ACMP connection + reservation and emits per
        // its kind (AM824 direct, AAF reframed, CRF decimated).
        (void)pts_base;
        for (auto& slot : kit_.talker().slots_) {
            kit_.talker().transmit_if_due(
                slot, tick, kit_.talker_should_transmit(slot.spec.index, now_steady_ns), samples, audio_buffer_);
        }
    }
}

void AvbEntityAudioIO::update_gps_ratio(uint64_t gptp_now_ns)
{
    // Sample CLOCK_REALTIME (GPS, disciplined by chrony <- the GPS grandmaster)
    // against gPTP a few times per second; the rate tracker turns the offset
    // slope into r = switch/GPS, which pins the media-clock rate, and feeds the
    // same pair to its GPS-TAI translator for the tunnel timeline. Rate-limited
    // so the CLOCK_REALTIME syscall stays off the per-packet hot path.
    if (!rate_tracker_.should_sample(gptp_now_ns)) {
        return;
    }
    timespec ts{};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return;
    }
    uint64_t const gps_ns = (static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(ts.tv_nsec);
    auto const est = rate_tracker_.add_sample(gptp_now_ns, gps_ns);
    // Publish the updated GPS-TAI mapping for the reactor-thread tunnel ingest,
    // which must never read the single-threaded Kalman directly (data race).
    tai_snapshot_.publish(rate_tracker_.tai_snapshot());
    if (est.valid && rate_tracker_.should_log(gptp_now_ns)) {
        // Live media-clock telemetry (rate-limited; runs on the media-timer
        // thread, so keep it to once every few seconds). Lets an operator SEE the
        // GPS-vs-switch frequency tracking and catch a clock-coupling mistake:
        //   - r is switch_rate/GPS_rate; offset-slope = (r-1) in ppm = the
        //     fractional frequency between the gPTP PHC and GPS CLOCK_REALTIME.
        //   - PHC-REALTIME is the raw instantaneous phase offset (ns).
        // A HEALTHY decoupled setup (ptp4l -> /dev/ptp0 <- switch GM; chrony ->
        // CLOCK_REALTIME <- local GPS; NO phc2sys) shows a small but NON-ZERO,
        // stable slope -- the switch GM's free-running oscillator drifting against
        // GPS. A slope pinned at ~0.000 ppm with a flat offset means the PHC and
        // CLOCK_REALTIME are coupled (e.g. phc2sys is running) and the media clock
        // is NOT actually GPS-locked -- see umbrella docs/MEDIA_CLOCK_TIMING.md.
        // RT media thread: deferred-formatting logger — never std::print here.
        kit_.host().media_log().status(
            "media-clock r(switch/GPS)={:.9f}  offset-slope={:+.3f} ppm  PHC-REALTIME={} ns  unc=+/-{:.1f} ppb",
            est.r,
            est.ppm(),
            static_cast<int64_t>(est.filtered_offset_ns),
            est.freq_uncertainty_ppb);
        rate_tracker_.mark_logged(gptp_now_ns);
    }
}

void AvbEntityAudioIO::configure_filter(double const freq_hz, double const gain_db, double const q)
{
    double const sample_rate_recip = 1.0 / static_cast<double>(SAMPLE_RATE);
    for (size_t ch = 0; ch < channels_; ++ch) {
        biquads_[ch].coeffs.calculate_peak(
            {.sample_rate_recip = sample_rate_recip, .frequency = freq_hz, .q = q, .gain_db = gain_db, .channel = 0});
        biquads_[ch].reset();
    }
}

}  // namespace statusbar::avb_entity
