#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// @file avb_entity_crf_clock_recovery.hpp
/// @brief CrfClockRecovery — remote media-clock rate/phase from received CRF
/// timestamps (Entity Construction Kit phase 3).
///
/// A CRF talker's timestamps are its media clock's event times in the shared
/// gPTP domain. Their offset against our local gPTP receive time drifts at
/// exactly the remote-vs-local frequency difference, so a KalmanRatioTracker
/// fed with (crf_timestamp − rx_gptp) pairs recovers r = remote_media_rate /
/// local_gptp_rate — precisely the per-tick rate MediaClockGenerator::advance
/// takes to slave the local media clock (the same mechanism the GPS tracker
/// uses in AvbEntityAudioIO). The filtered phase estimate carries the remote
/// clock's presentation lead; frequency slaving alone does not consume it
/// (Milan-style phase lock is a later step).
///
/// Threading: on_crf_timestamp() runs on the reactor/RX thread (wire it as a
/// ListenerStreams StreamCrfFn via make_consumer()); estimate() is wait-free
/// and safe from the SCHED_FIFO media thread (AtomicTripleBuffer publish/
/// consume, single-writer single-reader).
///
/// One PHC per NIC belongs to gPTP: the recovered clock lives as a software
/// rate on top of it, never as a second hardware discipline.

#include "statusbar/avb_entity/avb_entity_stream_spec.hpp"
#include "statusbar/itc/itc_atomic_triple_buffer.hpp"
#include "statusbar/ptpclient/ptpclient_freq_ratio.hpp"

#include <cstdint>

namespace statusbar::avb_entity {

/// The recovered remote clock, as visible to the media thread.
struct CrfClockEstimate
{
    double r{1.0};                  ///< remote_media_rate / local_gptp_rate
    int64_t filtered_offset_ns{0};  ///< phase: (crf_ts − rx_gptp) incl. the talker's presentation lead
    double freq_uncertainty_ppb{0.0};
    bool locked{false};  ///< enough samples and the filter reports valid
    uint64_t samples{0};
};

class CrfClockRecovery
{
  public:
    struct Config
    {
        /// Per-sample measurement jitter (sqrt R): CRF receive timestamping is
        /// software (RX-drain wake time), so allow more noise than the PHC
        /// bracket reads the GPS tracker sees.
        double meas_noise_ns{500.0};
        double jerk_psd{1e-3};
        /// Estimates are not `locked` until this many CRF timestamps arrived
        /// (a fresh filter's rate rings for the first few samples).
        uint32_t min_samples{16};
    };

    using Estimate = CrfClockEstimate;

    CrfClockRecovery()
        : CrfClockRecovery(Config{})
    {}

    explicit CrfClockRecovery(Config const& config)
        : config_{config}
        , kalman_{ptpclient::KalmanRatioTracker::Config{.meas_noise_ns = config.meas_noise_ns, .jerk_psd = config.jerk_psd}}
    {}

    /// One received CRF timestamp with its local gPTP receive time. RX thread.
    /// Timestamps sharing one receive wake (multiple per PDU) add no rate
    /// information — only the first of each distinct rx time feeds the filter.
    void on_crf_timestamp(uint64_t const crf_timestamp_ns, int64_t const rx_gptp_ns)
    {
        ++samples_;
        if (last_rx_ns_ == 0 || rx_gptp_ns > last_rx_ns_) {
            double const dt_s = last_rx_ns_ == 0 ? 0.0 : static_cast<double>(rx_gptp_ns - last_rx_ns_) * 1e-9;
            (void)kalman_.add(static_cast<int64_t>(crf_timestamp_ns) - rx_gptp_ns, dt_s);
            last_rx_ns_ = rx_gptp_ns;
        }
        auto const est = kalman_.estimate();
        published_.publish(Estimate{
            .r = est.r,
            .filtered_offset_ns = est.filtered_offset_ns,
            .freq_uncertainty_ppb = est.freq_uncertainty_ppb,
            .locked = est.valid && samples_ >= config_.min_samples,
            .samples = samples_});
    }

    /// Latest recovered estimate. Wait-free; media thread (single reader).
    [[nodiscard]] auto estimate() noexcept -> Estimate { return published_.consume(); }

    /// This recovery as a ListenerStreams CRF consumer:
    ///   listener.set_crf(crf_input_index, recovery.make_consumer());
    /// The recovery must outlive the listener.
    [[nodiscard]] auto make_consumer() noexcept -> StreamCrfFn
    {
        return [this](uint64_t ts_ns, uint16_t /*index*/, int64_t rx_gptp_ns) { on_crf_timestamp(ts_ns, rx_gptp_ns); };
    }

  private:
    Config config_;
    ptpclient::KalmanRatioTracker kalman_;
    int64_t last_rx_ns_{0};
    uint64_t samples_{0};
    itc::AtomicTripleBuffer<Estimate> published_{};
};

}  // namespace statusbar::avb_entity
