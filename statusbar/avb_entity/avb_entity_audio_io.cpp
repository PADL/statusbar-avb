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
#include "statusbar/avb_entity/avb_entity_identity.hpp"
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

namespace {

/// The format this entity gives a stream of @p kind.
[[nodiscard]] auto fixed_stream_format(AvbEntityAudioIOConfig const& config, size_t channels, StreamKind kind) -> StreamFormatFields
{
    StreamFormatFields format{
        .kind = kind,
        .sample_rate_hz = AvbEntityAudioIO::SAMPLE_RATE,
        .channels = static_cast<uint16_t>(channels),
        .bit_depth = kind == StreamKind::aaf ? AvbEntityAudioIO::AAF_BIT_DEPTH : uint8_t{24},
        .aaf_format = AvbEntityAudioIO::AAF_FORMAT};
    if (kind == StreamKind::crf) {
        // Milan 48 kHz audio-sample reference, pull x1.0: both 48 kHz and
        // 96 kHz clients lock to it; our 96 kHz audio rides as a 2x multiple.
        format.crf_type = static_cast<uint8_t>(avtp::CrfType::audio_sample);
        format.crf_base_frequency_hz = AvbEntityAudioIO::CRF_BASE_FREQUENCY;
        format.crf_timestamp_interval = config.crf_timestamp_interval;
        // In range: create() refuses a cadence that is not (crf_cadence_fits).
        format.crf_timestamps_per_pdu = static_cast<uint8_t>(config.crf_timestamps_per_packet);
    }
    return format;
}

/// This entity's fixed 3-stream topology (AM824@0, AAF@1, CRF@2) as a
/// StreamSpec table for the kit. The input side mirrors the output side (the
/// CRF input is the media-clock input), so one table serves both. (The tone
/// generator derives its specs from the blob; migrating this entity's whole
/// topology to the blob is future kit work.)
[[nodiscard]] auto fixed_stream_specs(AvbEntityAudioIOConfig const& config, size_t channels) -> StreamSpecs
{
    StreamSpecs specs{};
    for (auto const& [index, kind] :
         {std::pair{AvbEntityAudioIO::AM824_STREAM_INDEX, StreamKind::am824},
          std::pair{AvbEntityAudioIO::AAF_STREAM_INDEX, StreamKind::aaf},
          std::pair{AvbEntityAudioIO::CRF_STREAM_INDEX, StreamKind::crf}}) {
        (void)specs.try_push_back(StreamSpec{.index = index, .format = fixed_stream_format(config, channels, kind)});
    }
    return specs;
}

/// Everything the entity is built from that the descriptor blob decides.
struct AudioIoPlan
{
    EntityIdentityDescriptorHandler handler;
    StreamSpecs specs;
    uint16_t initial_clock_source;
    std::optional<uint16_t> crf_clock_source;
    size_t channels;
};

[[nodiscard]] auto plan_audio_io(AvbEntityAudioIOConfig const& config, DescriptorStorage const& storage) -> AudioIoPlan
{
    size_t const channels = channels_from_storage(storage, 2);
    // Kit phase 3c: the clock source backed by the CRF stream input
    // (INPUT_STREAM located at STREAM_INPUT CRF_INPUT_STREAM_INDEX) and the
    // CLOCK_DOMAIN's authored default selection.
    return AudioIoPlan{
        .handler = identity_handler(config, storage),
        .specs = fixed_stream_specs(config, channels),
        .initial_clock_source = authored_clock_source(storage),
        .crf_clock_source = find_input_stream_clock_source(storage, AvbEntityAudioIO::CRF_INPUT_STREAM_INDEX),
        .channels = channels};
}

static_assert(
    MAX_CRF_TIMESTAMPS_PER_PACKET == TalkerStreams::MAX_CRF_TIMESTAMPS_PER_PDU,
    "the configuration's limit is the talker's: what create() lets through, open_stream() takes");

/// The descriptor storage of a configuration an entity can be made from: its
/// CRF cadence is one a stream can have (crf_cadence_fits; invalid_argument
/// otherwise, before the value is cut to the stream format's narrower field)
/// and its blob parses.
[[nodiscard]] auto checked_storage(AvbEntityAudioIOConfig const& config) -> StatusValue<DescriptorStorage>
{
    if (!crf_cadence_fits(config)) {
        return failure(std::make_error_code(std::errc::invalid_argument));
    }
    return DescriptorStorage::create(std::span<uint8_t const>{config.descriptor_storage_blob});
}

}  // namespace

auto AvbEntityAudioIO::create(AvbEntityAudioIOConfig config, std::pmr::memory_resource* memory_resource)
    -> StatusValue<std::unique_ptr<AvbEntityAudioIO>>
{
    auto storage_result = checked_storage(config);
    if (!storage_result) {
        return failure(storage_result.error());
    }
    AudioIoPlan plan = plan_audio_io(config, *storage_result);
    auto entity = std::make_unique<AvbEntityAudioIO>(
        AvbEntityAudioIO::CreateKey{},
        std::move(config),
        std::move(plan.handler),
        plan.specs,
        plan.specs,
        plan.initial_clock_source,
        plan.crf_clock_source,
        plan.channels,
        memory_resource != nullptr ? memory_resource : std::pmr::get_default_resource());

    entity->configure_filter(entity->config_.filter_freq_hz, entity->config_.filter_gain_db, entity->config_.filter_q);

    return success(std::move(entity));
}

//
// Constructor / Destructor
//

AvbEntityAudioIO::AvbEntityAudioIO(
    CreateKey,
    AvbEntityAudioIOConfig config,
    EntityIdentityDescriptorHandler handler,
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
    , handler_{std::move(handler)}  // The kit owns the control plane + stream paths + their wiring; the
    // listener delivers accepted stream audio to the tunnel (StreamRxAudioSink).
    , kit_{
          config_,
          handler_,
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
    handler_.set_serial_number(config_.serial_number);
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

auto AvbEntityAudioIO::start(net::MessageReactor& reactor, TimePoint const now) -> Status
{
    udptun_->set_ctl_logger(kit_.host().ctl_log());

    // The deterministic media clock owns the presentation offset and supplies the
    // avtp_timestamp, so the stream-output contexts add ZERO extra offset -- the
    // timestamp we pass them is already the final presentation time. Assigned
    // before kit_.start() opens the TX slots (the kit's talker references it).
    media_clock_ = ptpclient::MediaClockGenerator{ptpclient::MediaClockGenerator::Config{
        .sample_rate_hz = static_cast<double>(SAMPLE_RATE), .presentation_offset_ns = effective_presentation_offset_ns(config_)}};
    rate_tracker_.configure(ptpclient::KalmanRatioTracker::Config{.meas_noise_ns = 1000.0, .jerk_psd = 1e-3});

    // Control plane + the whole stream data plane (TX/RX slots, sockets, MAAP,
    // CRF recovery feed, persisted listener bindings) -- see AvbEntityKit.
    if (auto status = kit_.start(reactor, now); !status) {
        return status;
    }

    // Inter-site UDPTUN (optional; any failure is non-fatal -- the entity runs
    // its local AVB streams normally without the tunnel). The bridge selects
    // the establishment path (STUN punch worker / direct-shared / per-direction
    // direct sockets) from the config.
    udptun_->start();

    return success();
}

auto AvbEntityAudioIO::stop(TimePoint const now) -> Status
{
    if (!is_running()) {
        return failure(std::make_error_code(std::errc::not_connected));
    }

    // Tear down our data plane, then the shared control plane (the kit's host
    // stops ADP + releases the net handlers + clears running_).
    udptun_->stop();
    return kit_.stop(now);
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
        "egress[resets={} repunch={}] | tunnel-clock[utc_error_ns={} excursions={} tu_tx={} tu_rx={}]\n",
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
        udptun_->telemetry().egress_repunch_count().load(),
        udptun_->telemetry().utc_accuracy_ns().load(),
        udptun_->telemetry().utc_excursions().load(),
        udptun_->telemetry().tx_uncertain().load(),
        udptun_->telemetry().rx_uncertain().load());
    if (auto const* listener = kit_.listener(); listener != nullptr) {
        listener->print_timing();
    }
}

//
// Audio processing
//

void AvbEntityAudioIO::process_audio(TimePoint time, realtime::MonotonicRawClock::time_point const raw_now)
{  // NOLINT(statusbar-block-statements) the start sequence, one step after another; pre-dates the gate
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
    // media_lock_to_gptp is set, r stays 1.0 (media clock == gPTP) and no pair
    // is fed to the ratio filter -- see Config::media_lock_to_gptp.
    // The system clock is judged either way: the tunnel's timestamps are made
    // from it even when the media clock does not follow it.
    update_gps_ratio(base_now_ns);

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

    for (size_t p = 0; p < config_.packets_per_wake;
         ++p) {  // NOLINT(statusbar-block-statements) the media tick, pre-dates the gate
        uint64_t const wake_ns = base_now_ns + (static_cast<uint64_t>(p) * PACKET_INTERVAL_NS);
        // Publish the gPTP media time so the RX thread can judge LATE/EARLY_TIMESTAMP
        // against gPTP (the reactor's own clock is monotonic, not gPTP).
        last_gptp_ns_.store(static_cast<int64_t>(wake_ns), std::memory_order_relaxed);

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
        // The gate is told this wake's RAW instant (its grace window's clock).
        // Each slot gates on ITS OWN ACMP connection + reservation and emits per
        // its kind (AM824 direct, AAF reframed, CRF decimated).
        (void)pts_base;
        for (auto& slot : kit_.talker().slots_) {
            kit_.talker().transmit_if_due(
                slot, tick, kit_.talker_should_transmit(slot.spec.index, raw_now), samples, audio_buffer_);
        }
        kit_.talker().flush_tx();
    }
}

void AvbEntityAudioIO::update_gps_ratio(uint64_t gptp_now_ns)
{
    // Sample CLOCK_REALTIME (GPS, disciplined by chrony <- the GPS grandmaster)
    // against gPTP a few times per second; the rate tracker turns the offset
    // slope into r = switch/GPS, which pins the media-clock rate, and feeds the
    // same pair to its GPS-TAI translator for the tunnel timeline. Rate-limited
    // so the clock reads stay off the per-packet hot path. A pair is fed only
    // while the system clock is within the entity's limit for following it:
    // outside it r holds.
    if (!rate_tracker_.should_sample(gptp_now_ns)) {
        return;
    }
    auto const clock = read_system_clock();
    auto const est = rate_tracker_.observe(gptp_now_ns, clock.utc_ns, clock.verdict, !config_.media_lock_to_gptp);
    // Publish the updated GPS-TAI mapping and the verdict for the reactor-thread
    // tunnel ingest, which must never read the single-threaded Kalman directly.
    tai_snapshot_.publish(rate_tracker_.tai_snapshot());
    if (est && est->valid && rate_tracker_.should_log(gptp_now_ns)) {
        log_media_clock(*est, gptp_now_ns);
    }
}

auto AvbEntityAudioIO::read_system_clock() -> SystemClockReading
{
    auto const raw = realtime::read_monotonic_raw();
    auto const utc = realtime::read_utc();
    auto const clock = clock_through_link(raw, utc);
    auto const tai = tai_limit_.judge(clock, raw);
    publish_system_clock(tai);
    return {
        .utc_ns = static_cast<uint64_t>(realtime::UtcClock::to_ns(utc)),
        .verdict = {.follow = follow_verdict(clock, raw), .tai_usable = tai.usable}};
}

auto AvbEntityAudioIO::clock_through_link(
    realtime::MonotonicRawClock::time_point const raw, realtime::UtcClock::time_point const utc) -> realtime::ClockQuality
{
    if (raw >= next_utc_report_) {
        utc_tracker_.observe(raw, utc, realtime::read_system_clock_error());
        next_utc_report_ = raw + UTC_REPORT_INTERVAL;
    }
    // A pair is one reading of each clock and a TAI timestamp is made from
    // both: the error of the two together. An unbounded link leaves no bound.
    return utc_tracker_.quality().widened_by(link_.accuracy(raw));
}

auto AvbEntityAudioIO::follow_verdict(realtime::ClockQuality const& clock, realtime::MonotonicRawClock::time_point const raw)
    -> realtime::AccuracyVerdict
{
    auto verdict = utc_follow_limit_.judge(clock, raw);
    // What the filters learned is the offset between the two clocks: a new
    // timeline of either one makes it void.
    verdict.timeline_changed = link_.take_timeline_change() || verdict.timeline_changed;
    return verdict;
}

void AvbEntityAudioIO::publish_system_clock(realtime::AccuracyVerdict const& tai)
{
    auto telemetry = udptun_->telemetry();
    telemetry.utc_accuracy_ns().publish(tai.accuracy == realtime::UNBOUNDED_ACCURACY ? -1 : tai.accuracy.count());
    telemetry.utc_excursions().publish(tai_limit_.record().excursions);
    log_tai_verdict(tai);
}

void AvbEntityAudioIO::log_tai_verdict(realtime::AccuracyVerdict const& tai)
{
    if (tai_verdict_logged_ && tai.usable == tai_vouched_) {
        return;
    }
    tai_verdict_logged_ = true;
    tai_vouched_ = tai.usable;
    // RT media thread: deferred-formatting logger. -1 ns is an unbounded error.
    kit_.host().media_log().status(
        "tunnel-clock: TAI timestamps {}  clock error={} ns  limit={} ns  excursions={}",
        logging::static_str(tai.usable ? "vouched for (tu=0)" : "NOT vouched for (tu=1)"),
        tai.accuracy == realtime::UNBOUNDED_ACCURACY ? int64_t{-1} : tai.accuracy.count(),
        config_.udptun_max_tai_error_ns,
        tai_limit_.record().excursions);
}

void AvbEntityAudioIO::log_media_clock(ptpclient::RatioEstimate const& est, uint64_t const gptp_now_ns)
{
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
        "media-clock r(switch/GPS)={:.9f}  offset-slope={:+.3f} ppm  PHC-REALTIME={} ns  unc=+/-{:.1f} ppb  utc_error={} ns  "
        "self_corrections={}",
        est.r,
        est.ppm(),
        static_cast<int64_t>(est.filtered_offset_ns),
        est.freq_uncertainty_ppb,
        udptun_->telemetry().utc_accuracy_ns().load(),
        rate_tracker_.self_corrections());
    rate_tracker_.mark_logged(gptp_now_ns);
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
